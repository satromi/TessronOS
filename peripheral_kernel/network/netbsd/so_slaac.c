/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_slaac.c
 *	IPv6 addresses from the routers on the link (stateless address
 *	autoconfiguration: RFC 4861, RFC 4862, RFC 8106, RFC 8981;
 *	design 9.3.3).
 *
 *	The NetBSD stack gives the card's interface its link-local address
 *	and leaves the router advertisements to a program, as NetBSD itself
 *	does: they come to a raw ICMPv6 socket here. A task of its own asks
 *	the routers to speak (three router solicitations, four seconds
 *	apart, once the link-local address has been checked for duplicates,
 *	and again when the cable comes back or the adapters' manager says the
 *	link came up again: the Wi-Fi joined a network, the card in use
 *	changed; knl_so_slaac_relink) and listens until it is
 *	stopped (the settings say IPv6 is not to take addresses from the
 *	routers) or the stack goes down.
 *
 *	An advertisement is taken only from a link-local address with a hop
 *	limit of 255, which no router beyond the link can send. Each prefix
 *	it marks for autoconfiguration, of 64 bits, gives an address: the
 *	prefix and the interface identifier of the link-local address. The
 *	address goes on the interface with the prefix's lifetimes; the
 *	kernel checks it for duplicates before it is used, stops choosing it
 *	when its preferred lifetime has run out and takes it away when its
 *	valid lifetime has. A prefix advertised again renews the address,
 *	never shortening what is left to less than two hours unless less was
 *	left already, so that a forged advertisement cannot take an address
 *	away. A router with a lifetime is a default router; the default route
 *	goes through the first of them still alive. The name servers an
 *	advertisement carries (RDNSS) are kept for their lifetime and asked
 *	after the IPv4 ones (so_dns.c).
 *
 *	With temporary addresses asked for, each prefix gives as well an
 *	address with a random interface identifier, valid for at most two
 *	days and preferred for at most one day less a random part of up to
 *	four tenths of it, never longer than the prefix's own lifetimes. A
 *	new one is made five seconds before the one in use stops being
 *	preferred, and the old one stays until it is no longer valid, so
 *	that the connections on it can finish. One that turns out to be
 *	another machine's is made again, at most three times. The kernel is
 *	told to choose a temporary address first as the source of a new
 *	connection (so_api.c).
 *
 *	All of the tables are worked by the task alone while it runs;
 *	stopping it waits until it has stopped, so that whoever stopped it
 *	may then take the addresses away.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include <ts/time.h>
#include <ts/uuid.h>
#include "tstdlib.h"
#include "so_nb.h"

#define SL_TASK_PRI		30
#define SL_TASK_STKSZ		(8 * 1024)
#define SL_SLICE_MS		1000		/* how often the lifetimes are looked at */
#define SL_STOP_MS		5000		/* the longest a stop waits for the task */
#define RS_COUNT		3
#define RS_INTERVAL_MS		4000
#define LL_WAIT_MS		10000		/* the link-local address checked for duplicates */
#define TWO_HOURS_S		7200
#define LIFE_INFINITE		0xffffffffU
#define UNTIL_NEVER		(~(UD)0)
#define NS_PER_S		1000000000ULL

#define ND_ROUTER_SOLICIT	133
#define ND_ROUTER_ADVERT	134
#define OPT_SOURCE_LLA		1
#define OPT_PREFIX		3
#define OPT_RDNSS		25
#define PI_ONLINK		0x80
#define PI_AUTO			0x40

#define ROUTERS			3
#define PREFIXES		4
#define SERVERS			3

/* Temporary addresses (RFC 8981 3.8) */
#define TEMPS			( PREFIXES * 2 )
#define TEMP_VALID_S		( 2 * 24 * 3600 )
#define TEMP_PREFERRED_S	( 24 * 3600 )
#define MAX_DESYNC_S		( TEMP_PREFERRED_S * 2 / 5 )
#define REGEN_ADVANCE_S		5		/* 2 + retries x transmits x 1 s */
#define TEMP_IDGEN_RETRIES	3
#define TEMP_CHECK_S		10		/* how long a new one is watched for a duplicate */
#define TEMP_RETRY_S		60		/* after one could not be put on the interface */

/* A router or a name server, and when it stops being one (monotonic ns) */
typedef struct {
	BOOL	used;
	UB	addr[16];
	UD	until;
} TIMED;

