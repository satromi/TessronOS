/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	test_wpa.c
 *	The WPA2 handshakes of the kernel's Wi-Fi, on the host
 *	(tools/wpa/build.sh test).
 *
 *	The supplicant is the kernel's glue (device/net/wifi/wpa/wpa_glue.c)
 *	with wpa_supplicant's state machine, driven through the same calls
 *	the Wi-Fi manager makes (knl_wpa_*); the access point is hostapd's
 *	authenticator (src/ap/wpa_auth.c) of the same release. The EAPOL
 *	frames each sends are queued and handed to the other, the timers run
 *	as the time is moved on, and the keys each side installs are kept.
 *
 *	Cases: CCMP for both keys; CCMP pairwise with a TKIP group key; a
 *	wrong passphrase, which must not end in the keys. In the first two
 *	the pairwise and group keys of both sides must be the same bytes.
 *	The access point's RSN element must be classified as WPA2-Personal,
 *	and the pairwise master key of the passphrase must be that of IEEE
 *	802.11's test vector.
 *
 *	And what happens on the air after that: message 1 lost (the access
 *	point sends it again), message 4 lost (message 3 comes again, after
 *	the station has its pairwise key: it answers once more and does not
 *	install the key a second time), a new group key and a new pairwise
 *	key while connected (the keys of both sides the same again, and not
 *	those before), and an access point that requires the protection of
 *	management frames, which the station must refuse at the start.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "ap/wpa_auth.h"
#include "wpa_glue.h"

static const u8 ap_addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 sta_addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

static struct wpa_authenticator *auth;
static struct wpa_state_machine *asm_;
static u8 ap_psk[PMK_LEN];
static unsigned long long msec;
static int failures;
static int eapol_vars[32];

struct key {
	int	set;
	int	alg;
	u8	key[32];
	int	len;
};
static struct key sta_ptk, sta_gtk, ap_ptk, ap_gtk;
static int sta_ptk_sets, sta_gtk_sets;	/* how many times the station installed each */
static int drop_to_sta, drop_m4;	/* frames to lose: the next to the station, the next message 4 */

/* frames between the two: 0 to the access point, 1 to the station */
#define QLEN 16
static struct {
	int	to;
	u8	data[1024];
	size_t	len;
} q[QLEN];
static int qh, qt;

/* Message 4 of the 4-way handshake: pairwise, no acknowledgement asked, secure */
static int is_m4(const u8 *d, size_t len)
{
	unsigned ki;

	if (len < 7 || d[1] != 3)
		return 0;
	ki = (d[5] << 8) | d[6];
	return (ki & 0x0008) && !(ki & 0x0080) && (ki & 0x0200);
}

static void put(int to, const u8 *data, size_t len)
{
	if (to == 1 && drop_to_sta > 0) {
		drop_to_sta--;
		printf("test: a frame to the station is lost\n");
		return;
	}
	if (to == 0 && drop_m4 > 0 && is_m4(data, len)) {
		drop_m4--;
		printf("test: message 4 is lost\n");
		return;
	}
	if (qt - qh >= QLEN || len > sizeof(q[0].data)) {
		printf("test: the queue is full\n");
		failures++;
		return;
	}
	q[qt % QLEN].to = to;
	memcpy(q[qt % QLEN].data, data, len);
	q[qt % QLEN].len = len;
	qt++;
}

/* ---------------------------------------------------------------- the station's side (the glue's callbacks) */

int knl_wifi_eapol_tx(const unsigned char *dst, const unsigned char *body, int len)
{
	put(0, body, len);
	return 0;
}

int knl_wifi_setkey(int cipher, const unsigned char *addr, int idx, int set_tx,
		    const unsigned char *seq, int seq_len, const unsigned char *key, int key_len)
{
	struct key *k = (addr[0] & 1) ? &sta_gtk : &sta_ptk;

	if (addr[0] & 1)
		sta_gtk_sets++;
	else
		sta_ptk_sets++;
	k->set = 1;
	k->alg = cipher;
	k->len = key_len > 32 ? 32 : key_len;
	memcpy(k->key, key, k->len);
	printf("test: station %s key, cipher %d, index %d, %d bytes\n",
	       (addr[0] & 1) ? "group" : "pairwise", cipher, idx, key_len);
	return 0;
}

int knl_wifi_delkey(const unsigned char *addr, int idx)
{
	return 0;
}

void knl_wifi_deauth(int reason)
{
	printf("test: the station leaves (reason %d)\n", reason);
}

void knl_wifi_say(const char *line)
{
	fputs(line, stdout);
}

void knl_wifi_halt(void)
{
	abort();
}

