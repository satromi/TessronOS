/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_vdec.c
 *	H.264 decoded on the video engine of Intel's integrated graphics of
 *	the 9th generation (docs/tessronos/13-gpu.md 13.12), offered as the
 *	device object "動画の復号器" of the kind "h264" (include/ts/h264dec.h)
 *	with the records of the Raspberry Pi 5's HEVC decoder (include/ts/
 *	hevc.h): the program configures the slots, writes one picture's job
 *	and reads a slot's picture as NV12.
 *
 *	Each slot is one piece of the GPU's memory: the picture Y-tiled
 *	(luma, then the interleaved chroma from row uv_rows on), then its
 *	motion vectors for direct prediction. A job's slices go into one
 *	buffer, the batch (igt_mfx.c) into another, and the batch runs on
 *	the video engine as a job of the kernel's (igt_kjob), waited for.
 *	The GPU and the processor share the LLC, so nothing is flushed
 *	either way. A picture read is untiled as it is copied out.
 *
 *	Before the object is made, the decoder is checked once on the
 *	engine with pictures whose result is known without a decoder: an IDR
 *	picture of two slices of I_PCM macroblocks (the samples are in the
 *	stream as they are), then two P pictures of skipped macroblocks
 *	(each a copy of its reference), deblocking off. Every sample has to
 *	come back. Only then is the object made; otherwise there is none and
 *	the programs decode H.264 on the processor as before. The lines
 *	"igt: video ..." say how far it went.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include "obj/obj.h"
#include "igt.h"
#include "igt_mfx.h"
#include "igt_vtest.h"

#define MOCS_WB		( 2 << 1 )	/* index 2 of the table (igt.c mocs_ctl): write-back in the LLC */
#define BATCH_PAGES	4
#define BITS_MAX	H264_JOB_MAX	/* the slices' bytes of a job at most */
#define TOLD_FAIL	16		/* failures told on the console */
#define TOLD_FIRST	3		/* pictures told on the console at first */

typedef struct {
	T_GPUBO	*bo;
	UB	*va;
	UD	gpu;
	UD	npages;
} VBUF;

LOCAL struct {
	BOOL		present;	/* the check passed; the object is made */
	ID		mtx;
	TS_UUID		uuid;
	T_HEVCCONF	conf;
	UW		mbw, mbh;	/* of the configuration */
	UW		pitch, uv_rows, rows;	/* of a slot's picture */
	UD		pic_bytes;	/* a slot's picture, then its motion vectors */
	VBUF		slot[H264_MAX_SLOTS];
	VBUF		rows_buf;	/* the row stores */
	UD		row_off[4];	/* intra, deblocking, bit stream parser, MPR */
	VBUF		bits, batch;
	T_HEVCINFO	info;
	UD		told;
	UD		sum_us;
} vd;

/* ---------------------------------------------------------------- memory */

