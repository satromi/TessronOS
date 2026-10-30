/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_api.c
 *	The T2EX socket calls over the NetBSD stack (design 12.6).
 *
 *	The stack is NetBSD's own, running as a rump kernel inside this one
 *	(tools/netbsd/build.sh, rumpuser.c). Every call here is a system call
 *	of that kernel made with rump_syscall, which answers a NetBSD errno
 *	value; it becomes an error code the way it always has (knl_so_errcode).
 *
 *	The calls keep their numbers and shapes (include/ts/sodef.h): the
 *	families, option names, flags and poll events are turned into
 *	NetBSD's on the way in and back on the way out. A socket number is
 *	the index of an entry here that holds the rump kernel's descriptor
 *	and the socket's owner -- the process it was made for, or 0 for the
 *	kernel -- so the numbers stay 0 .. SO_MAX - 1 as before. The owner is
 *	written when the socket is made and cleared when it is closed, so a
 *	number that comes round again is never taken for the one before it.
 *	The socket's real object is made and taken away at the same two
 *	points (obsock.c); the calls between never look at it.
 *
 *	All sockets are descriptors of one process of the rump kernel. A
 *	task that calls in becomes a thread of it for the time of the call.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "tstdlib.h"
#include "rumpglue.h"
#include "so_nb.h"

/* ---------------------------------------------------------------- NetBSD's numbers */

#define NB_AF_UNSPEC		0
#define NB_AF_INET		2
#define NB_AF_INET6		24

#define NB_SOL_SOCKET		0xffff
#define NB_SO_NOSIGPIPE		0x0800
#define NB_SO_SNDTIMEO		0x100b
#define NB_SO_RCVTIMEO		0x100c
#define NB_SO_ERROR		0x1007
#define NB_SO_TYPE		0x1008
#define NB_SO_REUSEPORT		0x0200
#define NB_SOMAXCONN		128

#define NB_TCP_NODELAY		1
#define NB_TCP_KEEPIDLE		3
#define NB_TCP_KEEPINTVL	5
#define NB_TCP_KEEPCNT		6

#define NB_IP_TOS		3
#define NB_IP_TTL		4
#define NB_IP_MULTICAST_IF	9
#define NB_IP_MULTICAST_TTL	10
#define NB_IP_MULTICAST_LOOP	11
#define NB_IP_ADD_MEMBERSHIP	12
#define NB_IP_DROP_MEMBERSHIP	13
#define NB_IP_RECVTTL		23

#define NB_IPV6_UNICAST_HOPS	4
#define NB_IPV6_MULTICAST_IF	9
#define NB_IPV6_MULTICAST_HOPS	10
#define NB_IPV6_MULTICAST_LOOP	11
#define NB_IPV6_JOIN_GROUP	12
#define NB_IPV6_LEAVE_GROUP	13
#define NB_IPV6_CHECKSUM	26
#define NB_IPV6_V6ONLY		27
#define NB_IPV6_RECVHOPLIMIT	37
#define NB_IPV6_RECVTCLASS	57
#define NB_IPV6_TCLASS		61

#define NB_MSG_OOB		0x0001
#define NB_MSG_PEEK		0x0002
#define NB_MSG_TRUNC		0x0010
#define NB_MSG_CTRUNC		0x0020
#define NB_MSG_WAITALL		0x0040
#define NB_MSG_DONTWAIT		0x0080
#define NB_MSG_NOSIGNAL		0x0400

#define NB_POLLIN		0x0001
#define NB_POLLPRI		0x0002
#define NB_POLLOUT		0x0004
#define NB_POLLERR		0x0008
#define NB_POLLHUP		0x0010
#define NB_POLLNVAL		0x0020
#define NB_POLLRDNORM		0x0040
#define NB_POLLRDBAND		0x0080
#define NB_POLLWRBAND		0x0100

#define NB_O_NONBLOCK		0x0004

/* The flags of so_fcntl as they have always read */
#define SO_O_NONBLOCK		1
#define SO_O_RDONLY		2
#define SO_O_WRONLY		4
#define SO_O_RDWR		6
#define NB_F_GETFL		3
#define NB_F_SETFL		4
#define NB_FIONBIO		0x8004667eUL

#define NB_EBADF		9
#define NB_EINVAL		22
#define NB_ENOSYS		78
#define NB_ENOPROTOOPT		42
#define NB_ERESTART		(-3)

#define SA_MAX			128		/* bytes of an address */

LOCAL BOOL		so_started = FALSE;
EXPORT BOOL		knl_so_nb_card = FALSE;
LOCAL char		so_hostname[32] = "tessronos";
LOCAL BOOL		so_asking = FALSE;	/* DHCP is running on the interface */
LOCAL UW		so_gw = 0;		/* the default route's gateway */
LOCAL UW		ns_fixed[2];		/* name servers the settings name */
LOCAL UW		ns_iface[2];		/* name servers the interface came with */
LOCAL char		so_domain[80];
/* IPv6 as the settings say (so_setip6) */
LOCAL UINT		ip6_mode = CNF_NET_SLAAC ? SO_IP6_AUTO : SO_IP6_LINK;
LOCAL UB		ip6_addr[16];		/* fixed: the address, its prefix length */
LOCAL INT		ip6_plen = 0;
LOCAL UB		ip6_gw[16];		/* fixed: the default router, all zero none */
LOCAL UB		ns6_fixed[2][16];	/* IPv6 name servers the settings name */

/* ---------------------------------------------------------------- errors */

/*
 * What BSD reports as errno becomes the nearest error code. Anything not
 * named here is a fault of the call itself rather than of the network,
 * so it comes out as E_SYS. The numbers are those of sodef.h.
 */
EXPORT ER knl_so_errcode( INT e )
{
	switch ( e ) {
	case 0:			return E_OK;
	case EINVAL:		return E_PAR;
	case EFAULT:		return E_PAR;
	case EBADF:		return E_ID;
	case ENOTSOCK:		return E_ID;
	case EACCES:		return E_OACV;
	case ENOMEM:		return E_NOMEM;
	case ENOBUFS:		return E_NOMEM;
	case EMFILE:		return E_LIMIT;
	case EAGAIN:		return E_TMOUT;
	case ETIMEDOUT:		return E_TMOUT;
	case EINPROGRESS:	return E_BUSY;
	case EALREADY:		return E_BUSY;
	case EADDRINUSE:	return E_BUSY;
	case EISCONN:		return E_OBJ;
	case ENOTCONN:		return E_OBJ;
	case ECONNRESET:	return E_IO;
	case ECONNABORTED:	return E_IO;
	case ECONNREFUSED:	return E_IO;
	case EHOSTUNREACH:	return E_IO;
	case ENETUNREACH:	return E_IO;
	case ENETDOWN:		return E_IO;
	case EHOSTDOWN:		return E_IO;
	case EPIPE:		return E_IO;
	case EOPNOTSUPP:	return E_NOSPT;
	case EAFNOSUPPORT:	return E_NOSPT;
	case EPROTONOSUPPORT:	return E_NOSPT;
	default:		return E_SYS;
	}
}

/* A NetBSD errno value as sodef.h numbers it (SO_ERROR's answer) */
LOCAL INT nb_errno( INT e )
{
	LOCAL CONST UB	net[] = {		/* NetBSD 35 .. 65 */
		EAGAIN, EINPROGRESS, EALREADY, ENOTSOCK, EDESTADDRREQ, EMSGSIZE,
		EPROTOTYPE, ENOPROTOOPT, EPROTONOSUPPORT, 94, EOPNOTSUPP, 96,
		EAFNOSUPPORT, EADDRINUSE, EADDRNOTAVAIL, ENETDOWN, ENETUNREACH, 102,
		ECONNABORTED, ECONNRESET, ENOBUFS, EISCONN, ENOTCONN, ESHUTDOWN,
		109, ETIMEDOUT, ECONNREFUSED, 40, 36, EHOSTDOWN, EHOSTUNREACH
	};

	if ( e >= 35 && e <= 65 ) {
		return net[e - 35];
	}
	switch ( e ) {
	case 11:		return 35;	/* EDEADLK */
	case NB_ENOSYS:		return ENOSYS;
	case NB_ERESTART:	return EINTR;	/* the socket was closed under the call */
	default:		return e;	/* 1 .. 34 are the same */
	}
}

EXPORT ER knl_so_nb_er( INT nberr )
{
	return knl_so_errcode(nb_errno(nberr));
}

/* ---------------------------------------------------------------- system calls */

/*
 * The threads of the rump kernel lent to the tasks that call in, one
 * for each call going on at a time; made when none is free, and kept
 * (rumpcomp/tsnet_conf.c).
 */
#define LWP_KEEP	256

LOCAL void		*lwp_free[LWP_KEEP];
LOCAL INT		lwp_nfree = 0;
LOCAL T_SPLOCK		lwp_lock;

LOCAL void *lwp_get( void )
{
	void	*l = NULL;
	UINT	imask;

	ISpinLock(&lwp_lock, &imask);
	if ( lwp_nfree > 0 ) {
		l = lwp_free[--lwp_nfree];
	}
	ISpinUnlock(&lwp_lock, &imask);
	return ( l != NULL ) ? l : rump_tsnet_lwp_make();
}

/* Back for the next call. Past LWP_KEEP at a time, the rest are left made but unused. */
LOCAL void lwp_put( void *l )
{
	UINT	imask;

	ISpinLock(&lwp_lock, &imask);
	if ( lwp_nfree < LWP_KEEP ) {
		lwp_free[lwp_nfree++] = l;
	}
	ISpinUnlock(&lwp_lock, &imask);
}

/* A system call of the rump kernel; its NetBSD errno value, the result in *rv */
LOCAL INT nb_call( INT num, D *args, INT nargs, D *rv )
{
	D	ret[2];
	void	*l;
	INT	e;

	ret[0] = ret[1] = 0;
	l = lwp_get();
	rump_lwproc_curlwp_set(l);
	e = rump_syscall(num, args, (UD)nargs * sizeof(D), ret);
	rump_lwproc_curlwp_clear(l);
	lwp_put(l);
	if ( rv != NULL ) {
		*rv = ret[0];
	}
	return e;
}

#define A(x)	((D)(UBINT)(x))

/* ---------------------------------------------------------------- sockets and owners */

typedef struct {
	INT	fdp1;			/* the rump kernel's descriptor + 1; 0: free */
	ID	owner;
} SO_ENT;

LOCAL SO_ENT		so_tab[SO_MAX];
LOCAL T_SPLOCK		so_lock;

/* The descriptor of socket s, -1 for none */
LOCAL INT so_fd( INT s )
{
	if ( s < 0 || s >= SO_MAX ) {
		return -1;
	}
	return so_tab[s].fdp1 - 1;
}

