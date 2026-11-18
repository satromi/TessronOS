/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	userenv.c
 *	ユーザ環境設定: the accessory (design 16.5.23)
 *
 *	Six sheets: 〈個人属性〉 the person's name, how kana are typed, the
 *	time and how it is kept (the time server, the zone); 〈ＰＤ属性〉 the
 *	pointer; 〈キー属性〉 the keyboard; 〈表示属性〉 the sizes and times the
 *	windows are drawn with; 〈音属性〉 the buzzer and the clicks, which
 *	are sound objects of the system's sound box whose tone this sets;
 *	〈キー操作〉 which keys the edit menus answer.
 *
 *	A value changed is used at once -- it is an entry of the look table,
 *	set through wm_set_look, which the pointer, the keyboard and the
 *	windows read as they go -- so it can be tried before it is kept.
 *	Closing asks whether to keep what was set: 更新して終了 writes it
 *	into the user information object (SYSDEF_CONF_USER), which the
 *	system takes at every start; 元に戻して終了 puts back what there was
 *	when the window opened. The zone is the calendar's and the time
 *	server the network settings', and they are written there. The name
 *	changed is the machine's from its next start.
 *
 *	Each zone offered is a place's: its standard offset and the IANA
 *	name of its time zone, set together (dt_setsyszone). The system's
 *	clock follows the summer time of the places that keep it, and the
 *	programs with a time zone database of their own (the browser) the
 *	whole of the place's rules.
 *
 *	On a PC the RTC is kept in the local time, as Windows keeps it, or
 *	in UTC (内蔵時計, dt_setrtc): set at once and kept in the calendar,
 *	as the zone is. The choice is shown only where there is one (x64),
 *	and not to be worked when the firmware gives the clock a zone.
 */

#include <string.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/look.h>
#include <ts/dt.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/snd.h>
#include <ts/dtreq.h>
#include <ts/uuid.h>
#include "../dtask.h"
#include <stdlib.h>

#define TICK_MS		20

/*
 * The parts, by the numbers the data box (SYSDEF_BOX_USERENV) gives
 * them: the program's own below 1000; one that stands for an entry of
 * the look table has the entry's number and 10000, and a volume's
 * value is shown in the box numbered 10000 above the volume.
 */
#define P_LOOK(n)	( 10000 + (INT)(n) )
#define P_SHOWN(n)	( 10000 + (INT)(n) )
#define P_NAME		1
#define P_INPUT		2
#define P_KANJI		3
#define P_TIME		4
#define P_NTP_ON	5
#define P_NTP		6
#define P_ZONE		7
#define P_BEEP		9
#define P_BUZ_HZ	11
#define P_BUZ_MS	12
#define P_CLK_HZ	13
#define P_CLK_MS	14
#define P_CLICK		15
#define P_RTC_LABEL	16
#define P_RTC		17		/* 内蔵時計: 1 the local time, 2 UTC */
#define P_RTC_NOTE	18

#define PN_MAIN		1
#define PN_WARN		10
#define PN_FINISH	11
#define B_UNDO		106
#define B_KEEP		107
#define PN_COPY		12		/* a 写し's values shown: 取り消し or 設定 */
#define B_COPY_SET	108

#define TAG_ME		1		/* 〈個人属性〉 */

/* The parts that stand for entries of the look table, and how each is shown */
#define K_VOL		1
#define K_SW		2
#define K_SEL		3

typedef struct {
	UINT		look;
	UB		kind;
	CONST INT	*steps;		/* the values a volume takes, when not every one */
	INT		nsteps;
	INT		step;		/* else what it moves by */
	INT		scale, decimals;	/* shown as value / scale */
	CONST char	*unit;
} LKPART;

LOCAL CONST INT	sizes[] = { 8, 12, 14, 16, 18, 20, 22, 24, 26, 28, 32, 40, 48 };
LOCAL CONST INT	bars[] = { 6, 7, 8, 9, 10, 11, 12, 13, 16, 18, 20, 22, 24, 28, 32 };
LOCAL CONST INT	near[] = { 4, 6, 8, 10, 14, 18, 24 };
#define NSTEPS(a)	( (INT)( sizeof(a) / sizeof(a[0]) ) )

LOCAL CONST LKPART lkparts[] = {
	{ LK_PD_ON,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_PD_OFF,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_DBLTIME,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_DBL_W,	K_VOL, near, NSTEPS(near), 1, 1, 0, "ドット" },
	{ LK_PD_SPEED,	K_VOL, NULL, 0, 1, 1, 0, "" },
	{ LK_PD_ACCEL,	K_VOL, NULL, 0, 1, 1, 0, "" },
	{ LK_PD_KEYSPD,	K_VOL, NULL, 0, 1, 1, 0, "" },
	{ LK_PD_MAIN,	K_SEL },
	{ LK_PD_ABS,	K_SEL },
	{ LK_PD_MIDDBL,	K_SW },
	{ LK_PD_WHEEL,	K_SW },
	{ LK_TCH_ON,	K_SW },
	{ LK_TCH_DBL,	K_VOL, NULL, 0, 5, 10, 1, "mm" },
	{ LK_TCH_HIT,	K_VOL, NULL, 0, 5, 10, 1, "mm" },
	{ LK_TCH_HOLD,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_TCH_SCR,	K_SW },
	{ LK_TCH_STEP,	K_VOL, NULL, 0, 5, 10, 1, "mm" },
	{ LK_KEY_ON,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_KEY_OFF,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_KEY_SIM,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_KRP,	K_SW },
	{ LK_KRP_START,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_KRP_INT,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_SCLK,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_TSHIFT,	K_SW },
	{ LK_TITLE_H,	K_VOL, sizes, NSTEPS(sizes), 1, 1, 0, "ドット" },
	{ LK_BAR_W,	K_VOL, bars, NSTEPS(bars), 1, 1, 0, "ドット" },
	{ LK_MENU_H,	K_VOL, sizes, NSTEPS(sizes), 1, 1, 0, "ドット" },
	{ LK_BLINK,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_MARCH,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_MENU_DLY,	K_VOL, NULL, 0, 50, 1000, 2, "秒" },
	{ LK_CARET_W,	K_SEL },
	{ LK_SEL_W,	K_SEL },
	{ LK_PTR_SIZE,	K_SEL },
	{ LK_SCR_LIVE,	K_SW },
	{ LK_CLK_KEY,	K_SW },
	{ LK_CLK_BTN,	K_SW },
	{ LK_SPEAKER,	K_SEL },
	{ LK_EDITKEYS,	K_SEL },
};
#define NLK	( (INT)( sizeof(lkparts) / sizeof(lkparts[0]) ) )

/* The volumes of the tones: not entries of the look table */
typedef struct {
	INT		num, lo, hi, step, scale, decimals;
	CONST char	*unit;
	INT		value;
} TONEVOL;

LOCAL TONEVOL tonevol[] = {
	{ P_BUZ_HZ, 100, 6000, 100, 1, 0, "Hz" },
	{ P_BUZ_MS, 100, 1500, 50, 1000, 2, "秒" },
	{ P_CLK_HZ, 100, 6000, 100, 1, 0, "Hz" },
	{ P_CLK_MS, 100, 1500, 50, 1000, 2, "秒" },
};
#define NTONE	4

