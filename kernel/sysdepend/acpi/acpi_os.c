/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpi_os.c
 *	The kernel's services the ACPI interpreter's OS layer is built on
 *	(acpica/tsacpi_os.h, acpica/osl.c): memory, the PCI configuration
 *	space, the SCI, time, semaphores, tasks and spin locks, in plain C
 *	types, so that the interpreter, built apart from the kernel, needs
 *	none of the kernel's headers.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include "sysdepend.h"
#include "acpi.h"
#include "acpica/tsacpi_os.h"

IMPORT ER	knl_obsys_write( INT recno, CONST UB *buf, SZ size );	/* obsys.c */

EXPORT unsigned long long knl_acpi_os_rsdp( void )
{
	return ( knl_bootinfo != NULL ) ? knl_bootinfo->rsdp_pa : 0;
}

/* RAM the firmware or the kernel has (the linear map covers it, cached); anything else is a device's */
LOCAL BOOL is_ram( UD pa, UD len )
{
	UW	i;

	for ( i = 0; knl_bootinfo != NULL && i < knl_bootinfo->nmem; i++ ) {
		CONST T_BIMEM	*m = &knl_bootinfo->mem[i];

		if ( pa >= m->start && pa + len <= m->start + m->size ) {
			return (BOOL)( m->type == BI_MEM_FREE || m->type == BI_MEM_KERNEL || m->type == BI_MEM_ACPI
				    || m->type == BI_MEM_NVS );
		}
	}
	return FALSE;
}

EXPORT void *knl_acpi_os_map( unsigned long long pa, unsigned long long len )
{
	if ( is_ram(pa, len) && pa + len <= knl_bootinfo->dmap_size ) {
		return PA2VA(pa);
	}
	return (void *)DEV_BASE(pa);
}

