/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_util.c
 *	Bytes, buffers, times, checksums and pictures for lib/libxfu
 */

#include "xfu_in.h"

/* ---------------------------------------------------------------- bytes */

EXPORT void xfu_mcpy( void *d, CONST void *s, SZ n )
{
	UB		*a = (UB *)d;
	CONST UB	*b = (CONST UB *)s;

	while ( n-- > 0 ) *a++ = *b++;
}

EXPORT void xfu_mset( void *d, INT c, SZ n )
{
	UB	*a = (UB *)d;

	while ( n-- > 0 ) *a++ = (UB)c;
}

EXPORT INT xfu_slen( CONST UB *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != 0 ) n++;
	return n;
}

EXPORT INT xfu_scpy( UB *d, INT max, CONST UB *s )
{
	INT	n = 0;

	if ( max <= 0 ) {
		return 0;
	}
	while ( s != NULL && s[n] != 0 && n < max - 1 ) {
		d[n] = s[n];
		n++;
	}
	d[n] = 0;
	return n;
}

EXPORT BOOL xfu_same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

EXPORT BOOL xfu_same_ci( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && lower(a[i]) == lower(b[i]); i++ ) ;
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* Where a word stands in bytes from 'from' on, or -1 */
EXPORT INT xfu_find( CONST UB *s, SZ n, CONST char *w, SZ from )
{
	SZ	i, k, wl = 0;

	while ( w[wl] != 0 ) wl++;
	for ( i = from; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return (INT)i;
	}
	return -1;
}

EXPORT ER xfu_join( UB *out, INT max, CONST UB *dir, CONST UB *name )
{
	INT	n = 0, k;

	if ( dir != NULL && dir[0] != 0 ) {
		n = xfu_scpy(out, max, dir);
		if ( n > 0 && out[n - 1] != '/' && name != NULL && name[0] != 0 ) {
			if ( n >= max - 1 ) return E_LIMIT;
			out[n++] = '/';
			out[n] = 0;
		}
	} else if ( max > 0 ) {
		out[0] = 0;
	}
	k = xfu_scpy(out + n, max - n, name);
	return ( k == xfu_slen(name) ) ? E_OK : E_LIMIT;
}

/* ---------------------------------------------------------------- a buffer that grows */

EXPORT void xb_init( XBUF *w )
{
	w->b = NULL;
	w->at = 0;
	w->max = 0;
	w->bad = FALSE;
}

EXPORT void xb_free( XBUF *w )
{
	if ( w->b != NULL ) xfu_sys_free(w->b);
	xb_init(w);
}

/* Room for n more bytes and a nought */
LOCAL BOOL xb_room( XBUF *w, SZ n )
{
	UB	*nb;
	SZ	cap;

	if ( w->bad ) {
		return FALSE;
	}
	if ( w->at + n + 1 <= w->max ) {
		return TRUE;
	}
	cap = ( w->max < 1024 ) ? 1024 : w->max * 2;
	while ( cap < w->at + n + 1 ) cap *= 2;
	nb = (UB *)xfu_sys_alloc(cap);
	if ( nb == NULL ) {
		w->bad = TRUE;
		return FALSE;
	}
	if ( w->b != NULL ) {
		xfu_mcpy(nb, w->b, w->at);
		xfu_sys_free(w->b);
	}
	w->b = nb;
	w->max = cap;
	return TRUE;
}

EXPORT void xb_bytes( XBUF *w, CONST void *s, SZ n )
{
	if ( n <= 0 || !xb_room(w, n) ) {
		return;
	}
	xfu_mcpy(w->b + w->at, s, n);
	w->at += n;
	w->b[w->at] = 0;
}

EXPORT void xb_put( XBUF *w, CONST char *s )
{
	xb_bytes(w, s, xfu_slen((CONST UB *)s));
}

