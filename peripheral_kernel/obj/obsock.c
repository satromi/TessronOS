/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsock.c
 *	The socket manager: the sockets of the network as objects
 *	(OB_T_CHANNEL, OB_S_SOCKET; design 18.8, 12.6)
 *
 *	A socket is an object from the time so_socket or so_accept gives
 *	out its number until so_close takes it back, which the end of the
 *	process that made it does too (knl_so_prc_end). The stack tells
 *	this manager at those two points (knl_so_obj_made, _accepted,
 *	_gone) and at no other: the calls that use a socket go on naming it
 *	by its number and never look for its object.
 *
 *	What the object shows is asked of the stack when it is read. Record
 *	0 says what the socket is, with a link to the process that owns it;
 *	record 1 is its data, received when read and sent when written, and
 *	waited for as a process's own call waits. The metadata's "socket"
 *	member has the number, the process, the family, type and protocol,
 *	the two ends and the state. The protection is fixed: the owner is
 *	the user the process acts for (the system for the kernel's own
 *	sockets), and only the owner has it, rw-------. Deleting the object
 *	closes the socket.
 *
 *	The table has an entry for each socket number, the object's UUID
 *	in it. It grows with the stack's table, a block of SO_BLK numbers at
 *	a time made before the stack gives out a number of it
 *	(knl_so_obj_room), and a block once made stays where it is. A handle
 *	carries the number and which of the sockets that have had that
 *	number it was opened on, so a key kept past so_close finds nothing
 *	(E_NOEXS) rather than the socket that has the number now.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "tstdlib.h"
#include <ts/so.h>
#include <ts/proc.h>
#include "obj.h"

#define SK_SA_MAX	128		/* bytes of an address from the stack */
#define OBK_NAME_ROOM	48		/* "socket N (process M)" of two numbers: the most such a name takes */
/*
 * A handle is the number + 1 in its low SK_NUM_BITS bits and the low bits
 * of the socket's count above them, short of the sign bit
 */
#define SK_NUM_BITS	15
#define SK_NUM_MASK	( ( 1U << SK_NUM_BITS ) - 1 )
#define SK_GEN_MASK	( 0x7FFFFFFFU >> SK_NUM_BITS )	/* the part of the count a handle carries */
#if SO_NUM_CEIL > ( 1 << SK_NUM_BITS ) - 1
#error "obsock.c: a handle has no room for the socket numbers (SK_NUM_BITS)"
#endif

typedef struct {
	BOOL	used;			/* the number is a socket's */
	TS_UUID	uuid;			/* all zeroes: it has no object */
	ID	owner;			/* the process, 0 the kernel */
	UINT	gen;			/* which socket of this number it is */
	INT	domain, type, protocol;
} OBSK;

#define OBSK_NBLK	( SO_NUM_CEIL / SO_BLK )

LOCAL OBSK	*obsk_blk[OBSK_NBLK];
LOCAL INT	obsk_n = 0;		/* numbers with an entry: obsk_n / SO_BLK blocks, made in order */
LOCAL T_SPLOCK	obsk_lock;		/* zero, as it starts, is unlocked */

/* The entry of number s, which is below obsk_n */
#define OBSK_AT(s)	( &obsk_blk[(s) / SO_BLK][(s) % SO_BLK] )

/* The numbers with an entry now */
LOCAL INT obsk_room( void )
{
	return __atomic_load_n(&obsk_n, __ATOMIC_ACQUIRE);
}

/* The entry of number s, NULL when it has none */
LOCAL OBSK *obsk_of( INT s )
{
	return ( s >= 0 && s < obsk_room() ) ? OBSK_AT(s) : NULL;
}

/* Room made for numbers 0 .. n - 1: a block at a time, in order */
EXPORT ER knl_so_obj_room( INT n )
{
	OBSK	*b;
	UINT	imask;
	INT	at;

	if ( n > SO_NUM_CEIL ) {
		return E_PAR;
	}
	while ( ( at = obsk_room() ) < n ) {
		b = (OBSK *)Kmalloc(sizeof(OBSK) * SO_BLK);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		knl_memset(b, 0, sizeof(OBSK) * SO_BLK);
		ISpinLock(&obsk_lock, &imask);
		if ( obsk_n == at ) {
			obsk_blk[at / SO_BLK] = b;
			__atomic_store_n(&obsk_n, at + SO_BLK, __ATOMIC_RELEASE);
			b = NULL;
		}
		ISpinUnlock(&obsk_lock, &imask);
		if ( b != NULL ) {
			Kfree(b);	/* another task made it first */
		}
	}
	return E_OK;
}
LOCAL UINT	obsk_gen = 0;
LOCAL BOOL	obsk_up = FALSE;	/* the object layer is up: objects are made */

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* The number whose object this is, -1 none (under the lock) */
LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	s, n = obsk_room();

	for ( s = 0; s < n; s++ ) {
		if ( OBSK_AT(s)->used && same_uuid(&OBSK_AT(s)->uuid, uuid) ) {
			return s;
		}
	}
	return -1;
}

