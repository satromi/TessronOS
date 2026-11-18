/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsvc.c
 *	The object calls of a process: SVC class 7 (design 18.5, 9.7).
 *
 *	Every pointer a process passes is made sure of before the object
 *	layer is given it: it must lie in the calling process's own pages,
 *	writable when something is to come back in it, or the call answers
 *	E_MACV and nothing is done. What the layer reads and acts on -- a
 *	UUID, a name, a path, a password, the creation block and what it
 *	points to, a protection, a notice request, attributes, an icon -- is
 *	copied into the kernel first, so that the process cannot change it
 *	between the look and the use. Data that is only carried (the bytes
 *	of a record or a resource) and room for answers are used where they
 *	are, once checked. A NULL pointer is handed on as NULL: the layer
 *	answers for it as it does for the kernel.
 *
 *	The kernel reaches the process's pages only with the way to them
 *	open (PAN): the copies are made with knl_ua_get and knl_ua_put, and
 *	a call of the layer that uses the process's memory where it is runs
 *	inside UACCESS, after every pointer it is handed has been made sure
 *	of.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/tsfsck.h>
#include <ts/sbx.h>
#include "obj.h"

#define F(fn)		((FP)(fn))

#define PAGE_BYTES	4096		/* a user string is asked about a page at a time */
#define PWD_MAX		256		/* bytes of a password, the terminator included */
#define ARG_MAX		4096		/* bytes of a process's start-up argument */

LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

/* Room for an answer: NULL, or len bytes the process may write */
LOCAL BOOL out_ok( CONST void *p, SZ len )
{
	return ( p == NULL || ( len >= 0 && user_ok(p, len, TRUE) ) );
}

/* Bytes the object layer only carries: NULL, or len bytes the process may read */
LOCAL BOOL in_ok( CONST void *p, SZ len )
{
	return ( p == NULL || ( len >= 0 && user_ok(p, len, FALSE) ) );
}

/*
 * A string of the process's, copied into dst of max bytes: E_MACV when
 * it leaves its pages, E_PAR when it does not end within max -- or, with
 * 'cut', cut short there, as the layer would cut a name.
 */
LOCAL ER str_in( CONST UB *src, UB *dst, INT max, BOOL cut )
{
	INT	i;

	for ( i = 0; i < max; i++ ) {
		if ( ( i == 0 || ( ( (UBINT)( src + i ) ) & ( PAGE_BYTES - 1 ) ) == 0 )
		  && !user_ok(src + i, 1, FALSE) ) {
			return E_MACV;
		}
		dst[i] = UACCESS(src[i]);
		if ( dst[i] == 0 ) {
			return E_OK;
		}
	}
	if ( !cut ) {
		return E_PAR;
	}
	dst[max - 1] = 0;
	return E_OK;
}

/*
 * A name of the process's, of any length, copied into memory of the
 * kernel's (*p, Kfree it): its length is measured a page at a time
 * first, and it is taken at once. A name goes into an object's metadata,
 * so one longer than the metadata a program may write (OB_META_LIMIT)
 * is E_LIMIT; E_MACV when it leaves its pages. Never cut.
 */
#define NAME_IN_MAX	OB_META_LIMIT

LOCAL ER str_dup_in( CONST UB *src, UB **p, SZ *p_len )
{
	SZ	n;

	*p = NULL;
	for ( n = 0; ; n++ ) {
		if ( ( n == 0 || ( ( (UBINT)( src + n ) ) & ( PAGE_BYTES - 1 ) ) == 0 )
		  && !user_ok(src + n, 1, FALSE) ) {
			return E_MACV;
		}
		if ( UACCESS(src[n]) == 0 ) {
			break;
		}
		if ( n >= NAME_IN_MAX ) {
			return E_LIMIT;
		}
	}
	*p = (UB *)Kmalloc(n + 1);
	if ( *p == NULL ) {
		return E_NOMEM;
	}
	if ( n > 0 ) {
		knl_ua_get(*p, src, n);
	}
	(*p)[n] = 0;
	if ( p_len != NULL ) {
		*p_len = n;
	}
	return E_OK;
}

