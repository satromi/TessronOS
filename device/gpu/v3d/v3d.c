/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	v3d.c
 *	The V3D 7.1 of the Raspberry Pi 5 (docs/tessronos/13-gpu.md 13.3).
 *
 *	The clock and the identification are verified on the board; the
 *	steps after them are not yet. The build decides how far it goes,
 *	so that the first runs on the board only look:
 *
 *	  CNF_V3D = 0	nothing (the default).
 *	  CNF_V3D = 1	turn the V3D's clock on through the firmware, read
 *			the identification words and the MMU's address
 *			widths, print them, and stop. Nothing is written
 *			to the V3D.
 *	  CNF_V3D = 2	also set up the MMU and the interrupts, register
 *			as the GPU backend and run a small self test
 *			(a BO entered in the MMU, a cache clean job), then
 *			the tests on the board (v3d_test.c), which end with
 *			the check of compositing on the GPU (v3d_compose.c).
 *	  CNF_V3D = 3	the same without the tests on the board: the GPU
 *			set up for use, its compositing checked once and
 *			used when the check passes.
 *
 *	The core's first identification word reads "V3D" when the block is
 *	powered and clocked. Nothing else is read before that word is seen,
 *	so that a block that is not powered costs one read and a message.
 *
 *	One job runs at a time (gpu.c). A control list job is two steps on
 *	the hardware: the binner list, whose end the core tells with the
 *	binner-flush-done interrupt, then the renderer list, told with
 *	frame-done. The interrupt handler starts the second step itself,
 *	so the common layer sees one job and one end.
 *
 *	A job ends only when the hardware is done with it. A binner that
 *	waits for memory, or a list that does not end, is left to the
 *	common layer's time limit, whose reset puts the whole block back in
 *	its power-on state (v3d_reset): a thread of the control list
 *	executor that is still running would otherwise go on into the next
 *	job, which then never starts.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "../gpudev.h"
#include "v3d.h"
#include "v3d_regs.h"

#ifndef CNF_V3D
#define CNF_V3D		0
#endif

/* Firmware property tags for clocks, and the V3D's clock */
#define TAG_GET_CLOCK_STATE	0x00030001U
#define TAG_SET_CLOCK_STATE	0x00038001U
#define TAG_GET_CLOCK_RATE	0x00030002U
#define TAG_SET_CLOCK_RATE	0x00038002U
#define TAG_GET_MAX_CLOCK_RATE	0x00030004U
#define TAG_ANSWERED		0x80000000U
#define CLOCK_V3D		5
#define CLOCK_ON		0x1
#define CLOCK_MISSING		0x2

/*
 * Binner overflow memory. The binner takes it in pieces: one is given
 * before the list starts, and each out-of-memory interrupt says that the
 * binner has taken the piece it had and wants the next one. The pieces
 * of an address space are made when first needed and kept for its later
 * jobs (one job runs at a time, and its renderer has read the tile lists
 * before the next job starts), up to OVF_CHUNKS_MAX.
 */
#define OVF_CHUNK_BYTES		( 256 * 1024 )
#define OVF_CHUNKS_FIRST	2			/* made with the space */
#define OVF_CHUNKS_MAX		256			/* 64MB a space */
#define OVF_TASK_PRI		8			/* above the GPU's task */
#define OVF_TASK_STKSZ		8192
#define OVF_TOLD		32			/* growths told on the console */

/* The reset: how long the power state manager may take, in us */
#define SMS_WAIT_US		( 100 * 1000 )
#define RESET_HOLD_US		10

/* Polling limits for the cache and TLB operations */
#define POLL_MAX		1000000

/*
 * A job that raises more interrupts than this is taken for a line that
 * cannot be cleared: the interrupt is turned off and the job ends, so
 * that the machine goes on. MMU faults are reported this many per job.
 */
#define JOB_INTS_MAX		1000
#define JOB_FAULTS_SHOWN	4

/* Where a control list job is */
#define PH_IDLE			0
#define PH_BIN			1
#define PH_RENDER		2

/*
 * The core's interrupts the driver acts on. Every other bit is allowed
 * too, and told of on the console the first time it comes, so that a
 * bit placed differently in this generation shows itself.
 */
#define CORE_INTS_KNOWN		( V3D_INT_FRDONE | V3D_INT_FLDONE | V3D_INT_OUTOMEM | V3D_INT_CSDDONE_71 )

/* The binner's and the renderer's ends are also seen in their counters */
#define POLL_CYC_MS		1

LOCAL T_SPLOCK	v3d_lock;

#define SMS(r)		( DEV_BASE(V3D_SMS_PA) + (r) )
#define PM(r)		( PM_BASE + (r) )

/* A piece of overflow memory, entered in the table of the space it serves */
typedef struct {
	T_GPUBO		*bo;
	UW		va;
} V3DOVF;

/* The pieces of a space; 'n' grows under v3d_lock once the piece is entered */
typedef struct {
	V3DOVF		*chunk;			/* OVF_CHUNKS_MAX of them */
	UINT		n;			/* made */
	BOOL		growing;		/* the overflow task enters one now */
	BOOL		told_max;		/* the limit told once */
} V3DPOOL;

/*
 * An address space of a process's (13-gpu.md 13.5): a page table of its
 * own over the whole 32 bit space, so that no address the GPU is given
 * reads an entry from outside the table, whatever the address cap; and
 * the binner's overflow memory of its own, entered there, so that no
 * job of the process writes memory another job reads.
 */
typedef struct {
	T_V3DMMU	mmu;
	V3DPOOL		pool;
} V3DAS;

/* The job before the running one, for the log at a reset */
#define END_NONE	0
#define END_FRAME	1			/* the renderer's end */
#define END_DISPATCH	2			/* the compute dispatch's end */
typedef struct {
	UINT		kind;			/* GPU_JOB_* */
	BOOL		in_process;		/* in a process's space */
	UINT		how;			/* END_* */
	UW		bcl_start, bcl_end;
	UW		rcl_start, rcl_end;
	UINT		ovf_taken;		/* overflow pieces given to its binner */
} V3DLAST;

/* Each step of the setup names itself first, so that a stop on the board
   shows which of the writes it was */
#define V3D_STEP(s)	tm_printf((UB *)"v3d: - " s "\n")

#define HUB(r)		( V3D_HUB_BASE + (r) )
#define CORE(r)		( V3D_CORE0_BASE + (r) )

typedef struct {
	T_V3DMMU	mmu;
	UINT		ver;			/* major * 10 + minor */
	UINT		rev;
	UINT		ncores;
	UINT		nqpu;
	UINT		va_width;
	UINT		pa_width;
	UW		hub_ident[4];
	UW		core_ident[3];
	BOOL		hw_ready;		/* MMU and interrupts set up */
	BOOL		resetting;		/* the handlers leave the block alone */
	BOOL		dead;			/* the block did not come back from a reset */
	UD		scratch_pa;		/* where invalid GPU accesses go */
	V3DPOOL		kpool;			/* overflow memory of the kernel's jobs */
	V3DPOOL		*pool;			/* the running job's */
	V3DAS		*job_as;		/* its space; NULL the kernel's */
	UINT		job_seq;		/* jobs started, to tell one from the next */
	UINT		ovf_next;		/* the next piece to give */
	BOOL		ovf_wait;		/* the binner waits for a piece not made yet */
	UINT		ovf_most;		/* the most pieces one job took */
	ID		ovf_tsk;		/* makes pieces when none is left */
	INT		ovf_told;
	V3DLAST		last;			/* the job before */
	UINT		resets;
	UD		cur_pt;			/* the table in the MMU now */
	BOOL		as_allowed;		/* the check of the spaces at start passed */
	T_GPUCL		cl;			/* the running control list job */
	BOOL		cl_running;
	UINT		fldone_at;		/* flush count before the binner started */
	UINT		frdone_at;		/* frame count before the renderer started */
	UINT		phase;			/* PH_*: where the running job is */
	UW		seen_bits;		/* core interrupt bits already told of */
	BOOL		faulted;		/* the running job met an MMU fault */
	UINT		job_ints;		/* interrupts since the job started */
	UINT		job_faults;		/* faults reported since the job started */
	T_V3DSTAT	st;			/* counted for the tests */
	T_V3DJOBT	jt;			/* the jobs' times, summed */
	UD		t_kick;			/* the running job's times: started */
	UD		t_bin;			/* the binner started */
	UD		t_binend;		/* its end seen */
	UD		t_render;		/* the renderer started */
	T_GPUCSD	csd;			/* the running compute dispatch */
	BOOL		csd_running;
	UINT		csd_done_at;		/* dispatches completed before it started */
} V3D;

