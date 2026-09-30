/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_status.h
 *	CPU status (ARMv8-A / AArch64 core)
 */

#ifndef _SYSDEPEND_CPU_CORE_STATUS_
#define _SYSDEPEND_CPU_CORE_STATUS_

#include <tk/syslib.h>
#include <sys/sysdef.h>
#include "sysdepend.h"

/*
 * Critical section
 *	Interrupts are masked with PSTATE.I. A dispatch request raised inside
 *	the section is executed at its end.
 */
#define BEGIN_CRITICAL_SECTION	{ UINT _intsts_ = knl_lock_kernel();
#define END_CRITICAL_SECTION	knl_unlock_kernel_keep();		\
				if ( !isDI(_intsts_)			\
				  && knl_ctxtsk != knl_schedtsk		\
				  && !knl_isTaskIndependent()		\
				  && !knl_dispatch_disabled ) {		\
					knl_dispatch();			\
				}					\
				enaint(_intsts_); }

#define BEGIN_DISABLE_INTERRUPT	{ UINT _intsts_ = disint();
#define END_DISABLE_INTERRUPT	enaint(_intsts_); }

#define ENABLE_INTERRUPT	{ enaint(0); }
#define DISABLE_INTERRUPT	{ disint(); }
#define ENABLE_INTERRUPT_UPTO(level)	{ enaint(0); }

/*
 * Task independent part (interrupt handler) nesting
 */
#define knl_taskindp	(knl_pcpu()->taskindp)	/* per processor */

Inline BOOL knl_isTaskIndependent( void )
{
	return ( knl_taskindp > 0 )? TRUE: FALSE;
}
Inline void knl_EnterTaskIndependent( void )
{
	knl_taskindp++;
}
Inline void knl_LeaveTaskIndependent( void )
{
	knl_taskindp--;
}
#define ENTER_TASK_INDEPENDENT	{ knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT	{ knl_LeaveTaskIndependent(); }

/*
 * Execution state queries
 */
#define in_indp()	( knl_isTaskIndependent() || knl_ctxtsk == NULL )

Inline UD knl_getDAIF(void)
{
	UD	daif;
	Asm("mrs %0, daif" : "=r"(daif));
	return daif;
}

#define in_ddsp()	( knl_dispatch_disabled		\
			|| in_indp() 			\
			|| (knl_getDAIF() & PSR_I) )

#define in_loc()	( (knl_getDAIF() & PSR_I)	\
			|| in_indp() )

#define in_qtsk()	( knl_ctxtsk->sysmode > knl_ctxtsk->isysmode )

/*
 * Dispatcher invocation (synchronous exception into vector.S)
 */
Inline void knl_force_dispatch( void )
{
	Asm("svc %0" :: "i"(SVC_FORCE_DISPATCH) : "memory");
}

Inline void knl_dispatch( void )
{
	Asm("svc %0" :: "i"(SVC_DISPATCH) : "memory");
}

#endif /* _SYSDEPEND_CPU_CORE_STATUS_ */
