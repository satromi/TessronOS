/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_stkg.c
 *	The stacks of the kernel's tasks (docs/tessronos/04-kernel.md 4.7.7):
 *	- a stack lies in the variable area, fills its pages, and has
 *	  TASK_STK_GUARD_PAGES unmapped pages under it;
 *	- it can be used to its foot, and a task's stack comes back for the
 *	  next task of the size, the addresses not running out;
 *	- running out of it stops the processor with the task named and the
 *	  address met, whether the stack ran out while pushing (no room for
 *	  the exception's frame either) or a store went below a stack that
 *	  still had room.
 *
 *	The last two cost a processor each, so this is run only when named
 *	(make KTONLY=stkg) and on a machine that has processors to lose.
 */

#include "ktest.h"
#include "kernel.h"
#include "sysdepend/sysdepend.h"

#define STK_PAGE	4096
#include <tk/smem.h>

IMPORT TCB		knl_tcb_table[];
IMPORT UBINT		knl_pt_l0_k_pa;
IMPORT volatile UW	knl_stack_out_count;
IMPORT volatile UD	knl_stack_out_addr;

#define STKG_SIZE	( 16 * 1024 )

LOCAL volatile INT	sg_state;
LOCAL volatile UD	sg_sum;

LOCAL ID sg_task( FP fn, INT stksz, UD prc_mask )
{
	T_CTSK	ctsk;

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0 | ( ( prc_mask != 0 ) ? TA_ASSPRC : 0 );
	ctsk.task    = fn;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = stksz;
	ctsk.assprc  = prc_mask;
	return tk_cre_tsk(&ctsk);
}

LOCAL void sg_end( void )
{
	sg_state = 1;
	tk_exd_tsk();
}

LOCAL void sg_wait_task( void )
{
	while ( sg_state == 0 ) {
		tk_dly_tsk(1);
	}
	tk_exd_tsk();
}

LOCAL void test_mapped( void )
{
	ID	t;
	TCB	*tcb;
	UBINT	foot;
	INT	i;

	sg_state = 0;
	t = sg_task((FP)sg_wait_task, STKG_SIZE + 123, 0);
	KT_ASSERT(t > 0);
	if ( t <= 0 ) return;
	tcb = &knl_tcb_table[t - 1];
	foot = (UBINT)tcb->isstack - (UBINT)tcb->sstksz;
	tm_printf((UB *)"stkg: task %d: stack %lx, %d bytes\n", t, (UD)foot, (INT)tcb->sstksz);
	KT_ASSERT(foot >= (UBINT)VMAP_VA_BASE && foot < (UBINT)VMAP_VA_BASE + CNF_VMAP_SIZE);
	KT_ASSERT_EQ(foot & ( STK_PAGE - 1 ), 0);
	KT_ASSERT_EQ(tcb->sstksz & ( STK_PAGE - 1 ), 0);
	KT_ASSERT(knl_pt_lookup(knl_pt_l0_k_pa, foot) != ~(UD)0);
	for ( i = 1; i <= TASK_STK_GUARD_PAGES; i++ ) {
		KT_ASSERT_EQ(knl_pt_lookup(knl_pt_l0_k_pa, foot - (UBINT)i * STK_PAGE), ~(UD)0);
	}
	KT_ASSERT(knl_stack_guard_hit(tcb, foot - 1));
	KT_ASSERT(knl_stack_guard_hit(tcb, foot - (UBINT)TASK_STK_GUARD_PAGES * STK_PAGE));
	KT_ASSERT(!knl_stack_guard_hit(tcb, foot));
	KT_ASSERT(!knl_stack_guard_hit(tcb, foot - (UBINT)TASK_STK_GUARD_PAGES * STK_PAGE - 1));
	KT_ASSERT_ER(tk_sta_tsk(t, 0), E_OK);
	sg_state = 1;
	while ( tk_ref_tsk(t, &(T_RTSK){0}) >= E_OK ) {
		tk_dly_tsk(1);
	}
}

/* Down to within a few frames of the foot: frames of about a kilobyte, each touched */
LOCAL __attribute__((noinline)) UD sg_deep( UBINT lowest )
{
	volatile UB	b[1024];
	UINT		i;
	UD		r = 0;

	for ( i = 0; i < sizeof(b); i += 256 ) {
		b[i] = (UB)i;
	}
	if ( (UBINT)&b[0] > lowest + 2 * sizeof(b) ) {
		r = sg_deep(lowest);
	}
	return r + b[0];
}

LOCAL void sg_deep_task( void )
{
	TCB	*me = knl_ctxtsk;

	sg_sum = sg_deep((UBINT)me->isstack - (UBINT)me->sstksz + 512);
	sg_end();
}

LOCAL void test_to_the_foot( void )
{
	ID	t;
	T_RTSK	r;

	sg_state = 0;
	t = sg_task((FP)sg_deep_task, STKG_SIZE, 0);
	KT_ASSERT(t > 0);
	if ( t <= 0 ) return;
	KT_ASSERT_ER(tk_sta_tsk(t, 0), E_OK);
	while ( sg_state == 0 ) {
		tk_dly_tsk(1);
	}
	while ( tk_ref_tsk(t, &r) >= E_OK ) {
		tk_dly_tsk(1);
	}
	KT_ASSERT_EQ(knl_stack_out_count, 0);
}

LOCAL void sg_nop_task( void )
{
	tk_exd_tsk();
}

/* More made than the area has room for, were the stacks not to come back: a megabyte each */
LOCAL void test_come_back( void )
{
	INT	i, reused = 0;
	UBINT	first = 0;
	T_RTSK	r;

	for ( i = 0; i < 2000; i++ ) {
		ID	t = sg_task((FP)sg_nop_task, 1024 * 1024, 0);
		UBINT	foot;

		KT_ASSERT(t > 0);
		if ( t <= 0 ) return;
		foot = (UBINT)knl_tcb_table[t - 1].isstack - (UBINT)knl_tcb_table[t - 1].sstksz;
		if ( first == 0 ) first = foot;
		if ( foot == first ) reused++;
		KT_ASSERT_ER(tk_sta_tsk(t, 0), E_OK);
		while ( tk_ref_tsk(t, &r) >= E_OK ) {
			tk_dly_tsk(1);
		}
	}
	tm_printf((UB *)"stkg: 2000 tasks of a megabyte; the first stack's addresses taken %d times\n", reused);
	KT_ASSERT(reused > 1);
}

/* Without end, frames of a kilobyte: the push that finds no room is one of them, or the call's */
LOCAL __attribute__((noinline)) UD sg_run_out( UD n )
{
	volatile UB	b[1024];

	b[0] = (UB)n;
	return sg_run_out(n + 1) + b[0];
}

LOCAL void sg_run_out_task( void )
{
	sg_sum = sg_run_out(0);
	tk_exd_tsk();
}

/* A store under the foot, the stack pointer far above it, room for all that is told */
LOCAL void sg_store_task( void )
{
	TCB	*me = knl_ctxtsk;

	*( (volatile UD *)( (UBINT)me->isstack - (UBINT)me->sstksz ) - 1 ) = 1;
	tk_exd_tsk();
}

LOCAL void wait_out( UW was )
{
	INT	i;

	for ( i = 0; i < 500 && knl_stack_out_count == was; i++ ) {
		tk_dly_tsk(10);
	}
}

LOCAL void test_out( FP fn, INT prc, CONST char *name )
{
	UW	was = knl_stack_out_count;
	ID	t;
	UBINT	foot;
	TCB	*tcb;

	t = sg_task(fn, STKG_SIZE, (UD)1 << prc);
	KT_ASSERT(t > 0);
	if ( t <= 0 ) return;
	tcb = &knl_tcb_table[t - 1];
	foot = (UBINT)tcb->isstack - (UBINT)tcb->sstksz;
	KT_ASSERT_ER(tk_sta_tsk(t, 0), E_OK);
	wait_out(was);
	KT_ASSERT_EQ(knl_stack_out_count, was + 1);
	tm_printf((UB *)"stkg: %s: told of %lx; the stack's foot is %lx\n", name, knl_stack_out_addr, (UD)foot);
	KT_ASSERT(knl_stack_guard_hit(tcb, (UBINT)knl_stack_out_addr));
	tk_dly_tsk(500);	/* the whole report told before the next begins */
#if defined(__x86_64__)
	{
		/* how much of the double fault's stack the report took */
		T_PCPU	*p = knl_pcpu_of(prc);
		INT	j;

		for ( j = 0; j < IST_STACK_SIZE && p->ist_stack[0][j] == 0; j++ ) ;
		tm_printf((UB *)"stkg: the double fault's stack: %d of %d bytes used\n",
			  IST_STACK_SIZE - j, IST_STACK_SIZE);
	}
#endif
}

LOCAL void test_run_out( void )
{
	if ( knl_num_prc < 3 ) {
		KT_SKIP("two processors to lose");
		return;
	}
	test_out((FP)sg_run_out_task, knl_num_prc - 1, "pushing");
}

LOCAL void test_store_under( void )
{
	if ( knl_num_prc < 3 ) {
		KT_SKIP("two processors to lose");
		return;
	}
	test_out((FP)sg_store_task, knl_num_prc - 2, "a store");
}

EXPORT void ktest_stkg( void )
{
	KT_RUN(test_mapped);
	KT_RUN(test_to_the_foot);
	KT_RUN(test_come_back);
	KT_RUN(test_run_out);
	KT_RUN(test_store_under);
}
