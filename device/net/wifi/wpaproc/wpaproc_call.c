/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpaproc_call.c (x64 PC, Raspberry Pi 5)
 *	The functions of wpa_glue.h on the kernel's side when wpa_supplicant
 *	runs in its process (design 9.6.11, WPA_MODE=proc): each makes its
 *	arguments into a CALL, hands it to the process, carries out the
 *	calls back into the kernel the process makes while it answers (the
 *	EAPOL frames out, the keys into the card, the words to the console)
 *	in the caller's own task, and takes the outputs out of the RET.
 *
 *	The way to the process is the transport's (wpaproc.c in the kernel,
 *	the host's test a loop into the process's side in the same
 *	program): wpp_k_begin takes the one call there may be at a time and
 *	says whether the process is there, wpp_k_xchg hands a message over
 *	and returns the process's next, wpp_k_end lets the next call in.
 *	When the process is not there, or does not answer in time, a call
 *	fails as a handshake that did not end would (-1, WPAG_SAE_FAIL,
 *	WPAG_NONE).
 *
 *	Plain C: the kernel and the host's test build it alike.
 */

#include "wpa_glue.h"
#include "wpaproc.h"

extern int	wpp_k_begin( void );
extern int	wpp_k_xchg( unsigned char *buf, int len );
extern void	wpp_k_end( void );
extern int	wpp_cb_run( const unsigned char *in, int n, unsigned char *out );

/* The messages of the call there is: the one being made and the callbacks' answers */
static unsigned char	wpp_buf[WPP_MAX] __attribute__((aligned(8)));
static unsigned char	wpp_cb[WPP_MAX] __attribute__((aligned(8)));

/* A call back into the kernel carried out: its answer into out, its length returned */
int wpp_cb_run( const unsigned char *in, int n, unsigned char *out )
{
	WPP_R	r;
	WPP_W	w;
	const WPP_HDR *h = wpp_r_begin(&r, in, n, WPP_CB);
	const unsigned char *a, *b, *c;
	int	la, lb, lc, i1, i2, i3, ret = -1;
	char	line[256];

	if ( h == 0 ) return 0;
	switch ( h->op ) {
	case WPP_CB_EAPOL_TX:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		b = wpp_r_bytes(&r, &lb, WPP_MAX);
		if ( !r.bad && a != 0 && la == WPP_ADDR_LEN && b != 0 ) ret = knl_wifi_eapol_tx(a, b, lb);
		break;
	case WPP_CB_SETKEY:
		i1 = wpp_r_int(&r);
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		i2 = wpp_r_int(&r);
		i3 = wpp_r_int(&r);
		b = wpp_r_bytes(&r, &lb, 32);
		c = wpp_r_bytes(&r, &lc, 64);
		if ( !r.bad && ( a == 0 || la == WPP_ADDR_LEN ) ) {
			ret = knl_wifi_setkey(i1, a, i2, i3, b, lb, c, lc);
		}
		break;
	case WPP_CB_DELKEY:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		i1 = wpp_r_int(&r);
		if ( !r.bad && ( a == 0 || la == WPP_ADDR_LEN ) ) ret = knl_wifi_delkey(a, i1);
		break;
	case WPP_CB_DEAUTH:
		i1 = wpp_r_int(&r);
		if ( !r.bad ) {
			knl_wifi_deauth(i1);
			ret = 0;
		}
		break;
	case WPP_CB_SAY:
		a = wpp_r_bytes(&r, &la, (int)sizeof(line) - 1);
		if ( !r.bad && a != 0 ) {
			for ( i1 = 0; i1 < la; i1++ ) line[i1] = (char)a[i1];
			line[la] = '\0';
			knl_wifi_say(line);
			ret = 0;
		}
		break;
	default:
		break;
	}
	wpp_w_begin(&w, out, WPP_CBRET, h->op, ret);
	return wpp_w_end(&w);
}

/*
 * The CALL in wpp_buf (n bytes) made: the process's answer read back
 * into wpp_buf, the callbacks carried out on the way. The RET's header,
 * or NULL when there is none (a reader set up on it in *r)
 */
static const WPP_HDR *call( int op, int n, WPP_R *r )
{
	const WPP_HDR *h;
	int	m;

	if ( n <= 0 ) return 0;
	for ( ;; ) {
		n = wpp_k_xchg(wpp_buf, n);
		if ( n <= 0 ) return 0;
		h = (const WPP_HDR *)(void *)wpp_buf;
		if ( n >= (int)sizeof(WPP_HDR) && h->magic == WPP_MAGIC && h->kind == WPP_CB ) {
			m = wpp_cb_run(wpp_buf, n, wpp_cb);
			wpp_wipe(wpp_buf, n);
			if ( m <= 0 ) return 0;
			for ( n = 0; n < m; n++ ) wpp_buf[n] = wpp_cb[n];
			wpp_wipe(wpp_cb, m);
			continue;
		}
		h = wpp_r_begin(r, wpp_buf, n, WPP_RET);
		return ( h != 0 && h->op == op ) ? h : 0;
	}
}

