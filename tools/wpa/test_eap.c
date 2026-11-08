/*
 * tools/wpa/test_eap.c: the peer of application/wpasupp/wps_eap.c against the
 * EAP server of the same wpa_supplicant release, over EAPOL frames in memory,
 * with real TLS. PEAP-MSCHAPv2 and EAP-TLS succeed with the same key on both
 * sides; the wrong password, a server the CA did not sign, the wrong domain
 * name and a client certificate the server does not trust all fail.
 *   test_eap DIR   (DIR holds what tools/wpa/test_eap.sh makes)
 */
#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "crypto/tls.h"
#include "eapol_supp/eapol_supp_sm.h"
#include "eap_server/eap.h"
#include "eap_server/eap_methods.h"
#include "wpa_glue.h"

static u8	out_frame[70000];
static size_t	out_len;

int knl_wpa_eap_send(const unsigned char *dest, const unsigned char *buf, int len)
{
	(void)dest;
	os_memcpy(out_frame, buf, len);
	out_len = len;
	return 0;
}

static const u8 ap[6] = { 2, 0, 0, 0, 0, 1 };
const unsigned char *knl_wpa_cur_bssid(void) { return ap; }

extern struct eapol_sm *knl_wpa_eap_sm(void);

static const char *srv_pass;
static int srv_method;
static const char *dir;
static int fails;

static int srv_user(void *ctx, const u8 *id, size_t id_len, int phase2, struct eap_user *user)
{
	(void)ctx;
	(void)id;
	(void)id_len;
	os_memset(user, 0, sizeof(*user));
	user->methods[0].vendor = EAP_VENDOR_IETF;
	user->methods[1].vendor = EAP_VENDOR_IETF;
	user->methods[1].method = EAP_TYPE_NONE;
	if (phase2) {
		user->methods[0].method = EAP_TYPE_MSCHAPV2;
		user->password = (u8 *)os_strdup(srv_pass);
		user->password_len = os_strlen(srv_pass);
	} else {
		user->methods[0].method = srv_method;
		user->force_version = -1;
	}
	return 0;
}

static const char *srv_text(void *ctx, size_t *len)
{
	(void)ctx;
	*len = 0;
	return NULL;
}

static const struct eapol_callbacks srv_cb = { .get_eap_user = srv_user, .get_eap_req_id_text = srv_text };

static u8 *slurp(const char *name, int *plen)
{
	char p[300];
	u8 *b = os_malloc(70000);
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", dir, name);
	f = fopen(p, "rb");
	if (f == NULL) {
		printf("no %s\n", p);
		exit(2);
	}
	*plen = (int)fread(b, 1, 70000, f);
	fclose(f);
	return b;
}

static char *pathof(const char *name)
{
	char *p = os_malloc(300);

	snprintf(p, 300, "%s/%s", dir, name);
	return p;
}

