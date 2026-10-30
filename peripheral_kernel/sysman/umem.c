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
 *	the rest; the lists are freed with the process (knl_umem_free).
 *
 *	Pages to be run (TS_MEM_EXEC) are read-only while they may be run:
 *	no page is ever writable and executable at once (W^X). Before a page
 *	becomes executable, what was written into it is cleaned out of the
 *	data cache and the instruction cache lines of it are invalidated
 *	(knl_icache_sync), and the other processors running the process
 *	take an exception (knl_sync_cores), so that every one of them fetches
 *	the new instructions.
 *
 *	A stretch asked for writable and executable at once is kept in a
 *	second list: its pages are written and run by turns. Each is made
 *	the first time it is touched, writable, or executable when it is
 *	first run; a write to it while it is executable, or running it while
 *	it is writable, faults, and the fault handler (knl_umem_fault) turns
 *	it the other way and has the access made again. This is what a
 *	compiler to machine code that writes into the pages it runs needs,
 *	while each page stays one or the other.
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
 *	space and the lists while they are used here.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include <ts/umem.h>
#include "pfalloc.h"
#include "space.h"
#include "umem.h"

#define UMEM_FLOOR	4096		/* pages always left to the kernel (16MB) */
#define UMEM_FIRST	16		/* intervals a list starts with */

#define PROT_ALL	( TS_MEM_RW | TS_MEM_EXEC )
#define IS_JIT(prot)	( ( (prot) & ( TS_MEM_WRITE | TS_MEM_EXEC ) ) == ( TS_MEM_WRITE | TS_MEM_EXEC ) )

/* What a process may do with a page, by its entry (sys/sysdef.h) */
#define ENT_EL0_R(e)	PTE_USER_R(e)
#define ENT_EL0_W(e)	PTE_USER_W(e)
#define ENT_EL0_X(e)	PTE_USER_X(e)

typedef struct {
	UBINT	s, e;			/* [s, e), page aligned */
} UMRNG;

typedef struct {
	UMRNG	*r;
	INT	n, cap;
} UMLIST;

struct umem {
	UMLIST	res;			/* the reservations */
	UMLIST	jit;			/* stretches written and run by turns */
	UD	pages;			/* pages made and still mapped */
	UD	held;			/* first page held back (its PA), 0 for none */
	UD	nheld;
	UD	to_exec, to_write;	/* turns of the pages of 'jit' */
};

LOCAL BOOL pow2( UD a )
{
	return ( a != 0 && ( a & ( a - 1 ) ) == 0 );
}

LOCAL UBINT align_up( UBINT v, UD a )
{
	return ( v + a - 1 ) & ~(UBINT)( a - 1 );
}

LOCAL ER ls_init( UMLIST *l )
{
	l->r = (UMRNG *)Kmalloc(sizeof(UMRNG) * UMEM_FIRST);
	if ( l->r == NULL ) return E_NOMEM;
	l->n = 0;
	l->cap = UMEM_FIRST;
	return E_OK;
}

LOCAL T_UMEM *um_get( T_UMEM **pp )
{
	T_UMEM	*um = *pp;

	if ( um == NULL ) {
		um = (T_UMEM *)Kcalloc(1, sizeof(T_UMEM));
		if ( um == NULL ) return NULL;
		if ( ls_init(&um->res) < E_OK ) {
			Kfree(um);
			return NULL;
		}
		if ( ls_init(&um->jit) < E_OK ) {
			Kfree(um->res.r);
			Kfree(um);
			return NULL;
		}
		*pp = um;
	}
	return um;
}

