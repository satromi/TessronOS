/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	modtest.c
 *	wpa_supplicant's own module tests, run on the host with the kernel's
 *	sources and cryptography (tools/wpa/build.sh modtest)
 */

#include "utils/includes.h"
#include "utils/common.h"

int common_module_tests(void);
int crypto_module_tests(void);

/* Mbed TLS's source of randomness, as the kernel's glue gives it */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	os_get_random(output, len);
	*olen = len;
	return 0;
}

int main(void)
{
	int c, k;

	wpa_debug_level = MSG_INFO;
	c = common_module_tests();
	k = crypto_module_tests();
	printf("modtest: common module tests %s, crypto module tests %s\n",
	       c == 0 ? "passed" : "FAILED", k == 0 ? "passed" : "FAILED");
	return ( c == 0 && k == 0 ) ? 0 : 1;
}
