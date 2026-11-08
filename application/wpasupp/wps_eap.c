/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wps_eap.c
 *	The enterprise networks' authentication (802.1X) in the process
 *	wpasupp (docs/tessronos/09-network.md 9.6.11): the EAPOL supplicant
 *	and EAP's peer with PEAP-MSCHAPv2 and EAP-TLS. When EAP has succeeded,
 *	the state machine of the 4-way handshake takes the PMK from here
 *	(wpa_sm_set_eapol), as for any network; the rest is as in wpa_glue.c.
 *
 *	The identity, the password and the certificates are handed by the
 *	kernel for one join (knl_wpa_eap_set), kept in memory for that join,
 *	and cleared when it ends. The certificates reach the TLS layer as
 *	blobs: nothing is read from a file.
 */

#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "eapol_supp/eapol_supp_sm.h"
#include "eap_peer/eap.h"
#include "eap_peer/eap_config.h"
#include "eap_peer/eap_methods.h"
#include "wpa_glue.h"
#include "rsn_supp/wpa.h"

#define BLOB_CA		"ca"
#define BLOB_CERT	"cert"
#define BLOB_KEY	"key"

static struct eapol_sm		*esm;
static struct eap_peer_config	cfg;
static struct wpa_config_blob	blobs[3];
static int			eap_result;	/* WPAG_EAP_* */
static int			registered;

/* Sent to the access point by the kernel, as the frames of the handshake are */
int knl_wpa_eap_send(const unsigned char *dest, const unsigned char *buf, int len);
const unsigned char *knl_wpa_cur_bssid(void);

static int e_send(void *ctx, int type, const u8 *buf, size_t len)
{
	u8 *f = os_malloc(4 + len);
	int r;

	(void)ctx;
	if (f == NULL)
		return -1;
	f[0] = 2;			/* 802.1X-2004 */
	f[1] = (u8)type;
	f[2] = (u8)(len >> 8);
	f[3] = (u8)len;
	if (len > 0)
		os_memcpy(f + 4, buf, len);
	r = knl_wpa_eap_send(knl_wpa_cur_bssid(), f, (int)(4 + len));
	os_free(f);
	return r;
}

static const struct wpa_config_blob *e_blob(void *ctx, const char *name)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < ARRAY_SIZE(blobs); i++) {
		if (blobs[i].name != NULL && os_strcmp(blobs[i].name, name) == 0)
			return &blobs[i];
	}
	return NULL;
}

static void e_cb(struct eapol_sm *s, enum eapol_supp_result result, void *ctx)
{
	(void)s;
	(void)ctx;
	if (result == EAPOL_SUPP_RESULT_SUCCESS)
		eap_result = WPAG_EAP_OK;
	else if (result == EAPOL_SUPP_RESULT_FAILURE)
		eap_result = WPAG_EAP_FAILED;
}

static void clear_str(u8 **p, size_t *plen)
{
	if (*p != NULL) {
		size_t n = plen != NULL ? *plen : os_strlen((char *)*p);

		forced_memzero(*p, n);
		os_free(*p);
		*p = NULL;
	}
	if (plen != NULL)
		*plen = 0;
}

static void cfg_clear(void)
{
	size_t i;

	clear_str(&cfg.identity, &cfg.identity_len);
	clear_str(&cfg.password, &cfg.password_len);
	os_free(cfg.eap_methods);
	os_free(cfg.cert.ca_cert);
	os_free(cfg.cert.client_cert);
	os_free(cfg.cert.private_key);
	os_free(cfg.phase2);
	os_free(cfg.cert.domain_match);
	if (cfg.cert.private_key_passwd != NULL)
		clear_str((u8 **)&cfg.cert.private_key_passwd, NULL);
	os_memset(&cfg, 0, sizeof(cfg));
	for (i = 0; i < ARRAY_SIZE(blobs); i++) {
		if (blobs[i].data != NULL) {
			forced_memzero(blobs[i].data, blobs[i].len);
			os_free(blobs[i].data);
		}
		os_memset(&blobs[i], 0, sizeof(blobs[i]));
	}
}

static int blob_put(int i, const char *name, const unsigned char *d, int len)
{
	blobs[i].name = (char *)name;
	blobs[i].data = os_malloc(len);
	if (blobs[i].data == NULL)
		return -1;
	os_memcpy(blobs[i].data, d, len);
	blobs[i].len = len;
	return 0;
}

static char *dup_len(const unsigned char *s, int n)
{
	char *r = os_malloc(n + 1);

	if (r != NULL) {
		os_memcpy(r, s, n);
		r[n] = 0;
	}
	return r;
}

/* The state machine is made once; it stays for the process's life, with no configuration between joins */
static struct eapol_sm *esm_get(void)
{
	static struct eapol_ctx c;

	if (esm != NULL)
		return esm;
	if (!registered) {
		if (eap_peer_mschapv2_register() != 0 || eap_peer_tls_register() != 0
		    || eap_peer_peap_register() != 0)
			return NULL;
		registered = 1;
	}
	os_memset(&c, 0, sizeof(c));
	c.cb = e_cb;
	c.eapol_send = e_send;
	c.get_config_blob = e_blob;
	esm = eapol_sm_init(&c);
	return esm;
}

