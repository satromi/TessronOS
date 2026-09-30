/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uuid.h
 *	UUID version 7 and the random source behind it (design 11.4)
 */

#ifndef __TS_UUID_H__
#define __TS_UUID_H__

#ifdef __cplusplus
extern "C" {
#endif

#define TS_UUID_STRLEN	36		/* 8-4-4-4-12, without the NUL */

typedef union {
	UB	b[16];
	struct {
		UD	hi;
		UD	lo;
	} d;
} TS_UUID;

/*
 * A new version 7 UUID: the first 48 bits are the Unix time in
 * milliseconds, so plain byte order comparison is creation order. Within
 * one millisecond a counter in rand_a keeps that order (RFC 9562 method
 * 1). Fails with E_OBJ while the system clock has not been set.
 */
IMPORT ER ts_gen_uuid( TS_UUID *p_uuid );

/* 019a1132-762b-7b02-ba2a-a918a9b37c39, lower case */
IMPORT ER ts_uuid_to_str( CONST TS_UUID *uuid, char *buf, SZ bufsz );
IMPORT ER ts_str_to_uuid( CONST char *str, TS_UUID *p_uuid );

/* -1, 0 or 1: unsigned lexicographic order of the 16 bytes */
IMPORT INT ts_uuid_cmp( CONST TS_UUID *a, CONST TS_UUID *b );

/*
 * Random bytes. Taken from the hardware generator where the machine has
 * one, otherwise from a generator seeded at start-up with the timer
 * counter and the device tree.
 */
IMPORT ER ts_get_random( void *buf, SZ len );

/* Target hook: one 32 bit word from the hardware generator (E_NOSPT: none) */
IMPORT ER knl_hwrng_read( UW *p_val );
IMPORT void knl_rng_init( void );
IMPORT void knl_rng_stir( CONST void *buf, SZ len );	/* bytes mixed into the generator */

#ifdef __cplusplus
}
#endif

#endif /* __TS_UUID_H__ */
