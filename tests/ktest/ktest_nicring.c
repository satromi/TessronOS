/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_nicring.c
 *	The descriptor rings and transfer framing of the LAN drivers
 *	(device/net/x86_64/nicring.h), with the test in the part of the chip.
 *	QEMU has no model of these chips, so this says only that the ring
 *	logic follows the descriptors' layout as written down: not that any
 *	chip behaves so.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "ktest.h"
#include "../../device/net/x86_64/nicring.h"

#define N	8
#define BUF	2048
#define PA	0x100000ULL

/* ---------------------------------------------------------------- layouts */

LOCAL void test_layout( void )
{
	RGE_RXD	r;
	RGE_TXD	t;
	RE_DESC	e;
	IGC_RXD	ir;
	IGC_TXD	it;

	KT_ASSERT_EQ(sizeof(RE_DESC), 16);
	KT_ASSERT_EQ(sizeof(RGE_RXD), 32);
	KT_ASSERT_EQ(sizeof(RGE_TXD), 32);
	KT_ASSERT_EQ(sizeof(IGC_RXD), 16);
	KT_ASSERT_EQ(sizeof(IGC_TXD), 16);
	KT_ASSERT_EQ((UB *)&r.addr - (UB *)&r, 16);
	KT_ASSERT_EQ((UB *)&r.cmdsts - (UB *)&r, 28);
	KT_ASSERT_EQ((UB *)&t.addr - (UB *)&t, 8);
	KT_ASSERT_EQ((UB *)&e.addr - (UB *)&e, 8);
	KT_ASSERT_EQ((UB *)&ir.status - (UB *)&ir, 8);
	KT_ASSERT_EQ((UB *)&ir.len - (UB *)&ir, 12);
	KT_ASSERT_EQ((UB *)&it.status - (UB *)&it, 12);
}

/* ---------------------------------------------------------------- the Realtek rings */

LOCAL void test_re_ring( void )
{
	RE_DESC	rx[N], tx[N];
	UINT	i;

	re_ring_init(rx, N, PA, BUF, tx, N);
	for ( i = 0; i < N; i++ ) {
		KT_ASSERT_EQ(rx[i].addr, PA + i * BUF);
		KT_ASSERT(( rx[i].cmdsts & RTL_OWN ) != 0);
		KT_ASSERT_EQ(rx[i].cmdsts & RTL_RX_LENMASK, BUF);
		KT_ASSERT_EQ(( rx[i].cmdsts & RTL_EOR ) != 0, i == N - 1);
		KT_ASSERT_EQ(tx[i].cmdsts & RTL_OWN, 0);
		KT_ASSERT_EQ(( tx[i].cmdsts & RTL_EOR ) != 0, i == N - 1);
	}

	/* nothing yet, while the chip has it */
	KT_ASSERT_EQ(rtl_rx_len(rx[0].cmdsts, RE_RX_FS, RE_RX_LS), NIC_RX_NONE);

	/* a frame of 100 bytes with its check sequence, written back */
	rx[0].cmdsts = RE_RX_FS | RE_RX_LS | 104;
	KT_ASSERT_EQ(rtl_rx_len(rx[0].cmdsts, RE_RX_FS, RE_RX_LS), 100);
	/* an error, a frame in pieces, a runt: dropped */
	KT_ASSERT_EQ(rtl_rx_len(RE_RX_FS | RE_RX_LS | RTL_RX_ERRSUM | 104, RE_RX_FS, RE_RX_LS), NIC_RX_BAD);
	KT_ASSERT_EQ(rtl_rx_len(RE_RX_FS | 2048, RE_RX_FS, RE_RX_LS), NIC_RX_BAD);
	KT_ASSERT_EQ(rtl_rx_len(RE_RX_LS | 100, RE_RX_FS, RE_RX_LS), NIC_RX_BAD);
	KT_ASSERT_EQ(rtl_rx_len(RE_RX_FS | RE_RX_LS | 17, RE_RX_FS, RE_RX_LS), NIC_RX_BAD);

	/* the marks of the RTL8125 are not those of the RTL8111 */
	KT_ASSERT_EQ(rtl_rx_len(RE_RX_FS | RE_RX_LS | 104, RGE_RX_FS, RGE_RX_LS), NIC_RX_BAD);
	KT_ASSERT_EQ(rtl_rx_len(RGE_RX_FS | RGE_RX_LS | 104, RGE_RX_FS, RGE_RX_LS), 100);

	/* given back: the chip's again, the end of the ring kept */
	KT_ASSERT_EQ(rtl_rx_word(N - 1, N, BUF), RTL_OWN | RTL_EOR | BUF);
	KT_ASSERT_EQ(rtl_rx_word(0, N, BUF), RTL_OWN | BUF);

	/* a frame to send */
	KT_ASSERT_EQ(rtl_tx_word(3, N, 60), RTL_OWN | RTL_TX_FS | RTL_TX_LS | 60);
	KT_ASSERT_EQ(rtl_tx_word(N - 1, N, 60), RTL_OWN | RTL_EOR | RTL_TX_FS | RTL_TX_LS | 60);
}

