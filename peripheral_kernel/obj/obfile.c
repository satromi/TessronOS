/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obfile.c
 *	The file manager: storage objects kept on volumes (design 18.6)
 *
 *	The objects are TSFS's (design 11): this manager keeps the volumes
 *	that are attached, finds which one an object is on, and turns the
 *	basic operations into TSFS calls. What TSFS does not know -- the
 *	record types beyond xmlTAD and bytes, and the protection -- is kept
 *	in the metadata under "tessronos".
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tsfsobj.h>
#include "obj.h"

#define OBF_VOL_MAX	4
#define OBF_HND_MAX	4096
#define OBF_DECL_MAX	TSFS_MAX_REC

typedef struct {
	BOOL	used;
	ID	vol;
	UB	path[FS_PATH_MAX];
	T_OBPRT	*dom;			/* its protection domain, NULL for none */
} OBFVOL;

typedef struct {
	BOOL	used;
	ID	vol;
	ID	od;
} OBFHND;

/* A record's type as the metadata declares it */
typedef struct {
	INT	n;
	UINT	rt, sub;
} OBFDECL;

LOCAL OBFVOL	obf_vol[OBF_VOL_MAX];
LOCAL OBFHND	obf_hnd[OBF_HND_MAX];
LOCAL ID	obf_mtx = 0;

#define LOCK()		tk_loc_mtx(obf_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obf_mtx)

LOCAL CONST char obf_default_json[] =
	"{\"name\":\"\",\"refCount\":0,\"recordCount\":0,"
	"\"editable\":true,\"deletable\":true,\"readable\":true}";

/* ---------------------------------------------------------------- volumes */

EXPORT ID knl_obfile_vol_of( CONST TS_UUID *uuid )
{
	INT	i;

	if ( ts_uuid_cmp(uuid, &ob_uuid_domain) == 0 ) {
		return E_NOEXS;			/* a volume's domain is not an object to use */
	}
	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && knl_tsfs_exists(obf_vol[i].vol, uuid) >= E_OK ) {
			return obf_vol[i].vol;
		}
	}
	return E_NOEXS;
}

EXPORT ID knl_obfile_first_vol( void )
{
	INT	i;

	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used ) {
			return obf_vol[i].vol;
		}
	}
	return E_NOEXS;
}

LOCAL BOOL same_path( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* ---------------------------------------------------------------- protection domains */

/*
 * A volume's domain is the protection in the metadata of the object of
 * fixed identity ob_uuid_domain on it, which is on every volume that
 * has one and is never an object to anyone else. It is read when the
 * volume is attached and kept here, since every opening asks for it.
 */
LOCAL void dom_load( OBFVOL *v )
{
	T_OBPRT	*d = NULL;
	UB	*j = (UB *)Kmalloc(OB_META_MAX);
	INT	len = 0;

	if ( j != NULL && knl_tsfs_get_meta(v->vol, &ob_uuid_domain, j, OB_META_MAX - 1, &len) >= E_OK
	  && knl_oj_path(j, len, "tessronos", "access") >= 0 ) {
		d = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
		if ( d != NULL && knl_ob_prt_parse(j, len, d) < E_OK ) {
			Kfree(d);
			d = NULL;
		}
	}
	if ( j != NULL ) Kfree(j);
	LOCK();
	if ( v->dom != NULL ) Kfree(v->dom);
	v->dom = d;
	UNLOCK();
}

LOCAL OBFVOL *vent_by_path( CONST char *path );

EXPORT ER knl_obfile_domain( CONST TS_UUID *uuid, T_OBPRT *prt )
{
	ID	vol = knl_obfile_vol_of(uuid);
	ER	er = E_NOEXS;
	INT	i;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && obf_vol[i].vol == vol && obf_vol[i].dom != NULL ) {
			*prt = *obf_vol[i].dom;
			er = E_OK;
		}
	}
	UNLOCK();

	return er;
}

EXPORT ER knl_obfile_get_dom( CONST char *path, T_OBPRT *prt )
{
	OBFVOL	*v;
	ER	er = E_NOEXS;

	LOCK();
	v = vent_by_path(path);
	if ( v != NULL && v->dom != NULL ) {
		*prt = *v->dom;
		er = E_OK;
	}
	UNLOCK();

	return er;
}

