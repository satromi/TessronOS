/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wifikey.c
 *	The Wi-Fi's saved networks and their keys (device/net/wifi/wifi.c,
 *	design 9.6.6), as a program asks for them through wlan0 (SAVE,
 *	FORGET) and as a join uses them. Built with the Wi-Fi (WIFI=1); a
 *	machine with no card has no manager, and the requests are made here
 *	as the manager makes them.
 *
 *	- protect: the settings object is the system's and the
 *	  administrators': 利用者 and the person logged in cannot read it.
 *	- save: a network a user saves with a passphrase, with no
 *	  administrator's rights: the settings keep its key, the PMK of the
 *	  passphrase and the SSID, with the user, and not the passphrase;
 *	  record 1 as that user's process reads it shows the network, not
 *	  the key.
 *	- keep: saved again with no passphrase, as the box left with its
 *	  stand-in marks saves it, the key is the same, and it is the key a
 *	  join uses; a new passphrase replaces it. A network with no key
 *	  cannot be saved with none.
 *	- others: another user does not see the network in record 1, has no
 *	  key to join it with, cannot forget it, and saves the same SSID as
 *	  a network of their own, with a key of their own.
 *	- switch: only the networks of the person logged in are joined by
 *	  themselves; when another person logs in, the one before's are not
 *	  (the network joined is left) and the new one's are.
 *	- forget: a network forgotten is gone, and the others stay.
 *	- old: settings written before keys were kept ($$WIFIPASS), and
 *	  settings of networks with no user or "all" (and an SSID twice), are
 *	  read as the networks of the person logged in, made into keys, and
 *	  written again.
 *	- wpa3: a network saved as WPA3-Personal keeps its password (SAE
 *	  needs it), as hexadecimal digits in the settings, and no PMK; what
 *	  a join is given is the password and H2E's element made from it;
 *	  record 1 shows neither; another user has none of it; a password
 *	  that is no WPA3 one is refused and the kept one stays. WPA2/WPA3
 *	  keeps both the PMK and the password; saved again as WPA2 it keeps
 *	  the PMK and lets the password go, and so cannot be saved again as
 *	  WPA3 without one.
 *
 *	The settings as they were are put back.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "obj/obj.h"
#include "tstdlib.h"

IMPORT ER   knl_wifi_request( CONST T_OBCRD *crd, CONST char *line );
IMPORT ER   knl_wifi_key_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len, UB *pmk );
IMPORT INT  knl_wifi_state_text( CONST T_OBCRD *crd, UB **p_text );
IMPORT BOOL knl_wifi_auto_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len );
IMPORT int knl_wpa_psk( const char *pass, int pass_len, const unsigned char *ssid,
			int ssid_len, unsigned char *pmk );
IMPORT INT  knl_wifi_secret_for( CONST T_OBCRD *crd, CONST UB *ssid, INT len, UB *pmk, BOOL *have_pmk,
				 UB *pt, INT *pt_len, char *pw, INT *pw_len );
IMPORT int knl_wpa_sae_pt( const unsigned char *ssid, int ssid_len, const char *pass, int pass_len,
			   unsigned char *pt, int *pt_len );

#define SAVED_WPA2	1		/* WPAG_SAVED_* (wpa_glue.h) */
#define SAVED_WPA3	2
#define SAVED_WPA2WPA3	3
#define SSID_C		"KtestWpa3"
#define HEX_C		"4b7465737457706133"
#define PASS_C		"dragonfly key 3"
#define PASS_C_HEX	"647261676f6e666c79206b65792033"

#define SSID_A		"KtestHome"
#define HEX_A		"4b74657374486f6d65"
#define PASS_A		"correct horse battery"
#define PASS_A2		"staple 12345"
#define SSID_B		"KtestShared"
#define HEX_B		"4b74657374536861726564"
#define PASS_B		"every one of us"

LOCAL TS_UUID	conf;
LOCAL UB	*was = NULL;		/* the settings as they were */
LOCAL INT	was_len = 0;
LOCAL T_OBCRD	*me, *other;		/* 利用者, and another user */

/* The settings' record 1, read whole as the system; its length, -1 none (Kfree *p) */
LOCAL INT conf_get( UB **p )
{
	ID	key = ob_opn_obj(&conf, OB_OP_READ);
	INT	len;

	*p = NULL;
	if ( key <= 0 ) return -1;
	len = knl_ob_rea_all(key, 1, 0, p, NULL);
	ob_cls_obj(key);
	return len;
}

