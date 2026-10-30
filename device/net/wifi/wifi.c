/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifi.c (x64 PC, Raspberry Pi 5)
 *	The Wi-Fi manager (design 9.6, 9.6.8).
 *
 *	The card is Intel's 8265 (or another of the iwm(4) family) or Wi-Fi 6
 *	AX200 (iwx(4)) on the PC, driven by OpenBSD's iwm or iwx with
 *	OpenBSD's IEEE 802.11 layer outside the rump kernel
 *	(device/net/wifi/obsd, design 9.6.9), and the
 *	CYW43455 of the Raspberry Pi 5 (on SDIO), driven by NetBSD's bwfm(4)
 *	with NetBSD's IEEE 802.11 layer inside the rump kernel
 *	(peripheral_kernel/network/netbsd/rumpcomp/wifi); both with the WPA2
 *	handshakes of wpa_supplicant
 *	(wpa/wpa_glue.c). This file is what wpa_supplicant's own program
 *	would be around them, in the kernel:
 *
 *	- it starts the card once the object layer is up (its firmware is an
 *	  object of the system volume), and brings its interface (W_IF,
 *	  iwm0, iwx0 or bwfm0) up, which scans;
 *	- it keeps the networks saved (the object Ｗｉ－Ｆｉの設定,
 *	  SYSDEF_CONF_WIFI, which only the system and the administrators
 *	  read): it alone writes them, as the programs ask through wlan0,
 *	  and keeps each network's key: for WPA2 the PMK the passphrase and
 *	  the SSID make, never the passphrase; for WPA3 the password itself,
 *	  which SAE needs at each join (and which the firmware of a card with
 *	  its own supplicant is given);
 *	- it joins the strongest of the networks the person logged in saved
 *	  that the last scan found, or the one asked for, with WPA2-Personal
 *	  or with no security; when another person logs in, the network of
 *	  the one before is left and the new one's are looked for;
 *	- it carries the EAPOL frames of the handshake between the card and
 *	  wpa_supplicant's state machine, and the keys from the machine into
 *	  net80211;
 *	- it is the card the device "neta" may use (netsel.c): the frames of
 *	  the stack go out through the card's interface and what it receives
 *	  comes back.
 *
 *	The device object "wlan0" (OB_S_NET) shows the card in record 1:
 *
 *	  STATE connected		off, starting, nofirmware, idle, scanning,
 *					joining, handshake, connected
 *	  MAC 00:11:22:33:44:55
 *	  SSID <hexadecimal>		the network joined or being joined
 *	  BSSID, FREQ (MHz), SIGNAL (dBm), RATE (Mb/s), SECURITY (open, wpa2,
 *	  wpa3: what the join uses)
 *	  REASON <word>			why the last join failed or ended:
 *					notfound, unsupported, nowpa3 (the
 *					card has no SAE), refused,
 *					timeout, password, lost, asked
 *	  SAE yes			whether the card joins with WPA3 (no)
 *	  SAVED <SSID hex> <open|wpa2|wpa3|wpa2wpa3> <auto|manual>
 *					each network the reader saved (never
 *					its key; the others' are not shown)
 *
 *	and in record 2 the networks the last scan found, the strongest
 *	first: "BSS <bssid> <MHz> <dBm> <open|wep|wpa|wpa2|wpa3|wpa2wpa3|
 *	8021x> <SSID in hexadecimal>" (wpa2wpa3: both PSK and SAE offered). Writing record 1 asks for something: SCAN,
 *	CONNECT <SSID in hexadecimal> (a network the writer saved), CONNECT
 *	(the saved ones again), DISCONNECT (until a CONNECT, or the settings
 *	change), and the writer's saved networks changed:
 *
 *	  SAVE <SSID hex> <open|wpa2|wpa3|wpa2wpa3> <auto|manual> [<passphrase>]
 *	  FORGET <SSID hex>
 *
 *	Each saved network is the user's who saved it (the writer's, as
 *	the process acts for them): no other user sees, joins or forgets
 *	it, and nobody is asked for a password to change their own. A SAVE
 *	with no passphrase keeps the key the network has (the PMK, the
 *	password, or both, as the security needs). wpa2wpa3 is joined with
 *	SAE where the access point offers it, else with PSK. The write is
 *	answered once the settings are written.
 *
 *	A change of record 1 is told as OB_E_CHANGE of it, and of record 2
 *	as each scan ends, by the manager's task as the state moves: the
 *	record is in place before the notice goes, so a reader told reads
 *	what made it. SIGNAL and RATE are measured values that change all
 *	the time: they are written as they are but not told, and a reader
 *	that shows them reads them again now and then while it shows them.
 *	A change that is told is said to the adapters' manager too
 *	(knl_netadp_link): joining and leaving are the card's link.
 *
 *	The settings are lines of record 1 of SYSDEF_CONF_WIFI:
 *
 *	  $$WIFI	on			off: join nothing by itself, and
 *					leave the network joined when the
 *					settings are made so
 *	  $$WIFINET	<SSID hex> <open|wpa2|wpa3|wpa2wpa3> <auto|manual> <user UUID>
 *	  $$WIFIPSK	<n> <64 hexadecimal digits>	the PMK of the n-th (0 up) $$WIFINET
 *	  $$WIFISAE	<n> <password in hexadecimal>	the password of the n-th, for SAE
 *
 *	A $$WIFIPASS <n> <passphrase> line of settings written before is
 *	taken as the passphrase it is, made into the key, and the settings
 *	written again without it. A $$WIFINET line with no user, or "all",
 *	becomes the network of the person logged in when it is read (利用者
 *	while nobody is), and a second line of the same SSID and user is
 *	taken into the first.
 *
 *	Every step goes to the console with "wifi:" (the card, the firmware,
 *	the start, each scan's networks, the authentication, the association,
 *	each message of the 4-way handshake, the keys set), and so into the
 *	records of the start (KERNEL.TXT) on a machine with no serial port.
 */

#include <sys/machine.h>

#if defined(X64) || defined(USE_NETSEL)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "network/netbsd/netlog.h"	/* the lines timed */
#include <ts/net.h>
#ifdef X64
#include <ts/pcie.h>
#endif
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/time.h>
#include "obj/obj.h"
#include "../netsel.h"
#include "wpa/wpa_glue.h"

#if defined(X64) && !defined(WIFI_OBSD)
#error the x64 PC's Wi-Fi is OpenBSD's IEEE 802.11 layer (build_make/x64.mk, WIFI_OBSD)
#endif

/* ---------------------------------------------------------------- the rump kernel's side */

/* What tswifi_if.c answers (the same layouts) */
typedef struct {
	W	state;			/* 0 init, 1 scan, 2 auth, 3 assoc, 4 run */
	W	scanning;
	W	freq;
	W	rssi;			/* as the driver gives it (dbm() below) */
	W	rate;			/* in 500 kb/s */
	W	ifflags;		/* 1 the interface is up, 2 the card runs */
	UB	bssid[6];
	UB	ssid_len;
	UB	ssid[32];
	UB	pad;
#ifdef WIFI_OBSD
	/* OpenBSD's layer's own (tswifi_obsd.c) */
	W	phy;			/* 0 none, 1 802.11a/b/g, 2 802.11n, 3 802.11ac */
	W	mcs, nss, width, sgi;	/* the MCS sent at, its streams, MHz, the short guard interval */
	W	pmf;			/* management frames protected */
	UW	scans, bgscans, roams;	/* scans ended (background ones too), background ones, moves */
	W	port;			/* data may pass */
#endif
} TSWIFI_STATUS;

typedef struct {
	UB	bssid[6];
	UB	ssid_len;
	UB	ssid[32];
	UB	pad;
	UH	freq;
	UH	capinfo;
	W	rssi;
	UH	ie_len;
	UB	ie[256];
} TSWIFI_BSS;

IMPORT int	rump_tswifi_start( UB *mac );
IMPORT void	knl_bootlog_soon( void ) __attribute__((weak));	/* the x64's records (bootlog.c) */
#ifdef X64
IMPORT void	knl_wifi_irq_say( CONST char *why );	/* wifi_user.c: the card's interrupt, for the record */
#endif
IMPORT int	rump_tswifi_param( int wpa, int drop );
IMPORT int	rump_tswifi_optie( CONST UB *ie, int len );
IMPORT int	rump_tswifi_scan( void );
IMPORT int	rump_tswifi_results( TSWIFI_BSS *list, int max );
IMPORT int	rump_tswifi_assoc( CONST UB *bssid, CONST UB *ssid, int ssid_len );
IMPORT int	rump_tswifi_deauth( CONST UB *bssid, int reason );
IMPORT int	rump_tswifi_setkey( int cipher, CONST UB *addr, int idx, int set_tx, CONST UB *seq,
				    int seq_len, CONST UB *key, int key_len );
IMPORT int	rump_tswifi_delkey( CONST UB *addr, int idx );
IMPORT int	rump_tswifi_status( TSWIFI_STATUS *st );
IMPORT int	rump_tswifi_send( CONST void *frame, int len );
IMPORT void	rump_tswifi_debug( int drv, UW net80211 );
IMPORT int	rump_tsnet_ifup( CONST char *ifname, int up );
IMPORT int	rump_tsnet_ip6on( CONST char *ifname, int on );

#ifdef WIFI_OBSD
/*
 * OpenBSD's IEEE 802.11 layer and iwm or iwx (make WIFI_NET80211=obsd,
 * device/net/wifi/obsd/tswifi_obsd.c, design 9.6.9): the manager names
 * the network and the layer chooses its access point, joins it, scans in
 * the background and moves to a better one. The layer stops before each
 * authentication (an event) for the manager to decide that access
 * point's security, and SAE's frames are the manager's.
 */
#define TSW_EV_AUTH	1		/* the layer is to authenticate with bssid */
#define TSW_EV_FRAME	2		/* an authentication frame of SAE came */
typedef struct {
	W	type;
	UB	bssid[6];
	UH	alg, seq, status;
	W	len;
	UB	body[512];
} TSWIFI_EV;

#define TSW_IE_RSN_MAX		64
#define TSW_AKM_PSK		0x01
#define TSW_AKM_PSK_SHA256	0x02
#define TSW_AKM_SAE		0x04

IMPORT int	rump_tswifi_ifup( int up );
IMPORT int	rump_tswifi_join( CONST UB *ssid, int ssid_len, int rsn, int akms, int mfp_required,
				  CONST UB *pmk );
IMPORT int	rump_tswifi_event( TSWIFI_EV *ev );
IMPORT int	rump_tswifi_bss_ie( CONST UB *bssid, int id, UB *buf, int max );
IMPORT int	rump_tswifi_auth_go( void );
IMPORT int	rump_tswifi_auth_send( int seq, int status, CONST UB *body, int len );
IMPORT int	rump_tswifi_auth_done( void );
IMPORT int	rump_tswifi_auth_fail( void );
IMPORT int	rump_tswifi_port( int valid );
IMPORT void	rump_tswifi_trace( void );	/* a join timed: the time event of the card watched */

/* The card's interface is not in the rump kernel: brought up and down by the layer, and has no IPv6 to stop */
#define rump_tsnet_ifup( name, up )	rump_tswifi_ifup(up)
#define rump_tsnet_ip6on( name, on )	0
#endif

/*
 * A card whose firmware has a supplicant of its own (tswifi_sdio.c,
 * 9.6.8): what it can do, the next join's keys given to it, and the link
 * as it has it. Weak: a rump kernel without them has no such card.
 */
typedef struct {
	W	authorized;		/* the keys are in */
	W	rssi;			/* dBm */
	W	tx_kbps, rx_kbps;
	W	mode;			/* 0 802.11a/b/g, 1 802.11n, 2 802.11ac */
	W	freq;			/* MHz */
	W	sup_status;		/* the card's supplicant's last state, -1 none */
	W	roams;
	UB	bssid[6];
	UB	pad[2];
} TSWIFI_FWLINK;

#define FW_IDSUP	1		/* rump_tswifi_fw_caps: the card's own supplicant */
#define FW_SAE		2
#define FW_MFP		4
#define FW_LINK_MS	2000		/* how often the link is asked of such a card while connected */

IMPORT int	rump_tswifi_fw_caps( void ) __attribute__((weak));
IMPORT int	rump_tswifi_fw_keys( int use, CONST UB *pmk, CONST UB *pw, int pw_len ) __attribute__((weak));
IMPORT int	rump_tswifi_fw_link( TSWIFI_FWLINK *l ) __attribute__((weak));
IMPORT int	rump_tswifi_stats( UW *v, int n ) __attribute__((weak));	/* tswifi_if.c: the frames' counts */

#if WIFI_SIM
IMPORT BOOL	knl_wifisim_present( void );		/* wifi_user_sim.c: the simulator's station (WIFI_SIM=1) */
IMPORT void	knl_wifisim_ap_start( void );		/* sim/wifisim_ap.c */
#endif
#ifdef X64
IMPORT BOOL	knl_wifi_pci_find( T_PCIDEV *d );	/* wifi_user.c */
IMPORT CONST char *rump_tswifi_ifname( void );	/* the layer's: iwm0 or iwx0, by the card's driver */
#define W_IF		rump_tswifi_ifname()
#else
IMPORT BOOL	knl_wifi5_present( void );		/* wifi_user_rpi5.c */
IMPORT int	rump_tswifi_take( UB *mac );
#define W_IF		"bwfm0"
#define W_TAKE_TRIES	60			/* a chip the SD/SDIO layer finds late: 30 s */
#endif

#define S_INIT		0
#define S_SCAN		1
#define S_AUTH		2
#define S_ASSOC		3
#define S_RUN		4

/*
 * net80211's words on the console while a network is joined, and after:
 * its changes of state, and the reason an access point gives when it
 * sends the station away (a deauthentication or a disassociation)
 */
#define DBG_JOIN	( 0x00080000 | 0x00400000 | 0x00800000 | 0x00001000 )	/* STATE AUTH ASSOC WPA */
#define DBG_RUN		( 0x00080000 | 0x00400000 | 0x00800000 )		/* STATE AUTH ASSOC */

/* ---------------------------------------------------------------- the manager's state */

#define W_PRI		26
#define W_STKSZ		(32 * 1024)
#define W_TICK_MS	50
#define W_NBSS		32		/* networks of a scan room is made for at first; more, more room */
#define RX_SLOTS	128
#define EQ_SLOTS	8
#define ETH_PAE		0x888e

#define JOIN_MS		8000		/* authentication and association */
#define SHAKE_MS	10000		/* the 4-way and group key handshakes */
#define SCAN_MS		15000		/* a scan that does not end */
#define SCAN_WAIT_MS	3000		/* for the scan of a card started again to be seen */
#define RESCAN_MS	30000		/* between scans while nothing is joined */
#define RETRY_MS	20000		/* before a network that failed is tried again */
#define SCAN_AGAIN_MS	200		/* a scan asked again, once the card is back to INIT */
#define ASK_MS		1000		/* between scans for a network asked for, while one cannot begin */
#define DOWN_MS		1000		/* the driver took its interface down: the card is started again */
#define STOPPED_MS	10000		/* the interface up but the card not running: started again */
#define LINK_SAY_MS	60000		/* how often the link is told while connected */

typedef enum { WS_OFF, WS_STARTING, WS_NOFW, WS_IDLE, WS_SCANNING, WS_JOINING, WS_HANDSHAKE, WS_CONNECTED } WSTATE;
LOCAL CONST char *CONST ws_name[] = { "off", "starting", "nofirmware", "idle", "scanning", "joining", "handshake", "connected" };

/* A saved network */
#define PW_MAX		63		/* a password of WPA3: 8 to 63 characters, as WPA2's passphrase */

typedef struct {
	UB	ssid[32];
	INT	ssid_len;
	INT	sec;		/* WPAG_SAVED_*, 0 no security */
	BOOL	wpa2;		/* WPA2-Personal may be used: sec WPA2 or WPA2WPA3 */
	BOOL	autojoin;
	BOOL	keyed;		/* pmk holds its key */
	UB	pmk[32];	/* the key: what the passphrase and the SSID make (PBKDF2-SHA1) */
	INT	pw_len;		/* the password for SAE (sec WPA3 or WPA2WPA3), 0 none */
	char	pw[PW_MAX + 1];
	TS_UUID	owner;		/* the user who saved it: theirs alone */
	UD failed_at;	/* when it last failed, 0 never */
	INT	fails;
	/*
	 * Left a while after a 4-way handshake that failed as of a wrong
	 * key, as wpa_supplicant leaves a network (its ssid->auth_failures
	 * and disabled_until, wpas_auth_failed): not saved
	 */
	UW	auth_failures;
	UD	disabled_until;	/* ms; 0 not left */
} WNET;

LOCAL BOOL	present = FALSE;	/* a card the driver drives is there */
LOCAL BOOL	ready = FALSE;		/* its interface is there */
#ifdef X64
LOCAL T_PCIDEV	wdev;
#endif
LOCAL UB	wmac[NET_MAC_LEN] = { 0x02, 0x00, 0x00, 0x54, 0x57, 0x46 };	/* until the card's is read */
LOCAL volatile BOOL	wmac_known = FALSE;	/* wmac is the card's own, read from it */
LOCAL volatile WSTATE	ws = WS_OFF;
LOCAL ID	wmtx, wflg;
LOCAL TS_UUID	wuuid;
LOCAL BOOL	wobj = FALSE;

