/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tswg.c
 *	WireGuard's interface wg0, set up from inside the rump kernel
 *	(docs/tessronos/09-network.md, make WG=1): made and given its private
 *	key, its one peer and its address through the driver's own requests,
 *	with the routes of what the peer may send and be sent. A route of the
 *	whole Internet goes in two halves, so that the default route stays and
 *	the way to the peer itself keeps going out by it.
 */

#include <sys/param.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/route.h>
#include <net/if_wg.h>
#include <netinet/in.h>
#include <netinet/in_var.h>

#include <prop/proplib.h>

#include <rump-sys/kern.h>

#define TSWG_IF		"wg0"
#define TSWG_KEY	32
#define TSWG_NETS	16

struct tswg_net {
	uint32_t	addr;
	int		cidr;
};

int	rump_tsnet_setaddr4(const char *, uint32_t, uint32_t, uint32_t);
int	rump_tswg_up(const uint8_t *, int, uint32_t, uint32_t, const uint8_t *, const uint8_t *,
		     uint32_t, int, const struct tswg_net *, int, uint32_t);
int	rump_tswg_down(void);
int	rump_tswg_stat(unsigned long long *, unsigned long long *);

/* The routes made, for taking them away */
static struct tswg_net	routes[TSWG_NETS + 2];
static int		nroutes;
static uint32_t		host_route;
static uint32_t		local_addr;

static int
wg_socket(struct socket **so)
{

	return socreate(AF_INET, so, SOCK_DGRAM, 0, curlwp, NULL);
}

/* One request of the driver, its dictionary handed as text */
static int
wg_request(struct socket *so, unsigned long cmd, prop_dictionary_t d)
{
	struct ifdrv	ifd;
	char		*xml;
	int		error;

	xml = prop_dictionary_externalize(d);
	if (xml == NULL)
		return ENOMEM;
	memset(&ifd, 0, sizeof(ifd));
	strlcpy(ifd.ifd_name, TSWG_IF, sizeof(ifd.ifd_name));
	ifd.ifd_cmd = cmd;
	ifd.ifd_data = xml;
	ifd.ifd_len = strlen(xml) + 1;
	error = (*ifioctl)(so, SIOCSDRVSPEC, &ifd, curlwp);
	explicit_memset(xml, 0, ifd.ifd_len);
	free(xml, M_TEMP);
	return error;
}

static void
sin_set(struct sockaddr_in *s, uint32_t a)
{

	memset(s, 0, sizeof(*s));
	s->sin_len = sizeof(*s);
	s->sin_family = AF_INET;
	s->sin_addr.s_addr = a;
}

static uint32_t
mask_of(int cidr)
{

	return (cidr <= 0) ? 0 : htonl(~(uint32_t)0 << (32 - cidr));
}

static int
route_on(uint32_t addr, int cidr, uint32_t via, int flags)
{
	struct sockaddr_in	dst, gw, mask;

	sin_set(&dst, addr & mask_of(cidr));
	sin_set(&gw, via);
	sin_set(&mask, mask_of(cidr));
	return rtrequest(RTM_ADD, sintosa(&dst), sintosa(&gw),
	    (cidr == 32) ? NULL : sintosa(&mask), RTF_UP | RTF_STATIC | flags
	    | ((cidr == 32) ? RTF_HOST : 0), NULL);
}

static void
route_off(uint32_t addr, int cidr)
{
	struct sockaddr_in	dst, mask;

	sin_set(&dst, addr & mask_of(cidr));
	sin_set(&mask, mask_of(cidr));
	(void)rtrequest(RTM_DELETE, sintosa(&dst), NULL,
	    (cidr == 32) ? NULL : sintosa(&mask), 0, NULL);
}

static void
routes_off(void)
{
	int	i;

	for (i = 0; i < nroutes; i++)
		route_off(routes[i].addr, routes[i].cidr);
	nroutes = 0;
	if (host_route != 0) {
		route_off(host_route, 32);
		host_route = 0;
	}
}

/*
 * wg0 made, or an error (errno). gw is the way to the peer's endpoint, kept
 * by a host route when the peer is to carry everything; 0 for none.
 */
