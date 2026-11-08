/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_crypto.h
 *	The functions of LE pairing (bt_crypto.c). Their values go the lowest
 *	byte first, as on the air, except knl_bts_cmac, which is the plain
 *	AES-CMAC.
 */

#ifndef __BT_CRYPTO_H__
#define __BT_CRYPTO_H__

IMPORT void knl_bts_e( CONST UB *k, CONST UB *in, UB *out );
IMPORT void knl_bts_cmac( CONST UB *k, CONST UB *m, INT n, UB *out );
IMPORT void knl_bts_c1( CONST UB *k, CONST UB *r, CONST UB *preq, CONST UB *pres,
			UB iat, CONST UB *ia, UB rat, CONST UB *ra, UB *out );
IMPORT void knl_bts_s1( CONST UB *k, CONST UB *r1, CONST UB *r2, UB *out );
IMPORT void knl_bts_ah( CONST UB *irk, CONST UB *r, UB *out );
IMPORT void knl_bts_f4( CONST UB *u, CONST UB *v, CONST UB *x, UB z, UB *out );
IMPORT void knl_bts_f5( CONST UB *w, CONST UB *n1, CONST UB *n2, CONST UB *a1, CONST UB *a2, UB *mackey, UB *ltk );
IMPORT void knl_bts_f6( CONST UB *w, CONST UB *n1, CONST UB *n2, CONST UB *r,
			CONST UB *iocap, CONST UB *a1, CONST UB *a2, UB *out );
IMPORT UW   knl_bts_g2( CONST UB *u, CONST UB *v, CONST UB *x, CONST UB *y );
IMPORT ER   knl_bts_p256_key( UB *priv, UB *pub, BOOL make );
IMPORT ER   knl_bts_p256_dh( CONST UB *priv, CONST UB *peer, UB *dh );

#endif /* __BT_CRYPTO_H__ */