LOCAL void test_rge_ring( void )
{
	RGE_RXD	rx[N];
	RGE_TXD	tx[N];
	UINT	i;

	rge_ring_init(rx, N, PA, BUF, tx, N);
	for ( i = 0; i < N; i++ ) {
		KT_ASSERT_EQ(rx[i].addr, PA + i * BUF);
		KT_ASSERT_EQ(rx[i].cmdsts, rtl_rx_word(i, N, BUF));
		KT_ASSERT_EQ(( tx[i].cmdsts & RTL_EOR ) != 0, i == N - 1);
	}
}

/* ---------------------------------------------------------------- the Intel rings */

LOCAL void test_igc_ring( void )
{
	IGC_RXD	r;
	IGC_TXD	t;

	igc_rx_give(&r, PA);
	KT_ASSERT_EQ(r.addr, PA);
	KT_ASSERT_EQ(r.status, 0);
	KT_ASSERT_EQ(igc_rx_len(&r), NIC_RX_NONE);

	/* the chip writes its own values over the address: giving back restores it */
	r.addr = 0x1234;
	r.status = IGC_RX_DD | IGC_RX_EOP;
	r.len = 100;
	KT_ASSERT_EQ(igc_rx_len(&r), 100);
	r.status = IGC_RX_DD | IGC_RX_EOP | IGC_RX_ERR;
	KT_ASSERT_EQ(igc_rx_len(&r), NIC_RX_BAD);
	r.status = IGC_RX_DD;
	KT_ASSERT_EQ(igc_rx_len(&r), NIC_RX_BAD);
	r.status = IGC_RX_DD | IGC_RX_EOP;
	r.len = 13;
	KT_ASSERT_EQ(igc_rx_len(&r), NIC_RX_BAD);
	igc_rx_give(&r, PA);
	KT_ASSERT_EQ(r.addr, PA);
	KT_ASSERT_EQ(igc_rx_len(&r), NIC_RX_NONE);

	/* a slot never used is free; given to the chip it is not, until it says it is done */
	t.addr = 0;
	t.cmd = 0;
	t.status = 0;
	KT_ASSERT(igc_tx_free(&t));
	igc_tx_put(&t, PA, 1514);
	KT_ASSERT_EQ(t.addr, PA);
	KT_ASSERT_EQ(t.cmd, 0x2b300000U | 1514);	/* DEXT, RS, IFCS, EOP, the data type */
	KT_ASSERT_EQ(t.status, (UW)1514 << IGC_TX_PAYSHIFT);
	KT_ASSERT(( t.status & IGC_TX_DD ) == 0);
	KT_ASSERT(!igc_tx_free(&t));
	t.status |= IGC_TX_DD;
	KT_ASSERT(igc_tx_free(&t));
}

/* ---------------------------------------------------------------- the USB transfers */

LOCAL UB	got[4][2048];
LOCAL INT	got_len[4];
LOCAL INT	got_n;

