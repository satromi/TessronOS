/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_proc.c
 *	Process management: loading a program from the file system,
 *	running it in its own space, collecting its exit code, refusing a
 *	bad image, terminating a process, and giving the memory back; and
 *	many processes at once, more than there were ASIDs of 8 bits.
 *	Skipped when the program is not on the volume.
 */

#include "ktest.h"
#include "kernel.h"
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>

IMPORT UD knl_pf_free_count( INT zone );
IMPORT BOOL knl_pt_reclaim( void );
IMPORT CONST T_OBCRD *knl_ob_crd_of( ID pid );

#if TS_NETSTACK_NETBSD
IMPORT UD knl_rump_pages( void );
#endif

/*
 * The free pages, with those the network stack has taken for its own
 * counted as free: it keeps every single page it takes (rumpuser.c), and
 * its pools grow now and then while a test runs, for nothing the test
 * did.
 */
IMPORT BOOL knl_pt_reclaim( void );

LOCAL UD free_pages( void );

/*
 * The free pages once the count stops moving: what ran before lets some
 * go late (page tables given back after every processor has passed);
 * taken once the count is the same three times running, a tenth of a
 * second apart, or after five seconds
 */
LOCAL UD free_quiet( void )
{
	UD	f = 0, last = 0;
	INT	i, k, same = 0;

	for ( i = 0; i < 50; i++ ) {
		for ( k = 0; k < 50 && knl_pt_reclaim(); k++ ) tk_dly_tsk(20);
		f = free_pages();
		same = ( i > 0 && f == last ) ? same + 1 : 0;
		if ( same >= 3 ) break;
		last = f;
		tk_dly_tsk(100);
	}
	return f;
}

LOCAL UD free_pages( void )
{
#if TS_NETSTACK_NETBSD
	UD	r, f;

	do {
		r = knl_rump_pages();
		f = knl_pf_free_count(-1);
	} while ( knl_rump_pages() != r );
	return f + r;
#else
	return knl_pf_free_count(-1);
#endif
}

#define PROG		"/boot/HELLO.ELF"

LOCAL BOOL	have_prog = FALSE;

/* the program is on the volume and is an ELF64 for this machine */
LOCAL void test_program_present( void )
{
	T_FSTAT	st;
	UB	hdr[20];
	INT	fd, n;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no /boot/HELLO.ELF");
	}
	KT_ASSERT(st.size > 0);
	tm_printf((UB*)"  %s: %d bytes\n", PROG, (INT)st.size);

	fd = fs_open(PROG, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	n = fs_read(fd, hdr, sizeof(hdr));
	KT_ASSERT_EQ(n, sizeof(hdr));
	KT_ASSERT_EQ(hdr[0], 0x7f);
	KT_ASSERT_EQ(hdr[1], (UB)'E');
	KT_ASSERT_EQ(hdr[4], 2);			/* ELFCLASS64 */
#if defined(__x86_64__)
	KT_ASSERT_EQ(hdr[18], 62);			/* EM_X86_64 */
#else
	KT_ASSERT_EQ(hdr[18], 183);			/* EM_AARCH64 */
#endif
	KT_ASSERT_ER(fs_close(fd), EX_OK);
	have_prog = TRUE;
}

/* the program runs and its return value comes back as the exit code */
LOCAL void test_run( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_RPRC	rprc;
	UW	arg = 42;
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = &arg;
	cprc.argsz  = sizeof(arg);

	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	KT_ASSERT_ER(ts_ref_prc(pid, &rprc), E_OK);
	KT_ASSERT_EQ(rprc.pid, pid);
	KT_ASSERT(rprc.memsz > 0);
	tm_printf((UB*)"  pid %d: %d bytes mapped\n", pid, (INT)rprc.memsz);

	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, (INT)arg);		/* main returned the argument */

	/* collected: it is gone */
	KT_ASSERT_ER(ts_ref_prc(pid, &rprc), E_NOEXS);
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 0), E_NOEXS);
}

