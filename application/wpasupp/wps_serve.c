/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wps_serve.c
 *	The process's side of the messages of wpaproc.h (design 9.6.11): a
 *	CALL of the kernel's Wi-Fi manager taken apart, the function of
 *	wpa_glue.c it names called with its arguments, and its outputs made
 *	into the RET. What the glue calls of the kernel while it runs (an
 *	EAPOL frame out, a key into the card or out of it, the association
 *	left, a line for the console) goes back as a CB, and its answer
 *	(CBRET) is waited for before the glue goes on.
 *
 *	The way to the kernel is the caller's: wps_p_xchg hands a CB over
 *	and returns the CBRET (the record of "wpasup" in the process, a
 *	call into the kernel's side in the host's test).
 *
 *	The messages and the outputs that held secrets (a passphrase, a
 *	password, a PMK) are wiped when the answer is made.
 */

#include <stddef.h>
#include "wpa_glue.h"
#include "wpaproc.h"

extern int	wps_p_xchg( unsigned char *buf, int len );
extern int	wps_serve( const unsigned char *in, int n, unsigned char *out );

static unsigned char	cbbuf[WPP_MAX] __attribute__((aligned(8)));

/* A CB made and its CBRET's value returned (-1 when there is none) */
static int cb( WPP_W *w, int op )
{
	WPP_R	r;
	const WPP_HDR *h;
	int	n = wpp_w_end(w), ret;

	if ( n <= 0 ) return -1;
	n = wps_p_xchg(cbbuf, n);
	h = wpp_r_begin(&r, cbbuf, n, WPP_CBRET);
	ret = ( h != 0 && h->op == op ) ? h->ret : -1;
	wpp_wipe(cbbuf, sizeof(cbbuf));
	return ret;
}

int knl_wifi_eapol_tx( const unsigned char *dst, const unsigned char *body, int len )
{
	WPP_W	w;

	wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_EAPOL_TX, 0);
	wpp_w_bytes(&w, dst, WPP_ADDR_LEN);
	wpp_w_bytes(&w, body, len);
	return cb(&w, WPP_CB_EAPOL_TX);
}

int knl_wifi_setkey( int cipher, const unsigned char *addr, int idx, int set_tx,
		     const unsigned char *seq, int seq_len,
		     const unsigned char *key, int key_len )
{
	WPP_W	w;

	wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_SETKEY, 0);
	wpp_w_int(&w, cipher);
	wpp_w_bytes(&w, addr, WPP_ADDR_LEN);
	wpp_w_int(&w, idx);
	wpp_w_int(&w, set_tx);
	wpp_w_bytes(&w, seq, seq_len);
	wpp_w_bytes(&w, key, key_len);
	return cb(&w, WPP_CB_SETKEY);
}

int knl_wifi_delkey( const unsigned char *addr, int idx )
{
	WPP_W	w;

	wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_DELKEY, 0);
	wpp_w_bytes(&w, addr, WPP_ADDR_LEN);
	wpp_w_int(&w, idx);
	return cb(&w, WPP_CB_DELKEY);
}

void knl_wifi_deauth( int reason )
{
	WPP_W	w;

	wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_DEAUTH, 0);
	wpp_w_int(&w, reason);
	(void)cb(&w, WPP_CB_DEAUTH);
}

/* A line for the console, cut to what the kernel takes */
void knl_wifi_say( const char *line )
{
	WPP_W	w;
	int	n = 0;

	while ( line[n] != '\0' && n < 254 ) n++;
	if ( n > 0 && line[n - 1] != '\n' && line[n] != '\0' ) {
		/* cut short: still a line */
		wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_SAY, 0);
		wpp_w_bytes(&w, line, n - 1);
		w.b[w.n - 1] = '\n';
		(void)cb(&w, WPP_CB_SAY);
		return;
	}
	wpp_w_begin(&w, cbbuf, WPP_CB, WPP_CB_SAY, 0);
	wpp_w_bytes(&w, line, n);
	(void)cb(&w, WPP_CB_SAY);
}

/* Outputs of the glue's functions, in the sizes the kernel's callers have */
static unsigned char	o_buf[WPAG_SAE_MAX];
static unsigned char	o_pmkid[WPP_PMKID_LEN];
static char		s_pass[128];

