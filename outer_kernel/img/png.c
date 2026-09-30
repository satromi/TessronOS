/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	png.c
 *	Reading a PNG into pixels (for <image> in records)
 *
 *	The pictures in the records this system is given are PNG files
 *	beside the record: <uuid>_<record>_<n>.png. What is read here is
 *	eight bits to a sample, in any of the colour kinds -- grey, grey
 *	with alpha, colour, colour with alpha, and a palette with or
 *	without transparency -- and not interlaced. That covers what an
 *	editor writes; a file outside it is refused with E_NOSPT rather
 *	than drawn wrongly.
 *
 *	What comes back is 0x00rrggbb pixels, with the ones that are more
 *	than half transparent set to IMG_CLEAR, which is the colour the
 *	drawing layer leaves alone. A picture is either there or not at a
 *	pixel; mixing half-transparent pixels with what is under them is
 *	not something a window's surface can do without knowing what will
 *	be under it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/img.h>

#define PNG_MAX_SIDE	8192		/* larger than any screen this runs on */

LOCAL UW be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

/* The eight bytes every PNG starts with */
LOCAL CONST UB	png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

LOCAL BOOL is_png( CONST UB *data )
{
	INT	i;

	for ( i = 0; i < 8; i++ ) {
		if ( data[i] != png_sig[i] ) {
			return FALSE;
		}
	}

	return TRUE;
}

/* What the head of the file says */
typedef struct {
	UW	w, h;
	UB	depth, kind, interlace;
	UB	pal[256][4];		/* r, g, b, a */
	INT	npal;
} PNGHEAD;

/* The bytes per pixel a kind comes to at eight bits a sample */
LOCAL INT bpp_of( UB kind )
{
	switch ( kind ) {
	case 0:		return 1;		/* grey */
	case 2:		return 3;		/* colour */
	case 3:		return 1;		/* palette */
	case 4:		return 2;		/* grey and alpha */
	case 6:		return 4;		/* colour and alpha */
	default:	return 0;
	}
}

/* Walk the chunks, collecting the head, the palette and the data */
LOCAL ER walk( CONST UB *data, SZ size, PNGHEAD *hd, UB **p_z, SZ *p_zlen )
{
	SZ	at = 8, zlen = 0, zcap = 0;
	UB	*z = NULL;
	BOOL	have_head = FALSE;

	hd->npal = 0;
	while ( at + 12 <= size ) {
		UW	len = be32(data + at);
		CONST UB *type = data + at + 4;
		CONST UB *body = data + at + 8;

		if ( at + 12 + (SZ)len > size ) {
			break;			/* cut short: use what came */
		}
		if ( type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R' ) {
			if ( len < 13 ) {
				goto bad;
			}
			hd->w = be32(body);
			hd->h = be32(body + 4);
			hd->depth = body[8];
			hd->kind = body[9];
			hd->interlace = body[12];
			have_head = TRUE;
		} else if ( type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E' ) {
			INT	i, n = (INT)( len / 3 );

			if ( n > 256 ) n = 256;
			for ( i = 0; i < n; i++ ) {
				hd->pal[i][0] = body[i * 3];
				hd->pal[i][1] = body[i * 3 + 1];
				hd->pal[i][2] = body[i * 3 + 2];
				hd->pal[i][3] = 255;
			}
			hd->npal = n;
		} else if ( type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S' ) {
			INT	i;

			for ( i = 0; i < (INT)len && i < hd->npal; i++ ) {
				hd->pal[i][3] = body[i];
			}
		} else if ( type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T' ) {
			if ( zlen + len > zcap ) {
				SZ	ncap = ( zcap == 0 ) ? 65536 : zcap * 2;
				UB	*nz;

				while ( ncap < zlen + len ) {
					ncap *= 2;
				}
				nz = (UB *)Kmalloc(ncap);
				if ( nz == NULL ) {
					goto bad;
				}
				if ( z != NULL ) {
					knl_memcpy(nz, z, zlen);
					Kfree(z);
				}
				z = nz;
				zcap = ncap;
			}
			knl_memcpy(z + zlen, body, len);
			zlen += len;
		} else if ( type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D' ) {
			break;
		}
		at += 12 + (SZ)len;
	}
	if ( !have_head || z == NULL ) {
		goto bad;
	}
	*p_z = z;
	*p_zlen = zlen;

	return E_OK;

    bad:
	if ( z != NULL ) {
		Kfree(z);
	}
	return E_PAR;
}