struct eapol_sm *knl_wpa_eap_sm(void)
{
	return esm_get();
}

int knl_wpa_eap_set(int method, const char *id, int id_len, const char *pw, int pw_len,
		    const unsigned char *ca, int ca_len, const char *domain, int domain_len,
		    const unsigned char *cert, int cert_len, const unsigned char *key, int key_len,
		    const char *key_pw, int key_pw_len)
{
	static struct eapol_config ec;

	if (esm_get() == NULL || id_len <= 0 || ca == NULL || ca_len <= 0)
		return -1;
	eapol_sm_notify_portEnabled(esm, false);
	eapol_sm_notify_config(esm, NULL, NULL);
	cfg_clear();
	eap_result = WPAG_EAP_NONE;

	cfg.fragment_size = 1398;	/* what fits a frame, as is usual */
	cfg.identity = (u8 *)dup_len((const u8 *)id, id_len);
	cfg.identity_len = id_len;
	cfg.eap_methods = os_calloc(2, sizeof(*cfg.eap_methods));
	if (cfg.identity == NULL || cfg.eap_methods == NULL)
		goto bad;
	cfg.eap_methods[0].vendor = EAP_VENDOR_IETF;
	cfg.eap_methods[1].vendor = EAP_VENDOR_IETF;
	cfg.eap_methods[1].method = EAP_TYPE_NONE;

	/* the server is always checked: a network that is not who it says would get the password */
	if (blob_put(0, BLOB_CA, ca, ca_len) != 0)
		goto bad;
	cfg.cert.ca_cert = (char *)os_strdup("blob://" BLOB_CA);
	if (domain != NULL && domain_len > 0)
		cfg.cert.domain_match = dup_len((const u8 *)domain, domain_len);

	if (method == WPAG_EAP_PEAP) {
		if (pw == NULL || pw_len <= 0)
			goto bad;
		cfg.eap_methods[0].method = EAP_TYPE_PEAP;
		cfg.password = (u8 *)dup_len((const u8 *)pw, pw_len);
		cfg.password_len = pw_len;
		cfg.phase2 = os_strdup("auth=MSCHAPV2");
		if (cfg.password == NULL || cfg.phase2 == NULL)
			goto bad;
	} else if (method == WPAG_EAP_TLS) {
		if (cert == NULL || cert_len <= 0 || key == NULL || key_len <= 0)
			goto bad;
		cfg.eap_methods[0].method = EAP_TYPE_TLS;
		if (blob_put(1, BLOB_CERT, cert, cert_len) != 0
		    || blob_put(2, BLOB_KEY, key, key_len) != 0)
			goto bad;
		cfg.cert.client_cert = os_strdup("blob://" BLOB_CERT);
		cfg.cert.private_key = os_strdup("blob://" BLOB_KEY);
		if (key_pw != NULL && key_pw_len > 0)
			cfg.cert.private_key_passwd = dup_len((const u8 *)key_pw, key_pw_len);
	} else {
		goto bad;
	}
	os_memset(&ec, 0, sizeof(ec));
	ec.fast_reauth = 0;
	ec.workaround = (unsigned int)-1;
	eapol_sm_notify_config(esm, &cfg, &ec);
	return 0;

    bad:
	cfg_clear();
	return -1;
}

/* Associated: the supplicant starts */
void knl_wpa_eap_assoc(void)
{
	if (esm == NULL || cfg.identity == NULL)
		return;
	eap_result = WPAG_EAP_NONE;
	eapol_sm_notify_portValid(esm, false);
	eapol_sm_notify_portEnabled(esm, true);
}

/* A frame that is not of the key: for the supplicant */
int knl_wpa_eap_rx(const unsigned char *src, const unsigned char *body, int len)
{
	if (esm == NULL || cfg.identity == NULL || len < 4)
		return 0;
	if (body[1] == 3)
		return 0;		/* EAPOL-Key: the handshake's */
	(void)eapol_sm_rx_eapol(esm, src, body, (size_t)len, FRAME_ENCRYPTION_UNKNOWN);
	return 1;
}

int knl_wpa_eap_result(void)
{
	return eap_result;
}

/* The join is over: nothing secret stays */
void knl_wpa_eap_end(void)
{
	if (esm != NULL) {
		eapol_sm_notify_portEnabled(esm, false);
		eapol_sm_notify_config(esm, NULL, NULL);
	}
	if (cfg.identity != NULL || blobs[0].data != NULL)
		cfg_clear();
	eap_result = WPAG_EAP_NONE;
}

/*
 * Pre-authentication and a directory of CA certificates are not used here;
 * with IEEE8021X_EAPOL the state machine and OpenSSL's store still name them.
 */
struct wpa_sm;
void rsn_preauth_deinit(struct wpa_sm *sm)
{
	(void)sm;
}

void rsn_preauth_candidate_process(struct wpa_sm *sm)
{
	(void)sm;
}

void *X509_LOOKUP_hash_dir(void)
{
	return NULL;
}
