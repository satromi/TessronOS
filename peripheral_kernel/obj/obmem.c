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
 *
 *	A record is a list of pages, each wherever the page allocator had
 *	one, so a record of any size up to OBM_REC_BIG is made of single
 *	pages and does not need a run of them. Mapped into a process, it
 *	takes a stretch of the shared window, the same in every process it
 *	is mapped into, and keeps to the pages it had then; the stretch goes
 *	back to the window when the last process lets it go.
 *
 *	An object made with OB_F_TRANSIENT has no name and is not listed: it
 *	is reached only by a key handed on (ob_dup_key). It goes by itself
 *	when nothing holds it any more -- no key open, no process it is
 *	mapped into, no link -- as an anonymous memory file does on Linux.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include "obj.h"

/*
 * The table of objects grows by OBM_GROW as it fills, up to OBM_MAX.
 * An object without a name is alive only while a key or a mapping
 * holds it, and a mapping is made through a key, so the name manager's
 * keys (OB_KEY_MAX, 65536) bound the objects alive at once; the named
 * ones, global names, are few.
 */
#define OBM_MAX		65536
#define OBM_GROW	256
#define OBM_HASH	1024		/* chains of objects, by UUID */
#define OBM_REC_MAX	16		/* records of one */

/*
 * The most one record holds: 2GB, the largest region Chromium makes
 * (base::PlatformSharedMemoryRegion refuses one over INT_MAX bytes),
 * rounded up to a page. Each page has its own entry in the list, so
 * this is a bound on the list (a top of 1024 leaves), not on runs of
 * memory. What the machine has free bounds it before this does.
 */
#define OBM_REC_BIG	( 2048ULL * 1024 * 1024 )
#define OBM_LEAF	( PAGE_SIZE / sizeof(UD) )	/* page addresses in a leaf of the list */

/*
 * The part of the machine's memory records do not take: the kernel's
 * own needs -- page tables, the stacks of new tasks, its allocations
 * -- must still be met when the programs share all they can.
 */
#define OBM_KEEP_DIV	64

/* A process a record is mapped into: at most once, at the record's address */
typedef struct {
	ID	pid;
	BOOL	write;
} OBMMAP;

/*
 * A record: its pages (leaf[i / OBM_LEAF][i % OBM_LEAF] the physical
 * address of page i), its size, and, while it is mapped, the stretch of
 * the shared window it is at (va, vlen bytes, every page of it mapped)
 * and the processes it is mapped into. There are as many processes in
 * the list as there are processes it is mapped into: each maps it once.
 */
typedef struct {
	UD	**leaf;
	UD	nleaf;			/* room in the top */
	UD	npg;			/* pages it has */
	UD	size;
	UINT	rt, sub;
	UBINT	va;
	UD	vlen;
	OBMMAP	*map;
	INT	nmap, mcap;
} OBMREC;

typedef struct {
	TS_UUID	uuid;
	UB	name[OB_NAME_MAX];
	BOOL	global;
	BOOL	transient;		/* goes when nothing holds it (OB_F_TRANSIENT) */
	BOOL	opened;			/* a key has been opened on it */
	ID	maker;			/* the process that made it, 0 the kernel */
	T_OBPRT	*prt;			/* its own copy */
	UB	*meta;
	INT	metalen;
	UB	*icon;			/* its icon, or NULL */
	SZ	iconlen;
	INT	nrec;
	OBMREC	rec[OBM_REC_MAX];
	INT	refcnt;
	INT	opens;
	BOOL	doomed;			/* deleted while open: goes at the last close */
	INT	hnext;			/* the next on its chain, as index + 1; 0 at the end */
} OBMOBJ;

LOCAL OBMOBJ	**obm = NULL;		/* obm_cap entries, NULL where free */
LOCAL INT	obm_cap = 0;
LOCAL INT	obm_count = 0;
LOCAL INT	obm_chain[OBM_HASH];	/* the first of each chain, as index + 1 */
LOCAL UD	obm_pages = 0;		/* pages the records hold */
LOCAL ID	obm_mtx = 0;

