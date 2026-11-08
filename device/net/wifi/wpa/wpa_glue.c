/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpa_glue.c (x64 PC, Raspberry Pi 5)
 *	The WPA2-Personal and WPA3-Personal handshakes of the Wi-Fi (design
 *	9.6, 9.6.9), with wpa_supplicant's state machine (src/rsn_supp), its
 *	SAE (src/common/sae.c) and its cryptography (Mbed TLS's, through
 *	crypto_mbedtls.c).
 *
 *	knl_wpa_start2 sets the state machine up for the key management,
 *	the protection of management frames and the keys a join was given
 *	(knl_wpa_choose): PSK, PSK-SHA256 or SAE; MFPC, and MFPR as well
 *	for SAE or an access point that requires it, with BIP-CMAC-128 the
 *	management group cipher; the PMK of the passphrase or SAE's with its
 *	PMKID in our element. The integrity group key it is given in the
 *	handshakes goes to the card like the others (cipher 6).
 *
 *	SAE (knl_wpa_sae_*) is the station's side of sae.c, as
 *	wpa_supplicant's own sme.c drives it: our Commit (by hunting and
 *	pecking from the password, or by H2E from its password element), the
 *	access point's Commit taken and our Confirm sent, its Confirm checked;
 *	a Commit again with the anti-clogging token asked for, or with the
 *	next group (19, 20, 21) when the access point does not take one.
 *
 *	This file is compiled with wpa_supplicant's headers into one object
 *	with them (tools/wpa/build.sh). It sets the state machine up as
 *	wpa_supplicant does for a network of WPA2-Personal: RSN, the key
 *	management of PSK, the pairwise cipher CCMP (TKIP when the access
 *	point has nothing else), the group cipher the access point names,
 *	no protection of management frames (an access point that requires
 *	it is refused at the start); the pairwise master key from the
 *	passphrase and the SSID. It gives the state machine its callbacks,
 *	which reach the card through the Wi-Fi manager (wifi.c): EAPOL
 *	frames out, keys into net80211, the association left.
 *
 *	What wpa_supplicant's own operating system layer, event loop and
 *	debug output would give is here too: memory from the kernel's C
 *	library, the time and random bytes from the manager, timers run
 *	from the manager's loop, and the state machine's words to the
 *	console (keys are never printed). Built for the host's test
 *	(TSWPA_HOST_TEST), wpa_supplicant's own os_unix.c and wpa_debug.c
 *	take the place of those.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "common/defs.h"
#include "common/eapol_common.h"
#ifdef IEEE8021X_EAPOL
#include "eapol_supp/eapol_supp_sm.h"
#endif
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "crypto/sha1.h"
#include "crypto/crypto.h"
#include "common/sae.h"
#include "rsn_supp/wpa.h"
#include "wpa_glue.h"

static struct wpa_sm		*sm;
static enum wpa_states		wstate = WPA_DISCONNECTED;
static u8			cur_bssid[ETH_ALEN];
static int			network;	/* the network context: any non-NULL pointer */

/* ---------------------------------------------------------------- the state machine's callbacks */

static void g_set_state(void *ctx, enum wpa_states s)
{
	wstate = s;
}

static enum wpa_states g_get_state(void *ctx)
{
	return wstate;
}

static void g_deauth(void *ctx, u16 reason)
{
	wstate = WPA_DISCONNECTED;
	knl_wifi_deauth(reason);
}

static void g_reconnect(void *ctx)
{
	knl_wifi_deauth(WLAN_REASON_DEAUTH_LEAVING);
}

static int g_set_key(void *ctx, int link_id, enum wpa_alg alg, const u8 *addr,
		     int key_idx, int set_tx, const u8 *seq, size_t seq_len,
		     const u8 *key, size_t key_len, enum key_flag key_flag)
{
	static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	int cipher;

	switch (alg) {
	case WPA_ALG_NONE:
		if (addr == NULL || is_broadcast_ether_addr(addr))
			return knl_wifi_delkey(NULL, key_idx);
		return knl_wifi_delkey(addr, key_idx);
	case WPA_ALG_TKIP:
		cipher = 1;
		break;
	case WPA_ALG_CCMP:
		cipher = WPAG_CIPHER_CCMP;
		break;
	case WPA_ALG_BIP_CMAC_128:
		cipher = WPAG_CIPHER_BIP;	/* the IGTK: idx 4 or 5, seq its IPN */
		break;
	default:
		return 0;
	}
	return knl_wifi_setkey(cipher, addr != NULL ? addr : bcast, key_idx, set_tx,
			       seq, (int)seq_len, key, (int)key_len);
}

static void *g_get_network_ctx(void *ctx)
{
	return &network;
}

static int g_get_bssid(void *ctx, u8 *bssid)
{
	os_memcpy(bssid, cur_bssid, ETH_ALEN);
	return 0;
}

static int g_ether_send(void *ctx, const u8 *dest, u16 proto, const u8 *buf, size_t len)
{
	return knl_wifi_eapol_tx(dest, buf, (int)len);
}

static int g_get_beacon_ie(void *ctx)
{
	return -1;		/* the access point's element was given at the start */
}

static void g_cancel_auth_timeout(void *ctx)
{
}

/* An EAPOL frame of the version wpa_supplicant sends by default */
static u8 *g_alloc_eapol(void *ctx, u8 type, const void *data, u16 data_len,
			 size_t *msg_len, void **data_pos)
{
	struct ieee802_1x_hdr *hdr;

	*msg_len = sizeof(*hdr) + data_len;
	hdr = os_malloc(*msg_len);
	if (hdr == NULL)
		return NULL;
	hdr->version = EAPOL_VERSION;
	hdr->type = type;
	hdr->length = host_to_be16(data_len);
	if (data != NULL)
		os_memcpy(hdr + 1, data, data_len);
	else
		os_memset(hdr + 1, 0, data_len);
	if (data_pos != NULL)
		*data_pos = hdr + 1;
	return (u8 *)hdr;
}

