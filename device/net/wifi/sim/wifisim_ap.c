/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifisim_ap.c (the Wi-Fi simulator, WIFI_SIM=1)
 *	The simulated access point and the network behind it (design
 *	9.6.12).
 *
 *	The access point's radio is net80211's HOSTAP mode in the rump
 *	kernel (rumpcomp/wifisim/tswifi_sim.c, wsimap0). This task is what
 *	hostapd and a home router would be behind it:
 *
 *	- the network: an SSID, open or WPA2-Personal (CCMP) with a
 *	  passphrase, set up in net80211 as hostapd does
 *	  (rump_tswifisim_ap_config);
 *	- the 4-way and group key handshakes of each station that
 *	  associates, with hostapd's authenticator (wsap_glue.c, wpaap.o):
 *	  its EAPOL frames carried, its keys set into net80211, the
 *	  station's port opened when the handshake is done, the station
 *	  sent away when it fails. Over OpenBSD's layer
 *	  (WIFISIM_LAYER_AUTH=1, obsd_sim.c) the layer's HOSTAP does them
 *	  itself with the PSK it is given, and the stations' ports are only
 *	  watched;
 *	- the network's services, answered here at the access point's
 *	  address 192.168.77.1/24: ARP for it; DHCP (an address from
 *	  192.168.77.100 for each station, the router, the name server and
 *	  the domain wifisim.test, the offer and the acknowledgement sent
 *	  to the client's address or to all as the client asks, or always
 *	  to all); DNS (any name under wifisim.test, and wifisim.test
 *	  itself, is 192.168.77.1; other names do not exist); the echo of
 *	  ICMP (ping) and of UDP's port 7.
 *
 *	What it saw is counted (knl_wifisim_counts), and it is changed
 *	from the tests through knl_wifisim_ctl: the access point off and
 *	on the air, the signal, the security, the passphrase, the SSID,
 *	DHCP off, on, to all, the lease, a REQUEST that comes too soon after
 *	the OFFER left unanswered. The DHCP messages are said on the
 *	console with "wifisim:".
 */

#include <sys/machine.h>

#if WIFI_SIM

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/time.h>
#include "tstdlib.h"
#include "wsap_glue.h"
#include "wifisim.h"

/* The rump kernel's side (tswifi_sim.c) */
typedef struct {
	UB	mac[6];
	UB	authorized;
	UB	ie_len;
	UW	gen;
	UB	ie[64];
} WSIM_STA;

IMPORT int	rump_tswifisim_ap_config( CONST UB *ssid, int ssid_len, int wpa, CONST UB *psk );
IMPORT int	rump_tswifisim_ap_send( CONST void *frame, int len );
IMPORT int	rump_tswifisim_ap_setkey( int cipher, CONST UB *addr, int idx, CONST UB *key, int key_len );
IMPORT int	rump_tswifisim_ap_port( CONST UB *addr, int on );
IMPORT int	rump_tswifisim_ap_kick( CONST UB *addr, int reason );
IMPORT int	rump_tswifisim_ap_mac( UB *mac );
IMPORT int	rump_tswifisim_ap_stations( WSIM_STA *list, int max );
IMPORT int	rump_tswifisim_stats( UW *v, int n );
/* OpenBSD's layer's simulator only (obsd_sim.c); weak: NetBSD's has none */
IMPORT int	rump_tswifisim_seqshare( int on ) __attribute__((weak));
IMPORT void	rump_tswifisim_air( int on, int rssi );

#define AP_PRI		25
#define AP_STKSZ	(32 * 1024)
#define AP_TICK_MS	20
#define RX_SLOTS	64
#define FRAME_MAX	1514
#define STAS		4
#define LEASES		4

#define ETH_IP		0x0800
#define ETH_ARP		0x0806
#define ETH_PAE		0x888e

#define AP_IP		0xc0a84d01UL		/* 192.168.77.1 */
#define POOL_IP		0xc0a84d64UL		/* 192.168.77.100 */
#define NET_MASK	0xffffff00UL
#define DOMAIN		"wifisim.test"

#ifndef WIFISIM_LAYER_AUTH
#define WIFISIM_LAYER_AUTH	0
#endif
#if WIFISIM_LAYER_AUTH
/* the PMK of the passphrase and the SSID (the station's glue, wpa_glue.c or the process's) */
IMPORT int	knl_wpa_psk( const char *pass, int pass_len, const unsigned char *ssid, int ssid_len,
			     unsigned char *pmk );
#define AP_AUTH		FALSE		/* the handshakes are the layer's */
/* hostapd's authenticator is not linked: its calls are nothing */
#define knl_wsap_join(sta, ie, n)	0
#define knl_wsap_leave(sta)		((void)0)
#define knl_wsap_rx(sta, b, n)		((void)0)
#define knl_wsap_tick()			((void)0)
#define knl_wsap_kick(sta, r)		((void)0)
#else
#define AP_AUTH		ap_wpa		/* the handshakes are hostapd's authenticator's here */
#endif