/* The call ended: what it left in the buffers wiped, the next call let in */
static void done( void )
{
	wpp_wipe(wpp_buf, sizeof(wpp_buf));
	wpp_k_end();
}

/* A call's start: its header written, or 0 when the process is not there */
static int begin( WPP_W *w, int op )
{
	if ( wpp_k_begin() != 0 ) return 0;
	wpp_w_begin(w, wpp_buf, WPP_CALL, op, 0);
	return 1;
}

/*
 * The kind of a network from its elements, while the process is not
 * there, read as the process reads it (wpa_glue.c): the key managements
 * of the RSN element suite by suite (PSK 2, 4, 6, 19; SAE 8, 9, 24, 25;
 * anything else, or an element cut short, not WPA-Personal), so that the
 * list of the scan does not call a network open because the process was
 * away
 */
static int classify_here( const unsigned char *ies, int ies_len, int privacy )
{
	const unsigned char *p = ies, *end = ies + ies_len, *e, *q;
	int	wpa1 = 0, psk, sae, n;

	while ( ies != 0 && end - p >= 2 && p + 2 + p[1] <= end ) {
		e = p + 2 + p[1];
		if ( p[0] == 48 ) {
			psk = sae = 0;
			q = p + 2;
			if ( e - q < 2 + 4 ) return WPAG_8021X;
			q += 2 + 4;				/* the version, the group cipher */
			if ( e - q < 2 ) return WPAG_8021X;
			n = q[0] | q[1] << 8;			/* the pairwise ciphers */
			if ( e - q < 2 + 4 * n ) return WPAG_8021X;
			q += 2 + 4 * n;
			if ( e - q < 2 ) return WPAG_8021X;
			n = q[0] | q[1] << 8;
			for ( q += 2; n > 0 && e - q >= 4; n--, q += 4 ) {
				if ( q[0] != 0x00 || q[1] != 0x0f || q[2] != 0xac ) continue;
				if ( q[3] == 2 || q[3] == 4 || q[3] == 6 || q[3] == 19 ) psk = 1;
				if ( q[3] == 8 || q[3] == 9 || q[3] == 24 || q[3] == 25 ) sae = 1;
			}
			if ( psk && sae ) return WPAG_WPA2WPA3;
			if ( psk ) return WPAG_WPA2;
			if ( sae ) return WPAG_WPA3;
			return WPAG_8021X;
		}
		if ( p[0] == 221 && p[1] >= 4 && p[2] == 0x00 && p[3] == 0x50 && p[4] == 0xf2 && p[5] == 1 ) {
			wpa1 = 1;
		}
		p = e;
	}
	if ( wpa1 ) return WPAG_WPA;
	return privacy ? WPAG_WEP : WPAG_OPEN;
}

int knl_wpa_classify( const unsigned char *ies, int ies_len, int privacy )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret;

	if ( !begin(&w, WPP_CLASSIFY) ) return classify_here(ies, ies_len, privacy);
	wpp_w_bytes(&w, ies, ies_len);
	wpp_w_int(&w, privacy);
	h = call(WPP_CLASSIFY, wpp_w_end(&w), &r);
	ret = ( h != 0 ) ? h->ret : classify_here(ies, ies_len, privacy);
	done();
	return ret;
}

int knl_wpa_psk( const char *pass, int pass_len, const unsigned char *ssid,
		 int ssid_len, unsigned char *pmk )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, len = 0;

	if ( !begin(&w, WPP_PSK) ) return -1;
	wpp_w_bytes(&w, pass, pass_len);
	wpp_w_bytes(&w, ssid, ssid_len);
	h = call(WPP_PSK, wpp_w_end(&w), &r);
	if ( h != 0 && h->ret == 0 ) {
		wpp_r_copy(&r, pmk, &len, WPP_PMK_LEN);
		if ( !r.bad && len == WPP_PMK_LEN ) ret = 0;
	}
	done();
	return ret;
}

