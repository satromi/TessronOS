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

#define WM_SB_MAX	6		/* fields in one box along a line */
/*
 * The fields a part keeps its words in: a label of 127 bytes, the keys
 * that do the same of 11. They are where short words are kept, not how
 * long words may be: longer ones are kept whole beside the part
 * (T_WMLONG), the field then holding their head cut at a letter's end.
 */
#define WM_LABEL_MAX	128
#define WM_KEY_MAX	12
/*
 * Panels open at once are as many as the memory holds: the table of
 * them grows as they open. A menu standing in a menu is two of them;
 * what stops a chain of menus whose definition points at itself is the
 * depth a definition may take another in (mn.c), not a count of panels.
 * wm_panel_last answers the highest panel number there may be now, for
 * a caller that goes through them all (1 to it).
 */
IMPORT INT wm_panel_last( void );

/* Whether name idx (1 up) of a panel part's list, selector or tags cannot be chosen */
IMPORT BOOL knl_pn_name_off( INT pid, INT i, INT idx );

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
/*
 * A panel of tags whose sheet the program draws in (P_VIEW): every time
 * the panel is drawn, its ground over what was drawn there, the window
 * is told OB_E_REDRAW so that the program draws it again.
 */
#define P_VIEW		0x20000000U	/* a panel of tags whose sheet the program draws in */

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
 * keys and Enter, becomes the part's. Its names are its own as a
 * selector's are, and its value is the one chosen (1 up), 0 none.
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

/*
 * The names a part holds (a list's rows, a selector's or a pop-up's
 * choices, a panel of tags' tags, the names of a box's fields), kept by
 * the part itself in one block as large as they are: there is no bound
 * on how many there are or how long they are together but the memory.
 * What is in the block is the window manager's: a definition is given
 * its names with wm_part_name, a panel's part with knl_pn_set_names
 * (record 4 of a part), and they are read with knl_pn_names.
 */
typedef struct T_WMNAMES	T_WMNAMES;

/*
 * A part's words longer than its field: its label, its keys and a box's
 * letters, each whole and allocated, the part's own. A part has this only
 * when one of them is longer than its field holds (lng NULL otherwise):
 * the label and the keys then keep their head in label[] and key[], cut
 * at a letter's end, and a box's letters are in text here alone, room
 * bytes of it. A panel opened from a definition has a copy of its own;
 * a definition is let go with wm_panel_free, which lets this go too.
 * What reads a part's words reads them through WM_PT_LABEL_S,
 * WM_PT_KEY_S and WM_PT_TEXT_S.
 */
typedef struct {
	UB	*label;			/* NULL: label[] holds it */
	UB	*key;			/* NULL: key[] holds it */
	UB	*text;			/* NULL: text[] holds it */
	INT	room;			/* the bytes text has room for */
} T_WMLONG;

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
	 * A list, a selector of one part, a pop-up or a tag panel: its
	 * names, one after another each ending in a nought, in a block of
	 * its own (NULL while it has none). A selector of one part lays
	 * them out itself; a choice of a group (count nought) is one name
	 * of the selector its group makes.
	 */
	T_WMNAMES	*names;
	INT		count;		/* how many there are */
	INT		top;		/* the first one shown (1 up) */
	INT		rows;		/* how many fit, worked out when drawn */
	INT		now;		/* the name in use now, underlined (1 up), 0 none */

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
	INT		sbpool[WM_SB_MAX];	/* a field of names: where its first name is among the part's names */
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

	T_WMLONG	*lng;		/* words longer than their fields, NULL none */
} T_WMPART;

#define WM_PT_LABEL_S(pt)	( ( (pt)->lng != NULL && (pt)->lng->label != NULL ) \
				  ? (CONST UB *)(pt)->lng->label : (CONST UB *)(pt)->label )
#define WM_PT_KEY_S(pt)		( ( (pt)->lng != NULL && (pt)->lng->key != NULL ) \
				  ? (CONST UB *)(pt)->lng->key : (CONST UB *)(pt)->key )
#define WM_PT_TEXT_S(pt)	( ( (pt)->lng != NULL && (pt)->lng->text != NULL ) \
				  ? (CONST UB *)(pt)->lng->text : (CONST UB *)(pt)->text )

