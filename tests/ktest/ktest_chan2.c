/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_chan2.c
 *	Pairs of streams between processes (OB_S_STREAM, peripheral_kernel/
 *	obj/obstrm.c; design 18.8) and the local sockets of lib/libcxxrt on
 *	them (ts_chan.c). The kernel's own calls first: making a pair, bytes
 *	both ways, what does not wait, the end of the other end, keys carried
 *	beside the bytes -- with no more rights than they had, and let go
 *	when they are never read -- and the notice of a change, and the poll
 *	sets (OB_S_POLL, obpoll.c) that wait for many ends at once. Then two
 *	processes (tests/uprog/chanprog.cc, /boot/CHANPROG.ELF) that go
 *	through socketpair, SCM_RIGHTS, epoll and the end of one of them;
 *	every end they made is gone once they are.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/gpuapp.h>
#include <ts/time.h>

#define PROG		"/boot/CHANPROG.ELF"
#define RUN_TMO		120000

LOCAL ID	ka, kb;			/* the ends of the pair of the tests */

LOCAL INT same( CONST void *a, CONST void *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( ((CONST UB *)a)[i] != ((CONST UB *)b)[i] ) return 1;
	}
	return 0;
}

/* A pair made and both ends opened as ops; the first end's UUID */
LOCAL ER pair( UINT ops, ID *pa, ID *pb, TS_UUID *pu )
{
	T_OBCRE		c;
	T_OBSTSTAT	s;
	TS_UUID		u;
	SZ		asz = 0;
	ER		er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.sub = OB_S_STREAM;
	er = ob_cre_obj(&c, &u);
	if ( er < E_OK ) return er;
	*pa = ob_opn_obj(&u, ops);
	if ( *pa < E_OK ) return *pa;
	er = ob_rea_rec(*pa, OB_ST_STATE, 0, &s, sizeof(s), &asz);
	if ( er < E_OK ) return er;
	*pb = ob_opn_obj(&s.peer, ops);
	if ( pu != NULL ) *pu = u;
	return ( *pb < E_OK ) ? *pb : E_OK;
}

LOCAL INT ends_now( void )
{
	TS_UUID	buf[64];
	INT	cnt = 0;

	(void)ob_lst_obj(OB_T_CHANNEL, OB_S_STREAM, NULL, buf, 64, &cnt);
	return cnt;
}

LOCAL void test_pair( void )
{
	T_OBREF		r;
	T_OBSTSTAT	s;
	TS_UUID		u;
	SZ		asz = 0;

	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &ka, &kb, &u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_CHANNEL);
	KT_ASSERT_EQ(r.sub, OB_S_STREAM);
	KT_ASSERT(kt_name_is(&u, ""));			/* no name */
	KT_ASSERT_ER(ob_rea_rec(kb, OB_ST_STATE, 0, &s, sizeof(s), &asz), E_OK);
	KT_ASSERT(ts_uuid_cmp(&s.peer, &u) == 0);	/* each names the other */
	KT_ASSERT(( s.flags & OB_SS_PEER ) != 0);
	KT_ASSERT_EQ((INT)s.room, OB_ST_BUF);
}

