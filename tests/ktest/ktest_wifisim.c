/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wifisim.c
 *	The Wi-Fi from the scan to the data, through the simulator (make
 *	WIFI_SIM=1, design 9.6.12): the Wi-Fi manager (wifi.c), the card's
 *	component (tswifi_if.c), net80211 and the handshakes as they are
 *	on a machine, with the simulated station in the card's place and
 *	the simulated access point (wifisim_ap.c) at the other end.
 *
 *	- scan: a scan asked through wlan0 finds the access point, its
 *	  SSID and WPA2.
 *	- join: the network saved with its passphrase (利用者's, joined by
 *	  itself) is joined; the 4-way handshake ends on both sides.
 *	- dhcp: the stack gets its address from the access point's DHCP,
 *	  with the router and the name server; no data frame was dropped
 *	  for the station's port not being open.
 *	- dns: a name under wifisim.test is found; another is not.
 *	- data: UDP datagrams of 1 to 1400 bytes come back from the access
 *	  point's echo, the same bytes.
 *	- leave: DISCONNECT leaves the network; the access point sees it.
 *	- again: CONNECT joins it again, and DHCP answers again: the
 *	  address still held is asked for at once (INIT-REBOOT), with no
 *	  DISCOVER.
 *	- reqagain: a server that leaves a REQUEST coming soon after its
 *	  OFFER unanswered: the REQUEST is sent again with the same
 *	  transaction a few seconds later and answered, with no DISCOVER
 *	  between (before, each DISCOVER's REQUEST came as soon and the
 *	  address never came).
 *	- declined: the address offered first is another machine's (it
 *	  answers the stack's ARP probes): the client sends a DECLINE and
 *	  asks again, and takes the next address offered.
 *	- off: the machine going off (knl_wifi_shutdown) leaves the
 *	  network: the access point hears the deauthentication before it
 *	  returns, and REASON is off.
 *	- agg: a block-ack agreement for what the station sends is not
 *	  asked for at the join and DHCP, is asked for once the TID carries
 *	  traffic, and the TID's sequence numbers stay continuous through
 *	  it: the access point drops nothing as old, nor (it keeping one
 *	  record for frames without the QoS field and TID 0) as before
 *	  the last of those.
 *	- lost: the access point taken off the air: the station sees the
 *	  network lost; put back, it is joined again by itself, and the
 *	  data goes again.
 *	- password: the access point's passphrase changed: the join ends
 *	  with REASON password; the passphrase put back, it is joined.
 *	- bcast: DHCP's answers sent to all (encrypted with the group key)
 *	  are taken as well.
 *	- ackuni: with no address yet, an ACK sent to the station alone
 *	  (its hardware address and the address offered, encrypted with
 *	  the pairwise key) though the broadcast was asked for, as some
 *	  routers send it, is taken and the address set.
 *	- bgscan: joined with a signal below the threshold, no background
 *	  scan is begun while DHCP asks nor in the first 30 s; one is
 *	  begun after.
 *	- open: an open network of the same name is joined, and DHCP and
 *	  the data go over it.
 *
 *	The machine is made to use the Wi-Fi (NET_ADAPTER wifi) and its
 *	address to come from DHCP while the group runs; the settings are
 *	put back at the end.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/so.h>
#include <ts/net.h>
#include <ts/conf.h>
#include "obj/obj.h"
#include "tstdlib.h"
#include "../../device/net/wifi/sim/wifisim.h"

IMPORT ER   knl_wifi_request( CONST T_OBCRD *crd, CONST char *line );
IMPORT INT  knl_wifi_state_text( CONST T_OBCRD *crd, UB **p_text );
IMPORT BOOL knl_wifi_connected( void );
IMPORT void knl_wifi_shutdown( void );
IMPORT ER   knl_so_nb_ifaddr( UW addr, UW mask, UW gw, BOOL dhcp );

#define T_JOIN_MS	60000
#define T_DHCP_MS	40000
#define T_LEAVE_MS	15000
#define T_DAD_MS	8000
#define T_BG_FIRST_MS	30000	/* no background scan this long after the port opened */

