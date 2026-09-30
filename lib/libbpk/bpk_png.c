/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_png.c
 *	Pixels as a PNG file
 *
 *	The picture is written without compressing it: the zlib stream is
 *	deflate's stored blocks, 65535 bytes at most each, then the Adler-32
 *	of the rows. Every row starts with filter 0. A picture with a mask
 *	is RGBA, one without RGB, eight bits a channel.
 */

#include <ts/bpk.h>
#include <stdlib.h>
#include <string.h>

#define STORED_MAX	65535

LOCAL UINT crc_table[256];
LOCAL BOOL crc_made;

LOCAL void crc_make( void )
{
	UINT	c, n, k;

	for ( n = 0; n < 256; n++ ) {
		c = n;
		for ( k = 0; k < 8; k++ ) c = ( c & 1 ) ? 0xEDB88320U ^ ( c >> 1 ) : c >> 1;
		crc_table[n] = c;
	}
	crc_made = TRUE;
}

LOCAL UINT crc32( const UB *p, UINT n )
{
	UINT	c = 0xFFFFFFFFU, i;

	if ( !crc_made ) crc_make();
	for ( i = 0; i < n; i++ ) c = crc_table[( c ^ p[i] ) & 0xFF] ^ ( c >> 8 );
	return c ^ 0xFFFFFFFFU;
}

LOCAL UB *be32( UB *p, UINT v )
{
	p[0] = (UB)( v >> 24 );
	p[1] = (UB)( v >> 16 );
	p[2] = (UB)( v >> 8 );
	p[3] = (UB)v;
	return p + 4;
}

/* A chunk's length and type written at p; its data is to follow */
LOCAL UB *chunk_head( UB *p, UINT len, const char *type )
{
	p = be32(p, len);
	memcpy(p, type, 4);
	return p + 4;
}

/* The CRC of the chunk whose type starts at t and whose data ends at p */
LOCAL UB *chunk_end( UB *t, UB *p )
{
	return be32(p, crc32(t, (UINT)( p - t )));
}

EXPORT UB *bpk_png_encode( const UINT *px, INT w, INT h, BOOL alpha, INT *p_len )
{
	UINT	bpp = alpha ? 4 : 3, rowlen = 1 + (UINT)w * bpp, raw = rowlen * (UINT)h;
	UINT	nblk = ( raw + STORED_MAX - 1 ) / STORED_MAX, zlen, total, at, s1 = 1, s2 = 0;
	UB	*png, *p, *t;
	UINT	left = 0;
	INT	x, y;

	if ( nblk == 0 ) nblk = 1;
	zlen = 2 + nblk * 5 + raw + 4;
	total = 8 + ( 12 + 13 ) + ( 12 + zlen ) + 12;
	png = malloc(total);
	if ( png == NULL ) return NULL;

	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	p = png + 8;
	t = chunk_head(p, 13, "IHDR") - 4;
	p = t + 4;
	p = be32(p, (UINT)w);
	p = be32(p, (UINT)h);
	*p++ = 8;				/* bits a channel */
	*p++ = alpha ? 6 : 2;			/* RGBA or RGB */
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	p = chunk_end(t, p);

	t = chunk_head(p, zlen, "IDAT") - 4;
	p = t + 4;
	*p++ = 0x78;				/* deflate, 32 KB window, no dictionary */
	*p++ = 0x01;
	at = 0;
	for ( y = 0; y < h; y++ ) {
		for ( x = -1; x < w; x++ ) {
			UB	v[4];
			UINT	n, k;

			if ( x < 0 ) {
				v[0] = 0;		/* the row's filter */
				n = 1;
			} else {
				UINT	c = px[y * w + x];

				v[0] = (UB)( c >> 16 );
				v[1] = (UB)( c >> 8 );
				v[2] = (UB)c;
				v[3] = (UB)( c >> 24 );
				n = bpp;
			}
			for ( k = 0; k < n; k++ ) {
				if ( left == 0 ) {
					/* a stored block: final or not, its length and the length's complement */
					UINT	len = ( raw - at > STORED_MAX ) ? STORED_MAX : raw - at;

					*p++ = ( at + len >= raw ) ? 1 : 0;
					*p++ = (UB)len;
					*p++ = (UB)( len >> 8 );
					*p++ = (UB)~len;
					*p++ = (UB)( ~len >> 8 );
					left = len;
				}
				*p++ = v[k];
				s1 = ( s1 + v[k] ) % 65521;
				s2 = ( s2 + s1 ) % 65521;
				at++;
				left--;
			}
		}
	}
	p = be32(p, ( s2 << 16 ) | s1);
	p = chunk_end(t, p);

	t = chunk_head(p, 0, "IEND") - 4;
	p = chunk_end(t, t + 4);
	*p_len = (INT)( p - png );
	return png;
}
