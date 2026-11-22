/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_mfx.c
 *	The batch of one H.264 picture for the video engine (MFX) of the 9th
 *	generation (docs/tessronos/13-gpu.md 13.12): the engine in VLD mode
 *	with the short format, where it parses each slice's header itself
 *	and builds the reference lists from the decoded picture buffer it is
 *	given. The order of the commands and what goes into them:
 *
 *	  MI_FLUSH_DW			the video pipeline's caches invalidated
 *	  MFX_PIPE_MODE_SELECT		AVC, decoding, VLD, short format, the
 *					deblocked picture written
 *	  MFX_SURFACE_STATE		the pictures: 4:2:0 of 8 bits, chroma
 *					interleaved, Y-tiled
 *	  MFX_PIPE_BUF_ADDR_STATE	the picture written, the row stores,
 *					the reference pictures
 *	  MFX_IND_OBJ_BASE_ADDR_STATE	the slices' bytes
 *	  MFX_BSP_BUF_BASE_ADDR_STATE	the bit stream parser's row stores
 *	  MFD_AVC_DPB_STATE		each reference: long-term, made up,
 *					used for reference, its FrameNum
 *	  MFD_AVC_PICID_STATE		each reference's picture ID (its slot)
 *	  MFX_AVC_IMG_STATE		the picture's and its parameter sets'
 *					fields the slice headers do not carry
 *	  MFX_QM_STATE			the scaling lists (4×4, and 8×8 with the
 *					8×8 transform)
 *	  MFX_AVC_DIRECTMODE_STATE	the motion vectors of the references and
 *					of the picture, the order counts
 *	  for each slice: MFD_AVC_SLICEADDR (where the next slice is, but for
 *	  the last) and MFD_AVC_BSD_OBJECT (where this one is)
 *	  MI_BATCH_BUFFER_END
 *
 *	The fields and their bits are those of the public description of the
 *	commands in Mesa's genxml for this generation (gen90.xml, MIT), and
 *	the way they are filled that of the H.264 decoding of Mesa's Vulkan
 *	driver for Intel (anv, the same licence). The host test sets every
 *	dword against Mesa's own packing of the commands.
 */

#ifdef IGT_MFX_HOST
#include "kernel.h"
#define X64
#else
#include <sys/machine.h>
#endif

#ifdef X64

#ifndef IGT_MFX_HOST
#include "kernel.h"
#endif
#include "igt_mfx.h"

/* A command of the video pipeline: type 3, pipeline 2 */
#define MFX(op, a, b, len)	( ( 3U << 29 ) | ( 2U << 27 ) | ( (UW)(op) << 24 ) | ( (UW)(a) << 21 ) \
				  | ( (UW)(b) << 16 ) | (UW)( (len) - 2 ) )

#define MFX_PIPE_MODE_SELECT		MFX(0, 0, 0, 5)
#define MFX_SURFACE_STATE		MFX(0, 0, 1, 6)
#define MFX_PIPE_BUF_ADDR_STATE		MFX(0, 0, 2, 65)
#define MFX_IND_OBJ_BASE_ADDR_STATE	MFX(0, 0, 3, 26)
#define MFX_BSP_BUF_BASE_ADDR_STATE	MFX(0, 0, 4, 10)
#define MFX_QM_STATE			MFX(0, 0, 7, 18)
#define MFX_AVC_IMG_STATE		MFX(1, 0, 0, 21)
#define MFX_AVC_DIRECTMODE_STATE	MFX(1, 0, 2, 71)
#define MFD_AVC_PICID_STATE		MFX(1, 1, 5, 10)
#define MFD_AVC_DPB_STATE		MFX(1, 1, 6, 27)
#define MFD_AVC_SLICEADDR		MFX(1, 1, 7, 3)
#define MFD_AVC_BSD_OBJECT		MFX(1, 1, 8, 6)

#define MI_FLUSH_DW_VIDEO	( ( 0x26U << 23 ) | ( 1U << 7 ) | 2 )	/* the video pipeline's caches invalidated */
#define MI_BB_END		( 0x0aU << 23 )

