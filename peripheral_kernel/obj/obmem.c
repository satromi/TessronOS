/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obmem.c
 *	The memory manager: storage objects kept in memory (design 18.2,
 *	18.6)
 *
 *	The same kind of object as a file -- metadata, records, protection,
 *	a reference count -- with the records in memory, gone when the
 *	system stops. Shared memory is such an object's records; a global
 *	name is such an object made with OB_F_GLOBAL, found by its name.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include "obj.h"

#define OBM_MAX		4096		/* objects */
#define OBM_REC_MAX	16		/* records of one */
#define OBM_REC_BIG	( 64 * 1024 * 1024 )	/* the most one record may hold */
#define OBM_MAP_MAX	16		/* processes one record is mapped into */

/*
 * A record. Once it is mapped into a process it lives in pages of its
 * own (pf, order), at one address of the shared window (va) for every
 * process it is mapped into (mpid), and keeps to their size.
 */
typedef struct {
	UB	*data;
	UD	size, cap;
	UINT	rt, sub;
	PFRAME	*pf;
	UINT	order;
	UBINT	va;
	INT	nmap;
	ID	mpid[OBM_MAP_MAX];
} OBMREC;

typedef struct {
	TS_UUID	uuid;
	UB	name[OB_NAME_MAX];
	BOOL	global;
	T_OBPRT	prt;
	UB	*meta;
	INT	metalen;
	UB	*icon;			/* its icon, or NULL */
	SZ	iconlen;
	INT	nrec;
	OBMREC	rec[OBM_REC_MAX];
	INT	refcnt;
	INT	opens;
	BOOL	doomed;			/* deleted while open: goes at the last close */
} OBMOBJ;

LOCAL OBMOBJ	*obm[OBM_MAX];
LOCAL ID	obm_mtx = 0;

#define LOCK()		tk_loc_mtx(obm_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obm_mtx)

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == b[i] );
}

LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; i < OBM_MAX; i++ ) {
		if ( obm[i] != NULL && !obm[i]->doomed
		  && ts_uuid_cmp(&obm[i]->uuid, uuid) == 0 ) {
			return i;
		}
	}
	return -1;
}

/* A record's memory given back: first taken out of every space it is in */
LOCAL void rec_free( OBMREC *r )
{
	INT	k;

	for ( k = 0; k < r->nmap; k++ ) {
		(void)knl_prc_unmap_shared(r->mpid[k], r->va, r->cap);
	}
	if ( r->pf != NULL ) {
		knl_free_pages(r->pf, r->order);
	} else if ( r->data != NULL ) {
		Kfree(r->data);
	}
	knl_memset(r, 0, sizeof(*r));
}

LOCAL void obj_free( INT i )
{
	OBMOBJ	*o = obm[i];
	INT	r;

	for ( r = 0; r < o->nrec; r++ ) {
		rec_free(&o->rec[r]);
	}
	if ( o->meta != NULL ) Kfree(o->meta);
	if ( o->icon != NULL ) Kfree(o->icon);
	Kfree(o);
	obm[i] = NULL;
}

