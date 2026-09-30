/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	smem.h
 *	System memory management (T-Kernel/SM 5.1) and address space
 *	services on top of the page frame allocator (design 6.5, 6.6).
 */

#ifndef __TK_SMEM_H__
#define __TK_SMEM_H__

#include <tk/typedef.h>

/* tk_get_smb attributes (besides TA_RNG0..3) */
#define TA_NORESIDENT	0x00000010U	/* may be non-resident (accepted, always resident for now) */
#define TA_NOCACHE	0x00000040U	/* Normal non-cacheable (for DMA descriptors) */
#define TA_DMA32	0x00000400U	/* TessronOS: physical address below 4GB */

typedef struct t_rsmb {
	INT	blksz;		/* block (page) size in bytes */
	INT	total;		/* total number of blocks */
	INT	free;		/* free blocks */
} T_RSMB;

IMPORT void	*tk_get_smb( INT nblk, UINT attr );
IMPORT ER	tk_rel_smb( void *addr );
IMPORT ER	tk_ref_smb( T_RSMB *pk_rsmb );

/* MapMemory attributes */
#define MM_USER		0x00000001U	/* mapped into the caller's process space (later phase) */
#define MM_SYSTEM	0x00000000U
#define MM_CDIS		0x00000002U	/* device memory (cache disabled) */
#define MM_READ		0x00000010U
#define MM_WRITE	0x00000020U
#define MM_EXECUTE	0x00000040U

IMPORT ER	MapMemory( CONST void *paddr, SZ len, UINT attr, void **laddr );
IMPORT ER	UnmapMemory( CONST void *laddr );
IMPORT INT	ConvPhysicalAddress( CONST void *laddr, INT len, void **paddr );

/* Kernel variable area (vmap): non-contiguous pages at a contiguous VA */
#define VMAP_NOCACHE	0x01
#define VMAP_DMA32	0x02

IMPORT void	*knl_vmap( UD npages, UINT flags );
/* The same area, but over memory that already exists (a framebuffer) */
IMPORT void	*knl_vmap_pa( UD pa, UD npages, UINT flags );
IMPORT void	knl_vunmap_pa( void *va, UD npages );
IMPORT void	knl_vunmap( void *va, UD npages );

#endif /* __TK_SMEM_H__ */
