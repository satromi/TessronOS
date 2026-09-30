/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	frame.h
 *	Assembler macros for the exception entry frame (see cpu_task.h).
 *	Frame layout (offsets from SP after SAVE_EXC_FRAME):
 *	  0..151 x0-x18, 152 x29, 160 x30, 168 ELR_EL1, 176 SPSR_EL1,
 *	  184 SP_EL0, 192 TPIDR_EL0, 200 pad
 */

#ifndef _SYSDEPEND_CPU_CORE_FRAME_
#define _SYSDEPEND_CPU_CORE_FRAME_

#define SSF_EXC_SIZE	208
#define SSF_CALLEE_SIZE	80

#define SSF_OFF_X29	152
#define SSF_OFF_ELR	168
#define SSF_OFF_SPSR	176
#define SSF_OFF_SP_EL0	184

	.macro	SAVE_EXC_FRAME
	sub	sp, sp, #SSF_EXC_SIZE
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	str	x18, [sp, #144]
	stp	x29, x30, [sp, #152]
	mrs	x0, elr_el1
	mrs	x1, spsr_el1
	stp	x0, x1, [sp, #168]
	mrs	x0, sp_el0
	mrs	x1, tpidr_el0
	stp	x0, x1, [sp, #184]
	.endm

	.macro	RESTORE_EXC_FRAME_ERET
	ldp	x0, x1, [sp, #184]
	msr	sp_el0, x0
	msr	tpidr_el0, x1
	ldp	x0, x1, [sp, #168]
	msr	elr_el1, x0
	msr	spsr_el1, x1
	ldp	x29, x30, [sp, #152]
	ldr	x18, [sp, #144]
	ldp	x16, x17, [sp, #128]
	ldp	x14, x15, [sp, #112]
	ldp	x12, x13, [sp, #96]
	ldp	x10, x11, [sp, #80]
	ldp	x8, x9, [sp, #64]
	ldp	x6, x7, [sp, #48]
	ldp	x4, x5, [sp, #32]
	ldp	x2, x3, [sp, #16]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #SSF_EXC_SIZE
	eret
	.endm

	.macro	SAVE_CALLEE_REGS
	sub	sp, sp, #SSF_CALLEE_SIZE
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	.endm

	.macro	RESTORE_CALLEE_REGS
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	add	sp, sp, #SSF_CALLEE_SIZE
	.endm

#endif /* _SYSDEPEND_CPU_CORE_FRAME_ */