/* The zones offered, west to east, as the data box lists them: the standard offset and the IANA name */
typedef struct { CONST char *name; INT min; CONST char *tz; } ZONE;
LOCAL CONST ZONE zones[] = {
	{ "UTC-12:00 日付変更線西", -720, "Etc/GMT+12" },
	{ "UTC-11:00 サモア", -660, "Pacific/Pago_Pago" },
	{ "UTC-10:00 ハワイ", -600, "Pacific/Honolulu" },
	{ "UTC-9:00 アラスカ", -540, "America/Anchorage" },
	{ "UTC-8:00 太平洋（米国）", -480, "America/Los_Angeles" },
	{ "UTC-7:00 山岳部（米国）", -420, "America/Denver" },
	{ "UTC-6:00 中部（米国）", -360, "America/Chicago" },
	{ "UTC-5:00 東部（米国）", -300, "America/New_York" },
	{ "UTC-4:00 大西洋", -240, "America/Halifax" },
	{ "UTC-3:00 ブラジリア", -180, "America/Sao_Paulo" },
	{ "UTC-2:00 中部大西洋", -120, "Atlantic/South_Georgia" },
	{ "UTC-1:00 アゾレス", -60, "Atlantic/Azores" },
	{ "UTC 協定世界時", 0, "UTC" },
	{ "UTC+1:00 中央ヨーロッパ", 60, "Europe/Paris" },
	{ "UTC+2:00 東ヨーロッパ", 120, "Europe/Athens" },
	{ "UTC+3:00 モスクワ", 180, "Europe/Moscow" },
	{ "UTC+3:30 テヘラン", 210, "Asia/Tehran" },
	{ "UTC+4:00 ドバイ", 240, "Asia/Dubai" },
	{ "UTC+4:30 カブール", 270, "Asia/Kabul" },
	{ "UTC+5:00 カラチ", 300, "Asia/Karachi" },
	{ "UTC+5:30 インド", 330, "Asia/Kolkata" },
	{ "UTC+5:45 カトマンズ", 345, "Asia/Kathmandu" },
	{ "UTC+6:00 ダッカ", 360, "Asia/Dhaka" },
	{ "UTC+6:30 ヤンゴン", 390, "Asia/Yangon" },
	{ "UTC+7:00 バンコク", 420, "Asia/Bangkok" },
	{ "UTC+8:00 北京・シンガポール", 480, "Asia/Shanghai" },
	{ "UTC+9:00 日本（東京）", 540, "Asia/Tokyo" },
	{ "UTC+9:30 アデレード", 570, "Australia/Adelaide" },
	{ "UTC+10:00 シドニー", 600, "Australia/Sydney" },
	{ "UTC+11:00 ソロモン諸島", 660, "Pacific/Guadalcanal" },
	{ "UTC+12:00 ニュージーランド", 720, "Pacific/Auckland" },
	{ "UTC+13:00 トンガ", 780, "Pacific/Tongatapu" },
	{ "UTC+14:00 ライン諸島", 840, "Pacific/Kiritimati" },
};
#define NZONE	( (INT)( sizeof(zones) / sizeof(zones[0]) ) )

LOCAL UI	ui;
LOCAL BOOL	closing;

/* What there was when the window opened, to go back to */
LOCAL UW	was_look[64];
LOCAL char	was_name[80];
LOCAL INT	was_zone;
LOCAL char	was_zone_name[DT_ZONE_MAX];
LOCAL char	was_ntp[80];
LOCAL BOOL	time_set;		/* the time was typed in */
LOCAL INT	kana_now;		/* the input method in use when the window opened */
LOCAL INT	was_rtc = -1;		/* how the RTC was kept (DT_RTC_*), -1 when there is no choice */

/* ---------------------------------------------------------------- values in and out */

/* A choice that stands for an entry: its names are the entry's values from the least up */
LOCAL INT choice_of( UINT look, INT v )
{
	switch ( look ) {
	case LK_PD_ABS:		return ( v != 0 ) ? 0 : 1;	/* 絶対, 相対 */
	case LK_SPEAKER:	return ( v != 0 ) ? 0 : 1;	/* オン, オフ */
	case LK_CARET_W:
	case LK_SEL_W:		return v - 1;			/* 細 中 太: 1..3 */
	default:		return v;
	}
}

LOCAL INT value_of_choice( UINT look, INT k )
{
	switch ( look ) {
	case LK_PD_ABS:
	case LK_SPEAKER:	return ( k == 0 ) ? 1 : 0;
	case LK_CARET_W:
	case LK_SEL_W:		return k + 1;
	default:		return k;
	}
}

/* A volume's value as it is shown: "0.25秒" */
LOCAL void shown( INT num, INT v, INT scale, INT decimals, CONST char *unit )
{
	char	t[40];

	if ( scale > 1 ) {
		INT	div = scale, k;

		for ( k = 0; k < decimals; k++ ) div /= 10;
		if ( div < 1 ) div = 1;
		snprintf(t, sizeof(t), "%d.%0*d%s", v / scale, decimals, ( v % scale ) / div, unit);
	} else {
		snprintf(t, sizeof(t), "%d%s", v, unit);
	}
	ui_set_text(&ui, P_SHOWN(num), t);
}

LOCAL INT step_index( CONST LKPART *l, INT v )
{
	INT	i;

	for ( i = 0; i < l->nsteps; i++ ) {
		if ( l->steps[i] >= v ) return i;
	}
	return l->nsteps - 1;
}

/*
 * One part standing for an entry of the look table, set from the
 * entry. A volume across runs from its right, so its high end is given
 * as where it starts; one that takes a list of values runs over their
 * places in it.
 */
LOCAL void lk_show_v( CONST LKPART *l, INT v );

LOCAL void lk_show( CONST LKPART *l )
{
	lk_show_v(l, (INT)wm_look(l->look));
}

/* The part of an entry showing v, which need not be what is set (a 写し's) */
LOCAL void lk_show_v( CONST LKPART *l, INT v )
{
	CONST CF_ITEM	*it = cf_user_look(l->look);
	INT		num = P_LOOK(l->look);

	switch ( l->kind ) {
	case K_VOL:
		if ( l->steps != NULL ) {
			ui_set_range(&ui, num, step_index(l, v), l->nsteps - 1, 0);
		} else {
			ui_set_range(&ui, num, v, ( it != NULL ) ? it->hi : 100, ( it != NULL ) ? it->lo : 0);
		}
		shown(num, v, l->scale, l->decimals, l->unit);
		break;
	case K_SW:
		ui_set_value(&ui, num, v != 0);
		break;
	case K_SEL:
		ui_set_value(&ui, num, choice_of(l->look, v) + 1);
		break;
	}
}

/*
 * The finger's lengths are worked only with タッチ操作補正 on: off, a
 * finger reaches the handles as the pointer does and a double tap is
 * not brought together, so they are shown but not to be worked. The
 * hold stays: it is the only second button a finger has. How far two
 * fingers move for a notch is worked only with 二本指スクロール on.
 */
LOCAL void touch_parts( void )
{
	BOOL	off = ( ui_value(&ui, P_LOOK(LK_TCH_ON)) == 0 );

	ui_off(&ui, P_LOOK(LK_TCH_DBL), off);
	ui_off(&ui, P_LOOK(LK_TCH_HIT), off);
	/* how far two fingers go for a notch: worked only while two fingers scroll */
	ui_off(&ui, P_LOOK(LK_TCH_STEP), ui_value(&ui, P_LOOK(LK_TCH_SCR)) == 0);
}