static int g_add_pmkid(void *ctx, void *network_ctx, const u8 *bssid, const u8 *pmkid,
		       const u8 *fils_cache_id, const u8 *pmk, size_t pmk_len,
		       u32 pmk_lifetime, u8 pmk_reauth_threshold, int akmp)
{
	return 0;
}

static int g_remove_pmkid(void *ctx, void *network_ctx, const u8 *bssid,
			  const u8 *pmkid, const u8 *fils_cache_id)
{
	return 0;
}

static int g_mlme_setprotection(void *ctx, const u8 *addr, int protection_type, int key_type)
{
	return 0;
}

static int g_key_mgmt_set_pmk(void *ctx, const u8 *pmk, size_t pmk_len)
{
	return 0;
}

/* ---------------------------------------------------------------- what the manager calls */

/* The security of a network, from its RSN and WPA elements */
int knl_wpa_classify(const unsigned char *ies, int ies_len, int privacy)
{
	const u8 *p = ies, *end = ies + ies_len;
	struct wpa_ie_data d;
	int wpa1 = 0;

	(void)d;
	while (end - p >= 2 && p + 2 + p[1] <= end) {
		if (p[0] == WLAN_EID_RSN) {
			/*
			 * The key managements read off the element itself, suite by
			 * suite (00-0F-AC:n, IEEE 802.11 Table 9-151): PSK 2, 4
			 * (FT), 6 (SHA-256), 19 (FT, SHA-384); SAE 8, 9 (FT), 24,
			 * 25 (FT, the extended key); anything else (802.1X 1, 3, 5,
			 * the Suite B ones 11 to 13, FILS, OWE...) is not
			 * WPA-Personal. Not a library's table, which leaves out
			 * the suites it was not built with
			 */
			const u8 *s = p + 2, *e = p + 2 + p[1];
			int n, i, psk = 0, sae = 0;

			if (e - s < 2 + 4)
				return WPAG_8021X;
			s += 2 + 4;			/* the version, the group cipher */
			if (e - s < 2)
				return WPAG_8021X;
			n = s[0] | (s[1] << 8);		/* the pairwise ciphers */
			if (e - s < 2 + 4 * n)
				return WPAG_8021X;
			s += 2 + 4 * n;
			if (e - s < 2)
				return WPAG_8021X;
			n = s[0] | (s[1] << 8);
			s += 2;
			for (i = 0; i < n && e - s >= 4; i++, s += 4) {
				if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac)
					continue;
				if (s[3] == 2 || s[3] == 4 || s[3] == 6 || s[3] == 19)
					psk = 1;
				if (s[3] == 8 || s[3] == 9 || s[3] == 24 || s[3] == 25)
					sae = 1;
			}
			if (psk && sae)
				return WPAG_WPA2WPA3;
			if (psk)
				return WPAG_WPA2;
			if (sae)
				return WPAG_WPA3;
			return WPAG_8021X;
		}
		if (p[0] == WLAN_EID_VENDOR_SPECIFIC && p[1] >= 4
		    && WPA_GET_BE32(p + 2) == WPA_IE_VENDOR_TYPE)
			wpa1 = 1;
		p += 2 + p[1];
	}
	if (wpa1)
		return WPAG_WPA;
	return privacy ? WPAG_WEP : WPAG_OPEN;
}

int knl_wpa_psk(const char *pass, int pass_len, const unsigned char *ssid,
		int ssid_len, unsigned char *pmk)
{
	char p[64];

	if (pass_len == 2 * PMK_LEN)
		return hexstr2bin(pass, pmk, PMK_LEN) == 0 ? 0 : -1;
	if (pass_len < 8 || pass_len > 63)
		return -1;
	os_memcpy(p, pass, pass_len);
	p[pass_len] = '\0';
	if (pbkdf2_sha1(p, ssid, ssid_len, 4096, pmk, PMK_LEN) != 0) {
		forced_memzero(p, sizeof(p));
		return -1;
	}
	forced_memzero(p, sizeof(p));
	return 0;
}

static int sm_make(void)
{
	struct wpa_sm_ctx *ctx;

	if (sm != NULL)
		return 0;
	ctx = os_zalloc(sizeof(*ctx));
	if (ctx == NULL)
		return -1;
	ctx->set_state = g_set_state;
	ctx->get_state = g_get_state;
	ctx->deauthenticate = g_deauth;
	ctx->reconnect = g_reconnect;
	ctx->set_key = g_set_key;
	ctx->get_network_ctx = g_get_network_ctx;
	ctx->get_bssid = g_get_bssid;
	ctx->ether_send = g_ether_send;
	ctx->get_beacon_ie = g_get_beacon_ie;
	ctx->cancel_auth_timeout = g_cancel_auth_timeout;
	ctx->alloc_eapol = g_alloc_eapol;
	ctx->add_pmkid = g_add_pmkid;
	ctx->remove_pmkid = g_remove_pmkid;
	ctx->mlme_setprotection = g_mlme_setprotection;
	ctx->key_mgmt_set_pmk = g_key_mgmt_set_pmk;
	sm = wpa_sm_init(ctx);
	if (sm == NULL) {
		os_free(ctx);
		return -1;
	}
	return 0;
}

