/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_hw.h
 *	What the part of the screen driver that knows the machine hands
 *	over to the part that does not (design 16.5).
 */

#ifndef __TS_DISP_HW_H__
#define __TS_DISP_HW_H__

#include <ts/dispout.h>

struct t_dispouthw;

typedef struct {
	UINT	width;
	UINT	height;
	UINT	pitch;			/* bytes from one row to the next */
	UINT	bpp;
	UINT	format;			/* DISP_FMT_* */
	UD	fb_pa;			/* where the pixels are, physically */
	UBINT	fb_va;			/* and how this kernel reaches them */
	UINT	alpha;			/* DISP_ALPHA_*: what the top byte must hold */
	CONST char *via;		/* how it was found, for the console */

	/*
	 * Two screens' worth of rows, one under the other, of which the
	 * screen shows either: what is sent goes to the one not shown,
	 * which is then shown (disp.c). Left 0 and NULL by a screen that
	 * has one.
	 *
	 *   pages	screens' worth of rows mapped from fb_va: 1 or 2
	 *   flip	the screen to show page 0 or 1 from its next refresh
	 *   vsync	wait for the next vertical blank; NULL when there is
	 *		no way to
	 *   latch_us	how long after a flip it has surely been taken
	 *		without waiting for a blank; 0 at once
	 *   flip_start	whether to flip from the start (else only when asked,
	 *		ts_disp_flip)
	 */
	UINT	pages;
	ER	(*flip)( UINT page );
	ER	(*vsync)( void );
	UINT	latch_us;
	BOOL	flip_start;
	UBINT	page2_va;		/* the second page, when it is not the rows after the first; 0 */

	/*
	 * The refresh as a clock, where the screen can say when its blanks
	 * end (left 0 and NULL where it cannot). With it a send that comes
	 * after the flip before has surely been taken does not wait at all,
	 * and one that comes long before the blank sleeps until just before
	 * it.
	 *
	 *   frame_ns	the time from the end of one blank to the end of the next
	 *   blank_ns	how long a blank lasts
	 *   blank_seen	when the end of a blank was last seen, in ns of
	 *		ts_get_mono; 0 when none has been
	 */
	UD	frame_ns;
	UD	blank_ns;
	UD	(*blank_seen)( void );

	/* told after each send to the framebuffer (the panel's self refresh made to leave); NULL none */
	void	(*dirty)( void );

	/* the output this framebuffer is shown on (dispout.c); NULL: one the firmware set up */
	CONST struct t_dispouthw *out;
} T_DISPHW;

/*
 * An output as its driver describes it to the layer of outputs
 * (dispout.c, docs/tessronos 10.4.9): the connector, how to read its EDID
 * and what is plugged in, and how to set it to a mode. ctx is given back
 * to each call. A NULL function is a thing the driver does not do.
 *
 *   detect	DISP_ST_*: whether something is plugged in
 *   get_edid	the EDID into buf, up to max bytes (the base block and its
 *		extensions): how many, 0 when there is none
 *   modes	the driver's own modes, where there is no EDID: how many
 *   check	whether it can drive mode m, nothing touched
 *   commit	set to mode m (NULL: off), hw made what its framebuffer then is;
 *		an output besides the main one only (the main one's size goes
 *		by knl_disp_hw_setmode)
 *
 * The places below are kept for what a driver of real hardware adds
 * (Intel's, 06-device.md 6.10); NULL for the others:
 *
 *   power	the display on and off (as DPMS does), the mode kept
 *   brightness	a built-in panel's backlight, per mille
 *   invalidate	the framebuffer shown is about to be written in r (front
 *		buffer tracking: a panel's self refresh to leave)
 *   flushed	and has been, or another page has been shown (r the whole)
 *   wm_again	the watermarks and the display buffer to be worked out
 *		again (the planes or the pipes changed)
 *
 * And after ctx, for an output that can mirror the main screen in a mode
 * of another size with a scaler of its own (Intel's pipe scaler):
 *
 *   fit	whether a framebuffer of src_w x src_h can be shown scaled
 *		into mode m (its ratio kept, in the middle, black round it),
 *		nothing touched when set is FALSE; with set TRUE, the next
 *		commit of m makes it so and gives a framebuffer of that size.
 *		src as large as m: no scaling. E_NOSPT past the scaler's
 *		limits. NULL: the send scales instead (disp.c).
 */
typedef struct t_dispouthw {
	UINT	kind;			/* DISP_CON_* */
	CONST char *name;		/* "Virtual-1", "HDMI-A-1", ... */
	INT	(*detect)( void *ctx );
	INT	(*get_edid)( void *ctx, UB *buf, INT max );
	INT	(*modes)( void *ctx, T_DISPMODE *m, INT max );
	ER	(*check)( void *ctx, CONST T_DISPMODE *m );
	ER	(*commit)( void *ctx, CONST T_DISPMODE *m, T_DISPHW *hw );
	ER	(*power)( void *ctx, BOOL on );
	ER	(*brightness)( void *ctx, INT permille );
	void	(*invalidate)( void *ctx, CONST T_DISPRECT *r );
	void	(*flushed)( void *ctx, CONST T_DISPRECT *r );
	void	(*wm_again)( void *ctx );
	void	*ctx;
	ER	(*fit)( void *ctx, CONST T_DISPMODE *m, UINT src_w, UINT src_h, BOOL set );
} T_DISPOUTHW;

