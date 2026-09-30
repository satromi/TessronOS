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
 *	starting a program object; deleting one ends it. Record 0 is what
 *	the process is doing, written as xmlTAD when it is read, with a
 *	virtual object of the program it runs and one of each socket it has
 *	open (obsock.c). Its owner is the user it acts for: that user may
 *	end it, others may only look.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include <ts/so.h>
#include "obj.h"

#define OBP_TEXT_MAX	6144		/* the status, the program's link and SO_MAX sockets' */

LOCAL ER obp_find( CONST TS_UUID *uuid )
{
	return ( knl_prc_of_uuid(uuid) > 0 ) ? E_OK : E_NOEXS;
}

LOCAL INT put_name( UB *out, INT max, ID pid )
{
	INT	n = knl_oj_put(out, 0, max, "process ");

	return knl_oj_put_num(out, n, max, pid);
}

LOCAL ER obp_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_RPRC	rp;
	ID	pid = knl_prc_of_uuid(uuid);

	if ( pid <= 0 || ts_ref_prc(pid, &rp) < E_OK ) {
		return E_NOEXS;
	}
	(void)put_name(r->name, OB_NAME_MAX, pid);
	r->flags = OB_F_VOLATILE;
	r->nrec = 1;
	r->size = rp.memsz;

	return E_OK;
}

/* The user it acts for owns it: rw-r--r-- */
LOCAL ER obp_prot( CONST TS_UUID *uuid, T_OBPRT *prt )
{
	ID		pid = knl_prc_of_uuid(uuid);
	CONST T_OBCRD	*crd;

	if ( pid <= 0 ) {
		return E_NOEXS;
	}
	crd = knl_ob_crd_of(pid);
	knl_ob_prt_default(prt, crd);

	return E_OK;
}

/* Starting a program object, with the argument given */
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
	pid = ts_cre_prc_obj(vol, &c->prog, &cprc);
	if ( pid <= 0 ) {
		return ( pid < 0 ) ? (ER)pid : E_SYS;
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

/* A paragraph with a link for each socket the process has open */
LOCAL INT socket_links( ID pid, UB *t, INT n, INT max )
{
	struct {
		INT	nums[SO_MAX];
		TS_UUID	u[SO_MAX];
	}	*l;
	UB	name[24];
	INT	k, i;

	l = Kmalloc(sizeof(*l));
	if ( l == NULL ) {
		return n;
	}
	k = knl_so_obj_list(pid, l->nums, l->u, SO_MAX);
	for ( i = 0; i < k && i < SO_MAX; i++ ) {
		INT	p = knl_oj_put(name, 0, sizeof(name), "socket ");

		(void)knl_oj_put_num(name, p, sizeof(name), l->nums[i]);
		n = knl_obdev_link(t, n, max, &l->u[i], name);
	}
	Kfree(l);
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
		return -1;
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
	return knl_oj_put(t, n, max, "</document></tad>");
}

LOCAL ER obp_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	UB	*t;
	INT	len;
	SZ	n = 0;

	(void)nowait;
	if ( recno != 0 ) {
		return E_NOEXS;
	}
	if ( off < 0 ) {
		return E_PAR;
	}
	t = (UB *)Kmalloc(OBP_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	len = status_text((ID)h, t, OBP_TEXT_MAX);
	if ( len < 0 ) {
		Kfree(t);
		return E_NOEXS;
	}
	if ( off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obp_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	UB	*t;
	INT	len;

	t = (UB *)Kmalloc(OBP_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	len = status_text((ID)h, t, OBP_TEXT_MAX);
	Kfree(t);
	if ( len < 0 ) {
		return E_NOEXS;
	}
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

	(void)put_name(name, sizeof(name), (ID)h);
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":");
	n = knl_oj_put_str(json, n, (INT)size, name);
	n = knl_oj_put(json, n, (INT)size, "}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obp_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	ID	pids[16];
	TS_UUID	u[16];
	INT	np, nu = 0, i, cnt = 0;

	np = knl_prc_list(pids, 16);
	for ( i = 0; i < np; i++ ) {
		if ( knl_prc_uuid(pids[i], &u[nu], NULL) >= E_OK && !knl_ob_uuid_zero(&u[nu]) ) {
			nu++;
		}
	}
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
	obp_gat, NULL, NULL, obp_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL
};

EXPORT ER knl_obprc_init( void )
{
	INT	no = knl_ob_regist(&obp_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
