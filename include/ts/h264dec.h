/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	h264dec.h
 *	The H.264 decoder of Intel's integrated graphics (the video engine
 *	of the 9th generation, docs/tessronos/13-gpu.md 13.12) as a device
 *	object (design 6.7): "動画の復号器", of the subtype OB_S_VIDEODEC,
 *	its attribute device.kind "h264" (the Raspberry Pi 5's is "hevc",
 *	include/ts/hevc.h).
 *
 *	The records are those of the HEVC decoder: HEVC_REC_CONF takes a
 *	T_HEVCCONF (bitdepth 8), HEVC_REC_JOB one picture, HEVC_REC_PICTURE
 *	gives a slot's picture as NV12 (the slot in the upper 32 bits of the
 *	offset), HEVC_REC_INFO a T_HEVCINFO. Only the job differs:
 *
 *	  a T_H264JOB, then T_H264JOB.nslice T_H264SLICE, then the slices'
 *	  bytes: each slice its NAL unit from the byte of its header on (no
 *	  start code), emulation prevention bytes left in.
 *
 *	The decoder reads the slices' headers itself; the program gives what
 *	the headers do not say: the sequence's and the picture's parameter
 *	sets, the picture's frame_num and order counts, and the pictures the
 *	decoder may refer to (the decoded picture buffer, each in a slot).
 *	It keeps no state of the stream between jobs: the program keeps the
 *	buffer, its marking and the order of output, as with the HEVC
 *	decoder.
 *
 *	Frames only (frame_mbs_only_flag 1), 4:2:0 of 8 bits, one slice
 *	group, pictures up to H264_MAX_WIDTH × H264_MAX_HEIGHT; the
 *	Constrained Baseline, Main and High profiles. Anything else is
 *	E_NOSPT, so that the program takes it to its own decoder.
 *
 *	Errors: E_NOEXS when there is no decoder, E_PAR for a job that does
 *	not fit the slots or itself, E_NOSPT for what the decoder does not
 *	do, E_IO when the engine did not finish the picture.
 */

#ifndef __TS_H264DEC_H__
#define __TS_H264DEC_H__

#include <tk/typedef.h>
#include <ts/hevc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H264_KIND		"h264"		/* the object's device.kind */

#define H264_MAX_WIDTH		1920
#define H264_MAX_HEIGHT		1088
#define H264_MAX_SLOTS		17		/* 16 references and the picture decoded */
#define H264_MAX_SLICES		128
#define H264_MAX_DPB		16
#define H264_JOB_MAX		( 4 * 1024 * 1024 )	/* bytes of a job, its slices' bytes included */

#define H264_JOB_MAGIC		0x48323634U	/* "H264" */

/* The sequence's parameters (T_H264SPS.flags) */
#define H264_SPS_FRAME_MBS_ONLY		0x0001
#define H264_SPS_MB_ADAPTIVE_FF		0x0002
#define H264_SPS_DIRECT_8X8_INFERENCE	0x0004
#define H264_SPS_DELTA_POC_ALWAYS_ZERO	0x0008
#define H264_SPS_SEPARATE_COLOUR_PLANE	0x0010

typedef struct {
	UB	profile_idc;
	UB	chroma_format_idc;		/* 1 */
	UB	bit_depth_luma, bit_depth_chroma;	/* 8 */
	UB	log2_max_frame_num_minus4;
	UB	pic_order_cnt_type;
	UB	log2_max_poc_lsb_minus4;
	UB	max_num_ref_frames;
	UH	pic_width_in_mbs_minus1;
	UH	pic_height_in_map_units_minus1;
	UW	flags;				/* H264_SPS_* */
} T_H264SPS;

/* The picture's parameters (T_H264PPS.flags) */
#define H264_PPS_CABAC			0x0001	/* entropy_coding_mode_flag */
#define H264_PPS_BOTTOM_FIELD_POC	0x0002	/* bottom_field_pic_order_in_frame_present_flag */
#define H264_PPS_WEIGHTED_PRED		0x0004
#define H264_PPS_CONSTRAINED_INTRA	0x0008
#define H264_PPS_TRANSFORM_8X8		0x0010
#define H264_PPS_DEBLOCKING_CONTROL	0x0020
#define H264_PPS_REDUNDANT_PIC_CNT	0x0040

typedef struct {
	B	pic_init_qp_minus26;
	B	chroma_qp_index_offset;
	B	second_chroma_qp_index_offset;
	UB	num_ref_idx_l0_default_minus1;
	UB	num_ref_idx_l1_default_minus1;
	UB	weighted_bipred_idc;
	UB	num_slice_groups_minus1;	/* 0 */
	UB	rsv;
	UW	flags;				/* H264_PPS_* */
} T_H264PPS;

/*
 * The scaling lists the picture uses, after the fall-back rules, in
 * raster order: 4×4 intra Y, Cb, Cr, inter Y, Cb, Cr; 8×8 intra Y, inter
 * Y. A stream without lists gives 16 everywhere.
 */
typedef struct {
	UB	s4x4[6][16];
	UB	s8x8[2][64];
} T_H264SCALE;

/* One picture of the decoded picture buffer (T_H264DPB.flags) */
#define H264_DPB_LONG_TERM		0x01
#define H264_DPB_NON_EXISTING		0x02	/* made up for a gap in frame_num */
#define H264_DPB_TOP			0x04	/* its top field is used for reference */
#define H264_DPB_BOTTOM			0x08	/* its bottom field is */

typedef struct {
	UB	slot;			/* the decoder's slot the picture is in */
	UB	flags;			/* H264_DPB_* */
	UH	frame_idx;		/* FrameNum, or LongTermFrameIdx for a long-term one */
	W	poc_top, poc_bottom;	/* TopFieldOrderCnt, BottomFieldOrderCnt */
} T_H264DPB;

typedef struct {
	UW	data_offset;		/* the slice's NAL unit in the job's bytes, from its header's byte */
	UW	data_size;		/* its bytes */
} T_H264SLICE;

/* The picture (T_H264JOB.flags) */
#define H264_PIC_IDR			0x01
#define H264_PIC_REFERENCE		0x02	/* nal_ref_idc is not 0 */

typedef struct {
	UW		magic;		/* H264_JOB_MAGIC */
	UW		size;		/* bytes of the whole job */
	UW		out_slot;	/* the slot the picture goes into */
	UW		nslice;		/* T_H264SLICE after this */
	UW		ndpb;		/* entries of dpb used */
	UH		frame_num;
	UB		flags;		/* H264_PIC_* */
	UB		rsv;
	W		poc_top, poc_bottom;
	T_H264DPB	dpb[H264_MAX_DPB];
	T_H264SPS	sps;
	T_H264PPS	pps;
	T_H264SCALE	scale;
} T_H264JOB;

#ifdef __cplusplus
}
#endif
#endif /* __TS_H264DEC_H__ */
