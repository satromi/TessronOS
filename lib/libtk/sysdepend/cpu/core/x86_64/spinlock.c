/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	spinlock.c (x86-64)
 *	Spin locks, atomic operations and memory barriers (design 8.7).
 *
 *	The spin lock is a ticket lock: a waiter takes a ticket (LOCK XADD),
 *	then watches the owner counter, with PAUSE between the looks.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include <tk/tkernel.h>

EXPORT void InitSpinLock( T_SPLOCK *lock )
{
	lock->next = 0;
	lock->owner = 0;
}

/*
 * The locks each processor holds, a few deep, with the place each was
 * taken from and when: what the lockup report (lockup.c) says a
 * processor that stopped is holding. A lock taken deeper than the table
 * goes is held all the same, only not shown. The processor is the one
 * the GS base names (its T_PCPU, whose prcid is 0 until the processor
 * is numbered); before that nothing is kept.
 */
EXPORT T_SPHELD	*knl_spin_held_p[CNF_MAX_PRCID];
EXPORT INT	knl_spin_held_n = 0;

/*
 * A waiter that has not had the lock for knl_spin_stall_cycles of the
 * system counter calls knl_spin_stall_hook (lockup.c), which says so on
 * the console straight away, and goes on waiting; it is called again
 * each time as long again has passed, and once more with 'waited' 0
 * when the lock comes at last. 0 cycles: never.
 */
EXPORT UD	knl_spin_stall_cycles = 0;
EXPORT void	(*knl_spin_stall_hook)( T_SPLOCK *lock, UBINT pc, UD waited ) = NULL;

/* The T_PCPU starts with a pointer to itself, then its prcid */
#define PCPU_PRCID_OFFSET	8

Inline INT spin_prc( void )
{
	INT	id;

	Asm("movl %%gs:%c1, %0" : "=r"(id) : "i"(PCPU_PRCID_OFFSET));
	return id - 1;
}

Inline UD spin_now( void )
{
	return ReadSysCounter();
}

LOCAL void spin_note( T_SPLOCK *lock, UBINT pc )
{
	INT	c = spin_prc(), i;
	T_SPHELD *h;

	if ( c < 0 || c >= knl_spin_held_n || ( h = knl_spin_held_p[c] ) == NULL ) {
		return;
	}
	for ( i = 0; i < SPIN_HELD_N; i++ ) {
		if ( h[i].lock == NULL ) {
			h[i].pc = pc;
			h[i].since = spin_now();
			h[i].lock = lock;
			return;
		}
	}
}

/* Let go of where it was taken: as a rule here, else on the processor that took it */
LOCAL void spin_forget( T_SPLOCK *lock )
{
	INT	c = spin_prc(), i, k, n = knl_spin_held_n;
	T_SPHELD *h;

	if ( c < 0 || c >= n ) {
		return;
	}
	for ( k = 0; k < n; k++, c = ( c + 1 ) % n ) {
		if ( ( h = knl_spin_held_p[c] ) == NULL ) continue;
		for ( i = SPIN_HELD_N - 1; i >= 0; i-- ) {
			if ( h[i].lock == lock ) {
				h[i].lock = NULL;
				return;
			}
		}
	}
}

LOCAL void spin_acquire( T_SPLOCK *lock, UBINT pc )
{
	UW	ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
	UW	cur;
	UD	t0 = 0, last = 0, lim = knl_spin_stall_cycles, now;
	BOOL	told = FALSE;

	for (;;) {
		cur = __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE);
		if ( cur == ticket ) break;
		if ( lim != 0 ) {
			now = spin_now();
			if ( t0 == 0 ) {
				t0 = last = now;
			} else if ( now - last >= lim && knl_spin_stall_hook != NULL ) {
				last = now;
				told = TRUE;
				(*knl_spin_stall_hook)(lock, pc, now - t0);
				continue;
			}
		}
		Asm("pause" ::: "memory");
	}
	if ( told && knl_spin_stall_hook != NULL ) {
		(*knl_spin_stall_hook)(lock, pc, 0);
	}
	spin_note(lock, pc);
}

EXPORT void SpinLock( T_SPLOCK *lock )
{
	spin_acquire(lock, (UBINT)__builtin_return_address(0));
}

LOCAL BOOL spin_try( T_SPLOCK *lock, UBINT pc )
{
	UW	n = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);

	if ( __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != n ) {
		return FALSE;
	}
	if ( !__atomic_compare_exchange_n(&lock->next, &n, n + 1, FALSE,
					  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ) {
		return FALSE;
	}
	spin_note(lock, pc);
	return TRUE;
}

EXPORT BOOL SpinTryLock( T_SPLOCK *lock )
{
	return spin_try(lock, (UBINT)__builtin_return_address(0));
}

EXPORT void SpinUnlock( T_SPLOCK *lock )
{
	spin_forget(lock);
	__atomic_store_n(&lock->owner, lock->owner + 1, __ATOMIC_RELEASE);
}

EXPORT void ISpinLock( T_SPLOCK *lock, UINT *intsts )
{
	*intsts = disint();
	spin_acquire(lock, (UBINT)__builtin_return_address(0));
}

EXPORT BOOL ISpinTryLock( T_SPLOCK *lock, UINT *intsts )
{
	*intsts = disint();
	if ( spin_try(lock, (UBINT)__builtin_return_address(0)) ) {
		return TRUE;
	}
	enaint(*intsts);
	return FALSE;
}

EXPORT void ISpinUnlock( T_SPLOCK *lock, UINT *intsts )
{
	SpinUnlock(lock);
	enaint(*intsts);
}

/*
 * Atomic operations (return values as in the SMP T-Kernel specification:
 * inc/dec/add/sub return the new value, the others the previous value)
 */
EXPORT UW atomic_inc( UW *addr )
{
	return __atomic_add_fetch(addr, 1, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_dec( UW *addr )
{
	return __atomic_sub_fetch(addr, 1, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_add( UW *addr, UW val )
{
	return __atomic_add_fetch(addr, val, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_sub( UW *addr, UW val )
{
	return __atomic_sub_fetch(addr, val, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_xchg( UW *addr, UW val )
{
	return __atomic_exchange_n(addr, val, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_cmpxchg( UW *addr, UW val, UW cmp )
{
	__atomic_compare_exchange_n(addr, &cmp, val, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return cmp;			/* the value found at addr */
}

EXPORT UW atomic_bitset( UW *addr, UW setptn )
{
	return __atomic_fetch_or(addr, setptn, __ATOMIC_ACQ_REL);
}

EXPORT UW atomic_bitclr( UW *addr, UW clrptn )
{
	return __atomic_fetch_and(addr, clrptn, __ATOMIC_ACQ_REL);
}

EXPORT UD atomic64_add( UD *addr, UD val )
{
	return __atomic_add_fetch(addr, val, __ATOMIC_ACQ_REL);
}

EXPORT UD atomic64_xchg( UD *addr, UD val )
{
	return __atomic_exchange_n(addr, val, __ATOMIC_ACQ_REL);
}

EXPORT UD atomic64_cmpxchg( UD *addr, UD val, UD cmp )
{
	__atomic_compare_exchange_n(addr, &cmp, val, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return cmp;
}

EXPORT void MemoryBarrier( void )
{
	Asm("mfence" ::: "memory");
}

EXPORT void ReadBarrier( void )
{
	Asm("lfence" ::: "memory");
}

EXPORT void WriteBarrier( void )
{
	Asm("sfence" ::: "memory");
}

#endif /* CPU_CORE_X86_64 */
