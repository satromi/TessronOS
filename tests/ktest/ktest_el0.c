/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_el0.c
 *	Tasks at protection level 3 (EL0, system process space): system calls
 *	through the SVC gateway, user stack, task ID in TPIDRRO_EL0, and the
 *	containment of faults (a task that touches kernel memory is terminated
 *	while the kernel goes on).
 *
 *	The user side code lives in .user.text/.user.data, which the linker
 *	places at USER_TEXT_VA; it must not call anything outside that section.
 */

#include "ktest.h"
#include <ts/svc.h>
#include "sysman/space.h"
#include "sysman/pfalloc.h"

#define USER_TEXT	__attribute__((section(".user.text"), noinline, used))
#define USER_DATA	__attribute__((section(".user.data"), used))

/* ---- user side ---------------------------------------------------- */

static inline __attribute__((always_inline)) UBINT u_svc( UBINT fncd, UBINT a0, UBINT a1, UBINT a2 )
{
	register UBINT x8 __asm__("x8") = fncd;
	register UBINT x0 __asm__("x0") = a0;
	register UBINT x1 __asm__("x1") = a1;
	register UBINT x2 __asm__("x2") = a2;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "x3", "x4", "x5");
	return x0;
}

USER_DATA volatile UD	u_shared[8];

/* normal task: uses its user stack and user data, delays, signals the semaphore */
USER_TEXT void u_task_ok( INT stacd, void *exinf )
{
	volatile UD	local[4];
	INT		i;

	for ( i = 0; i < 4; i++ ) local[i] = (UD)i * 3;
	u_shared[0] = local[3] + 1;
	u_svc(TSN_TK_DLY_TSK, 10, 0, 0);
	u_shared[1] = u_svc(TSN_TK_GET_TID, 0, 0, 0);
	u_svc(TSN_TK_SIG_SEM, (UBINT)stacd, 1, 0);
	u_svc(TSN_TK_EXT_TSK, 0, 0, 0);
	while (1) ;
}

/* task ID: TPIDRRO_EL0 matches tk_get_tid; undefined function code -> E_RSFN */
USER_TEXT void u_task_tid( INT stacd, void *exinf )
{
	UBINT	tid, ro;

	tid = u_svc(TSN_TK_GET_TID, 0, 0, 0);
	__asm__ volatile("mrs %0, tpidrro_el0" : "=r"(ro));
	u_shared[2] = ( (UW)tid == (UW)ro ) ? 1 : 0;
	u_shared[3] = u_svc(TSN(0x00ff, 1), 0, 0, 0);			/* unknown class */
	u_shared[4] = u_svc(TSN_TK_MAX + 1, 0, 0, 0);			/* beyond the table */
	u_svc(TSN_TK_SIG_SEM, (UBINT)stacd, 1, 0);
	u_svc(TSN_TK_EXT_TSK, 0, 0, 0);
	while (1) ;
}

/* faulting task: exinf is a kernel address; the write must be refused */
USER_TEXT void u_task_fault( INT stacd, void *exinf )
{
	u_shared[5] = 1;
	*(volatile UD *)exinf = 0xdead;
	u_shared[5] = 2;						/* never reached */
	u_svc(TSN_TK_SIG_SEM, (UBINT)stacd, 1, 0);
	u_svc(TSN_TK_EXT_TSK, 0, 0, 0);
	while (1) ;
}

/* the user entry addresses as absolute values (the kernel cannot take them with adrp) */
__asm__(
	"	.pushsection .data\n"
	"	.balign 8\n"
	"kt_user_entries:\n"
	"	.quad u_task_ok\n"
	"	.quad u_task_tid\n"
	"	.quad u_task_fault\n"
	"	.quad u_shared\n"
	"	.popsection\n"
);
IMPORT UBINT	kt_user_entries[4];

/* ---- kernel side -------------------------------------------------- */

LOCAL ID	sem_id;

/* user data is read (only) by the kernel through the linear map of the image, which is RO there */
IMPORT UBINT	knl_user_start_va;
IMPORT UBINT	knl_user_lma;

LOCAL volatile UD *u_shared_k( void )
{
	UBINT	off = kt_user_entries[3] - knl_user_start_va;
	return (volatile UD *)PA2VA(knl_user_lma + off);
}

LOCAL ID cre_user_task( INT idx, void *exinf )
{
	T_CTSK	ctsk;

	ctsk.exinf   = exinf;
	ctsk.tskatr  = TA_HLNG | TA_RNG3;
	ctsk.task    = (FP)kt_user_entries[idx];
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;					/* system stack */
	return tk_cre_tsk(&ctsk);
}

LOCAL void test_el0_syscall( void )
{
	ID	id;
	T_RTSK	rtsk;
	volatile UD *sh = u_shared_k();

	id = cre_user_task(0, NULL);
	KT_ASSERT(id > 0);
	KT_ASSERT_ER(tk_sta_tsk(id, sem_id), E_OK);
	KT_ASSERT_ER(tk_wai_sem(sem_id, 1, 1000), E_OK);
	KT_ASSERT_EQ(sh[0], 10);				/* 3*3 + 1 computed on the user stack */
	KT_ASSERT_EQ(sh[1], id);				/* tk_get_tid through the gateway */
	KT_ASSERT_ER(tk_dly_tsk(5), E_OK);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

LOCAL void test_el0_tid_rsfn( void )
{
	ID	id;
	volatile UD *sh = u_shared_k();

	id = cre_user_task(1, NULL);
	KT_ASSERT(id > 0);
	KT_ASSERT_ER(tk_sta_tsk(id, sem_id), E_OK);
	KT_ASSERT_ER(tk_wai_sem(sem_id, 1, 1000), E_OK);
	KT_ASSERT_EQ(sh[2], 1);					/* TPIDRRO_EL0 == tk_get_tid() */
	KT_ASSERT_EQ((BINT)sh[3], E_RSFN);
	KT_ASSERT_EQ((BINT)sh[4], E_RSFN);
	KT_ASSERT_ER(tk_dly_tsk(5), E_OK);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

LOCAL void test_el0_fault_contained( void )
{
	ID	id;
	T_RTSK	rtsk;
	volatile UD *sh = u_shared_k();
	UD	before = knl_pf_free_count(-1);

	id = cre_user_task(2, (void *)&sem_id);		/* a kernel address */
	KT_ASSERT(id > 0);
	KT_ASSERT_ER(tk_sta_tsk(id, sem_id), E_OK);
	KT_ASSERT_ER(tk_wai_sem(sem_id, 1, 100), E_TMOUT);	/* never signalled */
	KT_ASSERT_EQ(sh[5], 1);					/* stopped at the faulting store */
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);			/* terminated by the fault handler */
	KT_ASSERT_EQ(sem_id, sem_id);				/* kernel data untouched */
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);		/* user stack released */
}

EXPORT void ktest_el0( void )
{
	T_CSEM	csem;

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO;
	csem.isemcnt = 0;
	csem.maxsem = 4;
	sem_id = tk_cre_sem(&csem);

	KT_RUN(test_el0_syscall);
	KT_RUN(test_el0_tid_rsfn);
	KT_RUN(test_el0_fault_contained);

	tk_del_sem(sem_id);
}
