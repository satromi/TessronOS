/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	part.h
 *	Parts, panels and menus (design 16.2.2, 18.13, stage 10b)
 *
 *	A panel and a menu are both built from a definition: a list of
 *	parts, or a list of items, with a rectangle and a number. Nothing
 *	draws itself from measurements written at the place it is used:
 *	what a thing looks like comes from its definition and from the
 *	numbered look table (wm_look).
 *
 *	Two rules (design 16.2.3).
 *
 *	A definition is copied when it is opened, never held by pointer. A
 *	program that changed a definition held by pointer would change it
 *	under everyone using it, and one that died would leave it behind. A
 *	copy costs a few hundred bytes and rules out both.
 *
 *	A definition is named by (kind, number, owner), not by number
 *	alone. Two programs may both have a panel 3 and they are not the
 *	same panel.
 *
 *	What is not here yet is text. Labels and names need a font layer
 *	(design 16.6 stage 9c), and drawing something else in their place
 *	would put marks on the screen that no definition asked for. The
 *	places a label goes are laid out and left blank, so that adding the
 *	font changes what is drawn and not where anything sits.
 */

#ifndef __TS_PART_H__
#define __TS_PART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/dp.h>
#include <ts/wm.h>

#define WM_PART_MAX	200		/* parts in one panel: the words on it are parts too */
#define WM_POOL_BYTES	8192		/* the names of every list, field and tag in one panel: a list of a few hundred disks */
#define WM_SB_MAX	6		/* fields in one box along a line */
#define WM_LABEL_MAX	128		/* bytes of a label, with its terminator:
					   a name of 40 Japanese letters */
#define WM_KEY_MAX	12		/* bytes of the keys that do the same */
/*
 * Panels open at once. A menu standing in a menu standing in a menu is
 * three of these, so this is what sets how deep a chain of menus may
 * go: nothing else does. It is not a shape the system imposes on what a
 * program may write -- a menu is a tree of whatever depth its author
 * gave it -- it is only how many can be on the screen together.
 */
#define WM_PANEL_MAX	32

/*
 * What a part is, and how it is to be drawn: one word, the kind in its
 * low five bits and the rest flags.
 *
 * The numbers are fixed, because definitions carry them: a panel
 * arriving from a program says 5 for a button and 10 for a
 * scrolling selector, and sets 0x800 to grey one out. Renumbering them
 * would mean rewriting every definition, and a definition is data.
 */
#define TB_PARTS	1		/* a box of letters */
#define XB_PARTS	2		/* one whose letters are not shown */
/*
 * What a program puts in a box whose letters are not shown to stand for
 * letters it keeps elsewhere and does not know (a password saved): each
 * shows as a letter does, and the first key typed into the box takes
 * them all away, so that what is read from the box then is only what
 * was typed.
 */
#define XB_STANDIN	0x7F
#define NB_PARTS	3		/* a number */
#define SB_PARTS	4		/* fields along a line */
#define AS_PARTS	5		/* an alternate switch: on or off */
#define MS_PARTS	6		/* a momentary switch: a push button */
#define PA_PARTS	7		/* a switch showing a picture */
#define PM_PARTS	8		/* a push button showing a picture */
#define WS_PARTS	9		/* a selector: one name of several */
#define SS_PARTS	10		/* a selector that scrolls */
#define VL_PARTS	11		/* a volume */

