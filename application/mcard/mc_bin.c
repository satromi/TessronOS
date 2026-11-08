/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_bin.c
 *	マイクロカード: the records of マイクロカード実身 (design 11.18)
 *
 *	The database is kept as 超漢字 and B-right/V keep it, in the records
 *	of type 31 that マイクロカード実身データ構造説明書 (1999/05/20)
 *	describes, every word little-endian:
 *
 *	  subtype 1, the format: TC id1..id3 (Ｃ Ｒ Ｄ), UH version (0x100),
 *	  UH ncards, UH datalen (the bytes of one card), UH reserved[9], UH
 *	  nitems, and nitems of IFMT (24 bytes: TC name[8], UH offset, UH
 *	  kind -- 1 a number, 2 a string, 3 a link --, UH nchars, UH tchars)
 *	subtype 2, the cards: ncards of CARD, each UH startmark (0xA5B6), UH
 *	  datalen and its items -- a number as a DOUBLE, a string as TC
 *	  string[nchars + 1], a link as UH vobjnumber and 23 words unused
 *	subtype 3, the order: UH index[ncards], the card at each place
 *	subtype 4, the links: VDATA, each UH vobjnumber, UH linknumber, UH
 *	  attr[5] and a 仮身セグメント (its RECT, height, CHSIZE, four
 *	  COLORs, dlen and dlen bytes), 44 bytes and dlen
 *
 *	The cards are written in their order, so the order record is left
 *	out; it is read when it is there. A link of the 印刷書式 is vobjnumber
 *	1, the links of the items 5 and on. The n-th link record of the
 *	object -- the object a linknumber n points at -- is here the n-th
 *	<link> of record 0, one for each object; record 0 is written again
 *	with the binary records. Strings are TRON characters, and their
 *	sizes count the plane switches as the description says. A number
 *	field left empty is written as a quiet NaN and read back as empty.
 *
 *	The numbers are turned between their digits and a DOUBLE in integer
 *	arithmetic only, the kernel's test linking this too: the digits go
 *	to the nearest DOUBLE, and a DOUBLE is shown in 15 significant
 *	figures.
 */

#include "mc_db.h"
#include <ts/bpk.h>
#include <ts/vobjsz.h>

#define FMT_HEAD	32		/* the format record before its items */
#define IFMT_SIZE	24
#define CARD_HEAD	4
#define LINK_ITEM	48
#define VDATA_HEAD	14
#define VSEG_SIZE	30
#define START_MARK	0xA5B6
#define VOBJ_LAYOUT	1		/* the 印刷書式's link */
#define VOBJ_FIRST	5		/* the first of the items' links */
#define TC_NULL		0
#define TC_C		0x2343		/* Ｃ */
#define TC_R		0x2352		/* Ｒ */
#define TC_D		0x2344		/* Ｄ */
#define FMT_VERSION	0x100
#define COL_BLACK	0x10000000U
#define COL_WHITE	0x10FFFFFFU

/* What a link shows, as the link record's first attribute word has it */
#define V_NONAME	0x0001
#define V_NORELN	0x0002
#define V_NOTYPE	0x0004
#define V_NOTIME	0x0008
#define V_NOPICT	0x0040
#define V_NOFDISP	0x0080
#define V_AUTEXE	0x4000

/* The kinds of item as the format names them */
#define K_NONE		0
#define K_NUM		1
#define K_STR		2
#define K_LINK		3

/* ---------------------------------------------------------------- bytes */