/* Room for one more interval */
LOCAL ER ls_room( UMLIST *l )
{
	UMRNG	*nr;

	if ( l->n < l->cap ) {
		return E_OK;
	}
	nr = (UMRNG *)Kmalloc(sizeof(UMRNG) * (UINT)( l->cap * 2 ));
	if ( nr == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(nr, l->r, (SZ)( sizeof(UMRNG) * (UINT)l->n ));
	Kfree(l->r);
	l->r = nr;
	l->cap *= 2;
	return E_OK;
}

/* The interval holding all of [s, e), or -1 */
LOCAL INT ls_find( UMLIST *l, UBINT s, UBINT e )
{
	INT	lo = 0, hi = l->n - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;

		if ( l->r[mid].e <= s ) {
			lo = mid + 1;
		} else if ( l->r[mid].s > s ) {
			hi = mid - 1;
		} else {
			return ( e <= l->r[mid].e ) ? mid : -1;
		}
	}
	return -1;
}

/* Whether [s, e) meets no interval */
LOCAL BOOL ls_free_at( UMLIST *l, UBINT s, UBINT e )
{
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( l->r[i].s >= e ) break;
		if ( l->r[i].e > s ) return FALSE;
	}
	return TRUE;
}

/* The lowest free stretch of 'size' bytes on an 'align' boundary, 0 for none */
LOCAL UBINT ls_place( UMLIST *l, UD size, UD align )
{
	UBINT	cand = align_up(TS_MEM_BASE, align);
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( cand + size <= l->r[i].s ) {
			return cand;
		}
		if ( l->r[i].e > cand ) {
			cand = align_up(l->r[i].e, align);
		}
	}
	return ( cand + size <= TS_MEM_END && cand + size > cand ) ? cand : 0;
}

/* Add [s, e), which meets no interval; one that it touches takes it in */
LOCAL ER ls_add( UMLIST *l, UBINT s, UBINT e )
{
	INT	i, j;

	for ( i = 0; i < l->n && l->r[i].s < s; i++ ) ;
	if ( i > 0 && l->r[i - 1].e == s ) {
		l->r[i - 1].e = e;
		if ( i < l->n && l->r[i].s == e ) {
			l->r[i - 1].e = l->r[i].e;
			for ( j = i; j < l->n - 1; j++ ) l->r[j] = l->r[j + 1];
			l->n--;
		}
		return E_OK;
	}
	if ( i < l->n && l->r[i].s == e ) {
		l->r[i].s = s;
		return E_OK;
	}
	if ( ls_room(l) < E_OK ) {
		return E_NOMEM;
	}
	for ( j = l->n; j > i; j-- ) l->r[j] = l->r[j - 1];
	l->r[i].s = s;
	l->r[i].e = e;
	l->n++;
	return E_OK;
}

/* Take [s, e) out of interval i, which holds it */
LOCAL ER ls_cut( UMLIST *l, INT i, UBINT s, UBINT e )
{
	UMRNG	*r = &l->r[i];
	INT	j;

	if ( s == r->s && e == r->e ) {
		for ( j = i; j < l->n - 1; j++ ) l->r[j] = l->r[j + 1];
		l->n--;
	} else if ( s == r->s ) {
		r->s = e;
	} else if ( e == r->e ) {
		r->e = s;
	} else {
		if ( ls_room(l) < E_OK ) {
			return E_NOMEM;
		}
		r = &l->r[i];
		for ( j = l->n; j > i + 1; j-- ) l->r[j] = l->r[j - 1];
		l->r[i + 1].s = e;
		l->r[i + 1].e = r->e;
		r->e = s;
		l->n++;
	}
	return E_OK;
}

/* Take [s, e) out of whatever intervals it meets */
LOCAL ER ls_sub( UMLIST *l, UBINT s, UBINT e )
{
	INT	i = 0;

	while ( i < l->n ) {
		UMRNG	*r = &l->r[i];
		UBINT	cs, ce;
		INT	n0 = l->n;
		ER	er;

		if ( r->s >= e ) break;
		if ( r->e <= s ) {
			i++;
			continue;
		}
		cs = ( r->s > s ) ? r->s : s;
		ce = ( r->e < e ) ? r->e : e;
		er = ls_cut(l, i, cs, ce);
		if ( er < E_OK ) return er;
		if ( l->n >= n0 && l->r[i].e <= cs ) i++;	/* the part left before the cut */
	}
	return E_OK;
}

