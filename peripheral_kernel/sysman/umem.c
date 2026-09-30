/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	umem.c
 *	Memory a process asks for (design 9.8.1, include/ts/umem.h).
 *
 *	A process holds stretches of addresses in [TS_MEM_BASE, TS_MEM_END)
 *	of its own space: its reservations, kept as a sorted list of
 *	intervals that do not touch (two that meet are one). A page exists
 *	only where the process asked for one; a reserved address with no
 *	page behind it faults like any address the process does not have.
 *	The pages are the space's, so taking the space down frees them with
 *	the rest; the list is freed with the process (knl_umem_free).
 *
 *	A page taken away while a task of the process is inside a call of
 *	the file layer, the sockets, the objects, drawing or the windows is
 *	held back rather than freed: such a call may hand the kernel's view
 *	of the process's pages to a disk (knl_prc_dma), which would go on
 *	moving data into a page given to someone else. The page is no
 *	longer mapped for the process, and is freed once no call of the
 *	process is under way (knl_umem_flush).
 *
 *	The caller (proc.c) holds the process table's lock, which keeps the
 *	space and the list while they are used here.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include <ts/umem.h>
#include "pfalloc.h"
#include "space.h"
#include "umem.h"

#define UMEM_FLOOR	4096		/* pages always left to the kernel (16MB) */
#define UMEM_FIRST	16		/* intervals the list starts with */

typedef struct {
	UBINT	s, e;			/* [s, e), page aligned */
} UMRNG;

struct umem {
	UMRNG	*r;
	INT	n, cap;
	UD	pages;			/* pages made and still mapped */
	UD	held;			/* first page held back (its PA), 0 for none */
	UD	nheld;
};

LOCAL BOOL pow2( UD a )
{
	return ( a != 0 && ( a & ( a - 1 ) ) == 0 );
}

LOCAL UBINT align_up( UBINT v, UD a )
{
	return ( v + a - 1 ) & ~(UBINT)( a - 1 );
}

LOCAL T_UMEM *um_get( T_UMEM **pp )
{
	T_UMEM	*um = *pp;

	if ( um == NULL ) {
		um = (T_UMEM *)Kcalloc(1, sizeof(T_UMEM));
		if ( um == NULL ) return NULL;
		um->r = (UMRNG *)Kmalloc(sizeof(UMRNG) * UMEM_FIRST);
		if ( um->r == NULL ) {
			Kfree(um);
			return NULL;
		}
		um->cap = UMEM_FIRST;
		*pp = um;
	}
	return um;
}

/* Room for one more interval */
LOCAL ER um_room( T_UMEM *um )
{
	UMRNG	*nr;

	if ( um->n < um->cap ) {
		return E_OK;
	}
	nr = (UMRNG *)Kmalloc(sizeof(UMRNG) * (UINT)( um->cap * 2 ));
	if ( nr == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(nr, um->r, (SZ)( sizeof(UMRNG) * (UINT)um->n ));
	Kfree(um->r);
	um->r = nr;
	um->cap *= 2;
	return E_OK;
}

/* The interval holding all of [s, e), or -1 */
LOCAL INT um_find( T_UMEM *um, UBINT s, UBINT e )
{
	INT	lo = 0, hi = um->n - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;

		if ( um->r[mid].e <= s ) {
			lo = mid + 1;
		} else if ( um->r[mid].s > s ) {
			hi = mid - 1;
		} else {
			return ( e <= um->r[mid].e ) ? mid : -1;
		}
	}
	return -1;
}

/* Whether [s, e) meets no interval */
LOCAL BOOL um_free_at( T_UMEM *um, UBINT s, UBINT e )
{
	INT	i;

	for ( i = 0; i < um->n; i++ ) {
		if ( um->r[i].s >= e ) break;
		if ( um->r[i].e > s ) return FALSE;
	}
	return TRUE;
}

/* The lowest free stretch of 'size' bytes on an 'align' boundary, 0 for none */
LOCAL UBINT um_place( T_UMEM *um, UD size, UD align )
{
	UBINT	cand = align_up(TS_MEM_BASE, align);
	INT	i;

	for ( i = 0; i < um->n; i++ ) {
		if ( cand + size <= um->r[i].s ) {
			return cand;
		}
		if ( um->r[i].e > cand ) {
			cand = align_up(um->r[i].e, align);
		}
	}
	return ( cand + size <= TS_MEM_END && cand + size > cand ) ? cand : 0;
}

