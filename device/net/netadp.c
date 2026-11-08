/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netadp.c (x64 PC, and the Raspberry Pi 5 with the Wi-Fi)
 *	The network adapters' manager (design 9.6): which card the stack
 *	uses, and the adapters as a device object.
 *
 *	A PC may have several cards (netsel.c): virtio-net in QEMU, the
 *	chipset's Intel Ethernet, a USB LAN adapter, the Wi-Fi. The person
 *	chooses one in システム環境設定 (〈機器〉〈機器情報〉〈ネットワーク
 *	アダプタ〉), which writes the machine's setting NET_ADAPTER: auto
 *	(the default), vnet, e1k, cdcnet, re, rge, igc, usblan or wifi. Left to the system, a
 *	wired card whose link is up is used before the Wi-Fi that is
 *	connected, and the card in use stays while neither of those is
 *	there, so that a cable pulled for a moment does not move the stack.
 *
 *	When the card changes, the stack's interface neta0 is given the new
 *	card's address before frames go through it (an access point takes
 *	frames only from the address it associated), the network's settings
 *	are taken again (so_conf), the IPv6 routers are asked to speak again
 *	(router solicitations, so_slaac.c), and, when the address comes from
 *	DHCP, it is asked for anew. So it is too when the link of the card in use
 *	comes up again: a cable put back, or the Wi-Fi joined again after the
 *	network was lost or another one joined (knl_wifi_link_gen). The
 *	address held stays while it is asked for (so_dhcp.c), so that a join
 *	of the same network again keeps the connections that use it.
 *
 *	The device object "netadp" (OB_S_NET) shows the adapters in record 1:
 *
 *	  SELECT auto			what the person chose
 *	  USING e1k			the card in use
 *	  ADAPTER e1k wired up 1000 48:2a:e3:0a:7a:75 Intel Ethernet 8086:15d7
 *	  ADAPTER wifi wifi down 0 ... Intel Wi-Fi 8086:24fd
 *	  WIFI connected -52 <SSID in hexadecimal>
 *
 *	(an adapter's line: its name in the setting, wired or wifi, its link,
 *	Mb/s, its address and what it is). A change is told as OB_E_CHANGE;
 *	the Wi-Fi's signal and rate, measured values that change all the
 *	time, are written as they are but not told (a reader that shows them
 *	reads them again now and then while it shows them).
 *
 *	The manager looks at the cards when a driver says that a link came
 *	up or went down, or that a card came or went (knl_netadp_link): the
 *	Intel Ethernet by its interrupt of a change of the link, the USB
 *	adapter as it is plugged in and taken out, the Wi-Fi as its manager
 *	joins and leaves. A card nothing tells of -- the board's Ethernet,
 *	whose PHY has no interrupt, and the Intel Ethernet without its
 *	interrupt -- is looked at every second while it is there, as a PHY
 *	without an interrupt is looked at.
 *
 *	The manager's task starts behind the first ask for the card's address
 *	(the start-up's knl_netdev_init) and waits for the object layer; then
 *	it starts the Wi-Fi's manager (wifi.c), when the build has the Wi-Fi.
 *	It reads the setting every 2 seconds, opening the machine's settings
 *	only for the read, so that no volume is held by it: holding them
 *	open to be told of a write would keep the system volume from being
 *	taken off.
 *
 *	On the Arm machines (USE_NETSEL, with the Wi-Fi) the adapters are the
 *	board's Ethernet, "eth" in the setting, and the Wi-Fi (netsel.c).
 */

#include <sys/machine.h>

#if defined(X64) || defined(USE_NETSEL)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "network/netbsd/netlog.h"	/* the lines timed */
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/so.h>
#include <ts/time.h>
#include "obj/obj.h"
#include "netsel.h"

#if USE_WIFI
IMPORT int	rump_tswifi_setmac( CONST char *ifname, CONST UB *mac );
IMPORT UW	knl_wifi_link_gen( void );
#endif
#ifdef X64
IMPORT INT	knl_e1k_speed( UH *p_device );
#else
#define knl_e1k_speed(p)	0
#endif

/* the NetBSD stack's SLAAC (so_slaac.c); weak: a build with lwIP has none */
IMPORT void	knl_so_slaac_relink( void ) __attribute__((weak));

#define ADP_PRI		30
#define ADP_STKSZ	(12 * 1024)
#define ADP_START_MS	500		/* the object layer looked for, as it starts */
#define ADP_CONF_MS	2000		/* the setting read every 2 seconds */
#define ADP_FIRST_TRIES	50		/* at the start, the setting asked for again while it is being written, */
#define ADP_FIRST_MS	100		/* every 100 ms, up to 5 seconds */
#define ADP_LOOK_MS	1000		/* a card that says nothing of its link, looked at */
#define ADP_TEXT	1024
#define ADP_F_LINK	1		/* a driver said a link or a card changed */

#ifdef X64
LOCAL CONST char *CONST adp_name[NETSEL_N] = { "vnet", "e1k", "cdcnet", "wifi", "re", "rge", "igc", "usblan" };
#else
LOCAL CONST char *CONST adp_name[NETSEL_N] = { "eth", "e1k", "cdcnet", "wifi", "re", "rge", "igc", "usblan" };
#endif

LOCAL ID	adp_mtx;
LOCAL ID	adp_flg = 0;
LOCAL UB	adp_text[ADP_TEXT];
LOCAL INT	adp_tlen = 0;
LOCAL UB	adp_told[ADP_TEXT];		/* the record without the measured values, as last told */
LOCAL INT	adp_toldlen = -1;
LOCAL TS_UUID	adp_uuid;
LOCAL BOOL	adp_obj = FALSE;
LOCAL INT	adp_select = -1;		/* the setting: -1 auto, else NETSEL_* */
LOCAL UB	if_mac[NET_MAC_LEN];		/* the address neta0 has now */
LOCAL BOOL	lk_up = FALSE;			/* the link of the card in use, as last seen */
LOCAL UW	lk_gen = 0;			/* the Wi-Fi's joins, as last seen */
#if USE_WIFI
LOCAL BOOL	wait_said = FALSE;		/* the wait for the Wi-Fi's card said */
#endif

#define LOCK()		tk_loc_mtx(adp_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(adp_mtx)

LOCAL BOOL differ( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the device object */

LOCAL INT put_mac( UB *t, INT n, CONST UB *m )
{
	static CONST char hex[] = "0123456789abcdef";
	char	s[18];
	INT	i;

	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		s[i * 3] = hex[m[i] >> 4];
		s[i * 3 + 1] = hex[m[i] & 15];
		s[i * 3 + 2] = ( i == NET_MAC_LEN - 1 ) ? '\0' : ':';
	}
	return knl_oj_put(t, n, ADP_TEXT, s);
}

/*
 * The record as the cards are now: its length. Without measured (what
 * is told of), the Wi-Fi's signal and rate are left out.
 */
LOCAL INT adp_make( UB *t, BOOL measured )
{
	T_NETSTAT	st;
	UB		m[NET_MAC_LEN];
	UH		devid = 0;
	INT		n = 0, c, speed;
	char		num[16];
	CONST T_NETDRV	*d;

	n = knl_oj_put(t, n, ADP_TEXT, "SELECT ");
	n = knl_oj_put(t, n, ADP_TEXT, ( adp_select < 0 ) ? "auto" : adp_name[adp_select]);
	n = knl_oj_put(t, n, ADP_TEXT, "\nUSING ");
	n = knl_oj_put(t, n, ADP_TEXT, adp_name[knl_netsel_card()]);
	n = knl_oj_put(t, n, ADP_TEXT, "\n");
	for ( c = 0; c < NETSEL_N; c++ ) {
		if ( knl_netsel_mac(c, m) < E_OK ) continue;
		if ( c == NETSEL_WIFI ) {
#if USE_WIFI
			n = knl_wifi_line(t, n, ADP_TEXT, measured);
#endif
			continue;
		}
		knl_memset(&st, 0, sizeof(st));
		(void)knl_netsel_stat(c, &st);
		speed = 0;
		d = knl_netsel_drv(c);
		if ( c == NETSEL_E1K ) {
			speed = knl_e1k_speed(&devid);
			if ( speed < 0 ) speed = 0;
		} else if ( d != NULL ) {
			speed = d->speed(d->ctx);
		}
		n = knl_oj_put(t, n, ADP_TEXT, "ADAPTER ");
		n = knl_oj_put(t, n, ADP_TEXT, adp_name[c]);
		n = knl_oj_put(t, n, ADP_TEXT, st.up ? " wired up " : " wired down ");
		(void)cf_itoa(speed, num);
		n = knl_oj_put(t, n, ADP_TEXT, num);
		n = knl_oj_put(t, n, ADP_TEXT, " ");
		n = put_mac(t, n, m);
		if ( c == NETSEL_VNET ) {
#if defined(X64) || defined(QEMU_VIRT)
			n = knl_oj_put(t, n, ADP_TEXT, " virtio-net\n");
#else
			n = knl_oj_put(t, n, ADP_TEXT, " Ethernet\n");
#endif
		} else if ( c == NETSEL_E1K ) {
			n = knl_oj_put(t, n, ADP_TEXT, " Intel Ethernet 8086:");
			num[0] = "0123456789abcdef"[( devid >> 12 ) & 15];
			num[1] = "0123456789abcdef"[( devid >> 8 ) & 15];
			num[2] = "0123456789abcdef"[( devid >> 4 ) & 15];
			num[3] = "0123456789abcdef"[devid & 15];
			num[4] = '\0';
			n = knl_oj_put(t, n, ADP_TEXT, num);
			n = knl_oj_put(t, n, ADP_TEXT, "\n");
		} else if ( d != NULL ) {
			n = knl_oj_put(t, n, ADP_TEXT, " ");
			n += d->label(d->ctx, t + n, ADP_TEXT - n);
			n = knl_oj_put(t, n, ADP_TEXT, "\n");
		} else {
			n = knl_oj_put(t, n, ADP_TEXT, " USB LAN\n");
		}
	}
	return n;
}

/*
 * The name of the card the stack uses, as the adapters are shown, for
 * the system object's record of the network: its length, or 0 for the
 * board's own Ethernet, which the system object names itself.
 */
EXPORT INT knl_netadp_label( UB *t, INT max )
{
	INT	c = knl_netsel_card(), n = 0;
	UH	devid = 0;
	char	num[5];
	CONST T_NETDRV *d = knl_netsel_drv(c);

	if ( d != NULL ) {
		n = d->label(d->ctx, t, max);
	} else if ( c == NETSEL_E1K ) {
		(void)knl_e1k_speed(&devid);
		n = knl_oj_put(t, n, max, "Intel Ethernet 8086:");
		num[0] = "0123456789abcdef"[( devid >> 12 ) & 15];
		num[1] = "0123456789abcdef"[( devid >> 8 ) & 15];
		num[2] = "0123456789abcdef"[( devid >> 4 ) & 15];
		num[3] = "0123456789abcdef"[devid & 15];
		num[4] = '\0';
		n = knl_oj_put(t, n, max, num);
	} else if ( c == NETSEL_CDC ) {
		n = knl_oj_put(t, n, max, "USB LAN");
#if USE_WIFI
	} else if ( c == NETSEL_WIFI ) {
		n = knl_wifi_name(t, n, max);
#endif
#if defined(X64) || defined(QEMU_VIRT)
	} else if ( c == NETSEL_VNET ) {
		n = knl_oj_put(t, n, max, "virtio-net");
#endif
	}
	return ( n > 0 ) ? n : 0;
}

LOCAL INT adp_text_of( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>The network adapters of the machine. Record 1: each with its link, "
			  "the one in use and the one chosen (the machine's setting NET_ADAPTER), as key value lines; "
			  "a change is told on it.</p>");
}

