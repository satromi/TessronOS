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
/*
 * A window that never has the keys: pressed, it comes forward, but the
 * keys stay with the window that had them, so that what a screen
 * keyboard types (wm_obj_key_put) goes there. wm_focus leaves such a
 * window out.
 */
#define WM_ATTR_NOFOCUS		0x0200
/*
 * A window that floats: an ordinary window in every other way (its
 * frame, the input), but in front of every window that does not float,
 * and behind the popups. A window brought forward comes up behind the
 * floating ones, as a picture-in-picture window stays over the others.
 */
#define WM_ATTR_FLOAT		0x0400
/*
 * A window that covers the whole screen, the message line too: while
 * one such window is open the message line is taken down, and when the
 * last of them closes -- its program closing it, or ending -- it is put
 * back, unless it was down already before the first came. スライド
 * shows its slides in one.
 */
#define WM_ATTR_SCREEN		0x0800

/*
 * A window of its own, as a person sees one: it comes forward with the
 * keys when pressed or opened at their asking, and the keys go to it
 * when the window in front of it closes. A window with a title bar is
 * one, and so is one over the whole screen; a panel, a menu and a part
 * of another window are not.
 */
#define WM_ATTR_OWN		( WM_ATTR_TITLE | WM_ATTR_SCREEN )

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
	UINT		visible;	/* shown as far as its program asked (wm_show) */
	INT		parent;		/* a subordinate window's window (wm_set_parent), 0 none */
	UINT		away;		/* a subordinate window kept out of sight while its group is not in use */
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
IMPORT ER  wm_cpu_conf_apply( void );	/* of them, the processors' clock alone (CPU_MHZ) */
IMPORT ER  wm_dev_conf_put( CONST char *key, CONST char *val );
IMPORT BOOL wm_dev_conf_get( CONST char *key, char *v, INT max );
IMPORT ER  wm_conf_watch( void );		/* the settings objects taken again when written */

/* The buzzer, and the click of a press and of a let go (beep.c); a process may ask too */
#define WM_BEEP_BUZZER	0
#define WM_BEEP_PRESS	1
#define WM_BEEP_RELEASE	2
#define WM_BEEP_TICK	3	/* the buzzer's tone, short: a count going down */
#define WM_BEEP_LONG	4	/* and long: the count at its end */
#define WM_BEEP_TEST	5	/* 〈試聴〉 of システム環境設定: 880Hz for half a second */
#define WM_BEEP_TONE	0x100	/* with one of the above: its tone, though it hold a WAV */
IMPORT ER  wm_beep( UINT what );
/*
 * The same, waiting for the start: plays though the speaker be switched
 * off, and answers E_OK when it sounds, E_NOSPT when no output can
 * (beep.c). A process asks through the system object's record 8
 * (OB_SYS_SOUND: PLAY and buzzer, press, release, tick, long or test,
 * then tone for WM_BEEP_TONE).
 */
IMPORT ER  wm_beep_try( UINT what );
IMPORT INT wm_user_name( UB *buf, INT max );			/* 0 for a number that is not there */
IMPORT INT wm_num( UINT num, INT dflt );
IMPORT ER  wm_set_look( UINT num, UW value );
IMPORT ER  wm_set_look_tone( UINT num, INT tone, UW fore, UW back );
IMPORT ER  wm_look_pat( UINT num, T_DPPAT *pat );
/*
 * A rectangle of a drawing environment filled with an entry of the look
 * table as the system fills with it: its tone or its stripes
 * (wm_look_pat), or the work area's colour for an entry not there. A
 * process calls it for an environment it may draw in (class 9 of the
 * calls), as the ground of a window of its own is drawn (LK_PNLGROUND).
 */
IMPORT ER  wm_fill_look( INT gid, CONST T_DPRECT *r, UINT num );

/* ------------------------------------------------------- the look settings */