/*
 * A part's label, keys or a box's letters set: n bytes of s (n below
 * nought: up to its nought), as long as they are, kept in the field when
 * they fit and whole beside it when not. E_NOMEM when there is not the
 * memory for them, the words then as they were. A box's caret goes to
 * the end of its letters.
 */
IMPORT ER wm_part_set_label( T_WMPART *pt, CONST UB *s, INT n );
IMPORT ER wm_part_set_key( T_WMPART *pt, CONST UB *s, INT n );
IMPORT ER wm_part_set_text( T_WMPART *pt, CONST UB *s, INT n );

/*
 * A part's own words copied for another part that is a copy of it (dst
 * a byte copy of src: its lng is made its own), or let go. FALSE when
 * there was not the memory: dst then has no long words, its fields their
 * heads.
 */
IMPORT BOOL wm_part_long_dup( T_WMPART *dst );
IMPORT void wm_part_long_free( T_WMPART *pt );

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

/*
 * A panel's parts are a table of their own, as many as the definition
 * has: npart of them in use, room for room. There is no bound on how
 * many a panel has but the memory. A definition is made with
 * wm_panel_new and let go with wm_panel_free; wm_panel_part_add puts a
 * part after the ones it has, the table made larger when it is full,
 * so a part it answered is to be filled in before the next is added:
 * the table may move. A part it could not add (NULL) leaves 'lost' set,
 * and the definition is then not opened (E_NOMEM): a panel is never
 * shown with a part missing.
 */
typedef struct {
	INT		num;
	ID		owner;		/* the process it belongs to, 0 for the system */
	UINT		kind;		/* WM_PNL_* */
	T_DPRECT	r;		/* in the work area of the window */
	INT		npart;		/* parts in use */
	INT		room;		/* parts the table has room for */
	T_WMPART	*part;		/* the table, NULL while it has no room */
	BOOL		lost;		/* a part could not be added */
} T_WMPANEL;

/*
 * The bytes of a definition of n parts in the data box: the definition,
 * its parts one after another after it (its table's place and room are
 * not read from the box).
 */
#define WM_PANEL_BYTES(n)	( sizeof(T_WMPANEL) + sizeof(T_WMPART) * (SZ)(n) )

/*
 * A definition with room for that many parts and none in use, all
 * nought: NULL when there is not the memory (or room is below nought).
 * A part put after the ones it has, nought but for what the caller
 * fills in: NULL when the table cannot be made larger.
 */
IMPORT T_WMPANEL *wm_panel_new( INT room );
IMPORT T_WMPART  *wm_panel_part_add( T_WMPANEL *def );

/*
 * A name put after the names a part of a definition has, and where it
 * went among them in bytes (what a field of names' sbpool wants), or
 * E_NOMEM. The caller sets the part's count. The block grows as names
 * are put; a definition that has been given names is let go with
 * wm_panel_free.
 */
IMPORT INT wm_part_name( T_WMPART *pt, CONST UB *name );

/*
 * A definition let go: the names of its parts, its table of parts and
 * the definition itself. A panel opened from it has names and parts of
 * its own.
 */
IMPORT void wm_panel_free( T_WMPANEL *def );

/*
 * Open a panel in a window. The definition is copied, parts and names
 * and all, into a table as large as it has parts, so the caller may
 * change or free its own at once. Answers the panel's number, or an
 * error: E_NOMEM when its parts or their names could not be copied (no
 * part is left out), E_PAR for a definition whose count of parts is
 * below nought or past its table.
 */
IMPORT INT wm_panel_open( INT wid, CONST T_WMPANEL *def );

/*
 * The same, with the definition taken out of the data box rather than
 * passed in. This is how a program that keeps its panels as data opens
 * one: it puts the definition in the box once and opens it by number
 * afterwards. The box keeps a definition as bytes, WM_PANEL_BYTES(npart)
 * of them, so the names of its parts do not go into it: a panel opened
 * from it has none until they are set (knl_pn_set_names). Bytes of
 * another length than their count of parts says are E_PAR.
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

/*
 * A box's letters read or set; set on words alone, they are what the
 * words say. wm_panel_text copies as many whole letters as max has room
 * for, and the nought. wm_panel_text_all answers them whole, in a copy
 * made for the caller (*p_buf, to be Kfree'd, ending in a nought), and
 * how many bytes they are. wm_panel_set_text is E_NOMEM when there is
 * not the memory for words as long as they are, the part then as it was.
 */