/* two processes run at once, each in its own space */
LOCAL void test_two( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg1 = 7, arg2 = 9;
	ID	p1, p2;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz = 0;
	cprc.argsz = sizeof(UW);

	cprc.arg = &arg1;
	p1 = ts_cre_prc(PROG, &cprc);
	cprc.arg = &arg2;
	p2 = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(p1 > 0 && p2 > 0 && p1 != p2);
	if ( p1 <= 0 || p2 <= 0 ) return;

	KT_ASSERT_ER(ts_wai_prc(p1, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, (INT)arg1);
	KT_ASSERT_ER(ts_wai_prc(p2, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, (INT)arg2);
}

/* a file that is not a program is refused */
LOCAL void test_bad_image( void )
{
	T_CPRC	cprc;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz = 0;
	cprc.arg = NULL;
	cprc.argsz = 0;

	KT_ASSERT_EQ(ts_cre_prc("/boot/HELLO.TXT", &cprc), E_OBJ);
	KT_ASSERT_EQ(ts_cre_prc("/boot/NOSUCH.ELF", &cprc), E_NOEXS);
}

/* every page a process used comes back when it is collected */
LOCAL void test_resources( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg = 1;
	UD	before, after;
	ID	pid;
	INT	i;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz = 0;
	cprc.arg = &arg;
	cprc.argsz = sizeof(arg);

	/* one run first, so that any one time allocation is already done */
	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);

	before = free_quiet();
	for ( i = 0; i < 3; i++ ) {
		pid = ts_cre_prc(PROG, &cprc);
		KT_ASSERT(pid > 0);
		if ( pid <= 0 ) return;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	}
	after = free_quiet();
	tm_printf((UB*)"  free pages %d -> %d over three runs\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(before, after);
}

/*
 * A process that has ended and has not been collected says so: once its
 * end has been told its state is PS_ZOMBIE ("ended"), not PS_EXITING,
 * it has no task left and its space and pages have been given back.
 * Only the control block with the exit code waits for the collector
 * (design 5.5). One the kernel started may not be collected for a long
 * while, or ever; whoever watches it (the sound device, the list of
 * 強制終了) must not take it for one still ending.
 */
LOCAL void test_end_uncollected( void )
{
	T_CPRC		cprc;
	T_PSTS		psts;
	T_RPRC		r;
	T_PRCINFO	pi;
	UW		arg = 7;
	UD		ended, collected;
	ID		pid;
	INT		t;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz = 0;
	cprc.arg = &arg;
	cprc.argsz = sizeof(arg);

	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	knl_memset(&pi, 0, sizeof(pi));
	for ( t = 0; t < 500 && !( knl_prc_info(pid, &pi) >= E_OK && pi.done ); t++ ) {
		tk_dly_tsk(10);
	}
	KT_ASSERT(pi.done);			/* its end has been told */
	knl_memset(&r, 0, sizeof(r));
	for ( t = 0; t < 200; t++ ) {
		if ( ts_ref_prc(pid, &r) >= E_OK && r.state == PS_ZOMBIE && r.ntsk == 0 ) {
			break;
		}
		tk_dly_tsk(10);
	}
	ended = free_pages();
	tm_printf((UB*)"  pid %d told its end: state %d, %d task(s), %d bytes mapped before it is collected\n",
		  pid, (INT)r.state, (INT)r.ntsk, (INT)r.memsz);
	KT_ASSERT_EQ(r.state, PS_ZOMBIE);
	KT_ASSERT_EQ(r.ntsk, 0);
	KT_ASSERT_EQ(r.memsz, 0);

	/* the collector still gets the exit code, and has no page left to give back */
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 1000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	KT_ASSERT_ER(ts_ref_prc(pid, &r), E_NOEXS);
	collected = free_pages();
	tm_printf((UB*)"  free pages %d when it ended, %d once collected\n", (INT)ended, (INT)collected);
	KT_ASSERT_EQ(collected, ended);
}

/*
 * Processes the kernel starts and nobody collects, KT_CHURN_N of them
 * one after another: their control blocks go once they have been ended
 * for knl_prc_orphan_ms (made short here), so that neither the blocks
 * nor the pages pile up; one that is waited for is not taken away from
 * its waiter, and one asked for after that time is gone (design 5.5).
 */
IMPORT UW	knl_prc_orphan_ms;
IMPORT INT	knl_prc_count( void );

LOCAL void test_churn_uncollected( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_RPRC	r;
	UW	arg = 3, keep = knl_prc_orphan_ms;
	UD	before, after = 0;
	INT	n0, n = 0, most = 0, i, t, c;
	ID	pid, last = 0;

	if ( !have_prog ) KT_SKIP("no program");

	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz = 0;
	cprc.arg = &arg;
	cprc.argsz = sizeof(arg);

	knl_prc_orphan_ms = 100;
	tk_dly_tsk(1500);			/* the ones left before go first */
	(void)knl_pt_reclaim();
	n0 = knl_prc_count();
	before = free_pages();
	for ( i = 0; i < KT_CHURN_N; i++ ) {
		pid = ts_cre_prc(PROG, &cprc);
		if ( pid == E_LIMIT || pid == E_NOMEM ) {
			tk_dly_tsk(50);
			i--;
			continue;
		}
		KT_ASSERT(pid > 0);
		if ( pid <= 0 ) break;
		last = pid;
		n++;
		c = knl_prc_count();
		if ( c > most ) most = c;
		if ( ( i & 15 ) == 15 ) tk_dly_tsk(1);
	}
	for ( t = 0; t < 100; t++ ) {
		(void)knl_pt_reclaim();
		after = free_pages();
		if ( knl_prc_count() == n0 && after == before ) break;
		tk_dly_tsk(100);
	}
	tm_printf((UB*)"  %d processes not collected: blocks %d -> at most %d -> %d, free pages %d -> %d\n",
		  n, n0, most, knl_prc_count(), (INT)before, (INT)after);
	if ( knl_prc_count() != n0 ) {
		/* where the ones left stand: what their main task waits for, and the reaper */
		IMPORT ID	knl_prc_reaper_tid( void );
		INT	shown = 0;
		T_RTSK	rr;

		knl_memset(&rr, 0, sizeof(rr));
		(void)tk_ref_tsk(knl_prc_reaper_tid(), &rr);
		tm_printf((UB*)"  the reaper %d: state %x waits %x on %d, orphan keep %d ms\n",
			  (INT)knl_prc_reaper_tid(), (INT)rr.tskstat, (INT)rr.tskwait, (INT)rr.wid,
			  (INT)knl_prc_orphan_ms);
		{
			TCB	*tcb = knl_tsk_tcb(knl_prc_reaper_tid());

			if ( tcb != NULL && rr.tskstat != TTS_RUN ) {
				/* its saved context: the callee registers, then the exception frame */
				UD	*f = (UD *)tcb->tskctxb.ssp + 10;

				tm_printf((UB*)"  the reaper stopped at %lx\n", f[21]);
				knl_lockup_say_frames("  ", (UBINT)f[19]);
			}
			if ( rr.tskwait == TTW_MTX ) {
				T_RMTX	rm;
				T_RTSK	rh;
				TCB	*h;

				knl_memset(&rm, 0, sizeof(rm));
				knl_memset(&rh, 0, sizeof(rh));
				(void)tk_ref_mtx(rr.wid, &rm);
				(void)tk_ref_tsk(rm.htsk, &rh);
				h = knl_tsk_tcb(rm.htsk);
				tm_printf((UB*)"  mutex %d is held by task %d: state %x waits %x on %d, entry %lx\n",
					  (INT)rr.wid, (INT)rm.htsk, (INT)rh.tskstat, (INT)rh.tskwait, (INT)rh.wid,
					  ( h != NULL ) ? (UD)(UBINT)h->task : 0);
				if ( h != NULL ) {
					INT	k;

					for ( k = 0; k < 200 && h->runprc != 0; k++ ) {
						tk_dly_tsk(10);
					}
					tm_printf((UB*)"  holder: owner %d pri %d sysmode %d svcdepth %d svcin %d runprc %d state %x after %d tries; still holds %d\n",
						  (INT)h->owner, (INT)h->priority, (INT)h->sysmode, (INT)h->svcdepth,
						  (INT)h->svcin, (INT)h->runprc, (INT)h->state, k,
						  ( tk_ref_mtx(rr.wid, &rm) == E_OK ) ? (INT)rm.htsk : -1);
					if ( h->runprc == 0 ) {
						UD	*g = (UD *)h->tskctxb.ssp + 10;

						tm_printf((UB*)"  it stopped at %lx\n", g[21]);
						knl_lockup_say_frames("  ", (UBINT)g[19]);
					}
				}
			}
		}

		for ( pid = 1; pid < last && shown < 4; pid++ ) {
			T_PRCINFO	pi;
			T_RTSK		rt;

			if ( knl_prc_info(pid, &pi) < E_OK || ts_ref_prc(pid, &r) < E_OK ) continue;
			knl_memset(&rt, 0, sizeof(rt));
			(void)tk_ref_tsk(r.maintsk, &rt);
			tm_printf((UB*)"  pid %d: state %d ending %d incall %d done %d tasks %d;"
				  " main task %d state %x waits %x on %d\n",
				  pid, (INT)pi.state, (INT)pi.ending, (INT)pi.incall, (INT)pi.done, (INT)pi.ntsk,
				  (INT)r.maintsk, (INT)rt.tskstat, (INT)rt.tskwait, (INT)rt.wid);
			shown++;
		}
	}
	KT_ASSERT_EQ(n, KT_CHURN_N);
	KT_ASSERT_EQ(knl_prc_count(), n0);
	KT_ASSERT_EQ(after, before);
	KT_ASSERT_ER(ts_ref_prc(last, &r), E_NOEXS);	/* asked for too late: gone */

	/* one that is waited for keeps its exit code for its waiter */
	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	}
	knl_prc_orphan_ms = keep;
}

/* a message sent to a process reaches it, and a child reports its end */
LOCAL void test_message( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_TSMSG	msg;
	UW	arg[2];
	ID	pid;
	ER	er;

	if ( !have_prog ) KT_SKIP("no program");

	arg[0] = 0;			/* the exit code comes from the message */
	arg[1] = 1;			/* wait for one */

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);

	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	/* it reaches the wait after its greeting */
	KT_ASSERT_ER(tk_dly_tsk(50), E_OK);

	msg.type = 0x1234;
	msg.from = 0;
	msg.size = 4;
	msg.body[0] = 77;
	msg.body[1] = 0;
	msg.body[2] = 0;
	msg.body[3] = 0;
	KT_ASSERT_ER(ts_snd_msg(pid, &msg, 1000), E_OK);

	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 77);		/* the first byte of the message */

	/* a message to something that is not there */
	er = ts_snd_msg(9999, &msg, 0);
	KT_ASSERT_EQ(er, E_NOEXS);

	/* the queue belongs to a process; a kernel task has none */
	KT_ASSERT_EQ(ts_rcv_msg(&msg, 0), E_OBJ);

	/* a message longer than the queue holds is refused */
	msg.size = TS_MSG_MAX + 1;
	KT_ASSERT_EQ(ts_snd_msg(pid, &msg, 0), E_PAR);
}

