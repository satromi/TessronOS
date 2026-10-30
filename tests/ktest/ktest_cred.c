/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_cred.c
 *	Who a program acts as (design 8.7): what the desktop starts acts
 *	for the person at the machine, not as an administrator, and is
 *	given the administrator's rights only for a while.
 *
 *	- desktop start: a program object started by the kernel, as the
 *	  desktop starts one (ob_cre_obj of a process object), acts as
 *	  利用者; the time, the network, the volumes, the clock and the
 *	  network's settings are refused it, what is the person's to set is
 *	  let; given the rights it sets the time, gives them back and is
 *	  refused again, and rights given for a while go by themselves
 *	  (tests/uprog/svcprog.c SP_CRED).
 *	- system start: a process the kernel starts with ts_cre_prc acts
 *	  as the system, as the system's own services do.
 *	- session: the user logged in at the desktop is whom its programs
 *	  act for, the administrators' group left out.
 *	- password: 管理者 is made the first time it is asked for, has no
 *	  password until one is set, and only the first is set so; the
 *	  right one is let, a wrong one is not.
 *	- trusted: only a helper of the system's, on the system's volume
 *	  and closed to others, is trusted with the system's rights.
 *	- maker: what the desktop's task makes is the person's, what
 *	  another task of the kernel makes the system's.
 *	- media: a USB disk of which the system holds nothing is the
 *	  ordinary users' to read and write; the disk the system runs on
 *	  is not.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "obj/obj.h"

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_TKHOLD	15		/* as tests/uprog/svcprog.c */
#define SP_CRED		32
#define ADM_SHORT_MS	300		/* rights given for a while only */

LOCAL BOOL same( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( ts_uuid_cmp(a, b) == 0 );
}

LOCAL BOOL in_group( CONST T_OBCRD *c, CONST TS_UUID *g )
{
	INT	i;

	for ( i = 0; i < c->ngrp && i < CNF_OB_MAX_GROUPS; i++ ) {
		if ( same(&c->grp[i], g) ) return TRUE;
	}
	return FALSE;
}

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL have_svcprog( void )
{
	T_FSTAT		st;

	return ( fs_stat(SVCPROG, &st) >= EX_OK );
}

/* A program object of SVCPROG, as a program the desktop starts is */
LOCAL ER make_prog( TS_UUID *prog )
{
	CONST char	*meta = "{\"name\":\"ktcred\",\"refCount\":0,"
				"\"tessronos\":{\"exec\":{\"record\":0,\"arch\":\"aarch64\"}}}";
	T_OBCRE		c;
	UB		*buf;
	SZ		asz;
	INT		fd, n, total = 0, recno;
	ID		key;
	ER		er = E_OK;

	fd = fs_open(SVCPROG, O_RDONLY);
	if ( fd < 0 ) return E_NOEXS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)meta;
	c.jsonsz = s_len(meta);
	er = ob_cre_obj(&c, prog);
	if ( er < E_OK ) {
		fs_close(fd);
		return er;
	}
	key = ob_opn_obj(prog, OB_OP_R | OB_OP_W);
	buf = (UB *)Kmalloc(4096);
	if ( key <= 0 || buf == NULL || ob_apd_rec(key, OB_RT_PROG, 0, &recno) < E_OK ) {
		er = E_OBJ;
	}
	while ( er >= E_OK && ( n = fs_read(fd, buf, 4096) ) > 0 ) {
		er = ob_wri_rec(key, recno, total, buf, n, &asz);
		total += n;
	}
	fs_close(fd);
	if ( buf != NULL ) Kfree(buf);
	if ( key > 0 ) (void)ob_cls_obj(key);
	if ( er < E_OK ) (void)ob_del_obj(prog);
	return er;
}

/* The process a program object was started as, by the kernel: its id, or < 0 */
LOCAL ID start_obj( CONST TS_UUID *prog, UW *arg )
{
	T_OBCRE	c;
	TS_UUID	pu;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = *prog;
	c.arg = arg;
	c.argsz = 8 * sizeof(UW);
	er = ob_cre_obj(&c, &pu);
	return ( er < E_OK ) ? (ID)er : knl_prc_of_uuid(&pu);
}

LOCAL INT end_of( ID pid, TMO tmo )
{
	T_PSTS	psts;

	psts.exitcd = -1;
	if ( ts_wai_prc(pid, &psts, tmo) < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -2000;
	}
	if ( psts.exitcd != 0 ) {
		tm_printf((UB *)"  the program stopped at step %d\n", psts.exitcd);
	}
	return psts.exitcd;
}

