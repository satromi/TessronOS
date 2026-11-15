/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syscall.c (x86-64)
 *	The C side of SYSCALL (vector.S).
 *
 *	A call has its function code in rax and up to six arguments in rdi,
 *	rsi, rdx, r10, r8 and r9 (rcx and r11 are what SYSCALL keeps the
 *	return address and RFLAGS in). A function of the gateway's tables
 *	may take eight (svc_gate.c); the 7th and 8th are where the calling
 *	convention puts them, on the caller's stack, just above the return
 *	address of the call of the stub that made the SYSCALL (lib/libts,
 *	svc_stub.S): at rsp + 8 and rsp + 16 as SYSCALL found rsp. They are
 *	read only when the two words are on pages the task may read, and
 *	through the way to the user pages; a stack that has them not is
 *	given 0 for them, as a call that takes fewer arguments never looks.
 *	They go into the frame in place of the vector and the error code,
 *	which a call has none of (cpu_task.h, KNL_SVC_ARG).
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include "../../../sysdepend.h"
#include "cpu_task.h"

#define USER_HALF_END	U64(0x0000800000000000)

IMPORT UBINT knl_svc_gateway( T_EXCFRAME *frame );

/* Whether the task may read [va, va + len) of its space (at most two pages) */
LOCAL BOOL user_readable( UD root, UBINT va, UBINT len )
{
	UD	l0 = root & MMU_ROOT_PA;
	UBINT	p;

	if ( va < PAGE_SIZE || va + len < va || va + len > USER_HALF_END ) {
		return FALSE;
	}
	for ( p = va & ~(UBINT)( PAGE_SIZE - 1 ); p < va + len; p += PAGE_SIZE ) {
		if ( !PTE_USER_R(knl_pt_entry(l0, p)) ) {
			return FALSE;
		}
	}
	return TRUE;
}

EXPORT void knl_syscall_c( T_EXCFRAME *f )
{
	TCB	*me = knl_ctxtsk;
	UD	more[2] = { 0, 0 };
	UBINT	at = (UBINT)f->rsp + 8;

	if ( me != NULL && user_readable(me->tskctxb.ttbr0, at, sizeof(more)) ) {
		knl_ua_get(more, (CONST void *)at, sizeof(more));
	}
	f->vec = more[0];
	f->err = more[1];
	f->rax = knl_svc_gateway(f);
}

/*
 * ts_set_tls: the calling task's thread pointer, the FS base its code
 * reaches its thread local storage through (lib/libpthread). A program
 * sets it here on every processor: not every one lets user mode write
 * the base itself (WRFSBASE and CR4.FSGSBASE are not in the 2nd
 * generation Core i, mmu.c).
 *
 * The register takes only a canonical address (WRMSR faults on any
 * other), and nothing of a process is above the user half: such a value
 * is refused. The value goes into the register at once, and into the
 * task's context, which the dispatcher saves from the register and loads
 * back into it (dispatch.S); interrupts are masked between the two, so
 * that the task is not switched out halfway.
 */
EXPORT ER knl_set_tls( UBINT tp )
{
	TCB	*me;
	UINT	imask;

	if ( tp >= USER_HALF_END ) {
		return E_PAR;
	}
	DI(imask);
	me = knl_ctxtsk;
	wrmsr(MSR_FS_BASE, (UD)tp);
	if ( me != NULL ) {
		me->tskctxb.fsbase = (UD)tp;
	}
	EI(imask);
	return E_OK;
}

#endif /* CPU_CORE_X86_64 */