/* One exchange to its end: 1 success with the same key both sides, 0 failure */
static int exchange(int method, const char *pw, const char *srvpw, const char *ca_file,
		    const char *domain, const char *client)
{
	struct tls_config tc;
	struct tls_connection_params tp;
	struct eap_config ec;
	struct eap_session_data sess;
	struct eap_sm *srv;
	struct eap_eapol_interface *i;
	void *ssl;
	u8 *ca, *cert = NULL, *key = NULL, pmk[32];
	int ca_len, cert_len = 0, key_len = 0, r, rounds, ok = 0;
	char cn[40], kn[40];

	srv_pass = srvpw;
	srv_method = method == WPAG_EAP_PEAP ? EAP_TYPE_PEAP : EAP_TYPE_TLS;
	os_memset(&tc, 0, sizeof(tc));
	ssl = tls_init(&tc);
	os_memset(&tp, 0, sizeof(tp));
	tp.ca_cert = pathof("ca.pem");
	tp.client_cert = pathof("server.pem");
	tp.private_key = pathof("server.key");
	if (ssl == NULL || tls_global_set_params(ssl, &tp) != 0 || tls_global_set_verify(ssl, 0, 1) != 0) {
		printf("server's TLS not ready\n");
		exit(2);
	}
	os_memset(&ec, 0, sizeof(ec));
	os_memset(&sess, 0, sizeof(sess));
	ec.eap_server = 1;
	ec.max_auth_rounds = 100;
	ec.max_auth_rounds_short = 50;
	ec.ssl_ctx = ssl;
	srv = eap_server_sm_init(NULL, &srv_cb, &ec, &sess);
	if (srv == NULL) {
		printf("no server\n");
		exit(2);
	}
	i = eap_get_interface(srv);
	i->portEnabled = true;
	i->eapRestart = true;

	ca = slurp(ca_file, &ca_len);
	if (client != NULL) {
		snprintf(cn, sizeof(cn), "%s.pem", client);
		snprintf(kn, sizeof(kn), "%s.key", client);
		cert = slurp(cn, &cert_len);
		key = slurp(kn, &key_len);
	}
	r = knl_wpa_eap_set(method, "user@example", 12, pw, pw != NULL ? (int)os_strlen(pw) : 0,
			    ca, ca_len, domain, domain != NULL ? (int)os_strlen(domain) : 0,
			    cert, cert_len, key, key_len, NULL, 0);
	if (r != 0) {
		printf("knl_wpa_eap_set: %d\n", r);
		fails++;
		return 0;
	}
	out_len = 0;
	knl_wpa_eap_assoc();
	for (rounds = 0; rounds < 200; rounds++) {
		while (eap_server_sm_step(srv))
			;
		if (i->eapReq || i->eapSuccess || i->eapFail) {
			if (i->eapReqData != NULL) {
				static u8 f[70000];
				size_t n = wpabuf_len(i->eapReqData);

				f[0] = 2;
				f[1] = 0;
				f[2] = (u8)(n >> 8);
				f[3] = (u8)n;
				os_memcpy(f + 4, wpabuf_head(i->eapReqData), n);
				i->eapReq = false;
				out_len = 0;
				knl_wpa_eap_rx(ap, f, (int)(4 + n));
			}
			if (i->eapSuccess || i->eapFail)
				break;
		}
		if (out_len == 0)
			continue;
		if (out_frame[1] == 0) {
			wpabuf_free(i->eapRespData);
			i->eapRespData = wpabuf_alloc_copy(out_frame + 4, out_len - 4);
			i->eapResp = true;
		}
		out_len = 0;
	}
	if (i->eapSuccess && i->eapKeyData != NULL && i->eapKeyDataLen >= 32
	    && eapol_sm_get_key(knl_wpa_eap_sm(), pmk, 32) == 0) {
		ok = os_memcmp(pmk, i->eapKeyData, 32) == 0;
		if (!ok)
			printf("the keys differ\n");
	}
	knl_wpa_eap_end();
	eap_server_sm_deinit(srv);
	tls_deinit(ssl);
	os_free(ca);
	os_free(cert);
	os_free(key);
	return ok;
}

#define WANT(name, want, call) do { int r_ = (call); \
	printf("%s: %s: %s\n", (r_ == (want)) ? "ok" : "FAIL", name, r_ ? "succeeded" : "failed"); \
	if (r_ != (want)) fails++; } while (0)

int main(int ac, char **av)
{
	if (ac < 2 || eloop_init() != 0)
		return 2;
	dir = av[1];
	wpa_debug_level = getenv("EAPDBG") ? MSG_DEBUG : MSG_ERROR;
	if (eap_server_identity_register() != 0 || eap_server_mschapv2_register() != 0
	    || eap_server_peap_register() != 0 || eap_server_tls_register() != 0)
		return 2;

	WANT("PEAP-MSCHAPv2", 1, exchange(WPAG_EAP_PEAP, "secret pw", "secret pw", "ca.pem", NULL, NULL));
	WANT("PEAP, the domain right", 1, exchange(WPAG_EAP_PEAP, "secret pw", "secret pw", "ca.pem", "radius.example", NULL));
	WANT("PEAP, the wrong password", 0, exchange(WPAG_EAP_PEAP, "wrong", "secret pw", "ca.pem", NULL, NULL));
	WANT("PEAP, a server the CA did not sign", 0, exchange(WPAG_EAP_PEAP, "secret pw", "secret pw", "other_ca.pem", NULL, NULL));
	WANT("PEAP, the wrong domain", 0, exchange(WPAG_EAP_PEAP, "secret pw", "secret pw", "ca.pem", "other.example", NULL));
	WANT("EAP-TLS", 1, exchange(WPAG_EAP_TLS, NULL, "", "ca.pem", NULL, "client"));
	WANT("EAP-TLS, a client the server does not trust", 0, exchange(WPAG_EAP_TLS, NULL, "", "ca.pem", NULL, "bad"));
	WANT("EAP-TLS, a server the CA did not sign", 0, exchange(WPAG_EAP_TLS, NULL, "", "other_ca.pem", NULL, "client"));
	printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
	return fails != 0;
}
