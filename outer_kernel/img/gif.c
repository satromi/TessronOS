/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gif.c
 *	Reading a GIF (87a or 89a) into pixels
 *
 *	The first picture of the file, placed on the logical screen: what
 *	it does not cover, and the colour a graphic control extension says
 *	is transparent, come out as IMG_CLEAR. The codes are variable-length
 *	LZW, 2 to 12 bits, packed from the low bit up in sub-blocks of at
 *	most 255 bytes; an interlaced picture comes in four passes of rows.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/img.h>

#define GIF_MAX_SIDE	8192
#define LZW_MAX		4096		/* codes of 12 bits */

LOCAL UW le16( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 );
}

typedef struct {
	CONST UB *d;
	SZ	size, at;
	INT	w, h;			/* the logical screen */
	UW	gct[256];		/* the global colour table */
	INT	ngct;
	INT	trans;			/* the transparent index, -1 none */
} GIF;

/* The head: signature and logical screen, with its colour table */
LOCAL ER gif_head( GIF *g )
{
	CONST UB	*d = g->d;
	INT		i;

	if ( g->size < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F' || d[3] != '8'
	  || ( d[4] != '7' && d[4] != '9' ) || d[5] != 'a' ) {
		return E_PAR;
	}
	g->w = (INT)le16(d + 6);
	g->h = (INT)le16(d + 8);
	if ( g->w <= 0 || g->h <= 0 || g->w > GIF_MAX_SIDE || g->h > GIF_MAX_SIDE ) {
		return E_NOSPT;
	}
	g->at = 13;
	g->ngct = 0;
	g->trans = -1;
	if ( ( d[10] & 0x80 ) != 0 ) {
		g->ngct = 2 << ( d[10] & 7 );
		if ( g->at + (SZ)g->ngct * 3 > g->size ) return E_PAR;
		for ( i = 0; i < g->ngct; i++ ) {
			CONST UB *c = d + g->at + i * 3;

			g->gct[i] = ( (UW)c[0] << 16 ) | ( (UW)c[1] << 8 ) | c[2];
		}
		g->at += (SZ)g->ngct * 3;
	}
	return E_OK;
}

/* Sub-blocks passed over up to their end (a block of length 0) */
LOCAL ER skip_blocks( GIF *g )
{
	while ( g->at < g->size ) {
		UB	n = g->d[g->at++];

		if ( n == 0 ) return E_OK;
		g->at += n;
	}
	return E_PAR;
}

EXPORT ER img_gif_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	GIF	*g;
	ER	er;

	if ( data == NULL ) {
		return E_PAR;
	}
	g = (GIF *)Kmalloc(sizeof(GIF));
	if ( g == NULL ) {
		return E_NOMEM;
	}
	g->d = data;
	g->size = size;
	er = gif_head(g);
	if ( er >= E_OK ) {
		*p_w = g->w;
		*p_h = g->h;
	}
	Kfree(g);
	return er;
}

/* The bits of the image data: sub-blocks read one after another */
typedef struct {
	GIF	*g;
	INT	left;			/* bytes left in the sub-block at work */
	UW	acc;
	INT	nbits;
	BOOL	end;
} BITS;

LOCAL INT get_code( BITS *b, INT width )
{
	GIF	*g = b->g;

	while ( b->nbits < width ) {
		if ( b->left == 0 ) {
			if ( b->end || g->at >= g->size ) return -1;
			b->left = g->d[g->at++];
			if ( b->left == 0 ) {
				b->end = TRUE;
				return -1;
			}
		}
		if ( g->at >= g->size ) return -1;
		b->acc |= (UW)g->d[g->at++] << b->nbits;
		b->nbits += 8;
		b->left--;
	}
	{
		INT	c = (INT)( b->acc & ( ( 1U << width ) - 1 ) );

		b->acc >>= width;
		b->nbits -= width;
		return c;
	}
}

typedef struct {
	UH	prefix[LZW_MAX];
	UB	suffix[LZW_MAX];
	UB	stack[LZW_MAX + 1];
} LZW;

/*
 * The indices of a picture of n pixels, LZW decoded into out. A stream
 * that ends early leaves the rest as it was.
 */
LOCAL ER lzw_decode( GIF *g, INT minsize, UB *out, SZ n )
{
	LZW	*z;
	BITS	b;
	INT	clear = 1 << minsize, eoi = clear + 1, next, width, code, old = -1, first = 0, sp;
	SZ	at = 0;

	if ( minsize < 2 || minsize > 8 ) {
		return E_PAR;
	}
	z = (LZW *)Kmalloc(sizeof(LZW));
	if ( z == NULL ) {
		return E_NOMEM;
	}
	for ( code = 0; code < clear; code++ ) {
		z->prefix[code] = 0;
		z->suffix[code] = (UB)code;
	}
	b.g = g;
	b.left = 0;
	b.acc = 0;
	b.nbits = 0;
	b.end = FALSE;
	width = minsize + 1;
	next = eoi + 1;
	while ( at < n ) {
		code = get_code(&b, width);
		if ( code < 0 || code == eoi ) break;
		if ( code == clear ) {
			width = minsize + 1;
			next = eoi + 1;
			old = -1;
			continue;
		}
		if ( old < 0 ) {
			if ( code >= clear ) break;	/* the first code must be a colour */
			out[at++] = (UB)code;
			old = first = code;
			continue;
		}
		sp = 0;
		{
			INT	c = code;

			if ( code >= next ) {
				if ( code > next ) break;	/* not a code there is yet */
				z->stack[sp++] = (UB)first;
				c = old;
			}
			while ( c >= clear && sp < LZW_MAX ) {
				z->stack[sp++] = z->suffix[c];
				c = z->prefix[c];
			}
			z->stack[sp++] = (UB)c;
			first = c;
		}
		while ( sp > 0 && at < n ) out[at++] = z->stack[--sp];
		if ( next < LZW_MAX ) {
			z->prefix[next] = (UH)old;
			z->suffix[next] = (UB)first;
			next++;
			if ( next == ( 1 << width ) && width < 12 ) width++;
		}
		old = code;
	}
	Kfree(z);
	/* the sub-blocks left over, to their end */
	if ( !b.end ) {
		while ( b.left > 0 && g->at < g->size ) {
			g->at++;
			b.left--;
		}
		(void)skip_blocks(g);
	}
	return E_OK;
}