/* A number for a new descriptor: the lowest free one, or E_LIMIT */
LOCAL INT so_slot( INT fd, ID owner )
{
	UINT	imask;
	INT	s;

	ISpinLock(&so_lock, &imask);
	for ( s = 0; s < SO_MAX; s++ ) {
		if ( so_tab[s].fdp1 == 0 ) {
			so_tab[s].fdp1 = fd + 1;
			so_tab[s].owner = owner;
			break;
		}
	}
	ISpinUnlock(&so_lock, &imask);
	return ( s < SO_MAX ) ? s : E_LIMIT;
}

LOCAL void nb_close( INT fd )
{
	D	a[1];

	a[0] = fd;
	(void)nb_call(NB_SYS_close, a, 1, NULL);
}

/* A descriptor made, given a number; closed again if there is none */
LOCAL INT so_new( INT fd, ID owner )
{
	D	a[5];
	INT	on = 1, s;

	/* a write to a connection that has gone is an error, not a signal */
	a[0] = fd; a[1] = NB_SOL_SOCKET; a[2] = NB_SO_NOSIGPIPE; a[3] = A(&on); a[4] = sizeof(on);
	(void)nb_call(NB_SYS_setsockopt, a, 5, NULL);

	s = so_slot(fd, owner);
	if ( s < 0 ) {
		nb_close(fd);
	}
	return s;
}

EXPORT BOOL knl_so_mine( INT s, ID owner )
{
	return ( s >= 0 && s < SO_MAX && so_tab[s].fdp1 != 0 && so_tab[s].owner == owner );
}

/* A process is ending: whatever sockets it left open are closed */
EXPORT void knl_so_prc_end( ID pid )
{
	INT	s;

	if ( pid <= 0 ) {
		return;
	}
	for ( s = 0; s < SO_MAX; s++ ) {
		if ( so_tab[s].fdp1 != 0 && so_tab[s].owner == pid ) {
			(void)so_close(s);
		}
	}
}

/* Sockets of processes other than 'but' that are open */
EXPORT INT so_busy( ID but )
{
	INT	i, n = 0;

	for ( i = 0; i < SO_MAX; i++ ) {
		if ( so_tab[i].fdp1 != 0 && so_tab[i].owner > 0 && so_tab[i].owner != but ) n++;
	}
	return n;
}

/* ---------------------------------------------------------------- addresses */

LOCAL INT af_in( INT af )
{
	switch ( af ) {
	case AF_UNSPEC:	return NB_AF_UNSPEC;
	case AF_INET:	return NB_AF_INET;
	case AF_INET6:	return NB_AF_INET6;
	default:	return -1;
	}
}

LOCAL INT af_out( INT af )
{
	switch ( af ) {
	case NB_AF_INET:	return AF_INET;
	case NB_AF_INET6:	return AF_INET6;
	default:		return AF_UNSPEC;
	}
}

/* An address given, into NetBSD's form in nb */
LOCAL ER sa_in( CONST struct sockaddr *a, socklen_t len, UB *nb )
{
	INT	af;

	if ( a == NULL || len < 2 || len > SA_MAX ) {
		return E_PAR;
	}
	af = af_in(a->sa_family);
	if ( af < 0 ) {
		return E_NOSPT;
	}
	knl_memcpy(nb, a, (INT)len);
	nb[0] = (UB)len;
	nb[1] = (UB)af;
	return E_OK;
}

/*
 * An address answered, from NetBSD's form: as much of it as there is
 * room for, and *p_len the length that went in.
 */
LOCAL void sa_out( UB *nb, socklen_t nblen, struct sockaddr *a, socklen_t *p_len )
{
	socklen_t	n;

	if ( a == NULL || p_len == NULL ) {
		return;
	}
	if ( nblen > SA_MAX ) {
		nblen = SA_MAX;
	}
	nb[1] = (UB)af_out(nb[1]);
	nb[0] = (UB)nblen;
	n = ( *p_len < nblen ) ? *p_len : nblen;
	knl_memcpy(a, nb, (INT)n);
	*p_len = n;
}

/* ---------------------------------------------------------------- flags */

LOCAL INT msg_in( INT f )
{
	INT	nb = 0;

	if ( f & MSG_PEEK )	nb |= NB_MSG_PEEK;
	if ( f & MSG_WAITALL )	nb |= NB_MSG_WAITALL;
	if ( f & MSG_OOB )	nb |= NB_MSG_OOB;
	if ( f & MSG_DONTWAIT )	nb |= NB_MSG_DONTWAIT;
	return nb;			/* MSG_MORE: nothing to say it with */
}

LOCAL INT msgflags_out( INT nb )
{
	INT	f = 0;

	if ( nb & NB_MSG_TRUNC )	f |= MSG_TRUNC;
	if ( nb & NB_MSG_CTRUNC )	f |= MSG_CTRUNC;
	return f;
}

LOCAL INT poll_in( INT ev )
{
	INT	nb = 0;

	if ( ev & POLLIN )	nb |= NB_POLLIN;
	if ( ev & POLLOUT )	nb |= NB_POLLOUT;
	if ( ev & POLLRDNORM )	nb |= NB_POLLRDNORM;
	if ( ev & POLLRDBAND )	nb |= NB_POLLRDBAND;
	if ( ev & POLLPRI )	nb |= NB_POLLPRI;
	if ( ev & POLLWRNORM )	nb |= NB_POLLOUT;
	if ( ev & POLLWRBAND )	nb |= NB_POLLWRBAND;
	return nb;
}

LOCAL INT poll_out( INT nb, INT asked )
{
	INT	ev = 0;

	if ( nb & NB_POLLIN )		ev |= POLLIN;
	if ( nb & NB_POLLRDNORM )	ev |= POLLRDNORM;
	if ( nb & NB_POLLRDBAND )	ev |= POLLRDBAND;
	if ( nb & NB_POLLPRI )		ev |= POLLPRI;
	if ( nb & NB_POLLOUT )		ev |= ( asked & ( POLLOUT | POLLWRNORM ) );
	if ( nb & NB_POLLWRBAND )	ev |= POLLWRBAND;
	if ( nb & NB_POLLERR )		ev |= POLLERR;
	if ( nb & NB_POLLHUP )		ev |= POLLHUP;
	if ( nb & NB_POLLNVAL )		ev |= POLLNVAL;
	return ev;
}

/* ---------------------------------------------------------------- calls */

EXPORT INT knl_so_socket_as( INT domain, INT type, INT protocol, ID owner )
{
	D	a[3], fd;
	INT	af, e, s;

	if ( !so_started ) {
		return E_OBJ;
	}
	af = af_in(domain);
	if ( af <= 0 ) {
		return E_NOSPT;
	}
	a[0] = af; a[1] = type; a[2] = protocol;
	e = nb_call(NB_SYS___socket30, a, 3, &fd);
	if ( e != 0 ) {
		return knl_so_nb_er(e);
	}
	s = so_new((INT)fd, owner);
	if ( s >= 0 ) {
		knl_so_obj_made(s, owner, domain, type, protocol);
	}
	return s;
}

EXPORT INT knl_so_accept_as( INT s, struct sockaddr *addr, socklen_t *addrlen, ID owner )
{
	UB		nb[SA_MAX];
	UINT		nblen = SA_MAX;
	D		a[3], fd;
	INT		e, ns;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	a[1] = A(nb); a[2] = A(&nblen);
	e = nb_call(NB_SYS_accept, a, 3, &fd);
	if ( e != 0 ) {
		return knl_so_nb_er(e);
	}
	sa_out(nb, nblen, addr, addrlen);
	ns = so_new((INT)fd, owner);
	if ( ns >= 0 ) {
		knl_so_obj_accepted(ns, s, owner);
	}
	return ns;
}

EXPORT INT so_socket( INT domain, INT type, INT protocol )
{
	return knl_so_socket_as(domain, type, protocol, 0);
}

EXPORT INT so_accept( INT s, struct sockaddr *addr, socklen_t *addrlen )
{
	return knl_so_accept_as(s, addr, addrlen, 0);
}

/*
 * The object goes first, then the number: once either is free the
 * number may come round again, and it must not find the object of the
 * socket before it.
 */
EXPORT ER so_close( INT s )
{
	UINT	imask;
	INT	fd;

	if ( so_fd(s) < 0 ) {
		return E_ID;
	}
	knl_so_obj_gone(s);
	ISpinLock(&so_lock, &imask);
	fd = so_fd(s);
	if ( fd >= 0 ) {
		so_tab[s].fdp1 = 0;
		so_tab[s].owner = 0;
	}
	ISpinUnlock(&so_lock, &imask);
	if ( fd < 0 ) {
		return E_ID;
	}
	nb_close(fd);
	return E_OK;
}

/* bind and connect: the address in, nothing out */
LOCAL ER addr_call( INT num, INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	UB	nb[SA_MAX];
	D	a[3];
	ER	er;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	er = sa_in(name, namelen, nb);
	if ( er < E_OK ) {
		return er;
	}
	a[1] = A(nb); a[2] = namelen;
	return knl_so_nb_er(nb_call(num, a, 3, NULL));
}

EXPORT ER so_bind( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	return addr_call(NB_SYS_bind, s, name, namelen);
}

EXPORT ER so_connect( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	return addr_call(NB_SYS_connect, s, name, namelen);
}

/*
 * The backlog is taken as the largest the stack allows, whatever is
 * asked: connections waiting to be accepted have never been limited by
 * it here, and a caller that gives 1 still gets every one that comes.
 */
EXPORT ER so_listen( INT s, INT backlog )
{
	D	a[2];

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	a[1] = NB_SOMAXCONN;
	return knl_so_nb_er(nb_call(NB_SYS_listen, a, 2, NULL));
}

EXPORT ER so_shutdown( INT s, INT how )
{
	D	a[2];

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	a[1] = how;
	return knl_so_nb_er(nb_call(NB_SYS_shutdown, a, 2, NULL));
}

EXPORT INT so_sendto( INT s, CONST void *data, SZ size, INT flags,
		      CONST struct sockaddr *to, socklen_t tolen )
{
	UB	nb[SA_MAX];
	D	a[6], n;
	INT	e;
	ER	er;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( size < 0 ) {
		return E_PAR;
	}
	a[1] = A(data); a[2] = size; a[3] = msg_in(flags) | NB_MSG_NOSIGNAL;
	a[4] = 0; a[5] = 0;
	if ( to != NULL ) {
		er = sa_in(to, tolen, nb);
		if ( er < E_OK ) {
			return er;
		}
		a[4] = A(nb); a[5] = tolen;
	}
	e = nb_call(NB_SYS_sendto, a, 6, &n);
	return ( e != 0 ) ? (INT)knl_so_nb_er(e) : (INT)n;
}