/*
 * Many processes at once: each is there, has its own number, space and
 * credentials, takes a message and ends with the code it was sent; and
 * the memory they had comes back.
 */
#define MANY_PRC	300

LOCAL void test_many( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_RPRC	rprc;
	T_TSMSG	msg;
	UW	arg[2];
	ID	*pids;
	UD	before, after;
	INT	i, n, alive = 0, crd = 0, codes = 0;

	if ( !have_prog ) KT_SKIP("no program");
	pids = (ID *)Kmalloc(sizeof(ID) * MANY_PRC);
	KT_ASSERT(pids != NULL);
	if ( pids == NULL ) return;

	arg[0] = 0;
	arg[1] = 1;			/* each waits for a message */
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);

	before = free_quiet();
	for ( n = 0; n < MANY_PRC; n++ ) {
		pids[n] = ts_cre_prc(PROG, &cprc);
		if ( pids[n] <= 0 ) {
			tm_printf((UB*)"  process %d: %d\n", n, pids[n]);
			break;
		}
	}
	KT_ASSERT_EQ(n, MANY_PRC);
	KT_ASSERT_ER(tk_dly_tsk(200), E_OK);

	for ( i = 0; i < n; i++ ) {
		if ( ts_ref_prc(pids[i], &rprc) >= E_OK && rprc.state == PS_RUNNING
		  && rprc.pid == pids[i] ) {
			alive++;
		}
		if ( knl_ob_crd_of(pids[i]) == knl_ob_crd_of(0) ) {
			crd++;			/* as its parent, the kernel: the system */
		}
	}
	KT_ASSERT_EQ(alive, n);
	KT_ASSERT_EQ(crd, n);

	msg.type = 0x1234;
	msg.from = 0;
	msg.size = 4;
	msg.body[1] = msg.body[2] = msg.body[3] = 0;
	for ( i = 0; i < n; i++ ) {
		msg.body[0] = (UB)( i % 200 );
		(void)ts_snd_msg(pids[i], &msg, 1000);
	}
	for ( i = 0; i < n; i++ ) {
		psts.exitcd = -1;
		if ( ts_wai_prc(pids[i], &psts, 10000) >= E_OK && psts.exitcd == i % 200 ) {
			codes++;
		}
	}
	KT_ASSERT_EQ(codes, n);
	after = free_pages();
	tm_printf((UB*)"  %d processes; free pages %d -> %d at once", n, (INT)before, (INT)after);
	after = free_quiet();
	tm_printf((UB*)", %d once the count holds still\n", (INT)after);
	KT_ASSERT_EQ(before, after);
	Kfree(pids);
}

