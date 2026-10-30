/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kernel.h (host)
 *	Just enough of the kernel's types and calls for outer_kernel/wm/
 *	compose.c to build and run on the host, where tools/mesa/
 *	test_cp_shaders.c sets its stages against the GPU's shaders.
 */

#ifndef _HOSTSHIM_KERNEL_H_
#define _HOSTSHIM_KERNEL_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int8_t		B;
typedef int16_t		H;
typedef int32_t		W;
typedef int64_t		D;
typedef uint8_t		UB;
typedef uint16_t	UH;
typedef uint32_t	UW;
typedef uint64_t	UD;
typedef int		INT;
typedef unsigned int	UINT;
typedef unsigned int	BOOL;
typedef long		SZ;
typedef int		ER;
typedef int		ID;
typedef uintptr_t	UBINT;

#define TRUE		1
#define FALSE		0
#define CONST		const
#define LOCAL		static
#define EXPORT
#define IMPORT		extern
#define E_OK		0

#define PAGE_SIZE	4096UL

static inline void *knl_memcpy( void *d, const void *s, SZ n ) { return memcpy(d, s, (size_t)n); }
static inline void *knl_memset( void *d, int c, SZ n ) { return memset(d, c, (size_t)n); }
static inline void *knl_vmap( UD npages, UINT flags ) { (void)flags; return calloc(npages, PAGE_SIZE); }
static inline void knl_vunmap( void *p, UD npages ) { (void)npages; free(p); }

/* One task on the host, which may not use the vector loops (compose.c cp_simd_here) */
static inline ID tk_get_tid( void ) { return 1; }

#endif /* _HOSTSHIM_KERNEL_H_ */
