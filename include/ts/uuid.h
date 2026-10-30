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
 * The same text as ts_uuid_to_str, into out of TS_UUID_STRLEN + 1 bytes,
 * with nothing of the system's: for a program writing a UUID into the
 * attributes of what it makes ("shows" of a window)
 */
static __inline__ void ts_uuid_text( CONST TS_UUID *u, char *out )
{
	static CONST char	hex[] = "0123456789abcdef";
	INT			i, k = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[k++] = '-';
		out[k++] = hex[u->b[i] >> 4];
		out[k++] = hex[u->b[i] & 15];
	}
	out[k] = 0;
}

/*
 * The id of a link (<link id="...">, design 8.3): where the object it
 * points at is, from the outside in, each part a UUID and a slash after
 * it, then the object's UUID and the record ("_0.xtad"):
 *
 *	object_0.xtad			an object of the volume the link is on
 *	volume/object_0.xtad		an object of another volume
 *	machine/volume/object_0.xtad	an object of another machine
 *
 * ts_lnk_split answers where in s (len bytes) the object's UUID starts,
 * with where the volume's and the machine's are (-1 for none).
 */
#define TS_LNK_PART	( TS_UUID_STRLEN + 1 )	/* a UUID and its slash */

static __inline__ INT ts_lnk_split( CONST UB *s, INT len, INT *p_vol, INT *p_mach )
{
	INT	n = 0;

	while ( n < 2 && len >= ( n + 1 ) * TS_LNK_PART + TS_UUID_STRLEN
		&& s[( n + 1 ) * TS_LNK_PART - 1] == '/' ) {
		n++;
	}
	if ( p_vol != NULL ) *p_vol = ( n >= 1 ) ? ( n - 1 ) * TS_LNK_PART : -1;
	if ( p_mach != NULL ) *p_mach = ( n >= 2 ) ? 0 : -1;
	return n * TS_LNK_PART;
}

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