LOCAL ER vbuf_get( VBUF *b, UD bytes )
{
	T_GPUBOINFO	bi;
	void		*va;
	ER		er;

	b->bo = knl_gpu_bo_create((SZ)bytes, 0, &er);
	if ( b->bo == NULL ) return er;
	er = knl_gpu_bo_kmap(b->bo, &va);
	if ( er < E_OK ) {
		knl_gpu_bo_put(b->bo);
		b->bo = NULL;
		return er;
	}
	(void)knl_gpu_bo_ref(b->bo, &bi);
	b->va = (UB *)va;
	b->gpu = bi.offset;
	b->npages = ( bytes + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	return E_OK;
}

LOCAL void vbuf_put( VBUF *b )
{
	if ( b->bo == NULL ) return;
	knl_gpu_bo_kunmap(b->bo, b->va);
	knl_gpu_bo_put(b->bo);
	knl_memset(b, 0, sizeof(*b));
}

LOCAL void slots_free( void )
{
	INT	i;

	for ( i = 0; i < H264_MAX_SLOTS; i++ ) vbuf_put(&vd.slot[i]);
	vbuf_put(&vd.rows_buf);
	knl_memset(&vd.conf, 0, sizeof(vd.conf));
}

/* The size of the pictures and the slots: made anew */
LOCAL ER conf_set( CONST T_HEVCCONF *c )
{
	UD	dmv, rows_bytes;
	UW	i;
	ER	er;

	if ( c->width == 0 || c->height == 0 || c->width > H264_MAX_WIDTH || c->height > H264_MAX_HEIGHT
	  || ( c->width & 1 ) || ( c->height & 1 ) || c->bitdepth != 8 || c->nslots == 0 || c->nslots > H264_MAX_SLOTS ) {
		return E_PAR;
	}
	slots_free();
	vd.mbw = ( c->width + 15 ) / 16;
	vd.mbh = ( c->height + 15 ) / 16;
	vd.pitch = ( vd.mbw * 16 + 127 ) & ~127U;
	vd.uv_rows = ( vd.mbh * 16 + 31 ) & ~31U;
	vd.rows = vd.uv_rows + ( ( vd.mbh * 8 + 31 ) & ~31U );
	dmv = IGT_MFX_DMV(vd.mbw, vd.mbh);
	vd.pic_bytes = (UD)vd.pitch * vd.rows;		/* whole tiles: a multiple of 4KB */
	for ( i = 0; i < c->nslots; i++ ) {
		er = vbuf_get(&vd.slot[i], vd.pic_bytes + dmv);
		if ( er < E_OK ) goto fail;
	}
	vd.row_off[0] = 0;
	vd.row_off[1] = vd.row_off[0] + IGT_MFX_ROW_INTRA(vd.mbw);
	vd.row_off[2] = vd.row_off[1] + IGT_MFX_ROW_DEBLOCK(vd.mbw);
	vd.row_off[3] = vd.row_off[2] + IGT_MFX_ROW_BSD(vd.mbw);
	rows_bytes = vd.row_off[3] + IGT_MFX_ROW_MPR(vd.mbw);
	er = vbuf_get(&vd.rows_buf, rows_bytes);
	if ( er < E_OK ) goto fail;
	if ( vd.bits.bo == NULL && ( er = vbuf_get(&vd.bits, BITS_MAX) ) < E_OK ) goto fail;
	if ( vd.batch.bo == NULL && ( er = vbuf_get(&vd.batch, BATCH_PAGES * PAGE_SIZE) ) < E_OK ) goto fail;
	vd.conf = *c;
	tm_printf((UB *)"igt: video: %dx%d, %d slots of %ld KB (pitch %d, chroma at row %d)\n", c->width, c->height,
		  c->nslots, (long)( ( vd.pic_bytes + dmv ) >> 10 ), vd.pitch, vd.uv_rows);
	return E_OK;

fail:
	tm_printf((UB *)"igt: video: %dx%d with %d slots: no memory (%d)\n", c->width, c->height, c->nslots, er);
	slots_free();
	return er;
}

/* ---------------------------------------------------------------- the pictures */

/* The byte at (x, y) of a Y-tiled surface: tiles of 128 bytes × 32 rows, in columns of 16 bytes */
Inline UD ytile( UW pitch, UW x, UW y )
{
	return ( (UD)( y >> 5 ) * ( pitch >> 7 ) + ( x >> 7 ) ) * 4096 + ( ( x & 127 ) >> 4 ) * 512 + ( y & 31 ) * 16
	       + ( x & 15 );
}

/* A slot's picture as NV12, from byte 'off' of it */
LOCAL ER picture( UW slot, D off, void *buf, SZ size, SZ *p_asize )
{
	UW	w = vd.conf.width, h = vd.conf.height;
	UD	ysize = (UD)w * h, all = ysize + ysize / 2, at;
	UB	*d = (UB *)buf;
	CONST UB *s;
	SZ	n = 0;

	if ( slot >= vd.conf.nslots || off < 0 ) return E_PAR;
	s = vd.slot[slot].va;
	for ( at = (UD)off; at < all && n < size; ) {
		UW	x, y, run;

		if ( at < ysize ) {
			y = (UW)( at / w );
			x = (UW)( at % w );
		} else {
			y = (UW)( ( at - ysize ) / w ) + vd.uv_rows;
			x = (UW)( ( at - ysize ) % w );
		}
		/* to the end of the column of 16 bytes, of the row, of what is asked */
		run = 16 - ( x & 15 );
		if ( run > w - x ) run = w - x;
		if ( (SZ)run > size - n ) run = (UW)( size - n );
		knl_memcpy(d + n, s + ytile(vd.pitch, x, y), (INT)run);
		n += run;
		at += run;
	}
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

/* ---------------------------------------------------------------- a job */

/* What the job asks of the decoder, checked; E_NOSPT for what it does not do */
LOCAL ER job_check( CONST T_H264JOB *j, SZ size, CONST T_H264SLICE **p_sl, CONST UB **p_bytes, UD *p_nbytes )
{
	CONST T_H264SLICE *sl;
	UD	head, nbytes;
	UW	i, mbh;

	if ( size < (SZ)sizeof(T_H264JOB) || j->magic != H264_JOB_MAGIC || j->size != (UW)size ) return E_PAR;
	if ( j->nslice == 0 || j->nslice > H264_MAX_SLICES || j->ndpb > H264_MAX_DPB ) return E_PAR;
	head = sizeof(T_H264JOB) + (UD)j->nslice * sizeof(T_H264SLICE);
	if ( head > (UD)size ) return E_PAR;
	nbytes = (UD)size - head;
	if ( nbytes > BITS_MAX ) return E_PAR;
	if ( vd.conf.nslots == 0 || j->out_slot >= vd.conf.nslots ) return E_PAR;
	for ( i = 0; i < j->ndpb; i++ ) {
		if ( j->dpb[i].slot >= vd.conf.nslots || j->dpb[i].slot == j->out_slot ) return E_PAR;
	}
	if ( j->sps.chroma_format_idc != 1 || j->sps.bit_depth_luma != 8 || j->sps.bit_depth_chroma != 8
	  || ( j->sps.flags & H264_SPS_FRAME_MBS_ONLY ) == 0 || ( j->sps.flags & H264_SPS_SEPARATE_COLOUR_PLANE )
	  || j->pps.num_slice_groups_minus1 != 0 ) {
		return E_NOSPT;
	}
	mbh = (UW)j->sps.pic_height_in_map_units_minus1 + 1;
	if ( (UW)j->sps.pic_width_in_mbs_minus1 + 1 > vd.mbw || mbh > vd.mbh ) return E_PAR;
	sl = (CONST T_H264SLICE *)( j + 1 );
	for ( i = 0; i < j->nslice; i++ ) {
		if ( sl[i].data_size == 0 || sl[i].data_offset > nbytes || sl[i].data_size > nbytes - sl[i].data_offset ) {
			return E_PAR;
		}
	}
	*p_sl = sl;
	*p_bytes = (CONST UB *)j + head;
	*p_nbytes = nbytes;
	return E_OK;
}

LOCAL void fail_note( CONST T_H264JOB *j, ER er, CONST char *why )
{
	vd.info.errors++;
	vd.info.last_error = (UW)er;
	if ( vd.info.errors <= TOLD_FAIL ) {
		IGTENG	*e = &knl_igt.eng[IGT_VCS];

		tm_printf((UB *)"igt: video: picture %ld (frame_num %d, %d slices, %d references) NOT decoded: %s (%d); "
			  "the engine's jobs %ld, resets %ld\n", (long)( vd.info.decoded + vd.info.errors ),
			  j->frame_num, j->nslice, j->ndpb, why, er, (long)e->jobs, (long)e->resets);
	}
}

/* One picture decoded into its slot; returns when it is (or failed) */
LOCAL ER decode( CONST T_H264JOB *j, SZ size )
{
	CONST T_H264SLICE *sl;
	CONST UB	*bytes;
	UD		nbytes, t0;
	IGT_MFXPIC	p;
	T_GPUBO		*bo[4 + H264_MAX_DPB];
	INT		nbo = 0, len;
	UW		i;
	ER		er;

	if ( !vd.present ) return E_NOEXS;
	er = job_check(j, size, &sl, &bytes, &nbytes);
	if ( er < E_OK ) {
		fail_note(j, er, ( er == E_NOSPT ) ? "not a stream the engine decodes" : "the job does not fit");
		return er;
	}
	knl_memcpy(vd.bits.va, bytes, (INT)nbytes);

	knl_memset(&p, 0, sizeof(p));
	p.job = j;
	p.slice = sl;
	p.pitch = vd.pitch;
	p.uv_rows = vd.uv_rows;
	p.out.pic = vd.slot[j->out_slot].gpu;
	p.out.dmv = vd.slot[j->out_slot].gpu + vd.pic_bytes;
	for ( i = 0; i < j->ndpb; i++ ) {
		p.ref[i].pic = vd.slot[j->dpb[i].slot].gpu;
		p.ref[i].dmv = vd.slot[j->dpb[i].slot].gpu + vd.pic_bytes;
		bo[nbo++] = vd.slot[j->dpb[i].slot].bo;
	}
	p.bits = vd.bits.gpu;
	p.row_intra = vd.rows_buf.gpu + vd.row_off[0];
	p.row_deblock = vd.rows_buf.gpu + vd.row_off[1];
	p.row_bsd = vd.rows_buf.gpu + vd.row_off[2];
	p.row_mpr = vd.rows_buf.gpu + vd.row_off[3];
	p.mocs = MOCS_WB;
	len = igt_mfx_h264(&p, (UW *)vd.batch.va, (INT)( vd.batch.npages * PAGE_SIZE / 4 ));
	if ( len < 0 ) {
		fail_note(j, E_LIMIT, "the batch does not fit");
		return E_PAR;
	}
	bo[nbo++] = vd.slot[j->out_slot].bo;
	bo[nbo++] = vd.bits.bo;
	bo[nbo++] = vd.rows_buf.bo;
	bo[nbo++] = vd.batch.bo;

	t0 = igt_now_us();
	er = igt_kjob(IGT_VCS, vd.batch.gpu, (UW)len * 4, bo, nbo);
	if ( er < E_OK ) {
		fail_note(j, er, "the engine did not finish it");
		return E_IO;
	}
	vd.info.decoded++;
	vd.info.last_us = (UW)( igt_now_us() - t0 );
	vd.sum_us += vd.info.last_us;
	if ( vd.info.decoded <= TOLD_FIRST || ( vd.info.decoded % 3000 ) == 0 ) {
		tm_printf((UB *)"igt: video: picture %ld decoded: frame_num %d, %d slices, %d references, %d bytes, "
			  "batch %d dwords, %d us (%ld us on average)\n", (long)vd.info.decoded, j->frame_num, j->nslice,
			  j->ndpb, (INT)nbytes, len, vd.info.last_us, (long)( vd.sum_us / vd.info.decoded ));
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the object */

LOCAL INT ob_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>The H.264 decoder of the video engine of Intel's graphics (9th generation). "
			  "Record 3: the pictures' size and the slots (T_HEVCCONF, 8 bits); record 1: one picture's job "
			  "(T_H264JOB, its slices, their bytes), decoded as it is written; record 2: a slot's picture as "
			  "NV12 (the slot in the upper 32 bits of the offset); record 4: T_HEVCINFO "
			  "(include/ts/h264dec.h).</p>");
}

LOCAL INT ob_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(j, n, max, ",\"virtual\":false,\"chip\":\"Intel Gen9 VCS\",\"codec\":\"h264\",\"decoded\":");
	return knl_oj_put_num(j, n, max, (INT)vd.info.decoded);
}

LOCAL ER ob_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	ER	er;

	(void)ctx;
	(void)pos;
	tk_loc_mtx(vd.mtx, TMO_FEVR);
	switch ( recno ) {
	case HEVC_REC_PICTURE:
		er = picture((UW)( (UD)off >> 32 ), (D)( (UD)off & 0xFFFFFFFFULL ), buf, size, p_asize);
		break;
	case HEVC_REC_CONF:
	case HEVC_REC_INFO:
		{
			CONST void *src = ( recno == HEVC_REC_CONF ) ? (CONST void *)&vd.conf : (CONST void *)&vd.info;
			SZ	len = ( recno == HEVC_REC_CONF ) ? (SZ)sizeof(vd.conf) : (SZ)sizeof(vd.info);

			er = ( size >= len ) ? E_OK : E_PAR;
			if ( er >= E_OK ) {
				knl_memcpy(buf, src, (INT)len);
				if ( p_asize != NULL ) *p_asize = len;
			}
		}
		break;
	default:
		er = E_NOEXS;
		break;
	}
	tk_unl_mtx(vd.mtx);
	return er;
}