#define P_TYPE		0x0000001FU	/* the kind, in the low five bits */
#define P_HALIGN	0x00000020U	/* laid across rather than down */
#define P_DOUBLE	0x00000040U	/* a selector's names in two columns */
#define P_NOSEL		0x00000080U	/* a selector that may have none chosen */
#define P_EMPHAS	0x00000200U	/* the one that answers */
#define P_NOFRAME	0x00000400U	/* no edge drawn round it */
#define P_DISABLE	0x00000800U	/* shown, but cannot be worked */
#define P_INACT		0x00001000U	/* its window does not hold the input */
#define P_DISP		0x00004000U	/* drawn when the panel is drawn */
#define P_PARTDISP	0x00008000U	/* drawn by itself */
#define P_SBAR		0x00010000U	/* a bar this layer made for a selector */
#define P_HIDDEN	0x00020000U	/* belongs to another part, not to a program */
#define P_ZERO		0x00040000U	/* fields along a line shown with noughts to their width */
#define P_NOW		0x00080000U	/* a choice of a group in use now: its name underlined */
#define P_JP		0x00100000U	/* a box typed into through かな漢字変換 */
#define P_BLANK		0x00200000U	/* fields along a line shown empty until typed into */
#define P_DIGITS	0x00400000U	/* a box that takes digits only */
#define P_ASCII		0x00800000U	/* a box that takes letters, digits and signs only */
#define P_BASE		0x01000000U	/* words on a base line four above their rectangle's foot */
#define P_GONE		0x02000000U	/* put away: not shown and not worked until shown again */
#define P_DOWN		0x04000000U	/* a panel of tags whose tags stand down the sheet's left side */
/*
 * A panel of tags whose sheets scroll (P_SCROLL on the tags): on a
 * window whose work area is shorter than the panel, the panel ends at
 * the work area's foot, the parts of the sheet shown move in what is
 * left of the sheet as its scroll offset says, and the window's right
 * bar, when it has one, and the wheel work that offset. A part at the
 * foot (P_FOOT: a sheet's row of buttons) does not scroll: it rises
 * with the panel's foot and the sheet shows down to its head.
 */
#define P_FOOT		0x08000000U	/* stands at the panel's foot: never scrolled */
#define P_SCROLL	0x10000000U	/* a panel of tags whose sheets scroll */

/* What each field of a box along a line is (sbf) */
#define SBF_ZERO	0x01		/* shown with noughts to its width */
#define SBF_NAMES	0x02		/* a field of names, stepped through */
#define SBF_LEFT	0x04		/* set against its left rather than its right */
#define SBF_RIGHT	0x08		/* a list's column: its letters set against its right end */

/* The bits a bar is drawn by, which live in the same word */
#define OB_ACROSS	0x00000020U	/* along the foot rather than down a side */
#define OB_SMALLKNOB	0x00000080U	/* the knob kept two long */
#define OB_INACT	0x00001000U	/* the window does not hold the input */
#define OB_LEFT		0x00002000U	/* the bar down the left side */
#define OB_OFF		0x00001880U	/* no mark: not to be worked, or a small knob */

/* What this system adds, in numbers the other one leaves free */
#define WM_PT_LABEL	0		/* words and nothing else */
#define WM_PT_SWATCH	12		/* a colour, chosen by pressing it */
#define TG_PARTS	13		/* a panel of tags over sheets of parts */
#define WM_PT_LINE	14		/* a line from the rectangle's head and left to its foot and right */
#define WM_PT_ICON	15		/* an object's icon, as its link shows it: the object's UUID
					   is the part's words, set as a box's are; none shows nothing */
/*
 * A pop-up: one name of several, shown in a raised box with a mark at
 * its right. Pressed, the names open as a list over it, the one chosen
 * on the part's own row; the name let go on, or chosen with the arrow
 * keys and Enter, becomes the part's. Its names live in the pool as a
 * selector's do, and its value is the one chosen (1 up), 0 none.
 */
#define WM_PT_POPUP	16

/*
 * The plane and the bar a colour is chosen on. The plane shows, for one
 * hue, saturation from left to right and brightness from foot to head;
 * its value holds all three (WM_SVMAP_VALUE). The bar shows the hues
 * from right to left, thin and with round ends; its value is the hue, 0
 * to 359. Their marks are round. Pressing or dragging moves the mark and
 * the value, and the caller reads the value. They belong on a sheet
 * (WM_PNL_SHEET): the bar's rectangle is white outside what it shows.
 */