LOCAL V3D	v3d;
LOCAL T_GPUOPS	v3d_ops;			/* below; its features change once the checks at start pass */

/* ---------------------------------------------------------------- the firmware */

LOCAL UW	mb[16] __attribute__((aligned(64)));

/*
 * One clock tag with 'nval' value words; answers the first two words
 * the firmware wrote back.
 */
LOCAL ER fw_clock( UW tag, INT nval, UW v0, UW v1, UW v2, UW *r0, UW *r1 )
{
	ER	er;

	knl_memset(mb, 0, sizeof(mb));
	mb[0] = 9 * 4;
	mb[1] = 0;
	mb[2] = tag;
	mb[3] = 12;			/* room for three words either way */
	mb[4] = (UW)nval * 4;
	mb[5] = v0;
	mb[6] = v1;
	mb[7] = v2;
	mb[8] = 0;			/* the end tag */
	er = knl_mbox_property(mb);
	if ( er < E_OK ) {
		return er;
	}
	if ( ( mb[4] & TAG_ANSWERED ) == 0 ) {
		return E_IO;
	}
	if ( r0 != NULL ) *r0 = mb[5];
	if ( r1 != NULL ) *r1 = mb[6];
	return E_OK;
}

LOCAL ER v3d_clock_on( void )
{
	UW	id, st, max = 0, rate = 0;
	ER	er;

	er = fw_clock(TAG_GET_CLOCK_STATE, 1, CLOCK_V3D, 0, 0, &id, &st);
	if ( er < E_OK ) {
		tm_printf((UB *)"v3d: the firmware did not answer for the clock (%d)\n", er);
		return er;
	}
	if ( ( st & CLOCK_MISSING ) != 0 ) {
		tm_printf((UB *)"v3d: the firmware has no V3D clock\n");
		return E_NOEXS;
	}
	if ( ( st & CLOCK_ON ) == 0 ) {
		er = fw_clock(TAG_SET_CLOCK_STATE, 2, CLOCK_V3D, CLOCK_ON, 0, &id, &st);
		if ( er < E_OK ) {
			return er;
		}
	}
	if ( fw_clock(TAG_GET_MAX_CLOCK_RATE, 1, CLOCK_V3D, 0, 0, &id, &max) >= E_OK && max != 0 ) {
		fw_clock(TAG_SET_CLOCK_RATE, 3, CLOCK_V3D, max, 0, &id, &rate);
	}
	fw_clock(TAG_GET_CLOCK_RATE, 1, CLOCK_V3D, 0, 0, &id, &rate);
	tm_printf((UB *)"v3d: clock %s, %d Hz (max %d)\n",
		  ( st & CLOCK_ON ) ? "on" : "off", (INT)rate, (INT)max);
	return E_OK;
}

/* ---------------------------------------------------------------- what it is */

LOCAL ER v3d_identify( void )
{
	UW	id0, id1, dbg;
	INT	i;

	id0 = in_w(CORE(V3D_CTL_IDENT0));
	if ( ( id0 & V3D_IDENT0_MAGIC_MASK ) != V3D_IDENT0_MAGIC ) {
		tm_printf((UB *)"v3d: core 0 does not answer (ident0 %08x): not powered or not clocked\n", id0);
		return E_NOEXS;
	}
	id1 = in_w(CORE(V3D_CTL_IDENT1));
	v3d.core_ident[0] = id0;
	v3d.core_ident[1] = id1;
	v3d.core_ident[2] = in_w(CORE(V3D_CTL_IDENT2));
	for ( i = 0; i < 4; i++ ) {
		v3d.hub_ident[i] = in_w(HUB(V3D_HUB_IDENT0 + 4 * i));
	}
	if ( v3d.hub_ident[0] != V3D_HUB_IDENT0_MAGIC ) {
		tm_printf((UB *)"v3d: the hub does not answer (ident0 %08x)\n", v3d.hub_ident[0]);
		return E_NOEXS;
	}
	dbg = in_w(HUB(V3D_MMU_DEBUG_INFO));

	v3d.ver = ( id0 >> 24 ) * 10 + ( id1 & 0xF );
	v3d.nqpu = ( ( id1 >> 4 ) & 0xF ) * ( ( id1 >> 8 ) & 0xF );
	v3d.rev = ( v3d.hub_ident[3] >> 8 ) & 0xFF;
	v3d.ncores = V3D_HUB_IDENT1_NCORES(v3d.hub_ident[1]);
	v3d.va_width = V3D_MMU_VA_WIDTH(dbg);
	v3d.pa_width = V3D_MMU_PA_WIDTH(dbg);

	tm_printf((UB *)"v3d: V3D %d.%d rev %d, %d core(s), %d QPUs, MMU %s, VA %d bit, PA %d bit\n",
		  (INT)( v3d.ver / 10 ), (INT)( v3d.ver % 10 ), (INT)v3d.rev, (INT)v3d.ncores,
		  (INT)v3d.nqpu, ( v3d.hub_ident[2] & V3D_HUB_IDENT2_WITH_MMU ) ? "yes" : "no",
		  (INT)v3d.va_width, (INT)v3d.pa_width);
	tm_printf((UB *)"v3d: hub ident %08x %08x %08x %08x, core ident %08x %08x %08x, mmu debug %08x\n",
		  v3d.hub_ident[0], v3d.hub_ident[1], v3d.hub_ident[2], v3d.hub_ident[3],
		  v3d.core_ident[0], v3d.core_ident[1], v3d.core_ident[2], dbg);
	return E_OK;
}

/* ---------------------------------------------------------------- time */

