/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so.h
 *	Network API (design 12.6)
 *
 *	The T2EX socket calls. The shapes of the arguments are the BSD ones;
 *	what differs is that every call answers with an error code rather
 *	than setting errno. The stack underneath is NetBSD's (the default,
 *	TS_NETSTACK_NETBSD) or lwIP's (make NETSTACK=lwip). Either way the
 *	structures and the constants are the same: with the NetBSD stack
 *	they are include/ts/sodef.h, with lwIP its own headers, which have
 *	the same numbers and shapes.
 */

#ifndef __TS_SO_H__
#define __TS_SO_H__

#if TS_NETSTACK_NETBSD
#include <ts/sodef.h>
#else
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#define SO_MAX		MEMP_NUM_NETCONN	/* socket numbers run 0 .. SO_MAX - 1 */
#endif
#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the stack up: the interface, the addresses, and the threads */
IMPORT ER  so_main( void );
IMPORT ER  so_dhcp_start( void );	/* ask the network for an address */
IMPORT ER  so_sntp_start( void );	/* keep the system clock set from a time server (CNF_NET_SNTP) */
IMPORT ER  so_sntp_state( D *p_unix_ms );	/* E_OK once set; the Unix time of the last setting, ms */
IMPORT ER  so_finish( void );

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
/*
 * so_recvfrom that also gives the type of service (IPv4) or the traffic
 * class (IPv6) the datagram arrived with, its ECN in the two low bits,
 * when the socket asked for it (IP_RECVTOS, IPV6_RECVTCLASS); -1 in
 * *p_tos when it did not come
 */
IMPORT INT so_recvtos( INT s, void *mem, SZ size, INT flags,
		       struct sockaddr *from, socklen_t *fromlen, INT *p_tos );
IMPORT INT so_read( INT s, void *mem, SZ size );
IMPORT INT so_write( INT s, CONST void *data, SZ size );

IMPORT INT so_select( INT maxfdp1, fd_set *readset, fd_set *writeset,
		      fd_set *exceptset, TMO tmout );

IMPORT ER  so_getsockopt( INT s, INT level, INT optname,
			  void *optval, socklen_t *optlen );
IMPORT ER  so_setsockopt( INT s, INT level, INT optname,
			  CONST void *optval, socklen_t optlen );
IMPORT ER  so_getsockname( INT s, struct sockaddr *name, socklen_t *namelen );
IMPORT ER  so_getpeername( INT s, struct sockaddr *name, socklen_t *namelen );

IMPORT INT so_sendmsg( INT s, CONST struct msghdr *msg, INT flags );
IMPORT INT so_recvmsg( INT s, struct msghdr *msg, INT flags );

IMPORT ER  so_ioctl( INT s, INT cmd, void *argp );
IMPORT ER  so_fcntl( INT s, INT cmd, INT val );

/*
 * The flags of so_getnameinfo. There is no name lookup the other way
 * round: only the numeric answers are given and NI_NAMEREQD is refused.
 */
#define NI_NOFQDN	0x01
#define NI_NUMERICHOST	0x02
#define NI_NAMEREQD	0x04
#define NI_NUMERICSERV	0x08
#define NI_DGRAM	0x10

/* Names: the numeric forms work without a server; a name needs DNS */
IMPORT ER  so_getaddrinfo( CONST char *node, CONST char *service,
			   CONST struct addrinfo *hints,
			   struct addrinfo **res );
IMPORT void so_freeaddrinfo( struct addrinfo *ai );
IMPORT ER  so_getnameinfo( CONST struct sockaddr *sa, socklen_t salen,
			   char *host, socklen_t hostlen,
			   char *serv, socklen_t servlen, INT flags );

/* Interfaces, by the number the stack gave them */
IMPORT ER   so_ifindextoname( UINT ifindex, char *ifname );
IMPORT UINT so_ifnametoindex( CONST char *ifname );

/*
 * Out of band data, the resource limits and breaking a task out of a
 * blocked call are not carried: the stack underneath has none of them.
 * Each answers E_NOSPT so that a caller finds out rather than hangs.
 */
IMPORT ER  so_sockatmark( INT s );
IMPORT ER  so_resctl( INT kind, void *arg );
IMPORT ER  so_break( ID tskid );

IMPORT ER  so_gethostname( char *name, SZ namelen );
IMPORT ER  so_sethostname( CONST char *name, SZ namelen );

/* The address the interface ended up with, for a caller that wants it */
IMPORT ER  so_getifaddr( UW *p_addr, UW *p_mask, UW *p_gw );