LOCAL ER conf_put( CONST UB *t, INT len )
{
	ID	key = ob_opn_obj(&conf, OB_OP_READ | OB_OP_WRITE);
	SZ	asz = 0;
	ER	er;

	if ( key <= 0 ) return ( key < E_OK ) ? (ER)key : E_SYS;
	er = ob_wri_rec(key, 1, 0, t, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)len);
	ob_cls_obj(key);
	return er;
}

/* Whether len bytes of text hold the string s */
LOCAL BOOL holds( CONST UB *t, INT len, CONST char *s )
{
	INT	n = (INT)knl_strlen(s), i, k;

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && t[i + k] == (UB)s[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* 32 bytes as the 64 hexadecimal digits the settings keep them in */
LOCAL void hex64( CONST UB *b, char *out )
{
	static CONST char d[] = "0123456789abcdef";
	INT	i;

	for ( i = 0; i < 32; i++ ) {
		out[2 * i] = d[b[i] >> 4];
		out[2 * i + 1] = d[b[i] & 15];
	}
	out[64] = '\0';
}

LOCAL BOOL same32( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 32 && a[i] == b[i]; i++ ) ;
	return (BOOL)( i == 32 );
}

LOCAL void test_protect( void )
{
	T_OBCRD	*admin = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	UINT	ops;

	KT_ASSERT(admin != NULL);
	if ( admin == NULL ) return;
	ops = knl_ob_permit_on(&conf, &knl_ob_crd_person);
	KT_ASSERT(( ops & ( OB_OP_READ | OB_OP_WRITE ) ) == 0);
	ops = knl_ob_permit_on(&conf, knl_ob_session_crd());
	KT_ASSERT(( ops & ( OB_OP_READ | OB_OP_WRITE ) ) == 0);
	*admin = knl_ob_crd_person;
	if ( admin->ngrp < CNF_OB_MAX_GROUPS ) admin->grp[admin->ngrp++] = ob_group_admin;
	ops = knl_ob_permit_on(&conf, admin);
	KT_ASSERT(( ops & OB_OP_READ ) != 0);
	tm_printf((UB *)"  利用者 may 0x%x of the settings, an administrator 0x%x\n",
		  knl_ob_permit_on(&conf, &knl_ob_crd_person), ops);
	Kfree(admin);
}

LOCAL void test_save( void )
{
	UB	pmk[32], got[32], *t = NULL;
	char	h[72], us[40];
	INT	len;

	KT_ASSERT_EQ(knl_wpa_psk(PASS_A, (int)knl_strlen(PASS_A), (CONST UB *)SSID_A, 9, pmk), 0);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_A " wpa2 auto " PASS_A), E_OK);
	len = conf_get(&t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		hex64(pmk, h);
		KT_ASSERT(holds(t, len, "$$WIFIPSK"));
		KT_ASSERT(holds(t, len, h));			/* the key */
		KT_ASSERT(!holds(t, len, PASS_A));		/* not the passphrase */
		KT_ASSERT(!holds(t, len, "$$WIFIPASS"));
		KT_ASSERT(ts_uuid_to_str(&me->user, us, sizeof(us)) >= E_OK);
		KT_ASSERT(holds(t, len, HEX_A " wpa2 auto ") && holds(t, len, us));	/* with its user */
		knl_memset(t, 0, len);
		Kfree(t);
	}
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk));
	/* what the user's process reads: the network, without the user, and no key */
	len = knl_wifi_state_text(me, &t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		KT_ASSERT(holds(t, len, "SAVED " HEX_A " wpa2 auto\n"));
		hex64(pmk, h);
		KT_ASSERT(!holds(t, len, h));
		KT_ASSERT(!holds(t, len, PASS_A));
		Kfree(t);
	}
	/* a passphrase that is no WPA2 one */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_A " wpa2 auto short"), E_PAR);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk));
}

LOCAL void test_keep( void )
{
	UB	pmk[32], pmk2[32], got[32];

	KT_ASSERT_EQ(knl_wpa_psk(PASS_A, (int)knl_strlen(PASS_A), (CONST UB *)SSID_A, 9, pmk), 0);
	/* the box left with its stand-ins: saved with no passphrase, joined by itself no more */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_A " wpa2 manual"), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk));
	/* typed again: the new key */
	KT_ASSERT_EQ(knl_wpa_psk(PASS_A2, (int)knl_strlen(PASS_A2), (CONST UB *)SSID_A, 9, pmk2), 0);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_A " wpa2 auto " PASS_A2), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk2));
	KT_ASSERT(!same32(got, pmk));
	/* a network with no key saved cannot be saved with none */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE 6b7465737430 wpa2 auto"), E_NOEXS);
}

