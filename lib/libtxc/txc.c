/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	txc.c
 *	Text in the encodings of Japanese files, to and from UTF-8
 *	(design 18.20)
 *
 *	Every encoding is read one character at a time into a Unicode
 *	scalar value, and written one at a time from one. The double byte
 *	encodings meet in one grid of 94 cells a row (txc_tab.h): Shift_JIS
 *	numbers it by lead and trail byte, EUC-JP and ISO-2022-JP by row and
 *	cell. Going back, a character may stand in more than one place --
 *	the NEC and IBM extensions repeat each other and parts of JIS X 0208
 *	-- and each encoding chooses the place it can say and that the
 *	Windows code page chooses: for Shift_JIS the IBM rows rather than
 *	the NEC selected copy of them, for EUC-JP and ISO-2022-JP a place in
 *	the first 94 rows.
 *
 *	JIS X 0212 (補助漢字), which EUC-JP reaches with SS3 (0x8F) and
 *	ISO-2022-JP-2 with ESC $ ( D, is not a table of this library's own:
 *	it is the one lib/libbpk keeps for TRON code, where the same rows sit
 *	at first bytes A1 to ED of zone B of plane 1. Its symbols are taken
 *	weakly, so a program that does not link that table reads and writes
 *	the rest all the same and has no JIS X 0212 (txc_has_x0212).
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/txc.h>
#include "txc_tab.h"

/* ISO-2022-JP: what the bytes 0x21 to 0x7E mean at the moment */
#define G_ASCII		0
#define G_ROMAN		1		/* JIS X 0201 Roman */
#define G_KANA		2		/* JIS X 0201 katakana */
#define G_JIS		3		/* JIS X 0208 */
#define G_X0212		4		/* JIS X 0212 */

/* JIS X 0212 as lib/libbpk has it: zone B of TRON code's plane 1 */
#define ZB_FIRST	0x87		/* its first byte's first */
#define X0212_FIRST	0xA1		/* the first byte of row 1 of JIS X 0212 */
#define X0212_ROWS	77

IMPORT CONST UH   bpk_tron_zb[] __attribute__((weak));
IMPORT CONST UINT bpk_tron_rev_cp[] __attribute__((weak));
IMPORT CONST UH   bpk_tron_rev_code[] __attribute__((weak));
IMPORT CONST INT  bpk_tron_nrev __attribute__((weak));

#define BAD		(-1)		/* a character that cannot be read */

/* ---------------------------------------------------------------- UTF-8 */

EXPORT INT txc_utf8_get( CONST UB *s, SZ n, INT *p_c )
{
	INT	c, k, i, min;

	if ( n <= 0 ) {
		return 0;
	}
	c = s[0];
	if ( c < 0x80 ) {
		*p_c = c;
		return 1;
	}
	if ( c >= 0xC2 && c <= 0xDF ) {
		k = 1;  c &= 0x1F;  min = 0x80;
	} else if ( c >= 0xE0 && c <= 0xEF ) {
		k = 2;  c &= 0x0F;  min = 0x800;
	} else if ( c >= 0xF0 && c <= 0xF4 ) {
		k = 3;  c &= 0x07;  min = 0x10000;
	} else {
		*p_c = BAD;
		return 1;
	}
	if ( (SZ)k >= n ) {
		*p_c = BAD;
		return 1;
	}
	for ( i = 1; i <= k; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) {
			*p_c = BAD;
			return 1;
		}
		c = ( c << 6 ) | ( s[i] & 0x3F );
	}
	if ( c < min || c > 0x10FFFF || ( c >= 0xD800 && c <= 0xDFFF ) ) {
		*p_c = BAD;
		return 1;
	}
	*p_c = c;
	return k + 1;
}

EXPORT INT txc_utf8_put( INT c, UB *out )
{
	if ( c < 0x80 ) {
		out[0] = (UB)c;
		return 1;
	}
	if ( c < 0x800 ) {
		out[0] = (UB)( 0xC0 | ( c >> 6 ) );
		out[1] = (UB)( 0x80 | ( c & 0x3F ) );
		return 2;
	}
	if ( c < 0x10000 ) {
		out[0] = (UB)( 0xE0 | ( c >> 12 ) );
		out[1] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
		out[2] = (UB)( 0x80 | ( c & 0x3F ) );
		return 3;
	}
	out[0] = (UB)( 0xF0 | ( c >> 18 ) );
	out[1] = (UB)( 0x80 | ( ( c >> 12 ) & 0x3F ) );
	out[2] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
	out[3] = (UB)( 0x80 | ( c & 0x3F ) );
	return 4;
}

