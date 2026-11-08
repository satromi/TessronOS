/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_crypto.c
 *	The functions of LE pairing (Bluetooth Core, Vol 3 Part H, 2.2) and
 *	its keys on the curve P-256, on mbed TLS (design 6.8).
 *
 *	Every value here is given and taken as SMP has it on the air, the
 *	lowest byte first. The specification writes its functions with the
 *	most significant byte first, and AES and CMAC take their blocks so,
 *	which is why each function turns its arguments round.
 *
 *	Nothing here keeps state, so the test (ktest_bt.c) gives the
 *	specification's sample data straight to these same functions.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/uuid.h>
#include "bt_crypto.h"

#include "mbedtls/aes.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

EXPORT void *knl_bts_calloc( size_t n, size_t size )
{
	return Kcalloc((SZ)n, (SZ)size);
}

EXPORT void knl_bts_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

LOCAL void turned( UB *to, CONST UB *from, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) to[i] = from[n - 1 - i];
}

/* AES-128 of one block, key and block with the most significant byte first */
LOCAL void aes_msb( CONST UB *k, CONST UB *in, UB *out )
{
	mbedtls_aes_context	a;

	mbedtls_aes_init(&a);
	(void)mbedtls_aes_setkey_enc(&a, k, 128);
	(void)mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, in, out);
	mbedtls_aes_free(&a);
}

/* The security function e, the lowest byte first */
EXPORT void knl_bts_e( CONST UB *k, CONST UB *in, UB *out )
{
	UB	km[16], im[16], om[16];

	turned(km, k, 16);
	turned(im, in, 16);
	aes_msb(km, im, om);
	turned(out, om, 16);
}

/* Doubling in the field of CMAC's subkeys */
LOCAL void cmac_double( UB *b )
{
	INT	i, top = b[0] >> 7;

	for ( i = 0; i < 15; i++ ) b[i] = (UB)( ( b[i] << 1 ) | ( b[i + 1] >> 7 ) );
	b[15] = (UB)( b[15] << 1 );
	if ( top ) b[15] ^= 0x87;
}

/* AES-CMAC (RFC 4493), all with the most significant byte first */
EXPORT void knl_bts_cmac( CONST UB *k, CONST UB *m, INT n, UB *out )
{
	UB	sub[16], x[16], y[16];
	INT	i, at = 0;

	knl_memset(x, 0, 16);
	aes_msb(k, x, sub);
	cmac_double(sub);
	while ( n - at > 16 ) {
		for ( i = 0; i < 16; i++ ) y[i] = x[i] ^ m[at + i];
		aes_msb(k, y, x);
		at += 16;
	}
	if ( n - at < 16 ) cmac_double(sub);
	for ( i = 0; i < 16; i++ ) {
		UB	v = ( at + i < n ) ? m[at + i] : (UB)( ( at + i == n ) ? 0x80 : 0 );

		y[i] = x[i] ^ v ^ sub[i];
	}
	aes_msb(k, y, out);
}

/* The confirm value of the older pairing */
EXPORT void knl_bts_c1( CONST UB *k, CONST UB *r, CONST UB *preq, CONST UB *pres,
			UB iat, CONST UB *ia, UB rat, CONST UB *ra, UB *out )
{
	UB	p[16], t[16];
	INT	i;

	p[0] = iat;
	p[1] = rat;
	knl_memcpy(p + 2, preq, 7);
	knl_memcpy(p + 9, pres, 7);
	for ( i = 0; i < 16; i++ ) t[i] = r[i] ^ p[i];
	knl_bts_e(k, t, t);
	knl_memcpy(p, ra, 6);
	knl_memcpy(p + 6, ia, 6);
	knl_memset(p + 12, 0, 4);
	for ( i = 0; i < 16; i++ ) t[i] ^= p[i];
	knl_bts_e(k, t, out);
}

