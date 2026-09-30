/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	offset.c
 *	Generates offset.h (structure offsets used by the assembler sources).
 *	The makefile compiles this file with -S and extracts the "@@" lines.
 */

#include <sys/machine.h>
#include <tk/tkernel.h>
#include "kernel.h"
#include "cpu_task.h"

#define DEFINE(sym, val)	Asm("\n@@" #sym " %0" : : "i"((BINT)(val)))

void knl_offset_generator( void )
{
	DEFINE(TCB_tskid,	offsetof(TCB, tskid));
	DEFINE(TCB_tskatr,	offsetof(TCB, tskatr));
	DEFINE(TCB_tskctxb,	offsetof(TCB, tskctxb));
	DEFINE(TCB_state,	offsetof(TCB, state));
	DEFINE(TCB_isstack,	offsetof(TCB, isstack));
	DEFINE(TCB_runprc,	offsetof(TCB, runprc));
	DEFINE(PCPU_prcid,	offsetof(T_PCPU, prcid));
	DEFINE(PCPU_ctxtsk,	offsetof(T_PCPU, ctxtsk));
	DEFINE(PCPU_schedtsk,	offsetof(T_PCPU, schedtsk));
	DEFINE(PCPU_dispatch_disabled,	offsetof(T_PCPU, dispatch_disabled));
	DEFINE(PCPU_taskindp,	offsetof(T_PCPU, taskindp));
	DEFINE(PCPU_cur_ttbr0,	offsetof(T_PCPU, cur_ttbr0));
	DEFINE(PCPU_tmp_stack,	offsetof(T_PCPU, tmp_stack));
	DEFINE(PCPU_irq_stack,	offsetof(T_PCPU, irq_stack));
	DEFINE(PCPU_ctxsw_cnt,	offsetof(T_PCPU, ctxsw_cnt));
	DEFINE(CTXB_ssp,	offsetof(CTXB, ssp));
	DEFINE(CTXB_ttbr0,	offsetof(CTXB, ttbr0));
	DEFINE(SSF_SIZE,	sizeof(SStackFrame));
#if USE_FPU
	DEFINE(FPU_context,	sizeof(FPUContext));
#endif
}