int knl_wpa_start(const unsigned char *own, const unsigned char *bssid,
		  const unsigned char *ssid, int ssid_len,
		  const unsigned char *ap_ie, int ap_ie_len,
		  const unsigned char *pmk, unsigned char *ie, int *ie_len)
{
	static u8 ssid_kept[SSID_MAX_LEN];
	struct rsn_supp_config conf;
	struct wpa_ie_data d;
	size_t len = 64;
	int pairwise, group;

	if (sm_make() != 0)
		return -1;
	if (ap_ie == NULL || wpa_parse_wpa_ie_rsn(ap_ie, ap_ie_len, &d) != 0) {
		wpa_printf(MSG_INFO, "WPA: the access point has no RSN element this can read");
		return -1;
	}
	if (!(d.key_mgmt & WPA_KEY_MGMT_PSK)) {
		wpa_printf(MSG_INFO, "WPA: the access point offers no WPA2-Personal (key management 0x%x)",
			   d.key_mgmt);
		return -1;
	}
	/*
	 * An access point that requires the protection of management frames
	 * refuses an association whose element does not offer it, and
	 * net80211 here has no keys for it: said now rather than left to
	 * time out (one that only offers it takes the station without)
	 */
	if (d.capabilities & WPA_CAPABILITY_MFPR) {
		wpa_printf(MSG_INFO, "WPA: the access point requires the protection of management frames (PMF), which this card cannot give");
		return -1;
	}
	if (d.pairwise_cipher & WPA_CIPHER_CCMP)
		pairwise = WPA_CIPHER_CCMP;
	else if (d.pairwise_cipher & WPA_CIPHER_TKIP)
		pairwise = WPA_CIPHER_TKIP;
	else
		return -1;
	if (d.group_cipher & (WPA_CIPHER_CCMP | WPA_CIPHER_TKIP))
		group = d.group_cipher & WPA_CIPHER_CCMP ? WPA_CIPHER_CCMP : WPA_CIPHER_TKIP;
	else
		return -1;

	wpa_sm_notify_disassoc(sm);
	os_memcpy(cur_bssid, bssid, ETH_ALEN);
	if (ssid_len < 0 || ssid_len > SSID_MAX_LEN)
		return -1;
	os_memcpy(ssid_kept, ssid, ssid_len);
	os_memset(&conf, 0, sizeof(conf));
	conf.network_ctx = &network;
	conf.ssid = ssid_kept;
	conf.ssid_len = ssid_len;
	wpa_sm_set_config(sm, &conf);
	wpa_sm_set_own_addr(sm, own);
	wpa_sm_set_param(sm, WPA_PARAM_PROTO, WPA_PROTO_RSN);
	wpa_sm_set_param(sm, WPA_PARAM_PAIRWISE, pairwise);
	wpa_sm_set_param(sm, WPA_PARAM_GROUP, group);
	wpa_sm_set_param(sm, WPA_PARAM_KEY_MGMT, WPA_KEY_MGMT_PSK);
	wpa_sm_set_param(sm, WPA_PARAM_MGMT_GROUP, 0);
	wpa_sm_set_param(sm, WPA_PARAM_MFP, NO_MGMT_FRAME_PROTECTION);
	wpa_sm_set_param(sm, WPA_PARAM_RSN_ENABLED, 1);
	/* the element of a network joined before is forgotten, or the state machine keeps it */
	wpa_sm_set_assoc_wpa_ie(sm, NULL, 0);
	if (wpa_sm_set_ap_wpa_ie(sm, NULL, 0) != 0
	    || wpa_sm_set_ap_rsn_ie(sm, ap_ie, ap_ie_len) != 0
	    || wpa_sm_set_assoc_wpa_ie_default(sm, ie, &len) != 0)
		return -1;
	*ie_len = (int)len;
	wpa_sm_set_pmk(sm, pmk, PMK_LEN, NULL, NULL);
	wstate = WPA_ASSOCIATING;
	wpa_printf(MSG_INFO, "WPA: WPA2-Personal, pairwise %s, group %s",
		   pairwise == WPA_CIPHER_CCMP ? "CCMP" : "TKIP",
		   group == WPA_CIPHER_CCMP ? "CCMP" : "TKIP");
	return 0;
}

/* ---------------------------------------------------------------- WPA3 and PMF */

/* Whether an RSNXE says SAE-H2E (its capabilities' bit 5) */
static int rsnxe_h2e(const u8 *ie, int len)
{
	return ie != NULL && len >= 3 && ie[0] == WLAN_EID_RSNX && ie[1] >= 1
		&& (ie[2] & BIT(WLAN_RSNX_CAPAB_SAE_H2E)) != 0;
}

