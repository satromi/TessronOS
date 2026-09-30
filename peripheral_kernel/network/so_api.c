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
 *	The T2EX socket calls over lwIP (design 12.6).
 *
 *	Each call is the lwIP one with its answer turned into an error code:
 *	lwIP follows BSD and reports -1 with errno set, while everything
 *	else here reports the error itself. The mapping below is the only
 *	real work; the rest is passing arguments through.
 *
 *	Each socket also has an owner: the process it was made for, or 0
 *	for the kernel. The number is lwIP's, and the owner is written when
 *	the socket is made and cleared when it is closed, so a number that
 *	comes round again is never taken for the one before it. The
 *	socket's real object is made and taken away at the same two points
 *	(obsock.c); the calls between never look at it.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/errno.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/if_api.h"
#include "lwip/netdb.h"
#include "ts_netif.h"
#include <ts/so.h>

#define SO_NUM		MEMP_NUM_NETCONN	/* socket numbers run 0 .. SO_NUM - 1 */

LOCAL struct netif	ts_netif;
LOCAL BOOL		so_started = FALSE;
LOCAL char		so_hostname[32] = "tessronos";
LOCAL ID		so_owner[SO_NUM];
LOCAL BOOL		so_asking = FALSE;	/* DHCP is running on the interface */
LOCAL UW		so_dns_fixed[2];	/* name servers the settings name */
LOCAL char		so_domain[80];

/* ---------------------------------------------------------------- errors */

/*
 * What BSD reports as errno becomes the nearest error code. Anything not
 * named here is a fault of the call itself rather than of the network,
 * so it comes out as E_SYS.
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
	case EPIPE:		return E_IO;
	case EOPNOTSUPP:	return E_NOSPT;
	case EAFNOSUPPORT:	return E_NOSPT;
	case EPROTONOSUPPORT:	return E_NOSPT;
	default:		return E_SYS;
	}
}

LOCAL ER so_err( void )
{
	return knl_so_errcode(errno);
}

/* A call that answers a count: the count, or the error that stopped it */
LOCAL INT so_ret_n( int n )
{
	return ( n < 0 ) ? (INT)so_err() : (INT)n;
}

/* A call that answers nothing but whether it worked */
LOCAL ER so_ret( int n )
{
	return ( n < 0 ) ? so_err() : E_OK;
}

/* ---------------------------------------------------------------- calls */

/* ---------------------------------------------------------------- owners */

LOCAL INT so_owned( INT s, ID owner )
{
	if ( s >= 0 && s < SO_NUM ) {
		so_owner[s] = owner;
	}
	return s;
}

EXPORT BOOL knl_so_mine( INT s, ID owner )
{
	return ( s >= 0 && s < SO_NUM && so_owner[s] == owner );
}

/* A process is ending: whatever sockets it left open are closed */
EXPORT void knl_so_prc_end( ID pid )
{
	INT	s;

	if ( pid <= 0 ) {
		return;
	}
	for ( s = 0; s < SO_NUM; s++ ) {
		if ( so_owner[s] == pid ) {
			(void)so_close(s);
		}
	}
}

EXPORT INT knl_so_socket_as( INT domain, INT type, INT protocol, ID owner )
{
	INT	s;

	if ( !so_started ) {
		return E_OBJ;
	}
	s = so_owned(so_ret_n(lwip_socket(domain, type, protocol)), owner);
	if ( s >= 0 ) {
		knl_so_obj_made(s, owner, domain, type, protocol);
	}
	return s;
}

EXPORT INT knl_so_accept_as( INT s, struct sockaddr *addr, socklen_t *addrlen, ID owner )
{
	INT	ns = so_owned(so_ret_n(lwip_accept(s, addr, addrlen)), owner);

	if ( ns >= 0 ) {
		knl_so_obj_accepted(ns, s, owner);
	}
	return ns;
}

/* ---------------------------------------------------------------- calls */

EXPORT INT so_socket( INT domain, INT type, INT protocol )
{
	return knl_so_socket_as(domain, type, protocol, 0);
}

/*
 * The object and the owner go first: once lwIP has the number back it
 * may hand it out again
 */
EXPORT ER so_close( INT s )
{
	knl_so_obj_gone(s);
	(void)so_owned(s, 0);
	return so_ret(lwip_close(s));
}

EXPORT ER so_bind( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	return so_ret(lwip_bind(s, name, namelen));
}

EXPORT ER so_connect( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	return so_ret(lwip_connect(s, name, namelen));
}

EXPORT ER so_listen( INT s, INT backlog )
{
	return so_ret(lwip_listen(s, backlog));
}

