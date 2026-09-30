/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	space.h
 *	Process spaces (TTBR0), design 6.7
 */

#ifndef _SYSMAN_SPACE_H_
#define _SYSMAN_SPACE_H_

#define ASID_NULL	0		/* empty space of kernel tasks */
#define ASID_SYSPRC	1		/* system process (PID 0) */
#define ASID_MAX	65535		/* TCR is set for 16 bit ASIDs (TCR_EL1.AS) */

#define USER_TEXT_VA	0x0000000000400000ULL	/* program image base (design 6.2.2) */
#define USER_STACK_TOP	0x0000FFFFF0000000ULL	/* user stacks grow down from here */

/*
 * The shared memory window (design 6.8): what is mapped here is at the
 * same address in every process that maps it. The pages belong to
 * whoever shared them, not to the space, so taking a space down leaves
 * them alone.
 */
#define SHM_WINDOW_BASE	0x0000400000000000ULL
#define SHM_WINDOW_END	0x0000800000000000ULL

/*
 * One address space below the kernel half
 */
typedef struct {
	UD	l0_pa;		/* L0 table */
	UD	ttbr0;		/* l0_pa | ASID << 48 */
	UINT	asid;
	UBINT	stk_next;	/* next user stack top (grows down) */
} T_SPACE;

IMPORT UD	knl_ttbr0_null;		/* TTBR0 value of an empty space */
IMPORT UD	knl_sysprc_ttbr0;	/* TTBR0 value of the system process */
IMPORT UD	knl_sysprc_l0_pa;	/* its L0 table */
IMPORT T_SPACE	knl_sysprc_space;

/*
 * Space a new EL0 task takes its user stack from. Process creation points
 * this at the new process while it creates the main task, so that the
 * stack lands in the process's own space; it is NULL otherwise, meaning
 * the system process.
 */
IMPORT T_SPACE	*knl_new_task_space;

IMPORT void	knl_space_init( void );
IMPORT ER	knl_space_create( T_SPACE *sp );
IMPORT void	knl_space_delete( T_SPACE *sp );
IMPORT ER	knl_space_map_zero( T_SPACE *sp, UBINT va, UD size, UD attr );
IMPORT ER	knl_space_protect( T_SPACE *sp, UBINT va, UD size, UD attr );
IMPORT void	*knl_space_page( T_SPACE *sp, UBINT va );
IMPORT ER	knl_space_sink( T_SPACE *sp, UBINT va );

/* Addresses in the shared window: each handed out once, never again */
IMPORT UBINT	knl_shm_va_alloc( UD size );

IMPORT void	*knl_ustack_alloc_sp( T_SPACE *sp, UD npages, void **p_top );
IMPORT void	knl_ustack_free_sp( T_SPACE *sp, void *base, UD npages );
IMPORT void	knl_ustack_free_l0( UD l0_pa, void *base, UD npages );
IMPORT void	*knl_ustack_alloc( UD npages, void **p_top );	/* uses knl_new_task_space */
IMPORT void	knl_ustack_free( void *base, UD npages );

#endif /* _SYSMAN_SPACE_H_ */
