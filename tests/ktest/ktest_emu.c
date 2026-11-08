/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_emu.c
 *	BeFC and BeSFC (design 11.32): each program started with "selftest",
 *	which runs a small ROM of its own and checks the picture and a state
 *	saved and read back.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "ktest.h"


#define RUN_TMO		120000

LOCAL void run( CONST char *id, BOOL optional )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(id, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK && optional ) KT_SKIP("not installed: its package is apart from the system");
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return;
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
}

LOCAL void test_befc( void )
{
	run(SYSDEF_PROG_BEFC, FALSE);
}

LOCAL void test_besfc( void )
{
	run(SYSDEF_PROG_BESFC, TRUE);
}

EXPORT void ktest_emu( void )
{
	tm_printf((UB *)"\n[emu] BeFC, BeSFC (design 11.32)\n");
	KT_RUN(test_befc);
	KT_RUN(test_besfc);
}
