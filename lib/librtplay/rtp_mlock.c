/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtp_mlock.c
 *	The C library's memory taken by one task at a time, for a program
 *	of newlib alone that plays 実時間制御TAD (include/ts/rtplay.h)
 *
 *	The player's task takes memory while the program's own task does
 *	too, and newlib's malloc keeps one heap: its lock is made a
 *	semaphore of one that the task holding it may take again. The
 *	semaphore is made the first time the lock is taken, which is before
 *	any second task is there (the program takes memory before it opens
 *	a player). A program linked with the POSIX threads has the locks of
 *	that library and does not link this.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <string.h>
#include <reent.h>

static ID		ml_sem = 0;
static volatile ID	ml_owner = 0;
static INT		ml_depth = 0;

void __malloc_lock( struct _reent *r )
{
	ID	me;

	(void)r;
	if ( ml_sem == 0 ) {
		T_CSEM	s;

		memset(&s, 0, sizeof(s));
		s.sematr = TA_TFIFO;
		s.isemcnt = 1;
		s.maxsem = 1;
		ml_sem = tk_cre_sem(&s);
	}
	if ( ml_sem <= 0 ) return;
	me = tk_get_tid();
	if ( ml_owner == me ) {
		ml_depth++;
		return;
	}
	(void)tk_wai_sem(ml_sem, 1, TMO_FEVR);
	ml_owner = me;
	ml_depth = 1;
}

void __malloc_unlock( struct _reent *r )
{
	(void)r;
	if ( ml_sem <= 0 ) return;
	if ( --ml_depth > 0 ) return;
	ml_owner = 0;
	(void)tk_sig_sem(ml_sem, 1);
}
