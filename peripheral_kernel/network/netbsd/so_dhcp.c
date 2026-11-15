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
 *	A task of its own asks for an address. The broadcasts go to the card
 *	as whole frames past the stack, from 0.0.0.0 (or the address being
 *	renewed) to 255.255.255.255 (broadcast below); a renewal sent to the
 *	server itself, and every answer, go through a UDP socket of the
 *	stack bound to port 68. Until it has an address, the interface's
 *	address is 0.0.0.0 with the broadcast address of all ones, which is
 *	what lets the stack take the answers; the requests ask the server to
 *	broadcast its answers too, since the address they carry is not yet
 *	the interface's.
 *
 *	DISCOVER, OFFER, REQUEST, ACK: the address, its mask, the router and
 *	up to two name servers are then set (so_api.c). A REQUEST not
 *	answered is sent again with the same transaction ID after 4, 8 and
 *	16 s, each wait made up to a second longer or shorter at random
 *	(RFC 2131 4.1, as Linux's DHCP clients do), and only when the fourth
 *	has gone unanswered as well does the asking start again from a
 *	DISCOVER. A lease is kept for each network it was taken on (the
 *	card, and the SSID for the Wi-Fi's; net_key below): asking again
 *	on a network with a lease kept that has not run out first asks for
 *	its address (INIT-REBOOT, RFC 2131 3.2 and 4.3.2), and from a
 *	DISCOVER only when that is refused or not answered; on a network
 *	with none it asks from a DISCOVER at once, and an address the
 *	interface still has from another network is taken away first.
 *	Every message
 *	carries the seconds since the round of asking began (secs, RFC 2131
 *	2). Half way through the
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
#include "netlog.h"			/* the lines timed */
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
#define DHCP_REQ_TRIES		4		/* the REQUESTs for one offer: waits of 4, 8, 16, 32 s */
#define DHCP_FUZZ_MS		1000		/* each wait up to this longer or shorter (RFC 2131 4.1) */
#define DHCP_ACD_POLL_MS	20		/* how often the new address's check is looked at */
#define DHCP_ACD_MAX_MS		3000		/* the longest the check is waited for */
#define DHCP_DECLINE_WAIT_MS	10000		/* after a DECLINE, before asking again (RFC 2131 3.1) */

#define PORT_SERVER		67
#define PORT_CLIENT		68

#define BOOTREQUEST		1
#define BOOTREPLY		2
#define MSG_MAX			576
#define OPT_AT			240		/* after the fixed part and the cookie */

#define DHCPDISCOVER		1
#define DHCPOFFER		2
#define DHCPREQUEST		3
#define DHCPDECLINE		4
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
#define OPT_MESSAGE		56
#define OPT_T1			58
#define OPT_T2			59
#define OPT_CLIENT_ID		61
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
	char	msg[64];		/* the server's message (option 56), as much as fits */
} LEASE;

LOCAL ID		dhcp_tsk = 0;
LOCAL volatile BOOL	dhcp_want = FALSE;
LOCAL volatile UINT	dhcp_gen = 0;		/* changes when asked to start again or stop */
LOCAL UB		dhcp_mac[NET_MAC_LEN];

/*
 * The network a lease is of: the card it was taken with (the client
 * identifier carries its address, so another card has a lease of its
 * own) and, for the Wi-Fi's, the SSID of the network joined; a wired
 * card's is the card alone, as dhcpcd keeps one lease file for an
 * interface and one for each SSID on a wireless one. The BSSID is not
 * a part of it: the access points of one network (one SSID) share its
 * DHCP server and its addresses, and a move between them keeps the
 * lease.
 */
typedef struct {
	UB	mac[NET_MAC_LEN];
	UB	ssid_len;		/* 0: a wired card's */
	UB	ssid[32];
} NETKEY;

/*
 * The leases taken in this boot, one for each network, for asking for
 * the address again when the network is joined again (INIT-REBOOT).
 * As many as DHCP_NETS; when another is taken with all of them in use,
 * the one used longest ago gives its place. They are not kept over a
 * restart (design 9.3.1).
 */
#define DHCP_NETS		16

typedef struct {
	BOOL	used;
	NETKEY	key;
	LEASE	l;
	UD	end;			/* s: when it runs out; ~0 never */
	UD	at;			/* s: when it was last taken or renewed */
} NETLEASE;

LOCAL NETLEASE		dhcp_nets[DHCP_NETS];
LOCAL volatile BOOL	dhcp_forget = FALSE;	/* all forgotten as the next round begins */

/* The SSID of the Wi-Fi's network when mac is its card's and it is connected (wifi.c); weak: no Wi-Fi */
IMPORT INT	knl_wifi_ssid_of( CONST UB *mac, UB *ssid ) __attribute__((weak));

/* ---------------------------------------------------------------- time */

LOCAL UD now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000ULL;
}