EXPORT ER knl_obfile_set_dom( CONST char *path, CONST T_OBPRT *prt )
{
	OBFVOL	*v;
	UB	*j;
	INT	len;
	ER	er;
	CONST char *base = "{\"name\":\"保護区画\",\"refCount\":1,\"recordCount\":0}";

	LOCK();
	v = vent_by_path(path);
	UNLOCK();
	if ( v == NULL ) {
		return E_NOEXS;
	}
	if ( prt == NULL ) {
		/* it is counted once so that nothing collects it; let go of that first */
		er = E_OK;
		if ( knl_tsfs_exists(v->vol, &ob_uuid_domain) >= E_OK ) {
			(void)ts_unl_obj(v->vol, &ob_uuid_domain);
			er = ts_del_obj(v->vol, &ob_uuid_domain);
		}
		if ( er >= E_OK ) dom_load(v);
		return er;
	}
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	for ( len = 0; base[len] != 0; len++ ) j[len] = (UB)base[len];
	j[len] = 0;
	len = knl_ob_prt_store(j, len, OB_META_MAX, prt);
	if ( len < 0 ) {
		Kfree(j);
		return E_PAR;
	}
	er = ( knl_tsfs_exists(v->vol, &ob_uuid_domain) >= E_OK )
	   ? knl_tsfs_set_meta(v->vol, &ob_uuid_domain, j, len)
	   : knl_tsfs_cre_as(v->vol, &ob_uuid_domain, j, len);
	Kfree(j);
	if ( er >= E_OK ) {
		dom_load(v);
	}
	return er;
}

EXPORT ER knl_obfile_attach( CONST char *path, UINT flags )
{
	OBFVOL	*v = NULL;
	ID	vol;
	INT	i;

	if ( path == NULL ) {
		return E_PAR;
	}
	LOCK();
	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && same_path(obf_vol[i].path, path) ) {
			UNLOCK();
			return E_OK;		/* already there */
		}
		if ( !obf_vol[i].used && v == NULL ) {
			v = &obf_vol[i];
		}
	}
	if ( v == NULL ) {
		UNLOCK();
		return E_LIMIT;
	}
	vol = ts_opn_vol(path, flags);
	if ( vol <= 0 ) {
		UNLOCK();
		return ( vol < 0 ) ? (ER)vol : E_SYS;
	}
	v->vol = vol;
	for ( i = 0; path[i] != 0 && i < FS_PATH_MAX - 1; i++ ) {
		v->path[i] = (UB)path[i];
	}
	v->path[i] = 0;
	v->dom = NULL;
	v->used = TRUE;
	UNLOCK();

	dom_load(v);
	knl_obdev_volume_up();			/* the device box has somewhere to live */

	return E_OK;
}

/* The slot of the volume attached with a path, looked at under the lock */
LOCAL OBFVOL *vent_by_path( CONST char *path )
{
	INT	i;

	for ( i = 0; path != NULL && i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && same_path(obf_vol[i].path, path) ) {
			return &obf_vol[i];
		}
	}
	return NULL;
}

/* The volume attached with a path */
LOCAL ID vol_by_path( CONST char *path )
{
	INT	i;

	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && same_path(obf_vol[i].path, path) ) {
			return obf_vol[i].vol;
		}
	}
	return E_NOEXS;
}

/* A transaction of the volume an object is on, begun or ended */
EXPORT ER knl_obfile_trx( CONST TS_UUID *on, BOOL begin, BOOL commit )
{
	ID	vol = knl_obfile_vol_of(on);

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return begin ? ts_beg_trx(vol) : ts_end_trx(vol, commit);
}

EXPORT ER knl_obfile_detach( CONST char *path )
{
	INT	i;
	ER	er = E_NOEXS;

	LOCK();
	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		if ( obf_vol[i].used && same_path(obf_vol[i].path, path) ) {
			er = ts_cls_vol(obf_vol[i].vol);
			if ( er >= E_OK ) {
				obf_vol[i].used = FALSE;
				if ( obf_vol[i].dom != NULL ) {
					Kfree(obf_vol[i].dom);
					obf_vol[i].dom = NULL;
				}
			}
			break;
		}
	}
	UNLOCK();

	return er;
}

/* ---------------------------------------------------------------- record types */