/* A UUID of the process's, copied; *pp the copy, or NULL for NULL */
LOCAL ER uuid_in( CONST TS_UUID *u, TS_UUID *k, CONST TS_UUID **pp )
{
	if ( u == NULL ) {
		*pp = NULL;
		return E_OK;
	}
	if ( !user_ok(u, sizeof(*u), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(k, u, sizeof(*k));
	*pp = k;
	return E_OK;
}

/* size bytes of the process's copied into memory of the kernel's (NULL for none) */
/*
 * The most of a process's metadata copied in at once: the same bound as
 * reading a record or metadata whole (knl_ob_rea_all, knl_meta_all), so
 * that what one program writes another can read.
 */
#define META_IN_MAX	OB_META_LIMIT

LOCAL ER prt_in( CONST T_OBPRT *p, T_OBPRT **pp );

LOCAL ER buf_in( CONST void *src, SZ size, SZ max, void **p_copy )
{
	*p_copy = NULL;
	if ( src == NULL || size == 0 ) {
		return E_OK;
	}
	if ( size < 0 || size > max ) {
		return E_PAR;
	}
	if ( !user_ok(src, size, FALSE) ) {
		return E_MACV;
	}
	*p_copy = Kmalloc(size);
	if ( *p_copy == NULL ) {
		return E_NOMEM;
	}
	knl_ua_get(*p_copy, src, size);
	return E_OK;
}

LOCAL void buf_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

/* ---------------------------------------------------------------- the creation block */

/* A T_OBCRE and everything it points to, the kernel's own */
typedef struct {
	T_OBCRE	c;
	UB	*name;			/* as long as the process's is */
	char	vol[FS_PATH_MAX];
	T_OBPRT	*prt;			/* as long as the process's is */
	void	*json, *arg, *icon;
} CRECOPY;

LOCAL void cre_free( CRECOPY *k )
{
	if ( k == NULL ) return;
	buf_free(k->name);
	buf_free(k->prt);
	buf_free(k->json);
	buf_free(k->arg);
	buf_free(k->icon);
	Kfree(k);
}

LOCAL ER cre_in( CONST T_OBCRE *u, CRECOPY **pp )
{
	CRECOPY	*k;
	ER	er = E_OK;

	*pp = NULL;
	if ( u == NULL ) {
		return E_OK;
	}
	if ( !user_ok(u, sizeof(*u), FALSE) ) {
		return E_MACV;
	}
	k = (CRECOPY *)Kmalloc(sizeof(CRECOPY));
	if ( k == NULL ) {
		return E_NOMEM;
	}
	k->json = k->arg = k->icon = NULL;
	k->prt = NULL;
	k->name = NULL;
	knl_ua_get(&k->c, u, sizeof(k->c));
	if ( k->c.name != NULL ) {
		er = str_dup_in(k->c.name, &k->name, NULL);
		k->c.name = k->name;
	}
	if ( er >= E_OK && k->c.vol != NULL ) {
		er = str_in((CONST UB *)k->c.vol, (UB *)k->vol, sizeof(k->vol), FALSE);
		k->c.vol = k->vol;
	}
	if ( er >= E_OK && k->c.prt != NULL ) {
		er = prt_in(k->c.prt, &k->prt);
		k->c.prt = k->prt;
	}
	if ( er >= E_OK ) {
		er = buf_in(k->c.json, k->c.jsonsz, META_IN_MAX, &k->json);
		k->c.json = (CONST UB *)k->json;
	}
	if ( er >= E_OK ) {
		er = buf_in(k->c.arg, k->c.argsz, ARG_MAX, &k->arg);
		k->c.arg = k->arg;
	}
	if ( er >= E_OK ) {
		er = buf_in(k->c.icon, k->c.iconsz, OB_ICO_MAX, &k->icon);
		k->c.icon = k->icon;
	}
	if ( er < E_OK ) {
		cre_free(k);
		return er;
	}
	*pp = k;
	return E_OK;
}

/* ---------------------------------------------------------------- objects */

/*
 * The group of a restricted process's (include/ts/sbx.h) an object of a
 * kind is of, when it is opened or made by its UUID: the gateway lets
 * such calls by only when objects are not shut out as a whole (the
 * network's process), and the kind decides then.
 */
LOCAL UINT sbx_kind( UINT type, UINT sub )
{
	switch ( type ) {
	  case OB_T_DEVICE:	return ( sub == OB_S_GPU ) ? TS_SBX_GPU : TS_SBX_DEV;
	  case OB_T_PROCESS:	return TS_SBX_PROC;
	  case OB_T_WINDOW:	return TS_SBX_WIN;
	  case OB_T_CHANNEL:	return ( sub == OB_S_SOCKET ) ? TS_SBX_NET : 0;
	  default:		return 0;
	}
}

LOCAL ID svc_opn_obj( CONST TS_UUID *uuid, UINT ops )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er >= E_OK ) {
		CONST T_OBMGR	*m = knl_ob_mgr_of(u);

		if ( m != NULL ) {
			er = knl_sbx_may(sbx_kind(m->type, m->sub), TSN_OB_OPN_OBJ);
		}
	}
	return ( er < E_OK ) ? er : ob_opn_obj(u, ops);
}

LOCAL ER svc_cre_obj( CONST T_OBCRE *pk_cre, TS_UUID *p_uuid )
{
	CRECOPY	*k;
	ER	er;

	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	er = cre_in(pk_cre, &k);
	if ( er < E_OK ) return er;
	if ( k != NULL && ( er = knl_sbx_may(sbx_kind(k->c.type, k->c.sub), TSN_OB_CRE_OBJ) ) < E_OK ) {
		cre_free(k);
		return er;
	}
	er = UACCESS(ob_cre_obj(( k != NULL ) ? &k->c : NULL, p_uuid));
	cre_free(k);
	return er;
}

/* The keys of an object without a name (ob_cre_key): room for two for a pair of streams */
LOCAL ER svc_cre_key( CONST T_OBCRE *pk_cre, UINT ops, ID *keys, TS_UUID *p_uuid )
{
	CRECOPY	*k;
	TS_UUID	u;
	ID	kk[2] = { 0, 0 };
	INT	n;
	ER	er;

	if ( p_uuid != NULL && !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	er = cre_in(pk_cre, &k);
	if ( er < E_OK ) return er;
	if ( k == NULL ) return E_PAR;
	n = ( k->c.type == OB_T_CHANNEL && k->c.sub == OB_S_STREAM ) ? 2 : 1;
	if ( !out_ok(keys, (SZ)sizeof(ID) * n) ) {
		cre_free(k);
		return E_MACV;
	}
	er = ob_cre_key(&k->c, ops, kk, &u);
	cre_free(k);
	if ( er >= E_OK ) {
		knl_ua_put(keys, kk, (SZ)sizeof(ID) * n);
		if ( p_uuid != NULL ) {
			knl_ua_put(p_uuid, &u, sizeof(u));
		}
	}
	return er;
}

LOCAL ER svc_del_obj( CONST TS_UUID *uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_del_obj(u);
}

LOCAL ER svc_ref_obj2( CONST TS_UUID *uuid, T_OBREF *pk_ref )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_ref, sizeof(*pk_ref)) ) return E_MACV;
	return UACCESS(ob_ref_obj(u, pk_ref));
}

/*
 * The form before (TSN_OB_REF_OBJ): the name in the structure, 64 bytes,
 * for a program built before the name was taken out. The name is cut at
 * a whole letter, with OB_F_NAMECUT when it did not fit.
 */
#define OBREF_V1_NAME	64

typedef struct {
	TS_UUID	uuid;
	UINT	type;
	UINT	sub;
	UINT	flags;
	UB	name[OBREF_V1_NAME];
	INT	refcnt;
	INT	nrec;
	UD	size;
} T_OBREF_V1;

LOCAL ER svc_ref_obj( CONST TS_UUID *uuid, T_OBREF_V1 *pk_ref )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	T_OBREF		r;
	T_OBREF_V1	o;
	UB		*nm;
	SZ		len = 0, n;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_ref, sizeof(*pk_ref)) ) return E_MACV;
	er = ob_ref_obj(u, &r);
	if ( er < E_OK ) return er;
	knl_memset(&o, 0, sizeof(o));
	o.uuid = r.uuid;
	o.type = r.type;
	o.sub = r.sub;
	o.flags = r.flags;
	o.refcnt = r.refcnt;
	o.nrec = r.nrec;
	o.size = r.size;
	nm = knl_ob_name(u, &len, NULL);
	if ( nm != NULL ) {
		n = knl_utf8_fit(nm, len, OBREF_V1_NAME - 1);
		knl_memcpy(o.name, nm, n);
		if ( n < len ) o.flags |= OB_F_NAMECUT;
		Kfree(nm);
	}
	knl_ua_put(pk_ref, &o, sizeof(o));
	return E_OK;
}

