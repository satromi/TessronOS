/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	spinlock.c (ARMv8-A / AArch64)
 *	Spin locks, atomic operations and memory barriers (design 8.7).
 *
 *	The spin lock is a ticket lock: a waiter takes a ticket, then
 *	watches the owner counter with a load-exclusive so that the store
 *	of the releasing processor wakes it from WFE.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include <tk/tkernel.h>

EXPORT void InitSpinLock( T_SPLOCK *lock )
{
	lock->next = 0;
	lock->owner = 0;
}

EXPORT void SpinLock( T_SPLOCK *lock )
{
	UW	ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
	UW	cur;

	for (;;) {
		Asm("ldaxr %w0, [%1]" : "=r"(cur) : "r"(&lock->owner) : "memory");
		if ( cur == ticket ) break;
		Asm("wfe" ::: "memory");
	}
}

EXPORT BOOL SpinTryLock( T_SPLOCK *lock )
{
	UW	n = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);

	if ( __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != n ) {
		return FALSE;
	}
	return __atomic_compare_exchange_n(&lock->next, &n, n + 1, FALSE,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

EXPORT void SpinUnlock( T_SPLOCK *lock )
{
	__atomic_store_n(&lock->owner, lock->owner + 1, __ATOMIC_RELEASE);
	Asm("dsb ish\n\tsev" ::: "memory");
}

EXPORT void ISpinLock( T_SPLOCK *lock, UINT *intsts )
{
	*intsts = disint();
	SpinLock(lock);
}

EXPORT BOOL ISpinTryLock( T_SPLOCK *lock, UINT *intsts )
{
	*intsts = disint();
	if ( SpinTryLock(lock) ) {
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
	Asm("dmb ish" ::: "memory");
}

EXPORT void ReadBarrier( void )
{
	Asm("dmb ishld" ::: "memory");
}

EXPORT void WriteBarrier( void )
{
	Asm("dmb ishst" ::: "memory");
}

#endif /* CPU_CORE_ARMV8A */