LOCAL void take( void *ctx, CONST UB *frame, INT len )
{
	(void)ctx;
	if ( got_n < 4 ) {
		knl_memcpy(got[got_n], frame, ( len < 2048 ) ? len : 2048);
		got_len[got_n] = len;
	}
	got_n++;
}

LOCAL void copy( void *d, CONST void *s, INT n )
{
	knl_memcpy(d, s, n);
}

LOCAL UB	xfer[8192];

LOCAL INT ure_put( INT at, INT flen, UB fill )
{
	INT	plen = flen + NIC_CRC_LEN;

	knl_memset(xfer + at, 0, URE_RX_HDR);
	NIC_PUT32(xfer + at, (UW)plen);
	at += URE_RX_HDR;
	knl_memset(xfer + at, fill, plen);
	return at + ( ( plen + 7 ) & ~7 );
}

LOCAL void test_ure_rx( void )
{
	INT	end, bad = 0;

	/* two frames, the second after the 8 byte boundary */
	got_n = 0;
	end = ure_put(0, 61, 0xaa);
	end = ure_put(end, 1514, 0xbb);
	KT_ASSERT_EQ(ure_rx_walk(xfer, end, take, NULL, &bad), 2);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(got_len[0], 61);
	KT_ASSERT_EQ(got[0][60], 0xaa);
	KT_ASSERT_EQ(got_len[1], 1514);
	KT_ASSERT_EQ(got[1][0], 0xbb);
	KT_ASSERT_EQ(got[1][1513], 0xbb);

	/* the length of the second runs past the end: the first is kept and the walk stops */
	got_n = 0;
	bad = 0;
	end = ure_put(0, 100, 0x11);
	NIC_PUT32(xfer + end, 0x7000);
	KT_ASSERT_EQ(ure_rx_walk(xfer, end + URE_RX_HDR + 10, take, NULL, &bad), 1);
	KT_ASSERT_EQ(bad, 1);

	/* a transfer too short for a header holds nothing */
	got_n = 0;
	bad = 0;
	KT_ASSERT_EQ(ure_rx_walk(xfer, 10, take, NULL, &bad), 0);
	KT_ASSERT_EQ(got_n, 0);

	/* a header that claims no frame cannot make the walk go round for ever */
	bad = 0;
	knl_memset(xfer, 0, URE_RX_HDR * 2);
	KT_ASSERT_EQ(ure_rx_walk(xfer, URE_RX_HDR * 2, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);
}

LOCAL void test_ure_tx( void )
{
	UB	f[64], o[128];
	INT	n;

	knl_memset(f, 0x5a, sizeof(f));
	n = ure_tx_wrap(o, f, 60, copy);
	KT_ASSERT_EQ(n, 68);
	KT_ASSERT_EQ(NIC_LE32(o), 60 | URE_TX_FIRSTLAST);
	KT_ASSERT_EQ(NIC_LE32(o + 4), 0);
	KT_ASSERT_EQ(o[8], 0x5a);
	KT_ASSERT_EQ(o[67], 0x5a);
}

/* A transfer of the AX88179: the frames, then the table, then the word that finds the table */
LOCAL INT axen_build( INT count, CONST INT *flen, CONST UW *flags, INT pad )
{
	INT	at = 0, i, tbl;
	UW	h[8];

	for ( i = 0; i < count; i++ ) {
		INT	plen = flen[i] + pad;

		xfer[at] = 0xee;
		xfer[at + 1] = 0xee;
		knl_memset(xfer + at + 2, 0x20 + i, flen[i]);
		h[i] = ( (UW)plen << AXEN_RX_LENSHIFT ) | flags[i];
		at += ( plen + 7 ) & ~7;
	}
	tbl = at;
	for ( i = 0; i < count; i++ ) {
		NIC_PUT32(xfer + at, h[i]);
		at += 4;
	}
	NIC_PUT32(xfer + at, ( (UW)tbl << 16 ) | (UW)count);
	return at + 4;
}

LOCAL void test_axen_rx( void )
{
	INT	l[3] = { 60, 1514, 100 };
	UW	f[3] = { 0, 0, 0 };
	INT	n, bad = 0;

	/* the AX88179 itself: a trailer of 4 in the length */
	got_n = 0;
	n = axen_build(2, l, f, 6);
	KT_ASSERT_EQ(axen_rx_walk(xfer, n, 6, take, NULL, &bad), 2);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(got_len[0], 60);
	KT_ASSERT_EQ(got[0][0], 0x20);
	KT_ASSERT_EQ(got[0][59], 0x20);
	KT_ASSERT_EQ(got_len[1], 1514);
	KT_ASSERT_EQ(got[1][1513], 0x21);

	/* the later revisions: only the two marks */
	got_n = 0;
	n = axen_build(1, l, f, 2);
	KT_ASSERT_EQ(axen_rx_walk(xfer, n, 2, take, NULL, &bad), 1);
	KT_ASSERT_EQ(got_len[0], 60);

	/* a frame with a bad check sequence is skipped, the next is taken at its place */
	got_n = 0;
	bad = 0;
	f[0] = AXEN_RX_BAD;
	n = axen_build(2, l, f, 6);
	KT_ASSERT_EQ(axen_rx_walk(xfer, n, 6, take, NULL, &bad), 1);
	KT_ASSERT_EQ(bad, 1);
	KT_ASSERT_EQ(got[0][0], 0x21);
	f[0] = 0;

	/* the marks missing: the rest cannot be trusted */
	got_n = 0;
	bad = 0;
	n = axen_build(2, l, f, 6);
	xfer[0] = 0;
	KT_ASSERT_EQ(axen_rx_walk(xfer, n, 6, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);

	/* the last word nonsense: nothing read past the transfer */
	bad = 0;
	NIC_PUT32(xfer + 96, 0xffff0002U);
	KT_ASSERT_EQ(axen_rx_walk(xfer, 100, 6, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);
	bad = 0;
	NIC_PUT32(xfer + 96, 0x0000ffffU);
	KT_ASSERT_EQ(axen_rx_walk(xfer, 100, 6, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);
	bad = 0;
	KT_ASSERT_EQ(axen_rx_walk(xfer, 2, 6, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);
}

LOCAL void test_axen_tx( void )
{
	static UB f[2048], o[2048 + 32];
	INT	n;

	knl_memset(f, 0x77, sizeof(f));
	n = axen_tx_wrap(o, f, 60, 512, copy);
	KT_ASSERT_EQ(n, 68);
	KT_ASSERT_EQ(NIC_LE32(o), 60);
	KT_ASSERT_EQ(o[8], 0x77);

	/* a transfer on the boundary is not left to end by itself */
	n = axen_tx_wrap(o, f, 512 - AXEN_TX_HDR, 512, copy);
	KT_ASSERT_EQ(n, 512 + AXEN_TX_HDR);
	KT_ASSERT_EQ(NIC_LE32(o + 512), 0);
	KT_ASSERT_EQ(NIC_LE32(o + 516), AXEN_TX_PADMARK);
}

/* A record of the AX88772's stream: the header, the frame, and a byte to make it even */
LOCAL INT axe_put( INT at, INT flen, UB fill )
{
	NIC_PUT32(xfer + at, (UW)flen | ( ( ~(UW)flen & 0xffffU ) << 16 ));
	knl_memset(xfer + at + AXE_HDR, fill, flen);
	at += AXE_HDR + flen;
	if ( flen & 1 ) xfer[at++] = 0;
	return at;
}

LOCAL void test_axe_rx( void )
{
	static AXE_RX	st;
	INT		end, cut, bad = 0, n, good;

	/* two frames in one transfer, the first of an odd length */
	axe_rx_reset(&st);
	got_n = 0;
	end = axe_put(0, 61, 0xaa);
	end = axe_put(end, 1514, 0xbb);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, end, take, NULL, &bad), 2);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(got_len[0], 61);
	KT_ASSERT_EQ(got[0][60], 0xaa);
	KT_ASSERT_EQ(got_len[1], 1514);
	KT_ASSERT_EQ(got[1][0], 0xbb);
	KT_ASSERT_EQ(got[1][1513], 0xbb);

	/* the same stream cut at every place into two transfers: the same two frames */
	for ( cut = 1; cut < end; cut++ ) {
		axe_rx_reset(&st);
		got_n = 0;
		bad = 0;
		good = axe_rx_feed(&st, xfer, cut, take, NULL, &bad);
		good += axe_rx_feed(&st, xfer + cut, end - cut, take, NULL, &bad);
		if ( good != 2 || bad != 0 || got_len[0] != 61 || got_len[1] != 1514 || got[1][1513] != 0xbb ) break;
	}
	KT_ASSERT_EQ(cut, end);

	/* a header whose complement does not agree: the rest of the transfer is dropped */
	axe_rx_reset(&st);
	got_n = 0;
	bad = 0;
	end = axe_put(0, 60, 0x11);
	n = axe_put(end, 70, 0x22);
	xfer[end + 2] ^= 0x01;
	n = axe_put(n, 80, 0x33);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, n, take, NULL, &bad), 1);
	KT_ASSERT_EQ(bad, 1);
	/* and the next transfer is read from its start */
	got_n = 0;
	end = axe_put(0, 90, 0x44);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, end, take, NULL, &bad), 1);
	KT_ASSERT_EQ(got_len[0], 90);

	/*
	 * a frame left unfinished, then a transfer that is not its rest (one
	 * was lost): the header where its end would be fails, nothing is joined
	 */
	axe_rx_reset(&st);
	got_n = 0;
	bad = 0;
	end = axe_put(0, 1000, 0x55);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, 600, take, NULL, &bad), 0);
	end = axe_put(0, 1000, 0x66);
	end = axe_put(end, 60, 0x77);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, end, take, NULL, &bad), 0);
	KT_ASSERT_EQ(bad, 1);
	KT_ASSERT_EQ(got_n, 0);

	/* a frame shorter than an Ethernet header is counted and stepped over */
	axe_rx_reset(&st);
	got_n = 0;
	bad = 0;
	end = axe_put(0, 7, 0x88);
	end = axe_put(end, 60, 0x99);
	KT_ASSERT_EQ(axe_rx_feed(&st, xfer, end, take, NULL, &bad), 1);
	KT_ASSERT_EQ(bad, 1);
	KT_ASSERT_EQ(got[0][0], 0x99);
}