LOCAL T_OBCRD	*me;
LOCAL TS_UUID	wl;			/* wlan0 */
LOCAL char	ssid_hex[72];
LOCAL BOOL	joined = FALSE, addressed = FALSE;
LOCAL UB	*dev_was = NULL;	/* the machine's settings as they were */
LOCAL INT	dev_was_len = -1;

LOCAL void hex_of( CONST char *s, char *out )
{
	static CONST char d[] = "0123456789abcdef";
	INT	i;

	for ( i = 0; s[i] != '\0'; i++ ) {
		out[2 * i] = d[(UB)s[i] >> 4];
		out[2 * i + 1] = d[(UB)s[i] & 15];
	}
	out[2 * i] = '\0';
}

LOCAL BOOL holds( CONST UB *t, INT len, CONST char *s )
{
	INT	n = (INT)knl_strlen(s), i, k;

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && t[i + k] == (UB)s[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* wlan0's record 1 as 利用者 reads it holds s */
LOCAL BOOL rec1_holds( CONST char *s )
{
	UB	*t = NULL;
	INT	len = knl_wifi_state_text(me, &t);
	BOOL	r = FALSE;

	if ( t != NULL ) {
		r = ( len > 0 && holds(t, len, s) );
		Kfree(t);
	}
	return r;
}

LOCAL BOOL wait_rec1( CONST char *s, INT ms )
{
	INT	i;

	for ( i = 0; i < ms / 100; i++ ) {
		if ( rec1_holds(s) ) return TRUE;
		tk_dly_tsk(100);
	}
	return rec1_holds(s);
}

/* A command written to wlan0's record 1, as a program writes it */
LOCAL ER wl_cmd( CONST char *c )
{
	ID	key = ob_opn_obj(&wl, OB_OP_WRITE);
	SZ	asz = 0;
	ER	er;

	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, 1, 0, c, (SZ)knl_strlen(c), &asz);
	ob_cls_obj(key);
	return er;
}

/* wlan0's record 2 (the networks of the last scan) holds s */
LOCAL BOOL rec2_holds( CONST char *s )
{
	ID	key = ob_opn_obj(&wl, OB_OP_READ);
	UB	*t = NULL;
	INT	len;
	BOOL	r;

	if ( key < E_OK ) return FALSE;
	len = knl_ob_rea_all(key, 2, 0, &t, NULL);
	ob_cls_obj(key);
	r = ( t != NULL && len > 0 && holds(t, len, s) );
	if ( t != NULL ) Kfree(t);
	return r;
}

LOCAL BOOL wait_connected( INT ms )
{
	INT	i;

	for ( i = 0; i < ms / 100 && !knl_wifi_connected(); i++ ) tk_dly_tsk(100);
	return knl_wifi_connected();
}

LOCAL BOOL wait_left( INT ms )
{
	INT	i;

	for ( i = 0; i < ms / 100 && knl_wifi_connected(); i++ ) tk_dly_tsk(100);
	return !knl_wifi_connected();
}

LOCAL void counts_say( CONST char *when )
{
	WIFISIM_COUNTS	c;

	knl_wifisim_counts(&c);
	tm_printf((UB *)"  wifisim %s: joins %d, handshakes %d, DHCP discover %d offer %d request %d ack %d nak %d,"
		  " DNS %d, echo %d; air: beacons %d, the station's data dropped for its port not open %d,"
		  " the access point's %d\n", when, (INT)c.joins, (INT)c.handshakes, (INT)c.dhcp_discover,
		  (INT)c.dhcp_offer, (INT)c.dhcp_request, (INT)c.dhcp_ack, (INT)c.dhcp_nak, (INT)c.dns,
		  (INT)c.echo, (INT)c.air[WSIM_AIR_BEACONS], (INT)c.air[WSIM_AIR_STA_UNAUTH],
		  (INT)c.air[WSIM_AIR_AP_UNAUTH]);
}

/* The stack's address from the simulated network, within ms */
LOCAL BOOL wait_addr( INT ms )
{
	UW	a = 0, m = 0, g = 0;
	INT	i;

	for ( i = 0; i < ms / 100; i++ ) {
		/* the stack's addresses are in network byte order */
		if ( so_getifaddr(&a, &m, &g) >= E_OK
		     && ( lwip_ntohl(a) & 0xffffff00UL ) == ( WIFISIM_AP_IP & 0xffffff00UL ) ) {
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  the stack's address: %08x mask %08x gateway %08x\n", a, m, g);
	return FALSE;
}

/* A DHCP acknowledgement more than before, within ms */
LOCAL BOOL wait_ack( UW before, INT ms )
{
	WIFISIM_COUNTS	c;
	INT		i;

	for ( i = 0; i < ms / 100; i++ ) {
		knl_wifisim_counts(&c);
		if ( c.dhcp_ack > before ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

LOCAL UW acks( void )
{
	WIFISIM_COUNTS	c;

	knl_wifisim_counts(&c);
	return c.dhcp_ack;
}

/*
 * n datagrams of the sizes 1 to 1400 through the access point's echo:
 * how many came back whole.
 *
 * The access point's link address is made known first, by a datagram
 * of its own that is not counted. After the station's address is set
 * the stack has no entry for the access point, and a datagram sent
 * while it is asked for by ARP is held; the stack holds one for each
 * neighbour being asked for (NetBSD's net.inet.arp.maxqueuelen of 1,
 * the one RFC 1122 2.3.2.2 asks for), so that a datagram another task
 * sends to it then takes the place of the one held (the clock's DNS
 * query to the access point, asked again every 15 s, did so to the
 * first of a round 1 s after the address was set). The datagrams
 * counted are sent with the neighbour known.
 */
#define ECHO_PRIME_TRIES	3

LOCAL INT echo_round( INT n )
{
	struct sockaddr_in	ap;
	fd_set			rd;
	UB			*out, *in;
	INT			s, i, k, size, got = 0, r;
	BOOL			known = FALSE;

	out = (UB *)Kmalloc(1500);
	in = (UB *)Kmalloc(1500);
	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	if ( out == NULL || in == NULL || s < 0 ) {
		if ( s >= 0 ) so_close(s);
		if ( out != NULL ) Kfree(out);
		if ( in != NULL ) Kfree(in);
		return -1;
	}
	knl_memset(&ap, 0, sizeof(ap));
	ap.sin_family = AF_INET;
	ap.sin_port = lwip_htons(7);
	ap.sin_addr.s_addr = lwip_htonl(WIFISIM_AP_IP);
	for ( i = 0; i < ECHO_PRIME_TRIES && !known; i++ ) {
		out[0] = 0xA5;
		if ( so_sendto(s, out, 1, 0, (struct sockaddr *)&ap, sizeof(ap)) != 1 ) {
			continue;
		}
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		if ( so_select(s + 1, &rd, NULL, NULL, 3000) == 1 ) {
			known = (BOOL)( so_recv(s, in, 1500, 0) == 1 && in[0] == 0xA5 );
		}
		if ( !known ) tm_printf((UB *)"  echo: the access point not reached yet (%d)\n", i + 1);
	}
	if ( !known ) {
		so_close(s);
		Kfree(out);
		Kfree(in);
		return 0;
	}
	/* an answer to a try that came late is not taken for one of the round's */
	for ( ;; ) {
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		if ( so_select(s + 1, &rd, NULL, NULL, TMO_POL) != 1 || so_recv(s, in, 1500, 0) <= 0 ) break;
	}
	for ( i = 0; i < n; i++ ) {
		size = ( i == 0 ) ? 1 : ( i * 1400 ) / ( n - 1 );
		if ( size < 1 ) size = 1;
		for ( k = 0; k < size; k++ ) out[k] = (UB)( i * 7 + k );
		if ( ( r = so_sendto(s, out, size, 0, (struct sockaddr *)&ap, sizeof(ap)) ) != size ) {
			tm_printf((UB *)"  echo: %d bytes not sent (%d)\n", size, r);
			continue;
		}
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		if ( so_select(s + 1, &rd, NULL, NULL, 3000) != 1 ) {
			tm_printf((UB *)"  echo: %d bytes sent, nothing back in 3 s\n", size);
			continue;
		}
		r = so_recv(s, in, 1500, 0);
		if ( r != size ) {
			tm_printf((UB *)"  echo: %d bytes sent, %d back\n", size, r);
			continue;
		}
		for ( k = 0; k < size && in[k] == out[k]; k++ ) ;
		if ( k == size ) got++;
	}
	so_close(s);
	Kfree(out);
	Kfree(in);
	return got;
}

/* The machine's setting NET_ADAPTER (SYSDEF_CONF_DEV), or the settings as they were (val NULL) */
LOCAL ER adapter_set( CONST char *val )
{
	TS_UUID	u;
	ID	key;
	UB	*t = NULL, *w;
	INT	len;
	SZ	asz = 0;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_CONF_DEV, &u) < E_OK ) return E_NOEXS;
	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	if ( val == NULL ) {
		er = E_OK;
		if ( dev_was_len >= 0 ) {
			er = ob_wri_rec(key, 1, 0, dev_was, dev_was_len, &asz);
			if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)dev_was_len);
		}
		ob_cls_obj(key);
		return er;
	}
	len = knl_ob_rea_all(key, 1, 0, &t, NULL);
	if ( len < 0 ) len = 0;
	if ( dev_was_len < 0 ) {
		dev_was = (UB *)Kmalloc(len + 1);
		if ( dev_was != NULL ) {
			if ( len > 0 ) knl_memcpy(dev_was, t, len);
			dev_was_len = len;
		}
	}
	w = (UB *)Kmalloc(len + 256);
	er = E_NOMEM;
	if ( w != NULL ) {
		if ( len > 0 ) knl_memcpy(w, t, len);
		len = cf_put(w, len, len + 256, "NET_ADAPTER", 0, val);
		er = ( len < 0 ) ? E_LIMIT : ob_wri_rec(key, 1, 0, w, len, &asz);
		if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)len);
		Kfree(w);
	}
	if ( t != NULL ) Kfree(t);
	ob_cls_obj(key);
	return er;
}

