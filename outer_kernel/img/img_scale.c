/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	img_scale.c
 *	A picture made the size of the box it is drawn in (design 16.5)
 *
 *	Only memory is taken here, so the same code serves the kernel and a
 *	program running as a process (include/ts/tvport.h).
 */

#include <ts/tvport.h>
#include <ts/img.h>

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
