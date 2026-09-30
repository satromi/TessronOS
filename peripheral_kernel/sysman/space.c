/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	space.c
 *	Process spaces (TTBR0), design 6.7.
 *
 *	Three kinds exist:
 *	  - knl_ttbr0_null: an empty L0 table (ASID 0) for kernel tasks, so
 *	    that a stray access below the kernel half faults at once.
 *	  - the system process space (ASID 1): the kernel image's user
 *	    section plus the stacks of the protection level 2 and 3 tasks
 *	    that the kernel itself creates.
 *	  - one space per process (ASID 2 and up), created here and filled
 *	    by the program loader.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "pfalloc.h"
#include "space.h"

/*
 * Taken with interrupts disabled around the ASIDs and the windows handed out: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	space_lock;

EXPORT UD	knl_ttbr0_null;
EXPORT UD	knl_sysprc_ttbr0;
EXPORT UD	knl_sysprc_l0_pa;

EXPORT T_SPACE	knl_sysprc_space;
EXPORT T_SPACE	*knl_new_task_space = NULL;

/*
 * The page a process's call is given where the kernel met an address
 * of the process that was not there, or not to be written
 * (knl_space_sink). One for every space, and never freed with one.
 */
LOCAL UD	sink_pa = 0;

IMPORT UBINT	knl_user_start_va;	/* __user_start (VA in the process space) */
IMPORT UBINT	knl_user_text_end_va;
IMPORT UBINT	knl_user_end_va;
IMPORT UBINT	knl_user_lma;		/* physical address of the section in the image */

/*
 * ASID: one per live space, so that switching between processes never
 * has to empty the TLB. 0 and 1 are taken. The processor tells how many
 * bits it has (ID_AA64MMFR0_EL1.ASIDBits): with 16 there are as many
 * ASIDs as there may be processes, with 8 the processes are as many as
 * the ASIDs.
 */
LOCAL UINT	asid_next = ASID_SYSPRC + 1;
LOCAL UINT	asid_top = 255;			/* the highest the processor has */
LOCAL UW	asid_used[(ASID_MAX + 1 + 31) / 32];

LOCAL UD alloc_table( void )
{
	PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);

	if ( pf == NULL ) return 0;
	pf->flags = PF_PTABLE | PF_HEAD;
	return knl_pf_to_pa(pf);
}

LOCAL INT asid_alloc( void )
{
	UINT	imask;
	UINT	a, n;
	INT	got = -1;

	ISpinLock(&space_lock, &imask);
	for ( n = 0, a = asid_next; n <= asid_top; n++, a++ ) {
		if ( a > asid_top ) a = ASID_SYSPRC + 1;
		if ( a % 32 == 0 && asid_used[a / 32] == ~(UW)0 && a + 31 <= asid_top ) {
			a += 31;		/* a whole word taken */
			n += 31;
			continue;
		}
		if ( (asid_used[a / 32] & (1U << (a % 32))) == 0 ) {
			asid_used[a / 32] |= 1U << (a % 32);
			asid_next = a + 1;
			got = (INT)a;
			break;
		}
	}
	ISpinUnlock(&space_lock, &imask);

	return got;
}

LOCAL void asid_free( UINT a )
{
	UINT	imask;

	ISpinLock(&space_lock, &imask);
	asid_used[a / 32] &= ~(1U << (a % 32));
	ISpinUnlock(&space_lock, &imask);
}

