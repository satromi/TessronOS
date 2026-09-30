/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	owner.c
 *	Objects of processes (design 9.7): which process a task, semaphore,
 *	event flag, mutex or message buffer is, and the running task's
 *	view of what it may name. The owner is kept in the object's control
 *	block, set when the object is made and looked at inside the calls
 *	(knl_not_owner, kernel.h).
 */

#include "kernel.h"
#include "task.h"
#include "semaphore.h"
#include "eventflag.h"
#include "mutex.h"
#include "messagebuf.h"

LOCAL BOOL own_match( ID o, ID owner )
{
	return ( owner == TK_OWN_ANY ) ? ( o > 0 ) : ( o == owner );
}

/*
 * How many objects of each kind each process has, and all processes
 * together: kept as objects are made and deleted, so that asking costs
 * nothing however large the tables are. Changed and read only in the
 * critical section.
 */
#define OWN_KINDS	( TK_OWN_MBF + 1 )

LOCAL UH	own_cnt[OWN_KINDS][CNF_PID_MAX];
LOCAL INT	own_all[OWN_KINDS];

EXPORT void knl_own_note( INT kind, ID owner, INT d )
{
	if ( kind < 0 || kind >= OWN_KINDS || owner <= 0 || owner >= CNF_PID_MAX ) {
		return;
	}
	own_cnt[kind][owner] = (UH)( own_cnt[kind][owner] + d );
	own_all[kind] += d;
}

LOCAL INT own_count( INT kind, ID owner )
{
	if ( kind < 0 || kind >= OWN_KINDS ) {
		return 0;
	}
	if ( owner == TK_OWN_ANY ) {
		return own_all[kind];
	}
	return ( owner > 0 && owner < CNF_PID_MAX ) ? (INT)own_cnt[kind][owner] : 0;
}

/*
 * The objects of one kind that are the process owner's (TK_OWN_ANY:
 * of any process): as many IDs as max holds are put in ids, and how
 * many there are is answered.
 */
EXPORT INT knl_own_list( INT kind, ID owner, ID *ids, INT max )
{
	INT	i, n = 0, want, all;
	ID	id;

	BEGIN_CRITICAL_SECTION;
	want = all = own_count(kind, owner);
	if ( ids == NULL || max <= 0 ) {
		want = 0;		/* only how many */
	} else if ( want > max ) {
		want = max;		/* the rest need not be looked for */
	}
	switch ( ( want > 0 ) ? kind : -1 ) {
	  case TK_OWN_TSK:
		for ( i = 0; i < NUM_TSKID; i++ ) {
			if ( knl_tcb_table[i].state != TS_NONEXIST
			  && own_match(knl_tcb_table[i].owner, owner) ) {
				id = knl_tcb_table[i].tskid;
				if ( ids != NULL && n < max ) ids[n] = id;
				if ( ++n >= want ) break;
			}
		}
		break;
#if USE_SEMAPHORE == 1
	  case TK_OWN_SEM:
		for ( i = 0; i < NUM_SEMID; i++ ) {
			if ( knl_semcb_table[i].semid != 0 && own_match(knl_semcb_table[i].owner, owner) ) {
				if ( ids != NULL && n < max ) ids[n] = knl_semcb_table[i].semid;
				if ( ++n >= want ) break;
			}
		}
		break;
#endif
#if USE_EVENTFLAG == 1
	  case TK_OWN_FLG:
		for ( i = 0; i < NUM_FLGID; i++ ) {
			if ( knl_flgcb_table[i].flgid != 0 && own_match(knl_flgcb_table[i].owner, owner) ) {
				if ( ids != NULL && n < max ) ids[n] = knl_flgcb_table[i].flgid;
				if ( ++n >= want ) break;
			}
		}
		break;
#endif
#if USE_MUTEX == 1
	  case TK_OWN_MTX:
		for ( i = 0; i < NUM_MTXID; i++ ) {
			if ( knl_mtxcb_table[i].mtxid != 0 && own_match(knl_mtxcb_table[i].owner, owner) ) {
				if ( ids != NULL && n < max ) ids[n] = knl_mtxcb_table[i].mtxid;
				if ( ++n >= want ) break;
			}
		}
		break;
#endif
#if USE_MESSAGEBUFFER == 1
	  case TK_OWN_MBF:
		for ( i = 0; i < NUM_MBFID; i++ ) {
			if ( knl_mbfcb_table[i].mbfid != 0 && own_match(knl_mbfcb_table[i].owner, owner) ) {
				if ( ids != NULL && n < max ) ids[n] = knl_mbfcb_table[i].mbfid;
				if ( ++n >= want ) break;
			}
		}
		break;
#endif
	  default:
		break;
	}
	END_CRITICAL_SECTION;

	return ( want > 0 && n < want ) ? n : all;
}

/*
 * The process the running task's next calls of the core are made for
 * (0: the kernel's own). Read inside the critical section: the task may
 * move to another processor between two instructions outside it.
 */
EXPORT void knl_svc_own( ID owner )
{
	BEGIN_CRITICAL_SECTION;
	if ( knl_ctxtsk != NULL ) {
		knl_ctxtsk->svcown = owner;
	}
	END_CRITICAL_SECTION;
}

EXPORT TCB *knl_tsk_tcb( ID tskid )
{
	if ( tskid < MIN_TSKID || tskid > MAX_TSKID ) {
		return NULL;
	}
	return get_tcb(tskid);
}

EXPORT TCB *knl_tsk_self( void )
{
	TCB	*tcb;

	BEGIN_CRITICAL_SECTION;
	tcb = knl_ctxtsk;
	END_CRITICAL_SECTION;

	return tcb;
}

/* A task the kernel made for a process (its main task) given to it */
EXPORT void knl_tsk_set_owner( ID tskid, ID owner )
{
	TCB	*tcb = knl_tsk_tcb(tskid);

	if ( tcb == NULL ) {
		return;
	}
	BEGIN_CRITICAL_SECTION;
	if ( tcb->state != TS_NONEXIST ) {
		knl_own_note(TK_OWN_TSK, tcb->owner, -1);
		tcb->owner = owner;
		knl_own_note(TK_OWN_TSK, tcb->owner, 1);
	}
	END_CRITICAL_SECTION;
}
