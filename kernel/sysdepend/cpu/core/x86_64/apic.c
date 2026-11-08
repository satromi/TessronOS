/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	apic.c (x86-64)
 *	The Local APIC of each processor and the IOAPICs.
 *
 *	The Local APIC is used in x2APIC mode where the processor has it
 *	(its registers are MSRs, and an inter-processor interrupt is one
 *	write), else in xAPIC mode through its registers in the device
 *	window. Its timer is the system timer (sys_timer.h): in TSC deadline
 *	mode where there is one, else one shot, counted at the rate measured
 *	against the time stamp counter.
 *
 *	The IOAPICs take what is wired: the ISA interrupts and the PCI
 *	interrupt lines of a device without MSI. An input g goes to vector
 *	INTVEC_GSI(g) on the boot processor, masked until EnableInt; the
 *	trigger and polarity are what the bus says (ISA edge and high,
 *	PCI level and low) unless the MADT overrides them.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include <kernel.h>
#include <tm/tmonitor.h>
#include "../../../sysdepend.h"
#include "cpu_msi.h"

EXPORT BOOL	knl_x2apic_on = FALSE;
EXPORT UW	knl_msi_dest = 0;
LOCAL UBINT	xapic_base = 0;			/* xAPIC: the registers in the device window */
LOCAL BOOL	tsc_deadline = FALSE;
LOCAL UD	apic_timer_hz = 0;		/* one shot mode: the timer's count rate */

/* Local APIC registers (xAPIC offsets; x2APIC MSR = 0x800 + offset / 16) */
#define LAPIC_ID	0x020
#define LAPIC_VER	0x030
#define LAPIC_TPR	0x080
#define LAPIC_EOI	0x0b0
#define LAPIC_LDR	0x0d0
#define LAPIC_SVR	0x0f0
#define LAPIC_ISR	0x100
#define LAPIC_IRR	0x200
#define LAPIC_ESR	0x280
#define LAPIC_ICR_LO	0x300
#define LAPIC_ICR_HI	0x310
#define LAPIC_LVT_TMR	0x320
#define LAPIC_LVT_PERF	0x340
#define LAPIC_LVT_LINT0	0x350
#define LAPIC_LVT_LINT1	0x360
#define LAPIC_LVT_ERR	0x370
#define LAPIC_TMR_INIT	0x380
#define LAPIC_TMR_CUR	0x390
#define LAPIC_TMR_DIV	0x3e0

#define LVT_MASKED	( 1U << 16 )
#define LVT_TSC_DL	( 2U << 17 )
#define LVT_DM_NMI	( 4U << 8 )
#define SVR_ENABLE	( 1U << 8 )
#define ICR_PENDING	( 1U << 12 )
#define ICR_ASSERT	( 1U << 14 )
#define ICR_LEVEL	( 1U << 15 )

Inline UW lapic_rd( UW reg )
{
	if ( knl_x2apic_on ) {
		return (UW)rdmsr(MSR_X2APIC_BASE + ( reg >> 4 ));
	}
	return in_w(xapic_base + reg);
}

Inline void lapic_wr( UW reg, UW v )
{
	if ( knl_x2apic_on ) {
		wrmsr(MSR_X2APIC_BASE + ( reg >> 4 ), v);
	} else {
		out_w(xapic_base + reg, v);
	}
}

EXPORT void knl_apic_eoi( void )
{
	lapic_wr(LAPIC_EOI, 0);
}

EXPORT void knl_apic_set_tpr( UW tpr )
{
	lapic_wr(LAPIC_TPR, tpr & 0xff);
}

EXPORT UW knl_apic_get_tpr( void )
{
	return lapic_rd(LAPIC_TPR) & 0xff;
}

EXPORT BOOL knl_apic_pending( UINT vec )
{
	return ( lapic_rd(LAPIC_IRR + ( vec >> 5 ) * 0x10) & ( 1U << ( vec & 31 ) ) ) != 0;
}

