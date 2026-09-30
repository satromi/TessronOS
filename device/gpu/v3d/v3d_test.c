/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	v3d_test.c
 *	Tests of the V3D on the board (docs/tessronos/13-gpu.md 13.8.2).
 *
 *	Built with V3D=2, they run once in a task of their own after the
 *	driver has set the GPU up, and write "v3d test:" lines to the
 *	console: one line a test, then the driver's counts and a summary.
 *	Every job has a time limit and the common layer resets the GPU when
 *	a job does not end, so a test that fails lets the next one run and
 *	the system go on starting. The tests that could leave the GPU in a
 *	bad state (the overflow and the MMU fault) come last, followed by a
 *	job that shows whether the GPU recovered.
 *
 *	The jobs are the smallest frames the hardware renders: a binner
 *	list with no draws and a renderer list that clears the tile buffer
 *	to one colour (or loads it from memory) and stores every tile to a
 *	raster buffer in memory. The packets are packed by the code
 *	generated from Mesa's packet description (mesa/, MIT).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "../gpudev.h"
#include "v3d.h"

#ifndef CNF_V3D
#define CNF_V3D		0
#endif

/* The generated packing: here a GPU address is a plain 32 bit number */
#define __gen_user_data		void
#define __gen_address_type	UW
#define __gen_address_offset(a)	(*(a))
#define __gen_emit_reloc(d, a)	((void)0)
#include "mesa/v3d_packet_v71_pack.h"

#define TEST_PRI		30		/* below everything that starts the system */
#define TEST_STKSZ		16384
#define TEST_DELAY_MS		3000		/* let the boot messages go by first */
#define JOB_TMO_US		( 3 * 1000 * 1000 )	/* the common layer resets at 2 s */

/*
 * One render target of 32 bits a pixel (RGBA8UI) and no multisampling
 * fits the colour tile buffer at the largest tile size, 64 x 64.
 */
#define TILE			64
#define LOG2_TILE_ENUM		3		/* 8 << 3 = 64 */
#define MAX_SUPERTILES		256
#define TILE_ALLOC_BLOCK	128		/* the initial block of each tile's list */
#define TILE_ALLOC_BLOCK_ENUM	1		/* 128 bytes */
#define TILE_ALLOC_MORE_ENUM	0		/* 64 byte blocks when a list grows */
#define TILE_STATE_BYTES	256		/* tile state data a tile */

/* Where the lists sit in their BO */
#define LISTS_BYTES		( 32 * 1024 )
#define BCL_AT			0
#define RCL_AT			4096
#define GTL_AT			( 12 * 1024 )	/* the generic tile list */
#define RCL_ROOM		( GTL_AT - RCL_AT )
#define GTL_ROOM		4096

/* An address the driver never hands out: accesses there fault */
#define INVALID_GPU_ADDR	0x8000U

/* ---------------------------------------------------------------- building lists */

typedef struct {
	UB	*buf;		/* built here, then copied into the BO */
	UW	room;
	UW	len;
	UW	base;		/* GPU address of buf[0] */
	BOOL	over;
} CL;

LOCAL UB *cl_space( CL *cl, UW n )
{
	UB	*p;

	if ( cl->len + n > cl->room ) {
		cl->over = TRUE;
		return NULL;
	}
	p = cl->buf + cl->len;
	cl->len += n;
	return p;
}

LOCAL UW cl_addr( CL *cl )
{
	return cl->base + cl->len;
}

/* Pack one packet with its header's defaults and the fields given */
#define EMIT(cl, pkt, ...) do { \
		struct V3D71_##pkt _v = { V3D71_##pkt##_header, __VA_ARGS__ }; \
		UB *_p = cl_space((cl), V3D71_##pkt##_length); \
		if ( _p != NULL ) V3D71_##pkt##_pack(NULL, _p, &_v); \
	} while ( 0 )

/* ---------------------------------------------------------------- the memory of BOs */

LOCAL void *bo_va( T_GPUBO *bo, UD off )
{
	return (UB *)PA2VA(knl_gpu_bo_page(bo, off >> PAGE_SHIFT)) + ( off & ( PAGE_SIZE - 1 ) );
}

