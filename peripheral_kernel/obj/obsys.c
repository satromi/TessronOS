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
 *	  7 OB_SYS_NET      the network: the stack's host name, the card's
 *			    interface's number, the card, its link, the address
 *			    the interface has, its IPv6 addresses with their
 *			    lifetimes, router and name servers, and how many
 *			    sockets other programs hold open
 *	  8 OB_SYS_SOUND    what plays sound, and how: the output playing,
 *			    the one chosen, and every output with its kind,
 *			    flags and name (OUT, OUT_KIND, OUT_FLAGS,
 *			    OUT_NAME, a set for each); written "SELECT id",
 *			    that output is chosen (include/ts/snd.h);
 *			    written "PLAY what" (buzzer, press, release,
 *			    tick, long, test; "tone" after it for an
 *			    object's tone), that sound is played on it and
 *			    the write answers whether it sounds (beep.c);
 *			    KEEP says whether the stream is kept going in
 *			    silence, and "KEEP 1" or "KEEP 0" written sets it
 *	  9 OB_SYS_HW       ハードウェアの状態: the temperature of the chip and
 *			    the rates of the ARM and V3D clocks, which the
 *			    Raspberry Pi's firmware tells, unknown on QEMU;
 *			    on a PC the temperature, the clock the processors
 *			    run at (CLOCK_MHZ, their mean, and CLOCK_MOST_MHZ)
 *			    from their counters and the base and turbo clocks
 *			    (BASE_MHZ, TURBO_MHZ) they name;
 *			    the limit of the processors' clock (CPU_MHZ) and
 *			    the range the firmware allows, the fan's slowing,
 *			    what the firmware says of the supply, and the
 *			    watchdog's seconds
 *
 *	Reading is everyone's; writing is the administrators' and the
 *	ordinary users' (the object's protection, rw-rw-r-- with the
 *	administrators' group, and the ordinary users' group let write).
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
#include <ts/board.h>
#include <ts/cpuclk.h>
#include <ts/lockup.h>
#include "sysman/pfalloc.h"
#include "obj.h"

IMPORT INT	knl_num_prc;
IMPORT INT	knl_netadp_label( UB *t, INT max ) __attribute__((weak));	/* device/net/netadp.c */

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

IMPORT void knl_snd_arm( void );		/* device/snd: told of what plays sound as it changes */

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
#define TS_VERSION	"R1.110"
#endif

/* ---------------------------------------------------------------- the records */

/* A set of the first k processors as the machine's settings write it: "1,3-4" */
EXPORT INT knl_prcset_text( UB *t, INT n, INT max, CONST T_PRCSET *s, INT k )
{
	INT	i, j;
	BOOL	first = TRUE;

	for ( i = prcset_next(s, 0, k); i >= 0; i = prcset_next(s, j + 1, k) ) {
		for ( j = i; j + 1 < k && prcset_has(s, j + 1); j++ ) ;
		if ( !first ) n = knl_oj_put(t, n, max, ",");
		n = knl_oj_put_num(t, n, max, i + 1);
		if ( j > i ) {
			n = knl_oj_put(t, n, max, "-");
			n = knl_oj_put_num(t, n, max, j + 1);
		}
		first = FALSE;
	}
	return n;
}

/*
 * The devices' interrupts (design 4.5), a line for each device whose
 * driver named them: IRQ, the device's name, its object's UUID, the
 * processor its object asks for (0 none), and each interrupt with the
 * processor it goes to now ("80:1,81:2")
 */