#define LOCK()		tk_loc_mtx(obm_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obm_mtx)

#define PAGES(b)	( ( (UD)(b) + PAGE_SIZE - 1 ) >> PAGE_SHIFT )

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == b[i] );
}

LOCAL INT hash_of( CONST TS_UUID *u )
{
	UD	x = u->d.hi ^ u->d.lo;

	x ^= x >> 29;
	x ^= x >> 17;
	return (INT)( x & ( OBM_HASH - 1 ) );
}

LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = obm_chain[hash_of(uuid)] - 1; i >= 0; i = obm[i]->hnext - 1 ) {
		if ( !obm[i]->doomed && ts_uuid_cmp(&obm[i]->uuid, uuid) == 0 ) {
			return i;
		}
	}
	return -1;
}

LOCAL void hash_del( INT i )
{
	INT	*p = &obm_chain[hash_of(&obm[i]->uuid)];

	while ( *p != 0 && *p != i + 1 ) {
		p = &obm[*p - 1]->hnext;
	}
	if ( *p == i + 1 ) {
		*p = obm[i]->hnext;
	}
}

/* A free slot of the table, made larger when there is none; -1 when it is at its most */
LOCAL INT slot_new( void )
{
	OBMOBJ	**nt;
	INT	i, ncap;

	for ( i = 0; i < obm_cap; i++ ) {
		if ( obm[i] == NULL ) return i;
	}
	if ( obm_cap >= OBM_MAX ) {
		return -1;
	}
	ncap = obm_cap + OBM_GROW;
	nt = (OBMOBJ **)Kmalloc(sizeof(OBMOBJ *) * (SZ)ncap);
	if ( nt == NULL ) {
		return -1;
	}
	for ( i = 0; i < ncap; i++ ) {
		nt[i] = ( i < obm_cap ) ? obm[i] : NULL;
	}
	if ( obm != NULL ) Kfree(obm);
	obm = nt;
	i = obm_cap;
	obm_cap = ncap;
	return i;
}

/* ---------------------------------------------------------------- pages of a record */

LOCAL UD pg_pa( CONST OBMREC *r, UD i )
{
	return r->leaf[i / OBM_LEAF][i % OBM_LEAF];
}

/* The pages from 'keep' on given back, and the leaves no longer needed */
LOCAL void rec_trim( OBMREC *r, UD keep )
{
	UD	i, nl;

	for ( i = keep; i < r->npg; i++ ) {
		knl_free_pages(knl_pa_to_pf(pg_pa(r, i)), 0);
	}
	if ( r->npg > keep ) {
		obm_pages -= r->npg - keep;
		r->npg = keep;
	}
	nl = ( keep + OBM_LEAF - 1 ) / OBM_LEAF;
	for ( i = nl; i < r->nleaf; i++ ) {
		if ( r->leaf[i] != NULL ) {
			Kfree(r->leaf[i]);
			r->leaf[i] = NULL;
		}
	}
	if ( keep == 0 && r->leaf != NULL ) {
		Kfree(r->leaf);
		r->leaf = NULL;
		r->nleaf = 0;
	}
}

/*
 * The record given pages up to npg, zero filled. Not past what the
 * machine can give without going under its share for the kernel; on
 * failure it has the pages it had.
 */
