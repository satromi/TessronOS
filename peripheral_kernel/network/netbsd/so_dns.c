/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_dns.c
 *	Names looked up through a DNS server (RFC 1035, RFC 3596; design 12.6).
 *
 *	A stub resolver: a question for the IPv4 addresses (type A) of a
 *	name, for its IPv6 addresses (type AAAA), or both at once, goes to
 *	the name server over UDP, and the addresses in the answers come
 *	back, following whatever aliases (CNAME) the server put in with them.
 *	The servers are the IPv4 ones the settings name, else the ones that
 *	came with the address (so_api.c), then the IPv6 ones the settings
 *	name and those the routers named (so_getdns6, so_slaac.c); each is
 *	asked over its own family. Each server is
 *	asked in turn, four rounds, waiting twice as long each round (1, 2,
 *	4 and 8 s). When both kinds are asked for, both questions go to a
 *	server together and its answers are waited for together: once one
 *	has come, the other half a second more, and once there are addresses
 *	of one kind the asking stops.
 *
 *	An answer that does not fit in a UDP message comes back cut short
 *	(TC); the same question then goes to that server over TCP, where an
 *	answer can be as long as 65535 bytes (RFC 1035 4.2.2, RFC 7766 5).
 *	Every address an answer carries is taken, however many: the lists
 *	are made as large as the answers are.
 *
 *	Answers are kept for as long as they said they hold, up to five
 *	minutes, so that a page that asks for the same host again and again
 *	asks the network once; each kind of a name is kept on its own. A name
 *	that does not exist, or has no address of the kind asked, is kept for
 *	half a minute. "localhost" is always 127.0.0.1 and ::1.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/time.h>
#include <ts/uuid.h>
#include "tstdlib.h"
#include "so_nb.h"

#define DNS_PORT		53
#define DNS_ROUNDS		4
#define DNS_WAIT_MS		1000		/* the first round; twice as long each round after */
#define DNS_MSG_MAX		512		/* a UDP message (RFC 1035 4.2.1) */
#define DNS_NAME_MAX		255		/* RFC 1035 2.3.4 */
#define DNS_CACHE		64
#define DNS_TTL_MAX_S		300
#define DNS_TTL_NONAME_S	30
#define DNS_OTHER_MS		500		/* the wait for the second kind once the first has answered */
#define DNS_TCP_MS		5000		/* a question asked again over TCP, answer and all */

#define T_A			1
#define T_CNAME			5
#define T_AAAA			28
#define C_IN			1

/* The kinds of question: A and AAAA */
#define K_A			0
#define K_AAAA			1
#define KINDS			2

/* Where a kind stands while it is asked */
#define ST_WANT			0		/* not answered yet */
#define ST_OK			1		/* addresses */
#define ST_NONE			2		/* no such name, or none of the kind */
#define ST_DONE			3		/* not asked: in the cache, or not wanted */
#define ST_TRUNC		4		/* the UDP answer was cut short: asked again over TCP */

typedef struct {
	char	name[DNS_NAME_MAX + 1];	/* "" free */
	INT	kind;
	UD	until_ns;		/* monotonic time it holds until */
	UB	(*addr)[16];		/* n of them, a block of its own */
	INT	n;			/* 0: no such name, or none of the kind */
	UINT	used;
} DNS_ENT;

/* What one kind has come to */
typedef struct {
	INT	st;
	UH	id;
	UB	(*addr)[16];		/* n of them, room for cap */
	INT	n, cap;
	UINT	ttl;
	BOOL	trunc;			/* the last UDP answer was cut short */
} KIND;

LOCAL DNS_ENT	*dns_cache;
LOCAL T_SPLOCK	dns_lock;
LOCAL UINT	dns_clock = 0;

LOCAL CONST UH	kind_type[KINDS] = { T_A, T_AAAA };
LOCAL CONST INT	kind_len[KINDS] = { 4, 16 };

LOCAL UD now_ns( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns;
}

LOCAL char lower( char c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (char)( c - 'A' + 'a' ) : c;
}

LOCAL BOOL same_name( CONST char *a, CONST char *b )
{
	while ( *a != '\0' && lower(*a) == lower(*b) ) {
		a++;
		b++;
	}
	return ( *a == '\0' && *b == '\0' );
}

