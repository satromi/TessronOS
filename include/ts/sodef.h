/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sodef.h
 *	The structures and constants of the so_ calls (design 12.6)
 *
 *	The socket calls keep one set of numbers and shapes whatever stack
 *	carries them: those the calls have always had, which processes are
 *	built with (include/ts/soapp.h) and which the kernel's callers use.
 *	The NetBSD stack has numbers of its own; the so_ layer over it
 *	(peripheral_kernel/network/netbsd/so_api.c) turns these into its
 *	numbers and back, so nothing here is the stack's.
 *
 *	The address structures have a length byte before the family.
 *	Addresses and ports inside them are in network byte order.
 */

#ifndef __TS_SODEF_H__
#define __TS_SODEF_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef UB	sa_family_t;
typedef UH	in_port_t;
typedef UINT	in_addr_t;
typedef UINT	socklen_t;
typedef UINT	nfds_t;

/* ---------------------------------------------------------------- addresses */

struct in_addr {
	in_addr_t	s_addr;
};

struct in6_addr {
	union {
		UINT	u32_addr[4];
		UB	u8_addr[16];
	} un;
#define s6_addr	un.u8_addr
};

struct sockaddr {
	UB		sa_len;
	sa_family_t	sa_family;
	char		sa_data[14];
};

struct sockaddr_in {
	UB		sin_len;
	sa_family_t	sin_family;
	in_port_t	sin_port;
	struct in_addr	sin_addr;
#define SIN_ZERO_LEN	8
	char		sin_zero[SIN_ZERO_LEN];
};

struct sockaddr_in6 {
	UB		sin6_len;
	sa_family_t	sin6_family;
	in_port_t	sin6_port;
	UINT		sin6_flowinfo;
	struct in6_addr	sin6_addr;
	UINT		sin6_scope_id;
};

struct sockaddr_storage {
	UB		s2_len;
	sa_family_t	ss_family;
	char		s2_data1[2];
	UINT		s2_data2[3];
	UINT		s2_data3[3];
};

#define AF_UNSPEC	0
#define AF_INET		2
#define AF_INET6	10
#define PF_UNSPEC	AF_UNSPEC
#define PF_INET		AF_INET
#define PF_INET6	AF_INET6

#define INADDR_ANY		((in_addr_t)0x00000000UL)
#define INADDR_NONE		((in_addr_t)0xffffffffUL)
#define INADDR_LOOPBACK		((in_addr_t)0x7f000001UL)
#define INADDR_BROADCAST	((in_addr_t)0xffffffffUL)

/* ---------------------------------------------------------------- sockets */

#define SOCK_STREAM	1
#define SOCK_DGRAM	2
#define SOCK_RAW	3

#define IPPROTO_IP	0
#define IPPROTO_ICMP	1
#define IPPROTO_TCP	6
#define IPPROTO_UDP	17
#define IPPROTO_IPV6	41
#define IPPROTO_ICMPV6	58
#define IPPROTO_UDPLITE	136
#define IPPROTO_RAW	255

/* so_setsockopt / so_getsockopt, level SOL_SOCKET */
#define SOL_SOCKET	0xfff
#define SO_DEBUG	0x0001
#define SO_ACCEPTCONN	0x0002
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_DONTROUTE	0x0010
#define SO_BROADCAST	0x0020
#define SO_USELOOPBACK	0x0040
#define SO_LINGER	0x0080
#define SO_DONTLINGER	((int)(~SO_LINGER))
#define SO_OOBINLINE	0x0100
#define SO_REUSEPORT	0x0200
#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_SNDLOWAT	0x1003
#define SO_RCVLOWAT	0x1004
#define SO_SNDTIMEO	0x1005		/* a struct timeval */
#define SO_RCVTIMEO	0x1006		/* a struct timeval */
#define SO_ERROR	0x1007		/* the errno values below */
#define SO_TYPE		0x1008
#define SO_CONTIMEO	0x1009
#define SO_NO_CHECK	0x100a
#define SO_BINDTODEVICE	0x100b

struct linger {
	int	l_onoff;
	int	l_linger;
};

/* level IPPROTO_TCP */
#define TCP_NODELAY	0x01
#define TCP_KEEPALIVE	0x02		/* idle time before probes, ms */
#define TCP_KEEPIDLE	0x03		/* the same, s */
#define TCP_KEEPINTVL	0x04
#define TCP_KEEPCNT	0x05