LOCAL ER rec_pages( OBMREC *r, UD npg )
{
	UD	had = r->npg, nl, i, freec, keep;

	if ( npg <= r->npg ) {
		return E_OK;
	}
	if ( npg > PAGES(OBM_REC_BIG) ) {
		return E_LIMIT;
	}
	freec = knl_pf_free_count(-1);
	keep = knl_pf_total_count(-1) / OBM_KEEP_DIV;
	if ( npg - had > freec || freec - ( npg - had ) < keep ) {
		return E_NOMEM;
	}
	nl = ( npg + OBM_LEAF - 1 ) / OBM_LEAF;
	if ( nl > r->nleaf ) {
		UD	ncap = ( nl < 4 ) ? 4 : nl * 2, k;
		UD	**nt;

		if ( ncap > ( PAGES(OBM_REC_BIG) + OBM_LEAF - 1 ) / OBM_LEAF ) {
			ncap = ( PAGES(OBM_REC_BIG) + OBM_LEAF - 1 ) / OBM_LEAF;
		}
		nt = (UD **)Kmalloc(sizeof(UD *) * (SZ)ncap);
		if ( nt == NULL ) {
			return E_NOMEM;
		}
		for ( k = 0; k < ncap; k++ ) {
			nt[k] = ( k < r->nleaf ) ? r->leaf[k] : NULL;
		}
		if ( r->leaf != NULL ) Kfree(r->leaf);
		r->leaf = nt;
		r->nleaf = ncap;
	}
	for ( i = had; i < npg; i++ ) {
		PFRAME	*pf;

		if ( r->leaf[i / OBM_LEAF] == NULL ) {
			r->leaf[i / OBM_LEAF] = (UD *)Kmalloc(PAGE_SIZE);
			if ( r->leaf[i / OBM_LEAF] == NULL ) break;
		}
		pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);
		if ( pf == NULL ) break;
		r->leaf[i / OBM_LEAF][i % OBM_LEAF] = knl_pf_to_pa(pf);
		r->npg = i + 1;
		obm_pages++;
	}
	if ( r->npg < npg ) {
		rec_trim(r, had);
		return E_NOMEM;
	}
	return E_OK;
}

/* Bytes of a record copied out (into != FALSE: in), through the kernel's view of its pages */
LOCAL void rec_copy( OBMREC *r, UD off, void *buf, UD n, BOOL in )
{
	UB	*b = (UB *)buf;

	while ( n > 0 ) {
		UD	pg = off >> PAGE_SHIFT, at = off & ( PAGE_SIZE - 1 );
		UD	k = PAGE_SIZE - at;
		UB	*p;

		if ( k > n ) k = n;
		p = (UB *)PA2VA(pg_pa(r, pg)) + at;
		if ( in ) {
			if ( b != NULL ) {
				knl_memcpy(p, b, (SZ)k);
			} else {
				knl_memset(p, 0, (SZ)k);
			}
		} else {
			knl_memcpy(b, p, (SZ)k);
		}
		if ( b != NULL ) b += k;
		off += k;
		n -= k;
	}
}

/*
 * The record made 'size' bytes: pages added for more (not while it is
 * mapped: it keeps to the pages it is mapped with), pages past it given
 * back for less when it is not mapped, and the bytes past the size
 * zero, so that what grows again reads as zeros.
 */
LOCAL ER rec_resize( OBMREC *r, UD size )
{
	UD	npg = PAGES(size);
	ER	er;

	if ( size > OBM_REC_BIG ) {
		return E_LIMIT;
	}
	if ( npg > r->npg ) {
		if ( r->va != 0 ) {
			return E_LIMIT;
		}
		er = rec_pages(r, npg);
		if ( er < E_OK ) return er;
	}
	if ( size < r->size ) {
		UD	end = ( r->va == 0 ) ? npg << PAGE_SHIFT : r->size;

		if ( r->va == 0 ) rec_trim(r, npg);
		if ( end > r->size ) end = r->size;
		if ( end > size ) rec_copy(r, size, NULL, end - size, TRUE);
	}
	r->size = size;

	return E_OK;
}

/* ---------------------------------------------------------------- mappings */

typedef struct {
	OBMREC	*r;
	BOOL	write;
} OBMSPC;

/* Every page of the record's stretch mapped into a space (under the process table's lock) */
LOCAL ER spc_map( void *space, void *arg )
{
	T_SPACE	*sp = (T_SPACE *)space;
	OBMSPC	*a = (OBMSPC *)arg;
	UD	i, n = a->r->vlen >> PAGE_SHIFT;
	ER	er = E_OK;

	for ( i = 0; i < n && er >= E_OK; i++ ) {
		er = knl_pt_map(sp->l0_pa, a->r->va + ( i << PAGE_SHIFT ), pg_pa(a->r, i), PAGE_SIZE,
				a->write ? PTE_PAGE_UDATA : PTE_PAGE_URO);
	}
	if ( er < E_OK ) {
		(void)knl_pt_unmap(sp->l0_pa, a->r->va, a->r->vlen);
	}
	return er;
}