/* The parts from what is in use now */
LOCAL void from_system( void )
{
	INT	i;

	for ( i = 0; i < NLK; i++ ) lk_show(&lkparts[i]);
	touch_parts();
}

/* A part's value into the look table, used from now on */
LOCAL void to_system( CONST LKPART *l )
{
	INT	num = P_LOOK(l->look), v = ui_value(&ui, num);
	ER	er;

	switch ( l->kind ) {
	case K_VOL:
		if ( l->steps != NULL ) {
			if ( v < 0 ) v = 0;
			if ( v >= l->nsteps ) v = l->nsteps - 1;
			v = l->steps[v];
		} else if ( l->step > 1 ) {
			v = ( ( v + l->step / 2 ) / l->step ) * l->step;
		}
		shown(num, v, l->scale, l->decimals, l->unit);
		break;
	case K_SW:
		v = ( v != 0 );
		break;
	case K_SEL:
		v = value_of_choice(l->look, v - 1);
		break;
	}
	er = wm_set_look(l->look, (UW)v);
	if ( er < E_OK ) {
		char	s[80];

		snprintf(s, sizeof(s), "userenv: look %d = %d refused (%d)\n", (INT)l->look, v, (INT)er);
		tm_putstring((CONST UB *)s);
	} else if ( l->look >= LK_TCH_ON && l->look <= LK_TCH_STEP ) {
		char	s[48];

		/* a finger's settings said, for the tests that work the sheet */
		snprintf(s, sizeof(s), "userenv: touch %d = %d\n", (INT)l->look, v);
		tm_putstring((CONST UB *)s);
	}
}

LOCAL CONST LKPART *lk_of( INT num )
{
	INT	i;

	for ( i = 0; i < NLK; i++ ) {
		if ( P_LOOK(lkparts[i].look) == num ) return &lkparts[i];
	}
	return NULL;
}

LOCAL void warn( CONST char *line )
{
	CONST char *l[3] = { line, "", "" };

	(void)ui_ask(&ui, PN_WARN, l, 3);
}

/* The time server as the sheet has it: "" when the clock is not set by one */
LOCAL void ntp_now( char *buf, INT max )
{
	buf[0] = 0;
	if ( ui_value(&ui, P_NTP_ON) != 0 ) (void)ui_text(&ui, P_NTP, buf, max);
}

/* The zone offered that is the system's: by its name, else by its offset */
LOCAL INT zone_index( CONST char *name, INT min )
{
	INT	i, best = 0;

	for ( i = 0; i < NZONE; i++ ) {
		if ( strcmp(zones[i].tz, name) == 0 ) return i;
	}
	for ( i = 0; i < NZONE; i++ ) {
		if ( zones[i].min == min ) return i;
		if ( zones[i].min < min ) best = i;
	}
	return best;
}

/* The time now, as the person reads it, into its fields */
LOCAL INT	shown_min = -1;

LOCAL void time_show( void )
{
	TS_TIME	t;
	TS_TM	tm;
	INT	f[5];

	if ( time_set || dt_gettime(&t) < E_OK || dt_localtime(&t, &tm) < E_OK ) return;
	if ( tm.tm_min == shown_min ) return;
	shown_min = tm.tm_min;
	f[0] = tm.tm_year + 1900;
	f[1] = tm.tm_mon + 1;
	f[2] = tm.tm_mday;
	f[3] = tm.tm_hour;
	f[4] = tm.tm_min;
	ui_set_fields(&ui, P_TIME, f, 5);
}

/* The time typed in, set: the fields are the zone's, the clock is UTC */
LOCAL BOOL time_put( void )
{
	TS_TM	tm;
	TS_TIME	t;
	INT	z = 0, fv[5];
	ER	er, adm = E_NOEXS;
	CONST INT mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	(void)ui_fields(&ui, P_TIME, fv, 5);
	if ( fv[1] < 1 || fv[1] > 12 || fv[2] < 1 || fv[2] > mdays[fv[1] - 1]
	  || ( fv[1] == 2 && fv[2] == 29 && ( fv[0] % 4 != 0 || ( fv[0] % 100 == 0 && fv[0] % 400 != 0 ) ) ) ) {
		warn("指定した日付は不正です。");
		return FALSE;
	}
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = fv[0] - 1900;
	tm.tm_mon = fv[1] - 1;
	tm.tm_mday = fv[2];
	tm.tm_hour = fv[3];
	tm.tm_min = fv[4];
	(void)dt_getsystz(&z);
	if ( dt_mktime(&tm, &t) < E_OK ) return FALSE;
	t -= (TS_TIME)z * 60;
	{
		/* in summer time the clock is an hour ahead of the standard offset */
		TS_TIME	before = t - 3600;
		TS_TM	k;

		if ( dt_localtime(&before, &k) >= E_OK && k.tm_isdst ) t -= 3600;
	}
	er = dt_settime(t);
	if ( er == E_OACV ) {
		/* the clock is the administrator's: the password asked for, by the desktop */
		adm = ts_admin_ask("時刻の設定", &ui.win);
		if ( adm >= E_OK ) {
			er = dt_settime(t);
			(void)ob_end_adm();
		}
		if ( er == E_OACV ) {
			/* the rest is kept; the time stays as the clock has it */
			if ( adm != E_ABORT ) warn("時刻を設定する権限がありません。");
			tm_putstring((CONST UB *)"userenv: the time not set: no right\n");
			time_set = FALSE;
			shown_min = -1;
			time_show();
			return TRUE;
		}
	}
	return (BOOL)( er >= E_OK );
}

/* ---------------------------------------------------------------- the RTC */

/* How the RTC is kept, into its selector; the selector put away where there is no choice */
LOCAL void rtc_show( void )
{
#ifdef __x86_64__
	INT	m = 0;

	if ( dt_getrtc(&m) >= E_OK ) {
		ui_set_value(&ui, P_RTC, ( ( m & ~DT_RTC_FIRMWARE ) == DT_RTC_LOCAL ) ? 1 : 2);
		ui_off(&ui, P_RTC, ( m & DT_RTC_FIRMWARE ) != 0);
		was_rtc = ( m & DT_RTC_FIRMWARE ) ? -1 : m;
		return;
	}
#endif
	was_rtc = -1;
	ui_show(&ui, P_RTC_LABEL, FALSE);
	ui_show(&ui, P_RTC, FALSE);
	ui_show(&ui, P_RTC_NOTE, FALSE);
}

/* The RTC kept as the selector says, at once */
LOCAL void rtc_put( INT mode )
{
#ifdef __x86_64__
	char	line[64];
	ER	er = dt_setrtc(mode);

	snprintf(line, sizeof(line), "userenv: rtc %s (%d)\n", ( mode == DT_RTC_LOCAL ) ? "local" : "utc", (INT)er);
	tm_putstring((CONST UB *)line);
	if ( er == E_OACV ) warn("内蔵時計の設定を変える権限がありません。");
	if ( er < E_OK ) {
		INT	was = was_rtc;

		rtc_show();			/* the selector back to how it is kept */
		was_rtc = was;
	}
#else
	(void)mode;
#endif
}

/* ---------------------------------------------------------------- the settings objects */

LOCAL ER rec_read( CONST char *role, CONST char *uuid, UB *buf, INT max, INT *p_len )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	*p_len = 0;
	if ( ob_fnd_sys(role, uuid, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) return (ER)key;
	er = ob_rea_rec(key, 1, 0, buf, max, &asz);
	ob_cls_obj(key);
	if ( er >= E_OK ) *p_len = (INT)( ( asz > max ) ? max : asz );
	return er;
}

