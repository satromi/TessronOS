/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_socket.c
 *	BSD sockets on the so_ calls (include/tsposix/sys/socket.h,
 *	include/ts/soapp.h, design 12.6). A local socket (AF_UNIX) is an end
 *	of a stream between processes, which ts_chan.c serves; the calls here
 *	hand its descriptors there.
 *
 *	A socket is descriptor TS_SOCK_FD_BASE + its so_ number, so that
 *	read, write and close (ts_syscalls.c) and poll reach it. The
 *	structures and constants are Linux's; the stack's have a length byte
 *	before the family and other numbers, and are made here from them and
 *	back. Both IPv4 (AF_INET) and IPv6 (AF_INET6) sockets are carried.
 *	Errors of the stack become errno values: E_TMOUT is EAGAIN (a socket
 *	that may not block) or ETIMEDOUT, E_IO is ECONNREFUSED on connect
 *	and ECONNRESET after, E_BUSY EINPROGRESS or EAGAIN, E_NOMEM (the
 *	stack short of buffers for the moment) ENOBUFS, E_LIMIT (the
 *	system's sockets all taken, or all but those the kernel keeps for
 *	itself) ENFILE, and EX_MFILE (the process holding as many as its
 *	limit allows, so_getlimit) EMFILE.
 *
 *	getaddrinfo asks the system's resolver (so_resolveall) for the
 *	addresses of a name of both families, in the order to try them.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <tschan.h>

/* ts_chan.c: the ends of streams between processes (AF_UNIX) */
extern int	ts_chan_pair( int type, int sv[2] );
extern int	ts_chan_close( int fd );
extern long	ts_chan_read( int fd, void *buf, size_t n, int flags );
extern long	ts_chan_write( int fd, const void *buf, size_t n, int flags );
extern long	ts_chan_sendmsg( int fd, const struct msghdr *msg, int flags );
extern long	ts_chan_recvmsg( int fd, struct msghdr *msg, int flags );
extern int	ts_chan_shutdown( int fd, int how );
extern int	ts_chan_fcntl( int fd, int cmd, int arg );

/* ts_evfd.c: poll over descriptors that are not all network sockets */
extern int	ts_wait_fds( struct pollfd *fds, nfds_t nfds, int timeout );
extern int	ts_evfd_is( int fd );

/* The stack's side (include/ts/soapp.h), with names of their own here */
typedef struct {
	unsigned char	len;
	unsigned char	family;
	unsigned short	port;
	unsigned int	addr;
	char		zero[8];
} SO_SIN;

typedef struct {
	unsigned char	len;
	unsigned char	family;
	unsigned short	port;
	unsigned int	flowinfo;
	unsigned char	addr[16];
	unsigned int	scope_id;
} SO_SIN6;

typedef union {
	SO_SIN		in;
	SO_SIN6		in6;
} SO_SA;

typedef struct {
	int		fd;
	short		events;
	short		revents;
} SO_POLLFD;

typedef struct {
	long long	sec;
	long long	usec;
} SO_TIMEVAL;

typedef struct {
	int		family;
	unsigned int	scope;
	unsigned char	addr[16];
} SO_ADDR;

extern int so_socket( int domain, int type, int protocol );
extern int so_close( int s );
extern int so_bind( int s, const void *name, unsigned namelen );
extern int so_connect( int s, const void *name, unsigned namelen );
extern int so_listen( int s, int backlog );
extern int so_accept( int s, void *addr, unsigned *addrlen );
extern int so_shutdown( int s, int how );
extern int so_send( int s, const void *data, long size, int flags );
extern int so_recv( int s, void *mem, long size, int flags );
extern int so_sendto( int s, const void *data, long size, int flags, const void *to, unsigned tolen );
extern int so_recvfrom( int s, void *mem, long size, int flags, void *from, unsigned *fromlen );
extern int so_recvtos( int s, void *mem, long size, int flags, void *from, unsigned *fromlen, int *p_tos );
extern int so_getsockopt( int s, int level, int optname, void *optval, unsigned *optlen );
extern int so_setsockopt( int s, int level, int optname, const void *optval, unsigned optlen );
extern int so_getsockname( int s, void *name, unsigned *namelen );
extern int so_getpeername( int s, void *name, unsigned *namelen );
extern int so_poll( SO_POLLFD *fds, int nfds, int tmout );
extern int so_fcntl( int s, int cmd, int val );
extern int so_resolve( const char *name, unsigned *p_addr );
extern int so_resolveall( const char *name, int family, int flags, SO_ADDR *addrs, int max );

#define SO_AF_UNSPEC	0
#define SO_AF_INET	2
#define SO_AF_INET6	10
#define SO_SOL_SOCKET	0xfff
#define SO_O_REUSEADDR	0x0004
#define SO_O_KEEPALIVE	0x0008
#define SO_O_BROADCAST	0x0020
#define SO_O_LINGER	0x0080
#define SO_O_RCVBUF	0x1002
#define SO_O_SNDTIMEO	0x1005
#define SO_O_RCVTIMEO	0x1006
#define SO_O_ERROR	0x1007
#define SO_TCP_NODELAY	0x01
#define SO_IPPROTO_IP	0
#define SO_IP_TOS	1
#define SO_IP_RECVTOS	9
#define SO_IPPROTO_IPV6	41
#define SO_IPV6_RECVTCLASS 57
#define SO_IPV6_TCLASS	61
#define SO_IPV6_V6ONLY	27
#define SO_MSG_PEEK	0x01
#define SO_MSG_DONTWAIT	0x08
#define SO_MSG_MORE	0x10
#define SO_POLLIN	0x001
#define SO_POLLOUT	0x002
#define SO_POLLERR	0x004
#define SO_POLLNVAL	0x008
#define SO_POLLHUP	0x200
#define SO_F_GETFL	3
#define SO_F_SETFL	4
#define SO_O_NONBLOCK	1
#define SO_RA_ADDRCONFIG 0x0001