LOCAL void test_rw( void )
{
	UB	b[16];
	SZ	asz = 0;

	KT_ASSERT_ER(ob_wri_rec(ka, OB_ST_DATA, 0, "hello", 5, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 5);
	KT_ASSERT_ER(ob_rea_rec(kb, OB_ST_DATA, 0, b, sizeof(b), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 5);
	KT_ASSERT(same(b, "hello", 5) == 0);
	KT_ASSERT_ER(ob_wri_rec(kb, OB_ST_DATA, 0, "back", 4, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(ka, OB_ST_DATA, 0, b, 2, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 2);			/* in order, as much as asked */
	KT_ASSERT_ER(ob_rea_rec(ka, OB_ST_DATA, 0, b + 2, 8, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 2);
	KT_ASSERT(same(b, "back", 4) == 0);
}

/* Not waiting: nothing there is E_TMOUT; a full end takes no more; room comes back */
LOCAL void test_nowait( void )
{
	ID	a, b;
	UB	*big = Kmalloc(OB_ST_BUF);
	UB	t[64];
	SZ	asz = 0;

	if ( big == NULL ) KT_SKIP("no memory");
	knl_memset(big, 0x5a, OB_ST_BUF);
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W | OB_O_NOWAIT, &a, &b, NULL), E_OK);
	KT_ASSERT_ER(ob_rea_rec(b, OB_ST_DATA, 0, t, sizeof(t), &asz), E_TMOUT);
	KT_ASSERT_ER(ob_rea_tmo(b, OB_ST_DATA, 0, t, sizeof(t), &asz, 30), E_TMOUT);
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, big, 100, &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, big, OB_ST_BUF, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, OB_ST_BUF - 100);	/* what there was room for */
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, big, 1, &asz), E_TMOUT);
	KT_ASSERT_ER(ob_rea_rec(b, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, big, 1, &asz), E_OK);
	ob_cls_obj(a);
	ob_cls_obj(b);
	Kfree(big);
}

/* The other end gone: the rest is read, then the end; a write is E_IO */
LOCAL void test_hup( void )
{
	ID		a, b;
	T_OBSTSTAT	s;
	UB		t[8];
	SZ		asz = 0;

	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "ab", 2, &asz), E_OK);
	ob_cls_obj(b);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_STATE, 0, &s, sizeof(s), &asz), E_OK);
	KT_ASSERT(( s.flags & OB_SS_PEER ) == 0);
	KT_ASSERT(( s.flags & OB_SS_EOF ) != 0);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 2);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 0);			/* the end, not a wait */
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, "x", 1, &asz), E_IO);
	ob_cls_obj(a);

	/* this end says it writes no more */
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	{
		INT	how = OB_SHUT_WR;

		KT_ASSERT_ER(ob_wri_rec(a, OB_ST_STATE, 0, &how, sizeof(how), &asz), E_OK);
	}
	KT_ASSERT_ER(ob_rea_rec(b, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 0);
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, "x", 1, &asz), E_IO);
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "y", 1, &asz), E_OK);	/* the other way still goes */
	ob_cls_obj(a);
	ob_cls_obj(b);
}

/* A message with keys: its head, room for keys, then bytes */
typedef struct {
	T_OBSTMSG	m;
	T_OBSTKEY	k[4];
	UB		b[16];
} KMSG;