LOCAL UD ticks( void )
{
	UD	v;

	Asm("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
	return v;
}

EXPORT void v3d_jobt_take( T_V3DJOBT *t )
{
	UINT	imask;

	ISpinLock(&v3d_lock, &imask);
	*t = v3d.jt;
	knl_memset(&v3d.jt, 0, sizeof(v3d.jt));
	ISpinUnlock(&v3d_lock, &imask);
}

/* ---------------------------------------------------------------- caches and the MMU */

LOCAL BOOL poll_clear( UBINT reg, UW bits )
{
	INT	i;

	for ( i = 0; i < POLL_MAX; i++ ) {
		if ( ( in_w(reg) & bits ) == 0 ) return TRUE;
	}
	tm_printf((UB *)"v3d: register %lx kept %08x set\n", (UD)reg, bits);
	return FALSE;
}

/* Before a list runs: nothing it reads may come from a stale line */
LOCAL void caches_invalidate( void )
{
	out_w(CORE(V3D_CTL_L2TFLSTA), 0);
	out_w(CORE(V3D_CTL_L2TFLEND), ~0U);
	out_w(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS
				     | ( V3D_L2TCACTL_FLM_FLUSH << V3D_L2TCACTL_FLM_SHIFT ));
	out_w(CORE(V3D_CTL_SLCACTL), 0x0F0F0F0FU);	/* TVCCS, TDCCS, UCC, ICC */
}

/* After a job whose results the processor reads: write them all to memory */
LOCAL void caches_clean( void )
{
	out_w(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_TMUWCF);
	poll_clear(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_TMUWCF);
	out_w(CORE(V3D_CTL_L2TFLSTA), 0);
	out_w(CORE(V3D_CTL_L2TFLEND), ~0U);
	out_w(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS
				     | ( V3D_L2TCACTL_FLM_CLEAN << V3D_L2TCACTL_FLM_SHIFT ));
	poll_clear(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS);
}

/* The table changed: the MMU's TLB and its cache forget what they held */
LOCAL void mmu_flush( void *arg )
{
	if ( !v3d.hw_ready ) {
		return;
	}
	/* the cache of table entries first, then the TLB filled from it */
	poll_clear(HUB(V3D_MMU_CTL), V3D_MMU_CTL_TLB_CLEARING);
	out_w(HUB(V3D_MMUC_CONTROL), V3D_MMUC_ENABLE | V3D_MMUC_FLUSH);
	poll_clear(HUB(V3D_MMUC_CONTROL), V3D_MMUC_FLUSHING);
	out_w(HUB(V3D_MMU_CTL), in_w(HUB(V3D_MMU_CTL)) | V3D_MMU_CTL_TLB_CLEAR);
	poll_clear(HUB(V3D_MMU_CTL), V3D_MMU_CTL_TLB_CLEARING);
}

LOCAL void mmu_regs( void );

LOCAL ER mmu_setup( void )
{
	PFRAME	*pf;
	ER	er;

	V3D_STEP("page table");
	er = v3d_mmu_init(&v3d.mmu, V3D_VA_FULL);
	if ( er < E_OK ) {
		tm_printf((UB *)"v3d: no 4MB for the page table (%d)\n", er);
		return er;
	}
	v3d.mmu.flush = mmu_flush;
	v3d.mmu.flush_arg = NULL;

	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		v3d_mmu_fini(&v3d.mmu);
		return E_NOMEM;
	}
	v3d.scratch_pa = knl_pf_to_pa(pf);
	knl_gpu_dcache_clean_inval((UBINT)PA2VA(v3d.scratch_pa), PAGE_SIZE);

	V3D_STEP("MMU registers");
	mmu_regs();
	tm_printf((UB *)"v3d: page table at %lx, invalid accesses to %lx\n",
		  v3d.mmu.pt_pa, v3d.scratch_pa);
	return E_OK;
}

/*
 * The MMU with the kernel's table, the scratch page for invalid accesses,
 * the stops and reports of what the table does not allow, and its cache
 * on; then the TLB and the cache emptied. At start and after a reset
 */
LOCAL void mmu_regs( void )
{
	out_w(HUB(V3D_MMU_PT_PA_BASE), (UW)( v3d.mmu.pt_pa >> V3D_PAGE_SHIFT ));
	v3d.cur_pt = v3d.mmu.pt_pa;
	out_w(HUB(V3D_MMU_ILLEGAL_ADDR), (UW)( v3d.scratch_pa >> V3D_PAGE_SHIFT ) | V3D_MMU_ILLEGAL_ENABLE);
	out_w(HUB(V3D_MMU_CTL), V3D_MMU_CTL_ON);
	out_w(HUB(V3D_MMUC_CONTROL), V3D_MMUC_ENABLE);
	v3d.hw_ready = TRUE;
	mmu_flush(NULL);
}

/* ---------------------------------------------------------------- jobs */

LOCAL void start_render( void )
{
	v3d.t_binend = ticks();
	if ( v3d.t_bin == 0 ) {
		v3d.t_bin = v3d.t_binend;		/* nothing was binned */
	}
	caches_invalidate();
	poll_clear(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS);
	v3d.t_render = ticks();
	v3d.phase = PH_RENDER;
	v3d.frdone_at = in_w(CORE(V3D_CLE_RFC)) & 0xFF;
	out_w(CORE(V3D_CLE_CT1QBA), v3d.cl.rcl_start);
	out_w(CORE(V3D_CLE_CT1QEA), v3d.cl.rcl_end);		/* starts it */
}

/*
 * The next overflow piece of the running job's space to the binner.
 * Called with the lock held
 */
LOCAL void ovf_give( void )
{
	out_w(CORE(V3D_PTB_BPOA), v3d.pool->chunk[v3d.ovf_next].va);
	out_w(CORE(V3D_PTB_BPOS), OVF_CHUNK_BYTES);
	v3d.ovf_next++;
	v3d.ovf_wait = FALSE;
}

LOCAL void start_bin( void )
{
	caches_invalidate();
	poll_clear(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS);
	/*
	 * The binner's spare piece is set before every list: one left from
	 * a job before is an address in that job's table, or a piece of this
	 * space that is given out again below, and the binner would write
	 * its tile lists there.
	 */
	v3d.ovf_next = 0;
	v3d.ovf_wait = FALSE;
	if ( v3d.pool != NULL && v3d.pool->n > 0 ) {
		ovf_give();
	}
	if ( v3d.cl.qma != 0 ) {
		out_w(CORE(V3D_CLE_CT0QMA), v3d.cl.qma);
		out_w(CORE(V3D_CLE_CT0QMS), v3d.cl.qms);
	}
	if ( v3d.cl.qts != 0 ) {
		out_w(CORE(V3D_CLE_CT0QTS), V3D_CLE_CT0QTS_ENABLE | v3d.cl.qts);
	}
	v3d.t_bin = ticks();
	v3d.phase = PH_BIN;
	v3d.fldone_at = in_w(CORE(V3D_CLE_BFC)) & 0xFF;
	out_w(CORE(V3D_CLE_CT0QBA), v3d.cl.bcl_start);
	out_w(CORE(V3D_CLE_CT0QEA), v3d.cl.bcl_end);		/* starts it */
}

/*
 * The first jobs' threads, told on the console: their state before the
 * lists are given, and a little after, whether the binner runs.
 */
#define KICK_TOLD	8
LOCAL INT	kicks_told = 0;

LOCAL void kick_tell( CONST char *when )
{
	tm_printf((UB *)"v3d: %s job: ct0cs %08x ct0ca %08x ct1cs %08x ct1ca %08x bfc %02x rfc %02x int %08x, "
		  "bpca %08x bpcs %08x bpoa %08x bpos %08x\n",
		  when, in_w(CORE(V3D_CLE_CT0CS)), in_w(CORE(V3D_CLE_CT0CA)), in_w(CORE(V3D_CLE_CT1CS)),
		  in_w(CORE(V3D_CLE_CT1CA)), in_w(CORE(V3D_CLE_BFC)) & 0xFF, in_w(CORE(V3D_CLE_RFC)) & 0xFF,
		  in_w(CORE(V3D_CTL_INT_STS)), in_w(CORE(V3D_PTB_BPCA)), in_w(CORE(V3D_PTB_BPCS)),
		  in_w(CORE(V3D_PTB_BPOA)), in_w(CORE(V3D_PTB_BPOS)));
}

/*
 * A compute dispatch starts. The code, its uniforms and the buffers it
 * reads may have been written by the processor or by the job before:
 * the GPU's caches drop what they hold first. The configuration is
 * written from CFG1 on, CFG7 cleared (the dispatch takes no eighth
 * word), and CFG0 last, since writing it queues the dispatch. The count
 * of completed dispatches is taken before, so that the watch sees the
 * end without the interrupt. Called with the lock held.
 */
LOCAL void start_csd( void )
{
	INT	i;

	caches_invalidate();
	poll_clear(CORE(V3D_CTL_L2TCACTL), V3D_L2TCACTL_L2TFLS);
	v3d.csd_done_at = ( in_w(CORE(V3D_CSD_STATUS)) >> V3D_CSD_STATUS_DONE_SHIFT ) & 0xFF;
	for ( i = 1; i <= 6; i++ ) {
		out_w(CORE(V3D_CSD_QUEUED_CFG0_71 + 4 * i), v3d.csd.cfg[i]);
	}
	out_w(CORE(V3D_CSD_QUEUED_CFG7_71), 0);
	out_w(CORE(V3D_CSD_QUEUED_CFG0_71), v3d.csd.cfg[0]);	/* starts it */
}

/*
 * The MMU given the table of the job about to run, when it is not the
 * one there: what the caches hold of the job before is written out
 * first, through the table it was made with, and then the cached
 * entries and the TLB are dropped. Only between jobs, with both control
 * list threads stopped.
 */
