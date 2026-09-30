/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lterm.h
 *	Lines of text in a window of a program's own: what コンソール and
 *	シリアル通信 are drawn with (design 16.5.19)
 *
 *	The lines are kept in a ring, the oldest going when it is full; the
 *	bytes a terminal is sent are put in as a terminal takes them. A line
 *	longer than the window is wide goes on over as many rows as it takes,
 *	and the view is counted in those rows: the last of them, unless it
 *	has been wound back with the keys, the wheel or the scroll bar down
 *	the window's right side. The window, its channel and the notices are
 *	here too, since both programs have them the same way.
 */

#ifndef __LTERM_H__
#define __LTERM_H__

#include <tk/typedef.h>
#include <ts/uapp.h>

extern unsigned long strlen( const char *s );
IMPORT ER ts_get_mono( UD *p_ns );

#define LT_LINES	300		/* lines kept */
#define LT_COLS		1024		/* bytes of one line */
#define LT_PX		14		/* the letters' size */
#define LT_LH		18		/* one row to the next */
#define LT_PAD		4
#define LT_WHEEL	3		/* rows a notch of the wheel winds */

typedef struct {
	UB	line[LT_LINES][LT_COLS];
	INT	len[LT_LINES];
	INT	first;			/* the oldest line in the ring */
	INT	n;			/* lines in use; the last is being written */
	BOOL	cr;			/* a CR came: the line starts again */
	INT	esc;			/* in an escape sequence */
	INT	back;			/* rows the view is wound back from the foot */

	/* the rows each line takes, for the width they were counted at */
	INT	rows[LT_LINES];		/* 0: not counted since it changed */
	INT	wrap_w;

	/* what the last drawing showed: the scroll bar works from it */
	INT	total;			/* rows there are */
	INT	shown;			/* rows the window shows */
	T_DPRECT bar;			/* the bar, in the work area */
	INT	grab;			/* the knob held: where it was taken, -1 not */
} LTERM;

/* The window of the program and where what happens to it arrives */
typedef struct {
	TS_UUID	win, ch;
	ID	kw;			/* the window's key */
	ID	port;			/* the channel's */
	INT	gid;
} LTWIN;

IMPORT void lt_clear( LTERM *t );
IMPORT void lt_put( LTERM *t, CONST UB *s, INT n );
IMPORT void lt_puts( LTERM *t, CONST char *s );
IMPORT void lt_putn( LTERM *t, D v );

/* Drawn: the lines, then `tail` (a prompt and what is typed, or NULL) with the caret */
IMPORT void lt_draw( LTERM *t, LTWIN *w, CONST UB *tail, BOOL caret );
IMPORT void lt_wind( LTERM *t, INT rows );	/* + back, - forward */

/*
 * A press, a move or a let go of the first button, or a wheel: what the
 * scroll bar and the wheel do. Answers TRUE when it was theirs and the
 * view changed or may have.
 */
IMPORT BOOL lt_pointer( LTERM *t, CONST T_OBNTM *m );

IMPORT ER   lt_open( LTWIN *w, CONST char *name, CONST char *json );
IMPORT void lt_close( LTWIN *w );

/* Text helpers: a number into a buffer; answers the length */
IMPORT INT  lt_num( UB *out, D v );
IMPORT BOOL lt_same( CONST UB *a, CONST char *b );
IMPORT ER   lt_uuid( CONST char *s, TS_UUID *u );	/* "xxxxxxxx-xxxx-..." */

#endif /* __LTERM_H__ */
