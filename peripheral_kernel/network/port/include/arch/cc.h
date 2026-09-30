/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cc.h
 *	What lwIP needs to know about this compiler and machine
 *	(design 12.6).
 *
 *	AArch64 with gcc: the fixed width types come from the compiler's own
 *	stdint.h, and everything is little endian. There is no C library, so
 *	the diagnostic and assertion macros go to T-Monitor instead of
 *	printf, and the printf length modifiers are given here rather than
 *	taken from inttypes.h.
 */

#ifndef __TS_LWIP_ARCH_CC_H__
#define __TS_LWIP_ARCH_CC_H__

#include <stdint.h>
#include <stddef.h>

#define BYTE_ORDER		LITTLE_ENDIAN

/* The length modifiers lwIP would otherwise read out of inttypes.h */
#define X8_F			"02x"
#define U16_F			"u"
#define S16_F			"d"
#define X16_F			"x"
#define U32_F			"u"
#define S32_F			"d"
#define X32_F			"x"
#define SZT_F			"lu"

/* Structures that go on the wire are packed and never padded */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT	__attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x)	x

/* Both go to the console, which is the only thing there is this early */
struct ts_lwip_diag;			/* no type is needed; see the macros */
extern int tm_printf( const unsigned char *format, ... );

#define LWIP_PLATFORM_DIAG(x)	do { tm_printf x; } while (0)

#define LWIP_PLATFORM_ASSERT(x) \
	do { \
		tm_printf((const unsigned char *) \
			  "lwip: assertion \"%s\" failed at %s:%d\n", \
			  (x), __FILE__, __LINE__); \
		for (;;) { \
			; \
		} \
	} while (0)

#endif /* __TS_LWIP_ARCH_CC_H__ */