typedef struct {
	UH	len;
	UB	f[FRAME_MAX];
} SLOT;

LOCAL SLOT		*ring;
LOCAL UINT		r_head, r_tail;
LOCAL T_SPLOCK		r_lock;
LOCAL ID		r_sem;
LOCAL ID		ap_mtx;
LOCAL UB		ap_mac[6];
LOCAL WIFISIM_COUNTS	cnt;

/* The network as it is set */
LOCAL char		ap_ssid[33] = WIFISIM_SSID;
LOCAL char		ap_pass[64] = WIFISIM_PASS;
LOCAL BOOL		ap_wpa = TRUE;
LOCAL BOOL		dhcp_on = TRUE;
LOCAL BOOL		dhcp_bcast = FALSE;	/* the answers always to all */
LOCAL BOOL		dhcp_ackuni = FALSE;	/* the ACK to the client alone, whatever it asks */
/*
 * A server that does not answer a REQUEST coming less than this after
 * its OFFER (still checking the address it offered, or a relay that
 * lets one DHCP message of a client through in that time); 0 for none
 */
LOCAL INT		dhcp_slowreq_ms = 0;
LOCAL UD		dhcp_offer_ms = 0;	/* when the last OFFER was sent */
LOCAL UW		dhcp_lease = 3600;	/* s */
LOCAL BOOL		ap_ready = FALSE;

/* The stations, as the handshakes know them */
LOCAL struct {
	BOOL	used;
	UB	mac[6];
	UW	gen;
	BOOL	open;
} stas[STAS];

LOCAL struct {
	BOOL	used;
	BOOL	bad;		/* declined by a client: another machine has it */
	UB	mac[6];
} leases[LEASES];

/*
 * An address of the pool another machine on the link has: ARP's
 * questions about it are answered from that machine's address
 * (taken_mac); 0 for none
 */
LOCAL UW		arp_taken_ip = 0;
LOCAL CONST UB		taken_mac[6] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x99 };

#define LOCK()		tk_loc_mtx(ap_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(ap_mtx)

LOCAL UD now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (UD)( ns / 1000000 );
}

LOCAL BOOL same( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && a[i] == b[i]; i++ ) ;
	return (BOOL)( i == n );
}

LOCAL UH get16( CONST UB *p ) { return (UH)( ( p[0] << 8 ) | p[1] ); }
LOCAL UW get32( CONST UB *p ) { return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3]; }
LOCAL void put16( UB *p, UH v ) { p[0] = (UB)( v >> 8 ); p[1] = (UB)v; }
LOCAL void put32( UB *p, UW v ) { p[0] = (UB)( v >> 24 ); p[1] = (UB)( v >> 16 ); p[2] = (UB)( v >> 8 ); p[3] = (UB)v; }

/* The Internet checksum of n bytes, folded */
LOCAL UH csum( CONST UB *p, INT n, UW sum )
{
	INT	i;

	for ( i = 0; i + 1 < n; i += 2 ) sum += ( (UW)p[i] << 8 ) | p[i + 1];
	if ( n & 1 ) sum += (UW)p[n - 1] << 8;
	while ( sum >> 16 ) sum = ( sum & 0xffff ) + ( sum >> 16 );
	return (UH)~sum;
}

LOCAL void ip_str( UW a, char *s )
{
	tm_sprintf((UB *)s, (CONST UB *)"%d.%d.%d.%d", (INT)( a >> 24 ), (INT)( ( a >> 16 ) & 255 ),
		   (INT)( ( a >> 8 ) & 255 ), (INT)( a & 255 ));
}

/* ---------------------------------------------------------------- sending */

LOCAL void send_frame( UB *f, INT len )
{
	if ( len < 60 ) {
		knl_memset(f + len, 0, 60 - len);
		len = 60;
	}
	if ( rump_tswifisim_ap_send(f, len) == 0 ) cnt.frames_out++;
	else cnt.send_err++;
}

LOCAL void eth_head( UB *f, CONST UB *dst, UH type )
{
	knl_memcpy(f, dst, 6);
	knl_memcpy(f + 6, ap_mac, 6);
	put16(f + 12, type);
}

/* An IPv4 datagram of the protocol proto from the access point, its payload already at f + 34 */
LOCAL void ip_send( UB *f, CONST UB *dst_mac, UW dst_ip, UB proto, INT plen )
{
	UB	*ip = f + 14;
	LOCAL UH id = 1;

	eth_head(f, dst_mac, ETH_IP);
	ip[0] = 0x45;
	ip[1] = 0;
	put16(ip + 2, (UH)( 20 + plen ));
	put16(ip + 4, id++);
	put16(ip + 6, 0);
	ip[8] = 64;
	ip[9] = proto;
	put16(ip + 10, 0);
	put32(ip + 12, AP_IP);
	put32(ip + 16, dst_ip);
	put16(ip + 10, csum(ip, 20, 0));
	send_frame(f, 14 + 20 + plen);
}

