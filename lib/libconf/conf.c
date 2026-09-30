/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	conf.c
 *	The system's settings as text (design 16.5.23)
 *
 *	Lines of "key value" in a record: finding the nth line of a key,
 *	putting a value on it, and the words, numbers and addresses the
 *	values are made of. The kernel and the settings accessories both
 *	link this file, so it calls nothing and uses no library.
 */

/* built into the kernel, the machine's own types; into a process, the process's */
#if defined(_QEMU_VIRT_) || defined(_RPI5_)
#include <sys/machine.h>
#include <tk/tkernel.h>
#endif
#include <tk/typedef.h>
#include <ts/look.h>
#include <ts/conf.h>

LOCAL BOOL blank( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' );
}

LOCAL INT slen( CONST char *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != 0 ) n++;
	return n;
}

LOCAL void scopy( char *d, CONST char *s, INT max )
{
	INT	i;

	for ( i = 0; i < max - 1 && s[i] != 0; i++ ) d[i] = s[i];
	if ( max > 0 ) d[i] = 0;
}

LOCAL BOOL seq( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 || b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

/*
 * Where the nth line of a key starts and ends in the text, and where its
 * value starts: -1 when there is no such line.
 */
LOCAL INT line_of( CONST UB *text, INT len, CONST char *key, INT nth, INT *p_end, INT *p_val )
{
	INT	at = 0, k = slen(key), n = 0, e, v, i;

	while ( at < len ) {
		for ( e = at; e < len && text[e] != '\n'; e++ ) ;
		if ( e - at >= k && text[at] != '#' ) {
			for ( i = 0; i < k && text[at + i] == (UB)key[i]; i++ ) ;
			if ( i == k && ( at + k == e || blank(text[at + k]) || text[at + k] == '\r' ) ) {
				if ( n++ == nth ) {
					for ( v = at + k; v < e && blank(text[v]); v++ ) ;
					*p_end = e;
					*p_val = v;
					return at;
				}
			}
		}
		at = e + 1;
	}
	return -1;
}

EXPORT BOOL cf_find( CONST UB *text, INT len, CONST char *key, INT nth,
		     CONST UB **p_val, INT *p_len )
{
	INT	e, v;

	if ( text == NULL || key == NULL || line_of(text, len, key, nth, &e, &v) < 0 ) {
		return FALSE;
	}
	while ( e > v && ( text[e - 1] == '\r' || blank(text[e - 1]) ) ) e--;
	*p_val = text + v;
	*p_len = e - v;
	return TRUE;
}

EXPORT BOOL cf_get( CONST UB *text, INT len, CONST char *key, INT nth,
		    char *out, INT max )
{
	CONST UB	*v;
	INT		n, i;

	if ( max <= 0 ) return FALSE;
	out[0] = 0;
	if ( !cf_find(text, len, key, nth, &v, &n) ) return FALSE;
	for ( i = 0; i < n && i < max - 1; i++ ) out[i] = (char)v[i];
	out[i] = 0;
	return TRUE;
}

EXPORT INT cf_put( UB *text, INT len, INT max, CONST char *key, INT nth,
		   CONST char *val )
{
	INT	at, e, v, k = slen(key), vl = slen(val), nl, i, have = 0;

	/* the line as it is to read: the key, a tab and the value */
	nl = k + ( vl > 0 ? 1 + vl : 0 );
	at = line_of(text, len, key, nth, &e, &v);
	if ( at < 0 ) {
		/* not there: added after the last line, as many as it takes */
		for ( i = 0; i < 64; i++ ) {
			if ( line_of(text, len, key, i, &e, &v) >= 0 ) have++;
			else break;
		}
		while ( have <= nth ) {
			INT	add = ( len > 0 && text[len - 1] != '\n' ) ? 1 : 0;
			INT	this = ( have == nth ) ? nl : k;

			if ( len + add + this + 1 > max ) return -1;
			if ( add ) text[len++] = '\n';
			for ( i = 0; i < k; i++ ) text[len++] = (UB)key[i];
			if ( have == nth && vl > 0 ) {
				text[len++] = '\t';
				for ( i = 0; i < vl; i++ ) text[len++] = (UB)val[i];
			}
			text[len++] = '\n';
			have++;
		}
		return len;
	}

	/* there: what stood from at to e is replaced by the new line */
	if ( len - ( e - at ) + nl > max ) return -1;
	if ( nl != e - at ) {
		INT	shift = nl - ( e - at );

		if ( shift > 0 ) {
			for ( i = len - 1; i >= e; i-- ) text[i + shift] = text[i];
		} else {
			for ( i = e; i < len; i++ ) text[i + shift] = text[i];
		}
		len += shift;
	}
	for ( i = 0; i < k; i++ ) text[at + i] = (UB)key[i];
	if ( vl > 0 ) {
		text[at + k] = '\t';
		for ( i = 0; i < vl; i++ ) text[at + k + 1 + i] = (UB)val[i];
	}
	return len;
}

EXPORT BOOL cf_word( CONST char *val, INT nth, char *out, INT max )
{
	INT	at = 0, n = 0, i;

	if ( max <= 0 ) return FALSE;
	out[0] = 0;
	while ( val[at] != 0 ) {
		while ( blank((UB)val[at]) ) at++;
		if ( val[at] == 0 ) break;
		if ( n++ == nth ) {
			for ( i = 0; val[at + i] != 0 && !blank((UB)val[at + i]) && i < max - 1; i++ ) {
				out[i] = val[at + i];
			}
			out[i] = 0;
			return TRUE;
		}
		while ( val[at] != 0 && !blank((UB)val[at]) ) at++;
	}
	return FALSE;
}

EXPORT INT cf_num( CONST char *s, INT dflt )
{
	INT	v = 0, i = 0, d;
	BOOL	neg = FALSE, any = FALSE;

	if ( s == NULL ) return dflt;
	if ( s[0] == '-' ) {
		neg = TRUE;
		i = 1;
	}
	if ( s[i] == '0' && ( s[i + 1] == 'x' || s[i + 1] == 'X' ) ) {
		for ( i += 2; s[i] != 0; i++ ) {
			if ( s[i] >= '0' && s[i] <= '9' ) d = s[i] - '0';
			else if ( s[i] >= 'a' && s[i] <= 'f' ) d = s[i] - 'a' + 10;
			else if ( s[i] >= 'A' && s[i] <= 'F' ) d = s[i] - 'A' + 10;
			else return dflt;
			v = v * 16 + d;
			any = TRUE;
		}
	} else {
		for ( ; s[i] != 0; i++ ) {
			if ( s[i] < '0' || s[i] > '9' ) return dflt;
			v = v * 10 + ( s[i] - '0' );
			any = TRUE;
		}
	}
	if ( !any ) return dflt;
	return neg ? -v : v;
}

EXPORT BOOL cf_ip( CONST char *s, UW *p_addr )
{
	UW	b[4];
	INT	n = 0, i = 0, d;

	for ( n = 0; n < 4; n++ ) {
		if ( s[i] < '0' || s[i] > '9' ) return FALSE;
		for ( b[n] = 0, d = 0; s[i] >= '0' && s[i] <= '9'; i++, d++ ) {
			b[n] = b[n] * 10 + (UW)( s[i] - '0' );
			if ( d >= 3 || b[n] > 255 ) return FALSE;
		}
		if ( n < 3 ) {
			if ( s[i] != '.' ) return FALSE;
			i++;
		}
	}
	if ( s[i] != 0 ) return FALSE;
	/* network byte order: the first part in the lowest byte */
	*p_addr = b[0] | ( b[1] << 8 ) | ( b[2] << 16 ) | ( b[3] << 24 );
	return TRUE;
}

EXPORT INT cf_itoa( INT v, char *out )
{
	char	t[12];
	INT	n = 0, i = 0;
	UINT	u = ( v < 0 ) ? (UINT)( -v ) : (UINT)v;

	do {
		t[n++] = (char)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	if ( v < 0 ) out[i++] = '-';
	while ( n > 0 ) out[i++] = t[--n];
	out[i] = 0;
	return i;
}

EXPORT void cf_ipstr( UW addr, char *out )
{
	INT	i, at = 0;

	for ( i = 0; i < 4; i++ ) {
		at += cf_itoa((INT)( ( addr >> ( i * 8 ) ) & 0xFF ), out + at);
		if ( i < 3 ) out[at++] = '.';
	}
	out[at] = 0;
}

/* ---------------------------------------------------------------- the network */

LOCAL CONST char *const cf_noname[3] = { "___0", "___1", "___2" };

/* A name as the record has it: "" for the stand-in of an empty one */
LOCAL void name_in( char *out, CONST char *w, INT max, INT which )
{
	if ( seq(w, cf_noname[which]) ) out[0] = 0;
	else scopy(out, w, max);
}

EXPORT void cf_net_read( CONST UB *text, INT len, T_CFNET *net )
{
	char	v[CF_VAL_MAX], w[80];
	INT	i, j;

	for ( i = 0; i < (INT)sizeof(*net); i++ ) ((UB *)net)[i] = 0;
	cf_ip("255.255.255.0", &net->mask);

	if ( cf_get(text, len, "$$HOST", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) ) {
		name_in(net->host, w, sizeof(net->host), 0);
		if ( cf_word(v, 2, w, sizeof(w)) ) (void)cf_ip(w, &net->mask);
		if ( cf_word(v, 3, w, sizeof(w)) ) (void)cf_ip(w, &net->addr);
	}
	for ( j = 0; j < 2; j++ ) {
		if ( !cf_get(text, len, "$$DNS", j, v, sizeof(v)) || !cf_word(v, 0, w, sizeof(w)) ) continue;
		name_in(net->dnsname[j], w, sizeof(net->dnsname[j]), j + 1);
		if ( cf_word(v, 3, w, sizeof(w)) ) (void)cf_ip(w, &net->dns[j]);
		for ( i = 4; cf_word(v, i, w, sizeof(w)); i++ ) {
			if ( w[0] == 'D' && w[1] == 'O' && w[2] == 'M' && w[3] == 'A'
			  && w[4] == 'I' && w[5] == 'N' && w[6] == '=' && net->domain[0] == 0 ) {
				scopy(net->domain, w + 7, sizeof(net->domain));
			}
		}
	}
	if ( cf_get(text, len, "$$GW", 0, v, sizeof(v)) && cf_word(v, 3, w, sizeof(w)) ) {
		net->usegw = cf_ip(w, &net->gw);
	}
	if ( cf_get(text, len, "$$NTP", 0, v, sizeof(v)) ) {
		(void)cf_word(v, 0, net->ntp, sizeof(net->ntp));
	}
}

/* "<name> cnet <mask> <address>" and what follows */
LOCAL void host_line( char *out, CONST char *name, INT which, UW mask, UW addr, CONST char *tail )
{
	INT	at;

	scopy(out, ( name[0] != 0 ) ? name : cf_noname[which], 80);
	at = slen(out);
	scopy(out + at, " cnet ", 8);
	at = slen(out);
	cf_ipstr(mask, out + at);
	at = slen(out);
	out[at++] = ' ';
	cf_ipstr(addr, out + at);
	at = slen(out);
	if ( tail != NULL && tail[0] != 0 ) {
		out[at++] = ' ';
		scopy(out + at, tail, CF_VAL_MAX * 2 - at);
	}
}

EXPORT INT cf_net_write( UB *text, INT len, INT max, CONST T_CFNET *net )
{
	char	v[CF_VAL_MAX * 2], tail[CF_VAL_MAX];
	INT	j;

	host_line(v, net->host, 0, net->mask, net->addr, "NETDRV DEVICE=neta");
	len = cf_put(text, len, max, "$$HOST", 0, v);
	for ( j = 0; j < 2 && len >= 0; j++ ) {
		if ( net->dns[j] != 0 || ( j == 0 && net->domain[0] != 0 ) || net->dnsname[j][0] != 0 ) {
			tail[0] = 0;
			if ( j == 0 && net->domain[0] != 0 ) {
				scopy(tail, "DOMAIN=", sizeof(tail));
				scopy(tail + 7, net->domain, sizeof(tail) - 7);
			}
			host_line(v, net->dnsname[j], j + 1, net->mask, net->dns[j], tail);
			len = cf_put(text, len, max, "$$DNS", j, v);
		} else {
			len = cf_put(text, len, max, "$$DNS", j, NULL);
		}
	}
	if ( len >= 0 ) {
		if ( net->usegw ) {
			host_line(v, "gateway", 0, net->mask, net->gw, NULL);
			len = cf_put(text, len, max, "$$GW", 0, v);
		} else {
			len = cf_put(text, len, max, "$$GW", 0, NULL);
		}
	}
	if ( len >= 0 ) len = cf_put(text, len, max, "$$NTP", 0, net->ntp);
	return len;
}

/* ---------------------------------------------------------------- the person */

/*
 * What ユーザ環境設定 sets. The ranges and what nothing said gives are
 * the accessory's own: times in milliseconds, sizes in dots, pitches
 * in hertz.
 */
EXPORT CONST CF_ITEM cf_user_items[] = {
	{ "KANA_INPUT",		LK_KANA,	0, 1, 0 },

	{ "PD_ON",		LK_PD_ON,	0, 2500, 0 },
	{ "PD_OFF",		LK_PD_OFF,	0, 2500, 0 },
	{ "DOUBLE_CLICK",	LK_DBLTIME,	100, 2000, 400 },
	{ "PD_TOLERANCE",	LK_DBL_W,	4, 24, 10 },
	{ "PD_SPEED",		LK_PD_SPEED,	1, 15, 12 },
	{ "PD_ACCEL",		LK_PD_ACCEL,	0, 7, 7 },
	{ "PD_KEYSPEED",	LK_PD_KEYSPD,	1, 15, 4 },
	{ "PD_MAIN",		LK_PD_MAIN,	0, 1, 0 },
	{ "PD_ABSOLUTE",	LK_PD_ABS,	0, 1, 1 },
	{ "PD_MIDDLE_DOUBLE",	LK_PD_MIDDBL,	0, 1, 1 },
	{ "PD_WHEEL",		LK_PD_WHEEL,	0, 1, 1 },

	{ "KEY_ON",		LK_KEY_ON,	0, 2500, 0 },
	{ "KEY_OFF",		LK_KEY_OFF,	0, 2500, 0 },
	{ "KEY_SIMULTANEOUS",	LK_KEY_SIM,	0, 900, 0 },
	{ "KEY_REPEAT",		LK_KRP,		0, 1, 1 },
	{ "REPEAT_START",	LK_KRP_START,	100, 2000, 800 },
	{ "REPEAT_INTERVAL",	LK_KRP_INT,	50, 900, 100 },
	{ "SHIFT_CLICK",	LK_SCLK,	0, 900, 0 },
	{ "ONE_SHOT_SHIFT",	LK_TSHIFT,	0, 1, 0 },

	{ "TITLE_SIZE",		LK_TITLE_H,	8, 48, 16 },
	{ "SCROLL_WIDTH",	LK_BAR_W,	6, 32, 16 },
	{ "MENU_SIZE",		LK_MENU_H,	8, 48, 16 },
	{ "CARET_BLINK",	LK_BLINK,	0, 900, 800 },
	{ "SELECT_MARCH",	LK_MARCH,	50, 900, 800 },
	{ "MENU_DELAY",		LK_MENU_DLY,	50, 900, 100 },
	{ "CARET_WIDTH",	LK_CARET_W,	1, 3, 1 },
	{ "SELECT_WIDTH",	LK_SEL_W,	1, 3, 1 },
	{ "POINTER_SIZE",	LK_PTR_SIZE,	0, 2, 1 },
	{ "SCROLL_FOLLOW",	LK_SCR_LIVE,	0, 1, 1 },

	{ "CLICK_KEY",		LK_CLK_KEY,	0, 1, 0 },
	{ "CLICK_BUTTON",	LK_CLK_BTN,	0, 1, 0 },
	{ "SPEAKER",		LK_SPEAKER,	0, 1, 1 },

	{ "EDIT_KEYS",		LK_EDITKEYS,	0, 1, 0 },
};

EXPORT CONST INT cf_user_nitem = (INT)( sizeof(cf_user_items) / sizeof(cf_user_items[0]) );

EXPORT CONST CF_ITEM *cf_user_item( CONST char *key )
{
	INT	i;

	for ( i = 0; i < cf_user_nitem; i++ ) {
		if ( seq(cf_user_items[i].key, key) ) return &cf_user_items[i];
	}
	return NULL;
}

EXPORT CONST CF_ITEM *cf_user_look( UINT look )
{
	INT	i;

	for ( i = 0; i < cf_user_nitem; i++ ) {
		if ( cf_user_items[i].look == look ) return &cf_user_items[i];
	}
	return NULL;
}
