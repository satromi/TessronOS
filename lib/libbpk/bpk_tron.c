/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_tron.c
 *	TRON characters and Unicode, both ways
 *
 *	A TRON character is 16 bits in a plane; FE21 and on switch the plane
 *	(FE21 to plane 1). Each plane has four zones of two-byte codes: A
 *	(both bytes 21-7E), B (the first 80-FD), C (the second 80-FD) and D
 *	(both 80-FD). Plane 1 is the system script: zone A is JIS X 0208,
 *	except row 3's letters, which are ASCII, with JIS X 0213's first
 *	plane in its gaps; zone B holds JIS X 0213's second plane (first byte
 *	87..A0) and JIS X 0212 (A1..ED); zone C GB 2312 and zone D KS X 1001
 *	(from B7), each of these 94 by 94 laid 126 to a first byte. Planes 16
 *	and 17 lay out the Unicode BMP in zone order, so any character of the
 *	BMP has a TRON code. The other planes -- the GT fonts, 大漢和 and the
 *	rest -- have no Unicode and read as 〓; so does a place of plane 1 no
 *	table fills.
 *
 *	The tables come from tools/gen_jis.py bpk. A place that holds a
 *	character past the BMP, or a letter and a combining mark, holds
 *	0xD800 + n, n indexing bpk_tron_multi.
 */

#include <ts/bpk.h>
#include <string.h>

#define PLANE_BMP	16		/* planes 16 and 17: the BMP, 44032 characters each */
#define PLANE_SPAN	44032
#define ZONE_PACK	( 94 * 94 )	/* the places of a set laid 126 to a first byte */