/* Keys carried: the reader's own, never more than they could, the bytes in their place */
LOCAL void test_keys( void )
{
	ID	c, d, ro;
	KMSG	w, r;
	UB	t[8];
	SZ	asz = 0;

	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &c, &d, NULL), E_OK);
	ro = ob_dup_key(d, OB_OP_R, 0);			/* reads only */
	KT_ASSERT(ro > 0);

	KT_ASSERT_ER(ob_wri_rec(ka, OB_ST_DATA, 0, "12", 2, &asz), E_OK);
	knl_memset(&w, 0, sizeof(w));
	w.m.room = 4;
	w.m.nkey = 1;
	w.k[0].key = ro;
	w.b[0] = 'K';
	KT_ASSERT_ER(ob_wri_rec(ka, OB_ST_MSG, 0, &w, OB_STM_HEAD(4) + 1, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 1);
	ob_cls_obj(ro);					/* it stays open: the stream holds it */

	/* the read before the keyed byte stops there and brings no key */
	knl_memset(&r, 0, sizeof(r));
	r.m.room = 4;
	KT_ASSERT_ER(ob_rea_rec(kb, OB_ST_MSG, 0, &r, sizeof(r), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 2);
	KT_ASSERT_EQ(r.m.nkey, 0);
	knl_memset(&r, 0, sizeof(r));
	r.m.room = 4;
	KT_ASSERT_ER(ob_rea_rec(kb, OB_ST_MSG, 0, &r, sizeof(r), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 1);
	KT_ASSERT_EQ(OB_STM_DATA(&r.m)[0], 'K');
	KT_ASSERT_EQ(r.m.nkey, 1);
	KT_ASSERT_EQ(r.m.lost, 0);
	KT_ASSERT_EQ(r.k[0].type, OB_T_CHANNEL);
	KT_ASSERT_EQ(r.k[0].sub, OB_S_STREAM);
	KT_ASSERT(r.k[0].key > 0 && r.k[0].key != ro);

	/* the object is read through the key received, but not written */
	KT_ASSERT_ER(ob_wri_rec(c, OB_ST_DATA, 0, "via", 3, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(r.k[0].key, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 3);
	KT_ASSERT(same(t, "via", 3) == 0);
	KT_ASSERT_ER(ob_wri_rec(r.k[0].key, OB_ST_DATA, 0, "no", 2, &asz), E_OACV);
	ob_cls_obj(r.k[0].key);

	/* a key of the pair itself is refused; keys need a byte to go with */
	knl_memset(&w, 0, sizeof(w));
	w.m.room = 1;
	w.m.nkey = 1;
	w.k[0].key = kb;
	KT_ASSERT_ER(ob_wri_rec(ka, OB_ST_MSG, 0, &w, OB_STM_HEAD(1) + 1, &asz), E_PAR);
	w.k[0].key = c;
	KT_ASSERT_ER(ob_wri_rec(ka, OB_ST_MSG, 0, &w, OB_STM_HEAD(1), &asz), E_PAR);
	ob_cls_obj(c);
	ob_cls_obj(d);
}

/* Keys never read are let go: with the end closed, and with bytes read as record 1 */
LOCAL void test_unread( void )
{
	ID		a, b, c, d;
	KMSG		w;
	T_OBSTSTAT	s;
	UB		t[4];
	SZ		asz = 0;

	/* the end closed with a key waiting in it */
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &c, &d, NULL), E_OK);
	knl_memset(&w, 0, sizeof(w));
	w.m.room = 1;
	w.m.nkey = 1;
	w.k[0].key = d;
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_MSG, 0, &w, OB_STM_HEAD(1) + 1, &asz), E_OK);
	ob_cls_obj(d);
	KT_ASSERT_ER(ob_rea_rec(c, OB_ST_STATE, 0, &s, sizeof(s), &asz), E_OK);
	KT_ASSERT(( s.flags & OB_SS_PEER ) != 0);	/* held by the stream */
	ob_cls_obj(b);					/* never read */
	KT_ASSERT_ER(ob_rea_rec(c, OB_ST_STATE, 0, &s, sizeof(s), &asz), E_OK);
	KT_ASSERT(( s.flags & OB_SS_PEER ) == 0);	/* and let go with it */
	ob_cls_obj(a);
	ob_cls_obj(c);

	/* the bytes read without room for keys */
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &c, &d, NULL), E_OK);
	w.k[0].key = d;
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_MSG, 0, &w, OB_STM_HEAD(1) + 1, &asz), E_OK);
	ob_cls_obj(d);
	KT_ASSERT_ER(ob_rea_rec(b, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, 1);
	KT_ASSERT_ER(ob_rea_rec(c, OB_ST_STATE, 0, &s, sizeof(s), &asz), E_OK);
	KT_ASSERT(( s.flags & OB_SS_PEER ) == 0);
	ob_cls_obj(a);
	ob_cls_obj(b);
	ob_cls_obj(c);
}

/* A change is told on the port: bytes came, and the other end went */
LOCAL void test_notice( void )
{
	T_OBCRE	c;
	T_OBNTF	n;
	T_OBNTM	m;
	TS_UUID	pu;
	ID	port, a, b;
	SZ	asz = 0;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.sub = OB_S_QUEUE;
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	port = ob_opn_obj(&pu, OB_OP_R | OB_OP_W);
	KT_ASSERT(port > 0);
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	n.events = OB_E_CHANGE;
	n.id = 77;
	n.flags = 0;
	KT_ASSERT(ob_ntf_evt(b, OB_ST_DATA, &n, port) > 0);
	KT_ASSERT_ER(ob_wri_rec(a, OB_ST_DATA, 0, "z", 1, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, 2000), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_CHANGE);
	KT_ASSERT_EQ(m.id, 77);
	KT_ASSERT_EQ(m.recno, OB_ST_DATA);
	ob_cls_obj(a);
	KT_ASSERT_ER(ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, 2000), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_CHANGE);
	ob_cls_obj(b);
	ob_cls_obj(port);
	(void)ob_del_obj(&pu);
}

