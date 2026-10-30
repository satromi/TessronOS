/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pdf_font.c
 *	The faces a PDF file carries, cut down to the letters it uses
 *	(design 11.22.4)
 *
 *	A face is put in the file so that the pages look the same wherever
 *	they are opened and so that their words can be found and copied.
 *	A face for Japanese is large -- the system's is four and a half
 *	megabytes -- and a page uses a few hundred of its seventeen
 *	thousand glyphs, so only those are kept.
 *
 *	The page names each glyph by its number in the whole face (the
 *	encoding Identity-H, two bytes a glyph), and the file's table of
 *	letters (ToUnicode) says which letter each glyph was drawn for,
 *	which is what finding and copying the words go by. The page is
 *	written before it is known which glyphs the whole file uses, so
 *	those numbers have to keep meaning the same glyphs in the face cut
 *	down:
 *
 *	- A face of PostScript outlines keyed by CID (CFF with a ROS, as
 *	  the system's face is) keeps only the glyphs used. The face's own
 *	  list that ties its glyphs to CIDs (the charset) is written again
 *	  so that each kept glyph has its old number as its CID, which is
 *	  what the page and the widths name it by; the face is of the
 *	  Adobe-Identity-0 kind, where a CID means nothing else.
 *	- Any other face keeps every glyph in its place and makes those
 *	  not used empty, a byte each (for TrueType, nothing in 'glyf').
 *
 *	A face of PostScript outlines shares pieces of outline between
 *	glyphs as subroutines. Those the kept glyphs call are kept and the
 *	rest made empty in place, so that a call keeps its number. Which
 *	are called is found by walking each kept glyph's program with its
 *	stack of numbers, far enough to know the number a call is made
 *	with and how many bytes the masks of its hints take. A program
 *	that does arithmetic on the stack is not followed; then every
 *	subroutine is kept, which is larger and still right.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "pdf.h"

/* ---------------------------------------------------------------- reading */

LOCAL UW be16( CONST UB *p )
{
	return ( (UW)p[0] << 8 ) | p[1];
}

LOCAL UW be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

LOCAL INT sbe16( CONST UB *p )
{
	return (INT)(H)be16(p);
}

LOCAL void put16( UB *p, UW v )
{
	p[0] = (UB)( v >> 8 );
	p[1] = (UB)v;
}

LOCAL void put32( UB *p, UW v )
{
	p[0] = (UB)( v >> 24 );
	p[1] = (UB)( v >> 16 );
	p[2] = (UB)( v >> 8 );
	p[3] = (UB)v;
}

/* A whole table, in memory that is the caller's to Kfree */
LOCAL UB *table_all( PDFFACE *ff, UW tag, SZ *p_len )
{
	SZ	len = 0;
	UB	*b;

	if ( ff->table(ff->arg, tag, NULL, &len) < E_OK || len <= 0 ) {
		return NULL;
	}
	b = (UB *)Kmalloc(len + 4);
	if ( b == NULL ) {
		return NULL;
	}
	if ( ff->table(ff->arg, tag, b, &len) < E_OK ) {
		Kfree(b);
		return NULL;
	}
	*p_len = len;

	return b;
}

/* ---------------------------------------------------------------- the face */

/* The PostScript name out of the table of names, letters and digits only */
LOCAL void ps_name( PDFFACE *ff )
{
	SZ	len = 0;
	UB	*t = table_all(ff, PDF_TAG('n', 'a', 'm', 'e'), &len);
	INT	k = 0;

	ff->psname[0] = 0;
	if ( t != NULL && len >= 6 ) {
		UW	n = be16(t + 2), so = be16(t + 4), i;

		for ( i = 0; i < n && 6 + i * 12 + 12 <= (UW)len && k == 0; i++ ) {
			CONST UB	*r = t + 6 + i * 12;
			UW		pid = be16(r), nid = be16(r + 6);
			UW		l = be16(r + 8), o = be16(r + 10), j;
			BOOL		wide = (BOOL)( pid == 0 || pid == 3 );

			if ( nid != 6 || so + o + l > (UW)len ) {
				continue;
			}
			for ( j = 0; j < l && k < 60; j += wide ? 2 : 1 ) {
				UW	c = wide ? be16(t + so + o + j) : t[so + o + j];

				if ( ( c >= '0' && c <= '9' ) || ( c >= 'A' && c <= 'Z' )
				  || ( c >= 'a' && c <= 'z' ) || c == '-' || c == '_' ) {
					ff->psname[k++] = (UB)c;
				}
			}
		}
	}
	if ( t != NULL ) {
		Kfree(t);
	}
	if ( k == 0 ) {
		CONST char	*d = "TessronFace";

		while ( d[k] != 0 ) {
			ff->psname[k] = (UB)d[k];
			k++;
		}
	}
	ff->psname[k] = 0;
}

EXPORT ER pdf_face_init( PDFFACE *ff, PDF_TABLE table, void *arg )
{
	UB	*head, *hhea, *maxp, *hmtx, *os2;
	SZ	lhead = 0, lhhea = 0, lmaxp = 0, lhmtx = 0, los2 = 0, l = 0;
	INT	nhm, i;

	knl_memset(ff, 0, sizeof(*ff));
	ff->table = table;
	ff->arg = arg;
	head = table_all(ff, PDF_TAG('h', 'e', 'a', 'd'), &lhead);
	hhea = table_all(ff, PDF_TAG('h', 'h', 'e', 'a'), &lhhea);
	maxp = table_all(ff, PDF_TAG('m', 'a', 'x', 'p'), &lmaxp);
	hmtx = table_all(ff, PDF_TAG('h', 'm', 't', 'x'), &lhmtx);
	os2 = table_all(ff, PDF_TAG('O', 'S', '/', '2'), &los2);
	if ( head == NULL || hhea == NULL || maxp == NULL || hmtx == NULL
	  || lhead < 54 || lhhea < 36 || lmaxp < 6 ) {
		if ( head != NULL ) Kfree(head);
		if ( hhea != NULL ) Kfree(hhea);
		if ( maxp != NULL ) Kfree(maxp);
		if ( hmtx != NULL ) Kfree(hmtx);
		if ( os2 != NULL ) Kfree(os2);
		return E_NOSPT;
	}
	ff->cff = (BOOL)( table(arg, PDF_TAG('C', 'F', 'F', ' '), NULL, &l) >= E_OK );
	ff->upem = (INT)be16(head + 18);
	if ( ff->upem <= 0 ) {
		ff->upem = 1000;
	}
	ff->bbox[0] = sbe16(head + 36) * 1000 / ff->upem;
	ff->bbox[1] = sbe16(head + 38) * 1000 / ff->upem;
	ff->bbox[2] = sbe16(head + 40) * 1000 / ff->upem;
	ff->bbox[3] = sbe16(head + 42) * 1000 / ff->upem;
	ff->ascent = sbe16(hhea + 4) * 1000 / ff->upem;
	ff->descent = sbe16(hhea + 6) * 1000 / ff->upem;
	ff->capheight = ff->ascent * 7 / 10;
	if ( os2 != NULL && los2 >= 90 && be16(os2) >= 2 ) {
		ff->capheight = sbe16(os2 + 88) * 1000 / ff->upem;
	}
	ff->nglyph = (INT)be16(maxp + 4);
	nhm = (INT)be16(hhea + 34);
	if ( nhm < 1 || (SZ)nhm * 4 > lhmtx ) {
		nhm = (INT)( lhmtx / 4 );
	}
	ff->used = (UB *)Kmalloc((SZ)( ff->nglyph + 8 ) / 8);
	ff->uni = (UW *)Kmalloc(sizeof(UW) * (SZ)ff->nglyph);
	ff->adv = (UH *)Kmalloc(sizeof(UH) * (SZ)ff->nglyph);
	if ( ff->used != NULL && ff->uni != NULL && ff->adv != NULL ) {
		knl_memset(ff->used, 0, (SZ)( ff->nglyph + 8 ) / 8);
		knl_memset(ff->uni, 0, sizeof(UW) * (SZ)ff->nglyph);
		for ( i = 0; i < ff->nglyph; i++ ) {
			UW	a = ( nhm > 0 ) ? be16(hmtx + 4 * ( ( i < nhm ) ? i : nhm - 1 )) : (UW)ff->upem;

			ff->adv[i] = (UH)( a * 1000 / (UW)ff->upem );
		}
	}
	Kfree(head);
	Kfree(hhea);
	Kfree(maxp);
	Kfree(hmtx);
	if ( os2 != NULL ) Kfree(os2);
	if ( ff->used == NULL || ff->uni == NULL || ff->adv == NULL ) {
		pdf_face_free(ff);
		return E_NOMEM;
	}
	ps_name(ff);
	ff->used[0] |= 1;			/* .notdef is always there */

	return E_OK;
}

EXPORT void pdf_face_use( PDFFACE *ff, UINT gid, UW cp )
{
	if ( gid >= (UINT)ff->nglyph || ff->used == NULL ) {
		return;
	}
	ff->used[gid >> 3] |= (UB)( 1 << ( gid & 7 ) );
	if ( ff->uni[gid] == 0 ) {
		ff->uni[gid] = cp;
	}
}

EXPORT void pdf_face_free( PDFFACE *ff )
{
	if ( ff->used != NULL ) Kfree(ff->used);
	if ( ff->uni != NULL ) Kfree(ff->uni);
	if ( ff->adv != NULL ) Kfree(ff->adv);
	ff->used = NULL;
	ff->uni = NULL;
	ff->adv = NULL;
}

LOCAL BOOL is_used( CONST PDFFACE *ff, INT g )
{
	return (BOOL)( g >= 0 && g < ff->nglyph && ( ff->used[g >> 3] & ( 1 << ( g & 7 ) ) ) != 0 );
}

/* ---------------------------------------------------------------- CFF */

typedef struct {
	CONST UB	*b;		/* the whole table */
	SZ		blen;
	SZ		at, end;	/* the INDEX's first byte and the byte after it */
	INT		count;
	INT		offsize;
	SZ		base;		/* where offset 1 points */
} CIDX;

LOCAL UW off_at( CONST UB *p, INT sz )
{
	UW	v = 0;

	while ( sz-- > 0 ) {
		v = ( v << 8 ) | *p++;
	}
	return v;
}

LOCAL BOOL idx_read( CONST UB *b, SZ blen, SZ at, CIDX *x )
{
	x->b = b;
	x->blen = blen;
	x->at = at;
	if ( at + 2 > blen ) {
		return FALSE;
	}
	x->count = (INT)be16(b + at);
	if ( x->count == 0 ) {
		x->offsize = 0;
		x->base = at + 2;
		x->end = at + 2;
		return TRUE;
	}
	if ( at + 3 > blen ) {
		return FALSE;
	}
	x->offsize = b[at + 2];
	if ( x->offsize < 1 || x->offsize > 4 ) {
		return FALSE;
	}
	x->base = at + 3 + (SZ)( x->count + 1 ) * x->offsize - 1;
	if ( x->base + 1 > blen ) {
		return FALSE;
	}
	x->end = x->base + off_at(b + at + 3 + (SZ)x->count * x->offsize, x->offsize);

	return (BOOL)( x->end <= blen );
}

LOCAL CONST UB *idx_item( CONST CIDX *x, INT i, SZ *p_len )
{
	SZ	a, z;

	if ( i < 0 || i >= x->count ) {
		*p_len = 0;
		return NULL;
	}
	a = off_at(x->b + x->at + 3 + (SZ)i * x->offsize, x->offsize);
	z = off_at(x->b + x->at + 3 + (SZ)( i + 1 ) * x->offsize, x->offsize);
	if ( z < a || x->base + z > x->blen ) {
		*p_len = 0;
		return NULL;
	}
	*p_len = z - a;

	return x->b + x->base + a;
}

/* An INDEX written from items given by a function, into a buffer */
typedef CONST UB *(*ITEM_FN)( void *arg, INT i, SZ *p_len );

LOCAL void idx_write( PBUF *o, INT count, ITEM_FN item, void *arg )
{
	SZ	total = 0, len, at = 1;
	INT	i, osz;
	UB	t[4];

	put16(t, (UW)count);
	pb_bytes(o, t, 2);
	if ( count == 0 ) {
		return;
	}
	for ( i = 0; i < count; i++ ) {
		(void)item(arg, i, &len);
		total += len;
	}
	osz = ( total + 1 <= 0xFF ) ? 1 : ( total + 1 <= 0xFFFF ) ? 2
	    : ( total + 1 <= 0xFFFFFF ) ? 3 : 4;
	t[0] = (UB)osz;
	pb_bytes(o, t, 1);
	for ( i = 0; i <= count; i++ ) {
		INT	k;

		for ( k = 0; k < osz; k++ ) {
			t[k] = (UB)( at >> ( 8 * ( osz - 1 - k ) ) );
		}
		pb_bytes(o, t, osz);
		if ( i < count ) {
			(void)item(arg, i, &len);
			at += len;
		}
	}
	for ( i = 0; i < count; i++ ) {
		CONST UB	*p = item(arg, i, &len);

		pb_bytes(o, p, len);
	}
}

/* A DICT's entries: where each begins, how long, its operator and its first numbers */
#define DOP(a, b)	( ( (a) == 12 ) ? ( 1200 + (b) ) : (a) )
#define DICT_MAX	48

typedef struct {
	SZ	at, len;		/* the operands and the operator */
	INT	op;
	INT	nval;
	W	val[4];
} DENT;

LOCAL INT dict_read( CONST UB *d, SZ len, DENT *e, INT max )
{
	SZ	i = 0, start = 0;
	INT	n = 0, nval = 0;
	W	val[4];

	while ( i < len && n < max ) {
		UB	b = d[i];

		if ( b <= 21 ) {
			INT	op = b;

			i++;
			if ( b == 12 ) {
				if ( i >= len ) {
					break;
				}
				op = DOP(12, d[i]);
				i++;
			}
			e[n].at = start;
			e[n].len = i - start;
			e[n].op = op;
			e[n].nval = nval;
			for ( nval = 0; nval < 4; nval++ ) {
				e[n].val[nval] = val[nval];
			}
			n++;
			nval = 0;
			start = i;
			continue;
		}
		if ( b == 28 ) {
			if ( nval < 4 && i + 2 < len ) val[nval] = (W)(H)be16(d + i + 1);
			nval++;
			i += 3;
		} else if ( b == 29 ) {
			if ( nval < 4 && i + 4 < len ) val[nval] = (W)be32(d + i + 1);
			nval++;
			i += 5;
		} else if ( b == 30 ) {
			/* a real: nibbles until one of them is 0xF */
			i++;
			while ( i < len ) {
				UB	c = d[i++];

				if ( ( c & 0x0F ) == 0x0F || ( c & 0xF0 ) == 0xF0 ) {
					break;
				}
			}
			if ( nval < 4 ) val[nval] = 0;
			nval++;
		} else if ( b >= 32 && b <= 246 ) {
			if ( nval < 4 ) val[nval] = (W)b - 139;
			nval++;
			i++;
		} else if ( b >= 247 && b <= 250 ) {
			if ( nval < 4 && i + 1 < len ) val[nval] = ( (W)b - 247 ) * 256 + d[i + 1] + 108;
			nval++;
			i += 2;
		} else if ( b >= 251 && b <= 254 ) {
			if ( nval < 4 && i + 1 < len ) val[nval] = -( (W)b - 251 ) * 256 - d[i + 1] - 108;
			nval++;
			i += 2;
		} else {
			return -1;		/* not a DICT */
		}
	}

	return n;
}

LOCAL CONST DENT *dict_find( CONST DENT *e, INT n, INT op )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( e[i].op == op ) {
			return &e[i];
		}
	}
	return NULL;
}

