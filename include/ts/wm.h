/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wm.h
 *	Windows (design 16.4, 16.5.5, phase 10a)
 *
 *	A window is known by three rectangles, and only one of them is
 *	stored. The outer rectangle is where the window sits on the screen,
 *	frame and all. The work area is the part inside the frame that
 *	belongs to the program. The drawing rectangle is the work area
 *	before it was scrolled. Every conversion between them goes through
 *	the work area, and the work area is worked out from the outer
 *	rectangle and the window's attributes by one function that nothing
 *	else may write to. Keeping that in one place is what lets the frame
 *	be redesigned without hunting for its measurements.
 *
 *	Each window owns its pixels. Drawing into a window touches nothing
 *	but that window, and the screen is made by laying the windows over
 *	one another in order. A window that is entirely covered is not laid
 *	down at all.
 */

#ifndef __TS_WM_H__
#define __TS_WM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/dp.h>
#include <ts/look.h>
#include <ts/dbox.h>
#include <ts/uuid.h>
#include <ts/ob.h>

/*
 * What a window has. These are what decide the size of its frame, and
 * the frame is drawn from them and from the look table and from nothing
 * else.
 */
#define WM_ATTR_FRAME		0x0001	/* a frame around it at all */
#define WM_ATTR_TITLE		0x0002	/* a bar along the top with a name */
#define WM_ATTR_RESIZE		0x0004	/* its size can be changed */
#define WM_ATTR_RBAR		0x0008	/* a scroll bar down the right side */
#define WM_ATTR_BBAR		0x0010	/* one along the foot */
#define WM_ATTR_LBAR		0x0080	/* one down the left side as well */
/*
 * A popup: no frame, always in front of every ordinary window, never
 * given the input. A pixel whose top byte is WM_CLEAR's is not laid,
 * so what is under it shows through -- which is how a menu's half-tone
 * shadow falls on whatever it stands over.
 */
#define WM_ATTR_POPUP		0x0100
#define WM_CLEAR		0xFF000000U

#define WM_TITLE_MAX		128

/* Which of the three rectangles a call means */
#define WM_RECT_OUTER		0	/* the whole window on the screen */
#define WM_RECT_WORK		1	/* what belongs to the program */
#define WM_RECT_DRAW		2	/* the work area before scrolling */

/* What a window is, for a caller that asks */
typedef struct {
	UINT		attr;
	T_DPRECT	outer;		/* screen coordinates */
	T_DPRECT	work;		/* screen coordinates */
	T_DPPOINT	scroll;		/* how far the content is wound up */
	INT		gid;		/* the environment that draws into it */
	INT		z;		/* 0 is nearest the viewer */
	UINT		visible;
	INT		parent;		/* a subordinate window's window (wm_set_parent), 0 none */
} T_WMWIN;

/* ---------------------------------------------------------------- the look */

/*
 * Every colour, pattern, length and setting the system draws its own
 * furniture with is an entry in one numbered table (<ts/look.h>). The
 * numbers are fixed, because definitions carry them: a definition that
 * asks for 472 gets the knob of a scroll bar.
 *
 * The names below are what this system's own code says; each is the
 * number the table knows it by, so the two ways of naming an entry
 * cannot drift apart.
 */