int knl_wpa_choose(int saved, const unsigned char *ap_rsn, int ap_rsn_len,
		   const unsigned char *ap_rsnxe, int ap_rsnxe_len, int card_pmf,
		   int have_pt, int *akm, int *pmf, int *h2e)
{
	struct wpa_ie_data d;
	int mfpc, mfpr;

	if (ap_rsn == NULL || wpa_parse_wpa_ie_rsn(ap_rsn, ap_rsn_len, &d) != 0)
		return -1;
	mfpc = (d.capabilities & WPA_CAPABILITY_MFPC) != 0;
	mfpr = (d.capabilities & WPA_CAPABILITY_MFPR) != 0;
	*h2e = 0;
	if (saved == WPAG_SAVED_EAP) {
		if (mfpr && !card_pmf)
			return -1;
		if ((d.key_mgmt & WPA_KEY_MGMT_IEEE8021X_SHA256) && card_pmf && mfpc)
			*akm = WPAG_AKM_8021X_SHA256;
		else if (d.key_mgmt & WPA_KEY_MGMT_IEEE8021X)
			*akm = WPAG_AKM_8021X;
		else
			return -1;
		*pmf = !(card_pmf && mfpc) ? WPAG_PMF_OFF : mfpr ? WPAG_PMF_REQUIRED : WPAG_PMF_OPTIONAL;
		return 0;
	}
	/* SAE, which requires the protection of management frames on both sides */
	if ((saved == WPAG_SAVED_WPA3 || saved == WPAG_SAVED_WPA2WPA3)
	    && (d.key_mgmt & WPA_KEY_MGMT_SAE) && card_pmf && mfpc) {
		*akm = WPAG_AKM_SAE;
		*pmf = WPAG_PMF_REQUIRED;
		*h2e = have_pt && rsnxe_h2e(ap_rsnxe, ap_rsnxe_len);
		return 0;
	}
	if (saved != WPAG_SAVED_WPA2 && saved != WPAG_SAVED_WPA2WPA3)
		return -1;
	if (mfpr && !card_pmf)
		return -1;
	if ((d.key_mgmt & WPA_KEY_MGMT_PSK_SHA256) && card_pmf && mfpc)
		*akm = WPAG_AKM_PSK_SHA256;
	else if (d.key_mgmt & WPA_KEY_MGMT_PSK)
		*akm = WPAG_AKM_PSK;
	else
		return -1;
	*pmf = !(card_pmf && mfpc) ? WPAG_PMF_OFF : mfpr ? WPAG_PMF_REQUIRED : WPAG_PMF_OPTIONAL;
	return 0;
}

static void eap_tie(int on);

int knl_wpa_start2(const unsigned char *own, const unsigned char *bssid,
		   const unsigned char *ssid, int ssid_len,
		   const unsigned char *ap_rsn, int ap_rsn_len,
		   const unsigned char *ap_rsnxe, int ap_rsnxe_len, int akm, int pmf,
		   const unsigned char *pmk, const unsigned char *pmkid, int h2e,
		   unsigned char *ie, int *ie_len)
{
	static u8 ssid_kept[SSID_MAX_LEN];
	struct rsn_supp_config conf;
	struct wpa_ie_data d;
	size_t len = WPAG_IE_MAX, xlen;
	int pairwise, group, key_mgmt, mfp;

	if (sm_make() != 0)
		return -1;
	if (ap_rsn == NULL || wpa_parse_wpa_ie_rsn(ap_rsn, ap_rsn_len, &d) != 0)
		return -1;
	key_mgmt = akm == WPAG_AKM_SAE ? WPA_KEY_MGMT_SAE
		: akm == WPAG_AKM_8021X ? WPA_KEY_MGMT_IEEE8021X
		: akm == WPAG_AKM_8021X_SHA256 ? WPA_KEY_MGMT_IEEE8021X_SHA256
		: akm == WPAG_AKM_PSK_SHA256 ? WPA_KEY_MGMT_PSK_SHA256 : WPA_KEY_MGMT_PSK;
	if (!(d.key_mgmt & key_mgmt))
		return -1;
	mfp = pmf == WPAG_PMF_REQUIRED ? MGMT_FRAME_PROTECTION_REQUIRED
		: pmf == WPAG_PMF_OPTIONAL ? MGMT_FRAME_PROTECTION_OPTIONAL : NO_MGMT_FRAME_PROTECTION;
	if (mfp == NO_MGMT_FRAME_PROTECTION && (d.capabilities & WPA_CAPABILITY_MFPR))
		return -1;
	if (d.pairwise_cipher & WPA_CIPHER_CCMP)
		pairwise = WPA_CIPHER_CCMP;
	else if ((d.pairwise_cipher & WPA_CIPHER_TKIP) && mfp == NO_MGMT_FRAME_PROTECTION)
		pairwise = WPA_CIPHER_TKIP;
	else
		return -1;
	if (d.group_cipher & (WPA_CIPHER_CCMP | WPA_CIPHER_TKIP))
		group = d.group_cipher & WPA_CIPHER_CCMP ? WPA_CIPHER_CCMP : WPA_CIPHER_TKIP;
	else
		return -1;
	if (ssid_len < 0 || ssid_len > SSID_MAX_LEN)
		return -1;

	wpa_sm_notify_disassoc(sm);
	eap_tie(akm == WPAG_AKM_8021X || akm == WPAG_AKM_8021X_SHA256);
	os_memcpy(cur_bssid, bssid, ETH_ALEN);
	os_memcpy(ssid_kept, ssid, ssid_len);
	os_memset(&conf, 0, sizeof(conf));
	conf.network_ctx = &network;
	conf.ssid = ssid_kept;
	conf.ssid_len = ssid_len;
	wpa_sm_set_config(sm, &conf);
	wpa_sm_set_own_addr(sm, own);
	wpa_sm_set_param(sm, WPA_PARAM_PROTO, WPA_PROTO_RSN);
	wpa_sm_set_param(sm, WPA_PARAM_PAIRWISE, pairwise);
	wpa_sm_set_param(sm, WPA_PARAM_GROUP, group);
	wpa_sm_set_param(sm, WPA_PARAM_KEY_MGMT, key_mgmt);
	wpa_sm_set_param(sm, WPA_PARAM_MGMT_GROUP, mfp != NO_MGMT_FRAME_PROTECTION ? WPA_CIPHER_AES_128_CMAC : 0);
	wpa_sm_set_param(sm, WPA_PARAM_MFP, mfp);
	wpa_sm_set_param(sm, WPA_PARAM_SAE_PWE, h2e ? SAE_PWE_HASH_TO_ELEMENT : SAE_PWE_HUNT_AND_PECK);
	wpa_sm_set_param(sm, WPA_PARAM_RSN_ENABLED, 1);
	/* the elements of a network joined before are forgotten, or the state machine keeps them */
	wpa_sm_set_assoc_wpa_ie(sm, NULL, 0);
	wpa_sm_set_assoc_rsnxe(sm, NULL, 0);
	if (wpa_sm_set_ap_wpa_ie(sm, NULL, 0) != 0
	    || wpa_sm_set_ap_rsn_ie(sm, ap_rsn, ap_rsn_len) != 0
	    || wpa_sm_set_ap_rsnxe(sm, ap_rsnxe, ap_rsnxe != NULL ? ap_rsnxe_len : 0) != 0)
		return -1;
	/* SAE's key with its PMKID (put in our element), or the passphrase's */
	if (akm != WPAG_AKM_8021X && akm != WPAG_AKM_8021X_SHA256)
		wpa_sm_set_pmk(sm, pmk, PMK_LEN, akm == WPAG_AKM_SAE ? pmkid : NULL,
			       akm == WPAG_AKM_SAE ? bssid : NULL);
	if (wpa_sm_set_assoc_wpa_ie_default(sm, ie, &len) != 0)
		return -1;
	if (h2e) {
		xlen = WPAG_IE_MAX - len;
		if (wpa_sm_set_assoc_rsnxe_default(sm, ie + len, &xlen) != 0)
			return -1;
		len += xlen;
	}
	*ie_len = (int)len;
	wstate = WPA_ASSOCIATING;
	wpa_printf(MSG_INFO, "WPA: %s, pairwise %s, group %s, PMF %s%s",
		   akm == WPAG_AKM_SAE ? "WPA3-Personal (SAE)"
		   : akm == WPAG_AKM_8021X || akm == WPAG_AKM_8021X_SHA256 ? "WPA2-Enterprise"
		   : akm == WPAG_AKM_PSK_SHA256 ? "WPA2-Personal (PSK-SHA256)" : "WPA2-Personal",
		   pairwise == WPA_CIPHER_CCMP ? "CCMP" : "TKIP", group == WPA_CIPHER_CCMP ? "CCMP" : "TKIP",
		   mfp == MGMT_FRAME_PROTECTION_REQUIRED ? "required"
		   : mfp == MGMT_FRAME_PROTECTION_OPTIONAL ? "optional" : "off",
		   h2e ? ", H2E" : "");
	return 0;
}

