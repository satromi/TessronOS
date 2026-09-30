/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_net.c
 *	マイクロスクリプト: talking over the network
 *
 *	The statements TADjs Desktop added to the language, with the same
 *	meaning, so that its scripts run here as they are:
 *
 *	  HTTPREQ method, url[, headers][, body]
 *				one request and its answer, waited for. http and
 *				https; the headers are "Name:value" lines. $ERR 16
 *				when there is no answer at all
 *	  HTTPHDR var, name	a header of the last answer (any case)
 *	  JSONGET var, path	a value of the last answer's JSON: "a.b.0.c". A
 *				name with dots in it is found by joining the
 *				parts that follow until one is there
 *	  JSONLEN var, path	how many: elements, characters, members
 *	  TCPOPEN n, host, port[:timeout]  TCPSEND n, bytes...
 *	  TCPWAIT n, bytes...[:timeout]    TCPRECV n  TCPCLOSE n
 *				a plain connection, as the RS statements treat
 *				a serial line
 *
 *	with $HTTPST (the status), $HTTPLEN and $HTTPBODY[i] (the body's
 *	bytes), $TCP[i], $TCPCNT and $TCPST[n].
 *
 *	An answer that is an event stream (text/event-stream) is taken as a
 *	stream of JSON messages, and the JSON of it is the last that answers
 *	a request (has "result" or "error"), or else the last there is.
 *
 *	A host is reached only once it has been allowed: the first time,
 *	the person is asked in the window, and what they allow is kept in
 *	the figure's metadata ("networkGrants"), as TADjs keeps it.
 *
 *	Waiting for the other side is the thread's wait: the others run in
 *	the meantime, and a thread that is ended stops waiting.
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ts/soapp.h>
#include <ts/json.h>
#include <ts/tls.h>

#define ERR_NET		16
#define HTTP_TIMEOUT	30000		/* ms for the whole of one request */
#define CONN_TIMEOUT	10000		/* ms to connect, when the script does not say */
#define BODY_MAX	( 4 * 1024 * 1024 )
#define TCP_SOCKS	8
#define TCP_BUF		512		/* as the RS statements keep */

/* ---------------------------------------------------------------- the last answer */

static INT	http_st;
static UB	*http_body;
static INT	http_len;
static char	*http_hdr;		/* the header lines, as they came */
static char	*http_json;		/* the JSON of the answer, or NULL */
static INT	http_jlen;

static void answer_drop( void )
{
	ms_free(http_body);
	ms_free(http_hdr);
	ms_free(http_json);
	http_body = NULL;
	http_hdr = NULL;
	http_json = NULL;
	http_len = 0;
	http_jlen = 0;
	http_st = 0;
}

/* ---------------------------------------------------------------- values */

/* An argument as UTF-8, up to its 0: an array named alone is the whole of it */
static void text_arg( MSTH *t, MSN *e, MSSCOPE *sc, MSBUF *b )
{
	MSV	v = rt_whole(t, e, sc);

	mv_text(v, b);
	mv_drop(v);
	mb_putc(b, 0);
	b->n--;
}

/*
 * Text into a variable's elements, a character each and the rest of
 * the variable filled with 0, as TADjs fills it.
 */
static void put_text( MSVAR *v, INT start, const char *s, INT n )
{
	INT	i = 0, k = 0;
	UW	cp;

	while ( i < n && start + k < v->size ) {
		INT	m = ms_utf8_dec((const UB *)s + i, n - i, &cp);

		if ( m <= 0 ) break;
		i += m;
		var_set(v, start + k++, mv_num(cp));
	}
	while ( start + k < v->size ) var_set(v, start + k++, mv_num(0));
}

static BOOL target( MSTH *t, MSN *e, MSSCOPE *sc, MSVAR **p_v, INT *p_start )
{
	*p_v = rt_place(t, e, sc, p_start);
	return (BOOL)( *p_v != NULL );
}

/* ---------------------------------------------------------------- waiting */

/* The thread's turn to the others; FALSE when it is to stop */
static BOOL wait_turn( void *arg )
{
	MSTH	*t = arg;

	ms_yield(ms_now_ms() + 20);
	return (BOOL)!th_stop(t);
}

/* Until the socket can be read (POLLIN) or written (POLLOUT), or the time is up */
static ER sock_wait( MSTH *t, INT s, short ev, UD until )
{
	struct pollfd	p;

	for ( ;; ) {
		p.fd = s;
		p.events = ev;
		p.revents = 0;
		if ( so_poll(&p, 1, 0) > 0 ) return E_OK;
		if ( th_stop(t) ) return E_DISWAI;
		if ( ms_now_ms() >= until ) return E_TMOUT;
		ms_yield(ms_now_ms() + 20);
	}
}