/* An operand of five bytes, so that a DICT is the same size whatever the number */
LOCAL void dict_int5( PBUF *o, W v )
{
	UB	t[5];

	t[0] = 29;
	put32(t + 1, (UW)v);
	pb_bytes(o, t, 5);
}

LOCAL void dict_op( PBUF *o, INT op )
{
	UB	t[2];

	if ( op >= 1200 ) {
		t[0] = 12;
		t[1] = (UB)( op - 1200 );
		pb_bytes(o, t, 2);
	} else {
		t[0] = (UB)op;
		pb_bytes(o, t, 1);
	}
}

/* ---------------------------------------------------------------- the glyphs' programs */

#define CS_STACK	48
#define CS_DEPTH	10

typedef struct {
	CONST CIDX	*gsub;
	CONST CIDX	*lsub;		/* of the glyph's own font DICT, or NULL */
	UB		*gmark;		/* the global subroutines used */
	UB		*lmark;
	INT		gbias, lbias;
	W		stack[CS_STACK];
	INT		sp;
	INT		nstems;
	BOOL		seen_mask;
	BOOL		done;		/* endchar reached */
	BOOL		lost;		/* a program this cannot follow */
} CSWALK;

LOCAL INT subr_bias( INT count )
{
	return ( count < 1240 ) ? 107 : ( count < 33900 ) ? 1131 : 32768;
}

