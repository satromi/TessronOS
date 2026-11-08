/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pm.c (x64 PC)
 *	The machine asleep (docs/tessronos/03-platform.md 3.10.11).
 *
 *	One task, on the boot processor, runs a sleep from start to end, so
 *	that it is the same task that takes a driver's locks and lets them
 *	go. Tasks are not frozen: drivers hold requests off in their suspend,
 *	and whoever asks a device then waits until it is back.
 *
 *	 1. the records and the file systems written out, \_TTS(sleep state);
 *	 2. the drivers' suspend, in rising order, then every function's PCI
 *	    configuration saved and the endpoints put in D3hot;
 *	 3. the checks and the timers stopped; the other processors parked in
 *	    an interrupt (SGI_CALL), where they hold no lock;
 *	 4. S3: \_PTS(3), the waking vector, SLP_EN; every processor starts
 *	    again at the trampoline (tramp.S), sets up what the reset lost and
 *	    goes on where it stood. Low power S0 idle: the device's _DSM, then
 *	    the deepest MWAIT until an interrupt; any interrupt lets the
 *	    system run for a while so that the firmware's events reach their
 *	    tasks, and if none was a reason to wake it goes back.
 *	 5. the clock: the monotonic clock did not run during the sleep (each
 *	    timer has as long to wait as it had), the wall clock is put ahead
 *	    by what the RTC says went by;
 *	 6. the functions in D0 with their configuration, the drivers' resume
 *	    in falling order, \_TTS(0).
 *
 *	Which way: the firmware's FADT flag LOW_POWER_S0 with the PNP0D80
 *	device means low power S0 idle, and is what the machine was made
 *	for, even when the DSDT also has \_S3; else \_S3. The start-up
 *	setting sleep=off, sleep=s3 or sleep=s0 overrides it.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include <tk/device.h>
#include <ts/pcie.h>
#include <ts/fs.h>
#include <ts/pm.h>
#include "sysdepend.h"
#include "../acpi/acpi.h"
#include "../cpu/core/x86_64/cpu_task.h"
#include "../cpu/core/x86_64/sys_timer.h"

IMPORT BOOL	knl_acpi_os_alone;		/* acpi_os.c */
IMPORT D	knl_real_time_ofs;		/* timer.c */
IMPORT UD	knl_hrt_base;			/* sys_timer.c */
IMPORT UW	knl_xsave_size;
IMPORT BOOL	knl_xsave_on;
IMPORT void	knl_bootlog_final( CONST char *why );
IMPORT ER	knl_rtc_read( UD *p_sec );
IMPORT ER	knl_rtc_alarm_in( UINT sec );		/* rtc.c */
IMPORT void	knl_vtd_again( void );			/* vtd.c */
IMPORT INT	knl_acpica_sleep_caps( void );		/* acpica_sleep.c */
IMPORT void	knl_acpica_lps0_constraints( void );
IMPORT void	knl_acpica_lps0( INT fn );
IMPORT void	knl_acpica_wake_devices( INT on, INT state );
IMPORT INT	knl_acpica_s3_prep( unsigned long long vector );
IMPORT INT	knl_acpica_s3_enter( void );
IMPORT void	knl_acpica_s3_leave_prep( void );
IMPORT void	knl_acpica_s3_leave( void );
IMPORT void	knl_acpica_rtc_event( void (*fn)( void ) );
IMPORT INT	knl_acpica_tts( INT state );
IMPORT void	knl_apic_sleep_quiet( void );		/* apic.c */
IMPORT void	knl_apic_wake_init( void );
IMPORT BOOL	knl_apic_any_pending( void );
IMPORT INT	knl_ioapic_save( UW *buf, INT max );
IMPORT void	knl_ioapic_restore( CONST UW *buf );
IMPORT UD	knl_smp_wake_vector( void (*entry)( INT ) );	/* smp.c */
IMPORT void	knl_smp_wake_others( void );
IMPORT UD	knl_pm_ctx_save( UD *ctx );		/* pm_ctx.S */
IMPORT void	knl_pm_ctx_load( UD *ctx );
IMPORT void	knl_pm_segs_load( void );
IMPORT void	knl_lockup_stop( void );

#define PMC_S3		1
#define PMC_LOW_S0	2
#define PMC_LPS0	4

#define LPS0_SCREEN_OFF	3
#define LPS0_SCREEN_ON	4
#define LPS0_ENTRY	5
#define LPS0_EXIT	6

#define HOW_NONE	(-1)

/* What the machine's firmware and settings allow, and what the buttons do */
LOCAL INT	pm_way = HOW_NONE;		/* PM_S3, PM_S0IDLE or HOW_NONE */
LOCAL INT	pm_btn_sleep = 0;		/* the power button sleeps (else it turns off) */
LOCAL INT	pm_lid_sleep = 0;		/* the lid closed sleeps */
LOCAL BOOL	pm_set_by_user = FALSE;		/* the machine's settings were read */

/* ---------------------------------------------------------------- the drivers */

#define PM_DEV_MAX	32

LOCAL CONST T_PMDEV	*pm_dev[PM_DEV_MAX];
LOCAL INT		pm_ndev = 0;
LOCAL BOOL		pm_dev_ok[PM_DEV_MAX];