unsigned long long knl_wifi_ms(void)
{
	return msec;
}

void knl_wifi_random(void *buf, int len)
{
	os_get_random(buf, len);
}

/* Mbed TLS's source of randomness, as the kernel's glue gives it */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	os_get_random(output, len);
	*olen = len;
	return 0;
}

/* ---------------------------------------------------------------- the access point's side */

static void a_logger(void *ctx, const u8 *addr, logger_level level, const char *txt)
{
	printf("test: AP: %s\n", txt);
}

static void a_disconnect(void *ctx, const u8 *addr, u16 reason)
{
	printf("test: AP disconnects the station (reason %u)\n", reason);
}

static void a_set_eapol(void *ctx, const u8 *addr, wpa_eapol_variable var, int value)
{
	if ((int)var >= 0 && var < 32)
		eapol_vars[var] = value;
}

static int a_get_eapol(void *ctx, const u8 *addr, wpa_eapol_variable var)
{
	return ((int)var >= 0 && var < 32) ? eapol_vars[var] : -1;
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
	struct key *k;

	/* the group's integrity key of management frames is not the group key the station is given */
	if ((alg != WPA_ALG_TKIP && alg != WPA_ALG_CCMP) || key_len == 0)
		return 0;
	k = (addr == NULL || is_broadcast_ether_addr(addr)) ? &ap_gtk : &ap_ptk;
	k->set = 1;
	k->alg = alg == WPA_ALG_TKIP ? 1 : 3;
	k->len = key_len > 32 ? 32 : key_len;
	memcpy(k->key, key, k->len);
	return 0;
}

static int a_get_seqnum(void *ctx, const u8 *addr, int idx, u8 *seq)
{
	os_memset(seq, 0, WPA_KEY_RSC_LEN);
	return 0;
}

static int a_send_eapol(void *ctx, const u8 *addr, const u8 *data, size_t data_len, int encrypt)
{
	put(1, data, data_len);
	return 0;
}

static int a_for_each_sta(void *ctx, int (*cb)(struct wpa_state_machine *sm, void *ctx), void *cb_ctx)
{
	return asm_ != NULL ? cb(asm_, cb_ctx) : 0;
}

static int a_get_sta_count(void *ctx)
{
	return 1;
}

/* ---------------------------------------------------------------- one case */

/*
 * The station's key and the access point's are the same: a TKIP key's
 * two MIC keys are given to the station's driver the other way round
 * (what one side sends with, the other checks with)
 */
static int same(const struct key *s, const struct key *a)
{
	if (!s->set || !a->set || s->len != a->len)
		return 0;
	if (s->alg == 1 && s->len == 32)
		return memcmp(s->key, a->key, 16) == 0 && memcmp(s->key + 16, a->key + 24, 8) == 0
			&& memcmp(s->key + 24, a->key + 16, 8) == 0;
	return memcmp(s->key, a->key, s->len) == 0;
}

/* The frames carried and the timers run: until the handshake is done (ms 0), or for ms */
static void run_for(int ms)
{
	int i;

	for (i = 0; i < (ms > 0 ? ms / 10 : 800); i++) {
		while (qh != qt) {
			int k = qh % QLEN;
			int to = q[k].to;
			u8 d[1024];
			size_t l = q[k].len;

			memcpy(d, q[k].data, l);
			qh++;
			if (to == 0)
				wpa_receive(auth, asm_, d, l);
			else
				knl_wpa_rx(ap_addr, d, (int)l);
		}
		if (ms == 0 && knl_wpa_state() == WPAG_DONE && qh == qt)
			break;
		msec += 10;
		knl_wpa_tick();
	}
}

static void run(void)
{
	run_for(0);
}

/* What a case does once the station has associated */
enum { T_PLAIN, T_LOSE_M1, T_LOSE_M4, T_REKEY_GROUP, T_REKEY_PAIR };