#define WM_PT_SVMAP	17
#define WM_PT_HUEBAR	18
/*
 * A round spot of the part's colour, read and written as a swatch's is
 * and not to be worked; and the picture of a dropper, answering when let
 * go over it as a push button does.
 */
#define WM_PT_COLDOT	19
#define WM_PT_DROPPER	20
#define WM_SVMAP_VALUE(h, s, v)	( ( (INT)(h) << 16 ) | ( (INT)(s) << 8 ) | (INT)(v) )
#define WM_SVMAP_H(n)	( ( (n) >> 16 ) & 0xFFFF )
#define WM_SVMAP_S(n)	( ( (n) >> 8 ) & 0xFF )
#define WM_SVMAP_V(n)	( (n) & 0xFF )

/* And the names this system's own code says */
#define WM_PT_BOX	TB_PARTS
#define WM_PT_SECRET	XB_PARTS
#define WM_PT_NUMBOX	NB_PARTS
#define WM_PT_BUTTON	MS_PARTS
#define WM_PT_CHECK	AS_PARTS
#define WM_PT_CHOICE	WS_PARTS
#define WM_PT_LIST	SS_PARTS
#define WM_PT_VOL	VL_PARTS
#define WM_PT_TAGS	TG_PARTS

#define WM_PT_KIND(t)	( (UINT)((t) & P_TYPE) )

/* What a part is for, when the panel ends */
#define WM_ANS_NONE	0		/* it does not end the panel */
#define WM_ANS_OK	1		/* the panel was agreed to */
#define WM_ANS_CANCEL	2		/* the panel was abandoned */
/*
 * A button that does something and leaves the panel open answers
 * WM_ANS_ACT or more: the answer is given once, for the release that
 * pressed it, and the panel goes on as though nothing had ended it.
 */
#define WM_ANS_ACT	16
/*
 * A swatch with an answer does not mark itself when pressed: it is a
 * colour shown that does something, and answers when let go over it as
 * a push button does.
 */

typedef struct {
	UW		type;		/* the kind, and the flags above */
	INT		num;		/* how the caller knows this part */
	T_DPRECT	r;		/* in the panel's own coordinates */
	UINT		answer;		/* WM_ANS_* for a button */
	INT		group;		/* which choices belong together */
	INT		value;		/* on/off, the name chosen (1 up), or a tag panel's tag shown */
	UW		colour;		/* for a swatch */
	UB		label[WM_LABEL_MAX];

	/*
	 * A menu row: the keys that do the same thing, shown at its right
	 * end, and the number of the menu that opens beside it. A row with
	 * a menu under it answers nothing itself.
	 */
	UB		key[WM_KEY_MAX];
	INT		sub;		/* a menu row's menu; a volume's number box */

	/* a volume: what it runs between, and where its knob is */
	INT		lo, hi;

	/*
	 * A list, a selector of one part, or a tag panel: its names live in
	 * the panel's pool, one after another. A selector of one part lays
	 * them out itself; a choice of a group (count nought) is one name
	 * of the selector its group makes.
	 */
	INT		pool;		/* where the first of them starts */
	INT		count;		/* how many there are */
	INT		top;		/* the first one shown (1 up) */
	INT		rows;		/* how many fit, worked out when drawn */
	INT		now;		/* the name in use now, underlined (1 up), 0 none */
	UW		inact;		/* names that cannot be chosen, a bit each from bit 0 */

	/* a box: what has been typed into it, and where the caret is */
	UB		text[WM_LABEL_MAX];
	INT		caret;		/* bytes before it */
	INT		max;		/* letters it may hold, 0 as many as fit */

	/*
	 * A box of fields along a line: how many there are, what each
	 * holds, and how wide each is in characters. The words between the
	 * fields are the part's own label, with a hash where each field
	 * goes -- "####年##月##日" is three fields and the words around
	 * them, which is the whole of what such a box is.
	 *
	 * A list, a selector or a pop-up whose names are parted into
	 * columns by tabs keeps here the widths its definition gives the
	 * columns (cols="0,72r,96"): sb_n of them, each sbw[] in pixels or
	 * 0 for an equal share of what is left, SBF_RIGHT in sbf[] for one
	 * set against its right end. sb_n 0: the widths come from the names.
	 */
	INT		sbv[WM_SB_MAX];
	INT		sbw[WM_SB_MAX];
	INT		sblo[WM_SB_MAX], sbhi[WM_SB_MAX];
	UB		sbf[WM_SB_MAX];	/* SBF_* */
	INT		sbpool[WM_SB_MAX];	/* a field of names: its first name in the pool */
	INT		sb_n;		/* fields, worked out from the label */
	INT		sb_at;		/* the one the caret is in */

	/*
	 * A panel of tags: a row of them over the sheet 'inner', and a
	 * second row inside it under the tags that have one. Its names
	 * are in the pool, in the order the tags stand: a name written
	 * ＠名前 is a tag of the upper row whose lower row starts with the
	 * name after it, ＋名前 adds a tag to that lower row, and a name
	 * with neither is a tag of the upper row alone. The tags are
	 * numbered from one as the names go, a ＠ and the name after it
	 * being one tag; 'value' is the one shown, and sbv[] keeps, for
	 * each upper tag with a lower row in turn, the one last shown in
	 * that row.
	 *
	 * With P_DOWN the tags stand one under another down the left
	 * side of 'inner' instead, all as wide as the widest name, and
	 * there is no lower row: a ＠ or ＋ before a name is left out and
	 * the tag stands in the column like the rest.
	 *
	 * Any part of the panel says which tag it belongs under in
	 * 'sheet', and is shown only while that tag is: nought is under
	 * every one.
	 */
	T_DPRECT	inner;
	INT		sheet;
} T_WMPART;

