/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt.h
 *	The render and copy engines of Intel's integrated graphics of the
 *	9th generation: what the files of the driver share
 *	(docs/tessronos/13-gpu.md 13.11).
 *
 *	igt.c		the device, forcewake, power, workarounds, engines,
 *			submission, interrupts, reset, the common layer's backend
 *	igt_vm.c	page tables of the GPU (PPGTT) and address spaces
 *	igt_test.c	the tests at start-up
 *	igt_pm.c	forcewake, RC6, the frequency as the load asks (RPS)
 *	igt_compose.c	compositing windows: the choice of engine, the copy
 *			engine's way, the check against the processor
 *	igt_rcompose.c	compositing on the render engine (compute shaders)
 *	igt_rplan.c	its plan of rectangles, built for the host test too
 */

#ifndef _DEVICE_GPU_INTEL_IGT_H_
#define _DEVICE_GPU_INTEL_IGT_H_

#include "../gpudev.h"
#include "sysdepend/x64/igt_ggtt.h"
#include "igt_regs.h"

#define IGT_RCS		0		/* the render engine */
#define IGT_BCS		1		/* the copy (blitter) engine */
#define IGT_NENG	2

#define IGT_RING_BYTES	( 16 * 1024 )	/* a context's ring */
#define IGT_KVA_TOP	0x100000000ULL	/* the kernel's space: 32 bit addresses */
#define IGT_KVA_LOW	0x10000ULL	/* none below this is handed out */

typedef struct igt_vm	IGTVM;

/* A logical ring context: one engine's state in one address space */
typedef struct {
	INT		eng;
	IGTVM		*vm;
	PFRAME		*pf;		/* the image, then the ring, contiguous */
	UINT		order;
	UB		*kva;
	UD		pa;
	UD		ggtt;		/* GGTT address of the image */
	UD		img_bytes;
	UD		ring_ggtt;
	UB		*ring;
	UW		tail;		/* bytes into the ring */
	UW		id;		/* in the descriptor, to tell its status entries */
	BOOL		fresh;		/* made anew from the first image before it runs */
} IGTCTX;

/* An address space: a four level page table of the GPU's */
struct igt_vm {
	UD		pml4_pa;
	BOOL		kernel;
	IGTCTX		*ctx[GPU_HWC_MAX][IGT_NENG];
	UD		npt;		/* tables beyond the top one, for the record */
};

typedef struct {
	CONST char	*name;
	UW		base;
	UINT		shift;		/* its bits in GT interrupt register 0, and in its own RING_IMR */
	UW		grdom;		/* its reset bit */
	UD		img_bytes;	/* its context image */
	PFRAME		*hwsp_pf;
	UW		*hwsp;		/* its status page */
	UD		hwsp_ggtt;
	UB		*golden;	/* the image saved after the workarounds ran; NULL before */
	UW		seqno;		/* of the last job handed to it */
	IGTCTX		*cur;		/* the context of the job running, NULL when idle */
	BOOL		idle_seen;	/* its status said active to idle since the job started */
	UINT		csb_head;
	UD		t_kick;		/* when the job started (system counter) */
	UD		t_seq;		/* when its sequence number was first seen written */
	/* counts, for the record */
	UD		jobs, irq_user, irq_ctx, irq_err, by_poll, no_idle, resets;
	/* the last error a job raised, as the interrupt read it; told by the next kick */
	UW		err_eir, err_ipehr, err_ipeir, err_instdone;
	UD		err_acthd, err_bbaddr;
	UD		err_told;	/* errors told so far */
} IGTENG;

typedef struct {
	BOOL		ready;
	T_IGTDEV	dev;
	UBINT		mmio;
	UH		devid;
	UB		rev;
	BOOL		pre_c0;		/* a Kaby Lake stepping before C0 */
	UINT		intno;		/* 0: no interrupt, only the 1ms look */
	IGTENG		eng[IGT_NENG];
	INT		active;		/* the engine of the job running, -1 none */
	IGTVM		*kvm;		/* the kernel's space */
	UD		scratch_pa;	/* a page of zeroes, and tables of entries pointing at it */
	UD		scr_pt, scr_pd, scr_pdp;
	UW		slice_mask, ss_mask, eu_total, fuse2, eu_dis;
	UINT		rp0, rp1, rpn;	/* MHz */
	ID		cyc;
	T_SPLOCK	lock;		/* the engines' state between the interrupt, the look and the task */
	ID		mtx;		/* page tables and contexts */
	UD		irq_n, irq_spurious, irq_display;
	BOOL		irq_off;
	void		(*display_irq)( UW master );
	UW		next_ctx_id;

	/* forcewake, RC6 and RPS (igt_pm.c), under 'lock' */
	BOOL		fw_held;	/* the three wells are asked to stay awake */
	BOOL		fw_always;	/* and never let go of (RC6 off) */
	ID		fw_mtx;		/* who wakes them, one at a time */
	INT		fw_users;	/* who reach the GT's registers now (a job counts until it ends) */
	UD		fw_idle_since;	/* when the last let go (us) */
	BOOL		rc6_on, rps_on;
	BOOL		pm_ready;	/* set up: the look may park */
	BOOL		parked;		/* idle: RC6 asked for, the frequency at its lowest */
	UINT		f_min, f_max, f_rp1, f_eff, f_cur, f_last;	/* units of 50/3 MHz */
	INT		f_adj;
	INT		power_mode;	/* -1, or 0 low, 1 between, 2 high */
	UW		pm_iir;		/* PM events not yet handled */
	ID		pm_tsk;
	UD		n_park, n_unpark, n_up, n_down, n_wake, n_pm_irq, n_fw_kick, n_fw_stuck;
} IGT;

