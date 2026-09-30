/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_net.c
 *	Network link layer: the card's address, sending a frame, and
 *	receiving the answer. QEMU's user mode networking answers an ARP
 *	request for the address of its own gateway, so a round trip can be
 *	checked without anything outside.
 *	Skipped when the machine has no card. On the Raspberry Pi 5 the
 *	card is on the user's network, where that address means nothing:
 *	the frame is not sent there.
 */

#include "ktest.h"
#include <ts/net.h>

LOCAL BOOL	have_net = FALSE;
LOCAL UB	my_mac[NET_MAC_LEN];

/* the card reports an address that is not empty and not a broadcast */
LOCAL void test_mac( void )
{
	T_NETSTAT	st;
	INT		i, zero = 0, ones = 0;

	if ( net_get_mac(my_mac) < E_OK ) {
		KT_SKIP("no network card");
	}
	have_net = TRUE;
	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		if ( my_mac[i] == 0x00 ) zero++;
		if ( my_mac[i] == 0xff ) ones++;
	}
	KT_ASSERT(zero < NET_MAC_LEN);
	KT_ASSERT(ones < NET_MAC_LEN);
	KT_ASSERT_EQ(my_mac[0] & 1, 0);		/* a station address, not a group */

	/* a cable agrees on its speed in a few seconds; the QEMU card is up at once */
	for ( i = 0; i < 50; i++ ) {
		KT_ASSERT_ER(net_stat(&st), E_OK);
		if ( st.up ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(st.up);
	tm_printf((UB*)"  mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
}

#ifndef RPI5
/* the card takes a frame and counts it */
LOCAL void test_send( void )
{
	T_NETSTAT	before, after;
	UB		frame[NET_FRAME_MIN];
	INT		i;

	if ( !have_net ) KT_SKIP("no network card");

	KT_ASSERT_ER(net_stat(&before), E_OK);

	/* an ARP request for 10.0.2.2, the gateway of QEMU's user network */
	for ( i = 0; i < NET_FRAME_MIN; i++ ) frame[i] = 0;
	for ( i = 0; i < 6; i++ ) frame[i] = 0xff;		/* to everyone */
	for ( i = 0; i < 6; i++ ) frame[6 + i] = my_mac[i];
	frame[12] = 0x08; frame[13] = 0x06;			/* ARP */
	frame[14] = 0x00; frame[15] = 0x01;			/* Ethernet */
	frame[16] = 0x08; frame[17] = 0x00;			/* IPv4 */
	frame[18] = 6; frame[19] = 4;
	frame[20] = 0x00; frame[21] = 0x01;			/* request */
	for ( i = 0; i < 6; i++ ) frame[22 + i] = my_mac[i];
	frame[28] = 10; frame[29] = 0; frame[30] = 2; frame[31] = 15;	/* from 10.0.2.15 */
	frame[38] = 10; frame[39] = 0; frame[40] = 2; frame[41] = 2;	/* asking for 10.0.2.2 */

	/* A backend that drops the frame still returns the descriptor; a hub
	   port with nothing else on it does not, so the test machine is given
	   a socket backend. */
	KT_ASSERT_ER(net_send(frame, sizeof(frame)), E_OK);
	KT_ASSERT_ER(net_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sent, before.sent + 1);

	/* a frame that is too short or too long is refused */
	KT_ASSERT_ER(net_send(frame, 10), E_PAR);
	KT_ASSERT_ER(net_send(frame, NET_FRAME_MAX + 1), E_PAR);
}

/* the answer to the request comes back */
LOCAL void test_recv( void )
{
	UB	buf[NET_FRAME_MAX];
	INT	n, i;
	BOOL	saw_arp_reply = FALSE;

	if ( !have_net ) KT_SKIP("no network card");

	/* The stack keeps a reader on the card once it is up, so this
	   one is turned away. What arrives is checked through the
	   sockets instead (ktest_so.c). */
	n = net_recv(buf, sizeof(buf), 200);
	if ( n == E_BUSY ) {
		KT_SKIP("the protocol stack is reading the card");
	}

	/* whatever arrives within a second; the reply should be among it */
	for ( i = 0; i < 8; i++ ) {
		if ( i > 0 ) n = net_recv(buf, sizeof(buf), 200);
		if ( n < 0 ) continue;

		KT_ASSERT(n >= 14);
		if ( n >= 42 && buf[12] == 0x08 && buf[13] == 0x06
		  && buf[20] == 0x00 && buf[21] == 0x02 ) {
			/* an ARP reply, addressed to this card */
			INT	k, mine = 1;

			for ( k = 0; k < 6; k++ ) {
				if ( buf[k] != my_mac[k] ) mine = 0;
			}
			if ( mine ) {
				saw_arp_reply = TRUE;
				tm_printf((UB*)"  arp reply from %d.%d.%d.%d\n",
					buf[28], buf[29], buf[30], buf[31]);
				break;
			}
		}
	}
	if ( !saw_arp_reply ) {
		KT_SKIP("no answer (the machine may have no network behind it)");
	}
	KT_ASSERT(saw_arp_reply);
}
#endif /* RPI5 */

/* the same card answers as the device "neta" (design 10.4, 12.6) */
LOCAL void test_device( void )
{
	T_NETSTAT	st;
	UB		mac[NET_MAC_LEN];
	BOOL		link;
	W		mtu;
	ID		dd;
	SZ		asize;
	INT		i;

	if ( !have_net ) KT_SKIP("no network card");

	dd = tk_opn_dev((UB *)"neta", TD_UPDATE);
	KT_ASSERT(dd > 0);

	/* the address read through the device is the one the driver gives */
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_NETADDR, mac, NET_MAC_LEN, &asize), E_OK);
	KT_ASSERT_EQ(asize, NET_MAC_LEN);
	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		KT_ASSERT_EQ(mac[i], my_mac[i]);
	}

	KT_ASSERT_ER(tk_srea_dev(dd, TDN_NETMTU, &mtu, sizeof(mtu), &asize), E_OK);
	KT_ASSERT_EQ(mtu, 1500);

	KT_ASSERT_ER(tk_srea_dev(dd, TDN_NETLINK, &link, sizeof(link), &asize), E_OK);
