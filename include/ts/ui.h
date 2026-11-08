/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ui.h
 *	A program's window of panels from a data box (design 16.5.23, 18.13, 18.15)
 *
 *	What the settings accessories are worked with. The window's panel
 *	and every panel it asks something with are definitions in a data box
 *	object (its record 0, <databox>); the window manager builds them,
 *	draws them and works their parts. The program only reads and writes
 *	what a part holds -- its state record -- by the number the definition
 *	gives it, and hears what happened to the parts on its channel. Nothing
 *	here draws.
 */

#ifndef __TS_UI_H__
#define __TS_UI_H__

#include <tk/typedef.h>
#include <ts/uapp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PART_MAX	200		/* parts of one panel */

typedef struct {
	INT	num;			/* the definition's number for it */
	ID	key;			/* the part opened */
	UINT	kind;			/* the window manager's kind of part */
} UIPART;

typedef struct {
	TS_UUID	box;			/* the data box */
	TS_UUID	win, pnl, ch;
	ID	kw, pk, port;
	INT	w, h;			/* the panel's size, the window's work area */
	UIPART	p[UI_PART_MAX];
	INT	np;
	INT	tags;			/* the number of the part that is the tags, 0 none */
	INT	last;			/* the box last typed into */
} UI;

/* What ui_event said happened */
#define UI_EV_NONE	0
#define UI_EV_CHANGE	1		/* what a part holds changed: *p_num is it */
#define UI_EV_BUTTON	2		/* a push button was pushed: *p_num */
#define UI_EV_SHEET	3		/* another tag is shown: *p_num is the tag */
#define UI_EV_CLOSE	4		/* the window is to close */
#define UI_EV_MENU	5		/* the second button: the program's menu, at the notice's place */
#define UI_EV_KEY	6		/* a key no part took */
#define UI_EV_DROP	7		/* links were dropped on the window: ui_drop reads them */

/*
 * The window with panel num of the data box (its UUID as text) filling
 * its work area, at (x, y) on the screen; its title is the panel's name.
 */
IMPORT ER   ui_open( UI *u, CONST char *box, INT num, INT x, INT y );

/*
 * Another window of the program, made as ui_open makes one, whose
 * notices are told to the channel of the window with (with->port)
 * instead of one of its own: one wait has what both windows are told.
 * ui_mine says whether a notice is of u's window or panel, to be handed
 * to ui_event(u, ...) rather than with's; ui_close leaves the channel,
 * which is with's.
 */
IMPORT ER   ui_open_on( UI *u, CONST UI *with, CONST char *box, INT num, INT x, INT y );
IMPORT BOOL ui_mine( CONST UI *u, CONST T_OBNTM *m );

/*
 * The next window ui_open or ui_open_on makes, a subordinate window of
 * the window parent (its object's UUID): kept just in front of it,
 * carried with it, out of sight while a window of another program is
 * in use, and its own window again when parent closes (wm_set_parent).
 * NULL takes back what was asked.
 */
IMPORT void ui_sub( CONST TS_UUID *parent );

/* The program object whose icon the window wears in its band (set before ui_open) */
IMPORT void ui_icon( CONST char *prog );
IMPORT void ui_close( UI *u );

/* A notice from the window's channel (u->port): what it was, and the part */
IMPORT INT  ui_event( UI *u, CONST T_OBNTM *m, INT *p_num );

/*
 * The next notice on the window's channel, waited for up to tmo ms
 * (TMO_POL not at all, TMO_FEVR until one comes, ob_rea_tmo): E_OK with
 * it in *m, E_TMOUT when none came. A program waits here for what it
 * is told, the first notice of a round for as long as it has nothing
 * else to do and the rest with TMO_POL, rather than looking again and
 * again.
 *
 * ui_now is the monotonic clock in ms, ui_left the ms from now until
 * the time at on it (0 once it has passed): what a loop with work to do
 * at a time waits for. ui_ticks counts the whole ticks of tick ms gone
 * since *p_at and moves *p_at on by them, for a loop whose work is
 * counted in ticks however long it waited; after more than max of them
 * (a wait without end in between) it starts again from now and counts
 * one.
 */
