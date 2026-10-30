/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mmu.c (x86-64)
 *	Page table management (4 level paging, 4KB pages).
 *	  - the kernel's half as the UEFI loader built it (bootinfo.h): every
 *	    PML4 entry of it has its table from the start, so that a process
 *	    space shares the kernel's half by a copy of those 256 entries
 *	    and every later mapping there is seen by every space
 *	  - the device window: the first 4GB as the loader mapped them, a
 *	    gigabyte above that mapped when it is first touched
 *	  - generic 4KB page mapping for any PML4 (kernel vmap, process
 *	    spaces): tables are allocated from the page frame allocator
 *	  - MapMemory / UnmapMemory / ConvPhysicalAddress
 *
 *	The translations other processors may hold are dropped with them
 *	(knl_tlb_shootdown): x86 has no instruction for that, so each is
 *	asked by an NMI, which it takes even with its interrupts masked --
 *	waiting for it to take an ordinary interrupt while it waits for a
 *	lock this one holds would never end.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include <tm/tmonitor.h>
#include "../../../sysdepend.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include <tk/smem.h>

EXPORT UD	knl_pt_l0_k_pa;			/* the kernel's PML4 */
EXPORT BOOL	knl_pcid_on = FALSE;
LOCAL BOOL	pcid_asked = FALSE;

#define PT_IDX(va, shift)	(((UD)(va) >> (shift)) & (PT_ENTRIES - 1))
#define PTE_IS_TABLE(e)		( ( (e) & PTE_P ) != 0 && ( (e) & PTE_PS ) == 0 )
#define PTE_PA(e)		((e) & PTE_ADDR_MASK)
#define USER_HALF_END		U64(0x0000800000000000)

Inline void invlpg( UBINT va )
{
	Asm("invlpg (%0)" :: "r"(va) : "memory");
}

Inline void flush_local_all( void )
{
	UD	cr4;

	/* toggling PGE drops the global translations as well */
	Asm("mov %%cr4, %0" : "=r"(cr4));
	Asm("mov %0, %%cr4" :: "r"(cr4 & ~(UD)CR4_PGE) : "memory");
	Asm("mov %0, %%cr4" :: "r"(cr4) : "memory");
	knl_pcpu()->tlb_flushes++;		/* the spaces it noted may be forgotten (knl_tlb_note) */
}

/* ------------------------------------------------------------------------ */
/*
 * Other processors' translations
 *	A request is a word of the processor's T_PCPU: the address with bit
 *	0 for "everything", bit 1 for "off the space shoot_drop as well".
 *	The processor that asks writes it, sends the NMI and waits until the
 *	asked one has counted its acknowledge on; one request is out at a
 *	time (shoot_lock). A processor that does not answer within a bound
 *	(one stopped) is not waited for longer.
 */
LOCAL T_SPLOCK	shoot_lock;
LOCAL volatile UD shoot_drop;		/* the space of a SHOOT_DROP request */

#define SHOOT_ALL	1
#define SHOOT_DROP	2
#define SHOOT_SPINS	50000000

/*
 * Off a space that is to be freed, onto the empty one. Its PML4 holds
 * the kernel's half too, so a processor left on it after it is freed
 * would take its next interrupt through a page that is no longer a
 * table. Only a processor that ran none of its tasks since (idle) can
 * be on it: the dispatcher installs a task's own space.
 */
LOCAL void drop_space( T_PCPU *p, UD ttbr0 )
{
	if ( p->cur_ttbr0 == ttbr0 ) {
		p->cur_ttbr0 = knl_ttbr0_null;
		Asm("mov %0, %%cr3" :: "r"(knl_mmu_root_cr3(knl_ttbr0_null)) : "memory");
	}
}

/* NMI: what this processor was asked to drop (exc_hdl.c) */
EXPORT BOOL knl_tlb_nmi( void )
{
	T_PCPU	*p = knl_pcpu();
	UD	r = p->tlb_req;

	if ( r == 0 ) {
		return FALSE;
	}
	if ( ( r & SHOOT_DROP ) != 0 ) {
		drop_space(p, shoot_drop);
	}
	if ( ( r & SHOOT_ALL ) != 0 ) {
		flush_local_all();
	} else {
		invlpg((UBINT)r);
	}
	p->tlb_req = 0;
	__atomic_add_fetch(&p->tlb_ack, 1, __ATOMIC_RELEASE);
	return TRUE;
}