/* The entry of an object, copied; its number, or -1 */
LOCAL INT entry_of( CONST TS_UUID *uuid, OBSK *e )
{
	UINT	imask;
	INT	s;

	ISpinLock(&obsk_lock, &imask);
	s = index_of(uuid);
	if ( s >= 0 ) {
		*e = *OBSK_AT(s);
	}
	ISpinUnlock(&obsk_lock, &imask);
	return s;
}

/* The socket a handle was opened on, while it is still that socket; its number, or -1 */
LOCAL INT entry_of_handle( INT h, OBSK *e )
{
	UINT	imask;
	INT	s = (INT)( (UINT)h & SK_NUM_MASK ) - 1;
	OBSK	*k = obsk_of(s);
	BOOL	ok;

	if ( h <= 0 || k == NULL ) {
		return -1;
	}
	ISpinLock(&obsk_lock, &imask);
	ok = (BOOL)( k->used && !knl_ob_uuid_zero(&k->uuid)
		     && ( k->gen & SK_GEN_MASK ) == ( (UINT)h >> SK_NUM_BITS ) );
	if ( ok ) {
		*e = *k;
	}
	ISpinUnlock(&obsk_lock, &imask);
	return ok ? s : -1;
}

/* ---------------------------------------------------------------- what the stack says */

/* A UUID for a new object, or all zeroes when none can be made yet */
LOCAL void new_uuid( TS_UUID *u )
{
	if ( !obsk_up || ts_gen_uuid(u) < E_OK ) {
		knl_memset(u, 0, sizeof(*u));
	}
}

EXPORT void knl_so_obj_made( INT s, ID owner, INT domain, INT type, INT protocol )
{
	TS_UUID	u;
	UINT	imask;
	OBSK	*e;

	/* the stack's table made room first; a stack that does not has it made here */
	if ( s < 0 || s >= SO_NUM_CEIL || knl_so_obj_room(( s / SO_BLK + 1 ) * SO_BLK) < E_OK ) {
		return;
	}
	e = OBSK_AT(s);
	new_uuid(&u);
	ISpinLock(&obsk_lock, &imask);
	e->used = TRUE;
	e->uuid = u;
	e->owner = owner;
	e->gen = ++obsk_gen;
	e->domain = domain;
	e->type = type;
	e->protocol = protocol;
	ISpinUnlock(&obsk_lock, &imask);
}

/* A connection taken from socket 'from': its family, type and protocol are that socket's */
EXPORT void knl_so_obj_accepted( INT s, INT from, ID owner )
{
	OBSK	*f = obsk_of(from);
	UINT	imask;
	INT	domain = 0, type = 0, protocol = 0;

	if ( f != NULL ) {
		ISpinLock(&obsk_lock, &imask);
		if ( f->used ) {
			domain = f->domain;
			type = f->type;
			protocol = f->protocol;
		}
		ISpinUnlock(&obsk_lock, &imask);
	}
	knl_so_obj_made(s, owner, domain, type, protocol);
}

/* The socket is being closed: its object goes, and whoever watched it is told */
EXPORT void knl_so_obj_gone( INT s )
{
	TS_UUID	u;
	UINT	imask;
	OBSK	*e = obsk_of(s);

	if ( e == NULL ) {
		return;
	}
	knl_memset(&u, 0, sizeof(u));
	ISpinLock(&obsk_lock, &imask);
	if ( e->used ) {
		u = e->uuid;
		e->used = FALSE;
		knl_memset(&e->uuid, 0, sizeof(e->uuid));
	}
	ISpinUnlock(&obsk_lock, &imask);
	if ( !knl_ob_uuid_zero(&u) ) {
		knl_ob_post(&u, -1, OB_E_DELETE, NULL);
	}
}