LOCAL ER rec_write( CONST char *role, CONST char *uuid, CONST UB *buf, INT len )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ob_fnd_sys(role, uuid, &u) < E_OK ) return E_SYS;
	key = ui_conf_open(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(key, 1, buf, (SZ)len);
	(void)asz;
	ob_cls_obj(key);
	return er;
}

LOCAL UB	text[CF_TEXT_MAX];

LOCAL void tones_in( void );

/* Everything set, written into the user information object */
LOCAL ER user_write( void )
{
	INT	len = 0, i;
	char	v[16], v80[80];
	ER	er;

	(void)rec_read(SYSROLE_CONF_USER, SYSDEF_CONF_USER, text, sizeof(text), &len);
	(void)ui_text(&ui, P_NAME, v80, sizeof(v80));
	len = cf_put(text, len, sizeof(text), CF_USER_NAME, 0, v80);
	for ( i = 0; i < cf_user_nitem && len >= 0; i++ ) {
		CONST CF_ITEM	*it = &cf_user_items[i];
		INT		val;

		if ( it->look == LK_KANA ) {
			val = ( ui_value(&ui, P_INPUT) == 1 ) ? 1 : 0;
		} else {
			val = (INT)wm_look(it->look);
		}
		snprintf(v, sizeof(v), "%d", val);
		len = cf_put(text, len, sizeof(text), it->key, 0, v);
	}
	if ( len < 0 ) return E_LIMIT;
	er = rec_write(SYSROLE_CONF_USER, SYSDEF_CONF_USER, text, len);
	return er;
}

/* The time server, in the network's settings: "" asks none */
LOCAL ER ntp_write( CONST char *server )
{
	T_CFNET	n;
	INT	len = 0;
	ER	er;

	er = rec_read(SYSROLE_CONF_NET, SYSDEF_CONF_NET, text, sizeof(text), &len);
	if ( er < E_OK ) return er;
	cf_net_read(text, len, &n);
	strncpy(n.ntp, server, sizeof(n.ntp) - 1);
	n.ntp[sizeof(n.ntp) - 1] = 0;
	len = cf_net_write(text, len, sizeof(text), &n);
	if ( len < 0 ) return E_LIMIT;
	er = rec_write(SYSROLE_CONF_NET, SYSDEF_CONF_NET, text, len);	/* the system takes it as it sees it written */
	if ( er == E_OACV ) {
		/* the network's settings are the administrator's */
		ER	adm = ts_admin_ask("時刻サーバの設定", &ui.win);

		if ( adm >= E_OK ) {
			er = rec_write(SYSROLE_CONF_NET, SYSDEF_CONF_NET, text, len);
			(void)ob_end_adm();
		}
		if ( er == E_OACV && adm != E_ABORT ) warn("時刻サーバを設定する権限がありません。");
	}
	return er;
}

LOCAL void read_start( void )
{
	INT	len = 0, i, z = 0, next;
	char	v[CF_VAL_MAX];
	T_CFNET	n;

	for ( i = 0; i < cf_user_nitem && i < 64; i++ ) was_look[i] = wm_look(cf_user_items[i].look);
	was_name[0] = 0;
	kana_now = (INT)wm_look(LK_KANA);
	next = ( kana_now != 0 ) ? 1 : 2;
	if ( rec_read(SYSROLE_CONF_USER, SYSDEF_CONF_USER, text, sizeof(text), &len) >= E_OK ) {
		if ( cf_get(text, len, CF_USER_NAME, 0, v, sizeof(v)) ) strncpy(was_name, v, sizeof(was_name) - 1);
		/* the method for the next start, which may not be the one in use */
		if ( cf_get(text, len, "KANA_INPUT", 0, v, sizeof(v)) ) next = ( cf_num(v, 0) != 0 ) ? 1 : 2;
	}
	ui_set_now(&ui, P_INPUT, next, ( kana_now != 0 ) ? 1 : 2);
	ui_set_text(&ui, P_NAME, was_name);
	(void)dt_getsystz(&z);
	was_zone = z;
	if ( dt_getsyszone(was_zone_name, sizeof(was_zone_name)) < 0 ) was_zone_name[0] = 0;
	z = zone_index(was_zone_name, z);
	ui_set_fields(&ui, P_ZONE, &z, 1);
	was_ntp[0] = 0;
	if ( rec_read(SYSROLE_CONF_NET, SYSDEF_CONF_NET, text, sizeof(text), &len) >= E_OK ) {
		cf_net_read(text, len, &n);
		strncpy(was_ntp, n.ntp, sizeof(was_ntp) - 1);
	}
	ui_set_value(&ui, P_NTP_ON, was_ntp[0] != 0);
	ui_set_text(&ui, P_NTP, ( was_ntp[0] != 0 ) ? was_ntp : "pool.ntp.org");
	ui_off(&ui, P_NTP, was_ntp[0] == 0);
	rtc_show();
	from_system();
	tones_in();
	time_show();
}

/* ---------------------------------------------------------------- the sound objects */

/*
 * The buzzer's and the clicks' tones are in the metadata of their
 * objects (tessronos.sound.tone). The metadata as it was is kept, to be
 * put back as it was; a tone set is written into it at once, so that it
 * sounds as set.
 */
#define NSND	3
LOCAL CONST char *CONST snd_obj[NSND] = { SYSDEF_SND_BUZZER, SYSDEF_SND_PRESS, SYSDEF_SND_RELEASE };
LOCAL UB	*snd_meta[NSND];		/* each whole, in memory of its own */
LOCAL SZ	snd_mlen[NSND];
LOCAL BOOL	snd_changed;

LOCAL BOOL tone_span( CONST UB *meta, INT len, T_JSON *tone )
{
	T_JSON	root, tf, snd;

	return (BOOL)( js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
		    && js_get(&tf, "sound", &snd) >= E_OK && js_get(&snd, "tone", tone) >= E_OK );
}

LOCAL void tone_read( INT i, INT *p_hz, INT *p_ms )
{
	TS_UUID	u;
	ID	key;
	T_JSON	tone;
	INT	n;

	*p_hz = 2000;
	*p_ms = 100;
	snd_mlen[i] = 0;
	free(snd_meta[i]);
	snd_meta[i] = NULL;
	if ( ui_uuid(snd_obj[i], &u) < E_OK ) return;
	key = ob_opn_obj(&u, OB_OP_ATRRD);
	if ( key < E_OK ) return;
	n = ob_atr_read(key, 0, &snd_meta[i]);
	snd_mlen[i] = ( n > 0 ) ? n : 0;
	ob_cls_obj(key);
	if ( snd_mlen[i] > 0 && tone_span(snd_meta[i], (INT)snd_mlen[i], &tone) ) {
		*p_hz = (INT)js_get_num(&tone, "hz", 2000);
		*p_ms = (INT)js_get_num(&tone, "ms", 100);
	}
}