/*
 * The spaces each processor may hold translations of (design 4.5): the
 * dispatcher notes the space of the task it is about to run, before the
 * processor loads it, and a change in a space's user half is asked of
 * the processors that noted it. Without PCIDs, loading a space drops
 * the others, so the space loaded is the only one. With them, the
 * spaces run since the processor last dropped all its translations;
 * when it has since, the list starts again from the one loaded. More
 * than the list holds, and the processor is asked whatever the space.
 * Called with interrupts masked; the list grows before the space is
 * loaded, and the one who asks reads it after the change is made, so
 * a processor left out loads the changed tables.
 */
EXPORT void knl_tlb_note( UD ttbr0 )
{
	T_PCPU	*p = knl_pcpu();
	UD	l0 = ttbr0 & MMU_ROOT_PA, cur = p->cur_ttbr0;
	UW	n, f = p->tlb_flushes, i;

	if ( ttbr0 == 0 || ttbr0 == knl_ttbr0_null ) {
		return;
	}
	n = p->tlb_spn;
	if ( !knl_pcid_on || f != p->tlb_flushes_seen ) {
		p->tlb_flushes_seen = f;
		p->tlb_sp[0] = l0;
		n = 1;
		if ( knl_pcid_on && cur != 0 && cur != knl_ttbr0_null && ( cur & MMU_ROOT_PA ) != l0 ) {
			p->tlb_sp[1] = cur & MMU_ROOT_PA;
			n = 2;
		}
	} else if ( n <= TLB_SP_N ) {
		for ( i = 0; i < n && p->tlb_sp[i] != l0; i++ ) ;
		if ( i == n ) {
			if ( n < TLB_SP_N ) {
				p->tlb_sp[n] = l0;
			}
			n++;			/* TLB_SP_N + 1: any space */
		}
	}
	p->tlb_spn = n;
	Asm("mfence" ::: "memory");
}

/* Whether processor p may hold translations of the space at l0 (0: of the kernel's half, all may) */
LOCAL BOOL may_hold( T_PCPU *p, UD l0 )
{
	UW	n, i;

	if ( l0 == 0 || ( p->cur_ttbr0 & MMU_ROOT_PA ) == l0 ) {
		return TRUE;
	}
	n = p->tlb_spn;
	if ( n > TLB_SP_N ) {
		return TRUE;
	}
	for ( i = 0; i < n; i++ ) {
		if ( p->tlb_sp[i] == l0 ) return TRUE;
	}
	return FALSE;
}

/* The others asked for r (SHOOT_*, or an address), waited for as said above */
LOCAL void shoot_others_in( UD r, UD drop, UD l0 )
{
	T_PCPU		*me = knl_pcpu();
	T_PRCSET	mask;
	UINT		imask;
	INT		i, n;

	if ( knl_num_prc <= 1 ) {
		return;
	}
	prcset_zero(&mask);
	ISpinLock(&shoot_lock, &imask);
	shoot_drop = drop;
	Asm("mfence" ::: "memory");		/* the change made before the lists are read */
	for ( i = 0; i < knl_num_prc; i++ ) {
		T_PCPU	*p = knl_pcpu_of(i);

		if ( p == me || p->state != PC_ONLINE || !may_hold(p, l0) ) continue;
		p->tlb_seen = __atomic_load_n(&p->tlb_ack, __ATOMIC_ACQUIRE);
		p->tlb_req = r;
		prcset_add(&mask, i);
		knl_apic_send(p->apic_id, 0, APIC_DM_NMI);
	}
	for ( n = 0; n < SHOOT_SPINS && !prcset_empty(&mask); n++ ) {
		for ( i = prcset_next(&mask, 0, knl_num_prc); i >= 0; i = prcset_next(&mask, i + 1, knl_num_prc) ) {
			T_PCPU	*p = knl_pcpu_of(i);

			if ( __atomic_load_n(&p->tlb_ack, __ATOMIC_ACQUIRE) != p->tlb_seen ) {
				prcset_del(&mask, i);
			}
		}
		CpuRelax();
	}
	ISpinUnlock(&shoot_lock, &imask);
}

LOCAL void shoot_others( UD r, UD drop )
{
	shoot_others_in(r, drop, 0);
}

