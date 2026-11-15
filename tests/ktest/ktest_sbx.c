/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_sbx.c
 *	Restricted processes (include/ts/sbx.h, design 5.9): a process shut
 *	out of groups of calls does what it is still allowed -- memory,
 *	threads and their waits, time, random bytes, memory objects and
 *	streams without a name made and used, what it held before -- and
 *	is refused or ended for the rest: objects by UUID and by name,
 *	files by path, the network, devices, the GPU, other processes, the
 *	windows, the system's own. The set is handed to a child, grows only,
 *	and cannot be loosened. The processes are tests/uprog/cxxprog.cc in
 *	its "sbx" modes; each answers by its exit code the first check that
 *	went wrong. Skipped when the program is not on the volume.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/xf.h>
#include <ts/sbx.h>
#include <ts/svc.h>

#define PROG		"/boot/CXXPROG.ELF"
#define RUN_TMO		120000		/* ms: TCG on one processor is slow */

LOCAL BOOL	have_prog = FALSE;

LOCAL ID start_mode( CONST char *mode )
{
	T_CPRC	cprc;
	SZ	n = 0;

	while ( mode[n] != '\0' ) n++;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = (void *)mode;
	cprc.argsz  = n;
	return ts_cre_prc(PROG, &cprc);
}

LOCAL INT wait_code( ID pid )
{
	T_PSTS	psts;

	psts.exitcd = -1;
	if ( ts_wai_prc(pid, &psts, RUN_TMO) < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -1;
	}
	return psts.exitcd;
}

/* A mode that is sent the UUID of the program as a program object, and its exit code */
LOCAL INT run_with_prog( CONST char *mode, BOOL *p_skip )
{
	T_TSMSG	msg;
	TS_UUID	prog;
	ID	pid;
	INT	code;

	*p_skip = FALSE;
	if ( xf_import_path(PROG, "boot", NULL, NULL, NULL, &prog) < E_OK ) {
		*p_skip = TRUE;
		return 0;
	}
	pid = start_mode(mode);
	if ( pid <= 0 ) {
		(void)ob_del_obj(&prog);
		return -2;
	}
	knl_memset(&msg, 0, sizeof(msg));
	msg.type = 1;
	msg.size = sizeof(TS_UUID);
	knl_memcpy(msg.body, &prog, sizeof(TS_UUID));
	if ( ts_snd_msg(pid, &msg, 1000) < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
	}
	code = wait_code(pid);
	(void)ob_del_obj(&prog);
	return code;
}

/*
 * The counts of a process that has ended and not been collected yet
 * (its end is waited for without collecting): FALSE when it did not
 * end in time
 */
LOCAL BOOL ended_counts( ID pid, UW out[TS_SBX_NGROUP + 2] )
{
	T_RPRC	rp;
	INT	t;

	for ( t = 0; t < RUN_TMO; t += 50 ) {
		if ( ts_ref_prc(pid, &rp) >= E_OK && rp.state == PS_ZOMBIE ) {
			return knl_prc_sbx_refused_of(pid, out);
		}
		(void)tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL void test_present( void )
{
	T_FSTAT	st;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG);
	}
	have_prog = TRUE;
}

/* What is no process's to ask: the kernel's tasks have no set */
LOCAL void test_kernel( void )
{
	UW	cnt[TS_SBX_NGROUP + 2];

	KT_ASSERT_ER(knl_prc_set_sbx(TS_SBX_NET, 0), E_CTX);
	KT_ASSERT_EQ(knl_prc_sbx(), 0);
	KT_ASSERT_EQ(knl_prc_sbx_of(0), 0);
	/* nothing to count for them, and no process 0 to read */
	KT_ASSERT_EQ(knl_prc_sbx_count(TSN_OB_OPN_OBJ, TS_SBX_OBJ), 0);
	KT_ASSERT(!knl_prc_sbx_refused_of(0, cnt));
}

/*
 * The renderer's set, refusing: everything it is still allowed works,
 * and every group shut out answers E_OACV (EPERM, EX_PERM for the
 * files); nothing it held before is lost, and the set cannot be loosened
 */
LOCAL void test_renderer( void )
{
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	pid = start_mode("sbx r");
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		UW	cnt[TS_SBX_NGROUP + 2];
		INT	i, sum = 0;

		/* each group it was refused in is counted; a call of two groups counts in both */
		KT_ASSERT(ended_counts(pid, cnt));
		for ( i = 0; i < TS_SBX_NGROUP; i++ ) {
			KT_ASSERT(cnt[1 + i] >= 1);
			sum += (INT)cnt[1 + i];
		}
		KT_ASSERT(cnt[0] >= 10 && sum >= (INT)cnt[0]);
		KT_ASSERT(cnt[TS_SBX_NGROUP + 1] != 0);
		KT_ASSERT_EQ(wait_code(pid), 0);
	}
}

/* The set of the network's process: storage objects open, sockets made; devices, the GPU and windows not */
LOCAL void test_network( void )
{
	BOOL	skip;
	INT	code = run_with_prog("sbx n", &skip);

	if ( !have_prog ) KT_SKIP("no program");
	if ( skip ) KT_SKIP("no store for a program object");
	KT_ASSERT_EQ(code, 0);
}

/* Without TS_SBX_REFUSE a call shut out ends the process, by TS_ABORT_SBX */
LOCAL void test_end( void )
{
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	pid = start_mode("sbx k");
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		UW	cnt[TS_SBX_NGROUP + 2];

		/* the call that ended it is the one counted */
		KT_ASSERT(ended_counts(pid, cnt));
		KT_ASSERT_EQ(cnt[0], 1);
		KT_ASSERT_EQ(cnt[1], 1);
		KT_ASSERT_EQ(cnt[TS_SBX_NGROUP + 1], TSN_OB_OPN_OBJ);
		KT_ASSERT_EQ(wait_code(pid), TS_ABORT_SBX);
	}
}

/*
 * A restricted parent's child starts with the parent's set, adds to it
 * and cannot take any away nor go back to refusing; its end by a call
 * shut out reads as SIGSYS to its parent's waitpid
 */
LOCAL void test_inherit( void )
{
	BOOL	skip;
	INT	code;

	if ( !have_prog ) KT_SKIP("no program");
	code = run_with_prog("sbx p", &skip);
	if ( skip ) KT_SKIP("no store for a program object");
	KT_ASSERT_EQ(code, 0);
}

EXPORT void ktest_sbx( void )
{
	KT_RUN(test_present);
	KT_RUN(test_kernel);
	KT_RUN(test_renderer);
	KT_RUN(test_network);
	KT_RUN(test_end);
	KT_RUN(test_inherit);
}