LOCAL TSWIFI_STATUS	st;		/* as last read */
LOCAL TSWIFI_BSS	*bss = NULL;	/* the last scan's networks, as many as it found */
LOCAL INT		bss_cap = 0;
LOCAL INT		nbss = 0;
LOCAL INT		nscans = 0;
LOCAL WNET		*nets = NULL;	/* the networks saved, as many as the settings keep */
LOCAL INT		nnet = 0;
LOCAL BOOL		wifi_on = TRUE;
LOCAL BOOL		held = FALSE;	/* DISCONNECT asked: nothing joined by itself */
LOCAL INT		want_net = -1;	/* CONNECT asked for this one */
LOCAL INT		cur_net = -1;	/* the network being joined or joined */
LOCAL TSWIFI_BSS	cur_bss;
LOCAL BOOL		cur_wpa2;	/* the join has keys: WPA2's or WPA3's */
LOCAL INT		cur_akm = 0;	/* the key management the join uses (WPAG_AKM_*), 0 none told */
LOCAL UD		t_state, t_scan;
LOCAL INT		last_s = -1;
LOCAL BOOL		scan_wait = FALSE;	/* the card started again: its scan not seen yet */
LOCAL BOOL		scan_again = FALSE;	/* the card is being put back to INIT for a scan */
LOCAL CONST char	*reason = NULL;
LOCAL UD		t_down = 0;		/* since when the card has not been running, 0 it runs */
LOCAL UD		t_restart = 0;		/* when the card was last started again */
LOCAL INT		restarts = 0;		/* times it was, since it was last connected */
LOCAL UD		t_link = 0;		/* when the link was last told */
LOCAL volatile UW	link_gen = 0;		/* one more each time a network is joined */
LOCAL INT		fw_caps = 0;		/* FW_*: what the card's firmware does itself */
LOCAL BOOL		cur_fw = FALSE;		/* the network being joined has its handshakes made by the card */
LOCAL UD		t_fw = 0;		/* when the link was last asked of the card */
LOCAL W			fw_lk_rssi = 0;		/* the card's own signal (dBm) and rate (kb/s), */
LOCAL W			fw_lk_kbps = 0;		/* as last asked while connected */
LOCAL UW		fw_lk_gen = 0;		/* the join they are of (link_gen), 0 none */

LOCAL UB		*wtext = NULL;	/* record 1, made as long as it needs */
LOCAL INT		wtlen = 0;
LOCAL UB		*wtold = NULL;	/* record 1 without SIGNAL and RATE, as last told */
LOCAL INT		wtoldlen = 0;
LOCAL UB		*stext = NULL;	/* record 2 */
LOCAL INT		stlen = 0;

#define F_EAPOL		0x01
#define F_CMD		0x02
#define F_CONF		0x04
#define F_REQ		0x08

#define CMD_MAX		256		/* a line written to record 1: SAVE with its passphrase fits */

LOCAL char		cmd[CMD_MAX];	/* the command written, until the manager takes it */
LOCAL TS_UUID		cmd_user;	/* who wrote it */
LOCAL TS_UUID		cur_user;	/* the person logged in, as the manager last looked */

/* A change of the saved networks asked through record 1, done by the manager */
LOCAL ID		rq_free = 0;	/* the one slot for it: 1 while free */
LOCAL ID		rq_done = 0;	/* given once the manager has done it */
LOCAL char		rq_line[CMD_MAX];
LOCAL TS_UUID		rq_user;
LOCAL ER		rq_er;
LOCAL volatile BOOL	w_serving = FALSE;	/* the manager takes the changes */
LOCAL volatile BOOL	rq_pending = FALSE;

/* the frames the card received for the stack */
typedef struct {
	UH	len;
	UB	f[NET_FRAME_MAX];
} RXSLOT;

LOCAL RXSLOT		*rx_ring;
LOCAL UINT		rx_head, rx_tail;
LOCAL T_SPLOCK		rx_lock;
LOCAL ID		rx_sem;
LOCAL UD		n_sent, n_recv, n_drop;

/* the EAPOL frames, for the manager */
LOCAL RXSLOT		*eq;
LOCAL UINT		eq_head, eq_tail;
LOCAL RXSLOT		eapol_pend;		/* one that came before the association was seen */
LOCAL BOOL		eapol_pending = FALSE;

#define LOCK()		tk_loc_mtx(wmtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(wmtx)

LOCAL UD now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (UD)( ns / 1000000 );
}

/*
 * The signal in dBm, from what the driver hands net80211: iwm and iwx give its
 * size, the sign dropped (83 is -83 dBm); bwfm gives dBm as a signed
 * byte
 */
LOCAL INT dbm( W r )
{
#ifdef X64
	return ( r > 0 ) ? -(INT)r : (INT)r;
#else
	return (INT)(signed char)(UB)r;
#endif
}

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

/* An SSID for the console: its bytes, the unprintable ones as dots */
LOCAL void ssid_str( CONST UB *s, INT n, char *out )
{
	INT	i;

	for ( i = 0; i < n && i < 32; i++ ) out[i] = ( s[i] >= 0x20 && s[i] < 0x7f ) ? (char)s[i] : '.';
	out[i] = '\0';
}

LOCAL INT put_hex( UB *t, INT n, INT max, CONST UB *b, INT len )
{
	static CONST char hex[] = "0123456789abcdef";
	char	two[3];
	INT	i;

	two[2] = '\0';
	for ( i = 0; i < len; i++ ) {
		two[0] = hex[b[i] >> 4];
		two[1] = hex[b[i] & 15];
		n = knl_oj_put(t, n, max, two);
	}
	return n;
}

LOCAL INT put_mac( UB *t, INT n, INT max, CONST UB *m )
{
	INT	i;

	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		n = put_hex(t, n, max, m + i, 1);
		if ( i < NET_MAC_LEN - 1 ) n = knl_oj_put(t, n, max, ":");
	}
	return n;
}

LOCAL INT put_num( UB *t, INT n, INT max, INT v )
{
	char	s[16];

	(void)cf_itoa(v, s);
	return knl_oj_put(t, n, max, s);
}

LOCAL INT hexval( char c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

/* Hexadecimal into bytes: how many, or -1 */
LOCAL INT unhex( CONST char *s, UB *b, INT max )
{
	INT	n = 0, h, l;

	while ( s[0] != '\0' && s[0] != ' ' ) {
		h = hexval(s[0]);
		l = ( h >= 0 ) ? hexval(s[1]) : -1;
		if ( l < 0 || n >= max ) return -1;
		b[n++] = (UB)( h * 16 + l );
		s += 2;
	}
	return n;
}

LOCAL CONST char *sec_name( INT c )
{
	switch ( c ) {
	case WPAG_OPEN:		return "open";
	case WPAG_WEP:		return "wep";
	case WPAG_WPA:		return "wpa";
	case WPAG_WPA2:		return "wpa2";
	case WPAG_WPA3:		return "wpa3";
	case WPAG_WPA2WPA3:	return "wpa2wpa3";
	default:		return "8021x";
	}
}

/* The settings' word of a saved network's security, and back (-1 a word of none) */
LOCAL CONST char *saved_name( INT sec )
{
	switch ( sec ) {
	case WPAG_SAVED_WPA2:		return "wpa2";
	case WPAG_SAVED_WPA3:		return "wpa3";
	case WPAG_SAVED_WPA2WPA3:	return "wpa2wpa3";
	default:			return "open";
	}
}

LOCAL INT saved_of( CONST char *w )
{
	if ( knl_strcmp(w, "open") == 0 ) return 0;
	if ( knl_strcmp(w, "wpa2") == 0 ) return WPAG_SAVED_WPA2;
	if ( knl_strcmp(w, "wpa3") == 0 ) return WPAG_SAVED_WPA3;
	if ( knl_strcmp(w, "wpa2wpa3") == 0 ) return WPAG_SAVED_WPA2WPA3;
	return -1;
}

/* Whether a security of saved networks has a PMK (WPA2-Personal), or a password (SAE) */
#define SEC_PSK( sec )	( (sec) == WPAG_SAVED_WPA2 || (sec) == WPAG_SAVED_WPA2WPA3 )
#define SEC_SAE( sec )	( (sec) == WPAG_SAVED_WPA3 || (sec) == WPAG_SAVED_WPA2WPA3 )

LOCAL INT sec_of( CONST TSWIFI_BSS *b )
{
	return knl_wpa_classify(b->ie, b->ie_len, ( b->capinfo & 0x0010 ) != 0);
}

/* ---------------------------------------------------------------- the card for netsel.c */

EXPORT BOOL knl_wifi_present( void )
{
#if WIFI_SIM
	if ( !present ) present = knl_wifisim_present();
#elif defined(X64)
	if ( !present ) present = knl_wifi_pci_find(&wdev);
#else
	if ( !present ) present = knl_wifi5_present();
#endif
	return present;
}

EXPORT ER knl_wifi_get_mac( UB *mac )
{
	if ( !knl_wifi_present() ) return E_NOEXS;
	knl_memcpy(mac, wmac, NET_MAC_LEN);
	return E_OK;
}

/*
 * The card's own address has been read (the card started): until then
 * the address knl_wifi_get_mac gives is a stand-in, and the stack is not
 * moved onto the Wi-Fi (netadp.c)
 */
EXPORT BOOL knl_wifi_mac_known( void )
{
	return wmac_known;
}

/*
 * The machine going off or starting again (knl_finish_device): the
 * network joined is left first, with a deauthentication of reason 3
 * (leaving), as Linux leaves it when the interface goes down
 * (cfg80211_leave, WLAN_REASON_DEAUTH_LEAVING) and NetworkManager and
 * wpa_supplicant stop, and Windows when it shuts down. Without it the
 * access point, and a mesh of them, keeps the station as joined until
 * its own time runs out, and may send what is for the station's address
 * there when the station has joined another of them. The manager does
 * it as for DISCONNECT; this waits for it, a second at most, and for the
 * frame to go out.
 */
EXPORT void knl_wifi_shutdown( void )
{
	UD	t0;

	if ( !w_serving || wflg <= 0 || ws < WS_JOINING ) return;
	tm_printf((UB *)"wifi: the machine goes off: the network is left\n");
	LOCK();
	knl_strcpy(cmd, "SHUTDOWN");
	UNLOCK();
	(void)tk_set_flg(wflg, F_CMD);
	for ( t0 = now_ms(); ws >= WS_JOINING && now_ms() - t0 < 1000; ) {
		(void)tk_dly_tsk(20);
	}
	(void)tk_dly_tsk(100);		/* the deauthentication on the air */
	tm_printf((UB *)"wifi: the network %s\n", ( ws >= WS_JOINING ) ? "not left in time" : "left");
}

EXPORT BOOL knl_wifi_connected( void )
{
	return ws == WS_CONNECTED;
}

/*
 * One more each time a network is joined (netadp.c): a join again after
 * the network was lost, or of another one, is seen even when it came
 * between two of the adapters' manager's looks
 */
EXPORT UW knl_wifi_link_gen( void )
{
	return link_gen;
}

/*
 * The first data frames each way after a join, for the records: their
 * kind (the Ethernet type; for IPv4 the protocol and the UDP ports, so
 * that DHCP's are seen), the addresses and the length, and for those
 * sent what the card's interface answered
 */
#define FRAMES_SAID	6
LOCAL volatile INT	said_out = FRAMES_SAID, said_in = FRAMES_SAID;
LOCAL volatile UD	n_send_err = 0;

LOCAL void frame_say( CONST char *way, CONST UB *f, INT len, INT er )
{
	UH	type = (UH)( ( f[12] << 8 ) | f[13] );
	char	what[48];

	what[0] = '\0';
	if ( type == 0x0800 && len >= 14 + 20 ) {
		CONST UB	*ip = f + 14;
		INT		hl = ( ip[0] & 15 ) * 4;

		if ( ip[9] == 17 && len >= 14 + hl + 4 ) {
			tm_sprintf((UB *)what, (CONST UB *)" UDP %d->%d", ( ip[hl] << 8 ) | ip[hl + 1],
				   ( ip[hl + 2] << 8 ) | ip[hl + 3]);
		} else {
			tm_sprintf((UB *)what, (CONST UB *)" protocol %d", ip[9]);
		}
	} else if ( type == 0x0806 && len >= 14 + 8 ) {
		tm_sprintf((UB *)what, (CONST UB *)" ARP %s", ( f[21] == 1 ) ? "request" : "reply");
	}
	tm_printf((UB *)"wifi: data %s: %04x%s, %d bytes, %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x (%d)\n",
		  way, type, what, len, f[6], f[7], f[8], f[9], f[10], f[11], f[0], f[1], f[2], f[3], f[4], f[5], er);
}

EXPORT ER knl_wifi_send( CONST void *frame, SZ len )
{
	INT	er;

	if ( ws != WS_CONNECTED ) {
		n_drop++;
		return E_IO;
	}
	er = rump_tswifi_send(frame, (int)len);
	if ( said_out < FRAMES_SAID && len >= 14 ) {
		said_out++;
		frame_say("out", (CONST UB *)frame, (INT)len, er);
	}
	if ( er != 0 ) {
		n_drop++;
		n_send_err++;
		return E_IO;
	}
	n_sent++;
	return E_OK;
}

EXPORT INT knl_wifi_recv( void *frame, SZ size, TMO tmout )
{
	RXSLOT	*s;
	UINT	imask;
	INT	len;
	ER	er;

	if ( rx_ring == NULL ) {
		if ( tmout != TMO_POL ) tk_dly_tsk(( tmout == TMO_FEVR || tmout > 200 ) ? 200 : tmout);
		return E_TMOUT;
	}
	er = tk_wai_sem(rx_sem, 1, tmout);
	if ( er < E_OK ) return ( er == E_TMOUT ) ? E_TMOUT : (INT)er;
	ISpinLock(&rx_lock, &imask);
	s = &rx_ring[rx_head % RX_SLOTS];
	len = s->len;
	if ( len > size ) len = (INT)size;
	knl_memcpy(frame, s->f, len);
	rx_head++;
	ISpinUnlock(&rx_lock, &imask);
	n_recv++;
	return len;
}

EXPORT ER knl_wifi_stat( T_NETSTAT *nst )
{
	if ( !knl_wifi_present() ) return E_NOEXS;
	knl_memset(nst, 0, sizeof(*nst));
	nst->sent = n_sent;
	nst->recv = n_recv;
	nst->drop = n_drop;
	nst->up = ( ws == WS_CONNECTED );
	return E_OK;
}

/*
 * A frame the card received (wifi_user.c, wifi_user_rpi5.c, from the rump kernel's soft
 * interrupt): an EAPOL one to the manager, the rest to the stack's
 * reader. Neither waits.
 */
EXPORT void knl_wifi_rx( CONST UB *frame, INT len )
{
	RXSLOT	*s;
	UINT	imask;
	BOOL	put = FALSE;

	if ( len < 14 || len > NET_FRAME_MAX || rx_ring == NULL ) return;
	ISpinLock(&rx_lock, &imask);
	if ( ( ( (UH)frame[12] << 8 ) | frame[13] ) == ETH_PAE ) {
		if ( eq_tail - eq_head < EQ_SLOTS ) {
			s = &eq[eq_tail % EQ_SLOTS];
			knl_memcpy(s->f, frame, len);
			s->len = (UH)len;
			eq_tail++;
			put = TRUE;
		}
		ISpinUnlock(&rx_lock, &imask);
		if ( put ) (void)tk_set_flg(wflg, F_EAPOL);
		return;
	}
	if ( ws == WS_CONNECTED && rx_tail - rx_head < RX_SLOTS ) {
		s = &rx_ring[rx_tail % RX_SLOTS];
		knl_memcpy(s->f, frame, len);
		s->len = (UH)len;
		rx_tail++;
		put = TRUE;
	} else {
		n_drop++;
	}
	ISpinUnlock(&rx_lock, &imask);
	if ( put ) (void)tk_sig_sem(rx_sem, 1);
	if ( said_in < FRAMES_SAID ) {
		said_in++;
		frame_say(put ? "in" : "in, dropped", frame, len, 0);
	}
}

/* The card's name as the adapters are shown: its length */
EXPORT INT knl_wifi_name( UB *t, INT n, INT max )
{
#if WIFI_SIM
	n = knl_oj_put(t, n, max, "Simulated Wi-Fi (wifisim)");
#elif defined(X64)
	UB	id[2];

	n = knl_oj_put(t, n, max, "Intel Wi-Fi 8086:");
	id[0] = (UB)( wdev.device >> 8 );
	id[1] = (UB)wdev.device;
	n = put_hex(t, n, max, id, 2);
#else
	n = knl_oj_put(t, n, max, "Broadcom Wi-Fi CYW43455 02d0:a9bf");
#endif
	return n;
}

LOCAL INT link_dbm( void );
LOCAL INT link_mbps( void );

/* The adapters' list's lines of the card (netadp.c); without measured, the signal and the rate as 0 */
EXPORT INT knl_wifi_line( UB *t, INT n, INT max, BOOL measured )
{
	n = knl_oj_put(t, n, max, "ADAPTER wifi wifi ");
	n = knl_oj_put(t, n, max, ( ws == WS_CONNECTED ) ? "up " : "down ");
	n = put_num(t, n, max, ( ws == WS_CONNECTED && measured ) ? link_mbps() : 0);
	n = knl_oj_put(t, n, max, " ");
	n = put_mac(t, n, max, wmac);
	n = knl_oj_put(t, n, max, " ");
	n = knl_wifi_name(t, n, max);
	n = knl_oj_put(t, n, max, "\nWIFI ");
	n = knl_oj_put(t, n, max, ws_name[ws]);
	n = knl_oj_put(t, n, max, " ");
	n = put_num(t, n, max, ( ws == WS_CONNECTED && measured ) ? link_dbm() : 0);
	n = knl_oj_put(t, n, max, " ");
	if ( ws >= WS_JOINING ) n = put_hex(t, n, max, cur_bss.ssid, cur_bss.ssid_len);
	return knl_oj_put(t, n, max, "\n");
}

/* ---------------------------------------------------------------- what wpa_glue.c calls */

/*
 * Which message of the handshakes an EAPOL-Key frame is, from its key
 * information (IEEE 802.11 12.7.2): the 4-way's 1 to 4, the group key's
 * 1 and 2. body is the EAPOL frame, from its version byte on.
 */
LOCAL CONST char *eapol_msg( CONST UB *body, INT len, UD *p_replay )
{
	UH	ki;
	INT	i;

	*p_replay = 0;
	if ( len < 17 || body[1] != 3 ) return "not a key";
	ki = (UH)( ( body[5] << 8 ) | body[6] );
	for ( i = 0; i < 8; i++ ) *p_replay = ( *p_replay << 8 ) | body[9 + i];
	if ( ki & 0x0008 ) {			/* pairwise */
		if ( ( ki & 0x0080 ) && !( ki & 0x0100 ) ) return "4-way 1 of 4";
		if ( ( ki & 0x0080 ) && ( ki & 0x0040 ) ) return "4-way 3 of 4";
		if ( !( ki & 0x0080 ) && ( ki & 0x0200 ) ) return "4-way 4 of 4";
		if ( !( ki & 0x0080 ) ) return "4-way 2 of 4";
		return "pairwise, other";
	}
	return ( ki & 0x0080 ) ? "group key 1 of 2" : "group key 2 of 2";
}

EXPORT int knl_wifi_eapol_tx( CONST unsigned char *dst, CONST unsigned char *body, int len )
{
	UB	f[NET_FRAME_MAX];

	if ( len < 0 || len > NET_FRAME_MAX - 14 ) return -1;
	knl_memcpy(f, dst, 6);
	knl_memcpy(f + 6, wmac, 6);
	f[12] = (UB)( ETH_PAE >> 8 );
	f[13] = (UB)ETH_PAE;
	knl_memcpy(f + 14, body, len);
	{
		UD	rc;
		CONST char *m = eapol_msg(body, len, &rc);
		int	er = rump_tswifi_send(f, len + 14);

		/* what went out, and whether the card took it (an errno value of the rump kernel's, 0 taken) */
		tm_printf((UB *)"wifi: EAPOL-Key out: %s, %d bytes, replay %ld: %d\n", m, len, (D)rc, er);
		return ( er == 0 ) ? 0 : -1;
	}
}

EXPORT int knl_wifi_setkey( int cipher, CONST unsigned char *addr, int idx, int set_tx,
			    CONST unsigned char *seq, int seq_len, CONST unsigned char *key, int key_len )
{
	int	er = rump_tswifi_setkey(cipher, addr, idx, set_tx, seq, seq_len, key, key_len);

	tm_printf((UB *)"wifi: %s key set (%s, index %d, %d bytes): %s\n",
		  ( addr[0] & 1 ) ? "group" : "pairwise",
		  ( cipher == WPAG_CIPHER_CCMP ) ? "CCMP" : ( cipher == WPAG_CIPHER_BIP ) ? "BIP" : "TKIP",
		  idx, key_len, ( er == 0 ) ? "ok" : "refused");
	return ( er == 0 ) ? 0 : -1;
}

EXPORT int knl_wifi_delkey( CONST unsigned char *addr, int idx )
{
	return ( rump_tswifi_delkey(addr, idx) == 0 ) ? 0 : -1;
}

EXPORT void knl_wifi_deauth( int r )
{
	tm_printf((UB *)"wifi: leaving the network (reason %d)\n", r);
	(void)rump_tswifi_deauth(cur_bss.bssid, r);
}

EXPORT void knl_wifi_say( CONST char *line )
{
	tm_putstring((CONST UB *)"wifi: ");
	tm_putstring((CONST UB *)line);
}

EXPORT void knl_wifi_halt( void )
{
	(void)tk_slp_tsk(TMO_FEVR);
}

#ifdef WIFI_OBSD
/* Something of OpenBSD's layer waits for the manager (obsd_user.c): its loop woken */
EXPORT void knl_wifi_kick( void )
{
	if ( wflg > 0 ) (void)tk_set_flg(wflg, F_EAPOL);
}
#endif

EXPORT unsigned long long knl_wifi_ms( void )
{
	return (unsigned long long)now_ms();
}

EXPORT void knl_wifi_random( void *buf, int len )
{
	(void)ts_get_random(buf, (SZ)len);
}

/* ---------------------------------------------------------------- the device object */

LOCAL void put_line( UB *t, INT *n, INT max, CONST char *k, CONST char *v )
{
	*n = knl_oj_put(t, *n, max, k);
	*n = knl_oj_put(t, *n, max, " ");
	*n = knl_oj_put(t, *n, max, v);
	*n = knl_oj_put(t, *n, max, "\n");
}

LOCAL BOOL same_user( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return same_bytes((CONST UB *)a, (CONST UB *)b, (INT)sizeof(TS_UUID));
}

/* A user as the settings and record 1 say them: " <UUID>" */
LOCAL INT put_user( UB *t, INT n, INT max, CONST TS_UUID *u )
{
	char	us[40];

	if ( ts_uuid_to_str(u, us, sizeof(us)) < E_OK ) us[0] = '\0';
	n = knl_oj_put(t, n, max, " ");
	return knl_oj_put(t, n, max, us);
}

LOCAL BOOL card_sae( void );

/*
 * The signal and the rate record 1 says while connected: a card whose
 * firmware has the supplicant (9.6.8) has them itself, and net80211's
 * are not kept up to date for it
 */
LOCAL INT link_dbm( void )
{
	return ( fw_lk_gen != 0 && fw_lk_gen == link_gen && fw_lk_rssi != 0 ) ? (INT)fw_lk_rssi : dbm(st.rssi);
}

LOCAL INT link_mbps( void )
{
	return ( fw_lk_gen != 0 && fw_lk_gen == link_gen && fw_lk_kbps > 0 ) ? (INT)( fw_lk_kbps / 1000 ) : st.rate / 2;
}

/*
 * Record 1 as the card is now, as long as the networks saved make it,
 * each saved one with its user; ctx not NULL, without the signal and
 * the rate (what is told of)
 */
LOCAL INT state_make( void *ctx, UB *t, INT max )
{
	char	num[16];
	INT	n = 0, k;
	BOOL	measured = ( ctx == NULL );

	put_line(t, &n, max, "STATE", ws_name[ws]);
	n = knl_oj_put(t, n, max, "MAC ");
	n = put_mac(t, n, max, wmac);
	n = knl_oj_put(t, n, max, "\n");
	if ( ws >= WS_JOINING ) {
		n = knl_oj_put(t, n, max, "SSID ");
		n = put_hex(t, n, max, cur_bss.ssid, cur_bss.ssid_len);
		n = knl_oj_put(t, n, max, "\nBSSID ");
		n = put_mac(t, n, max, cur_bss.bssid);
		n = knl_oj_put(t, n, max, "\n");
		(void)cf_itoa(cur_bss.freq, num);
		put_line(t, &n, max, "FREQ", num);
		if ( measured ) {
			(void)cf_itoa(( ws == WS_CONNECTED ) ? link_dbm() : dbm(cur_bss.rssi), num);
			put_line(t, &n, max, "SIGNAL", num);
			(void)cf_itoa(( ws == WS_CONNECTED ) ? link_mbps() : 0, num);
			put_line(t, &n, max, "RATE", num);
		}
		put_line(t, &n, max, "SECURITY", !cur_wpa2 ? "open" : ( cur_akm == WPAG_AKM_SAE ) ? "wpa3" : "wpa2");
	}
	if ( reason != NULL ) put_line(t, &n, max, "REASON", reason);
	put_line(t, &n, max, "SAE", card_sae() ? "yes" : "no");
	for ( k = 0; k < nnet; k++ ) {
		n = knl_oj_put(t, n, max, "SAVED ");
		n = put_hex(t, n, max, nets[k].ssid, nets[k].ssid_len);
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put(t, n, max, saved_name(nets[k].sec));
		n = knl_oj_put(t, n, max, nets[k].autojoin ? " auto" : " manual");
		n = put_user(t, n, max, &nets[k].owner);
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

/*
 * Record 1 as a reader is shown it, into out (NULL: only its length):
 * the SAVED lines of the reader's own networks, without the user, and
 * none of the others'; everything to the kernel's own tasks. Under the
 * lock.
 */
LOCAL INT state_for( ID pid, UB *out, INT max )
{
	char	us[40];
	INT	i = 0, n = 0, e, ul;

	if ( wtext == NULL ) return 0;
	if ( pid <= 0 || ts_uuid_to_str(&knl_ob_crd_of(pid)->user, us, sizeof(us)) < E_OK ) {
		if ( out != NULL ) knl_memcpy(out, wtext, ( wtlen < max ) ? wtlen : max);
		return wtlen;
	}
	ul = (INT)knl_strlen(us);
	while ( i < wtlen ) {
		for ( e = i; e < wtlen && wtext[e] != '\n'; e++ ) ;
		if ( e - i > 6 && same_bytes(wtext + i, (CONST UB *)"SAVED ", 6) ) {
			/* " <UUID>" at its end: the reader's own, shown without it */
			if ( e - i <= ul + 1 || !same_bytes(wtext + e - ul, (CONST UB *)us, ul) ) {
				i = e + 1;
				continue;
			}
			if ( out != NULL && n + ( e - ul - 1 - i ) + 1 <= max ) {
				knl_memcpy(out + n, wtext + i, e - ul - 1 - i);
				out[n + ( e - ul - 1 - i )] = '\n';
			}
			n += e - ul - 1 - i + 1;
		} else {
			INT	k = ( e < wtlen ) ? e + 1 - i : e - i;

			if ( out != NULL && n + k <= max ) knl_memcpy(out + n, wtext + i, k);
			n += k;
		}
		i = e + 1;
	}
	return n;
}

/*
 * Record 1 as the card is now; told when it changed in more than the
 * signal and the rate. It is in place before the notice goes.
 */
LOCAL void state_refresh( void )
{
	UB	*t, *k, *old = NULL, *oldk = NULL;
	INT	n, kn;
	BOOL	told;

	n = knl_oj_make(state_make, NULL, &t);
	if ( n < 0 ) {
		return;				/* no memory: what was said stays */
	}
	kn = knl_oj_make(state_make, (void *)1, &k);
	if ( kn < 0 ) {
		Kfree(t);
		return;
	}
	LOCK();
	if ( n != wtlen || wtext == NULL || !same_bytes(t, wtext, n) ) {
		old = wtext;
		wtext = t;
		wtlen = n;
		t = NULL;
	}
	told = ( kn != wtoldlen || wtold == NULL || !same_bytes(k, wtold, kn) );
	if ( told ) {
		oldk = wtold;
		wtold = k;
		wtoldlen = kn;
		k = NULL;
	}
	UNLOCK();
	if ( t != NULL ) Kfree(t);
	if ( k != NULL ) Kfree(k);
	if ( old != NULL ) Kfree(old);
	if ( oldk != NULL ) Kfree(oldk);
	if ( told ) {
		if ( wobj ) knl_obdev_post(&wuuid, OB_NET_STATE, OB_E_CHANGE, NULL);
		knl_net_link_said();
	}
}

LOCAL INT w_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>The Wi-Fi card. Record 1: its state as key value lines; written, a command "
			  "(SCAN, CONNECT, DISCONNECT). Record 2: the networks the last scan found.</p>");
}

LOCAL INT w_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
#ifdef X64
	n = knl_oj_put(j, n, max, ",\"wifi\":true,\"vid\":32902,\"pid\":");
	return knl_oj_put_num(j, n, max, (D)wdev.device);
#else
	return knl_oj_put(j, n, max, ",\"wifi\":true,\"vid\":720,\"pid\":43455");	/* SDIO 02d0:a9bf */
#endif
}