/* ---------------------------------------------------------------- SAE */

#define SAE_RESENDS	5

static struct sae_data	sae;
static int		sae_groups[] = { 19, 20, 21, 0 };
static int		sae_gi;			/* which of them now */
static int		sae_h2e;
static struct sae_pt	*sae_pts;		/* H2E's password elements, one for each group */
static u8		sae_pw[64];		/* hunting and pecking's password, while SAE goes on */
static int		sae_pw_len;
static u8		sae_own[ETH_ALEN], sae_peer[ETH_ALEN];
static struct wpabuf	*sae_token;
static u8		sae_last[WPAG_SAE_MAX];	/* the last frame sent, for its resending */
static int		sae_last_len, sae_last_seq, sae_last_status, sae_sends;

/* A frame to send kept and handed out */
static int sae_out(const struct wpabuf *b, int seq, int status, unsigned char *out, int *out_len,
		   int *out_seq, int *out_status)
{
	if (b == NULL || wpabuf_len(b) > sizeof(sae_last))
		return WPAG_SAE_FAIL;
	os_memcpy(sae_last, wpabuf_head(b), wpabuf_len(b));
	sae_last_len = (int)wpabuf_len(b);
	sae_last_seq = seq;
	sae_last_status = status;
	sae_sends = 1;
	os_memcpy(out, sae_last, sae_last_len);
	*out_len = sae_last_len;
	if (out_seq != NULL)
		*out_seq = seq;
	*out_status = status;
	return WPAG_SAE_SEND;
}

/* Our Commit for the group of sae_gi */
static int sae_commit(unsigned char *out, int *out_len, int *out_seq, int *out_status)
{
	struct wpabuf *b;
	struct sae_pt *pt;
	int r;

	if (sae_groups[sae_gi] == 0 || sae_set_group(&sae, sae_groups[sae_gi]) < 0)
		return WPAG_SAE_FAIL;
	if (sae_h2e) {
		for (pt = sae_pts; pt != NULL && pt->group != sae_groups[sae_gi]; pt = pt->next)
			;
		if (pt == NULL || sae_prepare_commit_pt(&sae, pt, sae_own, sae_peer, NULL, NULL) < 0)
			return WPAG_SAE_FAIL;
	} else if (sae_prepare_commit(sae_own, sae_peer, sae_pw, sae_pw_len, &sae) < 0) {
		return WPAG_SAE_FAIL;
	}
	b = wpabuf_alloc(WPAG_SAE_MAX);
	if (b == NULL || sae_write_commit(&sae, b, sae_token, NULL) < 0) {
		wpabuf_free(b);
		return WPAG_SAE_FAIL;
	}
	sae.state = SAE_COMMITTED;
	r = sae_out(b, 1, sae_h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : WLAN_STATUS_SUCCESS,
		    out, out_len, out_seq, out_status);
	wpabuf_free(b);
	wpa_printf(MSG_INFO, "SAE: Commit of group %d (%s)", sae_groups[sae_gi],
		   sae_h2e ? "H2E" : "hunting and pecking");
	return r;
}

/* An H2E password element of group 19 out of its bytes (x, y) */
static struct sae_pt *pt_of(const unsigned char *pt, int pt_len)
{
	struct sae_pt *p = os_zalloc(sizeof(*p));

	if (p == NULL)
		return NULL;
	p->group = 19;
	p->ec = crypto_ec_init(19);
	if (p->ec == NULL || pt_len != 2 * (int)crypto_ec_prime_len(p->ec)
	    || (p->ecc_pt = crypto_ec_point_from_bin(p->ec, pt)) == NULL) {
		sae_deinit_pt(p);
		return NULL;
	}
	return p;
}