EXPORT void *knl_acpi_os_pci( unsigned int seg, unsigned int bus, unsigned int dev, unsigned int fn )
{
	INT	i;

	for ( i = 0; i < knl_acpi.nmcfg; i++ ) {
		T_ACPIMCFG	*m = &knl_acpi.mcfg[i];

		if ( m->seg == seg && bus >= m->start && bus <= m->end && dev < 32 && fn < 8 ) {
			return (void *)DEV_BASE(m->base + ( ( (UD)( bus - m->start ) << 20 ) | ( dev << 15 ) | ( fn << 12 ) ));
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- the SCI */

LOCAL void	(*sci_fn)( void *ctx ) = NULL;
LOCAL void	*sci_ctx;
LOCAL UINT	sci_intno = 0;

LOCAL void sci_inthdr( UINT intno )
{
	if ( sci_fn != NULL ) {
		(*sci_fn)(sci_ctx);
	}
	EndOfInt(intno);
}

/*
 * The interrupt is the FADT's SCI: an ISA number the MADT may move, or
 * an input of the IOAPICs. As ACPI has it, it is level triggered and
 * active low unless the MADT says otherwise.
 */
EXPORT int knl_acpi_os_irq( unsigned int irq, void (*fn)( void *ctx ), void *ctx )
{
	UH	flags = 0;
	UW	gsi = ( irq < 16 ) ? knl_acpi_isa_gsi((UB)irq, &flags) : irq;
	UINT	mode = IM_LEVEL | IM_LOW;

	if ( gsi >= INTVEC_GSI_NUM || sci_fn != NULL ) {
		return -1;
	}
	if ( ( flags & 3 ) == 1 ) mode &= ~(UINT)IM_LOW;	/* active high */
	if ( ( ( flags >> 2 ) & 3 ) == 1 ) mode |= IM_EDGE;	/* edge */
	sci_fn = fn;
	sci_ctx = ctx;
	sci_intno = INTVEC_GSI(gsi);
	knl_define_inthdr((INT)sci_intno, TA_HLNG, (FP)sci_inthdr);
	SetIntMode(sci_intno, mode);
	EnableInt(sci_intno, INTPRI_DEVICE);
	tm_printf((UB *)"acpi: the SCI is interrupt %d (GSI %d, %s, active %s)\n", (INT)sci_intno, (INT)gsi,
		  ( mode & IM_EDGE ) ? "edge" : "level", ( mode & IM_LOW ) ? "low" : "high");
	return 0;
}

EXPORT void knl_acpi_os_irq_remove( unsigned int irq )
{
	(void)irq;
	if ( sci_intno != 0 ) {
		DisableInt(sci_intno);
	}
	sci_fn = NULL;
}

/* ---------------------------------------------------------------- time */

EXPORT unsigned long long knl_acpi_os_timer( void )
{
	UD	t = ReadSysCounter(), f = ( knl_cntfrq != 0 ) ? knl_cntfrq : 1000000000ULL;

	return ( t / f ) * 10000000ULL + ( t % f ) * 10000000ULL / f;
}

EXPORT void knl_acpi_os_stall( unsigned int usec )
{
	WaitUsec(usec);
}

EXPORT void knl_acpi_os_sleep( unsigned int msec )
{
	if ( knl_acpi_os_can_wait() ) {
		tk_dly_tsk((TMO)msec);
	} else {
		WaitUsec(msec * 1000);
	}
}

/* ---------------------------------------------------------------- memory */

EXPORT void *knl_acpi_os_alloc( unsigned long long size )
{
	return Kmalloc((SZ)size);
}

EXPORT void knl_acpi_os_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

/* ---------------------------------------------------------------- semaphores, tasks */

EXPORT int knl_acpi_os_can_wait( void )
{
	UINT	s;
	BOOL	di, task;

	/* this processor's fields, read with the interrupts masked (in_indp) */
	DI(s);
	di = isDI(s);
	task = ( knl_ctxtsk != NULL && knl_taskindp == 0 );
	EI(s);
	return ( task && !di ) ? 1 : 0;
}

EXPORT int knl_acpi_os_sem_new( unsigned int max, unsigned int init )
{
	T_CSEM	c;
	ID	id;

	knl_memset(&c, 0, sizeof(c));
	c.sematr = TA_TFIFO | TA_FIRST;
	c.maxsem = ( max == 0 || max > 0x7fffffff ) ? 0x7fffffff : (INT)max;
	c.isemcnt = (INT)init;
	id = tk_cre_sem(&c);
	return ( id > 0 ) ? (int)id : 0;
}

EXPORT void knl_acpi_os_sem_del( int sem )
{
	if ( sem > 0 ) tk_del_sem((ID)sem);
}

/*
 * A wait: in a task with the interrupts on, the semaphore's own; where
 * nothing may wait (an interrupt, the last steps of turning the machine
 * off), polled every 50 us until the time is up
 */
/*
 * Set as the machine turns off or starts again: nothing runs but the
 * processor doing it, the interrupts off, and the interpreter's waits
 * are had at once (nobody else holds what they wait for)
 */
EXPORT BOOL	knl_acpi_os_alone = FALSE;

EXPORT int knl_acpi_os_sem_wait( int sem, unsigned int units, int tmo )
{
	ER	er;
	UD	polls = 0;

	if ( knl_acpi_os_alone ) {
		return 0;
	}
	if ( knl_acpi_os_can_wait() ) {
		er = tk_wai_sem((ID)sem, (INT)units, ( tmo < 0 ) ? TMO_FEVR : (TMO)tmo);
		return ( er >= E_OK ) ? 0 : -1;
	}
	for ( ;; ) {
		if ( tk_wai_sem((ID)sem, (INT)units, TMO_POL) >= E_OK ) {
			return 0;
		}
		if ( tmo >= 0 && polls >= (UD)tmo * 20 ) {
			return -1;
		}
		WaitUsec(50);
		polls++;
	}
}

EXPORT void knl_acpi_os_sem_signal( int sem, unsigned int units )
{
	if ( sem > 0 && !knl_acpi_os_alone ) tk_sig_sem((ID)sem, (INT)units);
}

EXPORT unsigned int knl_acpi_os_tid( void )
{
	if ( !in_indp() ) {
		return (unsigned int)knl_self_ctxtsk()->tskid;
	}
	return 0xffffu;			/* an interrupt: one of its own */
}

typedef struct {
	void	(*fn)( void *ctx );
	void	*ctx;
} T_ACPITASK;

LOCAL void acpi_task( INT stacd, void *exinf )
{
	T_ACPITASK	*a = (T_ACPITASK *)exinf;

	(void)stacd;
	(*a->fn)(a->ctx);
	Kfree(a);
	tk_exd_tsk();
}

/* A task of the interpreter's (its workers), at the drivers' priority */
EXPORT int knl_acpi_os_task( void (*fn)( void *ctx ), void *ctx )
{
	T_CTSK		c;
	T_ACPITASK	*a = (T_ACPITASK *)Kmalloc(sizeof(T_ACPITASK));
	ID		t;

	if ( a == NULL ) {
		return -1;
	}
	a->fn = fn;
	a->ctx = ctx;
	knl_memset(&c, 0, sizeof(c));
	c.exinf = a;
	c.tskatr = TA_HLNG | TA_RNG0;
	c.task = (FP)acpi_task;
	c.itskpri = 12;
	c.stksz = 32 * 1024;
	t = tk_cre_tsk(&c);
	if ( t <= 0 || tk_sta_tsk(t, 0) < E_OK ) {
		Kfree(a);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- spin locks */

EXPORT void *knl_acpi_os_lock_new( void )
{
	T_SPLOCK	*l = (T_SPLOCK *)Kmalloc(sizeof(T_SPLOCK));

	if ( l != NULL ) InitSpinLock(l);
	return l;
}

EXPORT void knl_acpi_os_lock_del( void *lock )
{
	if ( lock != NULL ) Kfree(lock);
}

EXPORT unsigned long long knl_acpi_os_lock( void *lock )
{
	UINT	s = 0;

	ISpinLock((T_SPLOCK *)lock, &s);
	return s;
}

EXPORT void knl_acpi_os_unlock( void *lock, unsigned long long was )
{
	UINT	s = (UINT)was;

	ISpinUnlock((T_SPLOCK *)lock, &s);
}

/* ---------------------------------------------------------------- the console, the power button */

EXPORT void knl_acpi_os_puts( const char *s )
{
	tm_putstring((UB *)s);
}

IMPORT BOOL	knl_acpi_power_pressed( BOOL array );	/* acpi_power.c */

/*
 * The power button pressed: told by the fixed event or a PNP0C0C's
 * notification, or by the 5-button array of the HID event filter
 * (array, acpi_power.c). A firmware that tells of one press both ways
 * is taken once.
 */
LOCAL void power_button( BOOL array )
{
	ER	er;

	if ( !knl_acpi_power_pressed(array) ) {
		tm_printf((UB *)"acpi: the power button: the same press, told the other way too\n");
		return;
	}
	tm_printf((UB *)"acpi: the power button: the session ends and the machine turns off\n");
	er = knl_obsys_write(OB_SYS_POWER, (CONST UB *)"OFF", 3);
	if ( er < E_OK ) {
		tm_printf((UB *)"acpi: no desktop to end the session (%d)\n", (INT)er);
	}
}

EXPORT void knl_acpi_os_power_button( void )
{
	power_button(FALSE);
}

/* The power button of the 5-button array (acpi_power.c) */
EXPORT void knl_acpi_power_button_array( void )
{
	power_button(TRUE);
}

#endif /* X64 */
