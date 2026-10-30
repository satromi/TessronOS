/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c (ARMv8-A / AArch64)
 *	Interrupt vector tables and GICv2 (GIC-400) initialization.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include <kernel.h>
#include "../../../sysdepend.h"
#include "cpu_msi.h"

/*
 * Interrupt vector tables (indexed by GIC interrupt ID)
 */
Noinit(EXPORT FP knl_intvec_tbl[N_INTVEC]);
Noinit(EXPORT FP knl_hll_inthdr_tbl[N_INTVEC]);

/*
 * SVC handler table (indexed by SVC number)
 */
EXPORT const FP knl_svcvec_tbl[N_SVCHDR] = {
	NULL, NULL, NULL, NULL, NULL, NULL,	/* 0 ~ 5 : reserved. */
	NULL,					/* 6 : system call gateway (later phase) */
	knl_dispatch_to_schedtsk,		/* 7 : force dispatch */
	knl_dispatch_entry,			/* 8 : task dispatcher */
	NULL,					/* 9 : debug support function */
	NULL					/* 10: Extended SVC */
};

/*
 * Register an interrupt handler
 *	TA_HLNG handlers are wrapped by knl_hll_inthdr (int_asm.S).
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	if ( (inthdr != NULL) && ((intatr & TA_HLNG) != 0) ) {
		knl_hll_inthdr_tbl[intno] = inthdr;
		inthdr = knl_hll_inthdr;
	}
	knl_intvec_tbl[intno] = inthdr;
	return E_OK;
}

/*
 * The processor a device's interrupt goes to (inttarget.c, design 4.5):
 * the distributor's target byte of the SPI, with the bit of that
 * processor's CPU interface. Each processor learns its own from the
 * first target register, which reads its own bit (knl_gic_cpuif_note).
 */
LOCAL UB	gic_cpuif[CNF_MAX_PRCID];

EXPORT void knl_gic_cpuif_note( void )
{
	ID	prc = knl_pcpu()->prcid;

	if ( prc >= 1 && prc <= CNF_MAX_PRCID ) {
		gic_cpuif[prc - 1] = (UB)( in_w(GICD_ITARGETSR(0)) & 0xff );
	}
}

EXPORT BOOL knl_int_is_dev( UINT intno )
{
	return ( intno >= 32 && intno < N_INTVEC );
}

EXPORT BOOL knl_int_is_msi( UINT intno )
{
#if KNL_HAS_MSI
	LOCAL UINT	first = 0, num = 0;

	if ( first == 0 ) {
		knl_msi_span(&first, &num);
	}
	return ( intno >= first && intno < first + num );
#else
	(void)intno;
	return FALSE;
#endif
}

EXPORT BOOL knl_int_in_use( UINT intno )
{
	return ( intno < N_INTVEC && ( knl_intvec_tbl[intno] != NULL || knl_hll_inthdr_tbl[intno] != NULL ) );
}

EXPORT ER knl_int_route( UINT intno, INT prc )
{
	UB	bit;

	if ( !knl_int_is_dev(intno) || prc < 1 || prc > knl_num_prc ) {
		return E_PAR;
	}
	bit = gic_cpuif[prc - 1];
	if ( bit == 0 ) {
		return E_NOSPT;			/* its interface not known */
	}
	*(volatile UB *)( GICD_ITARGETSR(0) + intno ) = bit;
	return E_OK;
}

/*
 * Interrupt initialization
 *	All interrupts: group 0, disabled, lowest priority, targeted to CPU0.
 *	Trigger configuration (GICD_ICFGR) keeps the reset values: PPIs are
 *	fixed by the hardware and SPIs default to level-sensitive; drivers
 *	change it with SetIntMode().
 */
EXPORT ER knl_init_interrupt( void )
{
	INT	i;
	_UW	*reg;

	for ( i = 0; i < N_INTVEC; i++ ) {
		knl_intvec_tbl[i] = (FP)NULL;
		knl_hll_inthdr_tbl[i] = (FP)NULL;
	}

	/* Register the handlers used by the kernel */
	knl_define_inthdr(INTNO_SYSTICK, TA_HLNG, (FP)knl_timer_handler);
	knl_smp_init_interrupt();

	/* Distributor */
	out_w(GICD_CTLR, 0);

	reg = (_UW*)GICD_ICENABLER(0);
	for ( i = 0; i < GICD_ICENABLER_N; i++ ) {
		reg[i] = 0xFFFFFFFFUL;			/* disable all */
	}
	reg = (_UW*)GICD_ICPENDR(0);
	for ( i = 0; i < GICD_ICENABLER_N; i++ ) {
		reg[i] = 0xFFFFFFFFUL;			/* clear pending */
	}
	reg = (_UW*)GICD_IGROUPR(0);
	for ( i = 0; i < GICD_IGROUPR_N; i++ ) {
		reg[i] = 0x00000000UL;			/* group 0 */
	}
	reg = (_UW*)GICD_IPRIORITYR(0);
	for ( i = 0; i < GICD_IPRIORITYR_N; i++ ) {
		reg[i] = 0xF0F0F0F0UL;			/* lowest priority (level 15) */
	}
	reg = (_UW*)GICD_ITARGETSR(0);
	for ( i = 8; i < GICD_ITARGETSR_N; i++ ) {	/* SPIs only (0-31 are read-only) */
		reg[i] = 0x01010101UL;			/* CPU0 */
	}

	out_w(GICD_CTLR, 0x00000001UL);			/* enable group 0 */

	/* CPU interface and banked interrupts of this processor */
	knl_gic_cpu_init();

	return E_OK;
}

#endif /* CPU_CORE_ARMV8A */