int knl_wpa_sae_pt(const unsigned char *ssid, int ssid_len, const char *pass, int pass_len,
		   unsigned char *pt, int *pt_len)
{
	int groups[] = { 19, 0 };
	struct sae_pt *p;
	size_t plen;
	int r = -1;

	p = sae_derive_pt(groups, ssid, ssid_len, (const u8 *)pass, pass_len, NULL);
	if (p == NULL)
		return -1;
	plen = crypto_ec_prime_len(p->ec);
	if (2 * plen <= WPAG_SAE_PT_MAX && crypto_ec_point_to_bin(p->ec, p->ecc_pt, pt, pt + plen) == 0) {
		*pt_len = (int)(2 * plen);
		r = 0;
	}
	sae_deinit_pt(p);
	return r;
}

void knl_wpa_sae_abort(void)
{
	sae_clear_data(&sae);
	os_memset(&sae, 0, sizeof(sae));
	if (sae_pts != NULL)
		sae_deinit_pt(sae_pts);
	sae_pts = NULL;
	wpabuf_free(sae_token);
	sae_token = NULL;
	forced_memzero(sae_pw, sizeof(sae_pw));
	sae_pw_len = 0;
	sae_last_len = 0;
}

int knl_wpa_sae_start(const unsigned char *own, const unsigned char *bssid, int h2e,
		      const char *pass, int pass_len, const unsigned char *pt, int pt_len,
		      unsigned char *out, int *out_len, int *out_status)
{
	knl_wpa_sae_abort();
	os_memcpy(sae_own, own, ETH_ALEN);
	os_memcpy(sae_peer, bssid, ETH_ALEN);
	sae_gi = 0;
	sae_h2e = h2e;
	if (h2e) {
		/* H2E from the password element of group 19 (knl_wpa_sae_pt) */
		if (pt == NULL || pt_len <= 0 || (sae_pts = pt_of(pt, pt_len)) == NULL)
			return -1;
	} else {
		if (pass == NULL || pass_len <= 0 || pass_len > (int)sizeof(sae_pw))
			return -1;
		os_memcpy(sae_pw, pass, pass_len);
		sae_pw_len = pass_len;
	}
	return sae_commit(out, out_len, NULL, out_status) == WPAG_SAE_SEND ? 0 : -1;
}

/* The token of an anti-clogging request: after the group, in its container element under H2E */
static void take_token(const u8 *body, int len)
{
	const u8 *p = body + 2, *end = body + len;

	wpabuf_free(sae_token);
	sae_token = NULL;
	if (len <= 2)
		return;
	if (sae_h2e && end - p >= 3 && p[0] == WLAN_EID_EXTENSION && p[2] == WLAN_EID_EXT_ANTI_CLOGGING_TOKEN
	    && p + 2 + p[1] <= end) {
		sae_token = wpabuf_alloc_copy(p + 3, p[1] - 1);
		return;
	}
	sae_token = wpabuf_alloc_copy(p, end - p);
}

int knl_wpa_sae_rx(int seq, int status, const unsigned char *body, int len,
		   unsigned char *out, int *out_len, int *out_seq, int *out_status)
{
	struct wpabuf *b;
	u16 res;
	int r;

	if (seq == 1) {
		if (status == WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ) {
			if (sae.state != SAE_COMMITTED)
				return WPAG_SAE_WAIT;
			wpa_printf(MSG_INFO, "SAE: the access point asks for its anti-clogging token");
			take_token(body, len);
			return sae_commit(out, out_len, out_seq, out_status);
		}
		if (status == WLAN_STATUS_FINITE_CYCLIC_GROUP_NOT_SUPPORTED) {
			wpa_printf(MSG_INFO, "SAE: the access point does not take group %d", sae_groups[sae_gi]);
			sae_gi++;
			wpabuf_free(sae_token);
			sae_token = NULL;
			return sae_commit(out, out_len, out_seq, out_status);
		}
		if (status != WLAN_STATUS_SUCCESS && status != WLAN_STATUS_SAE_HASH_TO_ELEMENT) {
			wpa_printf(MSG_INFO, "SAE: the access point refused our Commit (status %d)", status);
			return WPAG_SAE_FAIL;
		}
		if (sae.state == SAE_CONFIRMED) {
			/* its Commit again: our Confirm again */
			return knl_wpa_sae_resend(out, out_len, out_seq, out_status);
		}
		if (sae.state != SAE_COMMITTED)
			return WPAG_SAE_WAIT;
		res = sae_parse_commit(&sae, body, len, NULL, NULL, sae_groups,
				       status == WLAN_STATUS_SAE_HASH_TO_ELEMENT, NULL);
		if (res == SAE_SILENTLY_DISCARD)
			return WPAG_SAE_WAIT;
		if (res != WLAN_STATUS_SUCCESS || sae_process_commit(&sae) < 0) {
			wpa_printf(MSG_INFO, "SAE: the access point's Commit cannot be taken (%u)", res);
			return WPAG_SAE_FAIL;
		}
		b = wpabuf_alloc(WPAG_SAE_MAX);
		if (b == NULL || sae_write_confirm(&sae, b) < 0) {
			wpabuf_free(b);
			return WPAG_SAE_FAIL;
		}
		sae.state = SAE_CONFIRMED;
		r = sae_out(b, 2, WLAN_STATUS_SUCCESS, out, out_len, out_seq, out_status);
		wpabuf_free(b);
		return r;
	}
	if (seq == 2) {
		if (sae.state != SAE_CONFIRMED)
			return WPAG_SAE_WAIT;
		if (status != WLAN_STATUS_SUCCESS || sae_check_confirm(&sae, body, len, NULL) < 0) {
			wpa_printf(MSG_INFO, "SAE: the access point's Confirm is not ours: a wrong password");
			return WPAG_SAE_FAIL;
		}
		sae.state = SAE_ACCEPTED;
		forced_memzero(sae_pw, sizeof(sae_pw));
		sae_pw_len = 0;
		wpa_printf(MSG_INFO, "SAE: accepted (group %d)", sae.group);
		return WPAG_SAE_DONE;
	}
	return WPAG_SAE_WAIT;
}