/* ---------------------------------------------------------------- the addresses of a kind */

/* Room for n addresses in k, twice as much as before when it grows; FALSE when there is no memory */
LOCAL BOOL kind_room( KIND *k, INT n )
{
	UB	(*a)[16];
	INT	cap;

	if ( n <= k->cap ) {
		return TRUE;
	}
	cap = ( k->cap > 0 ) ? k->cap * 2 : 8;
	if ( cap < n ) {
		cap = n;
	}
	a = (UB (*)[16])Krealloc(k->addr, (size_t)cap * 16);
	if ( a == NULL ) {
		tm_printf((UB *)"net: DNS: no memory for %d addresses\n", n);
		return FALSE;
	}
	k->addr = a;
	k->cap = cap;
	return TRUE;
}

LOCAL void kind_free( KIND *k )
{
	if ( k->addr != NULL ) {
		Kfree(k->addr);
	}
	k->addr = NULL;
	k->n = k->cap = 0;
}

/* ---------------------------------------------------------------- the cache */

/*
 * The kind of name from the cache into k. The addresses are copied
 * while the cache is held; when k has too little room for them, the
 * room is made with the cache let go and the cache looked at again.
 */
LOCAL BOOL cache_get( CONST char *name, INT kind, KIND *k )
{
	UD	now = now_ns();
	UINT	imask;
	INT	i, need;
	BOOL	hit;

	if ( dns_cache == NULL ) {
		return FALSE;
	}
	for ( ;; ) {
		hit = FALSE;
		need = -1;
		ISpinLock(&dns_lock, &imask);
		for ( i = 0; i < DNS_CACHE; i++ ) {
			DNS_ENT	*e = &dns_cache[i];

			if ( e->name[0] != '\0' && e->kind == kind && e->until_ns > now && same_name(e->name, name) ) {
				if ( e->n > k->cap ) {
					need = e->n;
					break;
				}
				if ( e->n > 0 ) {
					knl_memcpy(k->addr, e->addr, (SZ)e->n * 16);
				}
				k->n = e->n;
				k->st = ( e->n > 0 ) ? ST_OK : ST_NONE;
				e->used = ++dns_clock;
				hit = TRUE;
				break;
			}
		}
		ISpinUnlock(&dns_lock, &imask);
		if ( need < 0 ) {
			return hit;
		}
		if ( !kind_room(k, need) ) {
			return FALSE;		/* asked of the servers instead */
		}
	}
}

LOCAL void cache_put( CONST char *name, INT kind, CONST KIND *k, UINT ttl_s )
{
	DNS_ENT	*e = NULL;
	UB	(*a)[16] = NULL, (*old)[16];
	UINT	imask;
	INT	i, len;

	if ( dns_cache == NULL ) {
		return;
	}
	for ( len = 0; name[len] != '\0'; len++ ) ;
	if ( len > DNS_NAME_MAX ) {
		return;
	}
	if ( ttl_s > DNS_TTL_MAX_S ) {
		ttl_s = DNS_TTL_MAX_S;
	}
	if ( k->n > 0 ) {
		a = (UB (*)[16])Kmalloc((size_t)k->n * 16);
		if ( a == NULL ) {
			tm_printf((UB *)"net: DNS: no memory to keep the %d addresses of %s\n", k->n, name);
			return;
		}
		knl_memcpy(a, k->addr, (SZ)k->n * 16);
	}
	ISpinLock(&dns_lock, &imask);
	for ( i = 0; i < DNS_CACHE; i++ ) {
		if ( dns_cache[i].name[0] != '\0' && dns_cache[i].kind == kind
		  && same_name(dns_cache[i].name, name) ) {
			e = &dns_cache[i];
			break;
		}
		if ( e == NULL || dns_cache[i].used < e->used ) {
			e = &dns_cache[i];	/* the one used longest ago */
		}
	}
	knl_memcpy(e->name, name, len + 1);
	e->kind = kind;
	old = e->addr;
	e->addr = a;
	e->n = k->n;
	e->until_ns = now_ns() + (UD)ttl_s * 1000000000ULL;
	e->used = ++dns_clock;
	ISpinUnlock(&dns_lock, &imask);
	if ( old != NULL ) {
		Kfree(old);
	}
}

