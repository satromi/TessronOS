/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_cxx.c
 *	A program in C++ as a process (design 17.19): tests/uprog/cxxprog.cc,
 *	on the boot volume as /boot/CXXPROG.ELF, built with lib/libcxxrt and
 *	lib/libpthread. It runs its own steps -- constructors, exceptions,
 *	the containers, thread local variables, std::thread and its locks,
 *	the POSIX threads, the memory calls of include/ts/umem.h -- and
 *	answers with its exit code. Every page it asked for comes back when
 *	it is collected. Skipped when the program is not on the volume.
 */

#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>

IMPORT UD knl_pf_free_count( INT zone );

#define PROG		"/boot/CXXPROG.ELF"
#define RUN_TMO		120000		/* ms: TCG on one processor is slow */

LOCAL BOOL	have_prog = FALSE;

LOCAL void test_present( void )
{
	T_FSTAT	st;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG);
	}
	tm_printf((UB*)"  %s: %d bytes\n", PROG, (INT)st.size);
	have_prog = TRUE;
}

LOCAL INT run_once( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	ID	pid;
	ER	er;

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = NULL;
	cprc.argsz  = 0;

	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return -1;

	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -1;
	}
	return psts.exitcd;
}

/* every step of the program passes: exit code 0 */
LOCAL void test_run( void )
{
	INT	code;

	if ( !have_prog ) KT_SKIP("no program");
	code = run_once();
	tm_printf((UB*)"  exit code %d\n", code);
	KT_ASSERT_EQ(code, 0);
}

/* the pages of its heap, stacks, threads and reservations all come back */
LOCAL void test_pages_back( void )
{
	UD	before, after;

	if ( !have_prog ) KT_SKIP("no program");

	before = knl_pf_free_count(-1);
	KT_ASSERT_EQ(run_once(), 0);
	after = knl_pf_free_count(-1);
	tm_printf((UB*)"  free pages %d -> %d\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(before, after);
}

EXPORT void ktest_cxx( void )
{
	KT_RUN(test_present);
	KT_RUN(test_run);
	KT_RUN(test_pages_back);
}