/* ---------------------------------------------------------------- the desktop's way */

LOCAL void test_desktop_start( void )
{
	CONST T_OBCRD	*c;
	TS_UUID		prog;
	UW		arg[8];
	ID		pid;
	ER		er;

	er = make_prog(&prog);
	if ( er == E_NOEXS ) KT_SKIP("no " SVCPROG);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_CRED;
	arg[1] = ADM_SHORT_MS;
	pid = start_obj(&prog, arg);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) {
		(void)ob_del_obj(&prog);
		return;
	}
	/* 利用者 from its first instruction, of no administrators' group */
	c = knl_ob_crd_of(pid);
	KT_ASSERT(same(&c->user, &ob_user_person));
	KT_ASSERT(in_group(c, &ob_group_users));
	KT_ASSERT(!knl_ob_is_admin(c));
	KT_ASSERT(!knl_ob_adm_held(pid));

	/* what it is refused and let as 利用者 done, the rights for the time */
	tk_dly_tsk(2500);
	KT_ASSERT_ER(knl_ob_adm_give(pid, 0), E_OK);
	KT_ASSERT(knl_ob_adm_held(pid));
	KT_ASSERT(knl_ob_pid_admin(pid));

	/* given back by the program; then given for a while only */
	tk_dly_tsk(1500);
	KT_ASSERT(!knl_ob_adm_held(pid));
	KT_ASSERT(!knl_ob_pid_admin(pid));
	KT_ASSERT_ER(knl_ob_adm_give(pid, ADM_SHORT_MS), E_OK);
	tk_dly_tsk(ADM_SHORT_MS + 200);
	KT_ASSERT(!knl_ob_pid_admin(pid));		/* gone by itself */

	KT_ASSERT_EQ(end_of(pid, 20000), 0);
	KT_ASSERT_ER(ob_del_obj(&prog), E_OK);
}

/* ---------------------------------------------------------------- the kernel's own */

LOCAL ID start_path( UW what, ATR atr )
{
	T_CPRC	cprc;
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = what;
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.pri = KT_PRI_HIGH;
	cprc.prcatr = atr;
	cprc.arg = arg;
	cprc.argsz = sizeof(arg);
	return ts_cre_prc(SVCPROG, &cprc);
}

LOCAL void stop( ID pid )
{
	T_PSTS	psts;

	if ( pid > 0 ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
}

LOCAL void test_system_start( void )
{
	CONST T_OBCRD	*c;
	ID		pid;
	T_RPRC		r;

	if ( !have_svcprog() ) KT_SKIP("no " SVCPROG);
	pid = start_path(SP_TKHOLD, 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(ts_ref_prc(pid, &r), E_OK);
	c = knl_ob_crd_of(pid);
	KT_ASSERT(same(&c->user, &ob_user_system));
	KT_ASSERT(knl_ob_is_admin(c));
	stop(pid);

	/* the person's when the kernel says so */
	pid = start_path(SP_TKHOLD, TS_PRC_USER);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	c = knl_ob_crd_of(pid);
	KT_ASSERT(same(&c->user, &ob_user_person));
	KT_ASSERT(!knl_ob_is_admin(c));
	stop(pid);
}

/* ---------------------------------------------------------------- the person logged in */

LOCAL void test_session( void )
{
	CONST char	*fmt = "{\"name\":\"ktcred\",\"tessronos\":{\"user\":{\"name\":\"ktcred\",\"groups\":[";
	T_OBCRD		*crd;
	CONST T_OBCRD	*s, *c;
	T_OBCRE		oc;
	TS_UUID		user;
	UB		meta[256];
	INT		n;
	ID		pid;

	if ( !have_svcprog() ) KT_SKIP("no " SVCPROG);
	n = knl_oj_put(meta, 0, sizeof(meta), fmt);
	n = knl_oj_put_uuid(meta, n, sizeof(meta), &ob_group_users);
	n = knl_oj_put(meta, n, sizeof(meta), ",");
	n = knl_oj_put_uuid(meta, n, sizeof(meta), &ob_group_admin);
	n = knl_oj_put(meta, n, sizeof(meta), "]}}}");
	knl_memset(&oc, 0, sizeof(oc));
	oc.type = OB_T_STORAGE;
	oc.sub = OB_S_FILE;
	oc.json = meta;
	oc.jsonsz = n;
	if ( ob_cre_obj(&oc, &user) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&user, (CONST UB *)"ktest-pw"), E_OK);
	crd = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	KT_ASSERT(crd != NULL);
	if ( crd == NULL ) return;

	/* no one: 利用者 */
	KT_ASSERT(same(&knl_ob_session_crd()->user, &ob_user_person));

	/* logged in: that user, without the administrators' group */
	KT_ASSERT_ER(knl_ob_login_check(&user, (CONST UB *)"ktest-pw", crd), E_OK);
	KT_ASSERT(knl_ob_is_admin(crd));
	KT_ASSERT_ER(knl_ob_session(crd), E_OK);
	s = knl_ob_session_crd();
	KT_ASSERT(same(&s->user, &user));
	KT_ASSERT(in_group(s, &ob_group_users));
	KT_ASSERT(!knl_ob_is_admin(s));
	pid = start_path(SP_TKHOLD, TS_PRC_USER);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		c = knl_ob_crd_of(pid);
		KT_ASSERT(same(&c->user, &user));
		KT_ASSERT(!knl_ob_is_admin(c));
		stop(pid);
	}
	/* the system is never the session's */
	KT_ASSERT_ER(knl_ob_session(&knl_ob_crd_system), E_PAR);

	/* logged out */
	KT_ASSERT_ER(knl_ob_session(NULL), E_OK);
	KT_ASSERT(same(&knl_ob_session_crd()->user, &ob_user_person));
	Kfree(crd);
	KT_ASSERT_ER(ob_del_obj(&user), E_OK);
}

