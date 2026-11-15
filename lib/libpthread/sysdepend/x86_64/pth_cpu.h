/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_cpu.h
 *	A thread's processor (x86-64). The thread pointer is the FS base,
 *	which the kernel keeps with the task. A thread sets its own through
 *	ts_set_tls, a system call every processor has: WRFSBASE is not in
 *	every one (the 2nd generation Core i has none), and a thread sets its
 *	pointer only when it starts and when it ends, where a call costs
 *	nothing that counts. The pointer is read at %fs:0, the control
 *	block's word with its own address, as the code the compiler makes
 *	for a thread local variable reads it, again on every processor. A
 *	task has its pointer set before it calls anything here: one without,
 *	whose FS base is 0, faults at the read.
 *
 *	The TLS is variant II: the thread's copy of the TLS image ends where
 *	the thread pointer points, rounded up to the image's alignment, and
 *	the control block starts there. The compiler reaches a thread local
 *	variable at the thread pointer minus the image's size so rounded
 *	plus the variable's offset in the image. The control block's first
 *	word is its own address, as the ABI asks (%fs:0 is the thread
 *	pointer); the second is the thread's record (PTH), the third where
 *	it ends (pth_thread.c).
 *
 *	The block is as long as the head of the GNU C library's (tcbhead_t,
 *	64 bytes): code made for Linux on x86-64 reads its stack protector's
 *	value at %fs:0x28 (stack_guard), and glibc keeps its pointer guard at
 *	%fs:0x30. Those two words are filled for a shared object made for
 *	glibc that the browser loads (design 14.6.3.1): the guard a value of
 *	the thread's own, its lowest byte zero as glibc's, the pointer guard
 *	zero.
 */

#ifndef __PTH_CPU_X86_64_H__
#define __PTH_CPU_X86_64_H__

#include <stddef.h>
#include <stdint.h>

#define PTH_TCB_SIZE	64		/* the control block */
#define PTH_TCB_SELF	0		/* its word with its own address */
#define PTH_TCB_OWNER	1		/* its word with the thread's record */
#define PTH_TCB_EXIT	2		/* its word with where the thread ends */
#define PTH_TCB_GUARD	5		/* glibc's stack_guard (%fs:0x28); the pointer guard after it */

/* lib/libts: the calling task's FS base (kernel/sysdepend/cpu/core/x86_64/syscall.c) */
extern int	ts_set_tls( void *tp );

static inline void *pth_tp_get( void )
{
	void	*tp;

	__asm__ volatile ( "mov %%fs:0, %0" : "=r"(tp) );
	return tp;
}

static inline void pth_tp_set( void *tp )
{
	(void)ts_set_tls(tp);
	__asm__ volatile ( "" ::: "memory" );
}

/* The stack protector's value for the thread whose control block is at tp */
static inline uintptr_t pth_tcb_guard( void *tp )
{
	uint32_t	lo, hi;
	uint64_t	z;

	__asm__ volatile ( "rdtsc" : "=a"(lo), "=d"(hi) );
	z = ( ( (uint64_t)hi << 32 ) | lo ) ^ (uint64_t)(uintptr_t)tp;
	z += 0x9e3779b97f4a7c15ULL;
	z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
	z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uintptr_t)( z & ~(uint64_t)0xff );
}

/* A moment's pause in a loop that waits for another processor */
static inline void pth_relax( void )
{
	__asm__ volatile ( "pause" );
}

static inline size_t pth_tls_up( size_t v, size_t a )
{
	return ( v + a - 1 ) & ~( a - 1 );
}

/*
 * A thread's block of TLS and control block, made at 'at' (aligned to
 * align, the TLS image's alignment and at least 16): where the thread
 * pointer goes, where the copy of the image goes, both from 'at', and
 * the bytes of the whole for an image of memsz bytes
 */
static inline size_t pth_tls_tp_off( size_t align, size_t memsz )
{
	return pth_tls_up(memsz, align);
}

static inline size_t pth_tls_data_off( size_t align, size_t memsz )
{
	(void)align;
	(void)memsz;
	return 0;
}

static inline size_t pth_tls_bytes( size_t align, size_t memsz )
{
	return pth_tls_tp_off(align, memsz) + PTH_TCB_SIZE;
}

#endif /* __PTH_CPU_X86_64_H__ */