/* Add [s, e), which meets no interval; one that it touches takes it in */
LOCAL ER um_add( T_UMEM *um, UBINT s, UBINT e )
{
	INT	i, j;

	for ( i = 0; i < um->n && um->r[i].s < s; i++ ) ;
	if ( i > 0 && um->r[i - 1].e == s ) {
		um->r[i - 1].e = e;
		if ( i < um->n && um->r[i].s == e ) {
			um->r[i - 1].e = um->r[i].e;
			for ( j = i; j < um->n - 1; j++ ) um->r[j] = um->r[j + 1];
			um->n--;
		}
		return E_OK;
	}
	if ( i < um->n && um->r[i].s == e ) {
		um->r[i].s = s;
		return E_OK;
	}
	if ( um_room(um) < E_OK ) {
		return E_NOMEM;
	}
	for ( j = um->n; j > i; j-- ) um->r[j] = um->r[j - 1];
	um->r[i].s = s;
	um->r[i].e = e;
	um->n++;
	return E_OK;
}

/* Take [s, e) out of interval i, which holds it */
LOCAL ER um_cut( T_UMEM *um, INT i, UBINT s, UBINT e )
{
	UMRNG	*r = &um->r[i];
	INT	j;

	if ( s == r->s && e == r->e ) {
		for ( j = i; j < um->n - 1; j++ ) um->r[j] = um->r[j + 1];
		um->n--;
	} else if ( s == r->s ) {
		r->s = e;
	} else if ( e == r->e ) {
		r->e = s;
	} else {
		if ( um_room(um) < E_OK ) {
			return E_NOMEM;
		}
		r = &um->r[i];
		for ( j = um->n; j > i + 1; j-- ) um->r[j] = um->r[j - 1];
		um->r[i + 1].s = e;
		um->r[i + 1].e = r->e;
		r->e = s;
		um->n++;
	}
	return E_OK;
}

LOCAL UD prot_attr( UINT prot )
{
	return ( ( prot & TS_MEM_WRITE ) != 0 ) ? PTE_PAGE_UDATA : PTE_PAGE_URO;
}

/* Give back or hold back one page that is no longer mapped */
LOCAL void page_gone( T_UMEM *um, UD pa, BOOL hold )
{
	if ( hold ) {
		*(UD *)PA2VA(pa) = um->held;
		um->held = pa;
		um->nheld++;
	} else {
		knl_free_pages(knl_pa_to_pf(pa), 0);
	}
}

/* Pages of [s, e) made and given 'prot'; E_NOMEM when memory runs out on the way */
LOCAL ER pages_make( T_UMEM *um, T_SPACE *sp, UBINT s, UBINT e, UINT prot )
{
	UD	attr = prot_attr(prot), need = 0;
	UBINT	va;

	for ( va = s; va < e; va += PAGE_SIZE ) {
		if ( knl_pt_entry(sp->l0_pa, va) == 0 ) need++;
	}
	if ( need + UMEM_FLOOR > knl_pf_free_count(-1) ) {
		return E_NOMEM;
	}
	for ( va = s; va < e; va += PAGE_SIZE ) {
		UD	ent = knl_pt_entry(sp->l0_pa, va);

		if ( ent == 0 ) {
			PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);

			if ( pf == NULL ) {
				return E_NOMEM;
			}
			if ( knl_pt_map(sp->l0_pa, va, knl_pf_to_pa(pf), PAGE_SIZE, attr) != E_OK ) {
				knl_free_pages(pf, 0);
				return E_NOMEM;
			}
			um->pages++;
		} else if ( ( ent & ~PTE_ADDR_MASK ) != attr ) {
			if ( knl_pt_map(sp->l0_pa, va, ent & PTE_ADDR_MASK, PAGE_SIZE, attr) != E_OK ) {
				return E_NOMEM;
			}
		}
	}
	return E_OK;
}

/* The pages of [s, e) taken away */
LOCAL void pages_drop( T_UMEM *um, T_SPACE *sp, UBINT s, UBINT e, BOOL hold )
{
	UBINT	va;

	for ( va = knl_pt_next(sp->l0_pa, s, e); va < e;
	      va = knl_pt_next(sp->l0_pa, va + PAGE_SIZE, e) ) {
		UD	pa = knl_pt_lookup(sp->l0_pa, va);

		if ( pa == ~(UD)0 ) continue;
		(void)knl_pt_unmap(sp->l0_pa, va, PAGE_SIZE);
		page_gone(um, pa & PTE_ADDR_MASK, hold);
		if ( um->pages > 0 ) um->pages--;
	}
}

