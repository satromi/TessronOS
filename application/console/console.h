/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	console.h
 *	コンソール: what its parts share (design 16.5.19)
 *
 *	console.c	the window, the line typed, the commands built in
 *	con_box.c	the place: the box it is in, its links by their names
 *	con_job.c	a program run as a process, and what it prints
 *	con_ime.c	かな漢字変換 on the line typed
 *
 *	Text is UTF-8 everywhere here: the line typed, the names of the
 *	objects, the argument a program is given and what it prints.
 */

#ifndef __CON_H__
#define __CON_H__

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include "../lterm/lterm.h"

#define CON_LINE_MAX	512		/* bytes of the line typed */
#define CON_ARGS_MAX	32		/* words of a line */

/* What goes out to the console (and so comes back into the window) */
IMPORT void say( CONST char *s );
IMPORT void say_u( CONST UB *s );
IMPORT void say_num( D v );

/* Bytes compared with the full-width forms of ASCII taken as ASCII */
IMPORT BOOL con_same_name( CONST UB *a, CONST UB *b );
/* s turned so in place: full-width ASCII and the ideographic space to ASCII */
IMPORT void con_fold( UB *s );

/* ---------------------------------------------------------------- con_box.c */

IMPORT void box_init( void );
IMPORT void box_where( UB *buf, INT max );	/* the path of the place from "/" */
IMPORT void box_here_uuid( TS_UUID *p_u );	/* the object of the place */
IMPORT CONST UB *box_here_name( void );	/* the name of the box it is in */
IMPORT void cmd_pwd( void );
IMPORT void cmd_ls( INT argc, UB **argv );
IMPORT void cmd_cd( INT argc, UB **argv );

/*
 * The program a command names: a path to it, or its name looked for in
 * the object it is in and then in bin. E_OK and the program found;
 * E_NOEXS when nothing has the name, E_OBJ when what has it is not a
 * program with an executable (the reason said then).
 */
IMPORT ER   box_find_prog( CONST UB *name, TS_UUID *prog, UB *pname, INT pmax );

/* Whether an object is a program with an executable: E_OK, and its name shown */
IMPORT ER   box_prog_named( CONST TS_UUID *u, UB *pname, INT pmax );

/*
 * The names of the links that start with `pre`, in the place or (cmd)
 * in the place and bin: how many there are, the first copied to
 * one and the part they all share copied to common.
 */
IMPORT INT  box_complete( CONST UB *pre, BOOL cmd, UB *common, INT cmax, BOOL list );

/* A name as it is typed: '\' before a space, a '/', a '"' or a '\' */
IMPORT INT  box_quote( CONST UB *name, UB *out, INT max );

/* ---------------------------------------------------------------- con_job.c */

/*
 * The one place a program run from the console starts, prints, is
 * waited for and is stopped. A program of the POSIX layer runs on a
 * pseudo terminal of rows by cols whose master the console reads and
 * writes (job_pty_*); what any other prints on its standard output and
 * standard error goes to the system's console (tm_putstring), and the
 * console's record brings it into the window; its standard input is at
 * its end.
 */
IMPORT ER   job_start( CONST TS_UUID *prog, CONST UB *name, INT argc, UB **argv, INT rows, INT cols );
IMPORT BOOL job_running( void );
IMPORT BOOL job_poll( void );		/* TRUE when the program has just ended */
IMPORT void job_stop( void );
IMPORT BOOL job_on_pty( void );		/* a program is on the pty, or its last output is left to read */
IMPORT INT  job_pty_read( UB *buf, INT max );	/* what it wrote, without waiting */
IMPORT void job_pty_write( CONST UB *s, INT n );	/* the keys typed */
IMPORT void job_pty_size( INT rows, INT cols );
IMPORT void job_pty_close( void );

/* ---------------------------------------------------------------- con_ime.c */

/* Letters the line takes: console.c */
IMPORT void con_insert( CONST UB *s, INT n );

IMPORT void job_port( ID port );		/* where the program's end and its output are told (con_job.c) */
IMPORT BOOL ime_key( UINT key, UINT mods, UINT mode );	/* TRUE when the key was the converter's */
IMPORT BOOL ime_busy( void );
IMPORT void ime_commit( void );
IMPORT void ime_press( void );			/* a press in the window: what is composed committed */
IMPORT void ime_pick( INT row );		/* a press on the system's list of candidates (OB_WR_CAND) */
IMPORT void ime_draw( LTWIN *w, INT x, INT y );	/* the composition with its top left at x, y */
IMPORT void ime_end( void );

#endif /* __CON_H__ */