LOCAL void cs_walk( CSWALK *w, CONST UB *p, SZ len, INT depth );

LOCAL void cs_call( CSWALK *w, BOOL global, INT depth )
{
	CONST CIDX	*x = global ? w->gsub : w->lsub;
	UB		*mark = global ? w->gmark : w->lmark;
	INT		i;
	SZ		len;
	CONST UB	*p;

	if ( w->sp < 1 || x == NULL || mark == NULL || depth >= CS_DEPTH ) {
		w->lost = TRUE;
		return;
	}
	i = (INT)w->stack[--w->sp] + ( global ? w->gbias : w->lbias );
	if ( i < 0 || i >= x->count ) {
		w->lost = TRUE;
		return;
	}
	mark[i >> 3] |= (UB)( 1 << ( i & 7 ) );
	p = idx_item(x, i, &len);
	if ( p == NULL ) {
		w->lost = TRUE;
		return;
	}
	cs_walk(w, p, len, depth + 1);
}

LOCAL void cs_walk( CSWALK *w, CONST UB *p, SZ len, INT depth )
{
	SZ	i = 0;

	while ( i < len && !w->done && !w->lost ) {
		UB	b = p[i];

		if ( b == 28 ) {
			if ( i + 2 >= len ) { w->lost = TRUE; return; }
			if ( w->sp < CS_STACK ) w->stack[w->sp++] = (W)(H)be16(p + i + 1);
			i += 3;
			continue;
		}
		if ( b >= 32 ) {
			W	v;

			if ( b <= 246 ) {
				v = (W)b - 139;
				i += 1;
			} else if ( b <= 250 ) {
				if ( i + 1 >= len ) { w->lost = TRUE; return; }
				v = ( (W)b - 247 ) * 256 + p[i + 1] + 108;
				i += 2;
			} else if ( b <= 254 ) {
				if ( i + 1 >= len ) { w->lost = TRUE; return; }
				v = -( (W)b - 251 ) * 256 - p[i + 1] - 108;
				i += 2;
			} else {
				if ( i + 4 >= len ) { w->lost = TRUE; return; }
				v = (W)be32(p + i + 1) >> 16;	/* 16.16: the whole part */
				i += 5;
			}
			if ( w->sp < CS_STACK ) w->stack[w->sp++] = v;
			continue;
		}
		i++;
		switch ( b ) {
		case 1:	case 3: case 18: case 23:	/* the stems */
			w->nstems += w->sp / 2;
			w->sp = 0;
			break;
		case 19: case 20:			/* hintmask, cntrmask */
			if ( !w->seen_mask ) {
				w->nstems += w->sp / 2;	/* the vstems before it */
				w->seen_mask = TRUE;
			}
			w->sp = 0;
			i += (SZ)( ( w->nstems + 7 ) / 8 );
			break;
		case 10:				/* callsubr */
			cs_call(w, FALSE, depth);
			break;
		case 29:				/* callgsubr */
			cs_call(w, TRUE, depth);
			break;
		case 11:				/* return */
			return;
		case 14:				/* endchar */
			w->done = TRUE;
			return;
		case 12:
			if ( i >= len ) {
				w->lost = TRUE;
				return;
			}
			b = p[i++];
			if ( b != 34 && b != 35 && b != 36 && b != 37 && b != 0 ) {
				w->lost = TRUE;		/* arithmetic on the stack */
				return;
			}
			w->sp = 0;
			break;
		default:
			w->sp = 0;
			break;
		}
	}
}

/* ---------------------------------------------------------------- CFF cut down */

typedef struct {
	CONST CIDX	*x;
	UB		*mark;		/* NULL: every item as it is */
	UB		empty;		/* the byte an item not wanted becomes */
} KEEP;

LOCAL CONST UB *keep_item( void *arg, INT i, SZ *p_len )
{
	KEEP		*k = (KEEP *)arg;
	LOCAL UB	one[2][1] = { { 11 }, { 14 } };

	if ( k->mark != NULL && ( k->mark[i >> 3] & ( 1 << ( i & 7 ) ) ) == 0 ) {
		*p_len = 1;
		return one[( k->empty == 14 ) ? 1 : 0];
	}
	return idx_item(k->x, i, p_len);
}