LOCAL ER ob_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	ER	er;

	(void)ctx;
	(void)pos;
	if ( off != 0 ) return E_PAR;
	tk_loc_mtx(vd.mtx, TMO_FEVR);
	switch ( recno ) {
	case HEVC_REC_JOB:
		er = ( size <= H264_JOB_MAX ) ? decode((CONST T_H264JOB *)buf, size) : E_PAR;
		break;
	case HEVC_REC_CONF:
		er = ( size >= (SZ)sizeof(T_HEVCCONF) ) ? conf_set((CONST T_HEVCCONF *)buf) : E_PAR;
		break;
	default:
		er = E_NOEXS;
		break;
	}
	tk_unl_mtx(vd.mtx);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

LOCAL UD ob_size( void *ctx, INT recno )
{
	(void)ctx;
	switch ( recno ) {
	case HEVC_REC_PICTURE:	return (UD)vd.conf.width * vd.conf.height * 3 / 2;
	case HEVC_REC_CONF:	return sizeof(T_HEVCCONF);
	case HEVC_REC_INFO:	return sizeof(T_HEVCINFO);
	default:		return 0;
	}
}

LOCAL CONST T_OBDVOPS vdec_ops = {
	OB_S_VIDEODEC, H264_KIND, HEVC_NREC, 0666, 0, 0,
	ob_text, ob_attr, ob_rea, ob_wri, ob_size, NULL
};