LOCAL ER check_range( UBINT va, UD size )
{
	if ( size == 0 || ( ( va | size ) & ( PAGE_SIZE - 1 ) ) != 0 ) {
		return E_PAR;
	}
	if ( va < TS_MEM_BASE || va + size > TS_MEM_END || va + size < va ) {
		return E_PAR;
	}
	return E_OK;
}

EXPORT ER knl_umem_map( T_UMEM **pp, T_SPACE *sp, UBINT *p_va, UD size, UD align,
			UINT prot, BOOL hold )
{
	T_UMEM	*um;
	UBINT	va = *p_va;
	ER	er;

	if ( ( prot & TS_MEM_EXEC ) != 0 ) {
		return E_NOSPT;
	}
	if ( ( prot & ~(UINT)TS_MEM_RW ) != 0 ) {
		return E_PAR;
	}
	if ( align == 0 ) align = PAGE_SIZE;
	if ( !pow2(align) || align < PAGE_SIZE || size == 0
	  || size > TS_MEM_END - TS_MEM_BASE ) {
		return E_PAR;
	}
	size = align_up(size, PAGE_SIZE);
	um = um_get(pp);
	if ( um == NULL ) {
		return E_NOMEM;
	}

	/* the address asked for, when it is free and on the boundary */
	if ( !( va != 0 && ( va & ( align - 1 ) ) == 0 && check_range(va, size) == E_OK
	     && um_free_at(um, va, va + size) ) ) {
		va = um_place(um, size, align);
		if ( va == 0 ) {
			return E_NOMEM;
		}
	}
	er = um_add(um, va, va + size);
	if ( er < E_OK ) {
		return er;
	}
	if ( prot != TS_MEM_NONE ) {
		er = pages_make(um, sp, va, va + size, prot);
		if ( er < E_OK ) {
			INT	i = um_find(um, va, va + size);

			pages_drop(um, sp, va, va + size, FALSE);	/* never handed out */
			if ( i >= 0 ) (void)um_cut(um, i, va, va + size);
			return er;
		}
	}
	*p_va = va;
	return E_OK;
}

EXPORT ER knl_umem_unmap( T_UMEM **pp, T_SPACE *sp, UBINT va, UD size, BOOL hold )
{
	T_UMEM	*um = *pp;
	INT	i;
	ER	er;

	size = align_up(size, PAGE_SIZE);
	er = check_range(va, size);
	if ( er < E_OK ) return er;
	if ( um == NULL || ( i = um_find(um, va, va + size) ) < 0 ) {
		return E_PAR;
	}
	er = um_cut(um, i, va, va + size);
	if ( er < E_OK ) return er;
	pages_drop(um, sp, va, va + size, hold);
	return E_OK;
}

EXPORT ER knl_umem_ctl( T_UMEM **pp, T_SPACE *sp, UBINT va, UD size, UINT prot, BOOL hold )
{
	T_UMEM	*um = *pp;
	ER	er;

	if ( ( prot & TS_MEM_EXEC ) != 0 ) {
		return E_NOSPT;
	}
	if ( ( prot & ~(UINT)TS_MEM_RW ) != 0 ) {
		return E_PAR;
	}
	size = align_up(size, PAGE_SIZE);
	er = check_range(va, size);
	if ( er < E_OK ) return er;
	if ( um == NULL || um_find(um, va, va + size) < 0 ) {
		return E_PAR;
	}
	if ( prot == TS_MEM_NONE ) {
		pages_drop(um, sp, va, va + size, hold);
		return E_OK;
	}
	return pages_make(um, sp, va, va + size, prot);
}

EXPORT void knl_umem_flush( T_UMEM *um )
{
	if ( um == NULL ) return;
	while ( um->held != 0 ) {
		UD	pa = um->held;

		um->held = *(UD *)PA2VA(pa);
		knl_free_pages(knl_pa_to_pf(pa), 0);
	}
	um->nheld = 0;
}

EXPORT UD knl_umem_bytes( T_UMEM *um )
{
	return ( um != NULL ) ? um->pages * PAGE_SIZE : 0;
}

EXPORT void knl_umem_free( T_UMEM **pp )
{
	T_UMEM	*um = *pp;

	if ( um == NULL ) return;
	knl_umem_flush(um);
	Kfree(um->r);
	Kfree(um);
	*pp = NULL;
}
