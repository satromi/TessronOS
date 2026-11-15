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
 *	SBC with 8 subbands and the loudness allocation (design 6.8). The
 *	encoder makes two forms: A2DP's, 16 blocks, two channels in stereo
 *	or joint stereo at 44.1 or 48 kHz; and mSBC, the hands-free
 *	profile's wide band speech, 15 blocks of one channel at 16 kHz with
 *	the bit pool 26. The decoder (sbc_dec.c) takes mSBC and SBC frames
 *	of 8 subbands, one channel, stereo or joint stereo.
 */

#ifndef __SBC_ENC_H__
#define __SBC_ENC_H__

#define SBC_FRAME_SAMPLES	128		/* samples of each channel in an A2DP frame */
#define SBC_FRAME_MAX		( 4 + 8 + 1 + 16 * 250 / 8 )	/* with the largest bit pool */

#define MSBC_SAMPLES		120		/* samples of an mSBC frame: 7.5 ms at 16 kHz */
#define MSBC_LEN		57		/* the bytes of an mSBC frame */
#define MSBC_SYNC		0xad
#define MSBC_BITPOOL		26

/* The sampling rate's code in the header: also the row of the loudness offsets */
#define SBC_FREQ_16K		0
#define SBC_FREQ_32K		1
#define SBC_FREQ_44K		2
#define SBC_FREQ_48K		3

typedef struct {
	int	x[2][80];		/* the samples the filter still holds, the newest first */
	int	c[80];			/* its taps */
	int	m[8][16];		/* the matrix, 30 fractional bits */
	int	rate48;
	int	joint;
	int	bitpool;
	int	nch;			/* channels: 1 or 2 */
	int	blocks;			/* 15 or 16 */
	int	freq;			/* SBC_FREQ_* */
	int	msbc;			/* the mSBC header */
} SBC_ENC;

void sbc_enc_init( SBC_ENC *e, int rate48, int joint, int bitpool );
void sbc_enc_init_msbc( SBC_ENC *e );
int  sbc_enc_frame_len( const SBC_ENC *e );

/*
 * blocks * 8 samples of each channel, the channels by turns, into a
 * frame; answers its length. SBC_FRAME_SAMPLES pairs for A2DP,
 * MSBC_SAMPLES single samples for mSBC.
 */
int  sbc_enc_frame( SBC_ENC *e, const short *pcm, unsigned char *out );

/* What the encoder and the decoder share */
void sbc_bit_alloc( int nch, int freq, int bitpool, const unsigned char sf[2][8], unsigned char bits[2][8] );
unsigned char sbc_crc8( unsigned char crc, const unsigned char *b, int nbits );
int  sbc_tap( int i );			/* the analysis window's tap i of 80, 31 fractional bits */
int  sbc_cos( int n );			/* cos(n pi / 16), 30 fractional bits */

/* ---------------------------------------------------------------- the decoder */

typedef struct {
	int	v[2][160];		/* the synthesis' vector, the newest first, 8 fractional bits */
	int	d[80];			/* its window, 28 fractional bits */
	int	n[16][8];		/* its matrix, 30 fractional bits */
} SBC_DEC;

#define SBC_DEC_SHORT		(-1)	/* fewer bytes than the frame says it has */
#define SBC_DEC_SYNC		(-2)	/* not a frame's sync byte */
#define SBC_DEC_FORM		(-3)	/* a form not decoded here: 4 subbands, dual channel, SNR */
#define SBC_DEC_CRC		(-4)	/* its check does not match */

void sbc_dec_init( SBC_DEC *d );

/*
 * A frame at in, n bytes there, into pcm (blocks * 8 samples of each
 * channel, the channels by turns): answers the samples of each channel,
 * or one of SBC_DEC_*. *p_nch the channels, *p_len the frame's bytes.
 */
int  sbc_dec_frame( SBC_DEC *d, const unsigned char *in, int n, short *pcm, int *p_nch, int *p_len );

#endif /* __SBC_ENC_H__ */
