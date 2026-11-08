/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	exc_hdl.c (x86-64)
 *	Exceptions: the faults the kernel puts right, the faults of a
 *	process, NMI, and the report of what stops the system.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <kernel.h>
#include "../../../sysdepend.h"
#include "cpu_task.h"
#include <ts/proc.h>
#include <ts/lockup.h>
#include "cpu_ufault.h"
#include "sysman/space.h"

/*
 * What is said of an exception goes to the console at once, without the
 * console's lock (tm_printf_urgent): the processor that holds that lock
 * may be this one, stopped in the middle of a line, or another that
 * will never let go of it. The screen console is brought back for it.
 */
#if USE_EXCEPTION_DBG_MSG
#define EXCEPTION_DBG_MSG(...)	tm_printf_urgent((UB*)__VA_ARGS__)
#else
#define EXCEPTION_DBG_MSG(...)
#endif

IMPORT BOOL	knl_tlb_nmi( void );		/* mmu.c */
IMPORT BOOL	knl_lockup_nmi( T_EXCFRAME *f );	/* lockup.c */
IMPORT void	knl_lockup_halted( void );
IMPORT volatile ID	knl_klock_holder;	/* smp.c: the PRC ID holding the kernel lock, 0 none */

/*
 * This processor stops. The others go on; they find it taking no
 * interrupts and say so (lockup.c).
 */
LOCAL void halt( void )
{
	disint();
	knl_lockup_halted();
	while (1) {
		Asm("hlt");
	}
}

/* A report of a stopping exception under way on each processor (T_PCPU.exc_saying): one met while saying it stops at once */
LOCAL INT exc_cpu( void )
{
	return knl_pcpu()->prcid - 1;
}

LOCAL void exc_begin( void )
{
	T_PCPU	*p = knl_pcpu();

	knl_fbcon_resume();
	if ( p != NULL ) {
		if ( p->exc_saying ) {
			tm_urgent_puts((UB*)"\n!EXCEPTION! another one while the last was being told: stopped\n");
			halt();
		}
		p->exc_saying = TRUE;
	}
}

LOCAL CONST char *exc_name( UW v )
{
	static CONST char *n[32] = {
		"#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
		"#BR bound", "#UD invalid opcode", "#NM no x87", "#DF double fault",
		"coprocessor overrun", "#TS invalid TSS", "#NP segment not present",
		"#SS stack fault", "#GP general protection", "#PF page fault", "15",
		"#MF x87 error", "#AC alignment check", "#MC machine check",
		"#XM SIMD error", "#VE virtualization", "#CP control protection",
		"22", "23", "24", "25", "26", "27", "#HV", "#VC", "#SX", "31"
	};

	return ( v < 32 ) ? n[v] : "interrupt";
}

Inline UD read_cr2( void )
{
	UD	v;

	Asm("mov %%cr2, %0" : "=r"(v));
	return v;
}

/*
 * A task wrote past the foot of its stack (kernel.h). Whatever lay below
 * the stack is gone with it, so this says which task and how large its
 * stack was, and goes no further.
 */
EXPORT void knl_stack_broken( TCB *tcb )
{
	UB	pin[160];

	disint();
	knl_fbcon_resume();
	tm_sprintf(pin, (CONST UB *)"!STACK! CPU%d: task=%d ran past the foot of its %d byte stack",
		   exc_cpu(), (tcb != NULL) ? tcb->tskid : 0, (tcb != NULL) ? (INT)tcb->sstksz : 0);
	knl_fbcon_pin(pin);
	tm_printf_urgent((UB*)"\n%s\n", pin);
	knl_lockup_say_task(" ", tcb);
	knl_bootlog_stopped("stopped: a task ran past its stack");
	halt();
}

/*
 * A task reached the unmapped pages below its stack (knl_vmap_stack).
 * The summary of it, the rest being told as for any exception
 */
EXPORT volatile UW	knl_stack_out_count = 0;	/* for the tests */
EXPORT volatile UD	knl_stack_out_addr = 0;

LOCAL void stack_out_say( UD addr, T_EXCFRAME *f )
{
	UB	pin[160];

	knl_stack_out_addr = addr;
	knl_fbcon_resume();
	tm_sprintf(pin, (CONST UB *)"!STACK! CPU%d: task=%d ran out of its %d byte stack: %lx, at RIP %lx, RSP %lx",
		   exc_cpu(), knl_ctxtsk->tskid, (INT)knl_ctxtsk->sstksz, addr, f->rip, f->rsp);
	knl_fbcon_pin(pin);
	tm_printf_urgent((UB*)"\n%s\n", pin);
	__atomic_add_fetch(&knl_stack_out_count, 1, __ATOMIC_SEQ_CST);
}