EXPORT ER ts_pm_register( CONST T_PMDEV *ops )
{
	INT	i, at;

	if ( pm_ndev >= PM_DEV_MAX ) {
		tm_printf((UB *)"pm: no room for the driver %s: it will not be told of a sleep\n", ops->name);
		return E_LIMIT;
	}
	for ( at = 0; at < pm_ndev && pm_dev[at]->order <= ops->order; at++ ) ;
	for ( i = pm_ndev; i > at; i-- ) pm_dev[i] = pm_dev[i - 1];
	pm_dev[at] = ops;
	pm_ndev++;
	return E_OK;
}

/* ---------------------------------------------------------------- the PCI functions */

#define PCI_CAP_PM	0x01
#define PCI_CAP_MSI	0x05
#define PCI_CAP_PCIE	0x10
#define PCI_CAP_MSIX	0x11

#define MSIX_ENTRIES_MAX 64

typedef struct {
	T_PCIDEV	d;
	UW		hdr[16];
	UW		msi[6];
	UW		exp[13];
	UW		msix_ctl;
	UW		*msix_tab;	/* the table's words, when in use */
	INT		msix_n;
	UB		pm, msi_cap, exp_cap, msix_cap;
	BOOL		d3;
} PCISAVE;

LOCAL PCISAVE	*pci_save = NULL;
LOCAL INT	pci_nsave = 0;

#define CFG_RD(s, off, w)	ts_pcie_cfg_read((s)->d.bus, (s)->d.dev, (s)->d.fn, (off), (w))
#define CFG_WR(s, off, v, w)	ts_pcie_cfg_write((s)->d.bus, (s)->d.dev, (s)->d.fn, (off), (v), (w))

/* The MSI-X table's place in its window, NULL when the window is not there */
LOCAL volatile UW *msix_table( PCISAVE *s )
{
	UW	t = CFG_RD(s, s->msix_cap + 4, 4);
	UBINT	base = ts_pcie_bar_base(&s->d, (INT)( t & 7 ));

	return ( base == 0 ) ? NULL : (volatile UW *)( base + ( t & ~7U ) );
}

LOCAL void pci_save_all( void )
{
	INT	i, k, n = ts_pcie_count();

	if ( pci_save == NULL ) {
		pci_save = (PCISAVE *)Kmalloc(sizeof(PCISAVE) * (SZ)( n + 1 ));
		if ( pci_save == NULL ) return;
		knl_memset(pci_save, 0, sizeof(PCISAVE) * (SZ)( n + 1 ));
	}
	pci_nsave = 0;
	for ( i = 0; i < n; i++ ) {
		PCISAVE	*s = &pci_save[pci_nsave];

		if ( ts_pcie_get(i, &s->d) < E_OK ) continue;
		for ( k = 0; k < 16; k++ ) s->hdr[k] = CFG_RD(s, (UINT)k * 4, 4);
		s->pm = (UB)ts_pcie_cap_find(&s->d, PCI_CAP_PM);
		s->msi_cap = (UB)ts_pcie_cap_find(&s->d, PCI_CAP_MSI);
		s->exp_cap = (UB)ts_pcie_cap_find(&s->d, PCI_CAP_PCIE);
		s->msix_cap = (UB)ts_pcie_cap_find(&s->d, PCI_CAP_MSIX);
		s->d3 = FALSE;
		if ( s->msi_cap != 0 ) {
			for ( k = 0; k < 6; k++ ) s->msi[k] = CFG_RD(s, s->msi_cap + (UINT)k * 4, 4);
		}
		if ( s->exp_cap != 0 ) {
			for ( k = 0; k < 13; k++ ) s->exp[k] = CFG_RD(s, s->exp_cap + (UINT)k * 4, 4);
		}
		if ( s->msix_cap != 0 ) {
			s->msix_ctl = CFG_RD(s, s->msix_cap + 2, 2);
			if ( ( s->msix_ctl & 0x8000 ) != 0 ) {
				volatile UW	*t = msix_table(s);

				s->msix_n = (INT)( s->msix_ctl & 0x7ff ) + 1;
				if ( s->msix_n > MSIX_ENTRIES_MAX ) s->msix_n = MSIX_ENTRIES_MAX;
				if ( s->msix_tab == NULL ) {
					s->msix_tab = (UW *)Kmalloc((SZ)MSIX_ENTRIES_MAX * 16);
				}
				if ( t != NULL && s->msix_tab != NULL ) {
					for ( k = 0; k < s->msix_n * 4; k++ ) s->msix_tab[k] = t[k];
				} else {
					s->msix_n = 0;
				}
			} else {
				s->msix_n = 0;
			}
		}
		pci_nsave++;
	}
	tm_printf((UB *)"pm: the configuration of %d PCI functions saved\n", pci_nsave);
}

/*
 * What goes to D3hot: the endpoints of the classes that have a driver
 * here. The bridges, the host bridge and the chipset's own functions
 * (the power management controller, the SMBus, the LPC) stay, and so does
 * the display function, whose pipe keeps running behind a dark panel:
 * nothing here can yet train the panel's link again after turning it off.
 */
LOCAL BOOL pci_may_d3( CONST PCISAVE *s )
{
	UW	cc = s->hdr[2] >> 16;		/* class, subclass */

	if ( ( s->hdr[3] >> 16 & 0x7f ) != 0 ) return FALSE;	/* not a type 0 header */
	if ( knl_pcie_is_domain_host(&s->d) ) return FALSE;	/* what is behind it would go with it */
	switch ( cc >> 8 ) {
	case 0x01:			/* storage */
	case 0x02:			/* network */
	case 0x04:			/* multimedia: the HD Audio, the camera's CSI-2 */
		return TRUE;
	case 0x0c:
		return cc == 0x0c03;	/* USB */
	default:
		return FALSE;
	}
}

