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
 *	System library dependent definition (x86-64)
 */

#ifndef __TK_SYSLIB_DEPEND_CORE_H__
#define __TK_SYSLIB_DEPEND_CORE_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

/*
 * Interrupt enable/disable (RFLAGS.IF)
 *	disint() returns 1 when interrupts were disabled before, so that
 *	enaint() can restore it and isDI() tells it, as on the other
 *	architecture.
 */
IMPORT UW disint( void );
IMPORT void enaint( UW intsts );

#define DI(intsts)	( (intsts) = (UINT)disint() )
#define EI(intsts)	( enaint((UW)intsts) )
#define isDI(intsts)	( (intsts) != 0 )

/*
 * Interrupt mode (SetIntMode): an IOAPIC input's trigger. Messages
 * (MSI) are always edges.
 */
#define	IM_LEVEL	0x00	/* level (high) detection */
#define	IM_EDGE		0x01	/* rising edge detection */
#define	IM_LOW		0x02	/* or with IM_LEVEL/IM_EDGE: active low */

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

/*
 * What each processor holds, and what a waiter that waits too long
 * calls (spinlock.c, the lockup report of design 8.10)
 */
typedef struct {
	T_SPLOCK * volatile lock;	/* NULL: the entry is free */
	UD	pc;			/* where it was taken */
	UD	since;			/* the system counter then */
} T_SPHELD;

#define SPIN_HELD_N	4

/*
 * Processor c's (PRC c+1's) list of SPIN_HELD_N, which lies in its
 * per-processor data: NULL until the processor is set up
 */
IMPORT T_SPHELD	*knl_spin_held_p[];
IMPORT INT	knl_spin_held_n;		/* entries of knl_spin_held_p there are */
IMPORT UD	knl_spin_stall_cycles;
IMPORT void	(*knl_spin_stall_hook)( T_SPLOCK *lock, UBINT pc, UD waited );
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
 * Barriers and cache maintenance for drivers and the common parts of
 * the kernel (docs/tessronos/04-kernel.md 4.8). Ordinary memory of a PC
 * is coherent with every bus master, and stores reach memory in program
 * order, so most of these only keep the compiler from moving accesses
 * across them.
 *	DeviceBarrier	every earlier access is complete before any later
 *			one, a device's registers included (MFENCE)
 *	SmpMemBarrier	among the processors, earlier loads and stores
 *			before later ones (MFENCE)
 *	SmpWriteBarrier	among the processors, earlier stores before later
 *			ones (they are kept in order already)
 *	CpuRelax	a moment's pause in a loop that spins (PAUSE)
 *	DCacheFlushRange nothing to do for a coherent bus master
 */
#define DCACHE_LINE	64

Inline void DeviceBarrier( void )
{
	__asm__ volatile ( "mfence" ::: "memory" );
}

Inline void SmpMemBarrier( void )
{
	__asm__ volatile ( "mfence" ::: "memory" );
}

Inline void SmpWriteBarrier( void )
{
	__asm__ volatile ( "" ::: "memory" );
}

Inline void CpuRelax( void )
{
	__asm__ volatile ( "pause" ::: "memory" );
}

Inline void DCacheFlushRange( CONST void *p, UBINT len )
{
	(void)p;
	(void)len;
	__asm__ volatile ( "" ::: "memory" );
}

/*
 * The system counter: the time stamp counter, which runs at the same
 * rate on every processor whatever their clocks do (an invariant TSC).
 * ReadSysCounterSync keeps the read behind the instructions before it
 * (LFENCE), ReadSysCounter does not. SysCounterFreq is its rate in Hz,
 * which the kernel measures as it starts (0 until then). CpuHwId is the
 * processor's own number as the hardware names it (the initial APIC
 * ID), known before the kernel has numbered the processors.
 */
IMPORT UD	knl_cntfrq;

Inline UD ReadSysCounter( void )
{
	UW	lo, hi;

	__asm__ volatile ( "rdtsc" : "=a"(lo), "=d"(hi) );
	return ( (UD)hi << 32 ) | lo;
}

Inline UD ReadSysCounterSync( void )
{
	UW	lo, hi;

	__asm__ volatile ( "lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory" );
	return ( (UD)hi << 32 ) | lo;
}

Inline UD SysCounterFreq( void )
{
	return knl_cntfrq;
}

Inline UW CpuHwId( void )
{
	UW	a, b, c, d;

	__asm__ volatile ( "cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0) );
	return b >> 24;
}

/*
 * Memory mapped registers
 *	Addresses are kernel virtual (DEV_BASE), hence UBINT.
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

/*
 * I/O ports (the 16550, the CMOS clock, the PICs, the ACPI registers
 * of the fixed hardware)
 */
Inline void out_port_b( UH port, UB data )
{
	__asm__ volatile ( "outb %0, %1" :: "a"(data), "Nd"(port) : "memory" );
}
Inline void out_port_h( UH port, UH data )
{
	__asm__ volatile ( "outw %0, %1" :: "a"(data), "Nd"(port) : "memory" );
}
Inline void out_port_w( UH port, UW data )
{
	__asm__ volatile ( "outl %0, %1" :: "a"(data), "Nd"(port) : "memory" );
}
Inline UB in_port_b( UH port )
{
	UB	v;

	__asm__ volatile ( "inb %1, %0" : "=a"(v) : "Nd"(port) : "memory" );
	return v;
}
Inline UH in_port_h( UH port )
{
	UH	v;

	__asm__ volatile ( "inw %1, %0" : "=a"(v) : "Nd"(port) : "memory" );
	return v;
}
Inline UW in_port_w( UH port )
{
	UW	v;

	__asm__ volatile ( "inl %1, %0" : "=a"(v) : "Nd"(port) : "memory" );
	return v;
}

/*
 * Model specific registers and CPUID
 */
Inline UD rdmsr( UW msr )
{
	UW	lo, hi;

	__asm__ volatile ( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
	return ( (UD)hi << 32 ) | lo;
}

Inline void wrmsr( UW msr, UD v )
{
	__asm__ volatile ( "wrmsr" :: "c"(msr), "a"((UW)v), "d"((UW)( v >> 32 )) : "memory" );
}

Inline void cpuid( UW leaf, UW sub, UW *a, UW *b, UW *c, UW *d )
{
	__asm__ volatile ( "cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub) );
}

#endif /* __TK_SYSLIB_DEPEND_CORE_H__ */
