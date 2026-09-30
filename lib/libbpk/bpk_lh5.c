/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_lh5.c
 *	The LH5 decoder (design 17.16)
 *
 *	The input is read as a stream of bits, most significant first; the
 *	next 16 of them are always in bitbuf. A block starts with its count
 *	of codes and three tables: the code lengths of the code lengths, the
 *	code lengths of the literals and lengths, and those of the
 *	distances. A code of up to 12 bits (8 for the small tables) is found
 *	in one look in its table; a longer one goes on down a tree of left
 *	and right from there.
 *
 *	A code below 256 is a byte; above, it is a length of 3 to 256 and a
 *	distance follows: the number of its bits and then the bits. The
 *	whole output is one buffer, so the window is the output itself.
 */

#include <ts/bpk.h>

#define THRESHOLD	3
#define CBIT		9
#define TBIT		5
#define PBIT		4

/* ---------------------------------------------------------------- bits */

LOCAL UINT next_byte( BPKLH5 *z )
{
	if ( z->inpos < z->inlen ) return z->in[z->inpos++];
	z->over++;
	return 0;
}

/* n bits out of bitbuf, the next n moved in */
LOCAL void fillbuf( BPKLH5 *z, INT n )
{
	z->bitbuf = ( z->bitbuf << n ) & 0xFFFF;
	while ( n > z->bitcount ) {
		n -= z->bitcount;
		z->bitbuf |= ( z->subbitbuf << n ) & 0xFFFF;
		z->subbitbuf = next_byte(z);
		z->bitcount = 8;
	}
	z->bitcount -= n;
	z->bitbuf |= z->subbitbuf >> z->bitcount;
	z->subbitbuf &= ( 1U << z->bitcount ) - 1;
}

LOCAL UINT getbits( BPKLH5 *z, INT n )
{
	UINT	x;

	if ( n == 0 ) return 0;
	x = z->bitbuf >> ( 16 - n );
	fillbuf(z, n);
	return x;
}

/* ---------------------------------------------------------------- tables */

/*
 * The table for codes of the lengths given: every code of up to
 * tablebits bits fills the entries it begins; a longer one takes the
 * entry of its first tablebits bits and a path of nodes from there.
 */
LOCAL BOOL make_table( BPKLH5 *z, INT nchar, const UB *bitlen, INT tablebits, UH *table )
{
	UINT	count[17], weight[17], start[18];
	UINT	i, k, len, avail, nextcode, mask, jutbits;
	INT	ch;
	UH	*p;

	for ( i = 1; i <= 16; i++ ) count[i] = 0;
	for ( ch = 0; ch < nchar; ch++ ) {
		if ( bitlen[ch] > 16 ) return FALSE;
		count[bitlen[ch]]++;
	}
	start[1] = 0;
	for ( i = 1; i <= 16; i++ ) start[i + 1] = start[i] + ( count[i] << ( 16 - i ) );
	if ( start[17] != 0x10000 ) return FALSE;

	jutbits = 16 - (UINT)tablebits;
	for ( i = 1; i <= (UINT)tablebits; i++ ) {
		start[i] >>= jutbits;
		weight[i] = 1U << ( tablebits - i );
	}
	for ( ; i <= 16; i++ ) weight[i] = 1U << ( 16 - i );

	i = start[tablebits + 1] >> jutbits;
	k = 1U << tablebits;
	while ( i < k ) table[i++] = 0;

	avail = (UINT)nchar;
	mask = 1U << ( 15 - tablebits );
	for ( ch = 0; ch < nchar; ch++ ) {
		len = bitlen[ch];
		if ( len == 0 ) continue;
		nextcode = start[len] + weight[len];
		if ( len <= (UINT)tablebits ) {
			if ( nextcode > ( 1U << tablebits ) ) return FALSE;
			for ( i = start[len]; i < nextcode; i++ ) table[i] = (UH)ch;
		} else {
			k = start[len];
			p = &table[k >> jutbits];
			i = len - (UINT)tablebits;
			while ( i != 0 ) {
				if ( *p == 0 ) {
					if ( avail >= 2 * LH5_NC - 1 ) return FALSE;
					z->right[avail] = z->left[avail] = 0;
					*p = (UH)avail++;
				}
				p = ( k & mask ) ? &z->right[*p] : &z->left[*p];
				k <<= 1;
				i--;
			}
			*p = (UH)ch;
		}
		start[len] = nextcode;
	}
	return TRUE;
}

/* A code of the table of nchar codes whose first 8 bits index pt_table */
LOCAL UINT pt_code( BPKLH5 *z, UINT nchar )
{
	UINT	c = z->pt_table[z->bitbuf >> 8], mask = 1U << 7;

	while ( c >= nchar ) {
		if ( mask == 0 || c >= 2 * LH5_NC - 1 ) {
			z->bad = TRUE;
			return 0;
		}
		c = ( z->bitbuf & mask ) ? z->right[c] : z->left[c];
		mask >>= 1;
	}
	fillbuf(z, z->pt_len[c]);
	return c;
}

