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
 */

#include <string.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/look.h>
#include <ts/dt.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/snd.h>

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

#define PN_MAIN		1
#define PN_WARN		10
#define PN_FINISH	11
#define B_UNDO		106
#define B_KEEP		107

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

/* The zones offered, west to east */
typedef struct { CONST char *name; INT min; } ZONE;
LOCAL CONST ZONE zones[] = {
	{ "UTC-12:00 日付変更線西", -720 }, { "UTC-11:00 サモア", -660 },
	{ "UTC-10:00 ハワイ", -600 }, { "UTC-9:00 アラスカ", -540 },
	{ "UTC-8:00 太平洋（米国）", -480 }, { "UTC-7:00 山岳部（米国）", -420 },
	{ "UTC-6:00 中部（米国）", -360 }, { "UTC-5:00 東部（米国）", -300 },
	{ "UTC-4:00 大西洋", -240 }, { "UTC-3:00 ブラジリア", -180 },
	{ "UTC-2:00 中部大西洋", -120 }, { "UTC-1:00 アゾレス", -60 },
	{ "UTC 協定世界時", 0 }, { "UTC+1:00 中央ヨーロッパ", 60 },
	{ "UTC+2:00 東ヨーロッパ", 120 }, { "UTC+3:00 モスクワ", 180 },
	{ "UTC+3:30 テヘラン", 210 }, { "UTC+4:00 ドバイ", 240 },
	{ "UTC+4:30 カブール", 270 }, { "UTC+5:00 カラチ", 300 },
	{ "UTC+5:30 インド", 330 }, { "UTC+5:45 カトマンズ", 345 },
	{ "UTC+6:00 ダッカ", 360 }, { "UTC+6:30 ヤンゴン", 390 },
	{ "UTC+7:00 バンコク", 420 }, { "UTC+8:00 北京・シンガポール", 480 },
	{ "UTC+9:00 日本（東京）", 540 }, { "UTC+9:30 アデレード", 570 },
	{ "UTC+10:00 シドニー", 600 }, { "UTC+11:00 ソロモン諸島", 660 },
	{ "UTC+12:00 ニュージーランド", 720 }, { "UTC+13:00 トンガ", 780 },
	{ "UTC+14:00 ライン諸島", 840 },
};
#define NZONE	( (INT)( sizeof(zones) / sizeof(zones[0]) ) )

LOCAL UI	ui;
LOCAL BOOL	closing;

/* What there was when the window opened, to go back to */
LOCAL UW	was_look[64];
LOCAL char	was_name[80];
LOCAL INT	was_zone;
LOCAL char	was_ntp[80];
LOCAL BOOL	time_set;		/* the time was typed in */
LOCAL INT	kana_now;		/* the input method in use when the window opened */

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
LOCAL void lk_show( CONST LKPART *l )
{
	CONST CF_ITEM	*it = cf_user_look(l->look);
	INT		v = (INT)wm_look(l->look), num = P_LOOK(l->look);

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

/* The parts from what is in use now */
LOCAL void from_system( void )
{
	INT	i;

	for ( i = 0; i < NLK; i++ ) lk_show(&lkparts[i]);
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

LOCAL INT zone_index( INT min )
{
	INT	i, best = 0;

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
	return (BOOL)( dt_settime(t) >= E_OK );
}

/* ---------------------------------------------------------------- the settings objects */

LOCAL ER rec_read( CONST char *uuid, UB *buf, INT max, INT *p_len )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	*p_len = 0;
	if ( ui_uuid(uuid, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) return (ER)key;
	er = ob_rea_rec(key, 1, 0, buf, max, &asz);
	ob_cls_obj(key);
	if ( er >= E_OK ) *p_len = (INT)( ( asz > max ) ? max : asz );
	return er;
}

LOCAL ER rec_write( CONST char *uuid, CONST UB *buf, INT len )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ui_uuid(uuid, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, 1, 0, buf, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)len);
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

	(void)rec_read(SYSDEF_CONF_USER, text, sizeof(text), &len);
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
	er = rec_write(SYSDEF_CONF_USER, text, len);
	return er;
}