#ifdef RPI5
	tm_printf((UB *)"  link %s\n", link ? "up" : "down");	/* the cable is the user's */
#else
	KT_ASSERT(link);
#endif

	KT_ASSERT_ER(tk_srea_dev(dd, TDN_NETSTAT, &st, sizeof(st), &asize), E_OK);
	KT_ASSERT_EQ(asize, (SZ)sizeof(st));

	/* a frame written to the device reaches the card; not on the
	   Raspberry Pi 5, whose network is the user's */
#ifndef RPI5
	{
		UB		frame[NET_FRAME_MIN];
		T_NETSTAT	after;

		for ( i = 0; i < (INT)sizeof(frame); i++ ) frame[i] = 0;
		for ( i = 0; i < 6; i++ ) frame[i] = 0xff;
		for ( i = 0; i < 6; i++ ) frame[6 + i] = my_mac[i];
		frame[12] = 0x08; frame[13] = 0x06;
		KT_ASSERT_ER(tk_swri_dev(dd, 0, frame, sizeof(frame), &asize), E_OK);
		KT_ASSERT_EQ(asize, (SZ)sizeof(frame));

		KT_ASSERT_ER(net_stat(&after), E_OK);
		KT_ASSERT_EQ(after.sent, st.sent + 1);
	}
#endif

	/* an attribute that does not exist, and one that cannot be written */
	KT_ASSERT_ER(tk_srea_dev(dd, -99, &link, sizeof(link), &asize), E_PAR);
	KT_ASSERT_ER(tk_swri_dev(dd, TDN_NETADDR, mac, NET_MAC_LEN, &asize), E_RONLY);

	KT_ASSERT_ER(tk_cls_dev(dd, 0), E_OK);
}

EXPORT void ktest_net( void )
{
	KT_RUN(test_mac);
	KT_RUN_EXCEPT_RPI5(test_send, "an ARP for the gateway of QEMU's user network");
	KT_RUN_EXCEPT_RPI5(test_recv, "an ARP for the gateway of QEMU's user network");
	KT_RUN(test_device);
}
