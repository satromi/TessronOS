/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_name.c
 *	Names of objects as names of files, and back (design 18.20)
 *
 *	A FAT volume refuses nine characters and the controls, drops dots
 *	and blanks at the end of a name, keeps a name within 255 UTF-16
 *	units and does not tell A from a. An object's name may hold any of
 *	that. The nine become the full width characters that look like them,
 *	so the name still reads the same and comes back as it was; a control
 *	becomes its picture. Two names of one directory that FAT would take
 *	for one are told apart by a number in brackets.
 */

#include "xfu_in.h"

#define WORK	( XFU_NAME_MAX * 2 )

/* The nine and what each becomes */
LOCAL CONST UH swap[][2] = {
	{ '\\', 0xFF3C }, { '/', 0xFF0F }, { ':', 0xFF1A }, { '*', 0xFF0A }, { '?', 0xFF1F },
	{ '"', 0xFF02 }, { '<', 0xFF1C }, { '>', 0xFF1E }, { '|', 0xFF5C },
};
#define NSWAP	( (INT)( sizeof(swap) / sizeof(swap[0]) ) )

/* Names FAT gives to devices, with or without an extension */
LOCAL CONST char * CONST device[] = { "con", "prn", "aux", "nul" };

EXPORT INT xfu_u16( CONST UB *s, INT n )
{
	INT	i = 0, k, c, u = 0;

	while ( i < n ) {
		k = txc_utf8_get(s + i, n - i, &c);
		u += ( c >= 0x10000 ) ? 2 : 1;
		i += k;
	}
	return u;
}

/* Whether the part before the first dot is a device's name */
LOCAL BOOL is_device( CONST UB *s, INT n )
{
	INT	i, k, base;

	for ( base = 0; base < n && s[base] != '.'; base++ ) ;
	for ( i = 0; i < 4; i++ ) {
		CONST char	*d = device[i];

		for ( k = 0; k < 3 && k < base && ( s[k] | 0x20 ) == (UB)d[k]; k++ ) ;
		if ( k == 3 && base == 3 ) return TRUE;
	}
	/* COM1 to COM9, LPT1 to LPT9 */
	if ( base == 4 && s[3] >= '1' && s[3] <= '9'
	  && ( ( ( s[0] | 0x20 ) == 'c' && ( s[1] | 0x20 ) == 'o' && ( s[2] | 0x20 ) == 'm' )
	    || ( ( s[0] | 0x20 ) == 'l' && ( s[1] | 0x20 ) == 'p' && ( s[2] | 0x20 ) == 't' ) ) ) {
		return TRUE;
	}
	return FALSE;
}

/* Dots and blanks at the end taken off: the new length */
LOCAL INT trim_end( CONST UB *s, INT n )
{
	while ( n > 0 && ( s[n - 1] == '.' || s[n - 1] == ' ' ) ) n--;
	return n;
}

/*
 * A name cut to fit: within u16 units and bytes - 1 bytes, the
 * extension (from the last dot, when it is short) kept whole and the
 * part before it cut at a character. Answers the length.
 */
LOCAL INT fit( UB *s, INT n, INT bytes )
{
	INT	e, i, k, c, ulen, elen, eu, stem;

	if ( n < bytes && xfu_u16(s, n) <= XFU_NAME_U16 ) {
		return n;
	}
	for ( e = n - 1; e > 0 && s[e] != '.'; e-- ) ;
	if ( e <= 0 || n - e > 32 ) e = n;		/* no extension worth keeping */
	elen = n - e;
	eu = xfu_u16(s + e, elen);
	stem = 0;
	ulen = 0;
	for ( i = 0; i < e; i += k ) {
		k = txc_utf8_get(s + i, e - i, &c);
		if ( i + k + elen >= bytes || ulen + ( ( c >= 0x10000 ) ? 2 : 1 ) + eu > XFU_NAME_U16 ) {
			break;
		}
		ulen += ( c >= 0x10000 ) ? 2 : 1;
		stem = i + k;
	}
	for ( i = 0; i < elen; i++ ) s[stem + i] = s[e + i];
	n = stem + elen;
	if ( elen == 0 ) n = trim_end(s, n);
	s[n] = 0;
	return n;
}

EXPORT INT xfu_name_out( CONST UB *name, UB *out, INT max )
{
	UB	*w;
	INT	i, k, c, n = 0, len, j;

	if ( out == NULL || max < 2 ) {
		return E_PAR;
	}
	w = (UB *)xfu_sys_alloc(WORK);
	if ( w == NULL ) {
		return E_NOMEM;
	}
	len = xfu_slen(name);
	for ( i = 0; i < len && n < WORK - 8; i += k ) {
		k = txc_utf8_get(name + i, len - i, &c);
		if ( c < 0 ) {
			c = '_';
		} else if ( c < 0x20 ) {
			c = 0x2400 + c;
		} else if ( c == 0x7F ) {
			c = 0x2421;
		} else {
			for ( j = 0; j < NSWAP; j++ ) {
				if ( swap[j][0] == c ) {
					c = swap[j][1];
					break;
				}
			}
		}
		n += txc_utf8_put(c, w + n);
	}
	n = trim_end(w, n);
	if ( n == 0 ) {
		w[n++] = '_';
	}
	if ( is_device(w, n) ) {
		for ( j = 0; j < n && w[j] != '.'; j++ ) ;
		for ( i = n; i > j; i-- ) w[i] = w[i - 1];
		w[j] = '_';
		n++;
	}
	w[n] = 0;
	n = fit(w, n, max);
	n = xfu_scpy(out, max, w);
	xfu_sys_free(w);
	return n;
}

