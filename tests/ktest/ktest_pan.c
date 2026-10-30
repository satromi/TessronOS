/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_pan.c
 *	The border between the processes and the kernel (docs/tessronos/
 *	04-kernel.md 4.6.1, 05-process.md, A-svc.md, 権限の表):
 *	- PAN is on where the processor has it, set on every processor and
 *	  by every exception taken to EL1;
 *	- the kernel reaches a page EL0 may reach only with the way open:
 *	  read straight it is refused, recorded, and the read made with the
 *	  way open for the rest of what was under way; a process whose call
 *	  the kernel served so ends with the fault recorded as TS_FLT_KPAN;
 *	- what a process hands and is answered -- the start-up argument, a
 *	  message buffer of its own, the time, a file's state, a line on the
 *	  console -- goes in and out under PAN;
 *	- a task of protection level 2/3 runs only at EL0: without a user
 *	  stack it is neither made nor started, and tk_set_reg does not take
 *	  it to EL1;
 *	- the calls that act on the whole system take the rights the table
 *	  of rights gives them: a user who is not an administrator is
 *	  refused them (E_OACV) unless the settings object they act on is
 *	  his to write, and the administrator is let make them all.
 */

#include "ktest.h"
#include "kernel.h"
#include "tstdlib.h"
#if defined(__x86_64__)
#include "sysdepend/cpu/core/x86_64/cpu_task.h"
#else
#include "sysdepend/cpu/core/armv8a/cpu_task.h"
#endif
#include "sysman/space.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/sysdef.h>

IMPORT UD	knl_pan_last_far;
IMPORT BOOL	knl_ustack_refuse;

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_PAN		30		/* as tests/uprog/svcprog.c */
#define SP_RIGHTS	31
#define PAN_WORD	0x50414E2D6B746573ULL

#define USER_DATA	__attribute__((section(".user.data"), used))

/* ---- a word of the system process's, at EL0's addresses -------------- */

USER_DATA volatile UD	u_pan_word = PAN_WORD;

/* its address as linked, which is where the system process sees it */
__asm__(
	"	.pushsection .data\n"
	"	.balign 8\n"
	"kt_pan_user_word:\n"
	"	.quad u_pan_word\n"
	"	.popsection\n"
);
IMPORT UBINT	kt_pan_user_word;

/* ---------------------------------------------------------------- PAN on */

