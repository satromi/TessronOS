/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_mfx.h
 *	The batch of one H.264 picture for the video engine of the 9th
 *	generation (MFX, VLD mode, the short format: the engine reads the
 *	slices' headers itself). Built from a job of the decoder object
 *	(include/ts/h264dec.h) and the GPU addresses of the surfaces and the
 *	buffers (docs/tessronos/13-gpu.md 13.12). Plain C with no calls of
 *	the kernel's, so that the host test (tools/x64/mesa/test_igt_mfx.c)
 *	builds it as it is and sets each command against Mesa's packing of
 *	the same command.
 */

#ifndef _DEVICE_GPU_INTEL_IGT_MFX_H_
#define _DEVICE_GPU_INTEL_IGT_MFX_H_

#include <ts/h264dec.h>

/* A picture in memory: luma rows, then chroma (U and V by turns) from row uv_rows on, Y-tiled */
typedef struct {
	UD	pic;			/* GPU address of the first luma row */
	UD	dmv;			/* of its motion vectors kept for direct prediction */
} IGT_MFXSURF;

typedef struct {
	CONST T_H264JOB		*job;
	CONST T_H264SLICE	*slice;		/* job->nslice of them */
	UW			pitch;		/* bytes of a row of the surfaces, a multiple of 128 */
	UW			uv_rows;	/* rows from the luma's start to the chroma's, a multiple of 32 */
	IGT_MFXSURF		out;
	IGT_MFXSURF		ref[H264_MAX_DPB];	/* as job->dpb */
	UD			bits;		/* GPU address of the slices' bytes (offset 0 of T_H264SLICE), 4KB aligned */
	UD			row_intra, row_deblock, row_bsd, row_mpr;	/* the engine's row stores, 64 bytes aligned */
	UW			mocs;		/* the memory object control state for all of it (its index × 2) */
} IGT_MFXPIC;

/* Bytes of the row stores a picture of mbw macroblocks across needs */
#define IGT_MFX_ROW_INTRA(mbw)		( (UD)(mbw) * 64 )
#define IGT_MFX_ROW_DEBLOCK(mbw)	( (UD)(mbw) * 64 * 4 )
#define IGT_MFX_ROW_BSD(mbw)		( (UD)(mbw) * 64 * 2 )
#define IGT_MFX_ROW_MPR(mbw)		( (UD)(mbw) * 64 * 2 )
/* Bytes of a picture's motion vectors for direct prediction */
#define IGT_MFX_DMV(mbw, mbh)		( (UD)(mbw) * (mbh) * 128 )

/* Dwords of the batch at most, for nslice slices */
#define IGT_MFX_MAX_DW(nslice)		( 400 + (nslice) * 9 )

/*
 * The batch written to b (max dwords there), ending with
 * MI_BATCH_BUFFER_END on an even dword: its dwords, or -1 when it does
 * not fit
 */
IMPORT INT igt_mfx_h264( CONST IGT_MFXPIC *p, UW *b, INT max );

#endif /* _DEVICE_GPU_INTEL_IGT_MFX_H_ */