EXPORT ER so_getobj( INT s, TS_UUID *p_uuid )
{
	UINT	imask;
	OBSK	*e = obsk_of(s);
	ER	er;

	if ( p_uuid == NULL ) {
		return E_PAR;
	}
	if ( e == NULL ) {
		return E_ID;
	}
	ISpinLock(&obsk_lock, &imask);
	if ( !e->used ) {
		er = E_ID;
	} else if ( knl_ob_uuid_zero(&e->uuid) ) {
		er = E_NOEXS;
	} else {
		*p_uuid = e->uuid;
		er = E_OK;
	}
	ISpinUnlock(&obsk_lock, &imask);
	return er;
}

EXPORT INT knl_so_obj_list( ID pid, INT *nums, TS_UUID *uuids, INT max )
{
	UINT	imask;
	OBSK	*e;
	INT	s, n = 0, room = obsk_room();

	ISpinLock(&obsk_lock, &imask);
	for ( s = 0; s < room; s++ ) {
		e = OBSK_AT(s);
		if ( !e->used || e->owner != pid || knl_ob_uuid_zero(&e->uuid) ) {
			continue;
		}
		if ( n < max ) {
			if ( nums != NULL ) nums[n] = s;
			if ( uuids != NULL ) uuids[n] = e->uuid;
		}
		n++;
	}
	ISpinUnlock(&obsk_lock, &imask);
	return n;
}

/* ---------------------------------------------------------------- what it is, as text */

/* "socket 3 (process 12)", "socket 3 (kernel)" */
LOCAL INT put_name( UB *t, INT n, INT max, INT s, ID owner )
{
	n = knl_oj_put(t, n, max, "socket ");
	n = knl_oj_put_num(t, n, max, s);
	if ( owner > 0 ) {
		n = knl_oj_put(t, n, max, " (process ");
		n = knl_oj_put_num(t, n, max, owner);
		return knl_oj_put(t, n, max, ")");
	}
	return knl_oj_put(t, n, max, " (kernel)");
}

LOCAL CONST char *family_name( INT domain )
{
	switch ( domain ) {
	case AF_INET:	return "inet";
	case AF_INET6:	return "inet6";
	default:	return "unspec";
	}
}

LOCAL CONST char *type_name( INT type )
{
	switch ( type & 0xF ) {
	case SOCK_STREAM:	return "stream";
	case SOCK_DGRAM:	return "datagram";
	case SOCK_RAW:		return "raw";
	default:		return "other";
	}
}

/* The protocol by its name: the one asked for, or the one its type means */
LOCAL CONST char *proto_name( INT type, INT protocol )
{
	switch ( protocol ) {
	case 0:
		switch ( type & 0xF ) {
		case SOCK_STREAM:	return "tcp";
		case SOCK_DGRAM:	return "udp";
		default:		return "raw";
		}
	case 1:		return "icmp";
	case 6:		return "tcp";
	case 17:	return "udp";
	case 58:	return "icmpv6";
	default:	return "other";
	}
}

/* The two ends and the state, asked of the stack */
typedef struct {
	UB	local[SK_SA_MAX];
	UB	remote[SK_SA_MAX];
	BOOL	has_local, has_remote;
	CONST char *state;
} SKNOW;

LOCAL UINT sa_port( CONST UB *sa )
{
	return ( (UINT)sa[2] << 8 ) | sa[3];
}

LOCAL void now_of( INT s, INT type, SKNOW *k )
{
	socklen_t	len;
	INT		acc = 0;
	socklen_t	alen = sizeof(acc);

	knl_memset(k, 0, sizeof(*k));
	len = sizeof(k->local);
	k->has_local = (BOOL)( so_getsockname(s, (struct sockaddr *)k->local, &len) >= E_OK && len >= 8 );
	len = sizeof(k->remote);
	k->has_remote = (BOOL)( so_getpeername(s, (struct sockaddr *)k->remote, &len) >= E_OK && len >= 8 );
	if ( ( type & 0xF ) == SOCK_STREAM
	  && so_getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &acc, &alen) >= E_OK && acc != 0 ) {
		k->state = "listening";
	} else if ( k->has_remote ) {
		k->state = "connected";
	} else if ( k->has_local && sa_port(k->local) != 0 ) {
		k->state = "bound";
	} else {
		k->state = "open";
	}
}