/* The k-th process of the record's list taken off, and the stretch given back after the last */
LOCAL void map_drop( OBMREC *r, INT k )
{
	(void)knl_prc_unmap_shared(r->map[k].pid, r->va, r->vlen);
	r->map[k] = r->map[--r->nmap];
	if ( r->nmap == 0 ) {
		knl_shm_va_free(r->va, r->vlen);
		r->va = 0;
		r->vlen = 0;
	}
}

/* A record's memory given back: first taken out of every space it is in */
LOCAL void rec_free( OBMREC *r )
{
	while ( r->nmap > 0 ) {
		map_drop(r, r->nmap - 1);
	}
	rec_trim(r, 0);
	if ( r->map != NULL ) Kfree(r->map);
	knl_memset(r, 0, sizeof(*r));
}

LOCAL void obj_free( INT i )
{
	OBMOBJ	*o = obm[i];
	INT	r;

	for ( r = 0; r < o->nrec; r++ ) {
		rec_free(&o->rec[r]);
	}
	hash_del(i);
	if ( o->meta != NULL ) Kfree(o->meta);
	if ( o->icon != NULL ) Kfree(o->icon);
	if ( o->prt != NULL ) Kfree(o->prt);
	Kfree(o);
	obm[i] = NULL;
	obm_count--;
}

LOCAL BOOL obj_mapped( CONST OBMOBJ *o )
{
	INT	r;

	for ( r = 0; r < o->nrec; r++ ) {
		if ( o->rec[r].nmap > 0 ) return TRUE;
	}
	return FALSE;
}

/*
 * An object without a name that nothing holds any more, gone: no key
 * open, no process it is mapped into, no link to it. One never opened
 * is held by its maker until the maker ends.
 */
LOCAL void obj_settle( INT i )
{
	OBMOBJ	*o = obm[i];

	if ( o != NULL && o->transient && o->opens <= 0 && o->refcnt <= 0 && o->opened
	  && !obj_mapped(o) ) {
		obj_free(i);
	}
}

LOCAL BOOL name_taken( CONST UB *name )
{
	INT	i;

	for ( i = 0; i < obm_cap; i++ ) {
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
	r->flags = OB_F_VOLATILE | ( obm[i]->global ? OB_F_GLOBAL : 0 )
		 | ( obm[i]->transient ? OB_F_TRANSIENT : 0 );
	r->refcnt = obm[i]->refcnt;
	r->nrec = obm[i]->nrec;
	r->size = 0;
	for ( k = 0; k < obm[i]->nrec; k++ ) {
		r->size += obm[i]->rec[k].size;
	}
	UNLOCK();

	return E_OK;
}

LOCAL ER obm_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *p_prt = knl_ob_prt_dup(obm[i]->prt);
	UNLOCK();

	return ( i < 0 ) ? E_NOEXS : ( *p_prt == NULL ) ? E_NOMEM : E_OK;
}

LOCAL ER obm_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	T_OBPRT	*n = knl_ob_prt_dup(prt), *old = NULL;
	INT	i;

	if ( n == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		old = obm[i]->prt;
		obm[i]->prt = n;
	}
	UNLOCK();
	Kfree( ( i >= 0 ) ? old : n );

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL void obj_drop_new( OBMOBJ *o )
{
	if ( o->meta != NULL ) Kfree(o->meta);
	if ( o->icon != NULL ) Kfree(o->icon);
	if ( o->prt != NULL ) Kfree(o->prt);
	Kfree(o);
}