LOCAL INT decl_read( CONST UB *j, INT len, OBFDECL *d, INT max )
{
	INT	arr = knl_oj_path(j, len, "tessronos", "records"), pos = arr, e, n = 0;

	while ( arr >= 0 && n < max && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		D	v;

		if ( j[e] != '{' ) continue;
		if ( !knl_oj_num(j, len, knl_oj_member(j, len, e, "n"), &v) ) continue;
		d[n].n = (INT)v;
		d[n].rt = knl_oj_num(j, len, knl_oj_member(j, len, e, "rt"), &v)
			? (UINT)v : OB_RT_SYSDATA;
		d[n].sub = knl_oj_num(j, len, knl_oj_member(j, len, e, "sub"), &v)
			 ? (UINT)v : 0;
		n++;
	}
	return n;
}

LOCAL INT decl_write( UB *j, INT len, INT max, CONST OBFDECL *d, INT n )
{
	UB	t[1024];
	INT	k = 0, i;

	k = knl_oj_put(t, k, sizeof(t), "[");
	for ( i = 0; i < n; i++ ) {
		k = knl_oj_put(t, k, sizeof(t), ( i > 0 ) ? ",{\"n\":" : "{\"n\":");
		k = knl_oj_put_num(t, k, sizeof(t), d[i].n);
		k = knl_oj_put(t, k, sizeof(t), ",\"rt\":");
		k = knl_oj_put_num(t, k, sizeof(t), d[i].rt);
		k = knl_oj_put(t, k, sizeof(t), ",\"sub\":");
		k = knl_oj_put_num(t, k, sizeof(t), d[i].sub);
		k = knl_oj_put(t, k, sizeof(t), "}");
	}
	k = knl_oj_put(t, k, sizeof(t), "]");
	if ( k < 0 ) {
		return -1;
	}
	return knl_oj_set_path(j, len, max, "tessronos", "records", t, k);
}