LOCAL INT put_hex4( UB *t, INT n, INT max, UINT v )
{
	CONST char	*hex = "0123456789abcdef";
	char		d[5];
	INT		i, k = 0;
	BOOL		any = FALSE;

	for ( i = 12; i >= 0; i -= 4 ) {
		UINT	x = ( v >> i ) & 0xF;

		if ( x != 0 || any || i == 0 ) {
			d[k++] = hex[x];
			any = TRUE;
		}
	}
	d[k] = 0;
	return knl_oj_put(t, n, max, d);
}

/* The address of an end, without its port: a.b.c.d, or eight groups of an IPv6 one */
LOCAL INT put_addr( UB *t, INT n, INT max, CONST UB *sa )
{
	INT	i;

	if ( sa[1] == AF_INET6 ) {
		for ( i = 0; i < 8; i++ ) {
			if ( i > 0 ) n = knl_oj_put(t, n, max, ":");
			n = put_hex4(t, n, max, ( (UINT)sa[8 + i * 2] << 8 ) | sa[9 + i * 2]);
		}
		return n;
	}
	for ( i = 0; i < 4; i++ ) {
		if ( i > 0 ) n = knl_oj_put(t, n, max, ".");
		n = knl_oj_put_num(t, n, max, sa[4 + i]);
	}
	return n;
}

/* An end as the text shows it: 127.0.0.1:80, [::1]:80 */
LOCAL INT put_end( UB *t, INT n, INT max, CONST UB *sa )
{
	BOOL	v6 = (BOOL)( sa[1] == AF_INET6 );

	if ( v6 ) n = knl_oj_put(t, n, max, "[");
	n = put_addr(t, n, max, sa);
	if ( v6 ) n = knl_oj_put(t, n, max, "]");
	n = knl_oj_put(t, n, max, ":");
	return knl_oj_put_num(t, n, max, sa_port(sa));
}

/* Record 0: what the socket is, as xmlTAD, with a link to its process */
LOCAL INT describe( INT s, CONST OBSK *e, UB *t, INT max )
{
	SKNOW	k;
	TS_UUID	pu;
	UB	name[OBK_NAME_ROOM];
	INT	n;

	now_of(s, e->type, &k);
	(void)put_name(name, 0, sizeof(name), s, e->owner);
	n = knl_oj_put(t, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, "\"><document><p>");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, ": ");
	n = knl_oj_put(t, n, max, proto_name(e->type, e->protocol));
	n = knl_oj_put(t, n, max, ", ");
	n = knl_oj_put(t, n, max, type_name(e->type));
	if ( k.has_local ) {
		n = knl_oj_put(t, n, max, ", ");
		n = put_end(t, n, max, k.local);
	}
	if ( k.has_remote ) {
		n = knl_oj_put(t, n, max, " to ");
		n = put_end(t, n, max, k.remote);
	}
	n = knl_oj_put(t, n, max, ", ");
	n = knl_oj_put(t, n, max, k.state);
	n = knl_oj_put(t, n, max, ". Record 1 is the data (read, what has come in; written, it is sent)</p>");
	if ( e->owner > 0 && knl_prc_uuid(e->owner, &pu, NULL) >= E_OK && !knl_ob_uuid_zero(&pu) ) {
		UB	pname[24];
		INT	p = knl_oj_put(pname, 0, sizeof(pname), "process ");

		(void)knl_oj_put_num(pname, p, sizeof(pname), e->owner);
		n = knl_obdev_link(t, n, max, &pu, pname);
	}
	return knl_oj_put(t, n, max, "</document></tad>");
}

/* Record 0 of socket s, made as long as it needs (knl_oj_make) */
typedef struct {
	INT		s;
	CONST OBSK	*e;
} SKDESC;

LOCAL INT describe_make( void *ctx, UB *t, INT max )
{
	SKDESC	*d = (SKDESC *)ctx;

	return describe(d->s, d->e, t, max);
}

