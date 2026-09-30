/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_arch.c
 *	What lwIP needs from the kernel (design 12.6).
 *
 *	Semaphores, mutexes and threads are kernel objects. A mailbox is
 *	built here out of a ring and two semaphores, one counting what is in
 *	it and one counting the room left, because lwIP wants a fixed depth
 *	and a sender that blocks when the ring is full.
 *
 *	Times are milliseconds on lwIP's side. Its idea of "no timeout" is
 *	zero, which is the opposite of TMO_FEVR, so every wait converts.
 */

#include <tk/tkernel.h>
#include <ts/time.h>
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"

#if !NO_SYS

/* ---------------------------------------------------------------- time */

/*
 * Milliseconds since the machine started. lwIP only ever takes the
 * difference of two of these, so where the count starts does not matter,
 * but it must not go backwards.
 */
EXPORT u32_t sys_now( void )
{
	UD	ns = 0;

	ts_get_mono(&ns);

	return (u32_t)(ns / 1000000U);
}

/* ---------------------------------------------------------------- locking */

/*
 * The coarse lock lwIP takes around the short critical sections it has
 * of its own (its pools, the counts of references of its buffers). The
 * stack's tasks run on every processor at once, so the lock is a spin
 * lock taken with the processor's interrupts off; stopping only this
 * processor's dispatcher would leave the others running into the same
 * section. lwIP takes it again inside itself, so the task holding it
 * only counts a nested take.
 */
LOCAL T_SPLOCK		prot_lock;
LOCAL volatile ID	prot_owner;	/* the task holding it, 0 none */
LOCAL UINT		prot_depth;
LOCAL UINT		prot_imask;

EXPORT sys_prot_t sys_arch_protect( void )
{
	ID	me = tk_get_tid();
	UINT	imask;

	if ( prot_owner == me ) {
		prot_depth++;
		return 1;
	}
	ISpinLock(&prot_lock, &imask);
	prot_owner = me;
	prot_depth = 1;
	prot_imask = imask;

	return 1;
}

EXPORT void sys_arch_unprotect( sys_prot_t pval )
{
	UINT	imask;

	if ( --prot_depth > 0 ) {
		return;
	}
	imask = prot_imask;
	prot_owner = 0;
	ISpinUnlock(&prot_lock, &imask);
}

/*
 * The rings of the mailboxes: senders and takers on different
 * processors must not take the same slot
 */
LOCAL T_SPLOCK		mbox_lock;

/* ---------------------------------------------------------------- semaphores */

EXPORT err_t sys_sem_new( sys_sem_t *sem, u8_t count )
{
	T_CSEM	csem;

	if ( sem == NULL ) {
		return ERR_ARG;
	}
	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = (INT)count;
	csem.maxsem  = 0xffff;

	sem->id = (int)tk_cre_sem(&csem);

	return ( sem->id > 0 ) ? ERR_OK : ERR_MEM;
}

EXPORT void sys_sem_free( sys_sem_t *sem )
{
	if ( sem != NULL && sem->id > 0 ) {
		tk_del_sem((ID)sem->id);
		sem->id = 0;
	}
}

EXPORT void sys_sem_signal( sys_sem_t *sem )
{
	if ( sem != NULL && sem->id > 0 ) {
		tk_sig_sem((ID)sem->id, 1);
	}
}

/*
 * Waits up to `timeout` milliseconds, zero meaning forever, and gives
 * back how long it took or SYS_ARCH_TIMEOUT.
 */
EXPORT u32_t sys_arch_sem_wait( sys_sem_t *sem, u32_t timeout )
{
	u32_t	start = sys_now();
	ER	er;

	if ( sem == NULL || sem->id <= 0 ) {
		return SYS_ARCH_TIMEOUT;
	}
	er = tk_wai_sem((ID)sem->id, 1,
			( timeout == 0 ) ? TMO_FEVR : (TMO)timeout);
	if ( er < E_OK ) {
		return SYS_ARCH_TIMEOUT;
	}

	return sys_now() - start;
}

/* ---------------------------------------------------------------- mutexes */

