/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_fax.c
 *	A compressed picture plane: CCITT T.4, MH and MR (design 17.16)
 *
 *	A picture segment may keep each of its one-bit planes compressed the
 *	way a facsimile is: MH codes every line as runs of white and black,
 *	each run a Huffman code (and codes of 64 and more before it for a long
 *	one); MR codes a line either so or as the changes from the line above
 *	(pass, horizontal, and vertical within three pixels), an EOL and one
 *	bit before each line saying which. The bits come first-bit-highest.
 *
 *	An EOL (eleven 0s or more and a 1) may stand before any line and is
 *	passed over; the lines end when the picture's height is reached, so
 *	the RTC after them is not needed.
 */

#include <ts/bpk.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	UH		run;
	const char	*bits;
} FAXCODE;

/* White runs: the terminating codes 0..63, then the make-up codes */
LOCAL const FAXCODE white[] = {
	{ 0, "00110101" }, { 1, "000111" }, { 2, "0111" }, { 3, "1000" }, { 4, "1011" },
	{ 5, "1100" }, { 6, "1110" }, { 7, "1111" }, { 8, "10011" }, { 9, "10100" },
	{ 10, "00111" }, { 11, "01000" }, { 12, "001000" }, { 13, "000011" }, { 14, "110100" },
	{ 15, "110101" }, { 16, "101010" }, { 17, "101011" }, { 18, "0100111" }, { 19, "0001100" },
	{ 20, "0001000" }, { 21, "0010111" }, { 22, "0000011" }, { 23, "0000100" }, { 24, "0101000" },
	{ 25, "0101011" }, { 26, "0010011" }, { 27, "0100100" }, { 28, "0011000" }, { 29, "00000010" },
	{ 30, "00000011" }, { 31, "00011010" }, { 32, "00011011" }, { 33, "00010010" }, { 34, "00010011" },
	{ 35, "00010100" }, { 36, "00010101" }, { 37, "00010110" }, { 38, "00010111" }, { 39, "00101000" },
	{ 40, "00101001" }, { 41, "00101010" }, { 42, "00101011" }, { 43, "00101100" }, { 44, "00101101" },
	{ 45, "00000100" }, { 46, "00000101" }, { 47, "00001010" }, { 48, "00001011" }, { 49, "01010010" },
	{ 50, "01010011" }, { 51, "01010100" }, { 52, "01010101" }, { 53, "00100100" }, { 54, "00100101" },
	{ 55, "01011000" }, { 56, "01011001" }, { 57, "01011010" }, { 58, "01011011" }, { 59, "01001010" },
	{ 60, "01001011" }, { 61, "00110010" }, { 62, "00110011" }, { 63, "00110100" },
	{ 64, "11011" }, { 128, "10010" }, { 192, "010111" }, { 256, "0110111" }, { 320, "00110110" },
	{ 384, "00110111" }, { 448, "01100100" }, { 512, "01100101" }, { 576, "01101000" }, { 640, "01100111" },
	{ 704, "011001100" }, { 768, "011001101" }, { 832, "011010010" }, { 896, "011010011" },
	{ 960, "011010100" }, { 1024, "011010101" }, { 1088, "011010110" }, { 1152, "011010111" },
	{ 1216, "011011000" }, { 1280, "011011001" }, { 1344, "011011010" }, { 1408, "011011011" },
	{ 1472, "010011000" }, { 1536, "010011001" }, { 1600, "010011010" }, { 1664, "011000" },
	{ 1728, "010011011" },
	{ 0, NULL }
};