EXPORT void knl_space_init( void )
{
	UD	null_pa = alloc_table();
	UD	text_size, data_size;

	knl_sysprc_l0_pa = alloc_table();
	sink_pa = alloc_table();
	knl_ttbr0_null   = null_pa | ((UD)ASID_NULL << 48);
	knl_sysprc_ttbr0 = knl_sysprc_l0_pa | ((UD)ASID_SYSPRC << 48);

	asid_used[0] = (1U << ASID_NULL) | (1U << ASID_SYSPRC);
	{
		UD	mmfr0;

		Asm("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
		asid_top = ( ( ( mmfr0 >> 4 ) & 0xF ) == 2 ) ? ASID_MAX : 255;
	}

	knl_sysprc_space.l0_pa    = knl_sysprc_l0_pa;
	knl_sysprc_space.ttbr0    = knl_sysprc_ttbr0;
	knl_sysprc_space.asid     = ASID_SYSPRC;
	knl_sysprc_space.stk_next = USER_STACK_TOP;

	/* user section: text pages RO+X (EL0), data pages RW (EL0) */
	text_size = knl_user_text_end_va - knl_user_start_va;
	data_size = knl_user_end_va - knl_user_text_end_va;
	if ( text_size != 0 ) {
		knl_pt_map(knl_sysprc_l0_pa, knl_user_start_va, knl_user_lma, text_size, PTE_PAGE_UTEXT);
	}
	if ( data_size != 0 ) {
		knl_pt_map(knl_sysprc_l0_pa, knl_user_text_end_va, knl_user_lma + text_size, data_size, PTE_PAGE_UDATA);
	}
}

/*
 * A new, empty space
 */
EXPORT ER knl_space_create( T_SPACE *sp )
{
	INT	asid = asid_alloc();

	if ( asid < 0 ) {
		return E_LIMIT;
	}
	sp->l0_pa = alloc_table();
	if ( sp->l0_pa == 0 ) {
		asid_free((UINT)asid);
		return E_NOMEM;
	}
	sp->asid     = (UINT)asid;
	sp->ttbr0    = sp->l0_pa | ((UD)asid << 48);
	sp->stk_next = USER_STACK_TOP;

	return E_OK;
}

/*
 * Free every page the space maps and the tables themselves. The caller
 * must be sure that no processor still has the space installed. What
 * is mapped in the shared window is not the space's: its tables go,
 * its pages stay with whoever shared them ('keep').
 */
#define SHM_L0_FIRST	( (INT)( SHM_WINDOW_BASE >> 39 ) )
#define SHM_L0_END	( (INT)( SHM_WINDOW_END >> 39 ) )

LOCAL void free_level( UD table_pa, INT level, BOOL keep )
{
	UD	*t = (UD *)PA2VA(table_pa);
	INT	i;

	for ( i = 0; i < PT_ENTRIES; i++ ) {
		UD	e = t[i];
		UD	pa;
		BOOL	k = keep || ( level == 0 && i >= SHM_L0_FIRST && i < SHM_L0_END );

		if ( (e & PTE_VALID) == 0 ) continue;
		pa = e & PTE_ADDR_MASK;

		/* bits [1:0] are 0b11 for a table at levels 0..2 and for a
		   page at level 3, so the level tells the two apart */
		if ( level < 3 && (e & 3) == 3 ) {
			free_level(pa, level + 1, k);
		} else if ( !k && pa != sink_pa ) {
			knl_free_pages(knl_pa_to_pf(pa), 0);	/* a mapped page */
		}
		t[i] = 0;
	}
	knl_free_pages(knl_pa_to_pf(table_pa), 0);
}

/*
 * The next free stretch of the shared window. 64TB handed out a piece
 * at a time and never taken back is more than a system ever uses, and
 * an address never used twice is one no stale mapping can alias.
 */
LOCAL UBINT	shm_next = SHM_WINDOW_BASE;

EXPORT UBINT knl_shm_va_alloc( UD size )
{
	UINT	imask;
	UBINT	va = 0;

	size = ( size + PAGE_SIZE - 1 ) & ~(UD)( PAGE_SIZE - 1 );
	ISpinLock(&space_lock, &imask);
	if ( size > 0 && shm_next + size <= SHM_WINDOW_END ) {
		va = shm_next;
		shm_next += size + PAGE_SIZE;	/* a page apart: an overrun faults */
	}
	ISpinUnlock(&space_lock, &imask);

	return va;
}

EXPORT void knl_space_delete( T_SPACE *sp )
{
	if ( sp->l0_pa == 0 ) {
		return;
	}
	free_level(sp->l0_pa, 0, FALSE);
	Asm("dsb ishst" ::: "memory");
	Asm("tlbi aside1is, %0" :: "r"((UD)sp->asid << 48) : "memory");
	Asm("dsb ish; isb" ::: "memory");

	asid_free(sp->asid);
	sp->l0_pa = 0;
	sp->ttbr0 = 0;
}

/*
 * The page of nothing put at va in place of what was there, for the
 * kernel to go on with a call that met va not mapped, or not to be
 * written (knl_prc_kfault; called from the exception handler with
 * interrupts disabled). A page of the space's own that it replaces is
 * freed: nothing else maps it, and the process does not run again. A
 * page of the shared window belongs to whoever shared it.
 */
EXPORT ER knl_space_sink( T_SPACE *sp, UBINT va )
{
	UD	old;

	if ( sink_pa == 0 || sp->l0_pa == 0 ) {
		return E_NOMEM;
	}
	va &= PAGE_MASK;
	old = knl_pt_lookup(sp->l0_pa, va);
	if ( knl_pt_map(sp->l0_pa, va, sink_pa, PAGE_SIZE, PTE_PAGE_UDATA) != E_OK ) {
		return E_NOMEM;
	}
	if ( old != ~(UD)0 && old != sink_pa && !( va >= SHM_WINDOW_BASE && va < SHM_WINDOW_END ) ) {
		knl_free_pages(knl_pa_to_pf(old), 0);
	}
	return E_OK;
}

/*
 * Map zero filled pages into a space (program segments, stacks, heap)
 */
EXPORT ER knl_space_map_zero( T_SPACE *sp, UBINT va, UD size, UD attr )
{
	UD	i, n = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	for ( i = 0; i < n; i++ ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);

		if ( pf == NULL ) return E_NOMEM;
		if ( knl_pt_map(sp->l0_pa, va + (i << PAGE_SHIFT), knl_pf_to_pa(pf),
				PAGE_SIZE, attr) != E_OK ) {
			knl_free_pages(pf, 0);
			return E_NOMEM;
		}
	}

	return E_OK;
}