EXPORT UW knl_apic_id( void )
{
	if ( knl_x2apic_on ) {
		return (UW)rdmsr(MSR_X2APIC_BASE + ( LAPIC_ID >> 4 ));
	}
	return lapic_rd(LAPIC_ID) >> 24;
}

/*
 * An interrupt to another processor: a vector, NMI, INIT or start-up
 * (mode APIC_DM_*). An INIT is asserted; the others are edges.
 */
EXPORT void knl_apic_send( UW apic_id, UW vec, UW mode )
{
	UW	lo = ( vec & 0xff ) | mode | ICR_ASSERT;

	if ( knl_x2apic_on ) {
		Asm("mfence" ::: "memory");	/* the write to the MSR is not ordered after the stores before it */
		wrmsr(MSR_X2APIC_BASE + ( LAPIC_ICR_LO >> 4 ), ( (UD)apic_id << 32 ) | lo);
		return;
	}
	lapic_wr(LAPIC_ICR_HI, apic_id << 24);
	lapic_wr(LAPIC_ICR_LO, lo);
	while ( ( lapic_rd(LAPIC_ICR_LO) & ICR_PENDING ) != 0 ) {
		CpuRelax();
	}
}

/*
 * The timer: an interrupt at the time stamp counter's value tsc, or none
 * for 0
 */
LOCAL void timer_hw( UD tsc )
{
	UD	now, n;

	if ( tsc_deadline ) {
		wrmsr(MSR_TSC_DEADLINE, tsc);
		return;
	}
	if ( tsc == 0 ) {
		lapic_wr(LAPIC_TMR_INIT, 0);
		return;
	}
	now = ReadSysCounterSync();
	n = ( tsc > now ) ? tsc - now : 1;
	if ( knl_cntfrq != 0 && apic_timer_hz != 0 ) {
		n = (UD)( (unsigned __int128)n * apic_timer_hz / knl_cntfrq );
	}
	if ( n == 0 ) n = 1;
	if ( n > 0xffffffffULL ) n = 0xffffffffULL;
	lapic_wr(LAPIC_TMR_INIT, (UW)n);
}

/*
 * The one timer of a processor serves two: the system timer's expiry,
 * which only PRC 1 has (sys_timer.h), and the period of the lockup
 * checks, which every processor has (lockup.c). It is set to the nearer;
 * its interrupt runs whichever is due and sets it again. Changed with
 * interrupts disabled only, on the processor itself.
 */
EXPORT void	(*knl_apic_lk_hook)( void ) = NULL;
IMPORT void	knl_cpu_freq_tick( ID prcid );
LOCAL void timer_program( T_PCPU *p )
{
	UD	a = p->apic_sys_dl, b = p->apic_lk_dl;

	timer_hw( ( a == 0 ) ? b : ( b == 0 || a < b ) ? a : b );
}

EXPORT void knl_apic_timer_deadline( UD tsc )
{
	T_PCPU	*p = knl_pcpu();

	p->apic_sys_dl = tsc;
	timer_program(p);
}

EXPORT void knl_apic_lk_arm( UD period )
{
	T_PCPU	*p = knl_pcpu();

	p->apic_lk_dl = ReadSysCounterSync() + period;
	timer_program(p);
}

/* The timer's interrupt (INTNO_SYSTICK, interrupt.c) */
EXPORT void knl_apic_timer_isr( UINT intno, UW iar )
{
	T_PCPU	*p = knl_pcpu();
	UD	now = ReadSysCounterSync();

	if ( p->apic_lk_dl != 0 && now >= p->apic_lk_dl ) {
		p->apic_lk_dl = 0;
		knl_cpu_freq_tick(p->prcid);		/* this processor's clock over the period (cpuclk.c) */
		if ( knl_apic_lk_hook != NULL ) {
			(*knl_apic_lk_hook)();		/* sets its next period */
		}
	}
	if ( p->apic_sys_dl != 0 && now >= p->apic_sys_dl ) {
		p->apic_sys_dl = 0;
		knl_timer_handler();			/* sets the next expiry, and ends the interrupt */
		return;
	}
	timer_program(p);
	knl_apic_eoi();
}