LOCAL ER w_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	SZ	k, len;

	(void)ctx;
	(void)pos;
	if ( recno != OB_NET_STATE && recno != OB_NET_SCAN ) return E_NOEXS;
	LOCK();
	if ( recno == OB_NET_STATE ) {
		/* the reader's own saved networks only */
		len = state_for(ts_get_pid(), NULL, 0);
		t = ( len > 0 ) ? (UB *)Kmalloc(len) : NULL;
		if ( t != NULL ) (void)state_for(ts_get_pid(), t, len);
		else len = 0;
	} else {
		t = stext;
		len = ( t == NULL ) ? 0 : stlen;
	}
	k = ( off < len ) ? len - off : 0;
	if ( size >= 0 && k > size ) k = size;
	if ( size > 0 && k > 0 ) knl_memcpy(buf, t + off, (INT)k);
	if ( p_asize != NULL ) *p_asize = ( size == 0 ) ? len : k;
	UNLOCK();
	if ( recno == OB_NET_STATE && t != NULL ) Kfree(t);
	return E_OK;
}

LOCAL ER req_pass( CONST TS_UUID *user, CONST char *line );

LOCAL BOOL starts( CONST char *s, CONST char *head )
{
	while ( *head != '\0' && *s == *head ) {
		s++;
		head++;
	}
	return (BOOL)( *head == '\0' );
}

/*
 * A command into record 1: taken by the manager. A change of the saved
 * networks (SAVE, FORGET) is answered once it is done.
 */
LOCAL ER w_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());
	char		line[CMD_MAX];
	INT		i;
	ER		er = E_OK;

	(void)ctx;
	(void)pos;
	if ( recno != OB_NET_STATE ) return E_RONLY;
	if ( size <= 0 || size >= (SZ)sizeof(line) ) return E_PAR;
	for ( i = 0; i < (INT)size && ((CONST UB *)buf)[i] != '\n' && ((CONST UB *)buf)[i] != '\0'; i++ ) {
		line[i] = ((CONST char *)buf)[i];
	}
	line[i] = '\0';
	if ( starts(line, "SAVE ") || starts(line, "FORGET ") ) {
		er = req_pass(&crd->user, line);
		knl_memset(line, 0, sizeof(line));	/* the passphrase is not left about */
	} else {
		LOCK();
		knl_memcpy(cmd, line, sizeof(cmd));
		cmd_user = crd->user;
		UNLOCK();
		(void)tk_set_flg(wflg, F_CMD);
	}
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

LOCAL UD w_size( void *ctx, INT recno )
{
	INT	n;

	(void)ctx;
	if ( recno != OB_NET_STATE ) return ( recno == OB_NET_SCAN ) ? (UD)stlen : 0;
	LOCK();
	n = state_for(ts_get_pid(), NULL, 0);
	UNLOCK();
	return (UD)n;
}

LOCAL CONST T_OBDVOPS w_ops = { OB_S_NET, "wifi", OB_NET_NREC, 0666, 0, 0, w_text, w_attr, w_rea, w_wri, w_size, NULL };

/* ---------------------------------------------------------------- the settings */

#define CONF_POLL_MS	2000
LOCAL UW	conf_sum = 0;		/* of the settings' text as last taken */

/*
 * The settings' record read whole, however long, into a buffer of its
 * own (*p_text, NULL when there is none; the caller clears and Kfrees
 * it): its sum, the length into *p_len. The object is open only for the
 * read.
 */
/*
 * The settings' record whole into *p_text, and a hash of it; 0 when
 * there is no settings object. *p_read is FALSE when the record could
 * not be read now (the object not opened, the read failed): what was
 * read before is then to be kept, not taken as settings with nothing in
 * them.
 */
LOCAL UW conf_text_( UB **p_text, INT *p_len, BOOL *p_read )
{
	TS_UUID	u;
	ID	key;
	INT	len = 0, i;
	UW	h = 2166136261U;
	static ER said = E_OK;
	ER	er = E_OK;

	*p_text = NULL;
	*p_read = TRUE;
	if ( ts_str_to_uuid(SYSDEF_CONF_WIFI, &u) < E_OK ) {
		h = 0;
	} else if ( ( key = ob_opn_obj(&u, OB_OP_READ) ) <= 0 ) {
		if ( key == E_NOEXS ) {
			h = 0;			/* no settings object */
		} else {
			er = ( key < E_OK ) ? (ER)key : E_SYS;
		}
	} else {
		len = knl_ob_rea_all(key, 1, 0, p_text, NULL);
		ob_cls_obj(key);
		if ( len < 0 ) {
			er = (ER)len;
			len = 0;
		}
	}
	if ( er < E_OK ) {
		*p_read = FALSE;
		if ( er != said ) {
			said = er;
			tm_printf((UB *)"wifi: the settings not read now (%d): kept as they were\n", (INT)er);
		}
		if ( p_len != NULL ) *p_len = 0;
		return 0;
	}
	said = E_OK;
	for ( i = 0; i < len; i++ ) h = ( h ^ (*p_text)[i] ) * 16777619U;
	if ( p_len != NULL ) *p_len = len;
	return h;
}

/* The settings' text let go, the passwords in it cleared first */
LOCAL void conf_drop( UB *text, INT len )
{
	if ( text != NULL ) {
		knl_memset(text, 0, len);
		Kfree(text);
	}
}

LOCAL UW conf_text( BOOL *p_read )
{
	UB	*text;
	INT	len = 0;
	UW	h = conf_text_(&text, &len, p_read);

	conf_drop(text, len);
	return h;
}

/* Whether the settings were written since they were taken; FALSE while they cannot be read */
LOCAL BOOL conf_changed( void )
{
	BOOL	rd;
	UW	h = conf_text(&rd);

	return (BOOL)( rd && h != conf_sum );
}

/*
 * The settings object opened to be written: other writers kept out,
 * readers let in (OB_O_WEXCL); a writer met there is waited for, up to a
 * second, as each holds it only while it replaces the record
 */
LOCAL ID conf_wopen( CONST TS_UUID *u )
{
	ID	key;
	INT	i;

	for ( i = 0; ; i++ ) {
		key = ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_WEXCL);
		if ( key != E_BUSY || i >= 100 ) {
			return key;
		}
		tk_dly_tsk(10);
	}
}

/* The network of an SSID a user saved; -1 none */
LOCAL INT net_find( CONST UB *ssid, INT len, CONST TS_UUID *user )
{
	INT	k;

	for ( k = 0; k < nnet; k++ ) {
		if ( nets[k].ssid_len == len && same_bytes(nets[k].ssid, ssid, len)
		  && same_user(&nets[k].owner, user) ) return k;
	}
	return -1;
}

/* The person logged in at the desktop (利用者 while nobody is) */
LOCAL CONST TS_UUID *session_user( void )
{
	return &knl_ob_session_crd()->user;
}