LOCAL UINT get16( CONST UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL void put16( CTBUF *b, UINT v )
{
	UB	w[2];

	w[0] = (UB)( v & 0xFF );
	w[1] = (UB)( ( v >> 8 ) & 0xFF );
	ct_putn(b, w, 2);
}

LOCAL void put32( CTBUF *b, UINT v )
{
	put16(b, v & 0xFFFF);
	put16(b, v >> 16);
}

LOCAL void zeros( CTBUF *b, INT n )
{
	static CONST UB	z[16] = { 0 };

	while ( n > 0 ) {
		INT	k = ( n > 16 ) ? 16 : n;

		ct_putn(b, z, k);
		n -= k;
	}
}

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	if ( s == NULL ) return 0;
	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL at_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( i >= n || s[i] != (UB)w[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL INT find( CONST UB *s, INT n, INT from, CONST char *w )
{
	INT	i;

	for ( i = from; i < n; i++ ) {
		if ( s[i] == (UB)w[0] && at_is(s + i, n - i, w) ) return i;
	}
	return -1;
}

LOCAL INT tag_end( CONST UB *s, INT n, INT at )
{
	INT	i;
	UB	q = 0;

	for ( i = at + 1; i < n; i++ ) {
		if ( q != 0 ) {
			if ( s[i] == q ) q = 0;
		} else if ( s[i] == '"' || s[i] == '\'' ) {
			q = s[i];
		} else if ( s[i] == '>' ) {
			return i + 1;
		}
	}
	return n;
}

/* A whole <link> element at s[at]: where it ends, with its end tag when it has one */
LOCAL INT link_end( CONST UB *s, INT n, INT at )
{
	INT	e = tag_end(s, n, at);

	if ( e >= 2 && s[e - 2] != '/' ) {
		INT	c = find(s, n, e, "</link>");

		if ( c >= 0 ) e = c + 7;
	}
	return e;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 || b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- TRON strings */

/* A UTF-8 text as TRON characters into out (at most max, the rest left 0); how many */
LOCAL INT to_tron( CONST UB *s, INT n, UH *out, INT max )
{
	INT	i = 0, k = 0, plane = 1;
	UINT	c;

	while ( i < n ) {
		UH	t[2];
		INT	l = mc_u8_get(s + i, n - i, &c), m, j, pl = plane;

		if ( l <= 0 ) break;
		m = bpk_ucs_tron(c, t, 2, &pl);
		if ( m <= 0 || k + m > max ) break;	/* a letter with its plane switch, whole or not at all */
		for ( j = 0; j < m; j++ ) out[k++] = t[j];
		plane = pl;
		i += l;
	}
	return k;
}

/* TRON characters as UTF-8, to the first TNULL; the length */
LOCAL INT from_tron( CONST UB *p, INT nwords, UB *out, INT max )
{
	INT	i, k = 0, plane = 1;

	for ( i = 0; i < nwords; i++ ) {
		UINT	u[2];
		UINT	ch = get16(p + 2 * i);
		INT	m, j;

		if ( ch == TC_NULL ) break;
		m = bpk_tron_chars(ch, &plane, u);
		if ( m == BPK_TRON_NOUCS ) m = 1;
		for ( j = 0; j < m && k < max - 5; j++ ) k += mc_u8_put(u[j], out + k);
	}
	if ( max > 0 ) out[k] = 0;
	return k;
}

/* The bytes of a text that go in so many TRON characters, plane switches counted */
EXPORT INT mc_tron_fit( CONST UB *s, INT n, INT max )
{
	INT	i = 0, k = 0, plane = 1;
	UINT	c;

	while ( i < n ) {
		UH	t[2];
		INT	l = mc_u8_get(s + i, n - i, &c), m, pl = plane;

		if ( l <= 0 ) break;
		m = bpk_ucs_tron(c, t, 2, &pl);
		if ( m <= 0 || k + m > max ) break;
		k += m;
		plane = pl;
		i += l;
	}
	return i;
}

/* ---------------------------------------------------------------- numbers as DOUBLE */

LOCAL INT msb( UD v )
{
	INT	p = 63;

	while ( p > 0 && ( ( v >> p ) & 1 ) == 0 ) p--;
	return p;
}

/* The next bit of r / d, d > r: r made what is left */
LOCAL UINT next_bit( UD *r, UD d )
{
	UD	hi = *r >> 63, r2 = *r << 1;

	if ( hi != 0 || r2 >= d ) {
		*r = r2 - d;
		return 1;
	}
	*r = r2;
	return 0;
}

LOCAL void put_bits( UB *out, UD v )
{
	INT	i;

	for ( i = 0; i < 8; i++ ) out[i] = (UB)( v >> ( 8 * i ) );
}

EXPORT void mc_num_bits( CONST UB *s, UB *out )
{
	UD	m = 0, d = 1, ip, r, mant;
	INT	n = s_len(s), i = 0, fd = 0, e2, p, k;
	BOOL	neg = FALSE, point = FALSE, any = FALSE;
	UINT	sticky;

	if ( n > 0 && s[0] == '-' ) {
		neg = TRUE;
		i = 1;
	}
	for ( ; i < n; i++ ) {
		if ( s[i] == '.' ) {
			point = TRUE;
			continue;
		}
		if ( s[i] < '0' || s[i] > '9' ) break;
		any = TRUE;
		if ( m > 1844674407370955160ULL ) continue;	/* past 19 digits: no room, no meaning */
		m = m * 10 + (UD)( s[i] - '0' );
		if ( point ) {
			fd++;
			d *= 10;
		}
	}
	if ( !any ) {
		put_bits(out, 0x7FF8000000000000ULL);	/* empty: a quiet NaN */
		return;
	}
	if ( m == 0 ) {
		put_bits(out, 0);
		return;
	}
	(void)fd;
	ip = m / d;
	r = m % d;
	if ( ip > 0 ) {
		p = msb(ip);
		mant = ( p < 63 ) ? ip << ( 63 - p ) : ip;
		for ( k = 62 - p; k >= 0; k-- ) mant |= (UD)next_bit(&r, d) << k;
		e2 = p;
	} else {
		INT	z = 0;

		while ( next_bit(&r, d) == 0 && z < 200 ) z++;
		mant = 1ULL << 63;
		for ( k = 62; k >= 0; k-- ) mant |= (UD)next_bit(&r, d) << k;
		e2 = -( z + 1 );
	}
	/* to 53 bits, the half to the even */
	sticky = ( r != 0 ) ? 1 : 0;
	{
		UD	low = mant & 0x7FF;
		UD	top = mant >> 11;

		if ( low > 0x400 || ( low == 0x400 && ( sticky || ( top & 1 ) ) ) ) {
			top++;
			if ( top >> 53 ) {
				top >>= 1;
				e2++;
			}
		}
		put_bits(out, ( neg ? 0x8000000000000000ULL : 0 ) | ( (UD)( e2 + 1023 ) << 52 ) | ( top & 0xFFFFFFFFFFFFFULL ));
	}
}

EXPORT INT mc_bits_num( CONST UB *in, UB *out, INT max )
{
	UD	v = 0, mant;
	INT	i, ex, e2, nd, point, first, keep, o = 0;
	BOOL	neg;
	UB	*dg;

	for ( i = 0; i < 8; i++ ) v |= (UD)in[i] << ( 8 * i );
	neg = (BOOL)( ( v >> 63 ) != 0 );
	ex = (INT)( ( v >> 52 ) & 0x7FF );
	mant = v & 0xFFFFFFFFFFFFFULL;
	if ( max <= 0 ) return 0;
	out[0] = 0;
	if ( ex == 0x7FF ) return 0;			/* NaN, infinity: empty */
	if ( ex == 0 && mant == 0 ) {
		if ( max > 1 ) {
			out[0] = '0';
			out[1] = 0;
		}
		return 1;
	}
	if ( ex == 0 ) {
		e2 = -1074;
	} else {
		mant |= 1ULL << 52;
		e2 = ex - 1075;
	}
	if ( e2 > 1100 || e2 < -1100 ) return 0;
	/* mant * 2^e2 as decimal digits: mant * 2^e2, or mant * 5^-e2 with -e2 places */
	dg = (UB *)mc_alloc(1200);
	if ( dg == NULL ) return 0;
	nd = 0;
	while ( mant > 0 ) {
		dg[nd++] = (UB)( mant % 10 );
		mant /= 10;
	}
	for ( i = 0; i < ( e2 >= 0 ? e2 : -e2 ); i++ ) {
		INT	k, carry = 0, mul = ( e2 >= 0 ) ? 2 : 5;

		for ( k = 0; k < nd; k++ ) {
			INT	t = dg[k] * mul + carry;

			dg[k] = (UB)( t % 10 );
			carry = t / 10;
		}
		while ( carry > 0 && nd < 1199 ) {
			dg[nd++] = (UB)( carry % 10 );
			carry /= 10;
		}
	}
	point = ( e2 >= 0 ) ? 0 : -e2;		/* digits after the point, from the low end */
	while ( nd <= point ) dg[nd++] = 0;	/* a 0 before the point at least */
	/* to 15 significant figures, the half up */
	for ( first = nd - 1; first > 0 && dg[first] == 0; first-- ) ;
	keep = first - 14;
	if ( keep > 0 ) {
		BOOL	up = (BOOL)( dg[keep - 1] >= 5 );

		for ( i = 0; i < keep; i++ ) dg[i] = 0;
		for ( i = keep; up && i < nd; i++ ) {
			if ( dg[i] == 9 ) {
				dg[i] = 0;
			} else {
				dg[i]++;
				up = FALSE;
			}
		}
		if ( up && nd < 1200 ) dg[nd++] = 1;
	}
	/* the whole part, then the fraction without its trailing zeros */
	{
		INT	top = nd - 1, low = 0;

		while ( top > point && dg[top] == 0 ) top--;
		while ( low < point && dg[low] == 0 ) low++;
		if ( neg && o < max - 1 ) out[o++] = '-';
		for ( i = top; i >= point && o < max - 1; i-- ) out[o++] = (UB)( '0' + dg[i] );
		if ( low < point ) {
			if ( o < max - 1 ) out[o++] = '.';
			for ( i = point - 1; i >= low && o < max - 1; i-- ) out[o++] = (UB)( '0' + dg[i] );
		}
		out[o] = 0;
	}
	mc_free(dg);
	if ( o == 2 && out[0] == '-' && out[1] == '0' ) {
		out[0] = '0';
		out[1] = 0;
		o = 1;
	}
	return o;
}

/* ---------------------------------------------------------------- links */

/* The object a link points at, as the 36 letters of its UUID; FALSE when it has none */
LOCAL BOOL target_of( CONST UB *el, INT n, UB *out )
{
	UB	id[TS_LNK_PART * 2 + 64];
	INT	k = ct_attr(el, n, "id", id, sizeof(id)), i, at;

	at = ( k > 0 ) ? ts_lnk_split(id, k, NULL, NULL) : 0;	/* past a volume it names */
	if ( k - at < 36 ) return FALSE;
	for ( i = 0; i < 36; i++ ) out[i] = id[at + i];
	out[36] = 0;
	return TRUE;
}

/* The objects the links point at, each once: the 印刷書式's first, then the cards' in order */
typedef struct {
	UB		(*t)[40];
	CONST UB	**el;		/* the first link to each, as it is */
	INT		*len;
	INT		n, max;
} LINKS;

LOCAL INT link_no( LINKS *l, CONST UB *el, INT len, BOOL add )
{
	UB	t[40];
	INT	i, k;

	if ( !target_of(el, len, t) ) return -1;
	for ( i = 0; i < l->n; i++ ) {
		for ( k = 0; k < 36 && l->t[i][k] == t[k]; k++ ) ;
		if ( k == 36 ) return i;
	}
	if ( !add || l->n >= l->max ) return -1;
	for ( k = 0; k <= 36; k++ ) l->t[l->n][k] = t[k];
	l->el[l->n] = el;
	l->len[l->n] = len;
	return l->n++;
}

LOCAL BOOL links_make( CONST MCDB *db, BOOL found_only, LINKS *l )
{
	INT	i, f, max = mc_link_count(db) + 1;

	l->n = 0;
	l->max = max;
	l->t = (UB (*)[40])mc_alloc(40 * max);
	l->el = (CONST UB **)mc_alloc((INT)sizeof(UB *) * max);
	l->len = (INT *)mc_alloc((INT)sizeof(INT) * max);
	if ( l->t == NULL || l->el == NULL || l->len == NULL ) {
		mc_free(l->t);
		mc_free(l->el);
		mc_free(l->len);
		l->t = NULL;
		return FALSE;
	}
	if ( db->layout != NULL ) (void)link_no(l, db->layout, db->layoutlen, TRUE);
	for ( i = 0; i < db->n; i++ ) {
		if ( found_only && db->searching && !db->c[i]->found ) continue;
		for ( f = 0; f < db->nf; f++ ) {
			CONST UB	*v = mc_get(db->c[i], f);

			if ( db->f[f].type == MC_T_LINK && v != NULL ) (void)link_no(l, v, s_len(v), TRUE);
		}
	}
	return TRUE;
}

LOCAL void links_free( LINKS *l )
{
	mc_free(l->t);
	mc_free(l->el);
	mc_free(l->len);
}

/* "#rrggbb" of a COLOR; dflt when it is not one of red, green and blue */
LOCAL void colour_text( UINT raw, CONST char *dflt, char *out )
{
	static CONST char	hx[] = "0123456789abcdef";
	INT			i;

	if ( ( raw >> 28 ) != 1 ) {
		for ( i = 0; dflt[i] != 0; i++ ) out[i] = dflt[i];
		out[i] = 0;
		return;
	}
	out[0] = '#';
	for ( i = 0; i < 6; i++ ) out[1 + i] = hx[( raw >> ( 20 - 4 * i ) ) & 0xF];
	out[7] = 0;
}

LOCAL UINT colour_raw( CONST UB *el, INT n, CONST char *name, UINT dflt )
{
	UB	v[16];
	INT	i, h;
	UINT	c = 0;

	if ( ct_attr(el, n, name, v, sizeof(v)) != 7 || v[0] != '#' ) return dflt;
	for ( i = 1; i < 7; i++ ) {
		UB	x = v[i];

		h = ( x >= '0' && x <= '9' ) ? x - '0' : ( x >= 'a' && x <= 'f' ) ? x - 'a' + 10
		  : ( x >= 'A' && x <= 'F' ) ? x - 'A' + 10 : -1;
		if ( h < 0 ) return dflt;
		c = ( c << 4 ) | (UINT)h;
	}
	return 0x10000000U | c;
}

LOCAL void put_attr_num( CTBUF *b, CONST char *name, INT v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_num(b, v);
	ct_put(b, "\"");
}

LOCAL void put_attr_str( CTBUF *b, CONST char *name, CONST char *v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_put(b, v);
	ct_put(b, "\"");
}

/* A CHSIZE in points: in 1/20 point, 1/20 Q, dots, or the units of where it is (points here) */
LOCAL INT chsize_pt( UINT v )
{
	UINT	n = v & 0x3FFF;

	switch ( v & 0xC000 ) {
	case 0x8000:	return (INT)( ( n + 10 ) / 20 );
	case 0x4000:	return (INT)( ( n * 709 + 10000 ) / 20000 );
	case 0xC000:	return (INT)( ( n * 3 + 2 ) / 4 );
	default:	return (INT)n;
	}
}

/* A <link> made from a VDATA (its link attributes and 仮身セグメント) and its object */
LOCAL UB *link_from( CONST UB *v, INT vn, CONST UB *target, INT *p_len )
{
	UB	*mem;
	CTBUF	b;
	char	col[4][8], vid[40];
	INT	l, t, r, bt, chsz, k;
	UINT	attr = get16(v + 4);
	static CONST char *CONST cn[4] = { "frcol", "chcol", "tbcol", "bgcol" };
	static CONST char *CONST cd[4] = { "#000000", "#000000", "#ffffff", "#ffffff" };
	CONST UB *s = v + VDATA_HEAD;

	if ( vn < VDATA_HEAD + VSEG_SIZE ) return NULL;
	l = (INT)(H)get16(s);
	t = (INT)(H)get16(s + 2);
	r = (INT)(H)get16(s + 4);
	bt = (INT)(H)get16(s + 6);
	chsz = chsize_pt(get16(s + 10));
	for ( k = 0; k < 4; k++ ) {
		UINT	raw = get16(s + 12 + 4 * k) | ( get16(s + 14 + 4 * k) << 16 );

		colour_text(raw, cd[k], col[k]);
	}
	mc_new_vobjid(vid);
	mem = (UB *)mc_alloc(1024);
	if ( mem == NULL ) return NULL;
	ct_buf(&b, mem, 1024);
	ct_put(&b, "<link id=\"");
	ct_put(&b, (CONST char *)target);
	ct_put(&b, "_0.xtad\"");
	put_attr_str(&b, "vobjid", vid);
	put_attr_num(&b, "vobjleft", l);
	put_attr_num(&b, "vobjtop", t);
	put_attr_num(&b, "vobjright", r);
	put_attr_num(&b, "vobjbottom", bt);
	put_attr_num(&b, "width", r - l);
	put_attr_num(&b, "height", bt - t);
	put_attr_num(&b, "chsz", ( chsz > 0 ) ? chsz : 14);
	for ( k = 0; k < 4; k++ ) put_attr_str(&b, cn[k], col[k]);
	put_attr_str(&b, "dlen", "0");
	put_attr_str(&b, "namedisp", ( attr & V_NONAME ) ? "false" : "true");
	put_attr_str(&b, "roledisp", ( attr & V_NORELN ) ? "false" : "true");
	put_attr_str(&b, "typedisp", ( attr & V_NOTYPE ) ? "false" : "true");
	put_attr_str(&b, "updatedisp", ( attr & V_NOTIME ) ? "false" : "true");
	put_attr_str(&b, "pictdisp", ( attr & V_NOPICT ) ? "false" : "true");
	put_attr_str(&b, "framedisp", ( attr & V_NOFDISP ) ? "false" : "true");
	put_attr_str(&b, "autoopen", ( attr & V_AUTEXE ) ? "true" : "false");
	ct_put(&b, " hidden=\"false\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>");
	if ( b.fail ) {
		mc_free(mem);
		return NULL;
	}
	mem[b.n] = 0;
	*p_len = b.n;
	return mem;
}

/* A VDATA of a link: its number, its link record's, and its look */
LOCAL void vdata_put( CTBUF *b, UINT vobjno, UINT linkno, CONST UB *el, INT n )
{
	INT	v[4], h, chsz, k;
	UINT	attr = 0;
	static CONST char *CONST cn[4] = { "frcol", "chcol", "tbcol", "bgcol" };
	static CONST UINT cd[4] = { COL_BLACK, COL_BLACK, COL_WHITE, COL_WHITE };

	v[0] = ct_attr_int(el, n, "vobjleft", 0);
	v[1] = ct_attr_int(el, n, "vobjtop", 0);
	v[2] = ct_attr_int(el, n, "vobjright", v[0] + ct_attr_int(el, n, "width", 150));
	chsz = ct_attr_int(el, n, "chsz", 0);
	/* with no height of its own, closed: the height its letters give it */
	v[3] = ct_attr_int(el, n, "vobjbottom", v[1] + ct_attr_int(el, n, "height",
			   VOBJ_CLOSED_H(chsz, ct_attr_bool(el, n, "framedisp", TRUE),
					 ct_attr_bool(el, n, "pictdisp", TRUE))));
	h = v[3] - v[1];
	if ( !ct_attr_bool(el, n, "namedisp", TRUE) ) attr |= V_NONAME;
	if ( !ct_attr_bool(el, n, "roledisp", FALSE) ) attr |= V_NORELN;
	if ( !ct_attr_bool(el, n, "typedisp", FALSE) ) attr |= V_NOTYPE;
	if ( !ct_attr_bool(el, n, "updatedisp", FALSE) ) attr |= V_NOTIME;
	if ( !ct_attr_bool(el, n, "pictdisp", TRUE) ) attr |= V_NOPICT;
	if ( !ct_attr_bool(el, n, "framedisp", TRUE) ) attr |= V_NOFDISP;
	if ( ct_attr_bool(el, n, "autoopen", FALSE) ) attr |= V_AUTEXE;
	put16(b, vobjno);
	put16(b, linkno);
	put16(b, attr);
	zeros(b, 8);
	for ( k = 0; k < 4; k++ ) put16(b, (UINT)( v[k] & 0xFFFF ));
	put16(b, (UINT)( h & 0xFFFF ));
	put16(b, ( chsz > 0 ) ? ( 0x8000U | (UINT)( chsz * 20 ) ) : 0);	/* in 1/20 point */
	for ( k = 0; k < 4; k++ ) put32(b, colour_raw(el, n, cn[k], cd[k]));
	put16(b, 0);				/* dlen: the object's own data is not kept */
}

/* ---------------------------------------------------------------- reading */

LOCAL CONST char *CONST face_name[3] = { "standard", "mincho", "gothic" };
LOCAL CONST char *CONST paper_name[MC_PAPERS] = { "a4", "a3", "a5", "b4", "b5", "letter" };

LOCAL INT name_index( CONST UB *v, CONST char *CONST *names, INT n, INT dflt )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( s_eq(v, names[i]) ) return i;
	}
	return dflt;
}

LOCAL void read_print( MCPRINT *p, CONST UB *el, INT len )
{
	UB	v[MC_LAYOUT_NAME];
	static CONST char *CONST side[4] = { "top", "bottom", "left", "right" };
	INT	k;

	if ( ct_attr_text(el, len, "layout", v, sizeof(v)) < 0 ) v[0] = 0;
	mc_print_default(p, v);
	p->across = ct_attr_int(el, len, "across", p->across);
	p->down = ct_attr_int(el, len, "down", p->down);
	p->gapx = ct_attr_int(el, len, "gapx", 0);
	p->gapy = ct_attr_int(el, len, "gapy", 0);
	if ( ct_attr(el, len, "order", v, sizeof(v)) > 0 ) p->downfirst = s_eq(v, "down");
	if ( ct_attr(el, len, "paper", v, sizeof(v)) > 0 ) p->paper = name_index(v, paper_name, MC_PAPERS, MC_PAPER_A4);
	p->turn = ct_attr_bool(el, len, "turn", FALSE);
	for ( k = 0; k < 4; k++ ) p->margin[k] = ct_attr_int(el, len, side[k], 20);
	(void)ct_attr_text(el, len, "head", p->head, sizeof(p->head));
	(void)ct_attr_text(el, len, "foot", p->foot, sizeof(p->foot));
	if ( p->across < 1 ) p->across = 1;
	if ( p->across > 20 ) p->across = 20;
	if ( p->down < 0 ) p->down = 0;
	if ( p->down > 50 ) p->down = 50;
}

/*
 * Record 0: TessronOS's settings in <microcard>, and the <link>s after
 * <mclinks/> -- the objects of the link records, in order -- as the 36
 * letters of their UUIDs in t[] (at most max); how many.
 */
LOCAL INT read_x( MCDB *db, CONST UB *x, INT n, UB (*t)[40], INT max, INT *face )
{
	INT	at, end, i, nl = 0;

	if ( x == NULL ) return 0;
	at = find(x, n, 0, "<microcard");
	if ( at >= 0 ) {
		CONST UB	*el = x + at;
		INT		len;

		end = tag_end(x, n, at);
		len = end - at;
		db->view.csize = ct_attr_int(el, len, "cardsize", 10);
		db->view.lsize = ct_attr_int(el, len, "listsize", 10);
		db->view.cprop = ct_attr_bool(el, len, "cardprop", TRUE);
		db->view.lprop = ct_attr_bool(el, len, "listprop", TRUE);
		db->view.cgray = ct_attr_bool(el, len, "cardgray", FALSE);
		db->view.lgray = ct_attr_bool(el, len, "listgray", FALSE);
		db->view.names = ct_attr_bool(el, len, "names", TRUE);
		if ( db->view.csize < 5 || db->view.csize > 48 ) db->view.csize = 10;
		if ( db->view.lsize < 5 || db->view.lsize > 48 ) db->view.lsize = 10;
		at = find(x, n, end, "</microcard>");
		if ( at < 0 ) at = end;
		for ( i = end; i < at; ) {
			INT	e;

			if ( x[i] != '<' ) {
				i++;
				continue;
			}
			e = tag_end(x, at, i);
			if ( at_is(x + i, at - i, "<field") ) {
				INT	k = ct_attr_int(x + i, e - i, "n", -1);
				UB	v[16];

				if ( k >= 0 && k < MC_FIELD_MAX && ct_attr(x + i, e - i, "face", v, sizeof(v)) > 0 ) {
					face[k] = name_index(v, face_name, 3, MC_FACE_MINCHO);
				}
			} else if ( at_is(x + i, at - i, "<print") && db->npr < MC_PRINT_MAX ) {
				read_print(&db->pr[db->npr++], x + i, e - i);
			}
			i = e;
		}
	}
	at = find(x, n, 0, "<mclinks");
	if ( at < 0 ) return 0;
	end = find(x, n, at, "</p>");
	if ( end < 0 ) end = n;
	for ( i = tag_end(x, end, at); i < end && nl < max; ) {
		INT	e;

		if ( !at_is(x + i, end - i, "<link") ) {
			i++;
			continue;
		}
		e = link_end(x, end, i);
		if ( !target_of(x + i, e - i, t[nl]) ) t[nl][0] = 0;
		nl++;
		i = e;
	}
	return nl;
}

/* The VDATA of vobjnumber no in the link record, and its length; NULL none */
LOCAL CONST UB *vdata_of( CONST MCRECS *rs, UINT no, INT *p_len )
{
	CONST UB	*v = rs->r[MC_SUB_VOBJ];
	INT		n = rs->n[MC_SUB_VOBJ], at = 0;

	while ( v != NULL && at + VDATA_HEAD + VSEG_SIZE <= n ) {
		INT	len = VDATA_HEAD + VSEG_SIZE + (INT)get16(v + at + VDATA_HEAD + 28);

		if ( at + len > n ) break;
		if ( get16(v + at) == no ) {
			*p_len = len;
			return v + at;
		}
		at += len;
	}
	return NULL;
}

/* The link of vobjnumber no, made from the records; NULL when it has no object */
LOCAL UB *link_of( CONST MCRECS *rs, UINT no, UB (*t)[40], INT nt, INT *p_len )
{
	INT		len = 0;
	CONST UB	*v = vdata_of(rs, no, &len);
	UINT		ln;

	if ( v == NULL ) return NULL;
	ln = get16(v + 2);
	if ( (INT)ln >= nt || t[ln][0] == 0 ) return NULL;
	return link_from(v, len, t[ln], p_len);
}

EXPORT ER mc_read( MCDB *db, CONST MCRECS *rs )
{
	CONST UB	*fm = rs->r[MC_SUB_FORMAT], *cd = rs->r[MC_SUB_CARDS], *ix = rs->r[MC_SUB_INDEX];
	INT		fn = rs->n[MC_SUB_FORMAT], ncards, datalen, nitems, k, i, nt;
	INT		off[MC_FIELD_MAX], face[MC_FIELD_MAX];
	UB		(*t)[40];
	UB		*buf;
	ER		er = E_OK;

	mc_clear(db);
	if ( fm == NULL || fn < FMT_HEAD || get16(fm) != TC_C || get16(fm + 2) != TC_R || get16(fm + 4) != TC_D
	  || get16(fm + 6) != FMT_VERSION ) {
		(void)mc_card_new(db, 0);
		return E_PAR;
	}
	ncards = (INT)get16(fm + 8);
	datalen = (INT)get16(fm + 10);
	nitems = (INT)get16(fm + 30);
	if ( nitems > MC_FIELD_MAX || fn < FMT_HEAD + IFMT_SIZE * nitems || ncards > MC_CARD_MAX ) {
		(void)mc_card_new(db, 0);
		return E_PAR;
	}
	for ( k = 0; k < MC_FIELD_MAX; k++ ) face[k] = -1;
	t = (UB (*)[40])mc_alloc(40 * ( MC_LINK_MAX + 2 ));
	buf = (UB *)mc_alloc(MC_VALUE_MAX * 2 + 8);
	if ( t == NULL || buf == NULL ) {
		mc_free(t);
		mc_free(buf);
		return E_NOMEM;
	}
	nt = read_x(db, rs->x, rs->xn, t, MC_LINK_MAX + 2, face);

	/* the format */
	for ( k = 0; k < nitems; k++ ) {
		CONST UB	*p = fm + FMT_HEAD + IFMT_SIZE * k;
		MCFIELD		*f = &db->f[k];
		UINT		kind = get16(p + 18);

		(void)from_tron(p, 8, f->name, MC_NAME_MAX);
		off[k] = (INT)get16(p + 16);
		f->type = ( kind == K_NUM ) ? MC_T_NUM : ( kind == K_LINK ) ? MC_T_LINK : MC_T_TEXT;
		f->max = (INT)get16(p + 20);
		f->width = (INT)get16(p + 22);
		if ( f->type == MC_T_TEXT && ( f->max < 1 || f->max > MC_TEXT_MAX ) ) f->max = MC_TEXT_MAX;
		if ( f->type == MC_T_NUM && ( f->max < 1 || f->max > MC_NUM_MAX ) ) f->max = MC_NUM_MAX;
		if ( f->type == MC_T_LINK && ( f->max < 1 || f->max > MC_LINK_LETTERS ) ) f->max = MC_LINK_LETTERS;
		if ( f->width > MC_WIDTH_MAX ) f->width = MC_WIDTH_MAX;
		f->face = ( face[k] >= 0 ) ? face[k] : ( f->type == MC_T_TEXT ) ? MC_FACE_MINCHO : MC_FACE_STD;
	}
	db->nf = nitems;

	/* the 印刷書式 */
	{
		INT	len = 0;
		UB	*el = link_of(rs, VOBJ_LAYOUT, t, nt, &len);

		if ( el == NULL ) el = link_of(rs, VOBJ_LAYOUT + 1, t, nt, &len);
		if ( el != NULL ) {
			db->layout = el;
			db->layoutlen = len;
		}
	}

	/* the cards, in the order the order record says, or as they are */
	if ( cd == NULL ) ncards = 0;
	if ( ncards > 0 && ( datalen < CARD_HEAD || rs->n[MC_SUB_CARDS] < datalen * ncards ) ) er = E_OBJ;
	for ( i = 0; i < ncards && er >= E_OK; i++ ) {
		INT		at = i;
		CONST UB	*c;
		MCCARD		*mc;

		if ( ix != NULL && rs->n[MC_SUB_INDEX] >= 2 * ncards ) {
			at = (INT)( get16(ix + 2 * i) & 0x7FFF );
			if ( at >= ncards ) at = i;
		}
		c = cd + datalen * at;
		if ( get16(c) != START_MARK ) {
			er = E_OBJ;
			break;
		}
		mc = mc_card_new(db, db->n);
		if ( mc == NULL ) {
			er = E_NOMEM;
			break;
		}
		for ( k = 0; k < nitems; k++ ) {
			CONST UB	*p = c + off[k];
			CONST MCFIELD	*f = &db->f[k];
			INT		len;

			if ( off[k] < CARD_HEAD ) continue;
			if ( f->type == MC_T_NUM ) {
				if ( off[k] + 8 > datalen ) continue;
				len = mc_bits_num(p, buf, MC_NUM_MAX * 2);
				if ( len > 0 ) (void)mc_set(db, mc, k, buf, len);
			} else if ( f->type == MC_T_LINK ) {
				UINT	no;
				UB	*el;

				if ( off[k] + 2 > datalen ) continue;
				no = get16(p);
				el = ( no != 0 ) ? link_of(rs, no, t, nt, &len) : NULL;
				if ( el != NULL ) {
					(void)mc_set(db, mc, k, el, len);
					mc_free(el);
				}
			} else {
				INT	words = f->max + 1;

				if ( off[k] + 2 * words > datalen ) words = ( datalen - off[k] ) / 2;
				if ( words <= 0 ) continue;
				len = from_tron(p, words, buf, MC_VALUE_MAX * 2);
				if ( len > 0 ) (void)mc_set(db, mc, k, buf, len);
			}
		}
	}
	mc_free(t);
	mc_free(buf);
	if ( db->n == 0 && mc_card_new(db, 0) == NULL ) return E_NOMEM;
	return er;
}

/* ---------------------------------------------------------------- writing */

/* The bytes of an item of a field */
LOCAL INT item_size( CONST MCFIELD *f )
{
	if ( f->type == MC_T_NUM ) return 8;
	if ( f->type == MC_T_LINK ) return LINK_ITEM;
	return 2 * ( f->max + 1 );
}

LOCAL INT cards_out( CONST MCDB *db, BOOL found_only )
{
	INT	i, n = 0;

	for ( i = 0; i < db->n; i++ ) {
		if ( !found_only || !db->searching || db->c[i]->found ) n++;
	}
	return n;
}

LOCAL void put_tron( CTBUF *b, CONST UB *s, INT words, INT room )
{
	UH	*t = (UH *)mc_alloc((INT)sizeof(UH) * ( words + 1 ));
	INT	k, i;

	if ( t == NULL ) {
		zeros(b, 2 * words);
		return;
	}
	k = to_tron(s, s_len(s), t, room);
	for ( i = 0; i < words; i++ ) put16(b, ( i < k ) ? t[i] : TC_NULL);
	mc_free(t);
}

EXPORT INT mc_write_bin( CTBUF *b, CONST MCDB *db, INT sub, BOOL found_only )
{
	INT	i, k, datalen = CARD_HEAD, start = b->n, ncards = cards_out(db, found_only);
	LINKS	l;

	for ( k = 0; k < db->nf; k++ ) datalen += item_size(&db->f[k]);
	switch ( sub ) {
	case MC_SUB_FORMAT:
		put16(b, TC_C);
		put16(b, TC_R);
		put16(b, TC_D);
		put16(b, FMT_VERSION);
		put16(b, (UINT)ncards);
		put16(b, (UINT)datalen);
		zeros(b, 18);
		put16(b, (UINT)db->nf);
		for ( k = 0, i = CARD_HEAD; k < db->nf; k++ ) {
			CONST MCFIELD	*f = &db->f[k];

			put_tron(b, f->name, 8, 8);		/* a name of eight letters has no TNULL */
			put16(b, (UINT)i);
			put16(b, ( f->type == MC_T_NUM ) ? K_NUM : ( f->type == MC_T_LINK ) ? K_LINK : K_STR);
			put16(b, (UINT)f->max);
			put16(b, (UINT)( f->width <= MC_WIDTH_MAX ? f->width : MC_WIDTH_MAX ));
			i += item_size(f);
		}
		break;
	case MC_SUB_CARDS:
	case MC_SUB_VOBJ:
		if ( !links_make(db, found_only, &l) ) {
			b->fail = TRUE;
			return 0;
		}
		if ( sub == MC_SUB_VOBJ && db->layout != NULL ) {
			vdata_put(b, VOBJ_LAYOUT, (UINT)link_no(&l, db->layout, db->layoutlen, FALSE), db->layout, db->layoutlen);
		}
		{
			UINT	vno = VOBJ_FIRST;

			for ( i = 0; i < db->n; i++ ) {
				CONST MCCARD	*c = db->c[i];

				if ( found_only && db->searching && !c->found ) continue;
				if ( sub == MC_SUB_CARDS ) {
					put16(b, START_MARK);
					put16(b, (UINT)datalen);
				}
				for ( k = 0; k < db->nf; k++ ) {
					CONST MCFIELD	*f = &db->f[k];
					CONST UB	*v = mc_get(c, k);

					if ( f->type == MC_T_LINK ) {
						INT	ln = ( v != NULL ) ? link_no(&l, v, s_len(v), FALSE) : -1;

						if ( sub == MC_SUB_CARDS ) {
							put16(b, ( ln >= 0 ) ? vno : 0);
							zeros(b, LINK_ITEM - 2);
						} else if ( ln >= 0 ) {
							vdata_put(b, vno, (UINT)ln, v, s_len(v));
						}
						if ( ln >= 0 ) vno++;
					} else if ( sub != MC_SUB_CARDS ) {
						continue;
					} else if ( f->type == MC_T_NUM ) {
						UB	bits[8];

						mc_num_bits(v, bits);
						ct_putn(b, bits, 8);
					} else {
						put_tron(b, ( v != NULL ) ? v : (CONST UB *)"", f->max + 1, f->max);
					}
				}
			}
		}
		links_free(&l);
		break;
	default:
		/* the cards are written in their order: no order record */
		break;
	}
	return b->n - start;
}

LOCAL void put_bool( CTBUF *b, CONST char *name, BOOL v )
{
	put_attr_str(b, name, v ? "true" : "false");
}

LOCAL void put_esc( CTBUF *b, CONST char *name, CONST UB *v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_esc(b, v, s_len(v));
	ct_put(b, "\"");
}

EXPORT void mc_write_x( CTBUF *b, CONST MCDB *db, CONST UB *name, BOOL found_only )
{
	INT	k, n = cards_out(db, found_only);
	LINKS	l;

	ct_put(b, "<tad version=\"1.0\" encoding=\"UTF-8\"");
	if ( name != NULL && name[0] != 0 ) put_esc(b, "filename", name);
	ct_put(b, ">\n<document>\n<microcard version=\"1\"");
	put_attr_num(b, "cardsize", db->view.csize);
	put_bool(b, "cardprop", db->view.cprop);
	put_bool(b, "cardgray", db->view.cgray);
	put_attr_num(b, "listsize", db->view.lsize);
	put_bool(b, "listprop", db->view.lprop);
	put_bool(b, "listgray", db->view.lgray);
	put_bool(b, "names", db->view.names);
	ct_put(b, ">\n");
	for ( k = 0; k < db->nf; k++ ) {
		if ( db->f[k].type != MC_T_TEXT ) continue;
		ct_put(b, "<field");
		put_attr_num(b, "n", k);
		put_attr_str(b, "face", face_name[db->f[k].face]);
		ct_put(b, "/>\n");
	}
	for ( k = 0; k < db->npr; k++ ) {
		CONST MCPRINT	*p = &db->pr[k];

		ct_put(b, "<print");
		put_esc(b, "layout", p->layout);
		put_attr_num(b, "across", p->across);
		put_attr_num(b, "down", p->down);
		put_attr_num(b, "gapx", p->gapx);
		put_attr_num(b, "gapy", p->gapy);
		put_attr_str(b, "order", p->downfirst ? "down" : "across");
		put_attr_str(b, "paper", paper_name[( p->paper >= 0 && p->paper < MC_PAPERS ) ? p->paper : 0]);
		put_bool(b, "turn", p->turn);
		put_attr_num(b, "top", p->margin[0]);
		put_attr_num(b, "bottom", p->margin[1]);
		put_attr_num(b, "left", p->margin[2]);
		put_attr_num(b, "right", p->margin[3]);
		put_esc(b, "head", p->head);
		put_esc(b, "foot", p->foot);
		ct_put(b, "/>\n");
	}
	ct_put(b, "</microcard>\n<p>マイクロカード");
	if ( name != NULL && name[0] != 0 ) {
		ct_put(b, "「");
		ct_esc(b, name, s_len(name));
		ct_put(b, "」");
	}
	ct_put(b, "　カード");
	ct_num(b, n);
	ct_put(b, "枚、項目");
	ct_num(b, db->nf);
	ct_put(b, "個</p>\n<p>項目：");
	for ( k = 0; k < db->nf; k++ ) {
		if ( k > 0 ) ct_put(b, "、");
		ct_esc(b, db->f[k].name, s_len(db->f[k].name));
	}
	ct_put(b, "</p>\n");
	/* the objects the links point at: the link records, in order */
	if ( links_make(db, found_only, &l) ) {
		if ( l.n > 0 ) {
			ct_put(b, "<p><mclinks/>");
			for ( k = 0; k < l.n; k++ ) ct_putn(b, l.el[k], l.len[k]);
			ct_put(b, "</p>\n");
		}
		links_free(&l);
	} else {
		b->fail = TRUE;
	}
	ct_put(b, "</document>\n</tad>\n");
}
