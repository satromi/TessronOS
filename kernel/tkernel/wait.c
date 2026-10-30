/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/*
 *	wait.c
 *	Common Routine for Synchronization
 *
 *	TessronOS: a wait begins and ends under the lock of what the task
 *	waits on and the scheduler's lock (design 4.3.3). The scheduler's
 *	lock is held from the task's being made ready until all it is to be
 *	given is written: the processors are assigned again only as that
 *	lock is let go, so no processor takes the task before then.
 */

#include "kernel.h"
#include "wait.h"

EXPORT void knl_wait_release_ok( TCB *tcb )
{
	KLK_SCHED_DI();
	knl_wait_release(tcb);
	KT_SPIN(knl_kt_release_spin)
	*tcb->wercd = E_OK;
	KLK_UNSCHED();
}

EXPORT void knl_wait_release_ok_ercd( TCB *tcb, ER ercd )
{
	KLK_SCHED_DI();
	knl_wait_release(tcb);
	*tcb->wercd = ercd;
	KLK_UNSCHED();
}

EXPORT void knl_wait_release_ng( TCB *tcb, ER ercd )
{
	KLK_SCHED_DI();
	knl_wait_release(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
	*tcb->wercd = ercd;
	KLK_UNSCHED();
}

EXPORT void knl_wait_release_tmout( TCB *tcb )
{
	KLK_SCHED_DI();
	QueRemove(&tcb->tskque);
	knl_make_non_wait(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
	KLK_UNSCHED();
}

/*
 * The timer's callback for a wait's limit. With the kernel's locks
 * (design 4.3.3) it is called holding none: it takes the lock of what
 * the task waits on and the scheduler's, and ends the wait only if it is
 * still the wait the limit was set for.
 */
EXPORT void knl_wait_tmout( TCB *tcb )
{
	KLOCK	*l = knl_lock_wait_of(tcb);

	if ( (tcb->state & TS_WAIT) != 0 && !knl_timer_stale(&tcb->wtmeb) ) {
		knl_wait_release_tmout(tcb);
	}
	knl_unlock_wait_of(l);
}

#if KLK_FINE_OBJ
/* The lock of the wait the task is in, NULL for none */
LOCAL KLOCK *wait_lock( TCB *tcb )
{
	if ( (tcb->state & TS_WAIT) == 0 || tcb->wspec == NULL || tcb->wspec->wlock == NULL ) {
		return NULL;
	}
	return (*tcb->wspec->wlock)(tcb);
}

LOCAL INT wait_lock_rank( KLOCK *l )
{
	return ( l == &knl_tctl_klk ) ? KLK_R_TCTL : ( l == &knl_por_klk ) ? KLK_R_POR : KLK_R_OBJ;
}

/*
 * The scheduler's lock comes after the objects' in the order, so the
 * lock of the wait is found under it, taken without it, and the task
 * looked at again: if it waits on something else by then, again.
 */
EXPORT KLOCK *knl_lock_wait_of( TCB *tcb )
{
	KLOCK	*l;

	for (;;) {
		KLK_SCHED_DI();
		l = wait_lock(tcb);
		if ( l == NULL || knl_klk_mine(l) ) {
			return NULL;
		}
		KLK_UNSCHED();
		KLK_KT_SPIN(knl_kt_waitof_spin)
		knl_klk_lock(l, wait_lock_rank(l), FALSE);
		KLK_SCHED_DI();
		if ( wait_lock(tcb) == l ) {
			return l;
		}
		KLK_UNSCHED();
		if ( l == &knl_tctl_klk ) {
			knl_tctl_unlock();
		} else {
			knl_klk_unlock(l);
		}
	}
}

EXPORT void knl_unlock_wait_of( KLOCK *l )
{
	KLK_UNSCHED();
	if ( l == &knl_tctl_klk ) {
		knl_tctl_unlock();
	} else if ( l != NULL ) {
		knl_klk_unlock(l);
	}
}
#endif /* KLK_FINE_OBJ */

/*
 * Change the active task state to wait state and connect to the
 * timer event queue.
 *	Normally, 'knl_ctxtsk' is in the RUN state, but when an interrupt
 *	occurs during executing system call, 'knl_ctxtsk' may become the
 *	other state by system call called in the interrupt handler.
 *	However, it does not be in WAIT state.
 *
 *	"include/tk/typedef.h"
 *	typedef	W		TMO;
 *	typedef UW		RELTIM;
 *	#define TMO_FEVR	(-1)
 */
LOCAL void knl_make_wait_state( void )
{
	switch ( knl_ctxtsk->state ) {
	  case TS_READY:
		knl_make_non_ready(knl_ctxtsk);
		knl_ctxtsk->state = TS_WAIT;
		break;
	  case TS_SUSPEND:
		knl_ctxtsk->state = TS_WAITSUS;
		break;
	}
}

EXPORT void knl_make_wait( TMO tmout, ATR atr )
{
	KLK_SCHED_DI();
	knl_make_wait_state();
	knl_timer_insert(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_tmout, knl_ctxtsk);
	KLK_UNSCHED();
}

EXPORT void knl_make_wait_reltim( RELTIM tmout, ATR atr )
{
	KLK_SCHED_DI();
	knl_make_wait_state();
	knl_timer_insert_reltim(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_tmout, knl_ctxtsk);
	KLK_UNSCHED();
}

/*
 * Microsecond versions (design 7.6)
 */
EXPORT void knl_make_wait_u( TMO_U tmout_u, ATR atr )
{
	KLK_SCHED_DI();
	knl_make_wait_state();
	knl_timer_insert_u(&knl_ctxtsk->wtmeb, tmout_u, (CBACK)knl_wait_tmout, knl_ctxtsk);
	KLK_UNSCHED();
}

EXPORT void knl_make_wait_reltim_u( RELTIM_U tmout_u, ATR atr )
{
	KLK_SCHED_DI();
	knl_make_wait_state();
	knl_timer_insert_reltim_u(&knl_ctxtsk->wtmeb, tmout_u, (CBACK)knl_wait_tmout, knl_ctxtsk);
	KLK_UNSCHED();
}

/*
 * Release all tasks connected to the wait queue, and define it
 * as E_DLT error.
 */
EXPORT void knl_wait_delete( QUEUE *wait_queue )
{
	TCB	*tcb;

	KLK_SCHED_DI();
	while ( !isQueEmpty(wait_queue) ) {
		tcb = (TCB*)wait_queue->next;
		knl_wait_release(tcb);
		*tcb->wercd = E_DLT;
	}
	KLK_UNSCHED();
}

/*
 * Get ID of the head task in the wait queue.
 */
EXPORT ID knl_wait_tskid( QUEUE *wait_queue )
{
	if ( isQueEmpty(wait_queue) ) {
		return 0;
	}

	return ((TCB*)wait_queue->next)->tskid;
}

/*
 * Change the active task state to wait state and connect to the timer wait
 * queue and the object wait queue. Also set 'wid' in 'knl_ctxtsk'.
 */
EXPORT void knl_gcb_make_wait( GCB *gcb, TMO tmout )
{
	*knl_ctxtsk->wercd = E_TMOUT;
	if ( tmout != TMO_POL ) {
		KLK_SCHED_DI();
		knl_ctxtsk->wid = gcb->objid;
		knl_make_wait(tmout, gcb->objatr);
		if ( (gcb->objatr & TA_TPRI) != 0 ) {
			knl_queue_insert_tpri(knl_ctxtsk, &gcb->wait_queue);
		} else {
			QueInsert(&knl_ctxtsk->tskque, &gcb->wait_queue);
		}
		KLK_UNSCHED();
	}
}

EXPORT void knl_gcb_make_wait_u( GCB *gcb, TMO_U tmout_u )
{
	*knl_ctxtsk->wercd = E_TMOUT;
	if ( tmout_u != TMO_POL ) {
		KLK_SCHED_DI();
		knl_ctxtsk->wid = gcb->objid;
		knl_make_wait_u(tmout_u, gcb->objatr);
		if ( (gcb->objatr & TA_TPRI) != 0 ) {
			knl_queue_insert_tpri(knl_ctxtsk, &gcb->wait_queue);
		} else {
			QueInsert(&knl_ctxtsk->tskque, &gcb->wait_queue);
		}
		KLK_UNSCHED();
	}
}

/*
 * When the task priority changes, adjust the task position at the wait queue.
 * It is called only if the object attribute TA_TPRI is specified.
 *
 */
EXPORT void knl_gcb_change_priority( GCB *gcb, TCB *tcb )
{
	QueRemove(&tcb->tskque);
	knl_queue_insert_tpri(tcb, &gcb->wait_queue);
}

/*
 * Search the first task of wait queue include "tcb" with target.
 * (Not insert "tcb" into wait queue.)
 *
 */
EXPORT TCB* knl_gcb_top_of_wait_queue( GCB *gcb, TCB *tcb )
{
	TCB	*q;

	if ( isQueEmpty(&gcb->wait_queue) ) {
		return tcb;
	}

	q = (TCB*)gcb->wait_queue.next;
	if ( (gcb->objatr & TA_TPRI) == 0 ) {
		return q;
	}

	return ( tcb->priority < q->priority )? tcb: q;
}