/* Black runs, the same way */
LOCAL const FAXCODE black[] = {
	{ 0, "0000110111" }, { 1, "010" }, { 2, "11" }, { 3, "10" }, { 4, "011" },
	{ 5, "0011" }, { 6, "0010" }, { 7, "00011" }, { 8, "000101" }, { 9, "000100" },
	{ 10, "0000100" }, { 11, "0000101" }, { 12, "0000111" }, { 13, "00000100" }, { 14, "00000111" },
	{ 15, "000011000" }, { 16, "0000010111" }, { 17, "0000011000" }, { 18, "0000001000" },
	{ 19, "00001100111" }, { 20, "00001101000" }, { 21, "00001101100" }, { 22, "00000110111" },
	{ 23, "00000101000" }, { 24, "00000010111" }, { 25, "00000011000" }, { 26, "000011001010" },
	{ 27, "000011001011" }, { 28, "000011001100" }, { 29, "000011001101" }, { 30, "000001101000" },
	{ 31, "000001101001" }, { 32, "000001101010" }, { 33, "000001101011" }, { 34, "000011010010" },
	{ 35, "000011010011" }, { 36, "000011010100" }, { 37, "000011010101" }, { 38, "000011010110" },
	{ 39, "000011010111" }, { 40, "000001101100" }, { 41, "000001101101" }, { 42, "000011011010" },
	{ 43, "000011011011" }, { 44, "000001010100" }, { 45, "000001010101" }, { 46, "000001010110" },
	{ 47, "000001010111" }, { 48, "000001100100" }, { 49, "000001100101" }, { 50, "000001010010" },
	{ 51, "000001010011" }, { 52, "000000100100" }, { 53, "000000110111" }, { 54, "000000111000" },
	{ 55, "000000100111" }, { 56, "000000101000" }, { 57, "000001011000" }, { 58, "000001011001" },
	{ 59, "000000101011" }, { 60, "000000101100" }, { 61, "000001011010" }, { 62, "000001100110" },
	{ 63, "000001100111" },
	{ 64, "0000001111" }, { 128, "000011001000" }, { 192, "000011001001" }, { 256, "000001011011" },
	{ 320, "000000110011" }, { 384, "000000110100" }, { 448, "000000110101" },
	{ 512, "0000001101100" }, { 576, "0000001101101" }, { 640, "0000001001010" },
	{ 704, "0000001001011" }, { 768, "0000001001100" }, { 832, "0000001001101" },
	{ 896, "0000001110010" }, { 960, "0000001110011" }, { 1024, "0000001110100" },
	{ 1088, "0000001110101" }, { 1152, "0000001110110" }, { 1216, "0000001110111" },
	{ 1280, "0000001010010" }, { 1344, "0000001010011" }, { 1408, "0000001010100" },
	{ 1472, "0000001010101" }, { 1536, "0000001011010" }, { 1600, "0000001011011" },
	{ 1664, "0000001100100" }, { 1728, "0000001100101" },
	{ 0, NULL }
};

/* The make-up codes of 1792 and more, the same for both */
LOCAL const FAXCODE extra[] = {
	{ 1792, "00000001000" }, { 1856, "00000001100" }, { 1920, "00000001101" },
	{ 1984, "000000010010" }, { 2048, "000000010011" }, { 2112, "000000010100" },
	{ 2176, "000000010101" }, { 2240, "000000010110" }, { 2304, "000000010111" },
	{ 2368, "000000011100" }, { 2432, "000000011101" }, { 2496, "000000011110" },
	{ 2560, "000000011111" },
	{ 0, NULL }
};

/* The modes of an MR line */
#define M_PASS		0
#define M_HORIZ		1
#define M_V0		2		/* M_V0 + d, d from -3 to 3 */
#define M_BAD		-100

LOCAL const FAXCODE modes[] = {
	{ M_PASS, "0001" }, { M_HORIZ, "001" }, { M_V0 + 3, "1" },
	{ M_V0 + 4, "011" }, { M_V0 + 5, "000011" }, { M_V0 + 6, "0000011" },
	{ M_V0 + 2, "010" }, { M_V0 + 1, "000010" }, { M_V0, "0000010" },
	{ 0, NULL }
};

typedef struct {
	const UB	*in;
	UINT		n;		/* bits in all */
	UINT		at;		/* the next bit */
} BITS;

LOCAL INT bit( BITS *b )
{
	if ( b->at >= b->n ) return -1;
	b->at++;
	return ( b->in[( b->at - 1 ) >> 3] >> ( 7 - ( ( b->at - 1 ) & 7 ) ) ) & 1;
}

/* The code of a table the next bits make, its value; -1 when none does */
LOCAL INT code( BITS *b, const FAXCODE *t1, const FAXCODE *t2 )
{
	char	got[16];
	INT	len, c, i;

	for ( len = 0; len < 13; ) {
		if ( ( c = bit(b) ) < 0 ) return -1;
		got[len++] = (char)( '0' + c );
		got[len] = 0;
		for ( i = 0; t1[i].bits != NULL; i++ ) {
			if ( strcmp(t1[i].bits, got) == 0 ) return t1[i].run;
		}
		for ( i = 0; t2 != NULL && t2[i].bits != NULL; i++ ) {
			if ( strcmp(t2[i].bits, got) == 0 ) return t2[i].run;
		}
	}
	return -1;
}

/* A run of one colour: make-up codes, then the terminating one */
LOCAL INT run( BITS *b, BOOL is_black )
{
	INT	total = 0, r;

	for ( ;; ) {
		r = code(b, is_black ? black : white, extra);
		if ( r < 0 ) return -1;
		total += r;
		if ( r < 64 ) return total;
	}
}