EXPORT INT so_recvfrom( INT s, void *mem, SZ size, INT flags,
			struct sockaddr *from, socklen_t *fromlen )
{
	UB	nb[SA_MAX];
	UINT	nblen = SA_MAX;
	D	a[6], n;
	INT	e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( size < 0 ) {
		return E_PAR;
	}
	a[1] = A(mem); a[2] = size; a[3] = msg_in(flags);
	a[4] = ( from != NULL ) ? A(nb) : 0;
	a[5] = ( from != NULL ) ? A(&nblen) : 0;
	e = nb_call(NB_SYS_recvfrom, a, 6, &n);
	if ( e != 0 ) {
		return (INT)knl_so_nb_er(e);
	}
	if ( from != NULL ) {
		sa_out(nb, nblen, from, fromlen);
	}
	return (INT)n;
}

EXPORT INT so_send( INT s, CONST void *data, SZ size, INT flags )
{
	return so_sendto(s, data, size, flags, NULL, 0);
}

EXPORT INT so_recv( INT s, void *mem, SZ size, INT flags )
{
	return so_recvfrom(s, mem, size, flags, NULL, NULL);
}

EXPORT INT so_read( INT s, void *mem, SZ size )
{
	return so_recvfrom(s, mem, size, 0, NULL, NULL);
}

EXPORT INT so_write( INT s, CONST void *data, SZ size )
{
	return so_sendto(s, data, size, 0, NULL, 0);
}

/*
 * The messages: the address goes through NetBSD's form; ancillary data
 * is not carried, as it never was.
 */
typedef struct {
	void		*msg_name;
	UINT		msg_namelen;
	void		*msg_iov;
	INT		msg_iovlen;
	void		*msg_control;
	UINT		msg_controllen;
	INT		msg_flags;
} NB_MSGHDR;

EXPORT INT so_sendmsg( INT s, CONST struct msghdr *msg, INT flags )
{
	NB_MSGHDR	m;
	UB		nb[SA_MAX];
	D		a[3], n;
	INT		e;
	ER		er;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( msg == NULL ) {
		return E_PAR;
	}
	m.msg_name = NULL;
	m.msg_namelen = 0;
	if ( msg->msg_name != NULL ) {
		er = sa_in((CONST struct sockaddr *)msg->msg_name, msg->msg_namelen, nb);
		if ( er < E_OK ) {
			return er;
		}
		m.msg_name = nb;
		m.msg_namelen = msg->msg_namelen;
	}
	m.msg_iov = msg->msg_iov;		/* iovec is laid out the same */
	m.msg_iovlen = msg->msg_iovlen;
	m.msg_control = NULL;
	m.msg_controllen = 0;
	m.msg_flags = 0;
	a[1] = A(&m); a[2] = msg_in(flags) | NB_MSG_NOSIGNAL;
	e = nb_call(NB_SYS_sendmsg, a, 3, &n);
	return ( e != 0 ) ? (INT)knl_so_nb_er(e) : (INT)n;
}

EXPORT INT so_recvmsg( INT s, struct msghdr *msg, INT flags )
{
	NB_MSGHDR	m;
	UB		nb[SA_MAX];
	D		a[3], n;
	INT		e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( msg == NULL ) {
		return E_PAR;
	}
	m.msg_name = ( msg->msg_name != NULL ) ? nb : NULL;
	m.msg_namelen = ( msg->msg_name != NULL ) ? SA_MAX : 0;
	m.msg_iov = msg->msg_iov;
	m.msg_iovlen = msg->msg_iovlen;
	m.msg_control = NULL;
	m.msg_controllen = 0;
	m.msg_flags = 0;
	a[1] = A(&m); a[2] = msg_in(flags);
	e = nb_call(NB_SYS_recvmsg, a, 3, &n);
	if ( e != 0 ) {
		return (INT)knl_so_nb_er(e);
	}
	if ( msg->msg_name != NULL ) {
		sa_out(nb, m.msg_namelen, (struct sockaddr *)msg->msg_name, &msg->msg_namelen);
	}
	msg->msg_controllen = 0;
	msg->msg_flags = msgflags_out(m.msg_flags);
	return (INT)n;
}

EXPORT INT knl_so_nb_recv_hlim( INT s, void *mem, SZ size, struct sockaddr *from,
				socklen_t *fromlen, INT *p_hlim )
{
	NB_MSGHDR	m;
	struct {
		void	*base;
		UD	len;
	}		iov;
	UB		nb[SA_MAX];
	UD		ctl[8];			/* the ancillary data, aligned as it is laid out */
	D		a[3], n;
	INT		e;

	*p_hlim = -1;
	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	iov.base = mem;
	iov.len = (UD)size;
	m.msg_name = nb;
	m.msg_namelen = SA_MAX;
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	m.msg_flags = 0;
	a[1] = A(&m); a[2] = NB_MSG_DONTWAIT;
	e = nb_call(NB_SYS_recvmsg, a, 3, &n);
	if ( e != 0 ) {
		return (INT)knl_so_nb_er(e);
	}
	if ( m.msg_controllen > 0 ) {
		*p_hlim = rump_tsnet_cmsg_hlim(ctl, m.msg_controllen);
	}
	sa_out(nb, m.msg_namelen, from, fromlen);
	return (INT)n;
}

/*
 * One datagram (or what has arrived of a stream) with the type of
 * service or traffic class that came with it in the ancillary data
 * (IP_RECVTOS, IPV6_RECVTCLASS)
 */
EXPORT INT so_recvtos( INT s, void *mem, SZ size, INT flags,
		       struct sockaddr *from, socklen_t *fromlen, INT *p_tos )
{
	NB_MSGHDR	m;
	struct {
		void	*base;
		UD	len;
	}		iov;
	UB		nb[SA_MAX];
	UD		ctl[16];		/* the ancillary data, aligned as it is laid out */
	D		a[3], n;
	INT		e;

	*p_tos = -1;
	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( size < 0 ) {
		return E_PAR;
	}
	iov.base = mem;
	iov.len = (UD)size;
	m.msg_name = ( from != NULL ) ? nb : NULL;
	m.msg_namelen = ( from != NULL ) ? SA_MAX : 0;
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	m.msg_flags = 0;
	a[1] = A(&m); a[2] = msg_in(flags);
	e = nb_call(NB_SYS_recvmsg, a, 3, &n);
	if ( e != 0 ) {
		return (INT)knl_so_nb_er(e);
	}
	if ( m.msg_controllen > 0 ) {
		*p_tos = rump_tsnet_cmsg_tos(ctl, m.msg_controllen);
	}
	if ( from != NULL ) {
		sa_out(nb, m.msg_namelen, from, fromlen);
	}
	return (INT)n;
}

/* ---------------------------------------------------------------- waiting */

typedef struct {
	INT	fd;
	short	events;
	short	revents;
} NB_POLLFD;

/*
 * poll over the rump kernel's descriptors. A number that is no socket
 * answers POLLNVAL. ms < 0 waits with no limit.
 */
LOCAL INT nb_poll( struct pollfd *fds, INT nfds, INT ms )
{
	NB_POLLFD	local[16], *p = local;
	D		a[3], n;
	INT		i, bad = 0, e;

	if ( nfds > (INT)( sizeof(local) / sizeof(local[0]) ) ) {
		p = (NB_POLLFD *)Kmalloc(sizeof(NB_POLLFD) * (size_t)nfds);
		if ( p == NULL ) {
			return E_NOMEM;
		}
	}
	for ( i = 0; i < nfds; i++ ) {
		p[i].fd = so_fd(fds[i].fd);
		p[i].events = (short)poll_in(fds[i].events);
		p[i].revents = 0;
		fds[i].revents = 0;
		if ( p[i].fd < 0 ) {
			fds[i].revents = POLLNVAL;
			bad++;
		}
	}
	a[0] = A(p); a[1] = nfds; a[2] = ( bad > 0 ) ? 0 : ms;
	e = nb_call(NB_SYS_poll, a, 3, &n);
	if ( e == 0 ) {
		for ( i = 0; i < nfds; i++ ) {
			if ( p[i].fd >= 0 ) {
				fds[i].revents = (short)poll_out(p[i].revents, fds[i].events);
			}
		}
		n += bad;
	}
	if ( p != local ) {
		Kfree(p);
	}
	return ( e != 0 ) ? (INT)knl_so_nb_er(e) : (INT)n;
}

/* The same wait over a list of sockets; the timeout as so_select's */
EXPORT INT so_poll( struct pollfd *fds, INT nfds, TMO tmout )
{
	if ( nfds < 0 || ( nfds > 0 && fds == NULL ) ) {
		return E_PAR;
	}
	if ( tmout == TMO_FEVR ) {
		tmout = -1;
	} else if ( tmout == TMO_POL ) {
		tmout = 0;
	} else if ( tmout < 0 ) {
		return E_PAR;
	}
	return nb_poll(fds, nfds, (INT)tmout);
}

/*
 * The timeout is the kernel's, so TMO_FEVR is no limit at all and
 * TMO_POL is a look without waiting. The answer is how many of the bits
 * asked for are set.
 */
EXPORT INT so_select( INT maxfdp1, fd_set *readset, fd_set *writeset,
		      fd_set *exceptset, TMO tmout )
{
	struct pollfd	p[SO_MAX];
	INT		s, n = 0, k, ms;

	if ( tmout == TMO_FEVR ) {
		ms = -1;
	} else if ( tmout == TMO_POL ) {
		ms = 0;
	} else if ( tmout < 0 ) {
		return E_PAR;
	} else {
		ms = (INT)tmout;
	}
	if ( maxfdp1 < 0 || maxfdp1 > SO_MAX ) {
		return E_PAR;
	}
	for ( s = 0; s < maxfdp1; s++ ) {
		INT	ev = 0;

		if ( readset != NULL && FD_ISSET(s, readset) ) ev |= POLLIN;
		if ( writeset != NULL && FD_ISSET(s, writeset) ) ev |= POLLOUT;
		if ( exceptset != NULL && FD_ISSET(s, exceptset) ) ev |= POLLPRI;
		if ( ev == 0 ) continue;
		if ( so_fd(s) < 0 ) {
			return E_ID;
		}
		p[n].fd = s;
		p[n].events = (short)ev;
		p[n].revents = 0;
		n++;
	}
	k = nb_poll(p, n, ms);
	if ( k < 0 ) {
		return k;
	}
	if ( readset != NULL ) FD_ZERO(readset);
	if ( writeset != NULL ) FD_ZERO(writeset);
	if ( exceptset != NULL ) FD_ZERO(exceptset);
	k = 0;
	for ( s = 0; s < n; s++ ) {
		INT	r = p[s].revents;

		if ( readset != NULL && ( p[s].events & POLLIN ) && ( r & ( POLLIN | POLLHUP | POLLERR ) ) ) {
			FD_SET(p[s].fd, readset);
			k++;
		}
		if ( writeset != NULL && ( p[s].events & POLLOUT ) && ( r & ( POLLOUT | POLLHUP | POLLERR ) ) ) {
			FD_SET(p[s].fd, writeset);
			k++;
		}
		if ( exceptset != NULL && ( p[s].events & POLLPRI ) && ( r & POLLPRI ) ) {
			FD_SET(p[s].fd, exceptset);
			k++;
		}
	}
	return k;
}

/* ---------------------------------------------------------------- options */