/* A UDP datagram from the access point's port sport, its payload already at f + 42 */
LOCAL void udp_send( UB *f, CONST UB *dst_mac, UW dst_ip, UH sport, UH dport, INT len )
{
	UB	*u = f + 34;

	put16(u, sport);
	put16(u + 2, dport);
	put16(u + 4, (UH)( 8 + len ));
	put16(u + 6, 0);		/* no checksum (IPv4) */
	ip_send(f, dst_mac, dst_ip, 17, 8 + len);
}

#if !WIFISIM_LAYER_AUTH
/* ---------------------------------------------------------------- the handshakes' callbacks */

EXPORT int knl_wsap_eapol_tx( CONST unsigned char *dst, CONST unsigned char *body, int len )
{
	UB	f[FRAME_MAX];

	if ( len <= 0 || len > FRAME_MAX - 14 ) return -1;
	eth_head(f, dst, ETH_PAE);
	knl_memcpy(f + 14, body, len);
	cnt.eapol_out++;
	send_frame(f, 14 + len);
	return 0;
}

EXPORT int knl_wsap_setkey( int cipher, CONST unsigned char *addr, int idx,
			    CONST unsigned char *key, int key_len )
{
	INT	er = rump_tswifisim_ap_setkey(cipher, addr, idx, key, key_len);

	tm_printf((UB *)"wifisim: AP: %s key set (cipher %d, index %d, %d bytes): %d\n",
		  ( addr[0] & 1 ) ? "group" : "pairwise", cipher, idx, key_len, er);
	if ( !( addr[0] & 1 ) ) cnt.ptk_set++;
	return ( er == 0 ) ? 0 : -1;
}

EXPORT void knl_wsap_port( CONST unsigned char *sta, int authorized )
{
	INT	er = rump_tswifisim_ap_port(sta, authorized);
	INT	i;

	for ( i = 0; i < STAS; i++ ) {
		if ( stas[i].used && same(stas[i].mac, sta, 6) ) stas[i].open = ( authorized != 0 );
	}
	if ( authorized ) cnt.handshakes++;
	tm_printf((UB *)"wifisim: AP: the port of %02x:%02x:%02x:%02x:%02x:%02x %s (%d)\n",
		  sta[0], sta[1], sta[2], sta[3], sta[4], sta[5], authorized ? "opened" : "closed", er);
}

EXPORT void knl_wsap_kick( CONST unsigned char *sta, int reason )
{
	cnt.kicks++;
	(void)rump_tswifisim_ap_kick(sta, reason);
}

EXPORT void knl_wsap_say( CONST char *line )
{
	tm_printf((UB *)"wifisim: %s", line);		/* one write: the manager's lines go between */
}

EXPORT unsigned long long knl_wsap_ms( void )
{
	return (unsigned long long)now_ms();
}

EXPORT void knl_wsap_random( void *buf, int len )
{
	(void)ts_get_random(buf, (SZ)len);
}

EXPORT void knl_wsap_halt( void )
{
	(void)tk_slp_tsk(TMO_FEVR);
}
#endif /* !WIFISIM_LAYER_AUTH */

/* ---------------------------------------------------------------- the network */

/* The network set up again as it is set: the stations forgotten. ap_mtx held */
LOCAL void ap_apply( void )
{
	INT	er, i, n = (INT)knl_strlen(ap_ssid);
	UB	psk[32];

	for ( i = 0; i < STAS; i++ ) stas[i].used = FALSE;
#if WIFISIM_LAYER_AUTH
	er = 0;
	if ( ap_wpa && knl_wpa_psk(ap_pass, (INT)knl_strlen(ap_pass), (CONST UB *)ap_ssid, n, psk) != 0 ) er = -1;
	if ( er == 0 ) er = rump_tswifisim_ap_config((CONST UB *)ap_ssid, n, ap_wpa ? 2 : 0, psk);
#else
	knl_wsap_stop();
	er = rump_tswifisim_ap_config((CONST UB *)ap_ssid, n, ap_wpa ? 2 : 0, NULL);
	if ( er == 0 && ap_wpa ) {
		er = knl_wsap_start(ap_mac, (CONST UB *)ap_ssid, n, ap_pass, (INT)knl_strlen(ap_pass),
				    WSAP_CIPHER_CCMP, 0);
	}
#endif
	knl_memset(psk, 0, sizeof(psk));
	ap_ready = ( er == 0 );
	tm_printf((UB *)"wifisim: AP \"%s\", %s, %02x:%02x:%02x:%02x:%02x:%02x: %s (%d)\n", ap_ssid,
		  ap_wpa ? "WPA2-Personal" : "open", ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4],
		  ap_mac[5], ap_ready ? "ready" : "not ready", er);
}

