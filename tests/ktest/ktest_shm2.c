/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_shm2.c
 *	Shared memory between processes and the ends of processes
 *	(design 18.6, 5.5.7, 4.6): memory objects without a name
 *	(OB_F_TRANSIENT) made of single pages, handed from process to
 *	process as keys, read-only when the key is, gone with the last
 *	key and mapping; addresses of the shared window given back and
 *	handed out again; and children that end with their parent
 *	(TS_PRC_WITHPARENT). The processes are tests/uprog/cxxprog.cc in
 *	its modes, through lib/libcxxrt's memfd_create, mmap, kill and
 *	waitpid. Skipped when the program is not on the volume.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/xf.h>
#include "sysman/pfalloc.h"
#include "sysman/space.h"

IMPORT INT knl_obmem_stat( UD *p_pages, UD *p_mapped, ID pid, UD *p_pid_pages );
#if TS_NETSTACK_NETBSD
IMPORT UD knl_rump_pages( void );
#endif

#define PROG		"/boot/CXXPROG.ELF"
#define RUN_TMO		120000		/* ms: TCG on one processor is slow */

LOCAL BOOL	have_prog = FALSE;

/* The pages free, with those the network stack keeps for its pools counted as free */
LOCAL UD pages_free( void )
{
#if TS_NETSTACK_NETBSD
	return knl_pf_free_count(-1) + knl_rump_pages();
#else
	return knl_pf_free_count(-1);
#endif
}

LOCAL ID start_mode( CONST char *mode, ATR atr )
{
	T_CPRC	cprc;
	SZ	n = 0;

	while ( mode[n] != '\0' ) n++;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = atr;
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

LOCAL void put_num( char *b, INT *at, INT v )
{
	char	t[12];
	INT	n = 0;

	do { t[n++] = (char)( '0' + v % 10 ); v /= 10; } while ( v > 0 );
	while ( n > 0 ) b[(*at)++] = t[--n];
}

LOCAL BOOL same( CONST void *a, CONST char *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && ((CONST UB *)a)[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( i == n );
}

LOCAL void test_present( void )
{
	T_FSTAT	st;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG);
	}
	have_prog = TRUE;
}

/* ---------------------------------------------------------------- the window */

/* A stretch given back is handed out again, and the window is as it was */
LOCAL void test_va_reuse( void )
{
	UD	used0 = knl_shm_va_used(NULL);
	UBINT	a, b, c;

	a = knl_shm_va_alloc(65536);
	b = knl_shm_va_alloc(65536);
	KT_ASSERT(a != 0 && b != 0 && b > a);
	KT_ASSERT(b >= a + 65536 + PAGE_SIZE);		/* a page of nothing between */
	knl_shm_va_free(a, 65536);
	c = knl_shm_va_alloc(16384);			/* first fit: where a was */
	KT_ASSERT_EQ(c, a);
	knl_shm_va_free(c, 16384);
	knl_shm_va_free(b, 65536);
	KT_ASSERT_EQ(knl_shm_va_used(NULL), used0);	/* joined, and back into the untouched part */
	c = knl_shm_va_alloc(65536);
	KT_ASSERT_EQ(c, a);
	knl_shm_va_free(c, 65536);
}

/* ---------------------------------------------------------------- the records */

LOCAL ER make_transient( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_TRANSIENT;
	return ob_cre_obj(&c, u);
}

/*
 * A record of 12MB -- more than the buddy allocator's largest block --
 * made of single pages, read and written across them; the object, not
 * listed, goes with its last key, and its pages with it
 */
LOCAL void test_pages( void )
{
	TS_UUID	u, buf[64];
	T_OBCRE	c;
	T_OBREF	r;
	UD	pg0, pg;
	SZ	asz = 0;
	INT	n0, recno = -1, cnt = 0, i;
	UB	in[16];
	ID	k;
	BOOL	listed = FALSE;

	n0 = knl_obmem_stat(&pg0, NULL, 0, NULL);
	KT_ASSERT_ER(make_transient(&u), E_OK);
	k = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_trn_rec(k, 0, 12ULL << 20), E_OK);
	(void)knl_obmem_stat(&pg, NULL, 0, NULL);
	KT_ASSERT_EQ(pg - pg0, ( 12ULL << 20 ) / PAGE_SIZE);
	/* across a page boundary, far in */
	KT_ASSERT_ER(ob_wri_rec(k, 0, ( 9ULL << 20 ) - 3, "boundary", 8, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(k, 0, ( 9ULL << 20 ) - 3, in, 8, &asz), E_OK);
	KT_ASSERT(asz == 8 && same(in, "boundary", 8));
	/* cut and grown again: what was past the cut reads as zeros */
	KT_ASSERT_ER(ob_trn_rec(k, 0, ( 9ULL << 20 ) - 1), E_OK);
	KT_ASSERT_ER(ob_trn_rec(k, 0, 10ULL << 20), E_OK);
	KT_ASSERT_ER(ob_rea_rec(k, 0, ( 9ULL << 20 ) - 3, in, 8, &asz), E_OK);
	KT_ASSERT(in[0] == 'b' && in[1] == 'o' && in[2] == 0 && in[7] == 0);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(( r.flags & OB_F_TRANSIENT ) != 0);
	/* not listed */
	if ( ob_lst_obj(OB_T_STORAGE, OB_S_MEMORY, NULL, buf, 64, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( ts_uuid_cmp(&buf[i], &u) == 0 ) listed = TRUE;
		}
	}
	KT_ASSERT(!listed);
	/* one found by its name cannot be one without */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_TRANSIENT | OB_F_GLOBAL;
	c.name = (CONST UB *)"shm2 named";
	KT_ASSERT_ER(ob_cre_obj(&c, &buf[0]), E_PAR);

	ob_cls_obj(k);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_NOEXS);
	KT_ASSERT_EQ(knl_obmem_stat(&pg, NULL, 0, NULL), n0);
	KT_ASSERT_EQ(pg, pg0);
}