/* Copy into a BO through the cache, then write the lines to memory */
LOCAL void bo_put( T_GPUBO *bo, UD off, CONST void *src, UD len )
{
	CONST UB	*s = src;

	while ( len > 0 ) {
		UD	in = off & ( PAGE_SIZE - 1 );
		UD	n = PAGE_SIZE - in;
		void	*d = bo_va(bo, off);

		if ( n > len ) n = len;
		knl_memcpy(d, s, n);
		knl_gpu_dcache_clean_inval((UBINT)d, n);
		off += n;
		s += n;
		len -= n;
	}
}

/* Forget what the cache holds of a BO, so that reads see memory */
LOCAL void bo_fresh( T_GPUBO *bo, UD off, UD len )
{
	while ( len > 0 ) {
		UD	in = off & ( PAGE_SIZE - 1 );
		UD	n = PAGE_SIZE - in;

		if ( n > len ) n = len;
		knl_gpu_dcache_clean_inval((UBINT)bo_va(bo, off), n);
		off += n;
		len -= n;
	}
}

LOCAL UW bo_word( T_GPUBO *bo, UD i )
{
	return *(volatile UW *)bo_va(bo, i * 4);
}

LOCAL void bo_set_word( T_GPUBO *bo, UD i, UW v )
{
	*(volatile UW *)bo_va(bo, i * 4) = v;
}

LOCAL UW bo_gpu( T_GPUBO *bo )
{
	T_GPUBOINFO	bi;

	knl_gpu_bo_ref(bo, &bi);
	return bi.offset;
}

/* ---------------------------------------------------------------- time */