#define WM_LOOK_FRAME		LK_OUTLINE
#define WM_LOOK_TITLE_ON	LK_ACTFRAME
#define WM_LOOK_TITLE_OFF	LK_INACTFRAME
#define WM_LOOK_TITLE_TEXT	LK_ACTWTFCOL
#define WM_LOOK_TITLE_BACK	LK_ACTWTBCOL
#define WM_LOOK_WORK		LK_MSGWHITE	/* white: paper, a box's inside */
#define WM_LOOK_LIGHT		LK_LIGHT
#define WM_LOOK_SHADOW		LK_SHADOW
#define WM_LOOK_BAR_BACK	LK_SBARBACK
#define WM_LOOK_BAR_KNOB	LK_SBARKNOB
#define WM_LOOK_BAR_MARK	LK_SBARTOMBO
#define WM_LOOK_BAR_OFF		LK_INACTFRAME
#define WM_LOOK_PART_FACE	LK_ACTPARTS
#define WM_LOOK_PART_OFF	LK_INACTPARTS
#define WM_LOOK_PART_TEXT	LK_ACTPARTSCOL
#define WM_LOOK_PART_TEXT_OFF	LK_INACTPARTSCOL
#define WM_LOOK_PART_EMPH	LK_PEMPHAS
#define WM_LOOK_LAMP_ON_IN	LK_LAMPON_IN
#define WM_LOOK_LAMP_ON_LT	LK_LAMPON_LT
#define WM_LOOK_LAMP_OFF_IN	LK_LAMPOFF_IN
#define WM_LOOK_LAMP_OFF_LT	LK_LAMPOFF_LT
#define WM_LOOK_DESK		LK_GROUND
#define WM_LOOK_TITLE_H		LK_TITLE_H
#define WM_LOOK_MENU_H		LK_MENU_H
#define WM_LOOK_DBLTIME		LK_DBLTIME
#define WM_LOOK_BAR_W		LK_BAR_W
#define WM_LOOK_PART_RAD	LK_PART_RAD

/* What a number holds: a colour, a pattern, or a plain number */
IMPORT UW  wm_look( UINT num );

/*
 * What a person sets (ユーザ環境設定): an entry written without counting
 * as a change of the scheme; the user information object's settings
 * taken into the table (uconf.c); and the person's name as it was at
 * the start.
 */
IMPORT ER  knl_look_setting( UINT num, UW value );
IMPORT void knl_input_sync( void );
IMPORT ER  wm_conf_apply( void );
IMPORT ER  wm_dev_conf_apply( void );	/* the machine's settings */
IMPORT ER  wm_dev_conf_put( CONST char *key, CONST char *val );
IMPORT BOOL wm_dev_conf_get( CONST char *key, char *v, INT max );
IMPORT ER  wm_conf_watch( void );		/* the settings objects taken again when written */

/* The buzzer, and the click of a press and of a let go (beep.c); a process may ask too */
#define WM_BEEP_BUZZER	0
#define WM_BEEP_PRESS	1
#define WM_BEEP_RELEASE	2
IMPORT ER  wm_beep( UINT what );
IMPORT INT wm_user_name( UB *buf, INT max );			/* 0 for a number that is not there */
IMPORT INT wm_num( UINT num, INT dflt );
IMPORT ER  wm_set_look( UINT num, UW value );
IMPORT ER  wm_set_look_tone( UINT num, INT tone, UW fore, UW back );
IMPORT ER  wm_look_pat( UINT num, T_DPPAT *pat );

/* ------------------------------------------------------- the look settings */

/*
 * The settings that change the shape of a frame rather than its ink.
 * They are entries of the same table, in the range kept for the
 * system's own settings.
 */
#define WM_SET_ROUND		LK_ROUND
#define WM_SET_SHADOW		LK_SHD_DARK
#define WM_SET_SHADOW_DX	LK_SHD_DX
#define WM_SET_SHADOW_DY	LK_SHD_DY
#define WM_SET_SHADOW_GROW	LK_SHD_SIZE
#define WM_SET_OPACITY		LK_OPACITY
#define WM_SET_WASH		LK_WASH
#define WM_SET_WASH_STR		LK_WASH_STR

IMPORT ER  wm_set( UINT num, INT value );
IMPORT INT wm_get( UINT num );		/* -1 for a number that holds nothing */

/* ---------------------------------------------------------------- schemes */

/*
 * A scheme is a whole set of colours at once. Putting one on writes the
 * numbers it names, draws every window's frame again, and gives every
 * program the whole of its work area to draw again -- which is the only
 * way a program that drew in the old colours hears about the new ones.
 *
 * The lengths (how tall a name is, how wide a bar) are not part of a
 * scheme: they change what fits where, and a person choosing a colour
 * is not asking for their windows to be re-laid.
 */
#define WM_SCHEME_LIGHT		0
#define WM_SCHEME_DARK		1
#define WM_SCHEME_CONTRAST	2
#define WM_SCHEME_BLUE		3
#define WM_SCHEME_MAX		4