/* ---------------------------------------------------------------- messages */

LOCAL UH get16( CONST UB *p )
{
	return (UH)( ( p[0] << 8 ) | p[1] );
}

LOCAL UW get32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

/* The question of one type for name; its length, or 0 when the name will not go */
LOCAL INT make_query( UB *m, UH id, CONST char *name, UH type )
{
	INT	at = 12, label, i;

	knl_memset(m, 0, 12);
	m[0] = (UB)( id >> 8 );
	m[1] = (UB)id;
	m[2] = 0x01;			/* a standard query, recursion desired */
	m[5] = 1;			/* one question */

	for ( i = 0; name[i] != '\0'; ) {
		label = at++;
		while ( name[i] != '\0' && name[i] != '.' ) {
			if ( at >= DNS_MSG_MAX - 5 || at - label > 63 ) {
				return 0;
			}
			m[at++] = (UB)name[i++];
		}
		if ( at - label - 1 == 0 ) {
			return 0;	/* an empty label */
		}
		m[label] = (UB)( at - label - 1 );
		if ( name[i] == '.' ) {
			i++;
		}
	}
	m[at++] = 0;
	m[at++] = (UB)( type >> 8 ); m[at++] = (UB)type;
	m[at++] = 0; m[at++] = C_IN;
	return at;
}

/* Past a name in a message; where it ends, or -1 */
LOCAL INT skip_name( CONST UB *m, INT len, INT at )
{
	while ( at < len ) {
		if ( m[at] == 0 ) {
			return at + 1;
		}
		if ( ( m[at] & 0xc0 ) == 0xc0 ) {
			return ( at + 2 <= len ) ? at + 2 : -1;
		}
		at += m[at] + 1;
	}
	return -1;
}

/*
 * An answer read into the kind it answers: ST_OK with the addresses,
 * ST_NONE when the name does not exist or has none of the kind,
 * ST_TRUNC when the answer was cut short to fit (TC: asked again over
 * TCP), E_IO when this server could not say, E_NOMEM when there was no
 * room for the addresses. ttl is the shortest of the records used.
 */
LOCAL INT read_answer( CONST UB *m, INT len, INT kind, KIND *k )
{
	INT	at, i, qd, an, n = 0;
	UINT	ttl = DNS_TTL_MAX_S;
	UH	type, class, rdlen;

	if ( len < 12 || ( m[2] & 0x80 ) == 0 ) {
		return E_IO;
	}
	if ( m[2] & 0x02 ) {
		return ST_TRUNC;
	}
	switch ( m[3] & 0x0f ) {
	case 0:		break;
	case 3:		k->n = 0; k->ttl = DNS_TTL_NONAME_S; return ST_NONE;
	default:	return E_IO;
	}
	qd = get16(m + 4);
	an = get16(m + 6);
	at = 12;
	for ( i = 0; i < qd; i++ ) {
		at = skip_name(m, len, at);
		if ( at < 0 || at + 4 > len ) {
			return E_IO;
		}
		at += 4;
	}
	for ( i = 0; i < an; i++ ) {
		at = skip_name(m, len, at);
		if ( at < 0 || at + 10 > len ) {
			return E_IO;
		}
		type = get16(m + at);
		class = get16(m + at + 2);
		rdlen = get16(m + at + 8);
		if ( at + 10 + rdlen > len ) {
			return E_IO;
		}
		if ( type == kind_type[kind] && class == C_IN && rdlen == kind_len[kind] ) {
			if ( !kind_room(k, n + 1) ) {
				return E_NOMEM;
			}
			knl_memset(k->addr[n], 0, 16);
			knl_memcpy(k->addr[n], m + at + 10, rdlen);	/* stays in network byte order */
			n++;
			if ( get32(m + at + 4) < ttl ) {
				ttl = get32(m + at + 4);
			}
		}
		at += 10 + rdlen;
	}
	k->n = n;
	k->ttl = ( n > 0 ) ? ttl : DNS_TTL_NONAME_S;
	return ( n > 0 ) ? ST_OK : ST_NONE;
}