/* A name the kernel holds whole, put as much as the process has room for, and the size it takes */
LOCAL ER name_out( CONST UB *nm, SZ len, UB *buf, SZ size, SZ *p_asize )
{
	SZ	n;

	if ( size < 0 || ( size > 0 && buf == NULL ) ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	if ( size > 0 ) {
		n = knl_utf8_fit(nm, len, size - 1);
		if ( n > 0 ) knl_ua_put(buf, nm, n);
		knl_ua_put(buf + n, "", 1);
	}
	if ( p_asize != NULL ) {
		n = len + 1;
		knl_ua_put(p_asize, &n, sizeof(n));
	}
	return E_OK;
}

LOCAL ER svc_ref_nam( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	UB		*nm;
	SZ		len = 0;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( u == NULL ) return E_PAR;
	nm = knl_ob_name(u, &len, &er);
	if ( nm == NULL ) return er;
	er = name_out(nm, len, buf, size, p_asize);
	Kfree(nm);
	return er;
}

LOCAL ER svc_acc_obj( CONST TS_UUID *uuid, UINT ops )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_acc_obj(u, ops);
}

LOCAL ER svc_ref_vol( CONST TS_UUID *uuid, T_OBVOL *pk_vol )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_vol, sizeof(*pk_vol)) ) return E_MACV;
	return UACCESS(ob_ref_vol(u, pk_vol));
}

LOCAL ER svc_ref_vid( CONST TS_UUID *uuid, TS_UUID *p_vol, TS_UUID *p_mach )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(p_vol, sizeof(*p_vol)) || !out_ok(p_mach, sizeof(*p_mach)) ) return E_MACV;
	return UACCESS(ob_ref_vid(u, p_vol, p_mach));
}

LOCAL ER svc_wip_vol( CONST TS_UUID *uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_wip_vol(u);
}

LOCAL ER svc_lst_obj( UINT type, UINT sub, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(from, &k, &u);

	if ( er < E_OK ) return er;
	if ( n < 0 ) return E_PAR;
	if ( !out_ok(buf, (SZ)n * (SZ)sizeof(TS_UUID)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return UACCESS(ob_lst_obj(type, sub, u, buf, n, p_cnt));
}

LOCAL ER svc_lnk_obj( CONST TS_UUID *uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_lnk_obj(u);
}

LOCAL ER svc_unl_obj( CONST TS_UUID *uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_unl_obj(u);
}

LOCAL ER svc_cpy_obj( CONST TS_UUID *uuid, CONST T_OBCRE *pk_cre, TS_UUID *p_uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		ku;
	CRECOPY		*k;
	ER		er = uuid_in(uuid, &ku, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	er = cre_in(pk_cre, &k);
	if ( er < E_OK ) return er;
	er = UACCESS(ob_cpy_obj(u, ( k != NULL ) ? &k->c : NULL, p_uuid));
	cre_free(k);
	return er;
}

LOCAL ER svc_dup_obj( CONST TS_UUID *uuid, CONST UB *name, TS_UUID *p_uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		ku;
	UB		*kn = NULL;
	ER		er = uuid_in(uuid, &ku, &u);

	if ( er < E_OK ) return er;
	if ( p_uuid == NULL ) return E_PAR;
	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	if ( name != NULL ) {
		er = str_dup_in(name, &kn, NULL);
		if ( er < E_OK ) return er;
	}
	er = UACCESS(ob_dup_obj(u, kn, p_uuid));
	buf_free(kn);
	return er;
}

/* ---------------------------------------------------------------- records */

LOCAL ER svc_rea_rec( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_rea_rec(key, recno, off, buf, size, p_asize));
}

LOCAL ER svc_rea_tmo( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	if ( size < 0 || ( tmo < 0 && tmo != TMO_FEVR ) ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_rea_tmo(key, recno, off, buf, size, p_asize, tmo));
}

LOCAL ER svc_wri_rec( ID key, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !in_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_wri_rec(key, recno, off, buf, size, p_asize));
}

LOCAL ER svc_rpl_rec( ID key, INT recno, CONST void *buf, SZ size )
{
	if ( size < 0 ) return E_PAR;
	if ( !in_ok(buf, size) ) return E_MACV;
	return UACCESS(ob_rpl_rec(key, recno, buf, size));
}

LOCAL ER svc_apd_rec( ID key, UINT rt, UINT sub, INT *p_recno )
{
	if ( !out_ok(p_recno, sizeof(*p_recno)) ) return E_MACV;
	return UACCESS(ob_apd_rec(key, rt, sub, p_recno));
}

LOCAL ER svc_lst_rec( ID key, T_OBREC *buf, INT n, INT *p_cnt )
{
	if ( n < 0 ) return E_PAR;
	if ( !out_ok(buf, (SZ)n * (SZ)sizeof(T_OBREC)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return UACCESS(ob_lst_rec(key, buf, n, p_cnt));
}

LOCAL ER svc_sch_rec( ID key, INT from, UINT rt, UINT sub, UINT mask, INT *p_recno )
{
	if ( !out_ok(p_recno, sizeof(*p_recno)) ) return E_MACV;
	return UACCESS(ob_sch_rec(key, from, rt, sub, mask, p_recno));
}

LOCAL ER svc_map_rec( ID key, INT recno, ID target, T_OBMAP *pk_map )
{
	T_OBMAP	m;
	ER	er;

	if ( pk_map == NULL ) {
		return ob_map_rec(key, recno, target, NULL);
	}
	if ( !user_ok(pk_map, sizeof(*pk_map), TRUE) ) return E_MACV;
	knl_ua_get(&m, pk_map, sizeof(m));
	er = ob_map_rec(key, recno, target, &m);
	if ( er >= E_OK && user_ok(pk_map, sizeof(*pk_map), TRUE) ) {
		knl_ua_put(pk_map, &m, sizeof(m));	/* the address it was given */
	}
	return er;
}

/* ---------------------------------------------------------------- attributes, icon, resources */

LOCAL ER svc_get_atr( ID key, UB *json, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(json, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_get_atr(key, json, size, p_asize));
}

LOCAL ER svc_get_atv( ID key, CONST UB *path, UB *buf, SZ size, SZ *p_asize )
{
	UB	p[OB_ATV_PATH];
	ER	er;

	if ( size < 0 ) return E_PAR;
	er = str_in(path, p, sizeof(p), FALSE);
	if ( er < E_OK ) return er;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_get_atv(key, p, buf, size, p_asize));
}

LOCAL ER svc_set_atr( ID key, CONST UB *json, SZ size )
{
	void	*k;
	ER	er = buf_in(json, size, META_IN_MAX, &k);

	if ( er < E_OK ) return er;
	er = ob_set_atr(key, ( k != NULL ) ? (CONST UB *)k : json, size);
	buf_free(k);
	return er;
}

LOCAL ER svc_get_ico( ID key, UB *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_get_ico(key, buf, size, p_asize));
}

LOCAL ER svc_set_ico( ID key, CONST UB *buf, SZ size )
{
	void	*k;
	ER	er = buf_in(buf, size, OB_ICO_MAX, &k);

	if ( er < E_OK ) return er;
	er = ob_set_ico(key, ( k != NULL ) ? (CONST UB *)k : buf, size);
	buf_free(k);
	return er;
}

/* A resource's name, copied; *pp the copy, or NULL for NULL */
LOCAL ER res_name_in( CONST UB *name, UB *k, CONST UB **pp )
{
	*pp = NULL;
	if ( name == NULL ) {
		return E_OK;
	}
	*pp = k;
	return str_in(name, k, OB_RES_NAME, FALSE);
}

LOCAL ER svc_rea_res( ID key, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize )
{
	UB		k[OB_RES_NAME];
	CONST UB	*n;
	ER		er = res_name_in(name, k, &n);

	if ( er < E_OK ) return er;
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_rea_res(key, n, off, buf, size, p_asize));
}