#if CNF_STK_WATCH
IMPORT TCB	knl_tcb_table[];

EXPORT void knl_stack_overwritten( TCB *tcb, UD *at, UD was, UB prc, UD since )
{
	UD	now = ReadSysCounterSync(), *p;
	INT	i;

	disint();
	if ( knl_num_prc > 1 ) {
		knl_send_ipi_others(SGI_STOP);
	}
	tm_printf_urgent((UB*)"\n!STKWATCH! CPU%d: task=%d: the return address at %lx was %lx"
			 " when CPU%d gave its context up %d us ago, and is %lx\n",
			 exc_cpu(), tcb->tskid, (UD)(UBINT)at, was, prc,
			 (INT)( ( now - since ) * 1000000 / ( knl_cntfrq != 0 ? knl_cntfrq : 1 ) ), *at);
	knl_lockup_say_task(" ", tcb);
	for ( p = at - 8; p < at + 8; p += 2 ) {
		tm_printf_urgent((UB*)"  %lx: %016lx %016lx\n", (UD)(UBINT)p, p[0], p[1]);
	}
	for ( i = 0; i < NUM_TSKID; i++ ) {
		TCB	*t = &knl_tcb_table[i];
		UBINT	w = (UBINT)t->wercd;

		if ( t->state != 0 && w != 0
		  && w + 0x200 > (UBINT)at && w < (UBINT)at + 0x200 ) {
			tm_printf_urgent((UB*)"  task %d state %d wid %d: its wait result goes to %lx\n",
					 t->tskid, t->state, t->wid, (UD)w);
		}
	}
	for ( i = 0; i < knl_num_prc; i++ ) {
		TCB	*t = knl_pcpu_of(i)->ctxtsk;

		tm_printf_urgent((UB*)"  CPU%d runs task %d\n", i, ( t != NULL ) ? t->tskid : 0);
	}
	halt();
}
#endif

/*
 * The machine stopped after the records were written (bootlog.c), when
 * an exception in a task let the other processors go on to write them.
 * The system timer and the devices' interrupts are the first processor's
 * (sys_timer.h): halted at once, it would take them with it, and no
 * record would be written. So a task that met an exception in the kernel
 * with interrupts open, the dispatcher free and the kernel lock not its
 * own is put to sleep for good instead, the report kept on the screen
 * and in the console; the bootlog task writes the records and then calls
 * knl_exc_stop_all, which stops every processor and shows the report
 * again. When the records do not get written in EXC_WRITE_MS (the disk
 * does not answer), the task that met the exception stops the machine
 * itself.
 */
#define EXC_WRITE_MS	15000
#define RFL_IF_BIT	0x200

LOCAL volatile BOOL	exc_pending = FALSE;	/* an exception waits for the records before the stop */
LOCAL volatile UW	exc_stopping = 0;

EXPORT void knl_exc_stop_all( void )
{
	UINT	s;

	if ( !exc_pending ) {
		return;
	}
	if ( __atomic_exchange_n(&exc_stopping, 1, __ATOMIC_SEQ_CST) != 0 ) {
		halt();
	}
	disint();
	if ( knl_num_prc > 1 ) {
		knl_send_ipi_others(SGI_STOP);
	}
	knl_fbcon_repaint();
	s = tm_urgent_begin();
	tm_printf_urgent((UB*)"!STOPPED! the machine stops after the exception kept above\n");
	tm_urgent_end(s);
	halt();
}

/* Whether the task that met the exception may sleep for good, the machine going on until the records are written */
LOCAL BOOL exc_may_sleep( T_EXCFRAME *f )
{
	return knl_ctxtsk != NULL && knl_taskindp == 0 && ( f->rflags & RFL_IF_BIT ) != 0
	    && !knl_dispatch_disabled && knl_klock_holder != knl_pcpu()->prcid
	    && f->vec != 2 && f->vec != 8 && f->vec != 18;	/* not an NMI, a double fault, a machine check */
}

/*
 * An exception that stops the system: what it was and where, the
 * registers, the task, where it was called from
 */
