/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crc32c.h
 *	CRC32C (Castagnoli) for file system metadata (design 11.6)
 */

#ifndef __TS_CRC32C_H__
#define __TS_CRC32C_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CRC32C of len bytes at p: the Castagnoli polynomial in its reflected
 * form 0x82F63B78, starting from all ones and finishing with an xor of
 * all ones. The string "123456789" gives 0xE3069283.
 *
 * On AArch64 this is the crc32c instruction group, which armv8.2-a has.
 * Building with TS_CRC32C_SW selects a bit at a time loop instead, for a
 * machine that lacks the instructions; both give the same value.
 */
IMPORT UW ts_crc32c( CONST UB *p, SZ len );

#ifdef __cplusplus
}
#endif
#endif /* __TS_CRC32C_H__ */
