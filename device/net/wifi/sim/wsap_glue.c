/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wsap_glue.c (the Wi-Fi simulator, WIFI_SIM=1)
 *	The access point's side of the WPA2-Personal handshakes (design
 *	9.6.12): hostapd's authenticator (src/ap/wpa_auth.c), of the same
 *	release of wpa_supplicant as the station's state machine, driven
 *	for the simulated access point (wifisim_ap.c).
 *
 *	This file is compiled with wpa_supplicant's headers into an object
 *	of its own (tools/wpa/build.sh ap, wpaap.o), with its own copy of
 *	the station's glue (wpa_glue.c, for its timers, its operating
 *	system's layer and its output) and of the cryptography. Only the
 *	knl_wsap_ names stay global, and what the copy of the glue would
 *	call of the Wi-Fi manager is renamed at its compilation to the
 *	access point's (knl_wsap_say, knl_wsap_ms ...) or to the stand-ins
 *	here: nothing of the access point's shares a state, a timer or a
 *	lock with the station's.
 *
 *	The authenticator is set up as hostapd is for wpa=2,
 *	wpa_key_mgmt=WPA-PSK, rsn_pairwise=CCMP and no protection of
 *	management frames. Its PSK is the PMK of the passphrase and the
 *	SSID. Up to WSAP_STAS stations at once.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "ap/wpa_auth.h"
#include "wpa_glue.h"
#include "wsap_glue.h"

#define WSAP_STAS	4

static struct wpa_authenticator	*auth;
static u8			ap_psk[PMK_LEN];
static u8			ap_addr[ETH_ALEN];

static struct wsta {
	int				used;
	u8				addr[ETH_ALEN];
	struct wpa_state_machine	*sm;
	int				vars[16];	/* the EAPOL variables (wpa_eapol_variable) */
} stas[WSAP_STAS];

static struct wsta *sta_of(const u8 *addr)
{
	int i;

	for (i = 0; i < WSAP_STAS; i++) {
		if (stas[i].used && os_memcmp(stas[i].addr, addr, ETH_ALEN) == 0)
			return &stas[i];
	}
	return NULL;
}

/* ---------------------------------------------------------------- the copy of the station's glue */

/* What the copy of wpa_glue.c sends or sets for a station: nothing here is one */
int wsap_sta_eapol_tx(const unsigned char *dst, const unsigned char *body, int len)
{
	return -1;
}

int wsap_sta_setkey(int cipher, const unsigned char *addr, int idx, int set_tx,
		    const unsigned char *seq, int seq_len, const unsigned char *key, int key_len)
{
	return -1;
}

int wsap_sta_delkey(const unsigned char *addr, int idx)
{
	return -1;
}

void wsap_sta_deauth(int reason)
{
}

/* ---------------------------------------------------------------- the authenticator's callbacks */

static void say_mac(const char *what, const u8 *addr, const char *txt)
{
	char line[200];

	if (addr != NULL)
		os_snprintf(line, sizeof(line), "AP: %s " MACSTR ": %s\n", what, MAC2STR(addr), txt);
	else
		os_snprintf(line, sizeof(line), "AP: %s: %s\n", what, txt);
	knl_wsap_say(line);
}

static void a_logger(void *ctx, const u8 *addr, logger_level level, const char *txt)
{
	say_mac("station", addr, txt);
}

static void a_disconnect(void *ctx, const u8 *addr, u16 reason)
{
	char why[40];

	os_snprintf(why, sizeof(why), "sent away (reason %u)", reason);
	say_mac("station", addr, why);
	knl_wsap_kick(addr, reason);
}

static void a_psk_failure(void *ctx, const u8 *addr)
{
	say_mac("station", addr, "message 2 of the 4-way handshake failed its MIC: a wrong passphrase");
}

static void a_set_eapol(void *ctx, const u8 *addr, wpa_eapol_variable var, int value)
{
	struct wsta *s = sta_of(addr);

	if (s == NULL || (int)var < 0 || var >= 16)
		return;
	if (var == WPA_EAPOL_authorized && s->vars[var] != value)
		knl_wsap_port(addr, value);
	s->vars[var] = value;
}