LOCAL void pci_d3_all( void )
{
	INT	i;

	for ( i = pci_nsave - 1; i >= 0; i-- ) {
		PCISAVE	*s = &pci_save[i];
		UW	v;

		if ( s->pm == 0 || !pci_may_d3(s) ) continue;
		v = CFG_RD(s, s->pm + 4, 2);
		CFG_WR(s, s->pm + 4, ( v & ~3U ) | 3, 2);
		s->d3 = TRUE;
	}
	WaitUsec(10000);
	for ( i = 0; i < pci_nsave; i++ ) {
		PCISAVE	*s = &pci_save[i];

		if ( s->d3 ) {
			tm_printf((UB *)"pm: %02x:%02x.%x %04x:%04x: PMCSR %04x\n", s->d.bus, s->d.dev, s->d.fn,
				  s->d.vendor, s->d.device, CFG_RD(s, s->pm + 4, 2));
		}
	}
}

/*
 * Every function back: D0 and its 10 ms, then the configuration with the
 * command register last, bus by bus as the walk found them, so that a
 * bridge is back before what is behind it. The function's status and the
 * read-only words take no harm from being written.
 */
LOCAL void pci_restore_all( void )
{
	INT	i, k;
	BOOL	waited = FALSE;

	for ( i = 0; i < pci_nsave; i++ ) {
		PCISAVE	*s = &pci_save[i];

		if ( s->pm == 0 ) continue;
		if ( ( CFG_RD(s, s->pm + 4, 2) & 3 ) != 0 ) {
			CFG_WR(s, s->pm + 4, CFG_RD(s, s->pm + 4, 2) & ~3U, 2);
			waited = TRUE;
		}
	}
	if ( waited ) WaitUsec(10000);
	for ( i = 0; i < pci_nsave; i++ ) {
		PCISAVE	*s = &pci_save[i];
		UW	id = CFG_RD(s, 0, 4);

		if ( id == 0xffffffffU ) {
			tm_printf((UB *)"pm: %02x:%02x.%x did not come back\n", s->d.bus, s->d.dev, s->d.fn);
			continue;
		}
		for ( k = 15; k >= 2; k-- ) {
			if ( k == 3 ) {
				CFG_WR(s, 0x0c, s->hdr[3] & 0xffff, 2);	/* the cache line and the latency */
			} else {
				CFG_WR(s, (UINT)k * 4, s->hdr[k], 4);
			}
		}
		if ( s->exp_cap != 0 ) {
			CFG_WR(s, s->exp_cap + 0x08, s->exp[2] & 0xffff, 2);	/* device control */
			CFG_WR(s, s->exp_cap + 0x10, s->exp[4] & 0xffff, 2);	/* link control */
			CFG_WR(s, s->exp_cap + 0x18, s->exp[6] & 0xffff, 2);	/* slot control */
			CFG_WR(s, s->exp_cap + 0x1c, s->exp[7] & 0xffff, 2);	/* root control */
			CFG_WR(s, s->exp_cap + 0x28, s->exp[10] & 0xffff, 2);	/* device control 2 */
			CFG_WR(s, s->exp_cap + 0x30, s->exp[12] & 0xffff, 2);	/* link control 2 */
		}
		if ( s->msi_cap != 0 ) {
			for ( k = 1; k < 6; k++ ) CFG_WR(s, s->msi_cap + (UINT)k * 4, s->msi[k], 4);
			CFG_WR(s, s->msi_cap + 2, s->msi[0] >> 16, 2);
		}
		CFG_WR(s, 0x04, s->hdr[1] & 0xffff, 2);
		if ( s->msix_cap != 0 ) {
			CFG_WR(s, s->msix_cap + 2, s->msix_ctl | 0x4000, 2);	/* enabled, every vector masked */
			if ( s->msix_n > 0 ) {
				volatile UW	*t = msix_table(s);

				if ( t != NULL ) {
					for ( k = 0; k < s->msix_n * 4; k++ ) t[k] = s->msix_tab[k];
				}
			}
			CFG_WR(s, s->msix_cap + 2, s->msix_ctl, 2);
		}
	}
	tm_printf((UB *)"pm: the configuration of %d PCI functions restored\n", pci_nsave);
}

/* ---------------------------------------------------------------- the processors */

typedef struct {
	UD	ctx[8];		/* pm_ctx.S */
	UD	cr3, fs, kgs;
	BOOL	fpu;		/* the running task's x87/SSE/AVX registers were saved in its area */
	UB	stack[16 * 1024] __attribute__((aligned(16)));	/* where it comes in from the trampoline */
} PMCPU;

LOCAL PMCPU		*pm_cpu = NULL;
LOCAL volatile INT	pm_how = HOW_NONE;	/* set while the others are to stay parked */
LOCAL volatile UW	pm_parked = 0;		/* how many have parked */
LOCAL volatile UW	pm_back = 0;		/* how many started again from the trampoline */
LOCAL volatile UW	pm_release = 0;		/* the parked ones go on */
LOCAL volatile UW	pm_touched = 0;		/* a parked processor found an interrupt */
LOCAL UD		pm_tsc_off = 0;		/* to add to the counter after S3 */
LOCAL BOOL		pm_tsc_adjust = FALSE;
LOCAL BOOL		pm_mwait = FALSE;	/* MWAIT with break on interrupt, on real hardware */
LOCAL UW		pm_mwait_hint = 0;