/* ---------------------------------------------------------------- where to go */

typedef struct {
	BOOL	tls;
	char	host[256];
	INT	port;
	char	path[2048];
} URL;

static BOOL url_parse( const char *u, URL *o )
{
	const char	*p, *h, *e;
	INT		n;

	memset(o, 0, sizeof(*o));
	if ( strncmp(u, "http://", 7) == 0 ) { o->tls = FALSE; o->port = 80; p = u + 7; }
	else if ( strncmp(u, "https://", 8) == 0 ) { o->tls = TRUE; o->port = 443; p = u + 8; }
	else return FALSE;
	h = p;
	while ( *p != 0 && *p != '/' && *p != '?' && *p != '#' ) p++;
	e = p;
	/* user:password@ is not taken */
	for ( n = 0; h + n < e; n++ ) if ( h[n] == '@' ) return FALSE;
	{
		const char	*c = e;

		while ( c > h && c[-1] != ':' && c[-1] != ']' ) c--;
		if ( c > h && c[-1] == ':' ) {
			o->port = atoi(c);
			e = c - 1;
		}
	}
	n = (INT)( e - h );
	if ( n <= 0 || n >= (INT)sizeof(o->host) || o->port <= 0 || o->port > 65535 ) return FALSE;
	memcpy(o->host, h, (size_t)n);
	o->host[n] = 0;
	if ( *p == 0 || *p == '#' ) {
		strcpy(o->path, "/");
	} else {
		const char	*q = p;

		while ( *q != 0 && *q != '#' ) q++;
		n = (INT)( q - p );
		if ( n >= (INT)sizeof(o->path) ) return FALSE;
		if ( *p != '/' ) {
			o->path[0] = '/';
			memcpy(o->path + 1, p, (size_t)n);
			o->path[n + 1] = 0;
		} else {
			memcpy(o->path, p, (size_t)n);
			o->path[n] = 0;
		}
	}
	return TRUE;
}

/* The host allowed, asking the person the first time */
static BOOL allowed( const char *host )
{
	if ( ms_net_granted(host) ) return TRUE;
	if ( ms_net_ask(host) != 1 ) return FALSE;
	ms_net_grant(host);
	return TRUE;
}

/* A connected socket, or an error; the socket does not block */
static INT tcp_connect( MSTH *t, const char *host, INT port, INT tmo_ms )
{
	struct sockaddr_in	sa;
	UINT			addr = 0;
	INT			s;
	ER			er;
	UD			until = ms_now_ms() + (UD)tmo_ms;

	if ( strcmp(host, "localhost") == 0 ) {
		addr = SO_IPADDR(127, 0, 0, 1);
	} else if ( so_resolve(host, &addr) < E_OK ) {
		return E_NOEXS;
	}
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return s;
	(void)so_fcntl(s, F_SETFL, O_NONBLOCK);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons((UH)port);
	sa.sin_addr.s_addr = addr;
	er = so_connect(s, (struct sockaddr *)&sa, sizeof(sa));
	if ( er == E_BUSY ) {
		INT		e = 0;
		socklen_t	el = sizeof(e);

		er = sock_wait(t, s, POLLOUT, until);
		if ( er >= E_OK && so_getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) >= E_OK && e != 0 ) er = E_IO;
	}
	if ( er < E_OK ) {
		(void)so_close(s);
		return er;
	}
	return s;
}

