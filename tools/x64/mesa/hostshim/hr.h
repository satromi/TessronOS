/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hr.h (host)
 *	The plan of compositing on Intel's render engine behind plain C
 *	types (host_rplan.c), for the host test of the shaders.
 */

#ifndef _X64_HOSTSHIM_HR_H_
#define _X64_HOSTSHIM_HR_H_

#include <stdint.h>
#include "../../../mesa/hostshim/hc.h"

#define HR_NPUSH	64

typedef struct {
	int		fs, barrier;
	int		l, t, r, b;
	uint32_t	par[HR_NPUSH];
} HR_RD;

/* The GPU address of len bytes from p, 0 when there is none */
typedef uint64_t HR_GPU( const void *p, uint64_t len );

int hr_plan( uint8_t *back, unsigned pitch, HC_RECT box, int rows, const uint32_t *ground, unsigned gpitch,
	     const HC_WIN *w, int n, HR_GPU *gpu, uint64_t t1, uint64_t t2, const uint64_t *cov, HR_RD *rd, int max,
	     int *p_nrd );

#endif /* _X64_HOSTSHIM_HR_H_ */
