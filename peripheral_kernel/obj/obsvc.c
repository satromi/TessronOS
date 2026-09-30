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
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/ob.h>
#include <ts/fs.h>
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
		dst[i] = src[i];
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
	*k = *u;
	*pp = k;
	return E_OK;
}

/* size bytes of the process's copied into memory of the kernel's (NULL for none) */
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
	knl_memcpy(*p_copy, src, size);
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
	UB	name[OB_NAME_MAX];
	char	vol[FS_PATH_MAX];
	T_OBPRT	prt;
	void	*json, *arg, *icon;
} CRECOPY;

LOCAL void cre_free( CRECOPY *k )
{
	if ( k == NULL ) return;
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
	k->c = *u;
	if ( k->c.name != NULL ) {
		er = str_in(k->c.name, k->name, sizeof(k->name), TRUE);
		k->c.name = k->name;
	}
	if ( er >= E_OK && k->c.vol != NULL ) {
		er = str_in((CONST UB *)k->c.vol, (UB *)k->vol, sizeof(k->vol), FALSE);
		k->c.vol = k->vol;
	}
	if ( er >= E_OK && k->c.prt != NULL ) {
		if ( user_ok(k->c.prt, sizeof(T_OBPRT), FALSE) ) {
			k->prt = *k->c.prt;
			k->c.prt = &k->prt;
		} else {
			er = E_MACV;
		}
	}
	if ( er >= E_OK ) {
		er = buf_in(k->c.json, k->c.jsonsz, OB_ATR_MAX, &k->json);
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

LOCAL ID svc_opn_obj( CONST TS_UUID *uuid, UINT ops )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_opn_obj(u, ops);
}

LOCAL ER svc_cre_obj( CONST T_OBCRE *pk_cre, TS_UUID *p_uuid )
{
	CRECOPY	*k;
	ER	er;

	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	er = cre_in(pk_cre, &k);
	if ( er < E_OK ) return er;
	er = ob_cre_obj(( k != NULL ) ? &k->c : NULL, p_uuid);
	cre_free(k);
	return er;
}

LOCAL ER svc_del_obj( CONST TS_UUID *uuid )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	return ( er < E_OK ) ? er : ob_del_obj(u);
}

LOCAL ER svc_ref_obj( CONST TS_UUID *uuid, T_OBREF *pk_ref )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_ref, sizeof(*pk_ref)) ) return E_MACV;
	return ob_ref_obj(u, pk_ref);
}

LOCAL ER svc_ref_vol( CONST TS_UUID *uuid, T_OBVOL *pk_vol )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_vol, sizeof(*pk_vol)) ) return E_MACV;
	return ob_ref_vol(u, pk_vol);
}