LOCAL ER svc_wri_res( ID key, CONST UB *name, CONST void *buf, SZ size )
{
	UB		k[OB_RES_NAME];
	CONST UB	*n;
	ER		er = res_name_in(name, k, &n);

	if ( er < E_OK ) return er;
	if ( size < 0 ) return E_PAR;
	if ( !in_ok(buf, size) ) return E_MACV;
	return UACCESS(ob_wri_res(key, n, buf, size));
}

LOCAL ER svc_del_res( ID key, CONST UB *name )
{
	UB		k[OB_RES_NAME];
	CONST UB	*n;
	ER		er = res_name_in(name, k, &n);

	return ( er < E_OK ) ? er : ob_del_res(key, n);
}

LOCAL ER svc_lst_res( ID key, UB *buf, SZ size, INT *p_cnt )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return UACCESS(ob_lst_res(key, buf, size, p_cnt));
}

/* ---------------------------------------------------------------- protection, names, users */

LOCAL ER svc_get_prt( CONST TS_UUID *uuid, T_OBPRT *pk_prt, SZ size, SZ *p_asize )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(pk_prt, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_get_prt(u, pk_prt, size, p_asize));
}

/*
 * A protection of the process's, copied into memory of the kernel's
 * (NULL for NULL): its head first, then as long as the head says it is.
 */
LOCAL ER prt_in( CONST T_OBPRT *p, T_OBPRT **pp )
{
	T_OBPRT	head;
	SZ	n;

	*pp = NULL;
	if ( p == NULL ) {
		return E_OK;
	}
	if ( !user_ok(p, sizeof(head), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(&head, p, sizeof(head));
	if ( head.nacl < 0 || head.nrmask < 0 || head.nacl > 0x00FFFFFF || head.nrmask > 0x00FFFFFF ) {
		return E_PAR;
	}
	n = (SZ)OB_PRT_LEN(&head);
	if ( !user_ok(p, n, FALSE) ) {
		return E_MACV;
	}
	*pp = (T_OBPRT *)Kmalloc(n);
	if ( *pp == NULL ) {
		return E_NOMEM;
	}
	knl_ua_get(*pp, p, n);
	(*pp)->nacl = head.nacl;		/* what was looked at, whatever the process changed since */
	(*pp)->nrmask = head.nrmask;
	return E_OK;
}

LOCAL ER svc_set_prt( CONST TS_UUID *uuid, CONST T_OBPRT *pk_prt )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	T_OBPRT		*p;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	er = prt_in(pk_prt, &p);
	if ( er < E_OK ) return er;
	er = ob_set_prt(u, p);
	buf_free(p);
	return er;
}

LOCAL ER svc_fnd_nam( CONST UB *name, TS_UUID *p_uuid )
{
	UB	*k;
	ER	er;

	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	if ( name == NULL ) {
		return UACCESS(ob_fnd_nam(NULL, p_uuid));
	}
	er = str_dup_in(name, &k, NULL);
	if ( er < E_OK ) return er;
	er = UACCESS(ob_fnd_nam(k, p_uuid));
	buf_free(k);
	return er;
}

/*
 * The box and the name of a link in it the process gives (an object name
 * or a file name), copied; *pn NULL for no name, else the caller's to
 * Kfree
 */
LOCAL ER lnk_in( CONST TS_UUID *box, CONST UB *name, TS_UUID *kb, CONST TS_UUID **pb, UB **pn )
{
	ER	er = uuid_in(box, kb, pb);

	*pn = NULL;
	if ( er < E_OK || name == NULL ) {
		return er;
	}
	return str_dup_in(name, pn, NULL);
}

LOCAL ER svc_fnd_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid )
{
	CONST TS_UUID	*b;
	UB		*nm;
	TS_UUID		kb;
	ER		er = lnk_in(box, name, &kb, &b, &nm);

	if ( er >= E_OK ) {
		er = out_ok(p_uuid, sizeof(*p_uuid)) ? UACCESS(ob_fnd_lnk(b, nm, p_uuid)) : E_MACV;
	}
	buf_free(nm);
	return er;
}

LOCAL ER svc_lst_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *buf, INT n, INT *p_cnt )
{
	CONST TS_UUID	*b;
	UB		*nm;
	TS_UUID		kb;
	ER		er = lnk_in(box, name, &kb, &b, &nm);

	if ( er >= E_OK && n < 0 ) {
		er = E_PAR;
	} else if ( er >= E_OK
		 && ( !out_ok(buf, (SZ)n * (SZ)sizeof(TS_UUID)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) ) {
		er = E_MACV;
	} else if ( er >= E_OK ) {
		er = UACCESS(ob_lst_lnk(b, nm, buf, n, p_cnt));
	}
	buf_free(nm);
	return er;
}

LOCAL ER svc_fnd_sys( CONST char *role, CONST char *dflt, TS_UUID *p_uuid )
{
	UB	*kr = NULL, kd[TS_UUID_STRLEN + 1];
	ER	er;

	if ( role == NULL || p_uuid == NULL ) return E_PAR;
	er = str_dup_in((CONST UB *)role, &kr, NULL);
	if ( er >= E_OK && dflt != NULL ) er = str_in((CONST UB *)dflt, kd, sizeof(kd), FALSE);
	if ( er >= E_OK && !out_ok(p_uuid, sizeof(*p_uuid)) ) er = E_MACV;
	if ( er >= E_OK ) {
		er = UACCESS(ob_fnd_sys((CONST char *)kr, ( dflt != NULL ) ? (CONST char *)kd : NULL, p_uuid));
	}
	buf_free(kr);
	return er;
}

LOCAL ER svc_get_crd( T_OBCRD *pk_crd )
{
	if ( !out_ok(pk_crd, sizeof(*pk_crd)) ) return E_MACV;
	return UACCESS(ob_get_crd(pk_crd));
}

LOCAL ER svc_login( CONST TS_UUID *user, CONST UB *password )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	UB		pw[PWD_MAX];
	ER		er = uuid_in(user, &k, &u);

	if ( er < E_OK ) return er;
	if ( password == NULL ) {
		return ob_login(u, NULL);
	}
	er = str_in(password, pw, sizeof(pw), FALSE);
	if ( er >= E_OK ) {
		er = ob_login(u, pw);
	}
	knl_memset(pw, 0, sizeof(pw));
	return er;
}