#if defined(__x86_64__)
/* What the processor says it has: SMAP, which is PAN on x86-64 (CPUID 7, EBX bit 20) */
LOCAL BOOL cpu_has_pan( void )
{
	UW	a, b, c, d;

	cpuid(7, 0, &a, &b, &c, &d);
	return ( b & ( 1U << 20 ) ) != 0;
}
#else
/* What the processor says it has: PAN from ARMv8.1 */
LOCAL BOOL cpu_has_pan( void )
{
	UD	mmfr1;

	Asm("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
	return ( ( ( mmfr1 >> ID_MMFR1_PAN_SHIFT ) & 0xf ) != 0 );
}
#endif

/*
 * PAN is on where the processor has it: SCTLR_EL1.SPAN clear, PSTATE.PAN
 * set in a task of the kernel's, opened and shut again by the way's
 * calls, and nested openings shut only by the outermost.
 */
LOCAL void test_pan_on( void )
{
	UD	sctlr;
	BOOL	o1, o2;

	tm_printf((UB*)"  PAN: the processor %s it, the kernel %s it\n",
		  cpu_has_pan() ? "has" : "has not", knl_pan_on ? "uses" : "does not use");
	KT_ASSERT_EQ(knl_pan_on, cpu_has_pan());
	if ( !knl_pan_on ) {
		KT_ASSERT(knl_ua_is_open());		/* nothing shut without it */
		KT_SKIP("no PAN on this processor");
	}
#if defined(__x86_64__)
	Asm("mov %%cr4, %0" : "=r"(sctlr));
	KT_ASSERT((sctlr & ( 1UL << 21 )) != 0);	/* CR4.SMAP */
#else
	Asm("mrs %0, sctlr_el1" : "=r"(sctlr));
	KT_ASSERT_EQ(sctlr & SCTLR_SPAN, 0);
#endif
	KT_ASSERT(!knl_ua_is_open());
	o1 = knl_ua_open();
	KT_ASSERT(o1);
	KT_ASSERT(knl_ua_is_open());
	o2 = knl_ua_open();
	KT_ASSERT(!o2);					/* already open */
	knl_ua_close(o2);
	KT_ASSERT(knl_ua_is_open());
	knl_ua_close(o1);
	KT_ASSERT(!knl_ua_is_open());
	tk_dly_tsk(2);					/* an interrupt and a switch between */
	KT_ASSERT(!knl_ua_is_open());
}

/*
 * A task of the kernel's on the system process's space: the word read
 * with the way open is read, and nothing is refused; read straight, PAN
 * refuses it, says so and lets the read go on with the way open.
 */
LOCAL volatile UD	probe_open, probe_straight;
LOCAL volatile BOOL	probe_shut, probe_left_open;
LOCAL ID		probe_sem;

LOCAL void probe_task( INT stacd, void *exinf )
{
	CONST volatile UD *p = (CONST volatile UD *)kt_pan_user_word;

	probe_open = UACCESS(*p);
	probe_shut = !knl_ua_is_open();
	probe_straight = *p;				/* refused, recorded, made again */
	probe_left_open = knl_ua_is_open();
	tk_sig_sem(probe_sem, 1);
	tk_ext_tsk();
}

LOCAL void test_pan_kernel_refused( void )
{
	T_CSEM	cs;
	T_CTSK	ct;
	ID	tid;
	UW	before = knl_pan_faults;

	if ( !knl_pan_on ) KT_SKIP("no PAN on this processor");
	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 1;
	probe_sem = tk_cre_sem(&cs);
	KT_ASSERT(probe_sem > 0);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)probe_task;
	ct.itskpri = KT_PRI_HIGH;
	ct.stksz = 4096;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) return;
	KT_ASSERT_ER(knl_tsk_set_space(tid, knl_sysprc_ttbr0), E_OK);
	probe_open = probe_straight = 0;
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	KT_ASSERT_ER(tk_wai_sem(probe_sem, 1, 2000), E_OK);
	KT_ASSERT_EQ(probe_open, PAN_WORD);
	KT_ASSERT(probe_shut);
	KT_ASSERT_EQ(probe_straight, PAN_WORD);
	KT_ASSERT(probe_left_open);
	KT_ASSERT_EQ(knl_pan_faults, before + 1);
	KT_ASSERT_EQ(knl_pan_last_far, kt_pan_user_word);
	tk_dly_tsk(5);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
	tk_del_sem(probe_sem);
}

/* ---------------------------------------------------------------- a process */

LOCAL BOOL have_svcprog( void )
{
	T_FSTAT	st;

	return ( fs_stat(SVCPROG, &st) >= EX_OK );
}

/* svcprog run with what to do and its words; its exit code, or below -1000 when it did not run */
LOCAL INT run_svcprog( UW what, CONST TS_UUID *u, UW w5 )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg[8];
	ID	pid;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = what;
	if ( u != NULL ) knl_memcpy(&arg[1], u, sizeof(TS_UUID));
	arg[5] = w5;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) return -1000 + pid;
	psts.exitcd = -1;
	if ( ts_wai_prc(pid, &psts, 10000) < E_OK ) return -2000;
	if ( psts.exitcd != 0 && psts.exitcd != TS_ABORT_FAULT ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	return psts.exitcd;
}

/*
 * A process under PAN: the kernel reads what it was handed once it has
 * made sure of it; the start-up argument, a message buffer of its own,
 * the time, a file's state and a line on the console go in and out with
 * nothing refused. Then the kernel reads its word straight: PAN refuses
 * it, and the process ends as the call returns, recorded as TS_FLT_KPAN
 * at the word's address.
 */
LOCAL void test_pan_process( void )
{
	T_PRCFLT	f;
	UW		lost = 0, before;
	INT		code;
	BOOL		seen = FALSE;

	if ( !have_svcprog() ) KT_SKIP("no " SVCPROG);
	while ( knl_prc_fault_take(&f, &lost) ) ;	/* what earlier tests left */
	before = knl_pan_faults;
	code = run_svcprog(SP_PAN, NULL, 0);
	if ( !knl_pan_on ) {
		KT_ASSERT_EQ(code, 0);			/* nothing refused it */
		KT_ASSERT_EQ(knl_pan_faults, before);
		return;
	}
	KT_ASSERT_EQ(code, TS_ABORT_FAULT);
	KT_ASSERT_EQ(knl_pan_faults, before + 1);	/* the straight read alone */
	while ( knl_prc_fault_take(&f, &lost) ) {
		if ( f.kind == TS_FLT_KPAN ) {
			seen = TRUE;
			KT_ASSERT_EQ(f.exitcd, TS_ABORT_FAULT);
			KT_ASSERT_EQ(f.far, knl_pan_last_far);
			KT_ASSERT(f.far < 0x0001000000000000ULL);
		}
	}
	KT_ASSERT(seen);
}