/* Whether 'va' lies in an interval */
LOCAL BOOL ls_has( UMLIST *l, UBINT va )
{
	return ( ls_find(l, va, va + 1) >= 0 );
}

/*
 * The entry bits of a protection: writable pages are not executable,
 * executable ones are read-only; a stretch that is both is made
 * writable first.
 */
LOCAL UD prot_attr( UINT prot )
{
	if ( ( prot & TS_MEM_WRITE ) != 0 ) return PTE_PAGE_UDATA;
	if ( ( prot & TS_MEM_EXEC ) != 0 ) return PTE_PAGE_UTEXT;
	return PTE_PAGE_URO;
}

/* Give back or hold back one page that is no longer mapped */
LOCAL void page_gone( T_UMEM *um, UD pa, BOOL hold )
{
	if ( hold ) {
		*(UD *)PA2VA(pa) = um->held;
		um->held = pa;
		__atomic_store_n(&um->nheld, um->nheld + 1, __ATOMIC_SEQ_CST);
	} else {
		knl_free_pages(knl_pa_to_pf(pa), 0);
	}
}

/*
 * Pages of [s, e) made and given 'prot'; E_NOMEM when memory runs out on
 * the way. A page that becomes executable has its instructions made
 * fetchable before its entry allows them to be fetched; the other
 * processors running the process are synchronized once at the end.
 */
LOCAL ER pages_make( T_UMEM *um, T_SPACE *sp, UBINT s, UBINT e, UINT prot )
{
	UD	attr = prot_attr(prot), need = 0;
	BOOL	exec = ENT_EL0_X(attr), made_x = FALSE;
	UBINT	va;
	ER	er = E_OK;

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
				er = E_NOMEM;
				break;
			}
			if ( exec ) {
				knl_icache_sync((UBINT)PA2VA(knl_pf_to_pa(pf)), PAGE_SIZE);
				made_x = TRUE;
			}
			if ( knl_pt_map(sp->l0_pa, va, knl_pf_to_pa(pf), PAGE_SIZE, attr) != E_OK ) {
				knl_free_pages(pf, 0);
				er = E_NOMEM;
				break;
			}
			um->pages++;
		} else if ( ( ent & ~PTE_ADDR_MASK ) != attr ) {
			if ( exec && !ENT_EL0_X(ent) ) {
				knl_icache_sync((UBINT)PA2VA(ent & PTE_ADDR_MASK), PAGE_SIZE);
				made_x = TRUE;
			}
			if ( knl_pt_map(sp->l0_pa, va, ent & PTE_ADDR_MASK, PAGE_SIZE, attr) != E_OK ) {
				er = E_NOMEM;
				break;
			}
		}
	}
	if ( made_x ) {
		knl_sync_cores(sp->ttbr0);
	}
	return er;
}

/*
 * The pages of [s, e) that are there given to be written and run by
 * turns: the writable and the executable stay as they are, a read-only
 * one becomes writable. The others are made when first touched.
 */
