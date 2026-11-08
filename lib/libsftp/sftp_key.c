/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sftp_key.c
 *	The person's private key for SSH: two objects of their own, in the
 *	shape the VPN's are (docs/tessronos/09-network.md 9.7.2).
 *
 *	  ＳＳＨの鍵		record 1, 32 random bytes, made the first time
 *	  ＳＳＨの秘密鍵	record 1, the PEM sealed with those by AES-256-GCM:
 *				the nonce, the text, the tag
 *
 *	Both are the owner's, rw-------, with no ACL. No link points to them, so a
 *	hold (ob_lnk_obj) keeps them, and they are found among all the
 *	objects by owner and by a mark in the metadata (tessronos.ssh), not
 *	by name or by any place. One that has been opened to others is not
 *	used. The separation protects a copy of the one object; it protects
 *	nothing against the same person's programs or a disk taken away.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/ftp.h>
#include <ts/sftp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

#define K_KEY		32
#define K_NONCE		12
#define K_TAG		16
#define K_AAD		"TESSRONOS-SSH1"
#define K_KIND_KEY	"sshkey"
#define K_KIND_PRIV	"sshpriv"

extern int mbedtls_hardware_poll( void *data, unsigned char *output, size_t len, size_t *olen );

static int k_rng( void *ctx, unsigned char *out, size_t n )
{
	size_t	got = 0;

	(void)ctx;
	return mbedtls_hardware_poll(NULL, out, n, &got);
}

/* Whether u carries the mark kind */
static BOOL k_marked( CONST TS_UUID *u, CONST char *kind )
{
	T_JSON	root, tf, ne;
	UB	*meta = NULL;
	INT	len;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	yes = FALSE;

	if ( key <= 0 ) return FALSE;
	len = ob_atr_read(key, 0, &meta);
	if ( len >= 0 && js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "ssh", &ne) >= E_OK ) {
		yes = js_get_bool(&ne, kind, FALSE);
	}
	free(meta);
	ob_cls_obj(key);
	return yes;
}

/* The person's object of a kind. E_NOEXS when there is none, E_OACV when others can reach it */
static ER k_find( CONST char *kind, TS_UUID *p_u )
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
		  || !k_marked(&ids[i], kind) ) {
			continue;
		}
		*p_u = ids[i];
		er = ( ( prt->mode & 0077 ) != 0 ) ? E_OACV : E_OK;
	}
	free(ids);
	return er;
}

/* A new object of the person's, theirs alone from before anything is in it */
static ER k_make( CONST char *kind, TS_UUID *p_u )
{
	static CONST char	text[] = "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>"
				"コマンドsshとファイル変換が使うＳＳＨの鍵のデータです。持ち主だけが読み書きできます。"
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

	if ( ob_get_crd(&me) < E_OK || ui_uuid(SYSDEF_PROG_XFCONV, &near) < E_OK ) return E_SYS;
	snprintf(meta, sizeof(meta),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},"
		 "\"tessronos\":{\"ssh\":{\"%s\":true},"
		 "\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}",
		 ( strcmp(kind, K_KIND_KEY) == 0 ) ? "ＳＳＨの鍵" : "ＳＳＨの秘密鍵", kind);
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
static ER k_put( CONST char *kind, CONST void *b, SZ n )
{
	TS_UUID	u;
	ID	key;
	ER	er = k_find(kind, &u);

	if ( er == E_NOEXS ) er = k_make(kind, &u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_O_WEXCL);
	if ( key < E_OK ) return (ER)key;
	er = ob_rpl_rec(key, 1, b, n);
	ob_cls_obj(key);
	return er;
}

/* The bytes of record 1 of the person's object of a kind, or an error */
static INT k_get( CONST char *kind, void *b, SZ max )
{
	TS_UUID	u;
	SZ	asz = 0;
	ID	key;
	ER	er = k_find(kind, &u);

	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) return (INT)key;
	er = ob_rea_rec(key, 1, 0, b, max, &asz);
	ob_cls_obj(key);
	return ( er < E_OK ) ? er : (INT)asz;
}

EXPORT ER ssh_key_store( CONST char *pem, SZ len )
{
	static UB		sealed[K_NONCE + SSH_KEY_MAX + K_TAG];
	static char		line[SSH_PUB_MAX];
	UB			key[K_KEY];
	mbedtls_gcm_context	g;
	INT			n;
	ER			er;
	BOOL			ok;

	n = ssh_key_public(pem, len, line, sizeof(line));
	if ( n < E_OK ) return n;
	n = k_get(K_KIND_KEY, key, K_KEY);
	if ( n == E_OACV ) return E_OACV;
	if ( n != K_KEY ) {
		if ( k_rng(NULL, key, K_KEY) != 0 ) return E_SYS;
		er = k_put(K_KIND_KEY, key, K_KEY);
		if ( er < E_OK ) return er;
	}
	if ( k_rng(NULL, sealed, K_NONCE) != 0 ) return E_SYS;
	mbedtls_gcm_init(&g);
	ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
	  && mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, (size_t)len, sealed, K_NONCE, (CONST UB *)K_AAD,
				       sizeof(K_AAD) - 1, (CONST UB *)pem, sealed + K_NONCE, K_TAG,
				       sealed + K_NONCE + len) == 0;
	mbedtls_gcm_free(&g);
	memset(key, 0, sizeof(key));
	if ( !ok ) return E_SYS;
	return k_put(K_KIND_PRIV, sealed, K_NONCE + len + K_TAG);
}

EXPORT INT ssh_key_load( char *pem, SZ max )
{
	static UB		sealed[K_NONCE + SSH_KEY_MAX + K_TAG];
	UB			key[K_KEY];
	mbedtls_gcm_context	g;
	INT			n, len;
	BOOL			ok;

	n = k_get(K_KIND_PRIV, sealed, sizeof(sealed));
	if ( n < E_OK ) return n;
	len = n - K_NONCE - K_TAG;
	if ( len <= 0 || len > max ) return E_OBJ;
	n = k_get(K_KIND_KEY, key, K_KEY);
	if ( n < E_OK ) return n;
	if ( n != K_KEY ) return E_OBJ;
	mbedtls_gcm_init(&g);
	ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
	  && mbedtls_gcm_auth_decrypt(&g, (size_t)len, sealed, K_NONCE, (CONST UB *)K_AAD, sizeof(K_AAD) - 1,
				      sealed + K_NONCE + len, K_TAG, sealed + K_NONCE, (UB *)pem) == 0;
	mbedtls_gcm_free(&g);
	memset(key, 0, sizeof(key));
	return ok ? len : E_OBJ;
}

EXPORT INT ssh_key_generate( INT bits, char *pem, SZ max )
{
	mbedtls_pk_context	pk;
	INT			r = E_SYS;

	mbedtls_pk_init(&pk);
	if ( mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0
	  && mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), k_rng, NULL, (unsigned int)bits, 65537) == 0
	  && mbedtls_pk_write_key_pem(&pk, (UB *)pem, (size_t)max) == 0 ) {
		r = (INT)strlen(pem);
	}
	mbedtls_pk_free(&pk);
	return r;
}