int knl_wpa_sae_resend(unsigned char *out, int *out_len, int *out_seq, int *out_status)
{
	if (sae_last_len <= 0 || sae_sends > SAE_RESENDS)
		return WPAG_SAE_FAIL;
	sae_sends++;
	os_memcpy(out, sae_last, sae_last_len);
	*out_len = sae_last_len;
	*out_seq = sae_last_seq;
	*out_status = sae_last_status;
	return WPAG_SAE_SEND;
}

int knl_wpa_sae_pmk(unsigned char *pmk, unsigned char *pmkid)
{
	if (sae.state != SAE_ACCEPTED || sae.pmk_len != PMK_LEN)
		return -1;
	os_memcpy(pmk, sae.pmk, PMK_LEN);
	os_memcpy(pmkid, sae.pmkid, SAE_PMKID_LEN);
	return 0;
}

void knl_wpa_assoc(const unsigned char *bssid)
{
	if (sm == NULL)
		return;
	os_memcpy(cur_bssid, bssid, ETH_ALEN);
	wstate = WPA_ASSOCIATED;
	wpa_sm_notify_assoc(sm, bssid);
	knl_wpa_eap_assoc();
}

void knl_wpa_rx(const unsigned char *src, const unsigned char *body, int len)
{
	if (sm == NULL || len <= 0)
		return;
	if (knl_wpa_eap_rx(src, body, len))
		return;
	(void)wpa_sm_rx_eapol(sm, src, body, (size_t)len, FRAME_ENCRYPTION_UNKNOWN);
}

int knl_wpa_state(void)
{
	switch (wstate) {
	case WPA_COMPLETED:
		return WPAG_DONE;
	case WPA_ASSOCIATED:
	case WPA_4WAY_HANDSHAKE:
	case WPA_GROUP_HANDSHAKE:
		return WPAG_HANDSHAKE;
	default:
		return WPAG_NONE;
	}
}

void knl_wpa_disassoc(void)
{
	if (sm != NULL)
		wpa_sm_notify_disassoc(sm);
	wstate = WPA_DISCONNECTED;
}

#ifdef IEEE8021X_EAPOL
/* What wps_eap.c needs of this file */
int knl_wpa_eap_send(const unsigned char *dest, const unsigned char *buf, int len)
{
	return knl_wifi_eapol_tx(dest, buf, len);
}

const unsigned char *knl_wpa_cur_bssid(void)
{
	return cur_bssid;
}

/* The handshake takes the PMK from the supplicant: for this join, or none */
extern struct eapol_sm *knl_wpa_eap_sm(void);

static void eap_tie(int on)
{
	wpa_sm_set_eapol(sm, on ? knl_wpa_eap_sm() : NULL);
}
#else
/* The kernel's own supplicant (WPA_MODE=kernel) has no EAP: such a network is not joined */
int knl_wpa_eap_set(int method, const char *id, int id_len, const char *pw, int pw_len,
		    const unsigned char *ca, int ca_len, const char *domain, int domain_len,
		    const unsigned char *cert, int cert_len, const unsigned char *key, int key_len,
		    const char *key_pw, int key_pw_len)
{
	return -1;
}
void knl_wpa_eap_assoc(void) {}
int knl_wpa_eap_rx(const unsigned char *src, const unsigned char *body, int len) { return 0; }
int knl_wpa_eap_result(void) { return WPAG_EAP_NONE; }
void knl_wpa_eap_end(void) {}

static void eap_tie(int on)
{
	(void)on;
}
#endif

/* ---------------------------------------------------------------- timers */

#define TIMERS	16

struct gtimer {
	int			used;
	unsigned long long	at;		/* ms */
	eloop_timeout_handler	handler;
	void			*eloop_data;
	void			*user_data;
};

static struct gtimer timers[TIMERS];

int eloop_register_timeout(unsigned int secs, unsigned int usecs,
			   eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
	int i;

	for (i = 0; i < TIMERS; i++) {
		if (!timers[i].used) {
			timers[i].used = 1;
			timers[i].at = knl_wifi_ms() + (unsigned long long)secs * 1000 + usecs / 1000;
			timers[i].handler = handler;
			timers[i].eloop_data = eloop_data;
			timers[i].user_data = user_data;
			return 0;
		}
	}
	return -1;
}

static int timer_is(struct gtimer *t, eloop_timeout_handler handler, void *eloop_data,
		    void *user_data)
{
	return t->used && t->handler == handler
		&& (eloop_data == ELOOP_ALL_CTX || t->eloop_data == eloop_data)
		&& (user_data == ELOOP_ALL_CTX || t->user_data == user_data);
}

int eloop_cancel_timeout(eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
	int i, n = 0;

	for (i = 0; i < TIMERS; i++) {
		if (timer_is(&timers[i], handler, eloop_data, user_data)) {
			timers[i].used = 0;
			n++;
		}
	}
	return n;
}

int eloop_is_timeout_registered(eloop_timeout_handler handler, void *eloop_data,
				void *user_data)
{
	int i;

	for (i = 0; i < TIMERS; i++) {
		if (timer_is(&timers[i], handler, eloop_data, user_data))
			return 1;
	}
	return 0;
}