/* The one shot timer's rate, against the time stamp counter over 10ms */
LOCAL void apic_timer_calibrate( void )
{
	UD	t0, t1;
	UW	c1;

	lapic_wr(LAPIC_TMR_DIV, 0x0b);			/* divide by 1 */
	lapic_wr(LAPIC_LVT_TMR, LVT_MASKED | INTVEC_TIMER);
	t0 = ReadSysCounterSync();
	lapic_wr(LAPIC_TMR_INIT, 0xffffffff);
	do {
		t1 = ReadSysCounterSync();
	} while ( t1 - t0 < knl_cntfrq / 100 );
	c1 = lapic_rd(LAPIC_TMR_CUR);
	lapic_wr(LAPIC_TMR_INIT, 0);
	apic_timer_hz = (UD)( 0xffffffffU - c1 ) * knl_cntfrq / ( t1 - t0 );
}

/*
 * This processor's Local APIC: enabled, accepting every class, the
 * timer stopped, LINT0 masked (the 8259s are not used), LINT1 the NMI
 * line where the MADT says
 */
EXPORT void knl_apic_cpu_init( void )
{
	UD	base = rdmsr(MSR_APIC_BASE);

	if ( knl_x2apic_on ) {
		if ( ( base & ( APIC_BASE_EN | APIC_BASE_EXTD ) ) != ( APIC_BASE_EN | APIC_BASE_EXTD ) ) {
			wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);
			wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
		}
	} else if ( ( base & APIC_BASE_EN ) == 0 ) {
		wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);
	}
	lapic_wr(LAPIC_TPR, 0);
	lapic_wr(LAPIC_LVT_LINT0, LVT_MASKED);
	lapic_wr(LAPIC_LVT_LINT1, ( knl_acpi.lint_nmi == 1 ) ? LVT_DM_NMI : LVT_MASKED);
	if ( knl_acpi.lint_nmi == 0 ) {
		lapic_wr(LAPIC_LVT_LINT0, LVT_DM_NMI);
	}
	lapic_wr(LAPIC_LVT_ERR, INTVEC_APIC_ERROR);
	lapic_wr(LAPIC_LVT_PERF, LVT_MASKED);
	lapic_wr(LAPIC_ESR, 0);
	lapic_wr(LAPIC_ESR, 0);
	lapic_wr(LAPIC_SVR, SVR_ENABLE | INTVEC_SPURIOUS);
	if ( tsc_deadline ) {
		lapic_wr(LAPIC_LVT_TMR, LVT_TSC_DL | INTVEC_TIMER);
		wrmsr(MSR_TSC_DEADLINE, 0);
	} else {
		lapic_wr(LAPIC_TMR_DIV, 0x0b);
		lapic_wr(LAPIC_LVT_TMR, INTVEC_TIMER);	/* one shot */
		lapic_wr(LAPIC_TMR_INIT, 0);
	}
	knl_apic_eoi();
}

/* ------------------------------------------------------------------------ */
/*
 * IOAPICs
 */
#define IOAPIC_REGSEL	0x00
#define IOAPIC_WIN	0x10
#define IOAPIC_VER	0x01
#define IOAPIC_RTE(n)	( 0x10 + 2 * (n) )

#define RTE_MASKED	( 1U << 16 )
#define RTE_LEVEL	( 1U << 15 )
#define RTE_LOW		( 1U << 13 )

LOCAL T_SPLOCK	ioapic_lock;

LOCAL UW ioapic_rd( UBINT base, UW reg )
{
	out_w(base + IOAPIC_REGSEL, reg);
	return in_w(base + IOAPIC_WIN);
}

LOCAL void ioapic_wr( UBINT base, UW reg, UW v )
{
	out_w(base + IOAPIC_REGSEL, reg);
	out_w(base + IOAPIC_WIN, v);
}