/*
 * A panel: where it sits, what is in it, and who it belongs to. The
 * owner is part of its identity, so that two programs' panel 3 are two
 * panels.
 */
/*
 * What a panel is for. The frame it wears comes from this, and so does
 * whether it can be walked away from: a panel that warns is drawn in
 * the warning frame, and one that cannot be carried on from wears the
 * heavy one.
 */
#define WM_PNL_PLAIN	0
#define WM_PNL_WARN	1
#define WM_PNL_FATAL	2
#define WM_PNL_SHEET	4		/* white in a thin outline: a pop-up of a few parts */
#define WM_PNL_BARE	3		/* the ground alone: a window's own panel, framed by the window */

typedef struct {
	INT		num;
	ID		owner;		/* the process it belongs to, 0 for the system */
	UINT		kind;		/* WM_PNL_* */
	T_DPRECT	r;		/* in the work area of the window */
	INT		npart;
	T_WMPART	part[WM_PART_MAX];

	/*
	 * The names every list in this panel shows, one after another with
	 * a nought between them. They live here rather than behind a
	 * pointer for the reason the definition itself is copied: a name
	 * a program can free while the panel is drawing it is a name that
	 * will be freed while the panel is drawing it.
	 */
	UB		pool[WM_POOL_BYTES];
	INT		used;		/* bytes of it taken */
} T_WMPANEL;

/*
 * Put a name in the pool and say where it went, which is what a list
 * part's 'pool' field wants. -1 when the pool is full.
 */
IMPORT INT wm_panel_name( T_WMPANEL *def, CONST UB *name );

/*
 * Open a panel in a window. The definition is copied, so the caller may
 * change or free its own at once. Answers the panel's number, or an
 * error.
 */
IMPORT INT wm_panel_open( INT wid, CONST T_WMPANEL *def );

/*
 * The same, with the definition taken out of the data box rather than
 * passed in. This is how a program that keeps its panels as data opens
 * one: it puts the definition in the box once and opens it by number
 * afterwards.
 */
IMPORT INT wm_panel_open_box( INT wid, INT num, ID owner );
IMPORT ER  wm_panel_close( INT pid );

