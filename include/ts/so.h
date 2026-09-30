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