/*
 * Kernel view of one page of a space, for filling a segment in.
 *	Returns the linear map address of the page holding 'va'.
 */
EXPORT void *knl_space_page( T_SPACE *sp, UBINT va )
{
	UD	pa = knl_pt_lookup(sp->l0_pa, va & PAGE_MASK);

	if ( pa == ~(UD)0 ) {
		return NULL;
	}
	return (void *)PA2VA(pa + (va & ~PAGE_MASK));
}

/*
 * Change the attributes of a range already mapped (W^X: a segment is
 * written as data and then turned into read only text).
 */
EXPORT ER knl_space_protect( T_SPACE *sp, UBINT va, UD size, UD attr )
{
	UD	i, n = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	for ( i = 0; i < n; i++ ) {
		UBINT	p = va + (i << PAGE_SHIFT);
		UD	pa = knl_pt_lookup(sp->l0_pa, p);

		if ( pa == ~(UD)0 ) return E_NOEXS;
		knl_pt_unmap(sp->l0_pa, p, PAGE_SIZE);
		if ( knl_pt_map(sp->l0_pa, p, pa, PAGE_SIZE, attr) != E_OK ) {
			return E_SYS;
		}
	}

	return E_OK;
}

/*
 * User stack in a space: npages plus a guard page below it.
 */
EXPORT void *knl_ustack_alloc_sp( T_SPACE *sp, UD npages, void **p_top )
{
	UBINT	top, base, va;
	UINT	imask;
	UD	i;

	ISpinLock(&space_lock, &imask);
	top = sp->stk_next;
	sp->stk_next -= (npages + 1) << PAGE_SHIFT;
	ISpinUnlock(&space_lock, &imask);
	base = top - (npages << PAGE_SHIFT);

	for ( i = 0, va = base; i < npages; i++, va += PAGE_SIZE ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);
		if ( pf == NULL || knl_pt_map(sp->l0_pa, va, knl_pf_to_pa(pf),
					      PAGE_SIZE, PTE_PAGE_UDATA) != E_OK ) {
			if ( pf != NULL ) knl_free_pages(pf, 0);
			knl_ustack_free_sp(sp, (void *)base, i);
			return NULL;
		}
	}
	*p_top = (void *)top;

	return (void *)base;
}

EXPORT void knl_ustack_free_sp( T_SPACE *sp, void *base, UD npages )
{
	knl_ustack_free_l0(sp->l0_pa, base, npages);
}

/*
 * The user stack of a task, taken out of the space whose L0 table is
 * at l0_pa and given back. The page of nothing (knl_space_sink) that
 * may stand in for one of its pages is not the stack's.
 */
EXPORT void knl_ustack_free_l0( UD l0_pa, void *base, UD npages )
{
	UBINT	va = (UBINT)base;
	UD	i;

	for ( i = 0; i < npages; i++, va += PAGE_SIZE ) {
		UD	pa = knl_pt_lookup(l0_pa, va);
		if ( pa != ~(UD)0 ) {
			knl_pt_unmap(l0_pa, va, PAGE_SIZE);
			if ( pa != sink_pa ) {
				knl_free_pages(knl_pa_to_pf(pa), 0);
			}
		}
	}
	/* the virtual range is not reused (bump allocator) */
}

EXPORT void *knl_ustack_alloc( UD npages, void **p_top )
{
	T_SPACE	*sp = ( knl_new_task_space != NULL ) ? knl_new_task_space : &knl_sysprc_space;

	return knl_ustack_alloc_sp(sp, npages, p_top);
}

EXPORT void knl_ustack_free( void *base, UD npages )
{
	knl_ustack_free_sp(&knl_sysprc_space, base, npages);
}