/*
 * The i-th output's framebuffer on the screen driver (0 the main one, 1 or
 * more those besides it in the order they were put, as
 * knl_disp_out_attach numbers them) and where it was put: E_NOEXS past
 * the last. knl_disp_out_screen: the screen it shows (ts_disp_screen's
 * number), -1 for one that mirrors the main screen or is no output.
 */
IMPORT ER  knl_disp_screen_hw( INT out, T_DISPHW *hw, INT *p_side, INT *p_offset );
IMPORT INT knl_disp_out_screen( INT out );
IMPORT INT knl_disp_out_count( void );		/* outputs besides the main one on the driver */

/*
 * The rectangle a picture of src_w x src_h stands in when shown in one
 * of dst_w x dst_h: as large as it goes with its ratio kept, in the
 * middle (the bars round it are black); the whole when the ratios are
 * the same. For the send's scaling and a scaler's window alike.
 */
IMPORT void knl_disp_fit( UINT src_w, UINT src_h, UINT dst_w, UINT dst_h, T_DISPRECT *r );

/* A driver's word that something was plugged into the output or taken out (dispout.c) */
IMPORT void knl_disp_out_hotplug( CONST T_DISPOUTHW *ops );

/*
 * An output the driver drives that shows no screen yet put on the list,
 * after those of the screens (dispout.c): off, with its state and EDID as
 * its functions say. One whose check refuses every mode stays off.
 */
IMPORT ER knl_disp_out_add( CONST T_DISPOUTHW *ops );

/*
 * A refresh made up for a screen that has none to wait for (QEMU's), so
 * that the waits for the blank and the pace they set can be tried
 * there: a blank of a twentieth of the period ends every period_us from
 * the call, and a flip is taken at the end of the first blank that
 * begins after it, as on a screen that has one. Flipping must be on.
 * 0 puts the screen back as it was.
 */
IMPORT ER knl_disp_sim_blank( UINT period_us );

/*
 * The byte above the colour in a four byte pixel. Most screens do not
 * look at it; one that takes it for opacity has to be sent it the way
 * it counts, whatever the drawing left there.
 */
#define DISP_ALPHA_ANY		0	/* not looked at: sent as drawn */
#define DISP_ALPHA_ZERO		1	/* 0 is opaque */
#define DISP_ALPHA_FF		2	/* 0xFF is opaque */

/*
 * Find the screen, set a mode, and map its memory. Answers 1 when one
 * was found, 0 when the machine has none, or an error.
 *
 * The mapping has to let writes merge and reorder, which is what
 * ordinary non-cacheable memory does. The device window will not do: it
 * forbids both, so every word written goes to the bus on its own.
 */
IMPORT INT knl_disp_hw_init( T_DISPHW *hw );

/*
 * The screen set to another size, w x h, as the machine's settings ask
 * (システム環境設定). 'hw' is what init gave and is changed to what the
 * new size is; its pixels are mapped anew, two pages of them when it
 * had two and the new size leaves room for them. Answers 1 when it took, 0
 * when this screen cannot be set to that size (it keeps the one it had),
 * or an error.
 */
IMPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h );

/*
 * Outputs besides the main one (10.4.8). Each shows a rectangle of the
 * back buffer of its own size, beside the screens before it, or the main
 * screen's (DISP_SIDE_MIRROR, scaled where the sizes differ); the send
 * copies what changed there into its framebuffer. One with two pages and
 * flip (pages, flip, latch_us, vsync as for the main one) is flipped at
 * its own refresh (disp.c); page2_va as for the main one.
 *
 * knl_disp_hw_more is asked at start-up, after the main screen is set
 * up, for the i-th further output the driver found (0, 1, ...): 1 when
 * there is one, described in hw with where it goes in *p_side
 * (DISP_SIDE_*) and *p_offset; 0 when there are no more. main_w and
 * main_h are the main screen's size, which a driver may give its output
 * as its height. A machine whose driver finds none does not define it.
 *
 * knl_disp_out_attach adds one while the system runs: the back buffer is
 * made again round all the screens and the windows laid again, as for a
 * new size. Its number on the driver (knl_disp_screen_hw), or E_LIMIT
 * (DISP_SCREENS_MAX), E_NOSPT (an offset less than 0), E_PAR (a
 * framebuffer that cannot be sent to, a side not known), E_NOMEM.
 */
IMPORT INT knl_disp_hw_more( INT i, UINT main_w, UINT main_h, T_DISPHW *hw, INT *p_side, INT *p_offset );
IMPORT INT knl_disp_out_attach( CONST T_DISPHW *hw, INT side, INT offset );

#endif /* __TS_DISP_HW_H__ */