LOCAL ER obm_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	OBMOBJ	*o;
	INT	slot = -1, n, h;
	BOOL	global = (BOOL)( ( c->flags & OB_F_GLOBAL ) != 0 );
	ER	er;

	if ( global && ( c->name == NULL || c->name[0] == 0 ) ) {
		return E_PAR;			/* a global name needs a name */
	}
	if ( global && ( c->flags & OB_F_TRANSIENT ) != 0 ) {
		return E_PAR;			/* one found by its name is not one without */
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
	o->transient = (BOOL)( ( c->flags & OB_F_TRANSIENT ) != 0 );
	o->maker = ts_get_pid();
	o->prt = knl_ob_prt_dup(prt);
	if ( o->prt == NULL ) {
		Kfree(o);
		return E_NOMEM;
	}

	/* the metadata: as given, however long, or just the name */
	o->meta = (UB *)Kmalloc(( ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < 0x3FFFFFFF )
				  ? c->jsonsz : OB_NAME_MAX * 2 + 16 ) + 1);
	if ( o->meta == NULL ) {
		obj_drop_new(o);
		return E_NOMEM;
	}
	if ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < 0x3FFFFFFF ) {
		knl_memcpy(o->meta, c->json, (INT)c->jsonsz);
		o->metalen = (INT)c->jsonsz;
	} else {
		n = knl_oj_put(o->meta, 0, OB_NAME_MAX * 2 + 16, "{\"name\":");
		n = knl_oj_put_str(o->meta, n, OB_NAME_MAX * 2 + 16, o->name);
		n = knl_oj_put(o->meta, n, OB_NAME_MAX * 2 + 16, "}");
		o->metalen = ( n > 0 ) ? n : 0;
	}

	if ( c->icon != NULL && c->iconsz > 0 && c->iconsz <= OB_ICO_MAX ) {
		o->icon = (UB *)Kmalloc(c->iconsz);
		if ( o->icon == NULL ) {
			obj_drop_new(o);
			return E_NOMEM;
		}
		knl_memcpy(o->icon, c->icon, (INT)c->iconsz);
		o->iconlen = c->iconsz;
	}

	/* the channels' names asked first, with no lock of ours held */
	if ( global && knl_obchan_name_taken(o->name) ) {
		obj_drop_new(o);
		return E_OBJ;
	}
	LOCK();
	if ( global && name_taken(o->name) ) {
		UNLOCK();
		obj_drop_new(o);
		return E_OBJ;			/* the name is someone else's */
	}
	slot = slot_new();
	if ( slot >= 0 ) {
		obm[slot] = o;
		h = hash_of(&o->uuid);
		o->hnext = obm_chain[h];
		obm_chain[h] = slot + 1;
		obm_count++;
	}
	UNLOCK();
	if ( slot < 0 ) {
		obj_drop_new(o);
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
	if ( i >= 0 ) {
		obm[i]->opens++;
		obm[i]->opened = TRUE;
	}
	UNLOCK();

	return ( i >= 0 ) ? i + 1 : E_NOEXS;
}

LOCAL OBMOBJ *obj_of( INT h )
{
	return ( h >= 1 && h <= obm_cap ) ? obm[h - 1] : NULL;
}

