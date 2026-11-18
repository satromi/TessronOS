/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obprc.c
 *	The process manager: running processes as objects (design 18.8)
 *
 *	A process is an object for as long as it lives. Making one is
 *	starting a program object; deleting one ends it. Its metadata says
 *	its number (tessronos.process.pid) for what waits for it or signals
 *	it; its name ("process" and the number) is only shown. Record 0 is what
 *	the process is doing, written as xmlTAD when it is read, with a
 *	virtual object of the program it runs and one of each socket it has
 *	open (obsock.c) and one of each end of a stream it holds a key to
 *	(obstrm.c). Its owner is the user it acts for: that user may
 *	end it, others may only look.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include <ts/so.h>
#include <ts/sysdef.h>
#include <ts/sbx.h>
#include "obj.h"


LOCAL ER obp_find( CONST TS_UUID *uuid )
{
	return ( knl_prc_of_uuid(uuid) > 0 ) ? E_OK : E_NOEXS;
}

LOCAL INT put_name( UB *out, INT max, ID pid )
{
	INT	n = knl_oj_put(out, 0, max, "process ");

	return knl_oj_put_num(out, n, max, pid);
}

/* "process " and a number of an ID: the most such a name takes */
#define OBP_NAME_ROOM	24

LOCAL ER obp_refnam( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	UB	t[OBP_NAME_ROOM];
	ID	pid = knl_prc_of_uuid(uuid);

	if ( pid <= 0 ) {
		return E_NOEXS;
	}
	(void)put_name(t, (INT)sizeof(t), pid);
	return knl_ob_name_put(t, -1, buf, size, p_asize);
}

LOCAL ER obp_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_RPRC	rp;
	ID	pid = knl_prc_of_uuid(uuid);

	if ( pid <= 0 || ts_ref_prc(pid, &rp) < E_OK ) {
		return E_NOEXS;
	}
	r->flags = OB_F_VOLATILE;
	r->nrec = 1;
	r->size = rp.memsz;

	return E_OK;
}

/* The user it acts for owns it: rw-r--r-- */
LOCAL ER obp_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	ID		pid = knl_prc_of_uuid(uuid);
	CONST T_OBCRD	*crd;

	if ( pid <= 0 ) {
		return E_NOEXS;
	}
	crd = knl_ob_crd_of(pid);
	*p_prt = knl_ob_prt_new(0, 0);
	if ( *p_prt == NULL ) {
		return E_NOMEM;
	}
	knl_ob_prt_default(*p_prt, crd);

	return E_OK;
}

/*
 * The programs the system trusts with its own rights when the kernel
 * starts them: helpers the desktop runs for its own work (実時間メディア,
 * what the player of 実時間制御TAD sounds and shows with), which work on
 * the channels and memory objects it makes for them and are never
 * started for a person's own use. None is in a program box. Such a
 * program says so in its metadata (tessronos.program.trusted), and
 * counts as one only when it is also on the volume of the system box,
 * owned by the system, and not to be written or have its protection
 * changed by anyone but an administrator: only those who may make the
 * system's objects can make one, and an object that only says it is one
 * does not count.
 */
LOCAL BOOL obp_says_trusted( CONST TS_UUID *prog )
{
	UB	b[8];
	SZ	asz = 0;
	ID	key = ob_opn_obj(prog, OB_OP_ATRRD);
	BOOL	yes = FALSE;

	if ( key <= 0 ) {
		return FALSE;
	}
	if ( ob_get_atv(key, (CONST UB *)"tessronos.program.trusted", b, sizeof(b), &asz) >= E_OK
	  && asz == 4 && b[0] == 't' && b[1] == 'r' && b[2] == 'u' && b[3] == 'e' ) {
		yes = TRUE;
	}
	ob_cls_obj(key);
	return yes;
}

