/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/socket.h
 *	BSD sockets for a program running as a process, on the so_ calls
 *	of include/ts/soapp.h (lib/libcxxrt/ts_socket.c). The structures and
 *	numbers are those of Linux, which is what programs brought to TessronOS
 *	are written against; the library turns them into the stack's.
 *
 *	A socket is a descriptor of its own range (from TS_SOCK_FD_BASE), so
 *	read, write, close and poll reach it as a file's would. IPv4 streams
 *	and datagrams only: AF_INET6 and AF_UNIX are refused (EAFNOSUPPORT).
 */

#ifndef __TSPOSIX_SYS_SOCKET_H__
#define __TSPOSIX_SYS_SOCKET_H__

#include <sys/types.h>
#include <sys/uio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_SOCK_FD_BASE	0x4000

typedef unsigned int	socklen_t;
typedef unsigned short	sa_family_t;

struct sockaddr {
	sa_family_t	sa_family;
	char		sa_data[14];
};

struct sockaddr_storage {
	sa_family_t	ss_family;
	char		__ss_pad[118];
	unsigned long	__ss_align;
};

struct linger {
	int	l_onoff;
	int	l_linger;
};

struct msghdr {
	void		*msg_name;
	socklen_t	msg_namelen;
	struct iovec	*msg_iov;
	size_t		msg_iovlen;
	void		*msg_control;
	size_t		msg_controllen;
	int		msg_flags;
};

struct cmsghdr {
	size_t	cmsg_len;
	int	cmsg_level;
	int	cmsg_type;
};

struct ucred {
	pid_t	pid;
	uid_t	uid;
	gid_t	gid;
};

#define CMSG_ALIGN(len)		( ( (len) + sizeof(size_t) - 1 ) & ~( sizeof(size_t) - 1 ) )
#define CMSG_SPACE(len)		( CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len) )
#define CMSG_LEN(len)		( CMSG_ALIGN(sizeof(struct cmsghdr)) + (len) )
#define CMSG_DATA(c)		( (unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)) )
#define CMSG_FIRSTHDR(m)	( (m)->msg_controllen >= sizeof(struct cmsghdr) \
				  ? (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0 )
#define CMSG_NXTHDR(m, c)	( (struct cmsghdr *)0 )

#define AF_UNSPEC	0
#define AF_UNIX		1
#define AF_LOCAL	AF_UNIX
#define AF_INET		2
#define AF_INET6	10
#define AF_NETLINK	16
#define AF_PACKET	17
#define PF_UNSPEC	AF_UNSPEC
#define PF_UNIX		AF_UNIX
#define PF_LOCAL	AF_LOCAL
#define PF_INET		AF_INET
#define PF_INET6	AF_INET6

#define SOCK_STREAM	1
#define SOCK_DGRAM	2
#define SOCK_RAW	3
#define SOCK_SEQPACKET	5
#define SOCK_NONBLOCK	04000
#define SOCK_CLOEXEC	02000000

#define SOL_SOCKET	1
#define SO_DEBUG	1
#define SO_REUSEADDR	2
#define SO_TYPE		3
#define SO_ERROR	4
#define SO_DONTROUTE	5
#define SO_BROADCAST	6
#define SO_SNDBUF	7
#define SO_RCVBUF	8
#define SO_KEEPALIVE	9
#define SO_OOBINLINE	10
#define SO_LINGER	13
#define SO_REUSEPORT	15
#define SO_PASSCRED	16
#define SO_PEERCRED	17
#define SO_RCVLOWAT	18
#define SO_SNDLOWAT	19
#define SO_RCVTIMEO	20
#define SO_SNDTIMEO	21

#define MSG_OOB		0x0001
#define MSG_PEEK	0x0002
#define MSG_DONTROUTE	0x0004
#define MSG_CTRUNC	0x0008
#define MSG_TRUNC	0x0020
#define MSG_DONTWAIT	0x0040
#define MSG_WAITALL	0x0100
#define MSG_NOSIGNAL	0x4000
#define MSG_MORE	0x8000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SCM_RIGHTS	1
#define SCM_CREDENTIALS	2

#define SHUT_RD		0
#define SHUT_WR		1
#define SHUT_RDWR	2

#define SOMAXCONN	128

int	socket( int domain, int type, int protocol );
int	socketpair( int domain, int type, int protocol, int sv[2] );
int	bind( int s, const struct sockaddr *addr, socklen_t len );
int	connect( int s, const struct sockaddr *addr, socklen_t len );
int	listen( int s, int backlog );
int	accept( int s, struct sockaddr *addr, socklen_t *len );
int	accept4( int s, struct sockaddr *addr, socklen_t *len, int flags );
int	shutdown( int s, int how );
ssize_t	send( int s, const void *buf, size_t len, int flags );
ssize_t	recv( int s, void *buf, size_t len, int flags );
ssize_t	sendto( int s, const void *buf, size_t len, int flags,
		const struct sockaddr *to, socklen_t tolen );
ssize_t	recvfrom( int s, void *buf, size_t len, int flags,
		  struct sockaddr *from, socklen_t *fromlen );
ssize_t	sendmsg( int s, const struct msghdr *msg, int flags );
ssize_t	recvmsg( int s, struct msghdr *msg, int flags );
int	getsockopt( int s, int level, int name, void *val, socklen_t *len );
int	setsockopt( int s, int level, int name, const void *val, socklen_t len );
int	getsockname( int s, struct sockaddr *addr, socklen_t *len );
int	getpeername( int s, struct sockaddr *addr, socklen_t *len );

/* Whether fd is a socket of this library, and the so_ number behind it */
int	ts_sock_so( int fd );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_SYS_SOCKET_H__ */