/* The stations associated now: new joins' handshakes begun, those gone let go. ap_mtx held */
LOCAL void look_stations( void )
{
	WSIM_STA	list[STAS];
	INT		n, i, k, r;
	BOOL		here;

	n = rump_tswifisim_ap_stations(list, STAS);
	if ( n < 0 ) return;
	if ( n > STAS ) n = STAS;
	for ( k = 0; k < STAS; k++ ) {
		if ( !stas[k].used ) continue;
		for ( here = FALSE, i = 0; i < n; i++ ) {
			if ( same(list[i].mac, stas[k].mac, 6) && list[i].gen == stas[k].gen ) here = TRUE;
		}
		if ( !here ) {
			tm_printf((UB *)"wifisim: AP: %02x:%02x:%02x:%02x:%02x:%02x left\n", stas[k].mac[0],
				  stas[k].mac[1], stas[k].mac[2], stas[k].mac[3], stas[k].mac[4], stas[k].mac[5]);
			if ( AP_AUTH ) knl_wsap_leave(stas[k].mac);
			stas[k].used = FALSE;
			cnt.leaves++;
		}
	}
	for ( i = 0; i < n; i++ ) {
		for ( k = 0; k < STAS && !( stas[k].used && same(stas[k].mac, list[i].mac, 6) ); k++ ) ;
		if ( k < STAS ) {
			if ( !AP_AUTH && ap_wpa && list[i].authorized && !stas[k].open ) {
				/* the layer's handshake done: its port open */
				stas[k].open = TRUE;
				cnt.handshakes++;
				tm_printf((UB *)"wifisim: AP: the port of %02x:%02x:%02x:%02x:%02x:%02x opened\n",
					  list[i].mac[0], list[i].mac[1], list[i].mac[2], list[i].mac[3],
					  list[i].mac[4], list[i].mac[5]);
			}
			continue;
		}
		for ( k = 0; k < STAS && stas[k].used; k++ ) ;
		if ( k == STAS ) break;
		stas[k].used = TRUE;
		knl_memcpy(stas[k].mac, list[i].mac, 6);
		stas[k].gen = list[i].gen;
		stas[k].open = !ap_wpa;
		cnt.joins++;
		tm_printf((UB *)"wifisim: AP: %02x:%02x:%02x:%02x:%02x:%02x associated (join %d, RSN element %d bytes)\n",
			  list[i].mac[0], list[i].mac[1], list[i].mac[2], list[i].mac[3], list[i].mac[4],
			  list[i].mac[5], (INT)list[i].gen, list[i].ie_len);
		if ( AP_AUTH ) {
			r = knl_wsap_join(list[i].mac, list[i].ie, list[i].ie_len);
			if ( r != 0 ) {
				tm_printf((UB *)"wifisim: AP: the station is refused (reason %d)\n", r);
				knl_wsap_kick(list[i].mac, r);
			}
		}
	}
}

/* ---------------------------------------------------------------- the services */

LOCAL void arp_in( CONST UB *f, INT len )
{
	UB	o[64];
	CONST UB *a = f + 14;

	if ( len < 14 + 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || get16(a + 6) != 1 ) return;
	cnt.arp++;
	if ( cnt.arp <= 4 ) {
		char	s1[16], s2[16];

		ip_str(get32(a + 14), s1);
		ip_str(get32(a + 24), s2);
		tm_printf((UB *)"wifisim: ARP: who has %s, tell %s\n", s2, s1);
	}
	if ( get32(a + 24) != AP_IP && ( arp_taken_ip == 0 || get32(a + 24) != arp_taken_ip ) ) return;
	eth_head(o, a + 8, ETH_ARP);
	put16(o + 14, 1);
	put16(o + 16, ETH_IP);
	o[18] = 6;
	o[19] = 4;
	put16(o + 20, 2);
	if ( get32(a + 24) == AP_IP ) {
		knl_memcpy(o + 22, ap_mac, 6);
		put32(o + 28, AP_IP);
	} else {
		/* the other machine that has the address answers */
		knl_memcpy(o + 6, taken_mac, 6);
		knl_memcpy(o + 22, taken_mac, 6);
		put32(o + 28, arp_taken_ip);
		tm_printf((UB *)"wifisim: ARP: the address asked about is another machine's: answered\n");
	}
	knl_memcpy(o + 32, a + 8, 10);		/* the asker's address and IP */
	send_frame(o, 42);
}

LOCAL void icmp_in( CONST UB *f, CONST UB *ip, INT hl, INT tot )
{
	UB	o[FRAME_MAX];
	CONST UB *ic = ip + hl;
	INT	n = tot - hl;

	if ( n < 8 || ic[0] != 8 || get32(ip + 16) != AP_IP ) return;
	cnt.ping++;
	knl_memcpy(o + 34, ic, n);
	o[34] = 0;			/* echo reply */
	put16(o + 36, 0);
	put16(o + 36, csum(o + 34, n, 0));
	ip_send(o, f + 6, get32(ip + 12), 1, n);
}

LOCAL void echo_in( CONST UB *f, CONST UB *ip, CONST UB *u, INT ulen )
{
	UB	o[FRAME_MAX];
	INT	n = ulen - 8;

	if ( n < 0 || get32(ip + 16) != AP_IP ) return;
	cnt.echo++;
	cnt.echo_bytes += (UW)n;
	knl_memcpy(o + 42, u + 8, n);
	udp_send(o, f + 6, get32(ip + 12), 7, get16(u), n);
}

