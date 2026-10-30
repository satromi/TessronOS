/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_net.c
 *	The connections of the FTP client over the process's sockets, and
 *	the settings kept in the program object (design 17.17)
 *
 *	A connection is a TCP socket; the host of the control connection is
 *	remembered, so that a data connection goes to the same machine
 *	whatever address the server names. Each read waits at most the time
 *	given (SO_RCVTIMEO) and answers E_TMOUT after it.
 *
 *	For the active way a socket listens on the address the control
 *	connection goes out from, on the port asked for (any when 0: a
 *	router in between that forwards a port wants a fixed one), and the
 *	server's connection to it is waited for as a read is.
 *
 *	The FTP connections the user made are kept in a settings object of
 *	the user's own, which only they read (the settings, below). The
 *	password is among them only when the user asked for it to be kept.
 */

#include "xfc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/sysdef.h>

/* ---------------------------------------------------------------- UUIDs */

EXPORT void xfc_uuid_str( CONST TS_UUID *u, char *out )
{
	static CONST char	hex[] = "0123456789abcdef";
	INT			i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[u->b[i] >> 4];
		out[n++] = hex[u->b[i] & 0x0F];
	}
	out[n] = 0;
}

EXPORT BOOL xfc_uuid_parse( CONST char *s, TS_UUID *u )
{
	INT	i, k = 0;

	for ( i = 0; s[i] != 0 && k < 32; i++ ) {
		char	c = s[i];
		INT	v;

		if ( c == '-' ) continue;
		if ( c >= '0' && c <= '9' ) v = c - '0';
		else if ( c >= 'a' && c <= 'f' ) v = c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) v = c - 'A' + 10;
		else return FALSE;
		if ( ( k & 1 ) == 0 ) u->b[k / 2] = (UB)( v << 4 );
		else u->b[k / 2] |= (UB)v;
		k++;
	}
	return (BOOL)( k == 32 );
}

/* ---------------------------------------------------------------- sockets */

LOCAL XFCNET *net_of( void *ctx )
{
	return (XFCNET *)ctx;
}

/* A socket's reads and writes bounded by the time given */
LOCAL void n_bound( INT s, UINT tmo )
{
	T_SOTIMEVAL	tv;

	tv.sec = tmo / 1000;
	tv.usec = ( tmo % 1000 ) * 1000;
	(void)so_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)so_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

LOCAL INT n_open( void *ctx, CONST char *host, UINT port, INT peer )
{
	XFCNET			*n = net_of(ctx);
	struct sockaddr_in	sa;
	UINT			addr = 0;
	INT			c, s;
	ER			er;

	for ( c = 0; c < XNET_MAX && n->s[c] >= 0; c++ ) ;
	if ( c == XNET_MAX ) {
		return E_LIMIT;
	}
	if ( peer >= 0 ) {
		if ( peer >= XNET_MAX || n->s[peer] < 0 ) return E_PAR;
		addr = n->addr[peer];
	} else {
		er = so_resolve(host, &addr);
		if ( er < E_OK ) return (INT)er;
	}
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) {
		return s;
	}
	n_bound(s, n->tmo);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons((UH)port);
	sa.sin_addr.s_addr = addr;
	er = so_connect(s, (CONST struct sockaddr *)&sa, sizeof(sa));
	if ( er < E_OK ) {
		(void)so_close(s);
		return (INT)er;
	}
	n->s[c] = s;
	n->addr[c] = addr;
	return c;
}

LOCAL INT n_listen( void *ctx, INT peer, UB addr[4], UINT *p_port )
{
	XFCNET			*n = net_of(ctx);
	struct sockaddr_in	sa;
	socklen_t		len = sizeof(sa);
	UINT			local, one = 1;
	INT			c, s;
	ER			er;

	for ( c = 0; c < XNET_MAX && n->s[c] >= 0; c++ ) ;
	if ( c == XNET_MAX ) {
		return E_LIMIT;
	}
	if ( peer < 0 || peer >= XNET_MAX || n->s[peer] < 0 ) {
		return E_PAR;
	}
	/* the address the server reaches this machine by */
	memset(&sa, 0, sizeof(sa));
	er = so_getsockname(n->s[peer], (struct sockaddr *)&sa, &len);
	if ( er < E_OK ) {
		return (INT)er;
	}
	local = sa.sin_addr.s_addr;
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) {
		return s;
	}
	(void)so_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons((UH)n->lport);
	sa.sin_addr.s_addr = INADDR_ANY;
	er = so_bind(s, (CONST struct sockaddr *)&sa, sizeof(sa));
	if ( er >= E_OK ) {
		er = so_listen(s, 1);
	}
	len = sizeof(sa);
	if ( er >= E_OK ) {
		er = so_getsockname(s, (struct sockaddr *)&sa, &len);
	}
	if ( er < E_OK ) {
		(void)so_close(s);
		return (INT)er;
	}
	n->s[c] = s;
	n->addr[c] = local;
	memcpy(addr, &local, 4);		/* network order: a.b.c.d as it is kept */
	*p_port = so_ntohs(sa.sin_port);
	return c;
}