LOCAL ER svc_lst_obj( UINT type, UINT sub, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(from, &k, &u);

	if ( er < E_OK ) return er;
	if ( n < 0 ) return E_PAR;
	if ( !out_ok(buf, (SZ)n * (SZ)sizeof(TS_UUID)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return ob_lst_obj(type, sub, u, buf, n, p_cnt);
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
	er = ob_cpy_obj(u, ( k != NULL ) ? &k->c : NULL, p_uuid);
	cre_free(k);
	return er;
}

/* ---------------------------------------------------------------- records */

LOCAL ER svc_rea_rec( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return ob_rea_rec(key, recno, off, buf, size, p_asize);
}

LOCAL ER svc_wri_rec( ID key, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !in_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return ob_wri_rec(key, recno, off, buf, size, p_asize);
}

LOCAL ER svc_apd_rec( ID key, UINT rt, UINT sub, INT *p_recno )
{
	if ( !out_ok(p_recno, sizeof(*p_recno)) ) return E_MACV;
	return ob_apd_rec(key, rt, sub, p_recno);
}

LOCAL ER svc_lst_rec( ID key, T_OBREC *buf, INT n, INT *p_cnt )
{
	if ( n < 0 ) return E_PAR;
	if ( !out_ok(buf, (SZ)n * (SZ)sizeof(T_OBREC)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return ob_lst_rec(key, buf, n, p_cnt);
}

LOCAL ER svc_sch_rec( ID key, INT from, UINT rt, UINT sub, UINT mask, INT *p_recno )
{
	if ( !out_ok(p_recno, sizeof(*p_recno)) ) return E_MACV;
	return ob_sch_rec(key, from, rt, sub, mask, p_recno);
}

LOCAL ER svc_map_rec( ID key, INT recno, ID target, T_OBMAP *pk_map )
{
	T_OBMAP	m;
	ER	er;

	if ( pk_map == NULL ) {
		return ob_map_rec(key, recno, target, NULL);
	}
	if ( !user_ok(pk_map, sizeof(*pk_map), TRUE) ) return E_MACV;
	m = *pk_map;
	er = ob_map_rec(key, recno, target, &m);
	if ( er >= E_OK && user_ok(pk_map, sizeof(*pk_map), TRUE) ) {
		*pk_map = m;			/* the address it was given */
	}
	return er;
}

/* ---------------------------------------------------------------- attributes, icon, resources */

LOCAL ER svc_get_atr( ID key, UB *json, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(json, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return ob_get_atr(key, json, size, p_asize);
}

LOCAL ER svc_set_atr( ID key, CONST UB *json, SZ size )
{
	void	*k;
	ER	er = buf_in(json, size, OB_ATR_MAX, &k);

	if ( er < E_OK ) return er;
	er = ob_set_atr(key, ( k != NULL ) ? (CONST UB *)k : json, size);
	buf_free(k);
	return er;
}

LOCAL ER svc_get_ico( ID key, UB *buf, SZ size, SZ *p_asize )
{
	if ( size < 0 ) return E_PAR;
	if ( !out_ok(buf, size) || !out_ok(p_asize, sizeof(*p_asize)) ) return E_MACV;
	return ob_get_ico(key, buf, size, p_asize);
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
	return ob_rea_res(key, n, off, buf, size, p_asize);
}

LOCAL ER svc_wri_res( ID key, CONST UB *name, CONST void *buf, SZ size )
{
	UB		k[OB_RES_NAME];
	CONST UB	*n;
	ER		er = res_name_in(name, k, &n);

	if ( er < E_OK ) return er;
	if ( size < 0 ) return E_PAR;
	if ( !in_ok(buf, size) ) return E_MACV;
	return ob_wri_res(key, n, buf, size);
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
	return ob_lst_res(key, buf, size, p_cnt);
}

/* ---------------------------------------------------------------- protection, names, users */

LOCAL ER svc_get_prt( CONST TS_UUID *uuid, T_OBPRT *pk_prt )
{
	CONST TS_UUID	*u;
	TS_UUID		k;
	ER		er = uuid_in(uuid, &k, &u);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_prt, sizeof(*pk_prt)) ) return E_MACV;
	return ob_get_prt(u, pk_prt);
}

/* A protection of the process's, copied into memory of the kernel's (NULL for NULL) */
LOCAL ER prt_in( CONST T_OBPRT *p, T_OBPRT **pp )
{
	*pp = NULL;
	if ( p == NULL ) {
		return E_OK;
	}
	if ( !user_ok(p, sizeof(*p), FALSE) ) {
		return E_MACV;
	}
	*pp = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( *pp == NULL ) {
		return E_NOMEM;
	}
	**pp = *p;
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
	UB	k[OB_NAME_MAX];
	ER	er;

	if ( name == NULL ) {
		return ob_fnd_nam(NULL, p_uuid);
	}
	er = str_in(name, k, sizeof(k), FALSE);
	if ( er < E_OK ) return er;
	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	return ob_fnd_nam(k, p_uuid);
}

/* A name of a link in a box: an object name or a file name */
#define LNK_NAME_MAX	256

/* The box and the name the process gives, copied; *pn NULL for no name */
LOCAL ER lnk_in( CONST TS_UUID *box, CONST UB *name, TS_UUID *kb, UB *kn, CONST TS_UUID **pb,
		 CONST UB **pn )
{
	ER	er = uuid_in(box, kb, pb);

	*pn = NULL;
	if ( er < E_OK || name == NULL ) {
		return er;
	}
	er = str_in(name, kn, LNK_NAME_MAX, FALSE);
	*pn = kn;
	return er;
}

LOCAL ER svc_fnd_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid )
{
	CONST TS_UUID	*b;
	CONST UB	*nm;
	TS_UUID		kb;
	UB		kn[LNK_NAME_MAX];
	ER		er = lnk_in(box, name, &kb, kn, &b, &nm);

	if ( er < E_OK ) return er;
	if ( !out_ok(p_uuid, sizeof(*p_uuid)) ) return E_MACV;
	return ob_fnd_lnk(b, nm, p_uuid);
}

LOCAL ER svc_lst_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *buf, INT n, INT *p_cnt )
{
	CONST TS_UUID	*b;
	CONST UB	*nm;
	TS_UUID		kb;
	UB		kn[LNK_NAME_MAX];
	ER		er = lnk_in(box, name, &kb, kn, &b, &nm);

	if ( er < E_OK ) return er;
	if ( n < 0 ) return E_PAR;
	if ( !out_ok(buf, (SZ)n * (SZ)sizeof(TS_UUID)) || !out_ok(p_cnt, sizeof(*p_cnt)) ) return E_MACV;
	return ob_lst_lnk(b, nm, buf, n, p_cnt);
}

LOCAL ER svc_get_crd( T_OBCRD *pk_crd )
{
	if ( !out_ok(pk_crd, sizeof(*pk_crd)) ) return E_MACV;
	return ob_get_crd(pk_crd);
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

LOCAL ER svc_get_dom( CONST char *vol, T_OBPRT *pk_prt )
{
	char		k[FS_PATH_MAX];
	CONST char	*p;
	ER		er = path_in(vol, k, &p);

	if ( er < E_OK ) return er;
	if ( !out_ok(pk_prt, sizeof(*pk_prt)) ) return E_MACV;
	return ob_get_dom(p, pk_prt);
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
	n = *pk_ntf;
	return ob_ntf_evt(key, recno, &n, port);
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
};
