/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_obsdwifi.c
 *	What the layer of OpenBSD's IEEE 802.11 code and iwm stands on
 *	(make WIFI_NET80211=obsd, device/net/wifi/obsd, design 9.6.9), tried
 *	on a machine with no card: the layer's own test
 *	(rump_tswifi_selftest, obsd_test.c) runs its lock, sleeping and
 *	waking, timeouts, task queues, read/write locks, mbufs and the DMA
 *	of their clusters, the transmit queue, the formats of its printf and
 *	the ciphers of net80211, and says each failure on the console.
 */

#include "kernel.h"
#include "ktest.h"

IMPORT int rump_tswifi_selftest( void );

LOCAL void test_layer( void )
{
	KT_ASSERT_EQ(rump_tswifi_selftest(), 0);
}

EXPORT void ktest_obsdwifi( void )
{
	KT_RUN(test_layer);
}