/* level IPPROTO_IP */
#define IP_TOS			1
#define IP_TTL			2
#define IP_ADD_MEMBERSHIP	3
#define IP_DROP_MEMBERSHIP	4
#define IP_MULTICAST_TTL	5
#define IP_MULTICAST_IF		6
#define IP_MULTICAST_LOOP	7
#define IP_PKTINFO		8

typedef struct ip_mreq {
	struct in_addr	imr_multiaddr;
	struct in_addr	imr_interface;
} ip_mreq;

struct in_pktinfo {
	unsigned int	ipi_ifindex;
	struct in_addr	ipi_addr;
};

/* level IPPROTO_IPV6 */
#define IPV6_CHECKSUM		7
#define IPV6_JOIN_GROUP		12
#define IPV6_ADD_MEMBERSHIP	IPV6_JOIN_GROUP
#define IPV6_LEAVE_GROUP	13
#define IPV6_DROP_MEMBERSHIP	IPV6_LEAVE_GROUP
#define IPV6_V6ONLY		27

/* so_send / so_recv */
#define MSG_PEEK	0x01
#define MSG_WAITALL	0x02
#define MSG_OOB		0x04
#define MSG_DONTWAIT	0x08
#define MSG_MORE	0x10
#define MSG_NOSIGNAL	0x20
/* msghdr.msg_flags */
#define MSG_TRUNC	0x04
#define MSG_CTRUNC	0x08

/* so_shutdown */
#define SHUT_RD		0
#define SHUT_WR		1
#define SHUT_RDWR	2

/* ---------------------------------------------------------------- messages */

struct iovec {
	void	*iov_base;
	SZ	iov_len;
};

struct msghdr {
	void		*msg_name;
	socklen_t	msg_namelen;
	struct iovec	*msg_iov;
	int		msg_iovlen;
	void		*msg_control;
	socklen_t	msg_controllen;
	int		msg_flags;
};

struct cmsghdr {
	socklen_t	cmsg_len;
	int		cmsg_level;
	int		cmsg_type;
};

/* ---------------------------------------------------------------- waiting */

/* The timeouts of SO_RCVTIMEO and SO_SNDTIMEO: two 64 bit words */
struct timeval {
	long	tv_sec;
	long	tv_usec;
};

/* so_poll */
struct pollfd {
	int	fd;
	short	events;
	short	revents;
};

#define POLLIN		0x1
#define POLLOUT		0x2
#define POLLERR		0x4
#define POLLNVAL	0x8
#define POLLRDNORM	0x10
#define POLLRDBAND	0x20
#define POLLPRI		0x40
#define POLLWRNORM	0x80
#define POLLWRBAND	0x100
#define POLLHUP		0x200

/* so_select: one bit for each socket number */
#define SO_MAX		64		/* sockets there can be, numbered 0 .. SO_MAX - 1 */
#define FD_SETSIZE	SO_MAX

typedef struct fd_set {
	UB	fd_bits[(FD_SETSIZE + 7) / 8];
} fd_set;

#define FD_SET(n, p)	do { if ( (UINT)(n) < FD_SETSIZE ) (p)->fd_bits[(n) / 8] |= (UB)( 1 << ( (n) & 7 ) ); } while ( 0 )
#define FD_CLR(n, p)	do { if ( (UINT)(n) < FD_SETSIZE ) (p)->fd_bits[(n) / 8] &= (UB)~( 1 << ( (n) & 7 ) ); } while ( 0 )
#define FD_ISSET(n, p)	( (UINT)(n) < FD_SETSIZE && ( (p)->fd_bits[(n) / 8] & ( 1 << ( (n) & 7 ) ) ) != 0 )
#define FD_ZERO(p)	do { INT _i; for ( _i = 0; _i < (INT)sizeof((p)->fd_bits); _i++ ) (p)->fd_bits[_i] = 0; } while ( 0 )

/* so_fcntl: only whether the socket may block */
#define F_GETFL		3
#define F_SETFL		4
#ifndef O_NONBLOCK
#define O_NONBLOCK	1
#endif
#define O_NDELAY	O_NONBLOCK
#ifndef O_RDONLY			/* the file system's, when it came first */
#define O_RDONLY	2
#endif
#ifndef O_WRONLY
#define O_WRONLY	4
#endif
#ifndef O_RDWR
#define O_RDWR		(O_RDONLY | O_WRONLY)
#endif