/* An address made from a prefix */
typedef struct {
	BOOL	used;
	UB	addr[16];
	INT	plen;
	UD	until;			/* when its valid lifetime runs out */
	UD	pref_until;		/* its preferred lifetime */
	UD	temp_try;		/* no temporary address is tried before then */
} MADE;

/* A temporary address of a prefix */
typedef struct {
	BOOL	used;
	BOOL	current;		/* the one in use; FALSE once another took its place */
	BOOL	checked;		/* no longer tentative */
	UB	addr[16];
	INT	from;			/* the sl_made it was made from */
	INT	tries;
	UD	made;
	UD	until, pref_until;	/* as set on the interface */
	UD	cap_until, cap_pref;	/* the most it may have, from when it was made */
} TEMP;

LOCAL TIMED		sl_rtr[ROUTERS];
LOCAL TIMED		sl_ns[SERVERS];
LOCAL MADE		sl_made[PREFIXES];
LOCAL TEMP		sl_temp[TEMPS];
LOCAL UB		sl_gw[16];
LOCAL BOOL		sl_have_gw = FALSE;
LOCAL T_SPLOCK		sl_lock;		/* sl_ns, sl_gw and the task's state, which other tasks read */
LOCAL UB		sl_iid[8];		/* the interface identifier */
LOCAL ID		sl_tsk = 0;
LOCAL volatile BOOL	sl_want = FALSE;
LOCAL volatile BOOL	sl_running = FALSE;	/* the task is listening */
LOCAL volatile UINT	sl_gen = 0;
LOCAL volatile UINT	sl_relink = 0;		/* counted up when the link came up again */
LOCAL volatile UINT	sl_solicits = 0;	/* the router solicitations sent */
LOCAL volatile BOOL	sl_temp_want = FALSE;	/* temporary addresses asked for */
LOCAL BOOL		sl_temp_on = FALSE;	/* and being made */
LOCAL UW		sl_desync;		/* seconds taken off a temporary address's preferred lifetime */
LOCAL UB		sl_msg[1500];

LOCAL UD now_ns( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns;
}

LOCAL UD until_of( UW life_s, UD now )
{
	return ( life_s == LIFE_INFINITE ) ? UNTIL_NEVER : now + (UD)life_s * NS_PER_S;
}

/* Seconds left until 'until', LIFE_INFINITE for no end */
LOCAL UW left_of( UD until, UD now )
{
	UD	s;

	if ( until == UNTIL_NEVER ) {
		return LIFE_INFINITE;
	}
	if ( until <= now ) {
		return 0;
	}
	s = ( until - now ) / NS_PER_S;
	return ( s >= LIFE_INFINITE ) ? LIFE_INFINITE - 1 : (UW)s;
}

LOCAL UD min_ud( UD a, UD b )
{
	return ( a < b ) ? a : b;
}

LOCAL UW get32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

LOCAL BOOL same( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && a[i] == b[i]; i++ ) ;
	return ( i == n );
}

LOCAL BOOL link_local( CONST UB *a )
{
	return ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0x80 );
}

LOCAL void say( CONST char *what, CONST UB *a, UW life )
{
	char	t[48];

	knl_so_ip6_text(t, sizeof(t), a);
	if ( life == LIFE_INFINITE ) {
		tm_printf((UB *)"net: IPv6 %s %s\n", what, t);
	} else {
		tm_printf((UB *)"net: IPv6 %s %s (%d s)\n", what, t, (INT)life);
	}
}

/* ---------------------------------------------------------------- the interface */

/*
 * The interface identifier, from the link-local address once it is no
 * longer tentative; from the card's address (EUI-64) when the
 * interface has none. FALSE when the link-local address turned out to
 * be another machine's, or the asking was stopped.
 */
