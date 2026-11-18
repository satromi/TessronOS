/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_prcobj.c
 *	What a process may do to objects of the core and to other processes
 *	(design 9.7, 18.8, 18.19 stages 8g and 8h), through the program
 *	tests/uprog/svcprog.c (/boot/SVCPROG.ELF):
 *
 *	- tkown: a process makes a semaphore, an event flag, a mutex, a
 *	  message buffer and tasks of its own and uses them, is refused what
 *	  a process has none of, fills its room for semaphores and tasks,
 *	  and is ended -- by returning, by a fault, by ts_ter_prc -- with
 *	  every one of them gone after.
 *	- foreign: another process tries the core's calls on the first
 *	  one's main task and semaphore and on a semaphore of the kernel's,
 *	  and is refused each; as the administrator it is by its parent it
 *	  ends a third process and sends the first a message; logged in as
 *	  a user who is not an administrator it may not end the first, send
 *	  to it, collect it or hand it a key. The first is untouched.
 *	- kill draw, kill menu: a process drawing without end in a window
 *	  of its own, or waiting in its menu, is ended again and again; the
 *	  screen and the panels hold together after, and another window
 *	  still draws.
 *	- surf: a window drawn through a surface mapped into its process,
 *	  made larger, closed under the process and ended with it: the
 *	  surface follows, and its pages are let go of each time.
 *	- dt ptr: the calendar and the console refuse pointers that are not
 *	  the process's.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>

IMPORT UW	knl_prc_ter_inside;

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_TKOWN	14		/* as tests/uprog/svcprog.c */
#define SP_TKHOLD	15
#define SP_TKATK	16
#define SP_KILLDRAW	19
#define SP_KILLMENU	20
#define SP_DTPTR	21
#define SP_KILLWIN	24
#define TK_SEM_ROOM	TS_PRC_SEM_MAX
#define TK_TSK_ROOM	TS_PRC_TSK_MAX

LOCAL BOOL	have_prog = FALSE;

LOCAL ID start_at( UW *arg, PRI pri )
{
	T_CPRC	cprc;

	cprc.pri    = pri;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(UW) * 8;
	return ts_cre_prc(SVCPROG, &cprc);
}

LOCAL ID start_args( UW *arg )
{
	return start_at(arg, KT_PRI_HIGH);
}

LOCAL ID start_prog( UW mode )
{
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	return start_args(arg);
}

/* The objects of each kind the process has, all of them */
LOCAL INT owned( ID pid )
{
	INT	kind, n = 0;

	for ( kind = TK_OWN_TSK; kind <= TK_OWN_MBF; kind++ ) {
		n += knl_own_list(kind, pid, NULL, 0);
	}
	return n;
}

LOCAL ER send( ID pid, UINT type, CONST void *body, SZ size )
{
	T_TSMSG	*m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	ER	er;

	if ( m == NULL ) {
		return E_NOMEM;
	}
	knl_memset(m, 0, sizeof(T_TSMSG));
	m->type = type;
	m->size = size;
	if ( size > 0 ) {
		knl_memcpy(m->body, body, (INT)size);
	}
	er = ts_snd_msg(pid, m, 1000);
	Kfree(m);
	return er;
}