LOCAL void test_axe_tx( void )
{
	static UB f[2048], o[2048 + 32];
	INT	n;

	knl_memset(f, 0x77, sizeof(f));
	n = axe_tx_wrap(o, f, 60, 512, copy);
	KT_ASSERT_EQ(n, 64);
	KT_ASSERT_EQ(NIC_LE32(o), 60 | ( ( ~60U & 0xffffU ) << 16 ));
	KT_ASSERT(axe_hdr_ok(o));
	KT_ASSERT_EQ(o[4], 0x77);
	KT_ASSERT_EQ(o[63], 0x77);

	/* a transfer on the boundary is not left to end by itself */
	n = axe_tx_wrap(o, f, 512 - AXE_HDR, 512, copy);
	KT_ASSERT_EQ(n, 512 + AXE_HDR);
	KT_ASSERT_EQ(NIC_LE32(o + 512), AXE_TX_PAD);
	n = axe_tx_wrap(o, f, 1024 - AXE_HDR, 64, copy);
	KT_ASSERT_EQ(n, 1024 + AXE_HDR);
}

EXPORT void ktest_nicring( void )
{
	KT_RUN(test_layout);
	KT_RUN(test_re_ring);
	KT_RUN(test_rge_ring);
	KT_RUN(test_igc_ring);
	KT_RUN(test_ure_rx);
	KT_RUN(test_ure_tx);
	KT_RUN(test_axen_rx);
	KT_RUN(test_axen_tx);
	KT_RUN(test_axe_rx);
	KT_RUN(test_axe_tx);
}
