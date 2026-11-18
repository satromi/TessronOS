/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpu_obj.c
 *	BOs of the GPU as real objects (OB_T_STORAGE, OB_S_GPUBO;
 *	include/ts/gpuapp.h, docs/tessronos/13-gpu.md)
 *
 *	A process names a BO by its number in one of its contexts, and a
 *	number means nothing to another process; a name of gp_shr_bo does,
 *	but anyone who knows the number may take the BO in, and there are
 *	few of them. An object of this manager holds one reference to a BO
 *	for as long as a handle is open on it, so that its key, carried by a
 *	stream (OB_ST_MSG), is the right to the BO: the process given the key
 *	enters the BO in a context of its own by reading record OB_GB_IMPORT.
 *	The BO goes when the last handle closes and nothing else holds it.
 *	An object never opened belongs to the process that made it and goes
 *	when that process ends.
 *
 *	The GPU itself is a device object too ("gpu0", OB_S_GPU, a fixed
 *	UUID), made when the object layer starts whether or not a backend
 *	is registered, so that a program may open it and ask to be told
 *	(OB_E_CHANGE of record OB_GP_STATE) when a backend registers later.
 *	Its record is the common layer's lines (BACKEND, KIND, FEATURE) and
 *	the lines the backend gave.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include <ts/gpuapp.h>
#include "obj/obj.h"
#include "gpudev.h"

IMPORT T_GPUBO *knl_gpu_bo_named( INT ctx, UINT handle, INT name );	/* gpu_svc.c */
IMPORT ER knl_gpu_bo_import( INT ctx, T_GPUBO *bo, T_GPBO *out );

/*
 * The objects, as many as there are, in a list of their own under
 * gb_lock. The handle of an opened one is a number of its own (h),
 * never 0 and never that of another object alive, so that a handle of
 * one gone is not taken for a new one until the numbers come round.
 */
typedef struct gbobj {
	struct gbobj	*next;
	TS_UUID	uuid;
	INT	h;
	T_GPUBO	*bo;
	T_OBPRT	*prt;
	ID	maker;
	INT	opens;
	BOOL	opened;
} GBOBJ;

LOCAL GBOBJ	*gb = NULL;
LOCAL T_SPLOCK	gb_lock;
LOCAL UINT	gb_gen;

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* The object of a UUID; under the lock */
LOCAL GBOBJ *find_of( CONST TS_UUID *uuid )
{
	GBOBJ	*o;

	for ( o = gb; o != NULL; o = o->next ) {
		if ( same_uuid(&o->uuid, uuid) ) return o;
	}
	return NULL;
}

/* The object of a handle; under the lock */
LOCAL GBOBJ *obj_of( INT h )
{
	GBOBJ	*o;

	if ( h <= 0 ) return NULL;
	for ( o = gb; o != NULL; o = o->next ) {
		if ( o->h == h ) return o;
	}
	return NULL;
}

/* An object taken off the list; under the lock */
LOCAL void unlink_obj( GBOBJ *o )
{
	GBOBJ	**pp;

	for ( pp = &gb; *pp != NULL; pp = &(*pp)->next ) {
		if ( *pp == o ) {
			*pp = o->next;
			o->next = NULL;
			return;
		}
	}
}

/* A handle no live object has; under the lock */
LOCAL INT new_handle( void )
{
	INT	h;

	do {
		h = (INT)( ( ++gb_gen ) & 0x7FFFFFFF );
	} while ( h == 0 || obj_of(h) != NULL );
	return h;
}

LOCAL void obj_free( GBOBJ *o )
{
	if ( o->bo != NULL ) knl_gpu_bo_put(o->bo);
	if ( o->prt != NULL ) Kfree(o->prt);
	Kfree(o);
}

LOCAL ER gb_find( CONST TS_UUID *uuid )
{
	UINT	imask;
	BOOL	there;

	ISpinLock(&gb_lock, &imask);
	there = ( find_of(uuid) != NULL );
	ISpinUnlock(&gb_lock, &imask);
	return there ? E_OK : E_NOEXS;
}

LOCAL ER gb_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	UINT	imask;
	BOOL	there;

	ISpinLock(&gb_lock, &imask);
	there = ( find_of(uuid) != NULL );
	ISpinUnlock(&gb_lock, &imask);
	if ( !there ) return E_NOEXS;
	r->flags = OB_F_VOLATILE;
	r->refcnt = 0;
	r->nrec = OB_GB_NREC;
	r->size = 0;
	return E_OK;
}