static int one(const char *name, int group, const char *ap_pass, const char *sta_pass, int want_ok,
	       int what, int mfp)
{
	static const struct wpa_auth_callbacks cb = {
		.logger = a_logger,
		.disconnect = a_disconnect,
		.set_eapol = a_set_eapol,
		.get_eapol = a_get_eapol,
		.get_psk = a_get_psk,
		.set_key = a_set_key,
		.get_seqnum = a_get_seqnum,
		.send_eapol = a_send_eapol,
		.get_sta_count = a_get_sta_count,
		.for_each_sta = a_for_each_sta,
	};
	static const u8 ssid[] = "TessronOS-test";
	struct wpa_auth_config conf;
	struct key before_p, before_g;
	const u8 *ap_ie;
	size_t ap_ie_len;
	u8 pmk[PMK_LEN], ie[64];
	int ie_len = 0, ok, started;

	printf("=== %s\n", name);
	memset(&sta_ptk, 0, sizeof(sta_ptk));
	memset(&sta_gtk, 0, sizeof(sta_gtk));
	memset(&ap_ptk, 0, sizeof(ap_ptk));
	memset(&ap_gtk, 0, sizeof(ap_gtk));
	memset(eapol_vars, 0, sizeof(eapol_vars));
	eapol_vars[WPA_EAPOL_portEnabled] = 1;
	qh = qt = 0;
	sta_ptk_sets = sta_gtk_sets = 0;
	drop_to_sta = what == T_LOSE_M1 ? 1 : 0;
	drop_m4 = what == T_LOSE_M4 ? 1 : 0;

	memset(&conf, 0, sizeof(conf));
	conf.wpa = WPA_PROTO_RSN;
	conf.wpa_key_mgmt = WPA_KEY_MGMT_PSK;
	conf.wpa_pairwise = WPA_CIPHER_CCMP;
	conf.rsn_pairwise = WPA_CIPHER_CCMP | (group == WPA_CIPHER_TKIP ? WPA_CIPHER_TKIP : 0);
	conf.wpa_group = group;
	conf.eapol_version = 2;
	conf.wpa_group_update_count = 4;
	conf.wpa_pairwise_update_count = 4;
	conf.ieee80211w = mfp;
	/* a new key from the access point every 2 seconds, in the cases of them */
	conf.wpa_group_rekey = what == T_REKEY_GROUP ? 2 : 0;
	conf.wpa_ptk_rekey = what == T_REKEY_PAIR ? 2 : 0;
	conf.group_mgmt_cipher = WPA_CIPHER_AES_128_CMAC;
	memcpy(conf.ssid, ssid, sizeof(ssid) - 1);
	conf.ssid_len = sizeof(ssid) - 1;
	auth = wpa_init(ap_addr, &conf, &cb, NULL);
	if (auth == NULL || wpa_init_keys(auth) != 0) {
		printf("test: the authenticator did not start\n");
		return 1;
	}
	if (knl_wpa_psk(ap_pass, strlen(ap_pass), ssid, sizeof(ssid) - 1, ap_psk) != 0)
		return 1;
	ap_ie = wpa_auth_get_wpa_ie(auth, &ap_ie_len);
	if (knl_wpa_classify(ap_ie, ap_ie_len, 1) != WPAG_WPA2) {
		printf("test: the access point's element was not taken as WPA2-Personal\n");
		return 1;
	}

	if (knl_wpa_psk(sta_pass, strlen(sta_pass), ssid, sizeof(ssid) - 1, pmk) != 0)
		return 1;
	started = knl_wpa_start(sta_addr, ap_addr, ssid, sizeof(ssid) - 1, ap_ie, (int)ap_ie_len,
				pmk, ie, &ie_len) == 0;
	if (!started) {
		/* refused at the start: right only where it is to be */
		wpa_deinit(auth);
		auth = NULL;
		knl_wpa_disassoc();
		printf("test: %s: the station refused the access point\n", name);
		if (want_ok) {
			printf("FAIL %s\n", name);
			return 1;
		}
		printf("PASS %s\n", name);
		return 0;
	}
	asm_ = wpa_auth_sta_init(auth, sta_addr, NULL);
	if (asm_ == NULL || wpa_validate_wpa_ie(auth, asm_, 2412, ie, ie_len, NULL, 0, NULL, 0,
						NULL, 0, NULL) != WPA_IE_OK) {
		printf("test: the access point refused the station's element\n");
		return 1;
	}
	knl_wpa_assoc(ap_addr);
	wpa_auth_sm_event(asm_, WPA_ASSOC);
	wpa_auth_sta_associated(auth, asm_);
	run();

	ok = knl_wpa_state() == WPAG_DONE && same(&sta_ptk, &ap_ptk) && same(&sta_gtk, &ap_gtk);
	printf("test: %s: handshake %s, pairwise keys %s, group keys %s\n", name,
	       knl_wpa_state() == WPAG_DONE ? "done" : "not done",
	       same(&sta_ptk, &ap_ptk) ? "the same" : "not the same",
	       same(&sta_gtk, &ap_gtk) ? "the same" : "not the same");
	/* with message 4 lost the access point has no pairwise key yet: it comes after message 3 again */
	if (what == T_LOSE_M4 && knl_wpa_state() == WPAG_DONE) {
		/* message 3 again, after the key was in: answered, the key not installed again */
		run_for(3000);
		ok = knl_wpa_state() == WPAG_DONE && same(&sta_ptk, &ap_ptk) && sta_ptk_sets == 1;
		printf("test: %s: after message 3 came again: pairwise key installed %d time%s, keys %s\n", name,
		       sta_ptk_sets, sta_ptk_sets == 1 ? "" : "s", same(&sta_ptk, &ap_ptk) ? "the same" : "not the same");
	}
	if (ok && (what == T_REKEY_GROUP || what == T_REKEY_PAIR)) {
		/* the access point's next key comes 2 seconds after the first: one more, not two */
		before_p = sta_ptk;
		before_g = sta_gtk;
		run_for(2900);
		if (what == T_REKEY_GROUP)
			ok = knl_wpa_state() == WPAG_DONE && same(&sta_gtk, &ap_gtk) && sta_gtk_sets == 2
				&& memcmp(before_g.key, sta_gtk.key, sta_gtk.len) != 0 && same(&sta_ptk, &ap_ptk);
		else
			ok = knl_wpa_state() == WPAG_DONE && same(&sta_ptk, &ap_ptk) && sta_ptk_sets == 2
				&& memcmp(before_p.key, sta_ptk.key, sta_ptk.len) != 0;
		printf("test: %s: %s key installed %d times, the keys of both sides %s, the new one %s the old\n",
		       name, what == T_REKEY_GROUP ? "group" : "pairwise",
		       what == T_REKEY_GROUP ? sta_gtk_sets : sta_ptk_sets,
		       (what == T_REKEY_GROUP ? same(&sta_gtk, &ap_gtk) : same(&sta_ptk, &ap_ptk)) ? "the same" : "not the same",
		       memcmp(what == T_REKEY_GROUP ? before_g.key : before_p.key,
			      what == T_REKEY_GROUP ? sta_gtk.key : sta_ptk.key,
			      what == T_REKEY_GROUP ? sta_gtk.len : sta_ptk.len) != 0 ? "is not" : "is");
	}
	wpa_auth_sta_deinit(asm_);
	asm_ = NULL;
	knl_wpa_disassoc();
	wpa_deinit(auth);
	auth = NULL;
	if (ok != want_ok) {
		printf("FAIL %s\n", name);
		return 1;
	}
	printf("PASS %s\n", name);
	return 0;
}