LOCAL BOOL name_taken( CONST UB *name )
{
	INT	i;

	for ( i = 0; i < OBM_MAX; i++ ) {
		if ( obm[i] != NULL && !obm[i]->doomed && obm[i]->global
		  && same_name(obm[i]->name, name) ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT BOOL knl_obmem_name_taken( CONST UB *name )
{
	BOOL	t;

	LOCK();
	t = name_taken(name);
	UNLOCK();

	return t;
}

/* A record made big enough for 'need' bytes */
LOCAL ER rec_room( OBMREC *r, UD need )
{
	UB	*nd;
	UD	cap;

	if ( need <= r->cap ) {
		return E_OK;
	}
	if ( need > OBM_REC_BIG || r->pf != NULL ) {
		return E_LIMIT;			/* mapped: it keeps to its pages */
	}
	cap = ( r->cap < 256 ) ? 256 : r->cap;
	while ( cap < need ) cap *= 2;
	nd = (UB *)Kmalloc((SZ)cap);
	if ( nd == NULL ) {
		return E_NOMEM;
	}
	if ( r->data != NULL ) {
		knl_memcpy(nd, r->data, (INT)r->size);
		Kfree(r->data);
	}
	r->data = nd;
	r->cap = cap;

	return E_OK;
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obm_find( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obm_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	INT	i, k;

	LOCK();
	i = index_of(uuid);
	if ( i < 0 ) {
		UNLOCK();
		return E_NOEXS;
	}
	knl_memcpy(r->name, obm[i]->name, OB_NAME_MAX);
	r->flags = OB_F_VOLATILE | ( obm[i]->global ? OB_F_GLOBAL : 0 );
	r->refcnt = obm[i]->refcnt;
	r->nrec = obm[i]->nrec;
	r->size = 0;
	for ( k = 0; k < obm[i]->nrec; k++ ) {
		r->size += obm[i]->rec[k].size;
	}
	UNLOCK();

	return E_OK;
}

LOCAL ER obm_prot( CONST TS_UUID *uuid, T_OBPRT *prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *prt = obm[i]->prt;
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obm_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) obm[i]->prt = *prt;
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obm_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	OBMOBJ	*o;
	INT	i, slot = -1, n;
	BOOL	global = (BOOL)( ( c->flags & OB_F_GLOBAL ) != 0 );
	ER	er;

	if ( global && ( c->name == NULL || c->name[0] == 0 ) ) {
		return E_PAR;			/* a global name needs a name */
	}
	o = (OBMOBJ *)Kmalloc(sizeof(OBMOBJ));
	if ( o == NULL ) {
		return E_NOMEM;
	}
	knl_memset(o, 0, sizeof(*o));
	/* the identity the maker chose (the name manager has let it), or a new one */
	if ( !knl_ob_uuid_zero(&c->uuid) ) {
		o->uuid = c->uuid;
	} else {
		er = ts_gen_uuid(&o->uuid);
		if ( er < E_OK ) {
			Kfree(o);
			return er;
		}
	}
	for ( n = 0; c->name != NULL && c->name[n] != 0 && n < OB_NAME_MAX - 1; n++ ) {
		o->name[n] = c->name[n];
	}
	o->global = global;
	o->prt = *prt;

	/* the metadata: as given, or just the name */
	o->meta = (UB *)Kmalloc(OB_META_MAX);
	if ( o->meta == NULL ) {
		Kfree(o);
		return E_NOMEM;
	}
	if ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < OB_META_MAX ) {
		knl_memcpy(o->meta, c->json, (INT)c->jsonsz);
		o->metalen = (INT)c->jsonsz;
	} else {
		n = knl_oj_put(o->meta, 0, OB_META_MAX, "{\"name\":");
		n = knl_oj_put_str(o->meta, n, OB_META_MAX, o->name);
		n = knl_oj_put(o->meta, n, OB_META_MAX, "}");
		o->metalen = ( n > 0 ) ? n : 0;
	}

	if ( c->icon != NULL && c->iconsz > 0 && c->iconsz <= OB_ICO_MAX ) {
		o->icon = (UB *)Kmalloc(c->iconsz);
		if ( o->icon == NULL ) {
			Kfree(o->meta);
			Kfree(o);
			return E_NOMEM;
		}
		knl_memcpy(o->icon, c->icon, (INT)c->iconsz);
		o->iconlen = c->iconsz;
	}

	/* the channels' names asked first, with no lock of ours held */
	if ( global && knl_obchan_name_taken(o->name) ) {
		Kfree(o->meta);
		Kfree(o);
		return E_OBJ;
	}
	LOCK();
	if ( global && name_taken(o->name) ) {
		UNLOCK();
		Kfree(o->meta);
		Kfree(o);
		return E_OBJ;			/* the name is someone else's */
	}
	for ( i = 0; i < OBM_MAX; i++ ) {
		if ( obm[i] == NULL ) {
			slot = i;
			break;
		}
	}
	if ( slot >= 0 ) {
		obm[slot] = o;
	}
	UNLOCK();
	if ( slot < 0 ) {
		Kfree(o->meta);
		Kfree(o);
		return E_LIMIT;
	}
	*p_uuid = o->uuid;

	return E_OK;
}

LOCAL ER obm_remove( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		if ( obm[i]->opens > 0 ) {
			obm[i]->doomed = TRUE;
		} else {
			obj_free(i);
		}
	}
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL INT obm_open( CONST TS_UUID *uuid, UINT ops )
{
	INT	i;

	(void)ops;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) obm[i]->opens++;
	UNLOCK();

	return ( i >= 0 ) ? i + 1 : E_NOEXS;
}

LOCAL OBMOBJ *obj_of( INT h )
{
	return ( h >= 1 && h <= OBM_MAX ) ? obm[h - 1] : NULL;
}

LOCAL ER obm_close( INT h )
{
	OBMOBJ	*o;

	LOCK();
	o = obj_of(h);
	if ( o != NULL && --o->opens <= 0 && o->doomed ) {
		obj_free(h - 1);
	}
	UNLOCK();

	return ( o != NULL ) ? E_OK : E_ID;
}

LOCAL ER obm_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBMOBJ	*o;
	SZ	n = 0;
	ER	er = E_OK;

	(void)nowait;
	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		er = ( o == NULL ) ? E_ID : E_NOEXS;
	} else if ( off < 0 ) {
		er = E_PAR;
	} else if ( (UD)off < o->rec[recno].size ) {
		n = (SZ)( o->rec[recno].size - (UD)off );
		if ( n > size ) n = size;
		knl_memcpy(buf, o->rec[recno].data + off, (INT)n);
	}
	UNLOCK();
	if ( er >= E_OK && p_asize != NULL ) {
		*p_asize = n;
	}
	return er;
}