LOCAL INT n_accept( void *ctx, INT l, INT tmo )
{
	XFCNET	*n = net_of(ctx);
	INT	c, s;

	if ( l < 0 || l >= XNET_MAX || n->s[l] < 0 ) return E_ID;
	for ( c = 0; c < XNET_MAX && n->s[c] >= 0; c++ ) ;
	if ( c == XNET_MAX ) {
		return E_LIMIT;
	}
	n_bound(n->s[l], ( tmo > 0 ) ? (UINT)tmo : n->tmo);	/* so_accept waits as a read */
	s = so_accept(n->s[l], NULL, NULL);
	if ( s < 0 ) {
		return s;
	}
	n_bound(s, n->tmo);
	n->s[c] = s;
	n->addr[c] = n->addr[l];
	return c;
}

LOCAL INT n_send( void *ctx, INT c, CONST void *buf, SZ len )
{
	XFCNET	*n = net_of(ctx);

	if ( c < 0 || c >= XNET_MAX || n->s[c] < 0 ) return E_ID;
	return so_send(n->s[c], buf, len, 0);
}

LOCAL INT n_recv( void *ctx, INT c, void *buf, SZ len )
{
	XFCNET	*n = net_of(ctx);

	if ( c < 0 || c >= XNET_MAX || n->s[c] < 0 ) return E_ID;
	return so_recv(n->s[c], buf, len, 0);
}

LOCAL void n_close( void *ctx, INT c )
{
	XFCNET	*n = net_of(ctx);

	if ( c < 0 || c >= XNET_MAX || n->s[c] < 0 ) return;
	(void)so_close(n->s[c]);
	n->s[c] = -1;
}

EXPORT void xfc_net_init( XFCNET *n, T_FTPNET *net, UINT tmo, UINT lport )
{
	INT	i;

	for ( i = 0; i < XNET_MAX; i++ ) n->s[i] = -1;
	n->tmo = tmo;
	n->lport = lport;
	net->ctx = n;
	net->open = n_open;
	net->send = n_send;
	net->recv = n_recv;
	net->close = n_close;
	net->listen = n_listen;
	net->accept = n_accept;
}

EXPORT ER xfc_net_up( void )
{
	UINT	a = 0;
	INT	i;
	ER	er;

	if ( so_getifaddr(&a, NULL, NULL) >= E_OK && a != 0 ) {
		return E_OK;
	}
	er = so_dhcp_start();
	if ( er == E_OACV ) {
		/* bringing the interface up is the administrator's: the password asked for, by the desktop */
		if ( ts_admin_ask("ネットワークの開始", NULL) >= E_OK ) {
			er = so_dhcp_start();
			(void)ob_end_adm();
		}
		if ( er == E_OACV ) ts_no_right("ネットワークを開始");
	}
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < 150; i++ ) {
		if ( so_getifaddr(&a, NULL, NULL) >= E_OK && a != 0 ) return E_OK;
		(void)tk_dly_tsk(100);
	}
	return E_TMOUT;
}

/* ---------------------------------------------------------------- the settings */

/*
 * The connections a person made, with the passwords they asked to be
 * kept, are a settings object of that person's: 「ファイル変換の接続」,
 * owned by them and to be read and written by them alone (rw-------),
 * linked from ファイル変換の箱. Its record 1 is the list, as JSON:
 * {"ftp": [...]}; its record 0 says what it is. Each person has one;
 * it is found among the box's links by what its metadata says it is
 * (tessronos.xfconv.connections) and by its owner, never by its name.
 *
 * A system before kept the list in the metadata of the box, which every
 * user reads, or of the program object. That list is read when the
 * person has no settings object yet, and taken out of the box once the
 * object is written; the program object's is taken out too when the
 * person may write it (an administrator).
 */