/* MFX_PIPE_MODE_SELECT */
#define PMS_AVC			2
#define PMS_POST_DEBLOCK_OUT	( 1U << 9 )
/* MFX_SURFACE_STATE */
#define SS_TILE_WALK_Y		( 1U << 0 )
#define SS_TILED		( 1U << 1 )
#define SS_INTERLEAVE_CHROMA	( 1U << 27 )
#define SS_PLANAR_420_8		( 4U << 28 )
/* MFX_QM_STATE */
#define QM_4X4_INTRA		0
#define QM_4X4_INTER		1
#define QM_8X8_INTRA		2
#define QM_8X8_INTER		3
/* MFD_AVC_BSD_OBJECT, its inline data */
#define BSD_LAST_SLICE		( 1U << 3 )
#define BSD_FIX_PREV_MB_SKIPPED	( 1U << 7 )
#define BSD_INTRA_PRED_ERROR	( 1U << 0 )
#define BSD_INTRA_8X8_4X4_ERROR	( 1U << 1 )
#define BSD_I_SLICE_CONCEAL	( 1U << 31 )

typedef struct {
	UW	*b;
	INT	n, max;
} BB;

Inline void put( BB *bb, UW v )
{
	if ( bb->n < bb->max ) bb->b[bb->n] = v;
	bb->n++;
}

/* An address of 48 bits in two dwords */
LOCAL void addr( BB *bb, UD a )
{
	put(bb, (UW)a);
	put(bb, (UW)( a >> 32 ) & 0xffff);
}

/* An address and its memory attributes (the MOCS in bits 6:0) */
LOCAL void addr_attr( BB *bb, UD a, UW mocs )
{
	addr(bb, a);
	put(bb, mocs & 0x7f);
}

/* A signed field of bits lo..hi */
Inline UW sfield( INT v, INT lo, INT hi )
{
	return ( (UW)v & ( ( 1U << ( hi - lo + 1 ) ) - 1 ) ) << lo;
}

LOCAL void qm( BB *bb, UW which, CONST UB *m, INT bytes )
{
	INT	i;

	put(bb, MFX_QM_STATE);
	put(bb, which);
	for ( i = 0; i < 64; i += 4 ) {
		UW	w = 0;
		INT	k;

		for ( k = 0; k < 4; k++ ) {
			if ( i + k < bytes ) w |= (UW)m[i + k] << ( 8 * k );
		}
		put(bb, w);
	}
}

