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
 *	A task of a process in user mode and its interrupt frame (x86-64
 *	core): what a fault of it was, and the picture of where it was that
 *	a fault or a signal hands to the program (T_TSFLT, include/ts/
 *	proc.h). peripheral_kernel/sysman/proc.c takes it in after
 *	<ts/proc.h>; nothing here knows of processes.
 */

#ifndef _SYSDEPEND_CPU_CORE_UFAULT_
#define _SYSDEPEND_CPU_CORE_UFAULT_

#include <sys/machine.h>
#include <tk/typedef.h>
#include <sys/sysdef.h>

/* ---------------------------------------------------------------- what the fault was */

/*
 * The "syndrome" of a fault on this architecture (exc_hdl.c): the
 * vector in bits 39:32 and the error code below. A page fault's error
 * code says what the access was.
 */
#define UFLT_VEC(e)	( (UW)( (e) >> 32 ) & 0xff )
#define VEC_PF		14
#define PFERR_P		0x01		/* the page was there: its rights refused it */
#define PFERR_W		0x02		/* a store */
#define PFERR_U		0x04		/* in user mode */
#define PFERR_RSVD	0x08		/* a reserved bit of an entry set */
#define PFERR_I		0x10		/* an instruction fetch */

/* Not a page fault an entry of the space can put right */
Inline BOOL knl_ufault_unfixable( UD esr )
{
	return UFLT_VEC(esr) != VEC_PF || ( esr & PFERR_RSVD ) != 0;
}

/* An instruction fetch */
Inline BOOL knl_ufault_is_exec( UD esr )
{
	return UFLT_VEC(esr) == VEC_PF && ( esr & PFERR_I ) != 0;
}

/* Of a load or store of data, a store */
Inline BOOL knl_ufault_is_write( UD esr )
{
	return ( esr & PFERR_W ) != 0;
}

/* A load or store of data (the faults a program may name an entry for) */
Inline BOOL knl_ufault_is_data( UD esr )
{
	return UFLT_VEC(esr) == VEC_PF && ( esr & PFERR_I ) == 0;
}

/* ---------------------------------------------------------------- the frame and the picture */

/* The interrupt frame (T_EXCFRAME) as an array: rax .. r15, then these */
#define FR_RAX		0
#define FR_RBX		1
#define FR_RCX		2
#define FR_RDX		3
#define FR_RSI		4
#define FR_RDI		5
#define FR_RBP		6
#define FR_R8		7
#define FR_R11		10
#define FR_R12		11
#define FR_R15		14
#define FR_VEC		15
#define FR_ERR		16
#define FR_RIP		17
#define FR_CS		18
#define FR_RFLAGS	19
#define FR_SP		20
#define FR_SS		21

/* What a program's code address and stack pointer must be aligned to (the low bits clear) */
#define UFLT_PC_MASK	0
#define UFLT_SP_MASK	7

/*
 * The bytes below the stack pointer a function may use without moving
 * it (the System V ABI's red zone): the picture goes below them, as a
 * signal frame does
 */
#define UFLT_RED_ZONE	128

/* The task's stack pointer in the frame */
#define knl_uframe_sp(f)	( (f)[FR_SP] )

/* The registers a function may change, and rbp: what goes into the picture and back */
Inline BOOL knl_uflt_reg_kept( INT i )
{
	return i == FR_RBX || i >= FR_R12;
}

/*
 * The general registers of the frame into the picture (rbx and r12-r15
 * are left as they are): of a fault, and of a call of the kernel's that
 * returns ret
 */
Inline void knl_uflt_from_frame( T_TSFLT *c, CONST UD *f )
{
	INT	i;

	for ( i = FR_RAX; i <= FR_R15; i++ ) {
		if ( !knl_uflt_reg_kept(i) ) {
			c->regs[i] = f[i];
		}
	}
	c->sp = f[FR_SP];
	c->pc = f[FR_RIP];
	c->pstate = f[FR_RFLAGS] & RFL_ARITH;
}

Inline void knl_uflt_from_call( T_TSFLT *c, CONST UD *f, UD ret )
{
	knl_uflt_from_frame(c, f);
	c->regs[FR_RAX] = ret;
}

/* The fault's address and syndrome, in the picture */
Inline void knl_uflt_set_fault( T_TSFLT *c, UD far, UD esr )
{
	c->far = far;
	c->esr = esr;
}

/*
 * The task's x87 and SSE registers into the picture. They are still its
 * own here: the kernel's code uses none.
 */
Inline void knl_uflt_save_fp( T_TSFLT *c )
{
	CONST UB	*img = (CONST UB *)c->vregs;

	Asm("fxsave64 %0" : "=m"(*(UB (*)[512])c->vregs));
	c->fpsr = *(CONST UW *)( img + 24 );
	c->fpcr = (UD)*(CONST UH *)( img + 0 ) | ( (UD)*(CONST UH *)( img + 2 ) << 16 );
}

/* The picture's general registers back into the frame, and its flags */
Inline void knl_uflt_to_frame( UD *f, CONST T_TSFLT *c )
{
	INT	i;

	for ( i = FR_RAX; i <= FR_R15; i++ ) {
		if ( !knl_uflt_reg_kept(i) ) {
			f[i] = c->regs[i];
		}
	}
	f[FR_RIP] = c->pc;
	f[FR_SP] = c->sp;
	f[FR_RFLAGS] = ( f[FR_RFLAGS] & ~(UD)RFL_ARITH ) | ( c->pstate & RFL_ARITH );
}

/*
 * The picture's x87 and SSE registers back into the processor's, for the
 * task to go on with. Bits of MXCSR the processor does not have would
 * fault the load, and are let go.
 */
Inline void knl_uflt_load_fp( CONST T_TSFLT *c )
{
	UD	img[64] __attribute__((aligned(16)));
	INT	i;

	for ( i = 0; i < 64; i++ ) {
		img[i] = c->vregs[i];
	}
	*(UW *)( (UB *)img + 24 ) = (UW)c->fpsr & 0xffff;
	*(UH *)( (UB *)img + 0 ) = (UH)c->fpcr;
	Asm("fxrstor64 %0" :: "m"(*(UB (*)[512])img));
}

/* Argument n (0 the first) of the entry the task goes on at */
Inline void knl_uframe_arg( UD *f, INT n, UD v )
{
	static const UB	reg[6] = { FR_RDI, FR_RSI, FR_RDX, FR_RCX, FR_R8, FR_R8 + 1 };

	if ( n >= 0 && n < 6 ) {
		f[reg[n]] = v;
	}
}

/*
 * The task goes on at entry with its stack below sp, as a function just
 * called: 8 below a multiple of 16. The entry does not return (it ends
 * with a call that puts a picture back).
 */
Inline void knl_uframe_call( UD *f, UD entry, UD sp )
{
	f[FR_SP] = ( sp & ~(UD)15 ) - 8;
	f[FR_RIP] = entry;
}

#endif /* _SYSDEPEND_CPU_CORE_UFAULT_ */
