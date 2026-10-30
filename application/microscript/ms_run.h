/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_run.h
 *	What the parts of the run time share among themselves
 */

#ifndef __MS_RUN_H__
#define __MS_RUN_H__

#include "ms.h"
#include <stdarg.h>
#include <stdio.h>

#define MS_MAX_GLOBAL	1048576		/* elements of a global array */
#define MS_MAX_LOCAL	4096		/* and of a local one */
#define MS_MAX_THREADS	64
#define MS_STACK	( 96 * 1024 )	/* the stack of a thread */
#define MS_MAX_DEPTH	128		/* procedures called inside each other */
#define MS_STACK_SPARE	( 16 * 1024 )	/* what a call must leave on a thread's stack */

/* How a block ended */
#define FL_NONE		0
#define FL_BREAK	1
#define FL_CONTINUE	2
#define FL_EXIT		3
#define FL_STOP		4		/* the thread is stopping: an error, TERMINATE, FINISH */

/* What a switch keeps: x19..x30, sp, d8..d15 */
typedef struct {
	UD	x[12];
	UD	sp;
	UD	d[8];
} MSCTX;

typedef struct msthread {
	INT		id;
	MSSTR		*name;
	MSPROC		*proc;
	BOOL		terminated, waiting, brk, done, turn, err, epilogue;
	BOOL		stir;		/* waits also for the run to stir (th_wait) */
	BOOL		again;		/* its turn ended looking again at what it waits for */
	double		cnt;		/* $CNT */
	INT		depth;
	INT		steps;		/* since it last let the others go */
	MSV		exitval;
	MSV		*args;
	INT		nargs;
	MSCTX		ctx;
	void		*stack;
	UD		wake;
	char		errmsg[300];
	MSSTR		*evkey;		/* the segment whose event started it, or NULL */
	struct msthread	*next;
} MSTH;

typedef struct {
	MSSCOPE		*global;
	MSTH		*threads;
	MSTH		*cur;
	MSCTX		sched;
	INT		next_id;
	BOOL		finished, epilogue_done;
	INT		lasterr;
	void		*segconsume;
} MSRT;

extern MSRT	rt;

void	ms_co_switch( MSCTX *from, MSCTX *to );
void	ms_co_entry( void );
void	ms_co_done( void );

INT	ms_int32( double d );
void	ms_num_text( double d, MSBUF *b );
BOOL	mv_truthy( MSV v );
MSSTR	*mv_name( MSV v );

MSV	var_get( const MSVAR *v, INT i );
void	var_set( MSVAR *v, INT i, MSV val );
void	var_free( MSVAR *v );
void	var_free_data( MSVAR *v );
MSVAR	*scope_find( MSSCOPE *s, MSSTR *name );
MSVAR	*scope_declare( MSSCOPE *s, MSDECL *d, INT size, INT share_off, BOOL force_array, BOOL local );
MSVAR	*scope_bind_alias( MSSCOPE *s, MSSTR *name, char type, MSVAR *of );
MSSCOPE	*scope_new( MSSCOPE *parent );
void	scope_free( MSSCOPE *s );

void	th_fail( MSTH *t, const char *fmt, ... );
BOOL	th_stop( MSTH *t );

/*
 * A thread waits: until the time at (MS_NEVER: no time), and, with
 * stir, until the run stirs (rt_stir) -- a notice came to the window's
 * channel, or another thread did something -- which may have made true
 * what it waits for. again says that the thread comes back to look at
 * the same thing once more, having done nothing else since: such a
 * turn does not stir the others, so that threads that wait for each
 * other's changes do not wake each other for ever. A thread that waits
 * for something the program is not told of -- a process ending, a
 * message, the bytes of a serial line or of a socket, a window opened
 * from a link closing -- looks at it again every MS_LOOK_MS (th_look)
 * as well. rt_wake wakes a thread whatever it waits for: BREAK,
 * TERMINATE and the run ending, which it then sees (th_stop, brk).
 */
#define MS_NEVER	( ~(UD)0 )
#define MS_LOOK_MS	50
void	th_wait( MSTH *t, UD at, BOOL stir, BOOL again );
UD	th_until( UD start, double tmo );	/* the end of a wait of tmo seconds, MS_NEVER none */
void	th_look( MSTH *t, UD at, BOOL again );
void	rt_stir( void );
void	rt_wake( MSTH *t );
void	rt_wake_all( void );
MSSEG	*th_seg( MSTH *t, MSN *e, MSSCOPE *sc );
MSV	ev( MSTH *t, MSN *n, MSSCOPE *sc );
INT	exec_block( MSTH *t, MSN **v, INT n, MSSCOPE *sc );
INT	exec_other( MSTH *t, MSN *s, MSSCOPE *sc );
void	assign( MSTH *t, MSN *target, MSV *vals, INT nvals, MSSCOPE *sc );
MSV	call_proc( MSTH *t, MSPROC *p, MSN **argn, INT nargn, MSSCOPE *sc );
MSPROC	*rt_proc( MSSTR *name );
MSTH	*rt_spawn( MSPROC *proc, MSV *args, INT nargs, BOOL epilogue );
INT	rt_last_spawned( void );
void	rt_bind_handlers( void );
void	rt_bind_segment( MSSEG *s );
BOOL	rt_epilogue_done( void );

/* ms_rtlib.c */
MSV	sysvar_get( MSTH *t, MSSTR *name, MSV index, MSSCOPE *sc );

/* What a statement's arguments give, for the statements outside ms_rtlib.c */
MSVAR	*rt_place( MSTH *t, MSN *e, MSSCOPE *sc, INT *p_start );	/* the place an argument names */
double	rt_num( MSTH *t, MSN *e, MSSCOPE *sc );
MSV	rt_whole( MSTH *t, MSN *e, MSSCOPE *sc );	/* the name of an array alone as the whole array */
void	rt_format( MSTH *t, MSN **v, INT n, MSSCOPE *sc, MSBUF *out );	/* a format and its arguments */
INT	ms_net_exec( MSTH *t, MSN *s, MSSCOPE *sc );
BOOL	ms_net_sysvar( const char *u, INT i, MSV *out );
void	sysvar_set( MSTH *t, MSSTR *name, MSV index, MSV val, MSSCOPE *sc );
MSV	call_builtin( MSTH *t, MSN *n, MSSCOPE *sc );
MSV	seg_state( MSSEG *s, MSSTR *state );
void	seg_state_set( MSSEG *s, MSSTR *state, MSV v );
INT	seg_consume( MSSTR *name );
INT	rt_mmask( void );

#endif /* __MS_RUN_H__ */
