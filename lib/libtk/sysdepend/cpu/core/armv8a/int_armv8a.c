/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	int_armv8a.c
 *	Interrupt control library (ARMv8-A / AArch64, GICv2)
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include <tk/tkernel.h>

/*
 * Disable interrupts, return the previous PSTATE.I
 */
EXPORT UW disint( void )
{
	UD	daif;

	Asm("mrs %0, daif" : "=r"(daif));
	Asm("msr daifset, #2" ::: "memory");
	return (UW)daif & PSR_I;
}

/*
 * Restore the PSTATE.I saved by disint()
 */
EXPORT void enaint( UW intsts )
{
	if ( intsts & PSR_I ) {
		Asm("msr daifset, #2" ::: "memory");
	} else {
		Asm("msr daifclr, #2" ::: "memory");
	}
}

/*
 * Interrupt controller mask level (GICC_PMR)
 *	Interrupts with priority level >= 'level' are masked.
 */
EXPORT void SetCtrlIntLevel( INT level )
{
	if ( level < INTPRI_HIGHEST || level > INTPRI_LOWEST + 1 ) return;
	out_w(GICC_PMR, ((UW)level << INTPRI_SHIFT) & 0xFF);
	(void)in_w(GICC_PMR);
}

EXPORT INT GetCtrlIntLevel( void )
{
	return (INT)((in_w(GICC_PMR) & 0xFF) >> INTPRI_SHIFT);
}

/*
 * Enable interrupt with priority level
 */
EXPORT void EnableInt( UINT intno, INT level )
{
	if ( intno >= N_INTVEC || level < INTPRI_HIGHEST || level > INTPRI_LOWEST ) return;

	/* Priority: one byte per interrupt */
	out_b(GICD_IPRIORITYR(0) + intno, (UB)((UW)level << INTPRI_SHIFT));

	out_w(GICD_ISENABLER(intno >> 5), 1UL << (intno & 0x1F));
}

EXPORT void DisableInt( UINT intno )
{
	if ( intno >= N_INTVEC ) return;
	out_w(GICD_ICENABLER(intno >> 5), 1UL << (intno & 0x1F));
}

/*
 * Trigger mode: GICD_ICFGR has 2 bits per interrupt, bit 1 of the pair is
 * 1 for edge and 0 for level.
 */
EXPORT void SetIntMode( UINT intno, UINT mode )
{
	UBINT	addr;
	UW	bit, val;

	if ( intno >= N_INTVEC || mode > IM_EDGE ) return;

	addr = GICD_ICFGR(intno >> 4);
	bit = 1UL << (((intno & 0x0F) << 1) + 1);
	val = in_w(addr);
	if ( mode == IM_EDGE ) {
		val |= bit;
	} else {
		val &= ~bit;
	}
	out_w(addr, val);
}

EXPORT void ClearInt( UINT intno )
{
	if ( intno >= N_INTVEC ) return;
	out_w(GICD_ICPENDR(intno >> 5), 1UL << (intno & 0x1F));
}

EXPORT BOOL CheckInt( UINT intno )
{
	if ( intno >= N_INTVEC ) return FALSE;
	return (in_w(GICD_ISPENDR(intno >> 5)) & (1UL << (intno & 0x1F))) ? TRUE : FALSE;
}

EXPORT void EndOfInt( UINT intno )
{
	if ( intno >= N_INTVEC ) return;
	out_w(GICC_EOIR, intno);
}

#endif /* CPU_CORE_ARMV8A */