LOCAL INT irq_lines( UB *t, INT n, INT max )
{
	UB	(*nm)[L_DEVNM + 1] = NULL;
	UINT	*intno = NULL;
	ID	*prc = NULL, want;
	TS_UUID	u;
	INT	k, d, i, c, room = 0, cap;

	/*
	 * as many devices as there are: a list that did not hold them all
	 * (the count it answers is larger than its room) is asked for again
	 * in more room
	 */
	for ( cap = 16; ; cap = k * 2 ) {
		nm = Kmalloc(sizeof(*nm) * (SZ)cap);
		if ( nm == NULL ) {
			return E_NOMEM;
		}
		k = knl_int_dev_names(nm, cap);
		if ( k <= cap ) break;
		Kfree(nm);
	}
	for ( d = 0; d < k && n >= 0; d++ ) {
		c = knl_int_dev_info(nm[d], &want, NULL, NULL, 0);
		if ( c > room ) {
			if ( intno != NULL ) Kfree(intno);
			if ( prc != NULL ) Kfree(prc);
			intno = Kmalloc(sizeof(UINT) * (SZ)c);
			prc = Kmalloc(sizeof(ID) * (SZ)c);
			room = c;
			if ( intno == NULL || prc == NULL ) {
				n = E_NOMEM;
				break;
			}
		}
		c = knl_int_dev_info(nm[d], &want, intno, prc, room);
		if ( c > room ) c = room;		/* one came between the two asks */
		n = knl_oj_put(t, n, max, "IRQ\t");
		n = knl_oj_put(t, n, max, (CONST char *)nm[d]);
		n = knl_oj_put(t, n, max, " ");
		if ( knl_obdev_uuid(nm[d], &u) >= E_OK ) {
			char	us[TS_UUID_STRLEN + 1];

			(void)ts_uuid_to_str(&u, us, sizeof(us));
			n = knl_oj_put(t, n, max, us);
		} else {
			n = knl_oj_put(t, n, max, "-");
		}
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put_num(t, n, max, want);
		n = knl_oj_put(t, n, max, " ");
		for ( i = 0; i < c; i++ ) {
			if ( i > 0 ) n = knl_oj_put(t, n, max, ",");
			n = knl_oj_put_num(t, n, max, intno[i]);
			n = knl_oj_put(t, n, max, ":");
			n = knl_oj_put_num(t, n, max, prc[i]);
		}
		n = knl_oj_put(t, n, max, "\n");
	}
	if ( nm != NULL ) Kfree(nm);
	if ( intno != NULL ) Kfree(intno);
	if ( prc != NULL ) Kfree(prc);
	return n;
}

/* The processors' roles as the system took them (design 4.5) */
LOCAL INT prc_roles( UB *t, INT n, INT max )
{
	T_PRCSET	c, r;

	knl_cpu_roles_get(&c, &r);
	n = knl_oj_put(t, n, max, "CPU_CHORE\t");
	n = knl_prcset_text(t, n, max, &c, knl_num_prc);
	n = knl_oj_put(t, n, max, "\nCPU_RT\t");
	n = knl_prcset_text(t, n, max, &r, knl_num_prc);
	n = knl_oj_put(t, n, max, "\n");
	return irq_lines(t, n, max);
}

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
	/* the board as the device tree and the firmware's revision name it (board.c) */
	n = put(t, n, max, "BOARD", ( knl_board.name != NULL ) ? knl_board.name : "unknown");
	if ( knl_board.rev != 0 ) {
		n = knl_oj_put(t, n, max, "BOARD_REV\t");
		n = hex(t, n, max, knl_board.rev, 6);
		n = knl_oj_put(t, n, max, "\n");
	}
#ifdef X64
	{
		IMPORT void knl_cpu_brand( char *b, INT max );	/* smp.c */
		IMPORT INT  knl_cpu_cores( INT *p_pkgs );
		char	brand[64];
		INT	pkgs = 0, cores = knl_cpu_cores(&pkgs);

		knl_cpu_brand(brand, sizeof(brand));
		n = put(t, n, max, "CPU", ( brand[0] != '\0' ) ? brand : "x86-64");
		n = put_num(t, n, max, "CPUS", knl_num_prc);
		n = put_num(t, n, max, "CPU_CORES", cores);		/* the threads of a core count once */
		n = put_num(t, n, max, "CPU_PACKAGES", pkgs);
	}
#elif defined(_RPI4_)
	n = put(t, n, max, "CPU", "Arm Cortex-A72 (AArch64)");	/* BCM2711 */
	n = put_num(t, n, max, "CPUS", knl_num_prc);
#else
	n = put(t, n, max, "CPU", "Arm Cortex-A76 (AArch64)");
	n = put_num(t, n, max, "CPUS", knl_num_prc);
#endif
	n = prc_roles(t, n, max);
	n = put_num(t, n, max, "MEMORY_MB", (D)( knl_pf_total_count(-1) >> 8 ));
	n = put_num(t, n, max, "FREE_MB", (D)( knl_pf_free_count(-1) >> 8 ));
	(void)ts_get_mono(&ns);
	n = put_num(t, n, max, "UPTIME_S", (D)( ns / 1000000000ULL ));
	return n;
}

