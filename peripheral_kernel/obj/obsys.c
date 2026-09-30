/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsys.c
 *	The system as an object (design 18.7, 16.5.23)
 *
 *	A virtual device of the device manager, as the clock is: the machine
 *	itself (ob_uuid_system). Each record past the description is lines
 *	of "key value" text (include/ts/conf.h), made when it is read:
 *
 *	  1 OB_SYS_INFO     the system: its name and version, the board, the
 *			    processors, the memory, how long it has run
 *	  2 OB_SYS_POWER    written "OFF" or "RESTART": the desktop saves and
 *			    closes its windows and the machine goes off or starts
 *			    again; read, what it can do
 *	  3 OB_SYS_DISPLAY  the screen: the object 画面 named, and its record
 *			    1 -- its size, its pixels, what drives it, the
 *			    sizes it can be set to (obdisp.c)
 *	  4 OB_SYS_USB      a line for each device on the USB buses, made
 *			    from the objects of those devices (obusb.c)
 *	  5 OB_SYS_SCHEME   the colour scheme in use and those there are;
 *			    written a number, that one is used
 *	  6 OB_SYS_WALL     the wallpaper, as the object its picture is;
 *			    written a UUID (and how it is laid), that one
 *	  7 OB_SYS_NET      the network: the card, its link, the address
 *			    the interface has, and how many sockets other
 *			    programs hold open
 *	  8 OB_SYS_SOUND    what plays sound, and how
 *	  9 OB_SYS_HW       ハードウェアの状態: the temperature of the chip and
 *			    the rates of the ARM and V3D clocks, which the
 *			    Raspberry Pi's firmware tells; unknown on QEMU
 *
 *	Reading is everyone's; writing is the administrators' (the
 *	object's protection, rw-rw-r-- with their group).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/disp.h>
#include <ts/usb.h>
#include <ts/net.h>
#include <ts/so.h>
#include <ts/snd.h>
#include <ts/wm.h>
#include <ts/xf.h>
#include <ts/conf.h>
#include "sysman/pfalloc.h"
#include "obj.h"

IMPORT INT	knl_num_prc;

LOCAL FP	sys_power = NULL;	/* what the desktop gives to be called: (INT how) */
LOCAL TS_UUID	sys_wall;		/* the wallpaper's object, as last laid */
LOCAL BOOL	sys_walled = FALSE;
LOCAL INT	sys_wall_mode = WM_WALL_FIT;

/* A line "key value" added to t */
LOCAL INT put( UB *t, INT n, INT max, CONST char *key, CONST char *val )
{
	n = knl_oj_put(t, n, max, key);
	n = knl_oj_put(t, n, max, "\t");
	n = knl_oj_put(t, n, max, val);
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT put_num( UB *t, INT n, INT max, CONST char *key, D v )
{
	n = knl_oj_put(t, n, max, key);
	n = knl_oj_put(t, n, max, "\t");
	n = knl_oj_put_num(t, n, max, v);
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT put_ip( UB *t, INT n, INT max, CONST char *key, UW a )
{
	char	s[16];

	cf_ipstr(a, s);
	return put(t, n, max, key, s);
}

LOCAL INT hex( UB *t, INT n, INT max, UW v, INT digits )
{
	char	s[12];
	INT	i;

	for ( i = 0; i < digits; i++ ) {
		UW	d = ( v >> ( ( digits - 1 - i ) * 4 ) ) & 0xF;

		s[i] = (char)( d < 10 ? '0' + d : 'a' + d - 10 );
	}
	s[i] = 0;
	return knl_oj_put(t, n, max, s);
}

/* The system's version, R and a number, a point and three digits (make TS_VERSION=) */
#ifndef TS_VERSION
#define TS_VERSION	"R1.000"
#endif

/* ---------------------------------------------------------------- the records */

LOCAL INT info_text( UB *t, INT max )
{
	UD	ns = 0;
	INT	n = 0;

	n = put(t, n, max, "NAME", "TessronOS");
	n = put(t, n, max, "VERSION", TS_VERSION);
	n = knl_oj_put(t, n, max, "KERNEL\tmicroT-Kernel ");
	n = knl_oj_put_num(t, n, max, VER_MAJOR);
	n = knl_oj_put(t, n, max, ".");
	n = knl_oj_put_num(t, n, max, VER_MINOR);
	n = knl_oj_put(t, n, max, "\n");
	n = put(t, n, max, "BUILD", __DATE__ " " __TIME__);
#ifdef RPI5
	n = put(t, n, max, "BOARD", "Raspberry Pi 5");
#else
	n = put(t, n, max, "BOARD", "QEMU virt");
#endif
	n = put(t, n, max, "CPU", "Arm Cortex-A76 (AArch64)");
	n = put_num(t, n, max, "CPUS", knl_num_prc);
	n = put_num(t, n, max, "MEMORY_MB", (D)( knl_pf_total_count(-1) >> 8 ));
	n = put_num(t, n, max, "FREE_MB", (D)( knl_pf_free_count(-1) >> 8 ));
	(void)ts_get_mono(&ns);
	n = put_num(t, n, max, "UPTIME_S", (D)( ns / 1000000000ULL ));
	return n;
}

/* The state of the hardware, as the firmware tells it (the property tags) */
#ifdef RPI5
IMPORT ER knl_fw_value( UW tag, UW id, UW *p_val );	/* kernel/sysdepend/rpi5/mbox.c */
#define TAG_GET_TEMPERATURE	0x00030006U
#define TAG_GET_CLOCK_RATE	0x00030002U
#define CLOCK_ARM		3
#define CLOCK_V3D		5
#endif

LOCAL INT hw_text( UB *t, INT max )
{
	INT	n = 0;
#ifdef RPI5
	UW	v = 0;

	if ( knl_fw_value(TAG_GET_TEMPERATURE, 0, &v) >= E_OK ) {
		n = knl_oj_put(t, n, max, "TEMPERATURE\t");
		n = knl_oj_put_num(t, n, max, v / 1000);
		n = knl_oj_put(t, n, max, ".");
		n = knl_oj_put_num(t, n, max, ( v % 1000 ) / 100);
		n = knl_oj_put(t, n, max, "\n");
		n = put_num(t, n, max, "TEMPERATURE_MC", v);
	} else {
		n = put(t, n, max, "TEMPERATURE", "unknown");
	}
	if ( knl_fw_value(TAG_GET_CLOCK_RATE, CLOCK_ARM, &v) >= E_OK ) {
		n = put_num(t, n, max, "ARM_HZ", v);
	} else {
		n = put(t, n, max, "ARM_HZ", "unknown");
	}
	if ( knl_fw_value(TAG_GET_CLOCK_RATE, CLOCK_V3D, &v) >= E_OK ) {
		n = put_num(t, n, max, "V3D_HZ", v);
	} else {
		n = put(t, n, max, "V3D_HZ", "unknown");
	}
#else
	/* QEMU has no sensor and no firmware to ask */
	n = put(t, n, max, "TEMPERATURE", "unknown");
	n = put(t, n, max, "ARM_HZ", "unknown");
	n = put(t, n, max, "V3D_HZ", "unknown");
#endif
	return n;
}

EXPORT INT knl_obhw_text( UB *t, INT max )
{
	return hw_text(t, max);
}

LOCAL INT scheme_text( UB *t, INT max )
{
	INT	n = 0, i;

	n = put_num(t, n, max, "SCHEME", wm_scheme());
	for ( i = 0; i < WM_SCHEME_MAX; i++ ) {
		CONST char *nm = wm_scheme_name((UINT)i);

		if ( nm != NULL ) n = put(t, n, max, "NAME", nm);
	}
	return n;
}

LOCAL INT wall_text( UB *t, INT max )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = 0;

	if ( sys_walled && ts_uuid_to_str(&sys_wall, us, sizeof(us)) >= E_OK ) {
		n = put(t, n, max, "WALLPAPER", us);
	}
	return put_num(t, n, max, "MODE", sys_wall_mode);
}

LOCAL INT net_text( UB *t, INT max )
{
	T_NETSTAT	st;
	UB		mac[6];
	UINT		a = 0, m = 0, g = 0, d = 0;
	INT		n = 0, i;

#ifdef RPI5
	n = put(t, n, max, "ADAPTER", "RP1 Gigabit Ethernet (GEM)");
#else
	n = put(t, n, max, "ADAPTER", "virtio-net");
#endif
	if ( net_get_mac(mac) >= E_OK ) {
		n = knl_oj_put(t, n, max, "MAC\t");
		for ( i = 0; i < 6; i++ ) {
			n = hex(t, n, max, mac[i], 2);
			if ( i < 5 ) n = knl_oj_put(t, n, max, ":");
		}
		n = knl_oj_put(t, n, max, "\n");
	}
	if ( net_stat(&st) >= E_OK ) {
		n = put(t, n, max, "LINK", st.up ? "up" : "down");
		n = put_num(t, n, max, "SENT", (D)st.sent);
		n = put_num(t, n, max, "RECEIVED", (D)st.recv);
	}
	if ( so_getifaddr((UW *)&a, (UW *)&m, (UW *)&g) >= E_OK ) {
		n = put_ip(t, n, max, "ADDRESS", a);
		n = put_ip(t, n, max, "MASK", m);
		n = put_ip(t, n, max, "GATEWAY", g);
		if ( so_getdns((UW *)&d) >= E_OK ) n = put_ip(t, n, max, "DNS", d);
		n = put(t, n, max, "DHCP", so_dhcp_on() ? "on" : "off");
	}
	/* the sockets of programs other than the one reading this */
	n = put_num(t, n, max, "BUSY", so_busy(ts_get_pid()));
	{
		D	ms = 0;

		if ( so_sntp_state(&ms) >= E_OK ) n = put_num(t, n, max, "NTP_SET_S", ms / 1000);
	}
	return n;
}

LOCAL INT sound_text( UB *t, INT max )
{
	INT	n = 0;
	ID	dd = tk_opn_dev((UB *)SND_DEVNM, TD_READ);
	SDStat	st;
	SZ	asz = 0;

	if ( dd < E_OK ) {
		return put(t, n, max, "DEVICE", "none");
	}
	n = put(t, n, max, "DEVICE", SND_DEVNM);
	if ( tk_srea_dev(dd, DN_SDSTAT, &st, sizeof(st), &asz) >= E_OK ) {
		n = put(t, n, max, "OUTPUT", ( st.backend == SND_BE_USB ) ? "USB audio"
				     : ( st.backend == SND_BE_I2S ) ? "I2S" : "none now");
		n = put_num(t, n, max, "RATE", st.out_rate);
		n = put_num(t, n, max, "CHANNELS", st.out_channels);
	}
	tk_cls_dev(dd, 0);
	return n;
}

/* ---------------------------------------------------------------- for the device manager */

EXPORT INT knl_obsys_text( INT recno, UB *t, INT max )
{
	switch ( recno ) {
	case OB_SYS_INFO:	return info_text(t, max);
	case OB_SYS_POWER:	return put(t, 0, max, "CAN", sys_power != NULL ? "OFF RESTART" : "");
	case OB_SYS_DISPLAY:	return knl_obdisp_text(t, max);
	case OB_SYS_USB:	return knl_obusb_text(t, max);
	case OB_SYS_SCHEME:	return scheme_text(t, max);
	case OB_SYS_WALL:	return wall_text(t, max);
	case OB_SYS_NET:	return net_text(t, max);
	case OB_SYS_SOUND:	return sound_text(t, max);
	case OB_SYS_HW:		return hw_text(t, max);
	default:		return -1;
	}
}

/* A word of what was written, as a string */
LOCAL void first_word( CONST UB *buf, SZ size, INT nth, char *out, INT max )
{
	char	s[CF_VAL_MAX];
	INT	i;

	for ( i = 0; i < (INT)size && i < (INT)sizeof(s) - 1 && buf[i] != '\n' && buf[i] != 0; i++ ) {
		s[i] = (char)buf[i];
	}
	s[i] = 0;
	if ( !cf_word(s, nth, out, max) ) out[0] = 0;
}

EXPORT ER knl_obsys_write( INT recno, CONST UB *buf, SZ size )
{
	char	w[64];
	ER	er;

	first_word(buf, size, 0, w, sizeof(w));
	switch ( recno ) {
	case OB_SYS_POWER:
		if ( sys_power == NULL ) return E_NOSPT;
		if ( knl_strcmp(w, "OFF") == 0 ) return (*(ER (*)(INT))sys_power)(0);
		if ( knl_strcmp(w, "RESTART") == 0 ) return (*(ER (*)(INT))sys_power)(1);
		return E_PAR;
	case OB_SYS_SCHEME:
		er = wm_set_scheme((UINT)cf_num(w, -1));
		if ( er >= E_OK ) knl_obsys_changed(OB_SYS_SCHEME);
		return er;
	case OB_SYS_WALL:
	{
		TS_UUID	u;
		char	m[8];
		INT	mode;

		if ( ts_str_to_uuid(w, &u) < E_OK ) return E_PAR;
		first_word(buf, size, 1, m, sizeof(m));
		mode = cf_num(m, sys_wall_mode);
		er = wm_load_wall_obj(&u, xf_data_rec(&u), (UINT)mode);
		if ( er >= E_OK ) {
			sys_wall = u;
			sys_walled = TRUE;
			sys_wall_mode = mode;
			knl_obsys_changed(OB_SYS_WALL);
		}
		return er;
	}
	default:
		return E_RONLY;
	}
}

/* What the desktop gives to be called to end the session: how 0 off, 1 start again */
EXPORT void knl_obsys_power( FP fn )
{
	sys_power = fn;
}

/* The wallpaper laid by the desktop itself at its start */
EXPORT void knl_obsys_wall( CONST TS_UUID *u, INT mode )
{
	sys_wall = *u;
	sys_walled = TRUE;
	sys_wall_mode = mode;
}

/* Something the system object shows changed: told to those who watch it */
EXPORT void knl_obsys_changed( INT recno )
{
	knl_ob_post(&ob_uuid_system, recno, OB_E_CHANGE, NULL);
}