LOCAL UB paeth( UB a, UB b, UB c )
{
	INT	p = (INT)a + (INT)b - (INT)c;
	INT	pa = ( p > a ) ? p - a : a - p;
	INT	pb = ( p > b ) ? p - b : b - p;
	INT	pc = ( p > c ) ? p - c : c - p;

	if ( pa <= pb && pa <= pc ) return a;
	if ( pb <= pc )             return b;
	return c;
}

/*
 * Undo the filters. Each row starts with a byte that says how it was
 * written: as it is, as the difference from the left, from above, from
 * their average, or from the Paeth guess of the three.
 */
LOCAL ER unfilter( UB *raw, UW w, UW h, INT bpp )
{
	SZ	stride = (SZ)w * bpp;
	UB	*prev = NULL;
	UW	y;

	for ( y = 0; y < h; y++ ) {
		UB	*row = raw + (SZ)y * ( stride + 1 );
		UB	f = row[0];
		UB	*cur = row + 1;
		SZ	i;

		for ( i = 0; i < stride; i++ ) {
			UB	a = ( i >= (SZ)bpp ) ? cur[i - bpp] : 0;
			UB	b = ( prev != NULL ) ? prev[i] : 0;
			UB	c = ( prev != NULL && i >= (SZ)bpp ) ? prev[i - bpp] : 0;

			switch ( f ) {
			case 0:	break;
			case 1:	cur[i] = (UB)( cur[i] + a );			break;
			case 2:	cur[i] = (UB)( cur[i] + b );			break;
			case 3:	cur[i] = (UB)( cur[i] + ( ( a + b ) >> 1 ) );	break;
			case 4:	cur[i] = (UB)( cur[i] + paeth(a, b, c) );	break;
			default: return E_PAR;
			}
		}
		prev = cur;
	}

	return E_OK;
}

EXPORT ER img_png_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	if ( data == NULL || size < 33 ) {
		return E_PAR;
	}
	if ( !is_png(data) ) {
		return E_PAR;
	}
	if ( p_w != NULL ) *p_w = (INT)be32(data + 16);
	if ( p_h != NULL ) *p_h = (INT)be32(data + 20);

	return E_OK;
}