/* so_ioctl */
#define IOCPARM_MASK	0x7fUL
#define IOC_VOID	0x20000000UL
#define IOC_OUT		0x40000000UL
#define IOC_IN		0x80000000UL
#define IOC_INOUT	(IOC_IN | IOC_OUT)
#define _IO(x, y)	((long)(IOC_VOID | ((x) << 8) | (y)))
#define _IOR(x, y, t)	((long)(IOC_OUT | ((sizeof(t) & IOCPARM_MASK) << 16) | ((x) << 8) | (y)))
#define _IOW(x, y, t)	((long)(IOC_IN | ((sizeof(t) & IOCPARM_MASK) << 16) | ((x) << 8) | (y)))
#define FIONREAD	_IOR('f', 127, unsigned long)	/* an int: bytes that can be read */
#define FIONBIO		_IOW('f', 126, unsigned long)	/* an int: non-zero, calls do not block */

/* ---------------------------------------------------------------- names */

struct addrinfo {
	int		ai_flags;
	int		ai_family;
	int		ai_socktype;
	int		ai_protocol;
	socklen_t	ai_addrlen;
	struct sockaddr	*ai_addr;
	char		*ai_canonname;
	struct addrinfo	*ai_next;
};

#define AI_PASSIVE	0x01
#define AI_CANONNAME	0x02
#define AI_NUMERICHOST	0x04
#define AI_NUMERICSERV	0x08
#define AI_V4MAPPED	0x10
#define AI_ALL		0x20
#define AI_ADDRCONFIG	0x40

#define EAI_NONAME	200
#define EAI_SERVICE	201
#define EAI_FAIL	202
#define EAI_MEMORY	203
#define EAI_FAMILY	204

/* ---------------------------------------------------------------- errors */

/*
 * The values SO_ERROR answers and knl_so_errcode takes. The calls
 * themselves answer T-Kernel error codes.
 */
#define EPERM		1
#define ENOENT		2
#define EINTR		4
#define EIO		5
#define EBADF		9
#define EAGAIN		11
#define EWOULDBLOCK	EAGAIN
#define ENOMEM		12
#define EACCES		13
#define EFAULT		14
#define EBUSY		16
#define EINVAL		22
#define EMFILE		24
#define EPIPE		32
#define ENOSYS		38
#define ENOTSOCK	88
#define EDESTADDRREQ	89
#define EMSGSIZE	90
#define EPROTOTYPE	91
#define ENOPROTOOPT	92
#define EPROTONOSUPPORT	93
#define EOPNOTSUPP	95
#define EAFNOSUPPORT	97
#define EADDRINUSE	98
#define EADDRNOTAVAIL	99
#define ENETDOWN	100
#define ENETUNREACH	101
#define ECONNABORTED	103
#define ECONNRESET	104
#define ENOBUFS		105
#define EISCONN		106
#define ENOTCONN	107
#define ESHUTDOWN	108
#define ETIMEDOUT	110
#define ECONNREFUSED	111
#define EHOSTDOWN	112
#define EHOSTUNREACH	113
#define EALREADY	114
#define EINPROGRESS	115

/* ---------------------------------------------------------------- byte order */

static __inline__ UH so_htons( UH v )
{
	return (UH)( ( v << 8 ) | ( v >> 8 ) );
}

static __inline__ UINT so_htonl( UINT v )
{
	return ( v << 24 ) | ( ( v & 0xff00U ) << 8 ) | ( ( v >> 8 ) & 0xff00U ) | ( v >> 24 );
}

#define so_ntohs(v)	so_htons(v)
#define so_ntohl(v)	so_htonl(v)

/* The names the kernel's callers have always used */
#define lwip_htons(v)	so_htons((UH)(v))
#define lwip_ntohs(v)	so_htons((UH)(v))
#define lwip_htonl(v)	so_htonl((UINT)(v))
#define lwip_ntohl(v)	so_htonl((UINT)(v))

#ifdef __cplusplus
}
#endif

#endif /* __TS_SODEF_H__ */