/* ---------------------------------------------------------------- the cases */

LOCAL void test_scan( void )
{
	char	want[96];
	INT	i;
	BOOL	seen = FALSE;

	/* wpa2, then the SSID in hexadecimal (record 2's lines) */
	tm_sprintf((UB *)want, (CONST UB *)" wpa2 %s\n", ssid_hex);
	for ( i = 0; i < 6 && !seen; i++ ) {
		(void)wl_cmd("SCAN");
		seen = rec2_holds(want);
		if ( !seen ) tk_dly_tsk(2000);
		seen = seen || rec2_holds(want);
	}
	KT_ASSERT(seen);
}

LOCAL void test_join( void )
{
	char	line[160];

	tm_sprintf((UB *)line, (CONST UB *)"SAVE %s wpa2 auto %s", ssid_hex, WIFISIM_PASS);
	KT_ASSERT_ER(knl_wifi_request(me, line), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_rec1("SECURITY wpa2\n", 3000));
	{
		WIFISIM_COUNTS	c;
		INT		i;

		for ( i = 0; i < 50; i++ ) {
			knl_wifisim_counts(&c);
			if ( c.open_stas > 0 ) break;
			tk_dly_tsk(100);
		}
		KT_ASSERT(c.handshakes >= 1);
		KT_ASSERT_EQ(c.open_stas, 1);
	}
	counts_say("after the join");
}

