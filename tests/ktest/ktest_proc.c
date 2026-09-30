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
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>

IMPORT UD knl_pf_free_count( INT zone );
IMPORT CONST T_OBCRD *knl_ob_crd_of( ID pid );

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
	KT_ASSERT_EQ(hdr[18], 183);			/* EM_AARCH64 */
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

	before = knl_pf_free_count(-1);
	for ( i = 0; i < 3; i++ ) {
		pid = ts_cre_prc(PROG, &cprc);
		KT_ASSERT(pid > 0);
		if ( pid <= 0 ) return;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	}
	after = knl_pf_free_count(-1);
	tm_printf((UB*)"  free pages %d -> %d over three runs\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(before, after);
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

	before = knl_pf_free_count(-1);
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
	after = knl_pf_free_count(-1);
	tm_printf((UB*)"  %d processes; free pages %d -> %d\n", n, (INT)before, (INT)after);
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
	before = knl_pf_free_count(-1);
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
	after = knl_pf_free_count(-1);
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
}