EXPORT INT so_accept( INT s, struct sockaddr *addr, socklen_t *addrlen )
{
	return knl_so_accept_as(s, addr, addrlen, 0);
}

EXPORT ER so_shutdown( INT s, INT how )
{
	return so_ret(lwip_shutdown(s, how));
}

EXPORT INT so_send( INT s, CONST void *data, SZ size, INT flags )
{
	return so_ret_n(lwip_send(s, data, (size_t)size, flags));
}

EXPORT INT so_recv( INT s, void *mem, SZ size, INT flags )
{
	return so_ret_n(lwip_recv(s, mem, (size_t)size, flags));
}

EXPORT INT so_sendto( INT s, CONST void *data, SZ size, INT flags,
		      CONST struct sockaddr *to, socklen_t tolen )
{
	return so_ret_n(lwip_sendto(s, data, (size_t)size, flags, to, tolen));
}

EXPORT INT so_recvfrom( INT s, void *mem, SZ size, INT flags,
			struct sockaddr *from, socklen_t *fromlen )
{
	return so_ret_n(lwip_recvfrom(s, mem, (size_t)size, flags, from, fromlen));
}

EXPORT INT so_read( INT s, void *mem, SZ size )
{
	return so_ret_n(lwip_read(s, mem, (size_t)size));
}

EXPORT INT so_write( INT s, CONST void *data, SZ size )
{
	return so_ret_n(lwip_write(s, data, (size_t)size));
}

/*
 * The timeout is the kernel's, so TMO_FEVR is no limit at all and
 * TMO_POL is a look without waiting.
 */
EXPORT INT so_select( INT maxfdp1, fd_set *readset, fd_set *writeset,
		      fd_set *exceptset, TMO tmout )
{
	struct timeval	tv;
	struct timeval	*ptv = &tv;

	if ( tmout == TMO_FEVR ) {
		ptv = NULL;
	} else if ( tmout == TMO_POL ) {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	} else if ( tmout < 0 ) {
		return E_PAR;
	} else {
		tv.tv_sec = tmout / 1000;
		tv.tv_usec = (tmout % 1000) * 1000;
	}

	return so_ret_n(lwip_select(maxfdp1, readset, writeset, exceptset, ptv));
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
	return so_ret_n(lwip_poll(fds, (nfds_t)nfds, (int)tmout));
}

EXPORT ER so_getsockopt( INT s, INT level, INT optname,
			 void *optval, socklen_t *optlen )
{
	return so_ret(lwip_getsockopt(s, level, optname, optval, optlen));
}

EXPORT ER so_setsockopt( INT s, INT level, INT optname,
			 CONST void *optval, socklen_t optlen )
{
	return so_ret(lwip_setsockopt(s, level, optname, optval, optlen));
}

EXPORT ER so_getsockname( INT s, struct sockaddr *name, socklen_t *namelen )
{
	return so_ret(lwip_getsockname(s, name, namelen));
}

EXPORT ER so_getpeername( INT s, struct sockaddr *name, socklen_t *namelen )
{
	return so_ret(lwip_getpeername(s, name, namelen));
}

EXPORT INT so_sendmsg( INT s, CONST struct msghdr *msg, INT flags )
{
	return so_ret_n((int)lwip_sendmsg(s, msg, flags));
}

EXPORT INT so_recvmsg( INT s, struct msghdr *msg, INT flags )
{
	return so_ret_n((int)lwip_recvmsg(s, msg, flags));
}

/*
 * Only the two the stack carries: how much has arrived (FIONREAD) and
 * whether a call may block (FIONBIO).
 */
EXPORT ER so_ioctl( INT s, INT cmd, void *argp )
{
	/* The command numbers carry their direction in the top bits,
	   so one of them looks negative in 32 bits. Widening it as
	   unsigned keeps the value the stack matches against. */
	return so_ret(lwip_ioctl(s, (long)(UW)cmd, argp));
}

/* F_GETFL answers the flags, the rest whether it worked */
EXPORT ER so_fcntl( INT s, INT cmd, INT val )
{
	int	n = lwip_fcntl(s, cmd, val);

	if ( n < 0 ) {
		return so_err();
	}
	return ( cmd == F_GETFL ) ? (ER)n : E_OK;
}

/* ---------------------------------------------------------------- names */

/*
 * An address for a name. A name that is not already a number needs a DNS
 * server, which a machine on its own does not have; the numeric forms
 * work either way.
 */