/* The state of the hardware, as the firmware tells it (the property tags) */
#ifdef RPI_VCFW
IMPORT ER knl_fw_value( UW tag, UW id, UW *p_val );	/* kernel/sysdepend/rpi/mbox.c */
#ifdef RPI5
IMPORT ER knl_fan_state( INT *p_level, INT *p_speed, BOOL *p_pwm );	/* kernel/sysdepend/rpi5/fan.c */
#endif
#define TAG_GET_TEMPERATURE	0x00030006U
#define TAG_GET_CLOCK_RATE	0x00030002U
#define CLOCK_ARM		3
#define CLOCK_V3D		5
#endif
#ifdef CPU_CORE_X86_64
IMPORT ER knl_cpu_x64_clock( UW *p_mean, UW *p_most );	/* kernel/sysdepend/cpu/core/x86_64/cpuclk.c */
IMPORT ER knl_cpu_x64_rated( UW *p_base, UW *p_most );
IMPORT ER knl_cpu_x64_temp( INT *p_deg );
#endif

/*
 * What the firmware says of the supply: low now, low at some time since
 * the start, or never
 */
LOCAL CONST char *undervolt( UW thr )
{
	return ( thr == 0xFFFFFFFFU ) ? "unknown" : ( thr & CPU_THR_UNDERVOLT ) ? "now"
		: ( thr & CPU_THR_UNDERVOLT_SEEN ) ? "before" : "no";
}