LOCAL BOOL take_iid( UINT gen )
{
	T_SOIF6	l[8];
	UB	mac[NET_MAC_LEN];
	INT	n, i, t;

	for ( t = 0; t < LL_WAIT_MS; t += 200 ) {
		if ( sl_gen != gen ) {
			return FALSE;
		}
		n = rump_tsnet_addrs6(SO_IFNAME, l, 8);
		for ( i = 0; i < n && i < 8; i++ ) {
			if ( !link_local(l[i].addr) ) continue;
			if ( l[i].flags & SO_IF6_DUPLICATED ) {
				tm_printf((UB *)"net: IPv6 link-local address in use by another machine\n");
				return FALSE;
			}
			if ( ( l[i].flags & SO_IF6_TENTATIVE ) == 0 ) {
				knl_memcpy(sl_iid, l[i].addr + 8, 8);
				return TRUE;
			}
			break;
		}
		if ( n == 0 ) {
			break;			/* none will come */
		}
		(void)tk_dly_tsk(200);
	}
	if ( net_get_mac(mac) < E_OK ) {
		return FALSE;
	}
	sl_iid[0] = mac[0] ^ 0x02;
	sl_iid[1] = mac[1];
	sl_iid[2] = mac[2];
	sl_iid[3] = 0xff;
	sl_iid[4] = 0xfe;
	sl_iid[5] = mac[3];
	sl_iid[6] = mac[4];
	sl_iid[7] = mac[5];
	return TRUE;
}

/* The default route through the first router still alive, when that has changed */
LOCAL void route_again( void )
{
	UB	gw[16];
	BOOL	have = FALSE;
	UINT	imask;
	INT	i;

	for ( i = 0; i < ROUTERS && !have; i++ ) {
		if ( sl_rtr[i].used ) {
			knl_memcpy(gw, sl_rtr[i].addr, 16);
			have = TRUE;
		}
	}
	if ( have == sl_have_gw && ( !have || same(gw, sl_gw, 16) ) ) {
		return;
	}
	if ( rump_tsnet_defroute6(SO_IFNAME, have ? gw : NULL) != 0 && have ) {
		tm_printf((UB *)"net: IPv6 default route not set\n");
		have = FALSE;
	}
	ISpinLock(&sl_lock, &imask);
	sl_have_gw = have;
	if ( have ) knl_memcpy(sl_gw, gw, 16);
	ISpinUnlock(&sl_lock, &imask);
	if ( have ) {
		say("default router", gw, LIFE_INFINITE);
	} else {
		tm_printf((UB *)"net: IPv6 no default router\n");
	}
}

/* ---------------------------------------------------------------- temporary addresses */

/*
 * A random interface identifier: not all zero, not the stable one, and
 * none of those kept for other uses (RFC 5453: the subnet-router
 * anycast of all zero, 0200:5eff:fe00:0000 to 0200:5eff:fe00:ffff and
 * fdff:ffff:ffff:ff80 to fdff:ffff:ffff:ffff).
 */