EXPORT BOOL knl_ob_trusted( CONST TS_UUID *prog )
{
	CONST T_OBMGR	*m;
	T_OBPRT		*prt;
	TS_UUID		box;
	INT		k;
	BOOL		ok = FALSE;

	if ( prog == NULL ) {
		return FALSE;
	}
	if ( !obp_says_trusted(prog) || ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &box) < E_OK
	  || knl_obfile_vol_of(prog) <= 0 || knl_obfile_vol_of(prog) != knl_obfile_vol_of(&box) ) {
		return FALSE;
	}
	m = knl_ob_mgr_of(prog);
	prt = NULL;
	if ( m != NULL && m->prot != NULL && m->prot(prog, &prt) >= E_OK && prt != NULL ) {
		ok = (BOOL)( ts_uuid_cmp(&prt->owner, &ob_user_system) == 0
			  && ( prt->mode & 0002 ) == 0
			  && ( ts_uuid_cmp(&prt->group, &ob_group_admin) == 0 || ( prt->mode & 0020 ) == 0 ) );
		for ( k = 0; ok && k < prt->nacl; k++ ) {
			if ( ( prt->acl[k].ops & ( OB_OP_W | OB_OP_PROT ) ) != 0 ) {
				ok = FALSE;
			}
		}
	}
	if ( prt != NULL ) Kfree(prt);
	return ok;
}

/*
 * Starting a program object, with the argument given. One the kernel
 * starts -- the desktop, for the person working it -- acts for that
 * person (TS_PRC_USER), but for a helper the system trusts, which acts
 * as the system; one a process starts acts as the process does.
 */
LOCAL INT says_withpar( CONST UB *json, INT len );
LOCAL INT says_role( CONST UB *json, INT len, UB *role );

/*
 * The metadata given may say that it ends with its maker
 * (tessronos.process.withParent, TS_PRC_WITHPARENT), and its role among
 * the processes of its program (tessronos.process.role).
 */
LOCAL ER obp_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	T_CPRC	cprc;
	ID	vol, pid;

	(void)prt;
	vol = knl_obfile_vol_of(&c->prog);
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.arg = c->arg;
	cprc.argsz = c->argsz;
	if ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < 0x3FFFFFFF
	  && says_withpar(c->json, (INT)c->jsonsz) == 1 ) {
		cprc.prcatr |= TS_PRC_WITHPARENT;
	}
	if ( ts_get_pid() <= 0 && !knl_ob_trusted(&c->prog) ) {
		cprc.prcatr = TS_PRC_USER;
	}
	pid = ts_cre_prc_obj(vol, &c->prog, &cprc);
	if ( pid <= 0 ) {
		return ( pid < 0 ) ? (ER)pid : E_SYS;
	}
	if ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < 0x3FFFFFFF ) {
		UB	role[TS_PRC_ROLE];
		INT	rl = says_role(c->json, (INT)c->jsonsz, role);

		if ( rl > 0 ) {
			(void)knl_prc_set_role(pid, role, rl);
		}
	}
	return knl_prc_uuid(pid, p_uuid, NULL);
}

LOCAL ER obp_remove( CONST TS_UUID *uuid )
{
	ID	pid = knl_prc_of_uuid(uuid);

	return ( pid > 0 ) ? ts_ter_prc(pid, TS_ABORT_TERM) : E_NOEXS;
}

/* The handle is the process ID */
LOCAL INT obp_open( CONST TS_UUID *uuid, UINT ops )
{
	ID	pid = knl_prc_of_uuid(uuid);

	(void)ops;
	return ( pid > 0 ) ? (INT)pid : E_NOEXS;
}

LOCAL ER obp_close( INT h )
{
	(void)h;
	return E_OK;
}

LOCAL CONST char *state_name( UINT st )
{
	switch ( st ) {
	case PS_RUNNING:	return "running";
	case PS_EXITING:	return "exiting";
	case PS_ZOMBIE:		return "ended";
	default:		return "none";
	}
}

/*
 * A paragraph with a link for each socket the process has open: the
 * table is asked how many there are first and taken as large, asked
 * again larger when more were opened in between. Answers the position,
 * -1 when t is full, or E_NOMEM.
 */