/*
 * TessronOS calls. Addresses are IPv4 in network byte order, as
 * so_getifaddr gives them.
 *	so_poll		so_select over a list (struct pollfd), timeout as so_select's
 *	so_resolve	the first address of a name (numeric, or through DNS)
 *	so_getdns	the first name server, 0 for none
 *	so_setifaddr	a fixed address: stops DHCP; dns 0 keeps the server there was
 */
IMPORT INT so_poll( struct pollfd *fds, INT nfds, TMO tmout );
IMPORT ER  so_resolve( CONST char *name, UW *p_addr );
IMPORT ER  so_getdns( UW *p_dns );
IMPORT ER  so_setifaddr( UW addr, UW mask, UW gw, UW dns );

/*
 * Addresses of either family.
 *	so_resolveall	the addresses of a name, IPv4 and IPv6 (family
 *			AF_INET, AF_INET6 or AF_UNSPEC), up to max into
 *			addrs, the one to try first first; how many, or an
 *			error code. With SO_RA_ADDRCONFIG a family the
 *			interface has no address of to reach the world with
 *			is not asked for. A numeric address ("2001:db8::1",
 *			"fe80::1%neta0") is its own answer
 *	so_getifaddr6	the IPv6 addresses of the card's interface, up to
 *			max into list; how many. gw (16 bytes, or NULL) the
 *			router the default route goes through, all zero for
 *			none
 *	so_getdns6	the nth (0 up) IPv6 name server that is asked: those
 *			the settings name first, then those the routers
 *			named; E_NOEXS for none
 */
typedef struct {
	INT	family;		/* AF_INET or AF_INET6 */
	UINT	scope;		/* the interface of a link-local IPv6 address, else 0 */
	UB	addr[16];	/* network byte order; an IPv4 one in the first four */
} T_SOADDR;

#define SO_RA_ADDRCONFIG	0x0001

typedef struct {
	UB	addr[16];
	INT	plen;		/* the prefix length */
	UINT	flags;		/* SO_IF6_ */
	UINT	vltime;		/* seconds of its valid lifetime left; 0xffffffff: no end */
	UINT	pltime;		/* of its preferred lifetime */
} T_SOIF6;

#define SO_IF6_ANYCAST		0x01
#define SO_IF6_TENTATIVE	0x02	/* still being checked for a duplicate on the link */
#define SO_IF6_DUPLICATED	0x04	/* another machine on the link has it */
#define SO_IF6_DETACHED		0x08
#define SO_IF6_DEPRECATED	0x10	/* past its preferred lifetime */
#define SO_IF6_AUTOCONF		0x40	/* made from a router's prefix */
#define SO_IF6_TEMPORARY	0x80	/* a temporary address (RFC 8981) */

IMPORT INT so_resolveall( CONST char *name, INT family, INT flags, T_SOADDR *addrs, INT max );
IMPORT INT so_getifaddr6( T_SOIF6 *list, INT max, UB *gw );
IMPORT ER  so_getdns6( INT n, UB *addr );

/*
 * IPv6 of the card's interface, as the settings say (so_conf.c).
 *	so_setip6	SO_IP6_OFF: no IPv6 on it at all; SO_IP6_LINK: its
 *			link-local address only; SO_IP6_AUTO: addresses from
 *			the routers' advertisements (SLAAC), and temporary
 *			ones as well with SO_IP6_TEMP; SO_IP6_FIXED: addr
 *			with prefix length plen, and gw (NULL or all zero for
 *			none) as the default router. The same settings again
 *			change nothing
 *	so_getip6	what so_setip6 was last given (with SO_IP6_TEMP)
 *	so_setdns6	the nth (0, 1) IPv6 name server the settings name,
 *			asked before those the routers name; NULL or all
 *			zero for none
 */
#define SO_IP6_OFF		0
#define SO_IP6_LINK		1
#define SO_IP6_AUTO		2
#define SO_IP6_FIXED		3
#define SO_IP6_MODE		0x0f
#define SO_IP6_TEMP		0x100

IMPORT ER   so_setip6( UINT mode, CONST UB *addr, INT plen, CONST UB *gw );
IMPORT UINT so_getip6( void );
IMPORT ER   so_setdns6( INT n, CONST UB *addr );

/*
 * The settings of design 16.5.23.
 *	so_setdns	the nth (0, 1) name server, kept when the network hands
 *			out others with an address; 0 takes the network's
 *	so_setdomain	what a name without a dot is looked up with as well
 *	so_dhcp_on	the interface asks the network for its address
 *	so_busy		sockets processes other than 'but' have open
 *	so_conf		SO_CONF_APPLY: the settings' record read and made so;
 *			SO_CONF_BUSY: so_busy of the caller (so_conf.c)
 *	so_sntp_server	the time server by name; "" asks none
 */
