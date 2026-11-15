/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_ufault.h
 *	A task of a process at EL0 and its exception frame (ARMv8-A /
 *	AArch64 core): what a fault of it was, and the picture of where it
 *	was that a fault or a signal hands to the program (T_TSFLT,
 *	include/ts/proc.h). peripheral_kernel/sysman/proc.c takes it in
 *	after <ts/proc.h>; nothing here knows of processes.
 */

#ifndef _SYSDEPEND_CPU_CORE_UFAULT_
#define _SYSDEPEND_CPU_CORE_UFAULT_

#include <sys/machine.h>
#include <tk/typedef.h>
#include <sys/sysdef.h>

/* ---------------------------------------------------------------- what the fault was */

/* The syndrome (ESR_EL1) of an abort taken from EL0 */
#define ESR_EC(e)	( ( (e) >> 26 ) & 0x3f )
#define EC_IABT_EL0	0x20
#define EC_DABT_EL0	0x24
#define ESR_FNV		( 1UL << 10 )
#define ESR_CM		( 1UL << 8 )		/* by cache maintenance: a read */
#define ESR_WNR		( 1UL << 6 )
#define FSC_TRANS(f)	( (f) >= 0x04 && (f) <= 0x07 )
#define FSC_PERM(f)	( (f) >= 0x0c && (f) <= 0x0f )

/*
 * Not a translation or permission fault with the address it met valid:
 * nothing the process's memory may put right
 */
Inline BOOL knl_ufault_unfixable( UD esr )
{
	UD	fsc = esr & 0x3f;

	return !( FSC_TRANS(fsc) || FSC_PERM(fsc) ) || ( esr & ESR_FNV ) != 0;
}

/* An instruction fetch */
Inline BOOL knl_ufault_is_exec( UD esr )
{
	return ESR_EC(esr) == EC_IABT_EL0;
}

/* Of a load or store of data, a store (cache maintenance counts as a read) */
Inline BOOL knl_ufault_is_write( UD esr )
{
	return ( esr & ESR_WNR ) != 0 && ( esr & ESR_CM ) == 0;
}

/* A load or store of data (the faults a program may name an entry for) */
Inline BOOL knl_ufault_is_data( UD esr )
{
	return ESR_EC(esr) == EC_DABT_EL0;
}

/* ---------------------------------------------------------------- the frame and the picture */

/* The exception entry frame (T_EXCFRAME): x0-x18, then these */
#define FR_FP		19
#define FR_LR		20
#define FR_ELR		21
#define FR_SPSR		22
#define FR_SP		23

/* What a program's code address and stack pointer must be aligned to (the low bits clear) */
#define UFLT_PC_MASK	3
#define UFLT_SP_MASK	15

/* The bytes below the stack pointer a function may use without moving it: none (AAPCS64) */
#define UFLT_RED_ZONE	0

/* The task's stack pointer in the frame */
#define knl_uframe_sp(f)	( (f)[FR_SP] )

/*
 * The general registers of the frame into the picture (x19-x28 are left
 * as they are): of a fault, and of a call of the kernel's that returns
 * ret
 */
Inline void knl_uflt_from_frame( T_TSFLT *c, CONST UD *f )
{
	INT	i;

	for ( i = 0; i < 19; i++ ) {
		c->regs[i] = f[i];
	}
	c->regs[29] = f[FR_FP];
	c->regs[30] = f[FR_LR];
	c->sp = f[FR_SP];
	c->pc = f[FR_ELR];
	c->pstate = f[FR_SPSR] & PSR_NZCV;
}

Inline void knl_uflt_from_call( T_TSFLT *c, CONST UD *f, UD ret )
{
	INT	i;

	for ( i = 0; i < 19; i++ ) {
		c->regs[i] = f[i];
	}
	c->regs[0] = ret;
	c->regs[29] = f[FR_FP];
	c->regs[30] = f[FR_LR];
	c->sp = f[FR_SP];
	c->pc = f[FR_ELR];
	c->pstate = f[FR_SPSR] & PSR_NZCV;
}