/*
 * The settings read: the networks saved and the key of each.
 * TRUE when they still keep a passphrase ($$WIFIPASS, as settings
 * written before keys were kept): it is made into the key here, and the
 * caller writes the settings again without it.
 */
LOCAL BOOL conf_read( void )
{
	UB	*text;
	char	v[CF_VAL_MAX], w[72];
	INT	len = 0, i, k, all;
	WNET	*nn;
	WNET	*p;
	BOOL	old = FALSE, rd;
	UW	h;

	h = conf_text_(&text, &len, &rd);
	if ( !rd ) {
		conf_drop(text, len);
		return FALSE;			/* not read now: the settings as they were */
	}
	conf_sum = h;
	if ( conf_sum == 0 ) {
		tm_putstring((UB *)"wifi: no settings object (SYSDEF_CONF_WIFI): nothing is joined by itself\n");
	}
	wifi_on = !( cf_get(text, len, "$$WIFI", 0, v, sizeof(v)) && knl_strcmp(v, "off") == 0 );
	/* every network the settings keep: room for as many as there are lines */
	for ( all = 0; cf_get(text, len, "$$WIFINET", all, v, sizeof(v)); all++ ) ;
	nn = (WNET *)Kmalloc(sizeof(WNET) * (SZ)( ( all > 0 ) ? all : 1 ));
	if ( nn == NULL ) {
		conf_drop(text, len);
		tm_putstring((UB *)"wifi: no room for the saved networks: those taken before stay\n");
		return FALSE;
	}
	if ( nets != NULL ) {
		knl_memset(nets, 0, sizeof(WNET) * (SZ)( ( nnet > 0 ) ? nnet : 1 ));	/* the keys are not left about */
		Kfree(nets);
	}
	nets = nn;
	nnet = 0;
	for ( i = 0; i < all && cf_get(text, len, "$$WIFINET", i, v, sizeof(v)); i++ ) {
		p = &nets[nnet];
		knl_memset(p, 0, sizeof(*p));
		if ( !cf_word(v, 0, w, sizeof(w)) ) continue;
		p->ssid_len = unhex(w, p->ssid, 32);
		if ( p->ssid_len <= 0 ) continue;
		p->sec = cf_word(v, 1, w, sizeof(w)) ? saved_of(w) : 0;
		if ( p->sec < 0 ) p->sec = 0;	/* a word of none: as no security, which joins nothing secured */
		p->wpa2 = SEC_PSK(p->sec);
		p->autojoin = !( cf_word(v, 2, w, sizeof(w)) && knl_strcmp(w, "manual") == 0 );
		/* whose: a user; none, or every user's ("all"): the person logged in now */
		if ( !cf_word(v, 3, w, sizeof(w)) || ts_str_to_uuid(w, &p->owner) < E_OK ) {
			p->owner = *session_user();
			old = TRUE;
		}
		nnet++;
	}
	/* the keys: "<n> <64 hexadecimal digits>" */
	for ( i = 0; cf_get(text, len, "$$WIFIPSK", i, v, sizeof(v)); i++ ) {
		k = cf_word(v, 0, w, sizeof(w)) ? cf_num(w, -1) : -1;	/* the number is the first word */
		if ( k < 0 || k >= nnet || !cf_word(v, 1, w, sizeof(w)) ) continue;
		nets[k].keyed = ( unhex(w, nets[k].pmk, 32) == 32 );
		if ( !nets[k].keyed ) knl_memset(nets[k].pmk, 0, sizeof(nets[k].pmk));
	}
	/*
	 * the passwords for SAE: "<n> <the password in hexadecimal>", read
	 * where they stand (up to 126 digits: longer than CF_VAL_MAX)
	 */
	for ( i = 0; i < len; i++ ) {
		CONST UB *val, *wd;
		INT	vl, wl, d;

		if ( !cf_find(text, len, "$$WIFISAE", i, &val, &vl) ) break;
		if ( !cf_wfind(val, vl, 0, &wd, &wl) || wl >= (INT)sizeof(v) ) continue;
		knl_memcpy(v, wd, wl);
		v[wl] = '\0';
		k = cf_num(v, -1);
		if ( k < 0 || k >= nnet || !cf_wfind(val, vl, 1, &wd, &wl) || ( wl & 1 ) || wl > 2 * PW_MAX ) continue;
		for ( d = 0; d < wl; d += 2 ) {
			INT	h = hexval((char)wd[d]), l = hexval((char)wd[d + 1]);

			if ( h < 0 || l < 0 ) break;
			nets[k].pw[d / 2] = (char)( h * 16 + l );
		}
		nets[k].pw_len = ( d == wl && wl / 2 >= 8 ) ? wl / 2 : 0;
		knl_memset(nets[k].pw + nets[k].pw_len, 0, sizeof(nets[k].pw) - (SZ)nets[k].pw_len);
	}
	knl_memset(w, 0, sizeof(w));
	/* the passphrases of settings written before: "<n> <passphrase>", the passphrase being the rest of the line */
	for ( i = 0; cf_get(text, len, "$$WIFIPASS", i, v, sizeof(v)); i++ ) {
		INT	at;

		old = TRUE;
		k = cf_word(v, 0, w, sizeof(w)) ? cf_num(w, -1) : -1;
		for ( at = 0; v[at] != '\0' && v[at] != ' '; at++ ) ;
		if ( v[at] == ' ' ) at++;
		if ( k < 0 || k >= nnet || nets[k].keyed ) continue;
		nets[k].keyed = ( knl_wpa_psk(v + at, (int)knl_strlen(v + at), nets[k].ssid, nets[k].ssid_len,
					      nets[k].pmk) == 0 );
	}
	conf_drop(text, len);			/* the keys are not left about */
	knl_memset(v, 0, sizeof(v));
	/* an SSID of one user saved twice (settings that made one user's of two): one network, with a key */
	for ( k = 1; k < nnet; k++ ) {
		for ( i = 0; i < k && !( nets[i].ssid_len == nets[k].ssid_len && same_user(&nets[i].owner, &nets[k].owner)
					&& same_bytes(nets[i].ssid, nets[k].ssid, nets[k].ssid_len) ); i++ ) ;
		if ( i == k ) continue;
		if ( !nets[i].keyed && nets[k].keyed ) {
			knl_memcpy(nets[i].pmk, nets[k].pmk, sizeof(nets[i].pmk));
			nets[i].keyed = TRUE;
		}
		if ( nets[i].pw_len == 0 && nets[k].pw_len > 0 ) {
			knl_memcpy(nets[i].pw, nets[k].pw, sizeof(nets[i].pw));
			nets[i].pw_len = nets[k].pw_len;
		}
		for ( i = k; i < nnet - 1; i++ ) nets[i] = nets[i + 1];
		nnet--;
		knl_memset(&nets[nnet], 0, sizeof(WNET));
		k--;
		old = TRUE;
	}
	tm_printf((UB *)"wifi: settings: %s, %d saved network%s%s\n", wifi_on ? "on" : "off", nnet,
		  ( nnet == 1 ) ? "" : "s", old ? ", to be written again" : "");
	return old;
}

/* The settings as text: every saved network, then the key of each that has one */
LOCAL INT conf_make( void *ctx, UB *t, INT max )
{
	char	num[16];
	INT	n = 0, k;

	(void)ctx;
	n = knl_oj_put(t, n, max, wifi_on ? "$$WIFI\ton\n" : "$$WIFI\toff\n");
	for ( k = 0; k < nnet; k++ ) {
		n = knl_oj_put(t, n, max, "$$WIFINET\t");
		n = put_hex(t, n, max, nets[k].ssid, nets[k].ssid_len);
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put(t, n, max, saved_name(nets[k].sec));
		n = knl_oj_put(t, n, max, nets[k].autojoin ? " auto" : " manual");
		n = put_user(t, n, max, &nets[k].owner);
		n = knl_oj_put(t, n, max, "\n");
	}
	for ( k = 0; k < nnet; k++ ) {
		if ( !SEC_PSK(nets[k].sec) || !nets[k].keyed ) continue;
		(void)cf_itoa(k, num);
		n = knl_oj_put(t, n, max, "$$WIFIPSK\t");
		n = knl_oj_put(t, n, max, num);
		n = knl_oj_put(t, n, max, " ");
		n = put_hex(t, n, max, nets[k].pmk, 32);
		n = knl_oj_put(t, n, max, "\n");
	}
	for ( k = 0; k < nnet; k++ ) {
		if ( !SEC_SAE(nets[k].sec) || nets[k].pw_len == 0 ) continue;
		(void)cf_itoa(k, num);
		n = knl_oj_put(t, n, max, "$$WIFISAE\t");
		n = knl_oj_put(t, n, max, num);
		n = knl_oj_put(t, n, max, " ");
		n = put_hex(t, n, max, (CONST UB *)nets[k].pw, nets[k].pw_len);
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

/* The saved networks written to the settings, in the kernel's task: no program writes them */
LOCAL ER conf_write( void )
{
	TS_UUID	u;
	ID	key;
	UB	*t = NULL;
	INT	n;
	SZ	asz = 0;
	ER	er;

	n = knl_oj_make(conf_make, NULL, &t);
	if ( n < 0 ) {
		return E_NOMEM;
	}
	if ( ts_str_to_uuid(SYSDEF_CONF_WIFI, &u) < E_OK ) {
		er = E_SYS;
	} else if ( ( key = conf_wopen(&u) ) <= 0 ) {
		er = ( key < E_OK ) ? (ER)key : E_SYS;
	} else {
		/* replaced in one change: a reader, let in meanwhile, sees the old record or the new one */
		er = ob_rpl_rec(key, 1, t, (SZ)n);
		(void)asz;
		ob_cls_obj(key);
	}
	if ( t != NULL ) {
		knl_memset(t, 0, n);
		Kfree(t);
	}
	tm_printf((UB *)"wifi: settings written: %d saved network%s (%d)\n", nnet, ( nnet == 1 ) ? "" : "s", er);
	return er;
}

/*
 * The settings read again, the network being joined and the one asked
 * for kept by what they are rather than by where they stood; those of
 * settings written before are written again with keys
 */
LOCAL void conf_take( void )
{
	WNET	cur, want;
	BOOL	hc = ( cur_net >= 0 && cur_net < nnet ), hw = ( want_net >= 0 && want_net < nnet );

	if ( hc ) cur = nets[cur_net];
	if ( hw ) want = nets[want_net];
	if ( conf_read() && conf_write() >= E_OK ) (void)conf_read();
	cur_net = hc ? net_find(cur.ssid, cur.ssid_len, &cur.owner) : -1;
	want_net = hw ? net_find(want.ssid, want.ssid_len, &want.owner) : -1;
	knl_memset(&cur, 0, sizeof(cur));
	knl_memset(&want, 0, sizeof(want));
}

/*
 * A change of a user's saved networks done (SAVE, FORGET; w_wri says
 * what they are): the settings read as they are now, changed, and
 * written. For WPA2 the passphrase is made into the key here and kept
 * nowhere; for WPA3 the password is kept, as SAE needs it (8 to 63
 * printable characters: the same for both, so that WPA2/WPA3 has one).
 */
LOCAL ER req_apply( CONST TS_UUID *user, CONST char *line )
{
	char	w[72], v[16];
	UB	ssid[32], pmk[32];
	INT	len, k, i, at;
	BOOL	save = starts(line, "SAVE "), autoj = TRUE, made = FALSE;
	INT	sec = 0, pl = 0;
	CONST char *pass = NULL;
	WNET	*p;
	ER	er = E_OK;

	if ( !cf_word(line, 1, w, sizeof(w)) || ( len = unhex(w, ssid, 32) ) <= 0 ) return E_PAR;
	if ( save ) {
		if ( !cf_word(line, 2, v, sizeof(v)) ) return E_PAR;
		if ( ( sec = saved_of(v) ) < 0 ) return E_PAR;
		if ( !cf_word(line, 3, v, sizeof(v)) ) return E_PAR;
		autoj = ( knl_strcmp(v, "manual") != 0 );
		/* the passphrase: what follows the fourth word, blanks and all */
		for ( at = 0, i = 0; i < 4 && line[at] != '\0'; i++ ) {
			while ( line[at] != '\0' && line[at] != ' ' ) at++;
			if ( line[at] == ' ' ) at++;
		}
		if ( line[at] != '\0' ) pass = line + at;
		if ( pass != NULL && SEC_SAE(sec) ) {
			/* a password, which SAE takes as it is: not 64 hexadecimal digits of a PSK */
			for ( pl = 0; pass[pl] >= 0x20 && pass[pl] <= 0x7e; pl++ ) ;
			if ( pass[pl] != '\0' || pl < 8 || pl > PW_MAX ) return E_PAR;
		}
	}
	conf_take();				/* the networks as they are saved now */
	k = net_find(ssid, len, user);
	if ( !save ) {
		if ( k < 0 ) return E_NOEXS;
		knl_memset(&nets[k], 0, sizeof(WNET));
		for ( i = k; i < nnet - 1; i++ ) nets[i] = nets[i + 1];
		nnet--;
		knl_memset(&nets[nnet], 0, sizeof(WNET));
		if ( cur_net == k ) cur_net = -1;
		else if ( cur_net > k ) cur_net--;
		if ( want_net == k ) want_net = -1;
		else if ( want_net > k ) want_net--;
	} else {
		if ( k < 0 ) {
			WNET	*nn = (WNET *)Kmalloc(sizeof(WNET) * (SZ)( nnet + 1 ));

			if ( nn == NULL ) return E_NOMEM;
			if ( nets != NULL ) {
				knl_memcpy(nn, nets, sizeof(WNET) * (SZ)nnet);
				knl_memset(nets, 0, sizeof(WNET) * (SZ)( ( nnet > 0 ) ? nnet : 1 ));
				Kfree(nets);
			}
			nets = nn;
			k = nnet++;
			knl_memset(&nets[k], 0, sizeof(WNET));
			knl_memcpy(nets[k].ssid, ssid, len);
			nets[k].ssid_len = len;
			nets[k].owner = *user;
			made = TRUE;
		}
		p = &nets[k];
		p->sec = sec;
		p->wpa2 = SEC_PSK(sec);
		p->autojoin = autoj;
		p->failed_at = 0;
		p->fails = 0;
		/* the PMK of WPA2-Personal */
		if ( !SEC_PSK(sec) ) {
			p->keyed = FALSE;
			knl_memset(p->pmk, 0, sizeof(p->pmk));
		} else if ( pass != NULL ) {
			if ( knl_wpa_psk(pass, (int)knl_strlen(pass), ssid, len, pmk) != 0 ) {
				er = E_PAR;		/* not 8 to 63 letters, nor 64 hexadecimal digits */
			} else {
				knl_memcpy(p->pmk, pmk, sizeof(pmk));
				p->keyed = TRUE;
			}
			knl_memset(pmk, 0, sizeof(pmk));
		} else if ( !p->keyed ) {
			er = E_NOEXS;			/* no passphrase, and no key saved */
		}
		/* the password of SAE */
		if ( !SEC_SAE(sec) ) {
			p->pw_len = 0;
			knl_memset(p->pw, 0, sizeof(p->pw));
		} else if ( pass != NULL ) {
			knl_memset(p->pw, 0, sizeof(p->pw));
			knl_memcpy(p->pw, pass, pl);
			p->pw_len = pl;
		} else if ( p->pw_len == 0 ) {
			er = E_NOEXS;			/* no password, and none saved */
		}
	}
	if ( er >= E_OK ) er = conf_write();
	tm_printf((UB *)"wifi: %s a network%s: %d%s%s%s\n", save ? ( made ? "saved" : "changed" ) : "forgot",
		  ( save && sec != 0 ) ? ( ( pass != NULL ) ? ", a new key" : ", the key kept" ) : "", er,
		  save ? " (" : "", save ? saved_name(sec) : "", save ? ")" : "");
	conf_take();				/* as written; as it was when it could not be */
	return er;
}

/*
 * A change of a user's saved networks, done by the manager, the caller
 * waiting for it; with no manager (a machine with no card) the kernel's
 * own tasks do it themselves.
 */
LOCAL ER req_pass( CONST TS_UUID *user, CONST char *line )
{
	ER	er;

	if ( !w_serving ) {
		if ( ts_get_pid() > 0 ) return E_NOEXS;
		return req_apply(user, line);
	}
	if ( tk_wai_sem(rq_free, 1, 10000) < E_OK ) return E_BUSY;
	knl_strncpy(rq_line, line, sizeof(rq_line) - 1);
	rq_line[sizeof(rq_line) - 1] = '\0';
	rq_user = *user;
	rq_er = E_TMOUT;
	rq_pending = TRUE;
	(void)tk_set_flg(wflg, F_REQ);
	er = tk_wai_sem(rq_done, 1, 30000);
	er = ( er < E_OK ) ? E_TMOUT : rq_er;
	(void)tk_sig_sem(rq_free, 1);
	return er;
}

/* The change asked, done by the manager */
LOCAL void take_req( void )
{
	if ( !rq_pending ) return;
	rq_pending = FALSE;
	rq_er = req_apply(&rq_user, rq_line);
	knl_memset(rq_line, 0, sizeof(rq_line));
	if ( ws == WS_IDLE ) t_scan = 0;	/* a network saved now is looked for at once */
	state_refresh();
	(void)tk_sig_sem(rq_done, 1);
}

/*
 * Whether the manager joins network k by itself now: one of the person
 * logged in. When the person changes, the network joined is left unless
 * it is the new one's.
 */
LOCAL BOOL net_auto( INT k )
{
	return k >= 0 && k < nnet && same_user(&nets[k].owner, session_user());
}

/*
 * What is saved of network k for a join (the joins of the x64 PC and of
 * the Raspberry Pi 5 take their keys here alone). Answers its security,
 * WPAG_SAVED_* (0 none). pmk (32 bytes) gets the PMK of WPA2-Personal,
 * *have_pmk whether there is one (WPAG_SAVED_WPA2, WPA2WPA3). pw (room
 * for PW_MAX + 1) gets the password for SAE with its nought, *pw_len its
 * length, 0 none (WPA3, WPA2WPA3): hunting and pecking's, and what a
 * card's own supplicant is given. pt, when not NULL (room for
 * WPAG_SAE_PT_MAX), gets H2E's password element made from the SSID and
 * the password (knl_wpa_sae_pt, made at each call: not kept), *pt_len
 * its length, 0 none. The caller clears all of them once used.
 */
LOCAL INT net_secret( INT k, UB *pmk, BOOL *have_pmk, UB *pt, INT *pt_len, char *pw, INT *pw_len )
{
	WNET	*p;
	int	n = 0;

	*have_pmk = FALSE;
	*pw_len = 0;
	if ( pt_len != NULL ) *pt_len = 0;
	if ( k < 0 || k >= nnet ) return 0;
	p = &nets[k];
	if ( SEC_PSK(p->sec) && p->keyed ) {
		knl_memcpy(pmk, p->pmk, 32);
		*have_pmk = TRUE;
	}
	if ( SEC_SAE(p->sec) && p->pw_len > 0 ) {
		knl_memcpy(pw, p->pw, p->pw_len);
		pw[p->pw_len] = '\0';
		*pw_len = p->pw_len;
		if ( pt != NULL && pt_len != NULL
		  && knl_wpa_sae_pt(p->ssid, p->ssid_len, p->pw, p->pw_len, pt, &n) == 0 ) *pt_len = n;
	}
	return p->sec;
}

LOCAL void fail( CONST char *why );

/* Another person logged in: the network of the one before left, the new one's looked for */
LOCAL void user_take( void )
{
	if ( same_user(&cur_user, session_user()) ) return;
	cur_user = *session_user();
	tm_putstring((UB *)"wifi: another person logged in: their networks are looked for\n");
	want_net = -1;
	held = FALSE;
	if ( ws >= WS_JOINING && !net_auto(cur_net) ) fail("asked");
	if ( ws == WS_IDLE ) t_scan = 0;
	state_refresh();
}

/*
 * For the kernel's tests (KTEST): a change of a user's saved networks,
 * as that user's write of record 1 asks it; the key a join of the SSID
 * for the user would use (E_NOEXS none); record 1 as the user's process
 * reads it (Kfree *p_text); whether the network of the SSID that user
 * saved is one the manager joins by itself now, for the person logged
 * in (and so is kept when the person changes).
 */
EXPORT ER knl_wifi_request( CONST T_OBCRD *crd, CONST char *line )
{
	return req_pass(&crd->user, line);
}

EXPORT BOOL knl_wifi_auto_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len )
{
	if ( !w_serving ) conf_take();
	return net_auto(net_find(ssid, len, &crd->user));
}

EXPORT ER knl_wifi_key_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len, UB *pmk )
{
	INT	k;

	if ( !w_serving ) conf_take();
	k = net_find(ssid, len, &crd->user);
	if ( k < 0 || !nets[k].keyed ) return E_NOEXS;
	knl_memcpy(pmk, nets[k].pmk, 32);
	return E_OK;
}