/* The record the metadata says holds the program, or -1 */
LOCAL INT exec_rec( CONST UB *j, INT len )
{
	INT	v = knl_oj_path(j, len, "tessronos", "exec");
	D	r;

	if ( v >= 0 && knl_oj_num(j, len, knl_oj_member(j, len, v, "record"), &r) ) {
		return (INT)r;
	}
	return -1;
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obf_find( CONST TS_UUID *uuid )
{
	return ( knl_obfile_vol_of(uuid) > 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obf_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_ROBJ	ro;
	ID	vol = knl_obfile_vol_of(uuid);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	er = ts_ref_obj(vol, uuid, &ro);
	if ( er < E_OK ) {
		return er;
	}
	knl_memcpy(r->name, ro.name, OB_NAME_MAX);
	r->name[OB_NAME_MAX - 1] = 0;
	r->refcnt = ro.refcnt;
	r->nrec = ro.nrec;
	r->size = ro.size;
	if ( ( ro.flags & TSFS_F_AUTOREF ) != 0 ) {
		r->flags |= OB_F_AUTOREF;
	}

	return E_OK;
}

LOCAL ER obf_prot( CONST TS_UUID *uuid, T_OBPRT *prt )
{
	UB	*j;
	INT	len = 0;
	ID	vol = knl_obfile_vol_of(uuid);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_get_meta(vol, uuid, j, OB_META_MAX, &len);
	if ( er >= E_OK ) {
		er = knl_ob_prt_parse(j, len, prt);
	}
	Kfree(j);

	return er;
}

LOCAL ER obf_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	UB	*j;
	INT	len = 0;
	ID	vol = knl_obfile_vol_of(uuid);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_get_meta(vol, uuid, j, OB_META_MAX, &len);
	if ( er >= E_OK ) {
		len = knl_ob_prt_store(j, len, OB_META_MAX, prt);
		er = ( len < 0 ) ? E_LIMIT : knl_tsfs_set_meta(vol, uuid, j, len);
	}
	if ( er >= E_OK ) {
		/* the store's own flags say the same as the attributes */
		er = knl_tsfs_set_flags(vol, uuid,
				( ( prt->attr & OB_A_RONLY ) ? 0 : TSFSO_F_EDITABLE )
				| ( ( prt->attr & OB_A_PERM ) ? 0 : TSFSO_F_DELETABLE ),
				TSFSO_F_EDITABLE | TSFSO_F_DELETABLE);
	}
	Kfree(j);

	return er;
}

LOCAL ER obf_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	T_COBJ	cobj;
	UB	*j;
	INT	len, i;
	ID	vol;
	ER	er;

	vol = !knl_ob_uuid_zero(&c->near) ? knl_obfile_vol_of(&c->near)
	    : ( c->vol != NULL ) ? vol_by_path(c->vol) : knl_obfile_first_vol();
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	{
		/* making an object on a volume is writing to it */
		CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());
		BOOL		may = TRUE;

		LOCK();
		for ( i = 0; i < OBF_VOL_MAX; i++ ) {
			if ( obf_vol[i].used && obf_vol[i].vol == vol && obf_vol[i].dom != NULL ) {
				may = ( knl_ob_permit(crd, obf_vol[i].dom, -1) & OB_OP_WRITE ) != 0;
			}
		}
		UNLOCK();
		if ( !may ) {
			return E_OACV;		/* the volume's domain does not let it */
		}
	}
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	if ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < OB_META_MAX ) {
		knl_memcpy(j, c->json, (INT)c->jsonsz);
		len = (INT)c->jsonsz;
	} else {
		len = 0;
		for ( i = 0; obf_default_json[i] != 0; i++ ) {
			j[len++] = (UB)obf_default_json[i];
		}
	}
	j[len] = 0;
	if ( c->name != NULL ) {
		UB	q[OB_NAME_MAX * 2 + 8];
		INT	qn = knl_oj_put_str(q, 0, sizeof(q), c->name);

		len = ( qn > 0 ) ? knl_oj_set(j, len, OB_META_MAX, knl_oj_root(j, len),
					       "name", q, qn) : -1;
	}
	if ( len >= 0 ) {
		len = knl_ob_prt_store(j, len, OB_META_MAX, prt);
	}
	if ( len < 0 ) {
		Kfree(j);
		return E_PAR;
	}
	if ( c->icon != NULL && c->iconsz > 0 ) {
		/* made with its icon, in one transaction on the native store */
		er = knl_tsfs_cre_icon(vol, knl_ob_uuid_zero(&c->uuid) ? NULL : &c->uuid, j, len,
				      (CONST UB *)c->icon, c->iconsz, p_uuid);
		Kfree(j);
		return er;
	}
	if ( !knl_ob_uuid_zero(&c->uuid) ) {
		er = knl_tsfs_cre_as(vol, &c->uuid, j, len);
		if ( er >= E_OK ) {
			*p_uuid = c->uuid;
		}
		Kfree(j);
		return er;
	}
	cobj.json = j;
	cobj.jsonsz = len;
	er = ts_cre_obj(vol, &cobj, p_uuid);
	Kfree(j);

	return er;
}

LOCAL ER obf_remove( CONST TS_UUID *uuid )
{
	ID	vol = knl_obfile_vol_of(uuid);

	return ( vol > 0 ) ? ts_del_obj(vol, uuid) : E_NOEXS;
}

LOCAL INT obf_open( CONST TS_UUID *uuid, UINT ops )
{
	UINT	omode = TFO_READ;
	ID	vol = knl_obfile_vol_of(uuid), od;
	INT	i;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	if ( ( ops & ( OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR ) ) != 0 ) {
		omode |= TFO_WRITE;
	}
	od = ts_opn_obj(vol, uuid, omode);
	if ( od <= 0 ) {
		return ( od < 0 ) ? (INT)od : E_SYS;
	}
	LOCK();
	for ( i = 0; i < OBF_HND_MAX; i++ ) {
		if ( !obf_hnd[i].used ) {
			obf_hnd[i].used = TRUE;
			obf_hnd[i].vol = vol;
			obf_hnd[i].od = od;
			UNLOCK();
			return i + 1;
		}
	}
	UNLOCK();
	ts_cls_obj(od);

	return E_LIMIT;
}

LOCAL OBFHND *hnd_of( INT h )
{
	return ( h >= 1 && h <= OBF_HND_MAX && obf_hnd[h - 1].used ) ? &obf_hnd[h - 1] : NULL;
}

LOCAL ER obf_close( INT h )
{
	OBFHND	*o = hnd_of(h);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	er = ts_cls_obj(o->od);
	o->used = FALSE;

	return er;
}

LOCAL ER obf_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBFHND	*o = hnd_of(h);

	(void)nowait;
	return ( o != NULL ) ? ts_rea_rec(o->od, recno, off, buf, size, p_asize) : E_ID;
}