/* The key of the older pairing: the low halves of r1 and r2, r1 above */
EXPORT void knl_bts_s1( CONST UB *k, CONST UB *r1, CONST UB *r2, UB *out )
{
	UB	t[16];

	knl_memcpy(t, r2, 8);
	knl_memcpy(t + 8, r1, 8);
	knl_bts_e(k, t, out);
}

/* The hash that tells whose a resolvable private address is */
EXPORT void knl_bts_ah( CONST UB *irk, CONST UB *r, UB *out )
{
	UB	t[16];

	knl_memcpy(t, r, 3);
	knl_memset(t + 3, 0, 13);
	knl_bts_e(irk, t, t);
	knl_memcpy(out, t, 3);
}

EXPORT void knl_bts_f4( CONST UB *u, CONST UB *v, CONST UB *x, UB z, UB *out )
{
	UB	m[65], km[16], om[16];

	turned(m, u, 32);
	turned(m + 32, v, 32);
	m[64] = z;
	turned(km, x, 16);
	knl_bts_cmac(km, m, 65, om);
	turned(out, om, 16);
}

EXPORT void knl_bts_f5( CONST UB *w, CONST UB *n1, CONST UB *n2, CONST UB *a1, CONST UB *a2, UB *mackey, UB *ltk )
{
	static CONST UB	salt[16] = {
		0x6c, 0x88, 0x83, 0x91, 0xaa, 0xf5, 0xa5, 0x38,
		0x60, 0x37, 0x0b, 0xdb, 0x5a, 0x60, 0x83, 0xbe
	};
	UB	t[16], wm[32], m[53], om[16];

	turned(wm, w, 32);
	knl_bts_cmac(salt, wm, 32, t);
	m[1] = 'b';
	m[2] = 't';
	m[3] = 'l';
	m[4] = 'e';
	turned(m + 5, n1, 16);
	turned(m + 21, n2, 16);
	turned(m + 37, a1, 7);
	turned(m + 44, a2, 7);
	m[51] = 0x01;			/* the length, 256 bits */
	m[52] = 0x00;
	m[0] = 0;
	knl_bts_cmac(t, m, 53, om);
	turned(mackey, om, 16);
	m[0] = 1;
	knl_bts_cmac(t, m, 53, om);
	turned(ltk, om, 16);
}

EXPORT void knl_bts_f6( CONST UB *w, CONST UB *n1, CONST UB *n2, CONST UB *r,
			CONST UB *iocap, CONST UB *a1, CONST UB *a2, UB *out )
{
	UB	m[65], km[16], om[16];

	turned(m, n1, 16);
	turned(m + 16, n2, 16);
	turned(m + 32, r, 16);
	turned(m + 48, iocap, 3);
	turned(m + 51, a1, 7);
	turned(m + 58, a2, 7);
	turned(km, w, 16);
	knl_bts_cmac(km, m, 65, om);
	turned(out, om, 16);
}

EXPORT UW knl_bts_g2( CONST UB *u, CONST UB *v, CONST UB *x, CONST UB *y )
{
	UB	m[80], km[16], om[16];

	turned(m, u, 32);
	turned(m + 32, v, 32);
	turned(m + 64, y, 16);
	turned(km, x, 16);
	knl_bts_cmac(km, m, 80, om);
	return ( (UW)om[12] << 24 ) | ( (UW)om[13] << 16 ) | ( (UW)om[14] << 8 ) | om[15];
}

/* ---------------------------------------------------------------- P-256 */

LOCAL int rng( void *ctx, unsigned char *b, size_t n )
{
	(void)ctx;
	return ( ts_get_random(b, (SZ)n) >= E_OK ) ? 0 : -1;
}