/* ---------------------------------------------------------------- the administrator's password */

/* 管理者 put back as it is made: no password */
LOCAL ER admin_unset( void )
{
	UB	*j;
	INT	len = 0, n;
	ER	er;

	j = (UB *)Kmalloc(KT_META_ROOM);
	if ( j == NULL ) return E_NOMEM;
	er = knl_ob_meta_read(&ob_user_admin, j, KT_META_ROOM, &len);
	if ( er >= E_OK ) {
		n = knl_oj_put(j, 0, KT_META_ROOM, "{\"name\":\"管理者\",\"refCount\":0,\"recordCount\":0,"
			       "\"editable\":true,\"deletable\":false,\"readable\":true,"
			       "\"tessronos\":{\"user\":{\"name\":\"管理者\",\"groups\":[");
		n = knl_oj_put_uuid(j, n, KT_META_ROOM, &ob_group_admin);
		n = knl_oj_put(j, n, KT_META_ROOM, ",");
		n = knl_oj_put_uuid(j, n, KT_META_ROOM, &ob_group_users);
		n = knl_oj_put(j, n, KT_META_ROOM, "]},\"access\":{\"owner\":");
		n = knl_oj_put_uuid(j, n, KT_META_ROOM, &ob_user_system);
		n = knl_oj_put(j, n, KT_META_ROOM, ",\"group\":");
		n = knl_oj_put_uuid(j, n, KT_META_ROOM, &ob_group_admin);
		n = knl_oj_put(j, n, KT_META_ROOM, ",\"mode\":\"rw-------\",\"attr\":[\"perm\"],\"acl\":[],\"records\":[]}}}");
		er = ( n > 0 ) ? knl_ob_meta_write(&ob_user_admin, j, n) : E_LIMIT;
	}
	Kfree(j);
	return er;
}