LOCAL ER obm_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBMOBJ	*o;
	OBMREC	*r;
	ER	er = E_OK;

	(void)nowait;
	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		er = ( o == NULL ) ? E_ID : E_NOEXS;
	} else if ( off < 0 || size < 0 ) {
		er = E_PAR;
	} else {
		r = &o->rec[recno];
		er = rec_room(r, (UD)off + (UD)size);
		if ( er >= E_OK ) {
			if ( (UD)off > r->size ) {
				knl_memset(r->data + r->size, 0, (INT)( (UD)off - r->size ));
			}
			knl_memcpy(r->data + off, buf, (INT)size);
			if ( (UD)off + (UD)size > r->size ) {
				r->size = (UD)off + (UD)size;
			}
		}
	}
	UNLOCK();
	if ( er >= E_OK && p_asize != NULL ) {
		*p_asize = size;
	}
	return er;
}

LOCAL ER obm_apd( INT h, UINT rt, UINT sub, INT *p_recno )
{
	OBMOBJ	*o;
	ER	er = E_OK;
	INT	n = 0;

	LOCK();
	o = obj_of(h);
	if ( o == NULL ) {
		er = E_ID;
	} else if ( o->nrec >= OBM_REC_MAX ) {
		er = E_LIMIT;
	} else {
		n = o->nrec++;
		knl_memset(&o->rec[n], 0, sizeof(OBMREC));
		o->rec[n].rt = rt;
		o->rec[n].sub = sub;
	}
	UNLOCK();
	if ( er >= E_OK && p_recno != NULL ) {
		*p_recno = n;
	}
	return er;
}