LOCAL INT adp_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(j, n, max, ",\"adapters\":true");
}

LOCAL ER adp_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	SZ	k;

	(void)ctx;
	(void)pos;
	if ( recno != OB_NET_STATE ) return E_NOEXS;
	LOCK();
	k = ( off < adp_tlen ) ? adp_tlen - off : 0;
	if ( size >= 0 && k > size ) k = size;
	if ( size > 0 && k > 0 ) knl_memcpy(buf, adp_text + off, (INT)k);
	if ( p_asize != NULL ) *p_asize = ( size == 0 ) ? adp_tlen : k;
	UNLOCK();
	return E_OK;
}

LOCAL UD adp_size( void *ctx, INT recno )
{
	(void)ctx;
	return ( recno == OB_NET_STATE ) ? (UD)adp_tlen : 0;
}

LOCAL CONST T_OBDVOPS adp_ops = { OB_S_NET, "adapters", 2, 0644, 0, 0, adp_text_of, adp_attr, adp_rea, NULL, adp_size, NULL };

/*
 * The record made again; told when it changed in more than the
 * measured values. The record is in place before the notice goes, so a
 * reader told reads what made it, and what changes after that read is
 * told again.
 */
LOCAL void adp_refresh( void )
{
	UB	*t = (UB *)Kmalloc(ADP_TEXT * 2), *k;
	INT	n, kn;
	BOOL	told;

	if ( t == NULL ) return;
	k = t + ADP_TEXT;
	n = adp_make(t, TRUE);
	kn = adp_make(k, FALSE);
	LOCK();
	if ( n != adp_tlen || differ(t, adp_text, n) ) {
		knl_memcpy(adp_text, t, n);
		adp_tlen = n;
	}
	told = ( kn != adp_toldlen || differ(k, adp_told, kn) );
	if ( told ) {
		knl_memcpy(adp_told, k, kn);
		adp_toldlen = kn;
	}
	UNLOCK();
	Kfree(t);
	if ( told && adp_obj ) knl_obdev_post(&adp_uuid, OB_NET_STATE, OB_E_CHANGE, NULL);
}