IMPORT ER   wm_set_scheme( UINT n );
IMPORT UINT wm_scheme( void );
IMPORT BOOL wm_scheme_changed( void );
IMPORT CONST char *wm_scheme_name( UINT n );

/* ------------------------------------------------------- the wall */

/*
 * The picture the windows stand on. The pixels belong to the caller and
 * have to stay where they are until another wall is set or the wall is
 * taken away with NULL; nothing is copied, because a wall is the size
 * of the screen and copying it would double the largest thing in the
 * system.
 *
 * Laid from the top left, over and over (WM_WALL_TILE), once in the
 * middle (WM_WALL_CENTRE), or stretched to the screen (WM_WALL_FIT).
 * Where no wall is set the ground colour of the look table is used.
 */
#define WM_WALL_TILE		0
#define WM_WALL_CENTRE		1
#define WM_WALL_FIT		2

IMPORT ER  wm_set_wall( CONST UW *pixels, INT w, INT h, UINT mode );

/*
 * The same, from a picture on the disk. The pixels are read into pages
 * of their own and kept until another picture takes their place; a path
 * of NULL takes the picture off the ground and gives the pages back.
 */
IMPORT ER  wm_load_wall( CONST char *path, UINT mode );

/* And from a record of a real object holding such a picture (design 18.18) */
IMPORT ER  wm_load_wall_obj( CONST TS_UUID *uuid, INT recno, UINT mode );

/* ------------------------------------------------------- seeing through one */

/*
 * How much of what is behind a window shows through its frame. 255 is
 * the whole of the frame's own colour and nothing behind it.
 *
 * **The work area is never see-through.** Letters on a moving ground
 * cannot be read, and mixing every pixel of every window costs the
 * screen rather than the strip around it (design 16.5.5).
 */
IMPORT ER  wm_set_opacity( INT wid, UINT opacity );
IMPORT ER  wm_set_tint( INT wid, UW colour, UINT strength );
IMPORT ER  wm_opacity( INT wid, UINT *p_opacity );

/* Open a window and close it again. The rectangle is the outer one. */
IMPORT INT wm_open( CONST T_DPRECT *outer, UINT attr, CONST char *title );
IMPORT ER  wm_set_title( INT wid, CONST char *title );
IMPORT ER  wm_close( INT wid );

IMPORT ER  wm_ref( INT wid, T_WMWIN *w );

/*
 * Turn one of a window's rectangles into another. This is the only
 * conversion there is; everything that needs a measurement of the frame
 * asks here rather than working it out again.
 */
IMPORT ER  wm_convert( INT wid, UINT from, UINT to, CONST T_DPRECT *in,
		       T_DPRECT *out );

/* Move a window, resize it, or both. The rectangle is the outer one. */
IMPORT ER  wm_move( INT wid, CONST T_DPRECT *outer );

/* Bring a window to the front, or send it behind everything */
IMPORT ER  wm_raise( INT wid );
/*
 * A subordinate window: wid kept just in front of parent whatever is
 * raised or lowered (0: its own again). A window object is made one with
 * "sub":"<the parent window's UUID>" in its metadata.
 */
IMPORT ER  wm_set_parent( INT wid, INT parent );

/* The windows shown with a name in the band, nearest first (ウインドウ menu) */
IMPORT INT wm_list( INT *wids, INT max );
IMPORT INT wm_title( INT wid, char *buf, INT max );
IMPORT ER  wm_lower( INT wid );

IMPORT ER  wm_show( INT wid, BOOL on );

/* How far the content has been wound up inside the work area */
IMPORT ER  wm_set_scroll( INT wid, INT x, INT y );

/* ------------------------------------------------------------ scroll bars */

/*
 * A window's own scroll bars. Which one is meant by its side.
 */
#define WM_BAR_R		0	/* down the right side */
#define WM_BAR_B		1	/* along the foot */
#define WM_BAR_L		2	/* down the left side */
#define WM_BAR_MAX		3

/*
 * What a bar says: the whole of what there is to see (lo..hi) and the
 * part of it the work area is showing (clo..chi). The knob's length is
 * the one as a share of the other, so a bar needs no separate notion of
 * a page: making the window larger shows more, and the knob grows.
 */