/*
 * The windows' look (LK_LOOK..LK_OPA_WHOLE in <ts/look.h>): how solid
 * the frames are and whether the work area is seen through too, the
 * washes, the corners, the shadow, the spread of what shows through,
 * the motion. They are entries of the same table, in the range kept
 * for the system's own settings, and are on every window with a frame
 * as soon as they are set. wm_set checks one against its range
 * (cf_look_items) and makes the windows' shapes follow it; the screen
 * shows it from the next time it is built. knl_wm_look is the same,
 * answering 1 when the value changed and 0 when it already was that.
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
IMPORT ER  knl_wm_look( UINT num, INT value );

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
 * How much of what is behind a window shows through its frame: the
 * program's own say for one window, 255 (the whole of the frame's own
 * colour and nothing behind it) until it says otherwise. The look's
 * LK_OPACITY comes on top of it for a window with a frame.
 *
 * The work area is not seen through unless the look says the whole
 * window is (LK_OPA_WHOLE). Letters on a moving ground are hard to read,
 * and mixing every pixel of every window costs the screen rather than
 * the strip around it (design 16.5.5).
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

/*
 * Move a window, resize it, or both. The rectangle is the outer one,
 * and becomes where the window is asked to stand -- but for a move that
 * keeps the size of a window a smaller screen made smaller, which keeps
 * the size asked for. A change of the screen's size lays every window
 * from where it was asked to stand (design 10.2.2).
 */
IMPORT ER  wm_move( INT wid, CONST T_DPRECT *outer );
/*
 * A program's window placed (OB_WR_PLACE): as wm_move, but kept on the
 * screen above the message line, made smaller only when it is larger
 * than that; what was asked for is kept, and a screen with room for it
 * gives it back.
 */
IMPORT ER  wm_place( INT wid, CONST T_DPRECT *outer );

/* Bring a window to the front, or send it behind everything */
IMPORT ER  wm_raise( INT wid );
/*
 * A subordinate window: wid kept just in front of parent whatever is
 * raised or lowered (0: its own again). It is carried with parent when
 * parent is moved, and raising it raises parent with it. A window object
 * is made one with "sub":"<the parent window's UUID>" in its metadata.
 * While the window in use (the one with the keys, or else the nearest
 * with a name band) is of another group, it is out of sight (T_WMWIN's
 * away); it comes back, in front of parent, when parent's group is in
 * use again. A pop-up is never put out of sight. When parent closes, it
 * is its own window again and is shown.
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
IMPORT ER   wm_set_icon_default( CONST UW *px, INT w, INT h );	/* the band's icon of a window with none */

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
 * The corner at a window's foot that sizes it: a press on the frame
 * this near the right and the lower edges of its outer rectangle
 */
#define WM_GRIP_CORNER		20

/*
 * The handles a finger reaches farther (design 10.4.6): how many pixels
 * of the view a handle -- the corner of a window, the right edge and
 * the foot of a link, a picked shape's handle, a line's end -- is to be
 * taken farther out than it is for the pointer, for the press made at
 * 'when' (T_WMEV.when, T_OBNTM.when). It is ユーザ環境設定's length
 * (LK_TCH_HIT, 0.1 mm) on the screen the finger touched, made pixels by
 * the size the screen says it is, for a press of a finger on a touch
 * screen with the correction on (LK_TCH_ON); 0 for a mouse's, a pen's,
 * one put in and one not known, which are taken as they always were.
 * What is drawn does not change: only where a press counts as on it.
 */
IMPORT INT wm_hit_slop( UD when );

/*
 * How far into a thing len long a handle at its edge is taken: the
 * pointer's 'in', and with a finger 'slop' more (wm_hit_slop), but not
 * past a third of the thing, so that two thirds of anything are still
 * its body to carry and to open. Out of the thing, a handle is reached
 * the pointer's distance and slop more.
 */
static inline INT wm_hit_in( INT in, INT slop, INT len )
{
	INT	r = in + slop;

	if ( r > len / 3 ) r = len / 3;
	return ( r > in ) ? r : in;
}

/*
 * A press read (wm_read_event) noted as a finger's or not, by what the
 * input layer says of the event it gave last (knl_hid_taken_touch). The
 * window manager calls it for each press; a test that reads the input
 * layer itself calls it after taking a press.
 */
IMPORT void knl_wm_touch_note( UD when );

/*
 * The places of a window where a finger means the main button at once
 * (design 6.4.1): rectangles of its work area, in its coordinates, that
 * take the place of the whole list given before; none takes them away.
 * Elsewhere a finger waits to be known for a tap, a drag or a hold, which
 * is the second button; here it is pressed as it touches and stays
 * pressed until it lets go, and a hold is nothing. They go with the
 * window, moved or closed; what is sized away counts no more, and a
 * window in front of a place takes it. E_LIMIT: too many windows have
 * places (WM_PRESS_WINS).
 */