/* The metadata as kept, with its tone made hz and ms, written */
LOCAL ER tone_write( INT i, INT hz, INT ms )
{
	TS_UUID	u;
	ID	key;
	T_JSON	tone;
	UB	*out;
	char	t[48];
	INT	a, tl, n;
	ER	er;

	if ( snd_mlen[i] <= 0 || !tone_span(snd_meta[i], (INT)snd_mlen[i], &tone) ) return E_NOEXS;
	a = (INT)( tone.s - snd_meta[i] );
	tl = snprintf(t, sizeof(t), "{\"hz\":%d,\"ms\":%d}", hz, ms);
	n = (INT)snd_mlen[i] - tone.len + tl;
	out = malloc((size_t)n);
	if ( out == NULL ) return E_NOMEM;
	memcpy(out, snd_meta[i], a);
	memcpy(out + a, t, tl);
	memcpy(out + a + tl, snd_meta[i] + a + tone.len, snd_mlen[i] - a - tone.len);
	key = ( ui_uuid(snd_obj[i], &u) < E_OK ) ? E_SYS : ob_opn_obj(&u, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key < E_OK ) {
		free(out);
		return (ER)key;
	}
	er = ob_set_atr(key, out, n);
	ob_cls_obj(key);
	free(out);
	if ( er >= E_OK ) snd_changed = TRUE;
	return er;
}

/*
 * A sound played as the system plays it, on the output the system has
 * chosen: asked of the system object's record 8 (PLAY, beep.c), the
 * tone the metadata now holds -- what is being set -- whatever record 1
 * holds. The write answers whether it sounds; when it does not, the
 * person is told.
 */
LOCAL CONST char *CONST play_what[NSND] = { "buzzer", "press", "release" };

LOCAL BOOL play( INT i )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	char	cmd[32], s[64];
	ER	er;

	snprintf(cmd, sizeof(cmd), "PLAY %s tone", play_what[i]);
	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) {
		er = E_SYS;
	} else if ( ( key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE) ) < E_OK ) {
		er = (ER)key;
	} else {
		er = ob_wri_rec(key, OB_SYS_SOUND, 0, cmd, (SZ)strlen(cmd), &asz);
		ob_cls_obj(key);
	}
	snprintf(s, sizeof(s), "userenv: %s %d\n", cmd, (INT)er);
	tm_putstring((CONST UB *)s);
	if ( er >= E_OK ) return TRUE;
	if ( er == E_NOSPT || er == E_NOEXS ) {
		warn("音を出せる出力先がありません。");
	} else if ( er == E_OACV ) {
		warn("音を鳴らす権限がありません。");
	} else {
		warn("音を鳴らせませんでした。");
	}
	return FALSE;
}

/* The metadata as it was when the window opened, back */
LOCAL void tone_undo( void )
{
	TS_UUID	u;
	ID	key;
	INT	i;

	if ( !snd_changed ) return;
	for ( i = 0; i < NSND; i++ ) {
		if ( snd_mlen[i] <= 0 || ui_uuid(snd_obj[i], &u) < E_OK ) continue;
		key = ob_opn_obj(&u, OB_OP_ATRRD | OB_OP_ATRWR);
		if ( key < E_OK ) continue;
		(void)ob_set_atr(key, snd_meta[i], snd_mlen[i]);
		ob_cls_obj(key);
	}
}

LOCAL TONEVOL *tone_of( INT num )
{
	INT	i;

	for ( i = 0; i < NTONE; i++ ) {
		if ( tonevol[i].num == num ) return &tonevol[i];
	}
	return NULL;
}

LOCAL void tone_show( INT num, INT v )
{
	TONEVOL	*t = tone_of(num);

	t->value = v;
	ui_set_range(&ui, num, v, t->hi, t->lo);
	shown(num, v, t->scale, t->decimals, t->unit);
}

LOCAL void tones_in( void )
{
	INT	hz, ms;

	tone_read(0, &hz, &ms);
	tone_show(P_BUZ_HZ, hz);
	tone_show(P_BUZ_MS, ms);
	tone_read(1, &hz, &ms);
	tone_show(P_CLK_HZ, hz);
	tone_show(P_CLK_MS, ms * 4);		/* shown as the whole of a click */
	tone_read(2, &hz, &ms);
}

/* A tone of the sheet changed: the buzzer, or both clicks (let go a fifth lower) */
LOCAL void tone_changed( INT id )
{
	TONEVOL	*t = tone_of(id);
	INT	v = ui_value(&ui, id);

	v = ( ( v + t->step / 2 ) / t->step ) * t->step;
	t->value = v;
	shown(id, v, t->scale, t->decimals, t->unit);
	if ( id == P_BUZ_HZ || id == P_BUZ_MS ) {
		(void)tone_write(0, tone_of(P_BUZ_HZ)->value, tone_of(P_BUZ_MS)->value);
	} else {
		INT	hz = tone_of(P_CLK_HZ)->value, ms = tone_of(P_CLK_MS)->value / 4;

		(void)tone_write(1, hz, ms);
		(void)tone_write(2, hz * 2 / 3, ms);
	}
}

/* ---------------------------------------------------------------- closing */

LOCAL BOOL changed_any( BOOL *p_name )
{
	INT	i, zone = 0;
	char	ntp[80], name[80];
	INT	kana_next = ( ui_value(&ui, P_INPUT) == 1 ) ? 1 : 0;

	ntp_now(ntp, sizeof(ntp));
	(void)ui_text(&ui, P_NAME, name, sizeof(name));
	(void)ui_fields(&ui, P_ZONE, &zone, 1);
	*p_name = (BOOL)( strcmp(was_name, name) != 0 );
	if ( *p_name || time_set || snd_changed || strcmp(ntp, was_ntp) != 0
	  || zones[zone].min != was_zone || strcmp(zones[zone].tz, was_zone_name) != 0 || kana_next != kana_now ) {
		return TRUE;
	}
#ifdef __x86_64__
	if ( was_rtc >= 0 && ( ( ui_value(&ui, P_RTC) == 1 ) ? DT_RTC_LOCAL : DT_RTC_UTC ) != was_rtc ) {
		return TRUE;
	}
#endif
	for ( i = 0; i < cf_user_nitem && i < 64; i++ ) {
		if ( cf_user_items[i].look == LK_KANA ) continue;
		if ( wm_look(cf_user_items[i].look) != was_look[i] ) return TRUE;
	}
	return FALSE;
}

/* 元に戻して終了: what there was, back */
LOCAL void undo( void )
{
	INT	i;

	for ( i = 0; i < cf_user_nitem && i < 64; i++ ) {
		if ( cf_user_items[i].look == LK_KANA ) continue;
		if ( wm_look(cf_user_items[i].look) != was_look[i] ) (void)wm_set_look(cf_user_items[i].look, was_look[i]);
	}
	(void)dt_setsyszone(was_zone_name, was_zone);
#ifdef __x86_64__
	if ( was_rtc >= 0 && ( ( ui_value(&ui, P_RTC) == 1 ) ? DT_RTC_LOCAL : DT_RTC_UTC ) != was_rtc ) {
		(void)dt_setrtc(was_rtc);
	}
#endif
	tone_undo();
}

/* 更新して終了: kept */
LOCAL BOOL keep( void )
{
	char	ntp[80], name[80];
	ER	er;

	ntp_now(ntp, sizeof(ntp));
	if ( ui_text(&ui, P_NAME, name, sizeof(name)) == 0 ) {
		warn("使用者を指定して下さい。");
		return FALSE;
	}
	if ( time_set && !time_put() ) return FALSE;
	(void)wm_set_look(LK_KANA, ( ui_value(&ui, P_INPUT) == 1 ) ? 1 : 0);
	er = user_write();
	if ( er >= E_OK && strcmp(ntp, was_ntp) != 0 ) {
		ER	e = ntp_write(ntp);

		if ( e == E_OACV ) {
			tm_putstring((CONST UB *)"userenv: the time server not set: no right\n");
		} else {
			er = e;
		}
	}
	if ( er < E_OK ) {
		CONST char *l[3] = { "『ユーザ情報』に設定した内容を保存できません。", "設定した内容は今回限りとなります。", "" };

		(void)ui_ask(&ui, PN_WARN, l, 3);
	}
	tm_putstring((CONST UB *)"userenv: kept\n");
	return TRUE;
}