EXPORT void knl_tlb_shootdown( UINT pcid, UBINT va, BOOL all )
{
	(void)pcid;
	/*
	 * With PCIDs, INVLPG drops a page of the space installed only: the
	 * same page of a space another task left in the TLB stays. A page
	 * dropped is then everything dropped (CR4.PGE toggled), here and on
	 * every other processor.
	 */
	UINT	imask;

	if ( knl_pcid_on ) {
		all = TRUE;
	}
	/*
	 * Here and the others with interrupts disabled throughout: a task
	 * moved to another processor after its own was dropped would leave
	 * that one out as "this one", and drop it nowhere
	 */
	DI(imask);
	if ( all ) {
		flush_local_all();
	} else {
		invlpg(va);
	}
	shoot_others(all ? SHOOT_ALL : ( va & ~(UBINT)( PAGE_SIZE - 1 ) ), 0);
	EI(imask);
}

/*
 * A page of the space at l0_pa: of the kernel's half, every processor
 * drops it; of a process's half, those that may hold it (knl_tlb_note)
 */
LOCAL void tlb_flush_in( UD l0_pa, UBINT va, BOOL all )
{
	UINT	imask;

	if ( l0_pa == 0 || l0_pa == knl_pt_l0_k_pa || ( !all && va >= USER_HALF_END ) ) {
		knl_tlb_shootdown(0, va, all);
		return;
	}
	if ( knl_pcid_on ) {
		all = TRUE;
	}
	DI(imask);
	if ( all ) {
		flush_local_all();
	} else {
		invlpg(va);
	}
	shoot_others_in(all ? SHOOT_ALL : ( va & ~(UBINT)( PAGE_SIZE - 1 ) ), 0, l0_pa);
	EI(imask);
}

EXPORT void knl_mmu_space_drop( UD ttbr0 )
{
	UINT	imask;

	if ( ttbr0 == 0 || ttbr0 == knl_ttbr0_null ) {
		return;
	}
	DI(imask);				/* this processor and the others, without moving between */
	drop_space(knl_pcpu(), ttbr0);
	shoot_others(SHOOT_ALL | SHOOT_DROP, ttbr0);
	EI(imask);
}

EXPORT void knl_tlb_flush_va( UBINT va )
{
	knl_tlb_shootdown(0, va, FALSE);
}

/* x86 keeps the instruction fetch coherent with the stores (sysdepend.h) */
EXPORT void knl_icache_sync( UBINT va, UD size )
{
	(void)va;
	(void)size;
	Asm("" ::: "memory");
}

/* ------------------------------------------------------------------------ */
/*
 * Each processor: no-execute, the page attribute table of sysdef.h,
 * global pages, write protection in the kernel's mode as well
 */
EXPORT void knl_mmu_cpu_init( void )
{
	UD	cr0, cr4;

	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE | EFER_SCE);
	wrmsr(MSR_PAT, PAT_VALUE);
	Asm("mov %%cr0, %0" : "=r"(cr0));
	Asm("mov %0, %%cr0" :: "r"(cr0 | CR0_WP));
	Asm("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= CR4_PGE;
	{
		UW	a, b, c, d;

		/* a program sets its own TLS pointer (WRFSBASE, lib/libpthread) */
		cpuid(7, 0, &a, &b, &c, &d);
		if ( ( b & 1 ) != 0 ) {
			cr4 |= CR4_FSGSBASE;
		}
		/*
		 * Process-context identifiers, where the processor has them:
		 * asked once, on the first processor before any space is made
		 * (its root carries its PCID, sysdepend.h), and the same for
		 * every processor after it. The PCID of a space is its ASID.
		 */
		if ( !pcid_asked ) {
			cpuid(1, 0, &a, &b, &c, &d);
			knl_pcid_on = (BOOL)( ( c & ( 1U << 17 ) ) != 0 );
			pcid_asked = TRUE;
		}
		if ( knl_pcid_on ) {
			cr4 |= CR4_PCIDE;	/* CR3's bits 11:0 are 0 here: the kernel's root */
		}
	}
	Asm("mov %0, %%cr4" :: "r"(cr4));
}

/*
 * The loader's tables are the kernel's: the PML4 of the kernel's half,
 * whose low half the loader filled with the identity map it jumped to
 * the kernel through. That stays until the other processors have come
 * up through it (smp.c); the kernel's tasks run on knl_ttbr0_null,
 * which has none of it.
 */
EXPORT void knl_mmu_init( void )
{
	knl_pt_l0_k_pa = knl_bootinfo->pml4_pa;
	knl_mmu_cpu_init();
	tm_printf((UB *)"TessronOS x64: process-context identifiers %s\n", knl_pcid_on ? "on" : "off (the processor has none)");
}

