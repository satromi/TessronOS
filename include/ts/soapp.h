/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	soapp.h
 *	The socket calls of a program running as a process (design 12.6)
 *
 *	The calls are the kernel's so_ of include/ts/so.h, reached through
 *	SVC class 5, with the same names and arguments. The kernel's header
 *	brings the network stack's own; a process has no use for those, so
 *	the structures and constants it needs are given here, laid out as
 *	the stack lays them out. IPv4 only.
 *
 *	A socket is the process's that made it: another process's number is
 *	refused (E_ID), and what is left open is closed when the process
 *	ends. Errors come back as T-Kernel error codes (E_TMOUT for a time
 *	limit that ran out, E_IO for a connection refused or reset, E_BUSY
 *	for a call on a socket that may not block that has not finished).
 *
 *	Addresses and ports inside the structures are in network byte order,
 *	as are the UINT addresses so_resolve and the interface calls use; the
 *	so_hton* helpers below turn a host value round. The 32 bit values
 *	are UINT rather than UW: a program built without config/config.h has
 *	a UW of 64 bits.
 */

#ifndef __TS_SOAPP_H__
#define __TS_SOAPP_H__

#include <tk/typedef.h>
#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(LWIP_HDR_SOCKETS_H) && !defined(__TS_SODEF_H__)	/* the kernel has these already */

typedef UINT	socklen_t;

struct in_addr {
	UINT	s_addr;
};

struct sockaddr {
	UB	sa_len;
	UB	sa_family;
	char	sa_data[14];
};

struct sockaddr_in {
	UB		sin_len;		/* sizeof(struct sockaddr_in), or 0 */
	UB		sin_family;		/* AF_INET */
	UH		sin_port;
	struct in_addr	sin_addr;
	char		sin_zero[8];
};

/* One socket of so_poll: what is waited for, and what happened */
struct pollfd {
	int	fd;
	short	events;
	short	revents;
};

#define AF_UNSPEC	0
#define AF_INET		2
#define PF_INET		AF_INET

#define SOCK_STREAM	1
#define SOCK_DGRAM	2

#define IPPROTO_IP	0
#define IPPROTO_TCP	6
#define IPPROTO_UDP	17

#define INADDR_ANY	((UINT)0x00000000UL)
#define INADDR_BROADCAST ((UINT)0xffffffffUL)

/* so_setsockopt / so_getsockopt */
#define SOL_SOCKET	0xfff
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_BROADCAST	0x0020
#define SO_RCVBUF	0x1002
#define SO_SNDTIMEO	0x1005		/* a T_SOTIMEVAL */
#define SO_RCVTIMEO	0x1006		/* a T_SOTIMEVAL; also bounds so_accept */
#define SO_ERROR	0x1007
#define TCP_NODELAY	0x01		/* level IPPROTO_TCP */

/* so_send / so_recv */
#define MSG_PEEK	0x01
#define MSG_DONTWAIT	0x08
#define MSG_MORE	0x10

/* so_shutdown */
#define SHUT_RD		0
#define SHUT_WR		1
#define SHUT_RDWR	2

/* so_poll */
#define POLLIN		0x001
#define POLLOUT		0x002
#define POLLERR		0x004
#define POLLNVAL	0x008
#define POLLHUP		0x200

/* so_fcntl: only whether the socket may block */
#define F_GETFL		3
#define F_SETFL		4
#define O_NONBLOCK	1

#endif /* LWIP_HDR_SOCKETS_H, __TS_SODEF_H__ */

/* SO_RCVTIMEO and SO_SNDTIMEO: two 64 bit words, as the stack's struct timeval */
typedef struct {
	D	sec;
	D	usec;
} T_SOTIMEVAL;

#ifndef __TS_SODEF_H__
/* Byte order: the network's is big endian, the machine's little */
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
#endif /* __TS_SODEF_H__ */

/* a.b.c.d as a UINT in network byte order */
#define SO_IPADDR(a, b, c, d)	so_htonl( ( (UINT)(a) << 24 ) | ( (UINT)(b) << 16 ) | ( (UINT)(c) << 8 ) | (UINT)(d) )

IMPORT INT so_socket( INT domain, INT type, INT protocol );
IMPORT ER  so_close( INT s );
IMPORT ER  so_bind( INT s, CONST struct sockaddr *name, socklen_t namelen );
IMPORT ER  so_connect( INT s, CONST struct sockaddr *name, socklen_t namelen );
IMPORT ER  so_listen( INT s, INT backlog );
IMPORT INT so_accept( INT s, struct sockaddr *addr, socklen_t *addrlen );
IMPORT ER  so_shutdown( INT s, INT how );

IMPORT INT so_send( INT s, CONST void *data, SZ size, INT flags );
IMPORT INT so_recv( INT s, void *mem, SZ size, INT flags );
IMPORT INT so_sendto( INT s, CONST void *data, SZ size, INT flags,
		      CONST struct sockaddr *to, socklen_t tolen );
IMPORT INT so_recvfrom( INT s, void *mem, SZ size, INT flags,
			struct sockaddr *from, socklen_t *fromlen );

/* Options of up to 64 bytes */
IMPORT ER  so_getsockopt( INT s, INT level, INT optname,
			  void *optval, socklen_t *optlen );
IMPORT ER  so_setsockopt( INT s, INT level, INT optname,
			  CONST void *optval, socklen_t optlen );
IMPORT ER  so_getsockname( INT s, struct sockaddr *name, socklen_t *namelen );
IMPORT ER  so_getpeername( INT s, struct sockaddr *name, socklen_t *namelen );

/*
 * Wait until one of up to 16 sockets is ready as asked (POLLIN,
 * POLLOUT); how many are. The timeout is in milliseconds, TMO_FEVR for
 * none and TMO_POL to look without waiting.
 */
IMPORT INT so_poll( struct pollfd *fds, INT nfds, TMO tmout );
IMPORT ER  so_fcntl( INT s, INT cmd, INT val );

/* The first IPv4 address of a name: "10.0.2.2" as it is, a host name through DNS */
IMPORT ER  so_resolve( CONST char *name, UINT *p_addr );

/*
 * The interface: its address, mask and gateway, and the first name
 * server (NULL for what is not wanted). Setting a fixed address (DHCP
 * stops; dns 0 keeps the server there was) and asking the network for
 * one are an administrator's (E_OACV otherwise); an address from DHCP
 * shows in so_getifaddr once it has come.
 */
IMPORT ER  so_getifaddr( UINT *p_addr, UINT *p_mask, UINT *p_gw );
IMPORT ER  so_getdns( UINT *p_dns );
IMPORT ER  so_setifaddr( UINT addr, UINT mask, UINT gw, UINT dns );
IMPORT ER  so_dhcp_start( void );

/*
 * The real object of one of the process's sockets (OB_T_CHANNEL,
 * OB_S_SOCKET, include/ts/ob.h): its UUID, to open with ob_opn_obj.
 * E_ID: not the process's socket; E_NOEXS: a socket with no object
 * (made before the system clock was set).
 */
IMPORT ER  so_getobj( INT s, TS_UUID *p_uuid );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SOAPP_H__ */