EXPORT ER img_png_decode( CONST UB *data, SZ size, UW **p_pixels,
			  INT *p_w, INT *p_h )
{
	PNGHEAD	*hd;
	UB	*z = NULL, *raw = NULL;
	SZ	zlen = 0, rawlen;
	UW	*px = NULL;
	INT	bpp, got;
	UW	x, y;
	ER	er;

	if ( data == NULL || p_pixels == NULL || size < 33 ) {
		return E_PAR;
	}
	if ( !is_png(data) ) {
		return E_PAR;			/* not a PNG at all */
	}
	hd = (PNGHEAD *)Kmalloc(sizeof(PNGHEAD));
	if ( hd == NULL ) {
		return E_NOMEM;
	}
	er = walk(data, size, hd, &z, &zlen);
	if ( er < E_OK ) {
		Kfree(hd);
		return er;
	}
	bpp = bpp_of(hd->kind);
	if ( hd->depth != 8 || hd->interlace != 0 || bpp == 0
	  || hd->w == 0 || hd->h == 0
	  || hd->w > PNG_MAX_SIDE || hd->h > PNG_MAX_SIDE ) {
		er = E_NOSPT;
		goto out;
	}
	rawlen = (SZ)hd->h * ( (SZ)hd->w * bpp + 1 );
	raw = (UB *)Kmalloc(rawlen);
	if ( raw == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	got = ts_zlib_inflate(z, zlen, raw, rawlen);
	if ( got < (INT)rawlen ) {
		er = E_PAR;			/* shorter than the head said */
		goto out;
	}
	er = unfilter(raw, hd->w, hd->h, bpp);
	if ( er < E_OK ) {
		goto out;
	}
	px = (UW *)Kmalloc((SZ)hd->w * hd->h * sizeof(UW));
	if ( px == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	for ( y = 0; y < hd->h; y++ ) {
		CONST UB *row = raw + (SZ)y * ( (SZ)hd->w * bpp + 1 ) + 1;
		UW	*dst = px + (SZ)y * hd->w;

		for ( x = 0; x < hd->w; x++ ) {
			UB	r, g, b, a = 255;

			switch ( hd->kind ) {
			case 0:
				r = g = b = row[x];
				break;
			case 2:
				r = row[x * 3]; g = row[x * 3 + 1]; b = row[x * 3 + 2];
				break;
			case 3: {
				UB	i = row[x];

				if ( (INT)i >= hd->npal ) {
					r = g = b = 0;
				} else {
					r = hd->pal[i][0];
					g = hd->pal[i][1];
					b = hd->pal[i][2];
					a = hd->pal[i][3];
				}
				break;
			}
			case 4:
				r = g = b = row[x * 2];
				a = row[x * 2 + 1];
				break;
			default:
				r = row[x * 4]; g = row[x * 4 + 1];
				b = row[x * 4 + 2]; a = row[x * 4 + 3];
				break;
			}
			dst[x] = ( a < 128 ) ? IMG_CLEAR
					     : ( ( (UW)r << 16 ) | ( (UW)g << 8 ) | b );
		}
	}
	*p_pixels = px;
	if ( p_w != NULL ) *p_w = (INT)hd->w;
	if ( p_h != NULL ) *p_h = (INT)hd->h;
	px = NULL;
	er = E_OK;

    out:
	if ( px != NULL )  Kfree(px);
	if ( raw != NULL ) Kfree(raw);
	if ( z != NULL )   Kfree(z);
	Kfree(hd);

	return er;
}

/*
 * A picture scaled to fit a box, nearest pixel. The picture in a record
 * is drawn at the size of the box it was placed in, which is rarely the
 * size it was made at.
 */
EXPORT UW *img_scale( CONST UW *src, INT sw, INT sh, INT dw, INT dh )
{
	UW	*dst;
	INT	x, y;

	if ( src == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ) {
		return NULL;
	}
	dst = (UW *)Kmalloc((SZ)dw * dh * sizeof(UW));
	if ( dst == NULL ) {
		return NULL;
	}
	for ( y = 0; y < dh; y++ ) {
		INT	sy = (INT)( (D)y * sh / dh );

		for ( x = 0; x < dw; x++ ) {
			INT	sx = (INT)( (D)x * sw / dw );

			dst[(SZ)y * dw + x] = src[(SZ)sy * sw + sx];
		}
	}

	return dst;
}

/* ---------------------------------------------------------------- writing */

/*
 * A picture written as a PNG: eight bits a sample, red, green, blue and
 * how opaque, a pixel of IMG_CLEAR written as wholly clear. The data is
 * kept in stored blocks rather than compressed: the file is larger, and
 * what reads it has nothing to undo but the framing.
 */
LOCAL UW	png_crc_tab[256];
LOCAL BOOL	png_crc_ready = FALSE;

LOCAL UW png_crc( UW crc, CONST UB *p, SZ n )
{
	SZ	i;

	if ( !png_crc_ready ) {
		UW	c;
		INT	k, b;

		for ( k = 0; k < 256; k++ ) {
			c = (UW)k;
			for ( b = 0; b < 8; b++ ) {
				c = ( c & 1 ) ? 0xEDB88320U ^ ( c >> 1 ) : c >> 1;
			}
			png_crc_tab[k] = c;
		}
		png_crc_ready = TRUE;
	}
	for ( i = 0; i < n; i++ ) {
		crc = png_crc_tab[( crc ^ p[i] ) & 0xFF] ^ ( crc >> 8 );
	}

	return crc;
}

LOCAL void put32( UB *p, UW v )
{
	p[0] = (UB)( v >> 24 );
	p[1] = (UB)( v >> 16 );
	p[2] = (UB)( v >> 8 );
	p[3] = (UB)v;
}

/* A chunk: its length, its kind, what it holds, and the check of both */
LOCAL SZ chunk( UB *out, CONST char *kind, CONST UB *data, SZ n )
{
	UW	crc;
	SZ	i;

	put32(out, (UW)n);
	for ( i = 0; i < 4; i++ ) {
		out[4 + i] = (UB)kind[i];
	}
	for ( i = 0; i < n; i++ ) {
		out[8 + i] = data[i];
	}
	crc = png_crc(0xFFFFFFFFU, out + 4, n + 4) ^ 0xFFFFFFFFU;
	put32(out + 8 + n, crc);

	return n + 12;
}

EXPORT ER img_png_encode( CONST UW *px, INT w, INT h, UB **p_out, SZ *p_len )
{
	UB	*raw, *z, *out, hd[13];
	SZ	rawlen, zlen, nblk, at, i, o;
	UW	a = 1, b = 0;
	INT	x, y;

	if ( px == NULL || w <= 0 || h <= 0 || p_out == NULL || p_len == NULL ) {
		return E_PAR;
	}
	/* each row: its filter, none, then its pixels */
	rawlen = (SZ)h * ( 1 + (SZ)w * 4 );
	raw = (UB *)Kmalloc(rawlen);
	if ( raw == NULL ) {
		return E_NOMEM;
	}
	at = 0;
	for ( y = 0; y < h; y++ ) {
		raw[at++] = 0;
		for ( x = 0; x < w; x++ ) {
			UW	v = px[(SZ)y * w + x];

			if ( v == IMG_CLEAR ) {
				raw[at++] = 0;  raw[at++] = 0;  raw[at++] = 0;  raw[at++] = 0;
			} else {
				raw[at++] = (UB)( v >> 16 );
				raw[at++] = (UB)( v >> 8 );
				raw[at++] = (UB)v;
				raw[at++] = 0xFF;
			}
		}
	}
	/* zlib: its head, stored blocks of at most 65535, and the sum */
	nblk = ( rawlen + 65534 ) / 65535;
	if ( nblk == 0 ) {
		nblk = 1;
	}
	zlen = 2 + nblk * 5 + rawlen + 4;
	z = (UB *)Kmalloc(zlen);
	if ( z == NULL ) {
		Kfree(raw);
		return E_NOMEM;
	}
	o = 0;
	z[o++] = 0x78;
	z[o++] = 0x01;
	for ( i = 0; i < rawlen || i == 0; ) {
		SZ	n = rawlen - i;

		if ( n > 65535 ) {
			n = 65535;
		}
		z[o++] = (UB)( ( i + n >= rawlen ) ? 1 : 0 );
		z[o++] = (UB)n;
		z[o++] = (UB)( n >> 8 );
		z[o++] = (UB)~n;
		z[o++] = (UB)( ~n >> 8 );
		knl_memcpy(z + o, raw + i, n);
		o += n;
		i += n;
		if ( n == 0 ) {
			break;
		}
	}
	for ( i = 0; i < rawlen; i++ ) {
		a = ( a + raw[i] ) % 65521U;
		b = ( b + a ) % 65521U;
	}
	put32(z + o, ( b << 16 ) | a);
	o += 4;
	Kfree(raw);

	out = (UB *)Kmalloc(8 + 25 + ( o + 12 ) + 12);
	if ( out == NULL ) {
		Kfree(z);
		return E_NOMEM;
	}
	knl_memcpy(out, png_sig, 8);
	put32(hd, (UW)w);
	put32(hd + 4, (UW)h);
	hd[8] = 8;		/* bits a sample */
	hd[9] = 6;		/* red, green, blue and opacity */
	hd[10] = 0;
	hd[11] = 0;
	hd[12] = 0;
	at = 8;
	at += chunk(out + at, "IHDR", hd, 13);
	at += chunk(out + at, "IDAT", z, o);
	at += chunk(out + at, "IEND", NULL, 0);
	Kfree(z);
	*p_out = out;
	*p_len = at;

	return E_OK;
}