IMPORT ER   ui_wait( UI *u, T_OBNTM *m, TMO tmo );
IMPORT UD   ui_now( void );
IMPORT TMO  ui_left( UD at );
IMPORT INT  ui_ticks( UD *p_at, INT tick, INT max );

/* What the parts hold, by number */
IMPORT INT  ui_value( UI *u, INT num );
IMPORT void ui_set_value( UI *u, INT num, INT v );
IMPORT void ui_set_now( UI *u, INT num, INT v, INT now );	/* a selector: chosen, and in use now */
IMPORT void ui_set_swatch( UI *u, INT num, UW colour, BOOL chosen );	/* a swatch: its colour, and marked */
/* A volume: its value, and the ends it runs between (one across starts from its right) */
IMPORT void ui_set_range( UI *u, INT num, INT v, INT lo, INT hi );
IMPORT INT  ui_text( UI *u, INT num, char *buf, INT max );
IMPORT void ui_set_text( UI *u, INT num, CONST char *s );	/* a box's letters, a label's words */
IMPORT INT  ui_fields( UI *u, INT num, INT *v, INT n );
IMPORT void ui_set_fields( UI *u, INT num, CONST INT *v, INT n );
IMPORT void ui_blank( UI *u, INT num );			/* fields shown empty */
IMPORT void ui_names( UI *u, INT num, CONST char *const *names, INT n );
IMPORT void ui_off( UI *u, INT num, BOOL off );		/* shown, but not to be worked */
IMPORT void ui_show( UI *u, INT num, BOOL shown );
IMPORT void ui_focus( UI *u, INT num );			/* a box given the keys, the caret after its letters */

/* Four fields as an IPv4 address, in network byte order; 0 when shown empty */
IMPORT void ui_set_ip( UI *u, INT num, UW addr );
IMPORT UW   ui_get_ip( UI *u, INT num );

/* The tag the window's tags show, and showing another */
IMPORT INT  ui_sheet( UI *u );
IMPORT void ui_show_sheet( UI *u, INT tag );

/*
 * A panel of the data box asked in front of the window: its words whose
 * parts are numbered from 1 set to lines[] first when given, and the
 * answer is the number of the push button pushed -- 0 for Esc, or the
 * window closed.
 */
IMPORT INT  ui_ask( UI *u, INT num, CONST char *const *lines, INT nline );

/*
 * A panel of the data box asked in front of the window whose parts the
 * program works while it is up: ui_dlg_open makes it (its window named
 * as the panel is) in q, whose parts are read and written with the
 * calls above; ui_dlg_next takes the next notice from u's channel and
 * says what it was -- UI_EV_BUTTON or UI_EV_CHANGE and the part's
 * number for the panel's own, UI_EV_CLOSE for Esc or the panel's window
 * closed, UI_EV_OTHER with the notice in *m for anything else that came
 * to the channel, UI_EV_NONE when nothing is waiting. ui_dlg_close
 * takes it away. u need only have its box, kw, port, w and h. A program
 * that reads its channel itself hands each notice to ui_dlg_take, which
 * says the same of it: one that waits for what it is told reads the
 * channel with ui_wait and hands what came to ui_dlg_take, rather than
 * looking again and again with ui_dlg_next.
 */
#define UI_EV_OTHER	8

/*
 * A window that can be made larger and smaller and has a scroll bar
 * down its right side: ui_attr(WM_ATTR_RESIZE | WM_ATTR_RBAR) before
 * ui_open. The bar is the program's, as a window's own bars are
 * (OB_WR_BARS): it says with ui_bar what there is and what of it the
 * panel shows, and hears the bar worked (UI_EV_SCROLL, *p_num where the
 * start of what is shown is asked to go), the wheel turned over the
 * window (UI_EV_WHEEL, *p_num the notches, away from the person
 * positive) and the window's size changed (UI_EV_SIZE; ui_work reads
 * the work area's size). The panel stays where it is: the program
 * shows in it what the bar says is shown.
 *
 * A panel whose tags say scroll="true" takes the bar of a window made
 * with WM_ATTR_RBAR itself: when the screen leaves the window shorter
 * than the panel, the sheets scroll in it and the parts at their foot
 * (foot="true") stay in sight; the program hears nothing of the bar.
 * What ui_attr asked is for the next window ui_open makes, alone.
 *
 * UI_ATTR_UNTITLED with them: the window has its frame but no title
 * bar, as a palette that comes up under a button has.
 */
