/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	test_igt_mfx.c (host)
 *	The batch of an H.264 picture for the video engine of the 9th
 *	generation (device/gpu/intel/igt_mfx.c) set against Mesa's packing of
 *	the same commands (the gen90_video_pack.h made from genxml's
 *	gen90.xml): for many made-up pictures, every command is packed again
 *	here from its fields, filled the way Mesa's Vulkan driver fills them
 *	for an H.264 picture, and the two batches must agree in every dword
 *	(docs/tessronos/13-gpu.md 13.12). Built and run by igt_mfx_test.sh.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define __gen_address_type	uint64_t
#define __gen_user_data		void

static inline uint64_t __gen_combine_address( void *data, void *loc, uint64_t addr, uint32_t delta )
{
	(void)data;
	(void)loc;
	return addr + delta;
}

#include "genxml/gen90_video_pack.h"

#include "kernel.h"
#include "igt_mfx.h"

static uint32_t	rnd_state = 12345;

static uint32_t rnd( void )
{
	rnd_state = rnd_state * 1103515245U + 12345U;
	return rnd_state >> 8;
}

static unsigned	bad, checked;

static void cmp( const char *what, const uint32_t *a, const uint32_t *b, int n, int at )
{
	int	i;

	for ( i = 0; i < n; i++ ) {
		checked++;
		if ( a[i] != b[i] ) {
			if ( bad < 20 ) {
				printf("mfx: %s dword %d (batch %d): ours %08x, Mesa's %08x\n", what, i, at + i, a[i], b[i]);
			}
			bad++;
		}
	}
}

#define MEMATTR(m)	((struct GFX9_MEMORYADDRESSATTRIBUTES){ .MOCS = (m) })