/* The row an interlaced picture's y-th row goes to */
LOCAL INT inter_row( INT y, INT h )
{
	INT	n;

	n = ( h + 7 ) / 8;			/* pass 1: every 8th from 0 */
	if ( y < n ) return y * 8;
	y -= n;
	n = ( h + 3 ) / 8;			/* pass 2: every 8th from 4 */
	if ( y < n ) return y * 8 + 4;
	y -= n;
	n = ( h + 1 ) / 4;			/* pass 3: every 4th from 2 */
	if ( y < n ) return y * 4 + 2;
	y -= n;
	return y * 2 + 1;			/* pass 4: every 2nd from 1 */
}

EXPORT ER img_gif_decode( CONST UB *data, SZ size, UW **p_pixels, INT *p_w, INT *p_h )
{
	GIF	*g;
	UW	*px = NULL, lct[256], *pal;
	UB	*idx = NULL;
	INT	x, y, fx, fy, fw, fh, npal, i;
	BOOL	inter;
	ER	er;

	if ( data == NULL || p_pixels == NULL ) {
		return E_PAR;
	}
	g = (GIF *)Kmalloc(sizeof(GIF));
	if ( g == NULL ) {
		return E_NOMEM;
	}
	g->d = data;
	g->size = size;
	er = gif_head(g);
	while ( er >= E_OK ) {
		UB	t;

		if ( g->at >= g->size ) {
			er = E_PAR;
			break;
		}
		t = data[g->at++];
		if ( t == 0x3B ) {			/* the end, and no picture */
			er = E_PAR;
			break;
		}
		if ( t == 0x21 ) {			/* an extension */
			UB	label = ( g->at < g->size ) ? data[g->at++] : 0;

			if ( label == 0xF9 && g->at + 5 < g->size && data[g->at] >= 4 ) {
				if ( ( data[g->at + 1] & 1 ) != 0 ) g->trans = data[g->at + 4];
			}
			er = skip_blocks(g);
			continue;
		}
		if ( t != 0x2C || g->at + 9 > g->size ) {
			er = E_PAR;
			break;
		}

		/* the first picture */
		fx = (INT)le16(data + g->at);
		fy = (INT)le16(data + g->at + 2);
		fw = (INT)le16(data + g->at + 4);
		fh = (INT)le16(data + g->at + 6);
		inter = (BOOL)( ( data[g->at + 8] & 0x40 ) != 0 );
		pal = g->gct;
		npal = g->ngct;
		if ( ( data[g->at + 8] & 0x80 ) != 0 ) {
			npal = 2 << ( data[g->at + 8] & 7 );
			g->at += 9;
			if ( g->at + (SZ)npal * 3 > g->size ) {
				er = E_PAR;
				break;
			}
			for ( i = 0; i < npal; i++ ) {
				CONST UB *c = data + g->at + i * 3;

				lct[i] = ( (UW)c[0] << 16 ) | ( (UW)c[1] << 8 ) | c[2];
			}
			g->at += (SZ)npal * 3;
			pal = lct;
		} else {
			g->at += 9;
		}
		if ( fw <= 0 || fh <= 0 || fw > GIF_MAX_SIDE || fh > GIF_MAX_SIDE || g->at >= g->size ) {
			er = E_PAR;
			break;
		}
		px = (UW *)Kmalloc((SZ)g->w * g->h * sizeof(UW));
		idx = (UB *)Kmalloc((SZ)fw * fh);
		if ( px == NULL || idx == NULL ) {
			er = E_NOMEM;
			break;
		}
		for ( i = 0; i < g->w * g->h; i++ ) px[i] = IMG_CLEAR;
		knl_memset(idx, ( g->trans >= 0 ) ? g->trans : 0, (SZ)fw * fh);
		er = lzw_decode(g, data[g->at++], idx, (SZ)fw * fh);
		if ( er < E_OK ) break;
		for ( y = 0; y < fh; y++ ) {
			INT	ry = inter ? inter_row(y, fh) : y;

			if ( fy + ry >= g->h ) continue;
			for ( x = 0; x < fw && fx + x < g->w; x++ ) {
				INT	c = idx[(SZ)y * fw + x];

				if ( c == g->trans || c >= npal ) continue;
				px[(SZ)( fy + ry ) * g->w + fx + x] = pal[c] & 0x00FFFFFFU;
			}
		}
		*p_pixels = px;
		*p_w = g->w;
		*p_h = g->h;
		px = NULL;
		break;
	}
	if ( px != NULL ) Kfree(px);
	if ( idx != NULL ) Kfree(idx);
	Kfree(g);
	return er;
}