#define CONF_NAME	"ファイル変換の接続"
#define CONF_TEXT	"<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>" \
			"ファイル変換のFTPの接続の設定です。持ち主だけが読み書きできます。" \
			"</p>\n</document>\n</tad>\n"

LOCAL BOOL conf_uuid( BOOL prog, TS_UUID *u )
{
	if ( !prog ) return (BOOL)( ob_fnd_sys(SYSROLE_XFCONV, SYSDEF_XFCONV_BOX, u) >= E_OK );
	return xfc_uuid_parse(SYSDEF_PROG_XFCONV, u);
}

LOCAL ID conf_key( UINT ops, BOOL prog )
{
	TS_UUID	u;

	if ( !conf_uuid(prog, &u) ) return E_PAR;
	return ob_opn_obj(&u, ops);
}

/* Whether u is the person's own settings object */
LOCAL BOOL conf_mine( CONST TS_UUID *u, CONST T_OBCRD *me )
{
	T_JSON	root, tf, xc;
	UB	pbuf[OB_PRT_SIZE(16, 4)];
	T_OBPRT	*prt = (T_OBPRT *)pbuf;
	UB	*meta = NULL;
	SZ	asz = 0;
	INT	len;
	ID	key;
	BOOL	yes = FALSE;

	if ( ob_get_prt(u, prt, sizeof(pbuf), &asz) < E_OK
	  || memcmp(&prt->owner, &me->user, sizeof(TS_UUID)) != 0 ) {
		return FALSE;
	}
	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key <= 0 ) return FALSE;
	len = ob_atr_read(key, 0, &meta);
	if ( len >= 0 && js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "xfconv", &xc) >= E_OK ) {
		yes = js_get_bool(&xc, "connections", FALSE);
	}
	free(meta);
	ob_cls_obj(key);
	return yes;
}

/* The person's settings object, found among the box's links */
LOCAL BOOL conf_find( TS_UUID *p_u )
{
	T_OBCRD	me;
	TS_UUID	box, *lk = NULL;
	INT	n, i;
	BOOL	hit = FALSE;

	if ( ob_get_crd(&me) < E_OK || !conf_uuid(FALSE, &box) ) return FALSE;
	n = ui_lst_lnk(&box, &lk);
	for ( i = 0; i < n && !hit; i++ ) {
		if ( conf_mine(&lk[i], &me) ) {
			*p_u = lk[i];
			hit = TRUE;
		}
	}
	free(lk);
	return hit;
}

/* The connections of a list of JSON ({"ftp": [...]}) */
LOCAL INT conf_parse( CONST T_JSON *xc, XFCCONN *c, INT max )
{
	T_JSON	list, it;
	INT	n = 0;

	if ( js_get(xc, "ftp", &list) < E_OK ) return 0;
	for ( it.s = NULL; n < max && js_next(&list, &it); n++ ) {
		XFCCONN	*k = &c[n];
		UB	e[24];

		memset(k, 0, sizeof(*k));
		(void)js_get_str(&it, "host", k->host, sizeof(k->host));
		(void)js_get_str(&it, "name", k->name, sizeof(k->name));
		k->ssh = js_get_bool(&it, "ssh", FALSE);
		snprintf((char *)k->port, sizeof(k->port), "%d", (INT)js_get_num(&it, "port", FTP_PORT));
		(void)js_get_str(&it, "user", k->user, sizeof(k->user));
		k->keep = js_get_bool(&it, "keep", FALSE);
		if ( k->keep ) (void)js_get_str(&it, "pass", k->pass, sizeof(k->pass));
		k->enc = ( js_get_str(&it, "encoding", e, sizeof(e)) > 0 ) ? txc_by_name(e) : TXC_UTF8;
		if ( k->enc == TXC_AUTO ) k->enc = TXC_UTF8;
		k->mode = js_get_bool(&it, "pasv", FALSE) ? XFC_DM_PASV : XFC_DM_EPSV;
		if ( js_get_str(&it, "data", e, sizeof(e)) > 0 ) {
			k->mode = ( strcmp((char *)e, "active") == 0 ) ? XFC_DM_ACTIVE
				: ( strcmp((char *)e, "pasv") == 0 ) ? XFC_DM_PASV : XFC_DM_EPSV;
		}
		if ( js_get_num(&it, "dataport", 0) > 0 ) {
			snprintf((char *)k->lport, sizeof(k->lport), "%d", (INT)js_get_num(&it, "dataport", 0));
		}
	}
	return n;
}

