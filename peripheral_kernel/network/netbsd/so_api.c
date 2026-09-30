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

#define NB_IPV6_JOIN_GROUP	12
#define NB_IPV6_LEAVE_GROUP	13
#define NB_IPV6_CHECKSUM	26
#define NB_IPV6_V6ONLY		27

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
		default:			return FALSE;
		}
	case IPPROTO_IPV6:
		*p_level = IPPROTO_IPV6;
		switch ( optname ) {
		case IPV6_V6ONLY:	*p_name = NB_IPV6_V6ONLY; return TRUE;
		case IPV6_CHECKSUM:	*p_name = NB_IPV6_CHECKSUM; return TRUE;
		case IPV6_JOIN_GROUP:	*p_name = NB_IPV6_JOIN_GROUP; return TRUE;
		case IPV6_LEAVE_GROUP:	*p_name = NB_IPV6_LEAVE_GROUP; return TRUE;
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

/* The one answer of so_getaddrinfo, in one block of its own */
LOCAL struct addrinfo *ai_make( UW addr, INT port, CONST struct addrinfo *hints, CONST char *name )
{
	struct addrinfo		*ai;
	struct sockaddr_in	*sin;
	INT			n = 0;
	SZ			size;

	while ( name != NULL && name[n] != '\0' ) n++;
	size = sizeof(struct addrinfo) + sizeof(struct sockaddr_storage) + n + 1;
	ai = (struct addrinfo *)Kcalloc(1, (size_t)size);
	if ( ai == NULL ) {
		return NULL;
	}
	sin = (struct sockaddr_in *)( ai + 1 );
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = so_htons((UH)port);
	sin->sin_addr.s_addr = addr;
	ai->ai_family = AF_INET;
	ai->ai_addr = (struct sockaddr *)sin;
	ai->ai_addrlen = sizeof(*sin);
	if ( hints != NULL ) {
		ai->ai_socktype = hints->ai_socktype;
		ai->ai_protocol = hints->ai_protocol;
	}
	if ( name != NULL ) {
		ai->ai_canonname = (char *)ai + sizeof(struct addrinfo) + sizeof(struct sockaddr_storage);
		knl_memcpy(ai->ai_canonname, name, n);
		ai->ai_canonname[n] = '\0';
	}
	return ai;
}

/*
 * An address for a name. A name that is not already a number needs a DNS
 * server, which a machine on its own does not have; the numeric forms
 * work either way. The service is a port number. IPv4 only, the first
 * address found.
 */
EXPORT ER so_getaddrinfo( CONST char *node, CONST char *service,
			  CONST struct addrinfo *hints, struct addrinfo **res )
{
	UW	addr = 0;
	INT	port = 0, i, n = 0;
	ER	er;

	if ( res == NULL ) {
		return E_PAR;
	}
	*res = NULL;
	if ( node == NULL && service == NULL ) {
		return E_NOEXS;
	}
	if ( hints != NULL && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET ) {
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
		addr = ( hints != NULL && ( hints->ai_flags & AI_PASSIVE ) != 0 )
			? INADDR_ANY : so_htonl(INADDR_LOOPBACK);
	} else if ( !ip4_parse(node, &addr) ) {
		if ( hints != NULL && ( hints->ai_flags & AI_NUMERICHOST ) != 0 ) {
			return E_NOEXS;
		}
		er = knl_so_dns_query(node, AF_INET, &addr, 1, &n);
		if ( er == E_NOEXS && so_domain[0] != '\0' ) {
			/* a name with no dot in it is looked for in the domain as well */
			char	full[160];
			INT	k = 0;
			BOOL	dot = FALSE;

			for ( i = 0; node[i] != '\0'; i++ ) {
				if ( node[i] == '.' ) dot = TRUE;
			}
			if ( !dot && i + 1 + (INT)sizeof(so_domain) < (INT)sizeof(full) ) {
				for ( i = 0; node[i] != '\0'; i++ ) full[k++] = node[i];
				full[k++] = '.';
				for ( i = 0; so_domain[i] != '\0'; i++ ) full[k++] = so_domain[i];
				full[k] = '\0';
				er = knl_so_dns_query(full, AF_INET, &addr, 1, &n);
			}
		}
		if ( er < E_OK ) {
			return er;
		}
		if ( n < 1 ) {
			return E_NOEXS;
		}
	}
	*res = ai_make(addr, port, hints, node);
	return ( *res != NULL ) ? E_OK : E_NOMEM;
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
 * The IPv4 address of a name, in network byte order: the first answer
 * of so_getaddrinfo, for a caller that wants no more than that.
 */
EXPORT ER so_resolve( CONST char *name, UW *p_addr )
{
	struct addrinfo	hints, *res = NULL;
	ER		er;

	if ( name == NULL || p_addr == NULL ) {
		return E_PAR;
	}
	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	er = so_getaddrinfo(name, NULL, &hints, &res);
	if ( er < E_OK ) {
		return er;
	}
	if ( res == NULL || res->ai_addr == NULL || res->ai_family != AF_INET ) {
		er = E_NOEXS;
	} else {
		*p_addr = (UW)((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	}
	so_freeaddrinfo(res);
	return er;
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
 * The name of an address, in numbers. A name from the network would mean
 * asking a server the other way round, which is not carried, so
 * NI_NAMEREQD is refused rather than answered wrongly.
 */
EXPORT ER so_getnameinfo( CONST struct sockaddr *sa, socklen_t salen,
			  char *host, socklen_t hostlen,
			  char *serv, socklen_t servlen, INT flags )
{
	CONST struct sockaddr_in	*sin = (CONST struct sockaddr_in *)sa;
	UW				addr;
	socklen_t			at;
	INT				i;

	if ( sa == NULL || salen < (socklen_t)sizeof(*sin) ) {
		return E_PAR;
	}
	if ( sa->sa_family != AF_INET ) {
		return E_NOSPT;
	}
	if ( (flags & NI_NAMEREQD) != 0 ) {
		return E_NOSPT;		/* no way to ask for a name */
	}

	if ( host != NULL && hostlen > 0 ) {
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
		at = put_u8(serv, 0, servlen, so_ntohs(sin->sin_port));
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
	if ( knl_so_nb_card ) {
		(void)rump_tsnet_ifup(SO_IFNAME, 0);
	}
	so_started = FALSE;

	return E_OK;
}
