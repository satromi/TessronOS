/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	keymap.c
 *	The keyboard's layouts and the keys given other meanings (ts/keymap.h)
 *
 *	The letters are tables over the key numbers 0x04 (the 'a' place) to
 *	0x38 (the '/' place), one for each shift state; what a shift does to
 *	a key is a table, not arithmetic. The Japanese keyboard has 0x35 as
 *	半角/全角, which types nothing, both 0x31 and 0x32 as the ] key, and
 *	two keys past the range, 0x87 (ろ, \ _) and 0x89 (¥, \ |). Dvorak is
 *	laid on the English keyboard's places and is the same on either.
 *
 *	The keys given other numbers are a table of 256 bytes that the
 *	keyboard's reports are looked up in as they come (device/usb/hid.c),
 *	under no lock: a byte is written whole, and a report taken while the
 *	table is being changed is one key either way. The table and the
 *	layouts are kept in the machine's settings (SYSDEF_CONF_DEV) as the
 *	lines KBLAYOUT, KBKANA and KBMAP, read at start-up (uconf.c).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/look.h>
#include <ts/keymap.h>
#include <ts/kconv.h>
#include <ts/conf.h>

/* ---------------------------------------------------------------- the letters */

LOCAL CONST char	jp_plain[] =
	"abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\033\b\t "
	"-^@[]];:" "\0" ",./";