/* ---------------------------------------------------------------- poll sets */

LOCAL ID pset( void )
{
	T_OBCRE	c;
	TS_UUID	u;
	ID	k = 0;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.sub = OB_S_POLL;
	if ( ob_cre_key(&c, OB_OP_R | OB_OP_W, &k, &u) < E_OK ) return 0;
	return k;
}

LOCAL ER pctl( ID s, INT op, ID key, UINT events, UD data )
{
	T_OBPLCTL	c;
	SZ		asz = 0;

	knl_memset(&c, 0, sizeof(c));
	c.op = op;
	c.key = key;
	c.events = events;
	c.data = data;
	return ob_wri_rec(s, OB_PL_REC, 0, &c, sizeof(c), &asz);
}

/* The keys ready in s, waited for up to tmo: how many (ev[0..]), or an error */
LOCAL INT pwait( ID s, T_OBPLEV *ev, INT n, TMO tmo )
{
	SZ	asz = 0;
	ER	er = ob_rea_tmo(s, OB_PL_REC, 0, ev, (SZ)sizeof(T_OBPLEV) * n, &asz, tmo);

	return ( er < E_OK ) ? (INT)er : (INT)( asz / (SZ)sizeof(T_OBPLEV) );
}

LOCAL INT sets_now( void )
{
	TS_UUID	buf[16];
	INT	cnt = 0;

	(void)ob_lst_obj(OB_T_CHANNEL, OB_S_POLL, NULL, buf, 16, &cnt);
	return cnt;
}

/*
 * A set told by the ends it holds: level, for once, edge, a change, a key
 * taken out, two sets on one end, the other end gone, the end itself
 * gone, a key it may not watch, and what is left when it is closed
 */