/* ---------------------------------------------------------------- the check at start */

#define T_W		IGT_VTEST_W
#define T_H		IGT_VTEST_H

/* The picture of a slot against the samples of the I_PCM one: the samples wrong */
LOCAL UD t_compare( UW slot, UB *nv12, UW *p_first )
{
	UD	bad = 0, i;
	SZ	got = 0;
	UW	x, y;

	*p_first = 0xffffffff;
	knl_memset(nv12, 0, T_W * T_H * 3 / 2);
	if ( picture(slot, 0, nv12, T_W * T_H * 3 / 2, &got) < E_OK || got != T_W * T_H * 3 / 2 ) return T_W * T_H * 3 / 2;
	for ( i = 0; i < (UD)T_W * T_H * 3 / 2; i++ ) {
		UB	want;

		if ( i < (UD)T_W * T_H ) {
			x = (UW)( i % T_W );
			y = (UW)( i / T_W );
			want = igt_vtest_sample(0, x, y);
		} else {
			x = (UW)( ( i - T_W * T_H ) % T_W );
			y = (UW)( ( i - T_W * T_H ) / T_W );
			want = igt_vtest_sample(( x & 1 ) ? 2 : 1, x / 2, y);
		}
		if ( nv12[i] != want ) {
			if ( bad == 0 ) *p_first = (UW)i | ( (UW)nv12[i] << 24 );
			bad++;
		}
	}
	return bad;
}

