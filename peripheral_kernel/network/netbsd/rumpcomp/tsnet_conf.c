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
 *	Addresses are IPv4 in network byte order.
 */

#include <sys/param.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_var.h>

#include <rump-sys/kern.h>

int	rump_tsnet_setaddr4(const char *, uint32_t, uint32_t, uint32_t);
int	rump_tsnet_getaddr4(const char *, uint32_t *, uint32_t *);
int	rump_tsnet_defroute4(uint32_t);
int	rump_tsnet_ifindex(const char *);
int	rump_tsnet_ifname(int, char *);
int	rump_tsnet_ifup(const char *, int);
int	rump_tsnet_fionread(int, int *);
void	*rump_tsnet_lwp_make(void);
void	rump_tsnet_host(void);

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