typedef struct {
	INT	lo, hi;			/* everything there is */
	INT	clo, chi;		/* what is shown of it */
} T_WMBAR;

/*
 * A scroll bar drawn in the rectangle given, in whatever environment is
 * passed. A window's own bars are drawn through this, and so is the bar
 * of a scrolling selector inside a panel: a bar that looked different
 * depending on what it was attached to would be a second design.
 *
 * The rectangle is the whole of the bar, its lines included. 'across'
 * says it runs along the foot rather than down a side.
 */
IMPORT ER  wm_draw_bar( INT gid, CONST T_DPRECT *r, CONST T_WMBAR *b,
			BOOL across, BOOL active );

/* ------------------------------------------------------------ pictograms */

/* A pixel of a colour picture that is not drawn */
#define WM_PICT_CLEAR	0xFFFFFFFFU

/*
 * One pictogram from the data box, centred in the box given, in the
 * size nearest to it without going over. E_NOEXS when there is no such
 * picture, so that the caller may draw the standing mark instead.
 */
/*
 * Which picture stands in a window's title band, and whose pictures are
 * looked in for it. A window that is given none shows the standing
 * mark.
 */
IMPORT ER   wm_set_pict( INT wid, INT num, ID owner );
IMPORT ER   wm_set_icon( INT wid, CONST UW *px, INT w, INT h );	/* an object's icon in the band */

IMPORT ER   wm_pict_draw( INT gid, INT num, ID owner, CONST T_DPRECT *box,
			  UW ink );

/* The standing mark: a leaf of paper with its corner turned */
IMPORT void wm_pict_mark( INT gid, CONST T_DPRECT *box, UW ink, UW paper );

/* How many bytes one size of a picture comes to, head and rows together */
IMPORT SZ   wm_pict_bytes( CONST T_DBPICT *p );

/*
 * Where the pieces of a bar lie along a rail that long: the gap before
 * the knob, the knob's own length, and the middle of the mark across
 * it. The knob is never shorter than a third of the rail or sixteen,
 * whichever is less, and what that costs the rail is part of the
 * answer.
 */
IMPORT void wm_bar_pieces( CONST T_WMBAR *bar, INT len, BOOL small_knob,
			   INT *p_off, INT *p_len, INT *p_mark );

/* Where the knob sits in a track that long, and how long it is */
IMPORT void wm_bar_knob( CONST T_WMBAR *b, INT len, INT *p_off, INT *p_len );

IMPORT ER  wm_set_bar( INT wid, UINT which, CONST T_WMBAR *b );
IMPORT ER  wm_bar( INT wid, UINT which, T_WMBAR *b );

/*
 * Where a place on the screen falls on a bar: which part of it was hit.
 * Nothing moves by itself -- the answer says what was pressed, and
 * whoever owns the window decides what that means.
 */
#define WM_BARHIT_NONE		0
#define WM_BARHIT_KNOB		1	/* the knob itself */
#define WM_BARHIT_BEFORE	2	/* the track above it, or to its left */
#define WM_BARHIT_AFTER		3	/* the track below it, or to its right */

IMPORT INT wm_bar_at( INT wid, INT x, INT y, UINT *p_which );

/*
 * Wind a bar to where a place on the screen says, as dragging its knob
 * does: the knob's middle goes to that place, kept inside the track,
 * and clo/chi move with it keeping their distance apart.
 */
IMPORT ER  wm_bar_drag( INT wid, UINT which, INT x, INT y );

/* ------------------------------------------------------- taking hold of one */

/*
 * What part of a window a place on the screen falls on. A press on the
 * frame is not acted on here: this says what was pressed, and whoever
 * owns the window decides whether that means moving it, changing its
 * size, or nothing at all. The manager moving a window because the
 * frame was pressed is what makes a window system impossible to build
 * a program on top of.
 */