EXPORT void knl_netadp_link( void )
{
	if ( adp_flg > 0 ) (void)tk_set_flg(adp_flg, ADP_F_LINK);
}

LOCAL UD adp_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000;
}

/* ---------------------------------------------------------------- the choice */

/*
 * The machine's setting NET_ADAPTER into *p_sel: -1 auto, else NETSEL_*.
 * FALSE when the settings could not be read now, *p_sel then as it was:
 * an object is not opened for reading while another holds it with every
 * other key kept out (OB_O_EXCL, E_BUSY), or the medium failed, and such
 * a moment is not a choice of 自動. The writers of the machine's settings
 * keep only other writers out (OB_O_WEXCL).
 */
LOCAL BOOL setting( INT *p_sel )
{
	TS_UUID	u;
	ID	key;
	UB	*text = NULL;
	char	v[CF_VAL_MAX];
	INT	c, sel = -1, len = -1;
	static ER said = E_OK;

	if ( ob_fnd_sys(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u) < E_OK ) {
		*p_sel = -1;			/* no machine's settings: auto */
		return TRUE;
	}
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key > 0 ) {
		/* the machine's settings whole: the line is found wherever it is in them */
		len = knl_ob_rea_all(key, 1, 0, &text, NULL);
		ob_cls_obj(key);
	}
	if ( key <= 0 || len < 0 ) {
		ER	er = ( key <= 0 ) ? (ER)key : (ER)len;

		if ( text != NULL ) Kfree(text);
		if ( er != said ) {
			said = er;
			tm_printf((UB *)"net: the machine's settings not read now (%d): the adapter chosen stays\n", (INT)er);
		}
		return FALSE;
	}
	said = E_OK;
	if ( len > 0 && cf_get(text, len, "NET_ADAPTER", 0, v, sizeof(v)) ) {
		for ( c = 0; c < NETSEL_N; c++ ) {
			if ( knl_strcmp(v, adp_name[c]) == 0 ) sel = c;
		}
	}
	if ( text != NULL ) Kfree(text);
	*p_sel = sel;
	return TRUE;
}