/*
 * A panel in a window of its own in the middle of the screen, in front
 * of everything: only the size of def->r counts. Keys given for 'owner'
 * reach it as well as those given for its own window.
 */
IMPORT INT wm_panel_open_centre( INT owner, CONST T_WMPANEL *def );

/* The same, its corner at (x, y) of the screen, or as near as the screen lets it be */
IMPORT INT wm_panel_open_at( INT owner, CONST T_WMPANEL *def, INT x, INT y );

/* Draw it, in the window it was opened in */
IMPORT ER  wm_panel_draw( INT pid );

/*
 * Give the panel something that happened. When the panel is finished it
 * says so: E_OK with *p_answer set to WM_ANS_OK or WM_ANS_CANCEL. While
 * it is still going it answers E_OK with WM_ANS_NONE, or once with a
 * button's WM_ANS_ACT or more when such a button was pressed.
 */
IMPORT ER  wm_panel_event( INT pid, CONST T_WMEV *ev, UINT *p_answer );

/* What a part holds now. A caller reads its answers through these. */
IMPORT ER  wm_panel_get( INT pid, INT num, INT *p_value );
IMPORT ER  wm_panel_set( INT pid, INT num, INT value );
IMPORT ER  wm_panel_colour( INT pid, INT num, UW *p_colour );
IMPORT ER  wm_panel_set_colour( INT pid, INT num, UW colour );

/* A part shown or put away; the letters move to a box still shown when theirs goes */
IMPORT ER  wm_panel_show( INT pid, INT num, BOOL shown );

/* Where a part stands on the screen */
IMPORT ER  wm_panel_part_screen( INT pid, INT num, T_DPRECT *out );

/*
 * What has been typed into a box, and setting it. The caret goes to the
 * end of whatever is set.
 */
/* What one field of a box along a line holds */
IMPORT ER  wm_panel_field( INT pid, INT num, INT field, INT *p_value );
IMPORT ER  wm_panel_set_field( INT pid, INT num, INT field, INT value );

/* A box's letters read or set; set on words alone, they are what the words say */
IMPORT ER  wm_panel_text( INT pid, INT num, UB *buf, INT max );
IMPORT ER  wm_panel_set_text( INT pid, INT num, CONST UB *s );

/*
 * Letters (UTF-8, n bytes) put into the box that has the keys in a panel
 * of window wid, at its caret, as a conversion's committed letters are
 * put: what was being converted there committed first, the letters
 * chosen in it replaced. A box of digits or of ASCII takes only those.
 * TRUE when a box took them; the box's program is told that it changed.
 */
IMPORT BOOL wm_panel_type( INT wid, CONST UB *s, INT n );

/* ---------------------------------------------------------------- menus */

typedef struct {
	INT	cmd;			/* what choosing it means, 0 for a line */
	UB	label[WM_LABEL_MAX];
	UB	key[WM_KEY_MAX];	/* the keys that do the same, or empty */
	INT	sub;			/* the menu that opens beside it, or 0 */
	BOOL	grey;			/* shown, but cannot be chosen */
	BOOL	tick;			/* shown with a mark */
} T_WMITEM;

typedef struct {
	INT		num;
	ID		owner;
	INT		nitem;
	/*
	 * Which item is the one in hand. A menu opens with that item
	 * under the pointer and the list hanging above and below it, so
	 * that choosing the same thing again is no movement at all and
	 * everything else is one movement from it. It is set to whatever
	 * was chosen last time.
	 */
	INT		cur;
	T_WMITEM	item[];		/* nitem of them */
} T_WMMENU;

/*
 * The bytes of a menu of n items. A menu has as many as its definition
 * gives: one taller than the screen is laid in columns side by side.
 */
#define WM_MENU_SIZE(n)	( sizeof(T_WMMENU) + sizeof(T_WMITEM) * (SZ)(n) )