/* The DNS name at p (within the message m of mlen bytes) as text: its end, or NULL */
LOCAL CONST UB *dns_name( CONST UB *m, INT mlen, CONST UB *p, char *out, INT max )
{
	INT	k = 0, l;

	while ( p < m + mlen && *p != 0 ) {
		l = *p++;
		if ( l > 63 || p + l > m + mlen || k + l + 1 >= max ) return NULL;
		if ( k > 0 ) out[k++] = '.';
		knl_memcpy(out + k, p, l);
		k += l;
		p += l;
	}
	if ( p >= m + mlen ) return NULL;
	out[k] = '\0';
	return p + 1;
}

LOCAL BOOL ends_with( CONST char *s, CONST char *t )
{
	INT	a = (INT)knl_strlen(s), b = (INT)knl_strlen(t), i;

	if ( a < b ) return FALSE;
	for ( i = 0; i < b; i++ ) {
		char	c = s[a - b + i];

		if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
		if ( c != t[i] ) return FALSE;
	}
	return (BOOL)( a == b || s[a - b - 1] == '.' );
}

LOCAL void dns_in( CONST UB *f, CONST UB *ip, CONST UB *u, INT ulen )
{
	UB	o[FRAME_MAX];
	CONST UB *m = u + 8, *q;
	INT	mlen = ulen - 8, qlen, n;
	char	name[256];
	UH	qtype;
	BOOL	ours;

	if ( mlen < 12 || ( m[2] & 0x80 ) || get16(m + 4) != 1 ) return;
	q = dns_name(m, mlen, m + 12, name, sizeof(name));
	if ( q == NULL || q + 4 > m + mlen ) return;
	qtype = get16(q);
	qlen = (INT)( q + 4 - m );
	ours = ends_with(name, DOMAIN);
	cnt.dns++;
	tm_printf((UB *)"wifisim: DNS: %s (type %d): %s\n", name, qtype,
		  !ours ? "no such name" : ( qtype == 1 ) ? "192.168.77.1" : "no such record");
	knl_memcpy(o + 42, m, qlen);
	o[42 + 2] = (UB)( 0x84 | ( m[2] & 0x01 ) );	/* a response, authoritative, RD as asked */
	o[42 + 3] = (UB)( ours ? 0x00 : 0x03 );		/* NOERROR, or NXDOMAIN */
	put16(o + 42 + 6, ( ours && qtype == 1 ) ? 1 : 0);
	put16(o + 42 + 8, 0);
	put16(o + 42 + 10, 0);
	n = qlen;
	if ( ours && qtype == 1 ) {
		UB	*a = o + 42 + n;

		put16(a, 0xc00c);		/* the name of the question */
		put16(a + 2, 1);
		put16(a + 4, 1);
		put32(a + 6, 60);
		put16(a + 10, 4);
		put32(a + 12, AP_IP);
		n += 16;
	}
	udp_send(o, f + 6, get32(ip + 12), 53, get16(u), n);
}

/* DHCP's option of the code at the options opt of n bytes: its value and length, or NULL */
LOCAL CONST UB *dhcp_opt( CONST UB *opt, INT n, UB code, INT *p_len )
{
	INT	i = 0;

	while ( i < n && opt[i] != 255 ) {
		if ( opt[i] == 0 ) {
			i++;
			continue;
		}
		if ( i + 1 >= n || i + 2 + opt[i + 1] > n ) return NULL;
		if ( opt[i] == code ) {
			*p_len = opt[i + 1];
			return opt + i + 2;
		}
		i += 2 + opt[i + 1];
	}
	return NULL;
}

LOCAL INT lease_of( CONST UB *mac )
{
	INT	i;

	for ( i = 0; i < LEASES; i++ ) {
		if ( leases[i].used && same(leases[i].mac, mac, 6) ) return i;
	}
	for ( i = 0; i < LEASES; i++ ) {
		if ( !leases[i].used && !leases[i].bad ) {
			leases[i].used = TRUE;
			knl_memcpy(leases[i].mac, mac, 6);
			return i;
		}
	}
	return -1;
}

LOCAL CONST char *CONST dhcp_name[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK",
					"RELEASE", "INFORM" };