/* A new PML4 of a process space or of the kernel's tasks: the kernel's half copied in */
EXPORT void knl_mmu_space_init_root( UD l0_pa )
{
	UD	*k = (UD *)PA2VA(knl_pt_l0_k_pa), *t = (UD *)PA2VA(l0_pa);
	INT	i;

	for ( i = PT_USER_ENTRIES; i < PT_ENTRIES; i++ ) {
		t[i] = k[i];
	}
}

/*
 * Allocate a zeroed page table page
 */
LOCAL UD *pt_alloc( void )
{
	PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);

	if ( pf == NULL ) return NULL;
	pf->flags = PF_PTABLE | PF_HEAD;
	return (UD *)knl_pf_to_va(pf);
}

/*
 * Taken with interrupts disabled while the tables are changed: two
 * processors making the same missing table would each put in their own,
 * and the mappings made in the one that lost would be gone.
 */
LOCAL T_SPLOCK	pt_lock;

/*
 * Return the page table (level 3) for 'va' under the PML4 at l0_pa,
 * creating the intermediate tables when 'create' is set. NULL if absent
 * or if a large page is in the way. A missing table of the kernel's
 * half is never made here: every one is there from the start.
 */
LOCAL UD *pt_l3_of( UD l0_pa, UBINT va, BOOL create )
{
	UD	*tbl = (UD *)PA2VA(l0_pa);
	UINT	shift;

	for ( shift = L0_SHIFT; shift > L3_SHIFT; shift -= 9 ) {
		UD	*ent = &tbl[PT_IDX(va, shift)];
		UD	e = *ent;

		if ( (e & PTE_P) == 0 ) {
			UD	*nt;
			if ( !create || ( shift == L0_SHIFT && va >= USER_HALF_END ) ) return NULL;
			nt = pt_alloc();
			if ( nt == NULL ) return NULL;
			*ent = VA2PA(nt) | PTE_TABLE_DESC;
			tbl = nt;
		} else if ( PTE_IS_TABLE(e) ) {
			tbl = (UD *)PA2VA(PTE_PA(e));
		} else {
			return NULL;		/* large page: cannot hold 4KB pages */
		}
	}
	return tbl;
}

/*
 * Map [va, va + size) to [pa, pa + size) with 4KB pages and entry bits
 * 'attr' (PTE_PAGE_*). An entry that was there is dropped from every
 * processor's TLB.
 */
EXPORT ER knl_pt_map( UD l0_pa, UBINT va, UD pa, UD size, UD attr )
{
	UBINT	end = va + size;
	UINT	imask;
	ER	er = E_OK;

	if ( (va | pa | size) & (PAGE_SIZE - 1) ) return E_PAR;

	ISpinLock(&pt_lock, &imask);
	for ( ; va < end; va += PAGE_SIZE, pa += PAGE_SIZE ) {
		UD	*l3 = pt_l3_of(l0_pa, va, TRUE);
		UD	*ent;

		if ( l3 == NULL ) {
			er = E_NOMEM;
			break;
		}
		ent = &l3[PT_IDX(va, L3_SHIFT)];
		if ( (*ent & PTE_P) != 0 ) {
			*ent = 0;
			tlb_flush_in(l0_pa, va, FALSE);
		}
		*ent = (pa & PTE_ADDR_MASK) | attr;
	}
	ISpinUnlock(&pt_lock, &imask);
	return er;
}

/*
 * Remove the 4KB mappings of [va, va + size). Tables are kept.
 */
EXPORT ER knl_pt_unmap( UD l0_pa, UBINT va, UD size )
{
	UBINT	end = va + size;
	UINT	imask;

	if ( (va | size) & (PAGE_SIZE - 1) ) return E_PAR;

	ISpinLock(&pt_lock, &imask);
	for ( ; va < end; va += PAGE_SIZE ) {
		UD	*l3 = pt_l3_of(l0_pa, va, FALSE);

		if ( l3 == NULL || ( l3[PT_IDX(va, L3_SHIFT)] & PTE_P ) == 0 ) continue;
		l3[PT_IDX(va, L3_SHIFT)] = 0;
		tlb_flush_in(l0_pa, va, FALSE);
	}
	ISpinUnlock(&pt_lock, &imask);
	return E_OK;
}