#define WM_PRESS_RECTS		256
#define WM_PRESS_WINS		8
IMPORT ER   wm_obj_press_area( INT wid, CONST T_DPRECT *r, INT n );

/*
 * Whether a finger touching (x, y) of the screen is on such a place. The
 * input layer asks as it takes the touch, so this takes no lock and waits
 * for nothing: a window being changed at that moment may be read either
 * way, for that one touch.
 */
IMPORT BOOL knl_wm_press_now( INT x, INT y );

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
 * How much wider and taller than its work area the outer edge of a
 * window of attr is: what to add to a panel's size for the window that
 * shows the panel whole.
 */
IMPORT void wm_frame_of( UINT attr, INT *p_wide, INT *p_high );

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

/*
 * A surface a process draws its window's work area in: pages of the
 * kernel's, as many pixels as the work area had when it was made, mapped
 * into that process at an address of the shared window, rows 'pitch'
 * bytes long. What is drawn there goes into the window's own pixels
 * only when the process puts it (wm_surf_put), a few rectangles at a
 * time and while no screen is being built, so the screen never shows a
 * page half drawn. A window has at most one, the last asked for; it
 * stays the size it was made at when the window changes size, until it
 * is asked for again. When it is replaced, when the window closes and
 * when the process ends, its pages go, and the addresses the process
 * had them at are left on the page of nothing: a write there goes
 * nowhere and does not fault.
 */
typedef struct {
	void	*addr;		/* where the process has it */
	INT	width, height;	/* in pixels */
	INT	pitch;		/* bytes from one row to the next */
	UW	serial;		/* different for each surface made */
} T_WMSURF;

#define WM_PUT_MAX	16	/* rectangles put at once */

IMPORT ER  wm_surf_get( INT wid, ID pid, T_WMSURF *s );
IMPORT ER  wm_surf_put( INT wid, ID pid, CONST T_DPRECT *r, INT n );

/* The surfaces a process had, let go of as it ends */
IMPORT void knl_wm_surf_prc_end( ID pid );

/* For a test: how many surfaces there are and the pages they hold; a window's, where its process has it */
IMPORT INT knl_wm_surf_count( UD *p_pages );
IMPORT ER  knl_wm_surf_ref( INT wid, ID *p_pid, UBINT *p_va, UD *p_pages );

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
 * The pointer drawn through a drawing environment, where the input layer
 * says it is. The window manager lays its own on the screen itself,
 * keeping what it covers; a program has no reason to call this.
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
 * the input. A window that never has the keys (WM_ATTR_NOFOCUS) is not
 * given them: E_OBJ, and the window that had them keeps them.
 */
IMPORT ER  wm_focus( INT wid );
IMPORT INT wm_focused( void );

/*
 * A window that never has the keys shown as the one that has them, for
 * as long as a panel it brought up stands over it (wm_panel_open_at with
 * it as the owner: the system's colour panel under a palette of its
 * program's). Its band is drawn as the band of the window with the
 * input, and that window's as one without, as when a window that can
 * take the keys brings the panel up and takes them. Put off again (on
 * FALSE), or closed, the window with the input is shown again. A window
 * that can take the keys is not shown so (E_OBJ): it is given them.
 */
IMPORT ER  wm_lit( INT wid, BOOL on );

/*
 * A window opened because of what a person did takes the keys; one that
 * opens by itself does not (design 10). A ticket is the count of what
 * has been pressed: it stays good until the next press, which says the
 * person has gone on to something else, and for WM_ACT_MS at most.
 * wm_act_age says how long ago the last press was, in milliseconds.
 */
#define WM_ACT_MS	60000
IMPORT UINT wm_act_ticket( void );
IMPORT BOOL wm_act_good( UINT ticket );
IMPORT UW   wm_act_age( void );

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
 * The key a command is: the key of the letter (or digit) a key types in
 * the latin layout in use, as it is on the keyboard of the keys as
 * written -- KEY_E (0x08) for the key that types e, wherever the layout
 * has it -- and any other key as it is. Ctrl and Alt with a key are
 * taken by this, as a command of the letter the key types, not of the
 * place the key is in: in Dvorak, Ctrl and the key that types e (D's
 * place, 0x07) is Ctrl+E, as Linux takes a command by the keysym and
 * Windows by the virtual key after the layout.
 */