#define WM_PART_NONE		0	/* bare screen */
#define WM_PART_WORK		1	/* the program's own area */
#define WM_PART_TITLE		2	/* the band with the name: take hold and move */
#define WM_PART_BAR		3	/* one of the scroll bars */
#define WM_PART_GRIP_R		4	/* the right edge: make it wider */
#define WM_PART_GRIP_B		5	/* the foot: make it taller */
#define WM_PART_GRIP_BR		6	/* the corner: both at once */
#define WM_PART_FRAME		7	/* frame, but nothing that can be taken hold of */
#define WM_PART_PICT		8	/* the pictogram in the band: close it */

IMPORT INT wm_part_at( INT x, INT y, INT *p_wid, UINT *p_bar );

/*
 * A press on a window's pictogram at (sx, sy) on the screen, at 'when'
 * (ns): TRUE when it is the second of a double press there, which is
 * what closes a window.
 */
IMPORT BOOL wm_pict_close( INT wid, INT sx, INT sy, UD when );

/*
 * The least a window may be made: what its frame takes, and sixteen
 * pixels of work area in each direction. A window smaller than this has
 * a work area a program cannot draw anything useful in, and a frame
 * whose parts begin to overlap.
 */
IMPORT ER  wm_least( UINT attr, INT *p_wide, INT *p_high );

/*
 * The environment that draws into this window's own pixels. Its origin
 * is the top left of the work area and its frame is the work area, so a
 * program draws in its own coordinates and cannot reach outside.
 */
IMPORT INT wm_gid( INT wid );

/*
 * Say that a part of a window's work area needs drawing again, and ask
 * what is outstanding. A program draws what it is told about and then
 * says it is done.
 */
IMPORT ER  wm_damage( INT wid, CONST T_DPRECT *r );
IMPORT ER  wm_take_damage( INT wid, T_DPRECT *r );	/* E_NOEXS when none */

/* --------------------------------------------------------------- pointer */

/*
 * What the pointer looks like: a hand, and for most a mark by it that
 * says which way the action goes. The ordinary one points with the tip
 * of its finger; the others with the middle of their mark.
 */
#define WM_PT_SELECT	0	/* the ordinary pointer */
#define WM_PT_MODIFY	1	/* changing what is chosen */
#define WM_PT_MOVE	2	/* moving a thing */
#define WM_PT_VMOVE	3	/*   up and down only */
#define WM_PT_HMOVE	4	/*   sideways only */
#define WM_PT_GRIP	5	/* holding a thing */
#define WM_PT_VGRIP	6	/*   up and down only */
#define WM_PT_HGRIP	7	/*   sideways only */
#define WM_PT_RSIZ	8	/* changing a size, at its limit */
#define WM_PT_VRSIZ	9	/*   the height */
#define WM_PT_HRSIZ	10	/*   the width */
#define WM_PT_PICK	11	/* changing a size */
#define WM_PT_VPICK	12	/*   the height only */
#define WM_PT_HPICK	13	/*   the width only */
#define WM_PT_BUSY	14	/* waiting */
#define WM_PT_MENU	15	/* a menu is up */
#define WM_PT_NKIND	16

IMPORT ER   wm_set_pointer( UINT kind );
IMPORT UINT wm_pointer_kind( void );
IMPORT ER   wm_show_pointer( BOOL on );

/*
 * The pointer laid on the screen, over everything. The window manager
 * calls this at the end of building a screen; a program has no reason
 * to.
 */
IMPORT void wm_draw_pointer( INT gid );

/* ---------------------------------------------------------------- events */

/*
 * What arrived, and for whom. The place is measured from the corner of
 * that window's work area, which is what a program draws in, so nothing
 * has to convert it again.
 */
typedef struct {
	UINT	type;			/* HID_EV_* */
	UINT	code;
	UINT	mods;
	INT	x, y;			/* from the work area's corner */
	UD	when;
	INT	wid;			/* the window it is for, 0 for none */
	INT	dz;			/* a wheel (HID_EV_WHEEL): the notches it turned */
} T_WMEV;

/*
 * Which window a place on the screen belongs to: the one nearest the
 * viewer whose outer rectangle covers it. 0 when the place is bare
 * screen.
 */
IMPORT INT wm_at( INT x, INT y );

/*
 * Give a window the input. Which window has it decides where a key
 * goes; where the pointer is decides where a button goes, whatever has
 * the input.
 */
