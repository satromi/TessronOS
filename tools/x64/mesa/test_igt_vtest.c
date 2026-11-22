/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	test_igt_vtest.c (host)
 *	The pictures the video engine's H.264 decoder is checked with at
 *	start (device/gpu/intel/igt_vtest.c) decoded by FFmpeg's decoder:
 *	the parameter sets, then each job's slices with start codes, and
 *	every sample of every picture has to be the one the check expects
 *	(docs/tessronos/13-gpu.md 13.12). So a picture the engine gets wrong
 *	is the engine's doing, not the stream's. Built and run by
 *	igt_mfx_test.sh with FFmpeg's libraries for the host.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "kernel.h"
#include "igt_vtest.h"

static int check( const AVFrame *f, int k )
{
	int	bad = 0, x, y;

	if ( f->width != IGT_VTEST_W || f->height != IGT_VTEST_H || f->format != AV_PIX_FMT_YUV420P ) {
		printf("vtest: picture %d: %dx%d format %d\n", k, f->width, f->height, f->format);
		return 1;
	}
	for ( y = 0; y < IGT_VTEST_H; y++ ) {
		for ( x = 0; x < IGT_VTEST_W; x++ ) {
			if ( f->data[0][y * f->linesize[0] + x] != igt_vtest_sample(0, (UW)x, (UW)y) ) bad++;
		}
	}
	for ( y = 0; y < IGT_VTEST_H / 2; y++ ) {
		for ( x = 0; x < IGT_VTEST_W / 2; x++ ) {
			if ( f->data[1][y * f->linesize[1] + x] != igt_vtest_sample(1, (UW)x, (UW)y) ) bad++;
			if ( f->data[2][y * f->linesize[2] + x] != igt_vtest_sample(2, (UW)x, (UW)y) ) bad++;
		}
	}
	printf("vtest: picture %d: %d of %d samples wrong\n", k, bad, IGT_VTEST_W * IGT_VTEST_H * 3 / 2);
	return bad != 0;
}

int main( void )
{
	static UB	job[65536], pkt_buf[65536 + AV_INPUT_BUFFER_PADDING_SIZE];
	const AVCodec	*codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	AVCodecContext	*ctx = codec ? avcodec_alloc_context3(codec) : NULL;
	AVFrame		*f = av_frame_alloc();
	AVPacket	*pkt = av_packet_alloc();
	int		k, got = 0, bad = 0;

	if ( ctx != NULL ) ctx->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
	if ( ctx == NULL || avcodec_open2(ctx, codec, NULL) < 0 ) {
		printf("vtest: no H.264 decoder\n");
		return 1;
	}
	for ( k = 0; k < IGT_VTEST_PICS; k++ ) {
		SZ		len = igt_vtest_job(job, sizeof(job), (UW)k, (UW)k, (UW)( ( k + 2 ) % 3 ));
		const T_H264JOB	*j = (const T_H264JOB *)job;
		const T_H264SLICE *sl = (const T_H264SLICE *)( j + 1 );
		const UB	*bytes = job + sizeof(T_H264JOB) + j->nslice * sizeof(T_H264SLICE);
		size_t		n = 0;
		UW		i;

		if ( len < 0 ) {
			printf("vtest: picture %d: no job (%ld)\n", k, (long)len);
			return 1;
		}
		if ( k == 0 ) n = (size_t)igt_vtest_params(pkt_buf, 256);
		for ( i = 0; i < j->nslice; i++ ) {
			static const UB sc[4] = { 0, 0, 0, 1 };

			memcpy(pkt_buf + n, sc, 4);
			memcpy(pkt_buf + n + 4, bytes + sl[i].data_offset, sl[i].data_size);
			n += 4 + sl[i].data_size;
		}
		memset(pkt_buf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		printf("vtest: picture %d: %u slices, %zu bytes\n", k, j->nslice, n);
		pkt->data = pkt_buf;
		pkt->size = (int)n;
		if ( avcodec_send_packet(ctx, pkt) < 0 ) {
			printf("vtest: picture %d not taken\n", k);
			bad++;
		}
		while ( avcodec_receive_frame(ctx, f) == 0 ) bad += check(f, got++);
	}
	avcodec_send_packet(ctx, NULL);
	while ( avcodec_receive_frame(ctx, f) == 0 ) bad += check(f, got++);
	if ( got != IGT_VTEST_PICS ) {
		printf("vtest: %d pictures out of %d\n", got, IGT_VTEST_PICS);
		bad++;
	}
	printf("vtest: %s\n", bad ? "FAILED" : "all samples as expected");
	return bad != 0;
}