EXPORT ER so_getaddrinfo( CONST char *node, CONST char *service,
			  CONST struct addrinfo *hints, struct addrinfo **res )
{
	int	n;

	if ( res == NULL ) {
		return E_PAR;
	}
	n = lwip_getaddrinfo(node, service, hints, res);
	if ( n == EAI_NONAME && node != NULL && so_domain[0] != '\0' ) {
		/* a name with no dot in it is looked for in the domain as well */
		char	full[160];
		INT	i, k = 0;
		BOOL	dot = FALSE;

		for ( i = 0; node[i] != '\0'; i++ ) {
			if ( node[i] == '.' ) dot = TRUE;
		}
		if ( !dot && i + 1 + (INT)sizeof(so_domain) < (INT)sizeof(full) ) {
			for ( i = 0; node[i] != '\0'; i++ ) full[k++] = node[i];
			full[k++] = '.';
			for ( i = 0; so_domain[i] != '\0'; i++ ) full[k++] = so_domain[i];
			full[k] = '\0';
			n = lwip_getaddrinfo(full, service, hints, res);
		}
	}
	switch ( n ) {
	case 0:			return E_OK;
	case EAI_MEMORY:	return E_NOMEM;
	case EAI_FAMILY:	return E_NOSPT;
	case EAI_SERVICE:	return E_PAR;
	case EAI_NONAME:	return E_NOEXS;
	default:		return E_IO;
	}
}

EXPORT void so_freeaddrinfo( struct addrinfo *ai )
{
	lwip_freeaddrinfo(ai);
}

/*
 * The IPv4 address of a name, in network byte order: the first answer
 * of so_getaddrinfo, for a caller that wants no more than that.
 */
EXPORT ER so_resolve( CONST char *name, UW *p_addr )
{
	struct addrinfo	hints, *res = NULL;
	INT		i;
	ER		er;

	if ( name == NULL || p_addr == NULL ) {
		return E_PAR;
	}
	for ( i = 0; i < (INT)sizeof(hints); i++ ) {
		((UB *)&hints)[i] = 0;
	}
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
	if ( res != NULL ) {
		so_freeaddrinfo(res);
	}
	return er;
}

/* One byte of an address as up to three digits, and where that ended */
LOCAL socklen_t put_u8( char *buf, socklen_t at, socklen_t len, UINT v )
{
	char	d[3];
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
	u32_t				addr;
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
		addr = lwip_ntohl(sin->sin_addr.s_addr);
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
		at = put_u8(serv, 0, servlen, lwip_ntohs(sin->sin_port));
		serv[at] = '\0';
	}

	return E_OK;
}

/* ---------------------------------------------------------------- interfaces */

EXPORT ER so_ifindextoname( UINT ifindex, char *ifname )
{
	if ( ifname == NULL ) {
		return E_PAR;
	}
	return ( lwip_if_indextoname(ifindex, ifname) != NULL ) ? E_OK : E_NOEXS;
}

EXPORT UINT so_ifnametoindex( CONST char *ifname )
{
	if ( ifname == NULL ) {
		return 0;
	}
	return lwip_if_nametoindex(ifname);
}

/* ---------------------------------------------------------------- not carried */

