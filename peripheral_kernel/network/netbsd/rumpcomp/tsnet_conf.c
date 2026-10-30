/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsnet_conf.c
 *	Setting up the interfaces and routes of the NetBSD stack (design 12.6).
 *
 *	This file is compiled with the NetBSD kernel headers into the rump
 *	kernel (tools/netbsd/build.sh). The interfaces' addresses and the
 *	default route are set here from inside the kernel, through the
 *	interface ioctls on a socket of the kernel's and a request to the
 *	routing table, so that the TessronOS side (so_api.c, so_dhcp.c)
 *	needs to know nothing of the kernel's structures. Each entry point
 *	enters the rump kernel as a thread of it for the time it runs.
 *
 *	IPv4 addresses are 32 bit words in network byte order, IPv6 ones 16
 *	bytes in the order they are sent. A link-local IPv6 address crosses
 *	in its plain form; the zone the kernel keeps inside it is put in and
 *	taken out here.
 */

#include <sys/param.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/pserialize.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/timevar.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/route.h>
#include <net/if_llatbl.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet6/in6_var.h>
#include <netinet6/in6_ifattach.h>
#include <netinet6/nd6.h>
#include <netinet6/scope6_var.h>

#include <rump-sys/kern.h>

/* One IPv6 address of an interface (so_nb.h has the same layout) */
struct tsnet_a6 {
	uint8_t		addr[16];
	int32_t		plen;
	uint32_t	flags;		/* IN6_IFF_ */
	uint32_t	vltime;		/* seconds of its valid lifetime left; ~0: no end */
	uint32_t	pltime;		/* of its preferred lifetime */
};

int	rump_tsnet_setaddr4(const char *, uint32_t, uint32_t, uint32_t);
int	rump_tsnet_getaddr4(const char *, uint32_t *, uint32_t *);
int	rump_tsnet_addrstate4(const char *, uint32_t);
int	rump_tsnet_defroute4(uint32_t);
int	rump_tsnet_addrs6(const char *, struct tsnet_a6 *, int);
int	rump_tsnet_setaddr6(const char *, const uint8_t *, int, uint32_t, uint32_t,
	    uint32_t);
int	rump_tsnet_ip6on(const char *, int);
void	rump_tsnet_prefer_temp(int);
int	rump_tsnet_deladdr6(const char *, const uint8_t *);
int	rump_tsnet_defroute6(const char *, const uint8_t *);
int	rump_tsnet_cmsg_hlim(void *, unsigned int);
int	rump_tsnet_cmsg_tos(void *, unsigned int);
int	rump_tsnet_ifindex(const char *);
int	rump_tsnet_ifname(int, char *);
int	rump_tsnet_ifup(const char *, int);
int	rump_tsnet_fionread(int, int *);
void	*rump_tsnet_lwp_make(void);
void	rump_tsnet_host(void);
int	rump_tswifi_setmac(const char *, const uint8_t *);

static int
tsnet_socket(struct socket **sop)
{

	return socreate(AF_INET, sop, SOCK_DGRAM, 0, curlwp, NULL);
}

static void
tsnet_ifreq(struct ifreq *ifr, const char *ifname)
{

	memset(ifr, 0, sizeof(*ifr));
	strlcpy(ifr->ifr_name, ifname, sizeof(ifr->ifr_name));
}

/*
 * The IPv4 addresses of the interface are replaced: the old ones go,
 * and addr with its mask and broadcast address comes, unless both
 * addr and bcast are 0. An address of 0 with a broadcast address of
 * all ones is what lets the interface send and take the broadcasts of
 * DHCP before it has an address of its own.
 */
