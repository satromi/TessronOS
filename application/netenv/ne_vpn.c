/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ne_vpn.c
 *	The sheet 〈ＶＰＮ〉 of ネットワーク設定 (make WG=1, included by netenv.c,
 *	docs/tessronos/09-network.md). One WireGuard tunnel for each person:
 *	their private key, sealed, with what the tunnel goes through.
 *
 *	Two objects of the person's, both rw-------: ＶＰＮの鍵, a random key
 *	in its record 1, and ＶＰＮの設定, in whose record 1 the private key and
 *	the preshared key are sealed with it by AES-256-GCM. No link points at
 *	them: a hold (ob_lnk_obj) keeps them, and they are found among all
 *	objects by owner and by the mark in their metadata.
 *	So the settings can be read from a copy without giving up the keys,
 *	another person's programs reach neither, and whoever has the whole disk
 *	has both. The kernel is handed the keys for each connection (device
 *	object wg0, so_wg.c) and holds them only while the tunnel is up.
 */

#include <mbedtls/gcm.h>
#include <mbedtls/ecp.h>
#include <mbedtls/base64.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/soapp.h>

#define PV_PRIV		110
#define PV_GEN		111
#define PV_PUB		112
#define PV_ADDR		113
#define PV_PEER		114
#define PV_PSK		115
#define PV_ENDPOINT	116
#define PV_ALLOWED	117
#define PV_SAVE		118
#define PV_UP		119
#define PV_DOWN		120
#define PV_STATE	121

#define V_KEY		32
#define V_B64		45		/* 32 bytes in base64, with its end */
#define V_AAD		"TESSRONOS-VPN1"
#define V_NONCE		12
#define V_TAG		16

extern int mbedtls_hardware_poll( void *data, unsigned char *output, size_t len, size_t *olen );

LOCAL TS_UUID	v_dev;
LOCAL BOOL	v_have;

LOCAL int v_rng( void *ctx, unsigned char *out, size_t n )
{
	size_t	got = 0;

	(void)ctx;
	return mbedtls_hardware_poll(NULL, out, n, &got);
}

LOCAL BOOL v_find( void )
{
	TS_UUID	*ids = NULL;
	T_OBREF	r;
	INT	cnt, i;

	v_have = FALSE;
	cnt = ui_lst_obj(OB_T_DEVICE, OB_S_NET, &ids);
	for ( i = 0; i < cnt && !v_have; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK && strcmp((CONST char *)r.name, "wg0") == 0 ) {
			v_dev = ids[i];
			v_have = TRUE;
		}
	}
	free(ids);
	return v_have;
}

LOCAL ER v_cmd( CONST char *c )
{
	ID	key = ob_opn_obj(&v_dev, OB_OP_R | OB_OP_WRITE);
	SZ	asz = 0;
	ER	er;

	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, OB_NET_STATE, 0, c, (SZ)strlen(c), &asz);
	ob_cls_obj(key);
	return er;
}

LOCAL void v_hex( CONST UB *b, INT n, char *out )
{
	static CONST char	d[] = "0123456789abcdef";
	INT			i;

	for ( i = 0; i < n; i++ ) {
		*out++ = d[b[i] >> 4];
		*out++ = d[b[i] & 15];
	}
	*out = 0;
}

/* What is kept: a mark in the metadata (tessronos.netenv), and the object's name */
#define V_KIND_KEY	"vpnkey"
#define V_KIND_CONF	"vpnconf"

/* Whether u carries the mark kind */
LOCAL BOOL v_marked( CONST TS_UUID *u, CONST char *kind )
{
	T_JSON	root, tf, ne;
	UB	*meta = NULL;
	INT	len;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	yes = FALSE;

	if ( key <= 0 ) return FALSE;
	len = ob_atr_read(key, 0, &meta);
	if ( len >= 0 && js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "netenv", &ne) >= E_OK ) {
		yes = js_get_bool(&ne, kind, FALSE);
	}
	free(meta);
	ob_cls_obj(key);
	return yes;
}

/*
 * The person's object of a kind. E_NOEXS when there is none, E_OACV when it
 * can be reached by others: a key that has been open to anyone is not used.
 */