LOCAL void table_for( V3DAS *as )
{
	UD	pt = ( as != NULL ) ? as->mmu.pt_pa : v3d.mmu.pt_pa;

	if ( pt == v3d.cur_pt ) {
		return;
	}
	caches_clean();
	out_w(HUB(V3D_MMU_PT_PA_BASE), (UW)( pt >> V3D_PAGE_SHIFT ));
	v3d.cur_pt = pt;
	mmu_flush(NULL);
}

LOCAL ER v3d_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	if ( as != NULL && !v3d.as_allowed ) {
		return E_NOSPT;			/* never in the kernel's table instead */
	}
	if ( v3d.dead ) {
		return E_IO;			/* the block did not come back from a reset */
	}
	if ( job->kind == GPU_JOB_CL || job->kind == GPU_JOB_CACHE || job->kind == GPU_JOB_CSD ) {
		table_for((V3DAS *)as);
	}
	switch ( job->kind ) {
	case GPU_JOB_CL: {
		UINT	imask;

		BOOL	tell = ( kicks_told < KICK_TOLD );

		if ( tell ) {
			kick_tell("before the");
		}
		/*
		 * Both threads from a known state. A thread that still runs a
		 * list, such as a binner waiting for memory, is not stopped by
		 * this bit: only the reset of the block does that (v3d_reset).
		 */
		out_w(CORE(V3D_CLE_CT0CS), V3D_CLE_CTCS_CTRSTA);
		out_w(CORE(V3D_CLE_CT1CS), V3D_CLE_CTCS_CTRSTA);
		out_w(CORE(V3D_CTL_INT_CLR), ~0U);	/* no end of the job before left set */
		ISpinLock(&v3d_lock, &imask);
		v3d.t_kick = ticks();
		v3d.t_bin = 0;
		v3d.cl = job->u.cl;
		v3d.job_as = (V3DAS *)as;
		v3d.pool = ( as != NULL ) ? &((V3DAS *)as)->pool : &v3d.kpool;
		v3d.job_seq++;
		v3d.cl_running = TRUE;
		v3d.faulted = FALSE;
		v3d.job_ints = 0;
		v3d.job_faults = 0;
		if ( v3d.cl.bcl_start == v3d.cl.bcl_end ) {
			start_render();			/* a list with nothing to bin */
		} else {
			start_bin();
		}
		ISpinUnlock(&v3d_lock, &imask);
		if ( tell ) {
			kicks_told++;
			tk_dly_tsk(1);
			kick_tell("1ms into the");
		}
		return E_OK;
	}

	case GPU_JOB_CACHE:
		caches_clean();				/* polled: no interrupt tells its end */
		knl_gpu_job_end(E_OK);
		return E_OK;

	case GPU_JOB_CSD: {
		UINT	imask;

		/* no end of a dispatch before left set */
		out_w(CORE(V3D_CTL_INT_CLR), V3D_INT_CSDDONE_71);
		ISpinLock(&v3d_lock, &imask);
		v3d.t_kick = ticks();
		v3d.csd = job->u.csd;
		v3d.job_as = (V3DAS *)as;
		v3d.pool = NULL;
		v3d.job_seq++;
		v3d.csd_running = TRUE;
		v3d.faulted = FALSE;
		v3d.job_ints = 0;
		v3d.job_faults = 0;
		start_csd();
		ISpinUnlock(&v3d_lock, &imask);
		return E_OK;
	}

	default:
		return E_NOSPT;				/* TFU waits for its registers (13.8) */
	}
}

LOCAL void irq_regs( void );
LOCAL void mmu_regs( void );

LOCAL CONST char *last_how( UINT how )
{
	switch ( how ) {
	case END_FRAME:		return "the renderer's end";
	case END_DISPATCH:	return "the dispatch's end";
	default:		return "none";
	}
}

/* The block's state, before and after the reset */
LOCAL void reset_tell( CONST char *when )
{
	tm_printf((UB *)"v3d: %s the reset: ct0cs %08x ct1cs %08x ct0ca %08x ct1ca %08x, bfc %02x rfc %02x, "
		  "core int %08x hub int %08x, err %08x, bpca %08x bpcs %08x bpoa %08x bpos %08x, "
		  "sms %08x, pm %08x, ident %08x\n",
		  when, in_w(CORE(V3D_CLE_CT0CS)), in_w(CORE(V3D_CLE_CT1CS)),
		  in_w(CORE(V3D_CLE_CT0CA)), in_w(CORE(V3D_CLE_CT1CA)),
		  in_w(CORE(V3D_CLE_BFC)) & 0xFF, in_w(CORE(V3D_CLE_RFC)) & 0xFF,
		  in_w(CORE(V3D_CTL_INT_STS)), in_w(HUB(V3D_HUB_INT_STS)), in_w(CORE(V3D_ERR_STAT)),
		  in_w(CORE(V3D_PTB_BPCA)), in_w(CORE(V3D_PTB_BPCS)),
		  in_w(CORE(V3D_PTB_BPOA)), in_w(CORE(V3D_PTB_BPOS)),
		  in_w(SMS(V3D_SMS_REE_CS)), in_w(PM(PM_GRAFX_2712)), in_w(CORE(V3D_CTL_IDENT0)));
}

/*
 * The power state manager resets the hub and the core: the command is
 * written and the state is waited for to leave "isolating for reset"
 * and "resetting". FALSE when it stays in one of them.
 */
LOCAL BOOL reset_sms( void )
{
	UINT	us, st;

	out_w(SMS(V3D_SMS_REE_CS), V3D_SMS_CMD_RESET);
	for ( us = 0; us < SMS_WAIT_US; us += 10 ) {
		st = in_w(SMS(V3D_SMS_REE_CS)) & V3D_SMS_STATE_MASK;
		if ( st != V3D_SMS_ISOLATING_FOR_RESET && st != V3D_SMS_RESETTING ) {
			return TRUE;
		}
		WaitUsec(10);
	}
	return FALSE;
}

/*
 * The block's reset line in the PM block held low and let go. The V3D's
 * clock stays on (the driver holds it), so the reset reaches every
 * register of the hub and the core.
 */
LOCAL void reset_line( void )
{
	UW	v = in_w(PM(PM_GRAFX_2712)) & 0x00FFFFFFU;

	out_w(PM(PM_GRAFX_2712), PM_PASSWORD | ( v & ~PM_V3DRSTN ));
	WaitUsec(RESET_HOLD_US);
	out_w(PM(PM_GRAFX_2712), PM_PASSWORD | v | PM_V3DRSTN);
	WaitUsec(RESET_HOLD_US);
}

/*
 * A job did not end in the common layer's time. A thread of the control
 * list executor that is still running (a binner that waits for memory,
 * a list that does not end) is not stopped by its own reset bit, so the
 * whole block is reset: the interrupts masked and the handlers kept
 * off it, the power state manager's reset, the block's reset line, and
 * then everything the driver set at start written again (the caches'
 * range, the MMU with the kernel's table, the interrupts). The state
 * before and after, and the job before the one that did not end, are
 * told on the console.
 */
