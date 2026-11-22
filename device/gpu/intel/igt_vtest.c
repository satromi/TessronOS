/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_vtest.c
 *	The pictures the H.264 decoder of the video engine is checked with
 *	(igt_vtest.h): written bit by bit as the syntax of ITU-T H.264
 *	(7.3) has them, emulation prevention bytes put in as they would be.
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
#include "igt_vtest.h"

typedef struct {
	UB	*b;
	UD	n, max;		/* bytes written (counted past max, not stored) */
	UW	acc;		/* bits not yet a byte */
	INT	nacc;
	INT	zeros;		/* zero bytes just written, for the emulation prevention */
} BW;

LOCAL void bw_byte( BW *w, UB v )
{
	if ( w->zeros >= 2 && v <= 3 ) {
		if ( w->n < w->max ) w->b[w->n] = 3;
		w->n++;
		w->zeros = 0;
	}
	if ( w->n < w->max ) w->b[w->n] = v;
	w->n++;
	w->zeros = ( v == 0 ) ? w->zeros + 1 : 0;
}

LOCAL void bw_bits( BW *w, UW v, INT n )
{
	while ( n-- > 0 ) {
		w->acc = ( w->acc << 1 ) | ( ( v >> n ) & 1 );
		if ( ++w->nacc == 8 ) {
			bw_byte(w, (UB)w->acc);
			w->acc = 0;
			w->nacc = 0;
		}
	}
}

/* ue(v): the Exp-Golomb code; se(0) is ue(0) */
LOCAL void bw_ue( BW *w, UW v )
{
	INT	len = 0;

	while ( ( ( v + 1 ) >> len ) > 1 ) len++;
	bw_bits(w, 0, len);
	bw_bits(w, v + 1, len + 1);
}

LOCAL void bw_align( BW *w )
{
	while ( w->nacc != 0 ) bw_bits(w, 0, 1);
}

/* rbsp_trailing_bits */
LOCAL void bw_trailing( BW *w )
{
	bw_bits(w, 1, 1);
	bw_align(w);
}

/* A start code of Annex B, as it is */
LOCAL void bw_start( BW *w )
{
	static CONST UB	sc[4] = { 0, 0, 0, 1 };
	INT		i;

	for ( i = 0; i < 4; i++ ) {
		if ( w->n < w->max ) w->b[w->n] = sc[i];
		w->n++;
	}
	w->zeros = 0;
}

/* A NAL unit starts: its header's byte, not looked at for emulation */
LOCAL void bw_nal( BW *w, UB header )
{
	w->zeros = 0;
	bw_bits(w, header, 8);
}

EXPORT UB igt_vtest_sample( UW plane, UW x, UW y )
{
	switch ( plane ) {
	case 0:		return (UB)( 16 + ( ( x * 7 + y * 13 + ( x >> 4 ) * 29 ) & 191 ) );
	case 1:		return (UB)( 64 + ( ( x * 5 + y * 3 ) & 63 ) );
	default:	return (UB)( 128 + ( ( x * 3 + y * 7 ) & 63 ) );
	}
}

/* A slice NAL unit: the header's byte, the slice header, its data */
LOCAL void slice( BW *w, BOOL idr, UW first_mb, UW nmb, UW frame_num )
{
	UW	mb, x, y;

	bw_nal(w, idr ? 0x65 : 0x41);		/* nal_ref_idc 3 or 2; type 5 (IDR) or 1 */
	bw_ue(w, first_mb);
	bw_ue(w, idr ? 7 : 5);			/* I or P, all of the picture */
	bw_ue(w, 0);				/* pic_parameter_set_id */
	bw_bits(w, frame_num, 4);
	if ( idr ) {
		bw_ue(w, 0);			/* idr_pic_id */
		bw_bits(w, 0, 1);		/* no_output_of_prior_pics_flag */
		bw_bits(w, 0, 1);		/* long_term_reference_flag */
	} else {
		bw_bits(w, 0, 1);		/* num_ref_idx_active_override_flag */
		bw_bits(w, 0, 1);		/* ref_pic_list_modification_flag_l0 */
		bw_bits(w, 0, 1);		/* adaptive_ref_pic_marking_mode_flag */
	}
	bw_ue(w, 0);				/* slice_qp_delta */
	bw_ue(w, 1);				/* disable_deblocking_filter_idc */
	if ( idr ) {
		for ( mb = first_mb; mb < first_mb + nmb; mb++ ) {
			UW	mx = ( mb % IGT_VTEST_MBW ) * 16, my = ( mb / IGT_VTEST_MBW ) * 16;

			bw_ue(w, 25);		/* mb_type I_PCM */
			bw_align(w);		/* pcm_alignment_zero_bit */
			for ( y = 0; y < 16; y++ ) {
				for ( x = 0; x < 16; x++ ) bw_bits(w, igt_vtest_sample(0, mx + x, my + y), 8);
			}
			for ( y = 0; y < 8; y++ ) {
				for ( x = 0; x < 8; x++ ) bw_bits(w, igt_vtest_sample(1, mx / 2 + x, my / 2 + y), 8);
			}
			for ( y = 0; y < 8; y++ ) {
				for ( x = 0; x < 8; x++ ) bw_bits(w, igt_vtest_sample(2, mx / 2 + x, my / 2 + y), 8);
			}
		}
	} else {
		bw_ue(w, nmb);			/* mb_skip_run: every macroblock */
	}
	bw_trailing(w);
}

