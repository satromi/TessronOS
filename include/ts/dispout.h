/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispout.h
 *	The outputs a screen is shown on (docs/tessronos 10.4.9): what each
 *	one is (its connector, its EDID and the modes it offers) and the
 *	state they are set to together (which are on, in which mode, where in
 *	the view), checked as a whole without touching anything and then
 *	committed, the way a kernel mode setting's atomic check and commit go.
 */

#ifndef __TS_DISPOUT_H__
#define __TS_DISPOUT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* How a mode is timed, in pixels and lines */
typedef struct {
	UW	clock_khz;		/* the pixel clock */
	UH	hdisp, hss, hse, htotal;	/* shown, sync start and end, total */
	UH	vdisp, vss, vse, vtotal;
	UH	flags;			/* DISP_MODE_* */
	UH	vrefresh;		/* refreshes a second, rounded */
} T_DISPMODE;

#define DISP_MODE_PREFERRED	0x0001	/* the one the display says is its own */
#define DISP_MODE_INTERLACE	0x0002
#define DISP_MODE_PHSYNC	0x0004	/* positive syncs */
#define DISP_MODE_PVSYNC	0x0008
#define DISP_MODE_DTD		0x0010	/* where it came from: a detailed timing */
#define DISP_MODE_STD		0x0020	/* a standard timing */
#define DISP_MODE_EST		0x0040	/* an established timing */
#define DISP_MODE_VIC		0x0080	/* a CTA video identification code */
#define DISP_MODE_DRIVER	0x0100	/* the driver's own, without an EDID */

/* The kind of connector */
#define DISP_CON_VIRTUAL	0	/* an emulator's display */
#define DISP_CON_EDP		1	/* a built-in panel */
#define DISP_CON_DP		2
#define DISP_CON_HDMI		3
#define DISP_CON_USB		4
#define DISP_CON_FIRMWARE	5	/* a framebuffer the firmware set up, whatever is behind it */

/* Whether something is plugged in */
#define DISP_ST_UNKNOWN		0
#define DISP_ST_ON		1
#define DISP_ST_OFF		2

#define DISP_EDID_MAX		512	/* the base block and three extensions */
#define DISP_MODES_MAX		48

/* What an output is (ts_disp_output) */
typedef struct {
	INT	screen;			/* its screen number while it shows one, -1 when it shows none (off, or mirroring the main screen) */
	UINT	kind;			/* DISP_CON_* */
	UINT	status;			/* DISP_ST_* */
	char	name[16];		/* "Virtual-1", "eDP-1", "HDMI-A-1", ... */
	char	model[16];		/* the display's name from its EDID; "" */
	char	maker[4];		/* the three letters of its maker; "" */
	UH	product;
	UW	serial;
	UW	ident;			/* its maker, product and serial in one number: what its settings are kept by */
	UH	mm_w, mm_h;		/* its size, mm; 0 not known */
	UINT	edid_len;		/* bytes of EDID it gave, 0 none */
	BOOL	hdmi;			/* the EDID says HDMI (a CTA extension with HDMI's identifier) */
	BOOL	audio;			/* and that it plays sound */
	INT	nmodes;			/* how many modes it offers */
	T_DISPMODE mode;		/* the mode it is in; all 0 when off */
	INT	x, y;			/* where it is in the view, view pixels */
	BOOL	usable;			/* it can be turned on (an output whose driver cannot set it yet is listed, off) */
} T_DISPOUTINFO;

/* The state the outputs are to be set to together: one entry an output */
typedef struct {
	BOOL	on;
	UH	w, h;			/* its mode, by its size: the first of the list of that size */
	INT	side;			/* DISP_SIDE_*: an output besides the main one */
	INT	offset;
} T_DISPSET;

typedef struct {
	INT	n;			/* entries, one for each output, in the order ts_disp_output numbers them */
	T_DISPSET out[4];
} T_DISPSTATE;

/* The outputs there are, shown or not */
IMPORT INT ts_disp_outputs( void );

/* What the i-th is: E_NOEXS past the last */
IMPORT ER  ts_disp_output( INT i, T_DISPOUTINFO *info );

/* Its modes, the display's preferred first: how many, up to max */
IMPORT INT ts_disp_out_modes( INT i, T_DISPMODE *modes, INT max );

/* Its EDID as it gave it: how many bytes, up to max */
IMPORT INT ts_disp_out_edid( INT i, UB *buf, INT max );

/* The state now */
IMPORT ER  ts_disp_state( T_DISPSTATE *st );

/*
 * Whether the outputs can be set to st, nothing touched: E_OK, or E_PAR
 * (an entry that does not make sense), E_NOSPT (a mode an output does not
 * offer, a side or place not taken), E_LIMIT (more than the drivers can
 * drive at once)
 */
IMPORT ER  ts_disp_check( CONST T_DISPSTATE *st );

/*
 * The outputs set to st: checked first; an output that does not take its
 * part puts every output back as it was, and the error is answered
 */
IMPORT ER  ts_disp_commit( CONST T_DISPSTATE *st );

/*
 * The mode output i (1 or more) mirrors the main screen in
 * (DISP_SIDE_MIRROR) into *p_w, *p_h: the main screen's size where the
 * output offers it, else its preferred mode, the main screen scaled into
 * it. Answers what ts_disp_check answers for it now (E_NOSPT: past the
 * output's scaler's limits), E_NOEXS when there is no such output.
 */
IMPORT ER  ts_disp_mirror_mode( INT i, UINT *p_w, UINT *p_h );

/*
 * Who is told when an output is plugged in or taken out (the screen
 * object): called by the outputs' task, the output's number given
 */
IMPORT void knl_disp_out_watch( void (*fn)( INT out ) );

#ifdef __cplusplus
}
#endif
#endif /* __TS_DISPOUT_H__ */