/* A timer brought nearer (deplete) or put further (replenish) than req; 1 when moved, 0 not, -1 none */
static int timer_move(unsigned int req_secs, unsigned int req_usecs, eloop_timeout_handler handler,
		      void *eloop_data, void *user_data, int nearer)
{
	unsigned long long at = knl_wifi_ms() + (unsigned long long)req_secs * 1000 + req_usecs / 1000;
	int i;

	for (i = 0; i < TIMERS; i++) {
		if (timers[i].used && timers[i].handler == handler
		    && timers[i].eloop_data == eloop_data && timers[i].user_data == user_data) {
			if (nearer ? timers[i].at > at : timers[i].at < at) {
				timers[i].at = at;
				return 1;
			}
			return 0;
		}
	}
	return -1;
}

int eloop_deplete_timeout(unsigned int req_secs, unsigned int req_usecs,
			  eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
	return timer_move(req_secs, req_usecs, handler, eloop_data, user_data, 1);
}

int eloop_replenish_timeout(unsigned int req_secs, unsigned int req_usecs,
			    eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
	return timer_move(req_secs, req_usecs, handler, eloop_data, user_data, 0);
}

void knl_wpa_tick(void)
{
	unsigned long long now = knl_wifi_ms();
	struct gtimer t;
	int i;

	for (i = 0; i < TIMERS; i++) {
		if (timers[i].used && timers[i].at <= now) {
			t = timers[i];
			timers[i].used = 0;
			t.handler(t.eloop_data, t.user_data);
		}
	}
}

#ifndef TSWPA_HOST_TEST

/* ---------------------------------------------------------------- the operating system's layer */

int errno;

int os_get_time(struct os_time *t)
{
	unsigned long long ms = knl_wifi_ms();

	t->sec = (os_time_t)(ms / 1000);
	t->usec = (os_time_t)(ms % 1000) * 1000;
	return 0;
}

int os_get_reltime(struct os_reltime *t)
{
	unsigned long long ms = knl_wifi_ms();

	t->sec = (os_time_t)(ms / 1000);
	t->usec = (os_time_t)(ms % 1000) * 1000;
	return 0;
}

int os_get_random(unsigned char *buf, size_t len)
{
	knl_wifi_random(buf, (int)len);
	return 0;
}

unsigned long os_random(void)
{
	unsigned long v;

	knl_wifi_random(&v, sizeof(v));
	return v;
}

void *os_zalloc(size_t size)
{
	void *p = malloc(size);

	if (p != NULL)
		memset(p, 0, size);
	return p;
}

size_t os_strlcpy(char *dest, const char *src, size_t siz)
{
	size_t n = 0;

	while (src[n] != '\0') {
		if (n + 1 < siz)
			dest[n] = src[n];
		n++;
	}
	if (siz > 0)
		dest[n < siz ? n : siz - 1] = '\0';
	return n;
}

int os_memcmp_const(const void *a, const void *b, size_t len)
{
	const u8 *aa = a, *bb = b;
	u8 res = 0;
	size_t i;

	for (i = 0; i < len; i++)
		res |= aa[i] ^ bb[i];
	return res;
}

void *os_memdup(const void *src, size_t len)
{
	void *r = malloc(len);

	if (r != NULL && src != NULL)
		memcpy(r, src, len);
	return r;
}

long strtol(const char *s, char **end, int base)
{
	int neg = 0;
	long v;

	while (*s == ' ' || *s == '	')
		s++;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	v = (long)strtoul(s, end, base);
	return neg ? -v : v;
}

/* What wpa_supplicant calls when it finds itself broken: said, and the manager stopped */
void abort(void)
{
	knl_wifi_say("wpa: abort\n");
	for (;;)
		knl_wifi_halt();
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *r = malloc(n);

	if (r != NULL)
		memcpy(r, s, n);
	return r;
}

/* ---------------------------------------------------------------- what it says */

int wpa_debug_level = MSG_DEBUG;
int wpa_debug_show_keys = 0;
int wpa_debug_timestamp = 0;

static void say(const char *fmt, va_list ap)
{
	char line[256];
	int n;

	n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 3)
		n = sizeof(line) - 3;
	line[n] = '\n';
	line[n + 1] = '\0';
	knl_wifi_say(line);
}

void wpa_printf(int level, const char *fmt, ...)
{
	va_list ap;

	if (level < wpa_debug_level)
		return;
	va_start(ap, fmt);
	say(fmt, ap);
	va_end(ap);
}

/* Bytes are not shown: the console is no place for the handshake's keys */
void wpa_hexdump(int level, const char *title, const void *buf, size_t len)
{
	if (level >= MSG_INFO)
		wpa_printf(level, "%s - hexdump(len=%lu)", title, (unsigned long)len);
}

void wpa_hexdump_key(int level, const char *title, const void *buf, size_t len)
{
}

void wpa_hexdump_ascii(int level, const char *title, const void *buf, size_t len)
{
}

void wpa_hexdump_ascii_key(int level, const char *title, const void *buf, size_t len)
{
}

void wpa_msg(void *ctx, int level, const char *fmt, ...)
{
	va_list ap;

	if (level < wpa_debug_level)
		return;
	va_start(ap, fmt);
	say(fmt, ap);
	va_end(ap);
}

void wpa_msg_ctrl(void *ctx, int level, const char *fmt, ...)
{
}

void wpa_msg_global(void *ctx, int level, const char *fmt, ...)
{
}

void wpa_msg_no_global(void *ctx, int level, const char *fmt, ...)
{
}

#endif /* !TSWPA_HOST_TEST */
