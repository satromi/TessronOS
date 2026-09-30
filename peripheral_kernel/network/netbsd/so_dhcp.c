/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_dhcp.c
 *	An address from the network (DHCP, RFC 2131; design 12.6).
 *
 *	A task of its own asks for an address over a UDP socket of the
 *	stack. Until it has one, the interface's address is 0.0.0.0 with the
 *	broadcast address of all ones, which is what lets the stack send the
 *	broadcasts from port 68 and take the answers; the requests ask the
 *	server to broadcast its answers too, since the address they carry is
 *	not yet the interface's.
 *
 *	DISCOVER, OFFER, REQUEST, ACK: the address, its mask, the router and
 *	up to two name servers are then set (so_api.c). Half way through the
 *	lease the same server is asked to renew it, and at seven eighths any
 *	server; a lease that runs out takes the address away, and the asking
 *	starts again. A NAK starts it again at once.
 *
 *	so_dhcp_start asks again from the beginning; a fixed address set
 *	(so_setifaddr) stops the asking. Nothing answering is no error: the
 *	task keeps asking, less and less often.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include <ts/uuid.h>
#include "tstdlib.h"
#include "so_nb.h"

#define DHCP_TASK_PRI		30
#define DHCP_TASK_STKSZ		(8 * 1024)
#define DHCP_SLICE_MS		500		/* how often a wait looks whether to stop */
#define DHCP_FIRST_MS		4000		/* the first wait for an answer */
#define DHCP_LAST_MS		64000		/* the longest */
#define DHCP_RENEW_MIN_S	60

#define PORT_SERVER		67
#define PORT_CLIENT		68

#define BOOTREQUEST		1
#define BOOTREPLY		2
#define MSG_MAX			576
#define OPT_AT			240		/* after the fixed part and the cookie */

#define DHCPDISCOVER		1
#define DHCPOFFER		2
#define DHCPREQUEST		3
#define DHCPACK			5
#define DHCPNAK			6

#define OPT_PAD			0
#define OPT_MASK		1
#define OPT_ROUTER		3
#define OPT_DNS			6
#define OPT_HOSTNAME		12
#define OPT_REQ_ADDR		50
#define OPT_LEASE		51
#define OPT_MSGTYPE		53
#define OPT_SERVER		54
#define OPT_PARAMS		55
#define OPT_T1			58
#define OPT_T2			59
#define OPT_END			255

typedef struct {
	UW	yiaddr;
	UW	server;
	UW	mask;
	UW	router;
	UW	dns[2];
	UW	lease;			/* s; 0xffffffff for ever */
	UW	t1, t2;
	INT	type;
} LEASE;

LOCAL ID		dhcp_tsk = 0;
LOCAL volatile BOOL	dhcp_want = FALSE;
LOCAL volatile UINT	dhcp_gen = 0;		/* changes when asked to start again or stop */
LOCAL UB		dhcp_mac[NET_MAC_LEN];
LOCAL BOOL		dhcp_noaddr;		/* the interface has no address of its own */

/* ---------------------------------------------------------------- time */

LOCAL UD now_s( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000000ULL;
}

/* Sleep up to ms; FALSE when told to stop or start again meanwhile */
LOCAL BOOL nap( INT ms, UINT gen )
{
	while ( ms > 0 && dhcp_gen == gen ) {
		INT	slice = ( ms > DHCP_SLICE_MS ) ? DHCP_SLICE_MS : ms;

		(void)tk_slp_tsk(slice);
		ms -= slice;
	}
	return ( dhcp_gen == gen );
}

/* ---------------------------------------------------------------- messages */

LOCAL INT put_opt( UB *m, INT at, UB code, CONST void *data, INT len )
{
	if ( at + 2 + len >= MSG_MAX - 1 ) {
		return at;
	}
	m[at++] = code;
	m[at++] = (UB)len;
	knl_memcpy(m + at, data, len);
	return at + len;
}

/*
 * A request: DISCOVER, or REQUEST for 'want' from 'server' (both 0 when
 * renewing, where the address is in ciaddr instead). The answer is
 * asked to be broadcast unless the address is ours already.
 */