/* All of it sent, the socket's wait between pieces */
static ER sock_send_all( MSTH *t, INT s, const void *buf, INT len, UD until )
{
	INT	done = 0, n;

	while ( done < len ) {
		n = so_send(s, (const UB *)buf + done, len - done, 0);
		if ( n > 0 ) { done += n; continue; }
		if ( n < 0 && n != E_BUSY && n != E_TMOUT ) return n;
		if ( sock_wait(t, s, POLLOUT, until) < E_OK ) return E_TMOUT;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- HTTP */

typedef struct {
	MSTH	*t;
	INT	s;
	TS_TLS	*tls;
	UD	until;
} CONN;

static INT conn_send( CONN *c, const void *buf, INT len )
{
	if ( c->tls != NULL ) return ts_tls_write(c->tls, buf, len);
	return ( sock_send_all(c->t, c->s, buf, len, c->until) >= E_OK ) ? len : -1;
}

/* Bytes that came, 0 at the end, negative on an error */
static INT conn_recv( CONN *c, void *buf, INT len )
{
	INT	n;

	if ( c->tls != NULL ) return ts_tls_read(c->tls, buf, len);
	if ( sock_wait(c->t, c->s, POLLIN, c->until) < E_OK ) return -1;
	n = so_recv(c->s, buf, len, 0);
	return ( n < 0 ) ? -1 : n;
}

/* A header's value in the header lines, the name in any case; NULL when there is none */
static const char *hdr_find( const char *h, const char *name, INT *p_len )
{
	INT	nl = (INT)strlen(name);

	while ( h != NULL && *h != 0 ) {
		const char	*e = strstr(h, "\r\n");
		INT		ll = ( e != NULL ) ? (INT)( e - h ) : (INT)strlen(h);

		if ( ll > nl && h[nl] == ':' && strncasecmp(h, name, (size_t)nl) == 0 ) {
			const char	*v = h + nl + 1;

			while ( *v == ' ' || *v == '\t' ) v++;
			*p_len = ll - (INT)( v - h );
			while ( *p_len > 0 && ( v[*p_len - 1] == ' ' || v[*p_len - 1] == '\t' ) ) (*p_len)--;
			return v;
		}
		h = ( e != NULL ) ? e + 2 : NULL;
	}
	return NULL;
}

/* A chunked body put together, in place; its length */
static INT dechunk( UB *b, INT n )
{
	INT	in = 0, out = 0;

	while ( in < n ) {
		INT	size = 0, k;

		while ( in < n ) {
			UB	c = b[in];

			if ( c >= '0' && c <= '9' ) size = size * 16 + ( c - '0' );
			else if ( c >= 'a' && c <= 'f' ) size = size * 16 + ( c - 'a' + 10 );
			else if ( c >= 'A' && c <= 'F' ) size = size * 16 + ( c - 'A' + 10 );
			else break;
			in++;
		}
		while ( in < n && b[in] != '\n' ) in++;	/* extensions and the CR */
		in++;
		if ( size == 0 ) break;
		k = ( in + size <= n ) ? size : n - in;
		if ( k <= 0 ) break;
		memmove(b + out, b + in, (size_t)k);
		out += k;
		in += k + 2;				/* the chunk's CR LF */
	}
	return out;
}

/*
 * An event stream's JSON: the data: lines of each event joined, and of
 * the messages that read as JSON the last that answers a request, or
 * the last.
 */
static void sse_json( const UB *b, INT n )
{
	MSBUF	ev, best, last;
	INT	i = 0;
	BOOL	have_best = FALSE, have_last = FALSE;

	mb_init(&ev);
	mb_init(&best);
	mb_init(&last);
	while ( i <= n ) {
		INT	j = i;

		while ( j < n && b[j] != '\n' ) j++;
		{
			INT	ll = j - i;

			if ( ll > 0 && b[i + ll - 1] == '\r' ) ll--;
			if ( ll == 0 || j >= n ) {
				/* the event ends */
				if ( ll > 0 && ll >= 5 && memcmp(b + i, "data:", 5) == 0 ) {
					INT	o = ( ll > 5 && b[i + 5] == ' ' ) ? 6 : 5;

					if ( ev.n > 0 ) mb_putc(&ev, '\n');
					mb_putn(&ev, (const char *)b + i + o, ll - o);
				}
				if ( ev.n > 0 ) {
					T_JSON	root, v;

					if ( js_parse((const UB *)ev.s, ev.n, &root) >= E_OK ) {
						mb_free(&last);
						mb_init(&last);
						mb_putn(&last, ev.s, ev.n);
						have_last = TRUE;
						if ( js_type(&root) == JS_OBJECT
						  && ( js_get(&root, "result", &v) >= E_OK || js_get(&root, "error", &v) >= E_OK ) ) {
							mb_free(&best);
							mb_init(&best);
							mb_putn(&best, ev.s, ev.n);
							have_best = TRUE;
						}
					}
					mb_free(&ev);
					mb_init(&ev);
				}
			} else if ( ll >= 5 && memcmp(b + i, "data:", 5) == 0 ) {
				INT	o = ( ll > 5 && b[i + 5] == ' ' ) ? 6 : 5;

				if ( ev.n > 0 ) mb_putc(&ev, '\n');
				mb_putn(&ev, (const char *)b + i + o, ll - o);
			}
		}
		i = j + 1;
	}
	if ( have_best || have_last ) {
		MSBUF	*w = have_best ? &best : &last;

		http_json = ms_alloc((size_t)w->n + 1);
		memcpy(http_json, w->s, (size_t)w->n);
		http_jlen = w->n;
	}
	mb_free(&ev);
	mb_free(&best);
	mb_free(&last);
}

/* The answer as it came: the status, the headers, the body and its JSON */
static BOOL answer_take( UB *raw, INT n )
{
	INT		i, hend = -1, blen;
	const char	*v;
	INT		vl;

	for ( i = 0; i + 3 < n; i++ ) {
		if ( raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n' ) {
			hend = i;
			break;
		}
	}
	if ( hend < 0 || n < 12 || memcmp(raw, "HTTP/", 5) != 0 ) return FALSE;
	{
		const char	*sp = memchr(raw, ' ', (size_t)hend);

		if ( sp == NULL ) return FALSE;
		http_st = atoi(sp + 1);
	}
	/* the lines after the status line */
	{
		const UB	*l = memchr(raw, '\n', (size_t)hend);
		INT		hl = ( l != NULL ) ? hend - (INT)( l + 1 - raw ) : 0;

		http_hdr = ms_alloc((size_t)hl + 3);
		if ( hl > 0 ) memcpy(http_hdr, l + 1, (size_t)hl);
		memcpy(http_hdr + hl, "\r\n", 3);
	}
	blen = n - ( hend + 4 );
	http_body = ms_alloc((size_t)blen + 1);
	memcpy(http_body, raw + hend + 4, (size_t)blen);
	v = hdr_find(http_hdr, "Transfer-Encoding", &vl);
	if ( v != NULL && vl >= 7 && strncasecmp(v, "chunked", 7) == 0 ) blen = dechunk(http_body, blen);
	v = hdr_find(http_hdr, "Content-Length", &vl);
	if ( v != NULL ) {
		INT	cl = atoi(v);

		if ( cl >= 0 && cl < blen ) blen = cl;
	}
	http_len = blen;
	http_body[blen] = 0;

	v = hdr_find(http_hdr, "Content-Type", &vl);
	if ( v != NULL && vl >= 17 && strstr(v, "text/event-stream") != NULL && strstr(v, "text/event-stream") < v + vl ) {
		sse_json(http_body, blen);
	} else {
		T_JSON	root;

		if ( blen > 0 && js_parse(http_body, blen, &root) >= E_OK ) {
			http_json = ms_alloc((size_t)blen + 1);
			memcpy(http_json, http_body, (size_t)blen);
			http_jlen = blen;
		}
	}
	return TRUE;
}

/* The request's head: the method, the path, Host, the script's headers, the length */
static void request_head( MSBUF *r, const char *method, const URL *u, const char *hdrs, INT bodylen )
{
	const char	*h = hdrs;
	BOOL		has_ua = FALSE;

	mb_printf(r, "%s %s HTTP/1.1\r\n", method, u->path);
	if ( ( u->tls && u->port == 443 ) || ( !u->tls && u->port == 80 ) ) mb_printf(r, "Host: %s\r\n", u->host);
	else mb_printf(r, "Host: %s:%d\r\n", u->host, u->port);
	while ( h != NULL && *h != 0 ) {
		const char	*e = strchr(h, '\n'), *c;
		INT		ll = ( e != NULL ) ? (INT)( e - h ) : (INT)strlen(h);

		if ( ll > 0 && h[ll - 1] == '\r' ) ll--;
		c = memchr(h, ':', (size_t)ll);
		if ( c != NULL && c > h ) {
			INT	nl = (INT)( c - h ), vs = nl + 1;

			while ( nl > 0 && h[nl - 1] == ' ' ) nl--;
			while ( vs < ll && h[vs] == ' ' ) vs++;
			if ( !( nl == 14 && strncasecmp(h, "Content-Length", 14) == 0 )
			  && !( nl == 4 && strncasecmp(h, "Host", 4) == 0 )
			  && !( nl == 10 && strncasecmp(h, "Connection", 10) == 0 ) ) {
				if ( nl == 10 && strncasecmp(h, "User-Agent", 10) == 0 ) has_ua = TRUE;
				mb_putn(r, h, nl);
				mb_puts(r, ": ");
				mb_putn(r, h + vs, ll - vs);
				mb_puts(r, "\r\n");
			}
		}
		h = ( e != NULL ) ? e + 1 : NULL;
	}
	if ( !has_ua ) mb_puts(r, "User-Agent: TessronOS-MicroScript/1.0\r\n");
	if ( bodylen > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0 ) {
		mb_printf(r, "Content-Length: %d\r\n", bodylen);
	}
	mb_puts(r, "Connection: close\r\n\r\n");
}

static void log_fail( const char *method, const char *url, const char *why )
{
	char	line[512];

	snprintf(line, sizeof(line), "[Net] HTTP %s %s failed: %s\n", method, url, why);
	tm_putstring((const UB *)line);
}

static INT st_httpreq( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSBUF	method, url, hdrs, body, req;
	URL	u;
	CONN	c;
	UB	*raw = NULL;
	INT	n = 0, cap = 0, err = 0, i;
	BOOL	ok = FALSE;
	char	why[128];

	answer_drop();
	mb_init(&method);
	mb_init(&url);
	mb_init(&hdrs);
	mb_init(&body);
	mb_init(&req);
	if ( s->nv > 0 ) text_arg(t, s->v[0], sc, &method); else mb_puts(&method, "GET");
	if ( s->nv > 1 ) text_arg(t, s->v[1], sc, &url);
	if ( s->nv > 2 ) text_arg(t, s->v[2], sc, &hdrs);
	if ( s->nv > 3 ) text_arg(t, s->v[3], sc, &body);
	mb_putc(&method, 0);
	mb_putc(&url, 0);
	mb_putc(&hdrs, 0);
	for ( i = 0; method.s[i] != 0; i++ ) {
		if ( method.s[i] >= 'a' && method.s[i] <= 'z' ) method.s[i] = (char)( method.s[i] - 'a' + 'A' );
	}
	why[0] = 0;
	memset(&c, 0, sizeof(c));
	c.t = t;
	c.s = -1;
	c.until = ms_now_ms() + HTTP_TIMEOUT;

	if ( !url_parse(url.s, &u) ) {
		snprintf(why, sizeof(why), "not an http or https URL");
		goto done;
	}
	if ( !allowed(u.host) ) {
		snprintf(why, sizeof(why), "%s is not allowed", u.host);
		goto done;
	}
	c.s = tcp_connect(t, u.host, u.port, CONN_TIMEOUT);
	if ( c.s < 0 ) {
		snprintf(why, sizeof(why), "no connection to %s:%d (%d)", u.host, u.port, c.s);
		goto done;
	}
	if ( u.tls ) {
		c.tls = ts_tls_open(c.s, u.host, HTTP_TIMEOUT, wait_turn, t, &err);
		if ( c.tls == NULL ) {
			char	e[96];

			ts_tls_strerror(err, e, sizeof(e));
			snprintf(why, sizeof(why), "TLS: %s", e);
			goto done;
		}
	}
	request_head(&req, method.s, &u, hdrs.s, body.n);
	if ( body.n > 0 ) mb_putn(&req, body.s, body.n);
	if ( conn_send(&c, req.s, req.n) != req.n ) {
		snprintf(why, sizeof(why), "could not send");
		goto done;
	}
	for ( ;; ) {
		INT	k;

		if ( n + 4096 > cap ) {
			cap = cap * 2 + 8192;
			raw = ms_realloc(raw, (size_t)cap);
		}
		k = conn_recv(&c, raw + n, cap - n);
		if ( k == 0 ) break;
		if ( k < 0 ) {
			if ( n > 0 ) break;		/* what came is the answer */
			snprintf(why, sizeof(why), th_stop(t) ? "stopped" : "no answer");
			goto done;
		}
		n += k;
		if ( n > BODY_MAX + 65536 ) break;
	}
	ok = answer_take(raw, n);
	if ( !ok ) snprintf(why, sizeof(why), "not an HTTP answer");
    done:
	if ( c.tls != NULL ) ts_tls_close(c.tls);
	if ( c.s >= 0 ) (void)so_close(c.s);
	if ( !ok ) {
		answer_drop();
		log_fail(method.s, url.s, why);
	}
	rt.lasterr = ok ? 0 : ERR_NET;
	ms_free(raw);
	mb_free(&method);
	mb_free(&url);
	mb_free(&hdrs);
	mb_free(&body);
	mb_free(&req);
	return FL_NONE;
}

static INT st_httphdr( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSBUF		name;
	MSVAR		*v;
	INT		st, vl = 0;
	const char	*val;
	MSBUF		all;
	const char	*h;

	if ( s->nv < 2 ) { rt.lasterr = 1; return FL_NONE; }
	mb_init(&name);
	text_arg(t, s->v[1], sc, &name);
	mb_putc(&name, 0);
	/* the same header more than once: its values joined with ", " */
	mb_init(&all);
	for ( h = http_hdr; h != NULL && ( val = hdr_find(h, name.s, &vl) ) != NULL; ) {
		if ( all.n > 0 ) mb_puts(&all, ", ");
		mb_putn(&all, val, vl);
		h = strstr(val, "\r\n");
		if ( h != NULL ) h += 2;
	}
	if ( all.n == 0 && ( http_hdr == NULL || hdr_find(http_hdr, name.s, &vl) == NULL ) ) {
		rt.lasterr = 1;
	} else {
		if ( target(t, s->v[0], sc, &v, &st) ) put_text(v, st, all.s != NULL ? all.s : "", all.n);
		rt.lasterr = 0;
	}
	mb_free(&all);
	mb_free(&name);
	return FL_NONE;
}

/* ---------------------------------------------------------------- JSON */

/* The value at a path of the answer's JSON */
static BOOL json_at( const char *path, T_JSON *out )
{
	T_JSON	cur;
	char	key[512];
	INT	pi = 0, pn = (INT)strlen(path);

	if ( http_json == NULL || js_parse((const UB *)http_json, http_jlen, &cur) < E_OK ) return FALSE;
	if ( pn == 0 ) { *out = cur; return TRUE; }
	while ( pi <= pn ) {
		INT	e = pi, kl;

		while ( e < pn && path[e] != '.' ) e++;
		kl = e - pi;
		if ( kl >= (INT)sizeof(key) ) return FALSE;
		memcpy(key, path + pi, (size_t)kl);
		key[kl] = 0;
		if ( js_type(&cur) == JS_ARRAY ) {
			T_JSON	it;
			INT	idx, k = 0;
			char	*end;

			idx = (INT)strtol(key, &end, 10);
			if ( kl == 0 || *end != 0 || idx < 0 ) return FALSE;
			it.s = NULL;
			while ( js_next(&cur, &it) ) {
				if ( k++ == idx ) break;
			}
			if ( k != idx + 1 || it.s == NULL ) return FALSE;
			cur = it;
		} else if ( js_type(&cur) == JS_OBJECT ) {
			T_JSON	v;

			/* a name with dots in it: the parts that follow joined on until one is there */
			while ( js_get(&cur, key, &v) < E_OK ) {
				INT	e2 = e + 1;

				if ( e >= pn ) return FALSE;
				while ( e2 < pn && path[e2] != '.' ) e2++;
				if ( e2 - pi >= (INT)sizeof(key) ) return FALSE;
				memcpy(key, path + pi, (size_t)( e2 - pi ));
				key[e2 - pi] = 0;
				e = e2;
			}
			cur = v;
		} else {
			return FALSE;
		}
		pi = e + 1;
	}
	*out = cur;
	return TRUE;
}

static INT st_jsonget( MSTH *t, MSN *s, MSSCOPE *sc, BOOL len_only )
{
	MSBUF	path;
	T_JSON	v;
	MSVAR	*var;
	INT	st;

	if ( s->nv < 2 ) { rt.lasterr = 1; return FL_NONE; }
	mb_init(&path);
	text_arg(t, s->v[1], sc, &path);
	mb_putc(&path, 0);
	if ( !json_at(path.s, &v) ) {
		mb_free(&path);
		rt.lasterr = 1;
		return FL_NONE;
	}
	mb_free(&path);
	if ( len_only ) {
		INT	cnt;

		switch ( js_type(&v) ) {
		case JS_ARRAY: case JS_OBJECT:
			cnt = js_count(&v);
			break;
		case JS_STRING: {
			UB	*b = ms_alloc((size_t)v.len + 1);
			INT	k, m;
			UW	cp;

			(void)js_str(&v, b, v.len + 1);
			cnt = 0;
			for ( k = 0; b[k] != 0; k += m ) {
				m = ms_utf8_dec(b + k, (INT)strlen((char *)b + k), &cp);
				if ( m <= 0 ) break;
				cnt++;
			}
			ms_free(b);
			break;
		}
		default:
			rt.lasterr = 1;
			return FL_NONE;
		}
		if ( target(t, s->v[0], sc, &var, &st) ) var_set(var, st, mv_num(cnt));
		rt.lasterr = 0;
		return FL_NONE;
	}
	if ( !target(t, s->v[0], sc, &var, &st) ) { rt.lasterr = 0; return FL_NONE; }
	switch ( js_type(&v) ) {
	case JS_NUMBER: {
		D	d = 0;

		(void)js_num(&v, &d);
		var_set(var, st, mv_num((double)ms_int32((double)d)));
		break;
	}
	case JS_TRUE:	var_set(var, st, mv_num(1)); break;
	case JS_FALSE:	var_set(var, st, mv_num(0)); break;
	case JS_STRING: {
		UB	*b = ms_alloc((size_t)v.len + 1);
		INT	m = js_str(&v, b, v.len + 1);

		put_text(var, st, (const char *)b, ( m < 0 ) ? 0 : m);
		ms_free(b);
		break;
	}
	default:
		/* an object, an array or null: its JSON */
		put_text(var, st, (const char *)v.s, v.len);
		break;
	}
	rt.lasterr = 0;
	return FL_NONE;
}

/* ---------------------------------------------------------------- SPRINTF */

static INT st_sprintf( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSBUF	b;
	MSVAR	*v;
	INT	st;

	if ( s->nv < 2 ) { rt.lasterr = 1; return FL_NONE; }
	mb_init(&b);
	rt_format(t, s->v + 1, s->nv - 1, sc, &b);
	if ( target(t, s->v[0], sc, &v, &st) ) put_text(v, st, b.s != NULL ? b.s : "", b.n);
	mb_free(&b);
	rt.lasterr = 0;
	return FL_NONE;
}

/* ---------------------------------------------------------------- TCP */

typedef struct {
	INT	s;
	BOOL	open;
	UB	buf[TCP_BUF];
	INT	n;
} TCPS;

static TCPS	tcps[TCP_SOCKS];
static UB	tcp_data[TCP_BUF];
static INT	tcp_cnt;
static BOOL	tcp_init;

static TCPS *tcp_of( INT n )
{
	INT	i;

	if ( !tcp_init ) {
		for ( i = 0; i < TCP_SOCKS; i++ ) tcps[i].s = -1;
		tcp_init = TRUE;
	}
	return ( n >= 0 && n < TCP_SOCKS ) ? &tcps[n] : NULL;
}

/* What has come in, into the buffer; the oldest goes when it is full */
static void tcp_poll( TCPS *p )
{
	struct pollfd	f;
	UB		tmp[512];
	INT		k, guard;

	if ( !p->open ) return;
	for ( guard = 0; guard < 16; guard++ ) {
		f.fd = p->s;
		f.events = POLLIN;
		f.revents = 0;
		if ( so_poll(&f, 1, 0) <= 0 ) break;
		k = so_recv(p->s, tmp, sizeof(tmp), 0);
		if ( k <= 0 ) {
			(void)so_close(p->s);
			p->s = -1;
			p->open = FALSE;
			break;
		}
		if ( p->n + k > TCP_BUF ) {
			INT	drop = p->n + k - TCP_BUF;

			if ( drop > p->n ) drop = p->n;
			memmove(p->buf, p->buf + drop, (size_t)( p->n - drop ));
			p->n -= drop;
			if ( k > TCP_BUF ) { memcpy(tmp, tmp + k - TCP_BUF, TCP_BUF); k = TCP_BUF; }
		}
		memcpy(p->buf + p->n, tmp, (size_t)k);
		p->n += k;
	}
}

/* The bytes the arguments from `from` on give, as RSPUT takes them */
static INT arg_bytes( MSTH *t, MSN *s, INT from, MSSCOPE *sc, UB *out, INT max )
{
	INT	i, n = 0, k;

	for ( i = from; i < s->nv && n < max; i++ ) {
		MSV	v = ev(t, s->v[i], sc);

		if ( v.t == V_ARR ) {
			for ( k = 0; k < v.u.a->n && n < max; k++ ) out[n++] = (UB)ms_int32(mv_n(v.u.a->e[k]));
		} else if ( v.t == V_STR ) {
			for ( k = 0; k < v.u.s->len && n < max; k++ ) out[n++] = (UB)v.u.s->s[k];
		} else {
			out[n++] = (UB)mv_i(v);
		}
		mv_drop(v);
	}
	return n;
}

static INT st_tcp( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	no = ( s->nv > 0 ) ? ms_int32(rt_num(t, s->v[0], sc)) : 0;
	TCPS	*p = tcp_of(no);
	UB	out[1024];
	INT	n;

	if ( p == NULL ) {
		if ( s->k == S_TCPRECV ) tcp_cnt = 0;
		rt.lasterr = ERR_NET;
		return FL_NONE;
	}
	switch ( s->k ) {
	case S_TCPOPEN: {
		MSBUF	host;
		INT	port = ( s->nv > 2 ) ? ms_int32(rt_num(t, s->v[2], sc)) : 0;
		double	tmo = ( s->a != NULL ) ? rt_num(t, s->a, sc) : ( s->nv > 3 ) ? rt_num(t, s->v[3], sc) : 0;
		INT	sk;

		if ( p->open ) { (void)so_close(p->s); p->open = FALSE; p->s = -1; }
		p->n = 0;
		mb_init(&host);
		if ( s->nv > 1 ) text_arg(t, s->v[1], sc, &host);
		mb_putc(&host, 0);
		if ( host.s[0] == 0 || port <= 0 || !allowed(host.s) ) {
			rt.lasterr = ERR_NET;
		} else {
			sk = tcp_connect(t, host.s, port, ( tmo > 0 ) ? (INT)( tmo * 1000 ) : CONN_TIMEOUT);
			if ( sk < 0 ) {
				rt.lasterr = ERR_NET;
			} else {
				p->s = sk;
				p->open = TRUE;
				rt.lasterr = 0;
			}
		}
		mb_free(&host);
		break;
	}
	case S_TCPSEND:
		if ( !p->open ) { rt.lasterr = ERR_NET; break; }
		n = arg_bytes(t, s, 1, sc, out, sizeof(out));
		rt.lasterr = ( n == 0 || sock_send_all(t, p->s, out, n, ms_now_ms() + HTTP_TIMEOUT) >= E_OK ) ? 0 : ERR_NET;
		break;
	case S_TCPWAIT: {
		double	tmo = ( s->a != NULL ) ? rt_num(t, s->a, sc) : -1;
		UD	start = ms_now_ms();

		n = arg_bytes(t, s, 1, sc, out, sizeof(out));
		if ( n == 0 ) { rt.lasterr = 0; break; }
		t->waiting = TRUE;
		while ( !th_stop(t) ) {
			INT	i;
			BOOL	found = FALSE;

			tcp_poll(p);
			for ( i = 0; i + n <= p->n && !found; i++ ) found = (BOOL)( memcmp(p->buf + i, out, (size_t)n) == 0 );
			if ( found ) { rt.lasterr = 0; break; }
			if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
			if ( tmo == 0 ) { rt.lasterr = 1; break; }
			if ( tmo > 0 && (double)( ms_now_ms() - start ) >= tmo * 1000 ) { rt.lasterr = 1; break; }
			ms_yield(ms_now_ms() + 20);
		}
		t->waiting = FALSE;
		break;
	}
	case S_TCPRECV:
		tcp_poll(p);
		memcpy(tcp_data, p->buf, (size_t)p->n);
		tcp_cnt = p->n;
		p->n = 0;
		rt.lasterr = 0;
		break;
	case S_TCPCLOSE:
		if ( p->open ) (void)so_close(p->s);
		p->open = FALSE;
		p->s = -1;
		p->n = 0;
		rt.lasterr = 0;
		break;
	}
	return FL_NONE;
}

/* ---------------------------------------------------------------- the runtime's side */

INT ms_net_exec( MSTH *t, MSN *s, MSSCOPE *sc )
{
	switch ( s->k ) {
	case S_HTTPREQ:		return st_httpreq(t, s, sc);
	case S_HTTPHDR:		return st_httphdr(t, s, sc);
	case S_JSONGET:		return st_jsonget(t, s, sc, FALSE);
	case S_JSONLEN:		return st_jsonget(t, s, sc, TRUE);
	case S_SPRINTF:		return st_sprintf(t, s, sc);
	case S_CONSOLE: {
		double	mode = ( s->nv > 0 ) ? rt_num(t, s->v[0], sc) : 1;

		ms_console((BOOL)( mode != 0 ));
		rt.lasterr = 0;
		return FL_NONE;
	}
	case S_TCPOPEN: case S_TCPSEND: case S_TCPWAIT: case S_TCPRECV: case S_TCPCLOSE:
		return st_tcp(t, s, sc);
	}
	return FL_NONE;
}

BOOL ms_net_sysvar( const char *u, INT i, MSV *out )
{
	if ( strcmp(u, "HTTPST") == 0 ) { *out = mv_num(http_st); return TRUE; }
	if ( strcmp(u, "HTTPLEN") == 0 ) { *out = mv_num(http_len); return TRUE; }
	if ( strcmp(u, "HTTPBODY") == 0 ) {
		*out = mv_num(( http_body != NULL && i >= 0 && i < http_len ) ? http_body[i] : 0);
		return TRUE;
	}
	if ( strcmp(u, "TCPCNT") == 0 ) { *out = mv_num(tcp_cnt); return TRUE; }
	if ( strcmp(u, "TCP") == 0 ) { *out = mv_num(( i >= 0 && i < tcp_cnt ) ? tcp_data[i] : 0); return TRUE; }
	if ( strcmp(u, "TCPST") == 0 ) {
		TCPS	*p = tcp_of(i);

		if ( p != NULL ) tcp_poll(p);
		*out = mv_num(( p != NULL && p->open ) ? 1 : 0);
		return TRUE;
	}
	return FALSE;
}

/* The program ends: what is open is closed */
void ms_net_end( void )
{
	INT	i;

	for ( i = 0; i < TCP_SOCKS && tcp_init; i++ ) {
		if ( tcps[i].open ) (void)so_close(tcps[i].s);
		tcps[i].open = FALSE;
	}
	answer_drop();
}