/* A point on the air (X and Y of 32 bytes each, the lowest byte first) into mbed's form */
LOCAL INT point_in( mbedtls_ecp_point *q, CONST UB *pub )
{
	UB	m[32];
	INT	er;

	turned(m, pub, 32);
	er = mbedtls_mpi_read_binary(&q->MBEDTLS_PRIVATE(X), m, 32);
	turned(m, pub + 32, 32);
	if ( er == 0 ) er = mbedtls_mpi_read_binary(&q->MBEDTLS_PRIVATE(Y), m, 32);
	if ( er == 0 ) er = mbedtls_mpi_lset(&q->MBEDTLS_PRIVATE(Z), 1);
	return er;
}

LOCAL INT point_out( CONST mbedtls_ecp_point *q, UB *pub )
{
	UB	m[32];
	INT	er;

	er = mbedtls_mpi_write_binary(&q->MBEDTLS_PRIVATE(X), m, 32);
	if ( er == 0 ) turned(pub, m, 32);
	if ( er == 0 ) er = mbedtls_mpi_write_binary(&q->MBEDTLS_PRIVATE(Y), m, 32);
	if ( er == 0 ) turned(pub + 32, m, 32);
	return er;
}

/* The public key of a private one; a private key is made first when make is set */
EXPORT ER knl_bts_p256_key( UB *priv, UB *pub, BOOL make )
{
	mbedtls_ecp_group	g;
	mbedtls_mpi		d;
	mbedtls_ecp_point	q;
	UB			m[32];
	INT			er;

	mbedtls_ecp_group_init(&g);
	mbedtls_mpi_init(&d);
	mbedtls_ecp_point_init(&q);
	er = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
	if ( er == 0 ) {
		if ( make ) {
			er = mbedtls_ecp_gen_privkey(&g, &d, rng, NULL);
			if ( er == 0 ) er = mbedtls_mpi_write_binary(&d, m, 32);
			if ( er == 0 ) turned(priv, m, 32);
		} else {
			turned(m, priv, 32);
			er = mbedtls_mpi_read_binary(&d, m, 32);
		}
	}
	if ( er == 0 ) er = mbedtls_ecp_mul(&g, &q, &d, &g.G, rng, NULL);
	if ( er == 0 ) er = point_out(&q, pub);
	knl_memset(m, 0, sizeof(m));
	mbedtls_ecp_point_free(&q);
	mbedtls_mpi_free(&d);
	mbedtls_ecp_group_free(&g);
	return ( er == 0 ) ? E_OK : E_SYS;
}

/*
 * The shared secret with the other side's public key, which is refused
 * when it is not a point of the curve: a false point would give away the
 * private key bit by bit.
 */
EXPORT ER knl_bts_p256_dh( CONST UB *priv, CONST UB *peer, UB *dh )
{
	mbedtls_ecp_group	g;
	mbedtls_mpi		d;
	mbedtls_ecp_point	q, s;
	UB			m[32];
	INT			er;
	ER			ret = E_SYS;

	mbedtls_ecp_group_init(&g);
	mbedtls_mpi_init(&d);
	mbedtls_ecp_point_init(&q);
	mbedtls_ecp_point_init(&s);
	er = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
	if ( er == 0 ) er = point_in(&q, peer);
	if ( er == 0 && mbedtls_ecp_check_pubkey(&g, &q) != 0 ) {
		ret = E_PAR;
		er = -1;
	}
	if ( er == 0 ) {
		turned(m, priv, 32);
		er = mbedtls_mpi_read_binary(&d, m, 32);
	}
	if ( er == 0 ) er = mbedtls_ecp_mul(&g, &s, &d, &q, rng, NULL);
	if ( er == 0 ) er = mbedtls_mpi_write_binary(&s.MBEDTLS_PRIVATE(X), m, 32);
	if ( er == 0 ) {
		turned(dh, m, 32);
		ret = E_OK;
	}
	knl_memset(m, 0, sizeof(m));
	mbedtls_ecp_point_free(&s);
	mbedtls_ecp_point_free(&q);
	mbedtls_mpi_free(&d);
	mbedtls_ecp_group_free(&g);
	return ret;
}
