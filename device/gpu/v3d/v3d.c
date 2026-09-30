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
 *			(a BO entered in the MMU, a cache clean job).
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

/* Binner overflow memory handed out when the tile lists run out */
#define OVERFLOW_BYTES		( 512 * 1024 )

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
#define CORE_INTS_KNOWN		( V3D_INT_FRDONE | V3D_INT_FLDONE | V3D_INT_OUTOMEM )

/* The binner's and the renderer's ends are also seen in their counters */
#define POLL_CYC_MS		1

LOCAL T_SPLOCK	v3d_lock;

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
	UD		scratch_pa;		/* where invalid GPU accesses go */
	T_GPUBO		*overflow;		/* binner overflow memory */
	BOOL		overflow_given;		/* given out during the running job */
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
} V3D;

LOCAL V3D	v3d;

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
	out_w(CORE(V3D_CTL_SLCACTL), ~0U);
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
	out_w(HUB(V3D_MMU_CTL), in_w(HUB(V3D_MMU_CTL)) | V3D_MMU_CTL_TLB_CLEAR);
	poll_clear(HUB(V3D_MMU_CTL), V3D_MMU_CTL_TLB_CLEARING);
	out_w(HUB(V3D_MMUC_CONTROL), V3D_MMUC_ENABLE | V3D_MMUC_FLUSH);
	poll_clear(HUB(V3D_MMUC_CONTROL), V3D_MMUC_FLUSHING);
}

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
	out_w(HUB(V3D_MMU_PT_PA_BASE), (UW)( v3d.mmu.pt_pa >> V3D_PAGE_SHIFT ));
	out_w(HUB(V3D_MMU_ILLEGAL_ADDR), (UW)( v3d.scratch_pa >> V3D_PAGE_SHIFT ) | V3D_MMU_ILLEGAL_ENABLE);
	out_w(HUB(V3D_MMU_CTL), V3D_MMU_CTL_ENABLE);
	out_w(HUB(V3D_MMUC_CONTROL), V3D_MMUC_ENABLE);
	v3d.hw_ready = TRUE;
	V3D_STEP("TLB clear");
	mmu_flush(NULL);
	tm_printf((UB *)"v3d: page table at %lx, invalid accesses to %lx\n",
		  v3d.mmu.pt_pa, v3d.scratch_pa);
	return E_OK;
}

/* ---------------------------------------------------------------- jobs */

LOCAL void start_render( void )
{
	caches_invalidate();
	v3d.phase = PH_RENDER;
	v3d.frdone_at = in_w(CORE(V3D_CLE_RFC)) & 0xFF;
	out_w(CORE(V3D_CLE_CT1QBA), v3d.cl.rcl_start);
	out_w(CORE(V3D_CLE_CT1QEA), v3d.cl.rcl_end);		/* starts it */
}

LOCAL void start_bin( void )
{
	caches_invalidate();
	if ( v3d.cl.qma != 0 ) {
		out_w(CORE(V3D_CLE_CT0QMA), v3d.cl.qma);
		out_w(CORE(V3D_CLE_CT0QMS), v3d.cl.qms);
	}
	if ( v3d.cl.qts != 0 ) {
		out_w(CORE(V3D_CLE_CT0QTS), V3D_CLE_CT0QTS_ENABLE | v3d.cl.qts);
	}
	v3d.phase = PH_BIN;
	v3d.fldone_at = in_w(CORE(V3D_CLE_BFC)) & 0xFF;
	out_w(CORE(V3D_CLE_CT0QBA), v3d.cl.bcl_start);
	out_w(CORE(V3D_CLE_CT0QEA), v3d.cl.bcl_end);		/* starts it */
}

LOCAL ER v3d_kick( void *priv, CONST T_GPUJOB *job )
{
	switch ( job->kind ) {
	case GPU_JOB_CL: {
		UINT	imask;

		ISpinLock(&v3d_lock, &imask);
		v3d.cl = job->u.cl;
		v3d.cl_running = TRUE;
		v3d.overflow_given = FALSE;
		v3d.faulted = FALSE;
		v3d.job_ints = 0;
		v3d.job_faults = 0;
		if ( v3d.cl.bcl_start == v3d.cl.bcl_end ) {
			start_render();			/* a list with nothing to bin */
		} else {
			start_bin();
		}
		ISpinUnlock(&v3d_lock, &imask);
		return E_OK;
	}

	case GPU_JOB_CACHE:
		caches_clean();				/* polled: no interrupt tells its end */
		knl_gpu_job_end(E_OK);
		return E_OK;

	default:
		return E_NOSPT;				/* TFU and CSD wait for their registers (13.8) */
	}
}