/* The same batch, packed with Mesa's functions as the Vulkan driver fills them */
static int mesa_batch( const IGT_MFXPIC *p, uint32_t *b )
{
	const T_H264JOB	*j = p->job;
	const T_H264SPS	*sps = &j->sps;
	const T_H264PPS	*pps = &j->pps;
	uint32_t	n = 0, i, mocs = p->mocs;
	uint32_t	mbw = sps->pic_width_in_mbs_minus1 + 1;
	uint32_t	pic_height = sps->pic_height_in_map_units_minus1 + 1;

	if ( !( sps->flags & H264_SPS_FRAME_MBS_ONLY ) ) pic_height *= 2;

	{
		struct GFX9_MI_FLUSH_DW c = { GFX9_MI_FLUSH_DW_header };

		c.DWordLength = 2;
		c.VideoPipelineCacheInvalidate = 1;
		GFX9_MI_FLUSH_DW_pack(NULL, b + n, &c);
		n += GFX9_MI_FLUSH_DW_length;
	}
	{
		struct GFX9_MFX_PIPE_MODE_SELECT c = { GFX9_MFX_PIPE_MODE_SELECT_header };

		c.StandardSelect = SS_AVC;
		c.CodecSelect = Decode;
		c.DecoderShortFormatMode = ShortFormatDriverInterface;
		c.DecoderModeSelect = VLDMode;
		c.PreDeblockingOutputEnable = 0;
		c.PostDeblockingOutputEnable = 1;
		GFX9_MFX_PIPE_MODE_SELECT_pack(NULL, b + n, &c);
		n += GFX9_MFX_PIPE_MODE_SELECT_length;
	}
	{
		struct GFX9_MFX_SURFACE_STATE c = { GFX9_MFX_SURFACE_STATE_header };

		c.Width = mbw * 16 - 1;
		c.Height = pic_height * 16 - 1;
		c.SurfaceFormat = MFX_PLANAR_420_8;
		c.InterleaveChroma = 1;
		c.SurfacePitch = p->pitch - 1;
		c.TiledSurface = 1;
		c.TileWalk = TW_YMAJOR;
		c.YOffsetforUCb = c.YOffsetforVCr = p->uv_rows;
		GFX9_MFX_SURFACE_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_SURFACE_STATE_length;
	}
	{
		struct GFX9_MFX_PIPE_BUF_ADDR_STATE c = { GFX9_MFX_PIPE_BUF_ADDR_STATE_header };

		c.PostDeblockingDestinationAddress = p->out.pic;
		c.PreDeblockingDestinationAttributes = MEMATTR(mocs);
		c.PostDeblockingDestinationAttributes = MEMATTR(mocs);
		c.IntraRowStoreScratchBufferAddress = p->row_intra;
		c.IntraRowStoreScratchBufferAttributes = MEMATTR(mocs);
		c.DeblockingFilterRowStoreScratchAddress = p->row_deblock;
		c.DeblockingFilterRowStoreScratchAttributes = MEMATTR(mocs);
		c.MBStatusBufferAttributes = MEMATTR(mocs);
		c.MBILDBStreamOutBufferAttributes = MEMATTR(mocs);
		c.SecondMBILDBStreamOutBufferAttributes = MEMATTR(mocs);
		c.ScaledReferenceSurfaceAttributes = MEMATTR(mocs);
		c.OriginalUncompressedPictureSourceAttributes = MEMATTR(mocs);
		c.StreamOutDataDestinationAttributes = MEMATTR(mocs);
		for ( i = 0; i < j->ndpb; i++ ) c.ReferencePictureAddress[i] = p->ref[i].pic;
		c.ReferencePictureAttributes = MEMATTR(mocs);
		GFX9_MFX_PIPE_BUF_ADDR_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_PIPE_BUF_ADDR_STATE_length;
	}
	{
		struct GFX9_MFX_IND_OBJ_BASE_ADDR_STATE c = { GFX9_MFX_IND_OBJ_BASE_ADDR_STATE_header };

		c.MFXIndirectBitstreamObjectAddress = p->bits;
		c.MFXIndirectBitstreamObjectAttributes = MEMATTR(mocs);
		c.MFXIndirectMVObjectAttributes = MEMATTR(mocs);
		c.MFDIndirectITCOEFFObjectAttributes = MEMATTR(mocs);
		c.MFDIndirectITDBLKObjectAttributes = MEMATTR(mocs);
		c.MFCIndirectPAKBSEObjectAttributes = MEMATTR(mocs);
		GFX9_MFX_IND_OBJ_BASE_ADDR_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_IND_OBJ_BASE_ADDR_STATE_length;
	}
	{
		struct GFX9_MFX_BSP_BUF_BASE_ADDR_STATE c = { GFX9_MFX_BSP_BUF_BASE_ADDR_STATE_header };

		c.BSDMPCRowStoreScratchBufferAddress = p->row_bsd;
		c.BSDMPCRowStoreScratchBufferAttributes = MEMATTR(mocs);
		c.MPRRowStoreScratchBufferAddress = p->row_mpr;
		c.MPRRowStoreScratchBufferAttributes = MEMATTR(mocs);
		c.BitplaneReadBufferAttributes = MEMATTR(mocs);
		GFX9_MFX_BSP_BUF_BASE_ADDR_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_BSP_BUF_BASE_ADDR_STATE_length;
	}
	{
		struct GFX9_MFD_AVC_DPB_STATE c = { GFX9_MFD_AVC_DPB_STATE_header };

		for ( i = 0; i < j->ndpb; i++ ) {
			const T_H264DPB *d = &j->dpb[i];
			unsigned top = ( d->flags & H264_DPB_TOP ) != 0, bot = ( d->flags & H264_DPB_BOTTOM ) != 0;

			c.NonExistingFrame[i] = ( d->flags & H264_DPB_NON_EXISTING ) != 0;
			c.LongTermFrame[i] = ( d->flags & H264_DPB_LONG_TERM ) != 0;
			c.UsedforReference[i] = ( !top && !bot ) ? 3 : ( top | ( bot << 1 ) );
			c.LTSTFrameNumberList[i] = d->frame_idx;
		}
		GFX9_MFD_AVC_DPB_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFD_AVC_DPB_STATE_length;
	}
	{
		struct GFX9_MFD_AVC_PICID_STATE c = { GFX9_MFD_AVC_PICID_STATE_header };

		c.PictureIDRemappingDisable = 0;
		for ( i = 0; i < j->ndpb; i++ ) c.PictureID[i] = j->dpb[i].slot;
		for ( ; i < 16; i++ ) c.PictureID[i] = 0xffff;
		GFX9_MFD_AVC_PICID_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFD_AVC_PICID_STATE_length;
	}
	{
		struct GFX9_MFX_AVC_IMG_STATE c = { GFX9_MFX_AVC_IMG_STATE_header };

		c.FrameWidth = sps->pic_width_in_mbs_minus1;
		c.FrameHeight = pic_height - 1;
		c.FrameSize = mbw * pic_height;
		c.ImageStructure = 0;
		c.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
		c.WeightedPredictionEnable = ( pps->flags & H264_PPS_WEIGHTED_PRED ) != 0;
		c.FirstChromaQPOffset = pps->chroma_qp_index_offset;
		c.SecondChromaQPOffset = pps->second_chroma_qp_index_offset;
		c.FieldPicture = 0;
		c.MBAFFMode = ( sps->flags & H264_SPS_MB_ADAPTIVE_FF ) != 0;
		c.FrameMBOnly = ( sps->flags & H264_SPS_FRAME_MBS_ONLY ) != 0;
		c._8x8IDCTTransformMode = ( pps->flags & H264_PPS_TRANSFORM_8X8 ) != 0;
		c.Direct8x8Inference = ( sps->flags & H264_SPS_DIRECT_8X8_INFERENCE ) != 0;
		c.ConstrainedIntraPrediction = ( pps->flags & H264_PPS_CONSTRAINED_INTRA ) != 0;
		c.NonReferencePicture = !( j->flags & H264_PIC_REFERENCE );
		c.EntropyCodingSyncEnable = ( pps->flags & H264_PPS_CABAC ) != 0;
		c.ChromaFormatIDC = sps->chroma_format_idc;
		c.TrellisQuantizationChromaDisable = true;
		c.NumberofReferenceFrames = j->ndpb;
		c.NumberofActiveReferencePicturesfromL0 = pps->num_ref_idx_l0_default_minus1 + 1;
		c.NumberofActiveReferencePicturesfromL1 = pps->num_ref_idx_l1_default_minus1 + 1;
		c.InitialQPValue = pps->pic_init_qp_minus26;
		c.PicOrderPresent = ( pps->flags & H264_PPS_BOTTOM_FIELD_POC ) != 0;
		c.DeltaPicOrderAlwaysZero = ( sps->flags & H264_SPS_DELTA_POC_ALWAYS_ZERO ) != 0;
		c.PicOrderCountType = sps->pic_order_cnt_type;
		c.DeblockingFilterControlPresent = ( pps->flags & H264_PPS_DEBLOCKING_CONTROL ) != 0;
		c.RedundantPicCountPresent = ( pps->flags & H264_PPS_REDUNDANT_PIC_CNT ) != 0;
		c.Log2MaxFrameNumber = sps->log2_max_frame_num_minus4;
		c.Log2MaxPicOrderCountLSB = sps->log2_max_poc_lsb_minus4;
		c.CurrentPictureFrameNumber = j->frame_num;
		GFX9_MFX_AVC_IMG_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_AVC_IMG_STATE_length;
	}
	{
		unsigned	m, q, k;
		unsigned	nqm = ( pps->flags & H264_PPS_TRANSFORM_8X8 ) ? 4 : 2;

		for ( k = 0; k < nqm; k++ ) {
			struct GFX9_MFX_QM_STATE c = { GFX9_MFX_QM_STATE_header };

			c.DWordLength = 16;
			c.AVC = k;
			if ( k < 2 ) {
				for ( m = 0; m < 3; m++ ) {
					for ( q = 0; q < 16; q++ ) c.ForwardQuantizerMatrix[m * 16 + q] = j->scale.s4x4[m + 3 * k][q];
				}
			} else {
				for ( q = 0; q < 64; q++ ) c.ForwardQuantizerMatrix[q] = j->scale.s8x8[k - 2][q];
			}
			GFX9_MFX_QM_STATE_pack(NULL, b + n, &c);
			n += GFX9_MFX_QM_STATE_length;
		}
	}
	{
		struct GFX9_MFX_AVC_DIRECTMODE_STATE c = { GFX9_MFX_AVC_DIRECTMODE_STATE_header };

		for ( i = 0; i < j->ndpb; i++ ) {
			c.DirectMVBufferAddress[i] = p->ref[i].dmv;
			c.POCList[2 * i] = (uint32_t)j->dpb[i].poc_top;
			c.POCList[2 * i + 1] = (uint32_t)j->dpb[i].poc_bottom;
		}
		c.DirectMVBufferAttributes = MEMATTR(mocs);
		c.DirectMVBufferWriteAddress = p->out.dmv;
		c.DirectMVBufferWriteAttributes = MEMATTR(mocs);
		c.POCList[32] = (uint32_t)j->poc_top;
		c.POCList[33] = (uint32_t)j->poc_bottom;
		GFX9_MFX_AVC_DIRECTMODE_STATE_pack(NULL, b + n, &c);
		n += GFX9_MFX_AVC_DIRECTMODE_STATE_length;
	}
	for ( i = 0; i < j->nslice; i++ ) {
		int last = ( i + 1 == j->nslice );

		if ( !last ) {
			struct GFX9_MFD_AVC_SLICEADDR c = { GFX9_MFD_AVC_SLICEADDR_header };

			c.IndirectBSDDataLength = p->slice[i + 1].data_size;
			c.IndirectBSDDataStartAddress = p->slice[i + 1].data_offset;
			GFX9_MFD_AVC_SLICEADDR_pack(NULL, b + n, &c);
			n += GFX9_MFD_AVC_SLICEADDR_length;
		}
		{
			struct GFX9_MFD_AVC_BSD_OBJECT c = { GFX9_MFD_AVC_BSD_OBJECT_header };

			c.IndirectBSDDataLength = p->slice[i].data_size;
			c.IndirectBSDDataStartAddress = p->slice[i].data_offset;
			c.InlineData.LastSlice = last;
			c.InlineData.FixPrevMBSkipped = 1;
			c.InlineData.IntraPredictionErrorControl = 1;
			c.InlineData.Intra8x84x4PredictionErrorConcealmentControl = 1;
			c.InlineData.ISliceConcealmentMode = 1;
			GFX9_MFD_AVC_BSD_OBJECT_pack(NULL, b + n, &c);
			n += GFX9_MFD_AVC_BSD_OBJECT_length;
		}
	}
	{
		struct GFX9_MI_BATCH_BUFFER_END c = { GFX9_MI_BATCH_BUFFER_END_header };

		GFX9_MI_BATCH_BUFFER_END_pack(NULL, b + n, &c);
		n += GFX9_MI_BATCH_BUFFER_END_length;
		if ( n & 1 ) b[n++] = 0;
	}
	return (int)n;
}