/*
 * The tables under [va, va + size) of the user half that map nothing
 * any more taken out of the space at l0_pa: a page table that has no
 * entry left, and the page directory above it when that has none left
 * either (design 4.6.2). Each one is unhooked and every processor's
 * translations dropped here, under the lock; the page itself is given
 * back later (knl_pt_reclaim), once every other processor has been seen
 * past any walk of the tables it may have begun without the lock.
 */
#define PT_DEAD_MAX	64

/*
 * The tables taken out by one call make a batch: what each processor's
 * ctxsw_cnt was then is kept once for the batch, in that processor's
 * data (T_PCPU.pt_seen[batch]), not for each table
 */
LOCAL struct {
	UD	pa;			/* the table's page */
	INT	batch;			/* 0..PT_BATCHES-1 */
}		pt_dead[PT_DEAD_MAX];
LOCAL INT	pt_ndead = 0;
LOCAL INT	pt_cur = -1;		/* the batch being filled, under pt_lock */
LOCAL UW	pt_pruned = 0;		/* tables taken out, for the tests */

/* Under pt_lock: a batch none of whose tables waits any more, opened now; -1 when all wait */
LOCAL INT pt_batch_open( void )
{
	BOOL	used[PT_BATCHES];
	INT	b, i;

	for ( b = 0; b < PT_BATCHES; b++ ) used[b] = FALSE;
	for ( i = 0; i < pt_ndead; i++ ) used[pt_dead[i].batch] = TRUE;
	for ( b = 0; b < PT_BATCHES && used[b]; b++ ) ;
	if ( b == PT_BATCHES ) {
		return -1;
	}
	for ( i = 0; i < knl_num_prc; i++ ) {
		T_PCPU	*p = knl_pcpu_of(i);

		p->pt_seen[b] = p->ctxsw_cnt;
	}
	return b;
}