/* The items of an INDEX picked by a list of their numbers */
typedef struct {
	CONST CIDX	*x;
	CONST INT	*list;
} LIST;

LOCAL CONST UB *list_item( void *arg, INT i, SZ *p_len )
{
	LIST	*l = (LIST *)arg;

	return idx_item(l->x, l->list[i], p_len);
}

#define CFF_MAX_FD	256

/* A private DICT written again with its subroutines right after it */
LOCAL void private_dict( PBUF *o, CONST UB *d, SZ len, BOOL has_subrs, W subrs_at )
{
	DENT	e[DICT_MAX];
	INT	n = dict_read(d, len, e, DICT_MAX), i;

	for ( i = 0; i < n; i++ ) {
		if ( e[i].op == 19 ) {
			continue;
		}
		pb_bytes(o, d + e[i].at, e[i].len);
	}
	if ( has_subrs ) {
		dict_int5(o, subrs_at);
		dict_op(o, 19);
	}
}

typedef struct {
	SZ		at, len;	/* the private DICT in the old table */
	CIDX		subrs;
	BOOL		has_subrs;
	UB		*mark;
	PBUF		priv;		/* written again */
	PBUF		lsub;		/* its subroutines, cut down */
} FDINFO;

EXPORT ER pdf_cff_subset( PDFFACE *ff, UB **p_out, SZ *p_len )
{
	SZ		blen = 0, len;
	UB		*b = table_all(ff, PDF_TAG('C', 'F', 'F', ' '), &blen);
	CIDX		names, tops, strs, gsub, cs, fda;
	DENT		*te = NULL;	/* the top DICT's entries, and three more DICTs' behind */
	INT		nte, nfd = 1, i, g;
	CONST DENT	*e;
	BOOL		cid;
	FDINFO		*fd = NULL;
	UB		*fdsel = NULL, *gmark = NULL;
	SZ		fdsel_at = 0, fdsel_len = 0;
	PBUF		top, gs, csb, fdab, out, cset_b, fsel_b;
	INT		*keepl = NULL, nkeep = 0;
	CONST UB	*td;
	SZ		tdlen;
	BOOL		keep_all = FALSE;
	ER		er = E_OBJ;
	KEEP		kp;

	*p_out = NULL;
	*p_len = 0;
	if ( b == NULL ) {
		return E_NOSPT;
	}
	pb_init(&top);
	pb_init(&gs);
	pb_init(&csb);
	pb_init(&fdab);
	pb_init(&out);
	pb_init(&cset_b);
	pb_init(&fsel_b);
	te = (DENT *)Kmalloc(sizeof(DENT) * DICT_MAX * 4);
	if ( te == NULL ) {
		er = E_NOMEM;
		goto done;
	}
	if ( blen < 4 || b[0] != 1 ) {
		goto done;
	}
	if ( !idx_read(b, blen, b[2], &names) || !idx_read(b, blen, names.end, &tops)
	  || !idx_read(b, blen, tops.end, &strs) || !idx_read(b, blen, strs.end, &gsub) ) {
		goto done;
	}
	td = idx_item(&tops, 0, &tdlen);
	if ( td == NULL ) {
		goto done;
	}
	nte = dict_read(td, tdlen, te, DICT_MAX);
	if ( nte <= 0 ) {
		goto done;
	}
	e = dict_find(te, nte, 17);
	if ( e == NULL || !idx_read(b, blen, (SZ)e->val[0], &cs) || cs.count != ff->nglyph ) {
		goto done;
	}
	cid = (BOOL)( dict_find(te, nte, DOP(12, 30)) != NULL );

	/* the font DICTs and which of them each glyph takes */
	if ( cid ) {
		CONST DENT	*fa = dict_find(te, nte, DOP(12, 36));
		CONST DENT	*fs = dict_find(te, nte, DOP(12, 37));

		if ( fa == NULL || fs == NULL || !idx_read(b, blen, (SZ)fa->val[0], &fda) ) {
			goto done;
		}
		nfd = fda.count;
		if ( nfd < 1 || nfd > CFF_MAX_FD ) {
			goto done;
		}
		fdsel_at = (SZ)fs->val[0];
		fdsel = (UB *)Kmalloc((SZ)ff->nglyph);
		if ( fdsel == NULL || fdsel_at >= blen ) {
			er = E_NOMEM;
			goto done;
		}
		if ( b[fdsel_at] == 0 ) {
			fdsel_len = 1 + (SZ)ff->nglyph;
			if ( fdsel_at + fdsel_len > blen ) goto done;
			for ( g = 0; g < ff->nglyph; g++ ) {
				fdsel[g] = b[fdsel_at + 1 + g];
			}
		} else if ( b[fdsel_at] == 3 ) {
			UW	nr = be16(b + fdsel_at + 1), r;

			fdsel_len = 3 + (SZ)nr * 3 + 2;
			if ( fdsel_at + fdsel_len > blen ) goto done;
			for ( r = 0; r < nr; r++ ) {
				CONST UB	*q = b + fdsel_at + 3 + r * 3;
				UW		first = be16(q), next = be16(q + 3);

				for ( g = (INT)first; g < (INT)next && g < ff->nglyph; g++ ) {
					fdsel[g] = q[2];
				}
			}
		} else {
			goto done;
		}
	}
	fd = (FDINFO *)Kmalloc(sizeof(FDINFO) * (SZ)nfd);
	if ( fd == NULL ) {
		er = E_NOMEM;
		goto done;
	}
	knl_memset(fd, 0, sizeof(FDINFO) * (SZ)nfd);
	for ( i = 0; i < nfd; i++ ) {
		DENT		*pe = te + DICT_MAX, *fe = te + 2 * DICT_MAX;
		INT		npe, nfe;
		CONST DENT	*pv, *sv;

		pb_init(&fd[i].priv);
		pb_init(&fd[i].lsub);
		if ( cid ) {
			CONST UB	*fdd = idx_item(&fda, i, &len);

			nfe = ( fdd != NULL ) ? dict_read(fdd, len, fe, DICT_MAX) : -1;
			pv = ( nfe > 0 ) ? dict_find(fe, nfe, 18) : NULL;
		} else {
			pv = dict_find(te, nte, 18);
		}
		if ( pv == NULL || pv->nval < 2 ) {
			continue;		/* a font DICT with no private DICT */
		}
		fd[i].len = (SZ)pv->val[0];
		fd[i].at = (SZ)pv->val[1];
		if ( fd[i].at + fd[i].len > blen ) {
			goto done;
		}
		npe = dict_read(b + fd[i].at, fd[i].len, pe, DICT_MAX);
		sv = ( npe > 0 ) ? dict_find(pe, npe, 19) : NULL;
		if ( sv != NULL && idx_read(b, blen, fd[i].at + (SZ)sv->val[0], &fd[i].subrs)
		  && fd[i].subrs.count > 0 ) {
			fd[i].has_subrs = TRUE;
			fd[i].mark = (UB *)Kmalloc((SZ)( fd[i].subrs.count + 8 ) / 8);
			if ( fd[i].mark == NULL ) {
				er = E_NOMEM;
				goto done;
			}
			knl_memset(fd[i].mark, 0, (SZ)( fd[i].subrs.count + 8 ) / 8);
		}
	}
	if ( gsub.count > 0 ) {
		gmark = (UB *)Kmalloc((SZ)( gsub.count + 8 ) / 8);
		if ( gmark == NULL ) {
			er = E_NOMEM;
			goto done;
		}
		knl_memset(gmark, 0, (SZ)( gsub.count + 8 ) / 8);
	}

	/* which subroutines the kept glyphs call */
	for ( g = 0; g < ff->nglyph && !keep_all; g++ ) {
		CSWALK		w;
		CONST UB	*p;
		INT		f = cid ? fdsel[g] : 0;

		if ( !is_used(ff, g) ) {
			continue;
		}
		if ( f >= nfd ) {
			keep_all = TRUE;
			break;
		}
		p = idx_item(&cs, g, &len);
		if ( p == NULL ) {
			continue;
		}
		knl_memset(&w, 0, sizeof(w));
		w.gsub = &gsub;
		w.gmark = gmark;
		w.gbias = subr_bias(gsub.count);
		if ( fd[f].has_subrs ) {
			w.lsub = &fd[f].subrs;
			w.lmark = fd[f].mark;
			w.lbias = subr_bias(fd[f].subrs.count);
		}
		cs_walk(&w, p, len, 0);
		if ( w.lost ) {
			keep_all = TRUE;
		}
	}

	/* the parts written again: the global subroutines, the glyphs, the private DICTs */
	kp.x = &gsub;
	kp.mark = keep_all ? NULL : gmark;
	kp.empty = 11;
	idx_write(&gs, gsub.count, keep_item, &kp);
	if ( cid ) {
		LIST	ls;

		/*
		 * Only the glyphs used, in the order of their old numbers. Each
		 * keeps its old number as its CID (the charset below), which is
		 * what the pages and the widths name it by.
		 */
		keepl = (INT *)Kmalloc(sizeof(INT) * (SZ)( ff->nglyph + 1 ));
		if ( keepl == NULL ) {
			er = E_NOMEM;
			goto done;
		}
		for ( g = 0; g < ff->nglyph; g++ ) {
			if ( g == 0 || is_used(ff, g) ) {
				keepl[nkeep++] = g;
			}
		}
		ls.x = &cs;
		ls.list = keepl;
		idx_write(&csb, nkeep, list_item, &ls);
		/* the CIDs: format 0, one for each glyph after .notdef */
		{
			UB	t[2];

			t[0] = 0;
			pb_bytes(&cset_b, t, 1);
			for ( i = 1; i < nkeep; i++ ) {
				put16(t, (UW)keepl[i]);
				pb_bytes(&cset_b, t, 2);
			}
			/* the font DICT of each: format 0, a byte each */
			t[0] = 0;
			pb_bytes(&fsel_b, t, 1);
			for ( i = 0; i < nkeep; i++ ) {
				t[0] = fdsel[keepl[i]];
				pb_bytes(&fsel_b, t, 1);
			}
		}
	} else {
		/* in place: those not used made empty, the numbers kept */
		kp.x = &cs;
		kp.mark = ff->used;
		kp.empty = 14;
		idx_write(&csb, cs.count, keep_item, &kp);
	}
	for ( i = 0; i < nfd; i++ ) {
		if ( fd[i].len == 0 && fd[i].at == 0 ) {
			continue;
		}
		private_dict(&fd[i].priv, b + fd[i].at, fd[i].len, fd[i].has_subrs, 0);
		if ( fd[i].has_subrs ) {
			kp.x = &fd[i].subrs;
			kp.mark = keep_all ? NULL : fd[i].mark;
			kp.empty = 11;
			idx_write(&fd[i].lsub, fd[i].subrs.count, keep_item, &kp);
		}
	}

	/*
	 * Where everything goes. Every offset in a DICT is written in five
	 * bytes, so the sizes are known before the offsets are: the DICTs
	 * are written once with nothing in them to learn their sizes, and
	 * again with the places.
	 */
	{
		SZ	hdr = b[2], at_top, at_strs, at_gs, at_charset = 0, at_fdsel = 0;
		SZ	at_cs = 0, at_fda = 0, toplen = 0, charset_len = 0;
		SZ	*priv_at;
		INT	pass;
		CONST UB *cset = NULL;
		W	cset_pre = -1;		/* a charset the format names by number */

		priv_at = (SZ *)Kmalloc(sizeof(SZ) * (SZ)nfd);
		if ( priv_at == NULL ) {
			er = E_NOMEM;
			goto done;
		}
		if ( cid ) {
			cset = cset_b.p;
			charset_len = cset_b.n;
			fdsel_len = fsel_b.n;
		} else {
			/* the face's own names of its glyphs, as they were */
			CONST DENT	*ce = dict_find(te, nte, 15);
			SZ		ca = ( ce != NULL ) ? (SZ)ce->val[0] : 0;

			if ( ca <= 2 ) {
				cset_pre = (W)ca;
			} else if ( ca < blen ) {
				SZ	k = ca + 1;
				INT	left = ff->nglyph - 1;

				if ( b[ca] == 0 ) {
					k += (SZ)left * 2;
				} else {
					while ( left > 0 && k + 3 <= blen ) {
						INT	nl = ( b[ca] == 1 ) ? b[k + 2] : (INT)be16(b + k + 2);

						k += ( b[ca] == 1 ) ? 3 : 4;
						left -= nl + 1;
					}
				}
				if ( k > blen ) {
					Kfree(priv_at);
					goto done;
				}
				cset = b + ca;
				charset_len = k - ca;
			}
		}
		for ( pass = 0; pass < 2; pass++ ) {
			SZ	at;

			top.n = 0;
			for ( i = 0; i < nte; i++ ) {
				INT	op = te[i].op;

				if ( op == 15 || op == 16 || op == 17 || op == 18
				  || op == DOP(12, 34) || op == DOP(12, 36) || op == DOP(12, 37) ) {
					continue;
				}
				pb_bytes(&top, td + te[i].at, te[i].len);
			}
			dict_int5(&top, ( cset_pre >= 0 ) ? cset_pre : pass ? (W)at_charset : 0);
			dict_op(&top, 15);
			dict_int5(&top, pass ? (W)at_cs : 0);
			dict_op(&top, 17);
			if ( cid ) {
				dict_int5(&top, ff->nglyph);
				dict_op(&top, DOP(12, 34));
				dict_int5(&top, pass ? (W)at_fda : 0);
				dict_op(&top, DOP(12, 36));
				dict_int5(&top, pass ? (W)at_fdsel : 0);
				dict_op(&top, DOP(12, 37));
			} else {
				dict_int5(&top, (W)fd[0].priv.n);
				dict_int5(&top, pass ? (W)priv_at[0] : 0);
				dict_op(&top, 18);
			}
			toplen = top.n;

			/* the places, from the sizes */
			at_top = hdr + ( names.end - names.at );
			at_strs = at_top + 2 + 1 + 2 * 4 + toplen;	/* one item, offsets of four bytes */
			at_gs = at_strs + ( strs.end - strs.at );
			at_charset = at_gs + gs.n;
			at = at_charset + charset_len;
			if ( cid ) {
				at_fdsel = at;
				at += fdsel_len;
			}
			at_cs = at;
			at += csb.n;
			if ( cid ) {
				at_fda = at;
				fdab.n = 0;
				for ( i = 0; i < nfd; i++ ) {
					priv_at[i] = 0;
				}
				/* the font DICTs, each naming its private DICT */
				{
					SZ	fdlen_total = 0, pat;
					PBUF	*fds = (PBUF *)Kmalloc(sizeof(PBUF) * (SZ)nfd);

					if ( fds == NULL ) {
						Kfree(priv_at);
						er = E_NOMEM;
						goto done;
					}
					/* sizes first */
					for ( i = 0; i < nfd; i++ ) {
						DENT		*fe = te + 3 * DICT_MAX;
						CONST UB	*fdd = idx_item(&fda, i, &len);
						INT		nfe = ( fdd != NULL ) ? dict_read(fdd, len, fe, DICT_MAX) : 0, k;

						pb_init(&fds[i]);
						for ( k = 0; k < nfe; k++ ) {
							if ( fe[k].op != 18 ) {
								pb_bytes(&fds[i], fdd + fe[k].at, fe[k].len);
							}
						}
						if ( fd[i].priv.n > 0 ) {
							dict_int5(&fds[i], (W)fd[i].priv.n);
							dict_int5(&fds[i], 0);
							dict_op(&fds[i], 18);
						}
						fdlen_total += fds[i].n;
					}
					{
						INT	osz = ( fdlen_total + 1 <= 0xFF ) ? 1 : ( fdlen_total + 1 <= 0xFFFF ) ? 2
							    : ( fdlen_total + 1 <= 0xFFFFFF ) ? 3 : 4;

						pat = at_fda + 3 + (SZ)( nfd + 1 ) * osz + fdlen_total;
					}
					for ( i = 0; i < nfd; i++ ) {
						if ( fd[i].priv.n > 0 ) {
							priv_at[i] = pat;
							pat += fd[i].priv.n + fd[i].lsub.n;
							/* the private DICT's offset: the five bytes before op 18 */
							put32(fds[i].p + fds[i].n - 5, (UW)priv_at[i]);
						}
					}
					{
						/* the INDEX of the font DICTs */
						SZ	tot = 0, o1 = 1;
						INT	osz, k;
						UB	t[4];

						for ( k = 0; k < nfd; k++ ) tot += fds[k].n;
						osz = ( tot + 1 <= 0xFF ) ? 1 : ( tot + 1 <= 0xFFFF ) ? 2
						    : ( tot + 1 <= 0xFFFFFF ) ? 3 : 4;
						put16(t, (UW)nfd);
						pb_bytes(&fdab, t, 2);
						t[0] = (UB)osz;
						pb_bytes(&fdab, t, 1);
						for ( k = 0; k <= nfd; k++ ) {
							INT	q;

							for ( q = 0; q < osz; q++ ) {
								t[q] = (UB)( o1 >> ( 8 * ( osz - 1 - q ) ) );
							}
							pb_bytes(&fdab, t, osz);
							if ( k < nfd ) o1 += fds[k].n;
						}
						for ( k = 0; k < nfd; k++ ) {
							pb_bytes(&fdab, fds[k].p, fds[k].n);
							pb_free(&fds[k]);
						}
					}
					Kfree(fds);
				}
				at += fdab.n;
			} else {
				priv_at[0] = at;
			}
		}
		/* the private DICTs again, now that their subroutines' place is known */
		for ( i = 0; i < nfd; i++ ) {
			if ( fd[i].priv.n > 0 && fd[i].has_subrs ) {
				put32(fd[i].priv.p + fd[i].priv.n - 5, (UW)fd[i].priv.n);
			}
		}

		/* and written out in that order */
		pb_bytes(&out, b, hdr);
		pb_bytes(&out, b + names.at, names.end - names.at);
		{
			UB	t[11];

			/* the top DICT INDEX: one item, offsets of four bytes */
			put16(t, 1);
			t[2] = 4;
			put32(t + 3, 1);
			put32(t + 7, (UW)( 1 + toplen ));
			pb_bytes(&out, t, 11);
			pb_bytes(&out, top.p, top.n);
		}
		pb_bytes(&out, b + strs.at, strs.end - strs.at);
		pb_bytes(&out, gs.p, gs.n);
		if ( cset != NULL ) {
			pb_bytes(&out, cset, charset_len);
		}
		if ( cid ) {
			pb_bytes(&out, fsel_b.p, fsel_b.n);
		}
		pb_bytes(&out, csb.p, csb.n);
		if ( cid ) {
			pb_bytes(&out, fdab.p, fdab.n);
		}
		for ( i = 0; i < nfd; i++ ) {
			if ( fd[i].priv.n > 0 ) {
				if ( out.n != priv_at[i] ) {
					out.fail = TRUE;	/* the reckoning and the writing disagree */
				}
				pb_bytes(&out, fd[i].priv.p, fd[i].priv.n);
				pb_bytes(&out, fd[i].lsub.p, fd[i].lsub.n);
			}
		}
		Kfree(priv_at);
	}
	if ( out.fail || top.fail || gs.fail || csb.fail || fdab.fail ) {
		er = E_NOMEM;
		goto done;
	}
	*p_out = out.p;
	*p_len = out.n;
	out.p = NULL;
	er = E_OK;

done:
	if ( fd != NULL ) {
		for ( i = 0; i < nfd; i++ ) {
			pb_free(&fd[i].priv);
			pb_free(&fd[i].lsub);
			if ( fd[i].mark != NULL ) Kfree(fd[i].mark);
		}
		Kfree(fd);
	}
	if ( fdsel != NULL ) Kfree(fdsel);
	if ( gmark != NULL ) Kfree(gmark);
	pb_free(&top);
	pb_free(&gs);
	pb_free(&csb);
	pb_free(&fdab);
	pb_free(&out);
	pb_free(&cset_b);
	pb_free(&fsel_b);
	if ( keepl != NULL ) Kfree(keepl);
	if ( te != NULL ) Kfree(te);
	Kfree(b);

	return er;
}