LOCAL ER obm_trn( INT h, INT recno, UD size )
{
	OBMOBJ	*o;
	OBMREC	*r;
	ER	er = E_OK;

	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		er = ( o == NULL ) ? E_ID : E_NOEXS;
	} else {
		r = &o->rec[recno];
		if ( size > r->size ) {
			er = rec_room(r, size);
			if ( er >= E_OK ) {
				knl_memset(r->data + r->size, 0, (INT)( size - r->size ));
			}
		}
		if ( er >= E_OK ) {
			r->size = size;
		}
	}
	UNLOCK();

	return er;
}

/* Any record may go; the ones after it move up */
LOCAL ER obm_drc( INT h, INT recno )
{
	OBMOBJ	*o;
	ER	er = E_OK;
	INT	k;

	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		er = ( o == NULL ) ? E_ID : E_NOEXS;
	} else {
		rec_free(&o->rec[recno]);
		for ( k = recno; k + 1 < o->nrec; k++ ) {
			o->rec[k] = o->rec[k + 1];
		}
		o->nrec--;
		knl_memset(&o->rec[o->nrec], 0, sizeof(OBMREC));
	}
	UNLOCK();

	return er;
}

LOCAL ER obm_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBMOBJ	*o;
	INT	k = 0;

	LOCK();
	o = obj_of(h);
	for ( ; o != NULL && k < o->nrec && k < n; k++ ) {
		buf[k].recno = k;
		buf[k].rt = o->rec[k].rt;
		buf[k].sub = o->rec[k].sub;
		buf[k].size = o->rec[k].size;
	}
	UNLOCK();
	*p_cnt = k;

	return ( o != NULL ) ? E_OK : E_ID;
}

LOCAL ER obm_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBMOBJ	*o;
	INT	n = 0;

	LOCK();
	o = obj_of(h);
	if ( o != NULL ) {
		n = ( o->metalen < (INT)size ) ? o->metalen : (INT)size;
		knl_memcpy(json, o->meta, n);
	}
	UNLOCK();
	if ( o != NULL && p_asize != NULL ) {
		*p_asize = n;
	}
	return ( o != NULL ) ? E_OK : E_ID;
}

LOCAL ER obm_gic( INT h, UB *buf, SZ size, SZ *p_asize )
{
	OBMOBJ	*o;
	ER	er = E_OK;

	LOCK();
	o = obj_of(h);
	if ( o == NULL ) {
		er = E_ID;
	} else if ( o->icon == NULL ) {
		er = E_NOEXS;
	} else {
		if ( p_asize != NULL ) *p_asize = o->iconlen;
		if ( size > 0 ) knl_memcpy(buf, o->icon, (INT)( ( size < o->iconlen ) ? size : o->iconlen ));
	}
	UNLOCK();
	return er;
}

LOCAL ER obm_sic( INT h, CONST UB *buf, SZ size )
{
	OBMOBJ	*o;
	UB	*nb = NULL, *was = NULL;
	ER	er = E_OK;

	if ( size < 0 || size > OB_ICO_MAX ) {
		return E_PAR;
	}
	if ( size > 0 ) {
		nb = (UB *)Kmalloc(size);
		if ( nb == NULL ) {
			return E_NOMEM;
		}
		knl_memcpy(nb, buf, (INT)size);
	}
	LOCK();
	o = obj_of(h);
	if ( o == NULL ) {
		er = E_ID;
		was = nb;
	} else {
		was = o->icon;
		o->icon = nb;
		o->iconlen = size;
	}
	UNLOCK();
	if ( was != NULL ) Kfree(was);
	return er;
}

LOCAL ER obm_sat( INT h, CONST UB *json, SZ size )
{
	OBMOBJ	*o;
	INT	v;
	ER	er = E_OK;

	if ( size >= OB_META_MAX ) {
		return E_LIMIT;
	}
	LOCK();
	o = obj_of(h);
	if ( o == NULL ) {
		er = E_ID;
	} else {
		knl_memcpy(o->meta, json, (INT)size);
		o->metalen = (INT)size;
		o->meta[size] = 0;
		/* the name follows the metadata, except a global one, which is its identity */
		v = knl_oj_member(o->meta, o->metalen, knl_oj_root(o->meta, o->metalen), "name");
		if ( v >= 0 && !o->global ) {
			(void)knl_oj_str(o->meta, o->metalen, v, o->name, OB_NAME_MAX);
		}
	}
	UNLOCK();

	return er;
}