EXPORT void xb_num( XBUF *w, D v )
{
	char	d[24];
	INT	n = 0, i;
	UB	o[24];
	UD	u = ( v < 0 ) ? (UD)-v : (UD)v;

	do {
		d[n++] = (char)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	i = 0;
	if ( v < 0 ) o[i++] = '-';
	while ( n > 0 ) o[i++] = (UB)d[--n];
	xb_bytes(w, o, i);
}

EXPORT void xb_xml( XBUF *w, CONST UB *s, SZ n )
{
	SZ	i, from = 0;

	for ( i = 0; i < n; i++ ) {
		CONST char	*e = NULL;

		switch ( s[i] ) {
		case '&':	e = "&amp;";	break;
		case '<':	e = "&lt;";	break;
		case '>':	e = "&gt;";	break;
		case '"':	e = "&quot;";	break;
		case '\r':	e = "";		break;
		default:
			if ( s[i] < 0x20 && s[i] != '\t' && s[i] != '\n' ) e = "";
			break;
		}
		if ( e != NULL ) {
			xb_bytes(w, s + from, i - from);
			xb_put(w, e);
			from = i + 1;
		}
	}
	xb_bytes(w, s + from, n - from);
}

EXPORT void xb_json( XBUF *w, CONST UB *s, SZ n )
{
	SZ	i, from = 0;

	for ( i = 0; i < n; i++ ) {
		if ( s[i] == '"' || s[i] == '\\' || s[i] < 0x20 ) {
			xb_bytes(w, s + from, i - from);
			if ( s[i] >= 0x20 ) {
				UB	e[2];

				e[0] = '\\';
				e[1] = s[i];
				xb_bytes(w, e, 2);
			} else if ( s[i] == '\t' ) {
				xb_put(w, "\\t");
			} else if ( s[i] == '\n' ) {
				xb_put(w, "\\n");
			}
			from = i + 1;
		}
	}
	xb_bytes(w, s + from, n - from);
}

/* ---------------------------------------------------------------- identities */

EXPORT void xfu_uuid_str( CONST TS_UUID *u, char *out )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[u->b[i] >> 4];
		out[n++] = hex[u->b[i] & 15];
	}
	out[n] = 0;
}

LOCAL INT hexv( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT BOOL xfu_str_uuid( CONST UB *s, TS_UUID *u )
{
	INT	i, n = 0, hi, lo;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) {
			if ( s[n++] != '-' ) return FALSE;
		}
		hi = hexv(s[n]);
		lo = ( hi >= 0 ) ? hexv(s[n + 1]) : -1;
		if ( lo < 0 ) return FALSE;
		u->b[i] = (UB)( ( hi << 4 ) | lo );
		n += 2;
	}
	return TRUE;
}

