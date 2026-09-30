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
 *	include/ts/soapp.h, design 12.6).
 *
 *	A socket is descriptor TS_SOCK_FD_BASE + its so_ number, so that
 *	read, write and close (ts_syscalls.c) and poll reach it. The
 *	structures and constants are Linux's; the stack's have a length byte
 *	before the family and other numbers, and are made here from them and
 *	back. Errors of the stack become errno values: E_TMOUT is EAGAIN (a
 *	socket that may not block) or ETIMEDOUT, E_IO is ECONNREFUSED on
 *	connect and ECONNRESET after, E_BUSY EINPROGRESS or EAGAIN.
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

/* The stack's side (include/ts/soapp.h), with names of their own here */
typedef struct {
	unsigned char	len;
	unsigned char	family;
	unsigned short	port;
	unsigned int	addr;
	char		zero[8];
} SO_SIN;

typedef struct {
	int	fd;
	short	events;
	short	revents;
} SO_POLLFD;

typedef struct {
	long long	sec;
	long long	usec;
} SO_TIMEVAL;

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
extern int so_getsockopt( int s, int level, int optname, void *optval, unsigned *optlen );
extern int so_setsockopt( int s, int level, int optname, const void *optval, unsigned optlen );
extern int so_getsockname( int s, void *name, unsigned *namelen );
extern int so_getpeername( int s, void *name, unsigned *namelen );
extern int so_poll( SO_POLLFD *fds, int nfds, int tmout );
extern int so_fcntl( int s, int cmd, int val );
extern int so_resolve( const char *name, unsigned *p_addr );

#define SO_AF_INET	2
#define SO_SOL_SOCKET	0xfff
#define SO_O_REUSEADDR	0x0004
#define SO_O_KEEPALIVE	0x0008
#define SO_O_BROADCAST	0x0020
#define SO_O_RCVBUF	0x1002
#define SO_O_SNDTIMEO	0x1005
#define SO_O_RCVTIMEO	0x1006
#define SO_O_ERROR	0x1007
#define SO_TCP_NODELAY	0x01
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

#define E_TMOUT_	(-50)
#define E_IO_		(-57)
#define E_BUSY_		(-65)
#define E_ID_		(-18)
#define E_PAR_		(-17)

#define MAXSOCK		64

static unsigned char	sock_nb[MAXSOCK];	/* made not to block */
static unsigned char	sock_conn[MAXSOCK];	/* connected (a failure after is a reset) */

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
	case E_TMOUT_:	errno = ( s >= 0 && s < MAXSOCK && sock_nb[s] ) ? EAGAIN : ETIMEDOUT; break;
	case E_IO_:	errno = connecting ? ECONNREFUSED : ECONNRESET; break;
	case E_BUSY_:	errno = connecting ? EINPROGRESS : EAGAIN; break;
	case E_ID_:	errno = EBADF; break;
	case E_PAR_:	errno = EINVAL; break;
	default:	errno = EIO; break;
	}
	return -1;
}

static int to_so_addr( const struct sockaddr *a, socklen_t len, SO_SIN *o )
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)a;

	if ( a == NULL || len < sizeof(struct sockaddr_in) || a->sa_family != AF_INET ) {
		errno = ( a != NULL && a->sa_family != AF_INET ) ? EAFNOSUPPORT : EINVAL;
		return -1;
	}
	memset(o, 0, sizeof(*o));
	o->len = sizeof(*o);
	o->family = SO_AF_INET;
	o->port = in->sin_port;
	o->addr = in->sin_addr.s_addr;
	return 0;
}

static void from_so_addr( const SO_SIN *s, struct sockaddr *a, socklen_t *len )
{
	struct sockaddr_in in;

	if ( a == NULL || len == NULL ) return;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_port = s->port;
	in.sin_addr.s_addr = s->addr;
	memcpy(a, &in, ( *len < sizeof(in) ) ? *len : sizeof(in));
	*len = sizeof(in);
}

static int to_so_flags( int flags )
{
	int	f = 0;

	if ( flags & MSG_PEEK )		f |= SO_MSG_PEEK;
	if ( flags & MSG_DONTWAIT )	f |= SO_MSG_DONTWAIT;
	if ( flags & MSG_MORE )		f |= SO_MSG_MORE;
	return f;
}