#define MSR_TSC			0x10
#define MSR_FS_BASE_PM		0xc0000100

/* The registers of a TA_FPU task that runs here are only in the processor */
LOCAL void fpu_save( PMCPU *c )
{
	TCB	*t = knl_pcpu()->ctxtsk;
	void	*a;

	c->fpu = FALSE;
	if ( t == NULL || ( t->tskatr & TA_FPU ) == 0 ) return;
	a = knl_fpu_area(t);
	if ( knl_xsave_on ) {
		Asm("xsave64 %0" : "=m"(*(UB *)a) : "a"(0xffffffffU), "d"(0xffffffffU) : "memory");
	} else {
		Asm("fxsave64 %0" : "=m"(*(UB *)a) :: "memory");
	}
	c->fpu = TRUE;
}

LOCAL void fpu_load( PMCPU *c )
{
	void	*a;

	if ( !c->fpu ) return;
	a = knl_fpu_area(knl_pcpu()->ctxtsk);
	if ( knl_xsave_on ) {
		Asm("xrstor64 %0" :: "m"(*(UB *)a), "a"(0xffffffffU), "d"(0xffffffffU) : "memory");
	} else {
		Asm("fxrstor64 %0" :: "m"(*(UB *)a) : "memory");
	}
}

LOCAL void cpu_state_save( PMCPU *c )
{
	Asm("mov %%cr3, %0" : "=r"(c->cr3));
	c->fs = rdmsr(MSR_FS_BASE_PM);
	c->kgs = rdmsr(MSR_KERNEL_GS_BASE);
	fpu_save(c);
}

/*
 * A processor that started from the trampoline: its tables, its control
 * registers and its Local APIC as at start-up, the counter put back where
 * it was, then the address space and the user's bases it had
 */
LOCAL void cpu_state_load( T_PCPU *p, PMCPU *c )
{
	UD	rsp0 = p->kstack;

	knl_cpu_init(p);
	knl_pm_segs_load();
	p->tss.rsp0 = rsp0;
	Asm("mov %0, %%cr3" :: "r"(c->cr3) : "memory");
	{
		UD	before = ReadSysCounterSync();

		if ( pm_tsc_adjust ) {
			wrmsr(MSR_TSC_ADJUST, rdmsr(MSR_TSC_ADJUST) + pm_tsc_off);
		}
		/* an adjust register that does not move the counter (a virtual machine's): write the counter */
		if ( !pm_tsc_adjust || ReadSysCounterSync() - before > pm_tsc_off / 2 + 1000000000ULL
		  || ReadSysCounterSync() - before < pm_tsc_off / 2 ) {
			wrmsr(MSR_TSC, before + pm_tsc_off);
		}
	}
	knl_apic_wake_init();
	wrmsr(MSR_FS_BASE_PM, c->fs);
	wrmsr(MSR_KERNEL_GS_BASE, c->kgs);
	fpu_load(c);
}

/* The top of the stack a processor comes in on from the trampoline (smp.c) */
EXPORT void *knl_pm_wake_stack( INT i )
{
	return &pm_cpu[i].stack[sizeof(pm_cpu[i].stack)];
}

/* For the start of the others (smp.c): how many are back; one that will not come counts as back, no more parked */
EXPORT UW knl_pm_back_count( void )
{
	return __atomic_load_n(&pm_back, __ATOMIC_ACQUIRE);
}

EXPORT void knl_pm_cpu_lost( void )
{
	__atomic_add_fetch(&pm_back, 1, __ATOMIC_ACQ_REL);
	__atomic_sub_fetch(&pm_parked, 1, __ATOMIC_ACQ_REL);
}

/* Where each processor comes in from the trampoline, on its temporary stack */
LOCAL void wake_entry( INT i )
{
	T_PCPU	*p = knl_pcpu_p[i];
	PMCPU	*c = &pm_cpu[i];

	if ( i == 0 ) {
		pm_tsc_off = ( pm_tsc_off - ReadSysCounterSync() );
	}
	cpu_state_load(p, c);
	knl_pm_ctx_load(c->ctx);
}

/* Parked processors wait: the deepest wait the machine gives while the system stays powered */
LOCAL void park_wait( void )
{
	while ( __atomic_load_n(&pm_release, __ATOMIC_ACQUIRE) == 0 ) {
		if ( pm_how == PM_S3 ) {
			Asm("wbinvd; hlt" ::: "memory");
		} else if ( pm_mwait ) {
			Asm("monitor" :: "a"(&pm_release), "c"(0), "d"(0));
			if ( pm_release != 0 ) break;
			Asm("mwait" :: "a"(pm_mwait_hint), "c"(1));
			if ( pm_release == 0 && knl_apic_any_pending() ) {
				__atomic_store_n(&pm_touched, 1, __ATOMIC_RELEASE);
			}
		} else {
			CpuRelax();
		}
	}
}