/* ---------------------------------------------------------------- the grid */

/* The character at a pointer, or 0 */
LOCAL INT ptr_char( INT p )
{
	if ( p >= 0 && p < TXC_MAIN_N ) {
		return txc_main[p];
	}
	if ( p >= TXC_PUA_FROM && p < TXC_PUA_TO ) {
		return 0xE000 + ( p - TXC_PUA_FROM );
	}
	if ( p >= TXC_IBM_FROM && p < TXC_IBM_FROM + TXC_IBM_N ) {
		return txc_ibm[p - TXC_IBM_FROM];
	}
	return 0;
}

EXPORT BOOL txc_has_x0212( void )
{
	return (BOOL)( bpk_tron_zb != NULL && bpk_tron_rev_cp != NULL && bpk_tron_rev_code != NULL
		    && &bpk_tron_nrev != NULL );
}

/* The character of JIS X 0212 at a row and cell (1 to 94), or 0 */
LOCAL INT x0212_char( INT row, INT cell )
{
	UH	v;

	if ( !txc_has_x0212() || row < 1 || row > X0212_ROWS || cell < 1 || cell > 94 ) {
		return 0;
	}
	v = bpk_tron_zb[( X0212_FIRST - ZB_FIRST + row - 1 ) * 94 + cell - 1];
	return ( v >= 0xD800 && v <= 0xDFFF ) ? 0 : v;	/* a place of two characters: none here */
}

