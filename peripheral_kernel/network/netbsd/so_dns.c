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
 *	Names looked up through a DNS server (RFC 1035; design 12.6).
 *
 *	A stub resolver: one question for the IPv4 addresses (type A) of a
 *	name goes to the name server over UDP, and the addresses in the
 *	answer come back, following whatever aliases (CNAME) the server put
 *	in with them. The server is the one the settings name, else the one
 *	that came with the address (so_api.c). Each server is asked in turn,
 *	four rounds, waiting twice as long each round (1, 2, 4 and 8 s).
 *
 *	Answers are kept for as long as they said they hold, up to five
 *	minutes, so that a page that asks for the same host again and again
 *	asks the network once. A name that does not exist is kept for half a
 *	minute. "localhost" is always 127.0.0.1.
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
#define DNS_MSG_MAX		512		/* a UDP answer */
#define DNS_NAME_MAX		255
#define DNS_CACHE		64
#define DNS_CACHE_ADDRS		4
#define DNS_TTL_MAX_S		300
#define DNS_TTL_NONAME_S	30

#define T_A			1
#define T_CNAME			5
#define C_IN			1

typedef struct {
	char	name[DNS_NAME_MAX + 1];	/* "" free */
	UD	until_ns;		/* monotonic time it holds until */
	UW	addr[DNS_CACHE_ADDRS];
	INT	n;			/* 0: the name does not exist */
	UINT	used;
} DNS_ENT;

LOCAL DNS_ENT	*dns_cache;
LOCAL T_SPLOCK	dns_lock;
LOCAL UINT	dns_clock = 0;

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

/* ---------------------------------------------------------------- the cache */

LOCAL BOOL cache_get( CONST char *name, UW *addrs, INT max, INT *p_n )
{
	UD	now = now_ns();
	UINT	imask;
	INT	i, k;
	BOOL	hit = FALSE;

	if ( dns_cache == NULL ) {
		return FALSE;
	}
	ISpinLock(&dns_lock, &imask);
	for ( i = 0; i < DNS_CACHE; i++ ) {
		DNS_ENT	*e = &dns_cache[i];

		if ( e->name[0] != '\0' && e->until_ns > now && same_name(e->name, name) ) {
			for ( k = 0; k < e->n && k < max; k++ ) {
				addrs[k] = e->addr[k];
			}
			*p_n = k;
			e->used = ++dns_clock;
			hit = TRUE;
			break;
		}
	}
	ISpinUnlock(&dns_lock, &imask);
	return hit;
}