LOCAL UD ticks( void )
{
	UD	v;

	Asm("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
	return v;
}

LOCAL UD ticks_us( UD t )
{
	UD	f;

	Asm("mrs %0, cntfrq_el0" : "=r"(f));
	return ( f == 0 ) ? 0 : t * 1000000 / f;
}

/* ---------------------------------------------------------------- one frame */

typedef struct {
	UINT	w, h;		/* pixels */
	BOOL	fill;		/* clear to 'color' (otherwise load from 'src') */
	UW	color;
	UW	src;		/* GPU address of the raster to load */
	UW	dst;		/* GPU address of the raster to store */
	BOOL	small_qms;	/* tell the binner less tile memory than it needs */
} FRAME;

typedef struct {
	T_GPUBO	*lists;
	T_GPUBO	*tile_alloc;
	T_GPUBO	*tile_state;
	T_GPUJOB job;
} FRAMEJOB;

LOCAL UB	stage[LISTS_BYTES];		/* lists are built here */

LOCAL void frame_free( FRAMEJOB *fj )
{
	if ( fj->lists != NULL ) knl_gpu_bo_put(fj->lists);
	if ( fj->tile_alloc != NULL ) knl_gpu_bo_put(fj->tile_alloc);
	if ( fj->tile_state != NULL ) knl_gpu_bo_put(fj->tile_state);
	fj->lists = fj->tile_alloc = fj->tile_state = NULL;
}

/*
 * The binner list, the renderer list and the generic tile list of one
 * frame, with the BOs they need. The frame is binned with no draws, so
 * each tile's list holds nothing and the renderer runs the generic
 * list once a tile.
 */
LOCAL ER frame_build( FRAMEJOB *fj, CONST FRAME *f )
{
	UINT	tx, ty, sw, sh, fsw, fsh, i, x, y, maxx, maxy;
	UW	ta_size, ts_size, lists, gtl_start, gtl_end;
	CL	bcl, rcl, gtl;
	UB	*zs;
	UW	one = 0x3F800000U;		/* 1.0f, the depth clear value */
	ER	er;

	knl_memset(fj, 0, sizeof(*fj));
	tx = ( f->w + TILE - 1 ) / TILE;
	ty = ( f->h + TILE - 1 ) / TILE;

	/* Supertiles grow until there are no more than the hardware takes */
	sw = sh = 1;
	for (;;) {
		fsw = ( tx + sw - 1 ) / sw;
		fsh = ( ty + sh - 1 ) / sh;
		if ( fsw < MAX_SUPERTILES && fsh < MAX_SUPERTILES && fsw * fsh <= MAX_SUPERTILES ) {
			break;
		}
		if ( sw < sh ) sw++; else sh++;
	}

	/* The initial blocks, rounded to 4KB, and the two 4KB chunks the binner takes first */
	ta_size = ( ( tx * ty * TILE_ALLOC_BLOCK + 4095 ) & ~4095U ) + 8192;
	ts_size = tx * ty * TILE_STATE_BYTES;

	fj->lists = knl_gpu_bo_create(LISTS_BYTES, 0, &er);
	if ( fj->lists == NULL ) goto fail;
	fj->tile_alloc = knl_gpu_bo_create(ta_size, 0, &er);
	if ( fj->tile_alloc == NULL ) goto fail;
	fj->tile_state = knl_gpu_bo_create(ts_size, 0, &er);
	if ( fj->tile_state == NULL ) goto fail;
	lists = bo_gpu(fj->lists);

	knl_memset(stage, 0, sizeof(stage));
	bcl = (CL){ stage + BCL_AT, RCL_AT - BCL_AT, 0, lists + BCL_AT, FALSE };
	rcl = (CL){ stage + RCL_AT, RCL_ROOM, 0, lists + RCL_AT, FALSE };
	gtl = (CL){ stage + GTL_AT, GTL_ROOM, 0, lists + GTL_AT, FALSE };

	/* the binner: set up the tiles, bin nothing, flush */
	EMIT(&bcl, NUMBER_OF_LAYERS, .number_of_layers = 1);
	EMIT(&bcl, TILE_BINNING_MODE_CFG,
	     .width_in_pixels = f->w,
	     .height_in_pixels = f->h,
	     .log2_tile_width = LOG2_TILE_ENUM,
	     .log2_tile_height = LOG2_TILE_ENUM,
	     .tile_allocation_initial_block_size = TILE_ALLOC_BLOCK_ENUM,
	     .tile_allocation_block_size = TILE_ALLOC_MORE_ENUM);
	EMIT(&bcl, FLUSH_VCD_CACHE);
	EMIT(&bcl, START_TILE_BINNING);
	EMIT(&bcl, FLUSH);

	/* the renderer: the frame and its one render target */
	EMIT(&rcl, TILE_RENDERING_MODE_CFG_COMMON,
	     .early_z_disable = true,
	     .image_width_pixels = f->w,
	     .image_height_pixels = f->h,
	     .number_of_render_targets = 1,
	     .log2_tile_width = LOG2_TILE_ENUM,
	     .log2_tile_height = LOG2_TILE_ENUM,
	     .internal_depth_type = V3D_INTERNAL_TYPE_DEPTH_32F);
	EMIT(&rcl, TILE_RENDERING_MODE_CFG_RENDER_TARGET_PART1,
	     .clear_color_low_bits = f->fill ? f->color : 0,
	     .internal_bpp = V3D_INTERNAL_BPP_32,
	     .internal_type_and_clamping = V3D_RENDER_TARGET_TYPE_CLAMP_8UI_CLAMPED,
	     .stride = TILE * 1 / 2,		/* 128 bit units covering two rows */
	     .base_address = 0,
	     .render_target_number = 0);
	zs = rcl.buf + rcl.len;
	EMIT(&rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, .stencil_clear_value = 0);
	if ( !rcl.over ) knl_memcpy(zs + 3, &one, 4);	/* the float field, packed by hand */
	EMIT(&rcl, TILE_LIST_INITIAL_BLOCK_SIZE,
	     .use_auto_chained_tile_lists = true,
	     .size_of_first_block_in_chained_tile_lists = TILE_ALLOC_BLOCK_ENUM);
	EMIT(&rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, .address = bo_gpu(fj->tile_alloc));
	EMIT(&rcl, MULTICORE_RENDERING_SUPERTILE_CFG,
	     .number_of_bin_tile_lists = 1,
	     .total_frame_width_in_tiles = tx,
	     .total_frame_height_in_tiles = ty,
	     .supertile_width_in_tiles = sw,
	     .supertile_height_in_tiles = sh,
	     .total_frame_width_in_supertiles = fsw,
	     .total_frame_height_in_supertiles = fsh);

	/* two dummy tiles first, the first one clearing the tile buffer */
	for ( i = 0; i < 2; i++ ) {
		EMIT(&rcl, TILE_COORDINATES, .tile_row_number = 0, .tile_column_number = 0);
		EMIT(&rcl, END_OF_LOADS);
		EMIT(&rcl, STORE_TILE_BUFFER_GENERAL, .buffer_to_store = NONE);
		if ( f->fill && i == 0 ) {
			EMIT(&rcl, CLEAR_RENDER_TARGETS);
		}
		EMIT(&rcl, END_OF_TILE_MARKER);
	}
	EMIT(&rcl, FLUSH_VCD_CACHE);

	/* what every tile does: (load,) run its binned list, store */
	gtl_start = cl_addr(&gtl);
	EMIT(&gtl, TILE_COORDINATES_IMPLICIT);
	if ( !f->fill ) {
		EMIT(&gtl, LOAD_TILE_BUFFER_GENERAL,
		     .buffer_to_load = RENDER_TARGET_0,
		     .address = f->src,
		     .input_image_format = V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI,
		     .memory_format = V3D_MEMORY_FORMAT_RASTER,
		     .height_in_ub_or_stride = f->w * 4,
		     .decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0);
	}
	EMIT(&gtl, END_OF_LOADS);
	EMIT(&gtl, BRANCH_TO_IMPLICIT_TILE_LIST);
	EMIT(&gtl, STORE_TILE_BUFFER_GENERAL,
	     .buffer_to_store = RENDER_TARGET_0,
	     .address = f->dst,
	     .clear_buffer_being_stored = false,
	     .output_image_format = V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI,
	     .memory_format = V3D_MEMORY_FORMAT_RASTER,
	     .height_in_ub_or_stride = f->w * 4,
	     .decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0);
	EMIT(&gtl, END_OF_TILE_MARKER);
	EMIT(&gtl, RETURN_FROM_SUB_LIST);
	gtl_end = cl_addr(&gtl);

	EMIT(&rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, .start = gtl_start, .end = gtl_end);
	maxx = ( f->w - 1 ) / ( TILE * sw );
	maxy = ( f->h - 1 ) / ( TILE * sh );
	for ( y = 0; y <= maxy; y++ ) {
		for ( x = 0; x <= maxx; x++ ) {
			EMIT(&rcl, SUPERTILE_COORDINATES,
			     .row_number_in_supertiles = y,
			     .column_number_in_supertiles = x);
		}
	}
	EMIT(&rcl, END_OF_RENDERING);

	if ( bcl.over || rcl.over || gtl.over ) {
		tm_printf((UB *)"v3d test: the lists do not fit\n");
		er = E_LIMIT;
		goto fail;
	}
	bo_put(fj->lists, 0, stage, LISTS_BYTES);

	fj->job.kind = GPU_JOB_CL;
	fj->job.u.cl.bcl_start = bcl.base;
	fj->job.u.cl.bcl_end = cl_addr(&bcl);
	fj->job.u.cl.rcl_start = rcl.base;
	fj->job.u.cl.rcl_end = cl_addr(&rcl);
	fj->job.u.cl.qma = bo_gpu(fj->tile_alloc);
	fj->job.u.cl.qms = f->small_qms ? ( ( tx * ty * TILE_ALLOC_BLOCK + 4095 ) & ~4095U ) : ta_size;
	fj->job.u.cl.qts = bo_gpu(fj->tile_state);
	fj->job.u.cl.flags = GPU_CL_FLUSH_CACHE;
	return E_OK;

fail:
	frame_free(fj);
	return er;
}

/* Submit a built frame that writes 'dst' (and reads 'src') */
LOCAL D frame_submit( FRAMEJOB *fj, T_GPUBO *dst, T_GPUBO *src )
{
	T_GPUBO	*bos[5];
	INT	n = 0;

	bos[n++] = fj->lists;
	bos[n++] = fj->tile_alloc;
	bos[n++] = fj->tile_state;
	if ( dst != NULL ) bos[n++] = dst;
	if ( src != NULL ) bos[n++] = src;
	return knl_gpu_submit(&fj->job, bos, n);
}

/* Build, run and wait for one frame; answers the job's result */
LOCAL ER frame_run( CONST FRAME *f, T_GPUBO *dst, T_GPUBO *src, UD *p_us )
{
	FRAMEJOB	fj;
	UD		t0;
	D		fence;
	ER		er;

	er = frame_build(&fj, f);
	if ( er < E_OK ) {
		return er;
	}
	t0 = ticks();
	fence = frame_submit(&fj, dst, src);
	er = ( fence > 0 ) ? knl_gpu_wait(fence, JOB_TMO_US) : (ER)fence;
	if ( p_us != NULL ) *p_us = ticks_us(ticks() - t0);
	frame_free(&fj);
	return er;
}

/* ---------------------------------------------------------------- checking results */

/* Words of the raster that are not 'want', and of the guard after it that are not 0 */
LOCAL UD count_bad( T_GPUBO *bo, UD words, UW want, UD guard, UD *p_guard_bad, UD *p_first )
{
	UD	i, bad = 0, gbad = 0;

	*p_first = (UD)-1;
	bo_fresh(bo, 0, ( words + guard ) * 4);
	for ( i = 0; i < words; i++ ) {
		if ( bo_word(bo, i) != want ) {
			if ( bad == 0 ) *p_first = i;
			bad++;
		}
	}
	for ( ; i < words + guard; i++ ) {
		if ( bo_word(bo, i) != 0 ) gbad++;
	}
	*p_guard_bad = gbad;
	return bad;
}

#define NTESTS		12

LOCAL struct {
	CONST char	*name;
	BOOL		ok;
} result[NTESTS];
LOCAL INT	nresult;

LOCAL void note( CONST char *name, BOOL ok )
{
	if ( nresult < NTESTS ) {
		result[nresult].name = name;
		result[nresult].ok = ok;
		nresult++;
	}
}

LOCAL CONST char *ername( ER er )
{
	switch ( er ) {
	case E_OK:	return "E_OK";
	case E_IO:	return "E_IO";
	case E_TMOUT:	return "E_TMOUT";
	case E_NOMEM:	return "E_NOMEM";
	case E_LIMIT:	return "E_LIMIT";
	default:	return "error";
	}
}

/* ---------------------------------------------------------------- the tests */

/* Clear a w x h frame to one colour and read it back; the page after it stays 0 */
LOCAL BOOL test_fill( CONST char *name, UINT w, UINT h, UW color )
{
	FRAME	f;
	T_GPUBO	*dst;
	UD	words = (UD)w * h, guard = PAGE_SIZE / 4, gbad, first, bad, us = 0;
	ER	er;
	BOOL	ok;

	dst = knl_gpu_bo_create(words * 4 + PAGE_SIZE, 0, &er);
	if ( dst == NULL ) {
		tm_printf((UB *)"v3d test: %s FAIL: no BO (%d)\n", name, er);
		note(name, FALSE);
		return FALSE;
	}
	knl_memset(&f, 0, sizeof(f));
	f.w = w;
	f.h = h;
	f.fill = TRUE;
	f.color = color;
	f.dst = bo_gpu(dst);
	er = frame_run(&f, dst, NULL, &us);
	bad = count_bad(dst, words, color, guard, &gbad, &first);
	ok = ( er == E_OK && bad == 0 && gbad == 0 );
	tm_printf((UB *)"v3d test: %s %dx%d %s: job %s, %ld of %ld pixels wrong",
		  name, w, h, ok ? "ok" : "FAIL", ername(er), bad, words);
	if ( bad != 0 ) {
		tm_printf((UB *)" (first at %ld: %08x)", first, bo_word(dst, first));
	}
	tm_printf((UB *)", %ld written past the end, %ld us", gbad, us);
	if ( us != 0 ) {
		tm_printf((UB *)", %ld MB/s", words * 4 / us);
	}
	tm_printf((UB *)", first page %lx\n", knl_gpu_bo_page(dst, 0));
	knl_gpu_bo_put(dst);
	note(name, ok);
	return ok;
}

/* Load a raster the processor wrote and store it to another */
LOCAL void test_copy( void )
{
	CONST UINT	w = 256, h = 256;
	UD		words = (UD)w * h, i, bad = 0, us = 0;
	T_GPUBO		*src, *dst;
	FRAME		f;
	ER		er;
	BOOL		ok;

	src = knl_gpu_bo_create(words * 4, 0, &er);
	dst = knl_gpu_bo_create(words * 4, 0, &er);
	if ( src == NULL || dst == NULL ) {
		tm_printf((UB *)"v3d test: copy FAIL: no BO\n");
		goto out;
	}
	for ( i = 0; i < words; i++ ) {
		bo_set_word(src, i, (UW)( i * 0x9E3779B1U ));
	}
	bo_fresh(src, 0, words * 4);
	knl_memset(&f, 0, sizeof(f));
	f.w = w;
	f.h = h;
	f.src = bo_gpu(src);
	f.dst = bo_gpu(dst);
	er = frame_run(&f, dst, src, &us);
	bo_fresh(dst, 0, words * 4);
	for ( i = 0; i < words; i++ ) {
		if ( bo_word(dst, i) != (UW)( i * 0x9E3779B1U ) ) bad++;
	}
	ok = ( er == E_OK && bad == 0 );
	tm_printf((UB *)"v3d test: copy %dx%d %s: job %s, %ld of %ld pixels wrong, %ld us\n",
		  w, h, ok ? "ok" : "FAIL", ername(er), bad, words, us);
	note("copy", ok);
	goto done;
out:
	note("copy", FALSE);
done:
	if ( src != NULL ) knl_gpu_bo_put(src);
	if ( dst != NULL ) knl_gpu_bo_put(dst);
}

/* Four frames submitted at once end in order, each with its own colour */
LOCAL void test_queue( void )
{
	FRAMEJOB	fj[4];
	T_GPUBO		*dst[4];
	D		fence[4];
	UD		gbad, first, bad = 0;
	FRAME		f;
	INT		i, built = 0;
	ER		er = E_OK;
	BOOL		ok, order = TRUE;

	knl_memset(dst, 0, sizeof(dst));
	knl_memset(fj, 0, sizeof(fj));
	for ( i = 0; i < 4; i++ ) {
		dst[i] = knl_gpu_bo_create(64 * 64 * 4, 0, &er);
		if ( dst[i] == NULL ) goto out;
		knl_memset(&f, 0, sizeof(f));
		f.w = f.h = 64;
		f.fill = TRUE;
		f.color = 0x01010101U * (UW)( i + 1 );
		f.dst = bo_gpu(dst[i]);
		er = frame_build(&fj[i], &f);
		if ( er < E_OK ) goto out;
		built++;
	}
	for ( i = 0; i < 4; i++ ) {
		fence[i] = frame_submit(&fj[i], dst[i], NULL);
		if ( i > 0 && fence[i] <= fence[i - 1] ) order = FALSE;
	}
	er = ( fence[3] > 0 ) ? knl_gpu_wait(fence[3], 4 * JOB_TMO_US) : (ER)fence[3];
	for ( i = 0; i < 3 && er == E_OK; i++ ) {
		if ( knl_gpu_wait(fence[i], 0) != E_OK ) er = E_IO;
	}
	for ( i = 0; i < 4; i++ ) {
		bad += count_bad(dst[i], 64 * 64, 0x01010101U * (UW)( i + 1 ), 0, &gbad, &first);
	}
out:
	ok = ( built == 4 && er == E_OK && bad == 0 && order );
	tm_printf((UB *)"v3d test: queue of 4 %s: last %s, %ld pixels wrong, fences %s\n",
		  ok ? "ok" : "FAIL", ername(er), bad, order ? "in order" : "OUT OF ORDER");
	for ( i = 0; i < built; i++ ) frame_free(&fj[i]);
	for ( i = 0; i < 4; i++ ) {
		if ( dst[i] != NULL ) knl_gpu_bo_put(dst[i]);
	}
	note("queue", ok);
}

/*
 * Whether the GPU and the processor's caches see each other. The
 * source holds A in memory and B only in the processor's cache; the
 * destination's lines are in the cache before the GPU writes it.
 */
LOCAL void test_coherence( void )
{
	CONST UINT	w = 64, h = 64;
	CONST UD	words = (UD)w * h;
	T_GPUBO		*src, *dst;
	UD		i, na = 0, nb = 0, nz = 0, fa = 0, fb = 0;
	volatile UW	sink = 0;
	FRAME		f;
	ER		er;
	BOOL		ok;

	src = knl_gpu_bo_create(words * 4, 0, &er);
	dst = knl_gpu_bo_create(words * 4, 0, &er);
	if ( src == NULL || dst == NULL ) {
		tm_printf((UB *)"v3d test: coherence FAIL: no BO\n");
		note("coherence", FALSE);
		goto done;
	}
	for ( i = 0; i < words; i++ ) bo_set_word(src, i, 0xAAAA0000U | (UW)i);
	bo_fresh(src, 0, words * 4);			/* memory: A */
	for ( i = 0; i < words; i++ ) bo_set_word(src, i, 0xBBBB0000U | (UW)i);	/* cache only: B */
	for ( i = 0; i < words; i++ ) sink += bo_word(dst, i);	/* the destination's lines cached */

	knl_memset(&f, 0, sizeof(f));
	f.w = w;
	f.h = h;
	f.src = bo_gpu(src);
	f.dst = bo_gpu(dst);
	er = frame_run(&f, dst, src, NULL);

	/* before forgetting the cache: what the processor sees at once */
	for ( i = 0; i < words; i++ ) {
		UW v = bo_word(dst, i);
		if ( v == ( 0xAAAA0000U | (UW)i ) ) na++;
		else if ( v == ( 0xBBBB0000U | (UW)i ) ) nb++;
		else if ( v == 0 ) nz++;
	}
	/* after: what the GPU wrote to memory */
	bo_fresh(dst, 0, words * 4);
	for ( i = 0; i < words; i++ ) {
		UW v = bo_word(dst, i);
		if ( v == ( 0xAAAA0000U | (UW)i ) ) fa++;
		else if ( v == ( 0xBBBB0000U | (UW)i ) ) fb++;
	}
	bo_fresh(src, 0, words * 4);

	ok = ( er == E_OK && fa + fb == words );
	tm_printf((UB *)"v3d test: coherence %s: job %s; the GPU read %s (A %ld, B %ld); "
		  "the processor saw the GPU's writes %s (new %ld, stale %ld)\n",
		  ok ? "ok" : "FAIL", ername(er),
		  ( fb == words ) ? "the processor's cache" : ( fa == words ) ? "memory only" : "a mixture",
		  fa, fb,
		  ( na + nb == words ) ? "without invalidating" : ( nz == words ) ? "only after invalidating" : "partly",
		  na + nb, nz);
	note("coherence", ok);
done:
	if ( src != NULL ) knl_gpu_bo_put(src);
	if ( dst != NULL ) knl_gpu_bo_put(dst);
}

/* The binner is told of less tile memory than it takes: the overflow memory must serve */
LOCAL void test_overflow( void )
{
	CONST UINT	w = 1920, h = 1080;
	CONST UD	words = (UD)w * h;
	T_V3DSTAT	a, b;
	T_GPUBO		*dst;
	UD		gbad, first, bad = words, us = 0;
	FRAME		f;
	ER		er;
	BOOL		ok;

	dst = knl_gpu_bo_create(words * 4, 0, &er);
	if ( dst == NULL ) {
		tm_printf((UB *)"v3d test: overflow FAIL: no BO\n");
		note("overflow", FALSE);
		return;
	}
	v3d_stat(&a);
	knl_memset(&f, 0, sizeof(f));
	f.w = w;
	f.h = h;
	f.fill = TRUE;
	f.color = 0x5A5A5A5AU;
	f.dst = bo_gpu(dst);
	f.small_qms = TRUE;
	er = frame_run(&f, dst, NULL, &us);
	v3d_stat(&b);
	if ( er == E_OK ) {
		bad = count_bad(dst, words, f.color, 0, &gbad, &first);
	}
	ok = ( er == E_OK && bad == 0 );
	tm_printf((UB *)"v3d test: overflow %s: job %s, out of memory %d time(s), %ld pixels wrong, %ld us\n",
		  ok ? "ok" : "FAIL", ername(er), (INT)( b.outomem - a.outomem ), bad, us);
	knl_gpu_bo_put(dst);
	note("overflow", ok);
}

/* A store to an address not in the MMU: the fault is reported and the job fails */
LOCAL void test_fault( void )
{
	T_V3DSTAT	a, b;
	FRAME		f;
	UD		i, nscratch = 0, us = 0;
	UW		*scratch;
	ER		er;
	BOOL		ok;

	v3d_stat(&a);
	knl_memset(&f, 0, sizeof(f));
	f.w = f.h = 64;
	f.fill = TRUE;
	f.color = 0xDEADBEEFU;
	f.dst = INVALID_GPU_ADDR;
	er = frame_run(&f, NULL, NULL, &us);
	v3d_stat(&b);

	scratch = (UW *)PA2VA(b.scratch_pa);
	knl_gpu_dcache_clean_inval((UBINT)scratch, PAGE_SIZE);
	for ( i = 0; i < PAGE_SIZE / 4; i++ ) {
		if ( scratch[i] == f.color ) nscratch++;
	}
	knl_memset(scratch, 0, PAGE_SIZE);
	knl_gpu_dcache_clean_inval((UBINT)scratch, PAGE_SIZE);

	ok = ( er == E_IO && b.faults > a.faults );
	tm_printf((UB *)"v3d test: MMU fault %s: job %s after %ld us, %d fault(s), last %08x at %08x "
		  "(client %08x), %ld words reached the scratch page\n",
		  ok ? "ok" : "FAIL", ername(er), us, (INT)( b.faults - a.faults ),
		  b.fault_sts, b.fault_addr, b.fault_id, nscratch);
	note("MMU fault", ok);
}

/* ---------------------------------------------------------------- the task */

LOCAL void test_task( INT stacd, void *exinf )
{
	T_V3DSTAT	st;
	INT		i, npass = 0;

	tk_dly_tsk(TEST_DELAY_MS);
	tm_printf((UB *)"v3d test: start\n");

	test_fill("fill", 64, 64, 0x11223344U);
	test_fill("fill uneven", 100, 37, 0x55AA00FFU);
	test_fill("fill large", 1920, 1080, 0xC0FFEE11U);
	test_copy();
	test_queue();
	test_coherence();
	test_overflow();
	test_fault();
	test_fill("after fault", 64, 64, 0x0F1E2D3CU);

	v3d_stat(&st);
	tm_printf((UB *)"v3d test: interrupts: core %d (frames %d, flushes %d, out of memory %d), "
		  "hub %d (MMU faults %d), turned off %d\n",
		  (INT)st.core_ints, (INT)st.frdone, (INT)st.fldone, (INT)st.outomem,
		  (INT)st.hub_ints, (INT)st.faults, (INT)st.storms);
	note("interrupts", st.frdone > 0 && st.fldone > 0 && st.storms == 0);

	for ( i = 0; i < nresult; i++ ) {
		if ( result[i].ok ) npass++;
	}
	tm_printf((UB *)"v3d test: summary %d/%d passed", npass, nresult);
	for ( i = 0; i < nresult; i++ ) {
		if ( !result[i].ok ) tm_printf((UB *)", FAIL %s", result[i].name);
	}
	tm_printf((UB *)"\n");
	tk_ext_tsk();
}

EXPORT void v3d_test_start( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	if ( CNF_V3D < 2 ) {
		return;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)test_task;
	ctsk.itskpri = TEST_PRI;
	ctsk.stksz = TEST_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 || tk_sta_tsk(tskid, 0) < E_OK ) {
		tm_printf((UB *)"v3d test: could not start (%d)\n", (INT)tskid);
	}
}

#endif /* RPI5 */