LOCAL ER obf_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBFHND	*o = hnd_of(h);

	(void)nowait;
	return ( o != NULL ) ? ts_wri_rec(o->od, recno, off, buf, size, p_asize) : E_ID;
}

/* The declarations in the metadata changed: one added (rt given) or taken away */
LOCAL ER decl_change( OBFHND *o, INT recno, BOOL add, UINT rt, UINT sub )
{
	OBFDECL	d[OBF_DECL_MAX];
	UB	*j;
	SZ	asz = 0;
	INT	n, i, k, len;
	ER	er;

	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = ts_get_met(o->od, j, OB_META_MAX - 1, &asz);
	if ( er < E_OK ) {
		Kfree(j);
		return er;
	}
	len = (INT)asz;
	j[len] = 0;
	n = decl_read(j, len, d, OBF_DECL_MAX);
	for ( i = 0, k = 0; i < n; i++ ) {
		if ( d[i].n != recno ) {
			d[k++] = d[i];
		}
	}
	if ( !add && k == n ) {
		Kfree(j);
		return E_OK;			/* nothing declared for it */
	}
	n = k;
	if ( add && n < OBF_DECL_MAX ) {
		d[n].n = recno;
		d[n].rt = rt;
		d[n].sub = sub;
		n++;
	}
	len = decl_write(j, len, OB_META_MAX, d, n);
	er = ( len < 0 ) ? E_LIMIT : ts_set_met(o->od, j, (SZ)len);
	Kfree(j);

	return er;
}

LOCAL ER obf_apd( INT h, UINT rt, UINT sub, INT *p_recno )
{
	OBFHND	*o = hnd_of(h);
	INT	n = 0;
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	/* the native store keeps the type with the record */
	er = ts_ins_rec(o->od, -1, rt, sub, &n);
	if ( er != E_NOSPT ) {
		if ( er >= E_OK && p_recno != NULL ) {
			*p_recno = n;
		}
		return er;
	}
	er = ts_apd_rec(o->od, ( rt == OB_RT_TAD ) ? TSFS_REC_XTAD : TSFS_REC_BIN, &n);
	if ( er < E_OK ) {
		return er;
	}
	/* xmlTAD is what an undeclared .xtad is taken for; the rest is declared */
	if ( rt != OB_RT_TAD ) {
		er = decl_change(o, n, TRUE, rt, sub);
	}
	if ( p_recno != NULL ) {
		*p_recno = n;
	}
	return er;
}

LOCAL ER obf_trn( INT h, INT recno, UD size )
{
	OBFHND	*o = hnd_of(h);

	return ( o != NULL ) ? ts_trn_rec(o->od, recno, size) : E_ID;
}

LOCAL ER obf_drc( INT h, INT recno )
{
	OBFHND	*o = hnd_of(h);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	er = ts_del_rec(o->od, recno);
	if ( er >= E_OK ) {
		(void)decl_change(o, recno, FALSE, 0, 0);
	}
	return er;
}

LOCAL ER obf_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBFHND	*o = hnd_of(h);
	T_RREC	*r;
	OBFDECL	d[OBF_DECL_MAX];
	UB	*j;
	SZ	asz = 0;
	INT	got = 0, nd = 0, i, k, ex = -1;
	ER	er;

	if ( o == NULL || buf == NULL || p_cnt == NULL || n <= 0 ) {
		return E_PAR;
	}
	r = (T_RREC *)Kmalloc(sizeof(T_RREC) * (SZ)n);
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( r == NULL || j == NULL ) {
		if ( r != NULL ) Kfree(r);
		if ( j != NULL ) Kfree(j);
		return E_NOMEM;
	}
	er = ts_lst_rec(o->od, r, n, &got);
	if ( er >= E_OK && ts_get_met(o->od, j, OB_META_MAX - 1, &asz) >= E_OK ) {
		nd = decl_read(j, (INT)asz, d, OBF_DECL_MAX);
		ex = exec_rec(j, (INT)asz);
	}
	for ( i = 0; er >= E_OK && i < got; i++ ) {
		buf[i].recno = r[i].recno;
		buf[i].size = r[i].size;
		buf[i].sub = r[i].sub;
		buf[i].rt = r[i].rt;
		if ( r[i].rt == OB_RT_SYSDATA && r[i].recno == ex ) {
			buf[i].rt = OB_RT_PROG;
		}
		for ( k = 0; k < nd; k++ ) {
			if ( d[k].n == r[i].recno ) {
				buf[i].rt = d[k].rt;
				buf[i].sub = d[k].sub;
				break;
			}
		}
	}
	if ( er >= E_OK ) {
		*p_cnt = got;
	}
	Kfree(r);
	Kfree(j);

	return er;
}