static int a_get_eapol(void *ctx, const u8 *addr, wpa_eapol_variable var)
{
	struct wsta *s = sta_of(addr);

	if (s == NULL || (int)var < 0 || var >= 16)
		return -1;
	return s->vars[var];
}

static const u8 *a_get_psk(void *ctx, const u8 *addr, const u8 *p2p_dev_addr,
			   const u8 *prev_psk, size_t *psk_len, int *vlan_id)
{
	if (prev_psk != NULL)
		return NULL;
	if (psk_len)
		*psk_len = PMK_LEN;
	if (vlan_id)
		*vlan_id = 0;
	return ap_psk;
}

static int a_set_key(void *ctx, int vlan_id, enum wpa_alg alg, const u8 *addr, int idx,
		     u8 *key, size_t key_len, enum key_flag key_flag)
{
	static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	int cipher;

	if (alg == WPA_ALG_NONE || key_len == 0)
		return 0;		/* a key taken away: the station's goes with its node */
	if (alg == WPA_ALG_CCMP)
		cipher = WSAP_CIPHER_CCMP;
	else if (alg == WPA_ALG_TKIP)
		cipher = WSAP_CIPHER_TKIP;
	else
		return 0;		/* the integrity group key: no protection of management frames */
	return knl_wsap_setkey(cipher, (addr == NULL || is_zero_ether_addr(addr)) ? bcast : addr,
			       idx, key, (int)key_len);
}

static int a_get_seqnum(void *ctx, const u8 *addr, int idx, u8 *seq)
{
	os_memset(seq, 0, WPA_KEY_RSC_LEN);
	return 0;
}

static int a_send_eapol(void *ctx, const u8 *addr, const u8 *data, size_t data_len, int encrypt)
{
	return knl_wsap_eapol_tx(addr, data, (int)data_len);
}

static int a_for_each_sta(void *ctx, int (*cb)(struct wpa_state_machine *sm, void *ctx), void *cb_ctx)
{
	int i;

	for (i = 0; i < WSAP_STAS; i++) {
		if (stas[i].used && stas[i].sm != NULL && cb(stas[i].sm, cb_ctx))
			return 1;
	}
	return 0;
}

static int a_get_sta_count(void *ctx)
{
	int i, n = 0;

	for (i = 0; i < WSAP_STAS; i++)
		n += stas[i].used;
	return n;
}

/* ---------------------------------------------------------------- what the access point calls */

int knl_wsap_start(const unsigned char *own, const unsigned char *ssid, int ssid_len,
		   const char *pass, int pass_len, int group, int group_rekey_s)
{
	static const struct wpa_auth_callbacks cb = {
		.logger = a_logger,
		.disconnect = a_disconnect,
		.psk_failure_report = a_psk_failure,
		.set_eapol = a_set_eapol,
		.get_eapol = a_get_eapol,
		.get_psk = a_get_psk,
		.set_key = a_set_key,
		.get_seqnum = a_get_seqnum,
		.send_eapol = a_send_eapol,
		.get_sta_count = a_get_sta_count,
		.for_each_sta = a_for_each_sta,
	};
	struct wpa_auth_config conf;

	knl_wsap_stop();
	if (ssid_len <= 0 || ssid_len > SSID_MAX_LEN)
		return -1;
	if (knl_wpa_psk(pass, pass_len, ssid, ssid_len, ap_psk) != 0)
		return -1;
	os_memcpy(ap_addr, own, ETH_ALEN);
	os_memset(&conf, 0, sizeof(conf));
	conf.wpa = WPA_PROTO_RSN;
	conf.wpa_key_mgmt = WPA_KEY_MGMT_PSK;
	conf.wpa_pairwise = WPA_CIPHER_CCMP;
	conf.rsn_pairwise = WPA_CIPHER_CCMP | (group == WSAP_CIPHER_TKIP ? WPA_CIPHER_TKIP : 0);
	conf.wpa_group = (group == WSAP_CIPHER_TKIP) ? WPA_CIPHER_TKIP : WPA_CIPHER_CCMP;
	conf.eapol_version = 2;
	conf.wpa_group_update_count = 4;
	conf.wpa_pairwise_update_count = 4;
	conf.wpa_group_rekey = group_rekey_s;
	conf.ieee80211w = NO_MGMT_FRAME_PROTECTION;
	conf.group_mgmt_cipher = WPA_CIPHER_AES_128_CMAC;
	os_memcpy(conf.ssid, ssid, ssid_len);
	conf.ssid_len = ssid_len;
	auth = wpa_init(ap_addr, &conf, &cb, NULL);
	if (auth == NULL)
		return -1;
	if (wpa_init_keys(auth) != 0) {
		wpa_deinit(auth);
		auth = NULL;
		return -1;
	}
	return 0;
}

