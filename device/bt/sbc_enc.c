/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sbc_enc.c
 *	The SBC encoder that A2DP needs (design 6.8), written from appendix B
 *	of the A2DP specification: 8 subbands, 16 blocks, the loudness
 *	allocation, stereo or joint stereo. Every sink must decode all of
 *	these, so no other form is made.
 *
 *	It works in integers, as the kernel has no floating point registers:
 *	the subband samples have 12 fractional bits, which leaves the largest
 *	16 bit sample room to spare in a word.
 *
 *	The file has no other dependency, so that the same source can be built
 *	on a host and its frames given to another decoder.
 */

#include "sbc_enc.h"

#define SBC_SYNC	0x9c
#define Q_SB		12		/* fractional bits of a subband sample */
#define SCALE_MAX	15		/* the highest scale factor */
#define MAX_BITS	16		/* the most bits of one subband's sample */

/*
 * One half of the 80 tap low pass of the specification's table, 31
 * fractional bits; the other half is its mirror. The analysis takes it
 * with the sign turned over in every other run of 16 taps.
 */
static const int proto[41] = {
	0, 336243, 737138, 1191038, 1769354, 2447970,
	3170548, 3830504, 4320362, 4517704, 4283254, 3471542,
	1937362, -383982, -3542770, -7510125, -12153672, -17243030,
	-22459338, -27374476, -31466060, -34154784, -34834004, -32896036,
	-27782384, -19021498, -6279424, 10556558, 31440036, 56070532,
	83913224, 114218864, 146026624, 178208416, 209541552, 238793072,
	264708608, 286183168, 302265856, 312222304, 315583616
};

/* cos(k pi / 16), 30 fractional bits */
static const int cos16[9] = {
	1073741824, 1053110176, 992008094, 892783698, 759250125,
	596539483, 410903207, 209476638, 0
};

/* What the loudness allocation takes from each subband, by the sampling rate 44.1, 48 kHz */
static const signed char offset8[2][8] = {
	{ -4, 0, 0, 0, 0, 0, 1, 2 },
	{ -4, 0, 0, 0, 0, 0, 1, 2 }
};

/* cos(n pi / 16) for any n */
static int cos_n( int n )
{
	if ( n < 0 ) {
		n = -n;
	}
	n %= 32;
	if ( n > 16 ) {
		n = 32 - n;
	}
	return ( n <= 8 ) ? cos16[n] : -cos16[16 - n];
}

void sbc_enc_init( SBC_ENC *e, int rate48, int joint, int bitpool )
{
	int	i, k;

	for ( i = 0; i < 2; i++ ) {
		for ( k = 0; k < 80; k++ ) {
			e->x[i][k] = 0;
		}
	}
	for ( i = 0; i < 80; i++ ) {
		int	v = proto[( i <= 40 ) ? i : 80 - i];

		e->c[i] = ( ( i / 16 ) & 1 ) ? -v : v;
	}
	for ( i = 0; i < 8; i++ ) {
		for ( k = 0; k < 16; k++ ) {
			e->m[i][k] = cos_n(( 2 * i + 1 ) * ( k - 4 ));
		}
	}
	e->rate48 = rate48;
	e->joint = joint;
	e->bitpool = bitpool;
}

int sbc_enc_frame_len( const SBC_ENC *e )
{
	int	bits = 16 * e->bitpool + ( e->joint ? 8 : 0 );

	return 4 + ( 4 * 8 * 2 ) / 8 + ( bits + 7 ) / 8;
}

/* One block of 8 samples of one channel into its 8 subband samples */
static void analyse( SBC_ENC *e, const short *pcm, int ch, int *sb )
{
	int		*x = e->x[ch];
	long long	y[16];
	int		yq[16];
	int		i, k;

	for ( i = 79; i >= 8; i-- ) {
		x[i] = x[i - 8];
	}
	for ( i = 7; i >= 0; i-- ) {
		x[i] = pcm[( 7 - i ) * 2 + ch];
	}
	for ( i = 0; i < 16; i++ ) {
		y[i] = 0;
	}
	for ( i = 0; i < 80; i++ ) {
		y[i & 15] += (long long)e->c[i] * x[i];
	}
	for ( i = 0; i < 16; i++ ) {
		yq[i] = (int)( y[i] >> 16 );		/* 15 fractional bits */
	}
	for ( i = 0; i < 8; i++ ) {
		long long	s = 0;

		for ( k = 0; k < 16; k++ ) {
			s += (long long)e->m[i][k] * yq[k];
		}
		sb[i] = (int)( s >> ( 30 + 15 - Q_SB ) );
	}
}