EXPORT INT igt_mfx_h264( CONST IGT_MFXPIC *p, UW *b, INT max )
{
	CONST T_H264JOB	*j = p->job;
	CONST T_H264SPS	*sps = &j->sps;
	CONST T_H264PPS	*pps = &j->pps;
	BB	bb;
	UW	mbw = (UW)sps->pic_width_in_mbs_minus1 + 1;
	UW	mbh = ( (UW)sps->pic_height_in_map_units_minus1 + 1 )
		      * ( ( sps->flags & H264_SPS_FRAME_MBS_ONLY ) ? 1 : 2 );
	UW	m = p->mocs, w;
	UW	nref = ( j->ndpb <= H264_MAX_DPB ) ? j->ndpb : H264_MAX_DPB;
	UW	i, s;

	bb.b = b;
	bb.n = 0;
	bb.max = max;

	/* the video pipeline's caches: nothing of another picture's kept */
	put(&bb, MI_FLUSH_DW_VIDEO);
	put(&bb, 0);
	put(&bb, 0);
	put(&bb, 0);
	put(&bb, 0);				/* a no-op after the command of four */

	put(&bb, MFX_PIPE_MODE_SELECT);
	put(&bb, PMS_AVC | PMS_POST_DEBLOCK_OUT);	/* decoding, VLD, the short format: all 0 */
	put(&bb, 0);
	put(&bb, 0);
	put(&bb, 0);

	put(&bb, MFX_SURFACE_STATE);
	put(&bb, 0);				/* the reference pictures and the one decoded */
	put(&bb, ( ( mbw * 16 - 1 ) << 4 ) | ( ( mbh * 16 - 1 ) << 18 ));
	put(&bb, SS_TILE_WALK_Y | SS_TILED | ( ( (UW)p->pitch - 1 ) << 3 ) | SS_INTERLEAVE_CHROMA | SS_PLANAR_420_8);
	put(&bb, p->uv_rows & 0x7fff);		/* the chroma's rows below the luma's start, U */
	put(&bb, p->uv_rows & 0xffff);		/* and V, interleaved with it */

	put(&bb, MFX_PIPE_BUF_ADDR_STATE);
	addr_attr(&bb, 0, m);			/* before deblocking: not written */
	addr_attr(&bb, p->out.pic, m);		/* after it: the picture */
	addr_attr(&bb, 0, m);			/* the original (encoding's) */
	addr_attr(&bb, 0, m);			/* streamed out */
	addr_attr(&bb, p->row_intra, m);
	addr_attr(&bb, p->row_deblock, m);
	for ( i = 0; i < H264_MAX_DPB; i++ ) addr(&bb, ( i < nref ) ? p->ref[i].pic : 0);
	put(&bb, m);
	addr_attr(&bb, 0, m);			/* the macroblocks' status */
	addr_attr(&bb, 0, m);			/* the deblocking's streams */
	addr_attr(&bb, 0, m);
	put(&bb, 0);				/* no compression of the references */
	addr_attr(&bb, 0, m);			/* the scaled reference */

	put(&bb, MFX_IND_OBJ_BASE_ADDR_STATE);
	addr_attr(&bb, p->bits, m);		/* the slices' bytes */
	addr(&bb, 0);				/* their upper bound: none */
	for ( i = 0; i < 4; i++ ) {		/* the motion vectors', coefficients', deblocking's, encoder's */
		addr_attr(&bb, 0, m);
		addr(&bb, 0);
	}

	put(&bb, MFX_BSP_BUF_BASE_ADDR_STATE);
	addr_attr(&bb, p->row_bsd, m);
	addr_attr(&bb, p->row_mpr, m);
	addr_attr(&bb, 0, m);			/* the bit planes (VC-1's) */

	/* the references, in the order of job->dpb: the frame store index the engine knows them by */
	put(&bb, MFD_AVC_DPB_STATE);
	w = 0;
	for ( i = 0; i < nref; i++ ) {
		if ( j->dpb[i].flags & H264_DPB_NON_EXISTING ) w |= 1U << i;
		if ( j->dpb[i].flags & H264_DPB_LONG_TERM ) w |= 1U << ( 16 + i );
	}
	put(&bb, w);
	w = 0;
	for ( i = 0; i < nref; i++ ) {
		UW	used = j->dpb[i].flags & ( H264_DPB_TOP | H264_DPB_BOTTOM );

		/* a frame: both fields */
		w |= ( ( used == 0 ) ? 3U : ( ( used & H264_DPB_TOP ) ? 1U : 0 ) | ( ( used & H264_DPB_BOTTOM ) ? 2U : 0 ) )
		     << ( 2 * i );
	}
	put(&bb, w);
	for ( i = 0; i < H264_MAX_DPB; i += 2 ) {
		UW	lo = ( i < nref ) ? j->dpb[i].frame_idx : 0;
		UW	hi = ( i + 1 < nref ) ? j->dpb[i + 1].frame_idx : 0;

		put(&bb, lo | ( hi << 16 ));
	}
	for ( i = 0; i < 16; i++ ) put(&bb, 0);	/* the views of multiview coding */

	put(&bb, MFD_AVC_PICID_STATE);
	put(&bb, 0);				/* 16 bit picture IDs */
	for ( i = 0; i < H264_MAX_DPB; i += 2 ) {
		UW	lo = ( i < nref ) ? j->dpb[i].slot : 0xffff;
		UW	hi = ( i + 1 < nref ) ? j->dpb[i + 1].slot : 0xffff;

		put(&bb, lo | ( hi << 16 ));
	}

	put(&bb, MFX_AVC_IMG_STATE);
	put(&bb, ( mbw * mbh ) & 0xffff);
	put(&bb, ( ( mbw - 1 ) & 0xff ) | ( ( ( mbh - 1 ) & 0xff ) << 16 ));
	put(&bb, ( 0U << 8 )			/* a frame */
		 | ( ( (UW)pps->weighted_bipred_idc & 3 ) << 10 )
		 | ( ( pps->flags & H264_PPS_WEIGHTED_PRED ) ? 1U << 12 : 0 )
		 | sfield(pps->chroma_qp_index_offset, 16, 20)
		 | sfield(pps->second_chroma_qp_index_offset, 24, 28));
	put(&bb, ( ( sps->flags & H264_SPS_MB_ADAPTIVE_FF ) ? 1U << 1 : 0 )
		 | ( ( sps->flags & H264_SPS_FRAME_MBS_ONLY ) ? 1U << 2 : 0 )
		 | ( ( pps->flags & H264_PPS_TRANSFORM_8X8 ) ? 1U << 3 : 0 )
		 | ( ( sps->flags & H264_SPS_DIRECT_8X8_INFERENCE ) ? 1U << 4 : 0 )
		 | ( ( pps->flags & H264_PPS_CONSTRAINED_INTRA ) ? 1U << 5 : 0 )
		 | ( ( j->flags & H264_PIC_REFERENCE ) ? 0 : 1U << 6 )
		 | ( ( pps->flags & H264_PPS_CABAC ) ? 1U << 7 : 0 )
		 | ( ( (UW)sps->chroma_format_idc & 3 ) << 10 ));
	put(&bb, 1U << 27);			/* no trellis quantization of chroma (the encoder's) */
	for ( i = 6; i <= 12; i++ ) put(&bb, 0);
	put(&bb, sfield(pps->pic_init_qp_minus26, 0, 7)
		 | ( ( (UW)pps->num_ref_idx_l0_default_minus1 + 1 ) & 0x3f ) << 8
		 | ( ( (UW)pps->num_ref_idx_l1_default_minus1 + 1 ) & 0x3f ) << 16
		 | ( nref & 0x1f ) << 24);
	put(&bb, ( ( pps->flags & H264_PPS_BOTTOM_FIELD_POC ) ? 1U : 0 )
		 | ( ( sps->flags & H264_SPS_DELTA_POC_ALWAYS_ZERO ) ? 1U << 1 : 0 )
		 | ( ( (UW)sps->pic_order_cnt_type & 3 ) << 2 )
		 | ( ( pps->flags & H264_PPS_REDUNDANT_PIC_CNT ) ? 1U << 11 : 0 )
		 | ( ( pps->flags & H264_PPS_DEBLOCKING_CONTROL ) ? 1U << 15 : 0 )
		 | ( (UW)sps->log2_max_frame_num_minus4 << 16 )
		 | ( (UW)sps->log2_max_poc_lsb_minus4 << 24 ));
	put(&bb, (UW)j->frame_num << 16);
	for ( i = 16; i <= 20; i++ ) put(&bb, 0);

	qm(&bb, QM_4X4_INTRA, &j->scale.s4x4[0][0], 48);
	qm(&bb, QM_4X4_INTER, &j->scale.s4x4[3][0], 48);
	if ( pps->flags & H264_PPS_TRANSFORM_8X8 ) {
		qm(&bb, QM_8X8_INTRA, j->scale.s8x8[0], 64);
		qm(&bb, QM_8X8_INTER, j->scale.s8x8[1], 64);
	}

	put(&bb, MFX_AVC_DIRECTMODE_STATE);
	for ( i = 0; i < H264_MAX_DPB; i++ ) addr(&bb, ( i < nref ) ? p->ref[i].dmv : 0);
	put(&bb, m);
	addr_attr(&bb, p->out.dmv, m);
	for ( i = 0; i < H264_MAX_DPB; i++ ) {
		put(&bb, (UW)( ( i < nref ) ? j->dpb[i].poc_top : 0 ));
		put(&bb, (UW)( ( i < nref ) ? j->dpb[i].poc_bottom : 0 ));
	}
	put(&bb, (UW)j->poc_top);
	put(&bb, (UW)j->poc_bottom);

	for ( s = 0; s < j->nslice; s++ ) {
		CONST T_H264SLICE *sl = &p->slice[s];
		BOOL	last = ( s + 1 == j->nslice );

		if ( !last ) {
			/* where the next slice starts: the engine conceals up to it should this one end early */
			put(&bb, MFD_AVC_SLICEADDR);
			put(&bb, sl[1].data_size);
			put(&bb, sl[1].data_offset & 0x1fffffff);
		}
		put(&bb, MFD_AVC_BSD_OBJECT);
		put(&bb, sl->data_size);
		put(&bb, sl->data_offset & 0x1fffffff);
		put(&bb, 0);
		put(&bb, ( last ? BSD_LAST_SLICE : 0 ) | BSD_FIX_PREV_MB_SKIPPED);
		put(&bb, BSD_INTRA_PRED_ERROR | BSD_INTRA_8X8_4X4_ERROR | BSD_I_SLICE_CONCEAL);
	}

	put(&bb, MI_BB_END);
	if ( bb.n & 1 ) put(&bb, 0);
	return ( bb.n <= max ) ? bb.n : -1;
}

#endif /* X64 */