int knl_wpa_start( const unsigned char *own, const unsigned char *bssid,
		   const unsigned char *ssid, int ssid_len,
		   const unsigned char *ap_ie, int ap_ie_len,
		   const unsigned char *pmk, unsigned char *ie, int *ie_len )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, len = 0;

	if ( !begin(&w, WPP_START) ) return -1;
	wpp_w_bytes(&w, own, WPP_ADDR_LEN);
	wpp_w_bytes(&w, bssid, WPP_ADDR_LEN);
	wpp_w_bytes(&w, ssid, ssid_len);
	wpp_w_bytes(&w, ap_ie, ap_ie_len);
	wpp_w_bytes(&w, pmk, WPP_PMK_LEN);
	h = call(WPP_START, wpp_w_end(&w), &r);
	if ( h != 0 && h->ret == 0 ) {
		wpp_r_copy(&r, ie, &len, WPP_START_IE_MAX);
		if ( !r.bad ) {
			*ie_len = len;
			ret = 0;
		}
	}
	done();
	return ret;
}

int knl_wpa_choose( int saved, const unsigned char *ap_rsn, int ap_rsn_len,
		    const unsigned char *ap_rsnxe, int ap_rsnxe_len, int card_pmf,
		    int have_pt, int *akm, int *pmf, int *h2e )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, a, p, x;

	if ( !begin(&w, WPP_CHOOSE) ) return -1;
	wpp_w_int(&w, saved);
	wpp_w_bytes(&w, ap_rsn, ap_rsn_len);
	wpp_w_bytes(&w, ap_rsnxe, ap_rsnxe_len);
	wpp_w_int(&w, card_pmf);
	wpp_w_int(&w, have_pt);
	h = call(WPP_CHOOSE, wpp_w_end(&w), &r);
	if ( h != 0 ) {
		a = wpp_r_int(&r);
		p = wpp_r_int(&r);
		x = wpp_r_int(&r);
		if ( !r.bad ) {
			if ( akm != 0 ) *akm = a;
			if ( pmf != 0 ) *pmf = p;
			if ( h2e != 0 ) *h2e = x;
			ret = h->ret;
		}
	}
	done();
	return ret;
}

int knl_wpa_start2( const unsigned char *own, const unsigned char *bssid,
		    const unsigned char *ssid, int ssid_len,
		    const unsigned char *ap_rsn, int ap_rsn_len,
		    const unsigned char *ap_rsnxe, int ap_rsnxe_len, int akm, int pmf,
		    const unsigned char *pmk, const unsigned char *pmkid, int h2e,
		    unsigned char *ie, int *ie_len )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, len = 0;

	if ( !begin(&w, WPP_START2) ) return -1;
	wpp_w_bytes(&w, own, WPP_ADDR_LEN);
	wpp_w_bytes(&w, bssid, WPP_ADDR_LEN);
	wpp_w_bytes(&w, ssid, ssid_len);
	wpp_w_bytes(&w, ap_rsn, ap_rsn_len);
	wpp_w_bytes(&w, ap_rsnxe, ap_rsnxe_len);
	wpp_w_int(&w, akm);
	wpp_w_int(&w, pmf);
	wpp_w_bytes(&w, pmk, WPP_PMK_LEN);
	wpp_w_bytes(&w, pmkid, WPP_PMKID_LEN);
	wpp_w_int(&w, h2e);
	h = call(WPP_START2, wpp_w_end(&w), &r);
	if ( h != 0 && h->ret == 0 ) {
		wpp_r_copy(&r, ie, &len, WPAG_IE_MAX);
		if ( !r.bad ) {
			*ie_len = len;
			ret = 0;
		}
	}
	done();
	return ret;
}

int knl_wpa_sae_pt( const unsigned char *ssid, int ssid_len, const char *pass, int pass_len,
		    unsigned char *pt, int *pt_len )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, len = 0;

	if ( !begin(&w, WPP_SAE_PT) ) return -1;
	wpp_w_bytes(&w, ssid, ssid_len);
	wpp_w_bytes(&w, pass, pass_len);
	h = call(WPP_SAE_PT, wpp_w_end(&w), &r);
	if ( h != 0 && h->ret == 0 ) {
		wpp_r_copy(&r, pt, &len, WPAG_SAE_PT_MAX);
		if ( !r.bad ) {
			*pt_len = len;
			ret = 0;
		}
	}
	done();
	return ret;
}

/* The answer of a call that sends a frame of SAE: the frame, its number and its status */
static int sae_answer( const WPP_HDR *h, WPP_R *r, unsigned char *out, int *out_len,
		       int *out_seq, int *out_status, int fail )
{
	int	len = 0, seq, st;

	if ( h == 0 ) return fail;
	wpp_r_copy(r, out, &len, WPAG_SAE_MAX);
	seq = wpp_r_int(r);
	st = wpp_r_int(r);
	if ( r->bad ) return fail;
	if ( out_len != 0 ) *out_len = len;
	if ( out_seq != 0 ) *out_seq = seq;
	if ( out_status != 0 ) *out_status = st;
	return h->ret;
}

