/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_btuart.c
 *	A Bluetooth controller on a UART (design 6.8), as far as it is
 *	code over bytes: H4's packets made and found again in a stream, the
 *	stream cut anywhere, bytes that are not a packet passed over; a patch
 *	file checked whole and the broken ones refused; and the start of a
 *	Broadcom controller with its patch against a controller made in
 *	software, which answers each command at once: the order of the
 *	commands, the speeds, the patch's commands sent as they are, the
 *	board's address given, and where it stops when something fails.
 *	The UART itself is the Raspberry Pi 5's (bt_uart.c), on the machine.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/bt.h>
#include "ktest.h"
#include "../../device/usb/usbdev.h"
#include "../../device/bt/btint.h"
#include "../../device/bt/bt_bcm.h"

LOCAL BOOL same( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- H4 */

/* What came out of the stream: each packet's type, length and bytes one after the other */
LOCAL struct {
	INT	n;
	UB	type[16];
	INT	len[16];
	UB	b[2048];
	INT	used;
} got;

LOCAL void got_out( void *ctx, UB type, CONST UB *p, INT len )
{
	(void)ctx;
	if ( got.n >= 16 || got.used + len > (INT)sizeof(got.b) ) return;
	got.type[got.n] = type;
	got.len[got.n] = len;
	knl_memcpy(got.b + got.used, p, len);
	got.used += len;
	got.n++;
}

LOCAL CONST UB	ev_done[] = { 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };	/* Reset's Command Complete */
LOCAL CONST UB	ev_none[] = { 0xff, 0x00 };				/* an event without parameters */
LOCAL CONST UB	acl_in[] = { 0x40, 0x20, 0x05, 0x00, 1, 2, 3, 4, 5 };	/* handle 0x040, 5 bytes */
LOCAL CONST UB	sco_in[] = { 0x01, 0x00, 0x02, 0xaa, 0xbb };

LOCAL void test_h4_pack( void )
{
	UB	out[16];
	UB	cmd[] = { 0x03, 0x0c, 0x00 };
	UB	badlen[] = { 0x03, 0x0c, 0x02, 0x00 };	/* says 2 parameters, has 1 */

	KT_ASSERT_EQ(knl_bth4_pack(H4_CMD, cmd, 3, out, sizeof(out)), 4);
	KT_ASSERT_EQ(out[0], 0x01);
	KT_ASSERT(same(out + 1, cmd, 3));
	KT_ASSERT_EQ(knl_bth4_pack(H4_ACL, acl_in, sizeof(acl_in), out, sizeof(out)), (INT)sizeof(acl_in) + 1);
	KT_ASSERT_EQ(out[0], 0x02);
	KT_ASSERT_EQ(knl_bth4_pack(H4_EVENT, ev_done, sizeof(ev_done), out, sizeof(out)), (INT)sizeof(ev_done) + 1);
	KT_ASSERT_EQ(out[0], 0x04);
	/* a header that does not say the length given, a type not known, no room */
	KT_ASSERT_EQ(knl_bth4_pack(H4_CMD, badlen, sizeof(badlen), out, sizeof(out)), E_PAR);
	KT_ASSERT_EQ(knl_bth4_pack(H4_CMD, cmd, 2, out, sizeof(out)), E_PAR);
	KT_ASSERT_EQ(knl_bth4_pack(0x06, cmd, 3, out, sizeof(out)), E_PAR);
	KT_ASSERT_EQ(knl_bth4_pack(H4_ACL, acl_in, sizeof(acl_in), out, 6), E_PAR);
	KT_ASSERT_EQ(knl_bth4_hlen(H4_ISO), 4);
	KT_ASSERT_EQ(knl_bth4_hlen(0x00), 0);
}

/* A stream: an event, a stray byte, ACL, SCO, an event of no parameters, a packet too long, an event */
LOCAL INT make_stream( UB *s )
{
	INT	n = 0, i;

	s[n++] = H4_EVENT;
	knl_memcpy(s + n, ev_done, sizeof(ev_done));
	n += sizeof(ev_done);
	s[n++] = 0x00;					/* not a type */
	s[n++] = 0x7f;
	s[n++] = H4_ACL;
	knl_memcpy(s + n, acl_in, sizeof(acl_in));
	n += sizeof(acl_in);
	s[n++] = H4_SCO;
	knl_memcpy(s + n, sco_in, sizeof(sco_in));
	n += sizeof(sco_in);
	s[n++] = H4_EVENT;
	knl_memcpy(s + n, ev_none, sizeof(ev_none));
	n += sizeof(ev_none);
	s[n++] = H4_ACL;				/* 1200 bytes of data: more than is held */
	s[n++] = 0x41;
	s[n++] = 0x20;
	s[n++] = (UB)1200;
	s[n++] = (UB)( 1200 >> 8 );
	for ( i = 0; i < 1200; i++ ) s[n++] = (UB)( i == 0 ? H4_EVENT : 0x5a );	/* a type byte inside: not taken for one */
	s[n++] = H4_EVENT;
	knl_memcpy(s + n, ev_done, sizeof(ev_done));
	n += sizeof(ev_done);
	return n;
}

LOCAL void check_stream( BTH4 *h )
{
	INT	at = 0;

	KT_ASSERT_EQ(got.n, 5);
	KT_ASSERT_EQ(h->frames, 5);
	KT_ASSERT_EQ(h->bad_type, 2);
	KT_ASSERT_EQ(h->too_long, 1);
	if ( got.n != 5 ) return;
	KT_ASSERT_EQ(got.type[0], H4_EVENT);
	KT_ASSERT_EQ(got.len[0], (INT)sizeof(ev_done));
	KT_ASSERT(same(got.b + at, ev_done, sizeof(ev_done)));
	at += got.len[0];
	KT_ASSERT_EQ(got.type[1], H4_ACL);
	KT_ASSERT_EQ(got.len[1], (INT)sizeof(acl_in));
	KT_ASSERT(same(got.b + at, acl_in, sizeof(acl_in)));
	at += got.len[1];
	KT_ASSERT_EQ(got.type[2], H4_SCO);
	KT_ASSERT(same(got.b + at, sco_in, sizeof(sco_in)));
	at += got.len[2];
	KT_ASSERT_EQ(got.type[3], H4_EVENT);
	KT_ASSERT_EQ(got.len[3], 2);
	at += got.len[3];
	KT_ASSERT_EQ(got.type[4], H4_EVENT);
	KT_ASSERT(same(got.b + at, ev_done, sizeof(ev_done)));
}

LOCAL void test_h4_stream( void )
{
	static UB	s[1400];
	static BTH4	h;
	INT		n = make_stream(s), i, cut;

	/* whole, a byte at a time, and in pieces of every size up to 13 */
	knl_memset(&got, 0, sizeof(got));
	knl_bth4_init(&h);
	knl_bth4_feed(&h, s, n, got_out, NULL);
	check_stream(&h);

	knl_memset(&got, 0, sizeof(got));
	knl_bth4_init(&h);
	for ( i = 0; i < n; i++ ) knl_bth4_feed(&h, s + i, 1, got_out, NULL);
	check_stream(&h);

	for ( cut = 2; cut <= 13; cut++ ) {
		knl_memset(&got, 0, sizeof(got));
		knl_bth4_init(&h);
		for ( i = 0; i < n; i += cut ) knl_bth4_feed(&h, s + i, ( n - i < cut ) ? n - i : cut, got_out, NULL);
		KT_ASSERT_EQ(got.n, 5);
		KT_ASSERT_EQ(h.too_long, 1);
	}
}

/* ---------------------------------------------------------------- the patch file */

/* Write RAM twice and Launch RAM: the form of a patch */
LOCAL CONST UB	hcd_good[] = {
	0x4c, 0xfc, 0x06, 0x00, 0x10, 0x20, 0x00, 0xaa, 0xbb,
	0x4c, 0xfc, 0x05, 0x04, 0x10, 0x20, 0x00, 0xcc,
	0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff,
};

LOCAL void test_hcd_check( void )
{
	UB	b[sizeof(hcd_good) + 4];
	INT	at = -1;

	KT_ASSERT_EQ(knl_btb_hcd_check(hcd_good, sizeof(hcd_good), &at), 3);
	/* cut in the last command's parameters, and in its header */
	KT_ASSERT_EQ(knl_btb_hcd_check(hcd_good, sizeof(hcd_good) - 1, &at), BTB_HCD_SHORT);
	KT_ASSERT_EQ(at, 17);
	KT_ASSERT_EQ(knl_btb_hcd_check(hcd_good, 19, &at), BTB_HCD_SHORT);
	KT_ASSERT_EQ(at, 17);
	/* two bytes after the end: a command cut too */
	knl_memcpy(b, hcd_good, sizeof(hcd_good));
	b[sizeof(hcd_good)] = 0x4c;
	b[sizeof(hcd_good) + 1] = 0xfc;
	KT_ASSERT_EQ(knl_btb_hcd_check(b, sizeof(hcd_good) + 2, &at), BTB_HCD_SHORT);
	KT_ASSERT_EQ(at, (INT)sizeof(hcd_good));
	/* a command of another group: Reset in the middle */
	knl_memcpy(b, hcd_good, sizeof(hcd_good));
	b[9] = 0x03;
	b[10] = 0x0c;
	KT_ASSERT_EQ(knl_btb_hcd_check(b, sizeof(hcd_good), &at), BTB_HCD_NOTVENDOR);
	KT_ASSERT_EQ(at, 9);
	/* no Launch RAM at its end */
	KT_ASSERT_EQ(knl_btb_hcd_check(hcd_good, 17, &at), BTB_HCD_NOLAUNCH);
	KT_ASSERT_EQ(knl_btb_hcd_check(hcd_good, 0, &at), BTB_HCD_EMPTY);
	KT_ASSERT_EQ(knl_btb_hcd_check(NULL, 10, &at), BTB_HCD_EMPTY);
}

/* ---------------------------------------------------------------- the start, against a controller in software */

#define FK_OPS		32

LOCAL struct {
	UH	ops[FK_OPS];
	INT	nops;
	UW	speed[8];
	INT	nspeed;
	UH	fail_op;		/* answered with a status not 0 */
	BOOL	silent;			/* nothing answered */
	UB	baud_p[6];		/* the last speed command's parameters */
	UB	addr[6];		/* what it answers for its address */
	UB	given[6];		/* the address written */
	UB	patch[sizeof(hcd_good)];	/* the vendor's commands as they came, the patch's form */
	INT	npatch;
} fk;

LOCAL ER fk_cmd( BTC *c, UB *b, INT n )
{
	UH	op = (UH)( b[0] | ( b[1] << 8 ) );
	UB	ev[32];
	INT	k = 6;

	if ( fk.nops < FK_OPS ) fk.ops[fk.nops++] = op;
	if ( ( op == 0xfc4c || op == 0xfc4e ) && fk.npatch + n <= (INT)sizeof(fk.patch) ) {
		knl_memcpy(fk.patch + fk.npatch, b, n);
		fk.npatch += n;
	}
	if ( op == 0xfc18 && n == 9 ) knl_memcpy(fk.baud_p, b + 3, 6);
	if ( op == 0xfc01 && n == 9 ) knl_memcpy(fk.given, b + 3, 6);
	if ( fk.silent ) return E_OK;
	ev[0] = 0x0e;
	ev[2] = 1;
	ev[3] = b[0];
	ev[4] = b[1];
	ev[5] = ( op == fk.fail_op ) ? 0x12 : 0x00;
	if ( ev[5] == 0 && op == 0x1001 ) {
		ev[6] = 9;			/* HCI 5.0 */
		ev[7] = 0x34;			/* its revision */
		ev[8] = 0x12;
		ev[9] = 9;
		ev[10] = 0x0f;			/* the maker */
		ev[11] = 0x00;
		ev[12] = 0x19;			/* subversion 0x6119 */
		ev[13] = 0x61;
		k = 14;
	}
	if ( ev[5] == 0 && op == 0x1009 ) {
		knl_memcpy(ev + 6, fk.addr, 6);
		k = 12;
	}
	ev[1] = (UB)( k - 2 );
	knl_btx_ring_in(c, 0, ev, k);
	return E_OK;
}

LOCAL ER fk_acl( BTC *c, UB *b, INT n )
{
	(void)c;
	(void)b;
	(void)n;
	return E_OK;
}

LOCAL ER fk_speed( BTC *c, UW baud )
{
	(void)c;
	if ( fk.nspeed < 8 ) fk.speed[fk.nspeed++] = baud;
	return E_OK;
}

LOCAL CONST BTTRANS	fk_tr = { fk_cmd, fk_acl, NULL };

/* A controller with only what the start uses: the ring and its semaphore */
LOCAL BTC *fk_new( void )
{
	BTC	*c = (BTC *)Kcalloc(1, sizeof(BTC));
	T_CSEM	cs;

	if ( c == NULL ) return NULL;
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RING_N * 2;
	c->sem = tk_cre_sem(&cs);
	if ( c->sem <= 0 ) {
		Kfree(c);
		return NULL;
	}
	c->tr = &fk_tr;
	c->st.state = BT_ST_STARTING;
	return c;
}

LOCAL void fk_del( BTC *c )
{
	tk_del_sem(c->sem);
	Kfree(c);
}

LOCAL void fk_cfg( BTBCFG *cfg )
{
	static CONST UB	board[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
	static CONST UB	ph[6] = { 0xac, 0x1f, 0x00, 0xc0, 0x45, 0x43 };

	knl_memset(&fk, 0, sizeof(fk));
	knl_memcpy(fk.addr, ph, 6);
	knl_memset(cfg, 0, sizeof(*cfg));
	cfg->init_baud = 115200;
	cfg->oper_baud = 3000000;
	knl_memcpy(cfg->bdaddr, board, 6);
	cfg->speed = fk_speed;
}

LOCAL void test_bcm_start( void )
{
	static CONST UH	want[] = {
		0x0c03, 0x1001, 0xfc18, 0x1001,			/* reset, which chip, faster */
		0xfc2e, 0xfc4c, 0xfc4c, 0xfc4e,			/* the patch */
		0x0c03, 0x1001, 0xfc18, 0x1001,			/* reset again, faster again */
		0x1009, 0xfc01,					/* the board's address */
	};
	static CONST UB	baud3m[6] = { 0x00, 0x00, 0xc0, 0xc6, 0x2d, 0x00 };	/* 3000000 */
	BTBCFG		cfg;
	BTC		*c = fk_new();
	INT		i;

	KT_ASSERT(c != NULL);
	if ( c == NULL ) return;
	fk_cfg(&cfg);
	KT_ASSERT(knl_btb_run(c, &cfg, hcd_good, sizeof(hcd_good), "test.hcd"));
	KT_ASSERT_EQ(fk.nops, (INT)( sizeof(want) / sizeof(want[0]) ));
	for ( i = 0; i < fk.nops && i < (INT)( sizeof(want) / sizeof(want[0]) ); i++ ) {
		KT_ASSERT_EQ(fk.ops[i], want[i]);
	}
	/* the host's end: faster, back for the patch's reset, faster again */
	KT_ASSERT_EQ(fk.nspeed, 3);
	KT_ASSERT_EQ(fk.speed[0], 3000000);
	KT_ASSERT_EQ(fk.speed[1], 115200);
	KT_ASSERT_EQ(fk.speed[2], 3000000);
	KT_ASSERT(same(fk.baud_p, baud3m, 6));
	/* the patch's commands went as the file has them */
	KT_ASSERT_EQ(fk.npatch, (INT)sizeof(hcd_good));
	KT_ASSERT(same(fk.patch, hcd_good, sizeof(hcd_good)));
	KT_ASSERT(same(fk.given, cfg.bdaddr, 6));
	KT_ASSERT(c->step != -1);
	fk_del(c);
}

LOCAL void test_bcm_fail( void )
{
	BTBCFG	cfg;
	BTC	*c = fk_new();
	UB	cut[sizeof(hcd_good) - 1];
	INT	i;
	BOOL	download;

	KT_ASSERT(c != NULL);
	if ( c == NULL ) return;

	/* a command of the patch refused: it stops there, the rest not sent */
	fk_cfg(&cfg);
	fk.fail_op = 0xfc4c;
	KT_ASSERT(!knl_btb_run(c, &cfg, hcd_good, sizeof(hcd_good), "test.hcd"));
	KT_ASSERT_EQ(c->st.state, BT_ST_FWFAILED);
	KT_ASSERT_EQ(c->step, -1);
	KT_ASSERT(same(c->st.file, "test.hcd", 9));
	KT_ASSERT_EQ(fk.ops[fk.nops - 1], 0xfc4c);
	for ( i = 0; i < fk.nops; i++ ) KT_ASSERT(fk.ops[i] != 0xfc4e);

	/* a broken patch is not given; the controller goes on as its ROM has it, its address all the same */
	c->step = 0;
	c->st.state = BT_ST_STARTING;
	fk_cfg(&cfg);
	knl_memcpy(cut, hcd_good, sizeof(cut));
	KT_ASSERT(knl_btb_run(c, &cfg, cut, sizeof(cut), "cut.hcd"));
	download = FALSE;
	for ( i = 0; i < fk.nops; i++ ) {
		if ( fk.ops[i] == 0xfc2e || fk.ops[i] == 0xfc4c ) download = TRUE;
	}
	KT_ASSERT(!download);
	KT_ASSERT_EQ(fk.ops[fk.nops - 1], 0xfc01);
	KT_ASSERT_EQ(c->st.state, BT_ST_STARTING);

	/* no patch at all: the same, and an address that is the board's own already is left */
	fk_cfg(&cfg);
	knl_memcpy(fk.addr, cfg.bdaddr, 6);
	KT_ASSERT(knl_btb_run(c, &cfg, NULL, 0, NULL));
	KT_ASSERT_EQ(fk.ops[fk.nops - 1], 0x1009);

	/* the speed command refused: stopped at step 3 */
	fk_cfg(&cfg);
	fk.fail_op = 0xfc18;
	KT_ASSERT(!knl_btb_run(c, &cfg, hcd_good, sizeof(hcd_good), "test.hcd"));
	KT_ASSERT_EQ(c->st.state, BT_ST_NOANSWER);
	KT_ASSERT_EQ(fk.nspeed, 0);

	/* nothing answers: Reset given up, nothing after it */
	c->step = 0;
	fk_cfg(&cfg);
	fk.silent = TRUE;
	KT_ASSERT(!knl_btb_run(c, &cfg, hcd_good, sizeof(hcd_good), "test.hcd"));
	KT_ASSERT_EQ(c->st.state, BT_ST_NOANSWER);
	KT_ASSERT_EQ(fk.nops, 1);
	fk_del(c);
}

EXPORT void ktest_btuart( void )
{
	KT_RUN(test_h4_pack);
	KT_RUN(test_h4_stream);
	KT_RUN(test_hcd_check);
	KT_RUN(test_bcm_start);
	KT_RUN(test_bcm_fail);
}