/* The time server, in the network's settings: "" asks none */
LOCAL ER ntp_write( CONST char *server )
{
	T_CFNET	n;
	INT	len = 0;
	ER	er;

	er = rec_read(SYSDEF_CONF_NET, text, sizeof(text), &len);
	if ( er < E_OK ) return er;
	cf_net_read(text, len, &n);
	strncpy(n.ntp, server, sizeof(n.ntp) - 1);
	n.ntp[sizeof(n.ntp) - 1] = 0;
	len = cf_net_write(text, len, sizeof(text), &n);
	if ( len < 0 ) return E_LIMIT;
	return rec_write(SYSDEF_CONF_NET, text, len);	/* the system takes it as it sees it written */
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
	if ( rec_read(SYSDEF_CONF_USER, text, sizeof(text), &len) >= E_OK ) {
		if ( cf_get(text, len, CF_USER_NAME, 0, v, sizeof(v)) ) strncpy(was_name, v, sizeof(was_name) - 1);
		/* the method for the next start, which may not be the one in use */
		if ( cf_get(text, len, "KANA_INPUT", 0, v, sizeof(v)) ) next = ( cf_num(v, 0) != 0 ) ? 1 : 2;
	}
	ui_set_now(&ui, P_INPUT, next, ( kana_now != 0 ) ? 1 : 2);
	ui_set_text(&ui, P_NAME, was_name);
	(void)dt_getsystz(&z);
	was_zone = z;
	z = zone_index(z);
	ui_set_fields(&ui, P_ZONE, &z, 1);
	was_ntp[0] = 0;
	if ( rec_read(SYSDEF_CONF_NET, text, sizeof(text), &len) >= E_OK ) {
		cf_net_read(text, len, &n);
		strncpy(was_ntp, n.ntp, sizeof(was_ntp) - 1);
	}
	ui_set_value(&ui, P_NTP_ON, was_ntp[0] != 0);
	ui_set_text(&ui, P_NTP, ( was_ntp[0] != 0 ) ? was_ntp : "pool.ntp.org");
	ui_off(&ui, P_NTP, was_ntp[0] == 0);
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
LOCAL UB	snd_meta[NSND][OB_ATR_MAX];
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

	*p_hz = 2000;
	*p_ms = 100;
	snd_mlen[i] = 0;
	if ( ui_uuid(snd_obj[i], &u) < E_OK ) return;
	key = ob_opn_obj(&u, OB_OP_ATRRD);
	if ( key < E_OK ) return;
	if ( ob_get_atr(key, snd_meta[i], OB_ATR_MAX - 1, &snd_mlen[i]) < E_OK ) snd_mlen[i] = 0;
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
	static UB out[OB_ATR_MAX];	/* kept off the stack */
	char	t[48];
	INT	a, tl, n;
	ER	er;

	if ( snd_mlen[i] <= 0 || !tone_span(snd_meta[i], (INT)snd_mlen[i], &tone) ) return E_NOEXS;
	a = (INT)( tone.s - snd_meta[i] );
	tl = snprintf(t, sizeof(t), "{\"hz\":%d,\"ms\":%d}", hz, ms);
	n = (INT)snd_mlen[i] - tone.len + tl;
	if ( n >= OB_ATR_MAX ) return E_LIMIT;
	memcpy(out, snd_meta[i], a);
	memcpy(out + a, t, tl);
	memcpy(out + a + tl, snd_meta[i] + a + tone.len, snd_mlen[i] - a - tone.len);
	if ( ui_uuid(snd_obj[i], &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key < E_OK ) return (ER)key;
	er = ob_set_atr(key, out, n);
	ob_cls_obj(key);
	if ( er >= E_OK ) snd_changed = TRUE;
	return er;
}

/*
 * A sound object played as the system plays it: its record 1 when that
 * is a WAV file, else the tone its metadata says, written to the sound
 * device's object (each opening a channel of its own).
 */
#define PLAY_RATE	48000
LOCAL H		play_pcm[PLAY_RATE * 3 / 2];

LOCAL void play( INT i )
{
	TS_UUID		u, dev;
	ID		key, dk;
	SZ		asz = 0;
	SDPcmMode	m;
	T_JSON		tone;
	INT		hz = 2000, ms = 100, n, k, half, e;
	W		v;

	if ( snd_mlen[i] > 0 && tone_span(snd_meta[i], (INT)snd_mlen[i], &tone) ) {
		hz = (INT)js_get_num(&tone, "hz", 2000);
		ms = (INT)js_get_num(&tone, "ms", 100);
	}
	/* a WAV file of its own is the system's to play; here the tone is what is being set */
	if ( ui_uuid(snd_obj[i], &u) < E_OK || ob_fnd_nam((CONST UB *)"snda", &dev) < E_OK ) return;
	if ( hz < 50 ) hz = 50;
	if ( ms < 5 ) ms = 5;
	n = PLAY_RATE / 1000 * ms;
	if ( n > (INT)( sizeof(play_pcm) / sizeof(play_pcm[0]) ) ) n = (INT)( sizeof(play_pcm) / sizeof(play_pcm[0]) );
	half = PLAY_RATE / hz / 2;
	if ( half < 1 ) half = 1;
	for ( k = 0; k < n; k++ ) {
		e = ( k < 48 ) ? k : ( n - k < 48 ) ? n - k : 48;
		play_pcm[k] = (H)( ( ( ( k / half ) & 1 ) ? -6000 : 6000 ) * e / 48 );
	}
	(void)key;
	dk = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
	if ( dk < E_OK ) return;
	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	v = n * 2;
	if ( ob_wri_rec(dk, OB_SND_MODE, 0, &m, sizeof(m), &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_BUFSZ, 0, &v, sizeof(v), &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_PCM, 0, play_pcm, n * 2, &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_ENDCNT, 0, &v, sizeof(v), &asz) >= E_OK ) {
		v = PCM_PLAY;
		if ( ob_wri_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz) >= E_OK ) {
			for ( k = 0; k < ms / 10 + 50; k++ ) {
				(void)tk_dly_tsk(10);
				if ( ob_rea_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz) < E_OK || v == PCM_STOP ) break;
			}
		}
	}
	ob_cls_obj(dk);
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
	  || zones[zone].min != was_zone || kana_next != kana_now ) {
		return TRUE;
	}
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
	(void)dt_setsystz(was_zone);
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
	if ( er >= E_OK && strcmp(ntp, was_ntp) != 0 ) er = ntp_write(ntp);
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
		INT		t = ( i < 11 ) ? 2 : ( i < 19 ) ? 3 : ( i < 29 ) ? 4 : ( i < 32 ) ? 5 : 6;

		if ( t != tag || it == NULL ) continue;
		(void)wm_set_look(l->look, (UW)it->dflt);
		lk_show(l);
	}
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
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		finish();
	} else if ( is_code(&sel, "clear") && box ) {
		ui_set_text(&ui, ui.last, "");
	} else if ( is_code(&sel, "standard") ) {
		standard();
	}
}