LOCAL void cache_put( CONST char *name, CONST UW *addrs, INT n, UINT ttl_s )
{
	DNS_ENT	*e = NULL;
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
	ISpinLock(&dns_lock, &imask);
	for ( i = 0; i < DNS_CACHE; i++ ) {
		if ( dns_cache[i].name[0] != '\0' && same_name(dns_cache[i].name, name) ) {
			e = &dns_cache[i];
			break;
		}
		if ( e == NULL || dns_cache[i].used < e->used ) {
			e = &dns_cache[i];	/* the one used longest ago */
		}
	}
	knl_memcpy(e->name, name, len + 1);
	e->n = ( n > DNS_CACHE_ADDRS ) ? DNS_CACHE_ADDRS : n;
	for ( i = 0; i < e->n; i++ ) {
		e->addr[i] = addrs[i];
	}
	e->until_ns = now_ns() + (UD)ttl_s * 1000000000ULL;
	e->used = ++dns_clock;
	ISpinUnlock(&dns_lock, &imask);
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

/* The question for the addresses of name; its length, or 0 when the name will not go */
LOCAL INT make_query( UB *m, UH id, CONST char *name )
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
	m[at++] = 0; m[at++] = T_A;
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
 * The answer read: E_OK with the addresses, E_NOEXS when the name does
 * not exist, E_IO when this server could not say. ttl is the shortest
 * of the records used.
 */
LOCAL ER read_answer( CONST UB *m, INT len, UH id, UW *addrs, INT max, INT *p_n, UINT *p_ttl )
{
	INT	at, i, qd, an, n = 0;
	UINT	ttl = DNS_TTL_MAX_S;
	UH	type, class, rdlen;

	if ( len < 12 || get16(m) != id || ( m[2] & 0x80 ) == 0 ) {
		return E_IO;
	}
	switch ( m[3] & 0x0f ) {
	case 0:		break;
	case 3:		*p_ttl = DNS_TTL_NONAME_S; return E_NOEXS;
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
		if ( type == T_A && class == C_IN && rdlen == 4 && n < max ) {
			UW	a;

			knl_memcpy(&a, m + at + 10, 4);	/* stays in network byte order */
			addrs[n++] = a;
			if ( get32(m + at + 4) < ttl ) {
				ttl = get32(m + at + 4);
			}
		}
		at += 10 + rdlen;
	}
	*p_n = n;
	*p_ttl = ttl;
	return ( n > 0 ) ? E_OK : E_NOEXS;
}

/* ---------------------------------------------------------------- asking */

/* One question to one server, waiting up to ms for its answer */
LOCAL ER ask( INT s, UW server, CONST UB *q, INT qlen, UH id, INT ms,
	      UW *addrs, INT max, INT *p_n, UINT *p_ttl )
{
	struct sockaddr_in	to;
	struct pollfd		p;
	UB			m[DNS_MSG_MAX];
	INT			n;
	UD			until = now_ns() + (UD)ms * 1000000ULL, now;
	ER			er;

	knl_memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_port = so_htons(DNS_PORT);
	to.sin_addr.s_addr = server;
	if ( so_sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof(to)) != qlen ) {
		return E_IO;
	}
	for ( ;; ) {
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
		if ( n <= 0 ) {
			continue;
		}
		er = read_answer(m, n, id, addrs, max, p_n, p_ttl);
		if ( er == E_IO && ( n < 2 || get16(m) != id ) ) {
			continue;		/* not the answer to this question */
		}
		return er;
	}
}

/*
 * The IPv4 addresses of name, in network byte order: up to max of them
 * into addrs, how many in *p_n. E_NOEXS when there is no such name,
 * E_TMOUT when no server answered, E_IO when there is no server.
 */
EXPORT ER knl_so_dns_query( CONST char *name, INT family, UW *addrs, INT max, INT *p_n )
{
	UB	q[DNS_MSG_MAX];
	UW	server[2];
	UINT	ttl = 0;
	UH	id;
	INT	s, qlen, round, i, nsrv = 0;
	ER	er = E_TMOUT, e;

	*p_n = 0;
	if ( family != AF_INET || max <= 0 ) {
		return E_NOSPT;
	}
	if ( same_name(name, "localhost") ) {
		addrs[0] = so_htonl(INADDR_LOOPBACK);
		*p_n = 1;
		return E_OK;
	}
	if ( dns_cache == NULL ) {
		DNS_ENT	*c = (DNS_ENT *)Kcalloc(DNS_CACHE, sizeof(DNS_ENT));
		UINT	imask;

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
	if ( cache_get(name, addrs, max, p_n) ) {
		return ( *p_n > 0 ) ? E_OK : E_NOEXS;
	}

	for ( i = 0; i < 2; i++ ) {
		UW	a = knl_so_dns_server(i);

		if ( a != 0 && ( nsrv == 0 || server[0] != a ) ) {
			server[nsrv++] = a;
		}
	}
	if ( nsrv == 0 ) {
		return E_IO;
	}
	(void)ts_get_random(&id, sizeof(id));
	qlen = make_query(q, id, name);
	if ( qlen == 0 ) {
		return E_PAR;
	}
	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	if ( s < 0 ) {
		return (ER)s;
	}
	for ( round = 0; round < DNS_ROUNDS; round++ ) {
		for ( i = 0; i < nsrv; i++ ) {
			e = ask(s, server[i], q, qlen, id, DNS_WAIT_MS << round,
				addrs, max, p_n, &ttl);
			if ( e == E_OK || e == E_NOEXS ) {
				er = e;
				goto done;
			}
			if ( e == E_IO ) {
				er = E_IO;
			}
		}
	}
done:
	so_close(s);
	if ( er == E_OK ) {
		cache_put(name, addrs, *p_n, ttl);
	} else if ( er == E_NOEXS ) {
		cache_put(name, addrs, 0, DNS_TTL_NONAME_S);
	}
	return er;
}
