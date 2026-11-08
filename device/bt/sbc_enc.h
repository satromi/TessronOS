/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sbc_enc.h
 *	The SBC encoder: 8 subbands, 16 blocks, the loudness allocation,
 *	two channels in stereo or joint stereo, 44.1 or 48 kHz.
 */

#ifndef __SBC_ENC_H__
#define __SBC_ENC_H__

#define SBC_FRAME_SAMPLES	128		/* samples of each channel in a frame */
#define SBC_FRAME_MAX		( 4 + 8 + 1 + 16 * 250 / 8 )	/* with the largest bit pool */

typedef struct {
	int	x[2][80];		/* the samples the filter still holds, the newest first */
	int	c[80];			/* its taps */
	int	m[8][16];		/* the matrix, 30 fractional bits */
	int	rate48;
	int	joint;
	int	bitpool;
} SBC_ENC;

void sbc_enc_init( SBC_ENC *e, int rate48, int joint, int bitpool );
int  sbc_enc_frame_len( const SBC_ENC *e );

/* SBC_FRAME_SAMPLES pairs of samples, left and right by turns, into a frame; answers its length */
int  sbc_enc_frame( SBC_ENC *e, const short *pcm, unsigned char *out );

#endif /* __SBC_ENC_H__ */
