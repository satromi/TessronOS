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

/* The program object whose icon the window wears in its band (set before ui_open) */
IMPORT void ui_icon( CONST char *prog );
IMPORT void ui_close( UI *u );

/* A notice from the window's channel (u->port): what it was, and the part */
IMPORT INT  ui_event( UI *u, CONST T_OBNTM *m, INT *p_num );

/* What the parts hold, by number */
IMPORT INT  ui_value( UI *u, INT num );
IMPORT void ui_set_value( UI *u, INT num, INT v );
IMPORT void ui_set_now( UI *u, INT num, INT v, INT now );	/* a selector: chosen, and in use now */
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
 * takes it away. u need only have its box, kw, port, w and h.
 */
#define UI_EV_OTHER	8
IMPORT ER   ui_dlg_open( UI *u, INT num, UI *q );
IMPORT INT  ui_dlg_next( UI *u, UI *q, T_OBNTM *m, INT *p_num );
IMPORT void ui_dlg_close( UI *q );

/*
 * The links dropped on the window, when ui_event said UI_EV_DROP: E_OK
 * and the drop in *d. It is answered with ui_drop_answer: OB_DR_ACCEPT
 * or OB_DR_REFUSE, and words for the message line ("" the usual ones).
 */
IMPORT ER   ui_drop( UI *u, T_OBDROP *d );
IMPORT void ui_drop_answer( UI *u, CONST T_OBDROP *d, UINT answer, CONST char *msg );

/* A UUID from its text ("xxxxxxxx-xxxx-..."), as the system's names are written */
IMPORT ER   ui_uuid( CONST char *s, TS_UUID *u );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UI_H__ */