/* The check: TRUE when all three pictures came back right */
LOCAL BOOL t_check( void )
{
	T_HEVCCONF	c = { T_W, T_H, 8, 3 };
	SZ		max = 64 * 1024, len;
	UB		*buf = (UB *)Kmalloc(max), *nv12 = (UB *)Kmalloc(T_W * T_H * 3 / 2);
	UW		k, first;
	UD		bad;
	BOOL		ok = TRUE;
	ER		er;

	if ( buf == NULL || nv12 == NULL ) {
		tm_printf((UB *)"igt: video test: no memory\n");
		ok = FALSE;
		goto out;
	}
	er = conf_set(&c);
	if ( er < E_OK ) {
		ok = FALSE;
		goto out;
	}
	for ( k = 0; k < IGT_VTEST_PICS && ok; k++ ) {
		len = igt_vtest_job(buf, max, k, k, ( k + 2 ) % 3);
		if ( len < 0 ) {
			ok = FALSE;
			break;
		}
		/* the slot written over first with a pattern the decoder does not make */
		knl_memset(vd.slot[k].va, 0xa5, (INT)vd.pic_bytes);
		er = decode((CONST T_H264JOB *)buf, len);
		bad = ( er >= E_OK ) ? t_compare(k, nv12, &first) : 0;
		tm_printf((UB *)"igt: video test: %s picture %d (%ld bytes): %s, %d us; %ld of %d samples wrong",
			  ( k == 0 ) ? "I_PCM" : "P_Skip", k, (long)len, ( er >= E_OK ) ? "decoded" : "NOT decoded",
			  vd.info.last_us, (long)bad, T_W * T_H * 3 / 2);
		if ( bad != 0 ) {
			tm_printf((UB *)" (the first at byte %d reads %02x)", first & 0xffffff, first >> 24);
		}
		tm_printf((UB *)"\n");
		if ( er < E_OK || bad != 0 ) ok = FALSE;
	}
	if ( !ok ) {
		IGTENG	*e = &knl_igt.eng[IGT_VCS];

		igt_eng_dump("after the check", e);
	}
out:
	slots_free();
	if ( buf != NULL ) Kfree(buf);
	if ( nv12 != NULL ) Kfree(nv12);
	return ok;
}

