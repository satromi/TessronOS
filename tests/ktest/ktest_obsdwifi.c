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
IMPORT int rumpcomp_tswifi_fw_open( CONST char *name, void **data, UD *size );
IMPORT void rumpcomp_tswifi_fw_close( void *data, UD size );

LOCAL void test_layer( void )
{
	KT_ASSERT_EQ(rump_tswifi_selftest(), 0);
}

/*
 * Each file the drivers ask for by name read from its object on the system
 * volume, as loadfirmware does; a name the volume has not is refused
 */
LOCAL void test_firmware( void )
{
	static CONST char *CONST name[] = {
#ifndef X64
		"brcmfmac43455-sdio.bin", "brcmfmac43455-sdio.clm_blob"
#else
		"iwm-8265-36", "iwx-cc-a0-77", "iwx-Qu-b0-hr-b0-77", "iwx-Qu-c0-hr-b0-77", "iwx-QuZ-a0-hr-b0-77",
		"iwx-ty-a0-gf-a0-77", "iwx-ty-a0-gf-a0.pnvm", "iwx-so-a0-gf-a0-77", "iwx-so-a0-gf-a0.pnvm",
		"iwx-so-a0-gf4-a0-77", "iwx-so-a0-gf4-a0.pnvm", "iwx-so-a0-hr-b0-77", "iwx-so-a0-jf-b0-77",
		"iwx-Qu-b0-jf-b0-77", "iwx-Qu-c0-jf-b0-77", "iwx-QuZ-a0-jf-b0-77",
		"iwm-3160-17", "iwm-3168-29", "iwm-7260-17", "iwm-7265-17", "iwm-7265D-29",
		"iwm-8000C-36", "iwm-9000-46", "iwm-9260-46",
		"mwx-mt7961_patch_mcu_1_2_hdr", "mwx-mt7961_ram_code_1", "mwx-mt7961_patch_mcu_1a_2_hdr",
		"mwx-mt7961_ram_code_1a", "mwx-mt7922_patch_mcu_1_1_hdr", "mwx-mt7922_ram_code_1",
		"athx-qca6174-firmware-6", "athx-qca6174-board-2"
#endif
	};
	void	*d = NULL;
	UD	sz = 0;
	INT	i;

	for ( i = 0; i < (INT)( sizeof(name) / sizeof(name[0]) ); i++ ) {
		KT_ASSERT_EQ(rumpcomp_tswifi_fw_open(name[i], &d, &sz), 0);
		KT_ASSERT(sz > 1024);
		rumpcomp_tswifi_fw_close(d, sz);
	}
	KT_ASSERT(rumpcomp_tswifi_fw_open("iwx-no-such-77", &d, &sz) != 0);
}

EXPORT void ktest_obsdwifi( void )
{
	KT_RUN(test_layer);
	KT_RUN(test_firmware);
}