IMPORT IGT	knl_igt;

/* ---------------------------------------------------------------- igt.c */
Inline UW igt_rd( UW off )
{
	return *(volatile UW *)( knl_igt.mmio + off );
}
Inline void igt_wr( UW off, UW v )
{
	*(volatile UW *)( knl_igt.mmio + off ) = v;
}
Inline UW igt_erd( CONST IGTENG *e, UW off )
{
	return igt_rd(e->base + off);
}
Inline void igt_ewr( CONST IGTENG *e, UW off, UW v )
{
	igt_wr(e->base + off, v);
}

/* The processor's caches written back over a range (the GPU's tables, the commands) */
IMPORT void	igt_clflush( CONST void *p, UD len );

/* Microseconds from the system counter */
IMPORT UD	igt_now_us( void );

/* The state of an engine, for the record */
IMPORT void	igt_eng_dump( CONST char *why, IGTENG *e );

/* Kernel jobs: a batch in the kernel's space, waited for (igt_test.c, igt_compose.c) */
IMPORT ER	igt_kjob( INT eng, UD batch_va, UW len, T_GPUBO *CONST *bo, INT nbo );

/* ---------------------------------------------------------------- igt_pm.c */
IMPORT BOOL	igt_fw_init( void );		/* the three wells woken the first time, held by the caller */
IMPORT void	igt_fw_get( void );		/* the GT's registers needed: awake on return (a task's) */
IMPORT void	igt_fw_put( void );		/* no more: the GT may sleep once idle a while */
IMPORT void	igt_pm_init( void );		/* both off, the frequencies read, RP1 */
IMPORT void	igt_pm_start( void );		/* RC6 and RPS as cmdline.txt says (igtrc6=, igtrps=) */
IMPORT void	igt_pm_tick( void );		/* from the 1ms look: the GT parked when idle */
IMPORT BOOL	igt_pm_irq( UW iir );		/* GT interrupt register 2, under the lock: TRUE, igt_pm_wake */
IMPORT void	igt_pm_wake( void );		/* the task woken, after the lock */
IMPORT INT	igt_pm_text( char *t, INT max );	/* the state, a line */
IMPORT INT	igt_setting( CONST char *word, INT dflt );	/* "word=N" of cmdline.txt */

/* ---------------------------------------------------------------- igt_vm.c */
IMPORT ER	igt_vm_init( void );
IMPORT IGTVM	*igt_vm_create( BOOL kernel );
IMPORT void	igt_vm_destroy( IGTVM *vm );
IMPORT ER	igt_vm_map( IGTVM *vm, UD va, CONST UD *pa, UD npages );
IMPORT void	igt_vm_unmap( IGTVM *vm, UD va, UD npages );
IMPORT UD	igt_vm_lookup( IGTVM *vm, UD va );	/* the entry, for tests and the record */
IMPORT ER	igt_kva_alloc( UD npages, UW *p_va );
IMPORT void	igt_kva_free( UW va, UD npages );
IMPORT UD	igt_kva_free_pages( void );

/* The backend's address space operations for the common layer */
IMPORT ER	igt_bo_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset );
IMPORT void	igt_bo_unbind( void *priv, UW offset, UD npages );
IMPORT ER	igt_as_create( void *priv, void **p_as );
IMPORT void	igt_as_destroy( void *priv, void *as );
IMPORT ER	igt_as_bind( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va );
IMPORT void	igt_as_unbind( void *priv, void *as, UW va, UD npages );
IMPORT ER	igt_as_bind_at( void *priv, void *as, T_GPUBO *bo, UD npages, UD va );
IMPORT void	igt_as_unbind_at( void *priv, void *as, UD va, UD npages );

/* Contexts (igt.c) are freed with their space */
IMPORT void	igt_ctx_free( IGTCTX *c );

/* ---------------------------------------------------------------- igt_test.c, igt_compose.c */
IMPORT void	igt_test_start( void );
IMPORT ER	igt_compose_init( void );
IMPORT BOOL	igt_compose_check( void );
IMPORT INT	igt_compose_used( void );	/* 0 the processor, 1 the copy engine, 2 the render engine */

#endif /* _DEVICE_GPU_INTEL_IGT_H_ */
