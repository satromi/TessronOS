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

/*
 * The panel's self refresh and the DC states (disp_igfx_psr.c). start
 * from the driver's start; dirty after each send to the framebuffer
 * (T_DISPHW dirty); hold TRUE before the plane changes, the panel goes
 * dark or the machine sleeps, or another pipe is lit, and FALSE after;
 * panic when the console draws on the screen itself; dc_active whether
 * the display's registers other than PSR's and the DMC's are not to be
 * read now; text for the records (idlelog=1)
 */
IMPORT void knl_igfx_psr_start( void );
IMPORT void knl_igfx_dirty( void );
IMPORT void knl_igfx_psr_hold( BOOL hold, CONST char *what );
IMPORT void knl_igfx_psr_panic( void );
IMPORT BOOL knl_igfx_dc_active( void );
IMPORT void knl_igfx_dc_pause( BOOL pause );	/* the DC states held off while the records read registers */
IMPORT INT  knl_igfx_psr_text( char *t );

/*
 * The watermarks of a pipe's primary plane and the shares of the display
 * buffer (DDB) of the pipe and its cursor, worked out from the memory
 * latencies (disp_igfx.c, knl_igfx_wm_calc): what is written to
 * PLANE_WM_1x(0-7), PLANE_BUF_CFG_1x and CUR_BUF_CFG_x
 */
#define IGFX_DBUF_BLOCKS	892		/* 896, 4 kept for the bypass path */
#define IGFX_WM_LEVELS		8

typedef struct {
	UW	wm[IGFX_WM_LEVELS];		/* the plane's levels, 0 for one off */
	UW	buf_cfg;			/* the plane's share: end - 1 << 16 | start */
	UW	cur_buf;			/* the cursor's */
	UW	share, cur;			/* their sizes, blocks */
	BOOL	sagv;				/* SAGV may stay on (one pipe, the deepest level long enough) */
} T_IGWM;

/*
 * For a linear plane w pixels wide of four byte pixels on a pipe of
 * htotal at khz, given the eight latencies (us, as taken: 0 after the
 * last level), the pipe's part [start, end) of the DDB and how many
 * pipes are on: the cursor's share at its end (at least 32 blocks with
 * one pipe, 8 with more), the rest the plane's. FALSE when it cannot be
 * worked out.
 */
IMPORT BOOL knl_igfx_wm_calc( CONST UH *lat, UINT w, UINT htotal, UD khz, UW start, UW end, INT npipes, T_IGWM *o );

/* The latencies as taken (8 of them): FALSE when the power controller did not give them */
IMPORT BOOL knl_igfx_latencies( UH *lat );

/*
 * Pipe A's part of the DDB made [0, a_end) for npipes pipes on, its
 * watermarks worked out again for it and written within a frame; SAGV
 * off with more than one pipe, and as the watermarks allow with one
 * (a_end IGFX_DBUF_BLOCKS: the whole buffer again). E_IO and put back
 * when the plane did not take it, E_NOSPT where the driver does not set
 * the watermarks.
 */
IMPORT ER   knl_igfx_pipes( INT npipes, UW a_end );

/* The panel's width (pipe A's mode), the GGTT aperture's physical address */
IMPORT UINT knl_igfx_panel_w( void );
IMPORT UD   knl_igfx_gmadr( void );

#endif /* __TS_DISP_PC_H__ */