LOCAL UD now_s( void )
{
	return now_ms() / 1000ULL;
}

/* The secs field: the seconds since t0_ms, as many as it holds */
LOCAL UH secs_since( UD t0_ms )
{
	UD	s = ( now_ms() - t0_ms ) / 1000ULL;

	return ( s > 0xffffULL ) ? 0xffff : (UH)s;
}

/* A wait of a retransmission: ms, up to DHCP_FUZZ_MS longer or shorter at random */
LOCAL INT fuzzed( INT ms )
{
	UW	r = 0;

	(void)ts_get_random(&r, sizeof(r));
	return ms - DHCP_FUZZ_MS + (INT)( r % ( 2 * DHCP_FUZZ_MS + 1 ) );
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
 * renewing, where the address is in ciaddr instead; server 0 alone when
 * asking for the address of the lease held, INIT-REBOOT), secs seconds
 * after the asking began; or DECLINE of 'want' to 'server', with only
 * what RFC 2131 4.4.1's table 5 lets a DECLINE carry. The answer is
 * asked to be broadcast unless the address is ours already.
 */
LOCAL INT make_msg( UB *m, UW xid, INT type, UW ciaddr, UW want, UW server, UH secs )
{
	LOCAL CONST UB	params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_T1, OPT_T2 };
	char		host[32];
	UB		cid[1 + NET_MAC_LEN];
	UB		t = (UB)type;
	INT		at, n;

	knl_memset(m, 0, MSG_MAX);
	m[0] = BOOTREQUEST;
	m[1] = 1;				/* Ethernet */
	m[2] = NET_MAC_LEN;
	knl_memcpy(m + 4, &xid, 4);
	m[8] = (UB)( secs >> 8 );
	m[9] = (UB)secs;
	if ( ciaddr == 0 && type != DHCPDECLINE ) {
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
	if ( type != DHCPDECLINE ) {
		at = put_opt(m, at, OPT_PARAMS, params, sizeof(params));
	}
	/* the client identifier: the hardware type and the card's address, so that
	 * the server keeps one lease for this machine whatever system asks for it */
	cid[0] = 1;
	knl_memcpy(cid + 1, dhcp_mac, NET_MAC_LEN);
	at = put_opt(m, at, OPT_CLIENT_ID, cid, 1 + NET_MAC_LEN);
	if ( type != DHCPDECLINE && so_gethostname(host, sizeof(host)) >= E_OK ) {
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
	INT	at, code, olen, n;

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
		case OPT_MESSAGE:
			for ( n = 0; n < olen && n < (INT)sizeof(l->msg) - 1; n++ ) {
				l->msg[n] = ( m[at + n] >= 0x20 && m[at + n] < 0x7f ) ? (char)m[at + n] : '?';
			}
			l->msg[n] = 0;
			break;
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

/* ---------------------------------------------------------------- the leases of the networks */

/* The network asked on now: the card in use and, when it is the Wi-Fi's, the network joined */
LOCAL void net_key( NETKEY *k )
{
	knl_memset(k, 0, sizeof(*k));
	knl_memcpy(k->mac, dhcp_mac, NET_MAC_LEN);
	if ( knl_wifi_ssid_of != NULL ) {
		INT	n = knl_wifi_ssid_of(dhcp_mac, k->ssid);

		k->ssid_len = (UB)( ( n > 0 && n <= 32 ) ? n : 0 );
	}
}

LOCAL BOOL same_key( CONST NETKEY *a, CONST NETKEY *b )
{
	return same_bytes(a->mac, b->mac, NET_MAC_LEN) && a->ssid_len == b->ssid_len
		&& same_bytes(a->ssid, b->ssid, a->ssid_len);
}

/* The network for the console: the Wi-Fi's "SSID" or the wired card, and the card's address */
LOCAL INT say_net( UB *b, CONST NETKEY *k )
{
	INT	n = 0, i;

	if ( k->ssid_len > 0 ) {
		n = tm_sprintf(b, (UB *)"the Wi-Fi's network \"");
		for ( i = 0; i < k->ssid_len; i++ ) {
			b[n++] = ( k->ssid[i] >= 0x20 && k->ssid[i] < 0x7f ) ? k->ssid[i] : '.';
		}
		b[n++] = '"';
	} else {
		n = tm_sprintf(b, (UB *)"the wired link");
	}
	return n + tm_sprintf(b + n, (UB *)" (card %02x:%02x:%02x:%02x:%02x:%02x)", k->mac[0], k->mac[1],
			      k->mac[2], k->mac[3], k->mac[4], k->mac[5]);
}

/* The lease kept for the network k, not run out: its place, or -1; one run out is forgotten */
LOCAL INT net_find( CONST NETKEY *k )
{
	INT	i;

	for ( i = 0; i < DHCP_NETS; i++ ) {
		if ( dhcp_nets[i].used && same_key(&dhcp_nets[i].key, k) ) {
			if ( dhcp_nets[i].end > now_s() ) return i;
			dhcp_nets[i].used = FALSE;
			return -1;
		}
	}
	return -1;
}

/* The lease, not run out, whose address is addr and taken with this card: its place, or -1 */
LOCAL INT net_of_addr( UW addr )
{
	INT	i;

	for ( i = 0; i < DHCP_NETS; i++ ) {
		if ( dhcp_nets[i].used && dhcp_nets[i].l.yiaddr == addr && dhcp_nets[i].end > now_s() ) {
			return i;
		}
	}
	return -1;
}

/* The lease l taken or renewed on the network k at got (s) kept for it */
LOCAL void net_keep( CONST NETKEY *k, CONST LEASE *l, UD got )
{
	INT	i, at = -1;

	for ( i = 0; i < DHCP_NETS; i++ ) {
		if ( dhcp_nets[i].used && same_key(&dhcp_nets[i].key, k) ) {
			at = i;
			break;
		}
	}
	for ( i = 0; at < 0 && i < DHCP_NETS; i++ ) {
		if ( !dhcp_nets[i].used ) at = i;
	}
	if ( at < 0 ) {				/* all in use: the one used longest ago */
		for ( at = 0, i = 1; i < DHCP_NETS; i++ ) {
			if ( dhcp_nets[i].at < dhcp_nets[at].at ) at = i;
		}
	}
	/* another network's lease of the same address with this card is of no use any more */
	for ( i = 0; i < DHCP_NETS; i++ ) {
		if ( i != at && dhcp_nets[i].used && dhcp_nets[i].l.yiaddr == l->yiaddr
		  && same_bytes(dhcp_nets[i].key.mac, k->mac, NET_MAC_LEN) ) {
			dhcp_nets[i].used = FALSE;
		}
	}
	dhcp_nets[at].used = TRUE;
	dhcp_nets[at].key = *k;
	dhcp_nets[at].l = *l;
	dhcp_nets[at].end = ( l->lease == 0xffffffffU ) ? ~(UD)0 : got + l->lease;
	dhcp_nets[at].at = got;
}

/* The lease of the network k forgotten: refused, given back, or run out */
LOCAL void net_drop( CONST NETKEY *k )
{
	INT	i;

	for ( i = 0; i < DHCP_NETS; i++ ) {
		if ( dhcp_nets[i].used && same_key(&dhcp_nets[i].key, k) ) dhcp_nets[i].used = FALSE;
	}
}

/* ---------------------------------------------------------------- the exchange */

/* A frame past the stack to the card in use (neta_user.c); 0 when it went into the ring */
IMPORT int	knl_neta_send_frame( CONST void *frame, UD len );

LOCAL UW sum16( UW sum, CONST UB *p, INT n )
{
	INT	i;

	for ( i = 0; i + 1 < n; i += 2 ) {
		sum += ( (UW)p[i] << 8 ) | p[i + 1];
	}
	if ( i < n ) {
		sum += (UW)p[i] << 8;
	}
	return sum;
}

LOCAL UH fold( UW sum )
{
	while ( ( sum >> 16 ) != 0 ) {
		sum = ( sum & 0xffff ) + ( sum >> 16 );
	}
	return (UH)~sum;
}

LOCAL void put16( UB *p, UW v )
{
	p[0] = (UB)( v >> 8 );
	p[1] = (UB)v;
}

/*
 * A message broadcast to the servers: an Ethernet frame to all, an IP
 * header from 'from' to 255.255.255.255 and a UDP header from port 68 to
 * 67 made here and handed to the card past the stack, as BSD's dhclient
 * and dhcpcd write it to BPF and Linux's clients to a packet socket.
 * Through a socket of the stack, a datagram to 255.255.255.255 leaves
 * for the broadcast address of the interface's address instead
 * (in_pcbconnect puts it there once the interface has an address): with
 * the address of the network left behind still held, the broadcasts of
 * a DHCP that asks again went to that network's broadcast address
 * (10.0.2.255), which a server on another network does not take. 'from'
 * is 0.0.0.0 except when the address of the lease is renewed from any
 * server (ciaddr, RFC 2131 4.1 and 4.4.5).
 */
LOCAL INT broadcast( CONST UB *m, INT len, UW from )
{
	LOCAL UH	ip_id = 0;
	UB		f[14 + 20 + 8 + MSG_MAX];
	UB		*ip = f + 14, *udp = f + 34;
	UW		sum;

	if ( len > MSG_MAX ) {
		return E_PAR;
	}
	knl_memset(f, 0xff, 6);				/* to all */
	knl_memcpy(f + 6, dhcp_mac, NET_MAC_LEN);
	put16(f + 12, 0x0800);				/* IPv4 */
	ip[0] = 0x45;					/* version 4, a header of 20 bytes */
	ip[1] = 0;
	put16(ip + 2, 20 + 8 + (UW)len);
	put16(ip + 4, ++ip_id);
	put16(ip + 6, 0);
	ip[8] = 64;					/* TTL */
	ip[9] = 17;					/* UDP */
	put16(ip + 10, 0);
	knl_memcpy(ip + 12, &from, 4);
	knl_memset(ip + 16, 0xff, 4);
	put16(ip + 10, fold(sum16(0, ip, 20)));
	put16(udp, PORT_CLIENT);
	put16(udp + 2, PORT_SERVER);
	put16(udp + 4, 8 + (UW)len);
	put16(udp + 6, 0);
	knl_memcpy(udp + 8, m, (SZ)len);
	/* the pseudo header: the addresses, the protocol and the UDP length */
	sum = sum16(0, ip + 12, 8) + 17 + 8 + (UW)len;
	sum = fold(sum16(sum, udp, 8 + len));
	put16(udp + 6, ( sum == 0 ) ? 0xffff : sum);
	return ( knl_neta_send_frame(f, (UD)( 14 + 20 + 8 + len )) == 0 ) ? len : E_IO;
}

/*
 * Send the message to 'to' (all ones for a broadcast) and wait up to ms
 * for an answer of one of the two types; FALSE when none came or the
 * asking was stopped. The answers come in through the socket, which is
 * bound to port 68.
 */
LOCAL BOOL exchange( INT s, UB *m, INT len, UW to, UW xid, INT ms, INT type1, INT type2,
		     LEASE *l, UINT gen )
{
	struct sockaddr_in	sa;
	struct pollfd		p;
	UB			r[MSG_MAX];
	UW			ciaddr;
	INT			n;

	if ( to == 0xffffffffU ) {
		knl_memcpy(&ciaddr, m + 12, 4);
		n = broadcast(m, len, ciaddr);
	} else {
		knl_memset(&sa, 0, sizeof(sa));
		sa.sin_len = sizeof(sa);
		sa.sin_family = AF_INET;
		sa.sin_port = so_htons(PORT_SERVER);
		sa.sin_addr.s_addr = to;
		n = so_sendto(s, m, len, 0, (struct sockaddr *)&sa, sizeof(sa));
	}
	if ( n != len ) {
		tm_printf((UB *)"net: DHCP: not sent (%d)\n", n);
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

/* " what a.b.c.d" put after the n bytes of the line in b: its length now */
LOCAL INT say_ip( UB *b, INT n, CONST char *what, UW a )
{
	a = so_ntohl(a);
	return n + tm_sprintf(b + n, (UB *)" %s %d.%d.%d.%d", what, (INT)( a >> 24 ), (INT)( ( a >> 16 ) & 0xff ),
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
	/* the line made whole and said at once: another task's line may come between two calls */
	{
		UB	line[160];
		INT	n = 0;

		n = say_ip(line, n, "address", l->yiaddr);
		n = say_ip(line, n, "mask", l->mask);
		if ( l->router != 0 ) n = say_ip(line, n, "gateway", l->router);
		if ( l->dns[0] != 0 ) n = say_ip(line, n, "dns", l->dns[0]);
		line[n] = 0;
		tm_printf((UB *)"net: DHCP%s lease %d s\n", line, (INT)l->lease);
	}
}

/*
 * The new address checked against the other machines on the link: the
 * stack probes with ARP before it uses an address it did not have (RFC
 * 5227, in at most 200 ms as NetworkManager does it;
 * tools/netbsd/patches/if_arp-dad-timeout.patch) and marks it another
 * machine's when one answers. TRUE when it is another's.
 */
LOCAL BOOL addr_taken( UW addr, UINT gen )
{
	UD	t0 = now_ms();
	INT	st = rump_tsnet_addrstate4(SO_IFNAME, addr);
	UB	line[48];

	while ( st == 1 && dhcp_gen == gen && now_ms() - t0 < DHCP_ACD_MAX_MS ) {
		(void)tk_slp_tsk(DHCP_ACD_POLL_MS);
		st = rump_tsnet_addrstate4(SO_IFNAME, addr);
	}
	(void)say_ip(line, 0, "address", addr);
	tm_printf((UB *)"net: DHCP: the%s checked against the other machines in %d ms: %s\n", line,
		  (INT)( now_ms() - t0 ),
		  ( st == 0 ) ? "not in use" : ( st == 2 ) ? "another machine's" :
		  ( st == 1 ) ? "still being checked" : ( st == 3 ) ? "not checked (the link is down)" : "gone");
	return ( st == 2 );
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
	UD			got, now, round0 = 0, renew0 = 0;
	INT			s, len, on = 1, wait = DHCP_FIRST_MS, rwait, sent;
	UW			held = 0;		/* the address of the lease held */
	UW			reboot = 0;		/* the address to ask for first (INIT-REBOOT) */
	BOOL			acked, first = TRUE;
	UB			line[96], net[96];
	NETKEY			key;			/* the network asked on */
	LEASE			kept;			/* the lease kept for it */
	UD			t_begin = now_ms();	/* the asking began: the network joined, or asked again */

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
	 * The network asked on, and the lease kept for it. An address the
	 * interface has already stays while the network is asked again, as
	 * connections may be using it, when it is this network's lease's or
	 * of no lease known. The address of a lease taken on another network
	 * (another SSID, or another card: the stack moved onto another
	 * adapter) is of no use here: it is taken away, and the asking is
	 * from no address (RFC 2131 4.1), as dhcpcd drops the lease of the
	 * network left and Linux leaves an address with the interface it was
	 * given to. With no address, the interface is made ready for the
	 * broadcasts. The address of this network's lease, not run out, is
	 * asked for first, held or not; with none, a DISCOVER is sent at once.
	 */
	if ( dhcp_forget ) {
		dhcp_forget = FALSE;
		knl_memset(dhcp_nets, 0, sizeof(dhcp_nets));
		tm_printf((UB *)"net: DHCP: the leases kept for the networks forgotten\n");
	}
	net_key(&key);
	(void)say_net(net, &key);
	{
		UW	addr = 0, mask = 0;
		INT	k = net_find(&key), o;

		knl_memset(&kept, 0, sizeof(kept));
		if ( k >= 0 ) {
			kept = dhcp_nets[k].l;
		}
		(void)rump_tsnet_getaddr4(SO_IFNAME, &addr, &mask);
		if ( addr != 0 && ( k < 0 || addr != kept.yiaddr ) && ( o = net_of_addr(addr) ) >= 0 ) {
			UB	other[96];

			(void)say_net(other, &dhcp_nets[o].key);
			(void)say_ip(line, 0, "address", addr);
			tm_printf((UB *)"net: DHCP: the%s was leased on %s, not on %s: taken away\n", line, other, net);
			addr = 0;
		}
		if ( addr == 0 ) {
			(void)knl_so_nb_ifaddr(0, 0, 0, TRUE);
		}
		if ( k >= 0 ) {
			reboot = kept.yiaddr;
			(void)say_ip(line, 0, "address", reboot);
			if ( dhcp_nets[k].end == ~(UD)0 ) {
				tm_printf((UB *)"net: DHCP: asking on %s: a lease kept for it,%s, for ever\n", net, line);
			} else {
				tm_printf((UB *)"net: DHCP: asking on %s: a lease kept for it,%s, %d s left\n", net, line,
					  (INT)( dhcp_nets[k].end - now_s() ));
			}
		} else {
			tm_printf((UB *)"net: DHCP: asking on %s: no lease kept for it: a DISCOVER at once\n", net);
		}
	}

	while ( dhcp_gen == gen ) {
		acked = FALSE;
		sent = 0;

		/* INIT-REBOOT: the address held asked for, from no address and to no server in particular */
		if ( reboot != 0 ) {
			round0 = now_ms();
			(void)ts_get_random(&xid, sizeof(xid));
			len = make_msg(m, xid, DHCPREQUEST, 0, reboot, 0, 0);
			(void)say_ip(line, 0, "address", reboot);
			tm_printf((UB *)"net: DHCP: the lease held asked for first (INIT-REBOOT):%s\n", line);
			ack.type = 0;
			rwait = fuzzed(DHCP_FIRST_MS);
			acked = exchange(s, m, len, 0xffffffffU, xid, rwait, DHCPACK, DHCPNAK, &ack, gen)
				&& ack.type == DHCPACK;
			if ( ack.type == DHCPNAK ) {
				held = reboot;		/* not to be used any more: taken away below */
				net_drop(&key);
			}
			reboot = 0;
			if ( dhcp_gen != gen ) {
				break;
			}
			if ( acked ) {
				if ( ack.server == 0 ) {
					ack.server = kept.server;
				}
				tm_printf((UB *)"net: DHCP: the address of the lease held acknowledged in %d ms\n",
					  (INT)( now_ms() - round0 ));
			} else {
				tm_printf((UB *)"net: DHCP: the address of the lease held %s%s; asked for from a DISCOVER\n",
					  ( ack.type == DHCPNAK ) ? "was refused (NAK), the server's message: " : "was not answered",
					  ( ack.type == DHCPNAK ) ? ack.msg : "");
				round0 = 0;		/* a new transaction for the DISCOVER */
			}
		}
		if ( !acked ) {
			/* INIT: the address of a lease lost is taken away first */
			if ( held != 0 ) {
				(void)knl_so_nb_ifaddr(0, 0, 0, TRUE);
				held = 0;
			}
			/*
			 * One transaction for the round: a DISCOVER sent again keeps
			 * it, with secs grown, as systemd-networkd and ISC's dhclient
			 * send it again (RFC 2131 4.1, 4.4.1); a new one only when the
			 * round begins again (a NAK, an offer not confirmed, a DECLINE,
			 * a lease lost)
			 */
			if ( round0 == 0 ) {
				round0 = now_ms();
				(void)ts_get_random(&xid, sizeof(xid));
			}
			len = make_msg(m, xid, DHCPDISCOVER, 0, 0, 0, secs_since(round0));
			if ( !exchange(s, m, len, 0xffffffffU, xid, wait, DHCPOFFER, 0, &offer, gen) ) {
				/* what the asking came to, for the records: nothing answered in the time */
				if ( dhcp_gen == gen ) {
					tm_printf((UB *)"net: DHCP: no offer in %d s to a DISCOVER from %02x:%02x:%02x:%02x:%02x:%02x (xid %08x);"
						  " sent again\n", wait / 1000, dhcp_mac[0], dhcp_mac[1], dhcp_mac[2], dhcp_mac[3],
						  dhcp_mac[4], dhcp_mac[5], (UINT)so_ntohl(xid));
				}
				wait = ( wait * 2 > DHCP_LAST_MS ) ? DHCP_LAST_MS : wait * 2;
				continue;
			}
			wait = DHCP_FIRST_MS;
			/*
			 * REQUESTING: the REQUEST sent again with the same transaction
			 * until it is answered or DHCP_REQ_TRIES have gone unanswered
			 */
			(void)say_ip(line, 0, "address", offer.yiaddr);
			ack.type = 0;
			for ( rwait = DHCP_FIRST_MS; ; rwait *= 2 ) {
				len = make_msg(m, xid, DHCPREQUEST, 0, offer.yiaddr, offer.server, secs_since(round0));
				sent++;
				if ( exchange(s, m, len, 0xffffffffU, xid, fuzzed(rwait), DHCPACK, DHCPNAK, &ack, gen) ) {
					break;
				}
				if ( dhcp_gen != gen || sent >= DHCP_REQ_TRIES ) {
					break;
				}
				tm_printf((UB *)"net: DHCP: no answer in about %d s to the REQUEST for%s (xid %08x); sent again, %d of %d\n",
					  rwait / 1000, line, (UINT)so_ntohl(xid), sent + 1, DHCP_REQ_TRIES);
			}
			if ( ack.type != DHCPACK ) {
				if ( dhcp_gen == gen ) {
					tm_printf((UB *)"net: DHCP: the offer of%s %s after %d REQUESTs%s%s%s; asked again\n", line,
						  ( ack.type == DHCPNAK ) ? "was refused (NAK)" : "was not confirmed (no ACK)", sent,
						  ( ack.type == DHCPNAK && ack.msg[0] != 0 ) ? " (the server says \"" : "",
						  ( ack.type == DHCPNAK ) ? ack.msg : "",
						  ( ack.type == DHCPNAK && ack.msg[0] != 0 ) ? "\")" : "");
				}
				round0 = 0;		/* the round begun again, with a new transaction */
				continue;
			}
			if ( ack.server == 0 ) {
				ack.server = offer.server;
			}
			tm_printf((UB *)"net: DHCP: acknowledged %d ms after the first DISCOVER, the REQUEST sent %d time%s\n",
				  (INT)( now_ms() - round0 ), sent, ( sent == 1 ) ? "" : "s");
		}
		take(&ack, gen);
		held = ack.yiaddr;
		got = now_s();
		round0 = 0;
		renew0 = 0;
		if ( dhcp_gen == gen ) {
			net_keep(&key, &ack, got);
		}
		/* how long the address took from the start of the asking (the join), for the records */
		if ( first && dhcp_gen == gen ) {
			first = FALSE;
			(void)say_ip(line, 0, "address", ack.yiaddr);
			tm_printf((UB *)"net: DHCP: the%s on %s in %d ms from the start of the asking (%s)\n", line, net,
				  (INT)( now_ms() - t_begin ), ( sent == 0 ) ? "INIT-REBOOT" : "DISCOVER");
		}

		/*
		 * Another machine's address: given back with a DECLINE from no
		 * address, and asked for again from a DISCOVER after ten seconds
		 * (RFC 2131 3.1 and 4.4.1), as NetworkManager does on a conflict
		 */
		if ( dhcp_gen == gen && addr_taken(ack.yiaddr, gen) ) {
			(void)knl_so_nb_ifaddr(0, 0, 0, TRUE);
			held = 0;
			net_drop(&key);
			(void)ts_get_random(&xid, sizeof(xid));
			len = make_msg(m, xid, DHCPDECLINE, 0, ack.yiaddr, ack.server, 0);
			(void)exchange(s, m, len, 0xffffffffU, xid, 0, 0, 0, &offer, gen);
			(void)say_ip(line, 0, "address", ack.yiaddr);
			tm_printf((UB *)"net: DHCP: the%s is another machine's: DECLINE sent; asked again in %d s\n",
				  line, DHCP_DECLINE_WAIT_MS / 1000);
			(void)nap(DHCP_DECLINE_WAIT_MS, gen);
			continue;
		}

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
				net_drop(&key);
				break;
			}
			(void)ts_get_random(&xid, sizeof(xid));
			if ( renew0 == 0 ) {
				renew0 = now_ms();
			}
			len = make_msg(m, xid, DHCPREQUEST, ack.yiaddr, 0, 0, secs_since(renew0));
			answered = exchange(s, m, len, ( now < t2 ) ? ack.server : 0xffffffffU, xid,
					    DHCP_FIRST_MS, DHCPACK, DHCPNAK, &r, gen);
			if ( answered && r.type == DHCPNAK ) {
				tm_printf((UB *)"net: DHCP lease refused\n");
				net_drop(&key);
				break;
			}
			if ( answered ) {
				if ( r.server == 0 ) r.server = ack.server;
				r.yiaddr = ack.yiaddr;
				ack = r;
				take(&ack, gen);
				got = now_s();
				renew0 = 0;
				if ( dhcp_gen == gen ) {
					net_keep(&key, &ack, got);
				}
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

	for ( ;; ) {
		while ( !dhcp_want ) {
			(void)tk_slp_tsk(TMO_FEVR);
		}
		gen = dhcp_gen;
		(void)net_get_mac(dhcp_mac);	/* the card's, as it is now: a PC may have moved to another */
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

/*
 * The leases kept for the networks forgotten, as on a machine never on
 * them: the next round of asking begins from a DISCOVER. The task alone
 * touches them, so they go as that round begins.
 */
EXPORT void knl_so_dhcp_forget( void )
{
	dhcp_forget = TRUE;
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