LOCAL ER svc_set_pwd( CONST TS_UUID *user, CONST UB *password )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	UB		pw[PWD_MAX];
	ER		er = uuid_in(user, &k, &u);

	if ( er < E_OK ) return er;
	if ( password == NULL ) {
		return ob_set_pwd(u, NULL);
	}
	er = str_in(password, pw, sizeof(pw), FALSE);
	if ( er >= E_OK ) {
		er = ob_set_pwd(u, pw);
	}
	knl_memset(pw, 0, sizeof(pw));
	return er;
}

/* ---------------------------------------------------------------- volumes and transactions */

/* A path of the process's, copied; *pp the copy, or NULL for NULL */
LOCAL ER path_in( CONST char *path, char *k, CONST char **pp )
{
	*pp = NULL;
	if ( path == NULL ) {
		return E_OK;
	}
	*pp = k;
	return str_in((CONST UB *)path, (UB *)k, FS_PATH_MAX, FALSE);
}

LOCAL ER svc_att_vol( CONST char *path, UINT flags )
{
	char		k[FS_PATH_MAX];
	CONST char	*p;
	ER		er = path_in(path, k, &p);

	return ( er < E_OK ) ? er : ob_att_vol(p, flags);
}

LOCAL ER svc_det_vol( CONST char *path )
{
	char		k[FS_PATH_MAX];
	CONST char	*p;
	ER		er = path_in(path, k, &p);

	return ( er < E_OK ) ? er : ob_det_vol(p);
}

/* ---------------------------------------------------------------- the disk check (ts/tsfsck.h) */

/*
 * Every volume, as many as there are: the kernel's copy taken for no
 * more than there are now (asked first) and the room the process gave,
 * and how many there are answered, as ob_chk_list answers
 */
LOCAL INT svc_chk_list( T_TSFSCKDEV *buf, INT max )
{
	T_TSFSCKDEV	*k = NULL;
	INT		n, have;

	if ( max < 0 ) return E_PAR;
	if ( buf == NULL ) max = 0;
	if ( !out_ok(buf, (SZ)sizeof(*buf) * max) ) return E_MACV;
	have = ob_chk_list(NULL, 0);
	if ( have < 0 ) return have;
	if ( max > have ) max = have;
	if ( max > 0 && ( k = (T_TSFSCKDEV *)Kmalloc(sizeof(*k) * (SZ)max) ) == NULL ) return E_NOMEM;
	n = ob_chk_list(k, max);
	if ( n > 0 && k != NULL ) {
		knl_ua_put(buf, k, (SZ)sizeof(*k) * ( ( n < max ) ? n : max ));
	}
	if ( k != NULL ) Kfree(k);
	return n;
}

/* A listed volume's label, whole: read into the kernel's memory, then as much as the process has room for */
LOCAL ER svc_chk_label( CONST char *devnm, UB *buf, SZ size, SZ *p_asize )
{
	char	kd[FS_PATH_MAX];
	UB	*lb = NULL;
	SZ	need = 0, cap;
	ER	er;

	if ( devnm == NULL ) return E_PAR;
	er = str_in((CONST UB *)devnm, (UB *)kd, sizeof(kd), FALSE);
	if ( er < E_OK ) return er;
	for ( cap = 64; ; cap = need ) {
		lb = (UB *)Kmalloc(cap);
		if ( lb == NULL ) return E_NOMEM;
		er = ob_chk_label(kd, lb, cap, &need);
		if ( er < E_OK || need <= cap ) break;
		Kfree(lb);
	}
	if ( er >= E_OK ) er = name_out(lb, need - 1, buf, size, p_asize);
	Kfree(lb);
	return er;
}

/* Where the events of a process's check go: its own memory, with the count after each */
typedef struct {
	ID		pid;
	T_TSFSCKEV	*ev;
	INT		max;
	INT		*told;
	INT		n;
} CHKOUT;

LOCAL void chk_put( void *ctx, CONST T_TSFSCKEV *ev )
{
	CHKOUT	*o = (CHKOUT *)ctx;

	/* the process's memory looked at again each time: it may have let go of it while the check ran */
	if ( o->n < o->max && knl_prc_user_ok(o->pid, &o->ev[o->n], (SZ)sizeof(*ev), TRUE) ) {
		knl_ua_put(&o->ev[o->n], ev, sizeof(*ev));
	}
	o->n++;
	__atomic_thread_fence(__ATOMIC_RELEASE);	/* the event before the count that says it is there */
	if ( o->told != NULL && knl_prc_user_ok(o->pid, o->told, (SZ)sizeof(INT), TRUE) ) {
		knl_ua_put(o->told, &o->n, sizeof(INT));
	}
}

LOCAL ER svc_chk_vol( CONST char *devnm, UINT mode, T_TSFSCK *res, T_TSFSCKEV *ev, INT max, INT *p_told )
{
	char		k[TSFSCK_DEVNM_MAX];
	T_TSFSCK	*r;
	CHKOUT		o;
	ER		er;

	if ( devnm == NULL || res == NULL || max < 0 || ( ev == NULL && max > 0 ) ) return E_PAR;
	er = str_in((CONST UB *)devnm, (UB *)k, sizeof(k), FALSE);
	if ( er < E_OK ) return er;
	if ( !out_ok(res, sizeof(*res)) || !out_ok(ev, (SZ)sizeof(*ev) * max) || !out_ok(p_told, sizeof(INT)) ) {
		return E_MACV;
	}
	r = (T_TSFSCK *)Kcalloc(1, sizeof(*r));
	if ( r == NULL ) return E_NOMEM;
	o.pid = ts_get_pid();
	o.ev = ev;
	o.max = max;
	o.told = p_told;
	o.n = 0;
	er = ob_chk_run(k, mode, r, chk_put, &o);
	if ( knl_prc_user_ok(o.pid, res, sizeof(*res), TRUE) ) {
		knl_ua_put(res, r, sizeof(*r));
	}
	Kfree(r);
	return er;
}