LOCAL INT hw_text( UB *t, INT max )
{
	INT		n = 0, wd;
	BOOL		hw = FALSE;
	T_CPUCLK	clk;
#ifdef RPI_VCFW
	UW	v = 0;
#endif

	knl_cpu_clock(&clk);
#ifdef RPI_VCFW
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
	if ( clk.hz != 0 ) {
		n = put_num(t, n, max, "ARM_HZ", clk.hz);
	} else {
		n = put(t, n, max, "ARM_HZ", "unknown");
	}
	if ( knl_fw_value(TAG_GET_CLOCK_RATE, CLOCK_V3D, &v) >= E_OK ) {
		n = put_num(t, n, max, "V3D_HZ", v);
	} else {
		n = put(t, n, max, "V3D_HZ", "unknown");
	}
#ifdef RPI5
	{
		INT	level = 0, speed = 0;
		BOOL	pwm = FALSE;

		if ( knl_fan_state(&level, &speed, &pwm) >= E_OK ) {
			n = put_num(t, n, max, "FAN_LEVEL", level);
			n = put(t, n, max, "FAN_DRIVE", pwm ? "pwm" : "onoff");
			if ( speed >= 0 ) {
				n = put_num(t, n, max, "FAN_SPEED", speed);
			}
		}
	}
#endif
#elif defined(CPU_CORE_X86_64)
	/*
	 * A PC: the processors' own counters and sensor, read by the kernel
	 * (cpuclk.c), in MHz, beyond what ARM_HZ's 32 bits of Hz hold; none
	 * on a virtual machine
	 */
	{
		UW	mean = 0, most = 0, base = 0, top = 0;
		INT	deg = 0;

		if ( knl_cpu_x64_temp(&deg) >= E_OK ) {
			n = put_num(t, n, max, "TEMPERATURE", deg);
			n = put_num(t, n, max, "TEMPERATURE_MC", (D)deg * 1000);
		} else {
			n = put(t, n, max, "TEMPERATURE", "unknown");
		}
		n = put(t, n, max, "ARM_HZ", "unknown");
		if ( knl_cpu_x64_clock(&mean, &most) >= E_OK ) {
			n = put_num(t, n, max, "CLOCK_MHZ", mean);
			n = put_num(t, n, max, "CLOCK_MOST_MHZ", most);
		}
		if ( knl_cpu_x64_rated(&base, &top) >= E_OK ) {
			n = put_num(t, n, max, "BASE_MHZ", base);
			n = put_num(t, n, max, "TURBO_MHZ", top);
		}
	}
#else
	/* QEMU has no sensor and no firmware to ask */
	n = put(t, n, max, "TEMPERATURE", "unknown");
	n = put(t, n, max, "ARM_HZ", "unknown");
	n = put(t, n, max, "V3D_HZ", "unknown");
#endif
	/* the processors' clock as the settings limit it (cpuclk.c), and the supply */
	n = put_num(t, n, max, "ARM_LIMIT_MHZ", clk.limit_mhz);
	if ( clk.max_hz != 0 ) {
		n = put_num(t, n, max, "ARM_MIN_HZ", clk.min_hz);
		n = put_num(t, n, max, "ARM_MAX_HZ", clk.max_hz);
	}
	n = put_num(t, n, max, "ARM_HEAT_HZ", clk.heat_hz);
	if ( clk.throttled != 0xFFFFFFFFU ) {
		n = knl_oj_put(t, n, max, "THROTTLED\t0x");
		n = hex(t, n, max, clk.throttled, 8);
		n = knl_oj_put(t, n, max, "\n");
	}
	n = put(t, n, max, "UNDERVOLT", undervolt(clk.throttled));
	/* the watchdog: seconds from a stopped machine found to its reset, 0 off (lockup.c) */
	wd = knl_wdog_get(&hw);
	n = put_num(t, n, max, "WATCHDOG_S", wd);
	n = put(t, n, max, "WATCHDOG_HW", hw ? "yes" : "no");
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

/* A lifetime in seconds, or "infinite" */
LOCAL INT life_word( char *s, INT at, UINT life )
{
	CONST char *inf = "infinite";

	s[at++] = ' ';
	if ( life == 0xffffffffU ) {
		while ( *inf != 0 ) s[at++] = *inf++;
		s[at] = 0;
		return at;
	}
	return at + cf_itoa((INT)life, s + at);
}

/*
 * IPv6: how the settings have it, then a line for each address of the
 * card's interface -- the address and its prefix length, what kind it
 * is, the seconds left of its valid and preferred lifetimes, and what
 * is wrong with it if anything -- the router of the default route and
 * the name servers asked.
 */
LOCAL INT net6_text( UB *t, INT n, INT max )
{
	T_SOIF6	*l = NULL;
	UB	gw[16];
	char	s[CF_IP6_STRLEN + 64];
	UINT	mode = so_getip6();
	INT	k, i, at, cap;

	switch ( mode & SO_IP6_MODE ) {
	case SO_IP6_OFF:	n = put(t, n, max, "IPV6", "off");	break;
	case SO_IP6_LINK:	n = put(t, n, max, "IPV6", "none");	break;
	case SO_IP6_FIXED:	n = put(t, n, max, "IPV6", "fixed");	break;
	default:		n = put(t, n, max, "IPV6", ( mode & SO_IP6_TEMP ) ? "auto temp" : "auto"); break;
	}
	/* every address the interface has: a list that came back full is asked for again in twice the room */
	for ( cap = 8; ; cap *= 2 ) {
		l = (T_SOIF6 *)Kmalloc(sizeof(T_SOIF6) * (SZ)cap);
		if ( l == NULL ) {
			k = E_NOMEM;
			break;
		}
		k = so_getifaddr6(l, cap, gw);
		if ( k < cap || cap >= 4096 ) break;
		Kfree(l);
	}
	for ( i = 0; i < k; i++ ) {
		CONST UB	*a = l[i].addr;

		cf_ip6str(a, s);
		at = (INT)knl_strlen(s);
		s[at++] = '/';
		at += cf_itoa(l[i].plen, s + at);
		knl_strcpy(s + at, ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0x80 ) ? " link"
				 : ( l[i].flags & SO_IF6_TEMPORARY ) ? " temp"
				 : ( l[i].flags & SO_IF6_AUTOCONF ) ? " auto" : " fixed");
		at = (INT)knl_strlen(s);
		at = life_word(s, at, l[i].vltime);
		at = life_word(s, at, l[i].pltime);
		if ( l[i].flags & SO_IF6_TENTATIVE ) at += (INT)knl_strlen(knl_strcpy(s + at, " tentative"));
		if ( l[i].flags & SO_IF6_DUPLICATED ) at += (INT)knl_strlen(knl_strcpy(s + at, " duplicated"));
		if ( l[i].flags & SO_IF6_DEPRECATED ) at += (INT)knl_strlen(knl_strcpy(s + at, " deprecated"));
		n = put(t, n, max, "ADDRESS6", s);
	}
	if ( k >= 0 && !cf_ip6zero(gw) ) {
		cf_ip6str(gw, s);
		n = put(t, n, max, "GATEWAY6", s);
	}
	for ( i = 0; i < 4 && so_getdns6(i, gw) == E_OK; i++ ) {
		cf_ip6str(gw, s);
		n = put(t, n, max, "DNS6", s);
	}
	if ( l != NULL ) Kfree(l);
	return n;
}

