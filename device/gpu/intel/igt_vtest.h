/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_vtest.h
 *	The pictures the H.264 decoder of the video engine is checked with
 *	before it is offered (igt_vdec.c; docs/tessronos/13-gpu.md 13.12):
 *	their result is known without a decoder. Plain C, so that the host
 *	test (tools/x64/mesa/test_igt_vtest.c) has FFmpeg decode the same
 *	stream and sets it against the same samples.
 *
 *	  picture 0	IDR, two slices of I_PCM macroblocks: the samples as
 *			igt_vtest_sample gives them
 *	  picture 1, 2	P, every macroblock skipped: a copy of the one before
 *
 *	IGT_VTEST_W × IGT_VTEST_H, Baseline, CAVLC, frame_num of 4 bits, order
 *	counts of type 2, one reference, deblocking off.
 */

#ifndef _DEVICE_GPU_INTEL_IGT_VTEST_H_
#define _DEVICE_GPU_INTEL_IGT_VTEST_H_

#include <ts/h264dec.h>

#define IGT_VTEST_MBW	10
#define IGT_VTEST_MBH	4
#define IGT_VTEST_W	( IGT_VTEST_MBW * 16 )	/* two tiles of 128 bytes across */
#define IGT_VTEST_H	( IGT_VTEST_MBH * 16 )
#define IGT_VTEST_PICS	3

/* The job of picture k into slot 'out', its reference in slot 'ref': its bytes, or < 0 */
IMPORT SZ igt_vtest_job( UB *buf, SZ max, UW k, UW out, UW ref );

/* A sample of the I_PCM picture: plane 0 luma (x, y), 1 Cb, 2 Cr (x, y of the chroma) */
IMPORT UB igt_vtest_sample( UW plane, UW x, UW y );

/* The sequence's and the picture's parameter sets the jobs stand for, as Annex B: their bytes */
IMPORT SZ igt_vtest_params( UB *buf, SZ max );

#endif /* _DEVICE_GPU_INTEL_IGT_VTEST_H_ */