LOCAL ER obm_close( INT h )
{
	OBMOBJ	*o;

	LOCK();
	o = obj_of(h);
	if ( o != NULL ) {
		if ( --o->opens <= 0 && o->doomed ) {
			obj_free(h - 1);
		} else {
			obj_settle(h - 1);
		}
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
	} else if ( off < 0 || size < 0 ) {
		er = E_PAR;
	} else if ( (UD)off < o->rec[recno].size ) {
		n = (SZ)( o->rec[recno].size - (UD)off );
		if ( n > size ) n = size;
		if ( n > 0 ) rec_copy(&o->rec[recno], (UD)off, buf, (UD)n, FALSE);
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
	} else if ( off < 0 || size < 0 || (UD)off + (UD)size > OBM_REC_BIG ) {
		er = ( off < 0 || size < 0 ) ? E_PAR : E_LIMIT;
	} else {
		r = &o->rec[recno];
		if ( (UD)off + (UD)size > r->size ) {
			er = rec_resize(r, (UD)off + (UD)size);
		}
		if ( er >= E_OK && size > 0 ) {
			rec_copy(r, (UD)off, (void *)buf, (UD)size, TRUE);
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
	ER	er;

	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		er = ( o == NULL ) ? E_ID : E_NOEXS;
	} else {
		er = rec_resize(&o->rec[recno], size);
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
	UB	*nm, *was = NULL;

	/* as long as it is: a buffer of its size, put in place of the one before */
	if ( size < 0 || size >= 0x3FFFFFFF ) {
		return E_PAR;
	}
	nm = (UB *)Kmalloc(size + 1);
	if ( nm == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	o = obj_of(h);
	if ( o == NULL ) {
		er = E_ID;
		was = nm;
	} else {
		was = o->meta;
		o->meta = nm;
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
	if ( was != NULL ) Kfree(was);

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
		obj_settle(i);
	}
	UNLOCK();

	return er;
}

/* In UUID order: the smallest n after 'from', picked one at a time; those without a name are not listed */
LOCAL ER obm_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	LOCK();
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < obm_cap; i++ ) {
			if ( obm[i] == NULL || obm[i]->doomed || obm[i]->transient ) continue;
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
	for ( i = 0; i < obm_cap; i++ ) {
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
 * A record mapped into a process (target is -pid). The first time, it
 * is given pages for at least m->size bytes and takes a stretch of the
 * shared window; every process it is mapped into sees it there, until
 * the last one lets it go. A process it is mapped into already keeps
 * its mapping, made writable when this one is; one mapped writable is
 * not made read-only by a mapping that is not.
 */
LOCAL ER obm_map( INT h, INT recno, INT target, T_OBMAP *m )
{
	OBMOBJ	*o;
	OBMREC	*r;
	OBMSPC	a;
	ID	pid = -target;
	UD	want;
	BOOL	write;
	ER	er = E_OK;
	INT	k;

	if ( target >= 0 || m == NULL ) {
		return E_NOSPT;			/* storage maps into a process only */
	}
	if ( m->size < 0 ) {
		return E_PAR;
	}
	write = (BOOL)( ( m->flags & OB_M_WRITE ) != 0 );
	LOCK();
	o = obj_of(h);
	if ( o == NULL || recno < 0 || recno >= o->nrec ) {
		UNLOCK();
		return ( o == NULL ) ? E_ID : E_NOEXS;
	}
	r = &o->rec[recno];
	want = ( (UD)m->size > r->size ) ? (UD)m->size : r->size;
	want = PAGES(( want > 0 ) ? want : 1);
	if ( r->va == 0 ) {
		UBINT	va;

		if ( want < r->npg ) want = r->npg;
		er = rec_pages(r, want);
		if ( er >= E_OK ) {
			va = knl_shm_va_alloc(want << PAGE_SHIFT);
			if ( va == 0 ) {
				er = E_LIMIT;
			} else {
				r->va = va;
				r->vlen = want << PAGE_SHIFT;
			}
		}
	} else if ( ( want << PAGE_SHIFT ) > r->vlen ) {
		er = E_LIMIT;			/* mapped: it keeps to its pages */
	}
	for ( k = 0; er >= E_OK && k < r->nmap && r->map[k].pid != pid; k++ ) ;
	if ( er >= E_OK && k == r->nmap ) {
		if ( r->nmap == r->mcap ) {
			INT	ncap = ( r->mcap < 4 ) ? 4 : r->mcap * 2;
			OBMMAP	*nm = (OBMMAP *)Kmalloc(sizeof(OBMMAP) * (SZ)ncap);

			if ( nm == NULL ) {
				er = E_NOMEM;
			} else {
				if ( r->map != NULL ) {
					knl_memcpy(nm, r->map, (INT)sizeof(OBMMAP) * r->nmap);
					Kfree(r->map);
				}
				r->map = nm;
				r->mcap = ncap;
			}
		}
		if ( er >= E_OK ) {
			r->map[k].pid = pid;
			r->map[k].write = write;
			r->nmap++;
			a.r = r;
			a.write = write;
			er = knl_prc_space_call(pid, spc_map, &a);
			if ( er < E_OK ) {
				r->map[k] = r->map[--r->nmap];
			}
		}
	} else if ( er >= E_OK && write && !r->map[k].write ) {
		a.r = r;
		a.write = TRUE;
		er = knl_prc_space_call(pid, spc_map, &a);
		if ( er >= E_OK ) {
			r->map[k].write = TRUE;
		}
	}
	if ( er < E_OK && r->nmap == 0 && r->va != 0 ) {
		knl_shm_va_free(r->va, r->vlen);
		r->va = 0;
		r->vlen = 0;
	}
	if ( er >= E_OK ) {
		m->addr = (void *)r->va;
		m->size = (SZ)r->vlen;
	}
	UNLOCK();

	return er;
}

LOCAL ER obm_unm( INT h, INT recno, INT target )
{
	OBMOBJ	*o;
	OBMREC	*r;
	ID	pid = -target;
	ER	er = E_OBJ;			/* not mapped there */
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
		if ( r->map[k].pid == pid ) {
			map_drop(r, k);
			er = E_OK;
			break;
		}
	}
	UNLOCK();

	return er;
}

/* ---------------------------------------------------------------- processes */

/*
 * A process is ending (prc_close_all, after its keys are closed and
 * while its space is still there): it is taken off every record it is
 * mapped into, and what it made and never opened goes. Then whatever
 * without a name nothing holds any more goes too.
 */
EXPORT void knl_obmem_prc_end( ID pid )
{
	OBMOBJ	*o;
	INT	i, r, k;

	if ( obm_mtx <= 0 || pid <= 0 ) {
		return;
	}
	LOCK();
	for ( i = 0; i < obm_cap; i++ ) {
		o = obm[i];
		if ( o == NULL ) continue;
		for ( r = 0; r < o->nrec; r++ ) {
			for ( k = 0; k < o->rec[r].nmap; k++ ) {
				if ( o->rec[r].map[k].pid == pid ) {
					map_drop(&o->rec[r], k);
					break;
				}
			}
		}
		if ( o->transient && !o->opened && o->maker == pid ) {
			o->opened = TRUE;	/* its maker can no longer open it */
		}
		obj_settle(i);
	}
	UNLOCK();
}

/*
 * What the records hold: the objects, the pages of all their records,
 * and of those the pages mapped into some process; with pid above 0,
 * the pages mapped into that one (*p_pid_pages)
 */
EXPORT INT knl_obmem_stat( UD *p_pages, UD *p_mapped, ID pid, UD *p_pid_pages )
{
	OBMOBJ	*o;
	UD	mapped = 0, mine = 0;
	INT	i, r, k, n;

	if ( obm_mtx <= 0 ) {
		return 0;
	}
	LOCK();
	for ( i = 0; i < obm_cap; i++ ) {
		o = obm[i];
		if ( o == NULL ) continue;
		for ( r = 0; r < o->nrec; r++ ) {
			if ( o->rec[r].nmap == 0 ) continue;
			mapped += o->rec[r].npg;
			for ( k = 0; pid > 0 && k < o->rec[r].nmap; k++ ) {
				if ( o->rec[r].map[k].pid == pid ) {
					mine += o->rec[r].vlen >> PAGE_SHIFT;
				}
			}
		}
	}
	n = obm_count;
	if ( p_pages != NULL ) *p_pages = obm_pages;
	UNLOCK();
	if ( p_mapped != NULL ) *p_mapped = mapped;
	if ( p_pid_pages != NULL ) *p_pid_pages = mine;
	return n;
}

LOCAL CONST T_OBMGR obm_mgr = {
	OB_T_STORAGE, OB_S_MEMORY, "memory",
	obm_find, obm_ref, obm_prot, obm_setprot, obm_create, obm_remove,
	obm_open, obm_close, obm_rea, obm_wri, obm_apd, obm_trn, obm_drc, obm_lrc,
	obm_gat, obm_sat, obm_lnk, obm_lst, obm_fndnam,
	NULL, NULL, NULL, NULL,
	obm_map, obm_unm,
	obm_gic, obm_sic,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	NULL			/* nothing comes in to wait for (ob_rea_tmo) */
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