int knl_wpa_sae_start( const unsigned char *own, const unsigned char *bssid, int h2e,
		       const char *pass, int pass_len, const unsigned char *pt, int pt_len,
		       unsigned char *out, int *out_len, int *out_status )
{
	WPP_W	w;
	WPP_R	r;
	int	ret;

	if ( !begin(&w, WPP_SAE_START) ) return -1;
	wpp_w_bytes(&w, own, WPP_ADDR_LEN);
	wpp_w_bytes(&w, bssid, WPP_ADDR_LEN);
	wpp_w_int(&w, h2e);
	wpp_w_bytes(&w, pass, pass_len);
	wpp_w_bytes(&w, pt, pt_len);
	ret = sae_answer(call(WPP_SAE_START, wpp_w_end(&w), &r), &r, out, out_len, 0, out_status, -1);
	done();
	return ret;
}

int knl_wpa_sae_rx( int seq, int status, const unsigned char *body, int len,
		    unsigned char *out, int *out_len, int *out_seq, int *out_status )
{
	WPP_W	w;
	WPP_R	r;
	int	ret;

	if ( !begin(&w, WPP_SAE_RX) ) return WPAG_SAE_FAIL;
	wpp_w_int(&w, seq);
	wpp_w_int(&w, status);
	wpp_w_bytes(&w, body, len);
	ret = sae_answer(call(WPP_SAE_RX, wpp_w_end(&w), &r), &r, out, out_len, out_seq, out_status,
			 WPAG_SAE_FAIL);
	done();
	return ret;
}

int knl_wpa_sae_resend( unsigned char *out, int *out_len, int *out_seq, int *out_status )
{
	WPP_W	w;
	WPP_R	r;
	int	ret;

	if ( !begin(&w, WPP_SAE_RESEND) ) return WPAG_SAE_FAIL;
	ret = sae_answer(call(WPP_SAE_RESEND, wpp_w_end(&w), &r), &r, out, out_len, out_seq, out_status,
			 WPAG_SAE_FAIL);
	done();
	return ret;
}

int knl_wpa_sae_pmk( unsigned char *pmk, unsigned char *pmkid )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1, l1 = 0, l2 = 0;

	if ( !begin(&w, WPP_SAE_PMK) ) return -1;
	h = call(WPP_SAE_PMK, wpp_w_end(&w), &r);
	if ( h != 0 && h->ret == 0 ) {
		wpp_r_copy(&r, pmk, &l1, WPP_PMK_LEN);
		wpp_r_copy(&r, pmkid, &l2, WPP_PMKID_LEN);
		if ( !r.bad && l1 == WPP_PMK_LEN && l2 == WPP_PMKID_LEN ) ret = 0;
	}
	done();
	return ret;
}

/* A call without outputs */
static int plain( int op, const unsigned char *a, int alen, const unsigned char *b, int blen, int nargs )
{
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	int	ret = -1;

	if ( !begin(&w, op) ) return -1;
	if ( nargs >= 1 ) wpp_w_bytes(&w, a, alen);
	if ( nargs >= 2 ) wpp_w_bytes(&w, b, blen);
	h = call(op, wpp_w_end(&w), &r);
	if ( h != 0 ) ret = h->ret;
	done();
	return ret;
}

void knl_wpa_sae_abort( void )
{
	(void)plain(WPP_SAE_ABORT, 0, 0, 0, 0, 0);
}

void knl_wpa_assoc( const unsigned char *bssid )
{
	(void)plain(WPP_ASSOC, bssid, WPP_ADDR_LEN, 0, 0, 1);
}

void knl_wpa_rx( const unsigned char *src, const unsigned char *body, int len )
{
	(void)plain(WPP_RX, src, WPP_ADDR_LEN, body, len, 2);
}

int knl_wpa_state( void )
{
	int	r = plain(WPP_STATE, 0, 0, 0, 0, 0);

	return ( r == WPAG_HANDSHAKE || r == WPAG_DONE ) ? r : WPAG_NONE;
}

void knl_wpa_disassoc( void )
{
	(void)plain(WPP_DISASSOC, 0, 0, 0, 0, 0);
}

void knl_wpa_tick( void )
{
	(void)plain(WPP_TICK, 0, 0, 0, 0, 0);
}