/* ---------------------------------------------------------------- asking */

LOCAL BOOL same16( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 16 && a[i] == b[i]; i++ ) ;
	return ( i == 16 );
}

/* Room for n servers in *srv of *cap; FALSE when there is no memory */
LOCAL BOOL srv_room( T_SOADDR **srv, INT *cap, INT n )
{
	T_SOADDR	*l;
	INT		c;

	if ( n <= *cap ) {
		return TRUE;
	}
	c = ( *cap > 0 ) ? *cap * 2 : 4;
	l = (T_SOADDR *)Krealloc(*srv, (size_t)c * sizeof(T_SOADDR));
	if ( l == NULL ) {
		tm_printf((UB *)"net: DNS: no memory for the list of %d name servers\n", n);
		return FALSE;
	}
	*srv = l;
	*cap = c;
	return TRUE;
}

/*
 * The servers to ask, IPv4 ones first, every one there is, into a list
 * of their own (*p_srv, freed by the caller); how many
 */
LOCAL INT servers( T_SOADDR **p_srv )
{
	T_SOADDR	*srv = NULL;
	UB		a6[16];
	UW		a;
	INT		i, j, n = 0, cap = 0;
	BOOL		dup;

	for ( i = 0; ( a = knl_so_dns_server(i) ) != 0; i++ ) {
		for ( dup = FALSE, j = 0; j < n; j++ ) {
			if ( srv[j].family == AF_INET && *(UW *)srv[j].addr == a ) dup = TRUE;
		}
		if ( dup ) {
			continue;
		}
		if ( !srv_room(&srv, &cap, n + 1) ) {
			break;
		}
		knl_memset(&srv[n], 0, sizeof(srv[n]));
		srv[n].family = AF_INET;
		knl_memcpy(srv[n].addr, &a, 4);
		n++;
	}
	for ( i = 0; so_getdns6(i, a6) == E_OK; i++ ) {
		for ( dup = FALSE, j = 0; j < n; j++ ) {
			if ( srv[j].family == AF_INET6 && same16(srv[j].addr, a6) ) dup = TRUE;
		}
		if ( dup ) {
			continue;
		}
		if ( !srv_room(&srv, &cap, n + 1) ) {
			break;
		}
		knl_memset(&srv[n], 0, sizeof(srv[n]));
		srv[n].family = AF_INET6;
		knl_memcpy(srv[n].addr, a6, 16);
		if ( a6[0] == 0xfe && ( a6[1] & 0xc0 ) == 0x80 ) {
			srv[n].scope = so_ifnametoindex(SO_IFNAME);	/* a link-local server */
		}
		n++;
	}
	*p_srv = srv;
	return n;
}

/* The socket to ask a server of 'family' over, made when it is first needed */
LOCAL INT sock_for( INT family, INT *s4, INT *s6 )
{
	INT	*p = ( family == AF_INET6 ) ? s6 : s4;

	if ( *p < 0 ) {
		*p = so_socket(family, SOCK_DGRAM, 0);
	}
	return *p;
}

typedef union {
	struct sockaddr_in	in;
	struct sockaddr_in6	in6;
} SRVADDR;

/* The socket address of a server's port 53; its length */
LOCAL socklen_t server_to( CONST T_SOADDR *server, SRVADDR *to )
{
	knl_memset(to, 0, sizeof(*to));
	if ( server->family == AF_INET6 ) {
		to->in6.sin6_len = sizeof(to->in6);
		to->in6.sin6_family = AF_INET6;
		to->in6.sin6_port = so_htons(DNS_PORT);
		knl_memcpy(&to->in6.sin6_addr, server->addr, 16);
		to->in6.sin6_scope_id = server->scope;
		return sizeof(to->in6);
	}
	to->in.sin_len = sizeof(to->in);
	to->in.sin_family = AF_INET;
	to->in.sin_port = so_htons(DNS_PORT);
	knl_memcpy(&to->in.sin_addr.s_addr, server->addr, 4);
	return sizeof(to->in);
}

/*
 * The kinds still wanted, asked of one server together, waiting up to
 * ms for all their answers. Each answered kind is ST_OK or ST_NONE
 * after; a server that could not say leaves it ST_WANT, and one whose
 * answer was cut short leaves it ST_WANT with trunc set. E_IO when the
 * question could not be sent at all, E_NOMEM when there was no room
 * for an answer's addresses.
 */