#define E_TMOUT_	(-50)
#define E_IO_		(-57)
#define E_BUSY_		(-65)
#define E_ID_		(-18)
#define E_PAR_		(-17)
#define E_NOSPT_	(-9)
#define E_NOMEM_	(-33)
#define E_OACV_		(-27)
#define E_LIMIT_	(-34)
#define EX_MFILE_	(-24)

#define ADDRS_MAX	16		/* addresses of a name getaddrinfo takes */

/*
 * Whether each socket was made not to block, by its number. The numbers
 * are those of the system's table, which the process's own sockets may
 * have any of; the flags are kept in blocks of SOCK_BLK numbers, each
 * made when a number in it first comes into use and kept from then on,
 * so that a thread looking at one never meets a block moved or freed
 * under it. The blocks reach as far as any number of the system's table
 * can be (SO_NUM_CEIL of include/ts/sodef.h, 16384), however large the
 * table grows below it.
 */
#define SOCK_BLK	64
#define SOCK_BLKS	( 16384 / SOCK_BLK )

static unsigned char	*sock_blk[SOCK_BLKS];

/* The flag of socket s; NULL past the blocks, or, with make, when there is no memory for one */
static unsigned char *sock_nb( int s, int make )
{
	unsigned char	*b, *none = NULL;

	if ( s < 0 || s / SOCK_BLK >= SOCK_BLKS ) return NULL;
	b = __atomic_load_n(&sock_blk[s / SOCK_BLK], __ATOMIC_ACQUIRE);
	if ( b == NULL && make ) {
		b = (unsigned char *)calloc(SOCK_BLK, 1);
		if ( b == NULL ) return NULL;
		if ( !__atomic_compare_exchange_n(&sock_blk[s / SOCK_BLK], &none, b, 0,
						  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) {
			free(b);		/* another thread made it first */
			b = none;
		}
	}
	return ( b != NULL ) ? &b[s % SOCK_BLK] : NULL;
}

static int nonblocking( int s )
{
	unsigned char	*nb = sock_nb(s, 0);

	return ( nb != NULL && *nb != 0 );
}

int ts_sock_so( int fd )
{
	return ( fd >= TS_SOCK_FD_BASE ) ? fd - TS_SOCK_FD_BASE : -1;
}

static int so_of( int fd )
{
	int	s = ts_sock_so(fd);

	if ( s < 0 ) {
		errno = ( fd >= 0 ) ? ENOTSOCK : EBADF;
	}
	return s;
}

static int set_err( int er, int connecting, int s )
{
	switch ( er ) {
	case E_TMOUT_:	errno = nonblocking(s) ? EAGAIN : ETIMEDOUT; break;
	case E_IO_:	errno = connecting ? ECONNREFUSED : ECONNRESET; break;
	case E_BUSY_:	errno = connecting ? EINPROGRESS : EAGAIN; break;
	case E_ID_:	errno = EBADF; break;
	case E_PAR_:	errno = EINVAL; break;
	case E_NOSPT_:	errno = EAFNOSUPPORT; break;
	case E_NOMEM_:	errno = ENOBUFS; break;
	case E_OACV_:	errno = EPERM; break;
	case E_LIMIT_:	errno = ENFILE; break;
	case EX_MFILE_:	errno = EMFILE; break;
	default:	errno = EIO; break;
	}
	return -1;
}

/* An address given, in the stack's form; its length, or -1 */
static int to_so_addr( const struct sockaddr *a, socklen_t len, SO_SA *o )
{
	memset(o, 0, sizeof(*o));
	if ( a != NULL && a->sa_family == AF_INET && len >= sizeof(struct sockaddr_in) ) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)a;

		o->in.len = sizeof(o->in);
		o->in.family = SO_AF_INET;
		o->in.port = in->sin_port;
		o->in.addr = in->sin_addr.s_addr;
		return sizeof(o->in);
	}
	if ( a != NULL && a->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6) ) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)a;

		o->in6.len = sizeof(o->in6);
		o->in6.family = SO_AF_INET6;
		o->in6.port = in6->sin6_port;
		o->in6.flowinfo = in6->sin6_flowinfo;
		memcpy(o->in6.addr, &in6->sin6_addr, 16);
		o->in6.scope_id = in6->sin6_scope_id;
		return sizeof(o->in6);
	}
	errno = ( a != NULL && a->sa_family != AF_INET && a->sa_family != AF_INET6 ) ? EAFNOSUPPORT : EINVAL;
	return -1;
}

static void from_so_addr( const SO_SA *s, struct sockaddr *a, socklen_t *len )
{
	struct sockaddr_in	in;
	struct sockaddr_in6	in6;
	const void		*p;
	socklen_t		n;

	if ( a == NULL || len == NULL ) return;
	if ( s->in.family == SO_AF_INET6 ) {
		memset(&in6, 0, sizeof(in6));
		in6.sin6_family = AF_INET6;
		in6.sin6_port = s->in6.port;
		in6.sin6_flowinfo = s->in6.flowinfo;
		memcpy(&in6.sin6_addr, s->in6.addr, 16);
		in6.sin6_scope_id = s->in6.scope_id;
		p = &in6;
		n = sizeof(in6);
	} else {
		memset(&in, 0, sizeof(in));
		in.sin_family = AF_INET;
		in.sin_port = s->in.port;
		in.sin_addr.s_addr = s->in.addr;
		p = &in;
		n = sizeof(in);
	}
	memcpy(a, p, ( *len < n ) ? *len : n);
	*len = n;
}