LOCAL void test_others( void )
{
	UB	pmk2[32], pmkb[32], got[32], *t = NULL;
	INT	len;

	KT_ASSERT_EQ(knl_wpa_psk(PASS_A2, (int)knl_strlen(PASS_A2), (CONST UB *)SSID_A, 9, pmk2), 0);
	/* not seen, not joined, not forgotten by another user */
	len = knl_wifi_state_text(other, &t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		KT_ASSERT(!holds(t, len, HEX_A));
		Kfree(t);
	}
	KT_ASSERT_ER(knl_wifi_key_for(other, (CONST UB *)SSID_A, 9, got), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_request(other, "FORGET " HEX_A), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_request(other, "SAVE " HEX_A " wpa2 auto"), E_NOEXS);	/* no key of its own */
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk2));
	/* the same SSID as a network of their own: their key, and the first user's stays */
	KT_ASSERT_EQ(knl_wpa_psk(PASS_B, (int)knl_strlen(PASS_B), (CONST UB *)SSID_A, 9, pmkb), 0);
	KT_ASSERT_ER(knl_wifi_request(other, "SAVE " HEX_A " wpa2 auto " PASS_B), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(other, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmkb));
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);
	KT_ASSERT(same32(got, pmk2));
	len = knl_wifi_state_text(me, &t);
	if ( t != NULL ) {
		KT_ASSERT(holds(t, len, "SAVED " HEX_A " wpa2 auto\n"));
		KT_ASSERT(!holds(t, len, "SAVED " HEX_A " wpa2 auto\nSAVED " HEX_A));	/* one line, its own */
		Kfree(t);
	}
}

LOCAL void test_switch( void )
{
	/* nobody logged in: the networks of 利用者 */
	KT_ASSERT_ER(knl_ob_session(NULL), E_OK);
	KT_ASSERT(knl_wifi_auto_for(me, (CONST UB *)SSID_A, 9));
	KT_ASSERT(!knl_wifi_auto_for(other, (CONST UB *)SSID_A, 9));
	/* the other user logs in: theirs are joined, the one before's left */
	KT_ASSERT_ER(knl_ob_session(other), E_OK);
	KT_ASSERT(!knl_wifi_auto_for(me, (CONST UB *)SSID_A, 9));
	KT_ASSERT(knl_wifi_auto_for(other, (CONST UB *)SSID_A, 9));
	KT_ASSERT_ER(knl_ob_session(NULL), E_OK);
	KT_ASSERT(knl_wifi_auto_for(me, (CONST UB *)SSID_A, 9));
	KT_ASSERT_ER(knl_wifi_request(other, "FORGET " HEX_A), E_OK);
}

LOCAL void test_forget( void )
{
	UB	pmk[32], got[32];

	KT_ASSERT_EQ(knl_wpa_psk(PASS_B, (int)knl_strlen(PASS_B), (CONST UB *)SSID_B, 11, pmk), 0);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_B " wpa2 auto " PASS_B), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_B, 11, got), E_OK);
	KT_ASSERT(same32(got, pmk));
	KT_ASSERT_ER(knl_wifi_request(me, "FORGET " HEX_A), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_B, 11, got), E_OK);
	KT_ASSERT(same32(got, pmk));
	KT_ASSERT_ER(knl_wifi_request(me, "FORGET " HEX_A), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_request(me, "FORGET " HEX_B), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_B, 11, got), E_NOEXS);
}