/*
 * A large program: the small one written out again with a segment of
 * BIG_SEG bytes of nothing after its own, so that it has much to read.
 * While a kernel task starts it, this task keeps asking for its process
 * number, which takes the lock of the process table: the reading is
 * done without that lock, so no answer waits for the reading. The
 * program runs as the small one does and its pages all come back.
 */
#define BIG_PROG	"/boot/KTBIG.ELF"
#define BIG_SEG		( 40 * 1024 * 1024 )
#define BIG_SHIFT	0x10000		/* where the small one's bytes go in the large */
#define BIG_PIECE	( 256 * 1024 )
#define BIG_PHMAX	8		/* program headers the loader looks at */

LOCAL struct {
	volatile BOOL	done;
	ID		pid;
	UW		took;		/* ms the start took */
	UW		code;
} big;

LOCAL UW big_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

LOCAL void big_start( INT stacd, void *exinf )
{
	T_CPRC	cprc;
	UW	t0 = big_now();

	(void)stacd;
	(void)exinf;
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.pri   = KT_PRI_HIGH;
	cprc.arg   = &big.code;
	cprc.argsz = sizeof(big.code);
	big.pid = ts_cre_prc(BIG_PROG, &cprc);
	big.took = big_now() - t0;
	big.done = TRUE;
	tk_exd_tsk();
}