static int to_so_flags( int flags )
{
	int	f = 0;

	if ( flags & MSG_PEEK )		f |= SO_MSG_PEEK;
	if ( flags & MSG_DONTWAIT )	f |= SO_MSG_DONTWAIT;
	if ( flags & MSG_MORE )		f |= SO_MSG_MORE;
	return f;
}

/* A socket the stack gave number s, as a descriptor; closed again when its flag has no room */
static int made( int s, int nonblock )
{
	unsigned char	*nb = sock_nb(s, 1);

	if ( nb == NULL ) {
		(void)so_close(s);
		errno = ( s / SOCK_BLK >= SOCK_BLKS ) ? EMFILE : ENOMEM;
		return -1;
	}
	*nb = 0;
	if ( nonblock ) {
		(void)so_fcntl(s, SO_F_SETFL, SO_O_NONBLOCK);
		*nb = 1;
	}
	return TS_SOCK_FD_BASE + s;
}

int socket( int domain, int type, int protocol )
{
	int	base = type & 0xf, s;

	if ( domain != AF_INET && domain != AF_INET6 ) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if ( base != SOCK_STREAM && base != SOCK_DGRAM ) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	s = so_socket(( domain == AF_INET6 ) ? SO_AF_INET6 : SO_AF_INET, base, protocol);
	if ( s < 0 ) {
		return set_err(s, 0, -1);
	}
	return made(s, ( type & SOCK_NONBLOCK ) != 0);
}

/* A pair of local sockets: two ends of a stream between processes (ts_chan.c) */
int socketpair( int domain, int type, int protocol, int sv[2] )
{
	if ( domain != AF_UNIX ) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if ( protocol != 0 ) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	return ts_chan_pair(type, sv);
}