LOCAL void test_old( void )
{
	static CONST char old[] = "$$WIFI\ton\n$$WIFINET\t" HEX_A " wpa2 auto\n$$WIFIPASS\t0 " PASS_A "\n";
	/* every user's ("all") and no user, the SSID twice: one network of the person logged in */
	static CONST char owned[] = "$$WIFI\ton\n"
		"$$WIFINET\t" HEX_B " wpa2 auto all\n"
		"$$WIFINET\t" HEX_B " wpa2 manual\n"
		"$$WIFIPASS\t1 " PASS_B "\n";
	UB	pmk[32], got[32], *t = NULL;
	char	h[72], us[40];
	INT	len;

	KT_ASSERT_ER(ts_uuid_to_str(&me->user, us, sizeof(us)), E_OK);
	KT_ASSERT_EQ(knl_wpa_psk(PASS_A, (int)knl_strlen(PASS_A), (CONST UB *)SSID_A, 9, pmk), 0);
	KT_ASSERT_ER(conf_put((CONST UB *)old, (INT)knl_strlen(old)), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_A, 9, got), E_OK);	/* 利用者 is logged in */
	KT_ASSERT(same32(got, pmk));
	KT_ASSERT_ER(knl_wifi_key_for(other, (CONST UB *)SSID_A, 9, got), E_NOEXS);
	len = conf_get(&t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		hex64(pmk, h);
		KT_ASSERT(!holds(t, len, PASS_A));
		KT_ASSERT(!holds(t, len, "$$WIFIPASS"));
		KT_ASSERT(holds(t, len, h));
		KT_ASSERT(holds(t, len, us));
		knl_memset(t, 0, len);
		Kfree(t);
	}
	KT_ASSERT_EQ(knl_wpa_psk(PASS_B, (int)knl_strlen(PASS_B), (CONST UB *)SSID_B, 11, pmk), 0);
	KT_ASSERT_ER(conf_put((CONST UB *)owned, (INT)knl_strlen(owned)), E_OK);
	KT_ASSERT_ER(knl_wifi_key_for(me, (CONST UB *)SSID_B, 11, got), E_OK);
	KT_ASSERT(same32(got, pmk));
	len = conf_get(&t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		hex64(pmk, h);
		KT_ASSERT(holds(t, len, "$$WIFINET\t" HEX_B " wpa2 auto "));
		KT_ASSERT(!holds(t, len, "manual"));		/* the second line taken into the first */
		KT_ASSERT(!holds(t, len, " all"));
		KT_ASSERT(holds(t, len, us));
		KT_ASSERT(holds(t, len, h));
		KT_ASSERT(!holds(t, len, PASS_B));
		knl_memset(t, 0, len);
		Kfree(t);
	}
}

/* What a join of SSID_C would be given, as me */
LOCAL INT secret_c( CONST T_OBCRD *crd, UB *pmk, BOOL *have_pmk, UB *pt, INT *pt_len, char *pw, INT *pw_len )
{
	return knl_wifi_secret_for(crd, (CONST UB *)SSID_C, 9, pmk, have_pmk, pt, pt_len, pw, pw_len);
}