/* The row and cell of JIS X 0212 of a character, (row << 8) | cell, or 0 */
LOCAL INT x0212_code( INT c )
{
	INT	lo = 0, hi, mid;
	UINT	code;

	if ( !txc_has_x0212() ) {
		return 0;
	}
	hi = bpk_tron_nrev;
	while ( lo < hi ) {
		mid = ( lo + hi ) / 2;
		if ( bpk_tron_rev_cp[mid] < (UINT)c ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if ( lo >= bpk_tron_nrev || bpk_tron_rev_cp[lo] != (UINT)c ) {
		return 0;
	}
	code = bpk_tron_rev_code[lo];
	if ( ( code >> 8 ) < X0212_FIRST || ( code >> 8 ) >= X0212_FIRST + X0212_ROWS ) {
		return 0;			/* JIS X 0213, which neither encoding here has */
	}
	return (INT)( ( ( ( code >> 8 ) - X0212_FIRST + 1 ) << 8 ) | ( ( code & 0xFF ) - 0x20 ) );
}

EXPORT INT txc_jis_char( INT row, INT cell )
{
	if ( row < 1 || row > 94 || cell < 1 || cell > 94 ) {
		return 0;
	}
	return txc_main[( row - 1 ) * 94 + cell - 1];
}

/*
 * Characters the table does not hold that have a place under another
 * name: those the JIS mapping gives where the Windows one differs.
 */
LOCAL CONST UH alias[][2] = {
	{ 0x00A2, 0xFFE0 }, { 0x00A3, 0xFFE1 }, { 0x00A6, 0xFFE4 }, { 0x00AC, 0xFFE2 },
	{ 0x2014, 0x2015 }, { 0x2016, 0x2225 }, { 0x2212, 0xFF0D }, { 0x301C, 0xFF5E },
};
#define NALIAS	( (INT)( sizeof(alias) / sizeof(alias[0]) ) )

/*
 * The pointer of a character for an encoding, or -1. Shift_JIS takes
 * the first place outside rows 89 to 94 when there is one; the others
 * take the first place in the 94 rows.
 */
LOCAL INT char_ptr( INT c, INT enc )
{
	INT	lo = 0, hi = TXC_BACK_N, mid, i, first = -1;

	for ( i = 0; i < NALIAS; i++ ) {
		if ( alias[i][0] == c ) {
			c = alias[i][1];
			break;
		}
	}
	if ( c < 0x80 || c > 0xFFFF ) {
		return -1;
	}
	while ( lo < hi ) {
		mid = ( lo + hi ) / 2;
		if ( txc_back[mid][0] < c ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	for ( i = lo; i < TXC_BACK_N && txc_back[i][0] == c; i++ ) {
		INT	p = txc_back[i][1];

		if ( enc == TXC_SJIS ) {
			if ( p < TXC_NEC_IBM_FROM || p >= TXC_MAIN_N ) return p;
			if ( first < 0 ) first = p;
		} else if ( p < TXC_MAIN_N ) {
			return p;
		}
	}
	return first;
}

EXPORT UINT txc_sjis_code( INT c )
{
	INT	p, lead, trail;

	if ( c < 0x80 ) {
		return (UINT)c;
	}
	if ( c == 0x00A5 ) return 0x5C;
	if ( c == 0x203E ) return 0x7E;
	if ( c >= 0xFF61 && c <= 0xFF9F ) {
		return (UINT)( c - 0xFF61 + 0xA1 );
	}
	if ( c >= 0xE000 && c < 0xE000 + ( TXC_PUA_TO - TXC_PUA_FROM ) ) {
		p = TXC_PUA_FROM + ( c - 0xE000 );
	} else {
		p = char_ptr(c, TXC_SJIS);
	}
	if ( p < 0 ) {
		return 0;
	}
	lead = p / 188;
	trail = p % 188;
	return (UINT)( ( ( lead + ( ( lead < 0x1F ) ? 0x81 : 0xC1 ) ) << 8 )
		       | ( trail + ( ( trail < 0x3F ) ? 0x40 : 0x41 ) ) );
}

/* ---------------------------------------------------------------- reading */

typedef struct {
	INT	enc;
	CONST UB *s;
	SZ	n, at;
	INT	g;			/* ISO-2022-JP: the set in use */
} DEC;

/* The next character, BAD for one that cannot be read; FALSE at the end */
LOCAL BOOL dec_next( DEC *d, INT *p_c )
{
	CONST UB	*s = d->s + d->at;
	SZ		left = d->n - d->at;
	INT		b, b2, c, k;

	if ( left <= 0 ) {
		return FALSE;
	}
	b = s[0];
	switch ( d->enc ) {
	case TXC_SJIS:
		if ( b < 0x80 ) {
			c = b;  k = 1;
		} else if ( b >= 0xA1 && b <= 0xDF ) {
			c = 0xFF61 + ( b - 0xA1 );  k = 1;
		} else if ( ( ( b >= 0x81 && b <= 0x9F ) || ( b >= 0xE0 && b <= 0xFC ) ) && left >= 2
			    && ( ( s[1] >= 0x40 && s[1] <= 0x7E ) || ( s[1] >= 0x80 && s[1] <= 0xFC ) ) ) {
			INT	lead = b - ( ( b < 0xA0 ) ? 0x81 : 0xC1 );
			INT	trail = s[1] - ( ( s[1] < 0x7F ) ? 0x40 : 0x41 );

			c = ptr_char(lead * 188 + trail);
			if ( c == 0 ) c = BAD;
			k = 2;
		} else {
			c = BAD;  k = 1;
		}
		break;

	case TXC_EUCJP:
		if ( b < 0x80 ) {
			c = b;  k = 1;
		} else if ( b == 0x8E && left >= 2 && s[1] >= 0xA1 && s[1] <= 0xDF ) {
			c = 0xFF61 + ( s[1] - 0xA1 );  k = 2;
		} else if ( b == 0x8F && left >= 3 && s[1] >= 0xA1 && s[1] <= 0xFE
			    && s[2] >= 0xA1 && s[2] <= 0xFE ) {
			c = x0212_char(s[1] - 0xA0, s[2] - 0xA0);	/* JIS X 0212 */
			if ( c == 0 ) c = BAD;
			k = 3;
		} else if ( b >= 0xA1 && b <= 0xFE && left >= 2 && s[1] >= 0xA1 && s[1] <= 0xFE ) {
			c = ptr_char(( b - 0xA1 ) * 94 + ( s[1] - 0xA1 ));
			if ( c == 0 ) c = BAD;
			k = 2;
		} else {
			c = BAD;  k = 1;
		}
		break;

	case TXC_JIS:
		/* escape sequences change the set and give no character */
		while ( b == 0x1B || b == 0x0E || b == 0x0F ) {
			k = 0;
			if ( b == 0x0E ) {
				d->g = G_KANA;  k = 1;
			} else if ( b == 0x0F ) {
				d->g = G_ASCII;  k = 1;
			} else if ( left >= 3 && s[1] == '$' && ( s[2] == 'B' || s[2] == '@' ) ) {
				d->g = G_JIS;  k = 3;
			} else if ( left >= 4 && s[1] == '$' && s[2] == '(' && s[3] == 'D' ) {
				d->g = G_X0212;  k = 4;
			} else if ( left >= 4 && s[1] == '$' && s[2] == '(' && s[3] == 'B' ) {
				d->g = G_JIS;  k = 4;
			} else if ( left >= 3 && s[1] == '(' && s[2] == 'B' ) {
				d->g = G_ASCII;  k = 3;
			} else if ( left >= 3 && s[1] == '(' && s[2] == 'J' ) {
				d->g = G_ROMAN;  k = 3;
			} else if ( left >= 3 && s[1] == '(' && s[2] == 'I' ) {
				d->g = G_KANA;  k = 3;
			}
			if ( k == 0 ) {
				break;			/* an escape of no set known: read as a fault */
			}
			d->at += k;
			s += k;
			left -= k;
			if ( left <= 0 ) {
				return FALSE;
			}
			b = s[0];
		}
		if ( b == 0x1B || b >= 0x80 ) {
			c = BAD;  k = 1;
		} else if ( b == '\n' || b == '\r' || b < 0x21 || b == 0x7F ) {
			c = b;  k = 1;			/* controls and blanks in every set */
		} else if ( d->g == G_JIS || d->g == G_X0212 ) {
			b2 = ( left >= 2 ) ? s[1] : 0;
			if ( b2 >= 0x21 && b2 <= 0x7E ) {
				c = ( d->g == G_X0212 ) ? x0212_char(b - 0x20, b2 - 0x20)
				    : ptr_char(( b - 0x21 ) * 94 + ( b2 - 0x21 ));
				if ( c == 0 ) c = BAD;
				k = 2;
			} else {
				c = BAD;  k = 1;
			}
		} else if ( d->g == G_KANA ) {
			c = ( b <= 0x5F ) ? 0xFF61 + ( b - 0x21 ) : BAD;  k = 1;
		} else if ( d->g == G_ROMAN ) {
			c = ( b == 0x5C ) ? 0x00A5 : ( b == 0x7E ) ? 0x203E : b;  k = 1;
		} else {
			c = b;  k = 1;
		}
		break;

	case TXC_UTF16LE:
	case TXC_UTF16BE:
		if ( left < 2 ) {
			c = BAD;  k = (INT)left;
			break;
		}
		c = ( d->enc == TXC_UTF16LE ) ? ( s[0] | ( s[1] << 8 ) ) : ( ( s[0] << 8 ) | s[1] );
		k = 2;
		if ( c >= 0xD800 && c <= 0xDBFF ) {
			b2 = -1;
			if ( left >= 4 ) {
				b2 = ( d->enc == TXC_UTF16LE ) ? ( s[2] | ( s[3] << 8 ) )
							       : ( ( s[2] << 8 ) | s[3] );
			}
			if ( b2 >= 0xDC00 && b2 <= 0xDFFF ) {
				c = 0x10000 + ( ( c - 0xD800 ) << 10 ) + ( b2 - 0xDC00 );
				k = 4;
			} else {
				c = BAD;
			}
		} else if ( c >= 0xDC00 && c <= 0xDFFF ) {
			c = BAD;
		}
		break;

	default:				/* UTF-8 */
		k = txc_utf8_get(s, left, &c);
		break;
	}
	d->at += k;
	*p_c = c;
	return TRUE;
}

EXPORT INT txc_bom( CONST UB *s, SZ n, INT *p_enc )
{
	INT	enc = TXC_AUTO, k = 0;

	if ( n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF ) {
		enc = TXC_UTF8;  k = 3;
	} else if ( n >= 2 && s[0] == 0xFF && s[1] == 0xFE ) {
		enc = TXC_UTF16LE;  k = 2;
	} else if ( n >= 2 && s[0] == 0xFE && s[1] == 0xFF ) {
		enc = TXC_UTF16BE;  k = 2;
	}
	if ( p_enc != NULL ) {
		*p_enc = enc;
	}
	return k;
}

EXPORT SZ txc_to_utf8( INT enc, CONST UB *s, SZ n, UB *out, SZ max, INT *p_bad )
{
	DEC	d;
	UB	u[4];
	SZ	w = 0;
	INT	c, k, bad = 0, benc;

	if ( enc == TXC_AUTO ) {
		enc = txc_detect(s, n);
	}
	d.enc = enc;
	d.s = s;
	d.n = n;
	d.g = G_ASCII;
	d.at = txc_bom(s, n, &benc);
	if ( benc != enc ) {
		d.at = 0;			/* a mark of another encoding is not one */
	}
	while ( dec_next(&d, &c) ) {
		if ( c == BAD ) {
			c = TXC_GETA;
			bad++;
		}
		k = txc_utf8_put(c, u);
		if ( out != NULL && w + k < max ) {
			INT	i;

			for ( i = 0; i < k; i++ ) out[w + i] = u[i];
			out[w + k] = 0;
		} else if ( out != NULL && w < max ) {
			out[w] = 0;
			max = w;		/* full: nothing more is written */
		}
		w += k;
	}
	if ( out != NULL && max > 0 && w == 0 ) {
		out[0] = 0;
	}
	if ( p_bad != NULL ) {
		*p_bad = bad;
	}
	return w;
}

/* ---------------------------------------------------------------- writing */

/* One character in an encoding into b (room for 8); how many bytes */
LOCAL INT enc_one( INT enc, INT c, UB *b, INT *g, BOOL *p_bad )
{
	UINT	code;
	INT	p, x, n = 0, want;

	*p_bad = FALSE;
	switch ( enc ) {
	case TXC_SJIS:
		code = txc_sjis_code(c);
		if ( code == 0 && c != 0 ) {
			code = txc_sjis_code(TXC_GETA);
			*p_bad = TRUE;
		}
		if ( code >= 0x100 ) b[n++] = (UB)( code >> 8 );
		b[n++] = (UB)code;
		return n;

	case TXC_EUCJP:
		if ( c < 0x80 ) {
			b[0] = (UB)c;
			return 1;
		}
		if ( c == 0x00A5 || c == 0x203E ) {
			b[0] = ( c == 0x00A5 ) ? 0x5C : 0x7E;
			return 1;
		}
		if ( c >= 0xFF61 && c <= 0xFF9F ) {
			b[0] = 0x8E;
			b[1] = (UB)( c - 0xFF61 + 0xA1 );
			return 2;
		}
		p = char_ptr(c, TXC_EUCJP);
		x = ( p < 0 ) ? x0212_code(c) : 0;
		if ( x > 0 ) {
			b[0] = 0x8F;
			b[1] = (UB)( ( x >> 8 ) + 0xA0 );
			b[2] = (UB)( ( x & 0xFF ) + 0xA0 );
			return 3;
		}
		if ( p < 0 ) {
			p = char_ptr(TXC_GETA, TXC_EUCJP);
			*p_bad = TRUE;
		}
		b[0] = (UB)( p / 94 + 0xA1 );
		b[1] = (UB)( p % 94 + 0xA1 );
		return 2;

	case TXC_JIS:
		if ( c < 0x80 || c == 0x00A5 || c == 0x203E ) {
			want = G_ASCII;
			if ( c == 0x00A5 ) c = 0x5C;
			if ( c == 0x203E ) c = 0x7E;
		} else if ( c >= 0xFF61 && c <= 0xFF9F ) {
			want = G_KANA;
		} else {
			want = G_JIS;
			p = char_ptr(c, TXC_JIS);
			x = ( p < 0 ) ? x0212_code(c) : 0;
			if ( x > 0 ) {
				want = G_X0212;		/* ISO-2022-JP-2 */
				p = x;
			} else if ( p < 0 ) {
				p = char_ptr(TXC_GETA, TXC_JIS);
				*p_bad = TRUE;
			}
		}
		if ( want != *g ) {
			b[n++] = 0x1B;
			b[n++] = ( want == G_JIS || want == G_X0212 ) ? '$' : '(';
			if ( want == G_X0212 ) b[n++] = '(';
			b[n++] = ( want == G_KANA ) ? 'I' : ( want == G_X0212 ) ? 'D' : 'B';
			*g = want;
		}
		if ( want == G_X0212 ) {
			b[n++] = (UB)( ( p >> 8 ) + 0x20 );
			b[n++] = (UB)( ( p & 0xFF ) + 0x20 );
		} else if ( want == G_JIS ) {
			b[n++] = (UB)( p / 94 + 0x21 );
			b[n++] = (UB)( p % 94 + 0x21 );
		} else if ( want == G_KANA ) {
			b[n++] = (UB)( c - 0xFF61 + 0x21 );
		} else {
			b[n++] = (UB)c;
		}
		return n;

	case TXC_UTF16LE:
	case TXC_UTF16BE:
		if ( c >= 0x10000 ) {
			INT	hi = 0xD800 + ( ( c - 0x10000 ) >> 10 );
			INT	lo = 0xDC00 + ( ( c - 0x10000 ) & 0x3FF );

			n = enc_one(enc, hi, b, g, p_bad);
			return n + enc_one(enc, lo, b + n, g, p_bad);
		}
		if ( enc == TXC_UTF16LE ) {
			b[0] = (UB)c;
			b[1] = (UB)( c >> 8 );
		} else {
			b[0] = (UB)( c >> 8 );
			b[1] = (UB)c;
		}
		return 2;

	default:
		return txc_utf8_put(c, b);
	}
}

EXPORT SZ txc_from_utf8( INT enc, CONST UB *s, SZ n, UB *out, SZ max, INT *p_bad )
{
	UB	b[16];
	SZ	w = 0, at = 0;
	INT	c, k, i, g = G_ASCII, bad = 0;
	BOOL	isbad, full = FALSE;

	if ( enc == TXC_AUTO ) {
		enc = TXC_UTF8;
	}
	at = ( n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF ) ? 3 : 0;
	for (;;) {
		if ( at < n ) {
			at += txc_utf8_get(s + at, n - at, &c);
			if ( c == BAD ) {
				c = TXC_GETA;
				bad++;
			}
			k = enc_one(enc, c, b, &g, &isbad);
			if ( isbad ) bad++;
		} else if ( enc == TXC_JIS && g != G_ASCII ) {
			b[0] = 0x1B;  b[1] = '(';  b[2] = 'B';
			k = 3;
			g = G_ASCII;
		} else {
			break;
		}
		if ( out != NULL && !full && w + k < max ) {
			for ( i = 0; i < k; i++ ) out[w + i] = b[i];
			out[w + k] = 0;
		} else if ( out != NULL && !full ) {
			full = TRUE;
			if ( w < max ) out[w] = 0;
		}
		w += k;
	}
	if ( out != NULL && max > 0 && w == 0 ) {
		out[0] = 0;
	}
	if ( p_bad != NULL ) {
		*p_bad = bad;
	}
	return w;
}

/* ---------------------------------------------------------------- guessing */

/*
 * How well a text reads in an encoding: a fault costs much, kana and
 * the kanji of the first level earn, half width katakana and the
 * private use area cost a little (where the other encoding's bytes
 * land when read wrongly). A fault in the last three bytes is the
 * text cut short and costs nothing.
 */
LOCAL INT score( INT enc, CONST UB *s, SZ n )
{
	DEC	d;
	INT	c, sc = 0;

	d.enc = enc;
	d.s = s;
	d.n = n;
	d.at = 0;
	d.g = G_ASCII;
	while ( dec_next(&d, &c) ) {
		if ( c == BAD ) {
			if ( d.at + 3 < n ) sc -= 50;
		} else if ( c >= 0x3041 && c <= 0x30FE ) {
			sc += 3;
		} else if ( c >= 0x3000 && c <= 0x303F ) {
			sc += 2;
		} else if ( c >= 0x4E00 && c <= 0x9FFF ) {
			INT	p = char_ptr(c, TXC_EUCJP);

			sc += ( p >= 15 * 94 && p < 47 * 94 ) ? 2 : 1;
		} else if ( c >= 0xFF01 && c <= 0xFF5E ) {
			sc += 1;
		} else if ( c >= 0xFF61 && c <= 0xFF9F ) {
			sc -= 1;
		} else if ( c >= 0xE000 && c <= 0xF8FF ) {
			sc -= 5;
		}
	}
	return sc;
}

EXPORT INT txc_detect( CONST UB *s, SZ n )
{
	SZ	i;
	INT	enc, k, c, esc = 0, high = 0, even0 = 0, odd0 = 0, mb = 0, bad = 0;

	if ( txc_bom(s, n, &enc) > 0 ) {
		return enc;
	}
	for ( i = 0; i < n; i++ ) {
		if ( s[i] >= 0x80 ) high++;
		if ( s[i] == 0 ) {
			if ( ( i & 1 ) == 0 ) even0++; else odd0++;
		}
		if ( s[i] == 0x1B && i + 2 < n
		  && ( ( s[i + 1] == '$' && ( s[i + 2] == 'B' || s[i + 2] == '@' ) )
		    || ( s[i + 1] == '(' && ( s[i + 2] == 'J' || s[i + 2] == 'I' ) ) ) ) {
			esc++;
		}
	}
	/* UTF-16 with no mark: one byte of each pair nought for most of the text */
	if ( n >= 4 && odd0 > (INT)( n / 4 ) && even0 <= odd0 / 8 ) return TXC_UTF16LE;
	if ( n >= 4 && even0 > (INT)( n / 4 ) && odd0 <= even0 / 8 ) return TXC_UTF16BE;
	if ( high == 0 ) {
		return ( esc > 0 ) ? TXC_JIS : TXC_UTF8;
	}
	for ( i = 0; i < n; i += k ) {
		k = txc_utf8_get(s + i, n - i, &c);
		if ( c == BAD ) {
			if ( i + 4 < n ) bad++;	/* not a character cut by the end */
		} else if ( k > 1 ) {
			mb++;
		}
	}
	if ( bad == 0 && mb > 0 ) {
		return TXC_UTF8;
	}
	return ( score(TXC_EUCJP, s, n) > score(TXC_SJIS, s, n) ) ? TXC_EUCJP : TXC_SJIS;
}

/* ---------------------------------------------------------------- names */

LOCAL CONST char * CONST enc_name[TXC_NENC] = {
	"", "utf-8", "shift_jis", "euc-jp", "iso-2022-jp", "utf-16le", "utf-16be"
};

EXPORT CONST char *txc_name( INT enc )
{
	return ( enc > 0 && enc < TXC_NENC ) ? enc_name[enc] : "";
}

/* Other names the same encodings go by */
LOCAL CONST struct {
	CONST char	*name;
	INT		enc;
} enc_alias[] = {
	{ "utf8", TXC_UTF8 }, { "sjis", TXC_SJIS }, { "cp932", TXC_SJIS },
	{ "ms932", TXC_SJIS }, { "windows-31j", TXC_SJIS }, { "eucjp", TXC_EUCJP },
	{ "jis", TXC_JIS }, { "utf-16", TXC_UTF16BE },
};

/* Two names the same, case and '_' against '-' ignored */
LOCAL BOOL same_name( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		UB	x = a[i], y = (UB)b[i];

		if ( x >= 'A' && x <= 'Z' ) x = (UB)( x - 'A' + 'a' );
		if ( x == '_' ) x = '-';
		if ( y == '_' ) y = '-';
		if ( x != y ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

EXPORT INT txc_by_name( CONST UB *name )
{
	INT	e;

	if ( name == NULL ) {
		return TXC_AUTO;
	}
	for ( e = 1; e < TXC_NENC; e++ ) {
		if ( same_name(name, enc_name[e]) ) return e;
	}
	for ( e = 0; e < (INT)( sizeof(enc_alias) / sizeof(enc_alias[0]) ); e++ ) {
		if ( same_name(name, enc_alias[e].name) ) return enc_alias[e].enc;
	}
	return TXC_AUTO;
}
