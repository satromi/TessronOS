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
 *	it is collected. Started with an argument, it writes into an
 *	executable page or runs a writable one, and is ended by the fault;
 *	or it holds pages of every kind while the kernel looks at what their
 *	entries allow (none writable and executable). Skipped when the
 *	program is not on the volume.
 */

#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>

IMPORT UD knl_pf_free_count( INT zone );
#if TS_NETSTACK_NETBSD
IMPORT UD knl_rump_pages( void );
#endif

/*
 * The pages free, with those the network stack holds counted as free:
 * the stack keeps the pages it takes for its own pools and gives none
 * back, and the program's sockets may have it take one more. That is
 * the stack's store, not what the program left behind.
 */
LOCAL UD pages_free( void )
{
#if TS_NETSTACK_NETBSD
	return knl_pf_free_count(-1) + knl_rump_pages();
#else
	return knl_pf_free_count(-1);
#endif
}

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

	before = pages_free();
	KT_ASSERT_EQ(run_once(), 0);
	after = pages_free();
	tm_printf((UB*)"  free pages %d -> %d\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(before, after);
}

/*
 * Code a process writes and runs (include/ts/umem.h, design 9.8.1).
 * The program started with an argument: "wx" writes into a page that
 * is executable, "xw" runs a page that is writable, and each is ended
 * by the fault, which says which access it was; "hold" leaves pages of
 * every kind and waits, while the kernel looks at the entries of its
 * pages: some writable, some executable, none both.
 */
IMPORT ER knl_prc_mem_wx( ID pid, UD out[6] );

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

LOCAL void fault_mode( CONST char *mode, UW ec_want )
{
	T_PSTS		psts;
	T_PRCFLT	f;
	UW		lost;
	ID		pid;
	ER		er;
	BOOL		seen = FALSE;

	if ( !have_prog ) KT_SKIP("no program");
	while ( knl_prc_fault_take(&f, &lost) ) ;		/* what earlier tests left */
	pid = start_mode(mode);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	tm_printf((UB*)"  %s: exit code %d\n", mode, psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_FAULT);
	while ( knl_prc_fault_take(&f, &lost) ) {
		if ( f.pid == pid ) {
			tm_printf((UB*)"  EC %02x FAR %08x%08x\n", (UW)( f.esr >> 26 ) & 0x3f,
				  (UW)( f.far >> 32 ), (UW)f.far);
#if defined(__x86_64__)
			/* a page fault (vector 14) on a page that is there (P), of the
			   program's (U), a write (W) or an instruction's fetch (I) */
			KT_ASSERT_EQ((UW)( f.esr >> 32 ), 14);
			KT_ASSERT_EQ((UW)f.esr & 0x17, ( ec_want == 0x24 ) ? 0x07 : 0x15);
#else
			KT_ASSERT_EQ((UW)( f.esr >> 26 ) & 0x3f, ec_want);
			KT_ASSERT_EQ((UW)( f.esr & 0x3c ), 0x0c);	/* a permission fault */
#endif
			seen = TRUE;
		}
	}
	KT_ASSERT(seen);
}

/* writing into an executable page: a data abort */
LOCAL void test_exec_write( void )
{
	fault_mode("wx", 0x24);
}

/* running a writable page: an instruction abort */
LOCAL void test_exec_run( void )
{
	fault_mode("xw", 0x20);
}

LOCAL void test_exec_wx( void )
{
	T_PSTS	psts;
	UD	out[6] = { 0, 0, 0, 0, 0, 0 };
	ID	pid;
	INT	n;
	BOOL	got = FALSE;

	if ( !have_prog ) KT_SKIP("no program");
	pid = start_mode("hold");
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	for ( n = 0; n < 3000 && !got; n++ ) {
		if ( knl_prc_mem_wx(pid, out) < E_OK ) break;
		got = ( out[2] >= 2 && out[4] >= 1 );
		if ( !got ) tk_dly_tsk(20);
	}
	tm_printf((UB*)"  pages %d: writable %d, executable %d, both %d; turns to run %d, to write %d\n",
		  (INT)out[0], (INT)out[1], (INT)out[2], (INT)out[3], (INT)out[4], (INT)out[5]);
	KT_ASSERT(got);
	KT_ASSERT(out[1] >= 1);
	KT_ASSERT_EQ((INT)out[3], 0);
	(void)ts_ter_prc(pid, TS_ABORT_TERM);
	(void)ts_wai_prc(pid, &psts, 10000);
}

EXPORT void ktest_cxx( void )
{
	KT_RUN(test_present);
	KT_RUN(test_run);
	KT_RUN(test_pages_back);
	KT_RUN(test_exec_write);
	KT_RUN(test_exec_run);
	KT_RUN(test_exec_wx);
}