int socket( int domain, int type, int protocol )
{
	int	base = type & 0xf, s;

	if ( domain != AF_INET ) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if ( base != SOCK_STREAM && base != SOCK_DGRAM ) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	s = so_socket(SO_AF_INET, base, protocol);
	if ( s < 0 ) {
		return set_err(s, 0, -1);
	}
	if ( s >= MAXSOCK ) {
		(void)so_close(s);
		errno = EMFILE;
		return -1;
	}
	sock_nb[s] = 0;
	sock_conn[s] = 0;
	if ( type & SOCK_NONBLOCK ) {
		(void)so_fcntl(s, SO_F_SETFL, SO_O_NONBLOCK);
		sock_nb[s] = 1;
	}
	return TS_SOCK_FD_BASE + s;
}

int socketpair( int domain, int type, int protocol, int sv[2] )
{
	(void)domain; (void)type; (void)protocol; (void)sv;
	errno = EAFNOSUPPORT;
	return -1;
}

/* Closing a socket (ts_syscalls.c's close) */
int ts_sock_close( int fd )
{
	int	s = so_of(fd), er;

	if ( s < 0 ) return -1;
	er = so_close(s);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

int bind( int fd, const struct sockaddr *addr, socklen_t len )
{
	SO_SIN	a;
	int	s = so_of(fd), er;

	if ( s < 0 || to_so_addr(addr, len, &a) < 0 ) return -1;
	er = so_bind(s, &a, sizeof(a));
	if ( er < 0 ) {
		errno = EADDRINUSE;
		return -1;
	}
	return 0;
}

int connect( int fd, const struct sockaddr *addr, socklen_t len )
{
	SO_SIN	a;
	int	s = so_of(fd), er;

	if ( s < 0 || to_so_addr(addr, len, &a) < 0 ) return -1;
	er = so_connect(s, &a, sizeof(a));
	if ( er < 0 ) {
		return set_err(er, 1, s);
	}
	if ( s < MAXSOCK ) sock_conn[s] = 1;
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
	SO_SIN		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), n;

	if ( s < 0 ) return -1;
	n = so_accept(s, &a, &alen);
	if ( n < 0 ) {
		return set_err(n, 0, s);
	}
	if ( n >= MAXSOCK ) {
		(void)so_close(n);
		errno = EMFILE;
		return -1;
	}
	sock_nb[n] = 0;
	sock_conn[n] = 1;
	if ( flags & SOCK_NONBLOCK ) {
		(void)so_fcntl(n, SO_F_SETFL, SO_O_NONBLOCK);
		sock_nb[n] = 1;
	}
	from_so_addr(&a, addr, len);
	return TS_SOCK_FD_BASE + n;
}

int accept( int fd, struct sockaddr *addr, socklen_t *len )
{
	return accept4(fd, addr, len, 0);
}

