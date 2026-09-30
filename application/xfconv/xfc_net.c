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
 *	The FTP connections the user made are kept in the metadata of the
 *	program object, "tessronos.xfconv": {"ftp": [...]}. The password is
 *	among them only when the user asked for it to be kept.
 */

#include "xfc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/json.h>
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

LOCAL ID prog_key( UINT ops )
{
	TS_UUID	u;

	if ( !xfc_uuid_parse(SYSDEF_PROG_XFCONV, &u) ) return E_PAR;
	return ob_opn_obj(&u, ops);
}

EXPORT INT xfc_conf_load( XFCCONN *c, INT max )
{
	T_JSON	root, tf, xc, list, it;
	UB	*meta;
	SZ	len = 0;
	INT	n = 0;
	ID	key;

	key = prog_key(OB_OP_ATRRD);
	if ( key <= 0 ) {
		return 0;
	}
	meta = (UB *)malloc(OB_ATR_MAX + 1);
	if ( meta != NULL && ob_get_atr(key, meta, OB_ATR_MAX, &len) >= E_OK
	  && js_parse(meta, (INT)len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "xfconv", &xc) >= E_OK && js_get(&xc, "ftp", &list) >= E_OK ) {
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
	}
	free(meta);
	ob_cls_obj(key);
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

EXPORT ER xfc_conf_save( CONST XFCCONN *c, INT n )
{
	T_JSON	root, tf, xc;
	UB	*meta, *out;
	char	*v;
	SZ	len = 0;
	INT	at = 0, i, w, from, to;
	ID	key;
	ER	er;

	key = prog_key(OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key <= 0 ) {
		return (ER)key;
	}
	meta = (UB *)malloc(OB_ATR_MAX + 1);
	out = (UB *)malloc(OB_ATR_MAX + 1);
	v = (char *)malloc(8192);
	er = ( meta != NULL && out != NULL && v != NULL ) ? ob_get_atr(key, meta, OB_ATR_MAX, &len) : E_NOMEM;
	if ( er >= E_OK && ( js_parse(meta, (INT)len, &root) < E_OK || js_get(&root, "tessronos", &tf) < E_OK
			  || js_type(&tf) != JS_OBJECT ) ) {
		er = E_OBJ;
	}
	if ( er >= E_OK ) {
		/* the value of "xfconv" */
		w = snprintf(v, 8192, "{\"ftp\":[");
		for ( i = 0; i < n && w < 8000; i++ ) {
			if ( i > 0 ) v[w++] = ',';
			w += snprintf(v + w, 8192 - w, "{\"host\":\"");
			w = jstr(v, w, 8192, c[i].host);
			w += snprintf(v + w, 8192 - w, "\",\"name\":\"");
			w = jstr(v, w, 8192, c[i].name);
			w += snprintf(v + w, 8192 - w, "\",\"ssh\":%s", c[i].ssh ? "true" : "false");
			w += snprintf(v + w, 8192 - w, ",\"port\":%d,\"user\":\"", atoi((CONST char *)c[i].port));
			w = jstr(v, w, 8192, c[i].user);
			w += snprintf(v + w, 8192 - w, "\",\"encoding\":\"%s\",\"pasv\":%s,\"data\":\"%s\","
				      "\"dataport\":%d,\"keep\":%s",
				      txc_name(c[i].enc), ( c[i].mode == XFC_DM_PASV ) ? "true" : "false",
				      ( c[i].mode == XFC_DM_ACTIVE ) ? "active" : ( c[i].mode == XFC_DM_PASV ) ? "pasv" : "epsv",
				      atoi((CONST char *)c[i].lport), c[i].keep ? "true" : "false");
			if ( c[i].keep ) {
				w += snprintf(v + w, 8192 - w, ",\"pass\":\"");
				w = jstr(v, w, 8192, c[i].pass);
				v[w++] = '"';
			}
			v[w++] = '}';
		}
		w += snprintf(v + w, 8192 - w, "]}");

		/* in place of the old one, or first in "tessronos" */
		if ( js_get(&tf, "xfconv", &xc) >= E_OK ) {
			from = (INT)( xc.s - meta );
			to = from + xc.len;
		} else {
			from = to = (INT)( tf.s - meta ) + 1;
		}
		if ( len + w + 32 > OB_ATR_MAX ) {
			er = E_LIMIT;
		} else {
			memcpy(out, meta, from);
			at = from;
			if ( from == to ) {
				at += snprintf((char *)out + at, 32, "\"xfconv\":");
			}
			memcpy(out + at, v, w);
			at += w;
			if ( from == to && tf.len > 2 ) {
				INT	k;

				for ( k = from; k < (INT)len && ( meta[k] == ' ' || meta[k] == '\n'
							     || meta[k] == '\r' || meta[k] == '\t' ); k++ ) ;
				if ( meta[k] != '}' ) out[at++] = ',';
			}
			memcpy(out + at, meta + to, len - to);
			at += (INT)( len - to );
			er = ob_set_atr(key, out, at);
		}
	}
	free(meta);
	free(out);
	free(v);
	ob_cls_obj(key);
	return er;
}
