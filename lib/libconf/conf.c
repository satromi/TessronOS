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
#if defined(_QEMU_VIRT_) || defined(_RPI5_) || defined(_RPI4_) || defined(_X64_)
#include <sys/machine.h>
#include <tk/tkernel.h>
#endif
#include <tk/typedef.h>
#include <ts/look.h>
#include <ts/conf.h>
#include <ts/uuid.h>
#include <ts/vobjsz.h>

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
		for ( i = 0; ; i++ ) {
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

EXPORT BOOL cf_wfind( CONST UB *val, INT len, INT nth, CONST UB **p_w, INT *p_wlen )
{
	INT	at = 0, n = 0, e;

	while ( val != NULL && at < len ) {
		while ( at < len && blank(val[at]) ) at++;
		if ( at >= len ) break;
		for ( e = at; e < len && !blank(val[e]); e++ ) ;
		if ( n++ == nth ) {
			*p_w = val + at;
			*p_wlen = e - at;
			return TRUE;
		}
		at = e;
	}
	return FALSE;
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

/* ---------------------------------------------------------------- IPv6 */

LOCAL INT hexd( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT BOOL cf_ip6zero( CONST UB *addr )
{
	INT	i;

	for ( i = 0; i < 16 && addr[i] == 0; i++ ) ;
	return (BOOL)( i == 16 );
}

/*
 * Groups of up to four hexadecimal digits parted by colons, one run of
 * them left out as "::", and the last 32 bits perhaps as a dotted IPv4
 * address.
 */
EXPORT BOOL cf_ip6( CONST char *s, UB *addr )
{
	UW	g[8], v4;
	INT	n = 0, gap = -1, i = 0, d, k, j;
	UW	v;

	if ( s == NULL ) return FALSE;
	if ( s[0] == ':' ) {
		if ( s[1] != ':' ) return FALSE;
		gap = 0;
		i = 2;
	}
	while ( s[i] != 0 ) {
		/* a dotted IPv4 address ends it */
		for ( j = i; s[j] != 0 && s[j] != ':' && s[j] != '.'; j++ ) ;
		if ( s[j] == '.' ) {
			if ( n > 6 || !cf_ip(s + i, &v4) ) return FALSE;
			g[n++] = ( ( v4 & 0xFF ) << 8 ) | ( ( v4 >> 8 ) & 0xFF );
			g[n++] = ( ( ( v4 >> 16 ) & 0xFF ) << 8 ) | ( ( v4 >> 24 ) & 0xFF );
			break;
		}
		for ( v = 0, d = 0; ( k = hexd(s[i]) ) >= 0; i++, d++ ) {
			if ( d >= 4 ) return FALSE;
			v = v * 16 + (UW)k;
		}
		if ( d == 0 || n >= 8 ) return FALSE;
		g[n++] = v;
		if ( s[i] == 0 ) break;
		if ( s[i] != ':' ) return FALSE;
		i++;
		if ( s[i] == ':' ) {
			if ( gap >= 0 ) return FALSE;
			gap = n;
			i++;
		} else if ( s[i] == 0 ) {
			return FALSE;		/* a colon at the end */
		}
	}
	if ( gap < 0 ? n != 8 : n > 7 ) return FALSE;
	for ( i = 0; i < 16; i++ ) addr[i] = 0;
	for ( j = 0; j < n; j++ ) {
		k = ( gap >= 0 && j >= gap ) ? 8 - n + j : j;
		addr[k * 2] = (UB)( g[j] >> 8 );
		addr[k * 2 + 1] = (UB)g[j];
	}
	return TRUE;
}

/* The groups in hexadecimal, the longest run of two or more zero groups (the first of equals) as "::" */
EXPORT void cf_ip6str( CONST UB *addr, char *out )
{
	static CONST char hx[] = "0123456789abcdef";
	UW	g[8];
	INT	i, at = 0, best = -1, blen = 0, run, k, sh;

	for ( i = 0; i < 8; i++ ) g[i] = ( (UW)addr[i * 2] << 8 ) | addr[i * 2 + 1];
	for ( i = 0; i < 8; i += ( run > 0 ) ? run : 1 ) {
		for ( run = 0; i + run < 8 && g[i + run] == 0; run++ ) ;
		if ( run >= 2 && run > blen ) {
			best = i;
			blen = run;
		}
	}
	/* ::ffff:a.b.c.d, an IPv4 address mapped */
	if ( best == 0 && blen == 5 && g[5] == 0xFFFF ) {
		out[at++] = ':';
		out[at++] = ':';
		for ( k = 0; k < 4; k++ ) out[at++] = "ffff"[k];
		out[at++] = ':';
		cf_ipstr((UW)addr[12] | ( (UW)addr[13] << 8 ) | ( (UW)addr[14] << 16 ) | ( (UW)addr[15] << 24 ), out + at);
		return;
	}
	for ( i = 0; i < 8; i++ ) {
		if ( i == best ) {
			out[at++] = ':';
			out[at++] = ':';
			i += blen - 1;
			continue;
		}
		if ( i > 0 && i != best + blen ) out[at++] = ':';
		for ( sh = 12; sh > 0 && ( ( g[i] >> sh ) & 0xF ) == 0; sh -= 4 ) ;
		for ( ; sh >= 0; sh -= 4 ) out[at++] = hx[( g[i] >> sh ) & 0xF];
	}
	out[at] = 0;
}

EXPORT void cf_net6_read( CONST UB *text, INT len, T_CFNET6 *net )
{
	char	v[CF_VAL_MAX], w[64];
	UB	a[16];
	INT	i, j;

	for ( i = 0; i < (INT)sizeof(*net); i++ ) ((UB *)net)[i] = 0;
	net->on = TRUE;
	net->how = CF_IP6_AUTO;
	net->plen = 64;

	if ( cf_get(text, len, "$$IPV6", 0, v, sizeof(v)) ) {
		if ( cf_word(v, 0, w, sizeof(w)) && seq(w, "off") ) net->on = FALSE;
		if ( cf_word(v, 1, w, sizeof(w)) ) {
			if ( seq(w, "fixed") ) net->how = CF_IP6_FIXED;
			else if ( seq(w, "none") ) net->how = CF_IP6_NONE;
		}
		for ( i = 2; cf_word(v, i, w, sizeof(w)); i++ ) {
			if ( seq(w, "temp") ) net->temp = TRUE;
		}
	}
	if ( cf_get(text, len, "$$IPV6ADDR", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) ) {
		for ( i = 0; w[i] != 0 && w[i] != '/'; i++ ) ;
		if ( w[i] == '/' ) {
			w[i] = 0;
			j = cf_num(w + i + 1, 64);
			net->plen = ( j >= 1 && j <= 128 ) ? j : 64;
		}
		if ( cf_ip6(w, a) ) for ( i = 0; i < 16; i++ ) net->addr[i] = a[i];
	}
	if ( cf_get(text, len, "$$IPV6GW", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) && cf_ip6(w, a) ) {
		for ( i = 0; i < 16; i++ ) net->gw[i] = a[i];
	}
	for ( j = 0; j < 2; j++ ) {
		if ( cf_get(text, len, "$$IPV6DNS", j, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) && cf_ip6(w, a) ) {
			for ( i = 0; i < 16; i++ ) net->dns[j][i] = a[i];
		}
	}
}

/* An address on its own line, or the key alone for none */
LOCAL INT put6( UB *text, INT len, INT max, CONST char *key, INT nth, CONST UB *a, INT plen )
{
	char	v[CF_IP6_STRLEN + 8];
	INT	at;

	if ( len < 0 ) return len;
	if ( cf_ip6zero(a) ) return cf_put(text, len, max, key, nth, NULL);
	cf_ip6str(a, v);
	if ( plen > 0 ) {
		at = slen(v);
		v[at++] = '/';
		(void)cf_itoa(plen, v + at);
	}
	return cf_put(text, len, max, key, nth, v);
}

EXPORT INT cf_net6_write( UB *text, INT len, INT max, CONST T_CFNET6 *net )
{
	char	v[32];
	INT	at;

	scopy(v, net->on ? "on " : "off ", sizeof(v));
	at = slen(v);
	scopy(v + at, ( net->how == CF_IP6_FIXED ) ? "fixed" : ( net->how == CF_IP6_NONE ) ? "none" : "auto",
	      (INT)sizeof(v) - at);
	if ( net->temp ) {
		at = slen(v);
		scopy(v + at, " temp", (INT)sizeof(v) - at);
	}
	len = cf_put(text, len, max, "$$IPV6", 0, v);
	len = put6(text, len, max, "$$IPV6ADDR", 0, net->addr, net->plen);
	len = put6(text, len, max, "$$IPV6GW", 0, net->gw, 0);
	len = put6(text, len, max, "$$IPV6DNS", 0, net->dns[0], 0);
	len = put6(text, len, max, "$$IPV6DNS", 1, net->dns[1], 0);
	return len;
}

/* ---------------------------------------------------------------- the person */

/*
 * What ユーザ環境設定 sets. The ranges and what nothing said gives are
 * the accessory's own: times in milliseconds, sizes in dots (a
 * finger's in tenths of a millimetre), pitches in hertz.
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
	{ "TOUCH_ADJUST",	LK_TCH_ON,	0, 1, 1 },
	{ "TOUCH_TOLERANCE",	LK_TCH_DBL,	10, 150, 50 },
	{ "TOUCH_HANDLE",	LK_TCH_HIT,	0, 100, 40 },
	{ "TOUCH_HOLD",		LK_TCH_HOLD,	300, 3000, 1000 },
	{ "TOUCH_SCROLL",	LK_TCH_SCR,	0, 1, 1 },
	{ "TOUCH_SCROLL_STEP",	LK_TCH_STEP,	30, 300, 100 },

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

/* ---------------------------------------------------------------- the windows' look */

/*
 * What システム環境設定〈ウィンドウ〉 sets, lines of the machine's
 * settings (SYSDEF_CONF_DEV). Colours are 0xRRGGBB, strengths and how
 * solid a frame is 0..255, lengths in dots, the motion in milliseconds.
 * What nothing says gives is how the windows look with no look set:
 * solid and square, with no shadow and no motion.
 */
EXPORT CONST CF_ITEM cf_look_items[] = {
	{ "WIN_LOOK",		LK_LOOK,	0, 1, 1 },
	{ "WIN_OPACITY",	LK_OPACITY,	0, 255, 255 },
	{ "WIN_OPACITY_WHOLE",	LK_OPA_WHOLE,	0, 1, 0 },
	{ "WIN_WASH",		LK_WASH,	0, 0xFFFFFF, 0x202838 },
	{ "WIN_WASH_STRENGTH",	LK_WASH_STR,	0, 255, 0 },
	{ "WIN_BAR_WASH",	LK_BAR_WASH,	0, 0xFFFFFF, 0x405878 },
	{ "WIN_BAR_STRENGTH",	LK_BAR_STR,	0, 255, 0 },
	{ "WIN_FROM_SCHEME",	LK_FROM,	0, 3, 1 },
	{ "WIN_ROUND",		LK_ROUND,	0, 32, 0 },
	{ "WIN_SHADOW_DX",	LK_SHD_DX,	-32, 32, 4 },
	{ "WIN_SHADOW_DY",	LK_SHD_DY,	-32, 32, 4 },
	{ "WIN_SHADOW_SIZE",	LK_SHD_SIZE,	0, 32, 2 },
	{ "WIN_SHADOW_DARK",	LK_SHD_DARK,	0, 255, 0 },
	{ "WIN_BLUR",		LK_BLUR,	0, 16, 0 },
	{ "WIN_MOTION",		LK_MOTION,	0, 2000, 0 },
	{ "WIN_LIVE_MOVE",	LK_LIVE,	0, 1, 1 },
	{ "WIN_GPU",		LK_GPU,		0, 1, 1 },
};

EXPORT CONST INT cf_look_nitem = (INT)( sizeof(cf_look_items) / sizeof(cf_look_items[0]) );

EXPORT CONST CF_ITEM *cf_look_look( UINT look )
{
	INT	i;

	for ( i = 0; i < cf_look_nitem; i++ ) {
		if ( cf_look_items[i].look == look ) return &cf_look_items[i];
	}
	return NULL;
}

/* ---------------------------------------------------------------- links in a settings object */

LOCAL INT hexv( UB c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

/* A UUID as text at s (36 letters); FALSE when it is not one */
LOCAL BOOL uuid_at( CONST UB *s, INT n, TS_UUID *u )
{
	INT	i, k = 0, h, l;

	for ( i = 0; i < 36 && k < 16; ) {
		if ( i >= n ) return FALSE;
		if ( s[i] == '-' ) {
			i++;
			continue;
		}
		if ( i + 1 >= n || ( h = hexv(s[i]) ) < 0 || ( l = hexv(s[i + 1]) ) < 0 ) return FALSE;
		u->b[k++] = (UB)( h * 16 + l );
		i += 2;
	}
	return (BOOL)( k == 16 );
}

LOCAL void uuid_put( UB *o, CONST TS_UUID *u )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i, d = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) o[d++] = '-';
		o[d++] = (UB)hex[u->b[i] >> 4];
		o[d++] = (UB)hex[u->b[i] & 15];
	}
}

/* Where the text of w (wl bytes) is at or after from, or -1 */
LOCAL INT find_at( CONST UB *t, INT len, CONST char *w, INT from )
{
	INT	wl = slen(w), i, k;

	for ( i = from; i + wl <= len; i++ ) {
		for ( k = 0; k < wl && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return i;
	}
	return -1;
}

/* The value of attribute name in the tag t[a..b): where it starts, its length in *p_n; -1 none */
LOCAL INT attr_of( CONST UB *t, INT a, INT b, CONST char *name, INT *p_n )
{
	INT	m = slen(name), i, k, v;

	for ( i = a; i + m + 2 < b; i++ ) {
		if ( !( blank(t[i]) || t[i] == '\n' || t[i] == '\r' ) ) continue;
		for ( k = 0; k < m && t[i + 1 + k] == (UB)name[k]; k++ ) ;
		if ( k < m || t[i + 1 + m] != '=' || t[i + 2 + m] != '"' ) continue;
		v = i + 3 + m;
		for ( k = v; k < b && t[k] != '"'; k++ ) ;
		*p_n = k - v;
		return v;
	}
	return -1;
}

/* Whether the words of t[v..v+n) have role among them */
LOCAL BOOL has_word( CONST UB *t, INT v, INT n, CONST char *role )
{
	INT	rl = slen(role), i = 0, s, k;

	while ( i < n ) {
		while ( i < n && t[v + i] == ' ' ) i++;
		s = i;
		while ( i < n && t[v + i] != ' ' ) i++;
		if ( i - s == rl ) {
			for ( k = 0; k < rl && t[v + s + k] == (UB)role[k]; k++ ) ;
			if ( k == rl ) return TRUE;
		}
	}
	return FALSE;
}

/* The link with the role: its element t[*p_a..*p_e) and the object it points at */
LOCAL BOOL link_from( CONST UB *t, INT len, INT from, CONST char *role, INT *p_a, INT *p_e, TS_UUID *p_u )
{
	INT	i, j, v, n, e, sp;

	for ( i = from; ( i = find_at(t, len, "<link", i) ) >= 0; i = j + 1 ) {
		j = find_at(t, len, ">", i);
		if ( j < 0 ) return FALSE;
		if ( t[i + 5] != ' ' && t[i + 5] != '\n' && t[i + 5] != '\t' ) continue;
		v = attr_of(t, i + 5, j, "relationship", &n);
		if ( v < 0 || !has_word(t, v, n, role) ) continue;
		v = attr_of(t, i + 5, j, "id", &n);
		if ( v < 0 ) continue;
		sp = ts_lnk_split(t + v, n, NULL, NULL);
		if ( !uuid_at(t + v + sp, n - sp, p_u) ) continue;
		if ( t[j - 1] == '/' ) {
			e = j + 1;
		} else {
			e = find_at(t, len, "</link>", j);
			if ( e < 0 ) return FALSE;
			e += 7;
		}
		*p_a = i;
		*p_e = e;
		return TRUE;
	}
	return FALSE;
}

LOCAL BOOL link_of( CONST UB *t, INT len, CONST char *role, INT *p_a, INT *p_e, TS_UUID *p_u )
{
	return link_from(t, len, 0, role, p_a, p_e, p_u);
}

EXPORT BOOL cf_link_get( CONST UB *text, INT len, CONST char *role, TS_UUID *p_u )
{
	INT	a, e;

	if ( text == NULL || role == NULL || p_u == NULL ) return FALSE;
	return link_of(text, len, role, &a, &e, p_u);
}

EXPORT BOOL cf_link_nth( CONST UB *text, INT len, CONST char *role, INT nth, TS_UUID *p_u )
{
	INT	a = 0, e = 0, k;

	if ( text == NULL || role == NULL || p_u == NULL || nth < 0 ) return FALSE;
	for ( k = 0; k <= nth; k++ ) {
		if ( !link_from(text, len, e, role, &a, &e, p_u) ) return FALSE;
	}
	return TRUE;
}

/* The link with the role written: in place of the one there, or (anew) always as a paragraph of its own */
LOCAL INT link_set( UB *text, INT len, INT max, CONST char *role, CONST TS_UUID *u,
		    CONST TS_UUID *vobj, CONST char *label, TS_UUID *p_old, BOOL anew )
{
	static CONST char *const part[] = {
		"<link id=\"", "_0.xtad\" vobjid=\"", "\" relationship=\"",
		"\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\" bgcol=\"#ffffff\""
		" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\" roledisp=\"false\""
		" typedisp=\"false\" updatedisp=\"false\" autoopen=\"false\"/>"
	};
	INT	a, e, nl, i, k, shift, rl = slen(role), ll = slen(label);
	BOOL	para = FALSE, fig = FALSE;
	UB	el[600];
	TS_UUID	old;

	if ( p_old != NULL ) for ( i = 0; i < 16; i++ ) p_old->b[i] = 0;
	if ( text == NULL || role == NULL || u == NULL || vobj == NULL || rl > 128 || ll > 128 ) return -1;

	/* the element: in place of the one with the role, or a paragraph of its own */
	nl = 0;
	if ( !anew && link_of(text, len, role, &a, &e, &old) ) {
		if ( p_old != NULL ) *p_old = old;
	} else {
		a = find_at(text, len, "</document>", 0);
		if ( a >= 0 ) {
			el[nl++] = '<';
			el[nl++] = 'p';
			el[nl++] = '>';
			for ( i = 0; i < ll; i++ ) el[nl++] = (UB)label[i];
			para = TRUE;
		} else {
			/* a figure (a box of links): the link alone, at its top left */
			a = find_at(text, len, "</figure>", 0);
			if ( a < 0 ) return -2;
			fig = TRUE;
		}
		e = a;
	}
	for ( i = 0; part[0][i] != 0; i++ ) el[nl++] = (UB)part[0][i];
	uuid_put(el + nl, u);
	nl += 36;
	for ( i = 0; part[1][i] != 0; i++ ) el[nl++] = (UB)part[1][i];
	uuid_put(el + nl, vobj);
	nl += 36;
	for ( i = 0; part[2][i] != 0; i++ ) el[nl++] = (UB)part[2][i];
	for ( i = 0; i < rl; i++ ) el[nl++] = (UB)role[i];
	if ( fig ) {
		/* closed, at the size of letters it is written at (VOBJ_CLOSED_H) */
		CONST char	*at[2] = { "\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"248\" vobjbottom=\"",
					   "\" height=\"" };
		INT		num[2] = { 8 + VOBJ_CLOSED_H(14, 1, 1), VOBJ_CLOSED_H(14, 1, 1) }, f;

		for ( f = 0; f < 2; f++ ) {
			UB	d[12];
			INT	nd = 0, v = num[f];

			for ( i = 0; at[f][i] != 0; i++ ) el[nl++] = (UB)at[f][i];
			do { d[nd++] = (UB)( '0' + v % 10 ); v /= 10; } while ( v > 0 );
			while ( nd > 0 ) el[nl++] = d[--nd];
		}
	}
	for ( i = 0; part[3][i] != 0; i++ ) el[nl++] = (UB)part[3][i];
	if ( fig ) el[nl++] = '\n';
	if ( para ) {
		CONST char	*end = "</p>\n";

		for ( i = 0; end[i] != 0; i++ ) el[nl++] = (UB)end[i];
	}

	/* what stood from a to e is replaced by it */
	shift = nl - ( e - a );
	if ( len + shift > max ) return -1;
	if ( shift > 0 ) {
		for ( k = len - 1; k >= e; k-- ) text[k + shift] = text[k];
	} else if ( shift < 0 ) {
		for ( k = e; k < len; k++ ) text[k + shift] = text[k];
	}
	for ( i = 0; i < nl; i++ ) text[a + i] = el[i];
	return len + shift;
}

EXPORT INT cf_link_put( UB *text, INT len, INT max, CONST char *role, CONST TS_UUID *u,
			CONST TS_UUID *vobj, CONST char *label, TS_UUID *p_old )
{
	return link_set(text, len, max, role, u, vobj, label, p_old, FALSE);
}

/* The link with the role that points at u: its element t[*p_a..*p_e) */
LOCAL BOOL link_to( CONST UB *t, INT len, CONST char *role, CONST TS_UUID *u, INT *p_a, INT *p_e )
{
	INT	a = 0, e = 0, k;
	TS_UUID	o;

	while ( link_from(t, len, e, role, &a, &e, &o) ) {
		for ( k = 0; k < 16 && o.b[k] == u->b[k]; k++ ) ;
		if ( k == 16 ) {
			*p_a = a;
			*p_e = e;
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT INT cf_link_add( UB *text, INT len, INT max, CONST char *role, CONST TS_UUID *u,
			CONST TS_UUID *vobj, CONST char *label )
{
	INT	a, e;

	if ( text == NULL || role == NULL || u == NULL ) return -1;
	if ( link_to(text, len, role, u, &a, &e) ) return len;
	return link_set(text, len, max, role, u, vobj, label, NULL, TRUE);
}

EXPORT INT cf_link_drop( UB *text, INT len, CONST char *role, CONST TS_UUID *u )
{
	INT	a, e, k;

	if ( text == NULL || role == NULL || u == NULL ) return -1;
	if ( !link_to(text, len, role, u, &a, &e) ) return -2;
	/* a paragraph that held only the link goes with it */
	if ( a >= 3 && text[a - 3] == '<' && text[a - 2] == 'p' && text[a - 1] == '>'
	  && e + 4 <= len && find_at(text, e + 4, "</p>", e) == e ) {
		a -= 3;
		e += 4;
		if ( e < len && text[e] == '\n' ) e++;
	}
	for ( k = e; k < len; k++ ) text[a + k - e] = text[k];
	return len - ( e - a );
}

EXPORT INT cf_link_tag( UB *text, INT len, INT max, CONST TS_UUID *u, CONST char *role )
{
	INT	i, j, v, n, sp, at, add, k, rl = slen(role);
	TS_UUID	t;
	BOOL	has;

	if ( text == NULL || u == NULL || role == NULL || rl == 0 ) return -1;
	for ( i = 0; ( i = find_at(text, len, "<link", i) ) >= 0; i = j + 1 ) {
		j = find_at(text, len, ">", i);
		if ( j < 0 ) return -1;
		v = attr_of(text, i + 5, j, "id", &n);
		if ( v < 0 ) continue;
		sp = ts_lnk_split(text + v, n, NULL, NULL);
		if ( !uuid_at(text + v + sp, n - sp, &t) ) continue;
		for ( k = 0; k < 16 && t.b[k] == u->b[k]; k++ ) ;
		if ( k < 16 ) continue;

		/* the word after the relationship's others, or the attribute put first */
		v = attr_of(text, i + 5, j, "relationship", &n);
		has = (BOOL)( v >= 0 );
		if ( has && has_word(text, v, n, role) ) return len;
		at = has ? v + n : i + 5;
		add = has ? ( ( n > 0 ) ? 1 : 0 ) + rl : 16 + rl;
		if ( len + add > max ) return -1;
		for ( k = len - 1; k >= at; k-- ) text[k + add] = text[k];
		k = at;
		if ( has ) {
			if ( n > 0 ) text[k++] = ' ';
			for ( n = 0; n < rl; n++ ) text[k++] = (UB)role[n];
		} else {
			CONST char	*h = " relationship=\"";

			for ( n = 0; h[n] != 0; n++ ) text[k++] = (UB)h[n];
			for ( n = 0; n < rl; n++ ) text[k++] = (UB)role[n];
			text[k++] = '"';
		}
		return len + add;
	}
	return -2;
}