LOCAL void test_password( void )
{
	static UD	p_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*p = (T_OBPRT *)p_b;
	ER	er;

	er = knl_ob_admin_check(NULL);
	if ( er != E_OK && er != E_NOEXS ) KT_SKIP("no store for 管理者");
	/* made on the system's volume, the system's and closed to others */
	KT_ASSERT_ER(ob_get_prt(&ob_user_admin, p, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT(same(&p->owner, &ob_user_system));
	KT_ASSERT_EQ(p->mode, 0600);
	KT_ASSERT_ER(admin_unset(), E_OK);
	KT_ASSERT_ER(knl_ob_admin_check(NULL), E_NOEXS);
	KT_ASSERT_ER(knl_ob_admin_check((CONST UB *)"anything"), E_NOEXS);

	/* the first one chosen; a second is not set so */
	KT_ASSERT_ER(knl_ob_admin_set((CONST UB *)""), E_PAR);
	KT_ASSERT_ER(knl_ob_admin_set((CONST UB *)"ktest-admin"), E_OK);
	KT_ASSERT_ER(knl_ob_admin_check(NULL), E_OK);
	KT_ASSERT_ER(knl_ob_admin_set((CONST UB *)"another"), E_OBJ);
	KT_ASSERT_ER(knl_ob_admin_check((CONST UB *)"ktest-admin"), E_OK);
	KT_ASSERT_ER(knl_ob_admin_check((CONST UB *)"Ktest-admin"), E_OACV);
	KT_ASSERT_ER(knl_ob_admin_check((CONST UB *)""), E_OACV);

	/* left as the machine had it: no password, for the person to choose */
	KT_ASSERT_ER(admin_unset(), E_OK);
	KT_ASSERT_ER(knl_ob_admin_check(NULL), E_NOEXS);
}

/* ---------------------------------------------------------------- trusted helpers */

LOCAL void test_trusted( void )
{
	TS_UUID	rtm, prog;
	T_OBREF	r;

	KT_ASSERT(!knl_ob_trusted(NULL));
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_RTMEDIA, &rtm), E_OK);
	if ( ob_ref_obj(&rtm, &r) >= E_OK ) {
		KT_ASSERT(knl_ob_trusted(&rtm));
	}
	/* a program of the store, made by anyone: not one */
	if ( make_prog(&prog) >= E_OK ) {
		KT_ASSERT(!knl_ob_trusted(&prog));
		KT_ASSERT_ER(ob_del_obj(&prog), E_OK);
	}
	/* a program of the system's own that is not a helper: not one */
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_USERENV, &prog), E_OK);
	KT_ASSERT(!knl_ob_trusted(&prog));
}

/* ---------------------------------------------------------------- what the desktop makes */

LOCAL void test_maker( void )
{
	T_OBCRE	c;
	static UD	p_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*p = (T_OBPRT *)p_b;
	TS_UUID	u;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.name = (CONST UB *)"ktcred maker";
	if ( ob_cre_obj(&c, &u) < E_OK ) KT_SKIP("no store");
	KT_ASSERT_ER(ob_get_prt(&u, p, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT(same(&p->owner, &ob_user_system));
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);

	knl_ob_make_for_person(TRUE);
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	knl_ob_make_for_person(FALSE);
	KT_ASSERT_ER(ob_get_prt(&u, p, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT(same(&p->owner, &knl_ob_session_crd()->user));
	KT_ASSERT(same(&p->group, &ob_group_users));
	KT_ASSERT_EQ(p->mode, 0644);
	/* the person's programs may change it */
	KT_ASSERT((knl_ob_permit_on(&u, &knl_ob_crd_person) & OB_OP_WRITE) != 0);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

/* ---------------------------------------------------------------- media */

LOCAL void test_media( void )
{
	TS_UUID	*ids;
	T_OBREF	r;
	INT	cnt = 0, i, usb = 0, sys = 0;
	UINT	ops;

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	if ( ids == NULL ) KT_SKIP("no memory");
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, ids, 256, &cnt) < E_OK ) {
		Kfree(ids);
		KT_SKIP("no list of the disks");
	}
	for ( i = 0; i < cnt && i < 256; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) < E_OK || r.sub != OB_S_DISK ) continue;
		ops = knl_ob_permit_on(&ids[i], &knl_ob_crd_person);
#if defined(X64)
		if ( r.name[0] == 'n' && r.name[1] == 'v' ) {
#else
		if ( r.name[0] == 'v' || ( r.name[0] == 's' && r.name[1] == 'd' ) ) {
#endif
			/* the disk the machine runs on: the administrators' alone */
			KT_ASSERT_EQ(ops & ( OB_OP_READ | OB_OP_WRITE ), 0);
			sys++;
		} else if ( r.name[0] == 'u' && r.name[1] == 'd' ) {
			tm_printf((UB *)"  %s: %s\n", r.name,
				  ( ops & OB_OP_WRITE ) ? "the person's" : "held by the system");
			usb++;
		}
	}
	Kfree(ids);
	tm_printf((UB *)"  %d disks of the machine, %d of USB\n", sys, usb);
	KT_ASSERT(sys > 0);
}

EXPORT void ktest_cred( void )
{
	tm_printf((UB *)"\n[cred] who a program acts as (design 8.7)\n");
	KT_RUN(test_system_start);
	KT_RUN(test_desktop_start);
	KT_RUN(test_session);
	KT_RUN(test_password);
	KT_RUN(test_trusted);
	KT_RUN(test_maker);
	KT_RUN(test_media);
}
