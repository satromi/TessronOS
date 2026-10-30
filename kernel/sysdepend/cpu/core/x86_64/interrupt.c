/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c (x86-64)
 *	The IDT, the interrupt vector table and the interrupt controllers'
 *	initialization.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include <kernel.h>
#include "../../../sysdepend.h"

/*
 * Interrupt vector table (indexed by vector): the C handlers, called as
 * handler(intno, 0) on the processor's interrupt stack (vector.S)
 */
Noinit(EXPORT FP knl_intvec_tbl[N_INTVEC]);

/*
 * The IDT, shared by all processors. Every gate is an interrupt gate of
 * the kernel's privilege, so a user program raises none of them with
 * INT; NMI, the double fault and the machine check have IST stacks.
 */
typedef struct {
	UH	off_lo;
	UH	sel;
	UB	ist;
	UB	type;
	UH	off_mid;
	UW	off_hi;
	UW	reserved;
} T_IDTENT;

LOCAL T_IDTENT	knl_idt[256] __attribute__((aligned(16)));

IMPORT const UD	knl_int_stubs[256];	/* vector.S */

#define GATE_INT	0x8e		/* present, DPL 0, 64 bit interrupt gate */

EXPORT void knl_idt_load( void )
{
	struct {
		UH	limit;
		UD	base;
	} __attribute__((packed)) d = { sizeof(knl_idt) - 1, (UD)(UBINT)knl_idt };

	Asm("lidt %0" :: "m"(d));
}

LOCAL void idt_init( void )
{
	INT	v;

	for ( v = 0; v < 256; v++ ) {
		UD	a = knl_int_stubs[v];
		T_IDTENT *e = &knl_idt[v];

		e->off_lo = (UH)a;
		e->sel = SEL_KCODE;
		e->ist = ( v == 2 ) ? IST_NMI : ( v == 8 ) ? IST_DF : ( v == 18 ) ? IST_MC : 0;
		e->type = GATE_INT;
		e->off_mid = (UH)( a >> 16 );
		e->off_hi = (UW)( a >> 32 );
		e->reserved = 0;
	}
}

/*
 * The exceptions are taken from the first instruction of the kernel's
 * own (reset_main): the IDT is made and loaded before anything else
 * can fault
 */
EXPORT void knl_idt_init( void )
{
	idt_init();
	knl_idt_load();
}

/*
 * Register an interrupt handler
 *	Only TA_HLNG handlers: the entry is the C function itself.
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	if ( intno < 0 || intno >= N_INTVEC ) {
		return E_PAR;
	}
	if ( inthdr != NULL && (intatr & TA_HLNG) == 0 ) {
		return E_NOSPT;
	}
	knl_intvec_tbl[intno] = inthdr;
	return E_OK;
}

/*
 * Interrupt initialization
 *	The vector table emptied, the kernel's own handlers registered, the
 *	8259 PICs masked for good, the Local APIC of this processor and the
 *	IOAPICs (all their inputs masked) set up.
 */
EXPORT ER knl_init_interrupt( void )
{
	INT	i;

	for ( i = 0; i < N_INTVEC; i++ ) {
		knl_intvec_tbl[i] = (FP)NULL;
	}

	/* Register the handlers used by the kernel */
	knl_define_inthdr(INTNO_SYSTICK, TA_HLNG, (FP)knl_apic_timer_isr);	/* and the lockup checks' period */
	knl_smp_init_interrupt();

	knl_apic_init();
	knl_lockup_cpu_init();		/* this processor's period of the lockup checks (lockup.c) */

	return E_OK;
}

#endif /* CPU_CORE_X86_64 */