int
rump_tsnet_setaddr4(const char *ifname, uint32_t addr, uint32_t mask,
    uint32_t bcast)
{
	struct socket		*so;
	struct ifreq		ifr;
	struct in_aliasreq	ifra;
	struct sockaddr_in	*sin;
	int			error, i;

	rump_schedule();
	error = tsnet_socket(&so);
	if (error)
		goto out;

	for (i = 0; i < 16; i++) {
		tsnet_ifreq(&ifr, ifname);
		if ((*ifioctl)(so, SIOCGIFADDR, &ifr, curlwp) != 0)
			break;
		if ((*ifioctl)(so, SIOCDIFADDR, &ifr, curlwp) != 0)
			break;
	}

	if (addr != 0 || bcast != 0) {
		memset(&ifra, 0, sizeof(ifra));
		strlcpy(ifra.ifra_name, ifname, sizeof(ifra.ifra_name));
		sin = &ifra.ifra_addr;
		sin->sin_len = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = addr;
		if (mask != 0) {
			sin = &ifra.ifra_mask;
			sin->sin_len = sizeof(*sin);
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = mask;
		}
		if (bcast != 0) {
			sin = &ifra.ifra_broadaddr;
			sin->sin_len = sizeof(*sin);
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = bcast;
		}
		error = (*ifioctl)(so, SIOCAIFADDR, &ifra, curlwp);
	}
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/* The first IPv4 address of the interface and its mask; 0 for none */
int
rump_tsnet_getaddr4(const char *ifname, uint32_t *addr, uint32_t *mask)
{
	struct socket	*so;
	struct ifreq	ifr;
	int		error;

	*addr = 0;
	*mask = 0;
	rump_schedule();
	error = tsnet_socket(&so);
	if (error)
		goto out;
	tsnet_ifreq(&ifr, ifname);
	if ((*ifioctl)(so, SIOCGIFADDR, &ifr, curlwp) == 0) {
		*addr = satosin(&ifr.ifr_addr)->sin_addr.s_addr;
		tsnet_ifreq(&ifr, ifname);
		if ((*ifioctl)(so, SIOCGIFNETMASK, &ifr, curlwp) == 0)
			*mask = satosin(&ifr.ifr_addr)->sin_addr.s_addr;
	}
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/*
 * The state of the IPv4 address addr of the interface as its check
 * against the other machines on the link left it: 0 usable, 1 being
 * checked (IN_IFF_TENTATIVE), 2 another machine's (IN_IFF_DUPLICATED),
 * 3 the link down (IN_IFF_DETACHED), -1 no such address
 */
int
rump_tsnet_addrstate4(const char *ifname, uint32_t addr)
{
	struct socket	*so;
	struct ifreq	ifr;
	struct sockaddr_in *sin;
	int		state = -1, f;

	rump_schedule();
	if (tsnet_socket(&so) != 0)
		goto out;
	tsnet_ifreq(&ifr, ifname);
	sin = satosin(&ifr.ifr_addr);
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	if ((*ifioctl)(so, SIOCGIFAFLAG_IN, &ifr, curlwp) == 0) {
		f = ifr.ifr_addrflags;
		state = (f & IN_IFF_DUPLICATED) ? 2 : (f & IN_IFF_TENTATIVE) ? 1 :
		    (f & IN_IFF_DETACHED) ? 3 : 0;
	}
	soclose(so);
out:
	rump_unschedule();
	return state;
}

/* The default route through gw; 0 takes it away */
int
rump_tsnet_defroute4(uint32_t gw)
{
	struct sockaddr_in	dst, via, mask;
	int			error = 0;

	memset(&dst, 0, sizeof(dst));
	dst.sin_len = sizeof(dst);
	dst.sin_family = AF_INET;
	memset(&mask, 0, sizeof(mask));
	mask.sin_len = sizeof(mask);
	mask.sin_family = AF_INET;
	memset(&via, 0, sizeof(via));
	via.sin_len = sizeof(via);
	via.sin_family = AF_INET;
	via.sin_addr.s_addr = gw;

	rump_schedule();
	(void)rtrequest(RTM_DELETE, sintosa(&dst), NULL, sintosa(&mask), 0, NULL);
	if (gw != 0) {
		error = rtrequest(RTM_ADD, sintosa(&dst), sintosa(&via),
		    sintosa(&mask), RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
	}
	rump_unschedule();
	return error;
}

/* ---------------------------------------------------------------- IPv6 */

/* What is left of a lifetime that ends at 'expire' (time_uptime); ~0 for none */
static uint32_t
tsnet_left(uint32_t lifetime, time_t expire)
{

	if (lifetime == ND6_INFINITE_LIFETIME || expire == 0)
		return ND6_INFINITE_LIFETIME;
	return (expire > time_uptime) ? (uint32_t)(expire - time_uptime) : 0;
}

/*
 * The IPv6 addresses of the interface, up to max of them into list;
 * how many there are, or -1 for no such interface.
 */
int
rump_tsnet_addrs6(const char *ifname, struct tsnet_a6 *list, int max)
{
	struct ifnet		*ifp;
	struct ifaddr		*ifa;
	struct in6_ifaddr	*ia;
	struct in6_addr		a;
	int			n = 0, s;

	rump_schedule();
	ifp = ifunit(ifname);
	if (ifp == NULL) {
		rump_unschedule();
		return -1;
	}
	s = pserialize_read_enter();
	IFADDR_READER_FOREACH(ifa, ifp) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		if (n >= max)
			break;
		ia = (struct in6_ifaddr *)ifa;
		a = ia->ia_addr.sin6_addr;
		in6_clearscope(&a);
		memcpy(list[n].addr, &a, sizeof(a));
		list[n].plen = in6_mask2len(&ia->ia_prefixmask.sin6_addr, NULL);
		list[n].flags = ia->ia6_flags;
		list[n].vltime = tsnet_left(ia->ia6_lifetime.ia6t_vltime,
		    ia->ia6_lifetime.ia6t_expire);
		list[n].pltime = tsnet_left(ia->ia6_lifetime.ia6t_pltime,
		    ia->ia6_lifetime.ia6t_preferred);
		n++;
	}
	pserialize_read_exit(s);
	rump_unschedule();
	return n;
}

static void
tsnet_sin6(struct sockaddr_in6 *sin6, const uint8_t *addr)
{

	memset(sin6, 0, sizeof(*sin6));
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_family = AF_INET6;
	if (addr != NULL)
		memcpy(&sin6->sin6_addr, addr, sizeof(sin6->sin6_addr));
}

/*
 * An address put on the interface with its prefix length, lifetimes
 * (seconds, ~0 for no end) and flags (IN6_IFF_AUTOCONF for one made
 * from a router's prefix, IN6_IFF_TEMPORARY as well for a temporary
 * one, 0 for one the settings fix), or the lifetimes of one it has
 * renewed. A new address is checked for duplicates on the link first and is
 * tentative until then; one whose valid lifetime runs out is taken away
 * by the kernel itself, and one past its preferred lifetime is no longer
 * chosen as the source of new connections.
 */
int
rump_tsnet_setaddr6(const char *ifname, const uint8_t *addr, int plen,
    uint32_t vltime, uint32_t pltime, uint32_t flags)
{
	struct socket		*so;
	struct in6_aliasreq	ifra;
	int			error;

	if (plen <= 0 || plen > 128)
		return EINVAL;
	rump_schedule();
	error = socreate(AF_INET6, &so, SOCK_DGRAM, 0, curlwp, NULL);
	if (error)
		goto out;
	memset(&ifra, 0, sizeof(ifra));
	strlcpy(ifra.ifra_name, ifname, sizeof(ifra.ifra_name));
	tsnet_sin6(&ifra.ifra_addr, addr);
	tsnet_sin6(&ifra.ifra_prefixmask, NULL);
	in6_prefixlen2mask(&ifra.ifra_prefixmask.sin6_addr, plen);
	ifra.ifra_flags = flags & (IN6_IFF_AUTOCONF | IN6_IFF_TEMPORARY);
	ifra.ifra_lifetime.ia6t_vltime = vltime;
	ifra.ifra_lifetime.ia6t_pltime = pltime;
	error = (*ifioctl)(so, SIOCAIFADDR_IN6, &ifra, curlwp);
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/* An IPv6 address taken off the interface */
int
rump_tsnet_deladdr6(const char *ifname, const uint8_t *addr)
{
	struct socket		*so;
	struct in6_ifreq	ifr;
	int			error;

	rump_schedule();
	error = socreate(AF_INET6, &so, SOCK_DGRAM, 0, curlwp, NULL);
	if (error)
		goto out;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	tsnet_sin6(&ifr.ifr_addr, addr);
	error = (*ifioctl)(so, SIOCDIFADDR_IN6, &ifr, curlwp);
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/*
 * The IPv6 default route through gw, a router's link-local address on
 * the interface; gw NULL takes it away. The routing table holds a
 * link-local gateway with its zone inside the address.
 */
int
rump_tsnet_defroute6(const char *ifname, const uint8_t *gw)
{
	struct sockaddr_in6	dst, via, mask;
	struct ifnet		*ifp;
	int			error = 0;

	tsnet_sin6(&dst, NULL);
	tsnet_sin6(&mask, NULL);
	tsnet_sin6(&via, gw);

	rump_schedule();
	(void)rtrequest(RTM_DELETE, sin6tosa(&dst), NULL, sin6tosa(&mask), 0, NULL);
	if (gw != NULL) {
		ifp = ifunit(ifname);
		if (ifp == NULL)
			error = ENXIO;
		else
			error = in6_setscope(&via.sin6_addr, ifp, NULL);
		if (error == 0)
			error = rtrequest(RTM_ADD, sin6tosa(&dst), sin6tosa(&via),
			    sin6tosa(&mask), RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
	}
	rump_unschedule();
	return error;
}

/*
 * IPv6 on the interface turned on (on != 0) or off. Off, the kernel
 * neither sends nor takes IPv6 there and holds every address of it as
 * tentative; on again, it checks them for duplicates once more, the
 * link-local address first.
 */
int
rump_tsnet_ip6on(const char *ifname, int on)
{
	struct socket		*so;
	struct in6_ndireq	nd;
	int			error;

	rump_schedule();
	error = socreate(AF_INET6, &so, SOCK_DGRAM, 0, curlwp, NULL);
	if (error)
		goto out;
	memset(&nd, 0, sizeof(nd));
	strlcpy(nd.ifname, ifname, sizeof(nd.ifname));
	error = (*ifioctl)(so, SIOCGIFINFO_IN6, &nd, curlwp);
	if (error == 0) {
		if (on)
			nd.ndi.flags &= ~ND6_IFF_IFDISABLED;
		else
			nd.ndi.flags |= ND6_IFF_IFDISABLED;
		error = (*ifioctl)(so, SIOCSIFINFO_FLAGS, &nd, curlwp);
	}
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/*
 * Whether a temporary address is chosen before the others of its
 * prefix as the source of a new connection (RFC 6724 rule 7).
 */
void
rump_tsnet_prefer_temp(int yes)
{
	extern int ip6_prefer_tempaddr;

	ip6_prefer_tempaddr = yes ? 1 : 0;
}

/*
 * The hop limit a datagram arrived with, from the ancillary data a
 * receive gave back (IPV6_RECVHOPLIMIT); -1 when it is not there.
 */
int
rump_tsnet_cmsg_hlim(void *ctl, unsigned int len)
{
	struct msghdr	m;
	struct cmsghdr	*c;
	int		h;

	memset(&m, 0, sizeof(m));
	m.msg_control = ctl;
	m.msg_controllen = len;
	for (c = CMSG_FIRSTHDR(&m); c != NULL; c = CMSG_NXTHDR(&m, c)) {
		if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT &&
		    c->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(&h, CMSG_DATA(c), sizeof(h));
			return h;
		}
	}
	return -1;
}

/*
 * The type of service of an IPv4 datagram as it is received (ECN).
 * NetBSD has no option that hands it over, so a socket asks for the
 * datagrams' TTL (IP_RECVTTL) in its stead, and the ancillary data made
 * for each of them gets one more message: the TOS byte, of the level
 * IPPROTO_IP and the type below, which no option of NetBSD's uses.
 * build.sh links the stack so that its calls of ip_savecontrol come
 * here (ld --wrap).
 */
#define TSNET_CMSG_TOS	0x7453

void	__real_rumpns_ip_savecontrol(struct inpcb *, struct mbuf **, struct ip *,
	    struct mbuf *);
void	__wrap_rumpns_ip_savecontrol(struct inpcb *, struct mbuf **, struct ip *,
	    struct mbuf *);

void
__wrap_rumpns_ip_savecontrol(struct inpcb *inp, struct mbuf **mp, struct ip *ip,
    struct mbuf *m)
{

	__real_rumpns_ip_savecontrol(inp, mp, ip, m);
	if ((inp->inp_flags & INP_RECVTTL) == 0)
		return;
	while (*mp != NULL)
		mp = &(*mp)->m_next;
	*mp = sbcreatecontrol(&ip->ip_tos, sizeof(uint8_t), TSNET_CMSG_TOS,
	    IPPROTO_IP);
}

/*
 * The type of service (IPv4) or the traffic class (IPv6) a datagram
 * arrived with, from the ancillary data a receive gave back; -1 when
 * it is not there.
 */
int
rump_tsnet_cmsg_tos(void *ctl, unsigned int len)
{
	struct msghdr	m;
	struct cmsghdr	*c;
	int		v;

	memset(&m, 0, sizeof(m));
	m.msg_control = ctl;
	m.msg_controllen = len;
	for (c = CMSG_FIRSTHDR(&m); c != NULL; c = CMSG_NXTHDR(&m, c)) {
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == TSNET_CMSG_TOS &&
		    c->cmsg_len >= CMSG_LEN(sizeof(uint8_t)))
			return *(uint8_t *)CMSG_DATA(c);
		if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS &&
		    c->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(&v, CMSG_DATA(c), sizeof(v));
			return v & 0xff;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- interfaces */

/* The index of an interface by its name; 0 for none */
int
rump_tsnet_ifindex(const char *ifname)
{
	struct ifnet	*ifp;
	int		idx = 0;

	rump_schedule();
	ifp = ifunit(ifname);
	if (ifp != NULL)
		idx = ifp->if_index;
	rump_unschedule();
	return idx;
}

/* The name of an interface by its index, into IFNAMSIZ bytes; ENXIO for none */
int
rump_tsnet_ifname(int idx, char *buf)
{
	struct ifnet	*ifp;
	int		error = ENXIO;

	if (idx <= 0)
		return ENXIO;
	rump_schedule();
	ifp = if_byindex((u_int)idx);
	if (ifp != NULL) {
		strlcpy(buf, ifp->if_xname, IFNAMSIZ);
		error = 0;
	}
	rump_unschedule();
	return error;
}

/* The interface brought up (up != 0) or down */
int
rump_tsnet_ifup(const char *ifname, int up)
{
	struct socket	*so;
	struct ifreq	ifr;
	int		error;

	rump_schedule();
	error = tsnet_socket(&so);
	if (error)
		goto out;
	tsnet_ifreq(&ifr, ifname);
	error = (*ifioctl)(so, SIOCGIFFLAGS, &ifr, curlwp);
	if (error == 0) {
		if (up)
			ifr.ifr_flags |= IFF_UP;
		else
			ifr.ifr_flags &= ~IFF_UP;
		error = (*ifioctl)(so, SIOCSIFFLAGS, &ifr, curlwp);
	}
	soclose(so);
out:
	rump_unschedule();
	return error;
}

/*
 * How many bytes of data wait on a socket. FIONREAD of NetBSD counts
 * everything in the receive buffer, which for a datagram socket takes
 * in the address of each datagram as well; only the data is counted
 * here.
 */
int
rump_tsnet_fionread(int fd, int *avail)
{
	file_t		*fp;
	struct socket	*so;
	struct mbuf	*m, *n;
	int		cnt = 0, error = 0;

	rump_schedule();
	fp = fd_getfile(fd);
	if (fp == NULL) {
		error = EBADF;
		goto out;
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fd_putfile(fd);
		error = ENOTSOCK;
		goto out;
	}
	so = fp->f_socket;
	solock(so);
	for (m = so->so_rcv.sb_mb; m != NULL; m = m->m_nextpkt) {
		for (n = m; n != NULL; n = n->m_next) {
			if (n->m_type == MT_DATA || n->m_type == MT_HEADER ||
			    n->m_type == MT_OOBDATA)
				cnt += n->m_len;
		}
	}
	sounlock(so);
	fd_putfile(fd);
	*avail = cnt;
out:
	rump_unschedule();
	return error;
}

/*
 * A thread of the process the sockets belong to (proc 1), made to be
 * lent to a task for the time of one system call. Entering the rump
 * kernel with no thread of its own would have one made and taken apart
 * again on every call, each time through the one thread (lwp0) all such
 * entries share; a thread kept and lent again avoids both. It is marked
 * as running, as a thread switched to is, and stays so.
 */
void *
rump_tsnet_lwp_make(void)
{
	struct lwp	*l;

	rump_schedule();
	l = rump__lwproc_alloclwp(initproc);
	l->l_pflag |= LP_RUNNING;
	rump_unschedule();
	return l;
}

/*
 * A host, not a router: the rump kernel's options forward packets
 * between interfaces by default.
 */
void
rump_tsnet_host(void)
{
	extern int ipforwarding, ip6_forwarding;

	ipforwarding = 0;
	ip6_forwarding = 0;
}

/*
 * The link-level address of an interface (neta0, when the card under
 * the device "neta" changes); 0 or an errno value. In every build of
 * the stack: the card may be outside the rump kernel (OpenBSD's
 * IEEE 802.11 layer, design 9.6.9)
 *
 * The IPv6 link-local address is made of the link-level one (its
 * interface identifier, EUI-64): the one made of the address before is
 * taken away and one of the new address made, as at the interface's
 * start (in6_ifattach), with its check for a duplicate. Without that,
 * neta0 kept the link-local address of the wired card it started on
 * after it moved onto the Wi-Fi.
 */
int
rump_tswifi_setmac(const char *ifname, const uint8_t *mac)
{
	struct ifnet *ifp;
	struct in6_ifaddr *ia;
	int s, same, n;

	rump_schedule();
	ifp = ifunit(ifname);
	if (ifp == NULL) {
		rump_unschedule();
		return ENXIO;
	}
	KERNEL_LOCK(1, NULL);
	s = splnet();
	same = ifp->if_sadl != NULL && memcmp(CLLADDR(ifp->if_sadl), mac, ETHER_ADDR_LEN) == 0;
	if_set_sadl(ifp, mac, ETHER_ADDR_LEN, false);
	splx(s);
	if (!same) {
		IFNET_LOCK(ifp);
		/* the addresses are read and changed only under the interface's lock */
		for (n = 0; n < 8 && (ia = in6ifa_ifpforlinklocal(ifp, 0)) != NULL
		    && (ia->ia_ifa.ifa_flags & IFA_DESTROYING) == 0; n++)
			in6_purgeaddr(&ia->ia_ifa);
		if (ifp->if_flags & IFF_UP)
			in6_ifattach(ifp, NULL);
		IFNET_UNLOCK(ifp);
		/*
		 * IPv4 as Linux has it on a change of the address (NETDEV_CHANGEADDR:
		 * the neighbours forgotten, the addresses told again with the new
		 * one): the ARP table emptied as when the interface goes down
		 * (in_if_down), and each address checked and announced again from
		 * the new address, as when the link comes back (in_if_link_down,
		 * in_if_link_up). The entries of the card before may be of another
		 * network, and the machines on the link know the address by the
		 * card's address before.
		 */
		if (ifp->if_afdata[AF_INET] != NULL) {
			s = splnet();
			in_if_link_down(ifp);
			lltable_purge_entries(LLTABLE(ifp));
			in_if_link_up(ifp);
			splx(s);
		}
	}
	KERNEL_UNLOCK_ONE(NULL);
	rump_unschedule();
	return 0;
}