/* NetBSD's level and name of an option; -1 when the stack has none such */
LOCAL BOOL opt_in( INT level, INT optname, INT *p_level, INT *p_name )
{
	switch ( level ) {
	case SOL_SOCKET:
		*p_level = NB_SOL_SOCKET;
		switch ( optname ) {
		case SO_SNDTIMEO:	*p_name = NB_SO_SNDTIMEO; return TRUE;
		case SO_RCVTIMEO:	*p_name = NB_SO_RCVTIMEO; return TRUE;
		case SO_CONTIMEO:
		case SO_NO_CHECK:
		case SO_BINDTODEVICE:	return FALSE;
		default:		*p_name = optname; return TRUE;	/* the same numbers */
		}
	case IPPROTO_TCP:
		*p_level = IPPROTO_TCP;
		switch ( optname ) {
		case TCP_NODELAY:	*p_name = NB_TCP_NODELAY; return TRUE;
		case TCP_KEEPALIVE:
		case TCP_KEEPIDLE:	*p_name = NB_TCP_KEEPIDLE; return TRUE;
		case TCP_KEEPINTVL:	*p_name = NB_TCP_KEEPINTVL; return TRUE;
		case TCP_KEEPCNT:	*p_name = NB_TCP_KEEPCNT; return TRUE;
		default:		return FALSE;
		}
	case IPPROTO_IP:
		*p_level = IPPROTO_IP;
		switch ( optname ) {
		case IP_TOS:			*p_name = NB_IP_TOS; return TRUE;
		case IP_TTL:			*p_name = NB_IP_TTL; return TRUE;
		case IP_MULTICAST_IF:		*p_name = NB_IP_MULTICAST_IF; return TRUE;
		case IP_MULTICAST_TTL:		*p_name = NB_IP_MULTICAST_TTL; return TRUE;
		case IP_MULTICAST_LOOP:		*p_name = NB_IP_MULTICAST_LOOP; return TRUE;
		case IP_ADD_MEMBERSHIP:		*p_name = NB_IP_ADD_MEMBERSHIP; return TRUE;
		case IP_DROP_MEMBERSHIP:	*p_name = NB_IP_DROP_MEMBERSHIP; return TRUE;
		/*
		 * NetBSD has no option for it: the TTL is asked for in its
		 * stead, and the interface component puts the TOS beside it
		 * (rumpcomp/tsnet_conf.c); the TTL itself is not handed over
		 */
		case IP_RECVTOS:		*p_name = NB_IP_RECVTTL; return TRUE;
		default:			return FALSE;
		}
	case IPPROTO_IPV6:
		*p_level = IPPROTO_IPV6;
		switch ( optname ) {
		case IPV6_V6ONLY:	*p_name = NB_IPV6_V6ONLY; return TRUE;
		case IPV6_CHECKSUM:	*p_name = NB_IPV6_CHECKSUM; return TRUE;
		case IPV6_JOIN_GROUP:	*p_name = NB_IPV6_JOIN_GROUP; return TRUE;
		case IPV6_LEAVE_GROUP:	*p_name = NB_IPV6_LEAVE_GROUP; return TRUE;
		case IPV6_UNICAST_HOPS:	*p_name = NB_IPV6_UNICAST_HOPS; return TRUE;
		case IPV6_MULTICAST_IF:	*p_name = NB_IPV6_MULTICAST_IF; return TRUE;
		case IPV6_MULTICAST_HOPS: *p_name = NB_IPV6_MULTICAST_HOPS; return TRUE;
		case IPV6_MULTICAST_LOOP: *p_name = NB_IPV6_MULTICAST_LOOP; return TRUE;
		case IPV6_RECVHOPLIMIT:	*p_name = NB_IPV6_RECVHOPLIMIT; return TRUE;
		case IPV6_RECVTCLASS:	*p_name = NB_IPV6_RECVTCLASS; return TRUE;
		case IPV6_TCLASS:	*p_name = NB_IPV6_TCLASS; return TRUE;
		default:		return FALSE;
		}
	default:
		*p_level = level;
		*p_name = optname;
		return TRUE;
	}
}

/* NetBSD's struct timeval: seconds in 64 bits, microseconds in 32 */
typedef struct {
	D	sec;
	INT	usec;
	INT	pad;
} NB_TIMEVAL;

EXPORT ER so_getsockopt( INT s, INT level, INT optname,
			 void *optval, socklen_t *optlen )
{
	UB	v[64];
	UINT	len;
	D	a[5];
	INT	nl, nn, e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( optval == NULL || optlen == NULL ) {
		return E_PAR;
	}
	if ( !opt_in(level, optname, &nl, &nn) ) {
		return knl_so_errcode(ENOPROTOOPT);
	}
	len = sizeof(v);
	a[1] = nl; a[2] = nn; a[3] = A(v); a[4] = A(&len);
	e = nb_call(NB_SYS_getsockopt, a, 5, NULL);
	if ( e != 0 ) {
		return knl_so_nb_er(e);
	}

	if ( nl == NB_SOL_SOCKET && ( nn == NB_SO_SNDTIMEO || nn == NB_SO_RCVTIMEO ) ) {
		NB_TIMEVAL	*t = (NB_TIMEVAL *)v;
		struct timeval	tv;

		tv.tv_sec = (long)t->sec;
		tv.tv_usec = (long)t->usec;
		len = ( *optlen < sizeof(tv) ) ? *optlen : sizeof(tv);
		knl_memcpy(optval, &tv, (INT)len);
		*optlen = len;
		return E_OK;
	}
	if ( nl == NB_SOL_SOCKET && nn == NB_SO_ERROR && len >= sizeof(INT) ) {
		*(INT *)v = nb_errno(*(INT *)v);
	}
	if ( level == IPPROTO_TCP && optname == TCP_KEEPALIVE && len >= sizeof(INT) ) {
		*(INT *)v *= 1000;		/* asked for in ms */
	}
	if ( len > *optlen ) {
		len = *optlen;
	}
	knl_memcpy(optval, v, (INT)len);
	*optlen = len;
	return E_OK;
}

EXPORT ER so_setsockopt( INT s, INT level, INT optname,
			 CONST void *optval, socklen_t optlen )
{
	UB	v[64];
	D	a[5];
	INT	nl, nn, e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( ( optval == NULL && optlen > 0 ) || optlen > sizeof(v) ) {
		return E_PAR;
	}
	if ( !opt_in(level, optname, &nl, &nn) ) {
		return knl_so_errcode(ENOPROTOOPT);
	}
	if ( optlen > 0 ) {
		knl_memcpy(v, optval, (INT)optlen);
	}

	if ( nl == NB_SOL_SOCKET && ( nn == NB_SO_SNDTIMEO || nn == NB_SO_RCVTIMEO ) ) {
		CONST struct timeval	*tv = (CONST struct timeval *)optval;
		NB_TIMEVAL		t;

		if ( optlen < sizeof(*tv) ) {
			return E_PAR;
		}
		t.sec = tv->tv_sec;
		t.usec = (INT)tv->tv_usec;
		t.pad = 0;
		knl_memcpy(v, &t, sizeof(t));
		optlen = sizeof(t);
	}
	if ( level == IPPROTO_TCP && optname == TCP_KEEPALIVE && optlen >= sizeof(INT) ) {
		INT	ms = *(INT *)v;

		*(INT *)v = ( ms + 999 ) / 1000;	/* given in ms */
	}
	a[1] = nl; a[2] = nn; a[3] = A(v); a[4] = optlen;
	e = nb_call(NB_SYS_setsockopt, a, 5, NULL);
	if ( e == 0 && nl == NB_SOL_SOCKET && nn == SO_REUSEADDR ) {
		/*
		 * Datagram sockets that both ask for it share their port, as
		 * they always did; NetBSD wants SO_REUSEPORT for that.
		 */
		INT	type = 0;
		UINT	tlen = sizeof(type);
		D	g[5];

		g[0] = a[0]; g[1] = NB_SOL_SOCKET; g[2] = NB_SO_TYPE; g[3] = A(&type); g[4] = A(&tlen);
		if ( nb_call(NB_SYS_getsockopt, g, 5, NULL) == 0 && type == SOCK_DGRAM ) {
			a[2] = NB_SO_REUSEPORT;
			(void)nb_call(NB_SYS_setsockopt, a, 5, NULL);
		}
	}
	return knl_so_nb_er(e);
}

/* getsockname and getpeername: an address out */
LOCAL ER name_call( INT num, INT s, struct sockaddr *name, socklen_t *namelen )
{
	UB	nb[SA_MAX];
	UINT	nblen = SA_MAX;
	D	a[3];
	INT	e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( name == NULL || namelen == NULL ) {
		return E_PAR;
	}
	a[1] = A(nb); a[2] = A(&nblen);
	e = nb_call(num, a, 3, NULL);
	if ( e != 0 ) {
		return knl_so_nb_er(e);
	}
	sa_out(nb, nblen, name, namelen);
	return E_OK;
}

EXPORT ER so_getsockname( INT s, struct sockaddr *name, socklen_t *namelen )
{
	return name_call(NB_SYS_getsockname, s, name, namelen);
}

EXPORT ER so_getpeername( INT s, struct sockaddr *name, socklen_t *namelen )
{
	return name_call(NB_SYS_getpeername, s, name, namelen);
}

/*
 * Only the two there have always been: how much has arrived (FIONREAD)
 * and whether a call may block (FIONBIO), each an int.
 */
EXPORT ER so_ioctl( INT s, INT cmd, void *argp )
{
	D	a[3];

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( argp == NULL ) {
		return E_PAR;
	}
	if ( (UW)cmd == (UW)FIONREAD ) {
		return knl_so_nb_er(rump_tsnet_fionread((int)a[0], (int *)argp));
	} else if ( (UW)cmd == (UW)FIONBIO ) {
		a[1] = (D)NB_FIONBIO;
	} else {
		return knl_so_errcode(ENOSYS);
	}
	a[2] = A(argp);
	return knl_so_nb_er(nb_call(NB_SYS_ioctl, a, 3, NULL));
}

/*
 * F_GETFL answers the flags as they have always read (O_NONBLOCK, with
 * O_RDWR), the rest whether it worked. F_SETFL takes only O_NONBLOCK.
 */