/* The IOAPIC that takes gsi, and gsi's input on it */
LOCAL UBINT ioapic_of( UINT gsi, UINT *p_pin )
{
	INT	i;

	for ( i = 0; i < knl_acpi.nioapic; i++ ) {
		T_ACPIIOAPIC	*a = &knl_acpi.ioapic[i];

		if ( gsi >= a->gsi_base && gsi < a->gsi_base + a->ninput ) {
			*p_pin = gsi - a->gsi_base;
			return DEV_BASE(a->addr);
		}
	}
	return 0;
}

EXPORT void knl_ioapic_mask( UINT gsi, BOOL mask )
{
	UINT	pin, imask;
	UBINT	base = ioapic_of(gsi, &pin);
	UW	lo;

	if ( base == 0 ) return;
	ISpinLock(&ioapic_lock, &imask);
	lo = ioapic_rd(base, IOAPIC_RTE(pin));
	lo = mask ? ( lo | RTE_MASKED ) : ( lo & ~RTE_MASKED );
	ioapic_wr(base, IOAPIC_RTE(pin), lo);
	ISpinUnlock(&ioapic_lock, &imask);
}

EXPORT void knl_ioapic_mode( UINT gsi, UINT mode )
{
	UINT	pin, imask;
	UBINT	base = ioapic_of(gsi, &pin);
	UW	lo;

	if ( base == 0 ) return;
	ISpinLock(&ioapic_lock, &imask);
	lo = ioapic_rd(base, IOAPIC_RTE(pin)) & ~( RTE_LEVEL | RTE_LOW );
	if ( ( mode & IM_EDGE ) == 0 ) lo |= RTE_LEVEL;
	if ( ( mode & IM_LOW ) != 0 ) lo |= RTE_LOW;
	ioapic_wr(base, IOAPIC_RTE(pin), lo);
	ISpinUnlock(&ioapic_lock, &imask);
}

/* The trigger and polarity a GSI starts with: the bus's, or the MADT's override */
LOCAL UW gsi_mode( UINT gsi )
{
	INT	i;
	UW	lo = ( gsi < 16 ) ? 0 : ( RTE_LEVEL | RTE_LOW );

	for ( i = 0; i < knl_acpi.niso; i++ ) {
		T_ACPIISO	*o = &knl_acpi.iso[i];

		if ( o->gsi != gsi ) continue;
		lo = ( o->irq < 16 ) ? 0 : ( RTE_LEVEL | RTE_LOW );
		switch ( o->flags & 3 ) {
		case 1:	lo &= ~RTE_LOW;	break;		/* active high */
		case 3:	lo |= RTE_LOW;	break;		/* active low */
		}
		switch ( ( o->flags >> 2 ) & 3 ) {
		case 1:	lo &= ~RTE_LEVEL; break;	/* edge */
		case 3:	lo |= RTE_LEVEL; break;		/* level */
		}
	}
	return lo;
}

LOCAL void ioapic_init( void )
{
	INT	i;
	UINT	pin;

	for ( i = 0; i < knl_acpi.nioapic; i++ ) {
		T_ACPIIOAPIC	*a = &knl_acpi.ioapic[i];
		UBINT		base = DEV_BASE(a->addr);

		a->ninput = ( ( ioapic_rd(base, IOAPIC_VER) >> 16 ) & 0xff ) + 1;
		for ( pin = 0; pin < a->ninput; pin++ ) {
			UINT	gsi = a->gsi_base + pin;
			UW	vec = ( gsi < INTVEC_GSI_NUM ) ? INTVEC_GSI(gsi) : INTVEC_SPURIOUS;

			ioapic_wr(base, IOAPIC_RTE(pin) + 1, knl_msi_dest << 24);
			ioapic_wr(base, IOAPIC_RTE(pin), RTE_MASKED | gsi_mode(gsi) | vec);
		}
	}
}