LOCAL void finish( void )
{
	BOOL	name;
	INT	a;
	CONST char *l[1] = { "" };

	if ( !changed_any(&name) ) {
		closing = TRUE;
		return;
	}
	if ( name ) l[0] = "変更した使用者名は、再起動後に有効になります。";
	a = ui_ask(&ui, PN_FINISH, l, 1);
	if ( a == B_UNDO ) {
		undo();
		tm_putstring((CONST UB *)"userenv: put back\n");
		closing = TRUE;
	} else if ( a == B_KEEP && keep() ) {
		closing = TRUE;
	}
}

/* 標準設定: the parts of the sheet shown back to what nothing said (lkparts in the sheets' order) */

LOCAL void standard( void )
{
	INT	tag = ui_sheet(&ui), i;

	for ( i = 0; i < NLK; i++ ) {
		CONST LKPART	*l = &lkparts[i];
		CONST CF_ITEM	*it = cf_user_look(l->look);
		INT		t = ( i < 17 ) ? 2 : ( i < 25 ) ? 3 : ( i < 35 ) ? 4 : ( i < 38 ) ? 5 : 6;

		if ( t != tag || it == NULL ) continue;
		(void)wm_set_look(l->look, (UW)it->dflt);
		lk_show(l);
	}
	touch_parts();
}

/* ---------------------------------------------------------------- pages, their links and their 写し */

/*
 * The program is started, or run again while its window is open, with
 * the 固有データ of the applist entry it was opened from (T_PRCOPEN's
 * data, design 11.3): the name of a sheet, which is shown; a name it
 * does not know, or none, leaves the window as it is. このページの仮身を
 * 作る puts a link to the program's object with the sheet in its applist;
 * 写しを作る writes what the sheet sets into an object of the person's
 * (a 写し, design 11.6), which opened shows its values on the sheet and
 * sets them only with 設定. 〈個人属性〉 -- the name, the kana input,
 * the time, the zone, the time server -- is copied by no 写し.
 */
typedef struct {
	CONST char	*name;		/* the data that asks for it */
	INT		tag;
	CONST char	*label;
	CONST char	*copy;		/* the page a 写し of it names (cf_copy_allowed); NULL none */
} UPAGE;

LOCAL CONST UPAGE upages[] = {
	{ "personal",	1, "個人属性",	NULL },
	{ "pointer",	2, "ＰＤ属性",	"user.pointer" },
	{ "key",	3, "キー属性",	"user.key" },
	{ "display",	4, "表示属性",	"user.display" },
	{ "sound",	5, "音属性",	"user.sound" },
	{ "editkeys",	6, "キー操作",	"user.editkeys" },
};
#define NUPAGE	( (INT)( sizeof(upages) / sizeof(upages[0]) ) )

LOCAL CONST UPAGE *upage_named( CONST char *s )
{
	INT	i;

	for ( i = 0; s != NULL && s[0] != 0 && i < NUPAGE; i++ ) {
		if ( strcmp(upages[i].name, s) == 0 || ( upages[i].copy != NULL && strcmp(upages[i].copy, s) == 0 ) ) {
			return &upages[i];
		}
	}
	return NULL;
}

LOCAL CONST UPAGE *upage_of_tag( INT tag )
{
	INT	i;

	for ( i = 0; i < NUPAGE; i++ ) {
		if ( upages[i].tag == tag ) return &upages[i];
	}
	return NULL;
}

LOCAL BOOL no_uuid( CONST TS_UUID *u )
{
	static CONST TS_UUID	zero;

	return (BOOL)( u == NULL || memcmp(u, &zero, sizeof(zero)) == 0 );
}

/*
 * A request to the desktop, answered on a channel of its own: what came
 * of it. name and text go whole in the request's tail (DT_TAIL_NAME and
 * DT_TAIL_TEXT); with DT_F_APPLIST the text is the link's applist
 * (DT_TAIL_APPLIST).
 */
LOCAL ER desk_req( UINT req, CONST TS_UUID *target, UINT flags, CONST char *name, CONST char *text, T_DTANS *an )
{
	static DTASK	q;		/* the words and the answer, as long as they are (dtask.h) */
	T_DTREQ		rq;

	memset(an, 0, sizeof(*an));
	memset(&rq, 0, sizeof(rq));
	rq.req = req;
	if ( target != NULL ) rq.target = *target;
	rq.recno = -1;
	rq.flags = flags;
	if ( name != NULL ) (void)dq_part(&q, DT_TAIL_NAME, (CONST UB *)name, -1);
	if ( text != NULL ) {
		(void)dq_part(&q, ( ( flags & DT_F_APPLIST ) != 0 ) ? DT_TAIL_APPLIST : DT_TAIL_TEXT, (CONST UB *)text, -1);
	}
	return dq_ask(&q, &rq, an, &ui.win, 10000);
}

LOCAL void page_show( CONST UPAGE *pg )
{
	char	s[64];

	ui_show_sheet(&ui, pg->tag);
	snprintf(s, sizeof(s), "userenv: page \"%s\"\n", pg->name);
	tm_putstring((CONST UB *)s);
}

/* The parts of a value not set (a 写し's), shown as lk_show shows what is set */
LOCAL void lk_show_v( CONST LKPART *l, INT v );

LOCAL BOOL is_copy( CONST TS_UUID *u, char *page, INT max )
{
	static UB	meta[4096];
	T_JSON		root, tf, g;
	SZ		asz = 0;
	ID		key;
	BOOL		is = FALSE;

	page[0] = 0;
	key = ob_opn_obj(u, OB_OP_R);
	if ( key <= 0 ) return FALSE;
	if ( ob_get_atr(key, meta, sizeof(meta) - 1, &asz) >= E_OK
	  && js_parse(meta, (INT)asz, &root) >= E_OK
	  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "confcopy", &g) >= E_OK ) {
		is = (BOOL)( js_get_str(&g, "page", (UB *)page, max) > 0 );
	}
	ob_cls_obj(key);
	return is;
}

/* What a 写し says of each entry of the look table, -1 for what it does not say */
#define UC_VAL_NONE	( (INT)0x80000000 )