LOCAL void exc_report( T_EXCFRAME *f, UD cr2 )
{
	UINT	s;

	UB	pin[160];

	disint();
	knl_fbcon_early_exc((UW)f->vec, f->rip, cr2, f->err);	/* before the console: drawn, and kept */
	exc_begin();
	/* the summary, kept under the band whatever is told after it */
	tm_sprintf(pin, (CONST UB *)"!EXCEPTION! CPU%d %s, RIP %016lx, CR2 %lx, error %lx, task %d",
		   exc_cpu(), exc_name((UW)f->vec), f->rip, cr2, f->err,
		   (knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0);
	knl_fbcon_pin(pin);
	tm_sprintf(pin, (CONST UB *)"  RSP %016lx, RBP %016lx, CS %04lx, RFLAGS %lx, the step: %s",
		   f->rsp, f->rbp, f->cs, f->rflags, knl_fbcon_last_stage());
	knl_fbcon_pin(pin);
	s = tm_urgent_begin();
	EXCEPTION_DBG_MSG("\n!EXCEPTION! CPU%d %s (vector %d) error %lx at %04lx:%016lx RFLAGS=%lx CR2=%lx\n",
		exc_cpu(), exc_name((UW)f->vec), (INT)f->vec, f->err, f->cs, f->rip, f->rflags, cr2);
	EXCEPTION_DBG_MSG(" task=%d indp=%d; the last step of the start-up: %s\n",
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0, knl_taskindp, knl_fbcon_last_stage());
	knl_lockup_say_task(" ", knl_ctxtsk);
	EXCEPTION_DBG_MSG(" rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
	EXCEPTION_DBG_MSG(" rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx\n", f->rsi, f->rdi, f->rbp, f->rsp);
	EXCEPTION_DBG_MSG(" r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
	EXCEPTION_DBG_MSG(" r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
	if ( ( f->cs & SEL_RPL3 ) == 0 ) {
		knl_lockup_say_frames(" ", (UBINT)f->rbp);	/* taken in the kernel: where it was called from */
	}
	if ( exc_may_sleep(f) && !exc_pending ) {
		EXCEPTION_DBG_MSG(" the task sleeps for good; the records are written, then the machine stops\n");
		tm_urgent_end(s);
		knl_fbcon_tail();
		exc_pending = TRUE;
		knl_bootlog_stopped("met an exception");
		knl_pcpu()->exc_saying = FALSE;
		enaint(0);
		(void)tk_slp_tsk(EXC_WRITE_MS);
		knl_exc_stop_all();		/* the records not written in time */
		halt();
	}
	tm_urgent_end(s);
	knl_fbcon_tail();
	knl_bootlog_stopped("stopped by an exception");	/* written by the other processors, if they can */
	halt();
}

/*
 * The ARM name of this function is kept for the common parts that call
 * it: a stopping exception, given its frame
 */
EXPORT void knl_exception_handler( void *frame, UD esr, UD far )
{
	(void)esr;
	exc_report((T_EXCFRAME *)frame, far);
}

/*
 * A page fault taken in the kernel. An access refused by SMAP to a page
 * a process may reach, met while the kernel works for it in a call, is
 * recorded, and the call goes on to its end with the way open (it would
 * leave a volume, a store or a lock half done otherwise); the process
 * ends as the call returns (knl_prc_kpan). An access to the running
 * process's half that is not mapped, or not to be written, is the
 * process's doing: the address is given a page of nothing and the access
 * is made again, and the process ends as the call returns
 * (knl_prc_kfault, design 9.11). A fault in the device window maps its
 * gigabyte. Anything else stops the system. Returns only when put
 * right.
 */
#define USER_HALF_END	U64(0x0000800000000000)

/* The last access SMAP refused, for the tests */
EXPORT UD	knl_pan_last_far = 0;
EXPORT UD	knl_pan_last_elr = 0;

LOCAL BOOL pan_refused( T_EXCFRAME *f, UD far )
{
	UD	cr3, ent;

	if ( !knl_pan_on || ( f->err & ( PFERR_P | PFERR_U | PFERR_I ) ) != PFERR_P
	  || ( f->rflags & RFL_AC ) != 0 || far >= USER_HALF_END ) {
		return FALSE;
	}
	Asm("mov %%cr3, %0" : "=r"(cr3));
	ent = knl_pt_entry(cr3 & MMU_ROOT_PA, (UBINT)far & ~(UBINT)0xfff);
	return ( ent != 0 && ( ent & PTE_US ) != 0 );
}

LOCAL BOOL pan_fault( T_EXCFRAME *f, UD far )
{
	TCB	*t = knl_ctxtsk;
	UD	esr = ( (UD)f->vec << 32 ) | f->err;

	knl_pan_faults++;
	knl_pan_last_far = far;
	knl_pan_last_elr = f->rip;
	tm_printf((UB*)"\n!PAN! the kernel touched a user page without making sure of it:"
		  " CR2=%lx RIP=%lx task=%d indp=%d\n",
		  far, f->rip, ( t != NULL ) ? t->tskid : 0, knl_taskindp);
	if ( t == NULL || knl_taskindp > 0 ) {
		return FALSE;
	}
	(void)knl_prc_kpan(t->tskctxb.ttbr0, t->tskid, (UBINT)far, esr, f->rip, f->rbp);
	f->rflags |= RFL_AC;		/* the way open until the call is over */
	return TRUE;
}

EXPORT void knl_kernel_fault( void *frame, UD esr, UD far )
{
	T_EXCFRAME	*f = (T_EXCFRAME *)frame;

	if ( f->vec == VEC_PF ) {
		if ( knl_stack_guard_hit(knl_ctxtsk, (UBINT)far) ) {
			stack_out_say(far, f);
			exc_report(f, far);
		}
		if ( knl_dev_fault((UBINT)far) ) {
			return;
		}
		if ( pan_refused(f, far) ) {
			if ( pan_fault(f, far) ) {
				return;
			}
			exc_report(f, far);
		}
		if ( ( f->err & ( PFERR_RSVD | PFERR_I ) ) == 0 && far < USER_HALF_END
		  && knl_ctxtsk != NULL && knl_taskindp == 0
		  && knl_prc_kfault(knl_ctxtsk->tskctxb.ttbr0, knl_ctxtsk->tskid, (UBINT)far,
				    esr, f->rip, f->rbp) ) {
			return;
		}
	}
	exc_report(f, far);
}

/*
 * Fault of a task in user mode: report and terminate the task; the
 * kernel and the other tasks go on. Runs on the task's system stack with
 * the frame at 'frame'. An invalid opcode ends the process with
 * TS_ABORT_ILL, every other exception with TS_ABORT_FAULT; what
 * happened is recorded with the process's end for the desktop
 * (knl_prc_abort).
 *
 * A page fault the process's memory can put right first goes to
 * knl_prc_ufault, as a call of the task would (interrupts enabled, the
 * task marked as inside the kernel); then this returns, and the access
 * is made again. A data fault it does not put right goes to the
 * program's entry for faults when it named one (knl_prc_flt_give).
 */
IMPORT BOOL knl_prc_ufault( UBINT va, UD esr );

EXPORT void knl_el0_fault( void *frame, UD esr, UD far )
{
	T_EXCFRAME	*f = (T_EXCFRAME *)frame;
	T_PRCFLT	info;
	TCB		*me = knl_ctxtsk;
	BOOL		fixed;

	if ( f->vec == VEC_PF && me != NULL && knl_taskindp == 0 ) {
		me->svcdepth++;
		enaint(0);
		fixed = knl_prc_ufault((UBINT)far, esr);
		if ( !fixed ) {
			/* the program's own entry for it (ts_def_flt) takes it */
			fixed = knl_prc_flt_give(frame, esr, far);
		}
		disint();
		me->svcdepth--;
		if ( fixed ) {
			return;
		}
	}

	EXCEPTION_DBG_MSG("\n!EL0 fault! task=%d %s (vector %d) error %lx CR2=%lx RIP=%lx RSP=%lx\n",
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0,
		exc_name((UW)f->vec), (INT)f->vec, f->err, far, f->rip, f->rsp);
	if ( knl_ctxtsk == NULL || knl_taskindp > 0 ) {
		halt();
	}
	enaint(0);		/* tk_ext_tsk is a task-context call: run it with interrupts enabled */
	{
		/* the words on top of its stack: after a call or a return through
		   a bad pointer, where it came from */
		UD	l0 = knl_ctxtsk->tskctxb.ttbr0 & PTE_ADDR_MASK, w[4];
		UBINT	sp = (UBINT)f->rsp;

		if ( sp >= USER_TEXT_VA && sp + sizeof(w) <= USER_STACK_TOP
		  && knl_pt_lookup(l0, sp & PAGE_MASK) != ~(UD)0
		  && knl_pt_lookup(l0, ( sp + sizeof(w) - 1 ) & PAGE_MASK) != ~(UD)0 ) {
			knl_ua_get(w, (CONST void *)sp, sizeof(w));
			EXCEPTION_DBG_MSG(" stack %lx %lx %lx %lx\n", w[0], w[1], w[2], w[3]);
		}
	}
	knl_memset(&info, 0, sizeof(info));
	info.kind = TS_FLT_EL0;
	info.tskid = knl_ctxtsk->tskid;
	info.esr = esr;
	info.far = far;
	info.elr = f->rip;
	info.lr = f->rbp;
	/* a task of a process takes the whole process down (design 9.11) */
	knl_prc_abort(knl_ctxtsk->tskctxb.ttbr0,
		( f->vec == 6 ) ? TS_ABORT_ILL : TS_ABORT_FAULT, &info);
	tk_ext_tsk();		/* no return */
	halt();
}

/*
 * Every exception (vector.S): its vector and error code make the
 * "syndrome" the common parts are handed (cpu_ufault.h), CR2 the address
 * met
 */
EXPORT void knl_exception_entry( T_EXCFRAME *f )
{
	UD	far = ( f->vec == VEC_PF ) ? read_cr2() : 0;
	UD	esr = ( (UD)f->vec << 32 ) | ( f->err & 0xffffffffULL );

	if ( ( f->cs & SEL_RPL3 ) != 0 ) {
		knl_el0_fault(f, esr, far);
	} else {
		knl_kernel_fault(f, esr, far);
	}
}

/*
 * NMI, the double fault, the machine check (vector.S, on their own
 * stacks): an NMI is a request of another processor's (the translations
 * to drop, where this processor is) or the hardware's; the other two
 * stop the system
 */
EXPORT void knl_nmi_entry( T_EXCFRAME *f )
{
	if ( f->vec == 2 ) {
		BOOL	mine = knl_tlb_nmi();

		mine = knl_lockup_nmi(f) || mine;
		if ( mine ) {
			return;
		}
		tm_printf_urgent((UB*)"\n!NMI! CPU%d at %lx (none was asked for)\n", exc_cpu(), f->rip);
		return;
	}
	/*
	 * The usual way a task runs out of its stack: the processor could
	 * not push the page fault's frame either, and came here on a stack
	 * of its own, with CR2 still saying where
	 */
	if ( f->vec == 8 && knl_stack_guard_hit(knl_ctxtsk, (UBINT)read_cr2()) ) {
		stack_out_say(read_cr2(), f);
	}
	exc_report(f, read_cr2());
}

/*
 * A vector that has no handler (vector.S). A device can raise its
 * interrupt before its driver defines the handler: a message enabled
 * while the function still holds an interrupt from the firmware's use
 * of it arrives at once. Such an interrupt is ended at the Local APIC,
 * counted and dropped, and the processor goes on; stopping here would
 * take the processor that serves the devices' interrupts with it. An
 * IOAPIC input is masked as well, since a level nobody clears would
 * come again at once; its driver unmasks it with EnableInt. The first
 * one of a vector and each tenfold are said on the console.
 */
IMPORT BOOL	knl_int_is_dev( UINT intno );	/* apic.c */
IMPORT BOOL	knl_int_is_msi( UINT intno );

LOCAL volatile UW	int_unhandled[N_INTVEC];

/* How many interrupts of intno arrived with no handler */
EXPORT UW knl_int_unhandled( UINT intno )
{
	return ( intno < N_INTVEC ) ? int_unhandled[intno] : 0;
}

WEAK_FUNC EXPORT void Default_Handler( UINT intno )
{
	UW	n, m;
	BOOL	line;

	knl_apic_eoi();
	if ( intno == INTVEC_APIC_ERROR || intno >= N_INTVEC ) {
		return;
	}
	n = __atomic_add_fetch(&int_unhandled[intno], 1, __ATOMIC_RELAXED);
	line = knl_int_is_dev(intno) && !knl_int_is_msi(intno);
	if ( line ) {
		DisableInt(intno);
	}
	for ( m = 1; m < n && m < 1000000000U; m *= 10 ) ;
	if ( m == n ) {
		tm_printf((UB *)"TessronOS: interrupt %d on CPU%d with no handler: ended and dropped%s (%u so far)\n",
			  intno, exc_cpu(), line ? ", its IOAPIC input masked" : "", n);
	}
}

#endif	/* CPU_CORE_X86_64 */
