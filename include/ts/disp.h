/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp.h
 *	Screen (design 16.5, phase 9a)
 *
 *	Drawing goes into a back buffer in ordinary memory, never into the
 *	framebuffer itself, and what changed is sent across afterwards.
 *	There are two reasons and both matter.
 *
 *	The first is that the framebuffer is slow to write and slower to
 *	read, so nothing reads it: everything that needs to know what is on
 *	the screen reads the back buffer instead.
 *
 *	The second is that a single change on screen usually takes many
 *	calls, each of which would otherwise send its own small rectangle.
 *	Holding the screen gathers them into one rectangle and one send.
 */

#ifndef __TS_DISP_H__
#define __TS_DISP_H__

#ifdef __cplusplus
extern "C" {
#endif

/* How the bytes of one pixel are arranged */
#define DISP_FMT_NONE		0
#define DISP_FMT_XRGB8888	1	/* four bytes, blue lowest */
#define DISP_FMT_RGB565		2	/* two bytes */
#define DISP_FMT_XBGR8888	3	/* four bytes, red lowest */

/*
 * What the screen is. width to format describe the back buffer, which
 * is what everything draws into and reads: always four bytes a pixel,
 * DISP_FMT_XRGB8888. The screen itself may differ in the order of its
 * bytes, their number and its row stride; the send converts, and only
 * something that reads the framebuffer directly (a test) needs fb_*.
 */
typedef struct {
	UINT	width;
	UINT	height;
	UINT	pitch;			/* bytes from one row to the next */
	UINT	bpp;			/* bits in one pixel */
	UINT	format;			/* DISP_FMT_* */
	UD	fb_pa;			/* where the pixels are, physically */
	UINT	held;			/* how deep the holds are nested */
	UINT	fb_pitch;		/* the framebuffer's own row stride */
	UINT	fb_bpp;			/* its bits in one pixel */
	UINT	fb_format;		/* its DISP_FMT_* */
} T_DISPSPEC;

/* A part of the screen */
typedef struct {
	INT	left, top, right, bottom;	/* right and bottom are past the end */
} T_DISPRECT;

/*
 * Find the screen and make the back buffer. Answers 1 when one was
 * found, 0 when the machine has none, or an error.
 */
IMPORT INT knl_disp_init( void );

IMPORT ER  ts_disp_ref( T_DISPSPEC *spec );

/* Another size of screen: E_NOSPT when this one cannot be set to it */
IMPORT ER  ts_disp_setmode( UINT w, UINT h );

/*
 * The back buffer, which is where everything draws. The row stride is
 * the same as the screen's, so a drawing routine needs to know only one
 * of the two.
 */
IMPORT void *ts_disp_buffer( void );

/*
 * Say that a part of the back buffer changed. Nothing is sent while the
 * screen is held; the rectangles are gathered and go together when the
 * last hold is released.
 */
IMPORT ER  ts_disp_damage( CONST T_DISPRECT *r );

/*
 * Hold the screen and let it go again. These nest, so a routine that
 * holds can call another that also holds.
 */
IMPORT ER  ts_disp_hold( void );
IMPORT ER  ts_disp_release( void );

/* Send what is outstanding now, whether or not the screen is held */
IMPORT ER  ts_disp_flush( void );

/* Fill the whole back buffer with one colour and send it */
IMPORT ER  ts_disp_clear( UW colour );

/*
 * What has been sent since the counters were last read: rectangles, rows
 * and the number of times the screen was written to. Holding is meant to
 * keep the last of these small, so it is worth being able to see it.
 */
typedef struct {
	UD	sends;			/* times the screen was written to */
	UD	rects;			/* rectangles those covered */
	UD	rows;			/* rows of pixels copied */
} T_DISPSTAT;

IMPORT ER  ts_disp_stat( T_DISPSTAT *st );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DISP_H__ */
