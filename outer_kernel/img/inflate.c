/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	inflate.c
 *	Undoing DEFLATE (RFC 1951), for the pictures in records
 *
 *	This is the whole of what a PNG needs: stored blocks, the fixed
 *	code, and the dynamic code with its own code lengths. Nothing
 *	compresses here -- a system that only reads pictures never needs
 *	to -- so what is written is the reading half and no more.
 *
 *	The decoder writes into a buffer the caller sizes. A PNG says how
 *	large its image is before the data begins, so the caller always
 *	knows; a stream that turns out to be longer than it said is not
 *	unpacked past the end, it is refused. Growing a buffer to fit
 *	whatever arrives is how a picture in a file becomes a way into
 *	the machine.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/img.h>

#define MAX_BITS	15
#define NLIT		288		/* literal/length codes */
#define NDIST		30		/* distance codes */
#define NCLEN		19		/* the lengths of the lengths */

/* A canonical Huffman code: how many of each length, and the symbols in order */
typedef struct {
	UH	count[MAX_BITS + 1];
	UH	sym[NLIT];
} HUFF;

typedef struct {
	CONST UB	*in;
	SZ		in_len;
	SZ		in_at;		/* bytes taken */
	UINT		bit_buf;
	UINT		bit_cnt;

	UB		*out;
	SZ		out_len;
	SZ		out_at;
	BOOL		over;		/* asked to write past the end */
} INF;

/* ---------------------------------------------------------------- bits */

LOCAL INT bits( INF *z, UINT n )
{
	UINT	v;

	while ( z->bit_cnt < n ) {
		if ( z->in_at >= z->in_len ) {
			return -1;		/* the stream ended in the middle */
		}
		z->bit_buf |= (UINT)z->in[z->in_at++] << z->bit_cnt;
		z->bit_cnt += 8;
	}
	v = z->bit_buf & ( ( 1U << n ) - 1 );
	z->bit_buf >>= n;
	z->bit_cnt -= n;

	return (INT)v;
}

/*
 * One symbol, read a bit at a time down the lengths. The codes are
 * canonical, so the first code of each length is known from how many
 * shorter codes there were, and a code is found by comparing.
 */
LOCAL INT decode( INF *z, CONST HUFF *h )
{
	INT	code = 0, first = 0, index = 0;
	UINT	len;

	for ( len = 1; len <= MAX_BITS; len++ ) {
		INT	b = bits(z, 1);

		if ( b < 0 ) {
			return -1;
		}
		code |= b;
		{
			INT	count = (INT)h->count[len];

			if ( code - first < count ) {
				return (INT)h->sym[index + ( code - first )];
			}
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
	}

	return -1;				/* no code that long */
}

/* The canonical code for a set of lengths */
LOCAL ER huff_make( HUFF *h, CONST UB *len, INT n )
{
	INT	i;
	UH	offs[MAX_BITS + 1];
	INT	left;

	for ( i = 0; i <= MAX_BITS; i++ ) {
		h->count[i] = 0;
	}
	for ( i = 0; i < n; i++ ) {
		h->count[len[i]]++;
	}
	if ( h->count[0] == n ) {
		return E_OK;			/* nothing is coded */
	}
	/* every code must be used, and none twice */
	left = 1;
	for ( i = 1; i <= MAX_BITS; i++ ) {
		left <<= 1;
		left -= (INT)h->count[i];
		if ( left < 0 ) {
			return E_PAR;		/* over-subscribed */
		}
	}
	offs[1] = 0;
	for ( i = 1; i < MAX_BITS; i++ ) {
		offs[i + 1] = (UH)( offs[i] + h->count[i] );
	}
	for ( i = 0; i < n; i++ ) {
		if ( len[i] != 0 ) {
			h->sym[offs[len[i]]++] = (UH)i;
		}
	}

	return E_OK;
}

/* ---------------------------------------------------------------- blocks */

LOCAL CONST UH	len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
	51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
LOCAL CONST UB	len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
	3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
LOCAL CONST UH	dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
	385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
	16385, 24577
};
LOCAL CONST UB	dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
	9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

LOCAL void put( INF *z, UB b )
{
	if ( z->out_at < z->out_len ) {
		z->out[z->out_at++] = b;
	} else {
		z->over = TRUE;
	}
}

LOCAL ER block_codes( INF *z, CONST HUFF *lit, CONST HUFF *dist )
{
	for (;;) {
		INT	sym = decode(z, lit);

		if ( sym < 0 ) {
			return E_PAR;
		}
		if ( sym < 256 ) {
			put(z, (UB)sym);
			continue;
		}
		if ( sym == 256 ) {
			return E_OK;		/* the end of the block */
		}
		sym -= 257;
		if ( sym >= 29 ) {
			return E_PAR;
		}
		{
			INT	len = (INT)len_base[sym];
			INT	e = bits(z, len_extra[sym]);
			INT	d, off;

			if ( e < 0 ) {
				return E_PAR;
			}
			len += e;

			d = decode(z, dist);
			if ( d < 0 || d >= NDIST ) {
				return E_PAR;
			}
			off = (INT)dist_base[d];
			e = bits(z, dist_extra[d]);
			if ( e < 0 ) {
				return E_PAR;
			}
			off += e;
			if ( (SZ)off > z->out_at ) {
				return E_PAR;	/* a copy from before the start */
			}
			while ( len-- > 0 ) {
				put(z, z->out[z->out_at - (SZ)off]);
			}
		}
		if ( z->over ) {
			return E_PAR;
		}
	}
}

