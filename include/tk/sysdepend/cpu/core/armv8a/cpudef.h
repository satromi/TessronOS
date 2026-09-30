/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpudef.h
 *	CPU-dependent definition (ARMv8-A / AArch64)
 */

#ifndef __TK_CPUDEF_CORE_H__
#define __TK_CPUDEF_CORE_H__

/*
 * Coprocessor attribute: the FP/SIMD unit is treated as coprocessor 0.
 */
#if USE_FPU
#define	TA_COPS		TA_COP0
#else
#define	TA_COPS		0
#endif
#define TA_FPU		TA_COP0

/*
 * General purpose register		tk_get_reg tk_set_reg
 */
typedef struct t_regs {
	VD	x[29];		/* x0-x28 */
	void	*fp;		/* x29 */
	void	*lr;		/* x30 */
} T_REGS;

/*
 * Exception-related register		tk_get_reg tk_set_reg
 */
typedef struct t_eit {
	void	*pc;		/* return address (ELR_EL1) */
	UD	spsr;		/* SPSR_EL1 */
	UW	taskmode;	/* Task mode flag */
} T_EIT;

/*
 * Control register			tk_get_reg tk_set_reg
 */
typedef struct t_cregs {
	void	*ssp;		/* System stack pointer (SP_EL1) */
} T_CREGS;

/*
 * Coprocessor register			tk_get_cpr tk_set_cpr
 */
#if NUM_COPROCESSOR > 0
typedef struct t_copregs {
	UD	q[32][2];	/* v0-v31 (128 bit each) */
	UW	fpsr;
	UW	fpcr;
} T_COPREGS;
#endif  /* NUM_COPROCESSOR  */

#endif /* __TK_CPUDEF_CORE_H__ */