LOCAL INT socket_links( ID pid, UB *t, INT n, INT max )
{
	INT	*nums;
	TS_UUID	*u;
	UB	name[24];
	INT	room, k, i;

	room = knl_so_obj_list(pid, NULL, NULL, 0);
	for ( ;; ) {
		if ( room <= 0 ) {
			return n;
		}
		nums = (INT *)Kmalloc(sizeof(INT) * (SZ)room);
		u = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)room);
		if ( nums == NULL || u == NULL ) {
			if ( nums != NULL ) Kfree(nums);
			if ( u != NULL ) Kfree(u);
			return E_NOMEM;
		}
		k = knl_so_obj_list(pid, nums, u, room);
		if ( k <= room ) {
			break;
		}
		Kfree(nums);
		Kfree(u);
		room = k;
	}
	for ( i = 0; i < k; i++ ) {
		INT	p = knl_oj_put(name, 0, sizeof(name), "socket ");

		(void)knl_oj_put_num(name, p, sizeof(name), nums[i]);
		n = knl_obdev_link(t, n, max, &u[i], name);
	}
	Kfree(nums);
	Kfree(u);
	return n;
}

/*
 * A paragraph with a link for each end of a stream the process holds a
 * key to (obstrm.c), as many as there are, taken the same way
 */
LOCAL INT stream_links( ID pid, UB *t, INT n, INT max )
{
	TS_UUID	*u;
	INT	room, k, i;

	room = knl_ob_key_objs(pid, OB_T_CHANNEL, OB_S_STREAM, NULL, 0);
	for ( ;; ) {
		if ( room <= 0 ) {
			return n;
		}
		u = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)room);
		if ( u == NULL ) {
			return E_NOMEM;
		}
		k = knl_ob_key_objs(pid, OB_T_CHANNEL, OB_S_STREAM, u, room);
		if ( k <= room ) {
			break;
		}
		Kfree(u);
		room = k;
	}
	for ( i = 0; i < k; i++ ) {
		n = knl_obdev_link(t, n, max, &u[i], (CONST UB *)"stream");
	}
	Kfree(u);
	return n;
}

/* What the process is doing, as xmlTAD */
LOCAL INT status_text( ID pid, UB *t, INT max )
{
	T_RPRC	rp;
	TS_UUID	prog;
	UB	name[24];
	INT	n;

	if ( ts_ref_prc(pid, &rp) < E_OK || knl_prc_uuid(pid, NULL, &prog) < E_OK ) {
		return E_NOEXS;
	}
	(void)put_name(name, sizeof(name), pid);
	n = knl_oj_put(t, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, "\"><document><p>pid ");
	n = knl_oj_put_num(t, n, max, rp.pid);
	n = knl_oj_put(t, n, max, ", parent ");
	n = knl_oj_put_num(t, n, max, rp.ppid);
	n = knl_oj_put(t, n, max, ", ");
	n = knl_oj_put(t, n, max, state_name(rp.state));
	n = knl_oj_put(t, n, max, ", exit code ");
	n = knl_oj_put_num(t, n, max, rp.exitcd);
	n = knl_oj_put(t, n, max, ", memory ");
	n = knl_oj_put_num(t, n, max, (D)rp.memsz);
	n = knl_oj_put(t, n, max, " bytes</p>");
	if ( !knl_ob_uuid_zero(&prog) ) {
		char	us[TS_UUID_STRLEN + 1];

		(void)ts_uuid_to_str(&prog, us, sizeof(us));
		n = knl_oj_put(t, n, max, "<p><link id=\"");
		n = knl_oj_put(t, n, max, us);
		n = knl_oj_put(t, n, max, "_0.xtad\"/></p>");
	}
	n = socket_links(pid, t, n, max);
	if ( n < -1 ) {
		return n;
	}
	n = stream_links(pid, t, n, max);
	if ( n < -1 ) {
		return n;
	}
	return knl_oj_put(t, n, max, "</document></tad>");
}

/* Record 0, made as long as the process's links need */
LOCAL INT status_make( void *ctx, UB *t, INT max )
{
	return status_text(*(ID *)ctx, t, max);
}

LOCAL ER obp_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	ID	pid = (ID)h;

	(void)nowait;
	if ( recno != 0 ) {
		return E_NOEXS;
	}
	return knl_oj_make_rea(status_make, &pid, off, buf, size, p_asize);
}