LOCAL void test_poll( void )
{
	T_OBPLEV	ev[4];
	ID		s, s2, a, b, c, d;
	UB		t[8];
	SZ		asz = 0;
	INT		ends0 = ends_now(), sets0 = sets_now();

	s = pset();
	s2 = pset();
	KT_ASSERT(s > 0 && s2 > 0);
	if ( s <= 0 || s2 <= 0 ) return;
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W | OB_O_NOWAIT, &a, &b, NULL), E_OK);
	KT_ASSERT_ER(pctl(s, OB_PL_ADD, a, OB_PL_IN, 11), E_OK);
	KT_ASSERT_ER(pctl(s, OB_PL_ADD, a, OB_PL_IN, 11), E_OBJ);
	KT_ASSERT_ER(pctl(s2, OB_PL_ADD, a, OB_PL_IN | OB_PL_RDHUP, 22), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_EQ(pwait(s, ev, 4, 30), E_TMOUT);

	/* level: told while it stays ready, in both sets */
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "xy", 2, &asz), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, 2000), 1);
	KT_ASSERT_EQ(ev[0].events, OB_PL_IN);
	KT_ASSERT((INT)ev[0].data == 11);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), 1);
	KT_ASSERT_EQ(pwait(s2, ev, 4, TMO_POL), 1);
	KT_ASSERT((INT)ev[0].data == 22);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);	/* read: no longer ready */
	KT_ASSERT_EQ(pwait(s2, ev, 4, TMO_POL), E_TMOUT);

	/* for once: not again until it is changed */
	KT_ASSERT_ER(pctl(s, OB_PL_MOD, a, OB_PL_IN | OB_PL_ONESHOT, 12), E_OK);
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "z", 1, &asz), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, 2000), 1);
	KT_ASSERT((INT)ev[0].data == 12);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "z", 1, &asz), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(pctl(s, OB_PL_MOD, a, OB_PL_IN | OB_PL_ONESHOT, 13), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), 1);		/* ready as it is changed */
	KT_ASSERT((INT)ev[0].data == 13);

	/* edge: once for each change */
	KT_ASSERT_ER(pctl(s, OB_PL_MOD, a, OB_PL_IN | OB_PL_ET, 14), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), 1);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(b, OB_ST_DATA, 0, "w", 1, &asz), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), 1);
	KT_ASSERT_ER(ob_rea_rec(a, OB_ST_DATA, 0, t, sizeof(t), &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(s2, 0, 0, t, 0, &asz), E_OK);	/* record 0 is there */

	/* room to write; then taken out */
	KT_ASSERT_ER(pctl(s, OB_PL_ADD, b, OB_PL_OUT, 15), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), 1);
	KT_ASSERT_EQ(ev[0].events, OB_PL_OUT);
	KT_ASSERT((INT)ev[0].data == 15);
	KT_ASSERT_ER(pctl(s, OB_PL_DEL, b, 0, 0), E_OK);
	KT_ASSERT_ER(pctl(s, OB_PL_DEL, b, 0, 0), E_NOEXS);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);

	/* woken with nothing, at once or under way */
	KT_ASSERT_ER(pctl(s, OB_PL_WAKE, 0, 0, 0), E_OK);
	KT_ASSERT_EQ(pwait(s, ev, 4, 2000), 0);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);

	/* the other end gone: an end reached and a hang-up, to the set that asked too nothing more to come */
	KT_ASSERT_ER(pctl(s, OB_PL_MOD, a, OB_PL_IN, 16), E_OK);
	ob_cls_obj(b);
	KT_ASSERT_EQ(pwait(s, ev, 4, 2000), 1);
	KT_ASSERT_EQ(ev[0].events, OB_PL_IN | OB_PL_HUP);
	KT_ASSERT_EQ(pwait(s2, ev, 4, 2000), 1);
	KT_ASSERT_EQ(ev[0].events, OB_PL_IN | OB_PL_HUP | OB_PL_RDHUP);

	/* the end closed: out of both sets, nothing told */
	ob_cls_obj(a);
	KT_ASSERT_EQ(pwait(s, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_EQ(pwait(s2, ev, 4, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(pctl(s, OB_PL_DEL, a, 0, 0), E_ID);

	/* a key that cannot be watched, and one that may neither read nor write */
	KT_ASSERT_ER(pctl(s, OB_PL_ADD, s2, OB_PL_IN, 0), E_NOSPT);
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &c, &d, NULL), E_OK);
	{
		ID	x = ob_dup_key(c, OB_OP_ATRRD, 0);

		KT_ASSERT(x > 0);
		if ( x > 0 ) {
			KT_ASSERT_ER(pctl(s, OB_PL_ADD, x, OB_PL_IN, 0), E_OACV);
			ob_cls_obj(x);
		}
	}

	/* closed with keys in it: the ends see nothing of it */
	KT_ASSERT_ER(pctl(s, OB_PL_ADD, c, OB_PL_IN, 17), E_OK);
	KT_ASSERT_ER(pctl(s2, OB_PL_ADD, c, OB_PL_IN, 18), E_OK);
	ob_cls_obj(s);
	KT_ASSERT_ER(ob_wri_rec(d, OB_ST_DATA, 0, "q", 1, &asz), E_OK);
	KT_ASSERT_EQ(pwait(s2, ev, 4, 2000), 1);
	KT_ASSERT((INT)ev[0].data == 18);
	ob_cls_obj(s2);
	ob_cls_obj(c);
	ob_cls_obj(d);
	KT_ASSERT_EQ(sets_now(), sets0);
	KT_ASSERT_EQ(ends_now(), ends0);
}

/* A wait under way woken by a task of the kernel: a write to the end, then OB_PL_WAKE */
LOCAL ID	pw_set, pw_end;

LOCAL void poll_kick( INT stacd, void *exinf )
{
	SZ	asz = 0;

	(void)exinf;
	tk_dly_tsk(50);
	if ( stacd == 0 ) (void)ob_wri_rec(pw_end, OB_ST_DATA, 0, "k", 1, &asz);
	else (void)pctl(pw_set, OB_PL_WAKE, 0, 0, 0);
	tk_exd_tsk();
}

LOCAL void test_poll_wake( void )
{
	T_OBPLEV	ev[2];
	ID		a, b, t;
	INT		k;

	pw_set = pset();
	KT_ASSERT(pw_set > 0);
	if ( pw_set <= 0 ) return;
	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &a, &b, NULL), E_OK);
	pw_end = b;
	KT_ASSERT_ER(pctl(pw_set, OB_PL_ADD, a, OB_PL_IN, 31), E_OK);
	for ( k = 0; k < 2; k++ ) {
		t = kt_cre_tsk_any((FP)poll_kick, KT_PRI_MAIN, NULL);
		KT_ASSERT(t > 0);
		if ( t <= 0 ) break;
		(void)tk_sta_tsk(t, k);
		if ( k == 0 ) {
			KT_ASSERT_EQ(pwait(pw_set, ev, 2, 5000), 1);
			KT_ASSERT((INT)ev[0].data == 31);
			KT_ASSERT_ER(pctl(pw_set, OB_PL_DEL, a, 0, 0), E_OK);
		} else {
			KT_ASSERT_EQ(pwait(pw_set, ev, 2, 5000), 0);
		}
	}
	tk_dly_tsk(10);
	ob_cls_obj(pw_set);
	ob_cls_obj(a);
	ob_cls_obj(b);
}