LOCAL INT make_msg( UB *m, UW xid, INT type, UW ciaddr, UW want, UW server )
{
	LOCAL CONST UB	params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_T1, OPT_T2 };
	char		host[32];
	UB		t = (UB)type;
	INT		at, n;

	knl_memset(m, 0, MSG_MAX);
	m[0] = BOOTREQUEST;
	m[1] = 1;				/* Ethernet */
	m[2] = NET_MAC_LEN;
	knl_memcpy(m + 4, &xid, 4);
	if ( ciaddr == 0 ) {
		m[10] = 0x80;			/* answer by broadcast */
	}
	knl_memcpy(m + 12, &ciaddr, 4);
	knl_memcpy(m + 28, dhcp_mac, NET_MAC_LEN);
	m[236] = 99; m[237] = 130; m[238] = 83; m[239] = 99;	/* the cookie */

	at = OPT_AT;
	at = put_opt(m, at, OPT_MSGTYPE, &t, 1);
	if ( want != 0 ) {
		at = put_opt(m, at, OPT_REQ_ADDR, &want, 4);
	}
	if ( server != 0 ) {
		at = put_opt(m, at, OPT_SERVER, &server, 4);
	}
	at = put_opt(m, at, OPT_PARAMS, params, sizeof(params));
	if ( so_gethostname(host, sizeof(host)) >= E_OK ) {
		for ( n = 0; host[n] != '\0'; n++ ) ;
		if ( n > 0 ) {
			at = put_opt(m, at, OPT_HOSTNAME, host, n);
		}
	}
	m[at++] = OPT_END;
	return ( at < 300 ) ? 300 : at;		/* the size BOOTP relays expect at least */
}

LOCAL UW get32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

LOCAL BOOL same_bytes( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*p = (CONST UB *)a, *q = (CONST UB *)b;

	while ( n-- > 0 ) {
		if ( *p++ != *q++ ) return FALSE;
	}
	return TRUE;
}

/* An answer to xid for this card, read into *l; FALSE when it is not one */
LOCAL BOOL read_msg( CONST UB *m, INT len, UW xid, LEASE *l )
{
	INT	at, code, olen;

	if ( len < OPT_AT + 3 || m[0] != BOOTREPLY
	  || !same_bytes(m + 4, &xid, 4)
	  || !same_bytes(m + 28, dhcp_mac, NET_MAC_LEN)
	  || m[236] != 99 || m[237] != 130 || m[238] != 83 || m[239] != 99 ) {
		return FALSE;
	}
	knl_memset(l, 0, sizeof(*l));
	knl_memcpy(&l->yiaddr, m + 16, 4);
	for ( at = OPT_AT; at < len; ) {
		code = m[at++];
		if ( code == OPT_PAD ) continue;
		if ( code == OPT_END || at >= len ) break;
		olen = m[at++];
		if ( at + olen > len ) break;
		switch ( code ) {
		case OPT_MSGTYPE:	if ( olen >= 1 ) l->type = m[at]; break;
		case OPT_MASK:		if ( olen >= 4 ) knl_memcpy(&l->mask, m + at, 4); break;
		case OPT_ROUTER:	if ( olen >= 4 ) knl_memcpy(&l->router, m + at, 4); break;
		case OPT_SERVER:	if ( olen >= 4 ) knl_memcpy(&l->server, m + at, 4); break;
		case OPT_LEASE:		if ( olen >= 4 ) l->lease = get32(m + at); break;
		case OPT_T1:		if ( olen >= 4 ) l->t1 = get32(m + at); break;
		case OPT_T2:		if ( olen >= 4 ) l->t2 = get32(m + at); break;
		case OPT_DNS:
			if ( olen >= 4 ) knl_memcpy(&l->dns[0], m + at, 4);
			if ( olen >= 8 ) knl_memcpy(&l->dns[1], m + at + 4, 4);
			break;
		default:
			break;
		}
		at += olen;
	}
	return ( l->type != 0 );
}

/* ---------------------------------------------------------------- the exchange */

/*
 * Send the message to 'to' (all ones for a broadcast) and wait up to ms
 * for an answer of one of the two types; FALSE when none came or the
 * asking was stopped.
 */