static __inline__ UINT wm_key_cmd( UINT code )
{
	UB	c = wm_key_char(code, 0);

	if ( c >= 'a' && c <= 'z' ) return 0x04 + (UINT)( c - 'a' );
	if ( c >= 'A' && c <= 'Z' ) return 0x04 + (UINT)( c - 'A' );
	if ( c >= '1' && c <= '9' ) return 0x1E + (UINT)( c - '1' );
	if ( c == '0' ) return 0x27;
	return code;
}

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
 * The mode is the keyboard's and is worked by the window manager as the
 * keys come, in their order (outer_kernel/wm/msgline.c): each key's
 * notice carries it (T_OBNTM imode). wm_kbmode_start sets the mode the
 * machine's settings say to start in; wm_kbmode_key says whether a key
 * is one that sets the mode.
 */
IMPORT void wm_kbmode_start( void );
IMPORT BOOL wm_kbmode_key( UINT key, UINT mods );

/*
 * Lay the windows onto the screen. Everything above only changes what
 * is kept; this is what makes the screen agree with it.
 *
 * The screen is built by the window manager's compositor, a task of its
 * own, at most once a refresh (60 a second), whatever was asked for in
 * the meantime built together. wm_composite, wm_composite_at, wm_update
 * and wm_set_drag ask for a build and return once it is on the screen.
 * wm_composite builds all of it, for what changes all of it (the look,
 * the scheme, the wallpaper, the screen's size); for anything smaller
 * wm_update builds what has changed. What a window's environment draws
 * into it is known without being said (dp_set_watch), as are windows
 * opened, closed, moved, raised or given a new frame.
 */
IMPORT ER  wm_composite( void );

/*
 * The same for one rectangle of it. What is laid down is the same --
 * the ground, then the windows from the back forward -- so a part built
 * this way is the part the whole would have had.
 */
IMPORT ER  wm_composite_at( CONST T_DPRECT *area );

/*
 * The pointer has moved to a place: what it covered where it was laid
 * before is put back, and it is laid where the input layer says it is
 * now. No window is laid again for it; the two squares are all that is
 * sent. Where it was is known here, so the caller does not carry it.
 * It returns at once; the compositor lays the pointer at its next build.
 */
IMPORT ER   wm_pointer_moved( INT to_x, INT to_y );

/*
 * What has changed since the last build, built again: every window's
 * outstanding damage, as a few rectangles (those near one another
 * joined, those far apart built apart), and the pointer moved as
 * wm_pointer_moved moves it. The damage is taken as it is built.
 */
IMPORT ER   wm_update( void );

/*
 * wm_update without waiting for the build, for a caller that does not
 * draw again straight away (a window coming in step by step); wm_flush
 * waits until everything asked for so far, by anyone, is on the screen.
 */
IMPORT ER   wm_post( void );
IMPORT ER   wm_flush( void );

/*
 * The outlines of what is being carried, in screen coordinates: drawn
 * over every window, as the pointer is, so that they follow the hand
 * out of the window they came from. n 0 takes them away.
 */
#define WM_DRAG_MAX	32
IMPORT ER   wm_set_drag( CONST T_DPRECT *r, INT n );

/*
 * What the things about to be carried look like, taken off the screen
 * where they lie now (screen coordinates): each is then laid in place of
 * the outline wm_set_drag gives at the same place in the list, while that
 * outline keeps its size. Given when a carry begins and the person chose
 * to see windows moved as they go (LK_LIVE); let go with the outlines.
 */
IMPORT ER   wm_set_drag_from( CONST T_DPRECT *r, INT n );

/*
 * A picture of w by h pixels (at most WM_LOUPE_MAX each way) laid with
 * its corner at (x, y) over every window and the outlines, as the
 * pointer is; px NULL takes it away. It is part of the screen as built,
 * so what is read from under it shows it.
 */
#define WM_LOUPE_MAX	192
IMPORT ER   wm_set_loupe( CONST UW *px, INT w, INT h, INT x, INT y );

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
	UD	parts;			/* rectangles built, over all of those */
	UD	pixels;			/* pixels built, a spread's margin with them */
	UD	ptr_moves;		/* times the pointer was laid somewhere new */
	UD	ptr_built;		/* of those, ones that came with something built */
} T_WMSTAT;

IMPORT ER  wm_stat( T_WMSTAT *st );

