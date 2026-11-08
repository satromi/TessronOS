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
 *
 *	Where the framebuffer has room for two screens and the screen can
 *	be told which to show, a send goes to the one not shown, which is
 *	shown once it is whole: nothing half sent is ever on the screen.
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
	UINT	fb_pages;		/* screens' worth of rows from fb_pa: 1, or 2 to flip between */
	UINT	fb_shown;		/* the one shown: its rows start fb_shown * height rows down */
	BOOL	flipping;		/* sends go to the one not shown, which is then shown */
	/*
	 * The scale the screen is drawn at, in percent, and the screen as
	 * the windows and programs see it: view pixels, each scale / 100
	 * pixels of the screen across and down (ts_disp_setscale). At 100
	 * the view is width by height.
	 */
	UINT	scale;
	UINT	view_w;
	UINT	view_h;
	UINT	scale_want;		/* the scale asked for; scale is less where the view would be too small */
} T_DISPSPEC;

/*
 * The scales a screen may be drawn at, in percent, and the least view
 * a scale may leave: a scale that would make the view smaller than
 * DISP_VIEW_MIN_W by DISP_VIEW_MIN_H is not used.
 */
#define DISP_NSCALE		7
#define DISP_SCALES		{ 100, 125, 150, 175, 200, 250, 300 }
#define DISP_VIEW_MIN_W		640
#define DISP_VIEW_MIN_H		480

/*
 * A length or a place in view pixels turned into pixels of the screen
 * at a scale: the view pixel v starts at pixel floor(v * scale / 100)
 * and the next one at floor((v + 1) * scale / 100). disp_view_of is the
 * other way: the view pixel the pixel u belongs to.
 */
static __inline__ INT disp_dev_of( INT v, UINT scale )
{
	D	p = (D)v * (D)scale;

	return (INT)( ( p >= 0 ) ? p / 100 : -( ( -p + 99 ) / 100 ) );
}

static __inline__ INT disp_view_of( INT u, UINT scale )
{
	D	p = (D)u * 100 + 99;

	return (INT)( ( p >= 0 ) ? p / (D)scale : -( ( -p + (D)scale - 1 ) / (D)scale ) );
}

/* The view of a screen w by h pixels at a scale: rounded up, so that no strip is left over */
static __inline__ UINT disp_view_len( UINT px, UINT scale )
{
	return (UINT)( ( (UD)px * 100 + scale - 1 ) / scale );
}

/*
 * The screen drawn at another scale (percent, one of DISP_SCALES). The
 * windows are laid again at it, their programs asked to draw again,
 * and the pointer kept within the new view. E_PAR for a scale not of
 * the list, E_NOSPT when it would leave the view smaller than the
 * least. What is asked is kept: a later size of the screen that can
 * take it uses it.
 */
IMPORT ER  ts_disp_setscale( UINT scale );

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

/*
 * The screens the view is shown on, side by side, the first being the main
 * one (10.4.8). The view is one surface; each screen shows its column of
 * it. How many; and the i-th one's rectangle in view coordinates, E_NOEXS
 * past the last. knl_disp_screens sets how many, 1 to DISP_SCREENS_MAX.
 */
#define DISP_SCREENS_MAX	4
IMPORT INT ts_disp_screens( void );
IMPORT ER  ts_disp_screen( INT i, INT *p_x, INT *p_y, INT *p_w, INT *p_h );
IMPORT void knl_disp_screens( INT n );

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
 * last hold is released, as a few separate rectangles (those near one
 * another joined into one).
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
 * Sends to the page not shown, then shown (TRUE), or to the page shown
 * (FALSE). The page not shown holds the screen as it was a send ago, so
 * each send also brings it the rectangles the last one sent; a flip
 * takes from the screen's next refresh, and a send waits until the last
 * flip has been taken before it writes the page that was shown.
 * E_NOSPT when the screen has no second page or cannot be told which to
 * show. The screen driver turns it on by itself where it was set up so
 * and a check at start-up passed (disp.c).
 */
IMPORT ER  ts_disp_flip( BOOL on );

/*
 * What has been sent since the counters were last read: rectangles, rows
 * and the number of times the screen was written to. Holding is meant to
 * keep the last of these small, so it is worth being able to see it.
 */
typedef struct {
	UD	sends;			/* times the screen was written to */
	UD	rects;			/* rectangles those covered */
	UD	rows;			/* rows of pixels copied */
	UD	pixels;			/* pixels copied */
	UD	send_us;		/* time spent copying them */
	UD	flips;			/* pages shown in turn */
	UD	waits;			/* sends that waited for the last flip to be taken */
	UD	wait_us;		/* and how long, all told */
} T_DISPSTAT;

IMPORT ER  ts_disp_stat( T_DISPSTAT *st );

/*
 * How the flips kept to the screen's refresh, counted from the start
 * (design 10.4.5). A refresh is the screen's own where it can say when
 * its blanks end, else one at 60 Hz. 'apart' sorts each flip by how
 * many refreshes came between it and the one before, rounded: within
 * half of one (two flips for one refresh), one, two, three, four or
 * more. 'phase' sorts the flips made where the blanks are known by
 * the quarter of the refresh they were made in, counted from the end
 * of the blank before. A send made while the flip before might not
 * have been taken yet either finds, by the clock of the blanks, that
 * it has been ('taken'), or waits for the blank at the screen
 * ('waited'), asleep until shortly before it where the clock says when
 * that is. 'off' counts the blanks seen more than DISP_BLANK_SLOP_US
 * from where the clock put them.
 */
#define DISP_BLANK_SLOP_US	300

typedef struct {
	UD	frame_ns;		/* the refresh; 0: the screen does not say */
	UD	flips;
	UD	apart[5];		/* 0 (within half a refresh), 1, 2, 3, 4 or more */
	UD	phase[4];
	UD	due;			/* sends made while the flip before might not have been taken */
	UD	taken;			/* of those, ones the clock said it had been */
	UD	waited;			/* ones that waited for the blank at the screen */
	UD	spin_us;		/* time spent watching the screen for it */
	UD	sleep_us;		/* time asleep before it */
	UD	off;
} T_DISPFRAME;

IMPORT ER  ts_disp_frames( T_DISPFRAME *st );

/*
 * Whether the sends keep to the refresh by themselves: with flipping on
 * and the screen able to say when a flip was taken, a send waits for the
 * flip before, so there is at most one a refresh. The refresh in us, or
 * 0 when the sends do not wait and whoever sends must keep the pace.
 */
IMPORT UINT ts_disp_paced( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DISP_H__ */