/* SGI_CALL: this processor parks, here where the interrupts were on, so without a lock */
EXPORT void knl_pm_park_here( void )
{
	T_PCPU	*p = knl_pcpu();
	PMCPU	*c = &pm_cpu[p->prcid - 1];

	cpu_state_save(c);
	if ( knl_pm_ctx_save(c->ctx) == 0 ) {
		knl_apic_sleep_quiet();
		__atomic_add_fetch(&pm_parked, 1, __ATOMIC_ACQ_REL);
		park_wait();
		knl_lockup_cpu_init();
	} else {
		__atomic_add_fetch(&pm_back, 1, __ATOMIC_ACQ_REL);
		while ( __atomic_load_n(&pm_release, __ATOMIC_ACQUIRE) == 0 ) CpuRelax();
		knl_lockup_cpu_init();
	}
	__atomic_sub_fetch(&pm_parked, 1, __ATOMIC_ACQ_REL);
}

/* The other processors parked, this one with its interrupts off. FALSE when one did not come */
LOCAL BOOL park_others( void )
{
	UD	t0;
	UW	want = (UW)knl_num_prc - 1;

	__atomic_store_n(&pm_release, 0, __ATOMIC_RELEASE);
	pm_touched = 0;
	if ( want == 0 ) return TRUE;
	knl_send_ipi_others(SGI_CALL);
	t0 = knl_get_mono_ns();
	while ( __atomic_load_n(&pm_parked, __ATOMIC_ACQUIRE) < want ) {
		if ( knl_get_mono_ns() - t0 > 2000000000ULL ) {
			tm_printf((UB *)"pm: only %u of %u processors parked\n", pm_parked, want);
			return FALSE;
		}
		CpuRelax();
	}
	return TRUE;
}

LOCAL void unpark_others( void )
{
	UD	t0;

	__atomic_store_n(&pm_release, 1, __ATOMIC_RELEASE);
	t0 = knl_get_mono_ns();
	while ( __atomic_load_n(&pm_parked, __ATOMIC_ACQUIRE) != 0 && knl_get_mono_ns() - t0 < 1000000000ULL ) {
		CpuRelax();
	}
}

/* Whether MWAIT with break on interrupt can be used, and the deepest state CPUID 5 lists */
LOCAL void mwait_find( void )
{
	UW	a, b, c, d;
	INT	i;

	cpuid(1, 0, &a, &b, &c, &d);
	if ( ( c & ( 1U << 3 ) ) == 0 || ( c & ( 1U << 31 ) ) != 0 ) return;	/* none, or virtual */
	cpuid(0, 0, &a, &b, &c, &d);
	if ( a < 5 ) return;
	cpuid(5, 0, &a, &b, &c, &d);
	if ( ( c & 3 ) != 3 ) return;
	for ( i = 7; i >= 0; i-- ) {
		UW	subs = ( d >> ( 4 * i ) ) & 0xf;

		if ( subs != 0 ) {
			pm_mwait_hint = ( (UW)( i > 0 ? i - 1 : 0 ) << 4 ) | ( subs - 1 );
			pm_mwait = TRUE;
			break;
		}
	}
	tm_printf((UB *)"pm: MWAIT %s, the sub-states of CPUID 5 %08x, the deepest hint %02x\n",
		  pm_mwait ? "with break on interrupt" : "not usable", d, pm_mwait_hint);
}

/* ---------------------------------------------------------------- the records of the low power state */

/*
 * The firmware's LPIT names where the time in the low power state is
 * counted: a model register or a word of the power controller. Read
 * before and after, it says whether the machine really got there.
 */
LOCAL UD lpit_read( INT index )
{
	CONST UB	*t = (CONST UB *)knl_acpi_table("LPIT", 0);
	UW		len, p = 36;
	INT		k = 0;

	if ( t == NULL ) return 0;
	len = *(CONST UW *)( t + 4 );
	while ( p + 56 <= len ) {
		UW	el = *(CONST UW *)( t + p + 4 );

		if ( el == 0 ) break;
		if ( k++ == index ) {
			UB	space = t[p + 36];
			UD	addr = *(CONST UD *)( t + p + 40 );
			UD	v = 0;

			if ( space == 0x7f ) {
				v = rdmsr((UW)addr);
			} else if ( space == 0 ) {
				v = in_w(DEV_BASE(addr & ~0xfffULL) + ( addr & 0xfff ));
			}
			return v;
		}
		p += el;
	}
	return 0;
}

/* ---------------------------------------------------------------- one sleep */

LOCAL volatile CONST char	*pm_wake_why = NULL;	/* a reason to wake, while asleep */
LOCAL volatile BOOL		pm_sleeping = FALSE;
LOCAL UD			pm_woke_at = 0;		/* ms of the monotonic clock */
LOCAL UD			pm_sleeps = 0;

#define WINDOW_MS	400	/* how long the system runs after an interrupt that may be a reason to wake */
#define AFTER_WAKE_MS	2000	/* a button's press this soon after waking was the one that woke it */

LOCAL UD mono_ms( void )
{
	return knl_get_mono_ns() / 1000000ULL;
}

LOCAL void wall_ahead( UD rtc0 )
{
	UD	rtc1 = rtc0;

	if ( knl_rtc_read(&rtc1) >= E_OK && rtc1 > rtc0 ) {
		knl_real_time_ofs += (D)( rtc1 - rtc0 ) * 1000000000LL;
	}
	tm_printf((UB *)"pm: asleep %ld s by the RTC; the clock put ahead\n", (D)( rtc1 - rtc0 ));
}