LOCAL ER obp_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	ID	pid = (ID)h;
	UB	*t;
	INT	len;

	len = knl_oj_make(status_make, &pid, &t);
	if ( len < 0 ) {
		return (ER)len;
	}
	Kfree(t);
	if ( n > 0 ) {
		buf[0].recno = 0;
		buf[0].rt = OB_RT_TAD;
		buf[0].sub = 0;
		buf[0].size = (UD)len;
	}
	*p_cnt = ( n > 0 ) ? 1 : 0;

	return E_OK;
}

LOCAL ER obp_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	UB	name[24];
	INT	n;

	/* its number as an attribute of its own, so that no one reads it out of the name */
	(void)put_name(name, sizeof(name), (ID)h);
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":");
	n = knl_oj_put_str(json, n, (INT)size, name);
	n = knl_oj_put(json, n, (INT)size, ",\"tessronos\":{\"process\":{\"pid\":");
	n = knl_oj_put_num(json, n, (INT)size, (D)h);
	n = knl_oj_put(json, n, (INT)size, ( knl_prc_withpar((ID)h) == 1 )
		       ? ",\"withParent\":true" : ",\"withParent\":false");
	/* the groups of calls it is shut out of (include/ts/sbx.h), 0 for none */
	n = knl_oj_put(json, n, (INT)size, ",\"sandbox\":");
	n = knl_oj_put_num(json, n, (INT)size, (D)( knl_prc_sbx_of((ID)h) & TS_SBX_ALL ));
	/* the calls its sandbox turned away: in all, per group, and the last one's code */
	{
		UW	r[TS_SBX_NGROUP + 2];
		UB	txt[96];

		if ( !knl_prc_sbx_refused_of((ID)h, r) ) {
			knl_memset(r, 0, sizeof(r));
		}
		(void)tm_sprintf(txt, (CONST UB *)"total=%d obj=%d net=%d dev=%d gpu=%d proc=%d win=%d sys=%d last=%x",
				 (INT)r[0], (INT)r[1], (INT)r[2], (INT)r[3], (INT)r[4], (INT)r[5], (INT)r[6],
				 (INT)r[7], (UINT)r[8]);
		n = knl_oj_put(json, n, (INT)size, ",\"sandbox_refused\":");
		n = knl_oj_put_str(json, n, (INT)size, txt);
	}
	n = knl_oj_put(json, n, (INT)size, "}}}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/* tessronos.process.withParent of metadata: 1 true, 0 false, -1 not said */
LOCAL INT says_withpar( CONST UB *json, INT len )
{
	INT	v;
	BOOL	b;

	if ( json == NULL || len <= 0 ) {
		return -1;
	}
	v = knl_oj_path(json, len, "tessronos", "process");
	v = ( v >= 0 ) ? knl_oj_member(json, len, v, "withParent") : -1;
	return ( v >= 0 && knl_oj_bool(json, len, v, &b) ) ? ( b ? 1 : 0 ) : -1;
}

/*
 * tessronos.process.role of metadata: its length put in role (at most
 * TS_PRC_ROLE - 1 bytes, ending in 0), 0 for an empty one, -1 not said
 */
LOCAL INT says_role( CONST UB *json, INT len, UB *role )
{
	UB	buf[TS_PRC_ROLE * 2];
	INT	v, n;

	role[0] = 0;
	if ( json == NULL || len <= 0 ) {
		return -1;
	}
	v = knl_oj_path(json, len, "tessronos", "process");
	v = ( v >= 0 ) ? knl_oj_member(json, len, v, "role") : -1;
	n = ( v >= 0 ) ? knl_oj_str(json, len, v, buf, (INT)sizeof(buf)) : -1;
	if ( n < 0 ) {
		return -1;
	}
	n = (INT)knl_utf8_fit(buf, (SZ)n, TS_PRC_ROLE - 1);	/* never half a character */
	knl_memcpy(role, buf, n);
	role[n] = 0;
	return n;
}

/*
 * What of the attributes may be changed: whether it ends with its
 * parent (tessronos.process.withParent) and its role
 * (tessronos.process.role). The rest is made from the process each time
 * it is read and is not kept.
 */
LOCAL ER obp_sat( INT h, CONST UB *json, SZ size )
{
	INT	w = says_withpar(json, (INT)size);
	UB	role[TS_PRC_ROLE];
	INT	rl = says_role(json, (INT)size, role);
	ER	er = E_OK;

	if ( rl >= 0 ) {
		er = knl_prc_set_role((ID)h, role, rl);
	}
	if ( er >= E_OK && w >= 0 ) {
		er = knl_prc_set_withpar((ID)h, (BOOL)( w == 1 ));
	}
	return er;
}

/*
 * The process objects after from, in the order of their UUIDs: every
 * process there is is looked at, so that asking on from the last one
 * answered reaches them all
 */
LOCAL ER obp_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	ID	*pids;
	TS_UUID	*u;
	INT	np, nu = 0, i, cnt = 0;

	*p_cnt = 0;
	np = knl_prc_list_all(&pids);
	if ( pids == NULL ) {
		return E_NOMEM;
	}
	u = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( np > 0 ? np : 1 ));
	if ( u == NULL ) {
		Kfree(pids);
		return E_NOMEM;
	}
	for ( i = 0; i < np; i++ ) {
		if ( knl_prc_uuid(pids[i], &u[nu], NULL) >= E_OK && !knl_ob_uuid_zero(&u[nu]) ) {
			nu++;
		}
	}
	Kfree(pids);
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < nu; i++ ) {
			if ( cnt > 0 && ts_uuid_cmp(&u[i], &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&u[i], from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&u[i], &u[best]) < 0 ) best = i;
		}
		if ( best < 0 ) break;
		buf[cnt++] = u[best];
	}
	Kfree(u);
	*p_cnt = cnt;

	return E_OK;
}