LOCAL BOOL up( INT c )
{
	T_NETSTAT	st;

	knl_memset(&st, 0, sizeof(st));
	return knl_netsel_stat(c, &st) >= E_OK && st.up;
}

/*
 * The card can carry the stack: it is there, and its own address is
 * known. The Wi-Fi's address is a stand-in until its card has started
 * (the firmware read, the address read from the card); Linux has no
 * interface of the card until then, and NetworkManager none to use. The
 * stack is not moved onto it before, so that nothing is asked for with
 * the stand-in, and the address is not changed twice.
 */
LOCAL BOOL usable( INT c, UB *m )
{
	if ( knl_netsel_mac(c, m) < E_OK ) return FALSE;
#if USE_WIFI
	if ( c == NETSEL_WIFI && !knl_wifi_mac_known() ) return FALSE;
#endif
	return TRUE;
}

/*
 * The card to use now. The person's choice of a card not usable yet
 * (the Wi-Fi's card still starting, or not started) is kept: the
 * setting is not changed, the card is taken once it is usable, and
 * meanwhile the stack stays where the automatic choice has it
 */
LOCAL INT want( INT cur )
{
	UB	m[NET_MAC_LEN];
	INT	c;

	if ( adp_select >= 0 && usable(adp_select, m) ) {
		return adp_select;		/* the person's choice, while the card is there */
	}
#if USE_WIFI
	if ( adp_select == NETSEL_WIFI && knl_netsel_mac(NETSEL_WIFI, m) >= E_OK && !wait_said ) {
		wait_said = TRUE;
		tm_printf((UB *)"net: the Wi-Fi chosen, its card not started yet: %s stays in use until it has\n",
			  adp_name[cur]);
	}
#endif
	for ( c = NETSEL_VNET; c < NETSEL_N; c++ ) {
		if ( c != NETSEL_WIFI && up(c) ) return c;	/* a wired card whose link is up */
	}
#if USE_WIFI
	if ( knl_wifi_connected() && usable(NETSEL_WIFI, m) ) return NETSEL_WIFI;
#endif
	if ( usable(cur, m) ) return cur;
	c = knl_netsel_first();
	return ( usable(c, m) || c == cur ) ? c : cur;
}