LOCAL void test_wpa3( void )
{
	UB	pmk[32], got[32], pt[132], want_pt[132], *t = NULL;
	char	pw[64], h[72];
	INT	len, pt_len = 0, pw_len = 0;
	int	wl = 0;
	BOOL	have = TRUE;

	/* WPA3: the password kept, no PMK */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 auto " PASS_C), E_OK);
	len = conf_get(&t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		KT_ASSERT(holds(t, len, HEX_C " wpa3 auto "));
		KT_ASSERT(holds(t, len, "$$WIFISAE"));
		KT_ASSERT(holds(t, len, PASS_C_HEX));
		KT_ASSERT(!holds(t, len, PASS_C));		/* not as letters */
		knl_memset(t, 0, len);
		Kfree(t);
	}
	KT_ASSERT_EQ(secret_c(me, got, &have, pt, &pt_len, pw, &pw_len), SAVED_WPA3);
	KT_ASSERT(!have);
	KT_ASSERT_EQ(pw_len, (INT)knl_strlen(PASS_C));
	KT_ASSERT(pw_len > 0 && knl_strcmp(pw, PASS_C) == 0);
	KT_ASSERT_EQ(knl_wpa_sae_pt((CONST UB *)SSID_C, 9, PASS_C, (int)knl_strlen(PASS_C), want_pt, &wl), 0);
	KT_ASSERT(pt_len > 0 && pt_len == wl);
	for ( len = 0; len < pt_len && len < (INT)sizeof(pt) && pt[len] == want_pt[len]; len++ ) ;
	KT_ASSERT_EQ(len, pt_len);
	tm_printf((UB *)"  WPA3: the password of %d letters and H2E's element of %d bytes for the join\n",
		  pw_len, pt_len);
	/* without the element: the password alone */
	KT_ASSERT_EQ(secret_c(me, got, &have, NULL, NULL, pw, &pw_len), SAVED_WPA3);
	KT_ASSERT(knl_strcmp(pw, PASS_C) == 0);
	/* what the user's process reads: the network, neither the password nor its digits */
	len = knl_wifi_state_text(me, &t);
	KT_ASSERT(len > 0);
	if ( t != NULL ) {
		KT_ASSERT(holds(t, len, "SAVED " HEX_C " wpa3 auto\n"));
		KT_ASSERT(!holds(t, len, PASS_C));
		KT_ASSERT(!holds(t, len, PASS_C_HEX));
		Kfree(t);
	}
	KT_ASSERT_ER(secret_c(other, got, &have, NULL, NULL, pw, &pw_len), E_NOEXS);
	/* not a password of WPA3 (short, 64 hexadecimal digits of a PSK, a letter not printable) */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 auto short"), E_PAR);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 auto "
				       "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), E_PAR);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 auto tab\there12"), E_PAR);
	KT_ASSERT_EQ(secret_c(me, got, &have, NULL, NULL, pw, &pw_len), SAVED_WPA3);
	KT_ASSERT(knl_strcmp(pw, PASS_C) == 0);
	/* kept when saved with no password */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 manual"), E_OK);
	KT_ASSERT_EQ(secret_c(me, got, &have, NULL, NULL, pw, &pw_len), SAVED_WPA3);
	KT_ASSERT(knl_strcmp(pw, PASS_C) == 0);
	/* WPA2/WPA3: a PMK is needed besides, so a password is typed */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa2wpa3 auto"), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa2wpa3 auto " PASS_C), E_OK);
	KT_ASSERT_EQ(knl_wpa_psk(PASS_C, (int)knl_strlen(PASS_C), (CONST UB *)SSID_C, 9, pmk), 0);
	KT_ASSERT_EQ(secret_c(me, got, &have, NULL, NULL, pw, &pw_len), SAVED_WPA2WPA3);
	KT_ASSERT(have && same32(got, pmk));
	KT_ASSERT(knl_strcmp(pw, PASS_C) == 0);
	len = conf_get(&t);
	if ( t != NULL ) {
		hex64(pmk, h);
		KT_ASSERT(holds(t, len, HEX_C " wpa2wpa3 auto "));
		KT_ASSERT(holds(t, len, h) && holds(t, len, PASS_C_HEX));
		knl_memset(t, 0, len);
		Kfree(t);
	}
	/* WPA2 again: the PMK stays, the password goes */
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa2 auto"), E_OK);
	KT_ASSERT_EQ(secret_c(me, got, &have, pt, &pt_len, pw, &pw_len), SAVED_WPA2);
	KT_ASSERT(have && same32(got, pmk));
	KT_ASSERT_EQ(pw_len, 0);
	KT_ASSERT_EQ(pt_len, 0);
	len = conf_get(&t);
	if ( t != NULL ) {
		KT_ASSERT(!holds(t, len, "$$WIFISAE"));
		KT_ASSERT(!holds(t, len, PASS_C_HEX));
		knl_memset(t, 0, len);
		Kfree(t);
	}
	KT_ASSERT_ER(knl_wifi_request(me, "SAVE " HEX_C " wpa3 auto"), E_NOEXS);
	KT_ASSERT_ER(knl_wifi_request(me, "FORGET " HEX_C), E_OK);
	KT_ASSERT_ER(secret_c(me, got, &have, NULL, NULL, pw, &pw_len), E_NOEXS);
	knl_memset(pw, 0, sizeof(pw));
	knl_memset(pt, 0, sizeof(pt));
}

EXPORT void ktest_wifikey( void )
{
	if ( ts_str_to_uuid(SYSDEF_CONF_WIFI, &conf) < E_OK || ( was_len = conf_get(&was) ) < 0 ) {
		kt_skip("wifikey", "no Wi-Fi settings object on the system volume (WIFI=1 puts it there)");
		return;
	}
	me = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD) * 2);
	if ( me == NULL ) {
		kt_skip("wifikey", "no memory");
		return;
	}
	other = me + 1;
	*me = knl_ob_crd_person;
	*other = knl_ob_crd_person;
	other->user.b[15] ^= 0x5a;			/* a user that is not 利用者 */
	(void)knl_ob_session(NULL);			/* nobody logged in: 利用者 */

	KT_RUN(test_protect);
	KT_RUN(test_save);
	KT_RUN(test_keep);
	KT_RUN(test_others);
	KT_RUN(test_switch);
	KT_RUN(test_forget);
	KT_RUN(test_old);
	KT_RUN(test_wpa3);

	/* the settings as they were, read again */
	(void)knl_ob_session(NULL);
	(void)conf_put(was, was_len);
	{
		UB	got[32];

		(void)knl_wifi_key_for(me, (CONST UB *)"x", 1, got);
	}
	if ( was != NULL ) {
		knl_memset(was, 0, was_len);
		Kfree(was);
	}
	Kfree(me);
}