/*
 * Whether the calling process may do ops (OB_OP_*) to process pid:
 * what its process object's protection gives the caller's credentials
 * (the user the process acts for owns it, others may read, an
 * administrator may do anything), as a key opened on it would carry.
 * A process may do anything to itself, and what 'kin' names besides:
 * OBP_PARENT to its children, OBP_CHILD to its parent. E_NOEXS when
 * there is no such process, E_OACV when it may not. The kernel's own
 * tasks do not come here.
 */
EXPORT ER knl_obprc_may( ID pid, UINT ops, UINT kin )
{
	T_RPRC	rp, mine;
	TS_UUID	u;
	ID	me = ts_get_pid();

	if ( pid <= 0 || ts_ref_prc(pid, &rp) < E_OK || knl_prc_uuid(pid, &u, NULL) < E_OK ) {
		return E_NOEXS;
	}
	if ( pid == me ) {
		return E_OK;
	}
	if ( ( kin & OBP_PARENT ) != 0 && rp.ppid == me ) {
		return E_OK;
	}
	if ( ( kin & OBP_CHILD ) != 0 && ts_ref_prc(me, &mine) >= E_OK && mine.ppid == pid ) {
		return E_OK;
	}
	if ( knl_ob_uuid_zero(&u) ) {
		/* made before there was a clock: not an object, an administrator's only */
		return knl_ob_is_admin(knl_ob_crd_of(me)) ? E_OK : E_OACV;
	}
	return ( ( knl_ob_permit_on(&u, knl_ob_crd_of(me)) & ops ) == ops ) ? E_OK : E_OACV;
}

LOCAL CONST T_OBMGR obp_mgr = {
	OB_T_PROCESS, 0, "process",
	obp_find, obp_ref, obp_prot, NULL, obp_create, obp_remove,
	obp_open, obp_close, obp_rea, NULL, NULL, NULL, NULL, obp_lrc,
	obp_gat, obp_sat, NULL, obp_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	NULL,			/* nothing comes in to wait for (ob_rea_tmo) */
	NULL,
	obp_refnam
};

EXPORT ER knl_obprc_init( void )
{
	INT	no = knl_ob_regist(&obp_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