EXPORT SZ igt_vtest_job( UB *buf, SZ max, UW k, UW out, UW ref )
{
	T_H264JOB	*j = (T_H264JOB *)buf;
	T_H264SLICE	*sl = (T_H264SLICE *)( j + 1 );
	UW		ns = ( k == 0 ) ? 2 : 1, i;
	UD		head = sizeof(T_H264JOB) + ns * sizeof(T_H264SLICE);
	BW		w;

	if ( max < (SZ)head || k >= IGT_VTEST_PICS ) return E_PAR;
	knl_memset(buf, 0, (SZ)head);
	j->magic = H264_JOB_MAGIC;
	j->out_slot = out;
	j->nslice = ns;
	j->frame_num = (UH)k;
	j->flags = H264_PIC_REFERENCE | ( ( k == 0 ) ? H264_PIC_IDR : 0 );
	j->poc_top = j->poc_bottom = (W)( 2 * k );
	if ( k > 0 ) {
		j->ndpb = 1;
		j->dpb[0].slot = (UB)ref;
		j->dpb[0].frame_idx = (UH)( k - 1 );
		j->dpb[0].poc_top = j->dpb[0].poc_bottom = (W)( 2 * ( k - 1 ) );
	}
	j->sps.profile_idc = 66;
	j->sps.chroma_format_idc = 1;
	j->sps.bit_depth_luma = j->sps.bit_depth_chroma = 8;
	j->sps.pic_order_cnt_type = 2;
	j->sps.max_num_ref_frames = 1;
	j->sps.pic_width_in_mbs_minus1 = IGT_VTEST_MBW - 1;
	j->sps.pic_height_in_map_units_minus1 = IGT_VTEST_MBH - 1;
	j->sps.flags = H264_SPS_FRAME_MBS_ONLY | H264_SPS_DIRECT_8X8_INFERENCE;
	j->pps.flags = H264_PPS_DEBLOCKING_CONTROL;
	knl_memset(&j->scale, 16, sizeof(j->scale));

	w.b = buf + head;
	w.max = (UD)max - head;
	w.n = 0;
	w.acc = 0;
	w.nacc = 0;
	w.zeros = 0;
	for ( i = 0; i < ns; i++ ) {
		UW	nmb = IGT_VTEST_MBW * IGT_VTEST_MBH / ns;

		sl[i].data_offset = (UW)w.n;
		slice(&w, k == 0, i * nmb, nmb, k);
		sl[i].data_size = (UW)( w.n - sl[i].data_offset );
	}
	if ( w.n > w.max ) return E_LIMIT;
	j->size = (UW)( head + w.n );
	return (SZ)j->size;
}

EXPORT SZ igt_vtest_params( UB *buf, SZ max )
{
	BW	w;

	w.b = buf;
	w.max = (UD)max;
	w.n = 0;
	w.acc = 0;
	w.nacc = 0;
	w.zeros = 0;
	bw_start(&w);
	bw_nal(&w, 0x67);			/* the sequence's */
	bw_bits(&w, 66, 8);			/* profile_idc: Baseline */
	bw_bits(&w, 0xc0, 8);			/* constraint_set0 and 1: Constrained Baseline */
	bw_bits(&w, 10, 8);			/* level_idc */
	bw_ue(&w, 0);				/* seq_parameter_set_id */
	bw_ue(&w, 0);				/* log2_max_frame_num_minus4 */
	bw_ue(&w, 2);				/* pic_order_cnt_type */
	bw_ue(&w, 1);				/* max_num_ref_frames */
	bw_bits(&w, 0, 1);			/* gaps_in_frame_num_value_allowed_flag */
	bw_ue(&w, IGT_VTEST_MBW - 1);
	bw_ue(&w, IGT_VTEST_MBH - 1);
	bw_bits(&w, 1, 1);			/* frame_mbs_only_flag */
	bw_bits(&w, 1, 1);			/* direct_8x8_inference_flag */
	bw_bits(&w, 0, 1);			/* frame_cropping_flag */
	bw_bits(&w, 0, 1);			/* vui_parameters_present_flag */
	bw_trailing(&w);
	bw_start(&w);
	bw_nal(&w, 0x68);			/* the picture's */
	bw_ue(&w, 0);				/* pic_parameter_set_id */
	bw_ue(&w, 0);				/* seq_parameter_set_id */
	bw_bits(&w, 0, 1);			/* entropy_coding_mode_flag: CAVLC */
	bw_bits(&w, 0, 1);			/* bottom_field_pic_order_in_frame_present_flag */
	bw_ue(&w, 0);				/* num_slice_groups_minus1 */
	bw_ue(&w, 0);				/* num_ref_idx_l0_default_active_minus1 */
	bw_ue(&w, 0);				/* num_ref_idx_l1_default_active_minus1 */
	bw_bits(&w, 0, 1);			/* weighted_pred_flag */
	bw_bits(&w, 0, 2);			/* weighted_bipred_idc */
	bw_ue(&w, 0);				/* pic_init_qp_minus26 */
	bw_ue(&w, 0);				/* pic_init_qs_minus26 */
	bw_ue(&w, 0);				/* chroma_qp_index_offset */
	bw_bits(&w, 1, 1);			/* deblocking_filter_control_present_flag */
	bw_bits(&w, 0, 1);			/* constrained_intra_pred_flag */
	bw_bits(&w, 0, 1);			/* redundant_pic_cnt_present_flag */
	bw_trailing(&w);
	return ( w.n <= w.max ) ? (SZ)w.n : E_LIMIT;
}

#endif /* X64 */