/* The fault's address and syndrome, in the picture */
Inline void knl_uflt_set_fault( T_TSFLT *c, UD far, UD esr )
{
	c->far = far;
	c->esr = esr;
}

/*
 * The task's FP/SIMD registers into the picture. They are still its own
 * here: the kernel's code uses none.
 */
Inline void knl_uflt_save_fp( T_TSFLT *c )
{
	Asm("stp q0, q1, [%0, #0]\n\tstp q2, q3, [%0, #32]\n\tstp q4, q5, [%0, #64]\n\t"
	    "stp q6, q7, [%0, #96]\n\tstp q8, q9, [%0, #128]\n\tstp q10, q11, [%0, #160]\n\t"
	    "stp q12, q13, [%0, #192]\n\tstp q14, q15, [%0, #224]\n\tstp q16, q17, [%0, #256]\n\t"
	    "stp q18, q19, [%0, #288]\n\tstp q20, q21, [%0, #320]\n\tstp q22, q23, [%0, #352]\n\t"
	    "stp q24, q25, [%0, #384]\n\tstp q26, q27, [%0, #416]\n\tstp q28, q29, [%0, #448]\n\t"
	    "stp q30, q31, [%0, #480]" :: "r"(c->vregs) : "memory");
	Asm("mrs %0, fpsr" : "=r"(c->fpsr));
	Asm("mrs %0, fpcr" : "=r"(c->fpcr));
}

/* The picture's general registers back into the frame, and its flags */
Inline void knl_uflt_to_frame( UD *f, CONST T_TSFLT *c )
{
	INT	i;

	for ( i = 0; i < 19; i++ ) {
		f[i] = c->regs[i];
	}
	f[FR_FP] = c->regs[29];
	f[FR_LR] = c->regs[30];
	f[FR_ELR] = c->pc;
	f[FR_SP] = c->sp;
	f[FR_SPSR] = ( f[FR_SPSR] & ~PSR_NZCV ) | ( c->pstate & PSR_NZCV );
}

/* The picture's FP/SIMD registers back into the processor's, for the task to go on with */
Inline void knl_uflt_load_fp( CONST T_TSFLT *c )
{
	Asm("ldp q0, q1, [%0, #0]\n\tldp q2, q3, [%0, #32]\n\tldp q4, q5, [%0, #64]\n\t"
	    "ldp q6, q7, [%0, #96]\n\tldp q8, q9, [%0, #128]\n\tldp q10, q11, [%0, #160]\n\t"
	    "ldp q12, q13, [%0, #192]\n\tldp q14, q15, [%0, #224]\n\tldp q16, q17, [%0, #256]\n\t"
	    "ldp q18, q19, [%0, #288]\n\tldp q20, q21, [%0, #320]\n\tldp q22, q23, [%0, #352]\n\t"
	    "ldp q24, q25, [%0, #384]\n\tldp q26, q27, [%0, #416]\n\tldp q28, q29, [%0, #448]\n\t"
	    "ldp q30, q31, [%0, #480]" :: "r"(c->vregs) : "memory");
	Asm("msr fpsr, %0" :: "r"(c->fpsr));
	Asm("msr fpcr, %0" :: "r"(c->fpcr));
}

/* Argument n (0 the first) of the entry the task goes on at */
Inline void knl_uframe_arg( UD *f, INT n, UD v )
{
	f[n] = v;
}

/*
 * The task goes on at entry with its stack at sp. The entry does not
 * return (it ends with a call that puts a picture back), so the link
 * register is left 0.
 */
Inline void knl_uframe_call( UD *f, UD entry, UD sp )
{
	f[FR_SP] = sp;
	f[FR_ELR] = entry;
	f[FR_LR] = 0;
}

#endif /* _SYSDEPEND_CPU_CORE_UFAULT_ */