EXPORT err_t sys_mutex_new( sys_mutex_t *mutex )
{
	T_CMTX	cmtx;

	if ( mutex == NULL ) {
		return ERR_ARG;
	}
	cmtx.exinf  = NULL;
	cmtx.mtxatr = TA_TFIFO | TA_INHERIT;
	cmtx.ceilpri = 0;

	mutex->id = (int)tk_cre_mtx(&cmtx);

	return ( mutex->id > 0 ) ? ERR_OK : ERR_MEM;
}

EXPORT void sys_mutex_free( sys_mutex_t *mutex )
{
	if ( mutex != NULL && mutex->id > 0 ) {
		tk_del_mtx((ID)mutex->id);
		mutex->id = 0;
	}
}

EXPORT void sys_mutex_lock( sys_mutex_t *mutex )
{
	if ( mutex != NULL && mutex->id > 0 ) {
		tk_loc_mtx((ID)mutex->id, TMO_FEVR);
	}
}

EXPORT void sys_mutex_unlock( sys_mutex_t *mutex )
{
	if ( mutex != NULL && mutex->id > 0 ) {
		tk_unl_mtx((ID)mutex->id);
	}
}

/* ---------------------------------------------------------------- mailboxes */

EXPORT err_t sys_mbox_new( sys_mbox_t *mbox, int size )
{
	T_CSEM	csem;

	if ( mbox == NULL ) {
		return ERR_ARG;
	}
	if ( size <= 0 || size > SYS_ARCH_MBOX_SIZE ) {
		size = SYS_ARCH_MBOX_SIZE;
	}
	mbox->head = 0;
	mbox->tail = 0;
	mbox->size = size;

	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;			/* nothing in it yet */
	csem.maxsem  = (INT)size;
	mbox->id = (int)tk_cre_sem(&csem);

	csem.isemcnt = (INT)size;		/* all of it is free */
	mbox->free_id = (int)tk_cre_sem(&csem);

	if ( mbox->id <= 0 || mbox->free_id <= 0 ) {
		if ( mbox->id > 0 ) {
			tk_del_sem((ID)mbox->id);
		}
		if ( mbox->free_id > 0 ) {
			tk_del_sem((ID)mbox->free_id);
		}
		mbox->id = 0;
		mbox->free_id = 0;
		return ERR_MEM;
	}

	return ERR_OK;
}

EXPORT void sys_mbox_free( sys_mbox_t *mbox )
{
	if ( mbox != NULL && mbox->id > 0 ) {
		tk_del_sem((ID)mbox->id);
		tk_del_sem((ID)mbox->free_id);
		mbox->id = 0;
		mbox->free_id = 0;
	}
}

/*
 * Put one message in, waiting for room. The ring is touched under
 * mbox_lock so that two senders cannot take the same slot.
 */
EXPORT void sys_mbox_post( sys_mbox_t *mbox, void *msg )
{
	if ( mbox == NULL || mbox->id <= 0 ) {
		return;
	}
	tk_wai_sem((ID)mbox->free_id, 1, TMO_FEVR);

	{
		UINT	imask;

		ISpinLock(&mbox_lock, &imask);
		mbox->msg[mbox->tail] = msg;
		mbox->tail = ( mbox->tail + 1 ) % mbox->size;
		ISpinUnlock(&mbox_lock, &imask);
	}

	tk_sig_sem((ID)mbox->id, 1);
}

/*
 * The same, but never waiting: used from places that must not block.
 */
EXPORT err_t sys_mbox_trypost( sys_mbox_t *mbox, void *msg )
{
	if ( mbox == NULL || mbox->id <= 0 ) {
		return ERR_ARG;
	}
	if ( tk_wai_sem((ID)mbox->free_id, 1, TMO_POL) < E_OK ) {
		return ERR_MEM;			/* it is full */
	}

	{
		UINT	imask;

		ISpinLock(&mbox_lock, &imask);
		mbox->msg[mbox->tail] = msg;
		mbox->tail = ( mbox->tail + 1 ) % mbox->size;
		ISpinUnlock(&mbox_lock, &imask);
	}

	tk_sig_sem((ID)mbox->id, 1);

	return ERR_OK;
}