EXPORT BOOL xfu_uuid_eq( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a->b[i] != b->b[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the CRC */

LOCAL CONST UINT crc_nib[16] = {
	0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
	0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
	0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
	0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
};

EXPORT UINT xfu_crc32( UINT crc, CONST UB *b, SZ n )
{
	crc = ~crc;
	while ( n-- > 0 ) {
		crc ^= *b++;
		crc = ( crc >> 4 ) ^ crc_nib[crc & 15];
		crc = ( crc >> 4 ) ^ crc_nib[crc & 15];
	}
	return ~crc;
}

/* ---------------------------------------------------------------- time */

/* Days from 1970-01-01 to a date of the Gregorian calendar */
LOCAL D days_of( D y, INT m, INT d )
{
	D	era, yoe, doy, doe;

	y -= ( m <= 2 ) ? 1 : 0;
	era = ( ( y >= 0 ) ? y : y - 399 ) / 400;
	yoe = y - era * 400;
	doy = ( 153 * ( m + ( ( m > 2 ) ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

#define DAYS_1985	5479

LOCAL void put2( UB *o, D v )
{
	o[0] = (UB)( '0' + ( v / 10 ) % 10 );
	o[1] = (UB)( '0' + v % 10 );
}

EXPORT INT xfu_time_iso( D t, UB *out )
{
	D	z, era, doe, yoe, y, doy, mp, d, m, sec;

	if ( t < 0 ) t = 0;
	z = t / 86400 + DAYS_1985 + 719468;
	sec = t % 86400;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
	y = yoe + era * 400;
	doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
	mp = ( 5 * doy + 2 ) / 153;
	d = doy - ( 153 * mp + 2 ) / 5 + 1;
	m = ( mp < 10 ) ? mp + 3 : mp - 9;
	if ( m <= 2 ) y++;
	put2(out, y / 100);
	put2(out + 2, y % 100);
	out[4] = '-';
	put2(out + 5, m);
	out[7] = '-';
	put2(out + 8, d);
	out[10] = 'T';
	put2(out + 11, sec / 3600);
	out[13] = ':';
	put2(out + 14, ( sec / 60 ) % 60);
	out[16] = ':';
	put2(out + 17, sec % 60);
	out[19] = 'Z';
	out[20] = 0;
	return 20;
}

/* n digits at s, or -1 */
LOCAL D digits( CONST UB *s, INT n )
{
	D	v = 0;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( s[i] < '0' || s[i] > '9' ) return -1;
		v = v * 10 + ( s[i] - '0' );
	}
	return v;
}

EXPORT D xfu_time_parse( CONST UB *s )
{
	D	y, mo, d, h = 0, mi = 0, se = 0, t, off = 0;
	INT	i = 19;

	if ( s == NULL ) {
		return 0;
	}
	y = digits(s, 4);
	mo = ( y >= 0 && s[4] == '-' ) ? digits(s + 5, 2) : -1;
	d = ( mo >= 1 && s[7] == '-' ) ? digits(s + 8, 2) : -1;
	if ( d < 1 || mo > 12 ) {
		return 0;
	}
	if ( s[10] == 'T' || s[10] == ' ' ) {
		h = digits(s + 11, 2);
		mi = ( s[13] == ':' ) ? digits(s + 14, 2) : -1;
		se = ( s[16] == ':' ) ? digits(s + 17, 2) : 0;
		if ( h < 0 || mi < 0 || se < 0 ) return 0;
		if ( s[16] != ':' ) i = 16;
		if ( s[i] == '.' ) {
			for ( i++; s[i] >= '0' && s[i] <= '9'; i++ ) ;
		}
		if ( s[i] == '+' || s[i] == '-' ) {
			D	oh = digits(s + i + 1, 2);
			D	om = digits(s + i + ( ( s[i + 3] == ':' ) ? 4 : 3 ), 2);

			if ( oh >= 0 && om >= 0 ) {
				off = ( oh * 60 + om ) * 60;
				if ( s[i] == '-' ) off = -off;
			}
		}
	}
	t = ( days_of(y, (INT)mo, (INT)d) - DAYS_1985 ) * 86400 + h * 3600 + mi * 60 + se - off;
	return ( t > 0 ) ? t : 0;
}

/* ---------------------------------------------------------------- PNG */

LOCAL void be32( UB *p, UINT v )
{
	p[0] = (UB)( v >> 24 );
	p[1] = (UB)( v >> 16 );
	p[2] = (UB)( v >> 8 );
	p[3] = (UB)v;
}

EXPORT ER xfu_png_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	if ( data == NULL || size < 24 || data[0] != 0x89 || data[1] != 'P' || data[2] != 'N'
	  || data[3] != 'G' || data[12] != 'I' || data[13] != 'H' || data[14] != 'D' || data[15] != 'R' ) {
		return E_PAR;
	}
	*p_w = (INT)( ( (UINT)data[16] << 24 ) | ( data[17] << 16 ) | ( data[18] << 8 ) | data[19] );
	*p_h = (INT)( ( (UINT)data[20] << 24 ) | ( data[21] << 16 ) | ( data[22] << 8 ) | data[23] );
	return ( *p_w > 0 && *p_h > 0 ) ? E_OK : E_PAR;
}

/* A chunk: its length, its type and data (already at p + 8), its CRC */
LOCAL SZ png_chunk( UB *p, CONST char *type, UINT len )
{
	be32(p, len);
	p[4] = (UB)type[0];
	p[5] = (UB)type[1];
	p[6] = (UB)type[2];
	p[7] = (UB)type[3];
	be32(p + 8 + len, xfu_crc32(0, p + 4, len + 4));
	return 12 + (SZ)len;
}

EXPORT ER xfu_png_encode( CONST UINT *px, INT w, INT h, UB **p_out, SZ *p_len )
{
	SZ	raw, nblk, idat, total, at, left, k;
	UB	*o, *z;
	UINT	a = 1, b = 0;
	INT	x, y, col, bpp;

	if ( px == NULL || w <= 0 || h <= 0 || (D)w * h > XFU_PIX_MAX ) {
		return E_PAR;
	}
	/* RGB, or RGBA when a pixel is clear (0xFFFFFFFF) */
	bpp = 3;
	for ( k = 0; k < (SZ)w * h; k++ ) {
		if ( px[k] == 0xFFFFFFFFU ) {
			bpp = 4;
			break;
		}
	}
	raw = (SZ)( w * bpp + 1 ) * h;			/* a filter byte a row, then the pixels */
	nblk = ( raw + 65534 ) / 65535;
	idat = 2 + raw + nblk * 5 + 4;			/* zlib head, stored blocks, Adler-32 */
	total = 8 + ( 12 + 13 ) + ( 12 + idat ) + 12;
	o = (UB *)xfu_sys_alloc(total);
	if ( o == NULL ) {
		return E_NOMEM;
	}
	o[0] = 0x89;  o[1] = 'P';  o[2] = 'N';  o[3] = 'G';
	o[4] = 0x0D;  o[5] = 0x0A;  o[6] = 0x1A;  o[7] = 0x0A;
	at = 8;
	be32(o + at + 8, (UINT)w);
	be32(o + at + 12, (UINT)h);
	o[at + 16] = 8;					/* bits a sample */
	o[at + 17] = (UB)( ( bpp == 4 ) ? 6 : 2 );		/* RGBA or RGB */
	o[at + 18] = 0;
	o[at + 19] = 0;
	o[at + 20] = 0;
	at += png_chunk(o + at, "IHDR", 13);

	z = o + at + 8;
	k = 0;
	z[k++] = 0x78;
	z[k++] = 0x01;
	left = raw;
	x = 0;
	y = 0;
	col = -1;					/* the filter byte comes first */
	while ( left > 0 ) {
		SZ	n = ( left > 65535 ) ? 65535 : left, i;

		z[k++] = (UB)( ( left == n ) ? 1 : 0 );
		z[k++] = (UB)n;
		z[k++] = (UB)( n >> 8 );
		z[k++] = (UB)~n;
		z[k++] = (UB)( ~n >> 8 );
		for ( i = 0; i < n; i++ ) {
			UB	v;

			if ( col < 0 ) {
				v = 0;
			} else {
				UINT	p = px[(SZ)y * w + x];

				v = ( col == 3 ) ? (UB)( ( p == 0xFFFFFFFFU ) ? 0 : 255 )
				    : ( p == 0xFFFFFFFFU ) ? 0 : (UB)( p >> ( 16 - col * 8 ) );
			}
			z[k++] = v;
			a = ( a + v ) % 65521;
			b = ( b + a ) % 65521;
			if ( ++col == bpp ) {
				col = 0;
				if ( ++x == w ) {
					x = 0;
					y++;
					col = -1;
				}
			}
		}
		left -= n;
	}
	be32(z + k, ( b << 16 ) | a);
	k += 4;
	at += png_chunk(o + at, "IDAT", (UINT)k);
	at += png_chunk(o + at, "IEND", 0);
	*p_out = o;
	*p_len = at;
	return E_OK;
}