LOCAL ER gb_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	T_OBPRT	*p = knl_ob_prt_new(0, 0), *mine = NULL;
	GBOBJ	*o;
	UINT	imask;

	if ( p == NULL ) return E_NOMEM;
	ISpinLock(&gb_lock, &imask);
	o = find_of(uuid);
	if ( o != NULL ) {
		mine = o->prt;
		p->owner = mine->owner;
		p->group = mine->group;
		p->mode = mine->mode;
		p->attr = mine->attr;
	}
	ISpinUnlock(&gb_lock, &imask);
	if ( o == NULL ) {
		Kfree(p);
		return E_NOEXS;
	}
	*p_prt = p;
	return E_OK;
}

LOCAL ER gb_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	T_OBGBMAKE	mk;
	GBOBJ		*o;
	UINT		imask;
	ER		er;

	if ( c->arg == NULL || c->argsz < (SZ)sizeof(mk) ) {
		return E_PAR;
	}
	knl_memcpy(&mk, c->arg, sizeof(mk));
	o = (GBOBJ *)Kmalloc(sizeof(GBOBJ));
	if ( o == NULL ) return E_NOMEM;
	knl_memset(o, 0, sizeof(*o));
	o->maker = ts_get_pid();
	o->prt = knl_ob_prt_new(0, 0);
	if ( o->prt == NULL ) {
		Kfree(o);
		return E_NOMEM;
	}
	o->prt->owner = prt->owner;
	o->prt->group = prt->group;
	o->prt->mode = ( c->prt != NULL ) ? prt->mode : 0600;
	o->prt->attr = prt->attr;
	er = ts_gen_uuid(&o->uuid);
	if ( er < E_OK ) {
		obj_free(o);
		return er;
	}
	o->bo = knl_gpu_bo_named(mk.ctx, mk.handle, mk.name);
	if ( o->bo == NULL ) {
		obj_free(o);
		return E_ID;			/* not a BO of the caller's, nor a name */
	}
	ISpinLock(&gb_lock, &imask);
	o->next = gb;
	gb = o;
	ISpinUnlock(&gb_lock, &imask);
	*p_uuid = o->uuid;
	return E_OK;
}

LOCAL ER gb_remove( CONST TS_UUID *uuid )
{
	GBOBJ	*o;
	UINT	imask;

	ISpinLock(&gb_lock, &imask);
	o = find_of(uuid);
	if ( o != NULL ) unlink_obj(o);
	ISpinUnlock(&gb_lock, &imask);
	if ( o == NULL ) return E_NOEXS;
	obj_free(o);
	return E_OK;
}

LOCAL INT gb_open( CONST TS_UUID *uuid, UINT ops )
{
	GBOBJ	*o;
	UINT	imask;
	INT	h = E_NOEXS;

	(void)ops;
	ISpinLock(&gb_lock, &imask);
	o = find_of(uuid);
	if ( o != NULL ) {
		o->opens++;
		o->opened = TRUE;
		if ( o->h == 0 ) o->h = new_handle();
		h = o->h;
	}
	ISpinUnlock(&gb_lock, &imask);
	return h;
}

LOCAL ER gb_close( INT h )
{
	GBOBJ	*o, *gone = NULL;
	UINT	imask;

	ISpinLock(&gb_lock, &imask);
	o = obj_of(h);
	if ( o != NULL && --o->opens <= 0 ) {
		gone = o;
		unlink_obj(o);
	}
	ISpinUnlock(&gb_lock, &imask);
	if ( gone != NULL ) obj_free(gone);
	return ( o != NULL ) ? E_OK : E_ID;
}

/* Record OB_GB_IMPORT: the BO entered in the reader's context */
LOCAL ER gb_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	T_OBGBIMP	imp;
	T_GPUBO		*bo = NULL;
	GBOBJ		*o;
	UINT		imask;
	ER		er;

	(void)nowait;
	if ( recno != OB_GB_IMPORT ) return E_NOEXS;
	if ( off != 0 || size < (SZ)sizeof(imp) ) return E_PAR;
	knl_memcpy(&imp, buf, sizeof(imp));
	ISpinLock(&gb_lock, &imask);
	o = obj_of(h);
	if ( o != NULL ) {
		bo = o->bo;
		knl_gpu_bo_get(bo);		/* held across the import */
	}
	ISpinUnlock(&gb_lock, &imask);
	if ( bo == NULL ) return E_NOEXS;
	er = knl_gpu_bo_import(imp.ctx, bo, &imp.bo);
	knl_gpu_bo_put(bo);
	if ( er < E_OK ) return er;
	knl_memcpy(buf, &imp, sizeof(imp));
	if ( p_asize != NULL ) *p_asize = sizeof(imp);
	return E_OK;
}