LOCAL ER v_find_obj( CONST char *kind, TS_UUID *p_u )
{
	T_OBCRD	me;
	UB	pbuf[OB_PRT_SIZE(16, 4)];
	T_OBPRT	*prt = (T_OBPRT *)pbuf;
	TS_UUID	*ids = NULL;
	SZ	asz = 0;
	INT	n, i;
	ER	er = E_NOEXS;

	if ( ob_get_crd(&me) < E_OK ) return E_SYS;
	n = ui_lst_obj(OB_T_STORAGE, OB_S_FILE, &ids);
	for ( i = 0; i < n && er == E_NOEXS; i++ ) {
		if ( ob_get_prt(&ids[i], prt, sizeof(pbuf), &asz) < E_OK
		  || memcmp(&prt->owner, &me.user, sizeof(TS_UUID)) != 0
		  || !v_marked(&ids[i], kind) ) {
			continue;
		}
		*p_u = ids[i];
		er = ( ( prt->mode & 0077 ) != 0 ) ? E_OACV : E_OK;
	}
	free(ids);
	return er;
}

/* A new object of the person's, theirs alone from before anything is in it */
LOCAL ER v_make_obj( CONST char *kind, TS_UUID *p_u )
{
	static CONST char	text[] = "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>"
				"ネットワーク設定のＶＰＮのデータです。持ち主だけが読み書きできます。"
				"</p>\n</document>\n</tad>\n";
	char	meta[320];
	T_OBCRD	me;
	T_OBPRT	prt;
	T_OBCRE	cr;
	TS_UUID	near;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( ob_get_crd(&me) < E_OK || ui_uuid(SYSDEF_PROG_NETENV, &near) < E_OK ) return E_SYS;
	snprintf(meta, sizeof(meta),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},"
		 "\"tessronos\":{\"netenv\":{\"%s\":true},"
		 "\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}",
		 ( strcmp(kind, V_KIND_KEY) == 0 ) ? "ＶＰＮの鍵" : "ＶＰＮの設定", kind);
	memset(&prt, 0, sizeof(prt));
	prt.owner = me.user;
	if ( me.ngrp > 0 ) prt.group = me.grp[0];
	prt.mode = 0600;
	memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_FILE;
	cr.json = (CONST UB *)meta;
	cr.jsonsz = (SZ)strlen(meta);
	cr.near = near;
	cr.prt = &prt;
	er = ob_cre_obj(&cr, p_u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(p_u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	er = ( key > 0 ) ? E_OK : ( key < 0 ) ? (ER)key : E_SYS;
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, text, (SZ)strlen(text), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
	if ( key > 0 ) ob_cls_obj(key);
	if ( er >= E_OK ) er = ob_lnk_obj(p_u);
	if ( er < E_OK ) (void)ob_del_obj(p_u);
	return er;
}

/* Record 1 of the person's object of a kind replaced whole, the object made if need be */
LOCAL ER v_put( CONST char *kind, CONST void *b, SZ n )
{
	TS_UUID	u;
	ID	key;
	ER	er = v_find_obj(kind, &u);

	if ( er == E_NOEXS ) er = v_make_obj(kind, &u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_O_WEXCL);
	if ( key < E_OK ) return (ER)key;
	er = ob_rpl_rec(key, 1, b, n);
	ob_cls_obj(key);
	return er;
}

/* The bytes of record 1 of the person's object of a kind, or an error */
LOCAL INT v_get( CONST char *kind, void *b, SZ max )
{
	TS_UUID	u;
	SZ	asz = 0;
	ID	key;
	ER	er = v_find_obj(kind, &u);

	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) return (INT)key;
	er = ob_rea_rec(key, 1, 0, b, max, &asz);
	ob_cls_obj(key);
	return ( er < E_OK ) ? er : (INT)asz;
}

/* The person's sealing key, made the first time */
LOCAL ER v_key( UB key[V_KEY] )
{
	INT	n = v_get(V_KIND_KEY, key, V_KEY);

	if ( n == V_KEY ) return E_OK;
	if ( n == E_OACV ) return E_OACV;
	if ( v_rng(NULL, key, V_KEY) != 0 ) return E_SYS;
	return v_put(V_KIND_KEY, key, V_KEY);
}