EXPORT INT bpk_utf8( UINT cp, char *o )
{
	if ( cp < 0x80 ) {
		o[0] = (char)cp;
		return 1;
	}
	if ( cp < 0x800 ) {
		o[0] = (char)( 0xC0 | ( cp >> 6 ) );
		o[1] = (char)( 0x80 | ( cp & 0x3F ) );
		return 2;
	}
	if ( cp < 0x10000 ) {
		o[0] = (char)( 0xE0 | ( cp >> 12 ) );
		o[1] = (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		o[2] = (char)( 0x80 | ( cp & 0x3F ) );
		return 3;
	}
	o[0] = (char)( 0xF0 | ( cp >> 18 ) );
	o[1] = (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
	o[2] = (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
	o[3] = (char)( 0x80 | ( cp & 0x3F ) );
	return 4;
}

/* A two-byte code as its place in its plane, -1 when it is in no zone */
LOCAL INT zone_linear( UINT hi, UINT lo )
{
	if ( hi >= 0x21 && hi <= 0x7E && lo >= 0x21 && lo <= 0x7E ) return (INT)( ( hi - 0x21 ) * 94 + ( lo - 0x21 ) );
	if ( hi >= 0x80 && hi <= 0xFD && lo >= 0x21 && lo <= 0x7E ) return (INT)( 8836 + ( hi - 0x80 ) * 94 + ( lo - 0x21 ) );
	if ( hi >= 0x21 && hi <= 0x7E && lo >= 0x80 && lo <= 0xFD ) return (INT)( 20680 + ( hi - 0x21 ) * 126 + ( lo - 0x80 ) );
	if ( hi >= 0x80 && hi <= 0xFD && lo >= 0x80 && lo <= 0xFD ) return (INT)( 32524 + ( hi - 0x80 ) * 126 + ( lo - 0x80 ) );
	return -1;
}

/* A place in a plane as its two-byte code */
LOCAL UINT zone_code( INT lin )
{
	if ( lin < 8836 ) return ( (UINT)( 0x21 + lin / 94 ) << 8 ) | (UINT)( 0x21 + lin % 94 );
	lin -= 8836;
	if ( lin < 11844 ) return ( (UINT)( 0x80 + lin / 94 ) << 8 ) | (UINT)( 0x21 + lin % 94 );
	lin -= 11844;
	if ( lin < 11844 ) return ( (UINT)( 0x21 + lin / 126 ) << 8 ) | (UINT)( 0x80 + lin % 126 );
	lin -= 11844;
	return ( (UINT)( 0x80 + lin / 126 ) << 8 ) | (UINT)( 0x80 + lin % 126 );
}

/* What a table's entry for a place holds: how many characters, into out */
LOCAL INT entry( UINT v, UINT out[2] )
{
	if ( v == 0 ) {
		out[0] = UC_GETA;
		return BPK_TRON_NOUCS;
	}
	if ( v >= 0xD800 && v <= 0xDFFF ) {
		INT	n = (INT)( v - 0xD800 );

		if ( n >= bpk_tron_nmulti ) {
			out[0] = UC_GETA;
			return BPK_TRON_NOUCS;
		}
		out[0] = bpk_tron_multi[n][0];
		out[1] = bpk_tron_multi[n][1];
		return ( out[1] != 0 ) ? 2 : 1;
	}
	out[0] = v;
	return 1;
}

/* A character of plane 1 */
LOCAL INT plane1( UINT hi, UINT lo, UINT out[2] )
{
	if ( hi == 0x23 && lo >= 0x20 && lo <= 0x7E ) {
		out[0] = lo;
		return 1;
	}
	if ( hi >= 0x21 && hi <= 0x7E && lo >= 0x21 && lo <= 0x7E ) {
		return entry(bpk_jis_char[( hi - 0x21 ) * 94 + ( lo - 0x21 )], out);
	}
	if ( hi >= 0x87 && hi <= 0xED && lo >= 0x21 && lo <= 0x7E ) {
		return entry(bpk_tron_zb[( hi - 0x87 ) * 94 + ( lo - 0x21 )], out);
	}
	if ( hi >= 0x21 && hi <= 0x7E && lo >= 0x80 && lo <= 0xFD ) {
		UINT	p = ( hi - 0x21 ) * 126 + ( lo - 0x80 );

		return entry(( p < ZONE_PACK ) ? bpk_tron_zc[p] : 0, out);
	}
	if ( hi >= 0xB7 && hi <= 0xFD && lo >= 0x80 && lo <= 0xFD ) {
		UINT	p = ( hi - 0xB7 ) * 126 + ( lo - 0x80 );

		return entry(( p < ZONE_PACK ) ? bpk_tron_zd[p] : 0, out);
	}
	if ( zone_linear(hi, lo) >= 0 ) {
		out[0] = UC_GETA;		/* 点字 and the places no set fills */
		return BPK_TRON_NOUCS;
	}
	return 0;
}

EXPORT INT bpk_tron_chars( UINT ch, INT *p_plane, UINT out[2] )
{
	UINT	hi = ( ch >> 8 ) & 0xFF, lo = ch & 0xFF;
	INT	lin;

	out[0] = out[1] = 0;
	if ( ( ch >= 0xFE21 && ch <= 0xFE7E ) || ( ch >= 0xFE80 && ch <= 0xFEFE ) ) {
		*p_plane = (INT)( ch - 0xFE21 + 1 );
		return 0;
	}
	if ( ch == TC_TAB || ch == TC_NL || ch == TC_CR ) {
		out[0] = ch;
		return 1;
	}
	if ( *p_plane == PLANE_BMP || *p_plane == PLANE_BMP + 1 ) {
		lin = zone_linear(hi, lo);
		if ( lin >= 0 && ( *p_plane - PLANE_BMP ) * PLANE_SPAN + lin <= 0xFFFF ) {
			out[0] = (UINT)( ( *p_plane - PLANE_BMP ) * PLANE_SPAN + lin );
			return 1;
		}
		if ( lin < 0 ) return 0;
		out[0] = UC_GETA;
		return BPK_TRON_NOUCS;
	}
	if ( *p_plane == 1 ) return plane1(hi, lo, out);
	if ( zone_linear(hi, lo) < 0 ) return 0;
	out[0] = UC_GETA;
	return BPK_TRON_NOUCS;
}

EXPORT UINT bpk_tron_ucs( UINT ch, INT *p_plane )
{
	UINT	u[2];

	return ( bpk_tron_chars(ch, p_plane, u) != 0 ) ? u[0] : 0;
}

/* The row and cell (row << 8 | cell) of a character in zone A, 0 none */
LOCAL UINT jis_of( UINT cp )
{
	INT	lo = 0, hi = bpk_jis_nrev - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;
		UINT	u = bpk_jis_rev[mid] >> 16;

		if ( u == cp ) return bpk_jis_rev[mid] & 0xFFFF;
		if ( u < cp ) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

/* The plane 1 code of a character zone A does not hold, 0 none */
LOCAL UINT other_of( UINT cp )
{
	INT	lo = 0, hi = bpk_tron_nrev - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;
		UINT	u = bpk_tron_rev_cp[mid];

		if ( u == cp ) return bpk_tron_rev_code[mid];
		if ( u < cp ) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

/* A code written in a plane, the plane switch before it when it has to be */
LOCAL INT put_code( INT plane, UINT code, UH *out, INT max, INT *p_plane )
{
	INT	n = 0;

	if ( plane != *p_plane ) {
		if ( max < 2 ) return 0;
		out[n++] = (UH)( 0xFE21 + plane - 1 );
		*p_plane = plane;
	} else if ( max < 1 ) {
		return 0;
	}
	out[n++] = (UH)code;
	return n;
}

EXPORT INT bpk_ucs_tron( UINT cp, UH *out, INT max, INT *p_plane )
{
	UINT	code, rc;

	if ( cp == TC_TAB || cp == TC_NL || cp == TC_CR ) {
		if ( max < 1 ) return 0;
		out[0] = (UH)cp;
		return 1;
	}
	/* plane 1 when it has the character: ASCII in row 3, the rest by the tables */
	if ( cp >= 0x20 && cp <= 0x7E ) return put_code(1, 0x2300 | cp, out, max, p_plane);
	/* row 3 of plane 1 reads as ASCII: what the tables put there goes elsewhere */
	if ( cp <= 0xFFFF && ( rc = jis_of(cp) ) != 0 && ( rc >> 8 ) != 3 ) {
		code = ( ( ( rc >> 8 ) + 0x20 ) << 8 ) | ( ( rc & 0xFF ) + 0x20 );
		return put_code(1, code, out, max, p_plane);
	}
	if ( ( code = other_of(cp) ) != 0 && ( code >> 8 ) != 0x23 ) return put_code(1, code, out, max, p_plane);
	if ( cp <= 0xFFFF ) return put_code(PLANE_BMP + (INT)( cp / PLANE_SPAN ), zone_code((INT)( cp % PLANE_SPAN )), out, max, p_plane);
	return put_code(1, TC_GETA, out, max, p_plane);
}

EXPORT void bpk_tron_utf8( const UH *s, INT n, char *out, INT max )
{
	INT	i, k = 0, plane = 1, c, j;

	for ( i = 0; i < n && s[i] != 0; i++ ) {
		UINT	u[2];
		char	o[8];
		INT	m = 0;

		c = bpk_tron_chars(s[i], &plane, u);
		if ( c == BPK_TRON_NOUCS ) c = 1;
		for ( j = 0; j < c; j++ ) {
			if ( u[j] >= 0x20 ) m += bpk_utf8(u[j], o + m);
		}
		if ( k + m >= max ) break;
		memcpy(out + k, o, (size_t)m);
		k += m;
	}
	out[k] = 0;
}

/* One character out of UTF-8; its length, 0 at the end, 1 for a stray byte */
LOCAL INT utf8_next( const UB *s, UINT *p_cp )
{
	UINT	c = s[0];
	INT	n, i;

	if ( c == 0 ) return 0;
	if ( c < 0x80 ) {
		*p_cp = c;
		return 1;
	}
	n = ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 0;
	if ( n == 0 ) {
		*p_cp = UC_GETA;
		return 1;
	}
	*p_cp = c & ( 0x3F >> ( n - 1 ) );
	for ( i = 1; i < n; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) {
			*p_cp = UC_GETA;
			return i;
		}
		*p_cp = ( *p_cp << 6 ) | ( s[i] & 0x3F );
	}
	return n;
}

/* The code of a letter and the mark after it that have a place together, 0 none */
LOCAL UINT seq_of( UINT a, UINT b )
{
	INT	i;

	for ( i = 0; i < bpk_tron_nseq; i++ ) {
		if ( bpk_tron_seq[i][0] == a && bpk_tron_seq[i][1] == b ) return bpk_tron_seq[i][2];
	}
	return 0;
}

EXPORT INT bpk_utf8_tron( const char *s, UH *out, INT max )
{
	const UB	*p = (const UB *)s;
	INT		k = 0, plane = 1, m, d, d2;
	UINT		cp = 0, next = 0, code;

	while ( ( d = utf8_next(p, &cp) ) > 0 ) {
		d2 = utf8_next(p + d, &next);
		if ( d2 > 0 && ( code = seq_of(cp, next) ) != 0 ) {
			m = put_code(1, code, out + k, max - k, &plane);
			d += d2;
		} else {
			m = bpk_ucs_tron(cp, out + k, max - k, &plane);
		}
		if ( m == 0 ) break;
		k += m;
		p += d;
	}
	for ( m = k; m < max; m++ ) out[m] = 0;
	return k;
}