#define UI_ATTR_UNTITLED	0x80000000U
/*
 * UI_ATTR_HIDDEN: the window is made hidden, so that what the program
 * sets of its parts first (ui_show, ui_set_value...) is drawn where no
 * one sees it, and it comes up once, as it is to be, with ui_reveal.
 */
#define UI_ATTR_HIDDEN		0x40000000U
IMPORT void ui_reveal( UI *u );
#define UI_EV_WHEEL	9
#define UI_EV_SCROLL	10
#define UI_EV_SIZE	11
IMPORT void ui_attr( UINT attr );
IMPORT void ui_bar( UI *u, INT lo, INT hi, INT clo, INT chi );
IMPORT void ui_work( UI *u, INT *p_w, INT *p_h );
IMPORT ER   ui_dlg_open( UI *u, INT num, UI *q );

/*
 * A panel of the data box (its UUID as text) placed on a window the
 * program made and draws in itself, at (x, y) of its work area: the
 * buttons along the top of a window whose content is the program's
 * picture, a list under it. Its parts are worked with the calls above;
 * what changes on them is told to port, as notices from the panel
 * (u->pnl) that ui_event reads. The size it takes is in u->w, u->h.
 * ui_strip_close takes it off again; the window stays the program's.
 */
IMPORT ER   ui_strip( UI *u, CONST char *box, INT num, ID kw, ID port, INT x, INT y );
IMPORT void ui_strip_close( UI *u );
IMPORT INT  ui_dlg_next( UI *u, UI *q, T_OBNTM *m, INT *p_num );
IMPORT INT  ui_dlg_take( UI *q, CONST T_OBNTM *m, INT *p_num );
IMPORT void ui_dlg_close( UI *q );

/*
 * The links dropped on the window, when ui_event said UI_EV_DROP: E_OK
 * and the drop in *d. It is answered with ui_drop_answer: OB_DR_ACCEPT
 * or OB_DR_REFUSE, and words for the message line ("" the usual ones).
 */
/* The links of the latest drop, every one: kept until the next call; NULL none */
IMPORT T_OBDROP *ui_drop( UI *u );
IMPORT void ui_drop_answer( UI *u, CONST T_OBDROP *d, UINT answer, CONST char *msg );

/* A UUID from its text ("xxxxxxxx-xxxx-..."), as the system's names are written */
IMPORT ER   ui_uuid( CONST char *s, TS_UUID *u );

/*
 * A settings object opened (ob_opn_obj): one opened to be written keeps
 * the other writers out but not the readers (OB_O_WEXCL), and a writer
 * met there is waited for, up to a second, as each holds the object only
 * while it replaces a record whole (ob_rpl_rec). The key, or an error.
 */
IMPORT ID   ui_conf_open( CONST TS_UUID *u, UINT ops );

/*
 * Lists of any length, read whole into buffers of their own (free them).
 * ui_rec_all reads a record of an object, ui_key_all one through a key
 * already open, with room bytes more after the text and a nought after
 * it: the length, or an error. ui_lst_obj gives every object of a kind,
 * ui_lst_lnk every link of a box: how many, or an error.
 */
IMPORT INT  ui_rec_all( CONST TS_UUID *u, INT recno, INT room, UB **p_buf );
IMPORT INT  ui_key_all( ID key, INT recno, INT room, UB **p_buf, INT *p_cap );
IMPORT INT  ui_lst_obj( UINT type, UINT sub, TS_UUID **p_ids );
IMPORT INT  ui_lst_lnk( CONST TS_UUID *box, TS_UUID **p_ids );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UI_H__ */