LOCAL ER svc_chk_request( CONST char *devnm, UINT mode )
{
	char	k[TSFSCK_DEVNM_MAX];
	ER	er;

	if ( devnm == NULL ) return E_PAR;
	er = str_in((CONST UB *)devnm, (UB *)k, sizeof(k), FALSE);
	return ( er < E_OK ) ? er : ob_chk_request(k, mode);
}

/* What the repair at start did: to the first process that asks, once */
LOCAL INT svc_chk_boot( UB *devnm, INT nmax, T_TSFSCK *res, T_TSFSCKEV *ev, INT max )
{
	UB		d[TSFSCK_DEVNM_MAX];
	T_TSFSCK	*r;
	T_TSFSCKEV	*k = NULL;
	INT		n;

	if ( nmax < 0 || max < 0 ) return E_PAR;
	if ( devnm == NULL ) nmax = 0;
	if ( ev == NULL ) max = 0;
	if ( !out_ok(devnm, nmax) || !out_ok(res, sizeof(*res)) || !out_ok(ev, (SZ)sizeof(*ev) * max) ) {
		return E_MACV;
	}
	if ( ob_chk_boot_result(NULL, 0, NULL, NULL, 0) <= 0 || !knl_ob_chk_boot_once() ) {
		return 0;
	}
	r = (T_TSFSCK *)Kcalloc(1, sizeof(*r));
	if ( max > 0 ) k = (T_TSFSCKEV *)Kmalloc(sizeof(*k) * (SZ)max);
	if ( r == NULL || ( max > 0 && k == NULL ) ) {
		if ( r != NULL ) Kfree(r);
		if ( k != NULL ) Kfree(k);
		return E_NOMEM;
	}
	n = ob_chk_boot_result(d, sizeof(d), r, k, max);
	if ( nmax > 0 ) {
		INT	i;

		for ( i = 0; d[i] != 0 && i < nmax - 1 && i < (INT)sizeof(d) - 1; i++ ) ;
		knl_ua_put(devnm, d, i);
		knl_ua_put(devnm + i, "", 1);
	}
	if ( res != NULL ) knl_ua_put(res, r, sizeof(*r));
	if ( max > 0 && n > 0 ) knl_ua_put(ev, k, (SZ)sizeof(*k) * ( ( n < max ) ? n : max ));
	Kfree(r);
	if ( k != NULL ) Kfree(k);
	return n;
}

LOCAL ER svc_get_dom( CONST char *vol, T_OBPRT *pk_prt, SZ size, SZ *p_asize )
{
	char		k[FS_PATH_MAX];
	CONST char	*p;
	ER		er = path_in(vol, k, &p);

	if ( er < E_OK ) return er;
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(pk_prt, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return UACCESS(ob_get_dom(p, pk_prt, size, p_asize));
}

LOCAL ER svc_set_dom( CONST char *vol, CONST T_OBPRT *pk_prt )
{
	char		k[FS_PATH_MAX];
	CONST char	*p;
	T_OBPRT		*prt;
	ER		er = path_in(vol, k, &p);

	if ( er < E_OK ) return er;
	er = prt_in(pk_prt, &prt);
	if ( er < E_OK ) return er;
	er = ob_set_dom(p, prt);
	buf_free(prt);
	return er;
}

LOCAL ER svc_beg_trx( CONST TS_UUID *on )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(on, &k, &u);

	return ( er < E_OK ) ? er : ob_beg_trx(u);
}

LOCAL ER svc_end_trx( CONST TS_UUID *on, BOOL commit )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(on, &k, &u);

	return ( er < E_OK ) ? er : ob_end_trx(u, commit);
}

/* ---------------------------------------------------------------- notices */

LOCAL ID svc_ntf_evt( ID key, INT recno, CONST T_OBNTF *pk_ntf, ID port )
{
	T_OBNTF	n;

	if ( pk_ntf == NULL ) {
		return ob_ntf_evt(key, recno, NULL, port);
	}
	if ( !user_ok(pk_ntf, sizeof(*pk_ntf), FALSE) ) return E_MACV;
	knl_ua_get(&n, pk_ntf, sizeof(n));
	return ob_ntf_evt(key, recno, &n, port);
}

/* ---------------------------------------------------------------- what links show */

/*
 * The words of a link copied in from the process (its name, relationship,
 * applist and viewMode, as long as they are): the T_VOBJ then holds its
 * own copies, let go with tad_vobj_clear; the process's pointers are not
 * kept. E_MACV when one is not the process's to read.
 */
LOCAL ER vobj_words_in( T_VOBJ *v )
{
	UB	*u[4];
	SZ	l[4];
	UB	*kb;
	INT	i;
	ER	er = E_OK;

	u[0] = v->vname;	l[0] = v->vname_len;
	u[1] = v->vrel;		l[1] = v->vrel_len;
	u[2] = v->vapplist;	l[2] = v->vapplist_len;
	u[3] = v->vview;	l[3] = v->vview_len;
	v->vname = v->vrel = v->vapplist = v->vview = NULL;
	v->vname_len = v->vrel_len = v->vapplist_len = v->vview_len = 0;
	for ( i = 0; i < 4 && er >= E_OK; i++ ) {
		if ( u[i] == NULL || l[i] <= 0 ) continue;
		if ( !user_ok(u[i], l[i], FALSE) ) {
			er = E_MACV;
			break;
		}
		kb = (UB *)Kmalloc(l[i]);
		if ( kb == NULL ) {
			er = E_NOMEM;
			break;
		}
		knl_ua_get(kb, u[i], l[i]);
		er = tad_vobj_set(v, ( i == 0 ) ? TAD_W_NAME : ( i == 1 ) ? TAD_W_REL : ( i == 2 ) ? TAD_W_APPLIST : TAD_W_VIEW,
				  kb, l[i]);
		Kfree(kb);
	}
	if ( er < E_OK ) {
		tad_vobj_clear(v);
	}
	return er;
}