IMPORT ER  wm_panel_text( INT pid, INT num, UB *buf, INT max );
IMPORT INT wm_panel_text_all( INT pid, INT num, UB **p_buf );
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

/*
 * An item's words are in label[] and key[] when they fit. Longer ones
 * are kept whole after the items, in the same bytes as the menu: llabel
 * and lkey say where, in bytes from the menu's start (0 none), and the
 * fields then hold their head. The bytes of such a menu are
 * WM_MENU_SIZE(nitem) and the words after; WM_ITEM_LABEL and WM_ITEM_KEY
 * read the words whole.
 */
typedef struct {
	INT	cmd;			/* what choosing it means, 0 for a line */
	UB	label[WM_LABEL_MAX];
	UB	key[WM_KEY_MAX];	/* the keys that do the same, or empty */
	INT	sub;			/* the menu that opens beside it, or 0 */
	BOOL	grey;			/* shown, but cannot be chosen */
	BOOL	tick;			/* shown with a mark */
	UINT	llabel, lkey;		/* the words whole, after the items; 0 none */
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
#define WM_ITEM_LABEL(m, it)	( ( (it)->llabel != 0 ) ? (CONST UB *)(m) + (it)->llabel : (CONST UB *)(it)->label )
#define WM_ITEM_KEY(m, it)	( ( (it)->lkey != 0 ) ? (CONST UB *)(m) + (it)->lkey : (CONST UB *)(it)->key )

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
 * for as long as the definitions go. A list opens only as the pointer
 * reaches its row, one at a time, and a menu that cannot be opened (no
 * memory for its panel or its window) simply does not open, leaving the
 * one it came from standing.
 *
 * A definition that points at itself, directly or round a ring, is not
 * a hang: the menu manager reads a definition taken in no deeper than it
 * allows (mn.c, MN_DEPTH), and the lists are only those it read. It is
 * still a fault in the definition; this layer does not correct it, it
 * only refuses to be hung by it.
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
 * selector's names set anew (count names, each ending in a nought: as
 * many and as long as they are, E_NOMEM only when there is not the
 * memory, the names it had then left as they were). knl_pn_names
 * answers how many bytes its names take, in a copy made for the caller
 * (*p_buf, to be Kfree'd; NULL when it has none). knl_pn_state answers
 * how many bytes the state is whole, and copies no more than max of
 * them: a caller whose room was short asks again with as much.
 */
IMPORT INT  knl_pn_state( INT pid, INT i, UB *buf, INT max );
IMPORT ER   knl_pn_set_state( INT pid, INT i, CONST UB *buf, INT len );
IMPORT UINT knl_pn_flags( INT pid, INT i );
IMPORT ER   knl_pn_set_flags( INT pid, INT i, UINT f );
IMPORT ER   knl_pn_set_names( INT pid, INT i, CONST UB *names, INT len );
IMPORT INT  knl_pn_names( INT pid, INT i, UB **p_buf );
/* A part's label whole, in a copy made for the caller (*p_buf, Kfree): its bytes */
IMPORT INT  knl_pn_label( INT pid, INT i, UB **p_buf );
#if USE_KTEST
/* For a test: the next n blocks of names are refused, as when the memory has run out */
IMPORT void knl_pn_test_nomem( INT n );
/* and the next n tables of parts, made or made larger */
IMPORT void knl_pn_test_nomem_parts( INT n );
#endif

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
 * box, whose record 0 is xmlTAD with a <databox> of <panel>s, into def
 * (made by wm_panel_new: whatever parts it had are let go, and its
 * table grows to as many parts as the panel has). Its name (the
 * window's title) comes back in title when asked. E_NOMEM when the
 * parts could not all be kept: none is left out unsaid.
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