int
rump_tswg_up(const uint8_t *priv, int port, uint32_t addr, uint32_t mask, const uint8_t *pub,
    const uint8_t *psk, uint32_t ep_addr, int ep_port, const struct tswg_net *nets, int nnets,
    uint32_t gw)
{
	struct socket		*so = NULL;
	struct ifreq		ifr;
	struct sockaddr_in	ep;
	prop_dictionary_t	d = NULL, peer = NULL;
	prop_array_t		ips = NULL;
	int			error, i;
	bool			everything = false;

	if (nnets > TSWG_NETS)
		return EINVAL;
	rump_schedule();
	error = wg_socket(&so);
	if (error)
		goto out;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, TSWG_IF, sizeof(ifr.ifr_name));
	error = (*ifioctl)(so, SIOCIFCREATE, &ifr, curlwp);
	if (error)
		goto out;

	d = prop_dictionary_create();
	if (d == NULL || !prop_dictionary_set_data(d, "private_key", priv, TSWG_KEY)) {
		error = ENOMEM;
		goto bad;
	}
	error = wg_request(so, WG_IOCTL_SET_PRIVATE_KEY, d);
	prop_object_release(d);
	d = NULL;
	if (error)
		goto bad;
	if (port != 0) {
		d = prop_dictionary_create();
		if (d == NULL || !prop_dictionary_set_uint16(d, "listen_port", (uint16_t)port)) {
			error = ENOMEM;
			goto bad;
		}
		error = wg_request(so, WG_IOCTL_SET_LISTEN_PORT, d);
		prop_object_release(d);
		d = NULL;
		if (error)
			goto bad;
	}

	peer = prop_dictionary_create();
	ips = prop_array_create();
	if (peer == NULL || ips == NULL) {
		error = ENOMEM;
		goto bad;
	}
	sin_set(&ep, ep_addr);
	ep.sin_port = htons((uint16_t)ep_port);
	if (!prop_dictionary_set_string(peer, "name", "peer0")
	    || !prop_dictionary_set_data(peer, "public_key", pub, TSWG_KEY)
	    || !prop_dictionary_set_data(peer, "endpoint", &ep, sizeof(ep))
	    || (psk != NULL && !prop_dictionary_set_data(peer, "preshared_key", psk, TSWG_KEY))) {
		error = ENOMEM;
		goto bad;
	}
	for (i = 0; i < nnets; i++) {
		prop_dictionary_t ip = prop_dictionary_create();

		if (ip == NULL || !prop_dictionary_set_int(ip, "family", AF_INET)
		    || !prop_dictionary_set_data(ip, "ip", &nets[i].addr, 4)
		    || !prop_dictionary_set_uint8(ip, "cidr", (uint8_t)nets[i].cidr)
		    || !prop_array_add(ips, ip)) {
			error = ENOMEM;
			if (ip != NULL)
				prop_object_release(ip);
			goto bad;
		}
		prop_object_release(ip);
	}
	if (!prop_dictionary_set(peer, "allowedips", ips)) {
		error = ENOMEM;
		goto bad;
	}
	error = wg_request(so, WG_IOCTL_ADD_PEER, peer);
	if (error)
		goto bad;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, TSWG_IF, sizeof(ifr.ifr_name));
	ifr.ifr_flags = IFF_UP;
	error = (*ifioctl)(so, SIOCSIFFLAGS, &ifr, curlwp);
	if (error)
		goto bad;
	rump_unschedule();
	error = rump_tsnet_setaddr4(TSWG_IF, addr, mask, 0);
	rump_schedule();
	if (error)
		goto bad;
	local_addr = addr;

	nroutes = 0;
	for (i = 0; i < nnets; i++) {
		if (nets[i].cidr == 0) {
			everything = true;
			routes[nroutes].addr = 0;
			routes[nroutes++].cidr = 1;
			routes[nroutes].addr = htonl(0x80000000U);
			routes[nroutes++].cidr = 1;
		} else {
			routes[nroutes++] = nets[i];
		}
	}
	if (everything && gw != 0) {
		error = route_on(ep_addr, 32, gw, RTF_GATEWAY);
		if (error != 0 && error != EEXIST)
			goto bad;
		host_route = ep_addr;
	}
	for (i = 0; i < nroutes; i++) {
		error = route_on(routes[i].addr, routes[i].cidr, local_addr, 0);
		if (error == EEXIST)
			error = 0;
		if (error)
			goto bad;
	}
	error = 0;
	goto done;

    bad:
	routes_off();
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, TSWG_IF, sizeof(ifr.ifr_name));
	(void)(*ifioctl)(so, SIOCIFDESTROY, &ifr, curlwp);
    done:
	if (d != NULL)
		prop_object_release(d);
	if (peer != NULL)
		prop_object_release(peer);
	if (ips != NULL)
		prop_object_release(ips);
    out:
	if (so != NULL)
		soclose(so);
	rump_unschedule();
	return error;
}

int
rump_tswg_down(void)
{
	struct socket	*so = NULL;
	struct ifreq	ifr;
	int		error;

	rump_schedule();
	routes_off();
	error = wg_socket(&so);
	if (error == 0) {
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, TSWG_IF, sizeof(ifr.ifr_name));
		error = (*ifioctl)(so, SIOCIFDESTROY, &ifr, curlwp);
		soclose(so);
	}
	rump_unschedule();
	return error;
}

/* What went through wg0, in bytes; ENXIO when it is not there */
int
rump_tswg_stat(unsigned long long *rx, unsigned long long *tx)
{
	struct ifnet	*ifp;
	struct psref	psref;
	int		bound, error = ENXIO;

	rump_schedule();
	bound = curlwp_bind();
	ifp = if_get(TSWG_IF, &psref);
	if (ifp != NULL) {
		struct if_data	d;

		if_export_if_data(ifp, &d, false);
		*rx = d.ifi_ibytes;
		*tx = d.ifi_obytes;
		if_put(ifp, &psref);
		error = 0;
	}
	curlwp_bindx(bound);
	rump_unschedule();
	return error;
}