LOCAL void test_dhcp( void )
{
	UW		a = 0, m = 0, g = 0, d = 0;
	WIFISIM_COUNTS	c;

	if ( !joined ) KT_SKIP("not joined");
	addressed = wait_addr(T_DHCP_MS);
	counts_say("DHCP");
	KT_ASSERT(addressed);
	knl_wifisim_counts(&c);
	KT_ASSERT_EQ(c.air[WSIM_AIR_STA_UNAUTH], 0);	/* no data dropped for a port not open */
	if ( !addressed ) return;
	KT_ASSERT_ER(so_getifaddr(&a, &m, &g), E_OK);
	KT_ASSERT_EQ(lwip_ntohl(a), WIFISIM_POOL_IP);
	KT_ASSERT_EQ(lwip_ntohl(m), 0xffffff00UL);
	KT_ASSERT_EQ(lwip_ntohl(g), WIFISIM_AP_IP);
	KT_ASSERT_ER(so_getdns(&d), E_OK);
	KT_ASSERT_EQ(lwip_ntohl(d), WIFISIM_AP_IP);
}

/*
 * The stack moved onto the Wi-Fi from the wired card it started on:
 * DHCP asks with the station's address, in the frame and in chaddr, and
 * the IPv6 link-local address is made of the station's address (EUI-64
 * of 02:77:73:00:00:02), not of the wired card's
 */