/*
 * 12MB mapped into a process: each page where the record has it, and
 * the mapping, the last hold, goes when the process ends
 */
LOCAL void test_map_big( void )
{
	TS_UUID	u, pu;
	T_OBMAP	m;
	UD	pg0, pa;
	UBINT	va;
	SZ	asz = 0;
	INT	n0, recno = -1;
	ID	k, kp, pid;

	if ( !have_prog ) KT_SKIP("no program");
	n0 = knl_obmem_stat(&pg0, NULL, 0, NULL);
	pid = start_mode("wait", 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(knl_prc_uuid(pid, &pu, NULL), E_OK);
	kp = ob_opn_obj(&pu, OB_OP_ALL);
	KT_ASSERT_ER(make_transient(&u), E_OK);
	k = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(k > 0 && kp > 0);
	KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(k, 0, 0, "first", 5, &asz), E_OK);
	knl_memset(&m, 0, sizeof(m));
	m.flags = OB_M_WRITE;
	m.size = 12 << 20;
	KT_ASSERT_ER(ob_map_rec(k, 0, kp, &m), E_OK);
	va = (UBINT)m.addr;
	KT_ASSERT_EQ(m.size, 12 << 20);
	pa = knl_prc_lookup(pid, va);
	KT_ASSERT(pa != ~(UD)0);
	if ( pa != ~(UD)0 ) {
		KT_ASSERT(same(PA2VA(pa), "first", 5));
	}
	KT_ASSERT(knl_prc_lookup(pid, va + ( 11ULL << 20 )) != ~(UD)0);
	KT_ASSERT_EQ(knl_prc_lookup(pid, va + ( 12ULL << 20 )), ~(UD)0);	/* the page of nothing */
	/* it keeps to its pages while it is mapped */
	KT_ASSERT_ER(ob_trn_rec(k, 0, 13ULL << 20), E_LIMIT);
	ob_cls_obj(k);			/* the mapping holds it */
	KT_ASSERT_EQ(knl_obmem_stat(NULL, NULL, 0, NULL), n0 + 1);
	ob_cls_obj(kp);
	(void)ts_ter_prc(pid, TS_ABORT_TERM);
	(void)wait_code(pid);
	KT_ASSERT_EQ(knl_obmem_stat(&pa, NULL, 0, NULL), n0);
	KT_ASSERT_EQ(pa, pg0);
}

/* ---------------------------------------------------------------- between processes */

/*
 * A makes 64KB and hands B a writable and a read-only key and C a
 * read-only one; each reads what A wrote, B writes back, B cannot map
 * the read-only one to write, and C's write ends it by the fault. Then
 * A's mappings are made where one given back was. Every object and
 * page comes back when the three are collected.
 */
