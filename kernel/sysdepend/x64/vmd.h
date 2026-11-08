/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vmd.h (x64 PC)
 *	Intel Volume Management Device (vmd.c)
 */

#ifndef __VMD_H__
#define __VMD_H__

IMPORT void	knl_vmd_init( BOOL off );

/* The number of the first bus of a segment, from its VMCAP and VMCONFIG */
IMPORT INT	knl_vmd_first_bus( UW vmcap, UW vmconfig, BOOL restricts );

/* What a function behind it writes so that the VMD raises its MSI-X entry */
IMPORT void	knl_vmd_child_msg( UINT entry, UD *p_addr, UW *p_data );

#ifdef USE_KTEST
IMPORT INT	knl_vmd_sim_on;
IMPORT void	knl_vmd_sim( void );
IMPORT void	knl_vmd_sim_port( UINT *p_sec, UD *p_base, UD *p_limit );
#endif

#endif /* __VMD_H__ */