/*
 * How the builds kept to the refresh, counted from the start (design
 * 10.4.5): the time from the start of one build to the start of the
 * next, where they come in a row (within WM_APART_MS); laying the
 * windows; sending what was built, which waits for the flip before to
 * be taken; asleep to keep a refresh between two builds where the sends
 * do not keep it themselves; and the callers that waited for a build
 * (wm_update and the like), a program putting a frame among them.
 */
#define WM_APART_MS	100

typedef struct {
	UD	builds;
	UD	apart;			/* builds begun within WM_APART_MS of the one before */
	UD	apart_us, apart_max;
	UD	late;			/* of those, ones begun more than a refresh and a half after it */
	UD	lay_us, lay_max;
	UD	send_us, send_max;
	UD	paced_us;
	UD	waits;
	UD	wait_us, wait_max;
} T_WMFRAMES;

IMPORT ER  wm_frames( T_WMFRAMES *st );

/* The size of the screen, 0 by 0 before the manager is up */
IMPORT void wm_screen( INT *p_w, INT *p_h );
IMPORT void wm_screen_main( INT *p_w, INT *p_h );	/* the main screen (10.4.8); wm_screen is the whole view */
IMPORT void wm_screen_at( INT x, INT y, T_DPRECT *r );	/* the screen a point of the view is on */

/*
 * The screen as last built, copied into 'dst' (width by height pixels,
 * 'pitch' pixels to a row) while no building is under way, so that the
 * copy never holds half of one screen and half of the next.
 */
IMPORT ER  wm_copy_screen( UW *dst, INT pitch );

/*
 * A rectangle of it, inside the screen, copied the same way into 'dst'
 * (its corner at r's, 'pitch' pixels to a row): with the pointer as it
 * lies on the screen, or without it, what it covers in its place.
 */
IMPORT ER  wm_grab_screen( UW *dst, INT pitch, CONST T_DPRECT *r, BOOL pointer );

/*
 * The windows a person can choose to share (wm_share_list), nearest the
 * viewer first: the shown windows with a name band that are subordinate
 * to none. A window is known by its number and the serial of that
 * opening, so a number used again by a later window is not taken for it.
 */
#define WM_SHARE_OWN		0x0001	/* a window of the caller's process */
#define WM_SHARE_SAMEPROG	0x0002	/* of a process of the caller's program */

typedef struct {
	INT	wid;
	UINT	serial;
	INT	gid;			/* the environment that draws into it */
	INT	pid;			/* the process that opened it; 0 a task of the kernel's */
	INT	w, h;			/* its work area, in view pixels */
	UINT	flags;			/* WM_SHARE_* */
	char	title[WM_TITLE_MAX];
} T_WMSHARE;

IMPORT INT knl_wm_nwin( void );
/* Up to max of them into v: how many were put there */
IMPORT INT wm_share_list( T_WMSHARE *v, INT max );

/*
 * Rows of a window's work area as its program last drew them, whatever
 * covers it on the screen (wm_grab_window). The window's own pixels are
 * read, so the frame and the band are not in it, nor the pointer.
 * rows rows from the row top, at most maxw pixels of each, go into dst,
 * 'pitch' pixels to a row; *p_w and *p_h are told the work area's size
 * as it is. E_NOEXS when that opening of the window has been closed.
 */
IMPORT ER  wm_grab_rows( INT wid, UINT serial, UW *dst, INT pitch, INT top, INT rows, INT maxw, INT *p_w, INT *p_h );

/* The call for a process: the whole work area, cut to maxw by maxh */
typedef struct {
	INT	wid;
	UINT	serial;
	UW	*dst;
	INT	pitch;
	INT	maxw, maxh;
	INT	w, h;			/* answered: the size copied */
} T_WMGRAB;

IMPORT ER  wm_grab_window( T_WMGRAB *g );

/*
 * The window in front with the windows subordinate to it (wm_set_parent),
 * at most max of them, the one they belong to first; 0 when no window
 * with a name band is shown.
 */
IMPORT INT wm_front_group( INT *wids, INT max );

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
/*
 * The object a window made as an object says it shows: TRUE with it in
 * *p_obj, FALSE when it said none. wm_obj_showing is the window nearest
 * the viewer that says it shows obj, 0 none.
 */