/* ------------------------------------------------------------------------ */
/*
 * The processor a device's interrupt goes to (inttarget.c, design 4.5).
 * An IOAPIC's input names it in its entry, a message in its address;
 * both hold an APIC ID of 8 bits, so a processor whose ID is larger is
 * reached only through the IOMMU's remapping, which is not used: such a
 * processor is not given a device's interrupt.
 */
#define IS_GSI_VEC(v)	( (v) >= INTVEC_GSI(0) && (v) < INTVEC_GSI(INTVEC_GSI_NUM) )
#define IS_MSI_VEC(v)	( (v) >= INTVEC_MSI_FIRST && (v) < INTVEC_MSI_FIRST + INTVEC_MSI_NUM )

/* pcie.c: the message of the interrupt written again with this address (E_OK: none made yet) */
EXPORT ER	(*knl_msi_rewrite_hook)( UINT intno, UD addr ) = NULL;

EXPORT BOOL knl_int_is_dev( UINT intno )
{
	return IS_GSI_VEC(intno) || IS_MSI_VEC(intno);
}

EXPORT BOOL knl_int_is_msi( UINT intno )
{
	return IS_MSI_VEC(intno);
}

EXPORT BOOL knl_int_in_use( UINT intno )
{
	return ( intno < N_INTVEC && knl_intvec_tbl[intno] != NULL );
}

/* The APIC ID of PRC prc, ~0 when it cannot take a device's interrupt */
LOCAL UW prc_apic( INT prc )
{
	T_PCPU	*p;

	if ( prc < 1 || prc > knl_num_prc || ( p = knl_pcpu_of(prc - 1) ) == NULL || p->state != PC_ONLINE ) {
		return ~(UW)0;
	}
	return ( p->apic_id <= 0xfe ) ? p->apic_id : ~(UW)0;
}

/* The APIC ID a message is to be sent to as it is made (cpu_msi.h) */
EXPORT UW knl_msi_apic( UINT intno )
{
	UW	id = prc_apic(knl_int_target_of(intno));

	return ( id == ~(UW)0 ) ? knl_msi_dest : id;
}

EXPORT ER knl_int_route( UINT intno, INT prc )
{
	UW	id = prc_apic(prc);
	UINT	pin, imask;
	UBINT	base;
	UW	lo;

	if ( !knl_int_is_dev(intno) || prc < 1 || prc > knl_num_prc ) {
		return E_PAR;
	}
	if ( id == ~(UW)0 ) {
		return E_NOSPT;
	}
	if ( IS_GSI_VEC(intno) ) {
		base = ioapic_of(intno - INTVEC_GSI(0), &pin);
		if ( base == 0 ) {
			return E_PAR;
		}
		/* masked while the destination changes, then as it was */
		ISpinLock(&ioapic_lock, &imask);
		lo = ioapic_rd(base, IOAPIC_RTE(pin));
		ioapic_wr(base, IOAPIC_RTE(pin), lo | RTE_MASKED);
		ioapic_wr(base, IOAPIC_RTE(pin) + 1, id << 24);
		ioapic_wr(base, IOAPIC_RTE(pin), lo);
		ISpinUnlock(&ioapic_lock, &imask);
		return E_OK;
	}
	if ( knl_msi_rewrite_hook == NULL ) {
		return E_OK;		/* no message made yet: made with the new address */
	}
	return (*knl_msi_rewrite_hook)(intno, (UD)( MSI_ADDR_BASE | ( ( id & 0xff ) << 12 ) ));
}

LOCAL void pic_off( void );

/*
 * The machine asleep (kernel/sysdepend/x64/pm.c): this processor's timer
 * stopped, both uses of it; started again from nothing after a sleep, and
 * whether an interrupt waits in its IRR
 */
EXPORT void knl_apic_sleep_quiet( void )
{
	T_PCPU	*p = knl_pcpu();

	p->apic_sys_dl = 0;
	p->apic_lk_dl = 0;
	timer_hw(0);
}

EXPORT void knl_apic_wake_init( void )
{
	T_PCPU	*p = knl_pcpu();

	p->apic_sys_dl = 0;
	p->apic_lk_dl = 0;
	knl_apic_cpu_init();
}