/* The queue's next expiry looked at again */
LOCAL void timer_again( void )
{
	knl_set_hw_timer(knl_get_mono_ns());
}

/* S3. The interrupts are off and the checks stopped; the others are parked */
LOCAL void s3_sleep( UD vector )
{
	INT	me = knl_pcpu()->prcid - 1;
	PMCPU	*c = &pm_cpu[me];
	static UW	ioa_save[512];

	knl_acpi_os_alone = TRUE;
	if ( knl_acpica_s3_prep(vector) < 0 ) {
		knl_acpi_os_alone = FALSE;
		return;
	}
	(void)knl_ioapic_save(ioa_save, sizeof(ioa_save) / sizeof(ioa_save[0]));
	cpu_state_save(c);
	pm_tsc_off = ReadSysCounterSync();
	if ( knl_pm_ctx_save(c->ctx) == 0 ) {
		knl_apic_sleep_quiet();
		(void)knl_acpica_s3_enter();
		tm_printf((UB *)"pm: S3 refused: the system goes on\n");
		knl_apic_wake_init();
		knl_ioapic_restore(ioa_save);
		knl_acpica_s3_leave_prep();
		knl_acpi_os_alone = FALSE;
		knl_lockup_cpu_init();
		timer_again();
		return;
	}
	/* back from the trampoline (wake_entry), the counter and this processor's tables in place */
	tm_printf((UB *)"pm: S3: back at the trampoline, the counter %ld\n", (D)ReadSysCounterSync());
	knl_ioapic_restore(ioa_save);
	knl_acpica_s3_leave_prep();
	knl_vtd_again();
	knl_acpi_os_alone = FALSE;
	knl_lockup_cpu_init();
	timer_again();
	tm_printf((UB *)"pm: S3: the firmware's steps done, the other processors next\n");
	knl_smp_wake_others();
	while ( __atomic_load_n(&pm_back, __ATOMIC_ACQUIRE) + 1 < (UW)knl_num_prc ) {
		CpuRelax();
	}
	pm_back = 0;
	tm_printf((UB *)"pm: S3: %d processors back\n", knl_num_prc);
}

/*
 * Low power S0 idle: this processor waits with the timer stopped until
 * an interrupt. Each interrupt lets the system run for WINDOW_MS, so that
 * an event of the firmware's reaches its task (the embedded controller's
 * query, a notification) and may give the reason to wake; the time this
 * processor stood is taken out of the monotonic clock.
 */
LOCAL void s0_wait( void )
{
	UD	t0, t1, wakes = 0;
	UINT	s;

	for ( ;; ) {
		DI(s);
		(void)park_others();
		knl_apic_sleep_quiet();
		t0 = ReadSysCounterSync();
		if ( pm_wake_why == NULL && pm_touched == 0 ) {
			if ( pm_mwait ) {
				Asm("monitor" :: "a"(&pm_touched), "c"(0), "d"(0));
				Asm("mwait" :: "a"(pm_mwait_hint), "c"(1));
				Asm("sti; nop; cli" ::: "memory");
			} else {
				Asm("sti; hlt; cli" ::: "memory");
			}
			wakes++;
		}
		t1 = ReadSysCounterSync();
		knl_hrt_base += t1 - t0;
		knl_apic_wake_init();
		knl_lockup_cpu_init();
		timer_again();
		unpark_others();
		EI(s);
		if ( pm_wake_why != NULL ) break;
		(void)tk_ena_dsp();
		{
			INT	k;

			for ( k = 0; k < WINDOW_MS / 20 && pm_wake_why == NULL; k++ ) {
				tk_dly_tsk(20);
			}
		}
		(void)tk_dis_dsp();
		if ( pm_wake_why != NULL ) break;
	}
	tm_printf((UB *)"pm: %ld interrupts while asleep\n", (D)wakes);
}