/* What a join of the SSID for the user would be given (net_secret, with H2E's element): E_NOEXS none */
EXPORT INT knl_wifi_secret_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len, UB *pmk, BOOL *have_pmk,
				UB *pt, INT *pt_len, char *pw, INT *pw_len )
{
	INT	k;

	if ( !w_serving ) conf_take();
	k = net_find(ssid, len, &crd->user);
	if ( k < 0 ) return E_NOEXS;
	return net_secret(k, pmk, have_pmk, pt, pt_len, pw, pw_len);
}

EXPORT INT knl_wifi_state_text( CONST T_OBCRD *crd, UB **p_text )
{
	UB	*all = NULL, *t = NULL;
	char	us[40];
	INT	n, i = 0, e, m = 0, ul;

	*p_text = NULL;
	if ( !w_serving ) conf_take();
	n = knl_oj_make(state_make, NULL, &all);
	if ( n < 0 || ( t = (UB *)Kmalloc(n + 1) ) == NULL || ts_uuid_to_str(&crd->user, us, sizeof(us)) < E_OK ) {
		if ( all != NULL ) Kfree(all);
		if ( t != NULL ) Kfree(t);
		return -1;
	}
	/* as state_for shows it to a process of that user */
	ul = (INT)knl_strlen(us);
	while ( i < n ) {
		for ( e = i; e < n && all[e] != '\n'; e++ ) ;
		if ( e - i > 6 && same_bytes(all + i, (CONST UB *)"SAVED ", 6) ) {
			if ( e - i > ul + 1 && same_bytes(all + e - ul, (CONST UB *)us, ul) ) {
				knl_memcpy(t + m, all + i, e - ul - 1 - i);
				m += e - ul - 1 - i;
				t[m++] = '\n';
			}
		} else {
			knl_memcpy(t + m, all + i, ( e < n ) ? e + 1 - i : e - i);
			m += ( e < n ) ? e + 1 - i : e - i;
		}
		i = e + 1;
	}
	Kfree(all);
	*p_text = t;
	return m;
}

/* ---------------------------------------------------------------- scanning */

