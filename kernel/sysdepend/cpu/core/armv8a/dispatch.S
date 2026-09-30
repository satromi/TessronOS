/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.S (ARMv8-A / AArch64)
 *	Task dispatcher, per processor.
 *
 *	TPIDR_EL1 points at the T_PCPU of the running processor. The
 *	dispatcher switches from pcpu->ctxtsk to pcpu->schedtsk; while
 *	schedtsk is NULL it idles on the processor's temporary stack with
 *	interrupts enabled around WFI.
 *
 *	TCB.runprc marks the processor that holds a task's register context.
 *	A task is switched in only after the processor that ran it last has
 *	saved its context and cleared runprc (store-release); a processor
 *	that finds runprc set keeps the interrupt window open so that a
 *	pending IPI can change its schedtsk.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#define	_in_asm_source_
#include <config.h>
#include "offset.h"
#include "frame.h"

#if USE_FPU
/*
 * FP/SIMD registers of a task created with TA_FPU (TA_COP0), kept in the
 * FPUContext at the top of its system stack: q0-q31, then FPSR and FPCR.
 * They are saved when the task is switched out and loaded when it is
 * switched in. The kernel itself never touches them (it is built with
 * -mgeneral-regs-only), so only tasks that use them need to own a copy.
 */
#define TA_FPU_BIT	0x1000

	.macro	FPU_AREA tcb
	ldr	x0, [\tcb, #TCB_isstack]
	and	x0, x0, #~0xf
	sub	x0, x0, #FPU_context
	.endm

	.macro	SAVE_FPU tcb
	ldr	w0, [\tcb, #TCB_tskatr]
	tst	w0, #TA_FPU_BIT
	b.eq	9f
	FPU_AREA \tcb
	stp	q0, q1, [x0, #0]
	stp	q2, q3, [x0, #32]
	stp	q4, q5, [x0, #64]
	stp	q6, q7, [x0, #96]
	stp	q8, q9, [x0, #128]
	stp	q10, q11, [x0, #160]
	stp	q12, q13, [x0, #192]
	stp	q14, q15, [x0, #224]
	stp	q16, q17, [x0, #256]
	stp	q18, q19, [x0, #288]
	stp	q20, q21, [x0, #320]
	stp	q22, q23, [x0, #352]
	stp	q24, q25, [x0, #384]
	stp	q26, q27, [x0, #416]
	stp	q28, q29, [x0, #448]
	stp	q30, q31, [x0, #480]
	mrs	x1, fpsr
	str	x1, [x0, #512]
	mrs	x1, fpcr
	str	x1, [x0, #520]
9:
	.endm

	.macro	LOAD_FPU tcb
	ldr	w0, [\tcb, #TCB_tskatr]
	tst	w0, #TA_FPU_BIT
	b.eq	9f
	FPU_AREA \tcb
	ldp	q0, q1, [x0, #0]
	ldp	q2, q3, [x0, #32]
	ldp	q4, q5, [x0, #64]
	ldp	q6, q7, [x0, #96]
	ldp	q8, q9, [x0, #128]
	ldp	q10, q11, [x0, #160]
	ldp	q12, q13, [x0, #192]
	ldp	q14, q15, [x0, #224]
	ldp	q16, q17, [x0, #256]
	ldp	q18, q19, [x0, #288]
	ldp	q20, q21, [x0, #320]
	ldp	q22, q23, [x0, #352]
	ldp	q24, q25, [x0, #384]
	ldp	q26, q27, [x0, #416]
	ldp	q28, q29, [x0, #448]
	ldp	q30, q31, [x0, #480]
	ldr	x1, [x0, #512]
	msr	fpsr, x1
	ldr	x1, [x0, #520]
	msr	fpcr, x1
9:
	.endm
#endif /* USE_FPU */

	.text
	.balign	4

/*
 * knl_dispatch_to_schedtsk
 *	Throw away the current context (task exit / system start) and
 *	dispatch to schedtsk. Entered through SVC 7 with interrupts masked.
 */
	.global	knl_dispatch_to_schedtsk
	.type	knl_dispatch_to_schedtsk, %function
knl_dispatch_to_schedtsk:
	mrs	x19, tpidr_el1
	mov	w0, #1
	str	w0, [x19, #PCPU_dispatch_disabled]	/* dispatch disable */
	ldr	x8, [x19, #PCPU_ctxtsk]
	cbz	x8, l_dispatch0
	add	x9, x8, #TCB_runprc
	stlrb	wzr, [x9]			/* context released */
	str	xzr, [x19, #PCPU_ctxtsk]	/* ctxtsk = NULL */
	b	l_dispatch0
	.size	knl_dispatch_to_schedtsk, . - knl_dispatch_to_schedtsk

/*
 * knl_dispatch_entry
 *	Save the current task's context and dispatch to schedtsk.
 *	Entered through SVC 8 (or from the interrupt return path) with the
 *	exception frame on the task's system stack and interrupts masked.
 */
	.global	knl_dispatch_entry
	.type	knl_dispatch_entry, %function
knl_dispatch_entry:
	mrs	x9, tpidr_el1
	mov	w0, #1
	str	w0, [x9, #PCPU_dispatch_disabled]	/* dispatch disable */
	SAVE_CALLEE_REGS			/* x19-x28 below the exception frame */
	mrs	x19, tpidr_el1
	ldr	x8, [x19, #PCPU_ctxtsk]		/* x8 = ctxtsk */
	cbz	x8, l_dispatch0
	mov	x0, sp
	str	x0, [x8, #(TCB_tskctxb + CTXB_ssp)]	/* save SP to TCB */
#if USE_FPU
	SAVE_FPU x8				/* before another processor may take it */
#endif
	add	x9, x8, #TCB_runprc
	stlrb	wzr, [x9]			/* context saved: release the task */
	str	xzr, [x19, #PCPU_ctxtsk]	/* ctxtsk = NULL */

l_dispatch0:
	ldr	x2, [x19, #PCPU_tmp_stack]
	mov	sp, x2				/* temporary stack of this processor */
	adrp	x21, knl_lowpow_discnt
	add	x21, x21, :lo12:knl_lowpow_discnt

l_dispatch1:
	msr	daifset, #2			/* IRQ disable */
	ldr	x8, [x19, #PCPU_schedtsk]	/* x8 = schedtsk */
	cbnz	x8, l_dispatch2
	ldr	w0, [x21]
	cbnz	w0, 1f
	bl	low_pow				/* WFI: wakes on any pending interrupt */
1:	msr	daifclr, #2			/* IRQ enable: pending interrupts are taken here */
	nop
	nop
	b	l_dispatch1

l_dispatch2:
	/*
	 * Take the task under the kernel lock (smp.c): schedtsk is then
	 * consistent with the ready queue and every value the task will read
	 * on return has been written. NULL when the processor that ran the
	 * task last has not saved its context yet: open the interrupt window
	 * so that an IPI can change schedtsk, then retry.
	 */
	bl	knl_dispatch_pick
	cbnz	x0, 3f
	msr	daifclr, #2
	nop
	nop
	b	l_dispatch1
3:
	mov	x8, x0				/* x8 = task to run (ctxtsk, runprc set) */
#if USE_FPU
	LOAD_FPU x8
#endif

	/* Process space (TTBR0 with ASID): switch only when it changes */
	ldr	x0, [x8, #(TCB_tskctxb + CTXB_ttbr0)]
	ldr	x1, [x19, #PCPU_cur_ttbr0]
	cmp	x0, x1
	b.eq	2f
	msr	ttbr0_el1, x0
	isb
	str	x0, [x19, #PCPU_cur_ttbr0]
2:
	ldr	w0, [x8, #TCB_tskid]
	msr	tpidrro_el0, x0			/* task ID readable from EL0 */

	ldr	x0, [x8, #(TCB_tskctxb + CTXB_ssp)]
	mov	sp, x0				/* restore SP from TCB */
	RESTORE_CALLEE_REGS
	mrs	x9, tpidr_el1
	str	wzr, [x9, #PCPU_dispatch_disabled]	/* dispatch enable */
	RESTORE_EXC_FRAME_ERET			/* return to the task */
	.size	knl_dispatch_entry, . - knl_dispatch_entry

#endif /* CPU_CORE_ARMV8A */
