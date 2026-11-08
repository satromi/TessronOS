/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rng.c
 *	Random source and UUID version 7 (design 11.4).
 *
 *	The generator is xoshiro256**, seeded at start-up from the timer
 *	counter and the device tree so that two boots of the same image do
 *	not produce the same stream. Where the machine has a hardware
 *	generator its words are mixed in as well.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/uuid.h>
#include "sysdepend/sysdepend.h"

LOCAL UD	rng_s[4];
LOCAL BOOL	rng_ready = FALSE;
LOCAL ID	rng_mtxid = 0;

/* the millisecond and counter that keep UUIDs of one millisecond in order */
LOCAL UD	uuid_last_ms = 0;
LOCAL UW	uuid_seq = 0;

LOCAL UD rotl( UD x, INT k )
{
	return (x << k) | (x >> (64 - k));
}

LOCAL UD splitmix64( UD *x )
{
	UD	z = (*x += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

	return z ^ (z >> 31);
}

LOCAL UD next_u64( void )
{
	UD	r = rotl(rng_s[1] * 5, 7) * 9;
	UD	t = rng_s[1] << 17;

	rng_s[2] ^= rng_s[0];
	rng_s[3] ^= rng_s[1];
	rng_s[1] ^= rng_s[2];
	rng_s[0] ^= rng_s[3];
	rng_s[2] ^= t;
	rng_s[3] = rotl(rng_s[3], 45);

	return r;
}

/*
 * Seed from the timer counter, the device tree and, when the machine has
 * one, the hardware generator.
 */
EXPORT void knl_rng_init( void )
{
	T_CMTX	cmtx;
	UD	seed;
	UW	hw;
	INT	i;

	if ( rng_ready ) {
		return;
	}
	seed = ReadSysCounter();
	seed ^= (UD)knl_dtb_addr * 0x2545F4914F6CDD1DULL;
	seed ^= (UD)knl_dtb_size << 32;

	if ( knl_dtb_addr != 0 ) {
		CONST UB *p = (CONST UB *)PA2VA(knl_dtb_addr);
		UD	n = ( knl_dtb_size < 4096 ) ? knl_dtb_size : 4096;

		for ( i = 0; i < (INT)n; i++ ) {
			seed = seed * 0x100000001B3ULL ^ p[i];	/* FNV style mix */
		}
	}
	for ( i = 0; i < 4; i++ ) {
		rng_s[i] = splitmix64(&seed);
	}
	for ( i = 0; i < 8; i++ ) {
		if ( knl_hwrng_read(&hw) != E_OK ) break;
		rng_s[i % 4] ^= (UD)hw << ((i / 4) * 32);
	}

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	rng_mtxid = tk_cre_mtx(&cmtx);
	rng_ready = TRUE;

	(void)next_u64();		/* discard the first word */
}

EXPORT ER ts_get_random( void *buf, SZ len )
{
	UB	*p = (UB *)buf;
	SZ	i;
	UD	v = 0;
	UW	hw;

	if ( buf == NULL ) {
		return E_PAR;
	}
	if ( !rng_ready ) {
		knl_rng_init();
	}
	if ( rng_mtxid > 0 ) tk_loc_mtx(rng_mtxid, TMO_FEVR);

	for ( i = 0; i < len; i++ ) {
		if ( (i % 8) == 0 ) {
			v = next_u64();
			if ( knl_hwrng_read(&hw) == E_OK ) {
				v ^= (UD)hw * 0x9E3779B97F4A7C15ULL;
			}
		}
		p[i] = (UB)(v >> ((i % 8) * 8));
	}

	if ( rng_mtxid > 0 ) tk_unl_mtx(rng_mtxid);

	return E_OK;
}

/*
 * Bytes stirred into the state (the random source object written):
 * each eight folded into a word of it, then the generator stepped so
 * that what was written is spread over all four words.
 */
EXPORT void knl_rng_stir( CONST void *buf, SZ len )
{
	CONST UB *p = (CONST UB *)buf;
	UD	w = 0;
	SZ	i;

	if ( buf == NULL || len <= 0 ) {
		return;
	}
	if ( !rng_ready ) {
		knl_rng_init();
	}
	if ( rng_mtxid > 0 ) tk_loc_mtx(rng_mtxid, TMO_FEVR);
	for ( i = 0; i < len; i++ ) {
		w = ( w << 8 ) | p[i];
		if ( ( i % 8 ) == 7 || i == len - 1 ) {
			rng_s[( i / 8 ) % 4] ^= splitmix64(&w);
			(void)next_u64();
			w = 0;
		}
	}
	if ( ( rng_s[0] | rng_s[1] | rng_s[2] | rng_s[3] ) == 0 ) {
		rng_s[0] = 1;			/* the one state the generator never leaves */
	}
	if ( rng_mtxid > 0 ) tk_unl_mtx(rng_mtxid);
}

/* ---------------------------------------------------------------- UUID v7 */

EXPORT ER ts_gen_uuid( TS_UUID *p_uuid )
{
	SYSTIM	tim;
	UD	ms;
	UB	rnd[10];
	UW	seq;
	ER	er;

	if ( p_uuid == NULL ) {
		return E_PAR;
	}
	er = tk_get_tim(&tim);
	if ( er < E_OK ) {
		return er;
	}
	/* TRON time (ms since 1985-01-01) to Unix time (ms since 1970-01-01) */
	ms = (((UD)(UW)tim.hi << 32) | tim.lo) + 473385600000ULL;
	if ( ms < 1000000000000ULL ) {
		/*
		 * The clock has not been set (a board without a battery for
		 * its clock, before SNTP): a UUID that says no time, random in
		 * all its 122 bits (version 4), rather than one dated 1985 that
		 * would sort before everything made since.
		 */
		er = ts_get_random(p_uuid->b, sizeof(p_uuid->b));
		if ( er < E_OK ) {
			return er;
		}
		p_uuid->b[6] = (UB)( 0x40 | ( p_uuid->b[6] & 0x0F ) );	/* version 4 */
		p_uuid->b[8] = (UB)( 0x80 | ( p_uuid->b[8] & 0x3F ) );	/* variant 10 */
		return E_OK;
	}

	er = ts_get_random(rnd, sizeof(rnd));
	if ( er < E_OK ) {
		return er;
	}

	if ( rng_mtxid > 0 ) tk_loc_mtx(rng_mtxid, TMO_FEVR);
	if ( ms == uuid_last_ms ) {
		uuid_seq++;
		if ( uuid_seq > 0x0FFF ) {
			ms++;			/* the counter is full: move on */
			uuid_last_ms = ms;
			uuid_seq = 0;
		}
	} else {
		uuid_last_ms = ms;
		uuid_seq = ((UW)rnd[0] << 8 | rnd[1]) & 0x0FFF;
	}
	seq = uuid_seq;
	if ( rng_mtxid > 0 ) tk_unl_mtx(rng_mtxid);

	p_uuid->b[0] = (UB)(ms >> 40);
	p_uuid->b[1] = (UB)(ms >> 32);
	p_uuid->b[2] = (UB)(ms >> 24);
	p_uuid->b[3] = (UB)(ms >> 16);
	p_uuid->b[4] = (UB)(ms >> 8);
	p_uuid->b[5] = (UB)ms;
	p_uuid->b[6] = (UB)(0x70 | ((seq >> 8) & 0x0F));	/* version 7 */
	p_uuid->b[7] = (UB)seq;
	p_uuid->b[8] = (UB)(0x80 | (rnd[2] & 0x3F));		/* variant 10 */
	p_uuid->b[9]  = rnd[3];
	p_uuid->b[10] = rnd[4];
	p_uuid->b[11] = rnd[5];
	p_uuid->b[12] = rnd[6];
	p_uuid->b[13] = rnd[7];
	p_uuid->b[14] = rnd[8];
	p_uuid->b[15] = rnd[9];

	return E_OK;
}

/* A UUID as text and back, and two compared (ts_uuid_to_str and the rest), are in uuidtext.c */