/* The smallest scale factor that holds the samples of a subband */
static int scale_of( const int *s, int step )
{
	int	i, top = 0, f = 0;

	for ( i = 0; i < 16; i++ ) {
		int	v = s[i * step];

		if ( v < 0 ) {
			v = -v;
		}
		if ( v > top ) {
			top = v;
		}
	}
	while ( f < SCALE_MAX && top >= ( 1 << ( f + 1 + Q_SB ) ) ) {
		f++;
	}
	return f;
}

/*
 * The bits of each subband, both channels drawing on one bit pool: the
 * loudness rule of the specification, stereo and joint stereo alike.
 */
static void allocate( const SBC_ENC *e, const unsigned char sf[2][8], unsigned char bits[2][8] )
{
	int	need[2][8];
	int	ch, sb, top = 0, slice, count = 0, in_slice = 0;
	int	pool = e->bitpool;

	for ( ch = 0; ch < 2; ch++ ) {
		for ( sb = 0; sb < 8; sb++ ) {
			int	n;

			if ( sf[ch][sb] == 0 ) {
				n = -5;
			} else {
				int	loud = sf[ch][sb] - offset8[e->rate48 ? 1 : 0][sb];

				n = ( loud > 0 ) ? loud / 2 : loud;
			}
			need[ch][sb] = n;
			if ( n > top ) {
				top = n;
			}
		}
	}
	slice = top + 1;
	do {
		slice--;
		count += in_slice;
		in_slice = 0;
		for ( ch = 0; ch < 2; ch++ ) {
			for ( sb = 0; sb < 8; sb++ ) {
				if ( need[ch][sb] > slice + 1 && need[ch][sb] < slice + 16 ) {
					in_slice++;
				} else if ( need[ch][sb] == slice + 1 ) {
					in_slice += 2;
				}
			}
		}
	} while ( count + in_slice < pool );
	if ( count + in_slice == pool ) {
		count += in_slice;
		slice--;
	}
	for ( ch = 0; ch < 2; ch++ ) {
		for ( sb = 0; sb < 8; sb++ ) {
			if ( need[ch][sb] < slice + 2 ) {
				bits[ch][sb] = 0;
			} else {
				int	b = need[ch][sb] - slice;

				bits[ch][sb] = (unsigned char)( ( b > MAX_BITS ) ? MAX_BITS : b );
			}
		}
	}
	ch = sb = 0;
	while ( count < pool && sb < 8 ) {
		if ( bits[ch][sb] >= 2 && bits[ch][sb] < MAX_BITS ) {
			bits[ch][sb]++;
			count++;
		} else if ( need[ch][sb] == slice + 1 && pool > count + 1 ) {
			bits[ch][sb] = 2;
			count += 2;
		}
		if ( ch == 1 ) {
			ch = 0;
			sb++;
		} else {
			ch = 1;
		}
	}
	ch = sb = 0;
	while ( count < pool && sb < 8 ) {
		if ( bits[ch][sb] < MAX_BITS ) {
			bits[ch][sb]++;
			count++;
		}
		if ( ch == 1 ) {
			ch = 0;
			sb++;
		} else {
			ch = 1;
		}
	}
}

/* The frame's check: x^8 + x^4 + x^3 + x^2 + 1 from 0x0f, over the bits given */
static unsigned char crc_bits( unsigned char crc, const unsigned char *b, int nbits )
{
	int	i;

	for ( i = 0; i < nbits; i++ ) {
		int	in = ( b[i >> 3] >> ( 7 - ( i & 7 ) ) ) & 1;
		int	top = ( crc >> 7 ) & 1;

		crc = (unsigned char)( crc << 1 );
		if ( top ^ in ) {
			crc ^= 0x1d;
		}
	}
	return crc;
}