IMPORT ER  wm_focus( INT wid );
IMPORT INT wm_focused( void );

/*
 * Take the next thing that happened, already addressed to a window and
 * in that window's coordinates. E_TMOUT when nothing came.
 *
 * Pressing a button does not bring a window forward. The press is
 * carried to whoever the window belongs to, and coming forward is a
 * thing a program asks for.
 */
IMPORT ER  wm_read_event( T_WMEV *ev, TMO tmout );

/*
 * The ASCII character a key makes with the shift as in mods (HID_MOD_*),
 * on the 106/109 Japanese keyboard; 0 for a key that makes none. The
 * codes are the keyboard's usages, which say where the key is, not what
 * is printed on it.
 */
IMPORT UB  wm_key_char( UINT code, UINT mods );

/*
 * A menu a process opened on its window (mn_pop_men), worked from the
 * input as it arrives: the command chosen, 0 for none, when it is done.
 */
IMPORT INT knl_wmobj_menu_run( INT pid, UD when );

/* ------------------------------------------------------- the message line */

/*
 * The message line: a band along the foot of the screen, over the
 * windows, that windows are to be kept off. From the left it shows a
 * program's message, the keyboard's input mode, and the day of the week
 * with the time (a press on the clock shows the date for three
 * seconds). Whoever runs the desktop puts it up; until then the whole
 * screen is the windows'. The clock moves on as events are read.
 */
#define WM_MODE_ALPH	0x00000004	/* 英語 (else 日本語) */
#define WM_MODE_KANA	0x00000008	/* カタカナ; in 英語, capitals */
#define WM_MODE_HAN	0x00010000	/* letters typed half width */
#define WM_MODE_ROMAN	0x00020000	/* romaji typed (else kana keys) */

IMPORT ER  wm_msg_show( BOOL on );
IMPORT INT wm_msg_height( void );		/* 0 while it is not up */
IMPORT ER  wm_msg_put( CONST char *utf8 );	/* NULL takes it away */
IMPORT INT wm_msg_text( char *buf, INT max );	/* the message shown, "" none */
IMPORT ER  wm_msg_mode( UINT mode );		/* WM_MODE_* */
IMPORT UINT wm_msg_get_mode( void );		/* the mode shown */

/*
 * Lay the windows onto the screen. Everything above only changes what
 * is kept; this is what makes the screen agree with it.
 */
IMPORT ER  wm_composite( void );

/*
 * The same for one rectangle of it. What is laid down is the same --
 * the ground, then the windows from the back forward -- so a part built
 * this way is the part the whole would have had.
 */
IMPORT ER  wm_composite_at( CONST T_DPRECT *area );

/*
 * The pointer has moved to a place: where it was laid before and where
 * it is going are built again, and the rest of the screen is left
 * alone. Where it was is known here, so the caller does not carry it.
 */
IMPORT ER   wm_pointer_moved( INT to_x, INT to_y );

/*
 * What has changed since the last build, built again: every window's
 * outstanding damage and the pointer's old and new places, as one
 * rectangle round them all. The damage is taken as it is built.
 */
IMPORT ER   wm_update( void );

/*
 * The outlines of what is being carried, in screen coordinates: drawn
 * over every window, as the pointer is, so that they follow the hand
 * out of the window they came from. n 0 takes them away.
 */
#define WM_DRAG_MAX	32
IMPORT ER   wm_set_drag( CONST T_DPRECT *r, INT n );

/*
 * Where it was last laid, how large the square it was laid in was (the
 * size ユーザ環境設定 chose, which may have changed since), and whether it
 * has been laid at all
 */
IMPORT BOOL wm_pointer_last( INT *p_x, INT *p_y, INT *p_side );

/*
 * The square the pointer covers: laid with its corner at the place
 * wm_pointer_last gives, or, for wm_pointer_box, with its point at
 * (x, y) in the shape and size it has now. WM_POINTER_SIDE is the size
 * the shapes are drawn at, the middle of the three.
 */
#define WM_POINTER_SIDE	24
IMPORT void wm_pointer_box( INT x, INT y, T_DPRECT *r );