LOCAL ER sleep_once( void )
{
	INT	how = pm_way, i;
	UD	rtc0 = 0, t0, lp0[2], vector = 0;
	UINT	s;
	ER	er;

	t0 = mono_ms();
	tm_printf((UB *)"pm: sleep %ld begins, by %s\n", (D)++pm_sleeps,
		  ( how == PM_S3 ) ? "S3" : "low power S0 idle");
	(void)fs_sync();
	knl_bootlog_final("goes to sleep");
	(void)knl_acpica_tts(( how == PM_S3 ) ? 3 : 0);

	for ( i = 0; i < pm_ndev; i++ ) {
		UD	d0 = mono_ms();

		pm_dev_ok[i] = FALSE;
		er = (*pm_dev[i]->suspend)(pm_dev[i]->arg, how);
		tm_printf((UB *)"pm: %s suspended in %ld ms: %d\n", pm_dev[i]->name, (D)( mono_ms() - d0 ), er);
		if ( er < E_OK ) {
			tm_printf((UB *)"pm: %s did not stop: the machine stays awake\n", pm_dev[i]->name);
			goto resume;
		}
		pm_dev_ok[i] = TRUE;
	}
	pci_save_all();
	if ( how == PM_S0IDLE ) {
		pci_d3_all();
		knl_acpica_lps0_constraints();
	}
	knl_acpica_wake_devices(1, ( how == PM_S3 ) ? 3 : 0);
	if ( how == PM_S3 ) {
		vector = knl_smp_wake_vector(wake_entry);
		if ( vector == 0 ) {
			tm_printf((UB *)"pm: no page below 1MB for the waking vector\n");
			goto restore;
		}
	}
	(void)knl_rtc_read(&rtc0);
	lp0[0] = lpit_read(0);
	lp0[1] = lpit_read(1);
	tm_printf((UB *)"pm: everything stopped in %ld ms; the last step\n", (D)( mono_ms() - t0 ));

	if ( how == PM_S0IDLE ) {
		knl_acpica_lps0(LPS0_SCREEN_OFF);
		knl_acpica_lps0(LPS0_ENTRY);
	}
	knl_lockup_stop();
	(void)tk_dis_dsp();
	pm_wake_why = NULL;
	pm_sleeping = TRUE;
	if ( how == PM_S3 ) {
		DI(s);
		pm_how = PM_S3;
		if ( park_others() ) {
			s3_sleep(vector);
		}
		pm_how = HOW_NONE;
		unpark_others();
		EI(s);
	} else {
		pm_how = PM_S0IDLE;
		s0_wait();
		pm_how = HOW_NONE;
	}
	pm_sleeping = FALSE;
	tm_printf((UB *)"pm: the others go on\n");
	(void)tk_ena_dsp();
	knl_lockup_arm();
	if ( how == PM_S3 ) {
		knl_acpica_s3_leave();
	} else {
		knl_acpica_lps0(LPS0_EXIT);
		knl_acpica_lps0(LPS0_SCREEN_ON);
	}
	wall_ahead(rtc0);
	tm_printf((UB *)"pm: awake: why %s; the low power state's counters went up %ld and %ld\n",
		  ( pm_wake_why != NULL ) ? pm_wake_why : "(a waking from S3)",
		  (D)( lpit_read(0) - lp0[0] ), (D)( lpit_read(1) - lp0[1] ));
	knl_acpica_wake_devices(0, ( how == PM_S3 ) ? 3 : 0);
restore:
	pci_restore_all();
resume:
	for ( i = pm_ndev - 1; i >= 0; i-- ) {
		UD	d0 = mono_ms();

		if ( !pm_dev_ok[i] ) continue;
		(*pm_dev[i]->resume)(pm_dev[i]->arg, how);
		tm_printf((UB *)"pm: %s resumed in %ld ms\n", pm_dev[i]->name, (D)( mono_ms() - d0 ));
	}
	(void)knl_acpica_tts(0);
	pm_woke_at = mono_ms() | 1;
	tm_printf((UB *)"pm: sleep %ld ended\n", (D)pm_sleeps);
	knl_bootlog_final("woke");
	return E_OK;
}

/* ---------------------------------------------------------------- the task and what asks it */

LOCAL ID	pm_flg = 0;
LOCAL UINT	pm_test_n = 0, pm_test_after = 60, pm_test_for = 20;

#define F_SLEEP		1

LOCAL void rtc_woke( void )
{
	pm_wake_why = "the RTC's alarm";
}

/* What wakes the machine, from where the firmware's events are told */
EXPORT void knl_pm_wake_event( CONST char *why )
{
	if ( pm_sleeping ) pm_wake_why = why;
}

LOCAL void pm_task( INT stacd, void *exinf )
{
	UINT	f;

	if ( pm_test_n > 0 ) {
		tk_dly_tsk((TMO)pm_test_after * 1000);
	}
	for ( ;; ) {
		if ( pm_test_n > 0 ) {
			tm_printf((UB *)"pm: test: %u sleeps left, each of %u s by the RTC's alarm\n", pm_test_n, pm_test_for);
			if ( knl_rtc_alarm_in(pm_test_for) >= E_OK ) {
				knl_acpica_rtc_event(rtc_woke);
			}
			(void)sleep_once();
			knl_acpica_rtc_event(NULL);
			pm_test_n--;
			tk_dly_tsk(pm_test_after > 20 ? 20000 : (TMO)pm_test_after * 1000);
			continue;
		}
		if ( tk_wai_flg(pm_flg, F_SLEEP, TWF_ORW | TWF_BITCLR, &f, TMO_FEVR) >= E_OK ) {
			(void)sleep_once();
		}
	}
}

/* Asleep on request (the system object's POWER record, the button, the lid): E_NOSPT when it cannot */
EXPORT ER knl_pm_sleep( CONST char *why )
{
	if ( pm_way == HOW_NONE || pm_flg <= 0 ) return E_NOSPT;
	tm_printf((UB *)"pm: asked to sleep: %s\n", why);
	return tk_set_flg(pm_flg, F_SLEEP);
}

EXPORT BOOL knl_pm_can_sleep( void )
{
	return pm_way != HOW_NONE;
}

/*
 * The power button was pressed (acpi_os.c): TRUE when it is dealt with
 * here. Asleep it wakes the machine; a press told just after a waking is
 * the same press; else it sleeps when the settings say so.
 */
EXPORT BOOL knl_pm_button( void )
{
	if ( pm_sleeping ) {
		pm_wake_why = "the power button";
		return TRUE;
	}
	if ( pm_woke_at != 0 && mono_ms() - pm_woke_at < AFTER_WAKE_MS ) {
		tm_printf((UB *)"pm: the power button's press that woke the machine: nothing more to do\n");
		return TRUE;
	}
	if ( pm_btn_sleep && knl_pm_sleep("the power button") >= E_OK ) return TRUE;
	return FALSE;
}