LOCAL void v3d_reset( void *priv )
{
	UINT	imask;
	BOOL	sms_ok;

	tm_printf((UB *)"v3d: at the reset: phase %d, core int %08x mask %08x, hub int %08x, "
		  "ct0cs %08x ct1cs %08x, ct0ca %08x ct1ca %08x, bfc %02x (from %02x) rfc %02x (from %02x)\n",
		  (INT)v3d.phase, in_w(CORE(V3D_CTL_INT_STS)), in_w(CORE(V3D_CTL_INT_MSK_STS)),
		  in_w(HUB(V3D_HUB_INT_STS)), in_w(CORE(V3D_CLE_CT0CS)), in_w(CORE(V3D_CLE_CT1CS)),
		  in_w(CORE(V3D_CLE_CT0CA)), in_w(CORE(V3D_CLE_CT1CA)),
		  in_w(CORE(V3D_CLE_BFC)) & 0xFF, v3d.fldone_at, in_w(CORE(V3D_CLE_RFC)) & 0xFF, v3d.frdone_at);
	if ( v3d.cl_running ) {
		/*
		 * Where the job's lists were, and the first words of the binner's
		 * list as memory holds them: whether the GPU ran what was written
		 */
		T_V3DMMU *m = ( v3d.job_as != NULL ) ? &v3d.job_as->mmu : &v3d.mmu;
		UW	pte = v3d_mmu_lookup(m, v3d.cl.bcl_start);
		UD	pa = ( (UD)( pte & V3D_PTE_PFN_MASK ) << V3D_PAGE_SHIFT )
			     + ( v3d.cl.bcl_start & ( ( 1U << V3D_PAGE_SHIFT ) - 1 ) );
		UW	w[4] = { 0, 0, 0, 0 };

		if ( ( pte & V3D_PTE_VALID ) != 0 ) {
			knl_gpu_dcache_clean_inval((UBINT)PA2VA(pa), sizeof(w));
			knl_memcpy(w, (CONST void *)PA2VA(pa), sizeof(w));
		}
		tm_printf((UB *)"v3d: the job: in %s space, bin %08x-%08x render %08x-%08x tile memory %08x+%x state %08x; "
			  "overflow pieces given %d of %d (waiting %d); bin list at %lx: %08x %08x %08x %08x\n",
			  ( v3d.job_as != NULL ) ? "a process's" : "the kernel's",
			  v3d.cl.bcl_start, v3d.cl.bcl_end, v3d.cl.rcl_start, v3d.cl.rcl_end,
			  v3d.cl.qma, v3d.cl.qms, v3d.cl.qts,
			  (INT)v3d.ovf_next, ( v3d.pool != NULL ) ? (INT)v3d.pool->n : 0, (INT)v3d.ovf_wait,
			  pa, w[0], w[1], w[2], w[3]);
	}
	if ( v3d.csd_running ) {
		tm_printf((UB *)"v3d: the dispatch at the reset: status %08x, cfg %08x %08x %08x %08x %08x %08x %08x\n",
			  in_w(CORE(V3D_CSD_STATUS)), v3d.csd.cfg[0], v3d.csd.cfg[1], v3d.csd.cfg[2],
			  v3d.csd.cfg[3], v3d.csd.cfg[4], v3d.csd.cfg[5], v3d.csd.cfg[6]);
	}
	tm_printf((UB *)"v3d: the job before: kind %d in %s space, ended by %s, bin %08x-%08x render %08x-%08x, "
		  "overflow pieces given %d\n",
		  (INT)v3d.last.kind, v3d.last.in_process ? "a process's" : "the kernel's", last_how(v3d.last.how),
		  v3d.last.bcl_start, v3d.last.bcl_end, v3d.last.rcl_start, v3d.last.rcl_end,
		  (INT)v3d.last.ovf_taken);
	reset_tell("before");

	/* the handlers and the watch leave the block alone from here */
	ISpinLock(&v3d_lock, &imask);
	v3d.resetting = TRUE;
	v3d.cl_running = FALSE;
	v3d.csd_running = FALSE;
	v3d.phase = PH_IDLE;
	v3d.pool = NULL;
	v3d.ovf_wait = FALSE;
	v3d.last.how = END_NONE;
	ISpinUnlock(&v3d_lock, &imask);
	out_w(CORE(V3D_CTL_INT_MSK_SET), ~0U);
	out_w(HUB(V3D_HUB_INT_MSK_SET), ~0U);
	out_w(CORE(V3D_CTL_INT_CLR), ~0U);
	out_w(HUB(V3D_HUB_INT_CLR), ~0U);

	sms_ok = reset_sms();
	reset_line();

	if ( ( in_w(CORE(V3D_CTL_IDENT0)) & V3D_IDENT0_MAGIC_MASK ) != V3D_IDENT0_MAGIC ) {
		reset_tell("after");
		tm_printf((UB *)"v3d: the block does not answer after the reset; no more jobs run on it\n");
		v3d.dead = TRUE;
		return;
	}

	/* what the driver set at start */
	out_w(CORE(V3D_CTL_L2TFLSTA), 0);
	out_w(CORE(V3D_CTL_L2TFLEND), ~0U);
	mmu_regs();
	irq_regs();
	ClearInt(V3D_CORE0_INTNO);
	ClearInt(V3D_HUB_INTNO);
	EnableInt(V3D_CORE0_INTNO, INTPRI_DEVICE);	/* again, when a storm turned it off */
	EnableInt(V3D_HUB_INTNO, INTPRI_DEVICE);

	ISpinLock(&v3d_lock, &imask);
	v3d.resetting = FALSE;
	v3d.resets++;
	ISpinUnlock(&v3d_lock, &imask);
	tm_printf((UB *)"v3d: reset %d: the power state manager %s\n",
		  (INT)v3d.resets, sms_ok ? "came back" : "stayed in its reset");
	reset_tell("after");
}

/* ---------------------------------------------------------------- the job's steps */

/* The job that ends kept for the log of a reset after it. Called with the lock held */
LOCAL void last_note( UINT kind, UINT how )
{
	v3d.last.kind = kind;
	v3d.last.in_process = ( v3d.job_as != NULL );
	v3d.last.how = how;
	v3d.last.bcl_start = ( kind == GPU_JOB_CL ) ? v3d.cl.bcl_start : 0;
	v3d.last.bcl_end = ( kind == GPU_JOB_CL ) ? v3d.cl.bcl_end : 0;
	v3d.last.rcl_start = ( kind == GPU_JOB_CL ) ? v3d.cl.rcl_start : 0;
	v3d.last.rcl_end = ( kind == GPU_JOB_CL ) ? v3d.cl.rcl_end : 0;
	v3d.last.ovf_taken = ( kind == GPU_JOB_CL ) ? v3d.ovf_next : 0;
}

/*
 * The most overflow pieces one job's binner took, told when it grows
 * past what a space starts with: how much the lists of a frame need
 */
LOCAL void ovf_most_tell( void )
{
	if ( v3d.ovf_next <= v3d.ovf_most ) {
		return;
	}
	v3d.ovf_most = v3d.ovf_next;
	if ( v3d.ovf_most > OVF_CHUNKS_FIRST && v3d.ovf_told < OVF_TOLD ) {
		v3d.ovf_told++;
		tm_printf((UB *)"v3d: a job's binner took %d overflow pieces (%d KB), the most so far, in %s space\n",
			  (INT)v3d.ovf_most, (INT)( v3d.ovf_most * ( OVF_CHUNK_BYTES / 1024 ) ),
			  ( v3d.job_as != NULL ) ? "a process's" : "the kernel's");
	}
}

/* The renderer is done: the job ends. Called with the lock held */
LOCAL void frame_done( void )
{
	UD	t_end, t_done = ticks();

	v3d.cl_running = FALSE;
	v3d.phase = PH_IDLE;
	ovf_most_tell();
	last_note(GPU_JOB_CL, END_FRAME);
	v3d.pool = NULL;
	v3d.ovf_wait = FALSE;
	if ( ( v3d.cl.flags & GPU_CL_FLUSH_CACHE ) != 0 ) {
		caches_clean();
	}
	t_end = ticks();
	v3d.jt.njob++;
	if ( v3d.jt.first_kick == 0 ) {
		v3d.jt.first_kick = v3d.t_kick;
	}
	v3d.jt.last_end = t_end;
	v3d.jt.setup += v3d.t_bin - v3d.t_kick;
	v3d.jt.bin += v3d.t_binend - v3d.t_bin;
	v3d.jt.mid += v3d.t_render - v3d.t_binend;
	v3d.jt.render += t_done - v3d.t_render;
	v3d.jt.clean += t_end - t_done;
	knl_gpu_job_end(v3d.faulted ? E_IO : E_OK);
}

/*
 * The compute dispatch is done: what its shaders wrote through the TMU
 * is written out of the write combiner and the L2T to memory, for the
 * processor and the jobs after it to read, and the job ends. Called
 * with the lock held
 */