LOCAL BOOL temp_iid( UB *iid )
{
	static CONST UB	eth[6] = { 0x02, 0x00, 0x5e, 0xff, 0xfe, 0x00 };
	static CONST UB	any[7] = { 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static CONST UB	zero[8];
	INT		t;

	for ( t = 0; t < 8; t++ ) {
		if ( ts_get_random(iid, 8) < E_OK ) {
			return FALSE;
		}
		if ( same(iid, zero, 8) || same(iid, sl_iid, 8) || same(iid, eth, 6)
		  || ( same(iid, any, 7) && iid[7] >= 0x80 ) ) {
			continue;
		}
		return TRUE;
	}
	return FALSE;
}

/* The lifetimes of a temporary address put on the interface again; FALSE when it has none left */
LOCAL BOOL temp_set( TEMP *p, UD until, UD pref_until, UD now, BOOL fresh )
{
	UW	vl = left_of(until, now), pl = left_of(pref_until, now);
	UINT	f = SO_IF6_AUTOCONF | SO_IF6_TEMPORARY;
	INT	e;

	if ( vl == 0 ) {
		return FALSE;
	}
	if ( pl > vl ) {
		pl = vl;
	}
	e = rump_tsnet_setaddr6(SO_IFNAME, p->addr, sl_made[p->from].plen, vl, pl, f);
	if ( e != 0 ) {
		if ( fresh ) tm_printf((UB *)"net: IPv6 temporary address not set (%d)\n", e);
		return FALSE;
	}
	p->until = until;
	p->pref_until = pref_until;
	return TRUE;
}

/* A temporary address taken off the interface */
LOCAL void temp_drop( TEMP *p )
{
	if ( p->used ) {
		(void)rump_tsnet_deladdr6(SO_IFNAME, p->addr);
		p->used = FALSE;
	}
}

/*
 * A new temporary address for the prefix sl_made[pi], in place of the
 * one in use (RFC 8981 3.4). None is made when what is left of the
 * prefix's preferred lifetime would not outlast the time it takes to
 * make the next one.
 */
LOCAL void temp_new( INT pi, INT tries, UD now )
{
	MADE	*m = &sl_made[pi];
	TEMP	*p = NULL;
	UD	cap_until = now + (UD)TEMP_VALID_S * NS_PER_S;
	UD	cap_pref = now + (UD)( TEMP_PREFERRED_S - sl_desync ) * NS_PER_S;
	UD	until = min_ud(cap_until, m->until), pref = min_ud(cap_pref, m->pref_until);
	UB	iid[8];
	INT	i;

	if ( left_of(pref, now) <= REGEN_ADVANCE_S || !temp_iid(iid) ) {
		m->temp_try = now + (UD)TEMP_RETRY_S * NS_PER_S;
		return;
	}
	for ( i = 0; i < TEMPS && p == NULL; i++ ) {
		if ( !sl_temp[i].used ) p = &sl_temp[i];
	}
	if ( p == NULL ) {
		/* full: the one that would have gone first goes now */
		p = &sl_temp[0];
		for ( i = 1; i < TEMPS; i++ ) {
			if ( !sl_temp[i].current && ( p->current || sl_temp[i].until < p->until ) ) p = &sl_temp[i];
		}
		temp_drop(p);
	}
	knl_memset(p, 0, sizeof(*p));
	knl_memcpy(p->addr, m->addr, 8);
	knl_memcpy(p->addr + 8, iid, 8);
	p->from = pi;
	if ( !temp_set(p, until, pref, now, TRUE) ) {
		m->temp_try = now + (UD)TEMP_RETRY_S * NS_PER_S;
		return;
	}
	for ( i = 0; i < TEMPS; i++ ) {
		if ( sl_temp[i].used && sl_temp[i].from == pi ) sl_temp[i].current = FALSE;
	}
	p->used = TRUE;
	p->current = TRUE;
	p->tries = tries;
	p->made = now;
	p->cap_until = cap_until;
	p->cap_pref = cap_pref;
	say("temporary address", p->addr, left_of(until, now));
}

/*
 * The temporary addresses of a prefix whose lifetimes have changed: each
 * keeps the lower of its own and the prefix's, and one is made when
 * there is none in use.
 */
LOCAL void temp_prefix( INT pi, UD now )
{
	MADE	*m = &sl_made[pi];
	TEMP	*p;
	BOOL	have = FALSE;
	UD	until, pref;
	INT	i;

	for ( i = 0; i < TEMPS; i++ ) {
		p = &sl_temp[i];
		if ( !p->used || p->from != pi ) continue;
		until = min_ud(p->cap_until, m->until);
		pref = min_ud(p->cap_pref, m->pref_until);
		if ( until != p->until || pref != p->pref_until ) {
			if ( !temp_set(p, until, pref, now, FALSE) ) {
				temp_drop(p);
				continue;
			}
		}
		if ( p->current ) have = TRUE;
	}
	if ( !have && now >= m->temp_try ) {
		temp_new(pi, 0, now);
	}
}

/*
 * Once a second: what has run out is forgotten, a new temporary address
 * is made before the one in use stops being preferred, and one that has
 * turned out to be another machine's is made again.
 */
LOCAL void temp_tick( UD now )
{
	T_SOIF6	l[16];
	TEMP	*p;
	BOOL	young = FALSE;
	INT	i, j, n;

	for ( i = 0; i < TEMPS; i++ ) {
		p = &sl_temp[i];
		if ( p->used && p->until <= now ) p->used = FALSE;	/* the kernel took it away */
		if ( p->used && !p->checked ) young = TRUE;
	}
	if ( young ) {
		n = rump_tsnet_addrs6(SO_IFNAME, l, 16);
		for ( i = 0; i < TEMPS; i++ ) {
			p = &sl_temp[i];
			if ( !p->used || p->checked ) continue;
			for ( j = 0; j < n && j < 16 && !same(l[j].addr, p->addr, 16); j++ ) ;
			if ( j < n && j < 16 && ( l[j].flags & SO_IF6_DUPLICATED ) ) {
				INT	pi = p->from, tries = p->tries + 1;
				BOOL	cur = p->current;

				say("temporary address in use by another machine:", p->addr, LIFE_INFINITE);
				temp_drop(p);
				if ( cur && tries <= TEMP_IDGEN_RETRIES && sl_made[pi].used ) temp_new(pi, tries, now);
			} else if ( j < n && j < 16 && !( l[j].flags & SO_IF6_TENTATIVE ) ) {
				p->checked = TRUE;
			} else if ( now - p->made > (UD)TEMP_CHECK_S * NS_PER_S ) {
				p->checked = TRUE;
			}
		}
	}
	for ( i = 0; i < PREFIXES; i++ ) {
		if ( !sl_made[i].used ) continue;
		for ( j = 0; j < TEMPS; j++ ) {
			p = &sl_temp[j];
			if ( p->used && p->from == i && p->current ) break;
		}
		if ( j == TEMPS ) {
			if ( now >= sl_made[i].temp_try ) temp_new(i, 0, now);
		} else if ( p->pref_until != UNTIL_NEVER
			 && p->pref_until <= now + (UD)REGEN_ADVANCE_S * NS_PER_S ) {
			temp_new(i, 0, now);
			if ( p->current ) {
				p->current = FALSE;	/* none could be made: this one is let run out */
				sl_made[i].temp_try = now + (UD)TEMP_RETRY_S * NS_PER_S;
			}
		}
	}
}

/* Temporary addresses begun or ended as they are asked for */
LOCAL void temp_turn( void )
{
	UW	r = 0;
	INT	i;

	if ( sl_temp_want == sl_temp_on ) {
		return;
	}
	sl_temp_on = sl_temp_want;
	if ( !sl_temp_on ) {
		for ( i = 0; i < TEMPS; i++ ) temp_drop(&sl_temp[i]);
		tm_printf((UB *)"net: IPv6 temporary addresses ended\n");
		return;
	}
	(void)ts_get_random(&r, sizeof(r));
	sl_desync = r % ( MAX_DESYNC_S + 1 );
	for ( i = 0; i < PREFIXES; i++ ) sl_made[i].temp_try = 0;
	tm_printf((UB *)"net: IPv6 temporary addresses begun\n");
}

/* ---------------------------------------------------------------- lifetimes */

/* What has run out goes: routers, name servers, and the records of addresses the kernel took away */
LOCAL void expire( UD now )
{
	UINT	imask;
	INT	i;

	for ( i = 0; i < ROUTERS; i++ ) {
		if ( sl_rtr[i].used && sl_rtr[i].until <= now ) sl_rtr[i].used = FALSE;
	}
	route_again();
	ISpinLock(&sl_lock, &imask);
	for ( i = 0; i < SERVERS; i++ ) {
		if ( sl_ns[i].used && sl_ns[i].until <= now ) sl_ns[i].used = FALSE;
	}
	ISpinUnlock(&sl_lock, &imask);
	for ( i = 0; i < PREFIXES; i++ ) {
		if ( sl_made[i].used && sl_made[i].until <= now ) sl_made[i].used = FALSE;
	}
	temp_turn();
	if ( sl_temp_on ) {
		temp_tick(now);
	}
}

/*
 * Everything taken from the routers given up: the addresses made from
 * their prefixes come off the interface, the default route through them
 * goes, and their name servers are no longer asked. Called only while
 * the task is not listening.
 */
LOCAL void forget( void )
{
	UINT	imask;
	INT	i;

	for ( i = 0; i < TEMPS; i++ ) temp_drop(&sl_temp[i]);
	for ( i = 0; i < PREFIXES; i++ ) {
		if ( sl_made[i].used ) {
			(void)rump_tsnet_deladdr6(SO_IFNAME, sl_made[i].addr);
			sl_made[i].used = FALSE;
		}
	}
	for ( i = 0; i < ROUTERS; i++ ) sl_rtr[i].used = FALSE;
	route_again();
	ISpinLock(&sl_lock, &imask);
	for ( i = 0; i < SERVERS; i++ ) sl_ns[i].used = FALSE;
	ISpinUnlock(&sl_lock, &imask);
	sl_temp_on = FALSE;
}

/* ---------------------------------------------------------------- advertisements */

LOCAL void take_router( CONST UB *from, UW life, UD now )
{
	INT	i, free = -1;

	for ( i = 0; i < ROUTERS; i++ ) {
		if ( sl_rtr[i].used && same(sl_rtr[i].addr, from, 16) ) break;
		if ( !sl_rtr[i].used && free < 0 ) free = i;
	}
	if ( i == ROUTERS ) {
		if ( life == 0 || free < 0 ) return;
		i = free;
		knl_memcpy(sl_rtr[i].addr, from, 16);
	}
	if ( life == 0 ) {
		sl_rtr[i].used = FALSE;
	} else {
		sl_rtr[i].used = TRUE;
		sl_rtr[i].until = until_of(life, now);
	}
}

/* A prefix to make an address from (RFC 4862 5.5.3) */
LOCAL void take_prefix( CONST UB *o, UD now )
{
	UB	a[16];
	UW	valid = get32(o + 4), pref = get32(o + 8), left;
	INT	plen = o[2], i, free = -1, e;

	if ( ( o[3] & PI_AUTO ) == 0 || plen != 64 || link_local(o + 16) || pref > valid ) {
		return;
	}
	knl_memcpy(a, o + 16, 8);
	knl_memcpy(a + 8, sl_iid, 8);
	for ( i = 0; i < PREFIXES; i++ ) {
		if ( sl_made[i].used && same(sl_made[i].addr, a, 16) ) break;
		if ( !sl_made[i].used && free < 0 ) free = i;
	}
	if ( i == PREFIXES ) {
		if ( valid == 0 || free < 0 ) return;
		i = free;
		knl_memcpy(sl_made[i].addr, a, 16);
		sl_made[i].plen = ( o[3] & PI_ONLINK ) ? 64 : 128;
		e = rump_tsnet_setaddr6(SO_IFNAME, a, sl_made[i].plen, valid, pref, SO_IF6_AUTOCONF);
		if ( e != 0 ) {
			tm_printf((UB *)"net: IPv6 address not set (%d)\n", e);
			return;
		}
		sl_made[i].used = TRUE;
		sl_made[i].until = until_of(valid, now);
		sl_made[i].pref_until = until_of(pref, now);
		sl_made[i].temp_try = 0;
		say("address", a, valid);
		if ( sl_temp_on ) temp_prefix(i, now);
		return;
	}
	/* renewed: no shorter than two hours unless less was left */
	left = left_of(sl_made[i].until, now);
	if ( valid > TWO_HOURS_S || valid > left ) {
		;
	} else if ( left <= TWO_HOURS_S ) {
		valid = left;
	} else {
		valid = TWO_HOURS_S;
	}
	if ( pref > valid ) {
		pref = valid;
	}
	if ( valid == 0 ) {
		return;
	}
	if ( rump_tsnet_setaddr6(SO_IFNAME, a, sl_made[i].plen, valid, pref, SO_IF6_AUTOCONF) == 0 ) {
		sl_made[i].until = until_of(valid, now);
		sl_made[i].pref_until = until_of(pref, now);
		if ( sl_temp_on ) temp_prefix(i, now);
	}
}

/* Name servers (RDNSS): a lifetime of 0 takes them away */
LOCAL void take_servers( CONST UB *o, INT len, UD now )
{
	UW	life = get32(o + 4);
	UINT	imask;
	INT	at, i, free;

	ISpinLock(&sl_lock, &imask);
	for ( at = 8; at + 16 <= len; at += 16 ) {
		free = -1;
		for ( i = 0; i < SERVERS; i++ ) {
			if ( sl_ns[i].used && same(sl_ns[i].addr, o + at, 16) ) break;
			if ( !sl_ns[i].used && free < 0 ) free = i;
		}
		if ( i == SERVERS ) {
			if ( life == 0 || free < 0 ) continue;
			i = free;
			knl_memcpy(sl_ns[i].addr, o + at, 16);
		}
		sl_ns[i].used = ( life != 0 );
		sl_ns[i].until = until_of(life, now);
	}
	ISpinUnlock(&sl_lock, &imask);
}

/* An advertisement come in from 'from' with hop limit 'hlim' */
LOCAL void take_ra( CONST UB *m, INT n, CONST UB *from, INT hlim )
{
	UD	now = now_ns();
	UW	life;
	INT	at, olen;

	if ( n < 16 || m[0] != ND_ROUTER_ADVERT || m[1] != 0 || hlim != 255 || !link_local(from) ) {
		return;
	}
	for ( at = 16; at + 2 <= n; at += olen ) {
		olen = m[at + 1] * 8;
		if ( olen == 0 || at + olen > n ) {
			return;			/* a broken option: the whole is not taken */
		}
	}
	life = ( (UW)m[6] << 8 ) | m[7];
	temp_turn();
	for ( at = 16; at + 2 <= n; at += olen ) {
		olen = m[at + 1] * 8;
		switch ( m[at] ) {
		case OPT_PREFIX:
			if ( olen == 32 ) take_prefix(m + at, now);
			break;
		case OPT_RDNSS:
			if ( olen >= 24 ) take_servers(m + at, olen, now);
			break;
		default:
			break;
		}
	}
	take_router(from, life, now);
	route_again();
}

/* ---------------------------------------------------------------- the task */

/* A router solicitation to all the routers on the link, with the card's address */
LOCAL void solicit( INT s, UINT ifindex )
{
	struct sockaddr_in6	to;
	UB			m[16];

	knl_memset(m, 0, sizeof(m));
	m[0] = ND_ROUTER_SOLICIT;
	m[8] = OPT_SOURCE_LLA;
	m[9] = 1;
	(void)net_get_mac(m + 10);
	knl_memset(&to, 0, sizeof(to));
	to.sin6_len = sizeof(to);
	to.sin6_family = AF_INET6;
	to.sin6_addr.s6_addr[0] = 0xff;
	to.sin6_addr.s6_addr[1] = 0x02;
	to.sin6_addr.s6_addr[15] = 0x02;
	to.sin6_scope_id = ifindex;
	if ( so_sendto(s, m, sizeof(m), 0, (struct sockaddr *)&to, sizeof(to)) >= 0 ) {
		sl_solicits++;
	}
}

/* The socket advertisements come to: ICMPv6 of that type only, with their hop limit */
LOCAL INT listen_socket( UINT ifindex )
{
	UW	filt[8];
	INT	s, on = 1, hops = 255;

	s = so_socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if ( s < 0 ) {
		return s;
	}
	knl_memset(filt, 0, sizeof(filt));
	filt[ND_ROUTER_ADVERT >> 5] = 1U << ( ND_ROUTER_ADVERT & 31 );
	(void)so_setsockopt(s, IPPROTO_ICMPV6, ICMP6_FILTER, filt, sizeof(filt));
	(void)so_setsockopt(s, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
	(void)so_setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
	(void)so_setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
	(void)so_setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
	return s;
}

LOCAL void sl_client( UINT gen )
{
	struct sockaddr_in6	from;
	struct pollfd		p;
	T_NETSTAT		st;
	socklen_t		flen;
	UD			now, next_rs = 0;
	UINT			ifindex;
	INT			s, n, hlim, rs_left = RS_COUNT;
	BOOL			was_up = TRUE;
	UINT			relink = sl_relink;

	if ( !take_iid(gen) ) {
		while ( sl_gen == gen ) (void)tk_slp_tsk(TMO_FEVR);
		return;
	}
	ifindex = so_ifnametoindex(SO_IFNAME);
	s = listen_socket(ifindex);
	if ( s < 0 ) {
		tm_printf((UB *)"net: IPv6 router advertisements cannot be listened to (%d)\n", s);
		while ( sl_gen == gen ) (void)tk_slp_tsk(TMO_FEVR);
		return;
	}
	while ( sl_gen == gen ) {
		now = now_ns();
		if ( net_stat(&st) >= E_OK ) {
			if ( st.up && !was_up ) {
				rs_left = RS_COUNT;	/* the cable is back: ask again */
				next_rs = now;
			}
			was_up = st.up;
		}
		if ( relink != sl_relink ) {
			UB	was[8];

			relink = sl_relink;
			rs_left = RS_COUNT;	/* a new link: the routers asked again at once */
			next_rs = now;
			/*
			 * Another card under neta0 gives the interface another
			 * link-local address (rump_tswifi_setmac): the addresses
			 * made with the identifier of the one before are given
			 * up, and those of the new link made with the new one
			 */
			knl_memcpy(was, sl_iid, 8);
			if ( !take_iid(gen) ) break;
			if ( !same(was, sl_iid, 8) ) {
				tm_putstring((UB *)"net: IPv6: the interface identifier changed with the card: "
					     "the addresses from the routers are made again\n");
				forget();
			}
			now = now_ns();
		}
		if ( rs_left > 0 && now >= next_rs ) {
			solicit(s, ifindex);
			rs_left--;
			next_rs = now + (UD)RS_INTERVAL_MS * 1000000ULL;
		}
		expire(now);

		p.fd = s;
		p.events = POLLIN;
		p.revents = 0;
		if ( so_poll(&p, 1, SL_SLICE_MS) <= 0 ) {
			continue;
		}
		flen = sizeof(from);
		hlim = -1;
		n = knl_so_nb_recv_hlim(s, sl_msg, sizeof(sl_msg), (struct sockaddr *)&from, &flen, &hlim);
		if ( n > 0 && flen >= sizeof(from) && from.sin6_family == AF_INET6 && sl_gen == gen ) {
			take_ra(sl_msg, n, from.sin6_addr.s6_addr, hlim);
		}
	}
	so_close(s);
}

LOCAL void sl_task( INT stacd, void *exinf )
{
	UINT	imask, gen = 0;
	BOOL	go;

	for ( ;; ) {
		ISpinLock(&sl_lock, &imask);
		go = sl_want;
		if ( go ) {
			sl_running = TRUE;
			gen = sl_gen;
		}
		ISpinUnlock(&sl_lock, &imask);
		if ( !go ) {
			(void)tk_slp_tsk(TMO_FEVR);
			continue;
		}
		sl_client(gen);
		sl_running = FALSE;
	}
}

EXPORT ER knl_so_slaac_run( void )
{
	T_CTSK	ctsk;
	UINT	imask;
	ID	tskid;

	if ( sl_tsk > 0 ) {
		ISpinLock(&sl_lock, &imask);
		sl_gen++;
		sl_want = TRUE;
		ISpinUnlock(&sl_lock, &imask);
		(void)tk_wup_tsk(sl_tsk);
		return E_OK;
	}
	InitSpinLock(&sl_lock);
	sl_gen++;
	sl_want = TRUE;
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)sl_task;
	ctsk.itskpri = SL_TASK_PRI;
	ctsk.stksz   = SL_TASK_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		sl_want = FALSE;
		return (ER)tskid;
	}
	sl_tsk = tskid;
	return tk_sta_tsk(tskid, 0);
}

