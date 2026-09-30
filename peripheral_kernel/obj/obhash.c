/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obhash.c
 *	SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018)
 *	for the passwords of users (design 18.10)
 *
 *	A password is never kept: what is kept is PBKDF2 of it with a salt
 *	of the user's own, so the same password gives a different hash for
 *	each user and guessing costs the iterations every time.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "obj.h"

typedef struct {
	UW	h[8];
	UB	buf[64];
	INT	nbuf;
	UD	total;
} SHA256;

LOCAL CONST UW k256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n)	( ( (x) >> (n) ) | ( (x) << ( 32 - (n) ) ) )

LOCAL void sha_block( SHA256 *s, CONST UB *p )
{
	UW	w[64], a, b, c, d, e, f, g, h, t1, t2;
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		w[i] = ( (UW)p[4 * i] << 24 ) | ( (UW)p[4 * i + 1] << 16 )
		     | ( (UW)p[4 * i + 2] << 8 ) | p[4 * i + 3];
	}
	for ( i = 16; i < 64; i++ ) {
		UW	s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ ( w[i - 15] >> 3 );
		UW	s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ ( w[i - 2] >> 10 );

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0];  b = s->h[1];  c = s->h[2];  d = s->h[3];
	e = s->h[4];  f = s->h[5];  g = s->h[6];  h = s->h[7];
	for ( i = 0; i < 64; i++ ) {
		t1 = h + ( ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25) ) + ( ( e & f ) ^ ( ~e & g ) )
		   + k256[i] + w[i];
		t2 = ( ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22) ) + ( ( a & b ) ^ ( a & c ) ^ ( b & c ) );
		h = g;  g = f;  f = e;  e = d + t1;
		d = c;  c = b;  b = a;  a = t1 + t2;
	}
	s->h[0] += a;  s->h[1] += b;  s->h[2] += c;  s->h[3] += d;
	s->h[4] += e;  s->h[5] += f;  s->h[6] += g;  s->h[7] += h;
}

LOCAL void sha_init( SHA256 *s )
{
	s->h[0] = 0x6a09e667;  s->h[1] = 0xbb67ae85;  s->h[2] = 0x3c6ef372;  s->h[3] = 0xa54ff53a;
	s->h[4] = 0x510e527f;  s->h[5] = 0x9b05688c;  s->h[6] = 0x1f83d9ab;  s->h[7] = 0x5be0cd19;
	s->nbuf = 0;
	s->total = 0;
}

LOCAL void sha_add( SHA256 *s, CONST UB *p, SZ n )
{
	s->total += (UD)n;
	while ( n > 0 ) {
		s->buf[s->nbuf++] = *p++;
		n--;
		if ( s->nbuf == 64 ) {
			sha_block(s, s->buf);
			s->nbuf = 0;
		}
	}
}

LOCAL void sha_end( SHA256 *s, UB out[OB_SHA256_LEN] )
{
	UD	bits = s->total * 8;
	UB	pad = 0x80, zero = 0, len[8];
	INT	i;

	sha_add(s, &pad, 1);
	while ( s->nbuf != 56 ) {
		sha_add(s, &zero, 1);
	}
	for ( i = 0; i < 8; i++ ) {
		len[i] = (UB)( bits >> ( 56 - 8 * i ) );
	}
	sha_add(s, len, 8);
	for ( i = 0; i < 8; i++ ) {
		out[4 * i]     = (UB)( s->h[i] >> 24 );
		out[4 * i + 1] = (UB)( s->h[i] >> 16 );
		out[4 * i + 2] = (UB)( s->h[i] >> 8 );
		out[4 * i + 3] = (UB)s->h[i];
	}
}

EXPORT void knl_ob_sha256( CONST UB *data, SZ len, UB out[OB_SHA256_LEN] )
{
	SHA256	s;

	sha_init(&s);
	sha_add(&s, data, len);
	sha_end(&s, out);
}

/* HMAC-SHA256 of two pieces of message, one after the other */
LOCAL void hmac( CONST UB *key, SZ klen, CONST UB *m1, SZ n1, CONST UB *m2, SZ n2,
		 UB out[OB_SHA256_LEN] )
{
	UB	k[64], pad[64], inner[OB_SHA256_LEN];
	SHA256	s;
	INT	i;

	knl_memset(k, 0, sizeof(k));
	if ( klen > 64 ) {
		knl_ob_sha256(key, klen, k);
	} else {
		knl_memcpy(k, key, (INT)klen);
	}
	for ( i = 0; i < 64; i++ ) pad[i] = k[i] ^ 0x36;
	sha_init(&s);
	sha_add(&s, pad, 64);
	sha_add(&s, m1, n1);
	if ( n2 > 0 ) sha_add(&s, m2, n2);
	sha_end(&s, inner);

	for ( i = 0; i < 64; i++ ) pad[i] = k[i] ^ 0x5c;
	sha_init(&s);
	sha_add(&s, pad, 64);
	sha_add(&s, inner, OB_SHA256_LEN);
	sha_end(&s, out);
}

/* PBKDF2-HMAC-SHA256, one block of output (32 bytes) */
EXPORT void knl_ob_pbkdf2( CONST UB *pw, SZ pwlen, CONST UB *salt, SZ saltlen,
			   INT iter, UB out[OB_SHA256_LEN] )
{
	CONST UB	one[4] = { 0, 0, 0, 1 };
	UB		u[OB_SHA256_LEN];
	INT		i, k;

	hmac(pw, pwlen, salt, saltlen, one, 4, u);
	knl_memcpy(out, u, OB_SHA256_LEN);
	for ( i = 1; i < iter; i++ ) {
		hmac(pw, pwlen, u, OB_SHA256_LEN, NULL, 0, u);
		for ( k = 0; k < OB_SHA256_LEN; k++ ) {
			out[k] ^= u[k];
		}
	}
}
