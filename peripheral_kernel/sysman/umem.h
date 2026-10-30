/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	umem.h
 *	The reservations and pages a process asked for (umem.c, design 9.8.1).
 *	Called by proc.c under the process table's lock; 'hold' says a
 *	task of the process is inside a call, so pages taken away are held
 *	back until knl_umem_flush.
 */

#ifndef _SYSMAN_UMEM_H_
#define _SYSMAN_UMEM_H_

typedef struct umem	T_UMEM;

/* The access a fault was met by (knl_umem_fault) */
#define UM_ACC_READ	0
#define UM_ACC_WRITE	1
#define UM_ACC_EXEC	2

IMPORT ER   knl_umem_map( T_UMEM **pp, T_SPACE *sp, UBINT *p_va, UD size, UD align,
			  UINT prot, BOOL hold );
IMPORT ER   knl_umem_unmap( T_UMEM **pp, T_SPACE *sp, UBINT va, UD size, BOOL hold );
IMPORT ER   knl_umem_ctl( T_UMEM **pp, T_SPACE *sp, UBINT va, UD size, UINT prot, BOOL hold );
IMPORT BOOL knl_umem_fault( T_UMEM *um, T_SPACE *sp, UBINT va, UINT acc );
IMPORT void knl_umem_wx( T_UMEM *um, T_SPACE *sp, UD out[6] );
IMPORT void knl_umem_flush( T_UMEM *um );
IMPORT BOOL knl_umem_held( T_UMEM *um );
IMPORT UD   knl_umem_bytes( T_UMEM *um );
IMPORT void knl_umem_free( T_UMEM **pp );

#endif /* _SYSMAN_UMEM_H_ */
