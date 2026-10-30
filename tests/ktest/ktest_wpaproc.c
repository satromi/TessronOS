/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wpaproc.c
 *	wpa_supplicant's process (design 9.6.11, WIFI=1 WPA_MODE=proc): the
 *	calls of knl_wpa_* answered by the process wpasupp through the
 *	object "wpasup" -- the PMK of IEEE 802.11's test vector, the kinds
 *	of networks from their elements, H2E's password element -- and the
 *	object refusing anyone but that process, the kernel's own tasks
 *	included.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/ob.h>
#include <ts/uuid.h>
#include "tstdlib.h"

IMPORT int knl_wpa_psk( const char *pass, int pass_len, const unsigned char *ssid,
			int ssid_len, unsigned char *pmk );
IMPORT int knl_wpa_classify( const unsigned char *ies, int ies_len, int privacy );
IMPORT int knl_wpa_sae_pt( const unsigned char *ssid, int ssid_len, const char *pass, int pass_len,
			   unsigned char *pt, int *pt_len );
IMPORT int knl_wpa_state( void );

/* IEEE Std 802.11-2020 J.4.2: "password", "IEEE" */
LOCAL CONST UB	pmk_ieee[32] = {
	0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
	0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
};

LOCAL BOOL same( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL void test_psk( void )
{
	UB	pmk[32];

	knl_memset(pmk, 0, sizeof(pmk));
	KT_ASSERT_EQ(knl_wpa_psk("password", 8, (CONST UB *)"IEEE", 4, pmk), 0);
	KT_ASSERT(same(pmk, pmk_ieee, sizeof(pmk)));
	KT_ASSERT(knl_wpa_psk("short", 5, (CONST UB *)"IEEE", 4, pmk) != 0);	/* refused */
}

LOCAL void test_classify( void )
{
	/* RSN: CCMP, CCMP, PSK; then PSK and SAE; then SAE alone */
	CONST UB psk[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
			   1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
	CONST UB both[] = { 48, 24, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
			    2, 0, 0x00, 0x0f, 0xac, 2, 0x00, 0x0f, 0xac, 8, 0xc0, 0 };
	CONST UB sae[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
			   1, 0, 0x00, 0x0f, 0xac, 8, 0xc0, 0 };

	KT_ASSERT_EQ(knl_wpa_classify(psk, sizeof(psk), 1), 3);		/* WPAG_WPA2 */
	KT_ASSERT_EQ(knl_wpa_classify(both, sizeof(both), 1), 6);	/* WPAG_WPA2WPA3 */
	KT_ASSERT_EQ(knl_wpa_classify(sae, sizeof(sae), 1), 4);		/* WPAG_WPA3 */
	KT_ASSERT_EQ(knl_wpa_classify(NULL, 0, 0), 0);			/* WPAG_OPEN */
	KT_ASSERT_EQ(knl_wpa_state(), 0);				/* WPAG_NONE */
}

LOCAL void test_pt( void )
{
	UB	pt[132];
	INT	n = 0;

	KT_ASSERT_EQ(knl_wpa_sae_pt((CONST UB *)"KtestWpa3", 9, "dragonfly key 3", 15, pt, &n), 0);
	KT_ASSERT_EQ(n, 64);					/* group 19: x and y */
}

/* Only the process the kernel started reaches record 1: a kernel task is refused */
LOCAL void test_refused( void )
{
	TS_UUID	u;
	ID	key;
	UB	buf[64];
	SZ	asz = 0;

	KT_ASSERT_ER(knl_obdev_uuid((CONST UB *)"wpasup", &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_rea_rec(key, 1, 0, buf, sizeof(buf), &asz), E_OACV);
		KT_ASSERT_ER(ob_wri_rec(key, 1, 0, buf, sizeof(buf), &asz), E_OACV);
		(void)ob_cls_obj(key);
	}
}

EXPORT void ktest_wpaproc( void )
{
	/* the first call starts the process, and waits for it */
	KT_RUN(test_psk);
	KT_RUN(test_classify);
	KT_RUN(test_pt);
	KT_RUN(test_refused);
}