LOCAL ER obf_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);

	return ( o != NULL ) ? ts_get_met(o->od, json, size, p_asize) : E_ID;
}

LOCAL ER obf_sat( INT h, CONST UB *json, SZ size )
{
	OBFHND	*o = hnd_of(h);

	return ( o != NULL ) ? ts_set_met(o->od, json, size) : E_ID;
}

LOCAL ER obf_gic( INT h, UB *buf, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);

	return ( o != NULL ) ? ts_get_ico(o->od, buf, size, p_asize) : E_ID;
}

LOCAL ER obf_sic( INT h, CONST UB *buf, SZ size )
{
	OBFHND	*o = hnd_of(h);

	return ( o != NULL ) ? ts_set_ico(o->od, buf, size) : E_ID;
}

LOCAL ER obf_lnk( CONST TS_UUID *uuid, INT delta )
{
	ID	vol = knl_obfile_vol_of(uuid);

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return ( delta > 0 ) ? ts_lnk_obj(vol, uuid) : ts_unl_obj(vol, uuid);
}

/* Every volume's objects after 'from', put together in UUID order */
LOCAL ER obf_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	TS_UUID	*part, *merged;
	INT	cnt = 0, i;

	part = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	merged = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( part == NULL || merged == NULL ) {
		if ( part != NULL ) Kfree(part);
		if ( merged != NULL ) Kfree(merged);
		return E_NOMEM;
	}
	for ( i = 0; i < OBF_VOL_MAX; i++ ) {
		INT	got = 0, a, b, k;

		if ( !obf_vol[i].used
		  || ts_lst_obj(obf_vol[i].vol, from, part, n, &got) < E_OK || got <= 0 ) {
			continue;
		}
		for ( a = 0, b = 0; a < got; a++ ) {	/* the domain is not listed */
			if ( ts_uuid_cmp(&part[a], &ob_uuid_domain) != 0 ) {
				part[b++] = part[a];
			}
		}
		got = b;
		for ( a = 0, b = 0, k = 0; k < n && ( a < cnt || b < got ); k++ ) {
			if ( b >= got || ( a < cnt && ts_uuid_cmp(&buf[a], &part[b]) <= 0 ) ) {
				merged[k] = buf[a++];
			} else {
				merged[k] = part[b++];
			}
		}
		knl_memcpy(buf, merged, (INT)( sizeof(TS_UUID) * (SZ)k ));
		cnt = k;
	}
	Kfree(part);
	Kfree(merged);
	*p_cnt = cnt;

	return E_OK;
}

/* ---------------------------------------------------------------- resources */

LOCAL ER obf_rres( INT h, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_rea(o->vol, &u, name, off, buf, size, p_asize);
}

LOCAL ER obf_wres( INT h, CONST UB *name, CONST void *buf, SZ size )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_wri(o->vol, &u, name, buf, size);
}

LOCAL ER obf_dres( INT h, CONST UB *name )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_del(o->vol, &u, name);
}

LOCAL ER obf_lres( INT h, UB *buf, SZ size, INT *p_cnt )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_lst(o->vol, &u, buf, size, p_cnt);
}

LOCAL CONST T_OBMGR obf_mgr = {
	OB_T_STORAGE, OB_S_FILE, "file",
	obf_find, obf_ref, obf_prot, obf_setprot, obf_create, obf_remove,
	obf_open, obf_close, obf_rea, obf_wri, obf_apd, obf_trn, obf_drc, obf_lrc,
	obf_gat, obf_sat, obf_lnk, obf_lst, NULL,
	obf_rres, obf_wres, obf_dres, obf_lres,
	NULL, NULL,
	obf_gic, obf_sic
};

EXPORT ER knl_obfile_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obf_mtx = tk_cre_mtx(&cmtx);
	if ( obf_mtx <= 0 ) {
		return (ER)obf_mtx;
	}
	no = knl_ob_regist(&obf_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