/* The list a system before kept in the metadata of the box (prog FALSE) or of the program object */
LOCAL INT conf_load_old( BOOL prog, XFCCONN *c, INT max )
{
	T_JSON	root, tf, xc;
	UB	*meta = NULL;
	INT	len, n = 0;
	ID	key;

	key = conf_key(OB_OP_ATRRD, prog);
	if ( key <= 0 ) return 0;
	len = ob_atr_read(key, 0, &meta);
	if ( len >= 0 && js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "xfconv", &xc) >= E_OK ) {
		n = conf_parse(&xc, c, max);
	}
	free(meta);
	ob_cls_obj(key);
	return n;
}

EXPORT INT xfc_conf_load( XFCCONN *c, INT max )
{
	T_JSON	root;
	TS_UUID	u;
	UB	*b = NULL;
	INT	len, n;

	if ( conf_find(&u) ) {
		len = ui_rec_all(&u, 1, 0, &b);
		n = ( len >= 0 && js_parse(b, len, &root) >= E_OK ) ? conf_parse(&root, c, max) : 0;
		free(b);
		return n;
	}
	n = conf_load_old(FALSE, c, max);
	if ( n <= 0 ) n = conf_load_old(TRUE, c, max);
	if ( n > 0 && xfc_conf_save(c, n) < E_OK ) {
		xfc_log("the connections were not moved to the person's own settings");
	}
	return n;
}

/* Text inside a JSON string */
LOCAL INT jstr( char *o, INT at, INT max, CONST UB *s )
{
	for ( ; *s != 0 && at < max - 3; s++ ) {
		if ( *s == '"' || *s == '\\' ) o[at++] = '\\';
		if ( *s >= 0x20 ) o[at++] = (char)*s;
	}
	return at;
}

/* The list as JSON, {"ftp": [...]}, in a buffer the caller frees; its length in *p_w */
LOCAL char *conf_text( CONST XFCCONN *c, INT n, INT *p_w )
{
	INT	vcap = 64 + n * ( 4 * 2 * XFC_FIELD + 256 );	/* each text escaped at most twice as long */
	char	*v = (char *)malloc((size_t)vcap);
	INT	w, i;

	if ( v == NULL ) return NULL;
	w = snprintf(v, vcap, "{\"ftp\":[");
	for ( i = 0; i < n; i++ ) {
		if ( i > 0 ) v[w++] = ',';
		w += snprintf(v + w, vcap - w, "{\"host\":\"");
		w = jstr(v, w, vcap, c[i].host);
		w += snprintf(v + w, vcap - w, "\",\"name\":\"");
		w = jstr(v, w, vcap, c[i].name);
		w += snprintf(v + w, vcap - w, "\",\"ssh\":%s", c[i].ssh ? "true" : "false");
		w += snprintf(v + w, vcap - w, ",\"port\":%d,\"user\":\"", atoi((CONST char *)c[i].port));
		w = jstr(v, w, vcap, c[i].user);
		w += snprintf(v + w, vcap - w, "\",\"encoding\":\"%s\",\"pasv\":%s,\"data\":\"%s\","
			      "\"dataport\":%d,\"keep\":%s",
			      txc_name(c[i].enc), ( c[i].mode == XFC_DM_PASV ) ? "true" : "false",
			      ( c[i].mode == XFC_DM_ACTIVE ) ? "active" : ( c[i].mode == XFC_DM_PASV ) ? "pasv" : "epsv",
			      atoi((CONST char *)c[i].lport), c[i].keep ? "true" : "false");
		if ( c[i].keep ) {
			w += snprintf(v + w, vcap - w, ",\"pass\":\"");
			w = jstr(v, w, vcap, c[i].pass);
			v[w++] = '"';
		}
		v[w++] = '}';
	}
	w += snprintf(v + w, vcap - w, "]}");
	*p_w = w;
	return v;
}