/* Bytes that have come in and not been read */
LOCAL UD waiting( INT s )
{
	INT	n = 0;

	return ( so_ioctl(s, (INT)FIONREAD, &n) >= E_OK && n > 0 ) ? (UD)n : 0;
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obk_find( CONST TS_UUID *uuid )
{
	OBSK	e;

	return ( entry_of(uuid, &e) >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obk_refnam( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	UB	t[OBK_NAME_ROOM];
	OBSK	e;
	INT	s = entry_of(uuid, &e);

	if ( s < 0 ) {
		return E_NOEXS;
	}
	(void)put_name(t, 0, (INT)sizeof(t), s, e.owner);
	return knl_ob_name_put(t, -1, buf, size, p_asize);
}

LOCAL ER obk_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	OBSK	e;
	INT	s = entry_of(uuid, &e);

	if ( s < 0 ) {
		return E_NOEXS;
	}
	r->flags = OB_F_VOLATILE;
	r->refcnt = 0;
	r->nrec = OB_SK_NREC;
	r->size = waiting(s);

	return E_OK;
}

/* The user the process acts for owns it, and only the owner has it: rw------- */
LOCAL ER obk_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	OBSK	e;

	if ( entry_of(uuid, &e) < 0 ) {
		return E_NOEXS;
	}
	*p_prt = knl_ob_prt_new(0, 0);
	if ( *p_prt == NULL ) {
		return E_NOMEM;
	}
	knl_ob_prt_default(*p_prt, knl_ob_crd_of(e.owner));
	(*p_prt)->mode = 0600;

	return E_OK;
}

/* Deleted: the socket is closed, which tells of it going (knl_so_obj_gone) */
LOCAL ER obk_remove( CONST TS_UUID *uuid )
{
	OBSK	e;
	INT	s = entry_of(uuid, &e);

	return ( s >= 0 ) ? so_close(s) : E_NOEXS;
}

LOCAL INT obk_open( CONST TS_UUID *uuid, UINT ops )
{
	OBSK	e;
	INT	s = entry_of(uuid, &e);

	(void)ops;
	if ( s < 0 ) {
		return E_NOEXS;
	}
	return (INT)( ( ( e.gen & SK_GEN_MASK ) << SK_NUM_BITS ) | (UINT)( s + 1 ) );
}

LOCAL ER obk_close( INT h )
{
	(void)h;
	return E_OK;
}

/* Record 0, the text; record 1, what has come in */
LOCAL ER obk_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBSK	e;
	SKDESC	d;
	INT	s = entry_of_handle(h, &e), len;

	if ( s < 0 ) {
		return E_NOEXS;
	}
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	if ( recno == OB_SK_DATA ) {
		if ( off != 0 ) {
			return E_PAR;
		}
		len = nowait ? so_recv(s, buf, size, MSG_DONTWAIT) : knl_so_recv_wait(s, buf, size, 0);
		if ( len < 0 ) {
			return len;
		}
		if ( p_asize != NULL ) {
			*p_asize = len;
		}
		return E_OK;
	}
	if ( recno != 0 ) {
		return E_NOEXS;
	}
	d.s = s;
	d.e = &e;
	return knl_oj_make_rea(describe_make, &d, off, buf, size, p_asize);
}

/* Record 1 written: sent */
LOCAL ER obk_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBSK	e;
	INT	s = entry_of_handle(h, &e), n;

	if ( s < 0 ) {
		return E_NOEXS;
	}
	if ( recno == 0 ) {
		return E_RONLY;
	}
	if ( recno != OB_SK_DATA ) {
		return E_NOEXS;
	}
	if ( off != 0 || size < 0 ) {
		return E_PAR;
	}
	n = nowait ? so_send(s, buf, size, MSG_DONTWAIT) : knl_so_send_wait(s, buf, size, 0);
	if ( n < 0 ) {
		return n;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obk_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBSK	e;
	SKDESC	d;
	UB	*t;
	INT	s = entry_of_handle(h, &e), len, k = 0;

	if ( s < 0 ) {
		return E_NOEXS;
	}
	d.s = s;
	d.e = &e;
	len = knl_oj_make(describe_make, &d, &t);
	if ( len < 0 ) {
		return (ER)len;
	}
	Kfree(t);
	if ( k < n ) {
		buf[k].recno = 0;
		buf[k].rt = OB_RT_TAD;
		buf[k].sub = 0;
		buf[k].size = (UD)len;
		k++;
	}
	if ( k < n ) {
		buf[k].recno = OB_SK_DATA;
		buf[k].rt = OB_RT_SYSDATA;
		buf[k].sub = 0;
		buf[k].size = waiting(s);
		k++;
	}
	*p_cnt = k;

	return E_OK;
}

/* An end in the metadata: {"address":..,"port":..}, or null */
LOCAL INT put_end_json( UB *j, INT n, INT max, BOOL has, CONST UB *sa )
{
	if ( !has ) {
		return knl_oj_put(j, n, max, "null");
	}
	n = knl_oj_put(j, n, max, "{\"address\":\"");
	n = put_addr(j, n, max, sa);
	n = knl_oj_put(j, n, max, "\",\"port\":");
	n = knl_oj_put_num(j, n, max, sa_port(sa));
	return knl_oj_put(j, n, max, "}");
}

