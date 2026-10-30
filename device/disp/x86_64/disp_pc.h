/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_pc.h (x64 PC)
 *	The screens of a PC whose size can be changed while the system
 *	runs, under the UEFI's framebuffer (disp_gop.c): Intel's integrated
 *	graphics of the 9th generation, driving its panel (disp_igfx.c).
 */

#ifndef __TS_DISP_PC_H__
#define __TS_DISP_PC_H__

/* How a size of another shape than the panel's is laid on it */
#define DISP_FIT_ASPECT		0	/* as large as it goes keeping its shape, black bands beside */
#define DISP_FIT_STRETCH	1	/* over the whole panel */

/*
 * Whether the framebuffer at fb_pa is the plane of Intel's display
 * engine set up as this driver can change it; it says why not on the
 * console when it is not
 */
IMPORT BOOL knl_igfx_probe( UD fb_pa );

/* As knl_disp_hw_setmode: 1 taken, 0 not this size, or an error */
IMPORT INT  knl_igfx_setmode( T_DISPHW *hw, UINT w, UINT h );

/* The shape kept (DISP_FIT_*) set, and laid now or with the next size; < 0 only asks. Answers the one in use */
IMPORT INT  knl_igfx_fit( INT fit, BOOL now );

/* The sizes offered, up to max: how many */
IMPORT INT  knl_igfx_modes( UH (*m)[2], INT max );

/* What was done to the display engine since the start, as lines of text, for DISPLAY.TXT */
IMPORT INT  knl_igfx_log( char *buf, INT max );

/*
 * The display's side of how compressed surfaces are hashed
 * (CHICKEN_PAR1_1), set to the drawing driver's: called from its
 * start-up
 */
IMPORT void knl_idisp_hash_mode( BOOL new_hash );

#endif /* __TS_DISP_PC_H__ */