LOCAL void copy_open( CONST TS_UUID *u )
{
	CONST UPAGE	*pg;
	char		page[32], l1[128], l2[160], s[128], v[CF_VAL_MAX];
	UB		*t = NULL, *w;
	INT		len, wl, i, n = 0, differ = 0, nl = 0, val[NLK];
	CONST char	*l[4];

	if ( !is_copy(u, page, (INT)sizeof(page)) || ( pg = upage_named(page) ) == NULL || pg->copy == NULL ) {
		warn("この写しはユーザ環境設定のページのものではありません。");
		return;
	}
	page_show(pg);
	len = ui_rec_all(u, 1, 0, &t);
	w = ( len > 0 ) ? (UB *)malloc((size_t)len + 2) : NULL;
	/* the lines of the page's table alone: any other a copy has is not read */
	wl = ( w != NULL ) ? cf_copy_lines(t, len, pg->copy, w, len + 2) : -1;
	free(t);
	l2[0] = 0;
	for ( i = 0; i < NLK; i++ ) {
		CONST CF_ITEM	*it = cf_user_look(lkparts[i].look);

		val[i] = UC_VAL_NONE;
		if ( it == NULL || wl <= 0 || !cf_get(w, wl, it->key, 0, v, sizeof(v)) ) continue;
		val[i] = cf_num(v, it->dflt);
		if ( val[i] < it->lo ) val[i] = it->lo;
		if ( val[i] > it->hi ) val[i] = it->hi;
		nl++;
		if ( val[i] == (INT)wm_look(lkparts[i].look) ) continue;
		differ++;
		if ( strlen(l2) + strlen(it->key) + 4 < 96 ) {
			if ( n++ > 0 ) strcat(l2, "、");
			strcat(l2, it->key);
		} else if ( strstr(l2, " ほか") == NULL ) {
			strcat(l2, " ほか");
		}
	}
	free(w);
	snprintf(s, sizeof(s), "userenv: copy of \"%s\" opened, %d lines, %d differ\n", pg->copy, nl, differ);
	tm_putstring((CONST UB *)s);
	if ( nl == 0 ) {
		warn("この写しには、このページで設定できる値がありません。");
		return;
	}
	for ( i = 0; i < NLK; i++ ) {
		if ( val[i] != UC_VAL_NONE ) lk_show_v(&lkparts[i], val[i]);
	}
	{
		UB	*cn = ui_obj_name(u);

		snprintf(l1, sizeof(l1), "「%s」の値を表示しています。", ( cn != NULL ) ? (CONST char *)cn : "");
		free(cn);
	}
	if ( differ == 0 ) {
		snprintf(l2, sizeof(l2), "今の設定と同じです。");
	} else {
		char	x[176];

		snprintf(x, sizeof(x), "違う項目：%s", l2);
		snprintf(l2, sizeof(l2), "%s", x);
	}
	l[0] = l1;
	l[1] = l2;
	l[2] = "〈設定〉を押すと、この値を設定します。";
	if ( ui_ask(&ui, PN_COPY, l, 3) == B_COPY_SET ) {
		for ( i = 0; i < NLK; i++ ) {
			if ( val[i] != UC_VAL_NONE ) (void)wm_set_look(lkparts[i].look, (UW)val[i]);
		}
		from_system();
		snprintf(s, sizeof(s), "userenv: copy of \"%s\" set\n", pg->copy);
	} else {
		from_system();
		snprintf(s, sizeof(s), "userenv: copy of \"%s\" put back\n", pg->copy);
	}
	tm_putstring((CONST UB *)s);
}

/* 写しを作る: the sheet shown, as it is now, into a new object linked from the first window */
LOCAL void copy_make( void )
{
	CONST UPAGE	*pg = upage_of_tag(ui_sheet(&ui));
	static char	json[1024], body[3072];
	static UB	doc[512];
	char		name[128], s[128];
	T_OBCRE		c;
	T_DTANS		an;
	TS_UUID		u;
	INT		recno = -1, i, n, dlen, nl = 0;
	SZ		asz = 0;
	ID		k;
	ER		er;

	if ( pg == NULL || pg->copy == NULL ) {
		warn("このページには写せる設定がありません。");
		return;
	}
	snprintf(name, sizeof(name), "ユーザ環境設定（%s）の写し", pg->label);
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"editable\":true,\"deletable\":true,\"readable\":true,"
		 "\"applist\":{\"tessronos-userenv\":{\"name\":\"ユーザ環境設定\",\"defaultOpen\":true,\"data\":\"%s\"}},"
		 "\"tessronos\":{\"confcopy\":{\"page\":\"%s\"}}}",
		 name, pg->name, pg->copy);
	n = snprintf(body, sizeof(body), "# ユーザ環境設定の写し：%s\n", pg->label);
	for ( i = 0; i < NLK && n < (INT)sizeof(body) - 1; i++ ) {
		CONST CF_ITEM	*it = cf_user_look(lkparts[i].look);

		if ( it == NULL || cf_user_page(it->look) == NULL || strcmp(cf_user_page(it->look), pg->copy) != 0 ) continue;
		n += snprintf(body + n, sizeof(body) - (size_t)n, "%s\t%d\n", it->key, (INT)wm_look(it->look));
		nl++;
	}
	dlen = snprintf((char *)doc, sizeof(doc),
			"<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>%s</p>\n</document>\n</tad>\n", name);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	er = ob_cre_obj(&c, &u);
	if ( er >= E_OK ) {
		k = ob_opn_obj(&u, OB_OP_R | OB_OP_W | OB_O_EXCL);
		er = ( k > 0 ) ? ob_apd_rec(k, OB_RT_TAD, 0, &recno) : (ER)k;
		if ( er >= E_OK ) er = ob_wri_rec(k, recno, 0, doc, (SZ)dlen, &asz);
		if ( er >= E_OK ) er = ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno);
		if ( er >= E_OK ) er = ob_wri_rec(k, recno, 0, body, (SZ)n, &asz);
		if ( k > 0 ) ob_cls_obj(k);
		if ( er < E_OK ) (void)ob_del_obj(&u);
	}
	if ( er < E_OK ) {
		warn("写しの実身を作れませんでした。");
		return;
	}
	er = desk_req(DT_RQ_LINK_PUT, &u, 0, name, NULL, &an);
	snprintf(s, sizeof(s), "userenv: copy of \"%s\" made, %d lines (%d)\n", pg->copy, nl, (INT)er);
	tm_putstring((CONST UB *)s);
}

/* このページの仮身を作る: a link to the program's object, the sheet in its applist's data */
LOCAL void page_link( void )
{
	CONST UPAGE	*pg = upage_of_tag(ui_sheet(&ui));
	char		name[96], appl[160], s[96];
	TS_UUID		me;
	T_DTANS		an;
	ER		er;

	if ( pg == NULL || ui_uuid(SYSDEF_PROG_USERENV, &me) < E_OK ) return;
	snprintf(name, sizeof(name), "ユーザ環境設定（%s）", pg->label);
	snprintf(appl, sizeof(appl), "{\"tessronos-userenv\":{\"name\":\"ユーザ環境設定\",\"data\":\"%s\"}}", pg->name);
	er = desk_req(DT_RQ_LINK_PUT, &me, DT_F_APPLIST, name, appl, &an);
	snprintf(s, sizeof(s), "userenv: link of page \"%s\" put (%d)\n", pg->name, (INT)er);
	tm_putstring((CONST UB *)s);
	if ( er < E_OK ) warn("このページの仮身を置けませんでした。");
}

/* Opened on target (0 none, or the program object itself) with data */
LOCAL BOOL	copy_due = FALSE;
LOCAL TS_UUID	copy_obj;

LOCAL void opened_on( CONST TS_UUID *target, CONST char *data, BOOL up )
{
	TS_UUID		me;
	CONST UPAGE	*pg;
	char		page[32], s[160];

	/* a note for the log: long data shown in part */
	snprintf(s, sizeof(s), "userenv: opened with data \"%.100s\"\n", ( data != NULL ) ? data : "");
	tm_putstring((CONST UB *)s);
	if ( !no_uuid(target) && ( ui_uuid(SYSDEF_PROG_USERENV, &me) < E_OK || memcmp(&me, target, sizeof(me)) != 0 )
	  && is_copy(target, page, (INT)sizeof(page)) ) {
		copy_obj = *target;
		copy_due = !up;
		if ( up ) copy_open(&copy_obj);
		return;
	}
	pg = upage_named(data);
	if ( pg != NULL ) page_show(pg);
}