typedef struct {
	unsigned char	*p;
	int		bit;
} BITS;

static void put_bits( BITS *w, unsigned int v, int n )
{
	int	i;

	for ( i = n - 1; i >= 0; i-- ) {
		int	byte = w->bit >> 3, m = 7 - ( w->bit & 7 );

		if ( m == 7 ) {
			w->p[byte] = 0;
		}
		w->p[byte] |= (unsigned char)( ( ( v >> i ) & 1 ) << m );
		w->bit++;
	}
}

int sbc_enc_frame( SBC_ENC *e, const short *pcm, unsigned char *out )
{
	int		s[2][16][8];
	unsigned char	sf[2][8], bits[2][8];
	unsigned int	join = 0;
	unsigned char	crc;
	BITS		w;
	int		ch, sb, b, head_bits;

	for ( b = 0; b < 16; b++ ) {
		analyse(e, pcm + b * 8 * 2, 0, s[0][b]);
		analyse(e, pcm + b * 8 * 2, 1, s[1][b]);
	}
	for ( ch = 0; ch < 2; ch++ ) {
		for ( sb = 0; sb < 8; sb++ ) {
			sf[ch][sb] = (unsigned char)scale_of(&s[ch][0][sb], 8);
		}
	}
	if ( e->joint ) {
		/* the last subband is never joined */
		for ( sb = 0; sb < 7; sb++ ) {
			int	m[16], d[16], fm, fd;

			for ( b = 0; b < 16; b++ ) {
				m[b] = ( s[0][b][sb] >> 1 ) + ( s[1][b][sb] >> 1 );
				d[b] = ( s[0][b][sb] >> 1 ) - ( s[1][b][sb] >> 1 );
			}
			fm = scale_of(m, 1);
			fd = scale_of(d, 1);
			if ( fm + fd < sf[0][sb] + sf[1][sb] ) {
				join |= 1u << sb;
				sf[0][sb] = (unsigned char)fm;
				sf[1][sb] = (unsigned char)fd;
				for ( b = 0; b < 16; b++ ) {
					s[0][b][sb] = m[b];
					s[1][b][sb] = d[b];
				}
			}
		}
	}
	allocate(e, sf, bits);

	out[0] = SBC_SYNC;
	out[1] = (unsigned char)( ( ( e->rate48 ? 3 : 2 ) << 6 )	/* the rate */
				| ( 3 << 4 )				/* 16 blocks */
				| ( ( e->joint ? 3 : 2 ) << 2 )		/* the channel mode */
				| ( 0 << 1 )				/* loudness */
				| 1 );					/* 8 subbands */
	out[2] = (unsigned char)e->bitpool;
	w.p = out + 4;
	w.bit = 0;
	if ( e->joint ) {
		for ( sb = 0; sb < 8; sb++ ) {
			put_bits(&w, ( join >> sb ) & 1, 1);
		}
	}
	for ( ch = 0; ch < 2; ch++ ) {
		for ( sb = 0; sb < 8; sb++ ) {
			put_bits(&w, sf[ch][sb], 4);
		}
	}
	head_bits = w.bit;
	crc = crc_bits(0x0f, out + 1, 16);
	crc = crc_bits(crc, out + 4, head_bits);
	out[3] = crc;
	for ( b = 0; b < 16; b++ ) {
		for ( ch = 0; ch < 2; ch++ ) {
			for ( sb = 0; sb < 8; sb++ ) {
				int		n = bits[ch][sb];
				unsigned int	levels;
				long long	q;

				if ( n == 0 ) {
					continue;
				}
				levels = ( 1u << n ) - 1;
				q = ( (long long)s[ch][b][sb] + ( 1ll << ( sf[ch][sb] + 1 + Q_SB ) ) ) * levels;
				q >>= sf[ch][sb] + 2 + Q_SB;
				if ( q < 0 ) {
					q = 0;
				} else if ( q > (long long)levels ) {
					q = levels;
				}
				put_bits(&w, (unsigned int)q, n);
			}
		}
	}
	while ( ( w.bit & 7 ) != 0 ) {
		put_bits(&w, 0, 1);
	}
	return 4 + w.bit / 8;
}