/* The test's own pair closed: no end of the kernel's tests is left */
LOCAL void test_closed( void )
{
	ob_cls_obj(ka);
	ob_cls_obj(kb);
	KT_ASSERT_EQ(ends_now(), 0);
}

/* A BO object is made only of a BO of the caller's */
LOCAL void test_gpubo( void )
{
	T_OBCRE		c;
	T_OBGBMAKE	mk;
	TS_UUID		u;

	knl_memset(&mk, 0, sizeof(mk));
	mk.name = 0x7FFF01;				/* no such name */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_GPUBO;
	c.arg = &mk;
	c.argsz = sizeof(mk);
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_ID);
	c.argsz = 0;
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_PAR);
}

/* Two processes over socketpair, SCM_RIGHTS and epoll; their ends gone with them */
LOCAL void test_procs( void )
{
	T_FSTAT	st;
	T_CPRC	cprc;
	T_PSTS	pp, cp;
	UB	arg[24];
	ID	child, par;
	INT	n;
	ER	er;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG);
	}
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.pri = KT_PRI_HIGH;
	cprc.arg = "child";
	cprc.argsz = 5;
	child = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(child > 0);
	if ( child <= 0 ) return;
	knl_strcpy((char *)arg, "parent:");
	n = 7;
	{
		char	d[12];
		INT	k = 0, v = child;

		do { d[k++] = (char)( '0' + v % 10 ); v /= 10; } while ( v > 0 );
		while ( k > 0 ) arg[n++] = (UB)d[--k];
	}
	arg[n] = 0;
	cprc.arg = arg;
	cprc.argsz = n;
	par = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(par > 0);
	pp.exitcd = cp.exitcd = -1;
	er = ( par > 0 ) ? ts_wai_prc(par, &pp, RUN_TMO) : E_NOEXS;
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK && par > 0 ) {
		(void)ts_ter_prc(par, TS_ABORT_TERM);
		(void)ts_wai_prc(par, &pp, 5000);
	}
	er = ts_wai_prc(child, &cp, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(child, TS_ABORT_TERM);
		(void)ts_wai_prc(child, &cp, 5000);
	}
	tm_printf((UB *)"  exit codes: parent %d, child %d\n", pp.exitcd, cp.exitcd);
	KT_ASSERT_EQ(pp.exitcd, 0);
	KT_ASSERT_EQ(cp.exitcd, 0);
	KT_ASSERT_EQ(ends_now(), 0);
	KT_ASSERT_EQ(sets_now(), 0);			/* their poll sets went with them */
}

/* ---------------------------------------------------------------- round trips timed */