/* Until the process has so many semaphores and tasks, or has ended */
LOCAL BOOL wait_owned( ID pid, INT nsem, INT ntsk )
{
	T_RPRC	r;
	INT	t;

	for ( t = 0; t < 100; t++ ) {
		if ( knl_own_list(TK_OWN_SEM, pid, NULL, 0) >= nsem
		  && knl_own_list(TK_OWN_TSK, pid, NULL, 0) >= ntsk ) {
			return TRUE;
		}
		if ( ts_ref_prc(pid, &r) < E_OK || r.state != PS_RUNNING ) {
			return FALSE;
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL void test_program( void )
{
	T_FSTAT	st;

	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	KT_ASSERT(st.size > 0);
	have_prog = TRUE;
}

/* ---------------------------------------------------------------- its own */

/*
 * The process's own objects, and what is left of them when it has
 * ended: 'how' 0 it returns, 1 it faults, 2 the kernel ends it.
 */
LOCAL void own_run( UB how )
{
	T_PSTS	psts;
	T_RPRC	r;
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	pid = start_prog(SP_TKOWN);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	KT_ASSERT(wait_owned(pid, TK_SEM_ROOM, TK_TSK_ROOM));
	KT_ASSERT_EQ(knl_own_list(TK_OWN_SEM, pid, NULL, 0), TK_SEM_ROOM);
	KT_ASSERT_EQ(knl_own_list(TK_OWN_TSK, pid, NULL, 0), TK_TSK_ROOM);
	KT_ASSERT_EQ(knl_own_list(TK_OWN_FLG, pid, NULL, 0), 1);
	KT_ASSERT_EQ(knl_own_list(TK_OWN_MTX, pid, NULL, 0), 1);
	KT_ASSERT_EQ(knl_own_list(TK_OWN_MBF, pid, NULL, 0), 1);
	KT_ASSERT_ER(ts_ref_prc(pid, &r), E_OK);
	KT_ASSERT_EQ(r.ntsk, TK_TSK_ROOM);

	if ( how == 2 ) {
		KT_ASSERT_ER(send(pid, 1, &how, 1), E_OK);
		tk_dly_tsk(100);			/* in its second wait by now */
		KT_ASSERT_ER(ts_ter_prc(pid, -9), E_OK);
	} else {
		KT_ASSERT_ER(send(pid, 1, &how, 1), E_OK);
	}
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	tm_printf((UB*)"  the program ended with %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, ( how == 0 ) ? 0 : ( how == 1 ) ? TS_ABORT_FAULT : -9);
	KT_ASSERT_EQ(owned(pid), 0);
}

LOCAL void test_tkown_exit( void )
{
	own_run(0);
}

LOCAL void test_tkown_fault( void )
{
	own_run(1);
}

LOCAL void test_tkown_ter( void )
{
	own_run(2);
}

/* ---------------------------------------------------------------- another's */

LOCAL void test_foreign( void )
{
	T_OBCRE	c;
	T_CSEM	cs;
	T_RSEM	rs;
	T_RTSK	rt;
	T_RPRC	r;
	T_PSTS	psts;
	TS_UUID	u;
	UW	arg[8];
	UINT	id[5];
	UB	meta[160];
	INT	n;
	ID	b, cp, a, ks, bsem = 0;
	CONST char *j = "{\"name\":\"ktuser2\",\"tessronos\":{\"user\":{\"name\":\"ktuser2\",\"groups\":[]}}}";

	if ( !have_prog ) KT_SKIP("no program");
	n = (INT)knl_strlen(j);
	knl_memcpy(meta, j, n);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = meta;
	c.jsonsz = n;
	if ( ob_cre_obj(&c, &u) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&u, (CONST UB *)"ktest-pw"), E_OK);

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 4;
	ks = tk_cre_sem(&cs);
	KT_ASSERT(ks > 0);

	b = start_prog(SP_TKHOLD);
	cp = start_prog(SP_TKHOLD);
	KT_ASSERT(b > 0 && cp > 0);
	KT_ASSERT(wait_owned(b, 1, 1) && wait_owned(cp, 1, 1));
	KT_ASSERT_EQ(knl_own_list(TK_OWN_SEM, b, &bsem, 1), 1);
	KT_ASSERT_ER(ts_ref_prc(b, &r), E_OK);

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_TKATK;
	knl_memcpy(&arg[1], &u, sizeof(u));
	a = start_args(arg);
	KT_ASSERT(a > 0);
	id[0] = (UINT)b;
	id[1] = (UINT)r.maintsk;
	id[2] = (UINT)bsem;
	id[3] = (UINT)ks;
	id[4] = (UINT)cp;
	KT_ASSERT_ER(send(a, 1, id, sizeof(id)), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(a, &psts, 10000), E_OK);
	tm_printf((UB*)"  the one that tried ended with %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(owned(a), 0);

	/* the third, ended by it */
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(cp, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, -9);
	KT_ASSERT_EQ(owned(cp), 0);

	/* the first, and the kernel's semaphore, as they were */
	KT_ASSERT_ER(ts_ref_prc(b, &r), E_OK);
	KT_ASSERT_EQ(r.state, PS_RUNNING);
	KT_ASSERT_ER(tk_ref_tsk(r.maintsk, &rt), E_OK);
	KT_ASSERT_EQ(rt.suscnt, 0);
	KT_ASSERT_ER(tk_ref_sem(bsem, &rs), E_OK);
	KT_ASSERT_EQ(rs.semcnt, 0);
	KT_ASSERT_ER(tk_ref_sem(ks, &rs), E_OK);
	KT_ASSERT_EQ(rs.semcnt, 0);

	KT_ASSERT_ER(send(b, 2, NULL, 0), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(b, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);			/* the one message it was sent, no more */
	KT_ASSERT_EQ(owned(b), 0);

	KT_ASSERT_ER(tk_del_sem(ks), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

/* ---------------------------------------------------------------- ended while drawing */

#define KILL_RUNS	8

/* A window by its name; FALSE when there is none */
LOCAL BOOL window_named( CONST char *name, TS_UUID *out )
{
	TS_UUID	*list;
	T_OBREF	r;
	INT	cnt, i;
	BOOL	found = FALSE;

	if ( ( cnt = kt_lst_obj(OB_T_WINDOW, 0, &list) ) < 0 ) return FALSE;
	for ( i = 0; i < cnt && !found; i++ ) {
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW
		  && kt_name_is(&list[i], name) ) {
			*out = list[i];
			found = TRUE;
		}
	}
	Kfree(list);
	return found;
}

/* Panels open now: a menu is one */
LOCAL INT panels( void )
{
	INT	pid, n = 0;

	for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) {
		if ( wm_panel_wid(pid) > 0 ) n++;
	}
	return n;
}

/*
 * Every window of the screen is an object whose window is there, the
 * screen's order and the panels hold together, and a window of the
 * kernel's still draws: a colour put in it is read back.
 */
LOCAL void screen_whole( INT wid, UW colour )
{
	TS_UUID		*list;
	T_OBREF		r;
	T_DPRECT	box;
	UW		px = 0;
	INT		cnt, i, gid = wm_gid(wid);

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	cnt = kt_lst_obj(OB_T_WINDOW, 0, &list);
	KT_ASSERT(cnt >= 0);
	for ( i = 0; i < cnt; i++ ) {
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW ) {
			KT_ASSERT(wm_obj_wid(&list[i]) > 0);
		}
	}
	if ( cnt >= 0 ) Kfree(list);
	KT_ASSERT(gid >= 0);
	if ( gid < 0 ) return;
	box.left = 10;
	box.top = 10;
	box.right = 60;
	box.bottom = 40;
	KT_ASSERT_ER(dp_fill_rect(gid, &box, colour), E_OK);
	KT_ASSERT_ER(dp_get_argb(gid, 30, 20, &px, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(px & 0xFFFFFF, colour & 0xFFFFFF);
	KT_ASSERT_ER(wm_damage(wid, &box), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);
}

/*
 * A process drawing without end, or waiting in its menu, ended by
 * ts_ter_prc again and again at moments that differ. A task inside a
 * call of drawing or of the windows is not taken down there: the
 * process ends as the call returns, a menu it waits in is put away
 * first, and nothing of the call is left behind.
 */
LOCAL void kill_draw_runs( UW mode, CONST TS_UUID *def )
{
	T_PSTS		psts;
	T_DPRECT	o;
	TS_UUID		win;
	SYSTIM		t0, t1;
	UW		arg[8];
	UW		inside = knl_prc_ter_inside;
	INT		r, t, ms, worst = 0, wid, npanel = panels();
	ID		pid;

	if ( !have_prog ) KT_SKIP("no program");
	o.left = 40;  o.top = 300;  o.right = 240;  o.bottom = 420;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "kernel's");
	if ( wid <= 0 ) KT_SKIP("no screen");
	for ( r = 0; r < KILL_RUNS; r++ ) {
		knl_memset(arg, 0, sizeof(arg));
		arg[0] = mode;
		if ( def != NULL ) knl_memcpy(&arg[1], def, sizeof(TS_UUID));
		pid = start_at(arg, KT_PRI_LOW);	/* it never waits: below the test */
		KT_ASSERT(pid > 0);
		if ( pid <= 0 ) break;
		KT_ASSERT(wait_owned(pid, 1, 0));		/* drawing, or its menu made */
		if ( def != NULL ) {
			for ( t = 0; t < 50 && panels() == npanel; t++ ) tk_dly_tsk(20);
			KT_ASSERT_EQ(panels(), npanel + 1);	/* the menu is up */
		}
		tk_dly_tsk(20 + ( r * 137 ) % 400);
		tk_get_tim(&t0);
		KT_ASSERT_ER(ts_ter_prc(pid, -7), E_OK);
		psts.exitcd = 0;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
		tk_get_tim(&t1);
		ms = (INT)( t1.lo - t0.lo );
		if ( ms > worst ) worst = ms;
		if ( psts.exitcd != -7 ) {
			tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
		}
		KT_ASSERT_EQ(psts.exitcd, -7);
		KT_ASSERT_EQ(owned(pid), 0);
		KT_ASSERT_EQ(panels(), npanel);			/* no menu of its left up */

		/* its window went with it */
		KT_ASSERT(!window_named("ktkill", &win));
		screen_whole(wid, 0x102030 * (UW)( r + 1 ));
	}
	tm_printf((UB*)"  ended %d times, %d of them inside a call, gone at most %d ms"
		   " after ts_ter_prc\n", KILL_RUNS, (INT)( knl_prc_ter_inside - inside ), worst);
	KT_ASSERT(knl_prc_ter_inside - inside > 0);
	KT_ASSERT(worst < 2000);
	(void)wm_close(wid);
	wm_update();
}

LOCAL void test_kill_draw( void )
{
	kill_draw_runs(SP_KILLDRAW, NULL);
}

/*
 * A process that faults with a window and a sub window of that window
 * open, closing neither: both go as it ends, and the screen's order and
 * panels hold together.
 */
LOCAL void test_kill_win( void )
{
	T_PSTS		psts;
	T_WMWIN		ref;
	TS_UUID		win, sub;
	UW		arg[8];
	INT		t;
	ID		pid;

	if ( !have_prog ) KT_SKIP("no program");
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_KILLWIN;
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT(wait_owned(pid, 1, 0));			/* both windows made */
	KT_ASSERT(window_named("ktkill", &win));
	KT_ASSERT(window_named("ktsub", &sub));
	knl_memset(&ref, 0, sizeof(ref));
	KT_ASSERT_ER(wm_ref(wm_obj_wid(&sub), &ref), E_OK);
	KT_ASSERT_EQ(ref.parent, wm_obj_wid(&win));		/* a sub window of the other */
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_FAULT);
	for ( t = 0; t < 20 && ( window_named("ktkill", &win) || window_named("ktsub", &sub) ); t++ ) {
		tk_dly_tsk(50);
	}
	KT_ASSERT(!window_named("ktkill", &win));
	KT_ASSERT(!window_named("ktsub", &sub));
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
}

LOCAL void test_kill_menu( void )
{
	TS_UUID	def;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_MENU_CONSOLE, &def), E_OK);
	kill_draw_runs(SP_KILLMENU, &def);
}

/* ---------------------------------------------------------------- pointers */

#define LOG_MAX		4096

/* Whether the console said w since *pos */
LOCAL BOOL said_since( UD pos, CONST char *w )
{
	UB	*b = (UB *)Kmalloc(LOG_MAX);
	INT	n = 0, k, i, j, len = (INT)knl_strlen(w);
	BOOL	yes = FALSE;

	if ( b == NULL ) return FALSE;
	while ( n < LOG_MAX && ( k = tm_log_read(&pos, b + n, LOG_MAX - n) ) > 0 ) n += k;
	for ( i = 0; i + len <= n && !yes; i++ ) {
		for ( j = 0; j < len && b[i + j] == (UB)w[j]; j++ ) ;
		yes = (BOOL)( j == len );
	}
	Kfree(b);
	return yes;
}

/*
 * The calendar (dt_) and the console (tm_putstring) from EL0: pointers
 * into the kernel, into the process's half where nothing is mapped, and
 * to its read-only text for what is written are refused with E_MACV
 * before anything is touched; the process's own still work, and a line
 * longer than the kernel's pieces comes out whole.
 */
LOCAL void test_dt_ptr( void )
{
	T_PSTS	psts;
	UW	arg[8];
	UD	pos = 0;
	UB	b[64];
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	while ( tm_log_read(&pos, b, sizeof(b)) > 0 ) ;	/* to the end of what was said */
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_DTPTR;
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT(said_since(pos, "xyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef"));
	KT_ASSERT(said_since(pos, "dtptr end"));
}

/*
 * Thirty processes more than are running already: the list of process
 * objects, asked a few at a time from the last one answered (as the
 * console's ps asks), holds every one of them, and the record of each
 * reads, beyond the 16 the list was once made of.
 */
#define SP_BGWAIT	33		/* as tests/uprog/svcprog.c */
#define PL_N		30
#define PL_PAGE		7

LOCAL void test_many_listed( void )
{
	ID	pid[PL_N];
	TS_UUID	u[PL_N], ids[PL_PAGE], last;
	BOOL	seen[PL_N];
	T_PSTS	psts;
	INT	started = 0, i, k, cnt, all = 0, found = 0, nread = 0;
	UB	t[256];
	SZ	asz;

	if ( !have_prog ) KT_SKIP("no program");
	for ( i = 0; i < PL_N; i++ ) {
		pid[i] = start_prog(SP_BGWAIT);
		KT_ASSERT(pid[i] > 0);
		if ( pid[i] <= 0 ) break;
		started++;
		seen[i] = FALSE;
		KT_ASSERT_ER(knl_prc_uuid(pid[i], &u[i], NULL), E_OK);
	}
	do {
		cnt = 0;
		KT_ASSERT_ER(ob_lst_obj(OB_T_PROCESS, 0, ( all > 0 ) ? &last : NULL, ids, PL_PAGE, &cnt), E_OK);
		for ( k = 0; k < cnt; k++ ) {
			for ( i = 0; i < started; i++ ) {
				if ( !seen[i] && ts_uuid_cmp(&ids[k], &u[i]) == 0 ) {
					seen[i] = TRUE;
					found++;
				}
			}
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == PL_PAGE );
	tm_printf((UB *)"  %d process objects listed, %d of the %d started\n", all, found, started);
	KT_ASSERT(all >= started);
	KT_ASSERT(all > 16);
	KT_ASSERT_EQ(found, started);
	for ( i = 0; i < started; i++ ) {
		ID	key = ob_opn_obj(&u[i], OB_OP_READ);

		if ( key <= 0 ) continue;
		asz = 0;
		if ( ob_rea_rec(key, 0, 0, t, sizeof(t) - 1, &asz) >= E_OK && asz > 0 ) nread++;
		(void)ob_cls_obj(key);
	}
	KT_ASSERT_EQ(nread, started);
	for ( i = 0; i < started; i++ ) {
		if ( ts_ter_prc(pid[i], -1) >= E_OK ) {
			(void)ts_wai_prc(pid[i], &psts, 5000);
		}
	}
}

/*
 * A process's own tasks are spread over the processors: four of them,
 * each busy for 300 ms, run side by side on a machine of several and
 * one after another on a machine of one.
 */
#define SP_PARALLEL	23		/* as tests/uprog/svcprog.c */
IMPORT INT	knl_num_prc;

LOCAL void test_parallel( void )
{
	T_PSTS	psts;
	ID	pid;
	INT	want = ( knl_num_prc >= 4 ) ? 3 : knl_num_prc;

	if ( !have_prog ) KT_SKIP("no program");
	pid = start_prog(SP_PARALLEL);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	tm_printf((UB*)"  %d of the process's tasks ran at once on %d processors\n",
		  psts.exitcd - 300, knl_num_prc);
	KT_ASSERT(psts.exitcd > 300 && psts.exitcd <= 304);
	KT_ASSERT(psts.exitcd - 300 >= want);
	if ( knl_num_prc == 1 ) {
		KT_ASSERT_EQ(psts.exitcd, 301);
	}
}

/*
 * A process makes, runs and deletes a task of its own KT_CHURN_N times
 * (SP_TCHURN): the user stacks go where the one before had its, so the
 * space does not grow (design 4.6.2), and all the pages come back when it
 * ends.
 */
#define SP_TCHURN	39		/* as tests/uprog/svcprog.c */
IMPORT UD	knl_pf_free_count( INT zone );
IMPORT BOOL	knl_pt_reclaim( void );
#if TS_NETSTACK_NETBSD
IMPORT UD	knl_rump_pages( void );
#endif

/* Free pages, those the network stack keeps for itself counted as free */
LOCAL UD pages_free( void )
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

LOCAL void test_task_churn( void )
{
	T_PSTS	psts;
	UW	arg[8];
	UD	before, after = 0;
	ID	pid;
	INT	t;

	if ( !have_prog ) KT_SKIP("no program");
	(void)knl_pt_reclaim();
	before = pages_free();
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_TCHURN;
	arg[1] = KT_CHURN_N;
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 300000), E_OK);
	for ( t = 0; t < 50; t++ ) {
		(void)knl_pt_reclaim();
		after = pages_free();
		if ( after == before ) break;
		tk_dly_tsk(100);
	}
	tm_printf((UB*)"  %d tasks made and deleted: ended with %d, free pages %d -> %d\n",
		  (INT)KT_CHURN_N, psts.exitcd, (INT)before, (INT)after);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(after, before);
}

/*
 * Turns among the tasks of one priority (design 4.3.1; SP_SLICE). Six
 * tasks of a process in the time-shared band never wait, on four
 * processors: the one that notes its gaps still runs every few turns
 * of CNF_SLICE_MS. And a task of the real-time band that sleeps and
 * wakes is not held back by them: it wakes on time.
 */
#define SP_SLICE	40		/* as tests/uprog/svcprog.c */
#define SL_TS_PRI	24		/* below the test's own task, in the time-shared band */
#define SL_RT_PRI	14		/* in the real-time band */

LOCAL ID slice_start( PRI pri, UW how, UW ms )
{
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_SLICE;
	arg[1] = how;
	arg[2] = ms;
	return start_at(arg, pri);
}

LOCAL void test_slice( void )
{
	T_PSTS	psts;
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	pid = slice_start(SL_TS_PRI, 0, 1500);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 30000), E_OK);
	tm_printf((UB*)"  six spinning tasks of one priority: the longest one went without running %d ms\n",
		  psts.exitcd);
	KT_ASSERT(psts.exitcd >= 0 && psts.exitcd <= 150);
}