LOCAL void dhcp_in( CONST UB *f, CONST UB *ip, CONST UB *u, INT ulen )
{
	UB	o[FRAME_MAX];
	CONST UB *b = u + 8, *v;
	INT	blen = ulen - 8, k, l, n;
	UB	type, reply;
	UW	yi, req = 0, ci;
	BOOL	bcast;
	char	s1[16], s2[16];
	static CONST UB all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if ( blen < 240 || b[0] != 1 || b[1] != 1 || b[2] != 6 || get32(b + 236) != 0x63825363 ) return;
	v = dhcp_opt(b + 240, blen - 240, 53, &l);
	if ( v == NULL || l != 1 ) return;
	type = v[0];
	ci = get32(b + 12);
	v = dhcp_opt(b + 240, blen - 240, 50, &l);
	if ( v != NULL && l == 4 ) req = get32(v);
	knl_memcpy(cnt.dhcp_chaddr, b + 28, 6);
	knl_memcpy(cnt.dhcp_src, f + 6, 6);
	if ( type == 1 ) cnt.dhcp_discover++;
	else if ( type == 3 ) cnt.dhcp_request++;
	ip_str(req != 0 ? req : ci, s1);
	tm_printf((UB *)"wifisim: DHCP: %s from %02x:%02x:%02x:%02x:%02x:%02x (asks %s, flags %04x)%s\n",
		  ( type <= 8 ) ? dhcp_name[type] : "?", b[28], b[29], b[30], b[31], b[32], b[33], s1,
		  get16(b + 10), dhcp_on ? "" : ": not answered (DHCP off)");
	if ( !dhcp_on ) return;
	k = lease_of(b + 28);
	if ( k < 0 ) return;
	yi = POOL_IP + (UW)k;
	if ( type == 1 ) {
		reply = 2;
	} else if ( type == 3 ) {
		if ( dhcp_slowreq_ms > 0 && ci == 0 && now_ms() - dhcp_offer_ms < (UD)dhcp_slowreq_ms ) {
			tm_printf((UB *)"wifisim: DHCP: the REQUEST not answered: %d ms after the OFFER\n",
				  (INT)( now_ms() - dhcp_offer_ms ));
			return;
		}
		reply = ( ( req != 0 && req == yi ) || ( req == 0 && ci == yi ) ) ? 5 : 6;
	} else if ( type == 7 ) {
		leases[k].used = FALSE;
		return;
	} else if ( type == 4 ) {
		/* the address declined: another machine has it, and it is not offered again */
		leases[k].used = FALSE;
		leases[k].bad = TRUE;
		return;
	} else {
		return;
	}

	knl_memset(o + 42, 0, 300);
	b = u + 8;
	o[42] = 2;
	o[43] = 1;
	o[44] = 6;
	knl_memcpy(o + 42 + 4, b + 4, 4);		/* xid */
	knl_memcpy(o + 42 + 10, b + 10, 2);		/* flags */
	knl_memcpy(o + 42 + 12, b + 12, 4);		/* ciaddr */
	if ( reply != 6 ) put32(o + 42 + 16, yi);
	put32(o + 42 + 20, AP_IP);
	knl_memcpy(o + 42 + 28, b + 28, 16);		/* chaddr */
	put32(o + 42 + 236, 0x63825363);
	n = 240;
	o[42 + n++] = 53; o[42 + n++] = 1; o[42 + n++] = reply;
	o[42 + n++] = 54; o[42 + n++] = 4; put32(o + 42 + n, AP_IP); n += 4;
	if ( reply != 6 ) {
		o[42 + n++] = 51; o[42 + n++] = 4; put32(o + 42 + n, dhcp_lease); n += 4;
		o[42 + n++] = 58; o[42 + n++] = 4; put32(o + 42 + n, dhcp_lease / 2); n += 4;
		o[42 + n++] = 59; o[42 + n++] = 4; put32(o + 42 + n, dhcp_lease / 8 * 7); n += 4;
		o[42 + n++] = 1; o[42 + n++] = 4; put32(o + 42 + n, NET_MASK); n += 4;
		o[42 + n++] = 3; o[42 + n++] = 4; put32(o + 42 + n, AP_IP); n += 4;
		o[42 + n++] = 6; o[42 + n++] = 4; put32(o + 42 + n, AP_IP); n += 4;
		o[42 + n++] = 15; o[42 + n++] = (UB)( sizeof(DOMAIN) - 1 );
		knl_memcpy(o + 42 + n, DOMAIN, sizeof(DOMAIN) - 1);
		n += sizeof(DOMAIN) - 1;
	}
	o[42 + n++] = 255;
	while ( n < 300 ) o[42 + n++] = 0;
	/* to all when the client asks (the broadcast flag) or has no address yet and the access point
	   always answers to all; else to the client's address */
	bcast = dhcp_bcast || ( get16(b + 10) & 0x8000 ) || reply == 6;
	/*
	 * Servers that send the ACK to the client's hardware address and the
	 * address offered though the client asked for a broadcast (RFC 2131
	 * 4.1 says a server SHOULD broadcast then): the frame is unicast,
	 * protected with the pairwise key
	 */
	if ( dhcp_ackuni && reply == 5 ) bcast = FALSE;
	ip_str(yi, s2);
	if ( reply == 2 ) {
		cnt.dhcp_offer++;
		dhcp_offer_ms = now_ms();
	}
	else if ( reply == 5 ) cnt.dhcp_ack++;
	else cnt.dhcp_nak++;
	tm_printf((UB *)"wifisim: DHCP: %s %s to %s\n", dhcp_name[reply], s2,
		  ( ci != 0 && !bcast ) ? "the client's address" : bcast ? "all (broadcast)" : "the client (unicast)");
	if ( ci != 0 && !bcast ) {
		udp_send(o, f + 6, ci, 67, 68, n);
	} else if ( bcast ) {
		udp_send(o, all, 0xffffffffUL, 67, 68, n);
	} else {
		udp_send(o, b + 28, yi, 67, 68, n);
	}
}