LOCAL void csd_done( void )
{
	UD	t_done = ticks();

	v3d.csd_running = FALSE;
	last_note(GPU_JOB_CSD, END_DISPATCH);
	caches_clean();
	v3d.st.csd_jobs++;
	v3d.jt.njob++;
	if ( v3d.jt.first_kick == 0 ) {
		v3d.jt.first_kick = v3d.t_kick;
	}
	v3d.jt.last_end = ticks();
	v3d.jt.render += t_done - v3d.t_kick;
	knl_gpu_job_end(v3d.faulted ? E_IO : E_OK);
}

/*
 * The binner took its spare piece of overflow memory and asks for the
 * next one. The job does not end here: until it has memory the binner
 * waits, and its flush, and so the job's end, come only after. When no
 * piece is made yet, the overflow task makes one and gives it; it also
 * makes the next one ahead, so that the binner seldom waits. A space at
 * its limit gives no more, and the job is left to the time limit and
 * the reset. Called with the lock held
 */
LOCAL void ovf_oom( void )
{
	V3DPOOL	*p = v3d.pool;

	v3d.st.outomem++;
	if ( !v3d.cl_running || v3d.phase != PH_BIN || p == NULL ) {
		return;				/* the binner ended meanwhile */
	}
	if ( v3d.ovf_next < p->n ) {
		ovf_give();
	} else {
		v3d.ovf_wait = TRUE;
		v3d.st.ovf_waits++;
	}
	if ( v3d.ovf_next >= p->n ) {
		if ( p->n < OVF_CHUNKS_MAX ) {
			if ( v3d.ovf_tsk > 0 ) {
				tk_wup_tsk(v3d.ovf_tsk);
			}
		} else if ( v3d.ovf_wait && !p->told_max ) {
			p->told_max = TRUE;
			tm_printf((UB *)"v3d: a job's binner wants more than %d KB of overflow memory; "
				  "it waits for the time limit\n", (INT)( OVF_CHUNKS_MAX * ( OVF_CHUNK_BYTES / 1024 ) ));
		}
	}
}

/*
 * The counters of flushes and frames tell the ends too, whatever the
 * interrupt bits are. Called with the lock held
 */
LOCAL void counters_check( void )
{
	if ( v3d.resetting ) {
		return;
	}
	if ( v3d.csd_running &&
	     ( ( in_w(CORE(V3D_CSD_STATUS)) >> V3D_CSD_STATUS_DONE_SHIFT ) & 0xFF ) != v3d.csd_done_at ) {
		csd_done();
		return;
	}
	if ( !v3d.cl_running ) {
		return;
	}
	/* out of tile memory with no interrupt told: the watch gives it too */
	if ( v3d.phase == PH_BIN && ( in_w(CORE(V3D_CTL_INT_STS)) & V3D_INT_OUTOMEM ) != 0 ) {
		out_w(CORE(V3D_CTL_INT_CLR), V3D_INT_OUTOMEM);
		ovf_oom();
	}
	if ( v3d.phase == PH_BIN && ( in_w(CORE(V3D_CLE_BFC)) & 0xFF ) != v3d.fldone_at ) {
		v3d.st.fldone_count++;
		start_render();
	} else if ( v3d.phase == PH_RENDER && ( in_w(CORE(V3D_CLE_RFC)) & 0xFF ) != v3d.frdone_at ) {
		v3d.st.frdone_count++;
		frame_done();
	}
}

LOCAL void v3d_poll( void *exinf )
{
	UINT	imask;

	ISpinLock(&v3d_lock, &imask);
	counters_check();
	ISpinUnlock(&v3d_lock, &imask);
}

/* ---------------------------------------------------------------- overflow memory */

/*
 * One piece more for a space: made, entered in the space's table (the
 * kernel's for the kernel's pool), and counted under the lock only then,
 * so that the interrupt handler never gives a piece not yet entered.
 * From a task, not from a handler.
 */
LOCAL ER pool_grow( V3DPOOL *p, V3DAS *as )
{
	T_GPUBO		*bo;
	T_GPUBOINFO	bi;
	UW		va;
	UINT		imask;
	ER		er;

	if ( p->chunk == NULL ) {
		return E_NOMEM;
	}
	if ( p->n >= OVF_CHUNKS_MAX ) {
		return E_LIMIT;
	}
	bo = knl_gpu_bo_create(OVF_CHUNK_BYTES, ( as != NULL ) ? GPU_BO_NOGPU : 0, &er);
	if ( bo == NULL ) {
		return er;
	}
	if ( as != NULL ) {
		er = v3d_mmu_alloc(&as->mmu, OVF_CHUNK_BYTES >> PAGE_SHIFT, 1, &va);
		if ( er < E_OK ) {
			knl_gpu_bo_put(bo);
			return er;
		}
		v3d_mmu_insert(&as->mmu, va, knl_gpu_bo_pages(bo), OVF_CHUNK_BYTES >> PAGE_SHIFT, TRUE);
	} else {
		knl_gpu_bo_ref(bo, &bi);
		va = bi.offset;
	}
	ISpinLock(&v3d_lock, &imask);
	p->chunk[p->n].bo = bo;
	p->chunk[p->n].va = va;
	p->n++;
	v3d.st.ovf_made++;
	ISpinUnlock(&v3d_lock, &imask);
	return E_OK;
}

LOCAL ER pool_init( V3DPOOL *p, V3DAS *as )
{
	INT	i;
	ER	er;

	knl_memset(p, 0, sizeof(*p));
	p->chunk = (V3DOVF *)Kmalloc(OVF_CHUNKS_MAX * sizeof(V3DOVF));
	if ( p->chunk == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < OVF_CHUNKS_FIRST; i++ ) {
		er = pool_grow(p, as);
		if ( er < E_OK ) {
			return er;
		}
	}
	return E_OK;
}

/* The pieces put back; the space's own entries go with its table */
LOCAL void pool_fini( V3DPOOL *p, V3DAS *as )
{
	UINT	i;

	if ( p->chunk == NULL ) {
		return;
	}
	for ( i = 0; i < p->n; i++ ) {
		if ( as != NULL ) {
			v3d_mmu_remove(&as->mmu, p->chunk[i].va, OVF_CHUNK_BYTES >> PAGE_SHIFT);
			v3d_mmu_free(&as->mmu, p->chunk[i].va, OVF_CHUNK_BYTES >> PAGE_SHIFT);
		}
		knl_gpu_bo_put(p->chunk[i].bo);
	}
	Kfree(p->chunk);
	knl_memset(p, 0, sizeof(*p));
}

/*
 * The overflow task: woken by the handler when the running job's space
 * has no piece left to give. It makes one for the space of the job that
 * still runs (the job's number tells it is the same), gives it when the
 * binner waits, and goes on while the binner has none ahead. The space
 * is marked while a piece is entered, and is not put away meanwhile
 * (v3d_as_destroy waits).
 */
LOCAL void ovf_task( INT stacd, void *exinf )
{
	for (;;) {
		tk_slp_tsk(TMO_FEVR);
		for (;;) {
			V3DPOOL	*p;
			V3DAS	*as;
			UINT	seq, imask;
			ER	er;

			ISpinLock(&v3d_lock, &imask);
			p = v3d.pool;
			if ( !v3d.cl_running || v3d.phase != PH_BIN || p == NULL
			     || p->n >= OVF_CHUNKS_MAX || v3d.ovf_next < p->n ) {
				ISpinUnlock(&v3d_lock, &imask);
				break;
			}
			as = v3d.job_as;
			seq = v3d.job_seq;
			p->growing = TRUE;
			ISpinUnlock(&v3d_lock, &imask);

			er = pool_grow(p, as);

			ISpinLock(&v3d_lock, &imask);
			p->growing = FALSE;
			if ( er >= E_OK && v3d.cl_running && v3d.job_seq == seq && v3d.ovf_wait
			     && v3d.ovf_next < p->n ) {
				ovf_give();
			}
			ISpinUnlock(&v3d_lock, &imask);
			if ( er < E_OK ) {
				if ( v3d.ovf_told < OVF_TOLD ) {
					v3d.ovf_told++;
					tm_printf((UB *)"v3d: no overflow piece made (%d); the binner waits\n", (INT)er);
				}
				break;
			}
			if ( v3d.ovf_told < OVF_TOLD ) {
				v3d.ovf_told++;
				tm_printf((UB *)"v3d: overflow memory of %s space grows to %d KB\n",
					  ( as != NULL ) ? "a process's" : "the kernel's",
					  (INT)( p->n * ( OVF_CHUNK_BYTES / 1024 ) ));
			}
		}
	}
}