LOCAL BOOL pt_empty( CONST UD *tbl )
{
	INT	i;

	for ( i = 0; i < PT_ENTRIES; i++ ) {
		if ( tbl[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* Under pt_lock: the table at pa to be given back once no walk can be in it */
LOCAL BOOL pt_bury( UD pa )
{
	if ( pt_ndead >= PT_DEAD_MAX || pt_cur < 0 ) {
		return FALSE;
	}
	pt_dead[pt_ndead].pa = pa;
	pt_dead[pt_ndead].batch = pt_cur;
	pt_ndead++;
	return TRUE;
}

EXPORT void knl_pt_prune( UD l0_pa, UBINT va, UD size )
{
	UBINT	end = va + size, r;
	UINT	imask;
	BOOL	any = FALSE;

	if ( size == 0 || va >= USER_HALF_END ) return;
	ISpinLock(&pt_lock, &imask);
	pt_cur = pt_batch_open();
	if ( pt_cur < 0 ) {
		ISpinUnlock(&pt_lock, &imask);
		return;				/* every batch still waits: taken out another time */
	}
	for ( r = va & ~(((UBINT)1 << L2_SHIFT) - 1); r < end; r += (UBINT)1 << L2_SHIFT ) {
		UD	*l0 = (UD *)PA2VA(l0_pa), *l1, *l2, *l3;
		UD	*e0, *e1, *e2;

		e0 = &l0[PT_IDX(r, L0_SHIFT)];
		if ( !PTE_IS_TABLE(*e0) ) continue;
		l1 = (UD *)PA2VA(PTE_PA(*e0));
		e1 = &l1[PT_IDX(r, L1_SHIFT)];
		if ( !PTE_IS_TABLE(*e1) ) continue;
		l2 = (UD *)PA2VA(PTE_PA(*e1));
		e2 = &l2[PT_IDX(r, L2_SHIFT)];
		if ( !PTE_IS_TABLE(*e2) ) continue;
		l3 = (UD *)PA2VA(PTE_PA(*e2));
		if ( !pt_empty(l3) || pt_ndead >= PT_DEAD_MAX - 1 ) continue;

		*e2 = 0;
		(void)pt_bury(VA2PA(l3));
		pt_pruned++;
		any = TRUE;
		if ( pt_empty(l2) ) {
			*e1 = 0;
			(void)pt_bury(VA2PA(l2));
			pt_pruned++;
		}
	}
	if ( any ) {
		tlb_flush_in(l0_pa, 0, TRUE);	/* the walks cached of the tables taken out */
	}
	pt_cur = -1;
	ISpinUnlock(&pt_lock, &imask);
}

/*
 * The tables knl_pt_prune took out given back, those that every other
 * online processor has gone through the dispatcher since, or is idle
 * in it: a walk without the lock is made with interrupts disabled, so
 * one that had begun before has ended by then. TRUE while some still
 * wait.
 */
EXPORT BOOL knl_pt_reclaim( void )
{
	UD	free_pa[PT_DEAD_MAX];
	INT	nfree = 0, i, k, me = knl_pcpu()->prcid - 1;
	UINT	imask;
	BOOL	left;

	ISpinLock(&pt_lock, &imask);
	for ( i = 0; i < pt_ndead; ) {
		BOOL	past = TRUE;

		for ( k = 0; k < knl_num_prc; k++ ) {
			T_PCPU	*p = knl_pcpu_of(k);

			if ( k == me || p->state != PC_ONLINE ) continue;
			if ( p->ctxsw_cnt == p->pt_seen[pt_dead[i].batch] && p->ctxtsk != NULL ) {
				past = FALSE;
				break;
			}
		}
		if ( past ) {
			free_pa[nfree++] = pt_dead[i].pa;
			pt_dead[i] = pt_dead[--pt_ndead];
		} else {
			i++;
		}
	}
	left = (BOOL)( pt_ndead > 0 );
	ISpinUnlock(&pt_lock, &imask);
	for ( i = 0; i < nfree; i++ ) {
		knl_free_pages(knl_pa_to_pf(free_pa[i]), 0);
	}
	return left;
}

/* Tables taken out so far, and those not yet given back (the tests) */
EXPORT UW knl_pt_pruned( INT *p_waiting )
{
	if ( p_waiting != NULL ) *p_waiting = pt_ndead;
	return pt_pruned;
}

/*
 * Physical address of a 4KB-mapped 'va' (page granularity). ~0 if unmapped
 * or mapped by a large page.
 */
EXPORT UD knl_pt_lookup( UD l0_pa, UBINT va )
{
	UINT	s = disint();			/* a walk without the lock (knl_pt_prune) */
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e = ( l3 != NULL ) ? l3[PT_IDX(va, L3_SHIFT)] : 0;

	enaint(s);
	if ( (e & PTE_P) == 0 ) return ~(UD)0;
	return PTE_PA(e) | (va & (PAGE_SIZE - 1));
}

/* The entry that maps a page, with its access bits; 0 when there is none */
EXPORT UD knl_pt_entry( UD l0_pa, UBINT va )
{
	UINT	s = disint();
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e = ( l3 != NULL ) ? l3[PT_IDX(va, L3_SHIFT)] : 0;

	enaint(s);
	return ( (e & PTE_P) != 0 ) ? e : 0;
}

/*
 * The first page address in [va, end) that the tables at l0_pa map, or
 * end when none does. A table that is not there is stepped over whole.
 */
EXPORT UBINT knl_pt_next( UD l0_pa, UBINT va, UBINT end )
{
	UINT	s;
	BOOL	found = FALSE;

	va &= ~(UBINT)(PAGE_SIZE - 1);
	while ( va < end && !found ) {
		UD	*tbl = (UD *)PA2VA(l0_pa);
		UINT	shift;

		s = disint();
		for ( shift = L0_SHIFT; ; shift -= 9 ) {
			UD	e = tbl[PT_IDX(va, shift)];

			if ( (e & PTE_P) == 0 ) {
				UBINT	next = ( va | ( ((UBINT)1 << shift) - 1 ) ) + 1;

				va = ( next <= va ) ? end : next;
				break;
			}
			if ( shift == L3_SHIFT || !PTE_IS_TABLE(e) ) {
				found = TRUE;
				break;
			}
			tbl = (UD *)PA2VA(PTE_PA(e));
		}
		enaint(s);
	}
	return ( va < end ) ? va : end;
}

/* ------------------------------------------------------------------------ */
/*
 * The device window. The loader mapped its first 4GB (the registers of
 * the chipset, the Local APIC and the IOAPICs, the 32 bit BARs); a
 * gigabyte above is mapped as it is first touched, from page
 * directories set aside here, since a fault may come where nothing may
 * be allocated.
 */
#define DEV_SPARE_PD	32
#define GB		U64(0x40000000)
#define DEV_WINDOW	( VMAP_VA_BASE - DEV_VA_BASE )
#define DEV_FAR_SLOTS	(INT)( ( DEV_WINDOW - DEV_NEAR_SIZE ) / GB )

LOCAL UD	dev_pd[DEV_SPARE_PD][PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
LOCAL INT	dev_pd_used = 0;
LOCAL T_SPLOCK	dev_lock;

/* The gigabytes far above, by slot of the window's last terabyte */
LOCAL UD	dev_far[DEV_SPARE_PD];
LOCAL INT	dev_far_n = 0;

/* The PDPT entry of the window that covers the window's address va */
LOCAL UD *dev_pdpte( UBINT va )
{
	UD	*pml4 = (UD *)PA2VA(knl_pt_l0_k_pa);
	UD	e = pml4[PT_IDX(va, L0_SHIFT)];

	if ( !PTE_IS_TABLE(e) ) {
		return NULL;
	}
	return &((UD *)PA2VA(PTE_PA(e)))[PT_IDX(va, L1_SHIFT)];
}

/* The physical gigabyte the window's address va shows, ~0 for none */
LOCAL UD dev_gb_of( UBINT va )
{
	UD	off = va - DEV_VA_BASE;
	INT	slot;

	if ( off < DEV_NEAR_SIZE ) {
		return off & ~( GB - 1 );
	}
	slot = (INT)( ( off - DEV_NEAR_SIZE ) / GB );
	return ( slot < dev_far_n ) ? dev_far[slot] : ~(UD)0;
}

/* The window's address of a physical address at or above DEV_NEAR_SIZE (DEV_BASE) */
EXPORT unsigned long long knl_dev_far( unsigned long long pa )
{
	UD	gb = pa & ~( GB - 1 );
	UINT	imask;
	INT	i;

	ISpinLock(&dev_lock, &imask);
	for ( i = 0; i < dev_far_n && dev_far[i] != gb; i++ ) ;
	if ( i == dev_far_n && dev_far_n < DEV_SPARE_PD && dev_far_n < DEV_FAR_SLOTS ) {
		dev_far[dev_far_n++] = gb;
	}
	ISpinUnlock(&dev_lock, &imask);
	if ( i >= dev_far_n ) {
		return 0;		/* no slot left: an address that faults at once */
	}
	return DEV_VA_BASE + DEV_NEAR_SIZE + (UD)i * GB + ( pa & ( GB - 1 ) );
}

EXPORT BOOL knl_dev_fault( UBINT va )
{
	UD	*e, gb;
	UINT	imask;
	BOOL	ok = FALSE;
	INT	i;

	if ( va < DEV_VA_BASE || va >= DEV_VA_BASE + DEV_WINDOW ) {
		return FALSE;
	}
	va &= ~(UBINT)( GB - 1 );
	ISpinLock(&dev_lock, &imask);
	gb = dev_gb_of(va);
	e = ( gb != ~(UD)0 ) ? dev_pdpte(va) : NULL;
	if ( e != NULL && ( *e & PTE_P ) != 0 ) {
		ok = TRUE;			/* another processor mapped it meanwhile */
	} else if ( e != NULL && dev_pd_used < DEV_SPARE_PD ) {
		UD	*pd = dev_pd[dev_pd_used++];

		for ( i = 0; i < PT_ENTRIES; i++ ) {
			pd[i] = ( gb + ( (UD)i << L2_SHIFT ) ) | PTE_LARGE_DEV;
		}
		*e = VA2PA(pd) | PTE_P | PTE_RW;
		ok = TRUE;
	}
	ISpinUnlock(&dev_lock, &imask);
	return ok;
}

EXPORT void *knl_dev_va( UBINT pa )
{
	return (void *)DEV_BASE(pa);
}

/*
 * The 2MB pages of the window over [pa, pa + size) made write-combining:
 * the screen, written a pixel at a time and never read back
 */
/*
 * Whether the device window shows pa now (a 2MB or a 1GB page there),
 * without touching it: a screen above 4GB is used only once it is
 */
EXPORT BOOL knl_dev_mapped( UD pa )
{
	UBINT	va = DEV_BASE(pa);
	UD	*e, *pd;

	if ( va == 0 || knl_pt_l0_k_pa == 0 ) {
		return FALSE;
	}
	e = dev_pdpte(va);
	if ( e == NULL || ( *e & PTE_P ) == 0 ) {
		return FALSE;
	}
	if ( !PTE_IS_TABLE(*e) ) {
		return TRUE;			/* a 1GB page */
	}
	pd = (UD *)PA2VA(PTE_PA(*e));
	return (BOOL)( ( pd[PT_IDX(va, L2_SHIFT)] & PTE_P ) != 0 );
}

EXPORT void knl_dev_set_wc( UD pa, UD size )
{
	UD	a, *pdpte, *pd;

	for ( a = pa & ~( ( U64(1) << L2_SHIFT ) - 1 ); a < pa + size; a += U64(1) << L2_SHIFT ) {
		pdpte = dev_pdpte(DEV_BASE(a));
		if ( pdpte == NULL || !PTE_IS_TABLE(*pdpte) ) {
			if ( pdpte == NULL || !knl_dev_fault(DEV_BASE(a)) ) continue;
		}
		pd = (UD *)PA2VA(PTE_PA(*pdpte));
		pd[PT_IDX(DEV_BASE(a), L2_SHIFT)] = ( pd[PT_IDX(DEV_BASE(a), L2_SHIFT)] & ~PTE_CACHE_UC ) | PTE_CACHE_WC;
		invlpg(DEV_BASE(a));
	}
}

/* ------------------------------------------------------------------------ */
/*
 * T-Kernel/SM address space services
 */

/*
 * MapMemory: physical range -> kernel virtual address.
 *	Device memory (MM_CDIS or outside RAM) comes from the device window,
 *	RAM from the linear map; nothing is allocated, so UnmapMemory is a no-op.
 */
EXPORT ER MapMemory( CONST void *paddr, SZ len, UINT attr, void **laddr )
{
	UD	pa = (UD)(UBINT)paddr;
	INT	i;
	BOOL	ram = FALSE;

	if ( laddr == NULL || len <= 0 ) return E_PAR;
	if ( (attr & MM_USER) != 0 ) return E_NOSPT;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	s = knl_mem_range[i].start, e = s + knl_mem_range[i].size;
		if ( pa >= s && pa + (UD)len <= e ) {
			ram = TRUE;
			break;
		}
	}
	if ( ram && (attr & MM_CDIS) == 0 ) {
		*laddr = PA2VA(pa);
	} else {
		if ( pa < DEV_NEAR_SIZE ? pa + (UD)len > DEV_NEAR_SIZE
				       : ( pa & ~( GB - 1 ) ) != ( ( pa + (UD)len - 1 ) & ~( GB - 1 ) ) ) {
			return E_PAR;		/* across the end of what one place of the window shows */
		}
		*laddr = (void *)DEV_BASE(pa);
		if ( *laddr == NULL ) return E_LIMIT;
	}
	return E_OK;
}

EXPORT ER UnmapMemory( CONST void *laddr )
{
	return E_OK;
}

/*
 * ConvPhysicalAddress: kernel virtual -> physical. Returns the length of
 * the physically contiguous range starting at laddr (up to len).
 */
EXPORT INT ConvPhysicalAddress( CONST void *laddr, INT len, void **paddr )
{
	UBINT	va = (UBINT)laddr;
	UD	pa;

	if ( paddr == NULL || len <= 0 ) return E_PAR;
	if ( va < KVA_BASE ) return E_PAR;	/* a process's address: its own tables map it (knl_prc_dma) */

	if ( va >= KIMG_VA_BASE || ( va >= KVA_BASE && va < DEV_VA_BASE ) ) {	/* the image, the linear map */
		*paddr = (void *)VA2PA(va);
		return len;
	}
	if ( va >= DEV_VA_BASE && va < VMAP_VA_BASE ) {		/* device window */
		pa = dev_gb_of(va);
		if ( pa == ~(UD)0 ) return E_PAR;
		*paddr = (void *)(UBINT)( pa + ( va & ( GB - 1 ) ) );
		return len;
	}
	pa = knl_pt_lookup(knl_pt_l0_k_pa, va);
	if ( pa == ~(UD)0 ) return E_PAR;
	*paddr = (void *)(UBINT)pa;
	{
		/* count the contiguous pages */
		INT	n = (INT)(PAGE_SIZE - (va & (PAGE_SIZE - 1)));
		UD	next = (pa & PAGE_MASK) + PAGE_SIZE;
		UBINT	v = (va & PAGE_MASK) + PAGE_SIZE;
		while ( n < len && knl_pt_lookup(knl_pt_l0_k_pa, v) == next ) {
			n += PAGE_SIZE;
			next += PAGE_SIZE;
			v += PAGE_SIZE;
		}
		return ( n < len ) ? n : len;
	}
}

#endif /* CPU_CORE_X86_64 */