/*
 * Round trips timed one by one, printed and not judged: two tasks of the
 * kernel by semaphores (on one processor and on any) and over a pair of
 * ends, then two processes (CHANPROG.ELF latp/latc) in the ways a stream
 * is waited on. LAT_N round trips each.
 */
#define LAT_N		300

LOCAL UW	lat_ns[LAT_N];
LOCAL ID	lat_s1, lat_s2, lat_ea, lat_eb;
LOCAL INT	lat_size;

LOCAL UD lat_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns;
}

LOCAL void lat_print( CONST char *what, INT size, INT n )
{
	UD	sum = 0;
	INT	i, j, gap;
	UW	v;

	if ( n <= 0 ) {
		tm_printf((UB *)"  lat %s %d: none\n", what, size);
		return;
	}
	for ( i = 0; i < n; i++ ) sum += lat_ns[i];
	for ( gap = n / 2; gap > 0; gap /= 2 ) {
		for ( i = gap; i < n; i++ ) {
			v = lat_ns[i];
			for ( j = i; j >= gap && lat_ns[j - gap] > v; j -= gap ) lat_ns[j] = lat_ns[j - gap];
			lat_ns[j] = v;
		}
	}
	tm_printf((UB *)"  lat %s %d: n %d mean %d median %d p99 %d max %d us\n", what, size, n,
		  (INT)( sum / (UD)n / 1000 ), (INT)( lat_ns[n / 2] / 1000 ),
		  (INT)( lat_ns[n * 99 / 100] / 1000 ), (INT)( lat_ns[n - 1] / 1000 ));
}

LOCAL void lat_sem_echo( INT stacd, void *exinf )
{
	INT	i;

	(void)stacd;
	(void)exinf;
	for ( i = 0; i < LAT_N; i++ ) {
		if ( tk_wai_sem(lat_s1, 1, 20000) < E_OK ) break;
		(void)tk_sig_sem(lat_s2, 1);
	}
	tk_exd_tsk();
}

LOCAL void lat_end_echo( INT stacd, void *exinf )
{
	UB	b[64];
	SZ	asz;
	INT	i, k;

	(void)stacd;
	(void)exinf;
	for ( i = 0; i < LAT_N; i++ ) {
		for ( k = 0; k < lat_size; k += (INT)asz ) {
			asz = 0;
			if ( ob_rea_tmo(lat_eb, OB_ST_DATA, 0, b, lat_size - k, &asz, 20000) < E_OK ) goto out;
		}
		if ( ob_wri_rec(lat_eb, OB_ST_DATA, 0, b, lat_size, &asz) < E_OK ) break;
	}
out:
	tk_exd_tsk();
}

LOCAL void lat_sems( BOOL any )
{
	T_CSEM	c;
	ID	t;
	INT	i;
	UD	t0;

	knl_memset(&c, 0, sizeof(c));
	c.sematr = TA_TFIFO | TA_FIRST;
	c.maxsem = 1;
	lat_s1 = tk_cre_sem(&c);
	lat_s2 = tk_cre_sem(&c);
	KT_ASSERT(lat_s1 > 0 && lat_s2 > 0);
	t = any ? kt_cre_tsk_any((FP)lat_sem_echo, KT_PRI_MAIN, NULL)
		: kt_cre_tsk((FP)lat_sem_echo, KT_PRI_MAIN, NULL);
	KT_ASSERT(t > 0);
	if ( t <= 0 ) return;
	(void)tk_sta_tsk(t, 0);
	for ( i = 0; i < LAT_N; i++ ) {
		t0 = lat_now();
		(void)tk_sig_sem(lat_s1, 1);
		if ( tk_wai_sem(lat_s2, 1, 20000) < E_OK ) break;
		lat_ns[i] = (UW)( lat_now() - t0 );
	}
	KT_ASSERT_EQ(i, LAT_N);
	lat_print(any ? "ktasks-sem-any" : "ktasks-sem", 0, i);
	tk_dly_tsk(10);
	(void)tk_del_sem(lat_s1);
	(void)tk_del_sem(lat_s2);
}