/* A made-up picture: fields of every width at their limits now and then */
static void make( T_H264JOB *j, T_H264SLICE *sl, IGT_MFXPIC *p )
{
	unsigned i;

	memset(j, 0, sizeof(*j));
	memset(p, 0, sizeof(*p));
	j->magic = H264_JOB_MAGIC;
	j->nslice = 1 + rnd() % 12;
	j->ndpb = rnd() % 17;
	j->frame_num = (UH)rnd();
	j->flags = (UB)( rnd() & 3 );
	j->poc_top = (W)( rnd() - ( 1U << 23 ) );
	j->poc_bottom = (W)( rnd() - ( 1U << 23 ) );
	for ( i = 0; i < j->ndpb; i++ ) {
		j->dpb[i].slot = (UB)( rnd() % 17 );
		j->dpb[i].flags = (UB)( rnd() & 0x0f );
		j->dpb[i].frame_idx = (UH)rnd();
		j->dpb[i].poc_top = (W)( rnd() - ( 1U << 23 ) );
		j->dpb[i].poc_bottom = -(W)( rnd() % 1000 );
	}
	j->sps.profile_idc = 100;
	j->sps.chroma_format_idc = 1;
	j->sps.bit_depth_luma = j->sps.bit_depth_chroma = 8;
	j->sps.log2_max_frame_num_minus4 = (UB)( rnd() % 13 );
	j->sps.pic_order_cnt_type = (UB)( rnd() % 3 );
	j->sps.log2_max_poc_lsb_minus4 = (UB)( rnd() % 13 );
	j->sps.max_num_ref_frames = (UB)( rnd() % 17 );
	j->sps.pic_width_in_mbs_minus1 = (UH)( rnd() % 120 );
	j->sps.pic_height_in_map_units_minus1 = (UH)( rnd() % 68 );
	j->sps.flags = rnd() & 0x1f;
	if ( !( j->sps.flags & H264_SPS_FRAME_MBS_ONLY ) ) j->sps.pic_height_in_map_units_minus1 /= 2;
	j->pps.pic_init_qp_minus26 = (B)( (int)( rnd() % 52 ) - 26 );
	j->pps.chroma_qp_index_offset = (B)( (int)( rnd() % 25 ) - 12 );
	j->pps.second_chroma_qp_index_offset = (B)( (int)( rnd() % 25 ) - 12 );
	j->pps.num_ref_idx_l0_default_minus1 = (UB)( rnd() % 32 );
	j->pps.num_ref_idx_l1_default_minus1 = (UB)( rnd() % 32 );
	j->pps.weighted_bipred_idc = (UB)( rnd() % 3 );
	j->pps.flags = rnd() & 0x7f;
	for ( i = 0; i < sizeof(j->scale); i++ ) ( (UB *)&j->scale )[i] = (UB)( 1 + rnd() % 255 );
	for ( i = 0; i < j->nslice; i++ ) {
		sl[i].data_offset = rnd() & 0x3fffff;
		sl[i].data_size = 1 + rnd() % 300000;
	}
	p->job = j;
	p->slice = sl;
	p->pitch = (UW)( 128 * ( 1 + rnd() % 32 ) );
	p->uv_rows = (UW)( 32 * ( 1 + rnd() % 40 ) );
	p->out.pic = ( (UD)rnd() << 12 ) & 0xffffffff000ULL;
	p->out.dmv = ( (UD)rnd() << 6 ) & 0xfffffffffc0ULL;
	for ( i = 0; i < j->ndpb; i++ ) {
		p->ref[i].pic = ( (UD)rnd() << 13 ) & 0xffffffff000ULL;
		p->ref[i].dmv = ( (UD)rnd() << 7 ) & 0xfffffffffc0ULL;
	}
	p->bits = ( (UD)rnd() << 12 ) & 0xffffffff000ULL;
	p->row_intra = ( (UD)rnd() << 6 ) & 0xfffffffffc0ULL;
	p->row_deblock = ( (UD)rnd() << 6 ) & 0xfffffffffc0ULL;
	p->row_bsd = ( (UD)rnd() << 6 ) & 0xfffffffffc0ULL;
	p->row_mpr = ( (UD)rnd() << 6 ) & 0xfffffffffc0ULL;
	p->mocs = 2 << 1;
}

int main( int argc, char **argv )
{
	static UW	ours[4096], theirs[4096];
	T_H264JOB	j;
	T_H264SLICE	sl[H264_MAX_SLICES];
	IGT_MFXPIC	p;
	int		k, n = ( argc > 1 ) ? atoi(argv[1]) : 2000;

	for ( k = 0; k < n; k++ ) {
		int a, b;

		make(&j, sl, &p);
		memset(ours, 0xee, sizeof(ours));
		memset(theirs, 0xee, sizeof(theirs));
		a = igt_mfx_h264(&p, ours, IGT_MFX_MAX_DW(j.nslice));
		b = mesa_batch(&p, theirs);
		if ( a != b ) {
			printf("mfx: picture %d: %d dwords, Mesa's %d\n", k, a, b);
			bad++;
			continue;
		}
		cmp("batch", ours, theirs, a, 0);
		if ( igt_mfx_h264(&p, ours, a - 1) != -1 ) {
			printf("mfx: picture %d: a batch of %d dwords said to fit in %d\n", k, a, a - 1);
			bad++;
		}
	}
	printf("mfx: %d pictures, %u dwords set against Mesa's packing, %u differ\n", n, checked, bad);
	return bad == 0 ? 0 : 1;
}