/* The lid moved (acpi_power.c) */
EXPORT void knl_pm_lid( BOOL open )
{
	if ( open ) {
		if ( pm_sleeping ) pm_wake_why = "the lid opened";
		return;
	}
	if ( pm_lid_sleep && !pm_sleeping ) (void)knl_pm_sleep("the lid closed");
}

/*
 * The machine's settings (uconf.c): what the power button and the lid do.
 * A word absent leaves the default: the button turns the machine off, and
 * the lid of a machine that has one sleeps.
 */
EXPORT void knl_pm_policy( INT button_sleep, INT lid_sleep )
{
	if ( button_sleep >= 0 ) pm_btn_sleep = button_sleep;
	if ( lid_sleep >= 0 ) pm_lid_sleep = lid_sleep;
	pm_set_by_user = TRUE;
	tm_printf((UB *)"pm: the power button %s, the lid %s\n", pm_btn_sleep ? "sleeps" : "turns off",
		  pm_lid_sleep ? "sleeps" : "does nothing");
}

EXPORT INT knl_pm_policy_get( INT *p_button, INT *p_lid )
{
	*p_button = pm_btn_sleep;
	*p_lid = pm_lid_sleep;
	return pm_way;
}

/* The value of a start-up setting "name=value", NULL when absent */
LOCAL CONST char *setting( CONST char *name )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; name[k] != '\0' && c[k] == name[k]; k++ ) ;
		if ( name[k] == '\0' && c[k] == '=' ) return c + k + 1;
	}
	return NULL;
}

LOCAL BOOL val_is( CONST char *v, CONST char *w )
{
	INT	k;

	if ( v == NULL ) return FALSE;
	for ( k = 0; w[k] != '\0' && v[k] == w[k]; k++ ) ;
	return w[k] == '\0' && ( v[k] == '\0' || v[k] == ' ' );
}

LOCAL UINT val_num( CONST char *v, UINT dflt )
{
	UINT	n = 0;

	if ( v == NULL || *v < '0' || *v > '9' ) return dflt;
	for ( ; *v >= '0' && *v <= '9'; v++ ) n = n * 10 + (UINT)( *v - '0' );
	return n;
}

/* Called once the interpreter runs (devinit.c): the way decided and the task started */
EXPORT void knl_pm_init( void )
{
	UW	a, b, c, d;
	INT	caps = knl_acpica_sleep_caps(), i;
	CONST char	*v = setting("sleep");
	T_CFLG	cf;
	T_CTSK	ct;
	ID	t;
	UBINT	set[( CNF_MAX_PRCID + 63 ) / 64];

	if ( ( caps & PMC_LOW_S0 ) != 0 && ( caps & PMC_LPS0 ) != 0 ) {
		pm_way = PM_S0IDLE;
	} else if ( ( caps & PMC_S3 ) != 0 ) {
		pm_way = PM_S3;
	}
	if ( val_is(v, "off") ) pm_way = HOW_NONE;
	if ( val_is(v, "s3") && ( caps & PMC_S3 ) != 0 ) pm_way = PM_S3;
	if ( val_is(v, "s0") ) pm_way = PM_S0IDLE;
	tm_printf((UB *)"pm: the machine sleeps by %s%s\n", ( pm_way == PM_S3 ) ? "S3"
		  : ( pm_way == PM_S0IDLE ) ? "low power S0 idle" : "nothing: it does not sleep",
		  ( v != NULL ) ? " (the start-up setting sleep=)" : "");
	if ( pm_way == HOW_NONE ) return;

	cpuid(7, 0, &a, &b, &c, &d);
	pm_tsc_adjust = ( b & 2U ) != 0;
	mwait_find();
	pm_cpu = (PMCPU *)Kmalloc(sizeof(PMCPU) * (SZ)knl_num_prc);
	if ( pm_cpu == NULL ) {
		pm_way = HOW_NONE;
		return;
	}
	if ( knl_acpica_device("PNP0C0D", 0) != NULL ) pm_lid_sleep = 1;
	if ( val_is(setting("pwrbtn"), "sleep") ) pm_btn_sleep = 1;
	if ( val_is(setting("pwrbtn"), "off") ) pm_btn_sleep = 0;
	if ( val_is(setting("lid"), "sleep") ) pm_lid_sleep = 1;
	if ( val_is(setting("lid"), "ignore") ) pm_lid_sleep = 0;
	pm_test_n = val_num(setting("sleeptest"), 0);
	pm_test_after = val_num(setting("sleeptest_after"), pm_test_after);
	pm_test_for = val_num(setting("sleeptest_for"), pm_test_for);

	knl_memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WMUL;
	pm_flg = tk_cre_flg(&cf);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)pm_task;
	ct.itskpri = 3;
	ct.stksz = 32 * 1024;
	t = tk_cre_tsk(&ct);
	if ( pm_flg <= 0 || t <= 0 ) {
		pm_way = HOW_NONE;
		return;
	}
	for ( i = 0; i < (INT)( sizeof(set) / sizeof(set[0]) ); i++ ) set[i] = 0;
	set[0] = 1;			/* the boot processor: the one the firmware wakes in S3 */
	(void)tk_set_asp(t, set, (SZ)sizeof(set));
	(void)tk_sta_tsk(t, 0);
}

#endif /* X64 */