LOCAL ER pages_jit( T_SPACE *sp, UBINT s, UBINT e )
{
	UBINT	va;

	for ( va = knl_pt_next(sp->l0_pa, s, e); va < e;
	      va = knl_pt_next(sp->l0_pa, va + PAGE_SIZE, e) ) {
		UD	ent = knl_pt_entry(sp->l0_pa, va);

		if ( ent == 0 || ENT_EL0_W(ent) || ENT_EL0_X(ent) ) continue;
		if ( knl_pt_map(sp->l0_pa, va, ent & PTE_ADDR_MASK, PAGE_SIZE, PTE_PAGE_UDATA) != E_OK ) {
			return E_NOMEM;
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

	if ( ( prot & ~(UINT)PROT_ALL ) != 0 ) {
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
	     && ls_free_at(&um->res, va, va + size) ) ) {
		va = ls_place(&um->res, size, align);
		if ( va == 0 ) {
			return E_NOMEM;
		}
	}
	er = ls_add(&um->res, va, va + size);
	if ( er < E_OK ) {
		return er;
	}
	if ( IS_JIT(prot) ) {
		er = ls_add(&um->jit, va, va + size);
	} else if ( prot != TS_MEM_NONE ) {
		er = pages_make(um, sp, va, va + size, prot);
		if ( er < E_OK ) {
			pages_drop(um, sp, va, va + size, FALSE);	/* never handed out */
		}
	}
	if ( er < E_OK ) {
		INT	i = ls_find(&um->res, va, va + size);

		if ( i >= 0 ) (void)ls_cut(&um->res, i, va, va + size);
		return er;
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
	if ( um == NULL || ( i = ls_find(&um->res, va, va + size) ) < 0 ) {
		return E_PAR;
	}
	er = ls_sub(&um->jit, va, va + size);
	if ( er < E_OK ) return er;
	er = ls_cut(&um->res, i, va, va + size);
	if ( er < E_OK ) return er;
	pages_drop(um, sp, va, va + size, hold);
	return E_OK;
}

/*
 * What the pages of [s, e) hold given up, what they allow kept: a page
 * of a stretch run by turns is taken away (made again when touched),
 * any other is zero filled where it is; one that is executable has its
 * zeros made fetchable.
 */
LOCAL void pages_discard( T_UMEM *um, T_SPACE *sp, UBINT s, UBINT e, BOOL hold )
{
	UBINT	va;
	BOOL	made_x = FALSE;

	for ( va = knl_pt_next(sp->l0_pa, s, e); va < e;
	      va = knl_pt_next(sp->l0_pa, va + PAGE_SIZE, e) ) {
		UD	ent = knl_pt_entry(sp->l0_pa, va);
		UBINT	kva;

		if ( ent == 0 ) continue;
		if ( ls_has(&um->jit, va) ) {
			pages_drop(um, sp, va, va + PAGE_SIZE, hold);
			continue;
		}
		kva = (UBINT)PA2VA(ent & PTE_ADDR_MASK);
		knl_memset((void *)kva, 0, PAGE_SIZE);
		if ( ENT_EL0_X(ent) ) {
			knl_icache_sync(kva, PAGE_SIZE);
			made_x = TRUE;
		}
	}
	if ( made_x ) {
		knl_sync_cores(sp->ttbr0);
	}
}

EXPORT ER knl_umem_ctl( T_UMEM **pp, T_SPACE *sp, UBINT va, UD size, UINT prot, BOOL hold )
{
	T_UMEM	*um = *pp;
	ER	er;

	if ( ( prot & ~(UINT)PROT_ALL ) != 0 && prot != TS_MEM_DISCARD ) {
		return E_PAR;
	}
	size = align_up(size, PAGE_SIZE);
	er = check_range(va, size);
	if ( er < E_OK ) return er;
	if ( um == NULL || ls_find(&um->res, va, va + size) < 0 ) {
		return E_PAR;
	}
	if ( prot == TS_MEM_DISCARD ) {
		pages_discard(um, sp, va, va + size, hold);
		return E_OK;
	}
	if ( IS_JIT(prot) ) {
		er = ls_sub(&um->jit, va, va + size);
		if ( er == E_OK ) er = ls_add(&um->jit, va, va + size);
		if ( er < E_OK ) return er;
		return pages_jit(sp, va, va + size);
	}
	er = ls_sub(&um->jit, va, va + size);
	if ( er < E_OK ) return er;
	if ( prot == TS_MEM_NONE ) {
		pages_drop(um, sp, va, va + size, hold);
		return E_OK;
	}
	return pages_make(um, sp, va, va + size, prot);
}

/*
 * A fault of the process at 'va' in its space: 'acc' is the access
 * (UM_ACC_*). In a stretch written and run by turns, a page that is not
 * there is made for the access, writable unless it is run; a write to
 * an executable page makes it writable, and running a writable one
 * makes it executable, its instructions made fetchable first. TRUE when
 * the access may be made again, FALSE when the fault is the process's
 * own.
 */
EXPORT BOOL knl_umem_fault( T_UMEM *um, T_SPACE *sp, UBINT va, UINT acc )
{
	UD	ent, pa;
	PFRAME	*pf;

	va &= ~(UBINT)( PAGE_SIZE - 1 );
	if ( um == NULL || !ls_has(&um->jit, va) ) {
		return FALSE;
	}
	ent = knl_pt_entry(sp->l0_pa, va);
	if ( ent == 0 ) {
		if ( UMEM_FLOOR >= knl_pf_free_count(-1) ) {
			return FALSE;
		}
		pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);
		if ( pf == NULL ) {
			return FALSE;
		}
		pa = knl_pf_to_pa(pf);
		if ( acc == UM_ACC_EXEC ) {
			knl_icache_sync((UBINT)PA2VA(pa), PAGE_SIZE);
		}
		if ( knl_pt_map(sp->l0_pa, va, pa, PAGE_SIZE,
				( acc == UM_ACC_EXEC ) ? PTE_PAGE_UTEXT : PTE_PAGE_UDATA) != E_OK ) {
			knl_free_pages(pf, 0);
			return FALSE;
		}
		um->pages++;
		return TRUE;
	}
	pa = ent & PTE_ADDR_MASK;
	if ( acc == UM_ACC_EXEC && !ENT_EL0_X(ent) ) {
		knl_icache_sync((UBINT)PA2VA(pa), PAGE_SIZE);
		if ( knl_pt_map(sp->l0_pa, va, pa, PAGE_SIZE, PTE_PAGE_UTEXT) != E_OK ) {
			return FALSE;
		}
		knl_sync_cores(sp->ttbr0);
		um->to_exec++;
	} else if ( acc == UM_ACC_WRITE && !ENT_EL0_W(ent) ) {
		if ( knl_pt_map(sp->l0_pa, va, pa, PAGE_SIZE, PTE_PAGE_UDATA) != E_OK ) {
			return FALSE;
		}
		um->to_write++;
	}
	return TRUE;
}

/*
 * What the pages of the reservations allow: out[0] pages there, [1]
 * writable, [2] executable, [3] both (never, by W^X), [4] and [5] the
 * turns to executable and to writable of the stretches run by turns.
 */
EXPORT void knl_umem_wx( T_UMEM *um, T_SPACE *sp, UD out[6] )
{
	INT	i;
	UBINT	va;

	for ( i = 0; i < 6; i++ ) out[i] = 0;
	if ( um == NULL ) return;
	for ( i = 0; i < um->res.n; i++ ) {
		UBINT	e = um->res.r[i].e;

		for ( va = knl_pt_next(sp->l0_pa, um->res.r[i].s, e); va < e;
		      va = knl_pt_next(sp->l0_pa, va + PAGE_SIZE, e) ) {
			UD	ent = knl_pt_entry(sp->l0_pa, va);

			if ( ent == 0 ) continue;
			out[0]++;
			if ( ENT_EL0_W(ent) ) out[1]++;
			if ( ENT_EL0_X(ent) ) out[2]++;
			if ( ENT_EL0_W(ent) && ENT_EL0_X(ent) ) out[3]++;
		}
	}
	out[4] = um->to_exec;
	out[5] = um->to_write;
}

/*
 * Whether pages are held back: read without the lock by the last call
 * of the process to come out (knl_prc_call_leave), which then gives
 * them back under it
 */
EXPORT BOOL knl_umem_held( T_UMEM *um )
{
	return (BOOL)( um != NULL && __atomic_load_n(&um->nheld, __ATOMIC_SEQ_CST) != 0 );
}

EXPORT void knl_umem_flush( T_UMEM *um )
{
	if ( um == NULL ) return;
	while ( um->held != 0 ) {
		UD	pa = um->held;

		um->held = *(UD *)PA2VA(pa);
		knl_free_pages(knl_pa_to_pf(pa), 0);
	}
	__atomic_store_n(&um->nheld, 0, __ATOMIC_SEQ_CST);
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
	Kfree(um->res.r);
	Kfree(um->jit.r);
	Kfree(um);
	*pp = NULL;
}