LOCAL void ip_in( CONST UB *f, INT len )
{
	CONST UB *ip = f + 14, *u;
	INT	hl, tot, ulen;
	UH	dport;

	if ( len < 14 + 20 || ( ip[0] >> 4 ) != 4 ) return;
	hl = ( ip[0] & 15 ) * 4;
	tot = get16(ip + 2);
	if ( hl < 20 || tot < hl || 14 + tot > len ) return;
	cnt.ip++;
	if ( ip[9] == 1 ) {
		icmp_in(f, ip, hl, tot);
		return;
	}
	if ( ip[9] != 17 || tot < hl + 8 ) return;
	u = ip + hl;
	ulen = get16(u + 4);
	if ( ulen < 8 || hl + ulen > tot ) return;
	dport = get16(u + 2);
	if ( dport == 67 ) dhcp_in(f, ip, u, ulen);
	else if ( dport == 53 ) dns_in(f, ip, u, ulen);
	else if ( dport == 7 ) echo_in(f, ip, u, ulen);
}

LOCAL void frame_in( CONST UB *f, INT len )
{
	UH	type = get16(f + 12);

	cnt.frames_in++;
	if ( type == ETH_PAE ) {
		cnt.eapol_in++;
		if ( AP_AUTH ) knl_wsap_rx(f + 6, f + 14, len - 14);
	} else if ( type == ETH_ARP ) {
		arp_in(f, len);
	} else if ( type == ETH_IP ) {
		ip_in(f, len);
	} else {
		cnt.other++;
	}
}

/* ---------------------------------------------------------------- the task */

/* A frame the access point received (wifi_user_sim.c, from the soft interrupt): queued, no wait */
EXPORT void knl_wifisim_ap_rx( CONST UB *frame, INT len )
{
	UINT	imask;
	BOOL	put = FALSE;

	if ( ring == NULL || len < 14 || len > FRAME_MAX ) return;
	ISpinLock(&r_lock, &imask);
	if ( r_tail - r_head < RX_SLOTS ) {
		ring[r_tail % RX_SLOTS].len = (UH)len;
		knl_memcpy(ring[r_tail % RX_SLOTS].f, frame, len);
		r_tail++;
		put = TRUE;
	} else {
		cnt.rx_full++;
	}
	ISpinUnlock(&r_lock, &imask);
	if ( put ) (void)tk_sig_sem(r_sem, 1);
}

LOCAL void ap_task( INT stacd, void *exinf )
{
	SLOT	*s = (SLOT *)Kmalloc(sizeof(SLOT));
	UINT	imask;
	UD	t_look = 0;
	BOOL	got;

	(void)stacd;
	(void)exinf;
	(void)rump_tswifisim_ap_mac(ap_mac);
	/*
	 * The radios may come after this task (OpenBSD's layer makes them at
	 * the card's start, rump_tswifi_start): the network set up once they
	 * are there
	 */
	for ( t_look = 0; t_look < 120; t_look++ ) {
		LOCK();
		ap_apply();
		UNLOCK();
		if ( ap_ready ) break;
		tk_dly_tsk(250);
	}
	t_look = 0;
	for ( ;; ) {
		(void)tk_wai_sem(r_sem, 1, AP_TICK_MS);
		LOCK();
		for ( ;; ) {
			got = FALSE;
			ISpinLock(&r_lock, &imask);
			if ( r_head != r_tail && s != NULL ) {
				knl_memcpy(s, &ring[r_head % RX_SLOTS], sizeof(SLOT));
				r_head++;
				got = TRUE;
			}
			ISpinUnlock(&r_lock, &imask);
			if ( !got ) break;
			frame_in(s->f, s->len);
		}
		if ( now_ms() - t_look >= AP_TICK_MS ) {
			t_look = now_ms();
			look_stations();
		}
		if ( AP_AUTH ) knl_wsap_tick();
		UNLOCK();
	}
}