/* The small program as a large one at BIG_PROG; FALSE when it could not be written */
LOCAL BOOL big_write( void )
{
	T_FSTAT	st;
	UB	*img = NULL, *buf = NULL;
	UD	phoff, vend = 0, off, left;
	INT	fd = -1, n, i, phnum, size;
	BOOL	ok = FALSE;

	if ( fs_stat(PROG, &st) < EX_OK || st.size <= 0 || st.size > BIG_SHIFT * 16 ) {
		return FALSE;
	}
	size = (INT)st.size;
	img = (UB *)Kmalloc(size);
	buf = (UB *)Kmalloc(BIG_PIECE);
	if ( img == NULL || buf == NULL ) goto out;
	fd = fs_open(PROG, O_RDONLY);
	if ( fd < 0 ) goto out;
	n = fs_read(fd, img, size);
	(void)fs_close(fd);
	if ( n != size ) goto out;

	/* the header, with the program headers right after it and no sections */
	phoff = *(UD *)(void *)( img + 32 );
	phnum = *(UH *)(void *)( img + 56 );
	if ( *(UH *)(void *)( img + 54 ) != 56 || phnum + 1 > BIG_PHMAX
	  || phoff + (UD)phnum * 56 > (UD)size ) {
		goto out;
	}
	knl_memset(buf, 0, BIG_PIECE);
	knl_memcpy(buf, img, 64);
	*(UD *)(void *)( buf + 32 ) = 64;		/* e_phoff */
	*(UD *)(void *)( buf + 40 ) = 0;		/* e_shoff */
	*(UH *)(void *)( buf + 56 ) = (UH)( phnum + 1 );
	*(UH *)(void *)( buf + 60 ) = 0;		/* e_shnum */
	*(UH *)(void *)( buf + 62 ) = 0;		/* e_shstrndx */
	for ( i = 0; i < phnum; i++ ) {
		UB	*ph = buf + 64 + i * 56;
		UD	end;

		knl_memcpy(ph, img + phoff + (UD)i * 56, 56);
		*(UD *)(void *)( ph + 8 ) += BIG_SHIFT;			/* p_offset */
		end = *(UD *)(void *)( ph + 16 ) + *(UD *)(void *)( ph + 40 );	/* p_vaddr + p_memsz */
		if ( *(UW *)(void *)ph == 1 && end > vend ) vend = end;
	}
	/* the segment of nothing: read only, 2MB past the program's own */
	{
		UB	*ph = buf + 64 + phnum * 56;
		UD	at = ( BIG_SHIFT + (UD)size + 0xFFF ) & ~(UD)0xFFF;

		*(UW *)(void *)( ph + 0 ) = 1;				/* PT_LOAD */
		*(UW *)(void *)( ph + 4 ) = 4;				/* PF_R */
		*(UD *)(void *)( ph + 8 ) = at;
		*(UD *)(void *)( ph + 16 ) = ( ( vend + 0x1FFFFF ) & ~(UD)0x1FFFFF ) + 0x200000;
		*(UD *)(void *)( ph + 24 ) = *(UD *)(void *)( ph + 16 );
		*(UD *)(void *)( ph + 32 ) = BIG_SEG;
		*(UD *)(void *)( ph + 40 ) = BIG_SEG;
		*(UD *)(void *)( ph + 48 ) = 0x1000;
	}
	fd = fs_open(BIG_PROG, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) goto out;
	if ( fs_write(fd, buf, BIG_SHIFT) != BIG_SHIFT || fs_write(fd, img, size) != size ) goto out;
	knl_memset(buf, 0, BIG_PIECE);
	off = BIG_SHIFT + (UD)size;
	n = (INT)( ( ( off + 0xFFF ) & ~(UD)0xFFF ) - off );
	if ( n > 0 && fs_write(fd, buf, n) != n ) goto out;
	for ( left = BIG_SEG; left > 0; left -= (UD)n ) {
		n = ( left > BIG_PIECE ) ? BIG_PIECE : (INT)left;
		if ( fs_write(fd, buf, n) != n ) goto out;
	}
	ok = TRUE;
out:
	if ( fd >= 0 ) (void)fs_close(fd);
	if ( img != NULL ) Kfree(img);
	if ( buf != NULL ) Kfree(buf);
	return ok;
}