/* The desktop's channel (DT_RQ_TELL): run again while the window is open (DT_HOW_AGAIN) */
LOCAL TS_UUID	told_ch;
LOCAL ID	told_port = 0;

LOCAL void listen( void )
{
	T_OBCRE	c;
	T_OBNTF	req;
	T_DTANS	an;
	char	t[TS_UUID_STRLEN + 1];

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &told_ch) < E_OK ) return;
	told_port = ob_opn_obj(&told_ch, OB_OP_READ | OB_OP_ATRRD | OB_O_NOWAIT);
	if ( told_port <= 0 ) {
		told_port = 0;
		(void)ob_del_obj(&told_ch);
		return;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 3;
	(void)ob_ntf_evt(told_port, OB_REC_ANY, &req, ui.port);
	ts_uuid_text(&told_ch, t);
	(void)desk_req(DT_RQ_TELL, NULL, 0, "tessronos-userenv", t, &an);
}

LOCAL BOOL told( CONST T_OBNTM *m )
{
	static UB	*buf = NULL;	/* each notice read whole, its tail after it */
	static SZ	cap = 0;
	SZ		n;

	if ( told_port <= 0 || memcmp(&m->uuid, &told_ch, sizeof(TS_UUID)) != 0 ) return FALSE;
	while ( ( n = dq_read(told_port, &buf, &cap, TMO_POL) ) >= (SZ)sizeof(T_DTANS) ) {
		CONST T_DTANS	*an = (CONST T_DTANS *)buf;

		if ( an->how == DT_HOW_AGAIN ) {
			/* the 固有データ it was run again with, as long as it is */
			opened_on(&an->into, (CONST char *)dq_told_text(buf, n, DT_TAIL_DATA, NULL), TRUE);
		}
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the menu */

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

LOCAL void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;
	BOOL	box = ( ui.last == P_NAME || ui.last == P_NTP );

	if ( ui_uuid(SYSDEF_MENU_USERENV, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( !box ) (void)mn_chg_atr(mid, NULL, "clear", MN_GREY);
	{
		CONST UPAGE	*pg = upage_of_tag(ui_sheet(&ui));

		(void)mn_chg_atr(mid, NULL, "copy.make", ( pg != NULL && pg->copy != NULL ) ? 0 : MN_GREY);
	}
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		finish();
	} else if ( is_code(&sel, "clear") && box ) {
		ui_set_text(&ui, ui.last, "");
	} else if ( is_code(&sel, "standard") ) {
		standard();
	} else if ( is_code(&sel, "copy.make") ) {
		copy_make();
	} else if ( is_code(&sel, "page.link") ) {
		page_link();
	}
}

/* ---------------------------------------------------------------- what happens */

LOCAL void changed( INT id )
{
	CONST LKPART	*l = lk_of(id);
	INT		z = 0;

	if ( l != NULL ) {
		to_system(l);
		if ( l->look == LK_TCH_ON || l->look == LK_TCH_SCR ) touch_parts();
		return;
	}
	switch ( id ) {
	case P_BUZ_HZ:
	case P_BUZ_MS:
	case P_CLK_HZ:
	case P_CLK_MS:
		tone_changed(id);
		break;
	case P_TIME:
		time_set = TRUE;
		break;
	case P_ZONE:
		(void)ui_fields(&ui, P_ZONE, &z, 1);
		if ( dt_setsyszone(zones[z].tz, zones[z].min) >= E_OK ) {
			char	line[80];

			snprintf(line, sizeof(line), "userenv: zone %s\n", zones[z].tz);
			tm_putstring((CONST UB *)line);
		}
		shown_min = -1;
		time_show();
		break;
	case P_NTP_ON:
		ui_off(&ui, P_NTP, ui_value(&ui, P_NTP_ON) == 0);
		break;
	case P_RTC:
		rtc_put(( ui_value(&ui, P_RTC) == 1 ) ? 1 : 0);
		break;
	default:
		break;
	}
}

int main( void )
{
	T_OBNTM	m;
	INT	id, ev;
	UD	shown_s = 0;

	ui_icon(SYSDEF_PROG_USERENV);
	/* the window stands for the program object: opened again, from a page's link or a copy, it is told ("once") */
	ui_shows(SYSDEF_PROG_USERENV);
	ui_attr(UI_ATTR_HIDDEN);		/* filled hidden, and shown once (ui_reveal) */
	if ( ui_open(&ui, SYSDEF_BOX_USERENV, PN_MAIN, wm_new_x(180), wm_new_y(90)) < E_OK ) {
		tm_putstring((CONST UB *)"userenv: no window\n");
		return 1;
	}
	read_start();
	/* the object and the 固有データ it was opened with (T_PRCOPEN): the sheet shown before the window comes up */
	{
		SZ		n = ts_get_arg(NULL, 0);	/* the whole argument's size, the data after it */
		T_PRCOPEN	*a = (T_PRCOPEN *)calloc(1, ( n > (SZ)sizeof(T_PRCOPEN) ) ? (size_t)n : sizeof(T_PRCOPEN));

		if ( a == NULL ) {
			warn("メモリが足りないため、開いたページを表示できませんでした。");
		} else {
			if ( n > 0 ) n = ts_get_arg(a, ( n > (SZ)sizeof(T_PRCOPEN) ) ? n : (SZ)sizeof(T_PRCOPEN));
			if ( n >= (SZ)sizeof(TS_UUID) ) {
				CONST UB	*data = dt_prcopen_data(a, n);

				opened_on(&a->target, ( data != NULL ) ? (CONST char *)data : "", FALSE);
			}
			free(a);
		}
	}
	listen();
	ui_reveal(&ui);
	tm_putstring((CONST UB *)"userenv: ready\n");
	if ( copy_due ) {
		copy_due = FALSE;
		copy_open(&copy_obj);
	}

	while ( !closing ) {
		/* the clock on 〈使用者〉 goes on at each second; nothing else is waited for but the notices */
		for ( TMO tmo = ( ui_sheet(&ui) == TAG_ME ) ? ui_left(( ui_now() / 1000 + 1 ) * 1000) : TMO_FEVR;
		      !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			if ( told(&m) ) continue;
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				if ( m.event == OB_E_DELETE ) closing = TRUE;
				else finish();
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_KEY:
				if ( ( m.mods & ( 0x01 | 0x10 ) ) != 0 ) {
					UB	c = wm_key_char(m.code, m.mods);

					if ( c == 'e' || c == 'E' ) finish();
				}
				break;
			case UI_EV_CHANGE:
				changed(id);
				break;
			case UI_EV_BUTTON:
				if ( id == P_BEEP ) (void)play(0);
				if ( id == P_CLICK && play(1) ) {
					(void)tk_dly_tsk(150);
					(void)play(2);
				}
				break;
			default:
				break;
			}
		}
		if ( !closing && ui_sheet(&ui) == TAG_ME && ui_now() / 1000 != shown_s ) {
			shown_s = ui_now() / 1000;
			time_show();			/* the clock goes on */
		}
	}
	if ( told_port > 0 ) {
		ob_cls_obj(told_port);
		(void)ob_del_obj(&told_ch);
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"userenv: closed\n");
	return 0;
}