LOCAL void ovf_task_start( void )
{
	T_CTSK	ctsk;

	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)ovf_task;
	ctsk.itskpri = OVF_TASK_PRI;
	ctsk.stksz = OVF_TASK_STKSZ;
	v3d.ovf_tsk = tk_cre_tsk(&ctsk);
	if ( v3d.ovf_tsk <= 0 || tk_sta_tsk(v3d.ovf_tsk, 0) < E_OK ) {
		v3d.ovf_tsk = 0;
		tm_printf((UB *)"v3d: no overflow task; a space keeps the overflow memory it starts with\n");
	}
}

/* ---------------------------------------------------------------- interrupts */

/*
 * Too many interrupts for one job: the line stays up; turn it off. The
 * job is not ended here, as the hardware may still run it: the watch of
 * the counters sees its end, or the time limit resets the block, which
 * turns the line on again.
 */
LOCAL BOOL storm( UINT intno )
{
	if ( ++v3d.job_ints <= JOB_INTS_MAX ) {
		return FALSE;
	}
	DisableInt(intno);
	v3d.st.storms++;
	tm_printf((UB *)"v3d: interrupt %d keeps coming; turned off\n", (INT)intno);
	return TRUE;
}

LOCAL void v3d_core_inthdr( UINT intno )
{
	UW	sts = in_w(CORE(V3D_CTL_INT_STS));
	UW	news;
	UINT	imask;

	out_w(CORE(V3D_CTL_INT_CLR), sts);
	ISpinLock(&v3d_lock, &imask);
	v3d.st.core_ints++;
	v3d.st.core_bits |= sts;
	news = sts & ~CORE_INTS_KNOWN & ~v3d.seen_bits;
	v3d.seen_bits |= news;
	if ( news != 0 ) {
		tm_printf((UB *)"v3d: core interrupt %08x (new bits %08x) in phase %d, bfc %02x rfc %02x\n",
			  sts, news, (INT)v3d.phase, in_w(CORE(V3D_CLE_BFC)) & 0xFF, in_w(CORE(V3D_CLE_RFC)) & 0xFF);
	}
	if ( v3d.resetting || storm(intno) ) {
		ISpinUnlock(&v3d_lock, &imask);
		EndOfInt(intno);
		return;
	}

	if ( ( sts & V3D_INT_OUTOMEM ) != 0 ) {
		ovf_oom();
	}
	if ( ( sts & V3D_INT_FLDONE ) != 0 ) {
		v3d.st.fldone++;
		if ( v3d.cl_running && v3d.phase == PH_BIN ) {
			start_render();
		}
	}
	if ( ( sts & V3D_INT_FRDONE ) != 0 ) {
		v3d.st.frdone++;
		if ( v3d.cl_running && v3d.phase == PH_RENDER ) {
			frame_done();
		}
	}
	if ( ( sts & V3D_INT_CSDDONE_71 ) != 0 ) {
		v3d.st.csddone++;
		if ( v3d.csd_running ) {
			csd_done();
		}
	}
	counters_check();
	ISpinUnlock(&v3d_lock, &imask);
	EndOfInt(intno);		/* the GIC takes the next only after this */
}

LOCAL void v3d_hub_inthdr( UINT intno )
{
	UW	sts = in_w(HUB(V3D_HUB_INT_STS));

	out_w(HUB(V3D_HUB_INT_CLR), sts);
	v3d.st.hub_ints++;
	if ( v3d.resetting || storm(intno) ) {
		EndOfInt(intno);
		return;
	}

	/*
	 * The access went to the scratch page, so the job runs on to its
	 * end, which then reports the failure. A job that stops instead
	 * is caught by the common layer's time limit.
	 */
	if ( ( sts & ( V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_CAP ) ) != 0 ) {
		v3d.st.faults++;
		v3d.st.fault_sts = sts;
		v3d.st.fault_addr = in_w(HUB(V3D_MMU_VIO_ADDR));
		v3d.st.fault_id = in_w(HUB(V3D_MMU_VIO_ID));
		v3d.faulted = TRUE;
		if ( ++v3d.job_faults <= JOB_FAULTS_SHOWN ) {
			tm_printf((UB *)"v3d: MMU fault %08x at %08x (client %08x)\n", sts,
				  v3d.st.fault_addr, v3d.st.fault_id);
		}
	}
	EndOfInt(intno);
}

EXPORT void v3d_stat( T_V3DSTAT *st )
{
	*st = v3d.st;
	st->scratch_pa = v3d.scratch_pa;
	st->resets = v3d.resets;
	st->ovf_most = v3d.ovf_most;
}

LOCAL void irq_regs( void )
{
	UW	hub = V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_CAP;

	/* every core bit: the ones not known are told of (CORE_INTS_KNOWN) */
	out_w(CORE(V3D_CTL_INT_MSK_CLR), ~0U);
	out_w(CORE(V3D_CTL_INT_CLR), ~0U);
	out_w(HUB(V3D_HUB_INT_MSK_SET), ~hub);
	out_w(HUB(V3D_HUB_INT_MSK_CLR), hub);
	out_w(HUB(V3D_HUB_INT_CLR), ~0U);
}

LOCAL void irq_setup( void )
{
	T_CCYC	cc;

	irq_regs();

	knl_define_inthdr(V3D_CORE0_INTNO, TA_HLNG, (FP)v3d_core_inthdr);
	knl_define_inthdr(V3D_HUB_INTNO, TA_HLNG, (FP)v3d_hub_inthdr);
	SetIntMode(V3D_CORE0_INTNO, IM_LEVEL);
	SetIntMode(V3D_HUB_INTNO, IM_LEVEL);
	EnableInt(V3D_CORE0_INTNO, INTPRI_DEVICE);
	EnableInt(V3D_HUB_INTNO, INTPRI_DEVICE);

	knl_memset(&cc, 0, sizeof(cc));
	cc.cycatr = TA_HLNG | TA_STA;
	cc.cychdr = (FP)v3d_poll;
	cc.cyctim = POLL_CYC_MS;
	if ( tk_cre_cyc(&cc) < E_OK ) {
		tm_printf((UB *)"v3d: no cyclic handler to watch the counters\n");
	}
}

/* ---------------------------------------------------------------- the backend */

LOCAL ER v3d_info( void *priv, T_GPUINFO *info )
{
	INT	i;

	info->kind = GPU_KIND_V3D;
	info->ver = v3d.ver;
	info->rev = v3d.rev;
	info->ncores = v3d.ncores;
	info->nqpu = v3d.nqpu;
	info->feature = v3d_ops.feature;
	info->va_size = v3d.mmu.va_size;
	for ( i = 0; i < 4; i++ ) info->hub_ident[i] = v3d.hub_ident[i];
	for ( i = 0; i < 3; i++ ) info->core_ident[i] = v3d.core_ident[i];
	return E_OK;
}

LOCAL ER v3d_bo_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset )
{
	UW	va;
	ER	er;

	er = v3d_mmu_alloc(&v3d.mmu, npages, 1, &va);
	if ( er < E_OK ) {
		return er;
	}
	v3d_mmu_insert(&v3d.mmu, va, knl_gpu_bo_pages(bo), npages, TRUE);
	*p_offset = va;
	return E_OK;
}

LOCAL void v3d_bo_unbind( void *priv, UW offset, UD npages )
{
	v3d_mmu_remove(&v3d.mmu, offset, npages);
	v3d_mmu_free(&v3d.mmu, offset, npages);
}

