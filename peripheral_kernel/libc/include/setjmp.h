/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	setjmp.h
 *	Leaving a deep call in one step, for vendored code that needs it.
 *
 *	FreeType's rasterizers report an overflow by jumping out rather
 *	than unwinding, so the font layer cannot be built without this.
 *
 *	What is saved is what the procedure call standard says a function
 *	must preserve: x19-x28, the frame pointer, the link register and
 *	the stack pointer. The floating point registers are not saved
 *	because the kernel is built without them (-mgeneral-regs-only), so
 *	no code that could reach here has any.
 */

#ifndef __TS_LIBC_SETJMP_H__
#define __TS_LIBC_SETJMP_H__

/* x19..x28, x29 (frame), x30 (link), sp */
typedef unsigned long jmp_buf[13];

int	setjmp( jmp_buf env );
void	longjmp( jmp_buf env, int val );

#endif /* __TS_LIBC_SETJMP_H__ */
