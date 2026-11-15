/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sbc_dec.c
 *	The SBC decoder that the hands-free profile's mSBC needs (design
 *	6.8), written from section 12.6 of the A2DP specification: the
 *	frame read, its subband samples made again from the scale factors
 *	and the bits the loudness allocation gives them (the same allocation
 *	as the encoder's), and the synthesis filter. It takes mSBC frames
 *	and SBC frames of 8 subbands in one channel, stereo or joint stereo
 *	with the loudness allocation, which is what this system's own
 *	encoder makes.
 *
 *	Integers only, as in the encoder: the subband samples have 12
 *	fractional bits, the synthesis' vector 8.
 *
 *	The file has no other dependency, so that the same source can be built
 *	on a host and checked against another encoder's frames.
 */

#include "sbc_enc.h"

#define SBC_SYNC	0x9c
#define Q_SB		12		/* fractional bits of a subband sample */
#define Q_V		8		/* and of the synthesis' vector */

void sbc_dec_init( SBC_DEC *d )
{
	int	i, k;

	for ( i = 0; i < 2; i++ ) {
		for ( k = 0; k < 160; k++ ) {
			d->v[i][k] = 0;
		}
	}
	/* the window is the analysis' taps times -8: 31 fractional bits become 28 */
	for ( i = 0; i < 80; i++ ) {
		d->d[i] = -sbc_tap(i);
	}
	/* N[k][i] = cos((i + 1/2)(k + 4) pi / 8) */
	for ( k = 0; k < 16; k++ ) {
		for ( i = 0; i < 8; i++ ) {
			d->n[k][i] = sbc_cos(( 2 * i + 1 ) * ( k + 4 ));
		}
	}
}

typedef struct {
	const unsigned char	*p;
	int			bit;
} RBITS;

static unsigned int get_bits( RBITS *r, int n )
{
	unsigned int	v = 0;
	int		i;

	for ( i = 0; i < n; i++ ) {
		v = ( v << 1 ) | ( ( r->p[r->bit >> 3] >> ( 7 - ( r->bit & 7 ) ) ) & 1 );
		r->bit++;
	}
	return v;
}

/* One block of 8 subband samples of one channel into 8 samples, stride apart in pcm */
static void synthesise( SBC_DEC *d, int ch, const int *s, short *pcm, int stride )
{
	int		*v = d->v[ch];
	int		i, j, k;

	for ( i = 159; i >= 16; i-- ) {
		v[i] = v[i - 16];
	}
	for ( k = 0; k < 16; k++ ) {
		long long	a = 0;

		for ( i = 0; i < 8; i++ ) {
			a += (long long)d->n[k][i] * s[i];
		}
		v[k] = (int)( a >> ( 30 + Q_SB - Q_V ) );
	}
	for ( j = 0; j < 8; j++ ) {
		long long	x = 0;
		int		y;

		/* U[16 i + j] is V[32 i + j], U[16 i + 8 + j] is V[32 i + 24 + j]: W = U D, summed by tens */
		for ( i = 0; i < 5; i++ ) {
			x += (long long)v[32 * i + j] * d->d[16 * i + j];
			x += (long long)v[32 * i + 24 + j] * d->d[16 * i + 8 + j];
		}
		y = (int)( ( x + ( 1ll << ( 28 + Q_V - 1 ) ) ) >> ( 28 + Q_V ) );
		if ( y > 32767 ) {
			y = 32767;
		} else if ( y < -32768 ) {
			y = -32768;
		}
		pcm[j * stride] = (short)y;
	}
}

int sbc_dec_frame( SBC_DEC *d, const unsigned char *in, int n, short *pcm, int *p_nch, int *p_len )
{
	int		s[2][16][8];
	unsigned char	sf[2][8], bits[2][8];
	unsigned int	join = 0;
	int		freq, blocks, nch, joint, bitpool, len, ch, sb, b, head_bits;
	unsigned char	crc;
	RBITS		r;

	if ( n < 4 ) {
		return SBC_DEC_SHORT;
	}
	if ( in[0] == MSBC_SYNC ) {
		freq = SBC_FREQ_16K;
		blocks = 15;
		nch = 1;
		joint = 0;
		bitpool = MSBC_BITPOOL;
	} else if ( in[0] == SBC_SYNC ) {
		int	mode = ( in[1] >> 2 ) & 3;

		freq = in[1] >> 6;
		blocks = 4 * ( ( ( in[1] >> 4 ) & 3 ) + 1 );
		/* 4 subbands, SNR, dual channel: not made here */
		if ( ( in[1] & 1 ) == 0 || ( in[1] & 2 ) != 0 || mode == 1 ) {
			return SBC_DEC_FORM;
		}
		nch = ( mode == 0 ) ? 1 : 2;
		joint = ( mode == 3 );
		bitpool = in[2];
		if ( bitpool < 2 || ( nch == 1 && bitpool > 128 ) ) {
			return SBC_DEC_FORM;
		}
	} else {
		return SBC_DEC_SYNC;
	}
	len = 4 + ( 4 * 8 * nch + ( joint ? 8 : 0 ) + blocks * bitpool + 7 ) / 8;
	if ( n < len ) {
		return SBC_DEC_SHORT;
	}
	r.p = in + 4;
	r.bit = 0;
	if ( joint ) {
		for ( sb = 0; sb < 8; sb++ ) {
			join |= get_bits(&r, 1) << sb;
		}
	}
	for ( ch = 0; ch < nch; ch++ ) {
		for ( sb = 0; sb < 8; sb++ ) {
			sf[ch][sb] = (unsigned char)get_bits(&r, 4);
		}
	}
	head_bits = r.bit;
	crc = sbc_crc8(0x0f, in + 1, 16);
	crc = sbc_crc8(crc, in + 4, head_bits);
	if ( crc != in[3] ) {
		return SBC_DEC_CRC;
	}
	sbc_bit_alloc(nch, freq, bitpool, sf, bits);
	for ( b = 0; b < blocks; b++ ) {
		for ( ch = 0; ch < nch; ch++ ) {
			for ( sb = 0; sb < 8; sb++ ) {
				int		k = bits[ch][sb];
				long long	q, levels;

				if ( k == 0 ) {
					s[ch][b][sb] = 0;
					continue;
				}
				/* the scale 2^(sf + 1) times ((2q + 1) / levels - 1) */
				levels = ( 1ll << k ) - 1;
				q = get_bits(&r, k);
				s[ch][b][sb] = (int)( ( ( ( 2 * q + 1 ) << ( sf[ch][sb] + 1 + Q_SB ) ) / levels )
						      - ( 1ll << ( sf[ch][sb] + 1 + Q_SB ) ) );
			}
		}
	}
	if ( joint ) {
		for ( b = 0; b < blocks; b++ ) {
			for ( sb = 0; sb < 8; sb++ ) {
				if ( ( join >> sb ) & 1 ) {
					int	m = s[0][b][sb], dd = s[1][b][sb];

					s[0][b][sb] = m + dd;
					s[1][b][sb] = m - dd;
				}
			}
		}
	}
	for ( b = 0; b < blocks; b++ ) {
		for ( ch = 0; ch < nch; ch++ ) {
			synthesise(d, ch, s[ch][b], pcm + b * 8 * nch + ch, nch);
		}
	}
	if ( p_nch != 0 ) {
		*p_nch = nch;
	}
	if ( p_len != 0 ) {
		*p_len = len;
	}
	return blocks * 8;
}
