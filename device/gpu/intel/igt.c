/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt.c
 *	The render and copy engines of Intel's integrated graphics of the
 *	9th generation (Kaby Lake and Amber Lake: UHD 620 8086:5917, UHD 615
 *	8086:591C), as a backend of the GPU's common layer
 *	(docs/tessronos/13-gpu.md 13.11).
 *
 *	The display engine is not driven here; the device and its global
 *	GTT are shared with the display's driver through igt_ggtt.c.
 *
 *	Start-up, step by step, each told on the console before it is taken
 *	(so that where a machine stopped can be read off KERNEL.TXT):
 *
 *	1. The device, its fuses, the frequencies it may run at. Nothing is
 *	   written (cmdline.txt "igt=1" stops here).
 *	2. Forcewake of the render, GT and media wells, held by the
 *	   start-up until it is over; the GT's own power states (RC6 and
 *	   power gating) and the frequency control off, RP1 asked for
 *	   (igt_pm.c).
 *	3. The workarounds of the GT and of the engines (a table against
 *	   Linux's in 13.11.6), the private PAT, the MOCS, the registers a
 *	   batch may write (whitelist), page tables of the kernel's space.
 *	4. Each engine: its status page, execlists on, its interrupts.
 *	5. Each engine's first context: run once with the context
 *	   workarounds written by the ring, and its saved image kept as the
 *	   one every new context of that engine starts from.
 *	6. RC6 and the frequency as the load asks (RPS) set up, as
 *	   cmdline.txt says ("igtrc6=0", "igtrps=0" turn them off; igt_pm.c).
 *	   From here on a job holds forcewake from before it is submitted
 *	   until it is over, and the GT is parked and may sleep once idle.
 *	7. The common layer's backend registered. Then the tests at start
 *	   (igt_test.c, "igt=2", the default for now) and the check of
 *	   compositing on the copy engine (igt_compose.c).
 *
 *	A job is a batch buffer in an address space (the kernel's or a
 *	process's), on one engine, in one of the space's hardware contexts.
 *	Its context's ring gets the commands that start the batch and,
 *	after it, write the engine's sequence number to its status page and
 *	raise the user interrupt; the context's descriptor goes to the
 *	execlist submit port. The job is over when the number is there and
 *	the engine said, in its context status buffer, that it went idle
 *	(the image is then saved and may be written again). An interrupt
 *	tells either; a look every millisecond catches what an interrupt
 *	did not. A job that does not end has the common layer reset the
 *	GPU: the engine alone is reset (the display goes on) and started
 *	again, and the context it ran is made anew.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"
#include <ts/ob.h>
#include "obj/obj.h"
#include "igt.h"

#define SEQ_IDLE_MS	20		/* the number written, and no word of idle: over all the same */
#define DIRECT_MS	500		/* a job run before the common layer has it */

EXPORT IGT	knl_igt;

/* Set by the display's driver when it takes its interrupts through this one's */
EXPORT void knl_igt_irq_display( void (*fn)( UW master ) )
{
	knl_igt.display_irq = fn;
}

IMPORT void knl_bootlog_soon( void );	/* bootlog.c: the record files written again soon */

/* The display's half of the hashing of compressed surfaces, when its driver has one */
IMPORT void knl_idisp_hash_mode( void ) __attribute__((weak));

/* ---------------------------------------------------------------- small things */

EXPORT void igt_clflush( CONST void *p, UD len )
{
	UBINT	a = (UBINT)p & ~(UBINT)63, end = (UBINT)p + len;

	__asm__ volatile ( "mfence" ::: "memory" );
	for ( ; a < end; a += 64 ) {
		__asm__ volatile ( "clflush (%0)" :: "r"(a) : "memory" );
	}
	__asm__ volatile ( "mfence" ::: "memory" );
}

EXPORT UD igt_now_us( void )
{
	UD	f = SysCounterFreq();

	return ( f != 0 ) ? ReadSysCounter() / ( f / 1000000 ) : 0;
}

LOCAL void udelay( UD us )
{
	UD	t0 = igt_now_us();

	while ( igt_now_us() - t0 < us ) CpuRelax();
}

/* A register come to 'val' under 'mask' within 'us' */
LOCAL BOOL wait_for( UW off, UW mask, UW val, UD us )
{
	UD	t0 = igt_now_us();

	for ( ;; ) {
		if ( ( igt_rd(off) & mask ) == val ) return TRUE;
		if ( igt_now_us() - t0 > us ) return FALSE;
		CpuRelax();
	}
}

LOCAL INT bits( UW v )
{
	INT	n = 0;

	for ( ; v != 0; v &= v - 1 ) n++;
	return n;
}

/* Slices, subslices and execution units the fuses leave, and the steering of reads */
LOCAL void topology( void )
{
	INT	ss, s0 = 0, ss0 = 0, n = 0;

	knl_igt.fuse2 = igt_rd(FUSE2);
	knl_igt.slice_mask = F2_S_ENA(knl_igt.fuse2);
	knl_igt.ss_mask = ~F2_SS_DIS(knl_igt.fuse2) & 0xf;
	knl_igt.eu_dis = igt_rd(EU_DISABLE(0));
	while ( s0 < 3 && ( knl_igt.slice_mask & ( 1 << s0 ) ) == 0 ) s0++;
	while ( ss0 < 4 && ( knl_igt.ss_mask & ( 1 << ss0 ) ) == 0 ) ss0++;
	for ( ss = 0; ss < 4; ss++ ) {
		if ( knl_igt.ss_mask & ( 1 << ss ) ) n += 8 - bits(( knl_igt.eu_dis >> ( ss * 8 ) ) & 0xff);
	}
	knl_igt.eu_total = (UW)( n * bits(knl_igt.slice_mask) );
	tm_printf((UB *)"igt: FUSE2 %08x EU_DISABLE0 %08x: slices %x, subslices %x, %d execution units\n",
		  knl_igt.fuse2, knl_igt.eu_dis, knl_igt.slice_mask, knl_igt.ss_mask, knl_igt.eu_total);
}

/* ---------------------------------------------------------------- workarounds and tables */

LOCAL void rmw( UW off, UW clr, UW set )
{
	igt_wr(off, ( igt_rd(off) & ~clr ) | set);
}

/* The GT's own (docs/tessronos/13-gpu.md 13.11.6) */
LOCAL void gt_workarounds( void )
{
	/* reads of registers kept per slice and subslice go to the first present */
	INT	s0 = 0, ss0 = 0;

	while ( s0 < 3 && ( knl_igt.slice_mask & ( 1 << s0 ) ) == 0 ) s0++;
	while ( ss0 < 4 && ( knl_igt.ss_mask & ( 1 << ss0 ) ) == 0 ) ss0++;
	rmw(MCR_SELECTOR, MCR_SLICE(3) | MCR_SUBSLICE(3), MCR_SLICE(s0) | MCR_SUBSLICE(ss0));

	rmw(GAM_ECOCHK, 0, ECOCHK_DIS_TLB | DISABLE_HDC_INVALIDATION);
	rmw(MMCD_MISC_CTRL, 0, MMCD_PCLA | MMCD_HOTSPOT_EN);
	rmw(UCGCTL4, 0, EU_GAUNIT_CLOCK_GATE_DIS);
	rmw(GAMT_ECO_REG_RW_IA, 0, GAMT_IN_PLACE_DECOMPRESS);
	if ( knl_igt.pre_c0 ) {
		rmw(GAMT_CHKN_BIT_REG, 0, GAMT_DYNAMIC_CREDIT_SHARING_DIS);
		rmw(UCGCTL6, 0, SDEUNIT_CLOCK_GATE_DIS);
		rmw(UCGCTL1, 0, GAMUNIT_CLOCK_GATE_DIS);
	}
	/* the display's half of the hashing of compressed surfaces, kept the same as the GT's */
	if ( knl_idisp_hash_mode != NULL ) knl_idisp_hash_mode();
}

/* The render engine's, kept in its registers (written again after a reset) */
LOCAL void rcs_workarounds( void )
{
	igt_wr(FF_SLICE_CS_CHICKEN1, MASK_EN(FFSC_PERCTX_PREEMPT_CTRL));
	rmw(GARBCNTL, 0, GAPS_TSV_CREDIT_DIS);
	igt_wr(CSFE_CHICKEN1_RCS, MASK_EN(PREEMPT_GPGPU_SYNC_SWITCH_DIS));
	rmw(SCRATCH1, EVICTION_PERF_FIX, LBS_SLA_RETRY_TIMER_DEC);
	rmw(L3SQCREG4, LQSQ_NONIA_COHERENT_ATOMICS, LQSC_FLUSH_COHERENT_LINES);
	rmw(SCRATCH_LNCF1, LNCF_NONIA_COHERENT_ATOMICS, 0);
}

/* The registers a batch of a process may write, beyond those it always may */
LOCAL CONST UW	rcs_nonpriv[] = { CTX_PREEMPT_REG, CS_CHICKEN1, HDC_CHICKEN1, COMMON_SLICE_CHICKEN2, L3SQCREG4 };

LOCAL void whitelist( IGTENG *e )
{
	INT	i, n = ( e == &knl_igt.eng[IGT_RCS] ) ? (INT)( sizeof(rcs_nonpriv) / sizeof(rcs_nonpriv[0]) ) : 0;

	for ( i = 0; i < NONPRIV_SLOTS; i++ ) {
		igt_ewr(e, RING_FORCE_TO_NONPRIV(i), ( i < n ) ? rcs_nonpriv[i] : e->base + RING_NOPID);
	}
}

/*
 * The memory object control states: index 0 uncached, 1 as the page
 * table says, 2 write-back, 63 what the L3 evicts with; the rest as 1.
 * A batch names them by index (iris uses 1 for what others see, 2 for
 * its own).
 */
LOCAL UW mocs_ctl( INT i )
{
	switch ( i ) {
	case 0:		return LE_UC | LE_TC_LLC_ELLC;
	case 2:		return LE_WB | LE_TC_LLC_ELLC | LE_LRUM(3);
	case 63:	return LE_WB | LE_TC_LLC | LE_LRUM(3);
	default:	return LE_PAGETABLE | LE_TC_PAGETABLE | LE_LRUM(3);
	}
}

LOCAL UW mocs_l3( INT i )
{
	return ( i == 0 || i == 63 ) ? L3_UC : L3_WB;
}

LOCAL void mocs( IGTENG *e )
{
	INT	i;
	UW	base = ( e == &knl_igt.eng[IGT_RCS] ) ? GFX_MOCS(0) : BLT_MOCS(0);

	for ( i = 0; i < 64; i++ ) igt_wr(base + i * 4, mocs_ctl(i));
	if ( e == &knl_igt.eng[IGT_RCS] ) {
		for ( i = 0; i < 32; i++ ) igt_wr(LNCFCMOCS(i), mocs_l3(2 * i) | ( mocs_l3(2 * i + 1) << 16 ));
	}
}

/*
 * The private PAT the entries of the page tables index: 0 write-back in
 * the LLC (the tables themselves, and the display's GGTT entries), 4
 * write-back in the LLC and eLLC (the pages), 3 uncached
 */
LOCAL void ppat( void )
{
	UW	lo, hi;

	lo = ( PPAT_WB | PPAT_LLC ) | ( ( PPAT_WC | PPAT_LLCELLC ) << 8 ) | ( ( PPAT_WB | PPAT_ELLC_OVERRIDE ) << 16 )
	   | ( PPAT_UC << 24 );
	hi = ( PPAT_WB | PPAT_LLCELLC | PPAT_AGE(0) ) | ( ( PPAT_WB | PPAT_LLCELLC | PPAT_AGE(1) ) << 8 )
	   | ( ( PPAT_WB | PPAT_LLCELLC | PPAT_AGE(2) ) << 16 ) | ( (UW)( PPAT_WB | PPAT_LLCELLC | PPAT_AGE(3) ) << 24 );
	igt_wr(PPAT_LO, lo);
	igt_wr(PPAT_HI, hi);
}

/* The context workarounds: masked writes the first context runs, saved with its image */
typedef struct {
	UW	reg, val;
} REGVAL;

LOCAL INT ctx_workarounds( REGVAL *w )
{
	INT	n = 0;
	UW	csc2 = PBE_COMPRESSED_HASH_SEL;

	if ( !knl_igt.pre_c0 ) csc2 |= SBE_REPLAY_BUF_OPT_DIS;
	w[n].reg = COMMON_SLICE_CHICKEN2;	w[n++].val = MASK_EN(csc2);
	w[n].reg = HALF_SLICE_CHICKEN7;
	w[n++].val = MASK_EN(SAMPLER_HASH_COMPRESSED_READ_ADDR | YV12_BUGFIX | GPGPU_PREEMPTION);
	w[n].reg = ROW_CHICKEN;			w[n++].val = MASK_EN(FLOW_CONTROL_EN | PARTIAL_INST_SHOOTDOWN_DIS);
	w[n].reg = CACHE_MODE_1;		w[n++].val = MASK_EN(STC_4X4_OPT_DIS | PARTIAL_RESOLVE_IN_VC_DIS);
	w[n].reg = HALF_SLICE_CHICKEN5;		w[n++].val = MASK_DIS(CCS_TLB_PREFETCH_EN);
	w[n].reg = HDC_CHICKEN0;
	w[n++].val = MASK_EN(HDC_FORCE_CTX_SR_NON_COHERENT | HDC_FORCE_CSR_NON_COHERENT_OVR_DIS | HDC_FORCE_NON_COHERENT);
	w[n].reg = HALF_SLICE_CHICKEN3;		w[n++].val = MASK_EN(SAMPLER_POWER_BYPASS_DIS);
	w[n].reg = HALF_SLICE_CHICKEN2;		w[n++].val = MASK_EN(ST_PO_DIS);
	w[n].reg = CS_CHICKEN1;
	w[n++].val = MASK_FIELD(PREEMPT_3D_OBJECT_LEVEL | PREEMPT_GPGPU_LEVEL_MASK, PREEMPT_GPGPU_COMMAND_LEVEL);
	w[n].reg = HALF_SLICE_CHICKEN1;		w[n++].val = MASK_EN(SBE_SS_CACHE_DISPATCH_PORT_SHARING_DIS);
	return n;
}

/* ---------------------------------------------------------------- contexts */

/*
 * The register state of a context image, as the engine lays it: blocks
 * of MI_LOAD_REGISTER_IMM with the registers (from the engine's base),
 * with gaps of no-ops between. The render and copy engines of this
 * generation lay theirs alike but for the one register of the third
 * block.
 */
LOCAL CONST UH	lrc_b1[] = { 0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140, 0x110, 0x11c, 0x114, 0x118,
			     0x1c0, 0x1c4, 0x1c8 };
LOCAL CONST UH	lrc_b2[] = { 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270 };
LOCAL CONST UH	lrc_b4a[] = { 0x028, 0x09c, 0x0c0, 0x178, 0x17c, 0x358, 0x170, 0x150, 0x154, 0x158, 0x41c };

LOCAL UW *lrc_block( UW *r, UW base, CONST UH *reg, INT n, BOOL posted )
{
	INT	i;

	*r++ = MI_LRI(n) | ( posted ? MI_LRI_FORCE_POSTED : 0 );
	for ( i = 0; i < n; i++ ) {
		r[0] = base + reg[i];
		r += 2;
	}
	return r;
}

LOCAL void lrc_layout( UW *r, INT eng )
{
	UW	base = ( eng == IGT_RCS ) ? RCS_BASE : BCS_BASE;
	UH	b3 = ( eng == IGT_RCS ) ? 0x0c8 : 0x200;
	UH	b4[44];
	INT	i;

	r += 1;							/* a no-op */
	r = lrc_block(r, base, lrc_b1, 14, TRUE);
	r += 3;
	r = lrc_block(r, base, lrc_b2, 9, TRUE);
	r += 13;
	r = lrc_block(r, base, &b3, 1, eng != IGT_RCS);
	r += 13;
	for ( i = 0; i < 11; i++ ) b4[i] = lrc_b4a[i];
	for ( i = 0; i < 32; i++ ) b4[11 + i] = (UH)( 0x600 + i * 4 );
	b4[43] = 0x068;
	(void)lrc_block(r, base, b4, 44, TRUE);
}

/*
 * The registers a context starts with: its ring, empty; its page
 * tables; restoring its engine's state (or not, when it is made from
 * nothing); the ring not stopped
 */
LOCAL void ctx_regs( IGTCTX *c, BOOL inhibit )
{
	UW	*r = (UW *)( c->kva + LRC_STATE_PAGE * PAGE_SIZE );
	UD	pml4 = c->vm->pml4_pa;

	r[CTX_CONTEXT_CONTROL] = MASK_EN(CTX_CTRL_INHIBIT_SYN_SWITCH)
			       | MASK_DIS(CTX_CTRL_RESTORE_INHIBIT | CTX_CTRL_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE)
			       | ( inhibit ? CTX_CTRL_RESTORE_INHIBIT : 0 );
	r[CTX_RING_HEAD] = 0;
	r[CTX_RING_TAIL] = 0;
	r[CTX_RING_START] = (UW)c->ring_ggtt;
	r[CTX_RING_CTL] = ( ( IGT_RING_BYTES - PAGE_SIZE ) & 0x1ff000 ) | RING_VALID;
	r[0x0c + 1] = 0;					/* the batch's address */
	r[0x0e + 1] = 0;
	r[CTX_BB_STATE] = BB_STATE_PPGTT;
	r[CTX_TIMESTAMP] = 0;
	r[CTX_PDP0_UDW] = (UW)( pml4 >> 32 );
	r[CTX_PDP0_LDW] = (UW)pml4;
	r[CTX_RING_MI_MODE + 1] = ( r[CTX_RING_MI_MODE + 1] & ~STOP_RING ) | ( STOP_RING << 16 );
	c->tail = 0;
}

/* Made anew: from the engine's first image when there is one, else from nothing */
LOCAL void ctx_fresh( IGTCTX *c )
{
	IGTENG	*e = &knl_igt.eng[c->eng];

	if ( e->golden != NULL ) {
		knl_memcpy(c->kva, e->golden, c->img_bytes);
		ctx_regs(c, FALSE);
	} else {
		knl_memset(c->kva, 0, c->img_bytes);
		lrc_layout((UW *)( c->kva + LRC_STATE_PAGE * PAGE_SIZE ), c->eng);
		ctx_regs(c, TRUE);
	}
	knl_memset(c->ring, 0, IGT_RING_BYTES);
	igt_clflush(c->kva, c->img_bytes + IGT_RING_BYTES);
	c->fresh = FALSE;
}

/* A context of an engine in a space; under the mutex */
LOCAL IGTCTX *ctx_new( IGTVM *vm, INT eng )
{
	IGTENG	*e = &knl_igt.eng[eng];
	IGTCTX	*c;
	UD	total = e->img_bytes + IGT_RING_BYTES, pa;
	UINT	order = 0;
	ER	er;

	while ( ( PAGE_SIZE << order ) < total ) order++;
	c = (IGTCTX *)Kmalloc(sizeof(IGTCTX));
	if ( c == NULL ) return NULL;
	knl_memset(c, 0, sizeof(IGTCTX));
	c->pf = knl_alloc_pages(order, ZONE_NORMAL, 0);
	if ( c->pf == NULL ) c->pf = knl_alloc_pages(order, ZONE_DMA32, KAF_DMA32);
	if ( c->pf == NULL ) {
		Kfree(c);
		return NULL;
	}
	c->order = order;
	pa = knl_pf_to_pa(c->pf);
	er = knl_igt_ggtt_alloc(total, PAGE_SIZE, 0, IGT_GGTT_RENDER, NULL, &c->ggtt);
	if ( er < E_OK || pa + ( PAGE_SIZE << order ) > ( 1ULL << 39 ) ) {
		if ( er >= E_OK ) (void)knl_igt_ggtt_free(c->ggtt);
		knl_free_pages(c->pf, order);
		Kfree(c);
		return NULL;
	}
	knl_igt_ggtt_set(c->ggtt, &pa, total >> PAGE_SHIFT, TRUE);
	c->eng = eng;
	c->vm = vm;
	c->pa = pa;
	c->kva = (UB *)PA2VA(pa);
	c->img_bytes = e->img_bytes;
	c->ring_ggtt = c->ggtt + e->img_bytes;
	c->ring = c->kva + e->img_bytes;
	if ( ++knl_igt.next_ctx_id >= ( 1U << 20 ) ) knl_igt.next_ctx_id = 1;
	c->id = knl_igt.next_ctx_id;
	c->fresh = TRUE;
	if ( c->id <= 6 ) {
		/* the first few, for the record: where in memory the GPU reads the image and the ring from */
		tm_printf((UB *)"igt: %s context %d: image and ring at %lx-%lx (GGTT %lx)\n", e->name, c->id, pa,
			  pa + total, c->ggtt);
	}
	return c;
}

EXPORT void igt_ctx_free( IGTCTX *c )
{
	UD	total = c->img_bytes + IGT_RING_BYTES;

	knl_igt_ggtt_clear(c->ggtt, total >> PAGE_SHIFT);
	(void)knl_igt_ggtt_free(c->ggtt);
	knl_free_pages(c->pf, c->order);
	Kfree(c);
}

/* ---------------------------------------------------------------- the ring */

Inline void put( IGTCTX *c, UW v )
{
	*(UW *)( c->ring + c->tail ) = v;
	c->tail = ( c->tail + 4 ) & ( IGT_RING_BYTES - 1 );
}

LOCAL void pipe_control( IGTCTX *c, UW flags, UW addr, UW val )
{
	put(c, PIPE_CONTROL);
	put(c, flags);
	put(c, addr);
	put(c, 0);
	put(c, val);
	put(c, 0);
}

LOCAL void flush_dw( IGTCTX *c, UW flags, UW addr, UW val )
{
	put(c, MI_FLUSH_DW | flags);
	put(c, addr);
	put(c, 0);
	put(c, val);
}

/*
 * The commands of a job: what each context needs as it comes in, the
 * caches and the TLBs invalidated, the batch (or, for an engine's first
 * context, the context workarounds), the caches flushed, the sequence
 * number written, the interrupt. 'start' 0 runs no batch.
 */
LOCAL UW emit( IGTENG *e, IGTCTX *c, UD start, BOOL first )
{
	UW	from = c->tail, seq_at = (UW)e->hwsp_ggtt + HWS_SEQNO * 4;
	UW	scratch = LRC_SCRATCH * 4;

	if ( ++e->seqno == 0 ) e->seqno = 1;

	if ( c->eng == IGT_RCS ) {
		if ( first ) {
			REGVAL	w[16];
			INT	i, n = ctx_workarounds(w);

			put(c, MI_LRI(n));
			for ( i = 0; i < n; i++ ) {
				put(c, w[i].reg);
				put(c, w[i].val);
			}
		}
		/* the shared local memory cleared and the coherent lines of the L3 flushed as a context comes in */
		pipe_control(c, PC_FLUSH_L3 | PC_STORE_DATA_INDEX | PC_CS_STALL | PC_QW_WRITE, scratch, 0);
		put(c, MI_LRI(3));
		put(c, COMMON_SLICE_CHICKEN2);
		put(c, MASK_DIS(GATHER_AT_SET_SHADER_DIS));
		put(c, FF_SLICE_CHICKEN);
		put(c, MASK_EN(FF_CL_PROVOKING_FIX));
		put(c, CHICKEN3_3D);
		put(c, MASK_EN(SF_PROVOKING_FIX));
		/* a pipe control of nothing first, before the vertex fetch cache is invalidated */
		pipe_control(c, 0, 0, 0);
		pipe_control(c, PC_CS_STALL | PC_TLB_INVALIDATE | PC_INST_INVALIDATE | PC_TEX_INVALIDATE
			     | PC_VF_INVALIDATE | PC_CONST_INVALIDATE | PC_STATE_INVALIDATE | PC_QW_WRITE
			     | PC_STORE_DATA_INDEX, scratch, 0);
		if ( start != 0 ) {
			put(c, MI_ARB_ON_OFF | MI_ARB_ENABLE);
			put(c, MI_BATCH_BUFFER_START | MI_BATCH_PPGTT);
			put(c, (UW)start);
			put(c, (UW)( start >> 32 ));
			put(c, MI_ARB_ON_OFF | MI_ARB_DISABLE);
			pipe_control(c, PC_CS_STALL | PC_RT_FLUSH | PC_DEPTH_FLUSH | PC_DC_FLUSH | PC_FLUSH_ENABLE, 0, 0);
		}
		pipe_control(c, PC_CS_STALL | PC_TLB_INVALIDATE | PC_RT_FLUSH | PC_DEPTH_FLUSH | PC_DC_FLUSH, 0, 0);
		pipe_control(c, PC_FLUSH_ENABLE | PC_CS_STALL | PC_QW_WRITE | PC_GLOBAL_GTT, seq_at, e->seqno);
	} else {
		flush_dw(c, FLUSH_DW_INVALIDATE_TLB | FLUSH_DW_STORE_INDEX | FLUSH_DW_OP_STOREDW, scratch, 0);
		if ( start != 0 ) {
			put(c, MI_ARB_ON_OFF | MI_ARB_ENABLE);
			put(c, MI_BATCH_BUFFER_START | MI_BATCH_PPGTT);
			put(c, (UW)start);
			put(c, (UW)( start >> 32 ));
			put(c, MI_ARB_ON_OFF | MI_ARB_DISABLE);
			flush_dw(c, FLUSH_DW_STORE_INDEX | FLUSH_DW_OP_STOREDW, scratch, 0);
		}
		flush_dw(c, FLUSH_DW_OP_STOREDW, seq_at | FLUSH_DW_USE_GTT, e->seqno);
	}
	put(c, MI_USER_INTERRUPT);
	put(c, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	if ( c->tail & 7 ) put(c, MI_NOOP);

	/* the lines written, back to memory */
	if ( c->tail >= from ) {
		igt_clflush(c->ring + from, c->tail - from);
	} else {
		igt_clflush(c->ring + from, IGT_RING_BYTES - from);
		igt_clflush(c->ring, c->tail);
	}
	return e->seqno;
}

/* The context to the engine: its tail in the image, its descriptor to the submit port */
LOCAL void submit( IGTENG *e, IGTCTX *c )
{
	UW	*r = (UW *)( c->kva + LRC_STATE_PAGE * PAGE_SIZE );
	UW	lo = (UW)c->ggtt | CTX_DESC_64B | CTX_DESC_VALID | CTX_DESC_PRIVILEGE;
	UINT	imask;

	r[CTX_RING_TAIL] = c->tail;
	igt_clflush(&r[CTX_RING_TAIL], 4);

	ISpinLock(&knl_igt.lock, &imask);
	e->cur = c;
	e->idle_seen = FALSE;
	e->t_seq = 0;
	e->t_kick = igt_now_us();
	e->jobs++;
	knl_igt.active = (INT)( e - knl_igt.eng );
	/* the second port empty, then the first; the last write submits */
	igt_ewr(e, RING_ELSP, 0);
	igt_ewr(e, RING_ELSP, 0);
	igt_ewr(e, RING_ELSP, c->id);
	igt_ewr(e, RING_ELSP, lo);
	ISpinUnlock(&knl_igt.lock, &imask);
}

/* ---------------------------------------------------------------- the end of a job */

LOCAL volatile BOOL	direct_done;
LOCAL volatile BOOL	job_error;

/* What the engine wrote to its status buffer since the last look; under the lock */
LOCAL void csb_read( IGTENG *e )
{
	UW	wp = igt_erd(e, RING_CSB_PTR) & 7;

	if ( wp >= CSB_ENTRIES ) return;		/* nothing written since the reset */
	while ( e->csb_head != wp ) {
		UW	st;

		e->csb_head = ( e->csb_head + 1 ) % CSB_ENTRIES;
		st = igt_erd(e, RING_CSB_LO(e->csb_head));
		if ( st & CSB_ACTIVE_IDLE ) e->idle_seen = TRUE;
	}
	igt_ewr(e, RING_CSB_PTR, MASK_FIELD(0x0700, e->csb_head << 8));
}

/* Whether the job on e is over; under the lock. Ends it when it is. */
LOCAL BOOL check( IGTENG *e, BOOL poll )
{
	UW	seq;

	if ( e->cur == NULL ) return FALSE;
	csb_read(e);
	/* the line read afresh: the number is the GPU's write, whatever the caches hold */
	igt_clflush(&e->hwsp[HWS_SEQNO], 4);
	seq = *(volatile UW *)&e->hwsp[HWS_SEQNO];
	if ( (INT)( seq - e->seqno ) < 0 ) return FALSE;
	if ( !e->idle_seen ) {
		UD	now = igt_now_us();

		if ( e->t_seq == 0 ) {
			e->t_seq = now;
			return FALSE;
		}
		if ( now - e->t_seq < SEQ_IDLE_MS * 1000 ) return FALSE;
		e->no_idle++;
	}
	if ( poll ) e->by_poll++;
	e->cur = NULL;
	knl_igt.active = -1;
	return TRUE;
}

/* The job over: told, and its forcewake let go of once the next has been handed on */
LOCAL void ended( void )
{
	if ( knl_igt.ready ) {
		knl_gpu_job_end(job_error ? E_IO : E_OK);
	} else {
		direct_done = TRUE;
	}
	igt_fw_put();
}

LOCAL void look( void *exinf )
{
	INT	a = knl_igt.active;
	UINT	imask;
	BOOL	over = FALSE;

	(void)exinf;
	if ( a >= 0 ) {
		ISpinLock(&knl_igt.lock, &imask);
		if ( knl_igt.active == a ) over = check(&knl_igt.eng[a], TRUE);
		ISpinUnlock(&knl_igt.lock, &imask);
		if ( over ) ended();
	}
	igt_pm_tick();
}

LOCAL void isr( UINT intno, UW iar )
{
	UW	master, iir = 0, pm = 0;
	INT	k;
	UINT	imask;
	BOOL	over = FALSE, pm_wake = FALSE;

	(void)iar;
	master = igt_rd(MASTER_IRQ);
	igt_wr(MASTER_IRQ, 0);
	knl_igt.irq_n++;
	if ( master & MASTER_GT0 ) {
		iir = igt_rd(GT_IIR(0));
		if ( iir != 0 ) igt_wr(GT_IIR(0), iir);
	}
	if ( master & MASTER_GT2 ) {
		pm = igt_rd(GT_IIR(2));
		if ( pm != 0 ) igt_wr(GT_IIR(2), pm);
	}
	ISpinLock(&knl_igt.lock, &imask);
	if ( pm != 0 ) pm_wake = igt_pm_irq(pm);
	for ( k = 0; k < IGT_NENG; k++ ) {
		IGTENG	*e = &knl_igt.eng[k];
		UW	b = ( iir >> e->shift ) & 0xffff;

		if ( b & GT_USER ) e->irq_user++;
		if ( b & GT_CTX_SWITCH ) e->irq_ctx++;
		if ( b & GT_CS_ERROR ) {
			UW	eir = igt_erd(e, RING_EIR);

			/* what the engine was at, for the line the next kick says (igt_kick) */
			e->err_eir = eir;
			e->err_ipehr = igt_erd(e, RING_IPEHR);
			e->err_ipeir = igt_erd(e, RING_IPEIR);
			e->err_instdone = igt_erd(e, RING_INSTDONE);
			e->err_acthd = ( (UD)igt_erd(e, RING_ACTHD_UDW) << 32 ) | igt_erd(e, RING_ACTHD);
			e->err_bbaddr = ( (UD)igt_erd(e, RING_BBADDR_UDW) << 32 ) | igt_erd(e, RING_BBADDR);
			e->irq_err++;
			job_error = TRUE;
			igt_ewr(e, RING_EMR, igt_erd(e, RING_EMR) | eir);	/* not again */
			igt_ewr(e, RING_EIR, eir);
		}
		if ( b != 0 && knl_igt.active == k ) over = check(e, FALSE);
	}
	ISpinUnlock(&knl_igt.lock, &imask);
	if ( master & MASTER_DISPLAY ) {
		if ( knl_igt.display_irq != NULL ) {
			knl_igt.display_irq(master);
		} else if ( ++knl_igt.irq_display > 100 ) {
			/*
			 * The display asks for interrupts nobody takes, and only
			 * its driver may quiet them: the engines are looked at
			 * every millisecond from now on instead
			 */
			knl_igt.irq_off = TRUE;
		}
	}
	if ( ( master & ( MASTER_GT0 | MASTER_GT2 | MASTER_DISPLAY ) ) == 0 ) knl_igt.irq_spurious++;
	if ( !knl_igt.irq_off ) igt_wr(MASTER_IRQ, MASTER_IRQ_CONTROL);
	EndOfInt(intno);
	if ( pm_wake ) igt_pm_wake();
	if ( over ) ended();
}

/* ---------------------------------------------------------------- engines */

EXPORT void igt_eng_dump( CONST char *why, IGTENG *e )
{
	tm_printf((UB *)"igt: %s %s: HEAD %08x TAIL %08x START %08x CTL %08x ACTHD %08x%08x BBADDR %08x%08x "
		  "INSTDONE %08x IPEIR %08x IPEHR %08x EIR %08x ESR %08x MI_MODE %08x EXECLIST %08x %08x "
		  "CSB_PTR %08x seqno %d of %d\n", e->name, why, igt_erd(e, RING_HEAD), igt_erd(e, RING_TAIL),
		  igt_erd(e, RING_START), igt_erd(e, RING_CTL), igt_erd(e, RING_ACTHD_UDW), igt_erd(e, RING_ACTHD),
		  igt_erd(e, RING_BBADDR_UDW), igt_erd(e, RING_BBADDR), igt_erd(e, RING_INSTDONE),
		  igt_erd(e, RING_IPEIR), igt_erd(e, RING_IPEHR), igt_erd(e, RING_EIR), igt_erd(e, RING_ESR),
		  igt_erd(e, RING_MI_MODE), igt_erd(e, RING_EXECLIST_LO), igt_erd(e, RING_EXECLIST_HI),
		  igt_erd(e, RING_CSB_PTR), e->hwsp[HWS_SEQNO], e->seqno);
	tm_printf((UB *)"igt: %s %s: fault %08x, error %08x, TLB %08x %08x; GT ISR %08x IIR %08x; master %08x\n",
		  e->name, why, igt_rd(RING_FAULT_REG), igt_rd(ERROR_GEN6), igt_rd(FAULT_TLB_DATA1),
		  igt_rd(FAULT_TLB_DATA0), igt_rd(GT_ISR(0)), igt_rd(GT_IIR(0)), igt_rd(MASTER_IRQ));
}

/* The value of an engine's RING_IMR with its user, context switch and error interrupts let through */
LOCAL UW eng_imr( CONST IGTENG *e )
{
	return ~( ( GT_USER | GT_CTX_SWITCH | GT_CS_ERROR ) << e->shift );
}

/* An engine made ready to take contexts: at start and after it was reset */
LOCAL void eng_resume( IGTENG *e )
{
	INT	i;

	igt_ewr(e, RING_HWSTAM, 0xffffffff);
	igt_ewr(e, RING_MODE, MASK_EN(RUN_LIST_ENABLE));
	igt_ewr(e, RING_MI_MODE, MASK_DIS(STOP_RING));
	igt_ewr(e, RING_HWS_PGA, (UW)e->hwsp_ggtt);
	(void)igt_erd(e, RING_HWS_PGA);
	/*
	 * The engine's own mask has its bits where GT interrupt register 0
	 * has that engine's: the copy engine's at 16 and up, the half below
	 * not there (reads as ones)
	 */
	igt_ewr(e, RING_IMR, eng_imr(e));
	igt_ewr(e, RING_EMR, 0xffffffff & ~0x1U);	/* only instruction errors raise the error interrupt */
	/* read back at once: an engine whose mask does not take raises nothing, and its jobs end only by the look */
	tm_printf((UB *)"igt: %s interrupt mask written %08x, reads %08x%s\n", e->name, eng_imr(e),
		  igt_erd(e, RING_IMR), ( igt_erd(e, RING_IMR) == eng_imr(e) ) ? "" : " (NOT taken)");

	/* the status buffer's pointers to its last entry, so that the next one written is the first */
	for ( i = 0; i < 12; i++ ) e->hwsp[HWS_CSB0 + i] = 0xffffffff;
	e->hwsp[HWS_CSB_WRITE] = CSB_ENTRIES - 1;
	igt_clflush(e->hwsp, PAGE_SIZE);
	igt_ewr(e, RING_CSB_PTR, 0xffff0000U | ( ( CSB_ENTRIES - 1 ) << 8 ) | ( CSB_ENTRIES - 1 ));
	e->csb_head = CSB_ENTRIES - 1;

	mocs(e);
	whitelist(e);
	if ( e == &knl_igt.eng[IGT_RCS] ) rcs_workarounds();
}

LOCAL ER eng_init( IGTENG *e )
{
	UD	pa;
	ER	er;

	e->hwsp_pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( e->hwsp_pf == NULL ) return E_NOMEM;
	pa = knl_pf_to_pa(e->hwsp_pf);
	e->hwsp = (UW *)PA2VA(pa);
	er = knl_igt_ggtt_alloc(PAGE_SIZE, PAGE_SIZE, 0, IGT_GGTT_RENDER,
				( e == &knl_igt.eng[IGT_RCS] ) ? "rcs status" : "bcs status", &e->hwsp_ggtt);
	if ( er < E_OK ) return er;
	knl_igt_ggtt_set(e->hwsp_ggtt, &pa, 1, TRUE);
	eng_resume(e);
	return E_OK;
}

/*
 * A job before the common layer runs them: submitted and looked at
 * until it is over (or not)
 */
LOCAL ER run_direct( IGTENG *e, IGTCTX *c, UD start, BOOL first )
{
	UD	t0;

	direct_done = FALSE;
	job_error = FALSE;
	igt_fw_get();
	(void)emit(e, c, start, first);
	submit(e, c);
	t0 = igt_now_us();
	while ( !direct_done ) {
		if ( igt_now_us() - t0 > DIRECT_MS * 1000 ) {
			igt_eng_dump("did not end", e);
			igt_fw_put();
			return E_TMOUT;
		}
		look(NULL);
		tk_dly_tsk(1);
	}
	return job_error ? E_IO : E_OK;
}

/*
 * The engine's first context run once, with the context workarounds,
 * and the image the engine saved kept for every new context
 */
LOCAL ER golden( IGTENG *e )
{
	IGTCTX	*c;
	UD	t0 = igt_now_us();
	ER	er;

	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	c = ctx_new(knl_igt.kvm, (INT)( e - knl_igt.eng ));
	if ( c != NULL ) ctx_fresh(c);
	tk_unl_mtx(knl_igt.mtx);
	if ( c == NULL ) return E_NOMEM;
	knl_igt.kvm->ctx[0][c->eng] = c;

	er = run_direct(e, c, 0, TRUE);
	tm_printf((UB *)"igt: %s: the first context ran in %ld us (%d): sequence %d, status entries %d, "
		  "status page copy %08x %08x\n", e->name, igt_now_us() - t0, er, e->hwsp[HWS_SEQNO],
		  igt_erd(e, RING_CSB_PTR), e->hwsp[HWS_CSB0 + 2 * ( e->csb_head % CSB_ENTRIES )],
		  e->hwsp[HWS_CSB_WRITE]);
	if ( er < E_OK ) return er;

	e->golden = (UB *)Kmalloc(c->img_bytes);
	if ( e->golden == NULL ) return E_NOMEM;
	knl_memcpy(e->golden, c->kva, c->img_bytes);
	c->fresh = TRUE;			/* from the first image the next time */
	{
		UW	*r = (UW *)( e->golden + LRC_STATE_PAGE * PAGE_SIZE );

		tm_printf((UB *)"igt: %s: saved image: control %08x head %08x tail %08x start %08x ctl %08x "
			  "PDP0 %08x%08x MI_MODE %08x\n", e->name, r[CTX_CONTEXT_CONTROL], r[CTX_RING_HEAD],
			  r[CTX_RING_TAIL], r[CTX_RING_START], r[CTX_RING_CTL], r[CTX_PDP0_UDW], r[CTX_PDP0_LDW],
			  r[CTX_RING_MI_MODE + 1]);
	}
	if ( e == &knl_igt.eng[IGT_RCS] ) {
		tm_printf((UB *)"igt: context workarounds as read: COMMON_SLICE_CHICKEN2 %08x HDC_CHICKEN0 %08x "
			  "CACHE_MODE_1 %08x CS_CHICKEN1 %08x HALF_SLICE_CHICKEN7 %08x ROW_CHICKEN %08x\n",
			  igt_rd(COMMON_SLICE_CHICKEN2), igt_rd(HDC_CHICKEN0), igt_rd(CACHE_MODE_1),
			  igt_rd(CS_CHICKEN1), igt_rd(HALF_SLICE_CHICKEN7), igt_rd(ROW_CHICKEN));
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the common layer's backend */

LOCAL ER igt_info( void *priv, T_GPUINFO *info )
{
	(void)priv;
	info->kind = GPU_KIND_INTEL;
	info->ver = 90;
	info->rev = knl_igt.rev;
	info->ncores = (UINT)bits(knl_igt.slice_mask);
	info->nqpu = knl_igt.eu_total;
	info->feature = GPU_FEAT_BATCH | GPU_FEAT_MMU;
	info->va_size = 1ULL << 48;
	info->hub_ident[0] = knl_igt.devid;
	info->hub_ident[1] = knl_igt.fuse2;
	info->hub_ident[2] = knl_igt.eu_dis;
	info->hub_ident[3] = knl_igt.slice_mask | ( (UW)knl_igt.ss_mask << 8 );
	info->core_ident[0] = 12000000;			/* the command streamer's time stamp, Hz */
	info->core_ident[1] = (UW)( knl_igt.dev.ggtt_size >> 20 );
	info->core_ident[2] = 1;			/* the GPU and the processor share the LLC */
	return E_OK;
}

LOCAL ER igt_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	CONST T_GPUBATCH	*b = &job->u.batch;
	IGTVM	*vm = ( as != NULL ) ? (IGTVM *)as : knl_igt.kvm;
	IGTENG	*e;
	IGTCTX	*c;
	UD	start;

	(void)priv;
	if ( job->kind != GPU_JOB_BATCH ) return E_NOSPT;
	if ( b->engine >= IGT_NENG || b->hwc >= GPU_HWC_MAX || b->len == 0 ) return E_PAR;
	start = ( ( (UD)b->start_hi << 32 ) | b->start_lo ) & ( ( 1ULL << 48 ) - 1 );
	if ( start == 0 || ( start & 3 ) != 0 ) return E_PAR;
	e = &knl_igt.eng[b->engine];

	/*
	 * An error a job raised since the last told (the first 16): the
	 * command's header and where the engine was, so that the console says
	 * which command of a process's batch the engine refused
	 */
	{
		INT	k;

		for ( k = 0; k < IGT_NENG; k++ ) {
			IGTENG	*x = &knl_igt.eng[k];

			if ( x->irq_err > x->err_told ) {
				if ( x->err_told < 16 ) {
					tm_printf((UB *)"igt: %s: a job ended in an error (%ld so far): EIR %08x IPEHR %08x "
						  "IPEIR %08x INSTDONE %08x ACTHD %08x%08x BBADDR %08x%08x\n", x->name,
						  (long)x->irq_err, x->err_eir, x->err_ipehr, x->err_ipeir, x->err_instdone,
						  (UW)( x->err_acthd >> 32 ), (UW)x->err_acthd,
						  (UW)( x->err_bbaddr >> 32 ), (UW)x->err_bbaddr);
				}
				x->err_told = x->irq_err;
			}
		}
	}

	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	c = vm->ctx[b->hwc][b->engine];
	if ( c == NULL ) {
		c = ctx_new(vm, (INT)b->engine);
		vm->ctx[b->hwc][b->engine] = c;
	}
	if ( c != NULL && ( c->fresh || ( b->flags & GPU_BATCH_FRESH ) != 0 ) ) ctx_fresh(c);
	tk_unl_mtx(knl_igt.mtx);
	if ( c == NULL ) return E_NOMEM;

	/* the GT woken (out of RC6) and kept so until the job is over */
	igt_fw_get();
	job_error = FALSE;
	(void)emit(e, c, start, FALSE);
	submit(e, c);
	return E_OK;
}

/*
 * A job ran out of time: the engine stopped, reset alone and started
 * again; the context it ran is made anew before it runs again
 */
LOCAL void igt_reset( void *priv )
{
	INT	a;
	IGTENG	*e;
	IGTCTX	*c;
	UW	ctl;
	UINT	imask;
	BOOL	stopped, ready, done;

	(void)priv;
	/* the job is the reset's from now on: the interrupt and the look leave the engine be */
	ISpinLock(&knl_igt.lock, &imask);
	a = knl_igt.active;
	knl_igt.active = -1;
	c = ( a >= 0 ) ? knl_igt.eng[a].cur : NULL;
	if ( a >= 0 ) knl_igt.eng[a].cur = NULL;
	ISpinUnlock(&knl_igt.lock, &imask);
	if ( a < 0 ) {
		tm_printf((UB *)"igt: reset asked with no job on an engine\n");
		return;
	}
	e = &knl_igt.eng[a];
	igt_eng_dump("before the reset", e);

	igt_ewr(e, RING_MI_MODE, MASK_EN(STOP_RING));
	stopped = wait_for(e->base + RING_MI_MODE, MODE_IDLE, MODE_IDLE, 100000);
	ctl = igt_erd(e, RING_RESET_CTL);
	if ( ctl & RESET_CAT_ERROR ) {
		igt_ewr(e, RING_RESET_CTL, MASK_EN(RESET_CAT_ERROR));
		ready = wait_for(e->base + RING_RESET_CTL, RESET_CAT_ERROR, 0, 700000);
	} else {
		igt_ewr(e, RING_RESET_CTL, MASK_EN(RESET_REQUEST));
		ready = wait_for(e->base + RING_RESET_CTL, RESET_READY, RESET_READY, 700000);
	}
	igt_wr(GDRST, e->grdom);
	done = wait_for(GDRST, e->grdom, 0, 2000);
	igt_wr(GDRST, e->grdom);
	done = wait_for(GDRST, e->grdom, 0, 2000) && done;
	udelay(50);
	igt_ewr(e, RING_RESET_CTL, MASK_DIS(RESET_REQUEST));

	if ( c != NULL ) c->fresh = TRUE;
	eng_resume(e);
	/* the number the engine would have written, written for it */
	e->hwsp[HWS_SEQNO] = e->seqno;
	e->resets++;
	tm_printf((UB *)"igt: %s reset: ring %s, %s, GDRST %s\n", e->name, stopped ? "stopped" : "NOT stopped",
		  ready ? "ready" : "NOT ready", done ? "done" : "NOT done");
	igt_eng_dump("after the reset", e);
	igt_fw_put();				/* the job's, taken from it */
}

LOCAL CONST T_GPUOPS	igt_ops = {
	GPU_KIND_INTEL,
	GPU_FEAT_BATCH | GPU_FEAT_MMU,
	1ULL << 39,
	igt_info,
	igt_bo_bind,
	igt_bo_unbind,
	igt_kick,
	igt_reset,
	igt_as_create,
	igt_as_destroy,
	igt_as_bind,
	igt_as_unbind,
	igt_as_bind_at,
	igt_as_unbind_at,
	TRUE
};

/* A batch of the kernel's (in its space) on an engine, waited for */
EXPORT ER igt_kjob( INT eng, UD batch_va, UW len, T_GPUBO *CONST *bo, INT nbo )
{
	T_GPUJOB	j;
	D		f;

	knl_memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_BATCH;
	j.u.batch.start_lo = (UW)batch_va;
	j.u.batch.start_hi = (UW)( batch_va >> 32 );
	j.u.batch.len = len;
	j.u.batch.engine = (UW)eng;
	j.u.batch.hwc = 0;
	f = knl_gpu_submit(&j, bo, nbo);
	if ( f < 0 ) return (ER)f;
	return knl_gpu_wait(f, 4 * 1000 * 1000);
}

#if USE_KTEST
/*
 * For the tests on a machine without the device (tests/ktest/x86_64/
 * ktest_igt.c): the register state of a new context of an engine laid
 * in 'page', and the commands of a job written into a ring of the
 * caller's from 'tail' on, as they would go to the engine
 */
EXPORT void igt_test_layout( INT eng, UW *page )
{
	knl_memset(page, 0, PAGE_SIZE);
	lrc_layout(page, eng);
}

EXPORT UW igt_test_emit( INT eng, BOOL first, UD start, UB *ring, UW tail, UW hwsp_ggtt, UW *p_seq )
{
	IGTENG	e;
	IGTCTX	c;

	knl_memset(&e, 0, sizeof(e));
	knl_memset(&c, 0, sizeof(c));
	e.hwsp_ggtt = hwsp_ggtt;
	e.seqno = 0xfffffffe;			/* the number goes round past 0 */
	c.eng = eng;
	c.ring = ring;
	c.tail = tail;
	*p_seq = emit(&e, &c, start, first);
	return c.tail;
}
#endif

/* ---------------------------------------------------------------- the device object */

/*
 * The GPU as a device object "gpu0" (OB_S_GPU) in the list of the
 * devices: record 1 tells what it is and how it does, as "key value"
 * lines, made again and told as a change when the start-up has gone
 * through a step
 */
#define OBJ_TEXT	768

LOCAL struct {
	BOOL	added;
	TS_UUID	uuid;
	UB	text[OBJ_TEXT];
	INT	len;
	CONST char *state;
	INT	compose;	/* 0 the processor, 1 the copy engine, 2 the render engine */
} gob;

LOCAL INT kv_num( UB *t, INT n, CONST char *key, D v )
{
	n = knl_oj_put(t, n, OBJ_TEXT, key);
	n = knl_oj_put(t, n, OBJ_TEXT, " ");
	n = knl_oj_put_num(t, n, OBJ_TEXT, v);
	return knl_oj_put(t, n, OBJ_TEXT, "\n");
}

LOCAL INT kv( UB *t, INT n, CONST char *key, CONST char *s )
{
	n = knl_oj_put(t, n, OBJ_TEXT, key);
	n = knl_oj_put(t, n, OBJ_TEXT, " ");
	n = knl_oj_put(t, n, OBJ_TEXT, s);
	return knl_oj_put(t, n, OBJ_TEXT, "\n");
}

LOCAL void obj_text( void )
{
	UB	t[OBJ_TEXT];
	char	id[16];
	INT	n = 0, k;

	(void)tm_sprintf((UB *)id, (CONST UB *)"8086:%04x", knl_igt.devid);
	n = kv(t, n, "DEVICE", id);
	n = kv_num(t, n, "REVISION", knl_igt.rev);
	n = kv(t, n, "STATE", ( gob.state != NULL ) ? gob.state : "found");
	n = kv_num(t, n, "EU", knl_igt.eu_total);
	n = kv_num(t, n, "RP0_MHZ", knl_igt.rp0);
	n = kv_num(t, n, "RP1_MHZ", knl_igt.rp1);
	n = kv_num(t, n, "RPN_MHZ", knl_igt.rpn);
	n = kv_num(t, n, "RPE_MHZ", (D)knl_igt.f_eff * 50 / FREQ_SCALE);
	n = kv(t, n, "RC6", knl_igt.rc6_on ? "on" : "off");
	n = kv(t, n, "RPS", knl_igt.rps_on ? "on" : "off");
	for ( k = 0; k < IGT_NENG; k++ ) {
		n = kv_num(t, n, ( k == IGT_RCS ) ? "RENDER_JOBS" : "COPY_JOBS", (D)knl_igt.eng[k].jobs);
		n = kv_num(t, n, ( k == IGT_RCS ) ? "RENDER_RESETS" : "COPY_RESETS", (D)knl_igt.eng[k].resets);
	}
	n = kv(t, n, "INTERRUPT", ( knl_igt.intno != 0 && !knl_igt.irq_off ) ? "message" : "polled");
	n = kv(t, n, "COMPOSE", ( gob.compose == 2 ) ? "render engine" : ( gob.compose == 1 ) ? "copy engine"
				: "processor");
	gob.len = n;
	knl_memcpy(gob.text, t, n);
}

LOCAL INT ob_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(t, n, max, "<p>The GPU: Intel's integrated graphics, its render and copy engines "
		       "(OpenGL ES of the processes, compositing the windows). Record 1: its state as key value "
		       "lines; a change is told on it.</p>");
	return n;
}

LOCAL INT ob_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(j, n, max, ",\"pci\":");
	return knl_oj_put_str(j, n, max, (CONST UB *)"00:02.0");
}

LOCAL ER ob_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	SZ	k;

	(void)ctx;
	(void)pos;
	if ( recno != OB_GP_STATE ) return E_NOEXS;
	k = ( off < gob.len ) ? gob.len - off : 0;
	if ( size >= 0 && k > size ) k = size;
	if ( size > 0 && k > 0 ) knl_memcpy(buf, gob.text + off, (INT)k);
	if ( p_asize != NULL ) *p_asize = ( size == 0 ) ? gob.len : k;
	return E_OK;
}

LOCAL UD ob_size( void *ctx, INT recno )
{
	(void)ctx;
	return ( recno == OB_GP_STATE ) ? (UD)gob.len : 0;
}

LOCAL CONST T_OBDVOPS	gpu_ops = { OB_S_GPU, "gpu", OB_GP_NREC, 0644, 0, 0, ob_text, ob_attr, ob_rea, NULL, ob_size, NULL };

/* The state told: the object made the first time, the change told after */
LOCAL void obj_state( CONST char *state )
{
	if ( state != NULL ) gob.state = state;
	obj_text();
	if ( !gob.added ) {
		if ( knl_obdev_add((CONST UB *)"gpu0", NULL, &gpu_ops, NULL, &gob.uuid) >= E_OK ) gob.added = TRUE;
		return;
	}
	knl_obdev_post(&gob.uuid, OB_GP_STATE, OB_E_CHANGE, NULL);
}

/* ---------------------------------------------------------------- start-up */

LOCAL INT	igt_mode;

LOCAL void step( CONST char *s )
{
	tm_printf((UB *)"igt: - %s\n", s);
}

/* The files on the EFI system partition written now, and time for it */
LOCAL void keep( void )
{
	if ( igt_mode == 2 ) {
		knl_bootlog_soon();
		tk_dly_tsk(4000);
	}
}

LOCAL ER bring_up( void )
{
	T_CCYC	cc;
	ER	er;
	INT	k;

	step("forcewake");
	if ( !igt_fw_init() ) return E_IO;
	step("power off for now");
	igt_pm_init();
	step("topology");
	topology();
	step("workarounds, PAT, page tables");
	gt_workarounds();
	ppat();
	er = igt_vm_init();
	if ( er < E_OK ) return er;
	knl_igt.kvm = igt_vm_create(TRUE);
	if ( knl_igt.kvm == NULL ) return E_NOMEM;

	knl_igt.eng[IGT_RCS].name = "rcs";
	knl_igt.eng[IGT_RCS].base = RCS_BASE;
	knl_igt.eng[IGT_RCS].shift = GT_RCS_SHIFT;
	knl_igt.eng[IGT_RCS].grdom = GRDOM_RENDER;
	knl_igt.eng[IGT_RCS].img_bytes = 22 * PAGE_SIZE;
	knl_igt.eng[IGT_BCS].name = "bcs";
	knl_igt.eng[IGT_BCS].base = BCS_BASE;
	knl_igt.eng[IGT_BCS].shift = GT_BCS_SHIFT;
	knl_igt.eng[IGT_BCS].grdom = GRDOM_BLT;
	knl_igt.eng[IGT_BCS].img_bytes = 2 * PAGE_SIZE;
	knl_igt.active = -1;
	for ( k = 0; k < IGT_NENG; k++ ) {
		step(( k == IGT_RCS ) ? "render engine" : "copy engine");
		er = eng_init(&knl_igt.eng[k]);
		if ( er < E_OK ) return er;
	}

	/* a look every millisecond, whether or not interrupts come */
	cc.exinf = NULL;
	cc.cycatr = TA_HLNG | TA_STA;
	cc.cychdr = (FP)look;
	cc.cyctim = 1;
	cc.cycphs = 1;
	knl_igt.cyc = tk_cre_cyc(&cc);

	step("interrupts");
	tm_printf((UB *)"igt: before: master %08x, GT IER %08x %08x %08x %08x, display IER pipes %08x %08x %08x "
		  "port %08x misc %08x, PCU %08x\n", igt_rd(MASTER_IRQ), igt_rd(GT_IER(0)), igt_rd(GT_IER(1)),
		  igt_rd(GT_IER(2)), igt_rd(GT_IER(3)), igt_rd(DE_PIPE_IER(0)), igt_rd(DE_PIPE_IER(1)),
		  igt_rd(DE_PIPE_IER(2)), igt_rd(DE_PORT_IER), igt_rd(DE_MISC_IER), igt_rd(PCU_IER));
	igt_wr(MASTER_IRQ, 0);
	for ( k = 1; k < 4; k++ ) {
		igt_wr(GT_IER(k), 0);
		igt_wr(GT_IMR(k), 0xffffffff);
		igt_wr(GT_IIR(k), 0xffffffff);
	}
	{
		UW	want = ( ( GT_USER | GT_CTX_SWITCH | GT_CS_ERROR ) << GT_RCS_SHIFT )
			     | ( ( GT_USER | GT_CTX_SWITCH | GT_CS_ERROR ) << GT_BCS_SHIFT );

		igt_wr(GT_IMR(0), ~want);
		igt_wr(GT_IER(0), want);
		igt_wr(GT_IIR(0), 0xffffffff);
		igt_wr(GT_IIR(0), 0xffffffff);
	}
	if ( ts_pcie_msi_plain_alloc(&knl_igt.dev.pci, &knl_igt.intno) >= E_OK ) {
		knl_define_inthdr((INT)knl_igt.intno, TA_HLNG, (FP)isr);
		EnableInt(knl_igt.intno, INTPRI_DEVICE);
		igt_wr(MASTER_IRQ, MASTER_IRQ_CONTROL);
		tm_printf((UB *)"igt: interrupt %d (a message)\n", knl_igt.intno);
	} else {
		knl_igt.intno = 0;
		tm_printf((UB *)"igt: no message interrupt: the engines are looked at every millisecond\n");
	}
	keep();

	for ( k = 0; k < IGT_NENG; k++ ) {
		step(( k == IGT_RCS ) ? "the render engine's first context" : "the copy engine's first context");
		er = golden(&knl_igt.eng[k]);
		if ( er < E_OK ) return er;
		{
			/* the mask read back once the engine has run a context: it is not part of the image */
			IGTENG	*e = &knl_igt.eng[k];

			tm_printf((UB *)"igt: %s interrupt mask after its first context: reads %08x%s\n",
				  e->name, igt_erd(e, RING_IMR), ( igt_erd(e, RING_IMR) == eng_imr(e) ) ? "" : " (changed)");
		}
	}

	step("RC6 and RPS");
	keep();
	igt_pm_start();
	keep();

	step("registered");
	knl_igt.ready = TRUE;
	er = knl_gpu_register(&igt_ops, &knl_igt);
	if ( er < E_OK ) {
		knl_igt.ready = FALSE;
		return er;
	}
	knl_igt_ggtt_report();
	keep();
	igt_fw_put();				/* the start-up's: the GT may sleep from now on */
	return E_OK;
}

LOCAL void start_task( INT stacd, void *exinf )
{
	ER	er;

	(void)stacd;
	(void)exinf;
	/*
	 * Brought up for trial only once the record files on the EFI system
	 * partition hold the start of the machine: should the GPU stop the
	 * machine, what came before is there
	 */
	if ( igt_mode == 2 ) {
		tk_dly_tsk(30000);
		knl_bootlog_soon();
		tk_dly_tsk(4000);
	} else {
		tk_dly_tsk(2000);
	}
	obj_state("starting");
	er = bring_up();
	if ( er < E_OK ) {
		tm_printf((UB *)"igt: the GPU is NOT used (%d)\n", er);
		obj_state("not used");
		knl_bootlog_soon();
		tk_ext_tsk();
	}
	obj_state("running");
	tm_printf((UB *)"igt: ready: render and copy engines, %d execution units, %s\n", knl_igt.eu_total,
		  ( knl_igt.intno != 0 ) ? "interrupts" : "no interrupts");
	if ( igt_mode == 2 ) {
		igt_test_start();
	}
	if ( igt_compose_init() >= E_OK ) {
		(void)igt_compose_check();
		gob.compose = igt_compose_used();
	}
	obj_state(NULL);
	knl_bootlog_soon();
	tk_ext_tsk();
}

/*
 * From the start-up of the devices (devinit.c): the device looked at;
 * what writes to it is done by a task of its own, later
 *
 *   igt=0	nothing
 *   igt=1	the device and its fuses read and told, nothing written
 *   igt=2	(for now the default) brought up step by step after the
 *		first record files, the tests at start-up, the check of
 *		compositing
 *   igt=3	brought up two seconds after, the check of compositing only
 */
EXPORT ER knl_igt_init( void )
{
	T_CTSK	ct;
	T_CMTX	cm;
	ID	tid;
	ER	er;

	igt_mode = igt_setting("igt", 2);
	if ( igt_mode == 0 ) return E_NOSPT;
	er = knl_igt_dev(&knl_igt.dev);
	if ( er < E_OK ) return er;
	knl_igt.mmio = knl_igt.dev.mmio;
	knl_igt.devid = knl_igt.dev.pci.device;
	knl_igt.rev = (UB)ts_pcie_cfg_read(knl_igt.dev.pci.bus, knl_igt.dev.pci.dev, knl_igt.dev.pci.fn, 0x08, 1);
	knl_igt.pre_c0 = ( ( knl_igt.devid & 0xff00 ) == 0x5900 && knl_igt.rev < 2 );
	if ( ( knl_igt.devid & 0xff00 ) != 0x5900 && knl_igt.devid != 0x87c0 ) {
		tm_printf((UB *)"igt: %04x is not a Kaby Lake or an Amber Lake: the engines are not used\n",
			  knl_igt.devid);
		return E_NOSPT;
	}
	tm_printf((UB *)"igt: render and copy engines of %04x rev %02x%s; igt=%d\n", knl_igt.devid, knl_igt.rev,
		  knl_igt.pre_c0 ? " (before C0)" : "", igt_mode);
	if ( igt_mode == 1 ) return E_OK;

	ts_pcie_enable(&knl_igt.dev.pci);
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	knl_igt.mtx = tk_cre_mtx(&cm);
	if ( knl_igt.mtx <= 0 ) return E_LIMIT;
	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)start_task;
	ct.itskpri = 30;
	ct.stksz = 16384;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) return E_LIMIT;
	return E_OK;
}

#endif /* X64 */