/* ---------------------------------------------------------------- TrueType cut down */

/*
 * A face of TrueType outlines: the glyphs used, and those they are
 * made of (a composite glyph names others), kept in 'glyf' and the
 * rest made empty in 'loca'. The tables a reader of the file needs to
 * draw glyphs by their numbers go with them; the table of letters
 * (cmap) does not, because the file names glyphs by number.
 */
LOCAL CONST UW tt_keep[] = {
	PDF_TAG('h', 'e', 'a', 'd'), PDF_TAG('h', 'h', 'e', 'a'), PDF_TAG('m', 'a', 'x', 'p'),
	PDF_TAG('h', 'm', 't', 'x'), PDF_TAG('c', 'v', 't', ' '), PDF_TAG('f', 'p', 'g', 'm'),
	PDF_TAG('p', 'r', 'e', 'p'), PDF_TAG('l', 'o', 'c', 'a'), PDF_TAG('g', 'l', 'y', 'f'),
	PDF_TAG('n', 'a', 'm', 'e')		/* the names, with the face's copyright notice */
};
#define N_TT_KEEP	( (INT)( sizeof(tt_keep) / sizeof(tt_keep[0]) ) )

LOCAL UW tt_sum( CONST UB *p, SZ n )
{
	UW	s = 0;
	SZ	i;

	for ( i = 0; i + 3 < n; i += 4 ) {
		s += be32(p + i);
	}
	if ( i < n ) {
		UB	t[4] = { 0, 0, 0, 0 };
		SZ	k;

		for ( k = 0; i + k < n; k++ ) t[k] = p[i + k];
		s += be32(t);
	}
	return s;
}