/* A passphrase or a password as the glue takes it: its bytes, kept with a terminating zero */
static const char *text_of( const unsigned char *p, int len )
{
	int	i;

	if ( p == NULL || len < 0 || len >= (int)sizeof(s_pass) ) return NULL;
	for ( i = 0; i < len; i++ ) s_pass[i] = (char)p[i];
	s_pass[len] = '\0';
	return s_pass;
}

static void sae_out( WPP_W *w, int len, int seq, int status )
{
	wpp_w_bytes(w, o_buf, len);
	wpp_w_int(w, seq);
	wpp_w_int(w, status);
}

/* A CALL answered: the RET's length in out, 0 for a message that is broken */
int wps_serve( const unsigned char *in, int n, unsigned char *out )
{
	WPP_R	r;
	WPP_W	w;
	const WPP_HDR *h = wpp_r_begin(&r, in, n, WPP_CALL);
	const unsigned char *a, *b, *c, *d, *e, *f;
	int	la, lb, lc, ld, le, lf, i1, i2, i3, i4, ret = -1;
	int	olen = 0, oseq = 0, ost = 0;
	const char *pw;

	if ( h == NULL ) return 0;
	switch ( h->op ) {
	case WPP_CLASSIFY:
		a = wpp_r_bytes(&r, &la, WPP_ARGS_MAX);
		i1 = wpp_r_int(&r);
		if ( r.bad ) return 0;
		ret = knl_wpa_classify(a, la, i1);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		break;
	case WPP_PSK:
		a = wpp_r_bytes(&r, &la, 64);
		b = wpp_r_bytes(&r, &lb, 32);
		pw = text_of(a, la);
		if ( r.bad || pw == NULL || b == NULL ) return 0;
		ret = knl_wpa_psk(pw, la, b, lb, o_buf);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		if ( ret == 0 ) wpp_w_bytes(&w, o_buf, WPP_PMK_LEN);
		break;
	case WPP_START:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		b = wpp_r_bytes(&r, &lb, WPP_ADDR_LEN);
		c = wpp_r_bytes(&r, &lc, 32);
		d = wpp_r_bytes(&r, &ld, 257);
		e = wpp_r_bytes(&r, &le, WPP_PMK_LEN);
		if ( r.bad || la != WPP_ADDR_LEN || lb != WPP_ADDR_LEN || c == NULL || e == NULL
		  || le != WPP_PMK_LEN ) return 0;
		ret = knl_wpa_start(a, b, c, lc, d, ld, e, o_buf, &olen);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		if ( ret == 0 ) wpp_w_bytes(&w, o_buf, olen > WPP_START_IE_MAX ? WPP_START_IE_MAX : olen);
		break;
	case WPP_CHOOSE:
		i1 = wpp_r_int(&r);
		a = wpp_r_bytes(&r, &la, 257);
		b = wpp_r_bytes(&r, &lb, 257);
		i2 = wpp_r_int(&r);
		i3 = wpp_r_int(&r);
		if ( r.bad ) return 0;
		i4 = olen = oseq = 0;
		ret = knl_wpa_choose(i1, a, la, b, lb, i2, i3, &i4, &olen, &oseq);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		wpp_w_int(&w, i4);
		wpp_w_int(&w, olen);
		wpp_w_int(&w, oseq);
		break;
	case WPP_START2:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		b = wpp_r_bytes(&r, &lb, WPP_ADDR_LEN);
		c = wpp_r_bytes(&r, &lc, 32);
		d = wpp_r_bytes(&r, &ld, 257);
		e = wpp_r_bytes(&r, &le, 257);
		i1 = wpp_r_int(&r);
		i2 = wpp_r_int(&r);
		f = wpp_r_bytes(&r, &lf, WPP_PMK_LEN);
		{
			int		lg;
			const unsigned char *g = wpp_r_bytes(&r, &lg, WPP_PMKID_LEN);

			i3 = wpp_r_int(&r);
			if ( r.bad || la != WPP_ADDR_LEN || lb != WPP_ADDR_LEN || c == NULL
			  || ( f != NULL && lf != WPP_PMK_LEN ) || ( g != NULL && lg != WPP_PMKID_LEN ) ) return 0;
			if ( g != NULL ) {
				for ( i4 = 0; i4 < WPP_PMKID_LEN; i4++ ) o_pmkid[i4] = g[i4];
			}
			ret = knl_wpa_start2(a, b, c, lc, d, ld, e, le, i1, i2, f, g != NULL ? o_pmkid : NULL,
					     i3, o_buf, &olen);
		}
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		if ( ret == 0 ) wpp_w_bytes(&w, o_buf, olen > WPAG_IE_MAX ? WPAG_IE_MAX : olen);
		break;
	case WPP_SAE_PT:
		a = wpp_r_bytes(&r, &la, 32);
		b = wpp_r_bytes(&r, &lb, 64);
		pw = text_of(b, lb);
		if ( r.bad || a == NULL || pw == NULL ) return 0;
		ret = knl_wpa_sae_pt(a, la, pw, lb, o_buf, &olen);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		if ( ret == 0 ) wpp_w_bytes(&w, o_buf, olen > WPAG_SAE_PT_MAX ? WPAG_SAE_PT_MAX : olen);
		break;
	case WPP_SAE_START:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		b = wpp_r_bytes(&r, &lb, WPP_ADDR_LEN);
		i1 = wpp_r_int(&r);
		c = wpp_r_bytes(&r, &lc, 64);
		d = wpp_r_bytes(&r, &ld, WPAG_SAE_PT_MAX);
		pw = ( c != NULL ) ? text_of(c, lc) : NULL;
		if ( r.bad || la != WPP_ADDR_LEN || lb != WPP_ADDR_LEN || ( c != NULL && pw == NULL ) ) return 0;
		ret = knl_wpa_sae_start(a, b, i1, pw, lc, d, ld, o_buf, &olen, &ost);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		sae_out(&w, ret == 0 ? olen : 0, 1, ost);
		break;
	case WPP_SAE_RX:
		i1 = wpp_r_int(&r);
		i2 = wpp_r_int(&r);
		a = wpp_r_bytes(&r, &la, WPP_ARGS_MAX);
		if ( r.bad ) return 0;
		ret = knl_wpa_sae_rx(i1, i2, a, la, o_buf, &olen, &oseq, &ost);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		sae_out(&w, ret == WPAG_SAE_SEND ? olen : 0, oseq, ost);
		break;
	case WPP_SAE_RESEND:
		ret = knl_wpa_sae_resend(o_buf, &olen, &oseq, &ost);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		sae_out(&w, ret == WPAG_SAE_SEND ? olen : 0, oseq, ost);
		break;
	case WPP_SAE_PMK:
		ret = knl_wpa_sae_pmk(o_buf, o_pmkid);
		wpp_w_begin(&w, out, WPP_RET, h->op, ret);
		if ( ret == 0 ) {
			wpp_w_bytes(&w, o_buf, WPP_PMK_LEN);
			wpp_w_bytes(&w, o_pmkid, WPP_PMKID_LEN);
		}
		break;
	case WPP_SAE_ABORT:
		knl_wpa_sae_abort();
		wpp_w_begin(&w, out, WPP_RET, h->op, 0);
		break;
	case WPP_ASSOC:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		if ( r.bad || la != WPP_ADDR_LEN ) return 0;
		knl_wpa_assoc(a);
		wpp_w_begin(&w, out, WPP_RET, h->op, 0);
		break;
	case WPP_RX:
		a = wpp_r_bytes(&r, &la, WPP_ADDR_LEN);
		b = wpp_r_bytes(&r, &lb, WPP_ARGS_MAX);
		if ( r.bad || la != WPP_ADDR_LEN || b == NULL ) return 0;
		knl_wpa_rx(a, b, lb);
		wpp_w_begin(&w, out, WPP_RET, h->op, 0);
		break;
	case WPP_STATE:
		wpp_w_begin(&w, out, WPP_RET, h->op, knl_wpa_state());
		break;
	case WPP_DISASSOC:
		knl_wpa_disassoc();
		wpp_w_begin(&w, out, WPP_RET, h->op, 0);
		break;
	case WPP_TICK:
		knl_wpa_tick();
		wpp_w_begin(&w, out, WPP_RET, h->op, 0);
		break;
	default:
		return 0;
	}
	wpp_wipe(s_pass, sizeof(s_pass));
	wpp_wipe(o_buf, sizeof(o_buf));
	wpp_wipe(o_pmkid, sizeof(o_pmkid));
	return wpp_w_end(&w);
}
