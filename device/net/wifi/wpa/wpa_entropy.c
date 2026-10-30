/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpa_entropy.c (x64 PC, Raspberry Pi 5)
 *	The source of randomness of the cryptography under wpa_supplicant's
 *	crypto.h (Mbed TLS, MBEDTLS_ENTROPY_HARDWARE_ALT): the Wi-Fi
 *	manager's random bytes (knl_wifi_random). Only this file knows the
 *	cryptography's own interface, so that another one under crypto.h
 *	replaces this file and crypto_mbedtls.c and nothing else.
 */

#include <stddef.h>
#include "wpa_glue.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	knl_wifi_random(output, (int)len);
	*olen = len;
	return 0;
}