LOCAL void v3d_reset( void *priv )
{
	UINT	imask;

	tm_printf((UB *)"v3d: at the reset: phase %d, core int %08x mask %08x, hub int %08x, "
		  "ct0cs %08x ct1cs %08x, ct0ca %08x ct1ca %08x, bfc %02x (from %02x) rfc %02x (from %02x)\n",
		  (INT)v3d.phase, in_w(CORE(V3D_CTL_INT_STS)), in_w(CORE(V3D_CTL_INT_MSK_STS)),
		  in_w(HUB(V3D_HUB_INT_STS)), in_w(CORE(V3D_CLE_CT0CS)), in_w(CORE(V3D_CLE_CT1CS)),
		  in_w(CORE(V3D_CLE_CT0CA)), in_w(CORE(V3D_CLE_CT1CA)),
		  in_w(CORE(V3D_CLE_BFC)) & 0xFF, v3d.fldone_at, in_w(CORE(V3D_CLE_RFC)) & 0xFF, v3d.frdone_at);
	ISpinLock(&v3d_lock, &imask);
	v3d.cl_running = FALSE;
	v3d.phase = PH_IDLE;
	ISpinUnlock(&v3d_lock, &imask);
	out_w(CORE(V3D_CLE_CT0CS), V3D_CLE_CTCS_CTRSTA);
	out_w(CORE(V3D_CLE_CT1CS), V3D_CLE_CTCS_CTRSTA);
	out_w(CORE(V3D_CTL_INT_CLR), ~0U);
	out_w(HUB(V3D_HUB_INT_CLR), ~0U);
	mmu_flush(NULL);
}

/* ---------------------------------------------------------------- the job's steps */

/* The renderer is done: the job ends. Called with the lock held */
LOCAL void frame_done( void )
{
	v3d.cl_running = FALSE;
	v3d.phase = PH_IDLE;
	if ( ( v3d.cl.flags & GPU_CL_FLUSH_CACHE ) != 0 ) {
		caches_clean();
	}
	knl_gpu_job_end(v3d.faulted ? E_IO : E_OK);
}

/*
 * The counters of flushes and frames tell the ends too, whatever the
 * interrupt bits are. Called with the lock held
 */
LOCAL void counters_check( void )
{
	if ( !v3d.cl_running ) {
		return;
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

/* ---------------------------------------------------------------- interrupts */

/* Too many interrupts for one job: the line stays up; turn it off */
LOCAL BOOL storm( UINT intno )
{
	if ( ++v3d.job_ints <= JOB_INTS_MAX ) {
		return FALSE;
	}
	DisableInt(intno);
	v3d.st.storms++;
	tm_printf((UB *)"v3d: interrupt %d keeps coming; turned off\n", (INT)intno);
	if ( v3d.cl_running ) {
		v3d.cl_running = FALSE;
		knl_gpu_job_end(E_IO);
	}
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
	if ( storm(intno) ) {
		ISpinUnlock(&v3d_lock, &imask);
		return;
	}

	if ( ( sts & V3D_INT_OUTOMEM ) != 0 ) {
		v3d.st.outomem++;
		if ( v3d.overflow != NULL && !v3d.overflow_given ) {
			T_GPUBOINFO	bi;

			knl_gpu_bo_ref(v3d.overflow, &bi);
			out_w(CORE(V3D_PTB_BPOA), bi.offset);
			out_w(CORE(V3D_PTB_BPOS), (UW)bi.size);
			v3d.overflow_given = TRUE;
		} else if ( v3d.cl_running ) {
			v3d.cl_running = FALSE;
			v3d.phase = PH_IDLE;
			knl_gpu_job_end(E_NOMEM);
			ISpinUnlock(&v3d_lock, &imask);
			return;
		}
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
	counters_check();
	ISpinUnlock(&v3d_lock, &imask);
}

LOCAL void v3d_hub_inthdr( UINT intno )
{
	UW	sts = in_w(HUB(V3D_HUB_INT_STS));

	out_w(HUB(V3D_HUB_INT_CLR), sts);
	v3d.st.hub_ints++;
	if ( storm(intno) ) {
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
}

EXPORT void v3d_stat( T_V3DSTAT *st )
{
	*st = v3d.st;
	st->scratch_pa = v3d.scratch_pa;
}

LOCAL void irq_setup( void )
{
	UW	hub = V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_CAP;
	T_CCYC	cc;

	/* every core bit: the ones not known are told of (CORE_INTS_KNOWN) */
	out_w(CORE(V3D_CTL_INT_MSK_CLR), ~0U);
	out_w(CORE(V3D_CTL_INT_CLR), ~0U);
	out_w(HUB(V3D_HUB_INT_MSK_SET), ~hub);
	out_w(HUB(V3D_HUB_INT_MSK_CLR), hub);
	out_w(HUB(V3D_HUB_INT_CLR), ~0U);

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
	info->feature = GPU_FEAT_CL | GPU_FEAT_CACHE | GPU_FEAT_MMU;
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

LOCAL T_GPUOPS	v3d_ops = {
	GPU_KIND_V3D,
	GPU_FEAT_CL | GPU_FEAT_CACHE | GPU_FEAT_MMU,
	0,
	v3d_info,
	v3d_bo_bind,
	v3d_bo_unbind,
	v3d_kick,
	v3d_reset
};

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
	v3d.overflow = knl_gpu_bo_create(OVERFLOW_BYTES, 0, &er);
	V3D_STEP("self test");
	v3d_selftest();
	v3d_test_start();
	return E_OK;
}

#endif /* RPI5 */