LOCAL BOOL exchange( INT s, UB *m, INT len, UW to, UW xid, INT ms, INT type1, INT type2,
		     LEASE *l, UINT gen )
{
	struct sockaddr_in	sa;
	struct pollfd		p;
	UB			r[MSG_MAX];
	INT			n, direct = ( to == 0xffffffffU && dhcp_noaddr ) ? 1 : 0;

	/* a broadcast from no address goes out on the interface itself, past the routes */
	(void)so_setsockopt(s, SOL_SOCKET, SO_DONTROUTE, &direct, sizeof(direct));
	knl_memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons(PORT_SERVER);
	sa.sin_addr.s_addr = to;
	if ( so_sendto(s, m, len, 0, (struct sockaddr *)&sa, sizeof(sa)) != len ) {
		(void)nap(ms, gen);
		return FALSE;
	}
	while ( ms > 0 && dhcp_gen == gen ) {
		p.fd = s;
		p.events = POLLIN;
		p.revents = 0;
		n = so_poll(&p, 1, ( ms > DHCP_SLICE_MS ) ? DHCP_SLICE_MS : ms);
		ms -= DHCP_SLICE_MS;
		if ( n <= 0 ) {
			continue;
		}
		while ( ( n = so_recv(s, r, sizeof(r), MSG_DONTWAIT) ) > 0 ) {
			if ( read_msg(r, n, xid, l) && ( l->type == type1 || l->type == type2 ) ) {
				return TRUE;
			}
		}
	}
	return FALSE;
}

LOCAL void say_ip( CONST char *what, UW a )
{
	a = so_ntohl(a);
	tm_printf((UB *)" %s %d.%d.%d.%d", what, (INT)( a >> 24 ), (INT)( ( a >> 16 ) & 0xff ),
		  (INT)( ( a >> 8 ) & 0xff ), (INT)( a & 0xff ));
}

/* The lease taken: the address and what came with it set, the times filled in */
LOCAL void take( LEASE *l, UINT gen )
{
	UW	a = so_ntohl(l->yiaddr);

	if ( l->mask == 0 ) {		/* the class of the address */
		l->mask = so_htonl(( a < 0x80000000U ) ? 0xff000000U
				: ( a < 0xc0000000U ) ? 0xffff0000U : 0xffffff00U);
	}
	if ( l->lease == 0 ) {
		l->lease = 3600;
	}
	if ( l->t1 == 0 || l->t1 >= l->lease ) {
		l->t1 = ( l->lease == 0xffffffffU ) ? l->lease : l->lease / 2;
	}
	if ( l->t2 == 0 || l->t2 >= l->lease || l->t2 <= l->t1 ) {
		l->t2 = ( l->lease == 0xffffffffU ) ? l->lease : l->lease - l->lease / 8;
	}
	if ( dhcp_gen != gen ) {
		return;				/* stopped meanwhile: a fixed address is set */
	}
	(void)knl_so_nb_ifaddr(l->yiaddr, l->mask, l->router, FALSE);
	knl_so_nb_ifdns(l->dns[0], l->dns[1]);
	dhcp_noaddr = FALSE;
	tm_printf((UB *)"net: DHCP");
	say_ip("address", l->yiaddr);
	say_ip("mask", l->mask);
	if ( l->router != 0 ) say_ip("gateway", l->router);
	if ( l->dns[0] != 0 ) say_ip("dns", l->dns[0]);
	tm_printf((UB *)" lease %d s\n", (INT)l->lease);
}

/*
 * One round of asking, from the start until the address is lost or the
 * asking is stopped or started again.
 */
