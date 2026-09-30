/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bmp.c
 *	Reading a BMP or an icon (ICO) into pixels, and any picture by what
 *	it starts with
 *
 *	Uncompressed BMP of 1, 4, 8, 16, 24 or 32 bits a pixel, bottom-up
 *	or top-down, with a palette where the depth needs one; 16 and 32
 *	bits with the usual masks or the masks the head gives. Run-length
 *	compressed files are refused with E_NOSPT.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/img.h>

#define BMP_MAX_SIDE	8192

LOCAL UW le16( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 );
}

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

typedef struct {
	INT	w, h;
	BOOL	topdown;
	INT	bpp;
	UW	comp;
	UW	off;			/* where the pixels start */
	CONST UB *pal;
	INT	npal, palsz;
	UW	mask[3];
} BMPHEAD;

LOCAL ER head_of( CONST UB *d, SZ size, BMPHEAD *b )
{
	UW	hs;
	INT	h;

	if ( d == NULL || size < 26 || d[0] != 'B' || d[1] != 'M' ) {
		return E_PAR;
	}
	knl_memset(b, 0, sizeof(*b));
	b->off = le32(d + 10);
	hs = le32(d + 14);
	if ( hs == 12 ) {
		/* the old head: 16-bit sizes, three-byte palette entries */
		b->w = (INT)le16(d + 18);
		h = (INT)(H)le16(d + 20);
		b->bpp = (INT)le16(d + 24);
		b->palsz = 3;
	} else if ( hs >= 40 && size >= 14 + 40 ) {
		b->w = (INT)le32(d + 18);
		h = (INT)le32(d + 22);
		b->bpp = (INT)le16(d + 28);
		b->comp = le32(d + 30);
		b->npal = (INT)le32(d + 46);
		b->palsz = 4;
		if ( b->comp == 3 && size >= 14 + 52 ) {
			b->mask[0] = le32(d + 54);
			b->mask[1] = le32(d + 58);
			b->mask[2] = le32(d + 62);
		}
	} else {
		return E_NOSPT;
	}
	b->topdown = (BOOL)( h < 0 );
	b->h = ( h < 0 ) ? -h : h;
	if ( b->w <= 0 || b->h <= 0 || b->w > BMP_MAX_SIDE || b->h > BMP_MAX_SIDE ) {
		return E_NOSPT;
	}
	if ( b->comp != 0 && b->comp != 3 ) {
		return E_NOSPT;
	}
	if ( b->bpp <= 8 ) {
		if ( b->npal <= 0 || b->npal > ( 1 << b->bpp ) ) b->npal = 1 << b->bpp;
		b->pal = d + 14 + hs;
		if ( b->pal + b->npal * b->palsz > d + size ) {
			return E_PAR;
		}
	} else if ( b->bpp == 16 && b->comp == 0 ) {
		b->mask[0] = 0x7C00;
		b->mask[1] = 0x03E0;
		b->mask[2] = 0x001F;
	} else if ( b->bpp == 32 && b->comp == 0 ) {
		b->mask[0] = 0x00FF0000;
		b->mask[1] = 0x0000FF00;
		b->mask[2] = 0x000000FF;
	} else if ( b->bpp != 24 && b->bpp != 16 && b->bpp != 32 ) {
		return E_NOSPT;
	}
	if ( b->off + (UW)( ( ( b->w * b->bpp + 31 ) / 32 ) * 4 ) * (UW)b->h > (UW)size ) {
		return E_PAR;
	}
	return E_OK;
}

/* A field of a pixel taken by its mask and made eight bits */
LOCAL UW field( UW v, UW mask )
{
	INT	shift = 0, bits = 0;

	if ( mask == 0 ) {
		return 0;
	}
	while ( !( mask & 1 ) ) {
		mask >>= 1;
		shift++;
	}
	while ( mask & 1 ) {
		mask >>= 1;
		bits++;
	}
	v = ( v >> shift ) & ( ( 1U << bits ) - 1 );
	if ( bits >= 8 ) {
		return v >> ( bits - 8 );
	}
	return ( v * 255 ) / ( ( 1U << bits ) - 1 );
}

EXPORT ER img_bmp_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	BMPHEAD	b;
	ER	er = head_of(data, size, &b);

	if ( er >= E_OK ) {
		if ( p_w != NULL ) *p_w = b.w;
		if ( p_h != NULL ) *p_h = b.h;
	}
	return er;
}