LOCAL void test_slice_rt( void )
{
	T_PSTS	psts, rsts;
	ID	load, rt;

	if ( !have_prog ) KT_SKIP("no program");
	load = slice_start(SL_TS_PRI, 0, 2500);
	KT_ASSERT(load > 0);
	if ( load <= 0 ) return;
	tk_dly_tsk(300);			/* the spinning under way */
	rt = slice_start(SL_RT_PRI, 1, 1500);
	KT_ASSERT(rt > 0);
	rsts.exitcd = -1;
	if ( rt > 0 ) KT_ASSERT_ER(ts_wai_prc(rt, &rsts, 30000), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(load, &psts, 30000), E_OK);
	tm_printf((UB*)"  a real-time task sleeping 2 ms beside them woke at most %d ms late"
		  " (the spinning: %d ms)\n", rsts.exitcd, psts.exitcd);
	/*
	 * Held back by them, it would wake only after turns of the six
	 * (well over 100 ms) or not at all. What an emulator on a busy
	 * host adds on its own is allowed for.
	 */
	KT_ASSERT(rsts.exitcd >= 0 && rsts.exitcd <= 60);
	KT_ASSERT(psts.exitcd >= 0 && psts.exitcd < 4000);
}

/*
 * A window drawn through a surface mapped into its process (wm_obj_surf,
 * wm_obj_put; SP_SURF): what is put is on the screen; when the window is
 * made larger the old surface stays the process's until it asks again,
 * and then the new one is at another address and the old address no
 * longer reaches its pages; when the window closes the surface's pages
 * go and the process, still writing where they were, does not fault and
 * is refused the put; a process ended while it draws leaves no surface.
 */