LOCAL void dhcp_client( UINT gen )
{
	struct sockaddr_in	me;
	LEASE			offer, ack;
	UB			m[MSG_MAX];
	UW			xid;
	UD			got, now;
	INT			s, len, on = 1, wait = DHCP_FIRST_MS;
	UW			held = 0;		/* the address of the lease held */

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	if ( s < 0 ) {
		(void)nap(DHCP_LAST_MS, gen);
		return;
	}
	(void)so_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	(void)so_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	knl_memset(&me, 0, sizeof(me));
	me.sin_len = sizeof(me);
	me.sin_family = AF_INET;
	me.sin_port = so_htons(PORT_CLIENT);
	if ( so_bind(s, (struct sockaddr *)&me, sizeof(me)) < E_OK ) {
		so_close(s);
		(void)nap(DHCP_LAST_MS, gen);
		return;
	}

	/*
	 * An address the interface has already stays while the network is
	 * asked again, as connections may be using it; with none, the
	 * interface is made ready for the broadcasts.
	 */
	{
		UW	addr = 0, mask = 0;

		(void)rump_tsnet_getaddr4(SO_IFNAME, &addr, &mask);
		dhcp_noaddr = ( addr == 0 );
		if ( dhcp_noaddr ) {
			(void)knl_so_nb_ifaddr(0, 0, 0, TRUE);
		}
	}

	while ( dhcp_gen == gen ) {
		/* INIT: the address of a lease lost is taken away first */
		if ( held != 0 ) {
			(void)knl_so_nb_ifaddr(0, 0, 0, TRUE);
			dhcp_noaddr = TRUE;
			held = 0;
		}
		(void)ts_get_random(&xid, sizeof(xid));
		len = make_msg(m, xid, DHCPDISCOVER, 0, 0, 0);
		if ( !exchange(s, m, len, 0xffffffffU, xid, wait, DHCPOFFER, 0, &offer, gen) ) {
			wait = ( wait * 2 > DHCP_LAST_MS ) ? DHCP_LAST_MS : wait * 2;
			continue;
		}
		wait = DHCP_FIRST_MS;
		len = make_msg(m, xid, DHCPREQUEST, 0, offer.yiaddr, offer.server);
		if ( !exchange(s, m, len, 0xffffffffU, xid, DHCP_FIRST_MS, DHCPACK, DHCPNAK, &ack, gen)
		  || ack.type != DHCPACK ) {
			continue;
		}
		if ( ack.server == 0 ) {
			ack.server = offer.server;
		}
		take(&ack, gen);
		held = ack.yiaddr;
		got = now_s();

		/* BOUND, RENEWING, REBINDING */
		while ( dhcp_gen == gen ) {
			UD	t1 = got + ack.t1, t2 = got + ack.t2;
			UD	end = got + ack.lease;
			LEASE	r;
			BOOL	answered;

			now = now_s();
			if ( ack.lease == 0xffffffffU ) {
				(void)nap(DHCP_LAST_MS, gen);
				continue;
			}
			if ( now < t1 ) {
				(void)nap((INT)( ( t1 - now > 3600 ) ? 3600000 : ( t1 - now ) * 1000 ), gen);
				continue;
			}
			if ( now >= end ) {
				tm_printf((UB *)"net: DHCP lease ran out\n");
				break;
			}
			(void)ts_get_random(&xid, sizeof(xid));
			len = make_msg(m, xid, DHCPREQUEST, ack.yiaddr, 0, 0);
			answered = exchange(s, m, len, ( now < t2 ) ? ack.server : 0xffffffffU, xid,
					    DHCP_FIRST_MS, DHCPACK, DHCPNAK, &r, gen);
			if ( answered && r.type == DHCPNAK ) {
				tm_printf((UB *)"net: DHCP lease refused\n");
				break;
			}
			if ( answered ) {
				if ( r.server == 0 ) r.server = ack.server;
				r.yiaddr = ack.yiaddr;
				ack = r;
				take(&ack, gen);
				got = now_s();
				continue;
			}
			/* no answer: again in half the time left, not sooner than a minute */
			now = now_s();
			{
				UD	left = ( ( now < t2 ) ? t2 : end ) - now;
				UD	next = ( left / 2 < DHCP_RENEW_MIN_S ) ? DHCP_RENEW_MIN_S : left / 2;

				(void)nap((INT)( ( next > 3600 ) ? 3600000 : next * 1000 ), gen);
			}
		}
	}
	so_close(s);
}

LOCAL void dhcp_task( INT stacd, void *exinf )
{
	UINT	gen;

	(void)net_get_mac(dhcp_mac);
	for ( ;; ) {
		while ( !dhcp_want ) {
			(void)tk_slp_tsk(TMO_FEVR);
		}
		gen = dhcp_gen;
		dhcp_client(gen);
	}
}

/* Ask from the beginning: the task is started, or told to start again */
EXPORT ER knl_so_dhcp_run( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	dhcp_gen++;
	dhcp_want = TRUE;
	if ( dhcp_tsk > 0 ) {
		(void)tk_wup_tsk(dhcp_tsk);
		return E_OK;
	}
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)dhcp_task;
	ctsk.itskpri = DHCP_TASK_PRI;
	ctsk.stksz   = DHCP_TASK_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		return (ER)tskid;
	}
	dhcp_tsk = tskid;
	return tk_sta_tsk(tskid, 0);
}

/* Stop asking; whatever address there is stays */
EXPORT void knl_so_dhcp_stop( void )
{
	if ( !dhcp_want ) {
		return;
	}
	dhcp_want = FALSE;
	dhcp_gen++;
	if ( dhcp_tsk > 0 ) {
		(void)tk_wup_tsk(dhcp_tsk);
	}
}
