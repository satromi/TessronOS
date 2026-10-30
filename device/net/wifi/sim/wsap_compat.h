/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wsap_compat.h (the Wi-Fi simulator, WIFI_SIM=1)
 *	What hostapd's headers ask of the C library besides what the
 *	station's sources ask (device/net/wifi/wpa/include): the group
 *	and user identities of its control interface's settings, and the
 *	byte order calls of its RADIUS header. Included first into each
 *	source of the access point's object (tools/wpa/build.sh ap); the
 *	machines are little endian.
 */

#ifndef WSAP_COMPAT_H
#define WSAP_COMPAT_H

#include <stdint.h>

typedef unsigned int	gid_t;
typedef unsigned int	uid_t;

static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }

#endif /* WSAP_COMPAT_H */
