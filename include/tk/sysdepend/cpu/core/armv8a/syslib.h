/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syslib.h
 *	System library dependent definition (ARMv8-A / AArch64)
 */

#ifndef __TK_SYSLIB_DEPEND_CORE_H__
#define __TK_SYSLIB_DEPEND_CORE_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

/*
 * Interrupt enable/disable (PSTATE.I)
 *	disint() returns the previous I mask so that enaint() can restore it.
 */
IMPORT UW disint( void );
IMPORT void enaint( UW intsts );

#define DI(intsts)	( (intsts) = (UINT)disint() )
#define EI(intsts)	( enaint((UW)intsts) )
#define isDI(intsts)	( (intsts) != 0 )

/*
 * Interrupt mode (SetIntMode)
 */
#define	IM_LEVEL	0x00	/* level (high) detection */
#define	IM_EDGE		0x01	/* rising edge detection */

/*
 * Spin locks, atomic operations, barriers (spinlock.c, design 8.7)
 */
typedef struct {
	UW	next;		/* next ticket */
	UW	owner;		/* ticket being served */
} T_SPLOCK;

IMPORT void InitSpinLock( T_SPLOCK *lock );
IMPORT void SpinLock( T_SPLOCK *lock );
IMPORT BOOL SpinTryLock( T_SPLOCK *lock );
IMPORT void SpinUnlock( T_SPLOCK *lock );
IMPORT void ISpinLock( T_SPLOCK *lock, UINT *intsts );
IMPORT BOOL ISpinTryLock( T_SPLOCK *lock, UINT *intsts );
IMPORT void ISpinUnlock( T_SPLOCK *lock, UINT *intsts );
IMPORT UW atomic_inc( UW *addr );
IMPORT UW atomic_dec( UW *addr );
IMPORT UW atomic_add( UW *addr, UW val );
IMPORT UW atomic_sub( UW *addr, UW val );
IMPORT UW atomic_xchg( UW *addr, UW val );
IMPORT UW atomic_cmpxchg( UW *addr, UW val, UW cmp );
IMPORT UW atomic_bitset( UW *addr, UW setptn );
IMPORT UW atomic_bitclr( UW *addr, UW clrptn );
IMPORT UD atomic64_add( UD *addr, UD val );
IMPORT UD atomic64_xchg( UD *addr, UD val );
IMPORT UD atomic64_cmpxchg( UD *addr, UD val, UD cmp );
IMPORT void MemoryBarrier( void );
IMPORT void ReadBarrier( void );
IMPORT void WriteBarrier( void );


/*
 * I/O port access
 *	Addresses are 40 bit physical (or kernel virtual) values, hence UBINT.
 */
Inline void out_d( UBINT port, UD data )
{
	*(_UD*)port = data;
}
Inline void out_w( UBINT port, UW data )
{
	*(_UW*)port = data;
}
Inline void out_h( UBINT port, UH data )
{
	*(_UH*)port = data;
}
Inline void out_b( UBINT port, UB data )
{
	*(_UB*)port = data;
}

Inline UD in_d( UBINT port )
{
	return *(_UD*)port;
}
Inline UW in_w( UBINT port )
{
	return *(_UW*)port;
}
Inline UH in_h( UBINT port )
{
	return *(_UH*)port;
}
Inline UB in_b( UBINT port )
{
	return *(_UB*)port;
}

Inline void and_w( UBINT port, UW data )
{
	*(_UW*)port &= data;
}
Inline void and_h( UBINT port, UH data )
{
	*(_UH*)port &= data;
}
Inline void and_b( UBINT port, UB data )
{
	*(_UB*)port &= data;
}

Inline void or_w( UBINT port, UW data )
{
	*(_UW*)port |= data;
}
Inline void or_h( UBINT port, UH data )
{
	*(_UH*)port |= data;
}
Inline void or_b( UBINT port, UB data )
{
	*(_UB*)port |= data;
}

#endif /* __TK_SYSLIB_DEPEND_CORE_H__ */