/*
 * The looks of n links, as the store holds them for the desktop
 * (ob_ref_vob, ts/tadview.h): each link copied in, its words with it, answered in the
 * kernel, and copied out with its icon. A link to an object the process
 * may not read is answered as a 虚身.
 */
/* Text of len bytes put in the process's room of max bytes, cut at a whole letter: FALSE when it did not fit */
LOCAL BOOL text_out( UB *room, INT max, CONST UB *s, SZ len )
{
	SZ	n;

	if ( room == NULL || max <= 0 ) return (BOOL)( room == NULL );
	n = knl_utf8_fit(s, len, max - 1);
	if ( n > 0 ) knl_ua_put(room, s, n);
	knl_ua_put(room + n, "", 1);
	return (BOOL)( n == len );
}

/* What a link's band says, whole: asked again in twice the room while it fills the room (in memory to Kfree) */
LOCAL UB *label_whole( CONST T_VOBJ *v, SZ *p_len )
{
	UB	*b;
	INT	cap = 256, n;

	for ( ;; ) {
		b = (UB *)Kmalloc(cap);
		if ( b == NULL ) return NULL;
		n = om_store_label(v, b, cap);
		if ( n < 0 ) {
			Kfree(b);
			return NULL;
		}
		if ( n < cap - 1 ) break;
		Kfree(b);
		cap *= 2;
	}
	*p_len = n;
	return b;
}

LOCAL INT svc_ref_vob( T_OBRVOB *pk, INT n )
{
	T_OBRVOB	*k;
	INT		i;
	CONST UW	*px;
	INT		w, h;
	TS_UUID		zero, seen[OB_RVOB_MAX];
	ER		seen_er[OB_RVOB_MAX];
	INT		nseen = 0, j;

	if ( pk == NULL || n <= 0 || n > OB_RVOB_MAX ) return E_PAR;
	if ( !user_ok(pk, (SZ)n * (SZ)sizeof(T_OBRVOB), TRUE) ) return E_MACV;
	k = (T_OBRVOB *)Kmalloc(sizeof(T_OBRVOB));
	if ( k == NULL ) return E_NOMEM;
	knl_memset(&zero, 0, sizeof(zero));
	for ( i = 0; i < n; i++ ) {
		knl_ua_get(k, &pk[i], sizeof(*k));
		if ( ( k->icon != NULL && ( k->icon_max < 0
		  || !user_ok(k->icon, (SZ)k->icon_max * (SZ)sizeof(UW), TRUE) ) )
		  || ( k->name != NULL && ( k->name_max < 0 || !user_ok(k->name, k->name_max, TRUE) ) )
		  || ( k->label != NULL && ( k->label_max < 0 || !user_ok(k->label, k->label_max, TRUE) ) ) ) {
			Kfree(k);
			return E_MACV;
		}
		{
			ER	we = vobj_words_in(&k->vobj);

			if ( we < E_OK ) {
				Kfree(k);
				return we;
			}
		}
		k->er = E_OK;
		k->state = 0;
		k->paper = TV_PAPER_LOOK;
		k->name_len = -1;
		k->label_len = -1;
		k->icon_w = k->icon_h = 0;
		/*
		 * whether it may be read, asked once a call for each object: the
		 * links of a window are often many to few objects, and the
		 * answer does not change within the call
		 */
		for ( j = 0; j < nseen && ts_uuid_cmp(&seen[j], &k->vobj.target) != 0; j++ ) {
			;
		}
		if ( j < nseen ) {
			k->er = seen_er[j];
		} else {
			k->er = ob_acc_obj(&k->vobj.target, OB_OP_READ);
			seen[nseen] = k->vobj.target;
			seen_er[nseen++] = k->er;
		}
		if ( k->er < E_OK ) {
			/* not to be read by it, or not to be found: drawn as a 虚身, with what the link carries */
			k->state = TV_ST_GONE;
		} else {
			if ( ( k->want & OB_VW_NAME ) != 0 ) {
				SZ	nl = 0;
				UB	*nm = om_store_name_dup(&k->vobj.target, &nl);

				k->name_len = ( nm != NULL ) ? (INT)nl : -1;
				if ( nm != NULL && !text_out(k->name, k->name_max, nm, nl) ) k->state |= OB_VW_CUT;
				if ( nm != NULL ) Kfree(nm);
			}
			if ( ( k->want & OB_VW_LABEL ) != 0 ) {
				SZ	ll = 0;
				UB	*lb = label_whole(&k->vobj, &ll);

				k->label_len = ( lb != NULL ) ? (INT)ll : -1;
				if ( lb != NULL && !text_out(k->label, k->label_max, lb, ll) ) k->state |= OB_VW_CUT;
				if ( lb != NULL ) Kfree(lb);
			}
			if ( ( k->want & OB_VW_PAPER ) != 0 && om_store_paper(&k->vobj.target, &k->paper) < E_OK ) {
				k->paper = TV_PAPER_LOOK;
			}
			if ( ( k->want & OB_VW_STATE ) != 0 ) {
				k->state = om_store_state_on(&k->vobj,
					( ts_uuid_cmp(&k->host, &zero) != 0 ) ? &k->host : NULL);
			}
			if ( ( k->want & OB_VW_ICON ) != 0 && k->icon != NULL
			  && om_store_icon(&k->vobj, &px, &w, &h) >= E_OK && px != NULL ) {
				k->icon_w = w;
				k->icon_h = h;
				if ( (SZ)w * h <= (SZ)k->icon_max ) {
					knl_ua_put(k->icon, px, (SZ)w * h * (SZ)sizeof(UW));
				} else {
					k->state |= OB_VW_SHORT;
				}
				om_store_icon_done(px);
			}
		}
		/* the answers only: what the process gave is left as it gave it */
		knl_ua_put(&pk[i].er, &k->er, sizeof(k->er));
		knl_ua_put(&pk[i].state, &k->state, sizeof(k->state));
		knl_ua_put(&pk[i].paper, &k->paper, sizeof(k->paper));
		knl_ua_put(&pk[i].name_len, &k->name_len, sizeof(k->name_len));
		knl_ua_put(&pk[i].label_len, &k->label_len, sizeof(k->label_len));
		knl_ua_put(&pk[i].icon_w, &k->icon_w, sizeof(k->icon_w));
		knl_ua_put(&pk[i].icon_h, &k->icon_h, sizeof(k->icon_h));
		tad_vobj_clear(&k->vobj);
	}
	Kfree(k);
	return n;
}