#define SP_SURF		29		/* as tests/uprog/svcprog.c */

/* The screen's pixel at the middle of a window's work area */
LOCAL UW work_px( INT wid )
{
	T_WMWIN		ref;
	T_DISPSPEC	spec;
	UB		*fb;

	if ( wm_ref(wid, &ref) < E_OK || ts_disp_ref(&spec) < E_OK
	  || ( fb = (UB *)ts_disp_buffer() ) == NULL ) return 0xFFFFFFFF;
	return ((UW *)( fb + (UD)( ( ref.work.top + ref.work.bottom ) / 2 ) * spec.pitch ))
		[( ref.work.left + ref.work.right ) / 2] & 0x00FFFFFF;
}

LOCAL void test_surf( void )
{
	T_PSTS		psts;
	T_WMWIN		ref;
	T_DPRECT	o;
	TS_UUID		win;
	UBINT		va = 0, va2 = 0;
	UD		pg0 = 0, pg = 0, pages = 0, pages2 = 0, pa, pa2;
	UW		arg[8];
	INT		n0, wid;
	ID		pid, owner = 0;

	if ( !have_prog ) KT_SKIP("no program");
	n0 = knl_wm_surf_count(&pg0);
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_SURF;
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT(wait_owned(pid, 1, 0));
	KT_ASSERT(window_named("ktsurf", &win));
	wid = wm_obj_wid(&win);
	KT_ASSERT(wid > 0);
	KT_ASSERT_ER(knl_wm_surf_ref(wid, &owner, &va, &pages), E_OK);
	KT_ASSERT_EQ(owner, pid);
	KT_ASSERT_EQ(knl_wm_surf_count(&pg), n0 + 1);
	KT_ASSERT_EQ(pg, pg0 + pages);
	pa = knl_prc_lookup(pid, va);
	KT_ASSERT(pa != ~(UD)0);
	KT_ASSERT_EQ(work_px(wid), 0x0000C0);

	/* made larger: the old surface is put clipped, then a new one */
	KT_ASSERT_ER(wm_ref(wid, &ref), E_OK);
	o = ref.outer;
	o.right += 80;
	o.bottom += 40;
	KT_ASSERT_ER(wm_move(wid, &o), E_OK);
	KT_ASSERT_ER(send(pid, 1, NULL, 0), E_OK);
	KT_ASSERT(wait_owned(pid, 2, 0));
	KT_ASSERT_ER(knl_wm_surf_ref(wid, &owner, &va2, &pages2), E_OK);
	KT_ASSERT(va2 != va && pages2 > pages);
	KT_ASSERT(knl_prc_lookup(pid, va) != pa);		/* the old pages are out of its reach */
	KT_ASSERT_EQ(knl_wm_surf_count(&pg), n0 + 1);
	KT_ASSERT_EQ(pg, pg0 + pages2);
	KT_ASSERT_EQ(work_px(wid), 0x00C000);
	tm_printf((UB*)"  surface %d pages at 0x%lx, then %d pages at 0x%lx\n", (INT)pages, va,
		  (INT)pages2, va2);

	/* closed under it: the pages go, its writes go nowhere, the put is refused */
	pa2 = knl_prc_lookup(pid, va2);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	(void)wm_update();
	KT_ASSERT_EQ(knl_wm_surf_count(&pg), n0);
	KT_ASSERT_EQ(pg, pg0);
	KT_ASSERT(knl_prc_lookup(pid, va2) != pa2);
	KT_ASSERT_ER(send(pid, 2, NULL, 0), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);

	/* ended while it draws: no surface is left */
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT(wait_owned(pid, 1, 0));
	KT_ASSERT_EQ(knl_wm_surf_count(NULL), n0 + 1);
	KT_ASSERT_ER(send(pid, 3, NULL, 0), E_OK);
	tk_dly_tsk(300);
	KT_ASSERT_ER(ts_ter_prc(pid, -7), E_OK);
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, -7);
	KT_ASSERT_EQ(knl_wm_surf_count(&pg), n0);
	KT_ASSERT_EQ(pg, pg0);
	KT_ASSERT(!window_named("ktsurf", &win));
	KT_ASSERT_EQ(wm_self_check(), 0);
}

EXPORT void ktest_prcobj( void )
{
	KT_RUN(test_program);
	KT_RUN(test_tkown_exit);
	KT_RUN(test_tkown_fault);
	KT_RUN(test_tkown_ter);
	KT_RUN(test_foreign);
	KT_RUN_SCREEN(test_kill_draw);
	KT_RUN_SCREEN(test_kill_menu);
	KT_RUN_SCREEN(test_kill_win);
	KT_RUN_SCREEN(test_surf);
	KT_RUN(test_dt_ptr);
	KT_RUN(test_many_listed);
	KT_RUN(test_parallel);
	KT_RUN(test_task_churn);
	KT_RUN(test_slice);
	KT_RUN(test_slice_rt);
}