/* ---------------------------------------------------------------- level 2/3 at EL0 only */

LOCAL void el0_task( INT stacd, void *exinf )
{
	for ( ;; ) ;
}

/*
 * A task of protection level 3 is not made without a user stack
 * (E_NOMEM), nor started should it ever have none (E_SYS); its frame
 * returns to EL0t, and tk_set_reg cannot make it EL1h. A task of level 0
 * starts with PAN set.
 */
LOCAL void test_el0_only( void )
{
	T_CTSK		ct;
	T_EIT		eit;
	T_RTSK		rt;
	TCB		*tcb;
	SStackFrame	*ssp;
	void		*ustk;
	ID		tid;

	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG3;
	ct.task = (FP)el0_task;
	ct.itskpri = KT_PRI_LOW;
	ct.stksz = 4096;

	/* no memory for its user stack: not made */
	knl_ustack_refuse = TRUE;
	KT_ASSERT_ER(tk_cre_tsk(&ct), E_NOMEM);
	KT_ASSERT(!knl_ustack_refuse);

	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) return;
	tcb = knl_tsk_tcb(tid);
	ssp = (SStackFrame *)tcb->tskctxb.ssp;
	KT_ASSERT(tcb->tskctxb.ustk != NULL);
#if defined(__x86_64__)
	KT_ASSERT_EQ(ssp->cs & SEL_RPL3, SEL_RPL3);

	/* the level of its frame stays whatever is asked: user mode, interrupts on */
	knl_memset(&eit, 0, sizeof(eit));
	eit.pc = (void *)el0_task;
	eit.rflags = RFL_ZF | RFL_AC | RFL_IOPL;
	KT_ASSERT_ER(tk_set_reg(tid, NULL, &eit, NULL), E_OK);
	KT_ASSERT_EQ(ssp->cs & SEL_RPL3, SEL_RPL3);
	KT_ASSERT_EQ(ssp->rflags & ( RFL_AC | RFL_IOPL ), 0);
	KT_ASSERT_EQ(ssp->rflags & ( RFL_ZF | RFL_IF ), RFL_ZF | RFL_IF);
#else
	KT_ASSERT_EQ(ssp->spsr & PSR_M_MASK, PSR_M_EL0t);

	/* the level of its frame stays whatever is asked */
	knl_memset(&eit, 0, sizeof(eit));
	eit.pc = (void *)el0_task;
	eit.spsr = PSR_M_EL1h | PSR_Z;
	KT_ASSERT_ER(tk_set_reg(tid, NULL, &eit, NULL), E_OK);
	KT_ASSERT_EQ(ssp->spsr & PSR_M_MASK, PSR_M_EL0t);
	KT_ASSERT_EQ(ssp->spsr & ( PSR_PAN | PSR_DAIF ), 0);
	KT_ASSERT_EQ(ssp->spsr & PSR_Z, PSR_Z);
#endif

	/* its user stack gone: not started */
	ustk = tcb->tskctxb.ustk;
	tcb->tskctxb.ustk = NULL;
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_SYS);
	tcb->tskctxb.ustk = ustk;
	KT_ASSERT_ER(tk_ref_tsk(tid, &rt), E_OK);
	KT_ASSERT_EQ(rt.tskstat, TTS_DMT);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);

	/* a task of level 0: EL1h, with PAN set where it is in use */
	ct.tskatr = TA_HLNG | TA_RNG0;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) return;
	tcb = knl_tsk_tcb(tid);
	ssp = (SStackFrame *)tcb->tskctxb.ssp;
#if defined(__x86_64__)
	/* the kernel's mode, the way to the user pages shut (AC clear) */
	KT_ASSERT_EQ(ssp->cs & SEL_RPL3, 0);
	KT_ASSERT_EQ(ssp->rflags & RFL_AC, 0);
	eit.rflags = RFL_AC;				/* the way opened in what is given */
	KT_ASSERT_ER(tk_set_reg(tid, NULL, &eit, NULL), E_OK);
	KT_ASSERT_EQ(ssp->rflags & RFL_AC, 0);
