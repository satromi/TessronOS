/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	int_asm.S (ARMv8-A / AArch64)
 *	High level language interrupt handler entry and return.
 *
 *	knl_hll_inthdr is registered in knl_intvec_tbl[] and receives
 *	x0 = interrupt number, x1 = GICC_IAR value from knl_exc_irq
 *	(vector.S) with the exception frame already on the stack. It runs
 *	the C handler from knl_hll_inthdr_tbl[] on the interrupt stack of
 *	the processor (first level only), then returns through
 *	knl_return_inthdr which dispatches when needed.
 *
 *	TPIDR_EL1 points at the T_PCPU of the running processor.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#define	_in_asm_source_
#include <config.h>
#include <sys/knldef.h>
#include "offset.h"
#include "frame.h"

	.text
	.balign	4

	.global	knl_hll_inthdr
	.type	knl_hll_inthdr, %function
knl_hll_inthdr:
	/* Enter the task independent part */
	mrs	x9, tpidr_el1
	ldr	w2, [x9, #PCPU_taskindp]
	add	w2, w2, #1
	str	w2, [x9, #PCPU_taskindp]	/* taskindp++ */
	cmp	w2, #1
	b.ne	1f				/* nested: stay on the interrupt stack */
	mov	x3, sp				/* x3 = task SP */
	ldr	x4, [x9, #PCPU_irq_stack]
	mov	sp, x4
	stp	x3, xzr, [sp, #-16]!		/* save the task SP */
1:
	/* Call knl_hll_inthdr_tbl[intno](intno, iar) */
	adrp	x9, knl_hll_inthdr_tbl
	add	x9, x9, :lo12:knl_hll_inthdr_tbl
	ldr	x9, [x9, x0, lsl #3]
	blr	x9
	b	knl_return_inthdr
	.size	knl_hll_inthdr, . - knl_hll_inthdr

/*
 * knl_return_inthdr
 *	Leave the task independent part, dispatch if needed, return to the
 *	interrupted context.
 */
	.global	knl_return_inthdr
	.type	knl_return_inthdr, %function
knl_return_inthdr:
	msr	daifset, #2			/* IRQ disable */
	mrs	x9, tpidr_el1
	ldr	w2, [x9, #PCPU_taskindp]
	sub	w2, w2, #1
	str	w2, [x9, #PCPU_taskindp]	/* taskindp-- */
	cbnz	w2, 2f				/* nested interrupt: just return */

	ldp	x3, x4, [sp], #16
	mov	sp, x3				/* restore the task SP */

	/* Dispatch disabled? */
	ldr	w2, [x9, #PCPU_dispatch_disabled]
	cbnz	w2, 2f

	/* Dispatch necessary? */
	ldr	x10, [x9, #PCPU_ctxtsk]
	ldr	x11, [x9, #PCPU_schedtsk]
	cmp	x10, x11
	b.ne	knl_dispatch_entry
2:
	RESTORE_EXC_FRAME_ERET
	.size	knl_return_inthdr, . - knl_return_inthdr

#endif /* CPU_CORE_ARMV8A */