/* An EOL passed over, when one stands next */
LOCAL BOOL eol( BITS *b )
{
	UINT	at = b->at;
	INT	zeros = 0, c;

	while ( ( c = bit(b) ) == 0 ) zeros++;
	if ( c == 1 && zeros >= 11 ) return TRUE;
	b->at = at;
	return FALSE;
}

LOCAL void fill( UB *row, INT from, INT to, INT w )
{
	INT	x;

	if ( from < 0 ) from = 0;
	if ( to > w ) to = w;
	for ( x = from; x < to; x++ ) row[x >> 3] |= (UB)( 0x80 >> ( x & 7 ) );
}

/*
 * The first change of the line above past a0 whose colour is not
 * 'is_black' after it; its changes are ref[0..nref), each where the
 * colour turns (to black at even places), w past the last.
 */
LOCAL INT b1_of( const INT *ref, INT nref, INT a0, BOOL is_black, INT *p_k )
{
	INT	k;

	for ( k = 0; k < nref; k++ ) {
		if ( ref[k] > a0 && ( ( k & 1 ) == 0 ) == !is_black ) break;
	}
	*p_k = k;
	return ( k < nref ) ? ref[k] : ref[nref];
}

EXPORT INT bpk_fax_decode( const UB *in, UINT inlen, BOOL mr, INT w, INT h, UB *out, UINT rowbytes )
{
	BITS	b;
	INT	*ref, *cur, nref = 0, ncur, y;
	BOOL	twod = FALSE;
	ER	er = E_OK;

	if ( w <= 0 || h <= 0 || rowbytes < (UINT)( ( w + 7 ) / 8 ) ) return -1;
	ref = malloc(sizeof(INT) * (size_t)( w + 2 ) * 2);
	if ( ref == NULL ) return -1;
	cur = ref + w + 2;
	ref[0] = w;
	memset(out, 0, (size_t)rowbytes * (size_t)h);
	b.in = in;
	b.n = inlen * 8;
	b.at = 0;
	for ( y = 0; y < h && er >= E_OK; y++ ) {
		UB	*row = out + (size_t)y * rowbytes;
		INT	a0 = -1, r1, r2;
		BOOL	is_black = FALSE;

		if ( eol(&b) && mr ) {
			twod = (BOOL)( bit(&b) == 0 );
		} else if ( !mr ) {
			twod = FALSE;
		}
		if ( y == 0 ) twod = FALSE;
		ncur = 0;
		if ( !twod ) {
			INT	x = 0;

			while ( x < w ) {
				if ( ( r1 = run(&b, is_black) ) < 0 ) {
					er = E_PAR;
					break;
				}
				if ( is_black ) fill(row, x, x + r1, w);
				x += r1;
				if ( x < w || is_black ) {
					if ( ncur <= w ) cur[ncur++] = ( x < w ) ? x : w;
				}
				is_black = !is_black;
			}
		} else {
			while ( a0 < w ) {
				INT	k, b1 = b1_of(ref, nref, a0, is_black, &k), b2 = ( k + 1 < nref ) ? ref[k + 1] : w;
				INT	m = code(&b, modes, NULL);

				if ( m == M_PASS ) {
					if ( is_black ) fill(row, a0, b2, w);
					a0 = b2;
				} else if ( m == M_HORIZ ) {
					INT	s = ( a0 < 0 ) ? 0 : a0;

					r1 = run(&b, is_black);
					r2 = ( r1 >= 0 ) ? run(&b, !is_black) : -1;
					if ( r2 < 0 ) {
						er = E_PAR;
						break;
					}
					fill(row, is_black ? s : s + r1, is_black ? s + r1 : s + r1 + r2, w);
					if ( ncur + 2 <= w + 1 ) {
						cur[ncur++] = ( s + r1 < w ) ? s + r1 : w;
						cur[ncur++] = ( s + r1 + r2 < w ) ? s + r1 + r2 : w;
					}
					a0 = s + r1 + r2;
				} else if ( m >= M_V0 && m <= M_V0 + 6 ) {
					INT	a1 = b1 + ( m - M_V0 - 3 );

					if ( a1 < 0 || a1 > w ) {
						er = E_PAR;
						break;
					}
					if ( is_black ) fill(row, a0, a1, w);
					if ( ncur <= w ) cur[ncur++] = a1;
					a0 = a1;
					is_black = !is_black;
				} else {
					er = E_PAR;
					break;
				}
			}
		}
		/* the changes of this line, for the next; w past the last */
		while ( ncur > 0 && cur[ncur - 1] >= w ) ncur--;
		memcpy(ref, cur, sizeof(INT) * (size_t)ncur);
		nref = ncur;
		ref[nref] = w;
		ref[nref + 1] = w;
	}
	free(ref);
	return ( er >= E_OK ) ? (INT)( ( b.at + 7 ) / 8 ) : -1;
}