/* The scan's networks, the strongest first, into record 2 and onto the console */
/* Record 2: a line for each network the scan found, however many */
LOCAL INT scan_make( void *ctx, UB *t, INT max )
{
	INT	i, n = 0;

	(void)ctx;
	for ( i = 0; i < nbss && n >= 0; i++ ) {
		n = knl_oj_put(t, n, max, "BSS ");
		n = put_mac(t, n, max, bss[i].bssid);
		n = knl_oj_put(t, n, max, " ");
		n = put_num(t, n, max, bss[i].freq);
		n = knl_oj_put(t, n, max, " ");
		n = put_num(t, n, max, dbm(bss[i].rssi));
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put(t, n, max, sec_name(sec_of(&bss[i])));
		n = knl_oj_put(t, n, max, " ");
		n = put_hex(t, n, max, bss[i].ssid, bss[i].ssid_len);
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

LOCAL void scan_take( void )
{
	TSWIFI_BSS	t;
	UB		*st2, *old;
	INT		i, k, n;
	char		s[33];
	BOOL		say = ( nscans < 4 );

	/*
	 * every network the scan found: the answer counts those that did not
	 * fit too, and the room is made that large and the list asked again
	 */
	if ( bss == NULL ) {
		bss = (TSWIFI_BSS *)Kmalloc(sizeof(TSWIFI_BSS) * W_NBSS);
		bss_cap = ( bss != NULL ) ? W_NBSS : 0;
	}
	n = ( bss != NULL ) ? rump_tswifi_results(bss, bss_cap) : 0;
	while ( n > bss_cap ) {
		TSWIFI_BSS	*nb = (TSWIFI_BSS *)Kmalloc(sizeof(TSWIFI_BSS) * (SZ)n);

		if ( nb == NULL ) {
			n = bss_cap;		/* those that fit: the console says no more room */
			tm_putstring((UB *)"wifi: no room for every network the scan found\n");
			break;
		}
		Kfree(bss);
		bss = nb;
		bss_cap = n;
		n = rump_tswifi_results(bss, bss_cap);
	}
	nbss = ( n > 0 ) ? n : 0;
	/* an entry with no BSSID (all zero) is no network: net80211's node of its own, not an access point */
	for ( i = 0, k = 0; i < nbss; i++ ) {
		static CONST UB	zero[6] = { 0, 0, 0, 0, 0, 0 };

		if ( same_bytes(bss[i].bssid, zero, 6) ) continue;
		if ( k != i ) bss[k] = bss[i];
		k++;
	}
	nbss = k;
	for ( i = 1; i < nbss; i++ ) {
		for ( k = i; k > 0 && dbm(bss[k].rssi) > dbm(bss[k - 1].rssi); k-- ) {
			t = bss[k];
			bss[k] = bss[k - 1];
			bss[k - 1] = t;
		}
	}
	nscans++;
	tm_printf((UB *)"wifi: scan %d: %d network%s\n", nscans, nbss, ( nbss == 1 ) ? "" : "s");
	k = knl_oj_make(scan_make, NULL, &st2);
	if ( k >= 0 ) {
		LOCK();
		old = stext;
		stext = st2;
		stlen = k;
		UNLOCK();
		if ( old != NULL ) Kfree(old);
	}
	if ( say ) {
		for ( i = 0; i < nbss && i < 20; i++ ) {
			ssid_str(bss[i].ssid, bss[i].ssid_len, s);
			tm_printf((UB *)"wifi:   %02x:%02x:%02x:%02x:%02x:%02x %4d MHz %4d dBm %-5s \"%s\"\n",
				  bss[i].bssid[0], bss[i].bssid[1], bss[i].bssid[2], bss[i].bssid[3],
				  bss[i].bssid[4], bss[i].bssid[5], bss[i].freq, dbm(bss[i].rssi),
				  sec_name(sec_of(&bss[i])), s);
		}
	}
	if ( wobj ) knl_obdev_post(&wuuid, OB_NET_SCAN, OB_E_CHANGE, NULL);
}

/*
 * A scan begun. The card is first put back to INIT when it is not there
 * (tswifi_if.c, EAGAIN): the scan is then asked again from the idle
 * state in a moment.
 */
LOCAL void scan_start( void )
{
	INT	er = rump_tswifi_scan();

	t_scan = now_ms();
	scan_again = FALSE;
	if ( er == 35 ) {			/* EAGAIN */
		scan_again = TRUE;
		ws = WS_IDLE;
		return;
	}
	if ( er != 0 ) {
		tm_printf((UB *)"wifi: the scan was not started (%d)\n", er);
		ws = WS_IDLE;
		return;
	}
	ws = WS_SCANNING;
	t_state = t_scan;
}

/* ---------------------------------------------------------------- joining */

LOCAL CONST UB *find_ie( CONST UB *ie, INT len, UB id )
{
	INT	i = 0;

	while ( i + 2 <= len && i + 2 + ie[i + 1] <= len ) {
		if ( ie[i] == id ) return ie + i;
		i += 2 + ie[i + 1];
	}
	return NULL;
}

#ifdef WIFI_OBSD
/* ---------------------------------------------------------------- joining with OpenBSD's layer */

#define OB_JOIN_MS	20000		/* the layer's scan, authentication and association */
#define OB_LOST_MS	30000		/* a network lost, looked for again by the layer */
#define OB_SAE_MS	1000		/* an SAE frame unanswered: sent again */

LOCAL void	shake_begin( UD t );
LOCAL void	now_connected( CONST char *how );
LOCAL void	card_restart( UD t, BOOL down );
LOCAL INT	choose( TSWIFI_BSS *out );

LOCAL UW	ob_scans = 0;		/* the layer's count of ended scans, as last seen */
LOCAL UW	ob_roams = 0;
LOCAL INT	ob_saved = 0;		/* WPAG_SAVED_* of the network being joined, 0 open */
LOCAL INT	ob_akm = 0, ob_pmf = 0, ob_h2e = 0;
LOCAL BOOL	ob_sae = FALSE;		/* SAE going on with the access point */
LOCAL UD	ob_t_sae = 0;
LOCAL UB	ob_rsn[TSW_IE_RSN_MAX], ob_rsnxe[TSW_IE_RSN_MAX];
LOCAL INT	ob_rsn_len = 0, ob_rsnxe_len = 0;
LOCAL BOOL	ob_lost = FALSE;	/* the layer looks again for the network it lost */
LOCAL void	net_auth_failed( INT k, CONST char *why );
LOCAL void	net_clear_temp_disabled( INT k, BOOL clear_failures );

/*
 * The connection lost in the first 4-way handshake of a join with a
 * PSK (the layer left the access point, or began to authenticate again,
 * while ws is WS_HANDSHAKE): taken as a wrong key and the network left
 * a while, as wpa_supplicant does (events.c's could_be_psk_mismatch,
 * wpa_supplicant.c's wpas_auth_failed). The layer, which would join it
 * again at once, is made to leave it (fail), and the network is not
 * chosen until that time is over (choose). The leave counts no failure
 * of its own (failed_at): the time is wpa_supplicant's alone. TRUE when
 * it was taken so; not for SAE, as wpa_supplicant's check
 */
LOCAL BOOL ob_wrong_key( void )
{
	INT	k = cur_net;
	UD	was_at;
	INT	was_fails;

	if ( cur_akm == WPAG_AKM_SAE || k < 0 || k >= nnet ) return FALSE;
	was_at = nets[k].failed_at;
	was_fails = nets[k].fails;
	tm_putstring((UB *)"wifi: WPA: 4-Way Handshake failed - pre-shared key may be incorrect\n");
	net_auth_failed(k, "WRONG_KEY");
	fail("password");
	nets[k].failed_at = was_at;
	nets[k].fails = was_fails;
	return TRUE;
}

/*
 * What is saved of network k for a join: its security (WPAG_SAVED_*, 0
 * none), its PMK (WPA2), and SAE's password or its element (WPA3)
 */
LOCAL INT ob_secret( INT k, UB *pmk, BOOL *have_pmk, UB *pt, INT *pt_len, char *pw, INT *pw_len )
{
	return net_secret(k, pmk, have_pmk, pt, pt_len, pw, pw_len);
}

LOCAL INT ob_akms( INT saved )
{
	return ( saved == WPAG_SAVED_WPA2 ) ? ( TSW_AKM_PSK | TSW_AKM_PSK_SHA256 )
	     : ( saved == WPAG_SAVED_WPA3 ) ? TSW_AKM_SAE
	     : ( TSW_AKM_PSK | TSW_AKM_PSK_SHA256 | TSW_AKM_SAE );
}

/*
 * The saved network k asked of the layer: its SSID, its security and
 * key. b is where a scan found it; the layer chooses the access point of
 * it itself (the strongest, 5 GHz before 2.4 GHz) and says which before
 * it authenticates (ob_auth)
 */
LOCAL BOOL ob_join( INT k, CONST TSWIFI_BSS *b )
{
	UB	pmk[32];
	char	pw[64], s[33];
	INT	er, c = sec_of(b), pt_len, pw_len;
	BOOL	have_pmk;

	ssid_str(b->ssid, b->ssid_len, s);
	cur_net = k;
	cur_bss = *b;
	reason = NULL;
	eapol_pending = FALSE;
	ob_sae = FALSE;
	ob_lost = FALSE;
	knl_wpa_disassoc();
	knl_wpa_sae_abort();
	cur_akm = 0;
	ob_saved = ob_secret(k, pmk, &have_pmk, NULL, &pt_len, pw, &pw_len);
	knl_memset(pw, 0, sizeof(pw));
	if ( ob_saved == 0 ) {
		if ( c != WPAG_OPEN ) {
			tm_printf((UB *)"wifi: \"%s\" is saved with no security but offers %s\n", s, sec_name(c));
			reason = "unsupported";
			return FALSE;
		}
		er = rump_tswifi_join(b->ssid, b->ssid_len, 0, 0, 0, NULL);
	} else {
		if ( c == WPAG_OPEN || c == WPAG_WEP || c == WPAG_WPA || c == WPAG_8021X ) {
			tm_printf((UB *)"wifi: \"%s\" offers %s, not WPA2 or WPA3-Personal\n", s, sec_name(c));
			reason = "unsupported";
			return FALSE;
		}
		if ( ( ob_saved == WPAG_SAVED_WPA2 && !have_pmk ) ||
		     ( ob_saved == WPAG_SAVED_WPA3 && pw_len <= 0 ) ||
		     ( ob_saved == WPAG_SAVED_WPA2WPA3 && !have_pmk && pw_len <= 0 ) ) {
			tm_printf((UB *)"wifi: \"%s\": no key is saved for it\n", s);
			reason = "password";
			return FALSE;
		}
		er = rump_tswifi_join(b->ssid, b->ssid_len, 1, ob_akms(ob_saved), ob_saved == WPAG_SAVED_WPA3,
				      have_pmk ? pmk : NULL);
	}
	knl_memset(pmk, 0, sizeof(pmk));
	cur_wpa2 = ( ob_saved != 0 );
	rump_tswifi_debug(1, DBG_JOIN);
	tm_printf((UB *)"wifi: joining \"%s\" (%s; the layer chooses its access point, the strongest seen "
		  "%02x:%02x:%02x:%02x:%02x:%02x %d MHz %d dBm): %s (%d)\n",
		  s, ( ob_saved == WPAG_SAVED_WPA3 ) ? "WPA3-Personal" : ( ob_saved == WPAG_SAVED_WPA2WPA3 )
		  ? "WPA2 or WPA3-Personal" : ( ob_saved != 0 ) ? "WPA2-Personal" : "no security",
		  b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
		  b->freq, dbm(b->rssi), ( er == 0 ) ? "asked" : "refused", er);
	if ( er != 0 ) {
		reason = "refused";
		return FALSE;
	}
	ws = WS_JOINING;
	t_state = now_ms();
	last_s = -1;
	return TRUE;
}

/* An access point told for the console: "aa:bb:...:ff" */
LOCAL void ob_mac( CONST UB *m, char *out )
{
	static CONST char hex[] = "0123456789abcdef";
	INT	i;

	for ( i = 0; i < 6; i++ ) {
		out[i * 3] = hex[m[i] >> 4];
		out[i * 3 + 1] = hex[m[i] & 15];
		out[i * 3 + 2] = ( i < 5 ) ? ':' : '\0';
	}
}

/*
 * The layer chose an access point of the network asked for and is about
 * to authenticate with it -- the first time, after the network was lost,
 * or moving to a better one while joined (a roam): its security decided
 * (knl_wpa_choose), our RSN element made for the association request,
 * and the open authentication let go, or SAE begun
 */
LOCAL void ob_auth( CONST UB *bssid )
{
	UB	pmk[32], pt[WPAG_SAE_PT_MAX], ie[WPAG_IE_MAX], out[WPAG_SAE_MAX];
	char	pw[64], a[18], o[18];
	INT	saved, pt_len, pw_len, ie_len = 0, out_len = 0, status = 0, er;
	BOOL	have_pmk, was = ( ws == WS_CONNECTED || ws == WS_HANDSHAKE || ob_lost );

	if ( cur_net < 0 || cur_net >= nnet || ws < WS_JOINING ) {
		(void)rump_tswifi_auth_fail();
		return;
	}
	/* the layer authenticating again in the middle of the first 4-way handshake: the access point sent it away */
	if ( ws == WS_HANDSHAKE && ob_wrong_key() ) {
		(void)rump_tswifi_auth_fail();
		return;
	}
	(void)rump_tswifi_status(&st);
	ob_mac(bssid, a);
	if ( was && !same_bytes(bssid, cur_bss.bssid, 6) ) {
		ob_mac(cur_bss.bssid, o);
		tm_printf((UB *)"wifi: roam: %s -> %s, %d dBm, %d MHz\n", o, a, dbm(st.rssi), st.freq);
		ob_roams++;
	}
	if ( ws != WS_JOINING ) {
		/* joined before (a roam, or the network found again): a new join and handshake */
		knl_wpa_disassoc();
		eapol_pending = FALSE;
		ws = WS_JOINING;
		t_state = now_ms();
	}
	knl_memcpy(cur_bss.bssid, bssid, 6);
	if ( st.freq != 0 ) cur_bss.freq = (UH)st.freq;
	cur_bss.rssi = st.rssi;
	knl_wpa_sae_abort();
	ob_sae = FALSE;
	ob_rsn_len = rump_tswifi_bss_ie(bssid, 48, ob_rsn, sizeof(ob_rsn));
	ob_rsnxe_len = rump_tswifi_bss_ie(bssid, 244, ob_rsnxe, sizeof(ob_rsnxe));
	saved = ob_secret(cur_net, pmk, &have_pmk, pt, &pt_len, pw, &pw_len);
	er = knl_wpa_choose(saved, ob_rsn_len ? ob_rsn : NULL, ob_rsn_len, ob_rsnxe_len ? ob_rsnxe : NULL,
			    ob_rsnxe_len, 1, pt_len > 0, &ob_akm, &ob_pmf, &ob_h2e);
	if ( er != 0 ) {
		tm_printf((UB *)"wifi: %s has nothing in common with the network as saved: another is looked for\n", a);
		(void)rump_tswifi_auth_fail();
	} else if ( ob_akm == WPAG_AKM_SAE ) {
		cur_akm = WPAG_AKM_SAE;
		er = knl_wpa_sae_start(wmac, bssid, ob_h2e, pw, pw_len, pt, pt_len, out, &out_len, &status);
		if ( er == 0 ) er = rump_tswifi_auth_send(1, status, out, out_len);
		tm_printf((UB *)"wifi: authenticating with %s: SAE%s, commit sent: %s (%d)\n", a,
			  ob_h2e ? " (H2E)" : " (hunting and pecking)", ( er == 0 ) ? "ok" : "refused", er);
		if ( er == 0 ) {
			ob_sae = TRUE;
			ob_t_sae = now_ms();
		} else {
			knl_wpa_sae_abort();
			(void)rump_tswifi_auth_fail();
		}
	} else {
		cur_akm = ob_akm;
		er = knl_wpa_start2(wmac, bssid, cur_bss.ssid, cur_bss.ssid_len, ob_rsn_len ? ob_rsn : NULL, ob_rsn_len,
				    ob_rsnxe_len ? ob_rsnxe : NULL, ob_rsnxe_len, ob_akm, ob_pmf, pmk, NULL, 0,
				    ie, &ie_len);
		if ( er == 0 ) er = rump_tswifi_optie(ie, ie_len);
		if ( er == 0 ) er = rump_tswifi_auth_go();
		tm_printf((UB *)"wifi: authenticating with %s: %s, PMF %s: %s (%d)\n", a,
			  ( ob_akm == WPAG_AKM_PSK_SHA256 ) ? "PSK-SHA256" : "PSK",
			  ( ob_pmf == WPAG_PMF_REQUIRED ) ? "required" : ( ob_pmf == WPAG_PMF_OPTIONAL ) ? "if the access point can" : "off",
			  ( er == 0 ) ? "open authentication sent" : "refused", er);
		if ( er != 0 ) (void)rump_tswifi_auth_fail();
	}
	knl_memset(pmk, 0, sizeof(pmk));
	knl_memset(pt, 0, sizeof(pt));
	knl_memset(pw, 0, sizeof(pw));
}

/* SAE's next step with the frame the access point sent, or with none (sent again in time) */
LOCAL void ob_sae_step( CONST TSWIFI_EV *ev )
{
	UB	out[WPAG_SAE_MAX], pmk[32], pmkid[16], ie[WPAG_IE_MAX];
	INT	r, out_len = 0, seq = 0, status = 0, ie_len = 0, er;

	if ( !ob_sae ) return;
	if ( ev != NULL ) {
		r = knl_wpa_sae_rx(ev->seq, ev->status, ev->body, ev->len, out, &out_len, &seq, &status);
		tm_printf((UB *)"wifi: SAE in: %s, status %d, %d bytes: %s\n", ( ev->seq == 1 ) ? "commit" : "confirm",
			  ev->status, ev->len, ( r == WPAG_SAE_SEND ) ? "answered" : ( r == WPAG_SAE_DONE ) ? "accepted"
			  : ( r == WPAG_SAE_WAIT ) ? "waiting" : "given up");
	} else {
		r = knl_wpa_sae_resend(out, &out_len, &seq, &status);
		tm_printf((UB *)"wifi: SAE: no answer in %d ms: %s\n", OB_SAE_MS,
			  ( r == WPAG_SAE_SEND ) ? "sent again" : "given up");
	}
	if ( r == WPAG_SAE_SEND ) {
		(void)rump_tswifi_auth_send(seq, status, out, out_len);
		ob_t_sae = now_ms();
	} else if ( r == WPAG_SAE_DONE ) {
		ob_sae = FALSE;
		er = knl_wpa_sae_pmk(pmk, pmkid);
		if ( er == 0 ) {
			er = knl_wpa_start2(wmac, cur_bss.bssid, cur_bss.ssid, cur_bss.ssid_len,
					    ob_rsn_len ? ob_rsn : NULL, ob_rsn_len, ob_rsnxe_len ? ob_rsnxe : NULL,
					    ob_rsnxe_len, WPAG_AKM_SAE, WPAG_PMF_REQUIRED, pmk, pmkid, ob_h2e,
					    ie, &ie_len);
		}
		knl_memset(pmk, 0, sizeof(pmk));
		if ( er == 0 ) er = rump_tswifi_optie(ie, ie_len);
		if ( er == 0 ) er = rump_tswifi_auth_done();
		tm_printf((UB *)"wifi: SAE done: associating: %s (%d)\n", ( er == 0 ) ? "asked" : "refused", er);
		if ( er != 0 ) (void)rump_tswifi_auth_fail();
	} else if ( r == WPAG_SAE_FAIL ) {
		ob_sae = FALSE;
		knl_wpa_sae_abort();
		reason = "password";
		(void)rump_tswifi_auth_fail();
	}
}

/* What the layer left for the manager */
LOCAL void ob_events( void )
{
	static TSWIFI_EV	ev;		/* large: not on the stack */

	while ( rump_tswifi_event(&ev) ) {
		if ( ev.type == TSW_EV_AUTH ) ob_auth(ev.bssid);
		else if ( ev.type == TSW_EV_FRAME && same_bytes(ev.bssid, cur_bss.bssid, 6) ) ob_sae_step(&ev);
	}
	if ( ob_sae && now_ms() - ob_t_sae > OB_SAE_MS ) ob_sae_step(NULL);
}

/* The rate as the console says it: "433 Mb/s (802.11ac MCS 9, 1 stream, 80 MHz, short GI)" */
LOCAL void ob_rate( char *out, INT max )
{
	INT	n;

	(void)max;
	if ( st.phy >= 2 ) {
		n = (INT)tm_sprintf((UB *)out, (UB *)"%d Mb/s (802.11%s MCS %d, %d stream%s, %d MHz%s)", st.rate / 2,
				    ( st.phy == 3 ) ? "ac" : "n", st.mcs, st.nss, ( st.nss == 1 ) ? "" : "s", st.width,
				    st.sgi ? ", short GI" : "");
	} else {
		n = (INT)tm_sprintf((UB *)out, (UB *)"%d Mb/s (802.11a/b/g)", st.rate / 2);
	}
	(void)n;
}

/* One look at the card and one step of the state, with OpenBSD's layer */
LOCAL void step_obsd( void )
{
	UD	t = now_ms();
	TSWIFI_BSS	b;
	INT		k;
	char		r[96];
	static CONST char *CONST sname[] = { "init", "scan", "authenticating", "associating", "run" };

	if ( ws == WS_JOINING || ws == WS_HANDSHAKE ) rump_tswifi_trace();

	(void)rump_tswifi_status(&st);
	if ( st.state != last_s && ws >= WS_JOINING && st.state >= S_INIT && st.state <= S_RUN ) {
		tm_printf((UB *)"wifi: net80211: %s\n", sname[st.state]);
	}
	last_s = st.state;
	if ( ws >= WS_IDLE ) {
		BOOL	down = !( st.ifflags & 1 );

		if ( ( st.ifflags & 3 ) == 3 ) {
			t_down = 0;
		} else {
			if ( t_down == 0 ) t_down = t;
			if ( t - t_down >= (UD)( down ? DOWN_MS : STOPPED_MS ) ) {
				card_restart(t, down);
				return;
			}
		}
	}
	/* the layer scans on while nothing is joined: each scan's networks taken as it ends */
	if ( st.scans != ob_scans ) {
		ob_scans = st.scans;
		if ( ws <= WS_SCANNING || nscans < 4 ) scan_take();
		if ( ws == WS_SCANNING ) ws = WS_IDLE;
		if ( ws == WS_IDLE ) {
			k = choose(&b);
			if ( k >= 0 ) {
				if ( !ob_join(k, &b) ) fail(( reason != NULL ) ? reason : "refused");
			} else if ( want_net >= 0 ) {
				tm_putstring((UB *)"wifi: the network asked for was not found\n");
				reason = "notfound";
				want_net = -1;
			}
		}
	}
	/*
	 * The time read again: a join begun above set t_state after t was
	 * read, and t - t_state would have wrapped round to a timeout at once
	 */
	t = now_ms();
	switch ( ws ) {
	case WS_SCANNING:
		if ( t - t_state > SCAN_MS ) ws = WS_IDLE;
		break;
	case WS_JOINING:
		if ( st.state == S_RUN ) {
			knl_memcpy(cur_bss.bssid, st.bssid, 6);
			if ( cur_wpa2 ) {
				tm_putstring((UB *)"wifi: associated; the 4-way handshake begins\n");
				shake_begin(t);
			} else {
				(void)rump_tswifi_port(1);
				now_connected("wifi: associated; no security: connected\n");
			}
		} else if ( t - t_state > (UD)( ob_lost ? OB_LOST_MS : OB_JOIN_MS ) ) {
			tm_printf((UB *)"wifi: not joined in %d s (net80211 %s)\n",
				  ( ob_lost ? OB_LOST_MS : OB_JOIN_MS ) / 1000,
				  ( st.state >= 0 && st.state <= 4 ) ? sname[st.state] : "?");
			fail(ob_lost ? "lost" : ( reason != NULL ) ? reason : "timeout");
		}
		break;
	case WS_HANDSHAKE:
		if ( knl_wpa_state() == WPAG_DONE ) {
			(void)rump_tswifi_port(1);
			ob_lost = FALSE;
			net_clear_temp_disabled(cur_net, TRUE);	/* connected: the failures counted forgotten */
			now_connected("wifi: the 4-way and group key handshakes are done: connected\n");
		} else if ( st.state != S_RUN ) {
			/* the layer left the access point during the handshake: it tries again, or another */
			tm_putstring((UB *)"wifi: the association was lost during the handshake\n");
			knl_wpa_disassoc();
			eapol_pending = FALSE;
			reason = "password";
			ws = WS_JOINING;
			ob_wrong_key();
		} else if ( t - t_state > SHAKE_MS ) {
			tm_printf((UB *)"wifi: the handshake did not end in %d s (a wrong passphrase?)\n", SHAKE_MS / 1000);
			fail("password");
		}
		break;
	case WS_CONNECTED:
		if ( st.state != S_RUN ) {
			/*
			 * The access point sent the station away, or its beacons
			 * were missed: the layer looks for the network again
			 * itself (another access point of it as well)
			 */
			tm_printf((UB *)"wifi: the network was lost (net80211 %s): the layer looks for it again\n",
				  ( st.state >= 0 && st.state <= 4 ) ? sname[st.state] : "?");
			if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
			knl_wpa_disassoc();
			(void)rump_tswifi_port(0);
			eapol_pending = FALSE;
			reason = "lost";
			ob_lost = TRUE;
			ws = WS_JOINING;
			t_state = t;
		} else if ( t - t_link >= LINK_SAY_MS ) {
			/* the link, for the records: the signal, the rate chosen, the frames, the scans and moves */
			t_link = t;
			ob_rate(r, sizeof(r));
			tm_printf((UB *)"wifi: link: %d dBm, %s, %d MHz, PMF %s, sent %ld, received %ld, dropped %ld, "
				  "background scans %d, roams %d\n", dbm(st.rssi), r, st.freq, st.pmf ? "on" : "off",
				  (D)n_sent, (D)n_recv, (D)n_drop, (INT)st.bgscans, (INT)ob_roams);
		}
		break;
	default:
		break;
	}
}
#endif /* WIFI_OBSD */

/* The saved network k joined through b; FALSE when it could not be begun */
LOCAL BOOL join( INT k, CONST TSWIFI_BSS *b )
{
	WNET	*p = &nets[k];
	UB	pmk[32], ie[64];
	INT	ie_len = 0, er, c = sec_of(b), reset = 0;
	CONST UB *rsn;
	char	s[33];

	ssid_str(b->ssid, b->ssid_len, s);
	cur_net = k;
	cur_bss = *b;
	reason = NULL;
	eapol_pending = FALSE;
	cur_fw = FALSE;
	cur_akm = 0;
	if ( rump_tswifi_fw_keys != NULL ) (void)rump_tswifi_fw_keys(0, NULL, NULL, 0);
	if ( p->sec != 0 && ( fw_caps & FW_IDSUP ) ) {
		/*
		 * The card's own supplicant makes the handshakes, and with them
		 * the protection of management frames and SAE when the access
		 * point asks for them: it is given the PMK or the password, and
		 * the host's state machine and RSN element are not used (9.6.8).
		 * It takes SAE when the access point offers it and there is a
		 * password, else PSK.
		 */
		BOOL	have_pmk, sae;
		char	pw[PW_MAX + 1];
		INT	pw_len;

		(void)net_secret(k, pmk, &have_pmk, NULL, NULL, pw, &pw_len);
		sae = ( pw_len > 0 && ( c == WPAG_WPA3 || c == WPAG_WPA2WPA3 ) && ( fw_caps & FW_SAE ) );
		if ( !sae && !( have_pmk && ( c == WPAG_WPA2 || c == WPAG_WPA2WPA3 ) ) ) {
			knl_memset(pmk, 0, sizeof(pmk));
			knl_memset(pw, 0, sizeof(pw));
			if ( c != WPAG_WPA2 && c != WPAG_WPA2WPA3 && c != WPAG_WPA3 ) {
				tm_printf((UB *)"wifi: \"%s\" offers %s, not WPA2- or WPA3-Personal\n", s, sec_name(c));
				reason = "unsupported";
			} else {
				tm_printf((UB *)"wifi: \"%s\" offers %s: no key or password is saved for it%s\n", s,
					  sec_name(c), ( c == WPAG_WPA3 && !( fw_caps & FW_SAE ) ) ? ", and the card has no SAE" : "");
				reason = ( c == WPAG_WPA3 && !( fw_caps & FW_SAE ) ) ? "unsupported" : "password";
			}
			return FALSE;
		}
		knl_wpa_disassoc();
		er = rump_tswifi_param(2, 1);
		if ( er >= 0 && er <= 1 ) {
			reset = er;
			er = rump_tswifi_optie(NULL, 0);
		}
		if ( er == 0 ) {
			er = rump_tswifi_fw_keys(1, have_pmk ? pmk : NULL, sae ? (CONST UB *)pw : NULL, sae ? pw_len : 0);
		}
		knl_memset(pmk, 0, sizeof(pmk));
		knl_memset(pw, 0, sizeof(pw));
		cur_fw = TRUE;
		cur_akm = sae ? WPAG_AKM_SAE : WPAG_AKM_PSK;
	} else if ( p->wpa2 ) {
		rsn = find_ie(b->ie, b->ie_len, 48);
		if ( ( c != WPAG_WPA2 && c != WPAG_WPA2WPA3 ) || rsn == NULL ) {
			tm_printf((UB *)"wifi: \"%s\" offers %s, not WPA2-Personal\n", s, sec_name(c));
			reason = "unsupported";
			return FALSE;
		}
		if ( !p->keyed ) {
			tm_printf((UB *)"wifi: \"%s\": no key is saved for it\n", s);
			reason = "password";
			return FALSE;
		}
		knl_memcpy(pmk, p->pmk, sizeof(pmk));	/* the saved key: the passphrase is kept nowhere */
		er = knl_wpa_start(wmac, b->bssid, b->ssid, b->ssid_len, rsn, 2 + rsn[1], pmk, ie, &ie_len);
		knl_memset(pmk, 0, sizeof(pmk));
		if ( er != 0 ) {
			reason = "unsupported";
			return FALSE;
		}
		er = rump_tswifi_param(2, 1);
		if ( er >= 0 && er <= 1 ) {
			reset = er;
			er = rump_tswifi_optie(ie, ie_len);
		}
	} else {
		if ( c != WPAG_OPEN ) {
			tm_printf((UB *)"wifi: \"%s\" is saved with no security but offers %s\n", s, sec_name(c));
			reason = "unsupported";
			return FALSE;
		}
		knl_wpa_disassoc();
		er = rump_tswifi_param(0, 0);
		if ( er >= 0 && er <= 1 ) {
			reset = er;
			er = rump_tswifi_optie(NULL, 0);
		}
	}
	if ( er == 0 && reset ) {
		/*
		 * net80211 started the card again for the new security, and the
		 * card scans anew: the network is joined once that scan is over
		 */
		tm_printf((UB *)"wifi: the card restarted for %s; joining \"%s\" after its scan\n",
			  p->wpa2 ? "WPA2" : "no security", s);
		if ( want_net < 0 ) want_net = k;
		cur_net = -1;
		ws = WS_SCANNING;
		t_state = t_scan = now_ms();
		scan_wait = TRUE;	/* the card's own scan, not the table it had before */
		return TRUE;
	}
	cur_wpa2 = p->wpa2 || cur_fw;		/* with keys (WPA3 too, through the card) */
	rump_tswifi_debug(1, DBG_JOIN);
	if ( er == 0 ) er = rump_tswifi_assoc(b->bssid, b->ssid, b->ssid_len);
	tm_printf((UB *)"wifi: joining \"%s\" at %02x:%02x:%02x:%02x:%02x:%02x, %d MHz, %d dBm, %s: %s (%d)\n",
		  s, b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
		  b->freq, dbm(b->rssi),
		  cur_fw ? ( ( cur_akm == WPAG_AKM_SAE ) ? "WPA3-Personal by the card's supplicant"
			     : "WPA2-Personal by the card's supplicant" )
		  : p->wpa2 ? "WPA2-Personal" : "no security",
		  ( er == 0 ) ? "asked" : "refused", er);
	if ( er != 0 ) {
		reason = "refused";
		return FALSE;
	}
	ws = WS_JOINING;
	t_state = now_ms();
	last_s = -1;
	return TRUE;
}

/* The join given up: the network rests a while before it is tried again */
LOCAL void fail( CONST char *why )
{
	if ( cur_net >= 0 && cur_net < nnet ) {
		nets[cur_net].failed_at = now_ms();
		nets[cur_net].fails++;
	}
	reason = why;
	eapol_pending = FALSE;
	tm_printf((UB *)"wifi: the join is given up (%s)\n", why);
	if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();	/* the record of it kept at once */
	knl_wpa_disassoc();
#ifdef WIFI_OBSD
	knl_wpa_sae_abort();
	ob_sae = FALSE;
	ob_lost = FALSE;
#endif
	(void)rump_tswifi_deauth(cur_bss.bssid, 3);
	rump_tswifi_debug(1, DBG_RUN);
	ws = WS_IDLE;
	t_state = now_ms();
	cur_net = -1;
}

/*
 * Whether the card joins with SAE (WPA3-Personal), by what it can do:
 * a card whose own supplicant has SAE (rump_tswifi_fw_caps), or OpenBSD's
 * layer, which hands SAE's frames to the host's (knl_wpa_sae_*). NetBSD's
 * net80211 with iwm has no way to send them.
 */
LOCAL BOOL card_sae( void )
{
#ifdef WIFI_OBSD
	return TRUE;
#else
	return ( fw_caps & ( FW_IDSUP | FW_SAE ) ) == ( FW_IDSUP | FW_SAE );
#endif
}

/*
 * Why saved network k cannot be joined through the access point b by
 * this card (the word of REASON), NULL when it can: the security saved
 * and the access point's have one in common the card does
 */
/*
 * A network left a while after an authentication that failed, in the
 * steps and times of wpa_supplicant (wpa_supplicant.c's
 * wpas_auth_failed, wpas_clear_temp_disabled; events.c's
 * wpas_temp_disabled): each failure counted, and the network not
 * joined for 10 s after the first, 20 s after the second, 30 s after
 * the third, 60 s after 4 or 5, 90 s after 6 to 10, 120 s after 11 to
 * 50, 300 s after more; a later end kept when it is later. The count
 * is cleared when a join of it ends in a connection, or the person
 * asks for it (CONNECT, SAVE); the time alone when it has run out.
 */
LOCAL void net_clear_temp_disabled( INT k, BOOL clear_failures )
{
	char	s[33];

	if ( k < 0 || k >= nnet ) return;
	if ( nets[k].disabled_until != 0 ) {
		ssid_str(nets[k].ssid, nets[k].ssid_len, s);
		tm_printf((UB *)"wifi: \"%s\" may be joined again (CTRL-EVENT-SSID-REENABLED)\n", s);
	}
	nets[k].disabled_until = 0;
	if ( clear_failures ) nets[k].auth_failures = 0;
}

/* Seconds the network is left still, 0 none (wpas_temp_disabled) */
LOCAL INT net_temp_disabled( INT k, UD t )
{
	if ( k < 0 || k >= nnet || nets[k].disabled_until == 0 ) return 0;
	if ( nets[k].disabled_until > t ) return (INT)( ( nets[k].disabled_until - t + 999 ) / 1000 );
	net_clear_temp_disabled(k, FALSE);
	return 0;
}

#ifdef WIFI_OBSD
LOCAL void net_auth_failed( INT k, CONST char *why )
{
	UD	t = now_ms();
	UW	n;
	INT	dur;
	char	s[33];

	if ( k < 0 || k >= nnet ) return;
	n = ++nets[k].auth_failures;
	dur = ( n > 50 ) ? 300 : ( n > 10 ) ? 120 : ( n > 5 ) ? 90 : ( n > 3 ) ? 60 : ( n > 2 ) ? 30 : ( n > 1 ) ? 20 : 10;
	if ( t + (UD)dur * 1000 <= nets[k].disabled_until ) return;
	nets[k].disabled_until = t + (UD)dur * 1000;
	ssid_str(nets[k].ssid, nets[k].ssid_len, s);
	tm_printf((UB *)"wifi: \"%s\" is left for %d s (CTRL-EVENT-SSID-TEMP-DISABLED auth_failures=%u reason=%s)\n",
		  s, dur, (UINT)n, why);
}
#endif

LOCAL CONST char *net_unjoinable( INT k, CONST TSWIFI_BSS *b )
{
	INT	sec = nets[k].sec, c = sec_of(b);
	BOOL	ap_psk = ( c == WPAG_WPA2 || c == WPAG_WPA2WPA3 ), ap_sae = ( c == WPAG_WPA3 || c == WPAG_WPA2WPA3 );

	if ( sec == 0 ) return ( c == WPAG_OPEN ) ? NULL : "unsupported";
	if ( SEC_PSK(sec) && ap_psk ) return NULL;
	if ( SEC_SAE(sec) && ap_sae ) return card_sae() ? NULL : "nowpa3";
	return "unsupported";
}

/*
 * The saved network to join now and where: the asked one, else the
 * strongest saved one; of each, an access point the card can join it
 * through. The asked one found only where it cannot be joined is given
 * up with the reason.
 */
LOCAL INT choose( TSWIFI_BSS *out )
{
	UD	t = now_ms();
	INT		i, k;
	CONST char	*w, *why = NULL;
	char		s[33];

	for ( i = 0; i < nbss; i++ ) {
		for ( k = 0; k < nnet; k++ ) {
			WNET	*p = &nets[k];

			if ( p->ssid_len != bss[i].ssid_len || !same_bytes(p->ssid, bss[i].ssid, p->ssid_len) ) continue;
			if ( net_temp_disabled(k, t) > 0 ) continue;	/* left a while (net_auth_failed) */
			if ( want_net >= 0 ) {
				if ( k != want_net ) continue;
			} else {
				if ( held || !wifi_on || !p->autojoin || !net_auto(k) ) continue;	/* the person logged in's only */
				if ( p->failed_at != 0 && t - p->failed_at < (UD)RETRY_MS * ( p->fails > 3 ? 6 : 1 ) ) continue;
			}
			if ( ( w = net_unjoinable(k, &bss[i]) ) != NULL ) {
				if ( k == want_net && why == NULL ) why = w;
				continue;		/* another access point of it, or another network */
			}
			*out = bss[i];
			return k;
		}
	}
	if ( why != NULL ) {
		ssid_str(nets[want_net].ssid, nets[want_net].ssid_len, s);
		tm_printf((UB *)"wifi: \"%s\" was asked for, but it is saved as %s and %s\n", s, saved_name(nets[want_net].sec),
			  ( why[0] == 'n' ) ? "the card has no SAE (WPA3-Personal)" : "the access point offers no security in common");
		reason = why;
		want_net = -1;
	}
	return -1;
}

/* ---------------------------------------------------------------- commands */

LOCAL void take_cmd( void )
{
	char	c[sizeof(cmd)];
	UB	ssid[32];
	INT	i, n;
	TS_UUID	who;

	LOCK();
	knl_memcpy(c, cmd, sizeof(c));
	who = cmd_user;
	cmd[0] = '\0';
	UNLOCK();
	if ( c[0] == '\0' ) return;
	tm_printf((UB *)"wifi: asked: %s\n", c);
	if ( knl_strcmp(c, "SCAN") == 0 ) {
		if ( ws == WS_IDLE || ws == WS_CONNECTED ) {
			if ( ws == WS_IDLE ) scan_start();
#ifdef WIFI_OBSD
			else (void)rump_tswifi_scan();		/* a background scan, the network kept */
#else
			else tm_putstring((UB *)"wifi: no scan while a network is joined\n");
#endif
		}
	} else if ( knl_strcmp(c, "DISCONNECT") == 0 || knl_strcmp(c, "SHUTDOWN") == 0 ) {
		held = TRUE;
		want_net = -1;
		if ( ws >= WS_JOINING ) {
			fail(( c[0] == 'S' ) ? "off" : "asked");
			if ( cur_net >= 0 ) nets[cur_net].failed_at = 0;
		}
	} else if ( c[0] == 'C' && c[1] == 'O' && c[2] == 'N' ) {
		held = FALSE;
		want_net = -1;
		for ( i = 0; c[i] != '\0' && c[i] != ' '; i++ ) ;
		if ( c[i] == ' ' && ( n = unhex(c + i + 1, ssid, 32) ) > 0 ) {
			want_net = net_find(ssid, n, &who);	/* the writer's own */
			if ( want_net < 0 ) tm_putstring((UB *)"wifi: that network is not saved by the user\n");
			/* a network asked for by name is no longer left, its failures forgotten (wpa_supplicant_select_network) */
			net_clear_temp_disabled(want_net, TRUE);
		}
		for ( i = 0; i < nnet; i++ ) {
			nets[i].failed_at = 0;
			nets[i].fails = 0;
		}
		if ( ws == WS_CONNECTED && want_net >= 0 && want_net == cur_net ) {
			tm_putstring((UB *)"wifi: that network is joined already\n");
			want_net = -1;
		}
		if ( ws >= WS_JOINING && want_net >= 0 && want_net != cur_net ) fail("asked");
		if ( ws == WS_IDLE ) scan_start();
	}
}

/* ---------------------------------------------------------------- the task */

/* The association seen: the handshake begins, with the message kept from before it if there is one */
LOCAL void shake_begin( UD t )
{
	ws = WS_HANDSHAKE;
	t_state = t;
	knl_wpa_assoc(cur_bss.bssid);
	if ( eapol_pending ) {
		eapol_pending = FALSE;
		tm_putstring((UB *)"wifi: the EAPOL-Key kept is given to the handshake\n");
		knl_wpa_rx(eapol_pend.f + 6, eapol_pend.f + 14, eapol_pend.len - 14);
	}
}

LOCAL void take_eapol( void )
{
	RXSLOT	s;
	UINT	imask;
	BOOL	got;

	for ( ;; ) {
		ISpinLock(&rx_lock, &imask);
		got = ( eq_head != eq_tail );
		if ( got ) {
			s = eq[eq_head % EQ_SLOTS];
			eq_head++;
		}
		ISpinUnlock(&rx_lock, &imask);
		if ( !got ) break;
		if ( cur_fw ) {
			/* the card's supplicant makes this network's handshakes: none is the host's */
			tm_putstring((UB *)"wifi: EAPOL-Key in, not taken: the card's supplicant makes the handshakes\n");
			continue;
		}
		if ( ws == WS_JOINING ) {
			/* the first message can come before the association is seen */
			(void)rump_tswifi_status(&st);
			if ( st.state == S_RUN ) {
				tm_putstring((UB *)"wifi: associated\n");
				shake_begin(now_ms());
			} else if ( same_bytes(s.f + 6, cur_bss.bssid, 6) ) {
				/*
				 * The access point sends it once its association
				 * response is out, while the card may still be
				 * moving to RUN: kept, and given to the handshake
				 * when the association is seen, not lost
				 */
				eapol_pend = s;
				eapol_pending = TRUE;
				tm_putstring((UB *)"wifi: EAPOL-Key kept until the association is seen\n");
			}
		}
		{
			UD	rc;
			CONST char *m = eapol_msg(s.f + 14, s.len - 14, &rc);

			tm_printf((UB *)"wifi: EAPOL-Key in: %s, %d bytes, replay %ld, from %02x:%02x:%02x:%02x:%02x:%02x%s\n",
				  m, s.len - 14, (D)rc, s.f[6], s.f[7], s.f[8], s.f[9], s.f[10], s.f[11],
				  ( ws != WS_HANDSHAKE && ws != WS_CONNECTED ) ? " (not taken: no association yet)" : "");
		}
		if ( ws != WS_HANDSHAKE && ws != WS_CONNECTED ) continue;
		knl_wpa_rx(s.f + 6, s.f + 14, s.len - 14);
	}
}

/* A network joined: told, and the adapters' manager sees it (link_gen) */
LOCAL void now_connected( CONST char *how )
{
	tm_putstring((CONST UB *)how);
	ws = WS_CONNECTED;
	want_net = -1;
	if ( cur_net >= 0 && cur_net < nnet ) nets[cur_net].fails = 0;
	restarts = 0;
	t_link = now_ms() - LINK_SAY_MS + 10000;	/* the first told 10 s after: are frames going and coming */
	said_out = said_in = 0;
	link_gen++;
	rump_tswifi_debug(0, DBG_RUN);
	if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
}

/*
 * The card stopped: the driver took its interface down after an error
 * of the firmware or of the card, or after a frame it sent did not go
 * out (iwm, iwx), or the interface is up and the card does not run (its start
 * failed). It is started again: the interface taken down and brought up,
 * as at the start, the network being joined given up. The first time at
 * once, then after 10 s, 30 s, 60 s, and then every 2 minutes, until it
 * is connected again.
 */
LOCAL void card_restart( UD t, BOOL down )
{
	static CONST UD	gap[] = { 0, 10000, 30000, 60000, 120000 };
	INT		er;

	if ( t_restart != 0 && t - t_restart < gap[( restarts < 4 ) ? restarts : 4] ) return;
	restarts++;
	t_restart = t;
	tm_printf((UB *)"wifi: the card stopped (%s): it is started again (%d)\n",
		  down ? "the driver took its interface down" : "its interface is up but it does not run", restarts);
#ifdef X64
	if ( restarts == 1 ) knl_wifi_irq_say("the card stopped");
#endif
	if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
	if ( ws >= WS_JOINING ) reason = "lost";
	knl_wpa_disassoc();
	eapol_pending = FALSE;
	cur_net = -1;
	scan_wait = FALSE;
	scan_again = TRUE;			/* a scan once it is up, saved networks or none */
	(void)rump_tsnet_ifup(W_IF, 0);
	er = rump_tsnet_ifup(W_IF, 1);
	tm_printf((UB *)"wifi: %s up again (%d)\n", W_IF, er);
	rump_tswifi_debug(1, DBG_RUN);
	ws = WS_IDLE;
	t_state = t;
	t_scan = 0;
	t_down = 0;
}

/* One look at the card and one step of the state */
LOCAL void step( void )
{
	UD	t = now_ms();
	TSWIFI_BSS	b;
	INT		k;
	static CONST char *CONST sname[] = { "init", "scan", "authenticating", "associating", "run" };

#ifdef WIFI_OBSD
	step_obsd();
	return;
#endif
	(void)rump_tswifi_status(&st);
	if ( st.state != last_s && ws >= WS_JOINING ) {
		if ( st.state >= S_INIT && st.state <= S_RUN ) {
			tm_printf((UB *)"wifi: net80211: %s\n", sname[st.state]);
		}
	}
	last_s = st.state;
	/* the card not running: started again once it has been so a while */
	if ( ws >= WS_IDLE ) {
		BOOL	down = !( st.ifflags & 1 );

		if ( ( st.ifflags & 3 ) == 3 ) {
			t_down = 0;
		} else {
			if ( t_down == 0 ) t_down = t;
			if ( t - t_down >= (UD)( down ? DOWN_MS : STOPPED_MS ) ) {
				card_restart(t, down);
				return;
			}
		}
	}
	switch ( ws ) {
	case WS_SCANNING:
		if ( scan_wait ) {
			/*
			 * The card was started again: its scan begins once it is
			 * up, and until then the flag says no scan; one begun
			 * here when none is seen in a while
			 */
			if ( st.scanning ) {
				scan_wait = FALSE;
			} else if ( t - t_state > SCAN_WAIT_MS ) {
				scan_wait = FALSE;
				tm_putstring((UB *)"wifi: no scan after the card started again: one begun\n");
				scan_start();
			}
			break;
		}
		if ( !st.scanning || t - t_state > SCAN_MS ) {
			if ( st.scanning ) tm_putstring((UB *)"wifi: the scan did not end; taking what it found\n");
			ws = WS_IDLE;
			t_state = t;
			scan_take();
			k = choose(&b);
			if ( k >= 0 ) {
				if ( !join(k, &b) ) fail(( reason != NULL ) ? reason : "refused");
			} else if ( want_net >= 0 ) {
				tm_putstring((UB *)"wifi: the network asked for was not found\n");
				reason = "notfound";
				want_net = -1;
			}
		}
		break;
	case WS_IDLE:
		if ( scan_again ? t - t_scan >= SCAN_AGAIN_MS
		     : ( want_net >= 0 ) ? t - t_scan >= ASK_MS
		     : ( !held && wifi_on && nnet > 0 && t - t_scan > RESCAN_MS ) ) {
			scan_start();
		}
		break;
	case WS_JOINING:
		if ( st.state == S_RUN ) {
			if ( cur_wpa2 ) {
				tm_putstring((UB *)"wifi: associated; the 4-way handshake begins\n");
				shake_begin(t);
			} else {
				now_connected("wifi: associated; no security: connected\n");
			}
		} else if ( t - t_state > JOIN_MS ) {
			tm_printf((UB *)"wifi: no association in %d s (net80211 %s)\n", JOIN_MS / 1000,
				  ( st.state >= 0 && st.state <= 4 ) ? sname[st.state] : "?");
			fail("timeout");
		}
		break;
	case WS_HANDSHAKE:
		if ( cur_fw ) {
			/* the card's supplicant: done when the card says the access point's port is open */
			TSWIFI_FWLINK	fl;

			if ( t - t_fw >= 500 && rump_tswifi_fw_link(&fl) == 0 ) {
				t_fw = t;
				if ( fl.authorized ) {
					now_connected("wifi: the card's supplicant has the keys: connected\n");
					fw_lk_rssi = fl.rssi;
					fw_lk_kbps = fl.tx_kbps;
					fw_lk_gen = link_gen;
					break;
				}
			}
			if ( st.state != S_RUN || t - t_state > SHAKE_MS ) {
				(void)rump_tswifi_fw_link(&fl);
				tm_printf((UB *)"wifi: the card's supplicant did not end the handshakes"
					  " (its state %d, %s; a wrong passphrase?)\n", fl.sup_status,
					  ( st.state != S_RUN ) ? "the association was lost" : "the time ran out");
				fail("password");
			}
		} else if ( knl_wpa_state() == WPAG_DONE ) {
			now_connected("wifi: the 4-way and group key handshakes are done: connected\n");
		} else if ( st.state != S_RUN ) {
			tm_putstring((UB *)"wifi: the association was lost during the handshake\n");
			fail("password");
		} else if ( t - t_state > SHAKE_MS ) {
			tm_printf((UB *)"wifi: the handshake did not end in %d s (a wrong passphrase?)\n", SHAKE_MS / 1000);
			fail("password");
		}
		break;
	case WS_CONNECTED:
		if ( st.state != S_RUN ) {
			/*
			 * The access point sent the station away, its beacons
			 * were missed, or the card was started again: the
			 * network is looked for at once, the strongest access
			 * point of the saved ones taken (another of the same
			 * SSID as well). net80211 may be left in AUTH, ASSOC or
			 * SCAN: the scan puts it back to INIT first.
			 */
			tm_printf((UB *)"wifi: the network was lost (net80211 %s): looked for again\n",
				  ( st.state >= 0 && st.state <= 4 ) ? sname[st.state] : "?");
			if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
			knl_wpa_disassoc();
			eapol_pending = FALSE;
			reason = "lost";
			ws = WS_IDLE;
			t_state = t;
			t_scan = 0;
			cur_net = -1;
		} else if ( cur_fw || ( fw_caps & FW_IDSUP ) ) {
			/*
			 * A card that roams itself (9.6.8): its access point, signal
			 * and rate as it has them, asked every 2 seconds; a roam is
			 * told as the access point changes
			 */
			TSWIFI_FWLINK	fl;
			static CONST UB	zero[6] = { 0, 0, 0, 0, 0, 0 };

			if ( t - t_fw < FW_LINK_MS || rump_tswifi_fw_link(&fl) != 0 ) break;
			t_fw = t;
			fw_lk_rssi = fl.rssi;
			fw_lk_kbps = fl.tx_kbps;
			fw_lk_gen = link_gen;
			if ( !same_bytes(fl.bssid, zero, 6) && !same_bytes(fl.bssid, cur_bss.bssid, 6) ) {
				tm_printf((UB *)"wifi: roam: %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x,"
					  " %d dBm, %d MHz\n",
					  cur_bss.bssid[0], cur_bss.bssid[1], cur_bss.bssid[2], cur_bss.bssid[3],
					  cur_bss.bssid[4], cur_bss.bssid[5], fl.bssid[0], fl.bssid[1], fl.bssid[2],
					  fl.bssid[3], fl.bssid[4], fl.bssid[5], fl.rssi, fl.freq);
				knl_memcpy(cur_bss.bssid, fl.bssid, 6);
				cur_bss.freq = (UH)fl.freq;
				if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
			}
			if ( t - t_link >= LINK_SAY_MS ) {
				/* the link, for the records: the signal, the rates and the frames */
				t_link = t;
				tm_printf((UB *)"wifi: link: %d dBm, %d Mb/s (received %d Mb/s, %s), %d MHz, keys %s,"
					  " roams %d, sent %ld, received %ld, dropped %ld\n",
					  fl.rssi, fl.tx_kbps / 1000, fl.rx_kbps / 1000,
					  ( fl.mode == 2 ) ? "802.11ac" : ( fl.mode == 1 ) ? "802.11n" : "802.11a/b/g",
					  fl.freq, fl.authorized ? "in" : "not in", fl.roams,
					  (D)n_sent, (D)n_recv, (D)n_drop);
			}
		} else if ( t - t_link >= LINK_SAY_MS ) {
			/* the link, for the records: the signal, the rate chosen, and the frames */
			t_link = t;
			tm_printf((UB *)"wifi: link: %d dBm, %d Mb/s, %d MHz, sent %ld (refused %ld), received %ld, dropped %ld;"
				  " the stack's card %d (%d is the Wi-Fi)\n",
				  dbm(st.rssi), st.rate / 2, st.freq, (D)n_sent, (D)n_send_err, (D)n_recv, (D)n_drop,
				  knl_netsel_card(), NETSEL_WIFI);
			if ( rump_tswifi_stats != NULL ) {
				UW	v[18];

				knl_memset(v, 0, sizeof(v));
				(void)rump_tswifi_stats(v, 18);
				tm_printf((UB *)"wifi: link: %s gave %u frames (%u bad), took %u (%u refused); net80211: CCMP %u,"
					  " MIC failures %u, replays %u, bad form %u, of a key index with no key %u, unencrypted dropped %u,"
					  " decryption failed %u, decapsulation failed %u, sent with no key %u, other BSS %u,"
					  " duplicates %u, not associated %u, port not open %u (the port %s)\n",
					  W_IF, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12],
					  v[13], v[14], v[15], v[16], v[17] ? "open" : "closed");
			}
		}
		break;
	default:
		break;
	}
}

/* A card that did not start: the saved networks may still be changed, and nothing else is done */
LOCAL void serve_only( void )
{
	UINT	ptn;

	conf_take();
	w_serving = TRUE;
	state_refresh();
	for ( ;; ) {
		(void)tk_wai_flg(wflg, F_REQ | F_CMD, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
		take_req();
		LOCK();
		cmd[0] = '\0';
		UNLOCK();
	}
}

LOCAL void w_task( INT stacd, void *exinf )
{
	UINT	ptn;
	INT	er;
	UD	t_conf;
	WSTATE	was = WS_OFF;
#ifndef X64
	INT	n_try;
#endif

	(void)stacd;
	(void)exinf;
	ws = WS_STARTING;
	if ( knl_obdev_add((CONST UB *)"wlan0", NULL, &w_ops, NULL, &wuuid) >= E_OK ) wobj = TRUE;
	state_refresh();
#ifdef X64
	tm_printf((UB *)"wifi: starting the card %04x:%04x at %d:%d.%d (iwm or iwx by its PCI ID, firmware from the system volume)\n",
		  wdev.vendor, wdev.device, wdev.bus, wdev.dev, wdev.fn);
	er = rump_tswifi_start(wmac);
#else
	tm_printf((UB *)"wifi: starting the chip on the SDIO host (bwfm, firmware from the system volume)\n");
	er = rump_tswifi_start(wmac);
	for ( n_try = 0; er == 6 && n_try < W_TAKE_TRIES; n_try++ ) {	/* ENXIO: not found yet */
		tk_dly_tsk(500);
		er = rump_tswifi_take(wmac);
	}
#endif
	if ( er != 0 ) {
		tm_printf((UB *)"wifi: the card did not start (%d): no firmware, or the card did not answer\n", er);
#ifdef X64
		knl_wifi_irq_say("the card did not start");
#endif
		ws = WS_NOFW;
		serve_only();
	} else {
		wmac_known = TRUE;
	}
	ready = TRUE;
	tm_printf((UB *)"wifi: %s ready, address %02x:%02x:%02x:%02x:%02x:%02x\n",
		  W_IF, wmac[0], wmac[1], wmac[2], wmac[3], wmac[4], wmac[5]);
	rump_tswifi_debug(1, DBG_RUN);
	(void)rump_tsnet_ip6on(W_IF, 0);	/* the card's interface carries no address: neta0 does */
	er = rump_tsnet_ifup(W_IF, 1);
	tm_printf((UB *)"wifi: %s up (%d): the runtime firmware loaded\n", W_IF, er);
	if ( er == 0 && rump_tswifi_fw_caps != NULL ) {
		fw_caps = rump_tswifi_fw_caps();
		tm_printf((UB *)"wifi: the card's firmware: its own supplicant %s, SAE %s, PMF %s\n",
			  ( fw_caps & FW_IDSUP ) ? "yes" : "no", ( fw_caps & FW_SAE ) ? "yes" : "no",
			  ( fw_caps & FW_MFP ) ? "yes" : "no");
	}
	/*
	 * The first scan: iwm and iwx begin one as they start, which is taken as the
	 * one asked for; bwfm, with the roaming left to the manager, does not
	 */
	ws = WS_IDLE;
	t_state = t_scan = now_ms();
	if ( er == 0 ) scan_start();

	conf_take();
	cur_user = *session_user();
	w_serving = TRUE;
	t_conf = now_ms();

	for ( ;; ) {
		(void)tk_wai_flg(wflg, F_EAPOL | F_CMD | F_REQ, TWF_ORW | TWF_BITCLR, &ptn,
				 ( ws == WS_JOINING || ws == WS_HANDSHAKE ) ? 20 : W_TICK_MS);
		/*
		 * The settings looked at every 2 seconds, the object opened only
		 * for the read: an object held open would keep the system volume
		 * from being taken off
		 */
		if ( now_ms() - t_conf >= CONF_POLL_MS && ( t_conf = now_ms(), conf_changed() ) ) {
			tm_putstring((UB *)"wifi: the settings were written\n");
			conf_take();
			held = FALSE;
			for ( er = 0; er < nnet; er++ ) nets[er].failed_at = 0;
			if ( ws >= WS_JOINING && ( cur_net < 0 || cur_net >= nnet ) ) {
				fail("asked");
			} else if ( ws >= WS_JOINING && !wifi_on ) {
				tm_putstring((UB *)"wifi: the settings say off: the network is left\n");
				fail("asked");
			}
			if ( ws == WS_IDLE ) t_scan = 0;
		}
		take_eapol();
		take_req();
		take_cmd();
		user_take();
		knl_wpa_tick();
#ifdef WIFI_OBSD
		ob_events();
#endif
		step();
		if ( ws != was ) {
			tm_printf((UB *)"wifi: %s -> %s\n", ws_name[was], ws_name[ws]);
			was = ws;
		}
		state_refresh();
	}
}

/* The two semaphores of the one change of the saved networks asked at a time */
LOCAL BOOL req_make( void )
{
	T_CSEM	cs;

	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 1;
	cs.maxsem = 1;
	rq_free = tk_cre_sem(&cs);
	cs.isemcnt = 0;
	rq_done = tk_cre_sem(&cs);
	return (BOOL)( rq_free > 0 && rq_done > 0 );
}

/* From the adapters' manager, once the object layer is up */
EXPORT void knl_wifi_start( void )
{
	T_CMTX	cm;
	T_CFLG	cf;
	T_CSEM	cs;
	T_CTSK	ct;
	ID	t;

	if ( !knl_wifi_present() ) return;
#if WIFI_SIM
	knl_wifisim_ap_start();		/* the simulator's access point, on the simulated air */
#endif
	if ( !req_make() ) {
		tm_putstring((UB *)"wifi: no room for the manager\n");
		return;
	}
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	wmtx = tk_cre_mtx(&cm);
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	wflg = tk_cre_flg(&cf);
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RX_SLOTS;
	rx_sem = tk_cre_sem(&cs);
	InitSpinLock(&rx_lock);
	eq = (RXSLOT *)Kmalloc(sizeof(RXSLOT) * EQ_SLOTS);
	if ( wmtx <= 0 || wflg <= 0 || rx_sem <= 0 || eq == NULL ) {
		tm_putstring((UB *)"wifi: no room for the manager\n");
		return;
	}
	rx_ring = (RXSLOT *)Kmalloc(sizeof(RXSLOT) * RX_SLOTS);
	if ( rx_ring == NULL ) return;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)w_task;
	ct.itskpri = W_PRI;
	ct.stksz = W_STKSZ;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}

#ifdef USE_UITEST
/*
 * A card for the tests of the screen (make UITEST=1, the UI test's
 * "wifitest"), on a machine with none: wlan0 with a scan's networks of
 * long names, the saved networks changed as on a card, and a CONNECT
 * said on the console with the key it would join with -- the saved one
 * or none -- rather than joined.
 */
LOCAL CONST struct {
	CONST char	*ssid;
	INT		freq, dbm;
	CONST char	*sec;
} test_bss[] = {
	{ "TessronOS-Test", 2412, -45, "wpa2" },			/* the strongest first, as a scan's */
	{ "DIRECT-42JE-HF-MS200AAH", 2437, -52, "wpa2" },
	{ "Cafe-Free-WiFi-5G-Guest-Network", 5180, -67, "open" },
	{ "Office-WPA3-Only", 5240, -71, "wpa3" },
};

LOCAL INT test_scan_make( void *ctx, UB *t, INT max )
{
	INT	i, n = 0;

	(void)ctx;
	for ( i = 0; i < (INT)( sizeof(test_bss) / sizeof(test_bss[0]) ); i++ ) {
		n = knl_oj_put(t, n, max, "BSS 02:00:00:00:00:0");
		n = put_num(t, n, max, i + 1);
		n = knl_oj_put(t, n, max, " ");
		n = put_num(t, n, max, test_bss[i].freq);
		n = knl_oj_put(t, n, max, " ");
		n = put_num(t, n, max, test_bss[i].dbm);
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put(t, n, max, test_bss[i].sec);
		n = knl_oj_put(t, n, max, " ");
		n = put_hex(t, n, max, (CONST UB *)test_bss[i].ssid, (INT)knl_strlen(test_bss[i].ssid));
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

LOCAL void w_test_task( INT stacd, void *exinf )
{
	UINT	ptn;
	char	c[sizeof(cmd)], s[33];
	UB	ssid[32];
	INT	i, n, k;
	TS_UUID	who;

	(void)stacd;
	(void)exinf;
	ws = WS_IDLE;
	fw_caps = FW_IDSUP | FW_SAE | FW_MFP;	/* a card that joins with WPA3, as the Raspberry Pi 5's */
	if ( knl_obdev_add((CONST UB *)"wlan0", NULL, &w_ops, NULL, &wuuid) >= E_OK ) wobj = TRUE;
	n = knl_oj_make(test_scan_make, NULL, &stext);
	stlen = ( n > 0 ) ? n : 0;
	conf_take();
	w_serving = TRUE;
	state_refresh();
	tm_putstring((UB *)"wifi: test card ready\n");
	for ( ;; ) {
		(void)tk_wai_flg(wflg, F_REQ | F_CMD, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
		take_req();
		LOCK();
		knl_memcpy(c, cmd, sizeof(c));
		who = cmd_user;
		cmd[0] = '\0';
		UNLOCK();
		if ( c[0] == 'C' && c[1] == 'O' && c[2] == 'N' ) {
			for ( i = 0; c[i] != '\0' && c[i] != ' '; i++ ) ;
			n = ( c[i] == ' ' ) ? unhex(c + i + 1, ssid, 32) : -1;
			k = ( n > 0 ) ? net_find(ssid, n, &who) : -1;
			if ( k < 0 ) {
				tm_putstring((UB *)"wifi: test: that network is not saved by the user\n");
			} else {
				INT	sec = nets[k].sec;

				ssid_str(nets[k].ssid, nets[k].ssid_len, s);
				tm_printf((UB *)"wifi: test: joining \"%s\", %s%s%s\n", s,
					  ( sec == 0 ) ? "no security" : !SEC_PSK(sec) ? ""
					  : nets[k].keyed ? "with the saved key" : "with no key",
					  ( sec == WPAG_SAVED_WPA2WPA3 ) ? ", " : "",
					  !SEC_SAE(sec) ? "" : ( nets[k].pw_len > 0 ) ? "by SAE with the saved password"
					  : "by SAE with no password");
			}
		} else if ( c[0] != '\0' ) {
			tm_printf((UB *)"wifi: test: asked: %s\n", c);
		}
		state_refresh();
	}
}

EXPORT void knl_wifi_test_card( void )
{
	T_CMTX	cm;
	T_CFLG	cf;
	T_CTSK	ct;
	ID	t;

	if ( knl_wifi_present() || wflg > 0 ) return;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	wmtx = tk_cre_mtx(&cm);
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	wflg = tk_cre_flg(&cf);
	if ( wmtx <= 0 || wflg <= 0 || !req_make() ) return;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)w_test_task;
	ct.itskpri = W_PRI;
	ct.stksz = W_STKSZ;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}
#endif /* USE_UITEST */

#endif /* X64 || USE_NETSEL */