LOCAL ER obm_lnk( CONST TS_UUID *uuid, INT delta )
{
	INT	i;
	ER	er = E_OK;

	LOCK();
	i = index_of(uuid);
	if ( i < 0 ) {
		er = E_NOEXS;
	} else if ( obm[i]->refcnt + delta < 0 ) {
		er = E_OBJ;
	} else {
		obm[i]->refcnt += delta;
	}
	UNLOCK();

	return er;
}

/* In UUID order: the smallest n after 'from', picked one at a time */
LOCAL ER obm_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	LOCK();
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < OBM_MAX; i++ ) {
			if ( obm[i] == NULL || obm[i]->doomed ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&obm[i]->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&obm[i]->uuid, from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&obm[i]->uuid, &obm[best]->uuid) < 0 ) {
				best = i;
			}
		}
		if ( best < 0 ) break;
		buf[cnt++] = obm[best]->uuid;
	}
	UNLOCK();
	*p_cnt = cnt;

	return E_OK;
}

LOCAL ER obm_fndnam( CONST UB *name, TS_UUID *p_uuid )
{
	INT	i;
	ER	er = E_NOEXS;

	LOCK();
	for ( i = 0; i < OBM_MAX; i++ ) {
		if ( obm[i] != NULL && !obm[i]->doomed && obm[i]->global
		  && same_name(obm[i]->name, name) ) {
			*p_uuid = obm[i]->uuid;
			er = E_OK;
			break;
		}
	}
	UNLOCK();

	return er;
}

/*
 * The processes a record is mapped into that have ended without taking
 * it off: struck out when the list is full, to make room.
 */
LOCAL void prune_maps( INT h, INT recno )
{
	OBMOBJ	*o;
	ID	pids[OBM_MAP_MAX], dead[OBM_MAP_MAX];
	INT	n = 0, nd = 0, i, k;

	LOCK();
	o = obj_of(h);
	if ( o != NULL && recno >= 0 && recno < o->nrec && o->rec[recno].nmap == OBM_MAP_MAX ) {
		n = o->rec[recno].nmap;
		knl_memcpy(pids, o->rec[recno].mpid, (INT)sizeof(ID) * n);
	}
	UNLOCK();
	for ( i = 0; i < n; i++ ) {
		if ( knl_prc_uuid(pids[i], NULL, NULL) < E_OK ) {
			dead[nd++] = pids[i];
		}
	}
	if ( nd == 0 ) {
		return;
	}
	LOCK();
	o = obj_of(h);
	if ( o != NULL && recno >= 0 && recno < o->nrec ) {
		OBMREC	*r = &o->rec[recno];

		for ( i = 0; i < nd; i++ ) {
			for ( k = 0; k < r->nmap; k++ ) {
				if ( r->mpid[k] == dead[i] ) {
					r->mpid[k] = r->mpid[--r->nmap];
					break;
				}
			}
		}
	}
	UNLOCK();
}

/*
 * A record mapped into a process (target is -pid). The first time, it
 * moves into pages of its own and takes an address in the shared
 * window; every process it is mapped into sees it there.
 */