EXPORT ER so_sockatmark( INT s )
{
	return E_NOSPT;			/* no out of band data */
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

EXPORT ER so_getifaddr( UW *p_addr, UW *p_mask, UW *p_gw )
{
	if ( !so_started ) {
		return E_OBJ;
	}
	if ( p_addr != NULL ) {
		*p_addr = (UW)ip4_addr_get_u32(netif_ip4_addr(&ts_netif));
	}
	if ( p_mask != NULL ) {
		*p_mask = (UW)ip4_addr_get_u32(netif_ip4_netmask(&ts_netif));
	}
	if ( p_gw != NULL ) {
		*p_gw = (UW)ip4_addr_get_u32(netif_ip4_gw(&ts_netif));
	}

	return E_OK;
}

/* The first name server, in network byte order; 0 for none */
EXPORT ER so_getdns( UW *p_dns )
{
	CONST ip_addr_t	*a;

	if ( p_dns == NULL ) {
		return E_PAR;
	}
	LOCK_TCPIP_CORE();
	a = dns_getserver(0);
	*p_dns = ( a != NULL ) ? (UW)ip4_addr_get_u32(ip_2_ip4(a)) : 0;
	UNLOCK_TCPIP_CORE();

	return E_OK;
}

/*
 * A fixed address for the interface, all in network byte order: asking
 * the network stops, and the address, mask and gateway are set in the
 * stack's own task. A name server of 0 leaves the one there was.
 */
EXPORT ER so_setifaddr( UW addr, UW mask, UW gw, UW dns )
{
	ip4_addr_t	a, m, g;
	ip_addr_t	d;

	if ( !so_started ) {
		return E_OBJ;
	}
	ip4_addr_set_u32(&a, addr);
	ip4_addr_set_u32(&m, mask);
	ip4_addr_set_u32(&g, gw);
#if LWIP_DHCP
	(void)netifapi_dhcp_release_and_stop(&ts_netif);
#endif
	so_asking = FALSE;
	if ( netifapi_netif_set_addr(&ts_netif, &a, &m, &g) != ERR_OK ) {
		return E_SYS;
	}
	if ( dns != 0 ) {
		ip_addr_set_ip4_u32_val(d, dns);
		LOCK_TCPIP_CORE();
		dns_setserver(0, &d);
		UNLOCK_TCPIP_CORE();
	}

	return E_OK;
}

/*
 * A name server the settings name. The network may hand out others with
 * an address, which would take its place; the interface's status
 * callback puts it back each time the address changes.
 */
LOCAL void dns_fixed( void )
{
	ip_addr_t	d;
	INT		i;

	for ( i = 0; i < 2; i++ ) {
		if ( so_dns_fixed[i] != 0 ) {
			ip_addr_set_ip4_u32_val(d, so_dns_fixed[i]);
			dns_setserver((u8_t)i, &d);
		}
	}
}

/* Called by the stack, in its own task, when the address changes */
LOCAL void so_status( struct netif *n )
{
	(void)n;
	dns_fixed();
}

EXPORT ER so_setdns( INT n, UW addr )
{
	if ( n < 0 || n > 1 ) {
		return E_PAR;
	}
	so_dns_fixed[n] = addr;
	if ( so_started ) {
		LOCK_TCPIP_CORE();
		dns_fixed();
		UNLOCK_TCPIP_CORE();
	}
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

/* Sockets of processes other than 'but' that are open */
EXPORT INT so_busy( ID but )
{
	INT	i, n = 0;

	for ( i = 0; i < SO_NUM; i++ ) {
		if ( so_owner[i] > 0 && so_owner[i] != but ) n++;
	}
	return n;
}

/* ---------------------------------------------------------------- start-up */

/*
 * The stack runs in a task of its own; tcpip_init starts it and calls
 * back once it is ready, which is what the semaphore waits for.
 */
LOCAL void tcpip_ready( void *arg )
{
	tk_sig_sem(*(ID *)arg, 1);
}

/*
 * Bring the stack up. The interface is given no address of its own: it
 * asks for one, and a machine with nothing answering keeps 0.0.0.0 and
 * can still talk to itself over the loopback interface.
 */
EXPORT ER so_main( void )
{
	T_CSEM		csem;
	ID		semid;
	ip4_addr_t	any;

	if ( so_started ) {
		return E_OK;
	}

	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem  = 1;
	semid = tk_cre_sem(&csem);
	if ( semid <= 0 ) {
		return E_LIMIT;
	}

	tcpip_init(tcpip_ready, &semid);
	if ( tk_wai_sem(semid, 1, 5000) < E_OK ) {
		tk_del_sem(semid);
		return E_TMOUT;		/* the stack did not come up */
	}
	tk_del_sem(semid);

	ip4_addr_set_zero(&any);
	if ( netif_add(&ts_netif, &any, &any, &any, NULL,
			ts_netif_init, tcpip_input) == NULL ) {
		return E_NOEXS;		/* no card to put an interface on */
	}
	netif_set_default(&ts_netif);
	netif_set_status_callback(&ts_netif, so_status);
	netif_set_up(&ts_netif);
	if ( ts_netif_link_up() ) {
		netif_set_link_up(&ts_netif);
	}
#if LWIP_DHCP && CNF_NET_DHCP
	/* Asking costs a timer of its own and gets no answer on a
	   machine with nothing listening, so it is off unless asked
	   for; so_dhcp_start() does the same thing later. */
	dhcp_start(&ts_netif);
	so_asking = TRUE;
#endif

	so_started = TRUE;
#if CNF_NET_SNTP
	(void)so_sntp_start();		/* the clock, once there is an address */
#endif

	return E_OK;
}

/*
 * Ask the network for an address. Nothing answers on a machine with
 * no server, and the interface simply keeps the address it had. The
 * asking is started in the stack's own task, as it must be.
 */
EXPORT ER so_dhcp_start( void )
{
#if LWIP_DHCP
	if ( !so_started ) {
		return E_OBJ;
	}
	if ( netifapi_dhcp_start(&ts_netif) != ERR_OK ) {
		return E_SYS;
	}
	so_asking = TRUE;
	return E_OK;
#else
	return E_NOSPT;
#endif
}

EXPORT ER so_finish( void )
{
	if ( !so_started ) {
		return E_OK;
	}
	netif_set_down(&ts_netif);
	so_started = FALSE;

	return E_OK;
}