EXPORT ER img_bmp_decode( CONST UB *data, SZ size, UW **p_pixels, INT *p_w, INT *p_h )
{
	BMPHEAD	b;
	UW	*px;
	INT	x, y, stride;
	ER	er;

	if ( p_pixels == NULL ) {
		return E_PAR;
	}
	er = head_of(data, size, &b);
	if ( er < E_OK ) {
		return er;
	}
	px = (UW *)Kmalloc((SZ)b.w * b.h * sizeof(UW));
	if ( px == NULL ) {
		return E_NOMEM;
	}
	stride = ( ( b.w * b.bpp + 31 ) / 32 ) * 4;
	for ( y = 0; y < b.h; y++ ) {
		CONST UB	*row = data + b.off + (SZ)( b.topdown ? y : b.h - 1 - y ) * stride;
		UW		*o = px + (SZ)y * b.w;

		for ( x = 0; x < b.w; x++ ) {
			UW	c;

			if ( b.bpp <= 8 ) {
				INT	bit = x * b.bpp;
				UW	i = ( row[bit >> 3] >> ( 8 - b.bpp - ( bit & 7 ) ) ) & ( ( 1U << b.bpp ) - 1 );
				CONST UB *e;

				if ( (INT)i >= b.npal ) i = 0;
				e = b.pal + i * b.palsz;
				c = ( (UW)e[2] << 16 ) | ( (UW)e[1] << 8 ) | e[0];
			} else if ( b.bpp == 24 ) {
				CONST UB *e = row + x * 3;

				c = ( (UW)e[2] << 16 ) | ( (UW)e[1] << 8 ) | e[0];
			} else {
				UW	v = ( b.bpp == 16 ) ? le16(row + x * 2) : le32(row + x * 4);

				c = ( field(v, b.mask[0]) << 16 ) | ( field(v, b.mask[1]) << 8 ) | field(v, b.mask[2]);
			}
			o[x] = c & 0x00FFFFFFU;
		}
	}
	*p_pixels = px;
	if ( p_w != NULL ) *p_w = b.w;
	if ( p_h != NULL ) *p_h = b.h;
	return E_OK;
}

/* ---------------------------------------------------------------- icons */

/*
 * An icon file: several pictures of one icon, each a PNG or a BMP without
 * its file head whose height counts the mask as well. The largest is
 * taken. A pixel is clear where the mask says so, or, at 32 bits, where
 * it is more than half transparent.
 */
LOCAL BOOL is_ico( CONST UB *d, SZ size )
{
	return (BOOL)( size >= 22 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0
		    && le16(d + 4) > 0 );
}

/* The picture of the icon to take: where it is and how long */
LOCAL ER ico_pick( CONST UB *d, SZ size, CONST UB **p_img, SZ *p_len )
{
	INT	n = (INT)le16(d + 4), i, best = -1, bw = -1;

	if ( 6 + n * 16 > size ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) {
		CONST UB *e = d + 6 + i * 16;
		INT	w = ( e[0] == 0 ) ? 256 : e[0];
		UW	len = le32(e + 8), off = le32(e + 12);

		if ( off + len > (UW)size || len < 16 ) {
			continue;
		}
		if ( w > bw ) {
			bw = w;
			best = i;
		}
	}
	if ( best < 0 ) {
		return E_PAR;
	}
	*p_img = d + le32(d + 6 + best * 16 + 12);
	*p_len = (SZ)le32(d + 6 + best * 16 + 8);
	return E_OK;
}

EXPORT ER img_ico_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	CONST UB *im;
	SZ	len;
	ER	er;

	if ( data == NULL || !is_ico(data, size) ) {
		return E_PAR;
	}
	er = ico_pick(data, size, &im, &len);
	if ( er < E_OK ) {
		return er;
	}
	if ( im[0] == 0x89 && im[1] == 'P' ) {
		return img_png_size(im, len, p_w, p_h);
	}
	if ( le32(im) < 40 ) {
		return E_NOSPT;
	}
	if ( p_w != NULL ) *p_w = (INT)le32(im + 4);
	if ( p_h != NULL ) *p_h = (INT)le32(im + 8) / 2;
	return E_OK;
}