LOCAL void test_moved( void )
{
	static CONST UB	sta[6] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x02 };
	static CONST UB	iid[8] = { 0x00, 0x77, 0x73, 0xff, 0xfe, 0x00, 0x00, 0x02 };
	WIFISIM_COUNTS	c;
	T_SOIF6		*l;
	INT		n, i, j, k, ll = 0, ours = 0;

	if ( !addressed ) KT_SKIP("no address");
	knl_wifisim_counts(&c);
	for ( k = 0; k < 6 && c.dhcp_chaddr[k] == sta[k] && c.dhcp_src[k] == sta[k]; k++ ) ;
	KT_ASSERT_EQ(k, 6);
	l = (T_SOIF6 *)Kmalloc(sizeof(T_SOIF6) * 16);
	if ( l == NULL ) return;
	for ( i = 0; i < 50 && ours == 0; i++ ) {
		n = so_getifaddr6(l, 16, NULL);
		for ( ll = 0, ours = 0, k = 0; k < n && k < 16; k++ ) {
			if ( l[k].addr[0] != 0xfe || ( l[k].addr[1] & 0xc0 ) != 0x80 ) continue;
			ll++;
			for ( j = 0; j < 8 && l[k].addr[8 + j] == iid[j]; j++ ) ;
			if ( j == 8 ) ours++;
			tm_printf((UB *)"  link-local %02x%02x:%02x%02x:%02x%02x:%02x%02x (flags %x)\n",
				  l[k].addr[8], l[k].addr[9], l[k].addr[10], l[k].addr[11], l[k].addr[12],
				  l[k].addr[13], l[k].addr[14], l[k].addr[15], l[k].flags);
		}
		if ( ours == 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(ours, 1);
	KT_ASSERT_EQ(ll, 1);		/* the wired card's is gone */
	Kfree(l);
}

LOCAL void test_dns( void )
{
	struct addrinfo	hints, *res = NULL;

	if ( !addressed ) KT_SKIP("no address");
	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	KT_ASSERT_ER(so_getaddrinfo("www." WIFISIM_DOMAIN, "7", &hints, &res), E_OK);
	if ( res != NULL ) {
		KT_ASSERT_EQ(lwip_ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr), WIFISIM_AP_IP);
		so_freeaddrinfo(res);
		res = NULL;
	}
	KT_ASSERT(so_getaddrinfo("nosuch.example", "7", &hints, &res) < E_OK);
	if ( res != NULL ) so_freeaddrinfo(res);
}

LOCAL void test_data( void )
{
	if ( !addressed ) KT_SKIP("no address");
	KT_ASSERT_EQ(echo_round(20), 20);
}