EXPORT void knl_so_slaac_stop( BOOL forget_all )
{
	UINT	imask;
	BOOL	running;
	INT	t;

	if ( sl_tsk <= 0 ) {
		return;
	}
	ISpinLock(&sl_lock, &imask);
	sl_want = FALSE;
	sl_gen++;
	running = sl_running;
	ISpinUnlock(&sl_lock, &imask);
	if ( running ) {
		(void)tk_wup_tsk(sl_tsk);
		for ( t = 0; t < SL_STOP_MS && sl_running; t += 50 ) {
			(void)tk_dly_tsk(50);
		}
		if ( sl_running ) {
			tm_printf((UB *)"net: IPv6 router advertisements still being listened to\n");
		}
	}
	if ( forget_all ) {
		forget();
	}
}

/*
 * The link came up again (netadp.c: the Wi-Fi joined a network, a cable
 * put back, another card): the routers are asked again, so that a new
 * network's prefixes are had without waiting for its next advertisement
 */
EXPORT void knl_so_slaac_relink( void )
{
	sl_relink++;
}

/* The router solicitations sent so far (the tests) */
EXPORT UINT knl_so_slaac_solicits( void )
{
	return sl_solicits;
}

EXPORT void knl_so_slaac_temp( BOOL on )
{
	sl_temp_want = on;
}

EXPORT BOOL knl_so_slaac_dns( INT n, UB *addr )
{
	UINT	imask;
	INT	i, k = 0;
	BOOL	yes = FALSE;

	if ( sl_tsk <= 0 ) {
		return FALSE;
	}
	ISpinLock(&sl_lock, &imask);
	for ( i = 0; i < SERVERS && !yes; i++ ) {
		if ( sl_ns[i].used && k++ == n ) {
			knl_memcpy(addr, sl_ns[i].addr, 16);
			yes = TRUE;
		}
	}
	ISpinUnlock(&sl_lock, &imask);
	return yes;
}

EXPORT BOOL knl_so_slaac_gw( UB *addr )
{
	UINT	imask;
	BOOL	yes;

	if ( sl_tsk <= 0 ) {
		return FALSE;
	}
	ISpinLock(&sl_lock, &imask);
	yes = sl_have_gw;
	if ( yes ) knl_memcpy(addr, sl_gw, 16);
	ISpinUnlock(&sl_lock, &imask);
	return yes;
}
