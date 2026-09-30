/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	smem.c
 *	System memory management (T-Kernel/SM 5.1): tk_get_smb / tk_rel_smb /
 *	tk_ref_smb in page units on top of the page frame allocator.
 *	Small requests get physically contiguous pages in the linear map;
 *	large or non-cacheable requests go through the kernel variable area.
 *	TA_RNG2/3 (user visible) is treated like the kernel until process
 *	spaces exist (design 6.5).
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "pfalloc.h"
#include <tk/smem.h>

/*
 * Taken with interrupts disabled around the table of system memory blocks: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	smb_lock;

typedef struct {
	void	*addr;		/* NULL: entry free */
	UD	npages;
	UB	order;		/* SMB_LINEAR: buddy order */
	UB	kind;
} SMB;

#define SMB_LINEAR	1
#define SMB_VMAP	2

LOCAL SMB	smb_tbl[CNF_MAX_SMB];

LOCAL UINT order_for( INT nblk )
{
	UINT	o = 0;
	while ( ((INT)1 << o) < nblk ) o++;
	return o;
}

EXPORT void *tk_get_smb( INT nblk, UINT attr )
{
	SMB	*s = NULL;
	INT	i;
	UINT	imask;
	void	*addr = NULL;
	UINT	order = 0;
	UB	kind;

	if ( nblk <= 0 ) return NULL;

	ISpinLock(&smb_lock, &imask);
	for ( i = 0; i < CNF_MAX_SMB; i++ ) {
		if ( smb_tbl[i].addr == NULL ) {
			s = &smb_tbl[i];
			s->addr = (void *)1;		/* claimed */
			break;
		}
	}
	ISpinUnlock(&smb_lock, &imask);
	if ( s == NULL ) return NULL;

	order = order_for(nblk);
	if ( (attr & TA_NOCACHE) == 0 && order <= PF_MAX_ORDER
	  && ((INT)1 << order) - nblk <= (nblk + 3) / 4 ) {
		/* contiguous: waste at most about 25% */
		PFRAME	*pf = knl_alloc_pages(order, ZONE_NORMAL, (attr & TA_DMA32) ? KAF_DMA32 : 0);
		if ( pf != NULL ) addr = knl_pf_to_va(pf);
		kind = SMB_LINEAR;
	} else {
		UINT	f = ( (attr & TA_NOCACHE) ? VMAP_NOCACHE : 0 ) | ( (attr & TA_DMA32) ? VMAP_DMA32 : 0 );
		addr = knl_vmap((UD)nblk, f);
		kind = SMB_VMAP;
	}

	if ( addr == NULL ) {
		s->addr = NULL;
		return NULL;
	}
	s->npages = (UD)nblk;
	s->order = (UB)order;
	s->kind = kind;
	s->addr = addr;
	return addr;
}

EXPORT ER tk_rel_smb( void *addr )
{
	SMB	*s = NULL;
	INT	i;
	UINT	imask;

	if ( addr == NULL ) return E_PAR;

	ISpinLock(&smb_lock, &imask);
	for ( i = 0; i < CNF_MAX_SMB; i++ ) {
		if ( smb_tbl[i].addr == addr ) {
			s = &smb_tbl[i];
			break;
		}
	}
	ISpinUnlock(&smb_lock, &imask);
	if ( s == NULL ) return E_PAR;

	if ( s->kind == SMB_LINEAR ) {
		knl_free_pages(knl_pa_to_pf(VA2PA(addr)), s->order);
	} else {
		knl_vunmap(addr, s->npages);
	}
	s->addr = NULL;
	return E_OK;
}

EXPORT ER tk_ref_smb( T_RSMB *pk_rsmb )
{
	if ( pk_rsmb == NULL ) return E_PAR;
	pk_rsmb->blksz = (INT)PAGE_SIZE;
	pk_rsmb->total = (INT)knl_pf_total_count(-1);
	pk_rsmb->free  = (INT)knl_pf_free_count(-1);
	return E_OK;
}