EXPORT ER so_fcntl( INT s, INT cmd, INT val )
{
	D	a[3], r;
	INT	e;

	a[0] = so_fd(s);
	if ( a[0] < 0 ) {
		return E_ID;
	}
	if ( cmd == F_GETFL ) {
		a[1] = NB_F_GETFL; a[2] = 0;
		e = nb_call(NB_SYS_fcntl, a, 3, &r);
		if ( e != 0 ) {
			return knl_so_nb_er(e);
		}
		return ( ( r & NB_O_NONBLOCK ) ? SO_O_NONBLOCK : 0 ) | SO_O_RDWR;
	}
	if ( cmd == F_SETFL ) {
		val &= ~( SO_O_RDONLY | SO_O_WRONLY | SO_O_RDWR );
		if ( ( val & ~SO_O_NONBLOCK ) != 0 ) {
			return knl_so_errcode(ENOSYS);
		}
		a[1] = NB_F_GETFL; a[2] = 0;
		e = nb_call(NB_SYS_fcntl, a, 3, &r);
		if ( e != 0 ) {
			return knl_so_nb_er(e);
		}
		r = ( val & SO_O_NONBLOCK ) ? ( r | NB_O_NONBLOCK ) : ( r & ~NB_O_NONBLOCK );
		a[1] = NB_F_SETFL; a[2] = r;
		return knl_so_nb_er(nb_call(NB_SYS_fcntl, a, 3, NULL));
	}
	return knl_so_errcode(ENOSYS);
}

/* ---------------------------------------------------------------- names */

#define RA_MAX		16		/* addresses of a name taken from the server */

/* A numeric IPv4 address a.b.c.d, in network byte order */
LOCAL BOOL ip4_parse( CONST char *s, UW *p_addr )
{
	UW	a = 0, part;
	INT	i, dots = 0, digits;

	for ( i = 0; ; ) {
		part = 0;
		digits = 0;
		while ( s[i] >= '0' && s[i] <= '9' ) {
			part = part * 10 + (UW)( s[i] - '0' );
			if ( part > 255 || ++digits > 3 ) return FALSE;
			i++;
		}
		if ( digits == 0 ) return FALSE;
		a = ( a << 8 ) | part;
		if ( s[i] == '\0' ) break;
		if ( s[i] != '.' || ++dots > 3 ) return FALSE;
		i++;
	}
	if ( dots != 3 ) return FALSE;
	*p_addr = so_htonl(a);
	return TRUE;
}

LOCAL INT hex_val( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/*
 * A numeric IPv6 address (RFC 4291 2.2: groups, "::", an IPv4 address
 * at the end), and after '%' the zone of a link-local one: an
 * interface's name or number, 0 for none.
 */
LOCAL BOOL ip6_parse( CONST char *s, UB *a, UINT *p_scope )
{
	UH	g[8];
	char	tail[16];
	INT	ng = 0, gap = -1, i = 0, k, n, d, at;
	UW	v, v4;

	*p_scope = 0;
	if ( s[0] == ':' ) {
		if ( s[1] != ':' ) return FALSE;
		gap = 0;
		i = 2;
	}
	while ( s[i] != '\0' && s[i] != '%' ) {
		/* the last 32 bits written as an IPv4 address */
		for ( k = i; s[k] != '\0' && s[k] != '%' && s[k] != ':' && s[k] != '.'; k++ ) ;
		if ( s[k] == '.' ) {
			for ( n = 0; s[i + n] != '\0' && s[i + n] != '%'; n++ ) {
				if ( n >= (INT)sizeof(tail) - 1 ) return FALSE;
				tail[n] = s[i + n];
			}
			tail[n] = '\0';
			if ( ng > 6 || !ip4_parse(tail, &v4) ) return FALSE;
			v4 = so_ntohl(v4);
			g[ng++] = (UH)( v4 >> 16 );
			g[ng++] = (UH)v4;
			i += n;
			break;
		}
		v = 0;
		for ( n = 0; ( d = hex_val(s[i]) ) >= 0; n++, i++ ) {
			if ( n == 4 ) return FALSE;
			v = v * 16 + (UW)d;
		}
		if ( n == 0 || ng == 8 ) return FALSE;
		g[ng++] = (UH)v;
		if ( s[i] == ':' ) {
			i++;
			if ( s[i] == ':' ) {
				if ( gap >= 0 ) return FALSE;
				gap = ng;
				i++;
			} else if ( s[i] == '\0' || s[i] == '%' ) {
				return FALSE;		/* a colon at the end */
			}
		} else if ( s[i] != '\0' && s[i] != '%' ) {
			return FALSE;
		}
	}
	if ( ( gap < 0 && ng != 8 ) || ( gap >= 0 && ng > 7 ) ) return FALSE;
	if ( gap < 0 ) gap = ng;
	knl_memset(a, 0, 16);
	for ( k = 0; k < ng; k++ ) {
		at = ( k < gap ) ? k : 8 - ( ng - k );
		a[at * 2] = (UB)( g[k] >> 8 );
		a[at * 2 + 1] = (UB)g[k];
	}
	if ( s[i] == '%' ) {
		CONST char	*z = s + i + 1;

		v = 0;
		for ( k = 0; z[k] >= '0' && z[k] <= '9' && v < 0x10000; k++ ) v = v * 10 + (UW)( z[k] - '0' );
		if ( k == 0 || z[k] != '\0' ) {
			v = so_ifnametoindex(z);
		}
		if ( v == 0 ) return FALSE;
		*p_scope = v;
	}
	return TRUE;
}

EXPORT void knl_so_ip6_text( char *buf, SZ len, CONST UB *a )
{
	CONST char	*hex = "0123456789abcdef";
	char		t[48];
	INT		i, j, n = 0, best = -1, blen = 1, run;
	UINT		gv;
	BOOL		any;

	/* the longest run of two or more zero groups, the first of equals */
	for ( i = 0; i < 8; i = j ) {
		for ( j = i; j < 8 && a[j * 2] == 0 && a[j * 2 + 1] == 0; j++ ) ;
		run = j - i;
		if ( run > blen ) {
			best = i;
			blen = run;
		}
		if ( j == i ) j++;
	}
	for ( i = 0; i < 8; i++ ) {
		if ( i == best ) {
			t[n++] = ':';
			if ( i == 0 ) t[n++] = ':';
			i += blen - 1;
			continue;
		}
		/* an IPv4 address mapped (::ffff:a.b.c.d) shows its last 32 bits as one */
		if ( i == 6 && best == 0 && blen == 5 && a[10] == 0xff && a[11] == 0xff ) {
			for ( j = 12; j < 16; j++ ) {
				UINT	v = a[j];

				if ( v >= 100 ) t[n++] = (char)( '0' + v / 100 );
				if ( v >= 10 ) t[n++] = (char)( '0' + ( v / 10 ) % 10 );
				t[n++] = (char)( '0' + v % 10 );
				if ( j < 15 ) t[n++] = '.';
			}
			break;
		}
		gv = ( (UINT)a[i * 2] << 8 ) | a[i * 2 + 1];
		any = FALSE;
		for ( j = 12; j >= 0; j -= 4 ) {
			UINT	x = ( gv >> j ) & 0xf;

			if ( x != 0 || any || j == 0 ) {
				t[n++] = hex[x];
				any = TRUE;
			}
		}
		if ( i < 7 ) t[n++] = ':';
	}
	t[n] = '\0';
	for ( i = 0; i < (INT)len - 1 && t[i] != '\0'; i++ ) buf[i] = t[i];
	if ( len > 0 ) buf[i] = '\0';
}

/* ---------------------------------------------------------------- the order of addresses */

/*
 * The addresses of a name are put in the order to try them, after the
 * destination address selection of RFC 6724: those this machine has no
 * source address for come last, then those whose source address is of
 * the same scope and the same kind (by the policy table) come first,
 * and of the rest those of the higher precedence. An IPv6 address with
 * only a link-local or a site-local source address of its own is
 * therefore tried after an IPv4 one.
 */

#define SCOPE_LINK	2
#define SCOPE_SITE	5
#define SCOPE_GLOBAL	14

LOCAL BOOL in_prefix( CONST UB *a, CONST UB *p, INT bits )
{
	INT	i;

	for ( i = 0; i < bits / 8; i++ ) {
		if ( a[i] != p[i] ) return FALSE;
	}
	if ( bits % 8 != 0 ) {
		UB	m = (UB)( 0xff << ( 8 - bits % 8 ) );

		if ( ( a[i] & m ) != ( p[i] & m ) ) return FALSE;
	}
	return TRUE;
}

/* The precedence and label of an IPv6 address (RFC 6724 2.1) */
LOCAL void policy6( CONST UB *a, INT *prec, INT *label )
{
	LOCAL CONST struct {
		UB	p[16];
		INT	bits, prec, label;
	} tab[] = {
		{ { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }, 128, 50, 0 },
		{ { 0,0,0,0,0,0,0,0,0,0,0xff,0xff }, 96, 35, 4 },
		{ { 0x20, 0x02 }, 16, 30, 2 },
		{ { 0x20, 0x01, 0, 0 }, 32, 5, 5 },
		{ { 0xfc }, 7, 3, 13 },
		{ { 0 }, 96, 1, 3 },
		{ { 0xfe, 0xc0 }, 10, 1, 11 },
		{ { 0x3f, 0xfe }, 16, 1, 12 },
	};
	INT	i;

	for ( i = 0; i < (INT)( sizeof(tab) / sizeof(tab[0]) ); i++ ) {
		if ( in_prefix(a, tab[i].p, tab[i].bits) ) {
			*prec = tab[i].prec;
			*label = tab[i].label;
			return;
		}
	}
	*prec = 40;
	*label = 1;
}

LOCAL INT scope6( CONST UB *a )
{
	if ( a[0] == 0xff ) return a[1] & 0x0f;			/* multicast */
	if ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0x80 ) return SCOPE_LINK;
	if ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0xc0 ) return SCOPE_SITE;
	if ( in_prefix(a, (CONST UB *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 128) ) return SCOPE_LINK;
	return SCOPE_GLOBAL;
}

LOCAL INT scope4( CONST UB *a )
{
	if ( a[0] == 127 || ( a[0] == 169 && a[1] == 254 ) ) return SCOPE_LINK;
	return SCOPE_GLOBAL;
}

/* What the machine has to send from */
typedef struct {
	BOOL	v4;			/* an IPv4 address */
	BOOL	gw6;			/* an IPv6 default router */
	INT	n6;
	T_SOIF6	a6[8];			/* the IPv6 addresses that can be used */
} SOURCES;

LOCAL void sources_of( SOURCES *src )
{
	UW	a = 0, m = 0;
	UB	gw[16];
	T_SOIF6	l[8];
	INT	i, n;

	knl_memset(src, 0, sizeof(*src));
	if ( !so_started || !knl_so_nb_card ) {
		return;
	}
	(void)rump_tsnet_getaddr4(SO_IFNAME, &a, &m);
	src->v4 = ( a != 0 );
	src->gw6 = knl_so_gw6(gw);
	n = rump_tsnet_addrs6(SO_IFNAME, l, 8);
	for ( i = 0; i < n && i < 8; i++ ) {
		if ( l[i].flags & ( SO_IF6_TENTATIVE | SO_IF6_DUPLICATED | SO_IF6_DETACHED | SO_IF6_ANYCAST ) ) {
			continue;
		}
		src->a6[src->n6++] = l[i];
	}
}

