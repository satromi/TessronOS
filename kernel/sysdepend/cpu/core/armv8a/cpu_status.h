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
 *
 *	Whether to dispatch is decided before the kernel lock is let go. A
 *	task that the section put into a wait then always goes through the
 *	dispatcher, which takes the lock again before it lets the task run
 *	on (knl_dispatch_pick): a processor that ends the wait meanwhile has
 *	by then written all it gives the task -- the wait's result, a
 *	message, a block. Decided after letting go, the task could find
 *	itself made ready again by that processor half way through, run on
 *	without the dispatcher and read its result before it was written
 *	(E_TMOUT from a wait without a limit), while the result went into
 *	its stack afterwards, over whatever the stack held by then
 *	(design 4.3).
 */
#if USE_KTEST
IMPORT volatile UW	knl_kt_unlock_spin;
IMPORT volatile UW	knl_kt_release_spin;
IMPORT volatile UW	knl_kt_exit_spin;
#define KT_SPIN(n)	{ UW _k_ = (n); while ( _k_ != 0 ) { _k_--; Asm("nop"); } }
#else
#define KT_SPIN(n)
#endif

#if KLK_GIANT
#define BEGIN_CRITICAL_SECTION	{ UINT _intsts_ = knl_lock_kernel(); KLK_ENTER_DECL
#define END_CRITICAL_SECTION	{ BOOL _dsp_ = ( !isDI(_intsts_)	\
				  && knl_ctxtsk != knl_schedtsk		\
				  && !knl_isTaskIndependent()		\
				  && !knl_dispatch_disabled );		\
				KLK_LEAVE_CHECK				\
				knl_unlock_kernel_keep();		\
				KT_SPIN(knl_kt_unlock_spin)		\
				if ( _dsp_ ) {				\
					knl_dispatch();			\
				}					\
				enaint(_intsts_); } }
#else
/* Without the big lock: the locks of what is touched (klk.h, design 4.3.3) */
#define BEGIN_CRITICAL_SECTION	KLK_BEGIN_CS
#define END_CRITICAL_SECTION	KLK_END_CS
#endif

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
 *	A task runs with interrupts enabled, and can be moved to another
 *	processor between reading TPIDR_EL1 (this processor's element) and
 *	reading a field of it: it would read the other processor's task,
 *	none, or the nesting of a handler there. The fields are read with
 *	IRQ masked, so that they are of the processor that reads them.
 */
Inline BOOL knl_in_indp( void )
{
	UD	daif;
	BOOL	r;

	Asm("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) :: "memory");
	r = ( knl_isTaskIndependent() || knl_pcpu()->ctxtsk == NULL );
	Asm("msr daif, %0" :: "r"(daif) : "memory");
	return r;
}
#define in_indp()	knl_in_indp()

Inline UD knl_getDAIF(void)
{
	UD	daif;
	Asm("mrs %0, daif" : "=r"(daif));
	return daif;
}

Inline BOOL knl_in_ddsp( void )
{
	UD	daif;
	BOOL	r;

	Asm("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) :: "memory");
	r = ( knl_pcpu()->dispatch_disabled || knl_isTaskIndependent()
	      || knl_pcpu()->ctxtsk == NULL || ( daif & PSR_I ) != 0 );
	Asm("msr daif, %0" :: "r"(daif) : "memory");
	return r;
}
#define in_ddsp()	knl_in_ddsp()

Inline BOOL knl_int_masked( void )
{
	return ( knl_getDAIF() & PSR_I ) != 0;
}

#define in_loc()	( (knl_getDAIF() & PSR_I)	\
			|| in_indp() )

#define in_qtsk()	( knl_self_ctxtsk()->sysmode > knl_self_ctxtsk()->isysmode )

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