LOCAL INT net_text( UB *t, INT max )
{
	T_NETSTAT	st;
	UB		mac[6];
	UINT		a = 0, m = 0, g = 0, d = 0;
	INT		n = 0, i;

	{
		UB	nm[48];
		INT	k = ( knl_netadp_label != NULL ) ? knl_netadp_label(nm, sizeof(nm) - 1) : 0;

		/* the card the stack uses, when the machine has several to choose from */
		if ( k > 0 ) {
			nm[k] = '\0';
			n = put(t, n, max, "ADAPTER", (CONST char *)nm);
		} else {
#if defined(RPI5)
			n = put(t, n, max, "ADAPTER", "RP1 Gigabit Ethernet (GEM)");
#elif defined(RPI4)
			n = put(t, n, max, "ADAPTER", "BCM2711 Gigabit Ethernet (GENET)");
#else
			n = put(t, n, max, "ADAPTER", "virtio-net");
#endif
		}
	}
	{
		char	h[32];

		/* the stack's host name, and the number of the card's interface (a link-local address's scope) */
		if ( so_gethostname(h, sizeof(h)) >= E_OK ) n = put(t, n, max, "HOSTNAME", h);
		n = put_num(t, n, max, "IFINDEX", (D)so_ifnametoindex("neta0"));
	}
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
	n = net6_text(t, n, max);
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
		n = put_num(t, n, max, "RATE", st.out_rate);
		n = put_num(t, n, max, "CHANNELS", st.out_channels);
	}
	tk_cls_dev(dd, 0);
	{
		SDOutSel	s;
		SDOut		*o = NULL;
		INT		k, m, room = 0;
		LOCAL CONST char *CONST kind[] = { "none", "usb", "i2s", "hdmi", "hda", "bt" };

		knl_snd_arm();			/* a change after this read is told (device/snd/snd.c) */
		dd = tk_opn_dev((UB *)SND_DEVNM, TD_READ);
		knl_memset(&s, 0, sizeof(s));
		if ( dd >= E_OK ) {
			(void)tk_srea_dev(dd, DN_SDOUTSEL, &s, sizeof(s), &asz);
			tk_cls_dev(dd, 0);
		}
		n = put(t, n, max, "OUTPUT", ( s.now[0] != 0 ) ? (CONST char *)s.now : "none");
		n = put(t, n, max, "SELECT", ( s.id[0] != 0 ) ? (CONST char *)s.id : SND_ID_AUTO);
		n = put(t, n, max, "KEEP", knl_snd_keeping() ? "1" : "0");
		n = put_num(t, n, max, "HW_NOTE", (D)knl_snd_note());
		/* every output: its id, kind, flags (SND_OF_*) and name, a line of each */
		while ( ( m = knl_snd_outputs(o, room) ) > room ) {
			if ( o != NULL ) Kfree(o);
			room = m + 4;
			o = (SDOut *)Kmalloc(sizeof(SDOut) * (SZ)room);
			if ( o == NULL ) {
				room = 0;
				break;
			}
		}
		for ( k = 0; k < m && k < room; k++ ) {
			n = put(t, n, max, "OUT", (CONST char *)o[k].id);
			n = put(t, n, max, "OUT_KIND", kind[( o[k].kind <= SND_BE_BT ) ? o[k].kind : 0]);
			n = put_num(t, n, max, "OUT_FLAGS", (D)o[k].flags);
			n = put(t, n, max, "OUT_NAME", (CONST char *)o[k].name);
		}
		if ( o != NULL ) Kfree(o);
	}
	return n;
}

/* ---------------------------------------------------------------- for the device manager */

IMPORT UD tm_log_at( void );			/* lib/libtm: bytes ever written to the console */