LOCAL void test_big( void )
{
	T_CTSK	ctsk;
	T_PSTS	psts;
	UD	before, after;
	UW	t0, d, worst = 0, start;
	INT	asks = 0;
	ID	tid;

	if ( !have_prog ) KT_SKIP("no program");
	if ( !big_write() ) {
		(void)fs_unlink(BIG_PROG);
		KT_SKIP("no room for a large program on /boot");
	}
	before = free_pages();
	big.done = FALSE;
	big.pid = 0;
	big.code = 57;
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)big_start;
	ctsk.itskpri = KT_PRI_MAIN + 1;		/* below this task: it is the one that is kept waiting */
	ctsk.stksz = 16 * 1024;
	tid = tk_cre_tsk(&ctsk);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) goto out;
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);

	start = big_now();
	while ( !big.done && big_now() - start < 60000 ) {
		t0 = big_now();
		(void)ts_get_pid();
		d = big_now() - t0;
		if ( d > worst ) worst = d;
		asks++;
		tk_dly_tsk(2);
	}
	tm_printf((UB*)"  %d MB program started in %d ms; %d asks, the longest %d ms\n",
		  BIG_SEG >> 20, (INT)big.took, asks, (INT)worst);
	KT_ASSERT(big.done);
	KT_ASSERT(big.pid > 0);
	/* never as long as the reading: a third of it, or a few ticks when it was quick */
	KT_ASSERT(worst < 150 || worst * 3 < big.took);
	if ( big.pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(big.pid, &psts, 10000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, 57);
	}
	after = free_pages();
	KT_ASSERT_EQ(before, after);
out:
	(void)fs_unlink(BIG_PROG);
}

EXPORT void ktest_proc( void )
{
	KT_RUN(test_program_present);
	KT_RUN(test_run);
	KT_RUN(test_two);
	KT_RUN(test_bad_image);
	KT_RUN(test_message);
	KT_RUN(test_resources);
	KT_RUN(test_many);
	KT_RUN(test_big);
	KT_RUN(test_end_uncollected);
	KT_RUN(test_churn_uncollected);
}
