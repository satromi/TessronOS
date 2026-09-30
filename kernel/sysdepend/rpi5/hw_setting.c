/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c (Raspberry Pi 5)
 *	Board dependent start-up and shutdown.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tm/tmonitor.h>
#include "sysdepend.h"

/*
 * PSCI call through SMC (the firmware's TF-A BL31 provides PSCI 1.1)
 */
EXPORT UD knl_psci_call( UD fid, UD a1, UD a2, UD a3 )
{
	register UD x0 __asm__("x0") = fid;
	register UD x1 __asm__("x1") = a1;
	register UD x2 __asm__("x2") = a2;
	register UD x3 __asm__("x3") = a3;

	Asm("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

/*
 * PSCI CPU_ON: start a secondary processor at a physical entry address
 */
EXPORT ER knl_psci_cpu_on( UD mpidr, UD entry_pa, UD ctx )
{
	D	r = (D)knl_psci_call(PSCI_CPU_ON, mpidr, entry_pa, ctx);

	if ( r == 0 ) return E_OK;
	if ( r == -4 ) return E_OBJ;		/* already on */
	if ( r == -2 || r == -9 ) return E_PAR;
	return E_SYS;
}

EXPORT void knl_startup_hw( void )
{
	/* nothing to do: the debug PL011 and the GIC are usable as they are */
}

#if USE_SHUTDOWN
EXPORT void knl_shutdown_hw( void )
{
	disint();
	tm_putstring((UB*)"\n<< TessronOS: system off >>\n");
	knl_psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
	while (1) {
		Asm("wfi");
	}
}
#endif /* USE_SHUTDOWN */

EXPORT ER knl_restart_hw( W mode )
{
	switch ( mode ) {
	case -1:	/* reset and re-start */
		disint();
		knl_psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
		while (1) {
			Asm("wfi");
		}
	default:
		return E_NOSPT;
	}
}

#endif /* RPI5 */
