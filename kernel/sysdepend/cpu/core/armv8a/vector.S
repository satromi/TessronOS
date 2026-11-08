/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vector.S (ARMv8-A / AArch64)
 *	Exception vector table and entry code for EL1.
 *
 *	The kernel runs in EL1 with SP_EL1 (EL1h). An exception pushes the
 *	entry frame (x0-x18, x29, x30, ELR, SPSR, SP_EL0, TPIDR_EL0) on the
 *	current system stack; see SStackFrame in cpu_task.h. Interrupts are
 *	masked on entry.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#define	_in_asm_source_
#include <config.h>
#include <sys/knldef.h>
#include "offset.h"
#include "frame.h"

	.text
	.balign	0x800
	.global	knl_vector_table
	.type	knl_vector_table, %function
knl_vector_table:
	/* Current EL with SP_EL0: not used (SPSel is always 1 in the kernel) */
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad

	/* Current EL with SP_ELx */
	.balign	0x80
	b	knl_exc_sync
	.balign	0x80
	b	knl_exc_irq
	.balign	0x80
	b	knl_exc_fiq
	.balign	0x80
	b	knl_exc_serror

	/* Lower EL, AArch64 (EL0 tasks) */
	.balign	0x80
	b	knl_exc_sync_el0
	.balign	0x80
	b	knl_exc_irq
	.balign	0x80
	b	knl_exc_fiq
	.balign	0x80
	b	knl_exc_serror

	/* Lower EL, AArch32: not supported */
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad
	.balign	0x80
	b	knl_exc_bad
	.size	knl_vector_table, . - knl_vector_table

/*
 * Synchronous exception from EL1: SVC (dispatcher) or a fault
 */
	.type	knl_exc_sync, %function
knl_exc_sync:
	/*
	 * A data abort on a stack with no room left for the frame would
	 * abort again at the first store, for ever. So for a data abort
	 * the room is asked of the translation first. CONTEXTIDR_EL1 holds
	 * x0 meanwhile: nothing else here reads it, and EL0 cannot.
	 */
	msr	contextidr_el1, x0
	mrs	x0, esr_el1
	lsr	x0, x0, #26
	cmp	x0, #0x25			/* data abort from EL1 */
	b.ne	3f
	mov	x0, sp
	sub	x0, x0, #SSF_EXC_SIZE
	at	s1e1w, x0
	isb
	mrs	x0, par_el1
	tbnz	x0, #0, knl_exc_stack_out	/* PAR_EL1.F: it would fault */
3:	mrs	x0, contextidr_el1
	SAVE_EXC_FRAME
	mrs	x0, esr_el1
	lsr	x1, x0, #26			/* EC */
	cmp	x1, #0x15			/* SVC from AArch64 */
	b.ne	1f
	and	x0, x0, #0xffff			/* imm16 = SVC number */
	cmp	x0, #N_SVCHDR
	b.hs	2f
	adrp	x2, knl_svcvec_tbl
	add	x2, x2, :lo12:knl_svcvec_tbl
	ldr	x2, [x2, x0, lsl #3]
	cbz	x2, 2f
	br	x2				/* x0 = SVC number */
2:	bl	SVC_default_Handler		/* x0 = SVC number, no return */
	b	.
1:	mov	x0, sp				/* frame */
	mrs	x1, esr_el1
	mrs	x2, far_el1
	bl	knl_kernel_fault		/* returns only when the fault is put right */
	RESTORE_EXC_FRAME_ERET			/* and the access is made again */
	.size	knl_exc_sync, . - knl_exc_sync

/*
 * The task has run out of its stack (only a task's stack has nothing
 * mapped below it). This processor's interrupt stack is not in use then,
 * since an interrupt handler would be on it and not on the task's: the
 * frame goes there, and the processor stops after telling. The two words
 * under the top are left alone; the lockup report reads them.
 */
	.type	knl_exc_stack_out, %function
knl_exc_stack_out:
	mrs	x0, tpidr_el1
	ldr	x0, [x0, #PCPU_irq_stack]
	sub	x0, x0, #32
	str	x1, [x0, #0]
	mov	x1, sp
	str	x1, [x0, #8]			/* the stack pointer as it was */
	ldr	x1, [x0, #0]
	mov	sp, x0
	mrs	x0, contextidr_el1
	SAVE_EXC_FRAME
	mov	x0, sp
	mrs	x1, esr_el1
	mrs	x2, far_el1
	ldr	x3, [sp, #(SSF_EXC_SIZE + 8)]
	bl	knl_stack_out			/* no return */
	b	.
	.size	knl_exc_stack_out, . - knl_exc_stack_out

/*
 * Synchronous exception from EL0: system call (svc #0, x8 = function code)
 * or a fault of the user task.
 */
	.type	knl_exc_sync_el0, %function
knl_exc_sync_el0:
	SAVE_EXC_FRAME
	mrs	x0, esr_el1
	lsr	x1, x0, #26			/* EC */
	cmp	x1, #0x15			/* SVC from AArch64 */
	b.ne	1f
	mov	x0, sp				/* frame: x8 = fncd, x0-x5 = args */
	bl	knl_svc_gateway
	str	x0, [sp, #0]			/* return value -> x0 of the task */
	RESTORE_EXC_FRAME_ERET
1:	mov	x0, sp
	mrs	x1, esr_el1
	mrs	x2, far_el1
	bl	knl_el0_fault			/* terminates the task, or returns when put right */
	RESTORE_EXC_FRAME_ERET			/* and the access is made again */
	.size	knl_exc_sync_el0, . - knl_exc_sync_el0

/*
 * IRQ: acknowledge at the GIC and jump to the registered handler.
 *	x0 = interrupt ID, x1 = GICC_IAR value (kept for EOI)
 */
	.type	knl_exc_irq, %function
knl_exc_irq:
	SAVE_EXC_FRAME
	ldr	x2, =GICC_BASE
	ldr	w1, [x2, #0x0c]			/* GICC_IAR */
	and	w0, w1, #0x3ff
	cmp	w0, #N_INTVEC
	b.hs	1f
	adrp	x2, knl_intvec_tbl
	add	x2, x2, :lo12:knl_intvec_tbl
	ldr	x2, [x2, x0, lsl #3]
	cbz	x2, 2f
	br	x2				/* registered handler */
1:	cmp	w0, #1023			/* spurious interrupt */
	b.eq	3f
2:	ldr	x2, =GICC_BASE
	str	w1, [x2, #0x10]			/* GICC_EOIR */
	bl	Default_Handler			/* x0 = interrupt ID, no return */
	b	.
3:	RESTORE_EXC_FRAME_ERET
	.size	knl_exc_irq, . - knl_exc_irq

	.type	knl_exc_fiq, %function
knl_exc_fiq:
	SAVE_EXC_FRAME
	mov	x0, sp				/* frame */
	bl	FIQ_Handler
	b	.
	.size	knl_exc_fiq, . - knl_exc_fiq

	.type	knl_exc_serror, %function
knl_exc_serror:
	SAVE_EXC_FRAME
	mov	x0, sp				/* frame */
	bl	SError_Handler
	b	.
	.size	knl_exc_serror, . - knl_exc_serror

	.type	knl_exc_bad, %function
knl_exc_bad:
	SAVE_EXC_FRAME
	mov	x0, sp
	mrs	x1, esr_el1
	mrs	x2, far_el1
	bl	knl_exception_handler
	b	.
	.size	knl_exc_bad, . - knl_exc_bad

	.ltorg

#endif /* CPU_CORE_ARMV8A */
