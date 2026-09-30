/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crc32c.c
 *	CRC32C (Castagnoli) for file system metadata (design 11.6).
 *
 *	Every metadata block of a native volume carries this value: the
 *	superblock, the nodes of the object index, and the journal records
 *	together with each block a record covers.
 *
 *	The polynomial is 0x1EDC6F41, written here in the reflected form
 *	0x82F63B78 because the computation runs from the low bit upwards.
 *	The value starts at all ones and is inverted at the end, so leading
 *	zero bytes change the result and a run of zeroes at the end does
 *	not leave the value at zero.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>

#define CRC32C_POLY	0x82F63B78U	/* Castagnoli, reflected */
#define CRC32C_INIT	0xFFFFFFFFU

#if defined(__aarch64__) && !defined(TS_CRC32C_SW)

/*
 * The crc32c instructions take the CRC so far and one operand, and give
 * back the CRC of the operand's bytes appended to it. The operand is
 * consumed from its least significant byte, so a little endian load of
 * eight bytes feeds them in the order they sit in memory.
 */

LOCAL UW crc32c_u64( UW c, UD v )
{
	__asm__ ( "crc32cx %w0, %w1, %x2" : "=r"(c) : "r"(c), "r"(v) );
	return c;
}

LOCAL UW crc32c_u32( UW c, UW v )
{
	__asm__ ( "crc32cw %w0, %w1, %w2" : "=r"(c) : "r"(c), "r"(v) );
	return c;
}

LOCAL UW crc32c_u16( UW c, UH v )
{
	__asm__ ( "crc32ch %w0, %w1, %w2" : "=r"(c) : "r"(c), "r"(v) );
	return c;
}

LOCAL UW crc32c_u8( UW c, UB v )
{
	__asm__ ( "crc32cb %w0, %w1, %w2" : "=r"(c) : "r"(c), "r"(v) );
	return c;
}

EXPORT UW ts_crc32c( CONST UB *p, SZ len )
{
	UW	c = CRC32C_INIT;

	/*
	 * The kernel is built with -mstrict-align, so the wide loads are
	 * only taken once the pointer is aligned for them. Metadata
	 * blocks are aligned already and skip this loop.
	 */
	while ( len > 0 && ((UD)p & 7U) != 0 ) {
		c = crc32c_u8(c, *p++);
		len--;
	}

	while ( len >= 8 ) {
		c = crc32c_u64(c, *(CONST UD *)(CONST void *)p);
		p += 8;
		len -= 8;
	}
	if ( (len & 4) != 0 ) {
		c = crc32c_u32(c, *(CONST UW *)(CONST void *)p);
		p += 4;
	}
	if ( (len & 2) != 0 ) {
		c = crc32c_u16(c, *(CONST UH *)(CONST void *)p);
		p += 2;
	}
	if ( (len & 1) != 0 ) {
		c = crc32c_u8(c, *p);
	}

	return c ^ CRC32C_INIT;
}

#else /* a machine without the instructions */

EXPORT UW ts_crc32c( CONST UB *p, SZ len )
{
	UW	c = CRC32C_INIT;
	SZ	i;
	INT	k;

	for ( i = 0; i < len; i++ ) {
		c ^= p[i];
		for ( k = 0; k < 8; k++ ) {
			c = ( (c & 1) != 0 ) ? ((c >> 1) ^ CRC32C_POLY) : (c >> 1);
		}
	}

	return c ^ CRC32C_INIT;
}

#endif