/* Closing a socket (ts_syscalls.c's close) */
int ts_sock_close( int fd )
{
	int	s, er;

	if ( ts_chan_is(fd) ) return ts_chan_close(fd);
	s = so_of(fd);
	if ( s < 0 ) return -1;
	er = so_close(s);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

int bind( int fd, const struct sockaddr *addr, socklen_t len )
{
	SO_SA	a;
	int	s = so_of(fd), n, er;

	if ( s < 0 || ( n = to_so_addr(addr, len, &a) ) < 0 ) return -1;
	er = so_bind(s, &a, (unsigned)n);
	if ( er < 0 ) {
		errno = ( er == E_PAR_ ) ? EINVAL : ( er == E_NOSPT_ ) ? EAFNOSUPPORT : EADDRINUSE;
		return -1;
	}
	return 0;
}

int connect( int fd, const struct sockaddr *addr, socklen_t len )
{
	SO_SA	a;
	int	s = so_of(fd), n, er;

	if ( s < 0 || ( n = to_so_addr(addr, len, &a) ) < 0 ) return -1;
	er = so_connect(s, &a, (unsigned)n);
	if ( er < 0 ) {
		return set_err(er, 1, s);
	}
	return 0;
}

int listen( int fd, int backlog )
{
	int	s = so_of(fd), er;

	if ( s < 0 ) return -1;
	er = so_listen(s, backlog);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

int accept4( int fd, struct sockaddr *addr, socklen_t *len, int flags )
{
	SO_SA		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), n;

	if ( s < 0 ) return -1;
	memset(&a, 0, sizeof(a));
	n = so_accept(s, &a, &alen);
	if ( n < 0 ) {
		return set_err(n, 0, s);
	}
	n = made(n, ( flags & SOCK_NONBLOCK ) != 0);
	if ( n >= 0 ) {
		from_so_addr(&a, addr, len);
	}
	return n;
}

int accept( int fd, struct sockaddr *addr, socklen_t *len )
{
	return accept4(fd, addr, len, 0);
}

int shutdown( int fd, int how )
{
	int	s, er;

	if ( ts_chan_is(fd) ) return ts_chan_shutdown(fd, how);
	s = so_of(fd);
	if ( s < 0 ) return -1;
	er = so_shutdown(s, how);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

ssize_t send( int fd, const void *buf, size_t len, int flags )
{
	int	s, n;

	if ( ts_chan_is(fd) ) return ts_chan_write(fd, buf, len, flags);
	s = so_of(fd);
	if ( s < 0 ) return -1;
	n = so_send(s, buf, (long)len, to_so_flags(flags));
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t recv( int fd, void *buf, size_t len, int flags )
{
	int	s, n;

	if ( ts_chan_is(fd) ) return ts_chan_read(fd, buf, len, flags);
	s = so_of(fd);
	if ( s < 0 ) return -1;
	n = so_recv(s, buf, (long)len, to_so_flags(flags));
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t sendto( int fd, const void *buf, size_t len, int flags,
		const struct sockaddr *to, socklen_t tolen )
{
	SO_SA	a;
	int	s = so_of(fd), n, alen;

	if ( s < 0 ) return -1;
	if ( to == NULL ) return send(fd, buf, len, flags);
	if ( ( alen = to_so_addr(to, tolen, &a) ) < 0 ) return -1;
	n = so_sendto(s, buf, (long)len, to_so_flags(flags), &a, (unsigned)alen);
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t recvfrom( int fd, void *buf, size_t len, int flags,
		  struct sockaddr *from, socklen_t *fromlen )
{
	SO_SA		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), n;

	if ( s < 0 ) return -1;
	memset(&a, 0, sizeof(a));
	n = so_recvfrom(s, buf, (long)len, to_so_flags(flags), &a, &alen);
	if ( n < 0 ) return set_err(n, 0, s);
	from_so_addr(&a, from, fromlen);
	return n;
}

ssize_t sendmsg( int fd, const struct msghdr *msg, int flags )
{
	ssize_t	done = 0;
	size_t	i;

	if ( ts_chan_is(fd) ) return ts_chan_sendmsg(fd, msg, flags);
	for ( i = 0; i < msg->msg_iovlen; i++ ) {
		ssize_t n = sendto(fd, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, flags,
				   (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
		if ( n < 0 ) return ( done > 0 ) ? done : -1;
		done += n;
		if ( (size_t)n < msg->msg_iov[i].iov_len ) break;
	}
	return done;
}

/*
 * The first buffer only. With room for ancillary data, the type of
 * service (IPv4, IP_TOS, a byte) or the traffic class (IPv6,
 * IPV6_TCLASS, an int) the datagram arrived with comes as one message,
 * when the socket asked for it (IP_RECVTOS, IPV6_RECVTCLASS).
 */
ssize_t recvmsg( int fd, struct msghdr *msg, int flags )
{
	SO_SA		a;
	unsigned	alen = sizeof(a);
	socklen_t	nl = msg->msg_namelen;
	size_t		room = msg->msg_controllen;
	struct cmsghdr	*c = (struct cmsghdr *)msg->msg_control;
	int		s, n, tos = -1;

	if ( ts_chan_is(fd) ) return ts_chan_recvmsg(fd, msg, flags);
	s = so_of(fd);
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
	if ( s < 0 ) return -1;
	if ( msg->msg_iovlen == 0 ) return 0;
	if ( c == NULL || room < CMSG_SPACE(sizeof(int)) ) {
		n = recvfrom(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
			     (struct sockaddr *)msg->msg_name, msg->msg_name ? &nl : NULL);
		if ( n >= 0 && msg->msg_name != NULL ) msg->msg_namelen = nl;
		return n;
	}
	memset(&a, 0, sizeof(a));
	n = so_recvtos(s, msg->msg_iov[0].iov_base, (long)msg->msg_iov[0].iov_len, to_so_flags(flags),
		       &a, &alen, &tos);
	if ( n < 0 ) return set_err(n, 0, s);
	if ( msg->msg_name != NULL ) {
		from_so_addr(&a, (struct sockaddr *)msg->msg_name, &nl);
		msg->msg_namelen = nl;
	}
	if ( tos >= 0 ) {
		memset(c, 0, CMSG_SPACE(sizeof(int)));
		if ( a.in.family == SO_AF_INET6 ) {
			c->cmsg_level = IPPROTO_IPV6;
			c->cmsg_type = IPV6_TCLASS;
			c->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(c), &tos, sizeof(int));
			msg->msg_controllen = CMSG_SPACE(sizeof(int));
		} else {
			c->cmsg_level = IPPROTO_IP;
			c->cmsg_type = IP_TOS;
			c->cmsg_len = CMSG_LEN(1);
			*CMSG_DATA(c) = (unsigned char)tos;
			msg->msg_controllen = CMSG_SPACE(1);
		}
	}
	return n;
}

int setsockopt( int fd, int level, int name, const void *val, socklen_t len )
{
	int	s, er = 0, sl = SO_SOL_SOCKET, sn = -1;

	if ( ts_chan_is(fd) ) return 0;		/* nothing of an end is set: taken as set */
	s = so_of(fd);
	if ( s < 0 ) return -1;
	if ( level == SOL_SOCKET ) {
		switch ( name ) {
		case SO_REUSEADDR:	sn = SO_O_REUSEADDR; break;
		case SO_KEEPALIVE:	sn = SO_O_KEEPALIVE; break;
		case SO_BROADCAST:	sn = SO_O_BROADCAST; break;
		case SO_LINGER:		sn = SO_O_LINGER; break;	/* struct linger: the same two ints */
		case SO_RCVBUF:		sn = SO_O_RCVBUF; break;
		case SO_RCVTIMEO:
		case SO_SNDTIMEO: {
			const struct timeval	*tv = (const struct timeval *)val;
			SO_TIMEVAL		t;

			if ( len < sizeof(*tv) ) { errno = EINVAL; return -1; }
			t.sec = tv->tv_sec;
			t.usec = tv->tv_usec;
			er = so_setsockopt(s, sl, ( name == SO_RCVTIMEO ) ? SO_O_RCVTIMEO : SO_O_SNDTIMEO,
					   &t, sizeof(t));
			return ( er < 0 ) ? set_err(er, 0, s) : 0;
		}
		default:
			return 0;		/* nothing to set here: taken as set */
		}
	} else if ( level == IPPROTO_TCP && name == TCP_NODELAY ) {
		sl = 6;
		sn = SO_TCP_NODELAY;
	} else if ( level == IPPROTO_IPV6 && name == IPV6_V6ONLY ) {
		sl = SO_IPPROTO_IPV6;
		sn = SO_IPV6_V6ONLY;
	} else if ( level == IPPROTO_IP && ( name == IP_TOS || name == IP_RECVTOS ) ) {
		sl = SO_IPPROTO_IP;
		sn = ( name == IP_TOS ) ? SO_IP_TOS : SO_IP_RECVTOS;
	} else if ( level == IPPROTO_IPV6 && ( name == IPV6_TCLASS || name == IPV6_RECVTCLASS ) ) {
		sl = SO_IPPROTO_IPV6;
		sn = ( name == IPV6_TCLASS ) ? SO_IPV6_TCLASS : SO_IPV6_RECVTCLASS;
	} else {
		return 0;
	}
	er = so_setsockopt(s, sl, sn, val, len);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

int getsockopt( int fd, int level, int name, void *val, socklen_t *len )
{
	int	s, er;

	if ( ts_chan_is(fd) ) {
		int	v = 0;

		if ( level == SOL_SOCKET && name == SO_TYPE ) v = SOCK_STREAM;
		else if ( level == SOL_SOCKET && ( name == SO_SNDBUF || name == SO_RCVBUF ) ) v = 128 * 1024;
		if ( *len >= sizeof(int) ) *(int *)val = v;
		*len = sizeof(int);
		return 0;
	}
	s = so_of(fd);
	if ( s < 0 ) return -1;
	if ( ( level == SOL_SOCKET && name == SO_ERROR ) || ( level == IPPROTO_IPV6 && name == IPV6_V6ONLY ) ) {
		unsigned	l = sizeof(int);
		int		v = 0;

		if ( level == SOL_SOCKET ) {
			er = so_getsockopt(s, SO_SOL_SOCKET, SO_O_ERROR, &v, &l);
		} else {
			er = so_getsockopt(s, SO_IPPROTO_IPV6, SO_IPV6_V6ONLY, &v, &l);
		}
		if ( er < 0 ) return set_err(er, 0, s);
		/* the stack's error numbers are lwIP's, which are errno values */
		if ( *len >= sizeof(int) ) *(int *)val = v;
		*len = sizeof(int);
		return 0;
	}
	if ( ( level == IPPROTO_IP && name == IP_TOS ) || ( level == IPPROTO_IPV6 && name == IPV6_TCLASS ) ) {
		unsigned	l = sizeof(int);
		int		v = 0;

		if ( level == IPPROTO_IP ) {
			er = so_getsockopt(s, SO_IPPROTO_IP, SO_IP_TOS, &v, &l);
		} else {
			er = so_getsockopt(s, SO_IPPROTO_IPV6, SO_IPV6_TCLASS, &v, &l);
		}
		if ( er < 0 ) return set_err(er, 0, s);
		if ( *len >= sizeof(int) ) *(int *)val = v;
		*len = sizeof(int);
		return 0;
	}
	if ( level == SOL_SOCKET && name == SO_TYPE ) {
		if ( *len >= sizeof(int) ) *(int *)val = SOCK_STREAM;
		*len = sizeof(int);
		return 0;
	}
	if ( *len >= sizeof(int) ) *(int *)val = 0;
	*len = sizeof(int);
	return 0;
}

int getsockname( int fd, struct sockaddr *addr, socklen_t *len )
{
	SO_SA		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), er;

	if ( s < 0 ) return -1;
	memset(&a, 0, sizeof(a));
	er = so_getsockname(s, &a, &alen);
	if ( er < 0 ) return set_err(er, 0, s);
	from_so_addr(&a, addr, len);
	return 0;
}

int getpeername( int fd, struct sockaddr *addr, socklen_t *len )
{
	SO_SA		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), er;

	if ( s < 0 ) return -1;
	memset(&a, 0, sizeof(a));
	er = so_getpeername(s, &a, &alen);
	if ( er < 0 ) {
		errno = ENOTCONN;
		return -1;
	}
	from_so_addr(&a, addr, len);
	return 0;
}

/* fcntl on a socket (ts_syscalls.c): only whether it blocks */
int ts_sock_fcntl( int fd, int cmd, int val )
{
	unsigned char	*nb;
	int		s;

	if ( ts_chan_is(fd) ) return ts_chan_fcntl(fd, cmd, val);
	s = so_of(fd);
	nb = ( s >= 0 ) ? sock_nb(s, 1) : NULL;
	if ( nb == NULL ) return -1;
	if ( cmd == F_GETFL ) {
		return O_RDWR | ( *nb ? O_NONBLOCK : 0 );
	}
	if ( cmd == F_SETFL ) {
		*nb = ( val & O_NONBLOCK ) ? 1 : 0;
		(void)so_fcntl(s, SO_F_SETFL, *nb ? SO_O_NONBLOCK : 0);
		return 0;
	}
	return 0;
}

/* ---------------------------------------------------------------- poll */

/*
 * A list for the stack's poll: on the stack up to POLL_LOCAL, in memory
 * of its own past that, as long as the caller's. The stack takes as many
 * as the process may hold (so_getlimit) and refuses more (EINVAL).
 */
#define POLL_LOCAL	64

static int poll_room( SO_POLLFD **pp, int **pmap, SO_POLLFD *p, int *map, size_t n )
{
	*pp = p;
	if ( pmap != NULL ) *pmap = map;
	if ( n <= POLL_LOCAL ) return 0;
	*pp = (SO_POLLFD *)malloc(sizeof(SO_POLLFD) * n);
	if ( pmap != NULL ) *pmap = (int *)malloc(sizeof(int) * n);
	if ( *pp == NULL || ( pmap != NULL && *pmap == NULL ) ) {
		if ( *pp != NULL ) free(*pp);
		if ( pmap != NULL && *pmap != NULL ) free(*pmap);
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

static void poll_free( SO_POLLFD *p, int *map, SO_POLLFD *lp, int *lmap )
{
	if ( p != lp ) free(p);
	if ( map != NULL && map != lmap ) free(map);
}

static short revents_of( short so )
{
	short	rv = 0;

	if ( so & SO_POLLIN )	rv |= POLLIN | POLLRDNORM;
	if ( so & SO_POLLOUT )	rv |= POLLOUT | POLLWRNORM;
	if ( so & SO_POLLERR )	rv |= POLLERR;
	if ( so & SO_POLLHUP )	rv |= POLLHUP;
	if ( so & SO_POLLNVAL )	rv |= POLLNVAL;
	return rv;
}

static short so_events_of( short ev )
{
	return (short)( ( ( ev & ( POLLIN | POLLRDNORM ) ) ? SO_POLLIN : 0 )
		      | ( ( ev & ( POLLOUT | POLLWRNORM ) ) ? SO_POLLOUT : 0 ) );
}

/*
 * Network sockets alone, waited for in the stack up to ms (-1 no end):
 * their revents as poll has them; how many are ready, or -1
 */
int ts_sock_poll( const int *fds, const short *events, short *revents, int n, int ms )
{
	SO_POLLFD	lp[POLL_LOCAL], *p;
	int		i, r, ready = 0;

	if ( n < 0 ) {
		errno = EINVAL;
		return -1;
	}
	if ( poll_room(&p, NULL, lp, NULL, (size_t)n) < 0 ) return -1;
	for ( i = 0; i < n; i++ ) {
		p[i].fd = ts_sock_so(fds[i]);
		p[i].events = so_events_of(events[i]);
		p[i].revents = 0;
	}
	r = so_poll(p, n, ( ms < 0 ) ? -1 : ms);
	if ( r < 0 && r != E_TMOUT_ ) {
		poll_free(p, NULL, lp, NULL);
		return set_err(r, 0, -1);
	}
	for ( i = 0; i < n; i++ ) {
		revents[i] = revents_of(p[i].revents);
		if ( revents[i] ) ready++;
	}
	poll_free(p, NULL, lp, NULL);
	return ready;
}

int poll( struct pollfd *fds, nfds_t nfds, int timeout )
{
	SO_POLLFD	lp[POLL_LOCAL], *p;
	int		lmap[POLL_LOCAL], *map;
	nfds_t		i;
	int		n = 0, ready = 0, r;

	/* an end of a stream, an eventfd or a pipe: the wait that knows them all */
	for ( i = 0; i < nfds; i++ ) {
		if ( fds[i].fd >= 0 && ts_sock_so(fds[i].fd) < 0
		  && ( ts_chan_is(fds[i].fd) || ts_evfd_is(fds[i].fd) ) ) {
			return ts_wait_fds(fds, nfds, timeout);
		}
	}
	if ( poll_room(&p, &map, lp, lmap, (size_t)nfds) < 0 ) return -1;
	for ( i = 0; i < nfds; i++ ) {
		int	s = ts_sock_so(fds[i].fd);

		fds[i].revents = 0;
		if ( fds[i].fd < 0 ) continue;
		if ( s < 0 ) {
			/* a file or the console is always ready */
			fds[i].revents = fds[i].events & ( POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM );
			if ( fds[i].revents ) ready++;
			continue;
		}
		p[n].fd = s;
		p[n].events = so_events_of(fds[i].events);
		p[n].revents = 0;
		map[n++] = (int)i;
	}
	if ( n == 0 ) {
		poll_free(p, map, lp, lmap);
		return ready;
	}
	r = so_poll(p, n, ( ready > 0 ) ? 0 : ( timeout < 0 ) ? -1 : timeout);
	if ( r < 0 && r != E_TMOUT_ ) {
		poll_free(p, map, lp, lmap);
		return set_err(r, 0, -1);
	}
	for ( i = 0; i < (nfds_t)n; i++ ) {
		short	rv = revents_of(p[i].revents);

		fds[map[i]].revents = rv;
		if ( rv ) ready++;
	}
	poll_free(p, map, lp, lmap);
	return ready;
}

/* ---------------------------------------------------------------- addresses and names */

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

int inet_aton( const char *cp, struct in_addr *in )
{
	unsigned	v[4] = { 0, 0, 0, 0 };
	int		i = 0;

	for ( ; i < 4; i++ ) {
		char	*end;
		unsigned long x;

		if ( *cp < '0' || *cp > '9' ) return 0;
		x = strtoul(cp, &end, 10);
		if ( end == cp || x > 255 ) return 0;
		v[i] = (unsigned)x;
		cp = end;
		if ( i < 3 ) {
			if ( *cp != '.' ) return 0;
			cp++;
		}
	}
	if ( *cp != 0 ) return 0;
	in->s_addr = htonl(( v[0] << 24 ) | ( v[1] << 16 ) | ( v[2] << 8 ) | v[3]);
	return 1;
}

in_addr_t inet_addr( const char *cp )
{
	struct in_addr	in;

	return inet_aton(cp, &in) ? in.s_addr : INADDR_NONE;
}

char *inet_ntoa( struct in_addr in )
{
	static char	buf[INET_ADDRSTRLEN];
	unsigned	a = ntohl(in.s_addr);

	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a >> 24, ( a >> 16 ) & 255, ( a >> 8 ) & 255, a & 255);
	return buf;
}

static int hex_val( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/*
 * An IPv6 address in text (RFC 4291 2.2: groups, "::", an IPv4 address
 * at the end), up to 'end' (a NUL, or the '%' before a zone)
 */
static int pton6( const char *s, const char *end, unsigned char *a )
{
	unsigned short	g[8];
	char		tail[16];
	int		ng = 0, gap = -1, k, n, d, at;
	unsigned	v;
	struct in_addr	v4;

	if ( s[0] == ':' ) {
		if ( s[1] != ':' ) return 0;
		gap = 0;
		s += 2;
	}
	while ( s < end ) {
		const char	*q;

		for ( q = s; q < end && *q != ':' && *q != '.'; q++ ) ;
		if ( q < end && *q == '.' ) {
			n = (int)( end - s );
			if ( n >= (int)sizeof(tail) || ng > 6 ) return 0;
			memcpy(tail, s, (size_t)n);
			tail[n] = 0;
			if ( !inet_aton(tail, &v4) ) return 0;
			v = ntohl(v4.s_addr);
			g[ng++] = (unsigned short)( v >> 16 );
			g[ng++] = (unsigned short)v;
			s = end;
			break;
		}
		v = 0;
		for ( n = 0; s < end && ( d = hex_val(*s) ) >= 0; n++, s++ ) {
			if ( n == 4 ) return 0;
			v = v * 16 + (unsigned)d;
		}
		if ( n == 0 || ng == 8 ) return 0;
		g[ng++] = (unsigned short)v;
		if ( s < end && *s == ':' ) {
			s++;
			if ( s < end && *s == ':' ) {
				if ( gap >= 0 ) return 0;
				gap = ng;
				s++;
			} else if ( s >= end ) {
				return 0;
			}
		} else if ( s < end ) {
			return 0;
		}
	}
	if ( ( gap < 0 && ng != 8 ) || ( gap >= 0 && ng > 7 ) ) return 0;
	if ( gap < 0 ) gap = ng;
	memset(a, 0, 16);
	for ( k = 0; k < ng; k++ ) {
		at = ( k < gap ) ? k : 8 - ( ng - k );
		a[at * 2] = (unsigned char)( g[k] >> 8 );
		a[at * 2 + 1] = (unsigned char)g[k];
	}
	return 1;
}

/* An IPv6 address in its shortest text (RFC 5952) */
static int ntop6( const unsigned char *a, char *dst, socklen_t size )
{
	char	t[INET6_ADDRSTRLEN];
	int	i, j, n = 0, best = -1, blen = 1;

	for ( i = 0; i < 8; i = j ) {
		for ( j = i; j < 8 && a[j * 2] == 0 && a[j * 2 + 1] == 0; j++ ) ;
		if ( j - i > blen ) {
			best = i;
			blen = j - i;
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
		if ( i == 6 && best == 0 && blen == 5 && a[10] == 0xff && a[11] == 0xff ) {
			n += snprintf(t + n, sizeof(t) - (size_t)n, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
			break;
		}
		n += snprintf(t + n, sizeof(t) - (size_t)n, "%x%s", ( a[i * 2] << 8 ) | a[i * 2 + 1], ( i < 7 ) ? ":" : "");
	}
	t[n] = 0;
	if ( (socklen_t)n >= size ) {
		errno = ENOSPC;
		return 0;
	}
	memcpy(dst, t, (size_t)n + 1);
	return 1;
}

int inet_pton( int af, const char *src, void *dst )
{
	if ( af == AF_INET ) {
		return inet_aton(src, (struct in_addr *)dst);
	}
	if ( af == AF_INET6 ) {
		return pton6(src, src + strlen(src), (unsigned char *)dst);
	}
	errno = EAFNOSUPPORT;
	return -1;
}

const char *inet_ntop( int af, const void *src, char *dst, socklen_t size )
{
	if ( af == AF_INET ) {
		unsigned a = ntohl(((const struct in_addr *)src)->s_addr);

		if ( (socklen_t)snprintf(dst, size, "%u.%u.%u.%u", a >> 24, ( a >> 16 ) & 255,
					 ( a >> 8 ) & 255, a & 255) >= size ) {
			errno = ENOSPC;
			return NULL;
		}
		return dst;
	}
	if ( af == AF_INET6 ) {
		return ntop6((const unsigned char *)src, dst, size) ? dst : NULL;
	}
	errno = EAFNOSUPPORT;
	return NULL;
}

/* One answer of getaddrinfo, with its address in the same block */
static struct addrinfo *ai_new( const SO_ADDR *a, int port, int socktype, int protocol, const char *canon )
{
	struct addrinfo	*ai;
	size_t		cn = ( canon != NULL ) ? strlen(canon) + 1 : 0;

	ai = (struct addrinfo *)calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in6) + cn);
	if ( ai == NULL ) {
		return NULL;
	}
	if ( a->family == SO_AF_INET6 ) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)( ai + 1 );

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons((uint16_t)port);
		memcpy(&sin6->sin6_addr, a->addr, 16);
		sin6->sin6_scope_id = a->scope;
		ai->ai_family = AF_INET6;
		ai->ai_addrlen = sizeof(*sin6);
		ai->ai_addr = (struct sockaddr *)sin6;
	} else {
		struct sockaddr_in *sin = (struct sockaddr_in *)( ai + 1 );

		sin->sin_family = AF_INET;
		sin->sin_port = htons((uint16_t)port);
		memcpy(&sin->sin_addr.s_addr, a->addr, 4);
		ai->ai_family = AF_INET;
		ai->ai_addrlen = sizeof(*sin);
		ai->ai_addr = (struct sockaddr *)sin;
	}
	ai->ai_socktype = socktype;
	ai->ai_protocol = protocol;
	if ( canon != NULL ) {
		ai->ai_canonname = (char *)( ai + 1 ) + sizeof(struct sockaddr_in6);
		memcpy(ai->ai_canonname, canon, cn);
	}
	return ai;
}

/*
 * The addresses of a node, both families: a number as it is (an IPv6
 * one with a zone goes to the system, which knows the interfaces), else
 * through the system's resolver. With AI_ADDRCONFIG a family the
 * machine has no address of to reach the world with is left out.
 */
int getaddrinfo( const char *node, const char *service,
		 const struct addrinfo *hints, struct addrinfo **res )
{
	SO_ADDR		l[ADDRS_MAX];
	struct addrinfo	*ai, **tail = res;
	int		family = AF_UNSPEC, flags = 0, socktype = SOCK_STREAM, protocol = 0;
	int		port = 0, n = 0, i;

	*res = NULL;
	if ( hints != NULL ) {
		family = hints->ai_family;
		flags = hints->ai_flags;
		if ( hints->ai_socktype ) socktype = hints->ai_socktype;
		protocol = hints->ai_protocol;
	}
	if ( family != AF_UNSPEC && family != AF_INET && family != AF_INET6 ) {
		return EAI_FAMILY;
	}
	if ( node == NULL && service == NULL ) {
		return EAI_NONAME;
	}
	if ( service != NULL ) {
		char	*end;

		port = (int)strtol(service, &end, 10);
		if ( *end != 0 || end == service ) {
			if ( flags & AI_NUMERICSERV ) return EAI_NONAME;
			if ( strcmp(service, "http") == 0 ) port = 80;
			else if ( strcmp(service, "https") == 0 ) port = 443;
			else return EAI_SERVICE;
		}
		if ( port < 0 || port > 65535 ) return EAI_SERVICE;
	}
	memset(l, 0, sizeof(l));
	if ( node == NULL ) {
		int	passive = ( flags & AI_PASSIVE ) != 0;

		if ( family != AF_INET6 ) {
			uint32_t a = passive ? INADDR_ANY : htonl(INADDR_LOOPBACK);

			l[n].family = SO_AF_INET;
			memcpy(l[n].addr, &a, 4);
			n++;
		}
		if ( family != AF_INET ) {
			l[n].family = SO_AF_INET6;
			if ( !passive ) l[n].addr[15] = 1;
			n++;
		}
	} else {
		struct in_addr	in;
		const char	*zone = strchr(node, '%');

		if ( inet_aton(node, &in) ) {
			if ( family == AF_INET6 ) return EAI_NONAME;
			l[0].family = SO_AF_INET;
			memcpy(l[0].addr, &in.s_addr, 4);
			n = 1;
		} else if ( zone == NULL && pton6(node, node + strlen(node), l[0].addr) ) {
			if ( family == AF_INET ) return EAI_NONAME;
			l[0].family = SO_AF_INET6;
			n = 1;
		} else if ( ( flags & AI_NUMERICHOST ) && ( zone == NULL || !pton6(node, zone, l[0].addr) ) ) {
			return EAI_NONAME;
		} else {
			n = so_resolveall(node, ( family == AF_INET6 ) ? SO_AF_INET6 : ( family == AF_INET ) ? SO_AF_INET : SO_AF_UNSPEC,
					  ( flags & AI_ADDRCONFIG ) ? SO_RA_ADDRCONFIG : 0, l, ADDRS_MAX);
			if ( n == E_TMOUT_ ) return EAI_AGAIN;
			if ( n <= 0 ) return EAI_NONAME;
		}
	}
	for ( i = 0; i < n; i++ ) {
		ai = ai_new(&l[i], port, socktype, protocol,
			    ( i == 0 && node != NULL && ( flags & AI_CANONNAME ) ) ? node : NULL);
		if ( ai == NULL ) {
			freeaddrinfo(*res);
			*res = NULL;
			return EAI_MEMORY;
		}
		*tail = ai;
		tail = &ai->ai_next;
	}
	return 0;
}

void freeaddrinfo( struct addrinfo *res )
{
	while ( res != NULL ) {
		struct addrinfo	*next = res->ai_next;

		free(res);
		res = next;
	}
}

const char *gai_strerror( int err )
{
	switch ( err ) {
	case EAI_NONAME:	return "name not known";
	case EAI_AGAIN:		return "no name server answered";
	case EAI_FAMILY:	return "address family not supported";
	case EAI_SERVICE:	return "service not known";
	case EAI_MEMORY:	return "out of memory";
	default:		return "name resolution failed";
	}
}

/* The numbers of an address: IPv6 with the zone of a link-local one */
int getnameinfo( const struct sockaddr *sa, socklen_t salen, char *host,
		 socklen_t hostlen, char *serv, socklen_t servlen, int flags )
{
	unsigned short	port;

	if ( sa == NULL ) {
		return EAI_FAMILY;
	}
	if ( flags & NI_NAMEREQD ) {
		return EAI_NONAME;		/* no way to ask for a name */
	}
	if ( sa->sa_family == AF_INET6 && salen >= sizeof(struct sockaddr_in6) ) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;

		if ( host != NULL ) {
			char	t[INET6_ADDRSTRLEN + 12];

			if ( !ntop6(in6->sin6_addr.s6_addr, t, INET6_ADDRSTRLEN) ) return EAI_OVERFLOW;
			if ( in6->sin6_scope_id != 0 && IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr) ) {
				snprintf(t + strlen(t), 12, "%%%u", in6->sin6_scope_id);
			}
			if ( strlen(t) >= hostlen ) return EAI_OVERFLOW;
			strcpy(host, t);
		}
		port = in6->sin6_port;
	} else if ( sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in) ) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)sa;

		if ( host != NULL && inet_ntop(AF_INET, &in->sin_addr, host, hostlen) == NULL ) {
			return EAI_OVERFLOW;
		}
		port = in->sin_port;
	} else {
		return EAI_FAMILY;
	}
	if ( serv != NULL ) {
		snprintf(serv, servlen, "%u", ntohs(port));
	}
	return 0;
}

struct hostent *gethostbyname( const char *name )
{
	static struct hostent	h;
	static struct in_addr	a;
	static char		*list[2];
	unsigned		addr;

	if ( so_resolve(name, &addr) < 0 ) {
		return NULL;
	}
	a.s_addr = addr;
	list[0] = (char *)&a;
	list[1] = NULL;
	h.h_name = (char *)name;
	h.h_aliases = NULL;
	h.h_addrtype = AF_INET;
	h.h_length = 4;
	h.h_addr_list = list;
	return &h;
}