LOCAL BOOL v_unhex( CONST char *s, UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		INT	h = w_hexval(s[i * 2]), l = ( h >= 0 ) ? w_hexval(s[i * 2 + 1]) : -1;

		if ( l < 0 ) return FALSE;
		b[i] = (UB)( h * 16 + l );
	}
	return s[n * 2] == 0;
}

/* 32 bytes sealed: nonce, text and tag in hexadecimal */
LOCAL BOOL v_seal( CONST UB key[V_KEY], CONST UB plain[V_KEY], char *out )
{
	mbedtls_gcm_context	g;
	UB			buf[V_NONCE + V_KEY + V_TAG];
	BOOL			ok;

	if ( v_rng(NULL, buf, V_NONCE) != 0 ) return FALSE;
	mbedtls_gcm_init(&g);
	ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
	  && mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, V_KEY, buf, V_NONCE, (CONST UB *)V_AAD,
				       sizeof(V_AAD) - 1, plain, buf + V_NONCE, V_TAG,
				       buf + V_NONCE + V_KEY) == 0;
	mbedtls_gcm_free(&g);
	if ( ok ) v_hex(buf, sizeof(buf), out);
	return ok;
}

LOCAL BOOL v_open( CONST UB key[V_KEY], CONST char *hex, UB plain[V_KEY] )
{
	mbedtls_gcm_context	g;
	UB			buf[V_NONCE + V_KEY + V_TAG];
	BOOL			ok;

	if ( !v_unhex(hex, buf, sizeof(buf)) ) return FALSE;
	mbedtls_gcm_init(&g);
	ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
	  && mbedtls_gcm_auth_decrypt(&g, V_KEY, buf, V_NONCE, (CONST UB *)V_AAD, sizeof(V_AAD) - 1,
				      buf + V_NONCE + V_KEY, V_TAG, buf + V_NONCE, plain) == 0;
	mbedtls_gcm_free(&g);
	return ok;
}

LOCAL BOOL v_b64_key( CONST char *s, UB out[V_KEY] )
{
	size_t	n = 0;

	return mbedtls_base64_decode(out, V_KEY, &n, (CONST UB *)s, strlen(s)) == 0 && n == V_KEY;
}

LOCAL void v_key_b64( CONST UB k[V_KEY], char *out )
{
	size_t	n = 0;

	(void)mbedtls_base64_encode((UB *)out, V_B64, &n, k, V_KEY);
	out[n] = 0;
}

/* The public key of a private one */
LOCAL BOOL v_public( CONST UB priv[V_KEY], UB pub[V_KEY] )
{
	mbedtls_ecp_group	g;
	mbedtls_mpi		d;
	mbedtls_ecp_point	q;
	BOOL			ok;

	mbedtls_ecp_group_init(&g);
	mbedtls_mpi_init(&d);
	mbedtls_ecp_point_init(&q);
	ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_CURVE25519) == 0
	  && mbedtls_mpi_read_binary_le(&d, priv, V_KEY) == 0
	  && mbedtls_ecp_mul(&g, &q, &d, &g.G, v_rng, NULL) == 0
	  && mbedtls_mpi_write_binary_le(&q.MBEDTLS_PRIVATE(X), pub, V_KEY) == 0;
	mbedtls_ecp_point_free(&q);
	mbedtls_mpi_free(&d);
	mbedtls_ecp_group_free(&g);
	return ok;
}

LOCAL void v_clamp( UB k[V_KEY] )
{
	k[0] &= 248;
	k[31] = (UB)( ( k[31] & 127 ) | 64 );
}

/* "name value" lines of the settings */
LOCAL BOOL v_line( CONST char *t, CONST char *name, char *out, INT max )
{
	INT	nl = (INT)strlen(name);

	out[0] = 0;
	while ( *t != 0 ) {
		CONST char *e = strchr(t, '\n');
		INT	len = ( e != NULL ) ? (INT)( e - t ) : (INT)strlen(t);

		if ( len > nl && strncmp(t, name, nl) == 0 && t[nl] == ' ' ) {
			len -= nl + 1;
			if ( len >= max ) return FALSE;
			memcpy(out, t + nl + 1, len);
			out[len] = 0;
			return TRUE;
		}
		t += len + ( e != NULL ? 1 : 0 );
	}
	return FALSE;
}