LOCAL ER ask( INT s, CONST T_SOADDR *server, CONST char *name, KIND *k, INT ms )
{
	SRVADDR		to;
	socklen_t	tolen;
	struct pollfd	p;
	UB		m[DNS_MSG_MAX];
	INT		n, kind, left = 0, st;
	UD		until = now_ns() + (UD)ms * 1000000ULL, now;
	BOOL		asked[KINDS];
	ER		er = E_OK;

	tolen = server_to(server, &to);
	for ( kind = 0; kind < KINDS; kind++ ) {
		asked[kind] = FALSE;
		if ( k[kind].st != ST_WANT ) {
			continue;
		}
		(void)ts_get_random(&k[kind].id, sizeof(k[kind].id));
		if ( kind > 0 && k[kind].id == k[0].id ) {
			k[kind].id++;
		}
		n = make_query(m, k[kind].id, name, kind_type[kind]);
		if ( n == 0 ) {
			return E_PAR;
		}
		if ( so_sendto(s, m, n, 0, (struct sockaddr *)&to, tolen) == n ) {
			asked[kind] = TRUE;
			left++;
		}
	}
	if ( left == 0 ) {
		return E_IO;
	}
	while ( left > 0 ) {
		now = now_ns();
		if ( now >= until ) {
			return E_TMOUT;
		}
		p.fd = s;
		p.events = POLLIN;
		p.revents = 0;
		if ( so_poll(&p, 1, (TMO)( ( until - now ) / 1000000ULL + 1 )) <= 0 ) {
			continue;
		}
		n = so_recv(s, m, sizeof(m), MSG_DONTWAIT);
		if ( n < 12 ) {
			continue;
		}
		for ( kind = 0; kind < KINDS; kind++ ) {
			if ( asked[kind] && get16(m) == k[kind].id ) break;
		}
		if ( kind == KINDS ) {
			continue;		/* not the answer to these questions */
		}
		asked[kind] = FALSE;
		left--;
		st = read_answer(m, n, kind, &k[kind]);
		if ( st == ST_OK || st == ST_NONE ) {
			k[kind].st = st;
		} else if ( st == ST_TRUNC ) {
			k[kind].trunc = TRUE;
		} else if ( st == E_NOMEM ) {
			er = E_NOMEM;
		}
		/* one answered: the other is not waited for long, a server may never answer it */
		now = now_ns();
		if ( left > 0 && until > now + (UD)DNS_OTHER_MS * 1000000ULL ) {
			until = now + (UD)DNS_OTHER_MS * 1000000ULL;
		}
	}
	return er;
}

/* All of len bytes sent (out) or received over a connection that does not block, before 'until' */
LOCAL BOOL tcp_all( INT s, UB *b, INT len, UD until, BOOL out )
{
	struct pollfd	p;
	INT		done = 0, r;
	UD		now;

	while ( done < len ) {
		now = now_ns();
		if ( now >= until ) {
			return FALSE;
		}
		p.fd = s;
		p.events = out ? POLLOUT : POLLIN;
		p.revents = 0;
		r = so_poll(&p, 1, (TMO)( ( until - now ) / 1000000ULL + 1 ));
		if ( r < 0 ) {
			return FALSE;
		}
		if ( r == 0 ) {
			continue;
		}
		r = out ? so_send(s, b + done, len - done, 0) : so_recv(s, b + done, len - done, 0);
		if ( r == E_TMOUT ) {
			continue;		/* not ready after all */
		}
		if ( r <= 0 ) {
			return FALSE;
		}
		done += r;
	}
	return TRUE;
}

/*
 * One kind asked of a server again over TCP, its answer having been cut
 * short over UDP (RFC 7766 5): a message of up to 65535 bytes, after
 * its two bytes of length (RFC 1035 4.2.2). The kind is ST_OK or
 * ST_NONE after when the server answered.
 */
