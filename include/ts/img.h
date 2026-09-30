/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	img.h
 *	Pictures read from files (design 16.3, 17.5)
 *
 *	What a record's <image> points at is a PNG beside it; a program may
 *	also hand in a JPEG or a BMP. This layer turns one into pixels the
 *	drawing layer can lay down, and nothing else: it does not keep
 *	them, does not know where they came from, and does not draw.
 */

#ifndef __TS_IMG_H__
#define __TS_IMG_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A pixel that is not there. It is the same colour the drawing layer
 * leaves alone when laying a picture, so a picture with holes in it
 * can be laid as it is.
 */
#define IMG_CLEAR	0xFFFFFFFFU

/*
 * DEFLATE undone into a buffer the caller sized, and the same inside a
 * zlib wrapper. Answers how many bytes came out; a stream that would
 * run past the end of the buffer is refused rather than cut.
 */
IMPORT INT ts_inflate( CONST UB *in, SZ in_len, UB *out, SZ out_len );
IMPORT INT ts_zlib_inflate( CONST UB *in, SZ in_len, UB *out, SZ out_len );

/* How large a PNG says it is, without reading the rest */
IMPORT ER  img_png_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );

/*
 * A PNG as 0x00rrggbb pixels, IMG_CLEAR where it is more than half
 * transparent. The pixels are the caller's to Kfree. Eight bits a
 * sample and no interlacing; anything else is E_NOSPT.
 */
IMPORT ER  img_png_decode( CONST UB *data, SZ size, UW **p_pixels,
			   INT *p_w, INT *p_h );

/*
 * A sequential JPEG of eight-bit samples, grey or YCbCr, as the same
 * kind of pixels; progressive and arithmetic-coded files are E_NOSPT.
 */
IMPORT ER  img_jpeg_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );
IMPORT ER  img_jpeg_decode( CONST UB *data, SZ size, UW **p_pixels,
			    INT *p_w, INT *p_h );

/* An uncompressed BMP, likewise */
IMPORT ER  img_bmp_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );
IMPORT ER  img_bmp_decode( CONST UB *data, SZ size, UW **p_pixels,
			   INT *p_w, INT *p_h );

/* An icon file (ICO): the largest of its pictures, PNG or BMP, the mask as IMG_CLEAR */
IMPORT ER  img_ico_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );
IMPORT ER  img_ico_decode( CONST UB *data, SZ size, UW **p_pixels,
			   INT *p_w, INT *p_h );

/*
 * A GIF (87a, 89a): its first picture on the logical screen, what it
 * does not cover and its transparent colour as IMG_CLEAR
 */
IMPORT ER  img_gif_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );
IMPORT ER  img_gif_decode( CONST UB *data, SZ size, UW **p_pixels,
			   INT *p_w, INT *p_h );

/* Any of PNG, JPEG, BMP, GIF and ICO, known by how the file starts */
IMPORT ER  img_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );
IMPORT ER  img_decode( CONST UB *data, SZ size, UW **p_pixels,
		       INT *p_w, INT *p_h );

/* The same pixels at another size, nearest pixel; the caller's to Kfree */
IMPORT UW *img_scale( CONST UW *src, INT sw, INT sh, INT dw, INT dh );

/*
 * Pixels written as a PNG, IMG_CLEAR as wholly clear. What comes back
 * is the caller's to Kfree.
 */
IMPORT ER  img_png_encode( CONST UW *px, INT w, INT h, UB **p_out, SZ *p_len );

#ifdef __cplusplus
}
#endif

#endif /* __TS_IMG_H__ */