EXPORT INT xfu_name_in( CONST UB *name, UB *out, INT max )
{
	INT	i, k, c, j, n = 0, len;
	UB	u[4];

	if ( out == NULL || max < 1 ) {
		return E_PAR;
	}
	len = xfu_slen(name);
	for ( i = 0; i < len; i += k ) {
		INT	m;

		k = txc_utf8_get(name + i, len - i, &c);
		if ( c < 0 ) c = '_';
		for ( j = 0; j < NSWAP; j++ ) {
			if ( swap[j][1] == c ) {
				c = swap[j][0];
				break;
			}
		}
		m = txc_utf8_put(c, u);
		if ( n + m >= max ) break;
		for ( j = 0; j < m; j++ ) out[n++] = u[j];
	}
	out[n] = 0;
	return n;
}

/* ---------------------------------------------------------------- the names of a directory */

EXPORT void xfu_names_init( T_XFUNAMES *s )
{
	s->buf = NULL;
	s->used = 0;
	s->max = 0;
}

EXPORT void xfu_names_free( T_XFUNAMES *s )
{
	if ( s->buf != NULL ) xfu_sys_free(s->buf);
	xfu_names_init(s);
}

EXPORT BOOL xfu_names_has( CONST T_XFUNAMES *s, CONST UB *name )
{
	INT	at = 0;

	while ( at < s->used ) {
		if ( xfu_same_ci(s->buf + at, name) ) return TRUE;
		at += xfu_slen(s->buf + at) + 1;
	}
	return FALSE;
}

EXPORT ER xfu_names_add( T_XFUNAMES *s, CONST UB *name )
{
	INT	n = xfu_slen(name) + 1;
	UB	*nb;

	if ( s->used + n > s->max ) {
		INT	cap = ( s->max < 1024 ) ? 1024 : s->max * 2;

		while ( cap < s->used + n ) cap *= 2;
		nb = (UB *)xfu_sys_alloc(cap);
		if ( nb == NULL ) {
			return E_NOMEM;
		}
		if ( s->buf != NULL ) {
			xfu_mcpy(nb, s->buf, s->used);
			xfu_sys_free(s->buf);
		}
		s->buf = nb;
		s->max = cap;
	}
	xfu_mcpy(s->buf + s->used, name, n);
	s->used += n;
	return E_OK;
}

EXPORT ER xfu_names_unique( T_XFUNAMES *s, UB *name, INT max, INT namemax )
{
	UB	*base, tail[16];
	INT	n, e, i, k, lim, t;

	lim = ( namemax > 0 && namemax + 1 < max ) ? namemax + 1 : max;
	n = fit(name, xfu_slen(name), lim);
	if ( !xfu_names_has(s, name) ) {
		return xfu_names_add(s, name);
	}
	base = (UB *)xfu_sys_alloc(XFU_NAME_MAX);
	if ( base == NULL ) {
		return E_NOMEM;
	}
	(void)xfu_scpy(base, XFU_NAME_MAX, name);
	for ( e = n - 1; e > 0 && base[e] != '.'; e-- ) ;
	if ( e <= 0 ) e = n;
	for ( k = 2; k < 100000; k++ ) {
		/* " (k)" before the extension */
		D	v = k;
		UB	d[8];
		INT	nd = 0, at;

		while ( v > 0 ) {
			d[nd++] = (UB)( '0' + v % 10 );
			v /= 10;
		}
		t = 0;
		tail[t++] = ' ';
		tail[t++] = '(';
		while ( nd > 0 ) tail[t++] = d[--nd];
		tail[t++] = ')';
		/* the stem cut so that the whole fits */
		at = e;
		while ( at > 0 && ( at + t + ( n - e ) >= lim
				 || xfu_u16(base, at) + t + xfu_u16(base + e, n - e) > XFU_NAME_U16 ) ) {
			do {
				at--;
			} while ( at > 0 && ( base[at] & 0xC0 ) == 0x80 );
		}
		for ( i = 0; i < at; i++ ) name[i] = base[i];
		for ( i = 0; i < t; i++ ) name[at + i] = tail[i];
		for ( i = e; i < n; i++ ) name[at + t + i - e] = base[i];
		name[at + t + n - e] = 0;
		if ( !xfu_names_has(s, name) ) {
			xfu_sys_free(base);
			return xfu_names_add(s, name);
		}
	}
	xfu_sys_free(base);
	return E_LIMIT;
}