LOCAL ER ask_tcp( CONST T_SOADDR *server, CONST char *name, INT kind, KIND *k )
{
	SRVADDR		to;
	socklen_t	tolen;
	UB		q[2 + DNS_MSG_MAX], hd[2], *m = NULL;
	UD		until = now_ns() + (UD)DNS_TCP_MS * 1000000ULL;
	INT		s, n, len, st;
	ER		er;

	tolen = server_to(server, &to);
	(void)ts_get_random(&k->id, sizeof(k->id));
	n = make_query(q + 2, k->id, name, kind_type[kind]);
	if ( n == 0 ) {
		return E_PAR;
	}
	q[0] = (UB)( n >> 8 );
	q[1] = (UB)n;
	s = so_socket(server->family, SOCK_STREAM, 0);
	if ( s < 0 ) {
		return E_IO;
	}
	(void)so_fcntl(s, F_SETFL, O_NONBLOCK);
	er = so_connect(s, (struct sockaddr *)&to, tolen);
	if ( er < E_OK && er != E_BUSY ) {
		goto out;		/* E_BUSY: the connection is being made, sending waits for it */
	}
	er = E_TMOUT;
	if ( !tcp_all(s, q, n + 2, until, TRUE) || !tcp_all(s, hd, 2, until, FALSE) ) {
		goto out;
	}
	len = get16(hd);
	m = (UB *)Kmalloc((size_t)( len > 0 ? len : 1 ));
	if ( m == NULL ) {
		tm_printf((UB *)"net: DNS: no memory for an answer of %d bytes\n", len);
		er = E_NOMEM;
		goto out;
	}
	if ( !tcp_all(s, m, len, until, FALSE) ) {
		goto out;
	}
	er = E_IO;
	if ( len < 12 || get16(m) != k->id ) {
		goto out;
	}
	st = read_answer(m, len, kind, k);
	if ( st == ST_OK || st == ST_NONE ) {
		k->st = st;
		er = E_OK;
	} else if ( st == E_NOMEM ) {
		er = E_NOMEM;
	}
out:
	so_close(s);
	if ( m != NULL ) {
		Kfree(m);
	}
	return er;
}

LOCAL void cache_ready( void )
{
	DNS_ENT	*c;
	UINT	imask;

	if ( dns_cache != NULL ) {
		return;
	}
	c = (DNS_ENT *)Kcalloc(DNS_CACHE, sizeof(DNS_ENT));
	ISpinLock(&dns_lock, &imask);
	if ( dns_cache == NULL ) {
		dns_cache = c;
		c = NULL;
	}
	ISpinUnlock(&dns_lock, &imask);
	if ( c != NULL ) {
		Kfree(c);
	}
}

/*
 * The addresses of name of the family asked (AF_INET, AF_INET6, or
 * AF_UNSPEC for both): all of them, the IPv4 ones first, in a list of
 * their own (*p_addrs, freed by the caller with Kfree), how many in
 * *p_n. E_NOEXS when there is no such name or it has no address of the
 * family, E_TMOUT when no server answered, E_IO when there is no
 * server, E_NOMEM when there was no room for the addresses.
 */