LOCAL ER obk_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBSK	e;
	SKNOW	k;
	UB	name[OBK_NAME_ROOM];
	INT	s = entry_of_handle(h, &e), n, max = (INT)size;

	if ( s < 0 ) {
		return E_NOEXS;
	}
	now_of(s, e.type, &k);
	(void)put_name(name, 0, sizeof(name), s, e.owner);
	n = knl_oj_put(json, 0, max, "{\"name\":");
	n = knl_oj_put_str(json, n, max, name);
	n = knl_oj_put(json, n, max, ",\"socket\":{\"number\":");
	n = knl_oj_put_num(json, n, max, s);
	n = knl_oj_put(json, n, max, ",\"process\":");
	n = knl_oj_put_num(json, n, max, e.owner);
	n = knl_oj_put(json, n, max, ",\"family\":\"");
	n = knl_oj_put(json, n, max, family_name(e.domain));
	n = knl_oj_put(json, n, max, "\",\"type\":\"");
	n = knl_oj_put(json, n, max, type_name(e.type));
	n = knl_oj_put(json, n, max, "\",\"protocol\":\"");
	n = knl_oj_put(json, n, max, proto_name(e.type, e.protocol));
	n = knl_oj_put(json, n, max, "\",\"local\":");
	n = put_end_json(json, n, max, k.has_local, k.local);
	n = knl_oj_put(json, n, max, ",\"remote\":");
	n = put_end_json(json, n, max, k.has_remote, k.remote);
	n = knl_oj_put(json, n, max, ",\"state\":\"");
	n = knl_oj_put(json, n, max, k.state);
	n = knl_oj_put(json, n, max, "\",\"waiting\":");
	n = knl_oj_put_num(json, n, max, (D)waiting(s));
	n = knl_oj_put(json, n, max, "}}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obk_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	UINT	imask;
	OBSK	*e;
	INT	cnt = 0, s, room = obsk_room();

	ISpinLock(&obsk_lock, &imask);
	while ( cnt < n ) {
		INT	best = -1;

		for ( s = 0; s < room; s++ ) {
			e = OBSK_AT(s);
			if ( !e->used || knl_ob_uuid_zero(&e->uuid) ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&e->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&e->uuid, from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&e->uuid, &OBSK_AT(best)->uuid) < 0 ) {
				best = s;
			}
		}
		if ( best < 0 ) break;
		buf[cnt++] = OBSK_AT(best)->uuid;
	}
	ISpinUnlock(&obsk_lock, &imask);
	*p_cnt = cnt;

	return E_OK;
}

LOCAL CONST T_OBMGR obk_mgr = {
	OB_T_CHANNEL, OB_S_SOCKET, "socket",
	obk_find, obk_ref, obk_prot, NULL, NULL, obk_remove,
	obk_open, obk_close, obk_rea, obk_wri, NULL, NULL, NULL, obk_lrc,
	obk_gat, NULL, NULL, obk_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	NULL,			/* nothing comes in to wait for (ob_rea_tmo) */
	NULL,
	obk_refnam
};

/*
 * Registered after the channel manager, which stays the one that makes
 * a channel of no subtype given. Sockets made before this, when the
 * stack came up, are given their objects now if a UUID can be made.
 */
EXPORT ER knl_obsock_init( void )
{
	TS_UUID	u;
	UINT	imask, gen;
	OBSK	*e;
	INT	no, s, room;

	no = knl_ob_regist(&obk_mgr);
	if ( no <= 0 ) {
		return (ER)no;
	}
	obsk_up = TRUE;
	room = obsk_room();
	for ( s = 0; s < room; s++ ) {
		e = OBSK_AT(s);
		ISpinLock(&obsk_lock, &imask);
		gen = ( e->used && knl_ob_uuid_zero(&e->uuid) ) ? e->gen : 0;
		ISpinUnlock(&obsk_lock, &imask);
		if ( gen == 0 ) {
			continue;
		}
		new_uuid(&u);
		ISpinLock(&obsk_lock, &imask);
		if ( e->used && e->gen == gen ) {
			e->uuid = u;
		}
		ISpinUnlock(&obsk_lock, &imask);
	}
	return E_OK;
}