EXPORT ER img_ico_decode( CONST UB *data, SZ size, UW **p_pixels, INT *p_w, INT *p_h )
{
	CONST UB *im, *pal = NULL, *xorp, *andp;
	SZ	len;
	INT	w, h, bpp, npal, xstride, astride, x, y;
	UW	*px;
	BOOL	alpha = FALSE;
	ER	er;

	if ( data == NULL || p_pixels == NULL || !is_ico(data, size) ) {
		return E_PAR;
	}
	er = ico_pick(data, size, &im, &len);
	if ( er < E_OK ) {
		return er;
	}
	if ( im[0] == 0x89 && im[1] == 'P' ) {
		return img_png_decode(im, len, p_pixels, p_w, p_h);
	}
	if ( len < 40 || le32(im) < 40 || le32(im + 16) != 0 ) {
		return E_NOSPT;			/* only the uncompressed kind */
	}
	w = (INT)le32(im + 4);
	h = (INT)le32(im + 8) / 2;
	bpp = (INT)le16(im + 14);
	if ( w <= 0 || h <= 0 || w > 256 || h > 256
	  || ( bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32 ) ) {
		return E_NOSPT;
	}
	npal = ( bpp <= 8 ) ? ( ( le32(im + 32) != 0 ) ? (INT)le32(im + 32) : ( 1 << bpp ) ) : 0;
	pal = im + le32(im);
	xorp = pal + npal * 4;
	xstride = ( ( w * bpp + 31 ) / 32 ) * 4;
	astride = ( ( w + 31 ) / 32 ) * 4;
	andp = xorp + (SZ)xstride * h;
	if ( andp + (SZ)astride * h > im + len ) {
		andp = NULL;			/* some leave the mask out */
		if ( xorp + (SZ)xstride * h > im + len ) {
			return E_PAR;
		}
	}
	px = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
	if ( px == NULL ) {
		return E_NOMEM;
	}
	if ( bpp == 32 ) {
		/* a picture with its own transparency says so in every pixel */
		for ( y = 0; y < h && !alpha; y++ ) {
			for ( x = 0; x < w; x++ ) {
				if ( xorp[(SZ)y * xstride + x * 4 + 3] != 0 ) {
					alpha = TRUE;
					break;
				}
			}
		}
	}
	for ( y = 0; y < h; y++ ) {
		CONST UB	*row = xorp + (SZ)( h - 1 - y ) * xstride;
		CONST UB	*mrow = ( andp != NULL ) ? andp + (SZ)( h - 1 - y ) * astride : NULL;
		UW		*o = px + (SZ)y * w;

		for ( x = 0; x < w; x++ ) {
			UW	c;
			BOOL	clear;

			if ( bpp <= 8 ) {
				INT	bit = x * bpp;
				UW	i = ( row[bit >> 3] >> ( 8 - bpp - ( bit & 7 ) ) ) & ( ( 1U << bpp ) - 1 );
				CONST UB *e = pal + ( ( (INT)i < npal ) ? i : 0 ) * 4;

				c = ( (UW)e[2] << 16 ) | ( (UW)e[1] << 8 ) | e[0];
			} else {
				CONST UB *e = row + x * ( bpp / 8 );

				c = ( (UW)e[2] << 16 ) | ( (UW)e[1] << 8 ) | e[0];
			}
			if ( alpha ) {
				clear = (BOOL)( row[x * 4 + 3] < 128 );
			} else {
				clear = (BOOL)( mrow != NULL && ( mrow[x >> 3] & ( 0x80 >> ( x & 7 ) ) ) != 0 );
			}
			o[x] = clear ? IMG_CLEAR : c;
		}
	}
	*p_pixels = px;
	if ( p_w != NULL ) *p_w = w;
	if ( p_h != NULL ) *p_h = h;
	return E_OK;
}

/* ---------------------------------------------------------------- any of them */

EXPORT ER img_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	if ( data == NULL || size < 4 ) {
		return E_PAR;
	}
	if ( data[0] == 0x89 && data[1] == 'P' ) return img_png_size(data, size, p_w, p_h);
	if ( data[0] == 0xFF && data[1] == 0xD8 ) return img_jpeg_size(data, size, p_w, p_h);
	if ( data[0] == 'B' && data[1] == 'M' ) return img_bmp_size(data, size, p_w, p_h);
	if ( data[0] == 'G' && data[1] == 'I' && data[2] == 'F' ) return img_gif_size(data, size, p_w, p_h);
	if ( is_ico(data, size) ) return img_ico_size(data, size, p_w, p_h);
	return E_NOSPT;
}

EXPORT ER img_decode( CONST UB *data, SZ size, UW **p_pixels, INT *p_w, INT *p_h )
{
	if ( data == NULL || size < 4 ) {
		return E_PAR;
	}
	if ( data[0] == 0x89 && data[1] == 'P' ) return img_png_decode(data, size, p_pixels, p_w, p_h);
	if ( data[0] == 0xFF && data[1] == 0xD8 ) return img_jpeg_decode(data, size, p_pixels, p_w, p_h);
	if ( data[0] == 'B' && data[1] == 'M' ) return img_bmp_decode(data, size, p_pixels, p_w, p_h);
	if ( data[0] == 'G' && data[1] == 'I' && data[2] == 'F' ) return img_gif_decode(data, size, p_pixels, p_w, p_h);
	if ( is_ico(data, size) ) return img_ico_decode(data, size, p_pixels, p_w, p_h);
	return E_NOSPT;
}