LOCAL CONST char	jp_shift[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!\"#$%&'()\0" "\n\033\b\t "
	"=~`{}}+*" "\0" "<>?";
LOCAL CONST char	us_plain[] =
	"abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\033\b\t "
	"-=[]\\\\;'" "`" ",./";
LOCAL CONST char	us_shift[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()" "\n\033\b\t "
	"_+{}||:\"" "~" "<>?";
LOCAL CONST char	dv_plain[] =
	"axje.uidchtnmbrl'poygk,qf;1234567890\n\033\b\t []/=\\\\s-`wvz";
LOCAL CONST char	dv_shift[] =
	"AXJE>UIDCHTNMBRL\"POYGK<QF:!@#$%^&*()\n\033\b\t {}?+||S_~WVZ";

EXPORT INT km_latin( void )
{
	INT	v = (INT)wm_num(LK_KBD_LAYOUT, KM_LATIN_KEYS);

	return ( v >= 0 && v < KM_LATIN_N ) ? v : KM_LATIN_KEYS;
}

EXPORT INT km_kana( void )
{
	INT	v = (INT)wm_num(LK_KBD_KANA, KM_KANA_JIS);

	return ( v >= 0 && v < KM_KANA_N ) ? v : KM_KANA_JIS;
}

EXPORT INT km_latin_of( UINT mods )
{
	UINT	v = ( mods >> KM_MOD_LATIN_SHIFT ) & 3U;

	if ( ( mods & KM_MOD_OWN ) == 0 || v >= KM_LATIN_N ) {
		return km_latin();
	}
	return (INT)v;
}

EXPORT INT km_kana_of( UINT mods )
{
	UINT	v = ( mods >> KM_MOD_KANA_SHIFT ) & 3U;

	if ( ( mods & KM_MOD_OWN ) == 0 || v >= KM_KANA_N ) {
		return km_kana();
	}
	return (INT)v;
}

/* The keypad's keys type the same in every layout */
LOCAL UB km_pad( UINT code )
{
	if ( code >= 0x59 && code <= 0x61 ) return (UB)( '1' + code - 0x59 );
	switch ( code ) {
	case 0x54:	return '/';
	case 0x55:	return '*';
	case 0x56:	return '-';
	case 0x57:	return '+';
	case 0x62:	return '0';
	case 0x63:	return '.';
	case 0x67:	return '=';
	case 0x85:	return ',';
	default:	return 0;
	}
}

EXPORT UB km_char( UINT code, UINT mods )
{
	BOOL	up = (BOOL)( ( mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 );
	BOOL	us = (BOOL)( wm_num(LK_KBD_US, 0) != 0 );

	if ( km_pad(code) != 0 ) {
		return km_pad(code);
	}
	if ( km_latin_of(mods) == KM_LATIN_DVORAK ) {
		if ( code == 0x87 || code == 0x89 ) {
			return (UB)( up ? '|' : '\\' );
		}
		if ( code < 0x04 || code > 0x38 || ( !us && code == 0x35 ) ) {
			return 0;
		}
		return (UB)( up ? dv_shift[code - 0x04] : dv_plain[code - 0x04] );
	}
	if ( us ) {
		if ( code < 0x04 || code > 0x38 ) {
			return 0;
		}
		return (UB)( up ? us_shift[code - 0x04] : us_plain[code - 0x04] );
	}
	if ( code == 0x87 ) {
		return (UB)( up ? '_' : '\\' );
	}
	if ( code == 0x89 ) {
		return (UB)( up ? '|' : '\\' );
	}
	if ( code < 0x04 || code > 0x38 ) {
		return 0;
	}
	return (UB)( up ? jp_shift[code - 0x04] : jp_plain[code - 0x04] );
}

/* ---------------------------------------------------------------- the kana by the thumbs */

/*
 * TRON かな配列 and 親指シフト (NICOLA): each key has three kana, typed
 * alone, with the thumb key on its own hand's side, and with the other.
 */
typedef struct {
	UB		code;
	CONST char	*plain, *same, *cross;
} KMKANA;

LOCAL CONST KMKANA	km_nicola[] = {
	{ 0x04, "う", "を", "ヴ" },
	{ 0x05, "へ", "ぃ", "べ" },
	{ 0x06, "す", "ろ", "ず" },
	{ 0x07, "て", "な", "で" },
	{ 0x08, "た", "り", "だ" },
	{ 0x09, "け", "ゅ", "げ" },
	{ 0x0A, "せ", "も", "ぜ" },
	{ 0x0B, "は", "み", "ば" },
	{ 0x0C, "く", "る", "ぐ" },
	{ 0x0D, "と", "お", "ど" },
	{ 0x0E, "き", "の", "ぎ" },
	{ 0x0F, "い", "ょ", "ぽ" },
	{ 0x10, "そ", "ゆ", "ぞ" },
	{ 0x11, "め", "ぬ", "ぷ" },
	{ 0x12, "つ", "ま", "づ" },
	{ 0x13, "，", "ぇ", "ぴ" },
	{ 0x14, "。", "ぁ", "" },
	{ 0x15, "こ", "ゃ", "ご" },
	{ 0x16, "し", "あ", "じ" },
	{ 0x17, "さ", "れ", "ざ" },
	{ 0x18, "ち", "に", "ぢ" },
	{ 0x19, "ふ", "や", "ぶ" },
	{ 0x1A, "か", "え", "が" },
	{ 0x1B, "ひ", "ー", "び" },
	{ 0x1C, "ら", "よ", "ぱ" },
	{ 0x1D, "．", "ぅ", "" },
	{ 0x2F, "、", "", "" },
	{ 0x33, "ん", "っ", "っ" },
	{ 0x36, "ね", "む", "ぺ" },
	{ 0x37, "ほ", "わ", "ぼ" },
	{ 0x38, "・", "ぉ", "" },
};

LOCAL CONST KMKANA	km_tron[] = {
	{ 0x04, "た", "ぬ", "だ" },
	{ 0x05, "な", "へ", "べ" },
	{ 0x06, "に", "せ", "ぜ" },
	{ 0x07, "か", "ゅ", "が" },
	{ 0x08, "こ", "・", "ご" },
	{ 0x09, "て", "よ", "で" },
	{ 0x0A, "も", "ふ", "ぶ" },
	{ 0x0B, "を", "お", "゛" },
	{ 0x0C, "く", "め", "ぐ" },
	{ 0x0D, "い", "ち", "ぢ" },
	{ 0x0E, "う", "ー", "ヴ" },
	{ 0x0F, "し", "み", "じ" },
	{ 0x10, "つ", "ぃ", "づ" },
	{ 0x11, "す", "わ", "ず" },
	{ 0x12, "あ", "む", "" },
	{ 0x13, "れ", "ろ", "ゐ" },
	{ 0x14, "ら", "ひ", "び" },
	{ 0x15, "は", "ゃ", "ば" },
	{ 0x16, "と", "ね", "ど" },
	{ 0x17, "ょ", "ほ", "ぼ" },
	{ 0x18, "の", "け", "げ" },
	{ 0x19, "さ", "ゆ", "ざ" },
	{ 0x1A, "る", "そ", "ぞ" },
	{ 0x1B, "り", "ぉ", "ヶ" },
	{ 0x1C, "き", "え", "ぎ" },
	{ 0x1D, "ま", "ぇ", "ヵ" },
	{ 0x33, "ん", "や", "ゑ" },
	{ 0x34, "ー", "", "" },
	{ 0x36, "、", "ぁ", "，" },
	{ 0x37, "。", "゜", "．" },
	{ 0x38, "っ", "ぅ", "ゎ" },
};

EXPORT BOOL km_left_hand( UINT code )
{
	switch ( code ) {
	case 0x14: case 0x1A: case 0x08: case 0x15: case 0x17:
	case 0x04: case 0x16: case 0x07: case 0x09: case 0x0A:
	case 0x1D: case 0x1B: case 0x06: case 0x19: case 0x05:
	case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22:
		return TRUE;
	default:
		return FALSE;
	}
}

EXPORT CONST char *km_kana_text( UINT code, UINT mods )
{
	CONST KMKANA	*t;
	UINT		n, i;
	CONST char	*s;
	BOOL		l = (BOOL)( ( mods & KM_MOD_THUMB_L ) != 0 );
	BOOL		r = (BOOL)( ( mods & KM_MOD_THUMB_R ) != 0 );
	INT		kana = km_kana_of(mods);

	/* TRON かな: the TRON keyboard's red (left) and blue (right) Shift choose the planes too */
	if ( kana == KM_KANA_TRON ) {
		l = (BOOL)( l || ( mods & HID_MOD_LSHIFT ) != 0 );
		r = (BOOL)( r || ( mods & HID_MOD_RSHIFT ) != 0 );
	}

	switch ( kana ) {
	case KM_KANA_TRON:
		t = km_tron;
		n = sizeof(km_tron) / sizeof(km_tron[0]);
		break;
	case KM_KANA_NICOLA:
		t = km_nicola;
		n = sizeof(km_nicola) / sizeof(km_nicola[0]);
		break;
	default:
		return NULL;
	}
	for ( i = 0; i < n; i++ ) {
		if ( t[i].code != code ) continue;
		if ( !l && !r ) s = t[i].plain;
		else if ( km_left_hand(code) ? l : r ) s = t[i].same;
		else s = t[i].cross;
		return ( s[0] != 0 ) ? s : NULL;
	}
	return NULL;
}

/* ---------------------------------------------------------------- the keys given others */

LOCAL UB	km_to[256];		/* 0: the key is itself */
LOCAL UINT	km_away = 0;		/* modifiers given another number */

LOCAL void km_away_make( void )
{
	UINT	i, a = 0;

	for ( i = 0; i < 8; i++ ) {
		if ( km_to[0xE0 + i] != 0 && km_to[0xE0 + i] != 0xE0 + i ) {
			a |= 1U << i;
		}
	}
	km_away = a;
}

EXPORT UINT km_remap( UINT code )
{
	return ( code < 256 && km_to[code] != 0 ) ? km_to[code] : code;
}

EXPORT UINT km_mods_away( void )
{
	return km_away;
}

EXPORT INT km_map_get( UB *from, UB *to, INT max )
{
	INT	i, n = 0;

	for ( i = 1; i < 256 && n < max; i++ ) {
		if ( km_to[i] != 0 && km_to[i] != (UB)i ) {
			if ( from != NULL ) from[n] = (UB)i;
			if ( to != NULL ) to[n] = km_to[i];
			n++;
		}
	}
	return n;
}

EXPORT ER km_map_set( CONST UB *from, CONST UB *to, INT n )
{
	UB	t[256];
	INT	i;

	if ( n < 0 || n > KM_MAP_MAX ) {
		return E_PAR;
	}
	knl_memset(t, 0, sizeof(t));
	for ( i = 0; i < n; i++ ) {
		if ( from[i] == 0 || to[i] == 0 ) {
			return E_PAR;
		}
		t[from[i]] = to[i];
	}
	for ( i = 0; i < 256; i++ ) {
		km_to[i] = t[i];
	}
	km_away_make();
	return E_OK;
}

LOCAL CONST char	km_hex[] = "0123456789ABCDEF";

EXPORT INT km_map_text( char *buf, INT max )
{
	UB	f[KM_MAP_MAX], t[KM_MAP_MAX];
	INT	n = km_map_get(f, t, KM_MAP_MAX), i, k = 0;

	for ( i = 0; i < n && k + 6 < max; i++ ) {
		if ( k > 0 ) buf[k++] = ',';
		buf[k++] = km_hex[f[i] >> 4];
		buf[k++] = km_hex[f[i] & 15];
		buf[k++] = km_hex[t[i] >> 4];
		buf[k++] = km_hex[t[i] & 15];
	}
	if ( max > 0 ) buf[k] = 0;
	return k;
}

LOCAL INT hexval( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	return -1;
}

EXPORT void km_map_parse( CONST char *s )
{
	UB	f[KM_MAP_MAX], t[KM_MAP_MAX];
	INT	n = 0, d[4], i;

	while ( s != NULL && *s != 0 && n < KM_MAP_MAX ) {
		for ( i = 0; i < 4; i++ ) d[i] = hexval(s[i]);
		if ( d[0] < 0 || d[1] < 0 || d[2] < 0 || d[3] < 0 ) {
			break;
		}
		f[n] = (UB)( d[0] * 16 + d[1] );
		t[n] = (UB)( d[2] * 16 + d[3] );
		if ( f[n] != 0 && t[n] != 0 ) n++;
		s += 4;
		while ( *s == ',' || *s == ' ' ) s++;
	}
	(void)km_map_set(f, t, n);
}

/* ---------------------------------------------------------------- kept */

LOCAL ER km_keep( CONST char *key, INT v )
{
	char	s[4];

	s[0] = (char)( '0' + v );
	s[1] = 0;
	return wm_dev_conf_put(key, s);
}

EXPORT ER km_set_latin( INT layout )
{
	if ( layout < 0 || layout >= KM_LATIN_N ) {
		return E_PAR;
	}
	(void)knl_look_setting(LK_KBD_LAYOUT, (UW)layout);
	return km_keep("KBLAYOUT", layout);
}

EXPORT ER km_set_kana( INT layout )
{
	if ( layout < 0 || layout >= KM_KANA_N ) {
		return E_PAR;
	}
	(void)knl_look_setting(LK_KBD_KANA, (UW)layout);
	return km_keep("KBKANA", layout);
}

EXPORT ER km_map_save( void )
{
	char	s[CF_VAL_MAX];

	(void)km_map_text(s, sizeof(s));
	return wm_dev_conf_put("KBMAP", s);
}

/* ---------------------------------------------------------------- names */

typedef struct {
	UB		code;
	CONST char	*name;
} KMNAME;

LOCAL CONST KMNAME	km_names[] = {
	{ 0x28, "Enter" }, { 0x29, "Esc" }, { 0x2A, "BS" }, { 0x2B, "Tab" }, { 0x2C, "空白" },
	{ 0x35, "半角/全角" }, { 0x39, "CapsLock" }, { 0x46, "PrintScreen" }, { 0x47, "ScrollLock" },
	{ 0x48, "Pause" }, { 0x49, "Insert" }, { 0x4A, "Home" }, { 0x4B, "PageUp" },
	{ 0x4C, "Delete" }, { 0x4D, "End" }, { 0x4E, "PageDown" }, { 0x4F, "→" }, { 0x50, "←" },
	{ 0x51, "↓" }, { 0x52, "↑" }, { 0x53, "NumLock" }, { 0x65, "Menu" },
	{ 0x87, "ろ" }, { 0x88, "カタカナひらがな" }, { 0x89, "¥" }, { 0x8A, "変換" }, { 0x8B, "無変換" },
	{ 0xE0, "左Ctrl" }, { 0xE1, "左Shift" }, { 0xE2, "左Alt" }, { 0xE3, "左Win" },
	{ 0xE4, "右Ctrl" }, { 0xE5, "右Shift" }, { 0xE6, "右Alt" }, { 0xE7, "右Win" },
};

LOCAL CONST char *CONST km_fkeys[12] = {
	"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"
};

EXPORT CONST char *km_key_name( UINT code )
{
	LOCAL char	one[2];
	UINT		i;

	for ( i = 0; i < sizeof(km_names) / sizeof(km_names[0]); i++ ) {
		if ( km_names[i].code == code ) {
			return km_names[i].name;
		}
	}
	if ( code >= 0x3A && code <= 0x45 ) {
		return km_fkeys[code - 0x3A];
	}
	/* a letter or a sign: what it types unshifted, a letter as a capital */
	one[0] = (char)km_char(code, 0);
	if ( one[0] >= 'a' && one[0] <= 'z' ) one[0] = (char)( one[0] - 'a' + 'A' );
	one[1] = 0;
	return ( one[0] > ' ' ) ? one : "?";
}

/* ---------------------------------------------------------------- for a process */

EXPORT ER km_get( T_KMSTATE *st )
{
	if ( st == NULL ) {
		return E_PAR;
	}
	knl_memset(st, 0, sizeof(*st));
	st->latin = km_latin();
	st->kana = km_kana();
	st->us = (BOOL)( wm_num(LK_KBD_US, 0) != 0 );
	st->nmap = km_map_get(st->from, st->to, KM_MAP_MAX);
	return E_OK;
}

EXPORT ER km_set( CONST T_KMSTATE *st, UINT what )
{
	INT	i;
	ER	er = E_OK;

	if ( st == NULL || ( what & ~( KM_SET_LATIN | KM_SET_KANA | KM_SET_MAP ) ) != 0 ) {
		return E_PAR;
	}
	/* all of it looked at before anything is set */
	if ( ( what & KM_SET_LATIN ) != 0 && ( st->latin < 0 || st->latin >= KM_LATIN_N ) ) {
		return E_PAR;
	}
	if ( ( what & KM_SET_KANA ) != 0 && ( st->kana < 0 || st->kana >= KM_KANA_N ) ) {
		return E_PAR;
	}
	if ( ( what & KM_SET_MAP ) != 0 ) {
		if ( st->nmap < 0 || st->nmap > KM_MAP_MAX ) {
			return E_PAR;
		}
		for ( i = 0; i < st->nmap; i++ ) {
			if ( st->from[i] == 0 || st->to[i] == 0 ) {
				return E_PAR;
			}
		}
	}
	if ( ( what & KM_SET_LATIN ) != 0 ) {
		er = km_set_latin(st->latin);
	}
	if ( er >= E_OK && ( what & KM_SET_KANA ) != 0 ) {
		er = km_set_kana(st->kana);
	}
	if ( er >= E_OK && ( what & KM_SET_MAP ) != 0 ) {
		er = km_map_set(st->from, st->to, st->nmap);
		if ( er >= E_OK ) {
			er = km_map_save();
		}
	}
	return er;
}

LOCAL INT copy_out( CONST char *s, char *buf, INT max )
{
	INT	n = 0;

	if ( s == NULL ) {
		s = "";
	}
	while ( s[n] != 0 && n < max - 1 ) {
		buf[n] = s[n];
		n++;
	}
	/* not cut in the middle of a letter */
	while ( s[n] != 0 && n > 0 && ( (UB)s[n] & 0xC0 ) == 0x80 ) {
		n--;
	}
	buf[n] = 0;
	return n;
}

EXPORT INT km_label( UINT what, UINT code, UINT mods, char *buf, INT max )
{
	char	one[2];

	if ( buf == NULL || max <= 0 || code > 0xFF ) {
		return E_PAR;
	}
	switch ( what ) {
	case KM_L_NAME:
		return copy_out(km_key_name(code), buf, max);
	case KM_L_CHAR:
		one[0] = (char)km_char(code, mods);
		one[1] = 0;
		return copy_out(( (UB)one[0] > ' ' && (UB)one[0] < 0x7F ) ? one : "", buf, max);
	case KM_L_KANA:
		return copy_out(km_kana_text(code, mods), buf, max);
	case KM_L_JIS:
		return copy_out(kc_kana_label(code, (BOOL)( ( mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 )),
				buf, max);
	default:
		return E_PAR;
	}
}