/* ---------------------------------------------------------------- what happens */

LOCAL void changed( INT id )
{
	CONST LKPART	*l = lk_of(id);
	INT		z = 0;

	if ( l != NULL ) {
		to_system(l);
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
		(void)dt_setsystz(zones[z].min);
		shown_min = -1;
		time_show();
		break;
	case P_NTP_ON:
		ui_off(&ui, P_NTP, ui_value(&ui, P_NTP_ON) == 0);
		break;
	default:
		break;
	}
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	id, ev, n = 0;

	ui_icon(SYSDEF_PROG_USERENV);
	if ( ui_open(&ui, SYSDEF_BOX_USERENV, PN_MAIN, 180, 90) < E_OK ) {
		tm_putstring((CONST UB *)"userenv: no window\n");
		return 1;
	}
	read_start();
	tm_putstring((CONST UB *)"userenv: ready\n");

	while ( !closing ) {
		while ( !closing && ob_rea_rec(ui.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
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
				if ( id == P_BEEP ) play(0);
				if ( id == P_CLICK ) {
					play(1);
					(void)tk_dly_tsk(100);
					play(2);
				}
				break;
			default:
				break;
			}
		}
		if ( !closing ) {
			if ( ++n % 50 == 0 && ui_sheet(&ui) == TAG_ME ) {
				time_show();			/* the clock goes on */
			}
			(void)tk_dly_tsk(TICK_MS);
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"userenv: closed\n");
	return 0;
}