int shutdown( int fd, int how )
{
	int	s = so_of(fd), er;

	if ( s < 0 ) return -1;
	er = so_shutdown(s, how);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

ssize_t send( int fd, const void *buf, size_t len, int flags )
{
	int	s = so_of(fd), n;

	if ( s < 0 ) return -1;
	n = so_send(s, buf, (long)len, to_so_flags(flags));
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t recv( int fd, void *buf, size_t len, int flags )
{
	int	s = so_of(fd), n;

	if ( s < 0 ) return -1;
	n = so_recv(s, buf, (long)len, to_so_flags(flags));
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t sendto( int fd, const void *buf, size_t len, int flags,
		const struct sockaddr *to, socklen_t tolen )
{
	SO_SIN	a;
	int	s = so_of(fd), n;

	if ( s < 0 ) return -1;
	if ( to == NULL ) return send(fd, buf, len, flags);
	if ( to_so_addr(to, tolen, &a) < 0 ) return -1;
	n = so_sendto(s, buf, (long)len, to_so_flags(flags), &a, sizeof(a));
	return ( n < 0 ) ? set_err(n, 0, s) : n;
}

ssize_t recvfrom( int fd, void *buf, size_t len, int flags,
		  struct sockaddr *from, socklen_t *fromlen )
{
	SO_SIN		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), n;

	if ( s < 0 ) return -1;
	n = so_recvfrom(s, buf, (long)len, to_so_flags(flags), &a, &alen);
	if ( n < 0 ) return set_err(n, 0, s);
	from_so_addr(&a, from, fromlen);
	return n;
}

ssize_t sendmsg( int fd, const struct msghdr *msg, int flags )
{
	ssize_t	done = 0;
	size_t	i;

	for ( i = 0; i < msg->msg_iovlen; i++ ) {
		ssize_t n = sendto(fd, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, flags,
				   (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
		if ( n < 0 ) return ( done > 0 ) ? done : -1;
		done += n;
		if ( (size_t)n < msg->msg_iov[i].iov_len ) break;
	}
	return done;
}

ssize_t recvmsg( int fd, struct msghdr *msg, int flags )
{
	socklen_t	nl = msg->msg_namelen;
	ssize_t		n;

	msg->msg_controllen = 0;
	msg->msg_flags = 0;
	if ( msg->msg_iovlen == 0 ) return 0;
	n = recvfrom(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
		     (struct sockaddr *)msg->msg_name, msg->msg_name ? &nl : NULL);
	if ( n >= 0 && msg->msg_name != NULL ) msg->msg_namelen = nl;
	return n;
}

int setsockopt( int fd, int level, int name, const void *val, socklen_t len )
{
	int	s = so_of(fd), er = 0, sl = SO_SOL_SOCKET, sn = -1;

	if ( s < 0 ) return -1;
	if ( level == SOL_SOCKET ) {
		switch ( name ) {
		case SO_REUSEADDR:	sn = SO_O_REUSEADDR; break;
		case SO_KEEPALIVE:	sn = SO_O_KEEPALIVE; break;
		case SO_BROADCAST:	sn = SO_O_BROADCAST; break;
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
	} else {
		return 0;
	}
	er = so_setsockopt(s, sl, sn, val, len);
	return ( er < 0 ) ? set_err(er, 0, s) : 0;
}

int getsockopt( int fd, int level, int name, void *val, socklen_t *len )
{
	int	s = so_of(fd), er;

	if ( s < 0 ) return -1;
	if ( level == SOL_SOCKET && name == SO_ERROR ) {
		unsigned	l = sizeof(int);
		int		v = 0;

		er = so_getsockopt(s, SO_SOL_SOCKET, SO_O_ERROR, &v, &l);
		if ( er < 0 ) return set_err(er, 0, s);
		/* the stack's error numbers are lwIP's, which are errno values */
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
	SO_SIN		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), er;

	if ( s < 0 ) return -1;
	er = so_getsockname(s, &a, &alen);
	if ( er < 0 ) return set_err(er, 0, s);
	from_so_addr(&a, addr, len);
	return 0;
}

int getpeername( int fd, struct sockaddr *addr, socklen_t *len )
{
	SO_SIN		a;
	unsigned	alen = sizeof(a);
	int		s = so_of(fd), er;

	if ( s < 0 ) return -1;
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
	int	s = so_of(fd);

	if ( s < 0 || s >= MAXSOCK ) return -1;
	if ( cmd == F_GETFL ) {
		return O_RDWR | ( sock_nb[s] ? O_NONBLOCK : 0 );
	}
	if ( cmd == F_SETFL ) {
		sock_nb[s] = ( val & O_NONBLOCK ) ? 1 : 0;
		(void)so_fcntl(s, SO_F_SETFL, sock_nb[s] ? SO_O_NONBLOCK : 0);
		return 0;
	}
	return 0;
}

/* ---------------------------------------------------------------- poll */

#define POLL_MAX	16

int poll( struct pollfd *fds, nfds_t nfds, int timeout )
{
	SO_POLLFD	p[POLL_MAX];
	int		map[POLL_MAX];
	nfds_t		i;
	int		n = 0, ready = 0, r;

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
		if ( n == POLL_MAX ) {
			errno = EINVAL;
			return -1;
		}
		p[n].fd = s;
		p[n].events = (short)( ( ( fds[i].events & ( POLLIN | POLLRDNORM ) ) ? SO_POLLIN : 0 )
				     | ( ( fds[i].events & ( POLLOUT | POLLWRNORM ) ) ? SO_POLLOUT : 0 ) );
		p[n].revents = 0;
		map[n++] = (int)i;
	}
	if ( n == 0 ) {
		return ready;
	}
	r = so_poll(p, n, ( ready > 0 ) ? 0 : ( timeout < 0 ) ? -1 : timeout);
	if ( r < 0 && r != E_TMOUT_ ) {
		return set_err(r, 0, -1);
	}
	for ( i = 0; i < (nfds_t)n; i++ ) {
		short	rv = 0;

		if ( p[i].revents & SO_POLLIN )		rv |= POLLIN | POLLRDNORM;
		if ( p[i].revents & SO_POLLOUT )	rv |= POLLOUT | POLLWRNORM;
		if ( p[i].revents & SO_POLLERR )	rv |= POLLERR;
		if ( p[i].revents & SO_POLLHUP )	rv |= POLLHUP;
		if ( p[i].revents & SO_POLLNVAL )	rv |= POLLNVAL;
		fds[map[i]].revents = rv;
		if ( rv ) ready++;
	}
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
		unsigned long x = strtoul(cp, &end, 10);

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

int inet_pton( int af, const char *src, void *dst )
{
	if ( af == AF_INET ) {
		return inet_aton(src, (struct in_addr *)dst);
	}
	if ( af == AF_INET6 ) {
		return 0;
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
		const unsigned char *b = (const unsigned char *)src;
		int	i, n = 0;

		for ( i = 0; i < 16; i += 2 ) {
			n += snprintf(dst + n, ( (socklen_t)n < size ) ? size - n : 0, "%s%x",
				      i ? ":" : "", ( b[i] << 8 ) | b[i + 1]);
		}
		if ( (socklen_t)n >= size ) {
			errno = ENOSPC;
			return NULL;
		}
		return dst;
	}
	errno = EAFNOSUPPORT;
	return NULL;
}

int getaddrinfo( const char *node, const char *service,
		 const struct addrinfo *hints, struct addrinfo **res )
{
	struct addrinfo		*ai;
	struct sockaddr_in	*sin;
	unsigned		addr = 0;
	int			port = 0;

	if ( hints != NULL && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET ) {
		return EAI_FAMILY;
	}
	if ( service != NULL ) {
		char	*end;

		port = (int)strtol(service, &end, 10);
		if ( *end != 0 ) {
			if ( strcmp(service, "http") == 0 ) port = 80;
			else if ( strcmp(service, "https") == 0 ) port = 443;
			else return EAI_SERVICE;
		}
	}
	if ( node == NULL ) {
		addr = ( hints != NULL && ( hints->ai_flags & AI_PASSIVE ) ) ? INADDR_ANY
		     : htonl(INADDR_LOOPBACK);
	} else {
		struct in_addr	in;

		if ( inet_aton(node, &in) ) {
			addr = in.s_addr;
		} else if ( hints != NULL && ( hints->ai_flags & AI_NUMERICHOST ) ) {
			return EAI_NONAME;
		} else if ( so_resolve(node, &addr) < 0 ) {
			return EAI_NONAME;
		}
	}
	ai = (struct addrinfo *)calloc(1, sizeof(*ai) + sizeof(*sin));
	if ( ai == NULL ) {
		return EAI_MEMORY;
	}
	sin = (struct sockaddr_in *)( ai + 1 );
	sin->sin_family = AF_INET;
	sin->sin_port = htons((uint16_t)port);
	sin->sin_addr.s_addr = addr;
	ai->ai_family = AF_INET;
	ai->ai_socktype = ( hints != NULL && hints->ai_socktype ) ? hints->ai_socktype : SOCK_STREAM;
	ai->ai_protocol = ( hints != NULL ) ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;
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
	case EAI_FAMILY:	return "address family not supported";
	case EAI_SERVICE:	return "service not known";
	case EAI_MEMORY:	return "out of memory";
	default:		return "name resolution failed";
	}
}

int getnameinfo( const struct sockaddr *sa, socklen_t salen, char *host,
		 socklen_t hostlen, char *serv, socklen_t servlen, int flags )
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)sa;

	(void)flags;
	if ( sa == NULL || salen < sizeof(*in) || sa->sa_family != AF_INET ) {
		return EAI_FAMILY;
	}
	if ( host != NULL && inet_ntop(AF_INET, &in->sin_addr, host, hostlen) == NULL ) {
		return EAI_OVERFLOW;
	}
	if ( serv != NULL ) {
		snprintf(serv, servlen, "%u", ntohs(in->sin_port));
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