EXPORT ER pdf_ttf_subset( PDFFACE *ff, UB **p_out, SZ *p_len )
{
	UB	*tab[N_TT_KEEP];
	SZ	tlen[N_TT_KEEP];
	UB	*keep;
	INT	i, g, nt = 0, iloca = 7, iglyf = 8, ihead = 0;
	BOOL	lng, more = TRUE;
	PBUF	glyf, loca, out;
	ER	er = E_OBJ;

	*p_out = NULL;
	*p_len = 0;
	for ( i = 0; i < N_TT_KEEP; i++ ) {
		tlen[i] = 0;
		tab[i] = table_all(ff, tt_keep[i], &tlen[i]);
	}
	pb_init(&glyf);
	pb_init(&loca);
	pb_init(&out);
	keep = (UB *)Kmalloc((SZ)( ff->nglyph + 8 ) / 8);
	if ( keep == NULL || tab[ihead] == NULL || tab[iloca] == NULL || tab[iglyf] == NULL
	  || tlen[ihead] < 54 ) {
		goto done;
	}
	knl_memcpy(keep, ff->used, (SZ)( ff->nglyph + 8 ) / 8);
	lng = (BOOL)( sbe16(tab[ihead] + 50) != 0 );

	/* the glyphs the kept composites are made of, until nothing new is found */
	while ( more ) {
		more = FALSE;
		for ( g = 0; g < ff->nglyph; g++ ) {
			UW		a, z;
			CONST UB	*p;

			if ( ( keep[g >> 3] & ( 1 << ( g & 7 ) ) ) == 0 ) continue;
			a = lng ? be32(tab[iloca] + 4 * g) : be16(tab[iloca] + 2 * g) * 2;
			z = lng ? be32(tab[iloca] + 4 * g + 4) : be16(tab[iloca] + 2 * g + 2) * 2;
			if ( z <= a + 10 || z > (UW)tlen[iglyf] ) continue;
			p = tab[iglyf] + a;
			if ( sbe16(p) >= 0 ) continue;
			{
				SZ	at = 10;
				UW	flags;

				do {
					UW	c;

					if ( a + at + 4 > z ) break;
					flags = be16(p + at);
					c = be16(p + at + 2);
					if ( c < (UW)ff->nglyph && ( keep[c >> 3] & ( 1 << ( c & 7 ) ) ) == 0 ) {
						keep[c >> 3] |= (UB)( 1 << ( c & 7 ) );
						more = TRUE;
					}
					at += 4 + ( ( flags & 0x0001 ) ? 4 : 2 );
					if ( flags & 0x0008 ) at += 2;
					else if ( flags & 0x0040 ) at += 4;
					else if ( flags & 0x0080 ) at += 8;
				} while ( flags & 0x0020 );
			}
		}
	}
	/* glyf and a long loca */
	for ( g = 0; g <= ff->nglyph; g++ ) {
		UB	t[4];

		put32(t, (UW)glyf.n);
		pb_bytes(&loca, t, 4);
		if ( g == ff->nglyph ) break;
		if ( ( keep[g >> 3] & ( 1 << ( g & 7 ) ) ) != 0 ) {
			UW	a = lng ? be32(tab[iloca] + 4 * g) : be16(tab[iloca] + 2 * g) * 2;
			UW	z = lng ? be32(tab[iloca] + 4 * g + 4) : be16(tab[iloca] + 2 * g + 2) * 2;

			if ( z > a && z <= (UW)tlen[iglyf] ) {
				UB	pad[3] = { 0, 0, 0 };

				pb_bytes(&glyf, tab[iglyf] + a, z - a);
				pb_bytes(&glyf, pad, (SZ)( ( 4 - ( ( z - a ) & 3 ) ) & 3 ));
			}
		}
	}
	put16(tab[ihead] + 50, 1);		/* loca is long now */
	put32(tab[ihead] + 8, 0);		/* checkSumAdjustment, not reckoned */
	for ( i = 0; i < N_TT_KEEP; i++ ) {
		if ( i == iloca || i == iglyf || tab[i] != NULL ) nt++;
	}
	{
		UB	hd[12];
		INT	sr = 1, es = 0;
		SZ	at = 12 + 16 * (SZ)nt;

		while ( sr * 2 <= nt ) {
			sr *= 2;
			es++;
		}
		put32(hd, 0x00010000);
		put16(hd + 4, (UW)nt);
		put16(hd + 6, (UW)( sr * 16 ));
		put16(hd + 8, (UW)es);
		put16(hd + 10, (UW)( nt * 16 - sr * 16 ));
		pb_bytes(&out, hd, 12);
		/* the directory, in the order of the tags: cvt, fpgm, glyf, head,
		   hhea, hmtx, loca, maxp, name, prep */
		{
			CONST INT	order[N_TT_KEEP] = { 4, 5, 8, 0, 1, 3, 7, 2, 9, 6 };
			INT		k;

			for ( k = 0; k < N_TT_KEEP; k++ ) {
				INT		t = order[k];
				CONST UB	*d = ( t == iloca ) ? loca.p : ( t == iglyf ) ? glyf.p : tab[t];
				SZ		dl = ( t == iloca ) ? loca.n : ( t == iglyf ) ? glyf.n : tlen[t];
				UB		ent[16];

				if ( d == NULL && t != iglyf ) continue;
				put32(ent, tt_keep[t]);
				put32(ent + 4, tt_sum(d, dl));
				put32(ent + 8, (UW)at);
				put32(ent + 12, (UW)dl);
				pb_bytes(&out, ent, 16);
				at += ( dl + 3 ) & ~(SZ)3;
			}
			for ( k = 0; k < N_TT_KEEP; k++ ) {
				INT		t = order[k];
				CONST UB	*d = ( t == iloca ) ? loca.p : ( t == iglyf ) ? glyf.p : tab[t];
				SZ		dl = ( t == iloca ) ? loca.n : ( t == iglyf ) ? glyf.n : tlen[t];
				UB		pad[3] = { 0, 0, 0 };

				if ( d == NULL && t != iglyf ) continue;
				pb_bytes(&out, d, dl);
				pb_bytes(&out, pad, (SZ)( ( 4 - ( dl & 3 ) ) & 3 ));
			}
		}
	}
	if ( out.fail || glyf.fail || loca.fail ) {
		er = E_NOMEM;
		goto done;
	}
	*p_out = out.p;
	*p_len = out.n;
	out.p = NULL;
	er = E_OK;
done:
	for ( i = 0; i < N_TT_KEEP; i++ ) {
		if ( tab[i] != NULL ) Kfree(tab[i]);
	}
	if ( keep != NULL ) Kfree(keep);
	pb_free(&glyf);
	pb_free(&loca);
	pb_free(&out);

	return er;
}