EXPORT err_t sys_mbox_trypost_fromisr( sys_mbox_t *mbox, void *msg )
{
	return sys_mbox_trypost(mbox, msg);
}

/*
 * Take one message out, waiting up to `timeout` milliseconds, zero
 * meaning forever.
 */
EXPORT u32_t sys_arch_mbox_fetch( sys_mbox_t *mbox, void **msg, u32_t timeout )
{
	u32_t	start = sys_now();
	ER	er;

	if ( mbox == NULL || mbox->id <= 0 ) {
		return SYS_ARCH_TIMEOUT;
	}
	er = tk_wai_sem((ID)mbox->id, 1,
			( timeout == 0 ) ? TMO_FEVR : (TMO)timeout);
	if ( er < E_OK ) {
		return SYS_ARCH_TIMEOUT;
	}

	{
		UINT	imask;

		ISpinLock(&mbox_lock, &imask);
		if ( msg != NULL ) {
			*msg = mbox->msg[mbox->head];
		}
		mbox->head = ( mbox->head + 1 ) % mbox->size;
		ISpinUnlock(&mbox_lock, &imask);
	}

	tk_sig_sem((ID)mbox->free_id, 1);

	return sys_now() - start;
}

EXPORT u32_t sys_arch_mbox_tryfetch( sys_mbox_t *mbox, void **msg )
{
	if ( mbox == NULL || mbox->id <= 0 ) {
		return SYS_MBOX_EMPTY;
	}
	if ( tk_wai_sem((ID)mbox->id, 1, TMO_POL) < E_OK ) {
		return SYS_MBOX_EMPTY;
	}

	{
		UINT	imask;

		ISpinLock(&mbox_lock, &imask);
		if ( msg != NULL ) {
			*msg = mbox->msg[mbox->head];
		}
		mbox->head = ( mbox->head + 1 ) % mbox->size;
		ISpinUnlock(&mbox_lock, &imask);
	}

	tk_sig_sem((ID)mbox->free_id, 1);

	return 0;
}

/* ---------------------------------------------------------------- threads */

/*
 * A task entry takes a start code and the information word, while lwIP
 * offers a function and one pointer. The pointer will not fit in the
 * start code, so the pair is kept here and the entry below unpacks it.
 * lwIP starts only a few threads and never stops one.
 */
#define SYS_ARCH_MAX_THREAD	4

typedef struct {
	lwip_thread_fn	fn;
	void		*arg;
} SYS_THREAD_ARG;

LOCAL SYS_THREAD_ARG	thread_arg[SYS_ARCH_MAX_THREAD];
LOCAL INT		thread_used = 0;

LOCAL void thread_entry( INT stacd, void *exinf )
{
	SYS_THREAD_ARG	*a = (SYS_THREAD_ARG *)exinf;

	a->fn(a->arg);

	tk_exd_tsk();			/* lwIP threads do not return */
}

EXPORT sys_thread_t sys_thread_new( const char *name, lwip_thread_fn thread,
				    void *arg, int stacksize, int prio )
{
	T_CTSK		ctsk;
	SYS_THREAD_ARG	*a;
	ID		tskid;

	if ( thread == NULL || thread_used >= SYS_ARCH_MAX_THREAD ) {
		return 0;
	}
	a = &thread_arg[thread_used++];
	a->fn  = thread;
	a->arg = arg;

	ctsk.exinf   = a;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)thread_entry;
	ctsk.itskpri = (PRI)(( prio > 0 ) ? prio : DEFAULT_THREAD_PRIO);
	ctsk.stksz   = ( stacksize > 0 ) ? stacksize : DEFAULT_THREAD_STACKSIZE;

	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		thread_used--;
		return 0;
	}
	if ( tk_sta_tsk(tskid, 0) < E_OK ) {
		tk_del_tsk(tskid);
		thread_used--;
		return 0;
	}

	return (sys_thread_t)tskid;
}

/*
 * Nothing of the platform has to be set up before the stack starts: the
 * kernel is already running by the time lwIP is brought up.
 */
EXPORT void sys_init( void )
{
}

#endif /* !NO_SYS */