LOCAL ER block_fixed( INF *z )
{
	LOCAL HUFF	lit, dist;
	LOCAL BOOL	made = FALSE;

	if ( !made ) {
		UB	len[NLIT];
		INT	i;

		for ( i = 0; i < 144; i++ )  len[i] = 8;
		for ( ; i < 256; i++ )       len[i] = 9;
		for ( ; i < 280; i++ )       len[i] = 7;
		for ( ; i < NLIT; i++ )      len[i] = 8;
		if ( huff_make(&lit, len, NLIT) < E_OK ) {
			return E_PAR;
		}
		for ( i = 0; i < NDIST; i++ ) {
			len[i] = 5;
		}
		if ( huff_make(&dist, len, NDIST) < E_OK ) {
			return E_PAR;
		}
		made = TRUE;
	}

	return block_codes(z, &lit, &dist);
}

LOCAL ER block_dynamic( INF *z )
{
	CONST UB	order[NCLEN] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	HUFF	lit, dist, clen;
	UB	len[NLIT + NDIST];
	INT	nlen, ndist, ncode, i;

	nlen  = bits(z, 5);
	ndist = bits(z, 5);
	ncode = bits(z, 4);
	if ( nlen < 0 || ndist < 0 || ncode < 0 ) {
		return E_PAR;
	}
	nlen  += 257;
	ndist += 1;
	ncode += 4;
	if ( nlen > NLIT || ndist > NDIST ) {
		return E_PAR;
	}
	for ( i = 0; i < NCLEN; i++ ) {
		len[i] = 0;
	}
	for ( i = 0; i < ncode; i++ ) {
		INT	v = bits(z, 3);

		if ( v < 0 ) {
			return E_PAR;
		}
		len[order[i]] = (UB)v;
	}
	if ( huff_make(&clen, len, NCLEN) < E_OK ) {
		return E_PAR;
	}
	i = 0;
	while ( i < nlen + ndist ) {
		INT	sym = decode(z, &clen);
		INT	n = 0;
		UB	v = 0;

		if ( sym < 0 ) {
			return E_PAR;
		}
		if ( sym < 16 ) {
			len[i++] = (UB)sym;
			continue;
		}
		if ( sym == 16 ) {
			INT	e;

			if ( i == 0 ) {
				return E_PAR;
			}
			v = len[i - 1];
			e = bits(z, 2);
			if ( e < 0 ) {
				return E_PAR;
			}
			n = 3 + e;
		} else if ( sym == 17 ) {
			INT	e = bits(z, 3);

			if ( e < 0 ) {
				return E_PAR;
			}
			n = 3 + e;
		} else {
			INT	e = bits(z, 7);

			if ( e < 0 ) {
				return E_PAR;
			}
			n = 11 + e;
		}
		if ( i + n > nlen + ndist ) {
			return E_PAR;
		}
		while ( n-- > 0 ) {
			len[i++] = v;
		}
	}
	if ( huff_make(&lit, len, nlen) < E_OK
	  || huff_make(&dist, len + nlen, ndist) < E_OK ) {
		return E_PAR;
	}

	return block_codes(z, &lit, &dist);
}

LOCAL ER block_stored( INF *z )
{
	UINT	n;

	z->bit_buf = 0;				/* stored blocks start on a byte */
	z->bit_cnt = 0;
	if ( z->in_at + 4 > z->in_len ) {
		return E_PAR;
	}
	n = (UINT)z->in[z->in_at] | ( (UINT)z->in[z->in_at + 1] << 8 );
	z->in_at += 4;				/* the length, and its complement */
	if ( z->in_at + n > z->in_len ) {
		return E_PAR;
	}
	while ( n-- > 0 ) {
		put(z, z->in[z->in_at++]);
	}

	return ( z->over ) ? E_PAR : E_OK;
}

/* ---------------------------------------------------------------- the call */

EXPORT INT ts_inflate( CONST UB *in, SZ in_len, UB *out, SZ out_len )
{
	INF	z;
	ER	er = E_OK;

	if ( in == NULL || out == NULL || in_len == 0 || out_len == 0 ) {
		return E_PAR;
	}
	knl_memset(&z, 0, sizeof(z));
	z.in = in;
	z.in_len = in_len;
	z.out = out;
	z.out_len = out_len;

	for (;;) {
		INT	last = bits(&z, 1);
		INT	type = bits(&z, 2);

		if ( last < 0 || type < 0 ) {
			return E_PAR;
		}
		switch ( type ) {
		case 0:		er = block_stored(&z);   break;
		case 1:		er = block_fixed(&z);    break;
		case 2:		er = block_dynamic(&z);  break;
		default:	return E_PAR;	/* the reserved kind */
		}
		if ( er < E_OK ) {
			return er;
		}
		if ( last ) {
			break;
		}
	}

	return (INT)z.out_at;
}

/*
 * A zlib stream (RFC 1950): two bytes of head, the deflate data, and a
 * checksum this does not look at. The checksum is what a stream from
 * elsewhere would be checked with; what is read here came out of a file
 * this system wrote or was given, and a wrong picture is a wrong
 * picture either way.
 */
EXPORT INT ts_zlib_inflate( CONST UB *in, SZ in_len, UB *out, SZ out_len )
{
	if ( in == NULL || in_len < 6 ) {
		return E_PAR;
	}
	if ( ( in[0] & 0x0F ) != 8 ) {
		return E_PAR;			/* not deflate */
	}
	if ( ( ( (UINT)in[0] << 8 ) | in[1] ) % 31 != 0 ) {
		return E_PAR;			/* the head does not check out */
	}
	if ( ( in[1] & 0x20 ) != 0 ) {
		return E_NOSPT;			/* a preset dictionary */
	}

	return ts_inflate(in + 2, in_len - 2, out, out_len);
}