LOCAL void test_leave( void )
{
	WIFISIM_COUNTS	c0, c;
	INT		i;

	if ( !joined ) KT_SKIP("not joined");
	knl_wifisim_counts(&c0);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	for ( i = 0; i < 50; i++ ) {
		knl_wifisim_counts(&c);
		if ( c.leaves > c0.leaves ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(c.leaves > c0.leaves);
	KT_ASSERT(wait_rec1("REASON asked\n", 3000));
}

LOCAL void test_again( void )
{
	UW		before = acks();
	WIFISIM_COUNTS	c0, c;

	knl_wifisim_counts(&c0);
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	addressed = wait_addr(T_DHCP_MS);
	KT_ASSERT(addressed);
	knl_wifisim_counts(&c);
	counts_say("joined again");
	KT_ASSERT_EQ(c.dhcp_discover, c0.dhcp_discover);	/* the address held asked for (INIT-REBOOT) */
	if ( addressed ) KT_ASSERT_EQ(echo_round(5), 5);
}

LOCAL void test_lost( void )
{
	UW	before;

	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("air off"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	KT_ASSERT(wait_rec1("REASON lost\n", 5000));
	before = acks();
	KT_ASSERT_ER(knl_wifisim_ctl("air on"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	if ( joined ) KT_ASSERT_EQ(echo_round(5), 5);
	counts_say("after the network came back");
}

LOCAL void test_password( void )
{
	KT_ASSERT_ER(knl_wifisim_ctl("pass not-the-saved-one"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	KT_ASSERT(wait_rec1("REASON password\n", T_JOIN_MS));
	KT_ASSERT(!knl_wifi_connected());
	KT_ASSERT_ER(knl_wifisim_ctl("pass " WIFISIM_PASS), E_OK);
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
}

LOCAL void test_bcast( void )
{
	UW	before;

	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp bcast"), E_OK);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	before = acks();
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	if ( joined ) KT_ASSERT_EQ(echo_round(5), 5);
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp ucast"), E_OK);
}

LOCAL void test_ackuni( void )
{
	UW	before;

	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp ackuni"), E_OK);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	/* the address taken away, as on a machine that has none yet */
	KT_ASSERT_ER(knl_so_nb_ifaddr(0, 0, 0, TRUE), E_OK);
	before = acks();
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	addressed = wait_addr(T_DHCP_MS);
	counts_say("the ACK unicast");
	KT_ASSERT(addressed);
	/*
	 * The address was set anew: the stack checks it is not another's
	 * first (ARP probes, at most 200 ms in all) and sends from it only
	 * then
	 */
	if ( addressed ) {
		tk_dly_tsk(T_DAD_MS);
		KT_ASSERT_EQ(echo_round(5), 5);
	}
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp ucast"), E_OK);
}

LOCAL void test_reqagain( void )
{
	WIFISIM_COUNTS	c0, c;
	SYSTIM		t0, t;
	UW		before;

	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp slowreq 1500"), E_OK);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	/* no address: the asking begins from a DISCOVER */
	KT_ASSERT_ER(knl_so_nb_ifaddr(0, 0, 0, TRUE), E_OK);
	before = acks();
	knl_wifisim_counts(&c0);
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	(void)tk_get_otm(&t0);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	(void)tk_get_otm(&t);
	knl_wifisim_counts(&c);
	counts_say("a REQUEST soon after the OFFER not answered");
	tm_printf((UB *)"  acknowledged %d ms after the join\n", (INT)( t.lo - t0.lo ));
	KT_ASSERT(c.dhcp_request >= c0.dhcp_request + 2);
	KT_ASSERT_EQ(c.dhcp_discover, c0.dhcp_discover + 1);
	KT_ASSERT((INT)( t.lo - t0.lo ) < 15000);
	addressed = wait_addr(T_DHCP_MS);
	KT_ASSERT(addressed);
	KT_ASSERT_ER(knl_wifisim_ctl("dhcp slowreq 0"), E_OK);
	if ( addressed ) {
		tk_dly_tsk(T_DAD_MS);
		KT_ASSERT_EQ(echo_round(5), 5);
	}
}

#define T_DECLINE_MS	40000
#define POOL_101	0xc0a84d65UL	/* 192.168.77.101, the pool's second address */

LOCAL void test_declined( void )
{
	UW	a = 0, m = 0, g = 0;
	INT	i;

	if ( !joined ) KT_SKIP("not joined");
	/* the station's lease forgotten, so that it is offered the pool's first address again */
	KT_ASSERT_ER(knl_wifisim_ctl("forget"), E_OK);
	KT_ASSERT_ER(knl_wifisim_ctl("taken 100"), E_OK);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	KT_ASSERT_ER(knl_so_nb_ifaddr(0, 0, 0, TRUE), E_OK);
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	/* the first address is another machine's: declined, and the next one offered and taken */
	for ( i = 0; i < T_DECLINE_MS / 100; i++ ) {
		if ( so_getifaddr(&a, &m, &g) >= E_OK && lwip_ntohl(a) == POOL_101 ) break;
		tk_dly_tsk(100);
	}
	counts_say("the address offered first another machine's");
	KT_ASSERT_EQ(lwip_ntohl(a), POOL_101);
	/* the lease of .101 kept, so that the tests after this one find the station's address unchanged */
	KT_ASSERT_ER(knl_wifisim_ctl("taken 0"), E_OK);
	if ( lwip_ntohl(a) == POOL_101 ) {
		tk_dly_tsk(1000);		/* the address's check, at most 200 ms */
		KT_ASSERT_EQ(echo_round(5), 5);
	}
}

/* The machine going off: the network left, the access point told, before knl_wifi_shutdown returns */
LOCAL void test_off( void )
{
	WIFISIM_COUNTS	c0, c;
	UW		before;

	if ( !joined ) KT_SKIP("not joined");
	knl_wifisim_counts(&c0);
	knl_wifi_shutdown();
	KT_ASSERT(!knl_wifi_connected());
	knl_wifisim_counts(&c);
	KT_ASSERT(c.leaves > c0.leaves);		/* the deauthentication heard by the access point */
	KT_ASSERT(wait_rec1("REASON off\n", 3000));
	/* joined again, for the tests after this one */
	before = acks();
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
}

/*
 * A block-ack agreement for what the station sends (OpenBSD's layer):
 * not asked for at the join and DHCP, asked for once the TID carries
 * traffic, and the TID's sequence numbers continuous through it, so
 * that the access point (its net80211 reordering under the agreement)
 * drops nothing as old. The access point keeps one record of the numbers
 * of the frames without the QoS field and of TID 0 ("seqshare on"), as
 * some do: nothing is dropped by it either, the station sending QoS data
 * frames from the join on.
 */
LOCAL void test_agg( void )
{
	WIFISIM_COUNTS	c0, c;
	UW		before;
	INT		i;

#ifndef X64
	KT_SKIP("the agreements are OpenBSD's layer's (x64)");
#endif
	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("seqshare on"), E_OK);
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	before = acks();
	knl_wifisim_counts(&c0);
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	knl_wifisim_counts(&c);
	KT_ASSERT_EQ(c.air[WSIM_AIR_TX_BA], c0.air[WSIM_AIR_TX_BA]);	/* none for DHCP's few frames */
	/* traffic: more than ten frames a second, then the agreement asked for and made */
	(void)echo_round(30);
	for ( i = 0; i < 50; i++ ) {
		knl_wifisim_counts(&c);
		if ( c.air[WSIM_AIR_AP_RX_BA] > c0.air[WSIM_AIR_AP_RX_BA] ) break;
		(void)echo_round(3);
	}
	counts_say("the block-ack agreement");
	tm_printf((UB *)"  agreements: the station's %d, the access point's %d; dropped as old %d,"
		  " by the one record %d\n", (INT)c.air[WSIM_AIR_TX_BA], (INT)c.air[WSIM_AIR_AP_RX_BA],
		  (INT)c.air[WSIM_AIR_AP_OLD], (INT)c.air[WSIM_AIR_AP_SEQSHARE]);
	KT_ASSERT(c.air[WSIM_AIR_AP_RX_BA] > c0.air[WSIM_AIR_AP_RX_BA]);
	/* the data through the agreement, the numbers continuous */
	KT_ASSERT_EQ(echo_round(10), 10);
	knl_wifisim_counts(&c);
	KT_ASSERT_EQ(c.air[WSIM_AIR_AP_OLD], c0.air[WSIM_AIR_AP_OLD]);
	KT_ASSERT_EQ(c.air[WSIM_AIR_AP_SEQSHARE], 0);
	KT_ASSERT_ER(knl_wifisim_ctl("seqshare off"), E_OK);
}

LOCAL UW bgscans( void )
{
	WIFISIM_COUNTS	c;

	knl_wifisim_counts(&c);
	return c.air[WSIM_AIR_BGSCANS];
}

LOCAL void test_bgscan( void )
{
	UW	before, b0;
	SYSTIM	t0, t;
	INT	ms;

#ifndef X64
	KT_SKIP("the background scans are put off in OpenBSD's layer (x64)");
#endif
	if ( !joined ) KT_SKIP("not joined");
	KT_ASSERT_ER(knl_wifisim_ctl("rssi 75"), E_OK);	/* below the layer's threshold */
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	before = acks();
	b0 = bgscans();
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	(void)tk_get_otm(&t0);
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	/* none while the address is asked for, nor in the first T_BG_FIRST_MS */
	do {
		tk_dly_tsk(500);
		(void)tk_get_otm(&t);
		ms = (INT)( t.lo - t0.lo );
	} while ( ms < T_BG_FIRST_MS - 3000 );
	KT_ASSERT_EQ(bgscans(), b0);
	/* one begun after it, the signal being as weak */
	do {
		tk_dly_tsk(500);
		(void)tk_get_otm(&t);
		ms = (INT)( t.lo - t0.lo );
	} while ( ms < T_BG_FIRST_MS + 8000 && bgscans() == b0 );
	KT_ASSERT(bgscans() > b0);
	tm_printf((UB *)"  the first background scan after the join: %d ms\n", ms);
	KT_ASSERT_ER(knl_wifisim_ctl("rssi 45"), E_OK);
	tk_dly_tsk(5000);			/* the scan over the channels ended */
	KT_ASSERT(knl_wifi_connected());
}

LOCAL void test_open( void )
{
	char	line[120];
	UW	before;

	/*
	 * Left first: an access point started again with another security
	 * sends its stations away, and the station would authenticate again
	 * at once with what it last heard of it (WPA2) before its new
	 * beacons came
	 */
	KT_ASSERT_ER(wl_cmd("DISCONNECT"), E_OK);
	KT_ASSERT(wait_left(T_LEAVE_MS));
	KT_ASSERT_ER(knl_wifisim_ctl("sec open"), E_OK);
	tm_sprintf((UB *)line, (CONST UB *)"SAVE %s open auto", ssid_hex);
	KT_ASSERT_ER(knl_wifi_request(me, line), E_OK);
	before = acks();
	KT_ASSERT_ER(wl_cmd("CONNECT"), E_OK);
	joined = wait_connected(T_JOIN_MS);
	KT_ASSERT(joined);
	KT_ASSERT(wait_rec1("SECURITY open\n", 3000));
	KT_ASSERT(wait_ack(before, T_DHCP_MS));
	if ( joined ) KT_ASSERT_EQ(echo_round(5), 5);
	/* the network as it was */
	KT_ASSERT_ER(knl_wifisim_ctl("sec wpa2"), E_OK);
}

EXPORT void ktest_wifisim( void )
{
	WIFISIM_COUNTS	c;
	char		line[80];
	INT		i;

	for ( i = 0; i < 300 && knl_obdev_uuid((CONST UB *)"wlan0", &wl) < E_OK; i++ ) tk_dly_tsk(100);
	if ( i == 300 ) {
		kt_skip("wifisim", "no wlan0 (the Wi-Fi manager did not start)");
		return;
	}
	for ( i = 0; i < 300; i++ ) {
		knl_wifisim_counts(&c);
		if ( c.ready ) break;
		tk_dly_tsk(100);
	}
	if ( !c.ready ) {
		kt_skip("wifisim", "the simulated access point did not start");
		return;
	}
	me = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	if ( me == NULL ) {
		kt_skip("wifisim", "no memory");
		return;
	}
	*me = knl_ob_crd_person;
	(void)knl_ob_session(NULL);		/* nobody logged in: 利用者's networks are joined */
	hex_of(WIFISIM_SSID, ssid_hex);
	(void)adapter_set("wifi");
	(void)so_dhcp_start();			/* the address from DHCP, asked again at each join */

	KT_RUN(test_scan);
	KT_RUN(test_join);
	KT_RUN(test_dhcp);
	KT_RUN(test_moved);
	KT_RUN(test_dns);
	KT_RUN(test_data);
	KT_RUN(test_leave);
	KT_RUN(test_again);
	KT_RUN(test_reqagain);
	KT_RUN(test_declined);
	KT_RUN(test_off);
	KT_RUN(test_agg);
	KT_RUN(test_lost);
	KT_RUN(test_password);
	KT_RUN(test_bcast);
	KT_RUN(test_ackuni);
	KT_RUN(test_bgscan);
	KT_RUN(test_open);

	counts_say("at the end");
	tm_sprintf((UB *)line, (CONST UB *)"FORGET %s", ssid_hex);
	(void)knl_wifi_request(me, line);
	(void)adapter_set(NULL);
	if ( dev_was != NULL ) Kfree(dev_was);
	Kfree(me);
}