int main(void)
{
	/* IEEE 802.11-2016 J.4.2: passphrase "password", SSID "IEEE" */
	static const u8 want[PMK_LEN] = {
		0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
		0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
	};
	static const char pass[] = "correct horse battery";
	u8 pmk[PMK_LEN];

	wpa_debug_level = getenv("WPADBG") ? MSG_DEBUG : MSG_INFO;
	if (knl_wpa_psk("password", 8, (const u8 *)"IEEE", 4, pmk) != 0 || memcmp(pmk, want, PMK_LEN) != 0) {
		printf("FAIL the pairwise master key of IEEE 802.11's test vector\n");
		failures++;
	} else {
		printf("PASS the pairwise master key of IEEE 802.11's test vector\n");
	}
	failures += one("CCMP, CCMP", WPA_CIPHER_CCMP, pass, pass, 1, T_PLAIN, NO_MGMT_FRAME_PROTECTION);
	failures += one("CCMP, TKIP group", WPA_CIPHER_TKIP, pass, pass, 1, T_PLAIN, NO_MGMT_FRAME_PROTECTION);
	failures += one("a wrong passphrase", WPA_CIPHER_CCMP, pass, "wrong horse battery", 0, T_PLAIN,
			NO_MGMT_FRAME_PROTECTION);
	failures += one("message 1 lost", WPA_CIPHER_CCMP, pass, pass, 1, T_LOSE_M1, NO_MGMT_FRAME_PROTECTION);
	failures += one("message 4 lost", WPA_CIPHER_CCMP, pass, pass, 1, T_LOSE_M4, NO_MGMT_FRAME_PROTECTION);
	failures += one("a new group key", WPA_CIPHER_CCMP, pass, pass, 1, T_REKEY_GROUP, NO_MGMT_FRAME_PROTECTION);
	failures += one("a new pairwise key", WPA_CIPHER_CCMP, pass, pass, 1, T_REKEY_PAIR, NO_MGMT_FRAME_PROTECTION);
	failures += one("management frame protection offered", WPA_CIPHER_CCMP, pass, pass, 1, T_PLAIN,
			MGMT_FRAME_PROTECTION_OPTIONAL);
	failures += one("management frame protection required", WPA_CIPHER_CCMP, pass, pass, 0, T_PLAIN,
			MGMT_FRAME_PROTECTION_REQUIRED);
	printf("test_wpa: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
	return failures ? 1 : 0;
}