/* From the Wi-Fi manager's start (wifi.c), once the rump kernel is up */
EXPORT void knl_wifisim_ap_start( void )
{
	T_CMTX	cm;
	T_CSEM	cs;
	T_CTSK	ct;
	ID	t;

	if ( ring != NULL ) return;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	ap_mtx = tk_cre_mtx(&cm);
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RX_SLOTS;
	r_sem = tk_cre_sem(&cs);
	InitSpinLock(&r_lock);
	ring = (SLOT *)Kmalloc(sizeof(SLOT) * RX_SLOTS);
	if ( ap_mtx <= 0 || r_sem <= 0 || ring == NULL ) {
		tm_putstring((UB *)"wifisim: no room for the access point\n");
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ap_task;
	ct.itskpri = AP_PRI;
	ct.stksz = AP_STKSZ;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}

/* ---------------------------------------------------------------- what the tests ask */

LOCAL BOOL word_is( CONST char *s, CONST char *w, CONST char **p_rest )
{
	INT	n = (INT)knl_strlen(w), i;

	for ( i = 0; i < n && s[i] == w[i]; i++ ) ;
	if ( i < n || ( s[n] != '\0' && s[n] != ' ' ) ) return FALSE;
	s += n;
	while ( *s == ' ' ) s++;
	*p_rest = s;
	return TRUE;
}

LOCAL INT num_of( CONST char *s )
{
	INT	v = 0;

	while ( *s >= '0' && *s <= '9' ) v = v * 10 + ( *s++ - '0' );
	return v;
}

/*
 * One change: "air off", "air on", "rssi <dBm's size>", "sec open",
 * "sec wpa2", "pass <passphrase>", "ssid <SSID>", "dhcp off",
 * "dhcp on", "dhcp bcast" (the answers always to all), "dhcp ucast"
 * (as the client asks), "dhcp ackuni" (the ACK unicast to the client),
 * "seqshare on" / "seqshare off" (the access point keeps one record of
 * the sequence numbers of a station's data frames without the QoS field
 * and of TID 0, and drops a QoS frame of TID 0 not after it; OpenBSD's
 * layer only), "dhcp slowreq <ms>" (a REQUEST sooner than ms after the OFFER not
 * answered; 0 for all answered), "taken <n>" (the pool's address ending
 * in n another machine's, answering ARP for it; 0 for none), "lease <s>",
 * "forget" (the leases, those declined too). A change of
 * the network starts it again, its stations forgotten.
 */
EXPORT ER knl_wifisim_ctl( CONST char *cmd )
{
	CONST char	*a;
	ER		er = E_OK;
	INT		n;

	if ( ring == NULL ) return E_NOEXS;
	tm_printf((UB *)"wifisim: asked: %s\n", cmd);
	if ( word_is(cmd, "air", &a) ) {
		rump_tswifisim_air(( knl_strcmp(a, "on") == 0 ) ? 1 : 0, -1);
		return E_OK;
	}
	if ( word_is(cmd, "seqshare", &a) ) {
		if ( rump_tswifisim_seqshare == NULL ) return E_NOSPT;
		(void)rump_tswifisim_seqshare(( knl_strcmp(a, "on") == 0 ) ? 1 : 0);
		return E_OK;
	}
	if ( word_is(cmd, "rssi", &a) ) {
		rump_tswifisim_air(-1, num_of(a));
		return E_OK;
	}
	LOCK();
	if ( word_is(cmd, "sec", &a) ) {
		ap_wpa = ( knl_strcmp(a, "wpa2") == 0 );
		ap_apply();
	} else if ( word_is(cmd, "pass", &a) && ( n = (INT)knl_strlen(a) ) >= 8 && n <= 63 ) {
		knl_strcpy(ap_pass, a);
		ap_apply();
	} else if ( word_is(cmd, "ssid", &a) && ( n = (INT)knl_strlen(a) ) >= 1 && n <= 32 ) {
		knl_strcpy(ap_ssid, a);
		ap_apply();
	} else if ( word_is(cmd, "dhcp", &a) ) {
		if ( knl_strcmp(a, "off") == 0 ) dhcp_on = FALSE;
		else if ( knl_strcmp(a, "on") == 0 ) dhcp_on = TRUE;
		else if ( knl_strcmp(a, "bcast") == 0 ) dhcp_bcast = TRUE;
		else if ( knl_strcmp(a, "ucast") == 0 ) dhcp_bcast = dhcp_ackuni = FALSE;
		else if ( knl_strcmp(a, "ackuni") == 0 ) dhcp_ackuni = TRUE;
		else if ( word_is(a, "slowreq", &a) ) dhcp_slowreq_ms = num_of(a);
		else er = E_PAR;
	} else if ( word_is(cmd, "taken", &a) ) {
		n = num_of(a);
		arp_taken_ip = ( n > 0 && n < 255 ) ? ( ( POOL_IP & 0xffffff00UL ) | (UW)n ) : 0;
	} else if ( word_is(cmd, "lease", &a) && num_of(a) >= 10 ) {
		dhcp_lease = (UW)num_of(a);
	} else if ( word_is(cmd, "forget", &a) ) {
		knl_memset(leases, 0, sizeof(leases));
	} else {
		er = E_PAR;
	}
	UNLOCK();
	return er;
}

/* What the access point and the air saw */
EXPORT void knl_wifisim_counts( WIFISIM_COUNTS *c )
{
	UW	v[20];
	INT	i;

	knl_memset(v, 0, sizeof(v));
	(void)rump_tswifisim_stats(v, 20);
	if ( ap_mtx > 0 ) LOCK();
	*c = cnt;
	for ( i = 0; i < 20; i++ ) c->air[i] = v[i];
	c->ready = ap_ready;
	for ( i = 0; i < STAS; i++ ) {
		if ( stas[i].used && stas[i].open ) c->open_stas++;
	}
	if ( ap_mtx > 0 ) UNLOCK();
}

#endif /* WIFI_SIM */
