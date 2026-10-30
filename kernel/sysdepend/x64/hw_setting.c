/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c (x64 PC)
 *	Board dependent start-up and shutdown: off through ACPI (\_S5),
 *	a reset through the FADT's reset register; through the ACPI
 *	interpreter when it is running (\_PTS run first, acpica_off.c),
 *	else by the tables alone (acpi.c). \_TTS and the PCI functions'
 *	wake are done earlier, while the system still runs (devinit.c,
 *	knl_finish_device).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include "sysdepend.h"

IMPORT BOOL	knl_acpi_os_alone;	/* acpi_os.c */

/* Time for the other processors to take the stop and halt */
#define PARK_US		10000

EXPORT void knl_startup_hw( void )
{
	/* nothing to do: the firmware left the machine as the kernel takes it */
}

/*
 * The last steps run on this processor alone: the others halted with
 * their interrupts off, so that no task or interrupt of theirs runs AML,
 * touches the embedded controller or starts a transfer while \_PTS runs
 * and the sleep registers are written. A processor stops only where its
 * interrupts are on, never inside a spinlock, so what the interpreter
 * waits on afterwards is at most a semaphore a stopped task held, which
 * knl_acpi_os_alone takes as free.
 */
LOCAL void alone( void )
{
	disint();
	knl_send_ipi_others(SGI_STOP);
	WaitUsec(PARK_US);
	knl_acpi_os_alone = TRUE;
}

#if USE_SHUTDOWN
EXPORT void knl_shutdown_hw( void )
{
	alone();
	tm_putstring((UB*)"\n<< TessronOS: system off >>\n");
	(void)knl_acpica_poweroff();
	(void)knl_acpi_poweroff();
	tm_putstring((UB*)"TessronOS: the machine did not turn off (S5 refused or no \\_S5)\n");
	while (1) {
		Asm("hlt");
	}
}
#endif /* USE_SHUTDOWN */

EXPORT ER knl_restart_hw( W mode )
{
	switch ( mode ) {
	case -1:	/* reset and re-start */
		alone();
		(void)knl_acpica_reset();
		(void)knl_acpi_reset();
		while (1) {
			Asm("hlt");
		}
	default:
		return E_NOSPT;
	}
}

#endif /* X64 */