/* Whether the machine has an address of the family to reach the world with */
LOCAL BOOL family_up( CONST SOURCES *src, INT family )
{
	INT	i;

	if ( family == AF_INET ) {
		return src->v4;
	}
	for ( i = 0; i < src->n6; i++ ) {
		if ( scope6(src->a6[i].addr) == SCOPE_GLOBAL ) return TRUE;
	}
	return FALSE;
}

/* How a destination stands: whether it can be reached, and what decides its place */
typedef struct {
	BOOL	usable;
	BOOL	same_scope;
	BOOL	deprecated;
	BOOL	same_label;
	INT	prec;
	INT	scope;
} RANK;

LOCAL void rank_of( CONST T_SOADDR *d, CONST SOURCES *src, RANK *r )
{
	INT	i, best = -1, bs = 0, s, dlabel, slabel, dummy;

	knl_memset(r, 0, sizeof(*r));
	if ( d->family == AF_INET ) {
		r->scope = scope4(d->addr);
		r->prec = 35;
		r->usable = ( d->addr[0] == 127 ) || src->v4;
		r->same_scope = TRUE;
		r->same_label = TRUE;
		return;
	}
	r->scope = scope6(d->addr);
	policy6(d->addr, &r->prec, &dlabel);
	if ( r->scope == SCOPE_LINK && d->addr[0] != 0xfe ) {
		r->usable = TRUE;			/* ::1 */
		r->same_scope = TRUE;
		r->same_label = TRUE;
		return;
	}
	/* the source: the smallest scope no smaller than the destination's, not deprecated if it can be */
	for ( i = 0; i < src->n6; i++ ) {
		s = scope6(src->a6[i].addr);
		if ( s < r->scope ) continue;
		if ( best < 0 || s < bs
		  || ( s == bs && ( src->a6[best].flags & SO_IF6_DEPRECATED ) && !( src->a6[i].flags & SO_IF6_DEPRECATED ) ) ) {
			best = i;
			bs = s;
		}
	}
	if ( best < 0 ) {
		return;
	}
	policy6(src->a6[best].addr, &dummy, &slabel);
	r->same_scope = ( bs == r->scope );
	r->deprecated = ( ( src->a6[best].flags & SO_IF6_DEPRECATED ) != 0 );
	r->same_label = ( slabel == dlabel );
	r->usable = ( r->scope == SCOPE_LINK ) || src->gw6
		 || in_prefix(d->addr, src->a6[best].addr, src->a6[best].plen);
}

/* Whether a goes before b */
LOCAL BOOL rank_before( CONST RANK *a, CONST RANK *b )
{
	if ( a->usable != b->usable ) return a->usable;
	if ( a->same_scope != b->same_scope ) return a->same_scope;
	if ( a->deprecated != b->deprecated ) return !a->deprecated;
	if ( a->same_label != b->same_label ) return a->same_label;
	if ( a->prec != b->prec ) return ( a->prec > b->prec );
	if ( a->scope != b->scope ) return ( a->scope < b->scope );
	return FALSE;
}

/* The addresses put in order, those that tie keeping theirs */
LOCAL void sort_addrs( T_SOADDR *l, INT n, CONST SOURCES *src )
{
	RANK		r[RA_MAX], rt;
	T_SOADDR	t;
	INT		i, j;

	for ( i = 0; i < n; i++ ) {
		rank_of(&l[i], src, &r[i]);
	}
	for ( i = 1; i < n; i++ ) {
		t = l[i];
		rt = r[i];
		for ( j = i; j > 0 && rank_before(&rt, &r[j - 1]); j-- ) {
			l[j] = l[j - 1];
			r[j] = r[j - 1];
		}
		l[j] = t;
		r[j] = rt;
	}
}

/* ---------------------------------------------------------------- looking up */

/* A name looked up, and with no dot in it looked for in the domain as well */
LOCAL ER lookup( CONST char *name, INT family, T_SOADDR *l, INT *p_n )
{
	char	full[160];
	INT	i, k = 0;
	BOOL	dot = FALSE;
	ER	er;

	er = knl_so_dns_query(name, family, l, RA_MAX, p_n);
	if ( er != E_NOEXS || so_domain[0] == '\0' ) {
		return er;
	}
	for ( i = 0; name[i] != '\0'; i++ ) {
		if ( name[i] == '.' ) dot = TRUE;
	}
	if ( dot || i + 1 + (INT)sizeof(so_domain) >= (INT)sizeof(full) ) {
		return er;
	}
	for ( i = 0; name[i] != '\0'; i++ ) full[k++] = name[i];
	full[k++] = '.';
	for ( i = 0; so_domain[i] != '\0'; i++ ) full[k++] = so_domain[i];
	full[k] = '\0';
	return knl_so_dns_query(full, family, l, RA_MAX, p_n);
}

LOCAL BOOL loopback( CONST T_SOADDR *a )
{
	return ( a->family == AF_INET ) ? ( a->addr[0] == 127 )
	     : in_prefix(a->addr, (CONST UB *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 128);
}

/*
 * The addresses of a name into l (RA_MAX of room), in the order to try
 * them: a number as it is, else through the name servers. 'numeric'
 * refuses a name that is not a number.
 */
LOCAL ER resolve_list( CONST char *name, INT family, INT flags, BOOL numeric,
		       T_SOADDR *l, INT *p_n )
{
	SOURCES	src;
	UW	a4;
	INT	n = 0, i, k;
	ER	er;

	*p_n = 0;
	if ( family != AF_UNSPEC && family != AF_INET && family != AF_INET6 ) {
		return E_NOSPT;
	}
	knl_memset(l, 0, sizeof(T_SOADDR));
	if ( ip4_parse(name, &a4) ) {
		if ( family == AF_INET6 ) return E_NOEXS;
		l[0].family = AF_INET;
		knl_memcpy(l[0].addr, &a4, 4);
		*p_n = 1;
		return E_OK;
	}
	if ( ip6_parse(name, l[0].addr, &l[0].scope) ) {
		if ( family == AF_INET ) return E_NOEXS;
		l[0].family = AF_INET6;
		*p_n = 1;
		return E_OK;
	}
	if ( numeric ) {
		return E_NOEXS;
	}
	sources_of(&src);
	if ( ( flags & SO_RA_ADDRCONFIG ) && !family_up(&src, AF_INET) && !family_up(&src, AF_INET6) ) {
		flags &= ~SO_RA_ADDRCONFIG;	/* no address yet: nothing to choose by */
	}
	if ( ( flags & SO_RA_ADDRCONFIG ) && family == AF_UNSPEC ) {
		/* a family the machine cannot reach the world with is not asked for */
		BOOL	up4 = family_up(&src, AF_INET), up6 = family_up(&src, AF_INET6);

		family = ( up4 && !up6 ) ? AF_INET : ( up6 && !up4 ) ? AF_INET6 : AF_UNSPEC;
	}
	er = lookup(name, family, l, &n);
	if ( er < E_OK ) {
		return er;
	}
	if ( flags & SO_RA_ADDRCONFIG ) {
		for ( i = k = 0; i < n; i++ ) {
			if ( loopback(&l[i]) || family_up(&src, l[i].family) ) l[k++] = l[i];
		}
		n = k;
		if ( n == 0 ) return E_NOEXS;
	}
	sort_addrs(l, n, &src);
	*p_n = n;
	return E_OK;
}

EXPORT INT so_resolveall( CONST char *name, INT family, INT flags, T_SOADDR *addrs, INT max )
{
	T_SOADDR	l[RA_MAX];
	INT		n = 0, i;
	ER		er;

	if ( name == NULL || addrs == NULL || max <= 0 ) {
		return E_PAR;
	}
	er = resolve_list(name, family, flags, FALSE, l, &n);
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < n && i < max; i++ ) {
		addrs[i] = l[i];
	}
	return i;
}

/* One answer of so_getaddrinfo, in one block of its own */
LOCAL struct addrinfo *ai_make( CONST T_SOADDR *a, INT port, CONST struct addrinfo *hints, CONST char *name )
{
	struct addrinfo		*ai;
	struct sockaddr_in	*sin;
	struct sockaddr_in6	*sin6;
	INT			n = 0;
	SZ			size;

	while ( name != NULL && name[n] != '\0' ) n++;
	size = sizeof(struct addrinfo) + sizeof(struct sockaddr_in6) + n + 1;
	ai = (struct addrinfo *)Kcalloc(1, (size_t)size);
	if ( ai == NULL ) {
		return NULL;
	}
	if ( a->family == AF_INET6 ) {
		sin6 = (struct sockaddr_in6 *)( ai + 1 );
		sin6->sin6_len = sizeof(*sin6);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = so_htons((UH)port);
		knl_memcpy(&sin6->sin6_addr, a->addr, 16);
		sin6->sin6_scope_id = a->scope;
		ai->ai_addr = (struct sockaddr *)sin6;
		ai->ai_addrlen = sizeof(*sin6);
	} else {
		sin = (struct sockaddr_in *)( ai + 1 );
		sin->sin_len = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_port = so_htons((UH)port);
		knl_memcpy(&sin->sin_addr.s_addr, a->addr, 4);
		ai->ai_addr = (struct sockaddr *)sin;
		ai->ai_addrlen = sizeof(*sin);
	}
	ai->ai_family = a->family;
	if ( hints != NULL ) {
		ai->ai_socktype = hints->ai_socktype;
		ai->ai_protocol = hints->ai_protocol;
	}
	if ( name != NULL ) {
		ai->ai_canonname = (char *)ai + sizeof(struct addrinfo) + sizeof(struct sockaddr_in6);
		knl_memcpy(ai->ai_canonname, name, n);
		ai->ai_canonname[n] = '\0';
	}
	return ai;
}

/*
 * The addresses for a name, IPv4 and IPv6, in the order to try them. A
 * name that is not already a number needs a DNS server, which a machine
 * on its own does not have; the numeric forms work either way. The
 * service is a port number. With no name, the addresses of any
 * interface (AI_PASSIVE) or of the loopback interface, the IPv4 one
 * first.
 */