/*
 * Every window measured again after the name's height or the scroll
 * bars' width changed, and a number that changes each time, for a
 * program that draws its windows itself
 */
IMPORT void wm_relayout( void );
IMPORT UW   wm_layout_serial( void );

/*
 * Check that what is kept still makes sense: the order is a permutation
 * with no gaps, every window's work area is what the conversion says it
 * should be, and no window's surface is the wrong size for it. Answers
 * how many faults were found, so a test can require zero.
 */
IMPORT INT wm_self_check( void );

typedef struct {
	UD	composites;		/* times the screen was rebuilt */
	UD	windows_drawn;		/* windows laid down, over all of those */
	UD	windows_skipped;	/* ones found to be wholly covered */
} T_WMSTAT;

IMPORT ER  wm_stat( T_WMSTAT *st );

/* The size of the screen, 0 by 0 before the manager is up */
IMPORT void wm_screen( INT *p_w, INT *p_h );

/*
 * The screen as last built, copied into 'dst' (width by height pixels,
 * 'pitch' pixels to a row) while no building is under way, so that the
 * copy never holds half of one screen and half of the next.
 */
IMPORT ER  wm_copy_screen( UW *dst, INT pitch );

IMPORT INT knl_wm_init( void );

/* A process ended, on whichever path: the windows it made are closed (sub windows too) */
IMPORT void knl_wmobj_prc_end( ID pid );

/*
 * Windows as real objects (design 18.13). A window's drawing record is
 * xmlTAD, and reading xmlTAD is done above this layer, so what draws it
 * is handed in: given the window's drawing environment, its work area
 * and the bytes written, it draws them there. Until one is handed in a
 * written drawing shows as the bare work area.
 */
typedef ER (*FP_WMPAINT)( INT gid, CONST T_DPRECT *r, CONST UB *xml, SZ len );

/*
 * And what turns a shape record -- an xmlTAD figure -- into the region
 * of a window w by h: whatever the figure draws is inside.
 */
typedef ER (*FP_WMSHAPE)( CONST UB *xml, SZ len, INT w, INT h, T_DPRGN **p_rgn );

IMPORT void wm_obj_painter( FP_WMPAINT paint, FP_WMSHAPE shape );

/*
 * A window's own outline, in coordinates from its outer corner: what
 * is outside it is not drawn and is not the window's to be pressed on,
 * so a press there goes to what is behind. NULL gives it back its
 * plain (or rounded) rectangle. The region is copied.
 */
IMPORT ER   wm_set_shape( INT wid, CONST T_DPRGN *rgn );

/*
 * A window's UUID as an object, and the object it shows. A program that
 * opened a window itself says what is in it, so whoever lists the
 * windows can tell (the attributes carry it as "shows"); and it asks to
 * be told OB_E_CLOSE, since deleting such a window as an object is only
 * a request to its program to close it.
 */
IMPORT ER   wm_obj_uuid( INT wid, TS_UUID *p_uuid );
IMPORT INT  wm_obj_wid( CONST TS_UUID *uuid );		/* a window's number, E_NOEXS */
IMPORT ER   wm_obj_shows( INT wid, CONST TS_UUID *obj );
IMPORT ER   knl_wmobj_init( void );		/* after the object layer is up */

/*
 * Links let go of over a window that belongs to someone else (design
 * 16.5.21): the drop becomes the window's record OB_WR_DROP and is told
 * with OB_E_DROP, at (sx, sy) on the screen. The caller fills in each
 * link's target, identity, name and look; the taker is granted on each
 * object what the caller may do to it, within OB_DROP_OPS (T_OBDROPV
 * ops). E_NOSPT: nobody takes drops on that window.
 */
IMPORT ER   wm_obj_drop( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
			 CONST T_OBDROPV *v, INT n );

/*
 * A scroll bar of a window that belongs to someone else, worked by the
 * user: told with OB_E_SCROLL on its record OB_WR_BARS, code the bar
 * (WM_BAR_*), x the start it is asked to go to, y how (OB_SCR_*).
 */
IMPORT void wm_obj_scroll( INT wid, UINT which, INT pos, UINT how );

#ifdef __cplusplus
}
#endif

#endif /* __TS_WM_H__ */