LOCAL void test_between( void )
{
	char	arg[32];
	UD	pg0, pg, free0, free1 = 0;
	INT	n0, at = 0, ca, cb, cc, i;
	ID	pa, pb, pc;

	if ( !have_prog ) KT_SKIP("no program");
	n0 = knl_obmem_stat(&pg0, NULL, 0, NULL);
	free0 = pages_free();
	pb = start_mode("shmb", 0);
	pc = start_mode("shmc", 0);
	KT_ASSERT(pb > 0 && pc > 0);
	if ( pb <= 0 || pc <= 0 ) return;
	knl_memcpy(arg, "shma ", 5);
	at = 5;
	put_num(arg, &at, pb);
	arg[at++] = ' ';
	put_num(arg, &at, pc);
	arg[at] = '\0';
	pa = start_mode(arg, 0);
	KT_ASSERT(pa > 0);
	if ( pa <= 0 ) return;
	ca = wait_code(pa);
	cb = wait_code(pb);
	cc = wait_code(pc);
	tm_printf((UB *)"  A %d, B %d, C %d\n", ca, cb, cc);
	KT_ASSERT_EQ(ca, 0);
	KT_ASSERT_EQ(cb, 0);
	KT_ASSERT_EQ(cc, TS_ABORT_FAULT);

	/* the last key and mapping gone: no object left, and the pages free again */
	KT_ASSERT_EQ(knl_obmem_stat(&pg, NULL, 0, NULL), n0);
	KT_ASSERT_EQ(pg, pg0);
	for ( i = 0; i < 20; i++ ) {
		free1 = pages_free();
		if ( free1 >= free0 ) break;
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  free pages %d -> %d\n", (INT)free0, (INT)free1);
	KT_ASSERT_EQ(free1, free0);
}

/* ---------------------------------------------------------------- ending with the parent */

LOCAL INT state_of( ID pid )
{
	T_RPRC	r;

	return ( ts_ref_prc(pid, &r) < E_OK ) ? PS_NONEXIST : (INT)r.state;
}

/*
 * A parent of four (cxxprog "pdp"), each started as a process object of
 * the program made a program object here: one started to end with it
 * (the metadata), one made to through its process object, one ended by
 * kill and collected by waitpid, one left alone. When the parent ends,
 * the first two end (TS_ABORT_TERM) and the last goes on.
 */
LOCAL void test_with_parent( void )
{
	T_TSMSG	msg;
	TS_UUID	prog;
	ID	pids[64], kids[4], with[2] = { 0, 0 }, alone = 0, pp;
	INT	n, i, nk = 0, nw = 0, code, k;
	T_RPRC	r;

	if ( !have_prog ) KT_SKIP("no program");
	if ( xf_import_path(PROG, "boot", NULL, NULL, NULL, &prog) < E_OK ) KT_SKIP("no store for a program object");
	pp = start_mode("pdp", 0);
	KT_ASSERT(pp > 0);
	if ( pp <= 0 ) {
		(void)ob_del_obj(&prog);
		return;
	}
	knl_memset(&msg, 0, sizeof(msg));
	msg.type = 1;
	msg.size = sizeof(TS_UUID);
	knl_memcpy(msg.body, &prog, sizeof(TS_UUID));
	KT_ASSERT_ER(ts_snd_msg(pp, &msg, 1000), E_OK);
	/* its three children, once the one it was to end with is marked and the killed one gone */
	for ( k = 0; k < 600; k++ ) {
		nk = nw = 0;
		n = knl_prc_list(pids, 64);
		for ( i = 0; i < n; i++ ) {
			if ( ts_ref_prc(pids[i], &r) >= E_OK && r.ppid == pp && r.state == PS_RUNNING && nk < 4 ) {
				kids[nk++] = pids[i];
				if ( knl_prc_withpar(pids[i]) == 1 ) nw++;
			}
		}
		if ( nk == 3 && nw == 2 ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(nk, 3);
	KT_ASSERT_EQ(nw, 2);
	for ( i = 0, nw = 0; i < nk; i++ ) {
		if ( knl_prc_withpar(kids[i]) == 1 && nw < 2 ) with[nw++] = kids[i];
		else alone = kids[i];
	}
	knl_memset(&msg, 0, sizeof(msg));
	msg.type = 1;
	msg.size = 1;
	KT_ASSERT_ER(ts_snd_msg(pp, &msg, 1000), E_OK);
	code = wait_code(pp);
	KT_ASSERT_EQ(code, 0);

	/* the two end with it; the third goes on */
	for ( k = 0; k < 100; k++ ) {
		if ( state_of(with[0]) != PS_RUNNING && state_of(with[1]) != PS_RUNNING ) break;
		tk_dly_tsk(50);
	}
	for ( i = 0; i < 2; i++ ) {
		KT_ASSERT(with[i] > 0 && state_of(with[i]) != PS_RUNNING);
		if ( with[i] > 0 ) KT_ASSERT_EQ(wait_code(with[i]), TS_ABORT_TERM);
	}
	KT_ASSERT(alone > 0 && state_of(alone) == PS_RUNNING);
	if ( alone > 0 ) {
		/* its parent collected, it has none to end with any more */
		KT_ASSERT_ER(knl_prc_set_withpar(alone, TRUE), E_OBJ);
		(void)ts_snd_msg(alone, &msg, 1000);
		KT_ASSERT_EQ(wait_code(alone), 7);
	}
	(void)ob_del_obj(&prog);
}

EXPORT void ktest_shm2( void )
{
	KT_RUN(test_present);
	KT_RUN(test_va_reuse);
	KT_RUN(test_pages);
	KT_RUN(test_map_big);
	KT_RUN(test_between);
	KT_RUN(test_with_parent);
}