#define V_SEALED	( ( V_NONCE + V_KEY + V_TAG ) * 2 + 1 )
#define V_STANDIN	"（保存済み）"

LOCAL void v_state( void )
{
	static char	t[128];
	char		s[64] = "";
	ID		key;
	SZ		asz = 0;

	if ( !v_have ) {
		ui_set_text(&ui, PV_STATE, "");
		return;
	}
	key = ob_opn_obj(&v_dev, OB_OP_R);
	if ( key < E_OK ) return;
	(void)ob_rea_rec(key, OB_NET_STATE, 0, t, sizeof(t) - 1, &asz);
	ob_cls_obj(key);
	t[asz] = 0;
	(void)v_line(t, "STATE", s, sizeof(s));
	ui_set_text(&ui, PV_STATE, ( strcmp(s, "up") == 0 ) ? "接続中" : "切断");
}

/* The settings into the parts; a key saved shows as its public half, and marks stand for it */
LOCAL void v_load( void )
{
	static char	t[1024];
	char		v[256];
	UB		key[V_KEY], priv[V_KEY], pub[V_KEY];
	INT		n;

	n = v_get(V_KIND_CONF, t, sizeof(t) - 1);
	if ( n <= 0 ) return;
	t[n] = 0;
	if ( v_line(t, "address", v, sizeof(v)) ) ui_set_text(&ui, PV_ADDR, v);
	if ( v_line(t, "peer", v, sizeof(v)) ) ui_set_text(&ui, PV_PEER, v);
	if ( v_line(t, "endpoint", v, sizeof(v)) ) ui_set_text(&ui, PV_ENDPOINT, v);
	if ( v_line(t, "allowed", v, sizeof(v)) ) ui_set_text(&ui, PV_ALLOWED, v);
	if ( v_line(t, "psk", v, sizeof(v)) ) ui_set_text(&ui, PV_PSK, V_STANDIN);
	if ( v_line(t, "private", v, sizeof(v)) && v_key(key) >= E_OK && v_open(key, v, priv) ) {
		ui_set_text(&ui, PV_PRIV, V_STANDIN);
		if ( v_public(priv, pub) ) {
			char	b[V_B64];

			v_key_b64(pub, b);
			ui_set_text(&ui, PV_PUB, b);
		}
	}
	memset(key, 0, sizeof(key));
	memset(priv, 0, sizeof(priv));
	memset(t, 0, sizeof(t));
}

LOCAL void v_generate( void )
{
	UB	priv[V_KEY], pub[V_KEY];
	char	b[V_B64];

	if ( v_rng(NULL, priv, V_KEY) != 0 ) {
		warn("乱数を取れません。");
		return;
	}
	v_clamp(priv);
	v_key_b64(priv, b);
	ui_set_text(&ui, PV_PRIV, b);
	if ( v_public(priv, pub) ) {
		v_key_b64(pub, b);
		ui_set_text(&ui, PV_PUB, b);
	}
	memset(priv, 0, sizeof(priv));
	memset(b, 0, sizeof(b));
}

/* A key box into sealed text: what is typed, or what was saved when its marks are there */
LOCAL BOOL v_box_sealed( INT id, CONST char *name, CONST char *was, CONST UB key[V_KEY], char *out, BOOL *have )
{
	char	b[V_B64 + 8];
	UB	k[V_KEY];
	BOOL	ok;

	(void)ui_text(&ui, id, b, sizeof(b));
	*have = TRUE;
	if ( strcmp(b, V_STANDIN) == 0 ) {
		ok = v_line(was, name, out, V_SEALED);
	} else if ( b[0] == 0 ) {
		*have = FALSE;
		ok = TRUE;
	} else {
		ok = v_b64_key(b, k) && v_seal(key, k, out);
		memset(k, 0, sizeof(k));
	}
	memset(b, 0, sizeof(b));
	return ok;
}