LOCAL ER obm_map( INT h, INT recno, INT target, T_OBMAP *m )
{
	OBMOBJ	*o;
	OBMREC	*r;
	ID	pid = -target;
	UD	pa = 0, cap = 0;
	UBINT	va = 0;
	ER	er = E_OK;
	INT	k;

	if ( target >= 0 || m == NULL ) {
		return E_NOSPT;			/* storage maps into a process only */
	}
	prune_maps(h, recno);
	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		UNLOCK();
		return ( o == NULL ) ? E_ID : E_NOEXS;
	}
	r = &o->rec[recno];
	if ( r->pf == NULL ) {
		UD	want = ( (UD)m->size > r->size ) ? (UD)m->size : r->size;
		UINT	order = 0;
		PFRAME	*pf;

		while ( ( (UD)PAGE_SIZE << order ) < want && order < PF_MAX_ORDER ) order++;
		if ( ( (UD)PAGE_SIZE << order ) < want ) {
			er = E_LIMIT;		/* more than one block of pages holds */
		} else if ( ( pf = knl_alloc_pages(order, ZONE_NORMAL, KAF_ZERO) ) == NULL ) {
			er = E_NOMEM;
		} else {
			UD	bytes = (UD)PAGE_SIZE << order;
			UB	*nd = (UB *)PA2VA(knl_pf_to_pa(pf));

			va = knl_shm_va_alloc(bytes);
			if ( va == 0 ) {
				knl_free_pages(pf, order);
				er = E_LIMIT;
			} else {
				if ( r->size > 0 ) {
					knl_memcpy(nd, r->data, (INT)r->size);
				}
				if ( r->data != NULL ) {
					Kfree(r->data);
				}
				r->data = nd;
				r->cap = bytes;
				r->pf = pf;
				r->order = order;
				r->va = va;
			}
		}
	}
	if ( er >= E_OK ) {
		pa = knl_pf_to_pa(r->pf);
		cap = r->cap;
		va = r->va;
		for ( k = 0; k < r->nmap && r->mpid[k] != pid; k++ ) ;
		if ( k == r->nmap ) {
			if ( r->nmap < OBM_MAP_MAX ) {
				r->mpid[r->nmap++] = pid;
			} else {
				er = E_LIMIT;
			}
		}
	}
	UNLOCK();
	if ( er >= E_OK ) {
		er = knl_prc_map_shared(pid, va, pa, cap, ( m->flags & OB_M_WRITE ) != 0);
	}
	if ( er >= E_OK ) {
		m->addr = (void *)va;
		m->size = (SZ)cap;
	}
	return er;
}

LOCAL ER obm_unm( INT h, INT recno, INT target )
{
	OBMOBJ	*o;
	OBMREC	*r;
	ID	pid = -target;
	UBINT	va = 0;
	UD	cap = 0;
	BOOL	was = FALSE;
	INT	k;

	if ( target >= 0 ) {
		return E_NOSPT;
	}
	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		UNLOCK();
		return ( o == NULL ) ? E_ID : E_NOEXS;
	}
	r = &o->rec[recno];
	for ( k = 0; k < r->nmap; k++ ) {
		if ( r->mpid[k] == pid ) {
			r->mpid[k] = r->mpid[--r->nmap];
			was = TRUE;
			break;
		}
	}
	va = r->va;
	cap = r->cap;
	UNLOCK();
	if ( !was ) {
		return E_OBJ;			/* not mapped there */
	}
	(void)knl_prc_unmap_shared(pid, va, cap);

	return E_OK;
}

LOCAL CONST T_OBMGR obm_mgr = {
	OB_T_STORAGE, OB_S_MEMORY, "memory",
	obm_find, obm_ref, obm_prot, obm_setprot, obm_create, obm_remove,
	obm_open, obm_close, obm_rea, obm_wri, obm_apd, obm_trn, obm_drc, obm_lrc,
	obm_gat, obm_sat, obm_lnk, obm_lst, obm_fndnam,
	NULL, NULL, NULL, NULL,
	obm_map, obm_unm,
	obm_gic, obm_sic
};

EXPORT ER knl_obmem_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obm_mtx = tk_cre_mtx(&cmtx);
	if ( obm_mtx <= 0 ) {
		return (ER)obm_mtx;
	}
	no = knl_ob_regist(&obm_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