EXPORT ER so_getaddrinfo( CONST char *node, CONST char *service,
			  CONST struct addrinfo *hints, struct addrinfo **res )
{
	T_SOADDR	l[RA_MAX];
	struct addrinfo	*ai, **tail = res;
	INT		family = AF_UNSPEC, port = 0, i, n = 0;
	ER		er;

	if ( res == NULL ) {
		return E_PAR;
	}
	*res = NULL;
	if ( node == NULL && service == NULL ) {
		return E_NOEXS;
	}
	if ( hints != NULL ) {
		family = hints->ai_family;
	}
	if ( family != AF_UNSPEC && family != AF_INET && family != AF_INET6 ) {
		return E_NOSPT;
	}
	if ( service != NULL ) {
		for ( i = 0; service[i] != '\0'; i++ ) {
			if ( service[i] < '0' || service[i] > '9' || port > 65535 ) {
				return E_PAR;
			}
			port = port * 10 + ( service[i] - '0' );
		}
		if ( port > 65535 ) {
			return E_PAR;
		}
	}

	if ( node == NULL ) {
		BOOL	passive = ( hints != NULL && ( hints->ai_flags & AI_PASSIVE ) != 0 );

		knl_memset(l, 0, sizeof(T_SOADDR) * 2);
		if ( family != AF_INET6 ) {
			UW	a = passive ? INADDR_ANY : so_htonl(INADDR_LOOPBACK);

			l[n].family = AF_INET;
			knl_memcpy(l[n].addr, &a, 4);
			n++;
		}
		if ( family != AF_INET ) {
			l[n].family = AF_INET6;
			if ( !passive ) l[n].addr[15] = 1;
			n++;
		}
	} else {
		er = resolve_list(node, family,
				  ( hints != NULL && ( hints->ai_flags & AI_ADDRCONFIG ) ) ? SO_RA_ADDRCONFIG : 0,
				  ( hints != NULL && ( hints->ai_flags & AI_NUMERICHOST ) != 0 ), l, &n);
		if ( er < E_OK ) {
			return er;
		}
		if ( n < 1 ) {
			return E_NOEXS;
		}
	}
	for ( i = 0; i < n; i++ ) {
		ai = ai_make(&l[i], port, hints, ( i == 0 ) ? node : NULL);
		if ( ai == NULL ) {
			so_freeaddrinfo(*res);
			*res = NULL;
			return E_NOMEM;
		}
		*tail = ai;
		tail = &ai->ai_next;
	}
	return E_OK;
}

EXPORT void so_freeaddrinfo( struct addrinfo *ai )
{
	struct addrinfo	*next;

	while ( ai != NULL ) {
		next = ai->ai_next;
		Kfree(ai);
		ai = next;
	}
}

/*
 * The IPv4 address of a name, in network byte order: the first IPv4
 * answer, for a caller that wants no more than that.
 */
EXPORT ER so_resolve( CONST char *name, UW *p_addr )
{
	T_SOADDR	a;
	INT		n;

	if ( name == NULL || p_addr == NULL ) {
		return E_PAR;
	}
	n = so_resolveall(name, AF_INET, 0, &a, 1);
	if ( n < 0 ) {
		return n;
	}
	if ( n == 0 || a.family != AF_INET ) {
		return E_NOEXS;
	}
	knl_memcpy(p_addr, a.addr, 4);
	return E_OK;
}

/* One byte of an address as up to three digits, and where that ended */
LOCAL socklen_t put_u8( char *buf, socklen_t at, socklen_t len, UINT v )
{
	char	d[5];
	INT	n = 0;

	do {
		d[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while ( v != 0 );

	while ( n > 0 && at < len - 1 ) {
		buf[at++] = d[--n];
	}

	return at;
}

/*
 * The name of an address, in numbers: a.b.c.d, or an IPv6 address in
 * its shortest form with the zone of a link-local one after '%'. A name
 * from the network would mean asking a server the other way round,
 * which is not carried, so NI_NAMEREQD is refused rather than answered
 * wrongly.
 */
EXPORT ER so_getnameinfo( CONST struct sockaddr *sa, socklen_t salen,
			  char *host, socklen_t hostlen,
			  char *serv, socklen_t servlen, INT flags )
{
	CONST struct sockaddr_in	*sin = (CONST struct sockaddr_in *)sa;
	CONST struct sockaddr_in6	*sin6 = (CONST struct sockaddr_in6 *)sa;
	UW				addr;
	UH				port;
	socklen_t			at;
	INT				i;

	if ( sa == NULL || salen < (socklen_t)sizeof(*sin) ) {
		return E_PAR;
	}
	if ( sa->sa_family == AF_INET6 ) {
		if ( salen < (socklen_t)sizeof(*sin6) ) {
			return E_PAR;
		}
	} else if ( sa->sa_family != AF_INET ) {
		return E_NOSPT;
	}
	if ( (flags & NI_NAMEREQD) != 0 ) {
		return E_NOSPT;		/* no way to ask for a name */
	}

	if ( host != NULL && hostlen > 0 && sa->sa_family == AF_INET6 ) {
		char	t[48 + 1 + 16];

		knl_so_ip6_text(t, 48, sin6->sin6_addr.s6_addr);
		if ( sin6->sin6_scope_id != 0 && scope6(sin6->sin6_addr.s6_addr) == SCOPE_LINK
		  && sin6->sin6_addr.s6_addr[0] == 0xfe ) {
			char	z[16];
			INT	k = (INT)knl_strlen(t);

			t[k++] = '%';
			if ( so_ifindextoname(sin6->sin6_scope_id, z) == E_OK ) {
				for ( i = 0; z[i] != '\0' && k < (INT)sizeof(t) - 1; i++ ) t[k++] = z[i];
				t[k] = '\0';
			} else {
				t[put_u8(t, (socklen_t)k, sizeof(t), sin6->sin6_scope_id & 0xffff)] = '\0';
			}
		}
		for ( i = 0; i < (INT)hostlen - 1 && t[i] != '\0'; i++ ) host[i] = t[i];
		host[i] = '\0';
	} else if ( host != NULL && hostlen > 0 ) {
		addr = so_ntohl(sin->sin_addr.s_addr);
		at = 0;
		for ( i = 3; i >= 0; i-- ) {
			at = put_u8(host, at, hostlen, (addr >> (i * 8)) & 0xff);
			if ( i > 0 && at < hostlen - 1 ) {
				host[at++] = '.';
			}
		}
		host[at] = '\0';
	}
	if ( serv != NULL && servlen > 0 ) {
		port = ( sa->sa_family == AF_INET6 ) ? sin6->sin6_port : sin->sin_port;
		at = put_u8(serv, 0, servlen, so_ntohs(port));
		serv[at] = '\0';
	}

	return E_OK;
}

/* ---------------------------------------------------------------- interfaces */

EXPORT ER so_ifindextoname( UINT ifindex, char *ifname )
{
	char	name[16];

	if ( ifname == NULL ) {
		return E_PAR;
	}
	if ( !so_started || rump_tsnet_ifname((int)ifindex, name) != 0 ) {
		return E_NOEXS;
	}
	knl_strcpy(ifname, name);
	return E_OK;
}

EXPORT UINT so_ifnametoindex( CONST char *ifname )
{
	if ( ifname == NULL || !so_started ) {
		return 0;
	}
	return (UINT)rump_tsnet_ifindex(ifname);
}

/* ---------------------------------------------------------------- not carried */

EXPORT ER so_sockatmark( INT s )
{
	return E_NOSPT;			/* out of band data is not offered */
}

EXPORT ER so_resctl( INT kind, void *arg )
{
	return E_NOSPT;			/* the limits are fixed at build time */
}

EXPORT ER so_break( ID tskid )
{
	return E_NOSPT;			/* nothing breaks a blocked call */
}

EXPORT ER so_gethostname( char *name, SZ namelen )
{
	SZ	i;

	if ( name == NULL || namelen <= 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < namelen - 1 && so_hostname[i] != '\0'; i++ ) {
		name[i] = so_hostname[i];
	}
	name[i] = '\0';

	return E_OK;
}

EXPORT ER so_sethostname( CONST char *name, SZ namelen )
{
	SZ	i;

	if ( name == NULL || namelen <= 0
	  || namelen >= (SZ)sizeof(so_hostname) ) {
		return E_PAR;
	}
	for ( i = 0; i < namelen; i++ ) {
		so_hostname[i] = name[i];
	}
	so_hostname[namelen] = '\0';

	return E_OK;
}

/* ---------------------------------------------------------------- the interface's address */

EXPORT ER so_getifaddr( UW *p_addr, UW *p_mask, UW *p_gw )
{
	UW	addr = 0, mask = 0;

	if ( !so_started || !knl_so_nb_card ) {
		return E_OBJ;
	}
	(void)rump_tsnet_getaddr4(SO_IFNAME, &addr, &mask);
	if ( p_addr != NULL ) *p_addr = addr;
	if ( p_mask != NULL ) *p_mask = mask;
	if ( p_gw != NULL ) *p_gw = ( addr != 0 ) ? so_gw : 0;
	return E_OK;
}

EXPORT ER knl_so_nb_ifaddr( UW addr, UW mask, UW gw, BOOL dhcp )
{
	int	e;

	UW	cur = 0, curmask = 0;

	if ( !knl_so_nb_card ) {
		return E_OBJ;
	}
	/* the same address again (a lease renewed) leaves the connections on it alone */
	(void)rump_tsnet_getaddr4(SO_IFNAME, &cur, &curmask);
	if ( addr != 0 && addr == cur && mask == curmask ) {
		if ( gw == so_gw ) {
			return E_OK;
		}
	} else if ( addr != 0 ) {
		e = rump_tsnet_setaddr4(SO_IFNAME, addr, mask, addr | ~mask);
		if ( e != 0 ) {
			return knl_so_nb_er(e);
		}
	} else {
		e = rump_tsnet_setaddr4(SO_IFNAME, 0, 0, dhcp ? 0xffffffffU : 0);
		if ( e != 0 ) {
			return knl_so_nb_er(e);
		}
	}
	e = rump_tsnet_defroute4(( addr != 0 ) ? gw : 0);
	so_gw = ( addr != 0 && e == 0 ) ? gw : 0;
	return ( e != 0 ) ? knl_so_nb_er(e) : E_OK;
}

EXPORT void knl_so_nb_ifdns( UW dns0, UW dns1 )
{
	ns_iface[0] = dns0;
	ns_iface[1] = dns1;
}

/* The name server to ask: the one the settings name, else the interface's */
EXPORT UW knl_so_dns_server( INT n )
{
	if ( n < 0 || n > 1 ) {
		return 0;
	}
	return ( ns_fixed[n] != 0 ) ? ns_fixed[n] : ns_iface[n];
}

/* The first name server, in network byte order; 0 for none */
EXPORT ER so_getdns( UW *p_dns )
{
	if ( p_dns == NULL ) {
		return E_PAR;
	}
	*p_dns = knl_so_dns_server(0);
	if ( *p_dns == 0 ) {
		*p_dns = knl_so_dns_server(1);
	}
	return E_OK;
}

/* The IPv6 addresses of the card's interface, and the router of the default route */
EXPORT INT so_getifaddr6( T_SOIF6 *list, INT max, UB *gw )
{
	INT	n;

	if ( !so_started || !knl_so_nb_card ) {
		return E_OBJ;
	}
	if ( max < 0 || ( max > 0 && list == NULL ) ) {
		return E_PAR;
	}
	n = rump_tsnet_addrs6(SO_IFNAME, list, max);
	if ( n < 0 ) {
		return E_OBJ;
	}
	if ( gw != NULL && !knl_so_gw6(gw) ) {
		knl_memset(gw, 0, 16);
	}
	return ( n > max ) ? max : n;
}

LOCAL BOOL zero16( CONST UB *a )
{
	INT	i;

	for ( i = 0; i < 16 && a[i] == 0; i++ ) ;
	return ( i == 16 );
}

LOCAL BOOL same16( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 16 && a[i] == b[i]; i++ ) ;
	return ( i == 16 );
}

/* The router the IPv6 default route goes through: the settings' when fixed, else the routers' own */
EXPORT BOOL knl_so_gw6( UB *gw )
{
	switch ( ip6_mode & SO_IP6_MODE ) {
	case SO_IP6_FIXED:
		if ( zero16(ip6_gw) ) return FALSE;
		knl_memcpy(gw, ip6_gw, 16);
		return TRUE;
	case SO_IP6_AUTO:
		return knl_so_slaac_gw(gw);
	default:
		return FALSE;
	}
}

/* The nth IPv6 name server to ask: those the settings name, then those the routers named */
EXPORT ER so_getdns6( INT n, UB *addr )
{
	INT	i;

	if ( addr == NULL || n < 0 ) {
		return E_PAR;
	}
	if ( ( ip6_mode & SO_IP6_MODE ) == SO_IP6_OFF ) {
		return E_NOEXS;
	}
	for ( i = 0; i < 2; i++ ) {
		if ( zero16(ns6_fixed[i]) ) continue;
		if ( n-- == 0 ) {
			knl_memcpy(addr, ns6_fixed[i], 16);
			return E_OK;
		}
	}
	return knl_so_slaac_dns(n, addr) ? E_OK : E_NOEXS;
}

EXPORT ER so_setdns6( INT n, CONST UB *addr )
{
	if ( n < 0 || n > 1 ) {
		return E_PAR;
	}
	if ( addr == NULL ) {
		knl_memset(ns6_fixed[n], 0, 16);
	} else {
		knl_memcpy(ns6_fixed[n], addr, 16);
	}
	return E_OK;
}

EXPORT UINT so_getip6( void )
{
	return ip6_mode;
}

/* An address an interface may be given: not unspecified, loopback, link-local or multicast */
LOCAL BOOL unicast6( CONST UB *a )
{
	static CONST UB	lo[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };

	return !( zero16(a) || same16(a, lo) || a[0] == 0xff || ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0x80 ) );
}