EXPORT ER knl_so_dns_query( CONST char *name, INT family, T_SOADDR **p_addrs, INT *p_n )
{
	T_SOADDR	*srv = NULL, *addrs;
	KIND		k[KINDS];
	INT		s4 = -1, s6 = -1, s, round, i, j, kind, nsrv, n = 0, all;
	BOOL		pending = FALSE;
	ER		e, er = E_TMOUT;

	*p_n = 0;
	*p_addrs = NULL;
	if ( family != AF_INET && family != AF_INET6 && family != AF_UNSPEC ) {
		return E_NOSPT;
	}
	for ( kind = 0; kind < KINDS; kind++ ) {
		knl_memset(&k[kind], 0, sizeof(k[kind]));
		k[kind].st = ( family == AF_UNSPEC
			    || ( family == AF_INET && kind == K_A )
			    || ( family == AF_INET6 && kind == K_AAAA ) ) ? ST_WANT : ST_DONE;
	}

	if ( same_name(name, "localhost") ) {
		if ( k[K_A].st == ST_WANT ) {
			UW	lo = so_htonl(INADDR_LOOPBACK);

			if ( !kind_room(&k[K_A], 1) ) {
				er = E_NOMEM;
				goto out;
			}
			knl_memcpy(k[K_A].addr[0], &lo, 4);
			k[K_A].n = 1;
		}
		if ( k[K_AAAA].st == ST_WANT ) {
			if ( !kind_room(&k[K_AAAA], 1) ) {
				er = E_NOMEM;
				goto out;
			}
			knl_memset(k[K_AAAA].addr[0], 0, 16);
			k[K_AAAA].addr[0][15] = 1;
			k[K_AAAA].n = 1;
		}
		goto out;
	}
	cache_ready();
	for ( kind = 0; kind < KINDS; kind++ ) {
		if ( k[kind].st == ST_WANT ) {
			(void)cache_get(name, kind, &k[kind]);
		}
	}
	if ( k[K_A].st != ST_WANT && k[K_AAAA].st != ST_WANT ) {
		goto out;
	}

	nsrv = servers(&srv);
	if ( nsrv == 0 ) {
		er = E_IO;
		goto out;
	}
	for ( round = 0; round < DNS_ROUNDS; round++ ) {
		for ( i = 0; i < nsrv; i++ ) {
			s = sock_for(srv[i].family, &s4, &s6);
			if ( s < 0 ) {
				continue;
			}
			e = ask(s, &srv[i], name, k, DNS_WAIT_MS << round);
			if ( e == E_PAR ) {
				er = E_PAR;
				goto done;
			}
			if ( e == E_IO || e == E_NOMEM ) {
				er = e;
			}
			for ( kind = 0; kind < KINDS; kind++ ) {
				if ( !k[kind].trunc || k[kind].st != ST_WANT ) continue;
				k[kind].trunc = FALSE;
				e = ask_tcp(&srv[i], name, kind, &k[kind]);
				if ( e == E_PAR ) {
					er = E_PAR;
					goto done;
				}
				if ( e == E_IO || e == E_NOMEM ) {
					er = e;
				}
			}
			if ( ( k[K_A].st != ST_WANT && k[K_AAAA].st != ST_WANT )
			  || k[K_A].st == ST_OK || k[K_AAAA].st == ST_OK ) {
				goto done;	/* all answered, or addresses of one kind to go on with */
			}
		}
	}
done:
	if ( s4 >= 0 ) so_close(s4);
	if ( s6 >= 0 ) so_close(s6);
	if ( srv != NULL ) Kfree(srv);
	for ( kind = 0; kind < KINDS; kind++ ) {
		if ( k[kind].ttl != 0 && ( k[kind].st == ST_OK || k[kind].st == ST_NONE ) ) {
			cache_put(name, kind, &k[kind], k[kind].ttl);
		}
	}
out:
	for ( all = kind = 0; kind < KINDS; kind++ ) {
		if ( !( k[kind].st == ST_WANT && k[kind].n == 0 ) ) all += k[kind].n;
	}
	addrs = NULL;
	if ( all > 0 ) {
		addrs = (T_SOADDR *)Kmalloc((size_t)all * sizeof(T_SOADDR));
		if ( addrs == NULL ) {
			tm_printf((UB *)"net: DNS: no memory for the %d addresses of %s\n", all, name);
			for ( kind = 0; kind < KINDS; kind++ ) kind_free(&k[kind]);
			return E_NOMEM;
		}
	}
	for ( kind = 0; kind < KINDS; kind++ ) {
		if ( k[kind].st == ST_WANT && k[kind].n == 0 ) {
			pending = TRUE;			/* no server said */
			continue;
		}
		for ( j = 0; j < k[kind].n; j++ ) {
			knl_memset(&addrs[n], 0, sizeof(addrs[n]));
			addrs[n].family = ( kind == K_A ) ? AF_INET : AF_INET6;
			knl_memcpy(addrs[n].addr, k[kind].addr[j], kind_len[kind]);
			n++;
		}
	}
	for ( kind = 0; kind < KINDS; kind++ ) kind_free(&k[kind]);
	*p_n = n;
	*p_addrs = addrs;
	if ( n > 0 ) {
		return E_OK;
	}
	if ( !pending && er != E_PAR ) {
		return E_NOEXS;			/* every kind asked was answered: there is none */
	}
	return er;
}