/* ---------------------------------------------------------------- written into the file */

/* Six capitals for the name of a cut-down face, from the glyphs it keeps */
LOCAL void subset_tag( CONST PDFFACE *ff, char *tag )
{
	UW	h = 2166136261U;
	INT	i;

	for ( i = 0; i < ( ff->nglyph + 7 ) / 8; i++ ) {
		h = ( h ^ ff->used[i] ) * 16777619U;
	}
	for ( i = 0; i < 6; i++ ) {
		tag[i] = (char)( 'A' + h % 26 );
		h /= 26;
		h = h * 31 + (UW)i;
	}
	tag[6] = '+';
	tag[7] = 0;
}

LOCAL void utf16_hex( PBUF *b, UW c )
{
	if ( c >= 0x10000 ) {
		c -= 0x10000;
		pb_hex(b, 0xD800 | ( c >> 10 ), 4);
		pb_hex(b, 0xDC00 | ( c & 0x3FF ), 4);
	} else {
		pb_hex(b, c, 4);
	}
}

EXPORT ER pdf_face_write( PDF *f, PDFFACE *ff )
{
	UB	*prog = NULL;
	SZ	plen = 0;
	INT	o_cid = pdf_new_obj(f), o_desc = pdf_new_obj(f), o_file = pdf_new_obj(f);
	INT	o_uni = pdf_new_obj(f), g, n;
	char	tag[8];
	PBUF	b;
	ER	er;

	er = ff->cff ? pdf_cff_subset(ff, &prog, &plen) : pdf_ttf_subset(ff, &prog, &plen);
	if ( er < E_OK ) {
		return er;
	}
	subset_tag(ff, tag);

	/* the font program */
	pb_init(&b);
	if ( ff->cff ) {
		pb_str(&b, " /Subtype /CIDFontType0C");
	} else {
		pb_str(&b, " /Length1 ");
		pb_int(&b, (D)plen);
	}
	er = pdf_stream_buf(f, o_file, &b, prog, plen, TRUE, 9);
	pb_free(&b);
	Kfree(prog);
	if ( er < E_OK ) {
		return er;
	}

	/* what the face is like */
	pdf_begin_obj(f, o_desc);
	pb_init(&b);
	pb_str(&b, "<< /Type /FontDescriptor /FontName /");
	pb_str(&b, tag);
	pb_str(&b, (CONST char *)ff->psname);
	pb_str(&b, " /Flags 4 /FontBBox [");
	for ( g = 0; g < 4; g++ ) {
		pb_str(&b, " ");
		pb_int(&b, ff->bbox[g]);
	}
	pb_str(&b, " ] /ItalicAngle 0 /Ascent ");
	pb_int(&b, ff->ascent);
	pb_str(&b, " /Descent ");
	pb_int(&b, ff->descent);
	pb_str(&b, " /CapHeight ");
	pb_int(&b, ff->capheight);
	pb_str(&b, " /StemV 80 ");
	pb_str(&b, ff->cff ? "/FontFile3 " : "/FontFile2 ");
	pb_int(&b, o_file);
	pb_str(&b, " 0 R >>");
	pdf_buf(f, &b);
	pb_free(&b);
	pdf_end_obj(f);

	/* the glyphs by number, and their widths */
	pdf_begin_obj(f, o_cid);
	pb_init(&b);
	pb_str(&b, "<< /Type /Font /Subtype ");
	pb_str(&b, ff->cff ? "/CIDFontType0" : "/CIDFontType2");
	pb_str(&b, " /BaseFont /");
	pb_str(&b, tag);
	pb_str(&b, (CONST char *)ff->psname);
	pb_str(&b, " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>");
	pb_str(&b, " /FontDescriptor ");
	pb_int(&b, o_desc);
	pb_str(&b, " 0 R /DW 1000");
	if ( !ff->cff ) {
		pb_str(&b, " /CIDToGIDMap /Identity");
	}
	pb_str(&b, " /W [");
	for ( g = 0; g < ff->nglyph; ) {
		INT	k;

		if ( !is_used(ff, g) ) {
			g++;
			continue;
		}
		pb_str(&b, " ");
		pb_int(&b, g);
		pb_str(&b, " [");
		for ( k = g; k < ff->nglyph && is_used(ff, k) && k - g < 64; k++ ) {
			pb_str(&b, " ");
			pb_int(&b, ff->adv[k]);
		}
		pb_str(&b, " ]");
		g = k;
	}
	pb_str(&b, " ] >>");
	pdf_buf(f, &b);
	pb_free(&b);
	pdf_end_obj(f);

	/* the glyphs back into letters */
	pb_init(&b);
	pb_str(&b, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
		   "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
		   "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
		   "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
	for ( g = 0; g < ff->nglyph; ) {
		INT	k, cnt = 0;

		for ( k = g; k < ff->nglyph && cnt < 100; k++ ) {
			if ( is_used(ff, k) && ff->uni[k] != 0 ) cnt++;
		}
		if ( cnt == 0 ) {
			break;
		}
		pb_int(&b, cnt);
		pb_str(&b, " beginbfchar\n");
		for ( n = 0; g < k; g++ ) {
			if ( !is_used(ff, g) || ff->uni[g] == 0 ) continue;
			pb_str(&b, "<");
			pb_hex(&b, (UW)g, 4);
			pb_str(&b, "> <");
			utf16_hex(&b, ff->uni[g]);
			pb_str(&b, ">\n");
			n++;
		}
		pb_str(&b, "endbfchar\n");
	}
	pb_str(&b, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
	er = pdf_stream(f, o_uni, NULL, b.p, b.n, TRUE, 6);
	pb_free(&b);
	if ( er < E_OK ) {
		return er;
	}

	/* and the face as a page names it */
	pdf_begin_obj(f, ff->obj);
	pb_init(&b);
	pb_str(&b, "<< /Type /Font /Subtype /Type0 /BaseFont /");
	pb_str(&b, tag);
	pb_str(&b, (CONST char *)ff->psname);
	pb_str(&b, " /Encoding /Identity-H /DescendantFonts [ ");
	pb_int(&b, o_cid);
	pb_str(&b, " 0 R ] /ToUnicode ");
	pb_int(&b, o_uni);
	pb_str(&b, " 0 R >>");
	pdf_buf(f, &b);
	pb_free(&b);
	pdf_end_obj(f);

	return f->fail ? E_NOMEM : E_OK;
}