/* The person's settings object made, theirs alone, and linked from the box */
LOCAL ER conf_make( TS_UUID *p_u )
{
	static CONST char meta[] =
		"{\"name\":\"" CONF_NAME "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		"\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},"
		"\"tessronos\":{\"xfconv\":{\"connections\":true},"
		"\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	T_OBCRD	me;
	T_OBPRT	prt;
	T_OBCRE	cr;
	TS_UUID	box;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( ob_get_crd(&me) < E_OK || !conf_uuid(FALSE, &box) ) return E_SYS;
	memset(&prt, 0, sizeof(prt));
	prt.owner = me.user;
	if ( me.ngrp > 0 ) prt.group = me.grp[0];
	prt.mode = 0600;
	memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_FILE;
	cr.json = (CONST UB *)meta;
	cr.jsonsz = (SZ)strlen(meta);
	cr.near = box;
	cr.prt = &prt;
	er = ob_cre_obj(&cr, p_u);
	if ( er < E_OK ) return er;
	/* a new object filled record by record: nobody reads it half made */
	key = ob_opn_obj(p_u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	er = ( key > 0 ) ? E_OK : ( key < 0 ) ? (ER)key : E_SYS;
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, CONF_TEXT, (SZ)strlen(CONF_TEXT), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
	if ( key > 0 ) ob_cls_obj(key);
	if ( er >= E_OK ) er = xfu_box_link(&box, p_u);
	if ( er < E_OK ) (void)ob_del_obj(p_u);
	return er;
}

/* The list a system before kept in an object's metadata, taken out of it */
LOCAL void conf_drop_old( BOOL prog )
{
	T_JSON	root, tf, xc;
	UB	*meta = NULL, *out = NULL;
	INT	len, from, to, k;
	ID	key;

	key = conf_key(OB_OP_ATRRD | OB_OP_ATRWR | OB_O_WEXCL, prog);	/* read, changed, written back */
	if ( key <= 0 ) return;
	len = ob_atr_read(key, 0, &meta);
	if ( len >= 0 && js_parse(meta, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "xfconv", &xc) >= E_OK && ( out = (UB *)malloc((size_t)len + 1) ) != NULL ) {
		/* from the member's name, and a comma on one side of it */
		to = (INT)( xc.s - meta ) + xc.len;
		for ( from = (INT)( xc.s - meta ) - 1; from > 0 && meta[from] != '"'; from-- ) ;	/* the name's end */
		for ( from--; from > 0 && meta[from] != '"'; from-- ) ;				/* its start */
		for ( k = to; k < len && ( meta[k] == ' ' || meta[k] == '\n' || meta[k] == '\r' || meta[k] == '\t' ); k++ ) ;
		if ( k < len && meta[k] == ',' ) {
			to = k + 1;
		} else {
			for ( k = from - 1; k > 0 && ( meta[k] == ' ' || meta[k] == '\n' || meta[k] == '\r' || meta[k] == '\t' ); k-- ) ;
			if ( meta[k] == ',' ) from = k;
		}
		memcpy(out, meta, (size_t)from);
		memcpy(out + from, meta + to, (size_t)( len - to ));
		(void)ob_set_atr(key, out, from + len - to);
	}
	free(out);
	free(meta);
	ob_cls_obj(key);
}

EXPORT ER xfc_conf_save( CONST XFCCONN *c, INT n )
{
	TS_UUID	u;
	char	*v;
	INT	w = 0;
	SZ	asz = 0;
	ID	key;
	ER	er = E_OK;

	if ( !conf_find(&u) ) er = conf_make(&u);
	if ( er < E_OK ) return er;
	v = conf_text(c, n, &w);
	if ( v == NULL ) return E_NOMEM;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_O_WEXCL);
	er = ( key > 0 ) ? E_OK : ( key < 0 ) ? (ER)key : E_SYS;
	/* replaced in one change: a reader sees the old record or the new one */
	if ( er >= E_OK ) er = ob_rpl_rec(key, 1, v, (SZ)w);
	(void)asz;
	if ( key > 0 ) ob_cls_obj(key);
	free(v);
	if ( er >= E_OK ) {
		/* what everyone could read is gone once the person's own object has it */
		conf_drop_old(FALSE);
		conf_drop_old(TRUE);
	}
	return er;
}