LOCAL BOOL v_save( void )
{
	static char	was[1024], text[1024];
	char		a[64], peer[V_B64 + 8], ep[128], al[256], spriv[V_SEALED], spsk[V_SEALED];
	UB		key[V_KEY], k[V_KEY];
	BOOL		hp, hk;
	INT		n;
	ER		er;

	(void)ui_text(&ui, PV_ADDR, a, sizeof(a));
	(void)ui_text(&ui, PV_PEER, peer, sizeof(peer));
	(void)ui_text(&ui, PV_ENDPOINT, ep, sizeof(ep));
	(void)ui_text(&ui, PV_ALLOWED, al, sizeof(al));
	if ( !v_b64_key(peer, k) ) {
		warn("相手の公開鍵は４４文字のBase64です。");
		return FALSE;
	}
	if ( a[0] == 0 || ep[0] == 0 || al[0] == 0 || strchr(ep, ':') == NULL ) {
		warn("アドレス、エンドポイント（ホスト:ポート）、許可ＩＰを入れてください。");
		return FALSE;
	}
	if ( v_key(key) < E_OK ) {
		warn("鍵を保存できません。");
		return FALSE;
	}
	n = v_get(V_KIND_CONF, was, sizeof(was) - 1);
	was[( n > 0 ) ? n : 0] = 0;
	if ( !v_box_sealed(PV_PRIV, "private", was, key, spriv, &hk) || !hk ) {
		warn("秘密鍵を入れるか、作成してください。");
		memset(key, 0, sizeof(key));
		return FALSE;
	}
	if ( !v_box_sealed(PV_PSK, "psk", was, key, spsk, &hp) ) {
		warn("事前共有鍵は４４文字のBase64です。");
		memset(key, 0, sizeof(key));
		return FALSE;
	}
	memset(key, 0, sizeof(key));
	n = snprintf(text, sizeof(text), "address %s\npeer %s\nendpoint %s\nallowed %s\nprivate %s\n", a, peer, ep, al, spriv);
	if ( hp ) n += snprintf(text + n, sizeof(text) - n, "psk %s\n", spsk);
	er = v_put(V_KIND_CONF, text, n);
	if ( er < E_OK ) {
		warn("ＶＰＮの設定を保存できません。");
		return FALSE;
	}
	tm_putstring((CONST UB *)"netenv: vpn saved\n");
	v_load();
	return TRUE;
}

/* "a.b.c.d" into 4 bytes */
LOCAL BOOL v_ip( CONST char *s, UB b[4], CONST char **pend )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		INT	v = 0, n = 0;

		while ( *s >= '0' && *s <= '9' && n < 4 ) {
			v = v * 10 + ( *s++ - '0' );
			n++;
		}
		if ( n == 0 || v > 255 ) return FALSE;
		b[i] = (UB)v;
		if ( i < 3 && *s++ != '.' ) return FALSE;
	}
	*pend = s;
	return TRUE;
}