/* ---------------------------------------------------------------- address spaces */

LOCAL ER v3d_as_bind( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va )
{
	V3DAS	*a = (V3DAS *)as;
	ER	er = v3d_mmu_alloc(&a->mmu, npages, 1, p_va);

	if ( er < E_OK ) {
		return er;
	}
	v3d_mmu_insert(&a->mmu, *p_va, knl_gpu_bo_pages(bo), npages, TRUE);
	return E_OK;
}

LOCAL void v3d_as_unbind( void *priv, void *as, UW va, UD npages )
{
	V3DAS	*a = (V3DAS *)as;

	v3d_mmu_remove(&a->mmu, va, npages);
	v3d_mmu_free(&a->mmu, va, npages);
}

/*
 * The spaces alive, and the refusals of one told on the console (the
 * first AS_TOLD of them): a process that gets none draws on softpipe, and
 * the line says why on the board's log
 */
#define AS_TOLD		8
LOCAL INT	as_live = 0;
LOCAL INT	as_refused = 0;

LOCAL void as_tell( CONST char *why, ER er )
{
	if ( as_refused < AS_TOLD ) {
		tm_printf((UB *)"v3d: a process gets no address space (%s, %d): %d alive; it draws on softpipe\n",
			  why, (INT)er, as_live);
	}
	(void)__atomic_add_fetch(&as_refused, 1, __ATOMIC_RELAXED);
}

LOCAL void v3d_as_destroy( void *priv, void *as )
{
	V3DAS	*a = (V3DAS *)as;
	UINT	imask;
	BOOL	growing;

	/* the overflow task may still be entering a piece made for its last job */
	for (;;) {
		ISpinLock(&v3d_lock, &imask);
		growing = a->pool.growing;
		ISpinUnlock(&v3d_lock, &imask);
		if ( !growing ) break;
		tk_dly_tsk(1);
	}
	pool_fini(&a->pool, a);
	v3d_mmu_fini(&a->mmu);
	Kfree(a);
	(void)__atomic_sub_fetch(&as_live, 1, __ATOMIC_RELAXED);
}

/*
 * A table of 4MB (every 32 bit address) with nothing in it but its
 * overflow memory. E_OBJ when the check at start did not pass: then a
 * process gets no space, and no job of a process runs on the V3D.
 */
LOCAL ER v3d_as_create( void *priv, void **p_as )
{
	V3DAS	*a;
	ER	er;

	if ( !v3d.as_allowed ) {
		as_tell("the check of the spaces at start did not pass", E_OBJ);
		return E_OBJ;
	}
	a = (V3DAS *)Kmalloc(sizeof(V3DAS));
	if ( a == NULL ) {
		as_tell("no memory", E_NOMEM);
		return E_NOMEM;
	}
	knl_memset(a, 0, sizeof(*a));
	er = v3d_mmu_init(&a->mmu, V3D_VA_FULL);
	if ( er < E_OK ) {
		Kfree(a);
		as_tell("no 4MB of pages in a row below 4GB for its table", er);
		return er;
	}
	a->mmu.flush = mmu_flush;
	a->mmu.flush_arg = NULL;
	if ( pool_init(&a->pool, a) < E_OK ) {
		pool_fini(&a->pool, a);
		v3d_mmu_fini(&a->mmu);
		Kfree(a);
		as_tell("no overflow memory", E_NOMEM);
		return E_NOMEM;
	}
	(void)__atomic_add_fetch(&as_live, 1, __ATOMIC_RELAXED);
	*p_as = a;
	return E_OK;
}

/* For the check at start (v3d_test.c): the table of a space (NULL: the kernel's) */
EXPORT T_V3DMMU *v3d_as_mmu( void *as )
{
	return ( as != NULL ) ? &((V3DAS *)as)->mmu : &v3d.mmu;
}

LOCAL T_GPUOPS	v3d_ops = {
	GPU_KIND_V3D,
	GPU_FEAT_CL | GPU_FEAT_CACHE | GPU_FEAT_MMU,
	0,
	v3d_info,
	v3d_bo_bind,
	v3d_bo_unbind,
	v3d_kick,
	v3d_reset,
	v3d_as_create,
	v3d_as_destroy,
	v3d_as_bind,
	v3d_as_unbind
};

/* Compute dispatches offered to the jobs, or not */
EXPORT void v3d_csd_offer( BOOL on )
{
	UINT	imask;

	ISpinLock(&v3d_lock, &imask);
	if ( on ) {
		v3d_ops.feature |= GPU_FEAT_CSD;
	} else {
		v3d_ops.feature &= ~GPU_FEAT_CSD;
	}
	ISpinUnlock(&v3d_lock, &imask);
}

/* A BO in the MMU and a job that runs without a list */
LOCAL void v3d_selftest( void )
{
	T_GPUBO		*bo;
	T_GPUBOINFO	bi;
	T_GPUJOB	job;
	D		fence;
	ER		er;

	bo = knl_gpu_bo_create(64 * 1024, 0, &er);
	if ( bo == NULL ) {
		tm_printf((UB *)"v3d: self test: no BO (%d)\n", er);
		return;
	}
	knl_gpu_bo_ref(bo, &bi);
	tm_printf((UB *)"v3d: self test: BO at GPU %08x, entry %08x for page %lx\n",
		  bi.offset, v3d_mmu_lookup(&v3d.mmu, bi.offset), knl_gpu_bo_page(bo, 0));

	knl_memset(&job, 0, sizeof(job));
	job.kind = GPU_JOB_CACHE;
	V3D_STEP("cache clean job");
	fence = knl_gpu_submit(&job, &bo, 1);
	er = ( fence > 0 ) ? knl_gpu_wait(fence, 1000 * 1000) : (ER)fence;
	tm_printf((UB *)"v3d: self test: cache clean job %s (%d)\n", ( er >= E_OK ) ? "ended" : "failed", er);
	knl_gpu_bo_put(bo);
}

EXPORT ER knl_v3d_init( void )
{
	ER	er;

	if ( CNF_V3D == 0 ) {
		return E_NOSPT;
	}
	er = v3d_clock_on();
	if ( er < E_OK ) {
		return er;
	}
	er = v3d_identify();
	if ( er < E_OK ) {
		return er;
	}
	if ( CNF_V3D < 2 ) {
		tm_printf((UB *)"v3d: built with V3D=1: nothing is written to the V3D\n");
		return E_OK;
	}
	if ( v3d.ver != 71 ) {
		tm_printf((UB *)"v3d: this driver is written for 7.1\n");
		return E_NOSPT;
	}

	/* BO pages only where the MMU's physical addresses reach */
	if ( v3d.pa_width >= 32 && v3d.pa_width <= 40 ) {
		v3d_ops.pa_limit = (UD)1 << v3d.pa_width;
	} else {
		v3d_ops.pa_limit = 0x100000000ULL;
	}

	er = mmu_setup();
	if ( er < E_OK ) {
		return er;
	}
	V3D_STEP("interrupts");
	irq_setup();
	V3D_STEP("register");
	er = knl_gpu_register(&v3d_ops, &v3d);
	if ( er < E_OK ) {
		return er;
	}
	V3D_STEP("overflow memory");
	if ( pool_init(&v3d.kpool, NULL) < E_OK ) {
		tm_printf((UB *)"v3d: no overflow memory for the kernel's jobs (%d pieces)\n", (INT)v3d.kpool.n);
	}
	ovf_task_start();
	V3D_STEP("self test");
	v3d_selftest();
	V3D_STEP("address spaces");
	v3d.as_allowed = TRUE;			/* for the check's own space */
	v3d.as_allowed = v3d_as_check();
	V3D_STEP("compositing");
	er = v3d_compose_init();
	if ( er >= E_OK ) {
		V3D_STEP("compute dispatch");
		v3d_csd_offer(v3d_csd_check());
	}
	if ( er >= E_OK && CNF_V3D != 2 ) {
		v3d_compose_start();		/* with the tests, they check it after theirs */
	}
	v3d_test_start();
	return E_OK;
}

#endif /* RPI5 */