/*
 * The link is new: the IPv6 routers asked to speak again (so_slaac.c),
 * and the address asked for again when it comes from DHCP. The kernel's
 * tests leave the address to themselves, but for the Wi-Fi simulator's
 * (WIFI_SIM=1), which follow the Wi-Fi as a machine does
 */
LOCAL void readdress( CONST char *why )
{
	if ( knl_so_slaac_relink != NULL ) knl_so_slaac_relink();
#if !USE_KTEST || WIFI_SIM
	if ( so_dhcp_on() ) {
		tm_printf((UB *)"net: %s: the address is asked for again (DHCP)\n", why);
		(void)so_dhcp_start();
	}
#else
	(void)why;
#endif
}

/* The stack moved onto card c: neta0 given its address, the address asked for again */
LOCAL void move( INT from, INT c )
{
	UB	m[NET_MAC_LEN];

	if ( knl_netsel_mac(c, m) < E_OK ) return;
#if USE_WIFI
	if ( differ(m, if_mac, NET_MAC_LEN) ) {
		INT	er = rump_tswifi_setmac("neta0", m);

		if ( er != 0 ) {
			tm_printf((UB *)"net: the adapter stays %s: neta0's address could not be changed (%d)\n",
				  adp_name[from], er);
			return;
		}
		knl_memcpy(if_mac, m, NET_MAC_LEN);
	}
#else
	if ( differ(m, if_mac, NET_MAC_LEN) ) {
		return;				/* neta0's address cannot be changed in this build */
	}
#endif
	knl_netsel_set(c);
	tm_printf((UB *)"net: adapter %s -> %s, address %02x:%02x:%02x:%02x:%02x:%02x\n",
		  adp_name[from], adp_name[c], m[0], m[1], m[2], m[3], m[4], m[5]);
#if !USE_KTEST || WIFI_SIM
	(void)so_conf(SO_CONF_APPLY);
#endif
	readdress("another adapter");	/* so_conf does not ask again while DHCP is on */
	lk_up = up(c);
#if USE_WIFI
	lk_gen = knl_wifi_link_gen();
#endif
}

/* ---------------------------------------------------------------- the task */