LOCAL ER gb_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	INT	k;

	(void)h;
	for ( k = 0; k < n && k < OB_GB_NREC; k++ ) {
		buf[k].recno = k;
		buf[k].rt = OB_RT_SYSDATA;
		buf[k].sub = 0;
		buf[k].size = ( k == OB_GB_IMPORT ) ? sizeof(T_OBGBIMP) : 0;
	}
	*p_cnt = k;
	return E_OK;
}

LOCAL ER gb_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	INT	n;

	(void)h;
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":\"\",\"gpu\":{\"bo\":true}}");
	if ( n < 0 ) return E_LIMIT;
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

LOCAL ER gb_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	GBOBJ	*o, *best;
	UINT	imask;
	INT	cnt = 0;

	ISpinLock(&gb_lock, &imask);
	while ( cnt < n ) {
		best = NULL;
		for ( o = gb; o != NULL; o = o->next ) {
			if ( cnt > 0 && ts_uuid_cmp(&o->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&o->uuid, from) <= 0 ) continue;
			if ( best == NULL || ts_uuid_cmp(&o->uuid, &best->uuid) < 0 ) best = o;
		}
		if ( best == NULL ) break;
		buf[cnt++] = best->uuid;
	}
	ISpinUnlock(&gb_lock, &imask);
	*p_cnt = cnt;
	return E_OK;
}

/*
 * The BO behind a key of the caller's, for one who is handed the key to
 * use the BO itself (the window manager, OB_WR_GSURF): the key is held
 * while the object is looked up, so that it cannot be closed under the
 * question, and the BO taken with a reference of its own, so that it
 * outlives the key.
 */
EXPORT ER knl_gpu_bo_of_key( ID key, UINT need, T_GPUBO **p_bo )
{
	T_OBKHOLD	h;
	TS_UUID		u;
	T_GPUBO		*bo = NULL;
	GBOBJ		*o;
	UINT		imask;
	ER		er;

	if ( p_bo == NULL ) return E_PAR;
	*p_bo = NULL;
	er = knl_ob_key_hold(key, &h, &u);
	if ( er < E_OK ) return er;
	if ( h.type != OB_T_STORAGE || h.sub != OB_S_GPUBO ) {
		er = E_OBJ;
	} else if ( ( h.ops & need ) != need ) {
		er = E_OACV;
	} else {
		ISpinLock(&gb_lock, &imask);
		o = find_of(&u);
		if ( o != NULL && o->bo != NULL ) {
			bo = o->bo;
			knl_gpu_bo_get(bo);
		}
		ISpinUnlock(&gb_lock, &imask);
		er = ( bo != NULL ) ? E_OK : E_NOEXS;
	}
	knl_ob_key_drop(&h);
	*p_bo = bo;
	return er;
}

/* The objects the process made and nobody opened, gone with it: one at a time, each freed with the lock let go */
EXPORT void knl_gpbo_prc_end( ID pid )
{
	GBOBJ	*o;
	UINT	imask;

	if ( pid <= 0 ) return;
	do {
		ISpinLock(&gb_lock, &imask);
		for ( o = gb; o != NULL && ( o->opened || o->maker != pid ); o = o->next ) ;
		if ( o != NULL ) unlink_obj(o);
		ISpinUnlock(&gb_lock, &imask);
		if ( o != NULL ) obj_free(o);
	} while ( o != NULL );
}

LOCAL CONST T_OBMGR gb_mgr = {
	OB_T_STORAGE, OB_S_GPUBO, "gpubo",
	gb_find, gb_ref, gb_prot, NULL, gb_create, gb_remove,
	gb_open, gb_close, gb_rea, NULL, NULL, NULL, NULL, gb_lrc,
	gb_gat, NULL, NULL, gb_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL,
	NULL
};