void knl_wsap_stop(void)
{
	int i;

	for (i = 0; i < WSAP_STAS; i++) {
		if (stas[i].used && stas[i].sm != NULL)
			wpa_auth_sta_deinit(stas[i].sm);
		os_memset(&stas[i], 0, sizeof(stas[i]));
	}
	if (auth != NULL) {
		wpa_deinit(auth);
		auth = NULL;
	}
	knl_wpa_tick();		/* the timers of what was let go run out */
}

int knl_wsap_ie(unsigned char *buf, int max)
{
	const u8 *ie;
	size_t len = 0;

	if (auth == NULL)
		return 0;
	ie = wpa_auth_get_wpa_ie(auth, &len);
	if (ie == NULL || (int)len > max)
		return 0;
	os_memcpy(buf, ie, len);
	return (int)len;
}

int knl_wsap_join(const unsigned char *sta, const unsigned char *ie, int ie_len)
{
	struct wsta *s;
	int i, res;

	if (auth == NULL)
		return WLAN_REASON_UNSPECIFIED;
	knl_wsap_leave(sta);
	for (i = 0; i < WSAP_STAS && stas[i].used; i++)
		;
	if (i == WSAP_STAS)
		return WLAN_REASON_DISASSOC_AP_BUSY;
	s = &stas[i];
	os_memset(s, 0, sizeof(*s));
	s->used = 1;
	os_memcpy(s->addr, sta, ETH_ALEN);
	s->vars[WPA_EAPOL_portEnabled] = 1;
	s->sm = wpa_auth_sta_init(auth, sta, NULL);
	if (s->sm == NULL) {
		s->used = 0;
		return WLAN_REASON_UNSPECIFIED;
	}
	res = wpa_validate_wpa_ie(auth, s->sm, 2412, ie, ie_len, NULL, 0, NULL, 0, NULL, 0, NULL);
	if (res != WPA_IE_OK) {
		say_mac("station", sta, "its RSN element is refused");
		wpa_auth_sta_deinit(s->sm);
		os_memset(s, 0, sizeof(*s));
		return WLAN_REASON_INVALID_IE;
	}
	wpa_auth_sm_event(s->sm, WPA_ASSOC);
	wpa_auth_sta_associated(auth, s->sm);
	return 0;
}

void knl_wsap_leave(const unsigned char *sta)
{
	struct wsta *s = sta_of(sta);

	if (s == NULL)
		return;
	if (s->sm != NULL) {
		wpa_auth_sm_event(s->sm, WPA_DISASSOC);
		wpa_auth_sta_deinit(s->sm);
	}
	os_memset(s, 0, sizeof(*s));
}

void knl_wsap_rx(const unsigned char *sta, const unsigned char *body, int len)
{
	struct wsta *s = sta_of(sta);

	if (auth == NULL || s == NULL || s->sm == NULL || len <= 0)
		return;
	wpa_receive(auth, s->sm, (u8 *)body, (size_t)len);
}

void knl_wsap_tick(void)
{
	knl_wpa_tick();
}

int knl_wsap_done(const unsigned char *sta)
{
	struct wsta *s = sta_of(sta);

	return s != NULL && s->vars[WPA_EAPOL_authorized];
}