EXPORT const FP knl_ob_svc_tbl[TSN_OB_MAX + 1] = {
	[TSN_NUMBER(TSN_OB_OPN_OBJ)]	= F(svc_opn_obj),
	[TSN_NUMBER(TSN_OB_CLS_OBJ)]	= F(ob_cls_obj),
	[TSN_NUMBER(TSN_OB_CRE_OBJ)]	= F(svc_cre_obj),
	[TSN_NUMBER(TSN_OB_DEL_OBJ)]	= F(svc_del_obj),
	[TSN_NUMBER(TSN_OB_REF_OBJ)]	= F(svc_ref_obj),
	[TSN_NUMBER(TSN_OB_LST_OBJ)]	= F(svc_lst_obj),
	[TSN_NUMBER(TSN_OB_LNK_OBJ)]	= F(svc_lnk_obj),
	[TSN_NUMBER(TSN_OB_UNL_OBJ)]	= F(svc_unl_obj),
	[TSN_NUMBER(TSN_OB_REA_REC)]	= F(svc_rea_rec),
	[TSN_NUMBER(TSN_OB_WRI_REC)]	= F(svc_wri_rec),
	[TSN_NUMBER(TSN_OB_APD_REC)]	= F(svc_apd_rec),
	[TSN_NUMBER(TSN_OB_TRN_REC)]	= F(ob_trn_rec),
	[TSN_NUMBER(TSN_OB_DEL_REC)]	= F(ob_del_rec),
	[TSN_NUMBER(TSN_OB_LST_REC)]	= F(svc_lst_rec),
	[TSN_NUMBER(TSN_OB_GET_ATR)]	= F(svc_get_atr),
	[TSN_NUMBER(TSN_OB_SET_ATR)]	= F(svc_set_atr),
	[TSN_NUMBER(TSN_OB_GET_PRT)]	= F(svc_get_prt),
	[TSN_NUMBER(TSN_OB_SET_PRT)]	= F(svc_set_prt),
	[TSN_NUMBER(TSN_OB_DUP_KEY)]	= F(ob_dup_key),
	[TSN_NUMBER(TSN_OB_FND_NAM)]	= F(svc_fnd_nam),
	[TSN_NUMBER(TSN_OB_GET_CRD)]	= F(svc_get_crd),
	[TSN_NUMBER(TSN_OB_LOGIN)]	= F(svc_login),
	[TSN_NUMBER(TSN_OB_SET_PWD)]	= F(svc_set_pwd),
	[TSN_NUMBER(TSN_OB_ATT_VOL)]	= F(svc_att_vol),
	[TSN_NUMBER(TSN_OB_DET_VOL)]	= F(svc_det_vol),
	[TSN_NUMBER(TSN_OB_REA_RES)]	= F(svc_rea_res),
	[TSN_NUMBER(TSN_OB_WRI_RES)]	= F(svc_wri_res),
	[TSN_NUMBER(TSN_OB_DEL_RES)]	= F(svc_del_res),
	[TSN_NUMBER(TSN_OB_LST_RES)]	= F(svc_lst_res),
	[TSN_NUMBER(TSN_OB_BEG_TRX)]	= F(svc_beg_trx),
	[TSN_NUMBER(TSN_OB_END_TRX)]	= F(svc_end_trx),
	[TSN_NUMBER(TSN_OB_SCH_REC)]	= F(svc_sch_rec),
	[TSN_NUMBER(TSN_OB_TRS_REC)]	= F(ob_trs_rec),
	[TSN_NUMBER(TSN_OB_CPY_OBJ)]	= F(svc_cpy_obj),
	[TSN_NUMBER(TSN_OB_NTF_EVT)]	= F(svc_ntf_evt),
	[TSN_NUMBER(TSN_OB_CAN_EVT)]	= F(ob_can_evt),
	[TSN_NUMBER(TSN_OB_MAP_REC)]	= F(svc_map_rec),
	[TSN_NUMBER(TSN_OB_UNM_REC)]	= F(ob_unm_rec),
	[TSN_NUMBER(TSN_OB_GET_DOM)]	= F(svc_get_dom),
	[TSN_NUMBER(TSN_OB_SET_DOM)]	= F(svc_set_dom),
	[TSN_NUMBER(TSN_OB_GET_ICO)]	= F(svc_get_ico),
	[TSN_NUMBER(TSN_OB_SET_ICO)]	= F(svc_set_ico),
	[TSN_NUMBER(TSN_OB_REF_VOL)]	= F(svc_ref_vol),
	[TSN_NUMBER(TSN_OB_FND_LNK)]	= F(svc_fnd_lnk),
	[TSN_NUMBER(TSN_OB_LST_LNK)]	= F(svc_lst_lnk),
	[TSN_NUMBER(TSN_OB_END_ADM)]	= F(ob_end_adm),
	[TSN_NUMBER(TSN_OB_WIP_VOL)]	= F(svc_wip_vol),
	[TSN_NUMBER(TSN_OB_GET_ATV)]	= F(svc_get_atv),
	[TSN_NUMBER(TSN_OB_RPL_REC)]	= F(svc_rpl_rec),
	[TSN_NUMBER(TSN_OB_SYN_OBJ)]	= F(ob_syn_obj),
	[TSN_NUMBER(TSN_OB_REA_TMO)]	= F(svc_rea_tmo),
	[TSN_NUMBER(TSN_OB_FND_SYS)]	= F(svc_fnd_sys),
	[TSN_NUMBER(TSN_OB_ACC_OBJ)]	= F(svc_acc_obj),
	[TSN_NUMBER(TSN_OB_REF_VOB)]	= F(svc_ref_vob),
	[TSN_NUMBER(TSN_OB_REF_VID)]	= F(svc_ref_vid),
	[TSN_NUMBER(TSN_OB_DUP_OBJ)]	= F(svc_dup_obj),
	[TSN_NUMBER(TSN_OB_CHK_LST)]	= F(svc_chk_list),
	[TSN_NUMBER(TSN_OB_CHK_VOL)]	= F(svc_chk_vol),
	[TSN_NUMBER(TSN_OB_CHK_REQ)]	= F(svc_chk_request),
	[TSN_NUMBER(TSN_OB_CHK_BOOT)]	= F(svc_chk_boot),
	[TSN_NUMBER(TSN_OB_CRE_KEY)]	= F(svc_cre_key),
	[TSN_NUMBER(TSN_OB_REF_NAM)]	= F(svc_ref_nam),
	[TSN_NUMBER(TSN_OB_CHK_LABEL)]	= F(svc_chk_label),
	[TSN_NUMBER(TSN_OB_REF_OBJ2)]	= F(svc_ref_obj2),
};