/* From the start-up task (igt.c), once the video engine is up */
EXPORT void igt_vdec_start( void )
{
	T_CMTX	cm;
	ER	er;

	if ( igt_setting("igtvdec", 1) == 0 ) {
		tm_printf((UB *)"igt: video: the H.264 decoder left off (igtvdec=0)\n");
		return;
	}
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cm.ceilpri = 0;
	vd.mtx = tk_cre_mtx(&cm);
	if ( vd.mtx <= 0 ) return;
	vd.present = TRUE;			/* for the check's own jobs */
	tk_loc_mtx(vd.mtx, TMO_FEVR);
	if ( !t_check() ) {
		vd.present = FALSE;
		tk_unl_mtx(vd.mtx);
		tm_printf((UB *)"igt: video: the check did NOT pass: no decoder object, H.264 is decoded by the processor\n");
		return;
	}
	knl_memset(&vd.info, 0, sizeof(vd.info));
	vd.sum_us = 0;
	vd.info.present = 1;
	vd.info.version = 90;
	tk_unl_mtx(vd.mtx);
	er = knl_obdev_add((CONST UB *)HEVC_OBJ_NAME, NULL, &vdec_ops, NULL, &vd.uuid);
	tm_printf((UB *)"igt: video: the check passed; the H.264 decoder is the object \"%s\" (%d)\n", HEVC_OBJ_NAME, er);
}

#if USE_KTEST
/*
 * For the tests on a machine without the device (tests/ktest/x86_64/
 * ktest_igt.c): the jobs of the check, the untiling of a slot's
 * picture, and the checks of a job, on memory of the test's
 */
EXPORT UD igt_vdec_test_ytile( UW pitch, UW x, UW y )
{
	return ytile(pitch, x, y);
}

/* The NV12 of a slot read from 'surf' laid as conf_set lays a picture of w × h */
EXPORT ER igt_vdec_test_read( UB *surf, UW w, UW h, D off, void *buf, SZ size, SZ *p_asize )
{
	T_HEVCCONF	save = vd.conf;
	VBUF		s0 = vd.slot[0];
	UW		pitch = vd.pitch, uv = vd.uv_rows;
	ER		er;

	vd.conf.width = w;
	vd.conf.height = h;
	vd.conf.nslots = 1;
	vd.pitch = ( ( ( w + 15 ) / 16 ) * 16 + 127 ) & ~127U;
	vd.uv_rows = ( ( ( h + 15 ) / 16 ) * 16 + 31 ) & ~31U;
	vd.slot[0].va = surf;
	er = picture(0, off, buf, size, p_asize);
	vd.conf = save;
	vd.slot[0] = s0;
	vd.pitch = pitch;
	vd.uv_rows = uv;
	return er;
}

EXPORT ER igt_vdec_test_check( CONST T_H264JOB *j, SZ size, UW w, UW h, UW nslots )
{
	T_HEVCCONF	save = vd.conf;
	UW		mbw = vd.mbw, mbh = vd.mbh;
	CONST T_H264SLICE *sl;
	CONST UB	*bytes;
	UD		nbytes;
	ER		er;

	vd.conf.width = w;
	vd.conf.height = h;
	vd.conf.nslots = nslots;
	vd.mbw = ( w + 15 ) / 16;
	vd.mbh = ( h + 15 ) / 16;
	er = job_check(j, size, &sl, &bytes, &nbytes);
	vd.conf = save;
	vd.mbw = mbw;
	vd.mbh = mbh;
	return er;
}
#endif

#endif /* X64 */