LOCAL void adp_task( INT stacd, void *exinf )
{
	TS_UUID	u;
	ID	key;
	UB	mac[NET_MAC_LEN];
	INT	cur, w;
	UINT	ptn;
	UD	now, conf_at, look_at, at;

	(void)stacd;
	(void)exinf;
	/* the object layer, and the machine's settings in it: looked for, and let go at once */
	(void)ob_fnd_sys(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u);
	while ( ( key = ob_opn_obj(&u, OB_OP_ATRRD) ) <= 0 ) {
		tk_dly_tsk(ADP_START_MS);
	}
	ob_cls_obj(key);

	/* the choice as the settings have it, asked again while they are being written */
	for ( w = 0; w < ADP_FIRST_TRIES && !setting(&adp_select); w++ ) {
		tk_dly_tsk(ADP_FIRST_MS);
	}
	lk_up = up(knl_netsel_card());	/* the link as the start found it: asked for at the start */
#if USE_WIFI
	lk_gen = knl_wifi_link_gen();
#endif
	adp_refresh();
	if ( knl_obdev_add((CONST UB *)"netadp", NULL, &adp_ops, NULL, &adp_uuid) >= E_OK ) {
		adp_obj = TRUE;
	}
	tm_printf((UB *)"net: adapters: %s chosen, %s in use\n",
		  ( adp_select < 0 ) ? "auto" : adp_name[adp_select], adp_name[knl_netsel_card()]);
#if USE_WIFI
	knl_wifi_start();
#endif
	conf_at = look_at = adp_now();
	for ( ;; ) {
		/*
		 * Until a driver says a link or a card changed, the time to read
		 * the setting again, or, while a card says nothing of its link,
		 * the time to look at it
		 */
		now = adp_now();
		at = conf_at + ADP_CONF_MS;
		if ( knl_netsel_silent() && look_at + ADP_LOOK_MS < at ) at = look_at + ADP_LOOK_MS;
		(void)tk_wai_flg(adp_flg, ADP_F_LINK, TWF_ORW | TWF_BITCLR, &ptn,
				 ( at > now ) ? (TMO)( at - now ) : TMO_POL);
		now = adp_now();
		if ( now >= look_at + ADP_LOOK_MS ) look_at = now;
		/*
		 * The setting read again, the object opened only for the read:
		 * an object held open keeps its volume from being taken off (the
		 * system volume's, at a shutdown or a check)
		 */
		if ( now >= conf_at + ADP_CONF_MS ) {
			conf_at = now;
			w = adp_select;
			(void)setting(&w);
			if ( w != adp_select ) {
				adp_select = w;
				tm_printf((UB *)"net: adapter chosen: %s\n", ( w < 0 ) ? "auto" : adp_name[w]);
			}
		}
		cur = knl_netsel_card();
		w = want(cur);
		/* another card, or the same one with another address (the Wi-Fi's, read from its card) */
		if ( w != cur || ( knl_netsel_mac(w, mac) >= E_OK && differ(mac, if_mac, NET_MAC_LEN) ) ) {
			move(cur, w);
		} else {
			/* the link of the card in use come up again, or the Wi-Fi joined again */
			BOOL	u = up(cur);
#if USE_WIFI
			UW	g = knl_wifi_link_gen();

			if ( cur == NETSEL_WIFI && g != lk_gen ) {
				lk_gen = g;
				if ( u ) readdress("the Wi-Fi joined a network");
			} else if ( g != lk_gen ) {
				/* joined with the stack on another card: why it stays there, for the records */
				lk_gen = g;
				if ( knl_wifi_connected() ) {
					tm_printf((UB *)"net: the Wi-Fi joined a network, the stack stays on %s"
						  " (chosen %s, the card's address %s)\n", adp_name[cur],
						  ( adp_select < 0 ) ? "auto" : adp_name[adp_select],
						  knl_wifi_mac_known() ? "read" : "not read yet");
				}
			} else
#endif
			if ( u && !lk_up ) {
				readdress("the link came up");
			}
			lk_up = u;
		}
		adp_refresh();
	}
}

EXPORT void knl_netadp_start( void )
{
	T_CMTX	cm;
	T_CFLG	cf;
	T_CTSK	ct;
	ID	t;

	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	adp_mtx = tk_cre_mtx(&cm);
	if ( adp_mtx <= 0 ) return;
	knl_memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WMUL;
	adp_flg = tk_cre_flg(&cf);
	if ( adp_flg <= 0 ) return;
	(void)knl_netsel_mac(knl_netsel_first(), if_mac);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)adp_task;
	ct.itskpri = ADP_PRI;
	ct.stksz = ADP_STKSZ;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}

#endif /* X64 || USE_NETSEL */