/*
 * Open a menu at a place in a window's work area. It is a panel of one
 * column, so it goes through the same machinery; what a menu adds is
 * that choosing an item ends it and says which command was chosen.
 */
/*
 * Open a menu at a place in a window's work area.
 *
 * The place is where the pointer is, not the menu's corner: the menu
 * comes up centred across the pointer, with its current item on the
 * pointer's own row, and is then moved inside the window if that would
 * put any of it outside. A menu whose corner went to the pointer would
 * make every item a movement away, including the one just chosen.
 */
IMPORT INT wm_menu_open( INT wid, CONST T_WMMENU *def, INT x, INT y );
IMPORT ER  wm_menu_event( INT pid, CONST T_WMEV *ev, INT *p_cmd );
/*
 * Whether a window is one of a menu's lists, or of the lists opened
 * from it: each list stands in a popup window of its own. A press in
 * any other window puts the menu away.
 */
IMPORT BOOL wm_menu_has( INT pid, INT wid );

/*
 * The press a menu or a pop-up's list was opened by, followed to its
 * release. wm_click_begin takes the press (the time and the button of
 * the event that opened it, as its notice says); wm_click_take is given
 * every event read after, before
 * anything else looks at it, and answers:
 *   WM_CK_KEPT  it is the release of that press, and the press was a
 *               click -- let go within the double press's time
 *               (WM_LOOK_DBLTIME), or with the pointer never more than
 *               a few pixels from where it went down, however long --:
 *               the menu or list stays open and chooses nothing;
 *   WM_CK_EAT   a press or release of another button while the opening
 *               one is held, or the release of such a press: not the
 *               menu's or the list's;
 *   WM_CK_PASS  the rest, to be worked as before.
 */
typedef struct {
	BOOL	live;			/* the opening press not yet let go */
	BOOL	placed;			/* where it went down is known */
	BOOL	moved;			/* the pointer went farther than a click allows */
	UINT	code;			/* its button */
	UINT	eaten;			/* buttons pressed meanwhile, their releases not passed on */
	UD	when;
	INT	x, y;			/* where it went down, on the screen */
} T_WMCLICK;

#define WM_CK_PASS	0
#define WM_CK_KEPT	1
#define WM_CK_EAT	2

/*
 * code is the opening press's button as its notice says, or WM_CK_FIND
 * for the button of the press read at time when (knl_wm_press_seen
 * keeps the last few, as they are read or handed to the objects'
 * layer); a press not among them is none: nothing is followed, and the
 * menu or list works every event as it comes.
 */
#define WM_CK_FIND	0xFFFFFFFFU
IMPORT void wm_click_begin( T_WMCLICK *c, UD when, UINT code );
IMPORT void knl_wm_press_seen( UD when, UINT code );
IMPORT INT  wm_click_take( T_WMCLICK *c, CONST T_WMEV *ev );
IMPORT void knl_menu_tick( void );	/* the lists of rows stayed on, opened */
IMPORT INT  knl_menu_wait( void );	/* ms until a row's list is due, -1 none */

/* Where a panel ended up, and how tall one row of a menu is */
IMPORT ER  wm_panel_rect( INT pid, T_DPRECT *out );
IMPORT INT wm_panel_wid( INT pid );
IMPORT INT wm_panel_front( void );	/* the window of the panel in front that is not a menu, 0 none */
IMPORT INT wm_menu_row_h( void );

/*
 * A menu whose rows carry menus of their own: a row with 'sub' set
 * opens the menu of that number, belonging to the same owner, out of
 * the data box (DB_MENU), beside the row the pointer is on. The row
 * before it closes as the pointer leaves, and choosing anything in any
 * of them ends the whole chain at once, which is why the command comes
 * back from the menu that was opened rather than from the one it was
 * chosen in.
 *
 * **A menu is a tree of any depth.** A menu opened from a row is a menu
 * like any other, so its own rows may carry menus in turn, and so on
 * for as long as the definitions go. Nothing here says how deep that
 * may be; what ends it is running out of panels to open (WM_PANEL_MAX),
 * and a menu that cannot be opened simply does not open, leaving the
 * one it came from standing.
 *
 * A definition that points at itself, directly or round a ring, is
 * therefore not a hang: each turn of the ring costs a panel and the
 * chain stops when the panels run out. It is still a fault in the
 * definition; this layer does not correct it, it only refuses to be
 * hung by it.
 *
 * Nothing has to be done to take part in this: a menu put in the data
 * box under a number is a menu a row may point at, and wm_menu_event on
 * the menu that was opened answers for the whole chain, however deep
 * the chain went.
 */