LOCAL void lat_ends( INT size, BOOL any )
{
	UB	b[64];
	SZ	asz;
	ID	t;
	INT	i, k;
	UD	t0;

	KT_ASSERT_ER(pair(OB_OP_R | OB_OP_W, &lat_ea, &lat_eb, NULL), E_OK);
	lat_size = size;
	knl_memset(b, 0x5a, sizeof(b));
	t = any ? kt_cre_tsk_any((FP)lat_end_echo, KT_PRI_MAIN, NULL)
		: kt_cre_tsk((FP)lat_end_echo, KT_PRI_MAIN, NULL);
	KT_ASSERT(t > 0);
	if ( t <= 0 ) return;
	(void)tk_sta_tsk(t, 0);
	for ( i = 0; i < LAT_N; i++ ) {
		t0 = lat_now();
		if ( ob_wri_rec(lat_ea, OB_ST_DATA, 0, b, size, &asz) < E_OK ) break;
		for ( k = 0; k < size; k += (INT)asz ) {
			asz = 0;
			if ( ob_rea_tmo(lat_ea, OB_ST_DATA, 0, b, size - k, &asz, 20000) < E_OK ) goto out;
		}
		lat_ns[i] = (UW)( lat_now() - t0 );
	}
out:
	KT_ASSERT_EQ(i, LAT_N);
	lat_print(any ? "ktasks-ends-any" : "ktasks-ends", size, i);
	tk_dly_tsk(10);
	(void)ob_cls_obj(lat_ea);
	(void)ob_cls_obj(lat_eb);
}

LOCAL void test_lat_kernel( void )
{
	lat_sems(FALSE);
	lat_sems(TRUE);
	lat_ends(1, FALSE);
	lat_ends(64, FALSE);
	lat_ends(1, TRUE);
	lat_ends(64, TRUE);
}

LOCAL void test_lat_procs( void )
{
	T_FSTAT	st;
	T_CPRC	cprc;
	T_PSTS	pp, cp;
	UB	arg[32];
	ID	child, par;
	INT	n;
	ER	er;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG);
	}
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.pri = KT_PRI_HIGH;
	cprc.arg = "latc";
	cprc.argsz = 4;
	child = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(child > 0);
	if ( child <= 0 ) return;
	knl_strcpy((char *)arg, "latp:");
	n = 5;
	{
		char	d[12];
		INT	k, v, f;

		for ( f = 0; f < 2; f++ ) {
			v = ( f == 0 ) ? child : LAT_N;
			k = 0;
			do { d[k++] = (char)( '0' + v % 10 ); v /= 10; } while ( v > 0 );
			while ( k > 0 ) arg[n++] = (UB)d[--k];
			if ( f == 0 ) arg[n++] = ':';
		}
	}
	arg[n] = 0;
	cprc.arg = arg;
	cprc.argsz = n;
	par = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(par > 0);
	pp.exitcd = cp.exitcd = -1;
	er = ( par > 0 ) ? ts_wai_prc(par, &pp, RUN_TMO * 5) : E_NOEXS;
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK && par > 0 ) {
		(void)ts_ter_prc(par, TS_ABORT_TERM);
		(void)ts_wai_prc(par, &pp, 5000);
	}
	er = ts_wai_prc(child, &cp, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(child, TS_ABORT_TERM);
		(void)ts_wai_prc(child, &cp, 5000);
	}
	tm_printf((UB *)"  exit codes: parent %d, child %d\n", pp.exitcd, cp.exitcd);
	KT_ASSERT_EQ(pp.exitcd, 0);
	KT_ASSERT_EQ(cp.exitcd, 0);
}

EXPORT void ktest_chan2( void )
{
	KT_RUN(test_pair);
	KT_RUN(test_rw);
	KT_RUN(test_nowait);
	KT_RUN(test_hup);
	KT_RUN(test_keys);
	KT_RUN(test_unread);
	KT_RUN(test_notice);
	KT_RUN(test_poll);
	KT_RUN(test_poll_wake);
	KT_RUN(test_closed);
	KT_RUN(test_gpubo);
	KT_RUN(test_procs);
	KT_RUN(test_lat_kernel);
	KT_RUN(test_lat_procs);
}
