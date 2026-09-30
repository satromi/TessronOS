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
} T_DISPHW;

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
 * new size is; its pixels are mapped anew. Answers 1 when it took, 0
 * when this screen cannot be set to that size (it keeps the one it had),
 * or an error.
 */
IMPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h );

#endif /* __TS_DISP_HW_H__ */