/*
 * Check that what is kept still makes sense: a panel's parts are inside
 * it, no two parts of one panel carry the same number, and at most one
 * choice of a group is on. Answers how many faults were found.
 */
IMPORT INT wm_panel_self_check( void );

/*
 * The caret of the box that has the keys, and a conversion under way in
 * it, go on while nothing happens: this is called every so often by
 * whatever reads the input (knl_pn_wait says when it is next due).
 */
IMPORT void knl_pn_tick( void );
IMPORT INT  knl_pn_wait( void );

/*
 * The list of a pop-up, while it is open: every input goes to it and to
 * nothing else until a name is chosen or the list is put away. Whatever
 * reads the input hands each input here first (wm_read_event does), and
 * an input taken here is not handed on: TRUE. knl_pn_list answers the
 * list's panel (0 when none is open) and, in *p_wid, its window.
 */
IMPORT BOOL knl_pn_list_event( CONST T_WMEV *ev );
IMPORT INT  knl_pn_list( INT *p_wid );

/*
 * What a part of an open panel holds, as its state record carries it,
 * whether it is shown and can be worked (OB_WP_SHOWN, OB_WP_OFF), and a
 * selector's names set anew (count names, each ending in a nought).
 */
IMPORT INT  knl_pn_state( INT pid, INT i, UB *buf, INT max );
IMPORT ER   knl_pn_set_state( INT pid, INT i, CONST UB *buf, INT len );
IMPORT UINT knl_pn_flags( INT pid, INT i );
IMPORT ER   knl_pn_set_flags( INT pid, INT i, UINT f );
IMPORT ER   knl_pn_set_names( INT pid, INT i, CONST UB *names, INT len );
IMPORT INT  knl_pn_names( INT pid, INT i, UB *buf, INT max );

/*
 * A panel whose sheets scroll (P_SCROLL): knl_pn_fit takes the height
 * of its window's work area again once the window's size changed;
 * knl_pn_scroll is the right bar of window wid worked (pos where the
 * start of what shows is asked to go): TRUE when a panel there took it,
 * and its program is not told.
 */
IMPORT void knl_pn_fit( INT pid );
IMPORT BOOL knl_pn_scroll( INT wid, INT pos );

/*
 * A panel placed beside what its program draws itself (ui_strip: the
 * object made with "beside":1): none of its boxes has the letters until
 * it is pressed, and a press on the content takes them away again. Any
 * other panel gives the first box the letters as it opens.
 */
IMPORT void knl_pn_set_beside( INT pid );

/* The lock of the panels' drawing made, once, as the window manager starts */
IMPORT ER   knl_pn_init( void );

/*
 * A panel made from a data box: definition number num of the object
 * box, whose record 0 is xmlTAD with a <databox> of <panel>s. Its name
 * (the window's title) comes back in title when asked.
 */
IMPORT ER   knl_pn_load( CONST TS_UUID *box, INT num, T_WMPANEL *def, UB *title, INT tmax );
/*
 * The picture of a push button of a data box (<button pict="0_N.png">):
 * the PNG resource href ("<uuid>_0_N.png") read now and kept, in the
 * caller's task; drawn in the middle of the button in place of words
 */
IMPORT void knl_pn_pict_load( CONST UB *href );

#ifdef __cplusplus
}
#endif

#endif /* __TS_PART_H__ */