#define SO_CONF_APPLY	1
#define SO_CONF_BUSY	2
IMPORT ER   so_setdns( INT n, UW addr );
IMPORT ER   so_setdomain( CONST char *domain );
IMPORT BOOL so_dhcp_on( void );
IMPORT INT  so_busy( ID but );
IMPORT ER   so_conf( UINT op );
IMPORT ER   so_sntp_server( CONST char *name );

/*
 * Sockets and their owners. One made for a process (owner, its pid) is
 * that process's alone and is closed when the process ends; the
 * kernel's own have owner 0. so_socket and so_accept make the kernel's.
 */
IMPORT INT  knl_so_socket_as( INT domain, INT type, INT protocol, ID owner );
IMPORT INT  knl_so_accept_as( INT s, struct sockaddr *addr, socklen_t *addrlen, ID owner );
IMPORT BOOL knl_so_mine( INT s, ID owner );
IMPORT void knl_so_prc_end( ID pid );

/*
 * The two limits on the sockets there are (design 9.2), as Linux and
 * NetBSD limit descriptors: the system's table (SO_MAX), whose last
 * SO_KERNEL_RESERVE numbers only the kernel is given, so that its name
 * lookups, DHCP, SLAAC and the clock go on when the processes have
 * taken the rest (E_LIMIT, ENFILE); and each process's own (EX_MFILE,
 * EMFILE), as RLIMIT_NOFILE: a soft limit that is the one that holds, and
 * a hard one the soft may be raised to. A process starts with its
 * parent's, the first ones with SO_PRC_SOFT and SO_PRC_HARD. Anyone may
 * lower either and raise the soft one up to the hard one; only an
 * administrator raises the hard one, and never past SO_PRC_CEIL, so that
 * no one program can take the table from the others.
 *	knl_prc_so_cur		the soft limit of process pid; SO_PRC_SOFT for
 *				an owner that is no process
 *	knl_prc_so_limit	both of them
 *	knl_prc_so_setlimit	both of them set; raise: the hard one may go up
 */
#if TS_NETSTACK_NETBSD
#define SO_KERNEL_RESERVE	32
#else
#define SO_KERNEL_RESERVE	0		/* lwIP: no numbers kept back */
#endif
#define SO_PRC_CEIL		( SO_MAX - SO_KERNEL_RESERVE )
#define SO_PRC_HARD		( ( SO_PRC_CEIL < 768 ) ? SO_PRC_CEIL : 768 )
#define SO_PRC_SOFT		( ( SO_PRC_HARD < 512 ) ? SO_PRC_HARD : 512 )
#ifndef EX_MFILE
#define EX_MFILE		(-24)		/* the process's limit (include/ts/fs.h) */
#endif

IMPORT INT  knl_prc_so_cur( ID pid );
IMPORT ER   knl_prc_so_limit( ID pid, INT *p_cur, INT *p_max );
IMPORT ER   knl_prc_so_setlimit( ID pid, INT cur, INT max, BOOL raise );

/*
 * Each socket is a real object (OB_T_CHANNEL, OB_S_SOCKET, peripheral_
 * kernel/obj/obsock.c) from when it is made until it is closed. The
 * stack says when a number is given out (made: domain, type and
 * protocol as asked; accepted: those of the socket it came to) and
 * when it is closed, before the number can be given out again (gone).
 * A socket made before the object layer is up, or before there is a
 * clock to make a UUID with, has no object.
 *	so_getobj	the UUID of socket s's object (E_ID: no such socket,
 *			E_NOEXS: it has none)
 *	knl_so_obj_list	the numbers and UUIDs of the objects of the
 *			sockets process pid owns, as many as fit; answers
 *			how many there are
 *	knl_so_recv_wait, knl_so_send_wait
 *			a receive or a send as a process's call waits: in
 *			slices, coming out with E_DISWAI when the process
 *			is being ended (so_prc.c)
 */
IMPORT void knl_so_obj_made( INT s, ID owner, INT domain, INT type, INT protocol );
IMPORT void knl_so_obj_accepted( INT s, INT from, ID owner );
IMPORT void knl_so_obj_gone( INT s );
IMPORT ER   so_getobj( INT s, TS_UUID *p_uuid );
IMPORT INT  knl_so_obj_list( ID pid, INT *nums, TS_UUID *uuids, INT max );
IMPORT INT  knl_so_recv_wait( INT s, void *mem, SZ size, INT flags );
IMPORT INT  knl_so_send_wait( INT s, CONST void *data, SZ size, INT flags );

/* The error code of a BSD errno value (SO_ERROR's) */
IMPORT ER   knl_so_errcode( INT e );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SO_H__ */