/*
 * IPv6 of the card's interface made as the settings say. What the old
 * settings put there that the new ones do not keep goes first: the
 * routers stop being listened to and what was taken from them is given
 * up, or the fixed address and its route come off. Then IPv6 is turned
 * on or off on the interface, and the new settings are put there.
 */
EXPORT ER so_setip6( UINT mode, CONST UB *addr, INT plen, CONST UB *gw )
{
	UINT	how = mode & SO_IP6_MODE, was = ip6_mode & SO_IP6_MODE;
	BOOL	temp = ( how == SO_IP6_AUTO && ( mode & SO_IP6_TEMP ) != 0 );
	UB	g[16];
	int	e;

	if ( how > SO_IP6_FIXED ) {
		return E_PAR;
	}
	if ( how == SO_IP6_FIXED && ( addr == NULL || plen < 1 || plen > 128 || !unicast6(addr) ) ) {
		return E_PAR;
	}
	if ( !so_started || !knl_so_nb_card ) {
		return E_OBJ;
	}
	knl_memset(g, 0, 16);
	if ( how == SO_IP6_FIXED && gw != NULL ) {
		knl_memcpy(g, gw, 16);
	}
	mode = how | ( temp ? SO_IP6_TEMP : 0 );
	if ( mode == ip6_mode && ( how != SO_IP6_FIXED
	  || ( same16(addr, ip6_addr) && plen == ip6_plen && same16(g, ip6_gw) ) ) ) {
		return E_OK;			/* as it is */
	}

	/* what the old settings put there */
	if ( was == SO_IP6_AUTO && how != SO_IP6_AUTO ) {
		knl_so_slaac_stop(TRUE);
	}
	if ( was == SO_IP6_FIXED ) {
		if ( !zero16(ip6_gw) ) (void)rump_tsnet_defroute6(SO_IFNAME, NULL);
		if ( how != SO_IP6_FIXED || !same16(addr, ip6_addr) || plen != ip6_plen ) {
			(void)rump_tsnet_deladdr6(SO_IFNAME, ip6_addr);
		}
	}
	e = rump_tsnet_ip6on(SO_IFNAME, how != SO_IP6_OFF);
	if ( e != 0 ) {
		tm_printf((UB *)"net: IPv6 not turned %s (%d)\n", ( how != SO_IP6_OFF ) ? "on" : "off", e);
	}
	ip6_mode = mode;
	knl_memset(ip6_gw, 0, 16);

	/* the new */
	switch ( how ) {
	case SO_IP6_FIXED:
		knl_memcpy(ip6_addr, addr, 16);
		ip6_plen = plen;
		e = rump_tsnet_setaddr6(SO_IFNAME, addr, plen, 0xffffffffU, 0xffffffffU, 0);
		if ( e != 0 ) {
			ip6_mode = SO_IP6_LINK;		/* nothing of it there: the same settings try again */
			return knl_so_nb_er(e);
		}
		if ( !zero16(g) ) {
			e = rump_tsnet_defroute6(SO_IFNAME, g);
			if ( e != 0 ) {
				return knl_so_nb_er(e);
			}
			knl_memcpy(ip6_gw, g, 16);
		}
		break;
	case SO_IP6_AUTO:
		knl_so_slaac_temp(temp);
		rump_tsnet_prefer_temp(temp);
		if ( was != SO_IP6_AUTO ) {
			return knl_so_slaac_run();
		}
		break;
	default:
		break;
	}
	if ( how != SO_IP6_AUTO ) {
		rump_tsnet_prefer_temp(0);
	}
	return E_OK;
}

/*
 * A fixed address for the interface, all in network byte order: asking
 * the network stops, and the address, mask and gateway are set. A name
 * server of 0 leaves the one there was.
 */
EXPORT ER so_setifaddr( UW addr, UW mask, UW gw, UW dns )
{
	if ( !so_started ) {
		return E_OBJ;
	}
	knl_so_dhcp_stop();
	so_asking = FALSE;
	if ( dns != 0 ) {
		ns_iface[0] = dns;
	}
	return knl_so_nb_ifaddr(addr, mask, gw, FALSE);
}

/*
 * A name server the settings name. It is asked before the ones the
 * network hands out with an address.
 */
EXPORT ER so_setdns( INT n, UW addr )
{
	if ( n < 0 || n > 1 ) {
		return E_PAR;
	}
	ns_fixed[n] = addr;
	return E_OK;
}

EXPORT ER so_setdomain( CONST char *domain )
{
	INT	i;

	for ( i = 0; domain != NULL && domain[i] != '\0' && i < (INT)sizeof(so_domain) - 1; i++ ) {
		so_domain[i] = domain[i];
	}
	so_domain[i] = '\0';
	return E_OK;
}

EXPORT BOOL so_dhcp_on( void )
{
	return so_asking;
}

/* ---------------------------------------------------------------- start-up */

#define RUMP_SIGMODEL_IGNORE	1

/*
 * Bring the stack up. The card's interface is given no address of its
 * own: it asks for one, and a machine with nothing answering keeps none
 * and can still talk to itself over the loopback interface.
 */
EXPORT ER so_main( void )
{
	INT	e;

	if ( so_started ) {
		return E_OK;
	}
	InitSpinLock(&so_lock);

	rump_boot_setsigmodel(RUMP_SIGMODEL_IGNORE);
	e = rump_init();
	if ( e != 0 ) {
		tm_printf((UB *)"net: the NetBSD stack did not start (%d)\n", e);
		return E_SYS;
	}
	rump_tsnet_host();

	knl_so_nb_card = ( rump_tsnet_ifindex(SO_IFNAME) > 0 );
	tm_printf((UB *)"net: the NetBSD stack is up%s\n", knl_so_nb_card ? "" : " (no card: the loopback interface only)");
	so_started = TRUE;
	if ( !knl_so_nb_card ) {
		return E_NOEXS;		/* no card: only the loopback interface */
	}
	(void)rump_tsnet_ifup(SO_IFNAME, 1);

#if CNF_NET_SLAAC
	/* IPv6: the routers on the link are listened to, and asked to speak */
	(void)knl_so_slaac_run();
#endif
#if CNF_NET_DHCP
	/* Asking costs a task of its own and gets no answer on a machine
	   with nothing listening, so it is off unless asked for;
	   so_dhcp_start() does the same thing later. */
	(void)so_dhcp_start();
#endif
#if CNF_NET_SNTP
	(void)so_sntp_start();		/* the clock, once there is an address */
#endif

	return E_OK;
}

/*
 * Ask the network for an address. Nothing answers on a machine with no
 * server, and the interface simply keeps the address it had.
 */
EXPORT ER so_dhcp_start( void )
{
	ER	er;

	if ( !so_started || !knl_so_nb_card ) {
		return E_OBJ;
	}
	er = knl_so_dhcp_run();
	if ( er < E_OK ) {
		return er;
	}
	so_asking = TRUE;
	return E_OK;
}

EXPORT ER so_finish( void )
{
	if ( !so_started ) {
		return E_OK;
	}
	knl_so_dhcp_stop();
	knl_so_slaac_stop(FALSE);
	if ( knl_so_nb_card ) {
		(void)rump_tsnet_ifup(SO_IFNAME, 0);
	}
	so_started = FALSE;

	return E_OK;
}

/* ---------------------------------------------------------------- the network's lines, timed */

/*
 * What netlog.h makes of the network's tm_printf and tm_putstring: the
 * console's lock taken (it may be taken again by the processor that
 * holds it), and before a line that begins with "wifi:" or "net:" the
 * milliseconds since the start; knl_net_stamp_end lets the lock go.
 */
IMPORT UINT	tm_out_lock( void );		/* lib/libtm/libtm.c */
IMPORT void	tm_out_unlock( UINT intsts );
IMPORT ER	ts_get_mono( UD *p_ns );

LOCAL UINT	net_stamp_sts;

LOCAL void net_stamp_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	tm_printf((CONST UB *)"%d.%03d ", (INT)( ns / 1000000000ULL ), (INT)( ( ns / 1000000ULL ) % 1000 ));
}

EXPORT void knl_net_stamp( CONST void *line )
{
	CONST UB	*s = (CONST UB *)line;
	UINT		sts = tm_out_lock();

	net_stamp_sts = sts;
	if ( s != NULL && ( ( s[0] == 'w' && s[1] == 'i' && s[2] == 'f' && s[3] == 'i' && s[4] == ':' )
			 || ( s[0] == 'n' && s[1] == 'e' && s[2] == 't' && s[3] == ':' ) ) ) {
		net_stamp_now();
	}
}

/* A whole line with the time before it, whatever it begins with (OpenBSD's layer's lines) */
EXPORT void knl_net_stamp_line( CONST UB *line )
{
	UINT	sts = tm_out_lock();

	net_stamp_now();
	tm_putstring(line);
	tm_out_unlock(sts);
}

EXPORT INT knl_net_stamp_end( void )
{
	tm_out_unlock(net_stamp_sts);
	return 0;
}