EXPORT ER knl_gpbo_init( void )
{
	INT	no = knl_ob_regist(&gb_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}

/* ---------------------------------------------------------------- the GPU's device object */

LOCAL CONST TS_UUID	gd_uuid = OB_UUID_GPU_INIT;
LOCAL T_SPLOCK		gd_lock;
LOCAL volatile BOOL	gd_added;
LOCAL UB		*gd_detail = NULL;	/* the backend's lines, as long as it gave them (Kmalloc) */
LOCAL INT		gd_dlen;

LOCAL CONST char *gd_kind( UINT kind )
{
	switch ( kind ) {
	case GPU_KIND_V3D:	return "v3d";
	case GPU_KIND_VIRTIO:	return "virtio";
	case GPU_KIND_INTEL:	return "intel";
	case GPU_KIND_TEST:	return "test";
	}
	return "unknown";
}

/* Record 1 whole: what the common layer knows, then the backend's lines; -1 when it does not fit in max */
LOCAL INT gd_build( void *ctx, UB *t, INT max )
{
	T_GPUINFO	info;
	UINT		imask;
	INT		n = 0, d;

	(void)ctx;
	if ( knl_gpu_ref(&info) >= E_OK ) {
		n = knl_oj_put(t, n, max, "BACKEND registered\nKIND ");
		n = knl_oj_put(t, n, max, gd_kind(info.kind));
		n = knl_oj_put(t, n, max, "\nFEATURE ");
		n = knl_oj_put_num(t, n, max, (D)info.feature);
		n = knl_oj_put(t, n, max, "\n");
	} else {
		n = knl_oj_put(t, n, max, "BACKEND none\n");
	}
	if ( n < 0 ) {
		return -1;
	}
	ISpinLock(&gd_lock, &imask);
	d = gd_dlen;
	if ( n + d > max ) {
		n = -1;			/* made again in more room */
	} else if ( d > 0 ) {
		knl_memcpy(t + n, gd_detail, d);
		n += d;
	}
	ISpinUnlock(&gd_lock, &imask);
	return n;
}

LOCAL INT gd_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>The GPU. Record 1: BACKEND (registered or none) and what the backend "
			  "tells, as key value lines. A change is told on it, when a backend registers "
			  "as well, so that one who opened it before may look again.</p>");
}

LOCAL ER gd_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	INT	n;
	SZ	k;

	(void)ctx;
	(void)pos;
	if ( recno != OB_GP_STATE ) return E_NOEXS;
	n = knl_oj_make(gd_build, NULL, &t);
	if ( n < 0 ) return (ER)n;
	k = ( off >= 0 && off < n ) ? n - off : 0;
	if ( size >= 0 && k > size ) k = size;
	if ( size > 0 && k > 0 ) knl_memcpy(buf, t + off, (INT)k);
	if ( p_asize != NULL ) *p_asize = ( size == 0 ) ? n : k;
	Kfree(t);
	return E_OK;
}

LOCAL UD gd_size( void *ctx, INT recno )
{
	(void)ctx;
	return ( recno == OB_GP_STATE ) ? knl_oj_make_len(gd_build, NULL) : 0;
}

LOCAL CONST T_OBDVOPS	gd_ops = { OB_S_GPU, "gpu", OB_GP_NREC, 0644, 0, 0, gd_text, NULL, gd_rea, NULL, gd_size, NULL };

EXPORT void knl_gpu_obj_start( void )
{
	/* set before the object exists: a backend that registers from now on is told */
	gd_added = TRUE;
	if ( knl_obdev_add((CONST UB *)GPU_OBJ_NAME, &gd_uuid, &gd_ops, NULL, NULL) < E_OK ) {
		gd_added = FALSE;
		tm_printf((UB *)"gpu: no object (gpu0)\n");
	}
}

EXPORT void knl_gpu_obj_told( void )
{
	if ( gd_added ) knl_obdev_post(&gd_uuid, OB_GP_STATE, OB_E_CHANGE, NULL);
}

/* The backend's lines, however long: a copy of their own put in place of the ones before */
EXPORT void knl_gpu_obj_detail( CONST UB *text, INT len )
{
	UB	*c = NULL, *old;
	UINT	imask;

	if ( len < 0 ) len = 0;
	if ( len > 0 ) {
		c = (UB *)Kmalloc((SZ)len);
		if ( c == NULL ) {
			tm_printf((UB *)"gpu: no memory for the %d bytes of the backend's lines: those before stay\n", len);
			return;
		}
		knl_memcpy(c, text, len);
	}
	ISpinLock(&gd_lock, &imask);
	old = gd_detail;
	gd_detail = c;
	gd_dlen = len;
	ISpinUnlock(&gd_lock, &imask);
	if ( old != NULL ) Kfree(old);
	knl_gpu_obj_told();
}