IMPORT BOOL	knl_pm_can_sleep( void ) __attribute__((weak));
IMPORT ER	knl_pm_sleep( CONST char *why ) __attribute__((weak));
IMPORT void	knl_pm_policy( INT button_sleep, INT lid_sleep ) __attribute__((weak));
IMPORT INT	knl_pm_policy_get( INT *p_button, INT *p_lid ) __attribute__((weak));

EXPORT INT knl_obsys_text( INT recno, UB *t, INT max )
{
	switch ( recno ) {
	case OB_SYS_INFO:	return info_text(t, max);
	case OB_SYS_POWER:
	{
		INT	n, b = 0, l = 0;

		n = put(t, 0, max, "CAN", ( knl_pm_can_sleep != NULL && knl_pm_can_sleep() )
			? ( sys_power != NULL ? "OFF RESTART SLEEP" : "SLEEP" )
			: ( sys_power != NULL ? "OFF RESTART" : "" ));
		if ( knl_pm_policy_get != NULL && knl_pm_can_sleep() ) {
			(void)knl_pm_policy_get(&b, &l);
			n = put(t, n, max, "BUTTON_SLEEP", b ? "1" : "0");
			n = put(t, n, max, "LID_SLEEP", l ? "1" : "0");
		}
		return n;
	}
	case OB_SYS_DISPLAY:	return knl_obdisp_text(t, max);
	case OB_SYS_USB:	return knl_obusb_text(t, max);
	case OB_SYS_SCHEME:	return scheme_text(t, max);
	case OB_SYS_WALL:	return wall_text(t, max);
	case OB_SYS_NET:	return net_text(t, max);
	case OB_SYS_SOUND:	return sound_text(t, max);
	case OB_SYS_HW:		return hw_text(t, max);
	case OB_SYS_LOG:	return put_num(t, 0, max, "END", (D)tm_log_at());
	default:		return E_NOEXS;
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
		/* the machine asleep, and what the power button and the lid do (kernel/sysdepend/x64/pm.c) */
		if ( knl_strcmp(w, "SLEEP") == 0 ) {
			return ( knl_pm_sleep != NULL ) ? knl_pm_sleep("the system object") : E_NOSPT;
		}
		if ( knl_strcmp(w, "POLICY") == 0 ) {
			char	b[8], l[8];

			if ( knl_pm_policy == NULL ) return E_NOSPT;
			first_word(buf, size, 1, b, sizeof(b));
			first_word(buf, size, 2, l, sizeof(l));
			knl_pm_policy(cf_num(b, -1), cf_num(l, -1));
			return E_OK;
		}
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
	case OB_SYS_SOUND:
	{
		char	id[SND_ID_MAX];

		/* "PLAY what [tone]": one of the system's sounds, on the output chosen */
		if ( knl_strcmp(w, "PLAY") == 0 ) {
			LOCAL CONST char *CONST what[] = { "buzzer", "press", "release", "tick", "long", "test" };
			char	t[16];
			UINT	k;

			first_word(buf, size, 1, id, sizeof(id));
			first_word(buf, size, 2, t, sizeof(t));
			for ( k = 0; k < sizeof(what) / sizeof(what[0]) && knl_strcmp(id, what[k]) != 0; k++ ) {
				;
			}
			if ( k >= sizeof(what) / sizeof(what[0]) ) {
				return E_PAR;
			}
			return wm_beep_try(k | ( ( knl_strcmp(t, "tone") == 0 ) ? WM_BEEP_TONE : 0 ));
		}
		/* "KEEP 1" or "KEEP 0": the stream kept going, or let stop */
		if ( knl_strcmp(w, "KEEP") == 0 ) {
			first_word(buf, size, 1, id, sizeof(id));
			if ( knl_strcmp(id, "1") != 0 && knl_strcmp(id, "0") != 0 ) {
				return E_PAR;
			}
			er = knl_snd_keep(knl_strcmp(id, "1") == 0);
			if ( er >= E_OK ) knl_obsys_changed(OB_SYS_SOUND);
			return er;
		}
		/* "SELECT id", or the id alone */
		if ( knl_strcmp(w, "SELECT") == 0 ) {
			first_word(buf, size, 1, id, sizeof(id));
		} else {
			first_word(buf, size, 0, id, sizeof(id));
		}
		er = knl_snd_select((CONST UB *)id);
		if ( er >= E_OK ) knl_obsys_changed(OB_SYS_SOUND);
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