#else
	KT_ASSERT_EQ(ssp->spsr & PSR_M_MASK, PSR_M_EL1h);
	KT_ASSERT_EQ(( ssp->spsr & PSR_PAN ) != 0, knl_pan_on);
	eit.spsr = PSR_M_EL1h;				/* PAN cleared in what is given */
	KT_ASSERT_ER(tk_set_reg(tid, NULL, &eit, NULL), E_OK);
	KT_ASSERT_EQ(( ssp->spsr & PSR_PAN ) != 0, knl_pan_on);
#endif
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
}

/* ---------------------------------------------------------------- rights */

/* The protection of the object named by s with ops given to the user u, or put back to *was */
LOCAL ER grant( CONST char *s, CONST TS_UUID *u, UINT ops, T_OBPRT *was, BOOL back )
{
	TS_UUID	o;
	T_OBPRT	*p;
	ER	er;
	INT	n;

	if ( ts_str_to_uuid(s, &o) < E_OK ) return E_SYS;
	if ( back ) return ob_set_prt(&o, was);
	er = ob_get_prt(&o, was, KT_PRT_SIZE, NULL);
	if ( er < E_OK ) return er;
	/* an entry more, the limits kept after the entries */
	n = was->nacl;
	p = knl_ob_prt_room(knl_ob_prt_dup(was), n + 1, was->nrmask);
	if ( p == NULL ) return E_NOMEM;
	p->acl[n].kind = OB_ACE_USER;
	p->acl[n].who = *u;
	p->acl[n].ops = ops;
	er = ob_set_prt(&o, p);
	Kfree(p);
	return er;
}

/*
 * The table of rights: a user who is not an administrator is refused
 * the time, the zone, the key layouts, the look of the windows, the
 * network interface and the volumes; given the calendar's attributes to
 * write, he may set the zone, and given the machine's settings to
 * write, the key layouts, and nothing more; the administrator may make
 * them all.
 */
LOCAL void test_rights( void )
{
	CONST char	*uj = "{\"name\":\"ktrights\",\"tessronos\":{\"user\":{\"name\":\"ktrights\",\"groups\":[]}}}";
	T_OBCRE		c;
	T_OBPRT		*cal, *dev;
	TS_UUID		user;
	ER		e1, e2;

	if ( !have_svcprog() ) KT_SKIP("no " SVCPROG);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)uj;
	c.jsonsz = (SZ)knl_strlen(uj);
	if ( ob_cre_obj(&c, &user) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&user, (CONST UB *)"ktest-pw"), E_OK);

	/* nothing given: every one refused */
	KT_ASSERT_EQ(run_svcprog(SP_RIGHTS, &user, 0), 0);

	/* the calendar's attributes and the machine's settings his to write */
	cal = (T_OBPRT *)Kmalloc(KT_PRT_SIZE);
	dev = (T_OBPRT *)Kmalloc(KT_PRT_SIZE);
	KT_ASSERT(cal != NULL && dev != NULL);
	if ( cal != NULL && dev != NULL ) {
		e1 = grant(SYSDEF_CALENDAR, &user, OB_OP_ATRRD | OB_OP_ATRWR, cal, FALSE);
		e2 = grant(SYSDEF_CONF_DEV, &user, OB_OP_READ | OB_OP_WRITE, dev, FALSE);
		KT_ASSERT_ER(e1, E_OK);
		KT_ASSERT_ER(e2, E_OK);
		if ( e1 >= E_OK && e2 >= E_OK ) {
			KT_ASSERT_EQ(run_svcprog(SP_RIGHTS, &user, 3), 0);
		}
		if ( e1 >= E_OK ) KT_ASSERT_ER(grant(SYSDEF_CALENDAR, &user, 0, cal, TRUE), E_OK);
		if ( e2 >= E_OK ) KT_ASSERT_ER(grant(SYSDEF_CONF_DEV, &user, 0, dev, TRUE), E_OK);
	}
	if ( cal != NULL ) Kfree(cal);
	if ( dev != NULL ) Kfree(dev);

	/* the administrator: all of them */
	KT_ASSERT_EQ(run_svcprog(SP_RIGHTS, NULL, 0), 0);

	KT_ASSERT_ER(ob_del_obj(&user), E_OK);
}

EXPORT void ktest_pan( void )
{
	KT_RUN(test_pan_on);
	KT_RUN(test_pan_kernel_refused);
	KT_RUN(test_pan_process);
	KT_RUN(test_el0_only);
	KT_RUN(test_rights);
}