LOCAL void v_up( void )
{
	static char	t[1024], line[900];
	char		v[256], h[80], hp[80];
	UB		key[V_KEY], priv[V_KEY], peer[V_KEY], psk[V_KEY], ip[4], m[4];
	UINT		ep;
	CONST char	*p;
	INT		n, cidr, w, port;
	ER		er;
	BOOL		ok;

	if ( !v_save() ) return;
	n = v_get(V_KIND_CONF, t, sizeof(t) - 1);
	t[( n > 0 ) ? n : 0] = 0;
	if ( v_key(key) < E_OK || !v_line(t, "private", v, sizeof(v)) || !v_open(key, v, priv) ) {
		warn("秘密鍵を開けません。");
		goto out;
	}
	v_hex(priv, V_KEY, h);
	w = snprintf(line, sizeof(line), "UP %s 0 ", h);
	memset(h, 0, sizeof(h));

	/* the address and its mask */
	if ( !v_line(t, "address", v, sizeof(v)) || !v_ip(v, ip, &p) || *p != '/' || ( cidr = atoi(p + 1) ) < 1 || cidr > 32 ) {
		warn("アドレスは 10.0.0.2/24 の形で入れてください。");
		goto out;
	}
	{
		UW	mk = ~0U << ( 32 - cidr );

		m[0] = (UB)( mk >> 24 );
		m[1] = (UB)( mk >> 16 );
		m[2] = (UB)( mk >> 8 );
		m[3] = (UB)mk;
	}
	v_hex(ip, 4, h);
	v_hex(m, 4, hp);
	w += snprintf(line + w, sizeof(line) - w, "%s %s ", h, hp);
	if ( !v_line(t, "peer", v, sizeof(v)) || !v_b64_key(v, peer) ) {
		warn("相手の公開鍵は４４文字のBase64です。");
		goto out;
	}
	v_hex(peer, V_KEY, h);
	w += snprintf(line + w, sizeof(line) - w, "%s ", h);
	if ( v_line(t, "psk", v, sizeof(v)) ) {
		if ( !v_open(key, v, psk) ) {
			warn("事前共有鍵を開けません。");
			goto out;
		}
		v_hex(psk, V_KEY, h);
		w += snprintf(line + w, sizeof(line) - w, "%s ", h);
		memset(h, 0, sizeof(h));
	} else {
		w += snprintf(line + w, sizeof(line) - w, "- ");
	}
	if ( !v_line(t, "endpoint", v, sizeof(v)) || ( p = strrchr(v, ':') ) == NULL ) {
		warn("エンドポイントは ホスト:ポート の形で入れてください。");
		goto out;
	}
	port = atoi(p + 1);
	*(char *)p = 0;
	if ( port < 1 || port > 65535 || so_resolve(v, &ep) < E_OK ) {
		warn("エンドポイントの名前が引けません。");
		goto out;
	}
	v_hex((CONST UB *)&ep, 4, h);
	w += snprintf(line + w, sizeof(line) - w, "%s %d", h, port);
	if ( !v_line(t, "allowed", v, sizeof(v)) ) goto out;
	ok = TRUE;
	for ( p = v; *p != 0 && ok; ) {
		while ( *p == ' ' || *p == ',' ) p++;
		if ( *p == 0 ) break;
		ok = v_ip(p, ip, &p) && *p == '/';
		if ( ok ) {
			cidr = atoi(p + 1);
			ok = ( cidr >= 0 && cidr <= 32 );
			p++;
			while ( *p >= '0' && *p <= '9' ) p++;
		}
		if ( ok ) {
			v_hex(ip, 4, h);
			w += snprintf(line + w, sizeof(line) - w, " %s/%d", h, cidr);
		}
	}
	if ( !ok || w >= (INT)sizeof(line) ) {
		warn("許可ＩＰは 0.0.0.0/0 の形で、コンマで区切って入れてください。");
		goto out;
	}
	er = v_cmd(line);
	if ( er < E_OK ) warn("ＶＰＮに接続できません。");
	tm_putstring((CONST UB *)( ( er >= E_OK ) ? "netenv: vpn up\n" : "netenv: vpn not up\n" ));
	v_state();

    out:
	memset(line, 0, sizeof(line));
	memset(t, 0, sizeof(t));
	memset(key, 0, sizeof(key));
	memset(priv, 0, sizeof(priv));
	memset(psk, 0, sizeof(psk));
	memset(h, 0, sizeof(h));
}

LOCAL void v_down( void )
{
	if ( v_cmd("DOWN") < E_OK ) warn("ＶＰＮの切断ができません。");
	tm_putstring((CONST UB *)"netenv: vpn down\n");
	v_state();
}

LOCAL void v_parts( void )
{
	CONST INT	ids[] = { PV_PRIV, PV_GEN, PV_ADDR, PV_PEER, PV_PSK, PV_ENDPOINT, PV_ALLOWED,
				  PV_SAVE, PV_UP, PV_DOWN };
	INT		i;

	(void)v_find();
	for ( i = 0; i < (INT)( sizeof(ids) / sizeof(ids[0]) ); i++ ) ui_off(&ui, ids[i], !v_have);
	if ( v_have ) {
		v_load();
		v_state();
	}
}

/* A button of the sheet; FALSE when it is none of these */
LOCAL BOOL v_button( INT id )
{
	switch ( id ) {
	case PV_GEN:	v_generate();	return TRUE;
	case PV_SAVE:	(void)v_save();	return TRUE;
	case PV_UP:	v_up();		return TRUE;
	case PV_DOWN:	v_down();	return TRUE;
	default:	return FALSE;
	}
}