IMPORT BOOL wm_obj_shown( INT wid, TS_UUID *p_obj );
IMPORT INT  wm_obj_showing( CONST TS_UUID *obj );
/* The process that made a window object, 0 the system's or no such window */
IMPORT ID   wm_obj_maker( INT wid );
IMPORT INT  wm_obj_close_programs( ID *pids, INT max );
IMPORT INT  wm_obj_program_window( ID pid );
IMPORT ER   knl_wmobj_init( void );		/* after the object layer is up */

/*
 * The input told to no window's program for a while: while the desktop
 * asks something in a panel over a program's window, the keys and the
 * presses that answer it are the panel's alone. Nested; TRUE begins,
 * FALSE ends.
 */
IMPORT void wm_obj_quiet( BOOL on );

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
 * The same for links carried out of the window of the process giver: the
 * person let them go, but the rights are the two processes'. E_OACV when
 * giver or the taker may not read one of the objects; the taker is
 * granted nothing, and each link's ops says what both may do.
 * wm_obj_drop_may asks beforehand, with nothing dropped and the keys left
 * where they are: E_NOSPT nobody takes drops there, E_OACV as above;
 * target NULL asks only whether anyone takes them.
 */
IMPORT ER   wm_obj_drop_for( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
			     ID giver, CONST T_OBDROPV *v, INT n );
IMPORT ER   wm_obj_drop_may( INT wid, ID giver, CONST TS_UUID *target );

/*
 * Whether the window would take links let go at (sx, sy) on the screen,
 * asked of the one who takes its drops before the objects are made
 * (OB_E_ASK, OB_WR_ASK of ts/ob.h); the question's number in *p_seq.
 * E_NOSPT: nobody there is asked. wm_obj_asked says the answer: E_OK it
 * would take them, E_NOEXS it would not, E_TMOUT not answered yet, E_OBJ
 * the window has gone or been asked again since. wm_obj_ask_text asks the
 * same of letters (OB_ASK_LETTERS, of the one who takes its letters).
 */
IMPORT ER   wm_obj_ask( INT wid, INT sx, INT sy, UINT mods, UINT *p_seq );
IMPORT ER   wm_obj_ask_text( INT wid, INT sx, INT sy, UINT mods, UINT *p_seq );
IMPORT ER   wm_obj_asked( INT wid, UINT seq );

/*
 * Letters carried out of a text and let go over a window that belongs to
 * someone else: a drop of no links whose text is the n bytes of UTF-8
 * (one line), told with OB_E_DROP; (sx, sy) on the screen is where the
 * corner of the outline carried was. E_NOSPT: nobody takes letters on
 * that window (OB_E_TEXT).
 */
IMPORT ER   wm_obj_drop_text( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
			      CONST UB *text, INT n );

/*
 * A scroll bar of a window that belongs to someone else, worked by the
 * user: told with OB_E_SCROLL on its record OB_WR_BARS, code the bar
 * (WM_BAR_*), x the start it is asked to go to, y how (OB_SCR_*).
 */
IMPORT void wm_obj_scroll( INT wid, UINT which, INT pos, UINT how );

/*
 * A key typed by a window that never has the keys (WM_ATTR_NOFOCUS),
 * a screen keyboard: put in as if the keyboard had sent it, pressed and
 * let go, so that it goes to the window that has the keys. code is the
 * key's usage (1..255), mods the modifiers held with it (HID_MOD_* and
 * KM_MOD_THUMB_*) and the layouts the key is read in (KM_MOD_LAYOUT, the
 * screen keyboard's own; none, the machine's), which go with its press
 * and its let go to the window it reaches. It is taken only while the last press of a button
 * went down on that window, within WM_KEY_PUT_MS of it: a window types
 * only as a person presses on it. E_OBJ for a window that may have the
 * keys, E_OACV for no such press.
 *
 * With WM_KEY_PUT_DOWN in mods the key is only pressed, and stays down
 * until WM_KEY_PUT_UP with the same code or the window goes, so that a
 * program that reads the key's state sees it held. While it is down the
 * press may be given again (the key repeating) however long after the
 * press on the window. A window holds one key: pressing another lets the
 * first go. E_OACV for letting go a key that is not down.
 */
#define WM_KEY_PUT_MS		3000
#define WM_KEY_PUT_DOWN		0x01000000U
#define WM_KEY_PUT_UP		0x02000000U
IMPORT ER   wm_obj_key_put( INT wid, UINT code, UINT mods );

#ifdef __cplusplus
}
#endif

#endif /* __TS_WM_H__ */