LOCAL void read_pt_len( BPKLH5 *z, INT nn, INT nbit, INT i_special )
{
	INT	i, n;
	UINT	c, mask;

	n = (INT)getbits(z, nbit);
	if ( n == 0 ) {
		c = getbits(z, nbit);
		for ( i = 0; i < nn; i++ ) z->pt_len[i] = 0;
		for ( i = 0; i < 256; i++ ) z->pt_table[i] = (UH)c;
		return;
	}
	if ( n > nn ) {
		z->bad = TRUE;
		return;
	}
	i = 0;
	while ( i < n ) {
		c = z->bitbuf >> 13;
		if ( c == 7 ) {
			mask = 1U << 12;
			while ( ( mask & z->bitbuf ) != 0 && c < 16 ) {
				mask >>= 1;
				c++;
			}
		}
		fillbuf(z, ( c < 7 ) ? 3 : (INT)c - 3);
		z->pt_len[i++] = (UB)c;
		if ( i == i_special ) {
			INT	k = (INT)getbits(z, 2);

			while ( --k >= 0 && i < nn ) z->pt_len[i++] = 0;
		}
	}
	while ( i < nn ) z->pt_len[i++] = 0;
	if ( !make_table(z, nn, z->pt_len, 8, z->pt_table) ) z->bad = TRUE;
}

LOCAL void read_c_len( BPKLH5 *z )
{
	INT	i, n, k;
	UINT	c;

	n = (INT)getbits(z, CBIT);
	if ( n == 0 ) {
		c = getbits(z, CBIT);
		for ( i = 0; i < LH5_NC; i++ ) z->c_len[i] = 0;
		for ( i = 0; i < 4096; i++ ) z->c_table[i] = (UH)c;
		return;
	}
	if ( n > LH5_NC ) {
		z->bad = TRUE;
		return;
	}
	i = 0;
	while ( i < n && !z->bad ) {
		c = pt_code(z, LH5_NT);
		if ( c <= 2 ) {
			k = ( c == 0 ) ? 1 : ( c == 1 ) ? (INT)getbits(z, 4) + 3 : (INT)getbits(z, CBIT) + 20;
			while ( --k >= 0 && i < LH5_NC ) z->c_len[i++] = 0;
		} else {
			z->c_len[i++] = (UB)( c - 2 );
		}
	}
	while ( i < LH5_NC ) z->c_len[i++] = 0;
	if ( !make_table(z, LH5_NC, z->c_len, 12, z->c_table) ) z->bad = TRUE;
}

/* ---------------------------------------------------------------- codes */

LOCAL UINT decode_c( BPKLH5 *z )
{
	UINT	j, mask;

	if ( z->blocksize == 0 ) {
		z->blocksize = getbits(z, 16);
		if ( z->blocksize == 0 ) {
			z->bad = TRUE;		/* the data ended before the output was whole */
			return 0;
		}
		read_pt_len(z, LH5_NT, TBIT, 3);
		if ( !z->bad ) read_c_len(z);
		if ( !z->bad ) read_pt_len(z, LH5_NP, PBIT, -1);
		if ( z->bad ) return 0;
	}
	z->blocksize--;
	j = z->c_table[z->bitbuf >> 4];
	mask = 1U << 3;
	while ( j >= LH5_NC ) {
		if ( mask == 0 || j >= 2 * LH5_NC - 1 ) {
			z->bad = TRUE;
			return 0;
		}
		j = ( z->bitbuf & mask ) ? z->right[j] : z->left[j];
		mask >>= 1;
	}
	fillbuf(z, z->c_len[j]);
	return j;
}

LOCAL UINT decode_p( BPKLH5 *z )
{
	UINT	j = pt_code(z, LH5_NP);

	if ( j != 0 ) j = ( 1U << ( j - 1 ) ) + getbits(z, (INT)j - 1);
	return j;
}

EXPORT INT lh5_decode( BPKLH5 *z, const UB *in, UINT inlen, UB *out, UINT outlen )
{
	UINT	pos = 0, c, len, dist;

	z->in = in;
	z->inlen = inlen;
	z->inpos = 0;
	z->over = 0;
	z->bitbuf = 0;
	z->subbitbuf = 0;
	z->bitcount = 0;
	z->blocksize = 0;
	z->bad = FALSE;
	fillbuf(z, 16);

	while ( pos < outlen ) {
		c = decode_c(z);
		if ( z->bad || z->over > 4 ) break;
		if ( c < 256 ) {
			out[pos++] = (UB)c;
			continue;
		}
		len = c - ( 256 - THRESHOLD );
		dist = decode_p(z) + 1;
		if ( z->bad ) break;
		while ( len-- > 0 && pos < outlen ) {
			/* before the start the window holds zeroes */
			out[pos] = ( dist <= pos ) ? out[pos - dist] : 0;
			pos++;
		}
	}
	if ( pos == 0 && ( z->bad || z->over > 4 ) ) return -1;
	return (INT)pos;
}