EXPORT BOOL knl_apic_any_pending( void )
{
	UW	r;

	for ( r = 0; r < 8; r++ ) {
		if ( lapic_rd(LAPIC_IRR + r * 0x10) != 0 ) return TRUE;
	}
	return FALSE;
}

/* The IOAPICs' routing entries in buf (two words each); how many words. A reset loses them */
EXPORT INT knl_ioapic_save( UW *buf, INT max )
{
	INT	i, n = 0;
	UINT	pin;

	for ( i = 0; i < knl_acpi.nioapic; i++ ) {
		T_ACPIIOAPIC	*a = &knl_acpi.ioapic[i];
		UBINT		base = DEV_BASE(a->addr);

		for ( pin = 0; pin < a->ninput && n + 2 <= max; pin++ ) {
			buf[n++] = ioapic_rd(base, IOAPIC_RTE(pin));
			buf[n++] = ioapic_rd(base, IOAPIC_RTE(pin) + 1);
		}
	}
	return n;
}

/* The same put back, the 8259s masked again as a reset of the chipset leaves them not */
EXPORT void knl_ioapic_restore( CONST UW *buf )
{
	INT	i, n = 0;
	UINT	pin;

	pic_off();
	for ( i = 0; i < knl_acpi.nioapic; i++ ) {
		T_ACPIIOAPIC	*a = &knl_acpi.ioapic[i];
		UBINT		base = DEV_BASE(a->addr);

		for ( pin = 0; pin < a->ninput; pin++ ) {
			ioapic_wr(base, IOAPIC_RTE(pin), buf[n] | RTE_MASKED);
			ioapic_wr(base, IOAPIC_RTE(pin) + 1, buf[n + 1]);
			ioapic_wr(base, IOAPIC_RTE(pin), buf[n]);
			n += 2;
		}
	}
}

/* The 8259s remapped out of the way of the exceptions, and masked */
LOCAL void pic_off( void )
{
	out_port_b(0x20, 0x11);
	out_port_b(0xa0, 0x11);
	out_port_b(0x21, INTVEC_PIC);
	out_port_b(0xa1, INTVEC_PIC + 8);
	out_port_b(0x21, 0x04);
	out_port_b(0xa1, 0x02);
	out_port_b(0x21, 0x01);
	out_port_b(0xa1, 0x01);
	out_port_b(0x21, 0xff);
	out_port_b(0xa1, 0xff);
}

/*
 * The boot processor's Local APIC and the IOAPICs (knl_init_interrupt).
 * Which mode the Local APIC is used in and whether its timer has the
 * TSC deadline mode is decided here for all processors.
 */
EXPORT void knl_apic_init( void )
{
	UW	a, b, c, d;

	cpuid(1, 0, &a, &b, &c, &d);
	knl_x2apic_on = ( c & ( 1U << 21 ) ) != 0;
	tsc_deadline = ( c & ( 1U << 24 ) ) != 0;
	if ( !knl_x2apic_on ) {
		UD	pa = rdmsr(MSR_APIC_BASE) & 0x000ffffffffff000ULL;

		xapic_base = DEV_BASE(pa);
	}
	pic_off();
	knl_apic_cpu_init();
	knl_msi_dest = knl_apic_id();
	if ( !tsc_deadline ) {
		apic_timer_calibrate();
		lapic_wr(LAPIC_LVT_TMR, INTVEC_TIMER);
	}
	ioapic_init();
	tm_printf((UB *)"TessronOS x64: Local APIC %s, ID %d, timer %s; %d IOAPIC inputs\n",
		  knl_x2apic_on ? "x2APIC" : "xAPIC", knl_msi_dest,
		  tsc_deadline ? "TSC deadline" : "one shot",
		  ( knl_acpi.nioapic > 0 ) ? (INT)knl_acpi.ioapic[0].ninput : 0);
}

#endif /* CPU_CORE_X86_64 */
