/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_bt.c
 *	Bluetooth (design 6.8): a controller made of the USB controller in
 *	software, answering as a real one does. The start's commands; a scan
 *	and what it finds; a keyboard paired by "just works", its key kept in
 *	an object, its channels opened, and its reports becoming keys. A
 *	device that refuses its service records is read in the boot protocol;
 *	one that gives its report descriptor, in pieces, in the report protocol.
 *
 *	The functions of LE pairing against the specification's sample data;
 *	an LE keyboard and mouse paired both ways and come back by a private
 *	address, its reports read by GATT; earphones set up by AVDTP and fed
 *	SBC; the kinds of Intel controller, told apart and the boot loader
 *	left alone.
 *
 *	LE Audio: a controller that can be the central of CISes, and
 *	earphones of two sink ASEs that are one of a set of two, set up by
 *	BAP and fed LC3 SDUs on two CISes; LC3 itself in a task of TA_FPU;
 *	the functions of CSIS against the specification's sample data; and
 *	the same earphones on a controller without CISes, asked nothing of it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/sysdef.h>
#include <ts/conf.h>
#include <ts/bt.h>
#include <ts/snd.h>
#include "ktest.h"

#if defined(_QEMU_VIRT_) || defined(X64)
#include "../../device/usb/usbdev.h"
#include "../../device/usb/vhc.h"
#include "../../device/bt/bt_crypto.h"
#include "../../device/bt/sbc_enc.h"
#include "../../device/bt/btint.h"
#include "../../device/bt/btaudio.h"
#include "../../device/snd/sndhw.h"
#include "../../lib/liblc3/include/lc3.h"

/* The frames a host made of this same input, and another decoder took back to it */
#define SBC_KAT_HASH	0xFC455807U

#define EP_EV		0x81
#define EP_ACL		0x82
#define EP_OUT		0x02
#define H_LE_C		6

LOCAL BOOL same_bytes( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL CONST UB bt_ddesc[18] = {
	18, 1, 0x00, 0x02, 0xe0, 1, 1, 64, 0x34, 0x12, 0x56, 0x78, 0x00, 0x01, 1, 2, 0, 1
};
#define EP_SCO_IN	0x83
#define EP_SCO_OUT	0x03
LOCAL CONST UB bt_cfg[85] = {
	9, 2, 85, 0, 2, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
	7, 5, EP_EV, 3, 16, 0, 1,
	7, 5, EP_ACL, 2, 64, 0, 0,
	7, 5, EP_OUT, 2, 64, 0, 0,
	/* the voice: nothing in setting 0, one channel of 16-bit samples in setting 2 */
	9, 4, 1, 0, 2, 0xe0, 1, 1, 0,
	7, 5, EP_SCO_IN, 1, 0, 0, 1,
	7, 5, EP_SCO_OUT, 1, 0, 0, 1,
	9, 4, 1, 2, 2, 0xe0, 1, 1, 0,
	7, 5, EP_SCO_IN, 1, 17, 0, 1,
	7, 5, EP_SCO_OUT, 1, 17, 0, 1
};

/* What it was told, for the test's task to answer: a little queue */
#define NQ	32
#define QLEN	1100
LOCAL UB	q[NQ][QLEN];
LOCAL INT	qn[NQ];
LOCAL UB	qk[NQ];		/* 0 a command, 1 ACL */
LOCAL volatile INT qh, qt;

LOCAL void q_put( UB kind, CONST UB *d, INT n )
{
	INT	t = qt;

	if ( n > QLEN ) n = QLEN;
	knl_memcpy(q[t], d, n);
	qn[t] = n;
	qk[t] = kind;
	qt = ( t + 1 ) % NQ;
}

LOCAL INT bt_klass( CONST UB *setup, UB *data, INT len )
{
	(void)setup;
	if ( data != NULL ) q_put(0, data, len);
	return 0;
}

LOCAL void sco_out( CONST UB *d, INT n );

LOCAL void bt_out( UB ep, CONST UB *d, INT n )
{
	if ( ep == EP_OUT ) q_put(1, d, n);
	if ( ep == EP_SCO_OUT ) sco_out(d, n);
}

LOCAL CONST T_VHCDEV bt_dev = {
	bt_ddesc, bt_cfg, sizeof(bt_cfg), { "TessronOS test", "Bluetooth test", NULL, NULL },
	{ NULL, NULL, NULL, NULL }, { 0, 0, 0, 0 },
	bt_out, bt_klass
};

/* An event, in pieces of the endpoint's packet size as a controller on USB sends a long one */
#define EV_PACKET	16
LOCAL void ev( CONST UB *b, INT n )
{
	INT	at = 0, k;

	do {
		k = ( n - at > EV_PACKET ) ? EV_PACKET : n - at;
		KT_ASSERT_ER(knl_vhc_in(1, EP_EV, b + at, k), E_OK);
		at += k;
	} while ( at < n );
}

LOCAL void cmd_done( UH op, CONST UB *ret, INT n )
{
	UB	b[64];

	b[0] = 0x0e;
	b[1] = (UB)( n + 4 );
	b[2] = 1;
	b[3] = (UB)op;
	b[4] = (UB)( op >> 8 );
	b[5] = 0;
	if ( n > 0 ) knl_memcpy(b + 6, ret, n);
	ev(b, n + 6);
}

LOCAL void cmd_status( UH op )
{
	UB	b[6];

	b[0] = 0x0f;
	b[1] = 4;
	b[2] = 0;
	b[3] = 1;
	b[4] = (UB)op;
	b[5] = (UB)( op >> 8 );
	ev(b, 6);
}

/* An L2CAP signalling frame from the device */
LOCAL void acl_sig( UB code, UB ident, CONST UB *d, INT n )
{
	UB	b[64];

	b[0] = 5;		/* handle 5 */
	b[1] = 0x20;
	b[2] = (UB)( n + 8 );
	b[3] = 0;
	b[4] = (UB)( n + 4 );
	b[5] = 0;
	b[6] = 1;
	b[7] = 0;
	b[8] = code;
	b[9] = ident;
	b[10] = (UB)n;
	b[11] = 0;
	knl_memcpy(b + 12, d, n);
	KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, b, n + 12), E_OK);
}

LOCAL CONST UB kb_addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

LOCAL BOOL	mock_silent, mock_reset_fail, fw_boot_mode;
LOCAL BOOL	av_dev_first;		/* the earphones set their stream up themselves */
LOCAL BOOL	avc_dev_first;		/* and open the remote control once the link is encrypted */
LOCAL BOOL	rfc_dev_first;		/* and RFCOMM for hands-free */
LOCAL void	dev_open( UH psm, UB dev );
#define CID_DEV_SDPC	0x75		/* the device asking this end's records */
#define CID_DEV_AVS2	0x76		/* AVDTP of the device's own opening */
#define CID_DEV_AVM2	0x77
IMPORT INT	knl_bti_fw_wait_ms;
LOCAL void fw_frag( CONST UB *b, INT n );
LOCAL BOOL	fw_booted;
LOCAL UW	fw_boot_seen;
LOCAL INT	n_ddc;
LOCAL UW	mock_cod = 0x000540;
LOCAL UB	mock_evmask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 0 };	/* a controller's mask after a reset */
LOCAL INT	mock_inq_mode = 0;	/* standard results until the host asks for another form */
LOCAL INT	n_ext_inq;		/* inquiry results sent in the extended form */
LOCAL UH	mock_acl_len = 64;
LOCAL CONST UB	*intel_answer = NULL;
LOCAL INT	intel_answer_n, n_intel_ver, n_intel_boot;

/* A packet in pieces the size the USB side carries */
LOCAL void acl_pieces( CONST UB *b, INT n )
{
	INT	at = 0;

	while ( at < n ) {
		INT	k = ( n - at > 64 ) ? 64 : n - at;

		KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, b + at, k), E_OK);
		at += k;
	}
}

/* Seen by the test */
LOCAL INT	n_start, n_authreq, n_enc, n_lkneg, n_iocap, n_confirm, n_ctrl_set, n_ctrl_report;
LOCAL INT	n_sdp_ask, n_sdp_disc;
LOCAL BOOL	saw_conn;

/* The device's report descriptor, NULL for one that refuses its service records */
LOCAL CONST UB	*dev_rd = NULL;
LOCAL INT	dev_rdlen = 0;

/* A keyboard of ID 1 in the boot layout, a mouse of ID 2, consumer keys of ID 3 */
LOCAL CONST UB rd_combo[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0,
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xc0, 0xc0,
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
	0x09, 0xe9, 0x81, 0x02, 0x95, 0x07, 0x81, 0x01, 0xc0
};

/* A keyboard alone with no report ID: the modifiers, then a bit for each of the usages 0..0x67 */
LOCAL CONST UB rd_bitmap[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x19, 0x00, 0x29, 0x67, 0x95, 0x68, 0x81, 0x02,
	0xc0
};

LOCAL UB	sdp_list[32 + sizeof(rd_combo)];
LOCAL INT	sdp_listn;

/* The answer of the service records: one record, its attribute 0x0206, one report descriptor */
LOCAL void sdp_make( void )
{
	INT	n = dev_rdlen, l4 = n + 3 + 2, l3 = l4 + 3, l2 = l3 + 3 + 3, l1 = l2 + 3, k = 0;

	sdp_list[k++] = 0x36;
	sdp_list[k++] = (UB)( l1 >> 8 );
	sdp_list[k++] = (UB)l1;
	sdp_list[k++] = 0x36;
	sdp_list[k++] = (UB)( l2 >> 8 );
	sdp_list[k++] = (UB)l2;
	sdp_list[k++] = 0x09;
	sdp_list[k++] = 0x02;
	sdp_list[k++] = 0x06;
	sdp_list[k++] = 0x36;
	sdp_list[k++] = (UB)( l3 >> 8 );
	sdp_list[k++] = (UB)l3;
	sdp_list[k++] = 0x36;
	sdp_list[k++] = (UB)( l4 >> 8 );
	sdp_list[k++] = (UB)l4;
	sdp_list[k++] = 0x08;
	sdp_list[k++] = 0x22;
	sdp_list[k++] = 0x26;
	sdp_list[k++] = (UB)( n >> 8 );
	sdp_list[k++] = (UB)n;
	knl_memcpy(sdp_list + k, dev_rd, n);
	sdp_listn = k + n;
}

#define SDP_PIECE	30
#define CID_DEV_SDP	0x72

/* An answer of the records from the place the question's continuation says, one piece of the list */
LOCAL void sdp_answer( CONST UB *q2, INT n )
{
	UB	b[8 + 8 + SDP_PIECE + 2];
	INT	from = 0, cnt, k = 0, len;

	n_sdp_ask++;
	KT_ASSERT_EQ(q2[0], 0x06);
	if ( n >= 18 && q2[17] == 1 ) from = q2[18];
	cnt = sdp_listn - from;
	if ( cnt > SDP_PIECE ) cnt = SDP_PIECE;
	len = 7 + cnt + ( ( from + cnt < sdp_listn ) ? 2 : 1 );
	b[k++] = 5;
	b[k++] = 0x20;
	b[k++] = (UB)( len + 4 );
	b[k++] = 0;
	b[k++] = (UB)len;
	b[k++] = 0;
	b[k++] = 0x42;		/* to our end */
	b[k++] = 0;
	b[k++] = 0x07;
	b[k++] = q2[1];
	b[k++] = q2[2];
	b[k++] = 0;
	b[k++] = (UB)( len - 5 );
	b[k++] = 0;
	b[k++] = (UB)cnt;
	knl_memcpy(b + k, sdp_list + from, cnt);
	k += cnt;
	if ( from + cnt < sdp_listn ) {
		b[k++] = 1;
		b[k++] = (UB)( from + cnt );
	} else {
		b[k++] = 0;
	}
	acl_pieces(b, k);
}

LOCAL BOOL serve_le( UH op, CONST UB *p );
LOCAL BOOL sco_cmd( UH op, CONST UB *p );

LOCAL void serve_cmd( UH op, CONST UB *p )
{
	static CONST UB ver[8] = { 9, 0, 0, 9, 0x34, 0x12, 0, 0 };
	static CONST UB addr[6] = { 1, 2, 3, 4, 5, 6 };
	static UB	big[2 + 255];		/* an extended inquiry result is the longest event */
	UB	e[64];

	if ( mock_silent ) return;
	if ( fw_boot_mode ) {
		UB	r[24];

		if ( op == 0xfc05 ) {
			static CONST UB	v[10] = { 0, 0x37, 0x0c, 0x12, 0x06, 0x00, 0x1a, 0x26, 0x0f, 0x00 };

			n_intel_ver++;
			knl_memcpy(r, v, 10);
			if ( fw_booted ) r[4] = 0x23;
			e[0] = 0x0e;
			e[1] = 13;
			e[2] = 1;
			e[3] = (UB)op;
			e[4] = (UB)( op >> 8 );
			knl_memcpy(e + 5, r, 10);
			ev(e, 15);
			return;
		}
		if ( op == 0xfc0d ) {
			knl_memset(r, 0, sizeof(r));
			r[4] = 16;
			n_intel_boot++;
			e[0] = 0x0e;
			e[1] = 3 + 23;
			e[2] = 1;
			e[3] = (UB)op;
			e[4] = (UB)( op >> 8 );
			knl_memcpy(e + 5, r, 23);
			ev(e, 5 + 23);
			return;
		}
		if ( op == 0xfc01 ) {
			fw_boot_seen = (UW)p[4] | ( (UW)p[5] << 8 ) | ( (UW)p[6] << 16 ) | ( (UW)p[7] << 24 );
			KT_ASSERT_EQ(p[1], 1);
			KT_ASSERT_EQ(p[3], 1);
			fw_booted = TRUE;
			e[0] = 0xff;
			e[1] = 2;
			e[2] = 0x02;
			e[3] = 0;
			ev(e, 4);
			return;
		}
		if ( op == 0xfc8b ) {
			n_ddc++;
			e[0] = 0x0e;
			e[1] = 4;
			e[2] = 1;
			e[3] = (UB)op;
			e[4] = (UB)( op >> 8 );
			e[5] = 0;
			ev(e, 6);
			return;
		}
		if ( !fw_booted ) {		/* the boot loader knows nothing else */
			e[0] = 0x0e;
			e[1] = 4;
			e[2] = 1;
			e[3] = (UB)op;
			e[4] = (UB)( op >> 8 );
			e[5] = 1;
			ev(e, 6);
			return;
		}
	}
	if ( mock_reset_fail && op == 0x0c03 ) {
		e[0] = 0x0e;
		e[1] = 4;
		e[2] = 1;
		e[3] = (UB)op;
		e[4] = (UB)( op >> 8 );
		e[5] = 0x01;
		ev(e, 6);
		return;
	}
	if ( sco_cmd(op, p) ) return;
	if ( serve_le(op, p) ) return;
	switch ( op ) {
	case 0xfc05: {				/* what an Intel controller is, the status in its answer */
		UB	b[60];

		n_intel_ver++;
		KT_ASSERT_EQ(p[0], 0xff);
		b[0] = 0x0e;
		b[1] = (UB)( 3 + intel_answer_n );
		b[2] = 1;
		b[3] = (UB)op;
		b[4] = (UB)( op >> 8 );
		knl_memcpy(b + 5, intel_answer, intel_answer_n);
		ev(b, 5 + intel_answer_n);
		break;
	}
	case 0xfc0d: {
		static CONST UB	bp[] = { 0x11, 0x22, 0x33, 0, 0x10, 0x00 };

		n_intel_boot++;
		cmd_done(op, bp, sizeof(bp));
		break;
	}
	case 0x1005: {
		UB	buf[7];

		n_start++;
		buf[0] = (UB)mock_acl_len;
		buf[1] = (UB)( mock_acl_len >> 8 );
		buf[2] = 64;
		buf[3] = 8;
		buf[4] = 0;
		buf[5] = 8;
		buf[6] = 0;
		cmd_done(op, buf, 7);
		break;
	}
	case 0x0c01:				/* the events let through, kept as a controller keeps them */
		knl_memcpy(mock_evmask, p, 8);
		n_start++;
		cmd_done(op, NULL, 0);
		break;
	case 0x0c45:				/* the form inquiry results come in */
		mock_inq_mode = p[0];
		n_start++;
		cmd_done(op, NULL, 0);
		break;
	case 0x0c03: {				/* a reset: the mask and the inquiry mode back to their defaults */
		static CONST UB	mask0[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 0 };

		knl_memcpy(mock_evmask, mask0, 8);
		mock_inq_mode = 0;
		n_start++;
		cmd_done(op, NULL, 0);
		break;
	}
	case 0x0c56: case 0x0c24: case 0x0c1a:
		n_start++;
		cmd_done(op, NULL, 0);
		break;
	case 0x1001:
		n_start++;
		cmd_done(op, ver, 8);
		break;
	case 0x1003: {
		static CONST UB feat[8] = { 0xff, 0xff, 0x8f, 0xfe, 0xdb, 0xff, 0x5b, 0x87 };

		n_start++;
		cmd_done(op, feat, 8);
		break;
	}
	case 0x1009:
		n_start++;
		cmd_done(op, addr, 6);
		break;
	case 0x0401:				/* inquiry: one found, in the form the inquiry mode asks for */
		cmd_status(op);
		if ( mock_inq_mode == 2 ) {
			/*
			 * The extended form, the one of a device that tells its name.
			 * A controller drops the event when the mask does not let it
			 * through (bit 46), and nothing of the device comes.
			 */
			if ( ( mock_evmask[5] & 0x40 ) != 0 ) {
				knl_memset(big, 0, sizeof(big));
				big[0] = 0x2f;
				big[1] = 255;
				big[2] = 1;
				knl_memcpy(big + 3, kb_addr, 6);
				big[9] = 1;
				big[11] = (UB)mock_cod;
				big[12] = (UB)( mock_cod >> 8 );
				big[13] = (UB)( mock_cod >> 16 );
				big[17] = 5;		/* its name in what it tells: "Mock" */
				big[18] = 0x09;
				knl_memcpy(big + 19, "Mock", 4);
				ev(big, sizeof(big));
				n_ext_inq++;
			}
		} else if ( ( mock_evmask[4] & 0x02 ) != 0 ) {
			e[0] = 0x22;
			e[1] = 15;
			e[2] = 1;
			knl_memcpy(e + 3, kb_addr, 6);
			e[9] = 1;
			e[10] = 0;
			e[11] = (UB)mock_cod;	/* a keyboard, or what the test says */
			e[12] = (UB)( mock_cod >> 8 );
			e[13] = (UB)( mock_cod >> 16 );
			e[14] = 0;
			e[15] = 0;
			e[16] = 0;
			ev(e, 17);
		}
		e[0] = 0x01;
		e[1] = 1;
		e[2] = 0;
		ev(e, 3);
		break;
	case 0x0405:				/* connection */
		saw_conn = TRUE;
		cmd_status(op);
		e[0] = 0x03;
		e[1] = 11;
		e[2] = 0;
		e[3] = 5;
		e[4] = 0;
		knl_memcpy(e + 5, kb_addr, 6);
		e[11] = 1;
		e[12] = 0;
		ev(e, 13);
		break;
	case 0x0419:				/* its name */
		cmd_status(op);
		break;
	case 0x0411:				/* authentication: it has no key yet, so it asks */
		n_authreq++;
		cmd_status(op);
		e[0] = 0x17;
		e[1] = 6;
		knl_memcpy(e + 2, kb_addr, 6);
		ev(e, 8);
		break;
	case 0x040c:				/* no key: simple pairing */
		n_lkneg++;
		cmd_done(op, kb_addr, 6);
		e[0] = 0x31;
		e[1] = 6;
		knl_memcpy(e + 2, kb_addr, 6);
		ev(e, 8);
		break;
	case 0x042b:
		n_iocap++;
		KT_ASSERT_EQ(p[6], 3);
		cmd_done(op, kb_addr, 6);
		e[0] = 0x33;
		e[1] = 10;
		knl_memcpy(e + 2, kb_addr, 6);
		knl_memset(e + 8, 0, 4);
		ev(e, 12);
		break;
	case 0x042c:				/* confirmed: the key, then authenticated */
		n_confirm++;
		cmd_done(op, kb_addr, 6);
		e[0] = 0x18;
		e[1] = 23;
		knl_memcpy(e + 2, kb_addr, 6);
		knl_memset(e + 8, 0xab, 16);
		e[24] = 4;
		ev(e, 25);
		e[0] = 0x06;
		e[1] = 3;
		e[2] = 0;
		e[3] = 5;
		e[4] = 0;
		ev(e, 5);
		break;
	case 0x0413:
		n_enc++;
		cmd_status(op);
		e[0] = 0x08;
		e[1] = 4;
		e[2] = 0;
		e[3] = 5;
		e[4] = 0;
		e[5] = 1;
		ev(e, 6);
		if ( av_dev_first ) dev_open(0x0019, CID_DEV_AVS2);
		if ( avc_dev_first ) dev_open(0x0017, 0x78);
		if ( rfc_dev_first ) dev_open(0x0003, 0x79);
		break;
	default:
		tm_printf((UB *)"  the test's controller: command %04x left unanswered\n", op);
		break;
	}
}

LOCAL void le_serve( CONST UB *b, INT n );
LOCAL void av_serve( CONST UB *f, INT n );
LOCAL void av_media_in( CONST UB *f, INT n );

/* Channels the device opens itself: its end, this end's from the answer, and their configuration */
typedef struct {
	UB	dev;
	UB	ours;
	UB	res;		/* the answer's result, 0xff before it */
	UB	up;		/* 1 its configuration answered, 2 this end's asked and answered */
} MCH;
#define NMCH		8
LOCAL MCH	mch[NMCH];
LOCAL UH	mock_media_mtu;		/* the MTU the device takes on the media channel, 0 left unsaid */
LOCAL UB	mock_ident = 200;


LOCAL MCH *mch_of( UB dev )
{
	INT	i;

	for ( i = 0; i < NMCH; i++ ) {
		if ( dev != 0 && mch[i].dev == dev ) return &mch[i];
	}
	return NULL;
}

/* This end's channel for the device's end */
LOCAL UB ours_of( UB dcid )
{
	MCH	*c = mch_of(dcid);

	if ( c != NULL ) return c->ours;
	if ( dcid >= 0x7a && dcid <= 0x7c ) return (UB)( 0x46 + ( dcid - 0x7a ) );
	return (UB)( dcid == CID_DEV_SDP ? 0x42 : dcid == 0x70 ? 0x40 : dcid == 0x73 ? 0x43 : dcid == 0x74 ? 0x44 : 0x41 );
}

/* The configuration the device asks of this end, with the MTU on the media channel the test says */
LOCAL void dev_config( UB ours, UB ident, BOOL media )
{
	UB	r[8];
	INT	n = 4;

	r[0] = ours;
	r[1] = 0;
	r[2] = r[3] = 0;
	if ( media && mock_media_mtu != 0 ) {
		r[n++] = 1;
		r[n++] = 2;
		r[n++] = (UB)mock_media_mtu;
		r[n++] = (UB)( mock_media_mtu >> 8 );
	}
	acl_sig(0x04, ident, r, n);
}

/* A channel the device asks for */
LOCAL void dev_open( UH psm, UB dev )
{
	UB	r[4];
	INT	i;

	for ( i = 0; i < NMCH && mch[i].dev != 0 && mch[i].dev != dev; i++ ) ;
	if ( i == NMCH ) return;
	mch[i].dev = dev;
	mch[i].ours = 0;
	mch[i].res = 0xff;
	mch[i].up = 0;
	r[0] = (UB)psm;
	r[1] = (UB)( psm >> 8 );
	r[2] = dev;
	r[3] = 0;
	acl_sig(0x02, mock_ident++, r, 4);
}

/* An L2CAP frame from the device to this end's channel, in packets of the size the USB side carries */
LOCAL void cl_send( UB ours, CONST UB *d, INT n )
{
	UB	b[8 + 700];

	if ( n > 700 ) return;
	b[0] = 5;
	b[1] = 0x20;
	b[2] = (UB)( n + 4 );
	b[3] = (UB)( ( n + 4 ) >> 8 );
	b[4] = (UB)n;
	b[5] = (UB)( n >> 8 );
	b[6] = ours;
	b[7] = 0;
	knl_memcpy(b + 8, d, n);
	acl_pieces(b, n + 8);
}

LOCAL void chan_ready( UB dev );
LOCAL void dev_frame( UB dev, CONST UB *f, INT n );

/* The channels from the device's side: asked, answered and configured */
LOCAL void serve_frame( CONST UB *b, INT n )
{
	INT	cid;

	if ( n < 8 ) return;
	if ( ( b[0] | ( ( b[1] & 0x0f ) << 8 ) ) == H_LE_C ) {
		le_serve(b, n);
		return;
	}
	cid = b[6] | ( b[7] << 8 );
	if ( cid == 1 && n >= 12 ) {
		UB	code = b[8], id = b[9];
		UB	r[8];
		MCH	*c;

		if ( code == 0x02 ) {		/* connect: the device's end by this end's */
			UB	scid = b[14];

			if ( b[12] == 0x01 && scid == 0x42 && dev_rd == NULL ) {	/* the service records: refused */
				r[0] = r[1] = 0;
				r[2] = scid;
				r[3] = b[15];
				r[4] = 2;
				r[5] = r[6] = r[7] = 0;
				acl_sig(0x03, id, r, 8);
				return;
			}
			r[0] = ( scid >= 0x46 && scid <= 0x48 ) ? (UB)( 0x7a + ( scid - 0x46 ) )
			     : ( b[12] == 0x01 ) ? CID_DEV_SDP : ( scid == 0x43 ) ? 0x73 : ( scid == 0x44 ) ? 0x74
			     : (UB)( 0x70 + ( scid & 1 ) );
			r[1] = 0;
			r[2] = scid;
			r[3] = b[15];
			r[4] = r[5] = r[6] = r[7] = 0;
			acl_sig(0x03, id, r, 8);
			/* and asks its own config of us */
			dev_config(scid, (UB)( id + 100 ), (BOOL)( scid == 0x44 ));
		} else if ( code == 0x03 && ( c = mch_of(b[14]) ) != NULL ) {	/* this end's answer to the device's asking */
			c->res = b[16];
			if ( b[16] == 0 ) {
				c->ours = b[12];
				dev_config(c->ours, mock_ident++, (BOOL)( c->dev == CID_DEV_AVM2 ));
			}
		} else if ( code == 0x04 ) {	/* our config: all right */
			UB	dcid = b[12];

			r[0] = ours_of(dcid);
			r[1] = 0;
			r[2] = r[3] = r[4] = r[5] = 0;
			acl_sig(0x05, id, r, 6);
			if ( ( c = mch_of(dcid) ) != NULL ) {
				c->up |= 2;
				if ( c->up == 3 ) chan_ready(c->dev);
			}
		} else if ( code == 0x05 && ( c = mch_of(b[12]) ) != NULL ) {
			c->up |= 1;
			if ( c->up == 3 ) chan_ready(c->dev);
		} else if ( code == 0x06 && b[12] == CID_DEV_SDP ) {
			n_sdp_disc++;
		}
	} else if ( cid == CID_DEV_SDP ) {
		sdp_answer(b + 8, n - 8);
	} else if ( cid == 0x73 ) {
		av_serve(b + 8, n - 8);
	} else if ( cid == 0x74 || cid == CID_DEV_AVM2 ) {
		av_media_in(b + 8, n - 8);
	} else if ( ( cid == 0x70 ) && b[8] == 0x70 ) {
		n_ctrl_set++;			/* SET_PROTOCOL boot */
	} else if ( ( cid == 0x70 ) && b[8] == 0x71 ) {
		n_ctrl_report++;		/* SET_PROTOCOL report */
	} else if ( cid >= CID_DEV_SDPC ) {
		dev_frame((UB)cid, b + 8, n - 8);
	}
}

/* LE Audio's side of the controller and of the earphones (below) */
LOCAL BOOL	lea_chip, lea_dev;
LOCAL BOOL lea_cmd( UH op, CONST UB *p );
LOCAL BOOL lea_iso_in( CONST UB *b, INT n );
LOCAL void la_att( CONST UB *f, INT n );
IMPORT void knl_btbap_sef( CONST UB *k, CONST UB *sirk, UB *out );

/* A packet out: the room back, and a frame given on when its last piece has come */
LOCAL UB	acl_buf[1200];
LOCAL INT	acl_have;

LOCAL void serve_acl( CONST UB *b, INT n )
{
	UB	e[7];
	INT	pb = ( b[1] >> 4 ) & 3;

	if ( n < 4 ) return;
	if ( fw_boot_mode && !fw_booted && b[0] == 0x09 && b[1] == 0xfc ) {
		fw_frag(b, n);
		return;
	}
	if ( lea_iso_in(b, n) ) return;
	e[0] = 0x13;
	e[1] = 5;
	e[2] = 1;
	e[3] = b[0];
	e[4] = (UB)( b[1] & 0x0f );
	e[5] = 1;
	e[6] = 0;
	ev(e, 7);
	if ( pb != 1 ) {
		knl_memcpy(acl_buf, b, n);
		acl_have = n;
	} else if ( acl_have + n - 4 <= (INT)sizeof(acl_buf) ) {
		knl_memcpy(acl_buf + acl_have, b + 4, n - 4);
		acl_have += n - 4;
	}
	if ( acl_have >= 8 && acl_have >= ( acl_buf[4] | ( acl_buf[5] << 8 ) ) + 8 ) {
		serve_frame(acl_buf, acl_have);
		acl_have = 0;
	}
}

LOCAL void pump( TMO ms )
{
	INT	k;

	for ( k = 0; k < ms / 5; k++ ) {
		while ( qh != qt ) {
			INT	h = qh;

			if ( qk[h] == 0 ) serve_cmd((UH)( q[h][0] | ( q[h][1] << 8 ) ), q[h] + 3);
			else serve_acl(q[h], qn[h]);
			qh = ( h + 1 ) % NQ;
		}
		tk_dly_tsk(5);
	}
}

LOCAL void bt_end( CONST T_HIDSTAT *s0 );

LOCAL void test_bt_pair( void )
{
	T_BTFOUND	f[4];
	T_HIDATTR	a, keep;
	T_HIDEV		e;
	T_HIDSTAT	s0, s1;
	UB		r[12];
	INT		i, n, ext0;

	qh = qt = 0;
	dev_rd = NULL;
	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	(void)ts_hid_setattr(&a);
	KT_ASSERT_ER(ts_hid_stat(&s0), E_OK);
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
	KT_ASSERT_EQ(n_start, 13);
	if ( !knl_bt_ready() ) {
		(void)knl_vhc_unplug(1);
		return;
	}

	ext0 = n_ext_inq;
	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	for ( n = 0, i = 0; i < 40 && n == 0; i++ ) {
		pump(50);
		n = knl_bt_found(f, 4);
	}
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT(same_bytes(f[0].addr, kb_addr, 6));
	KT_ASSERT_EQ(f[0].cod, 0x000540);
	/* results asked in the extended form, and let through: the device came with its name */
	KT_ASSERT_EQ(mock_inq_mode, 2);
	KT_ASSERT_EQ(n_ext_inq, ext0 + 1);
	KT_ASSERT(same_bytes(f[0].name, "Mock", 5));

	KT_ASSERT_ER(knl_bt_pair(kb_addr), E_OK);
	pump(1500);
	KT_ASSERT(saw_conn);
	KT_ASSERT_EQ(n_authreq, 1);
	KT_ASSERT_EQ(n_lkneg, 1);
	KT_ASSERT_EQ(n_iocap, 1);
	KT_ASSERT_EQ(n_confirm, 1);
	KT_ASSERT_EQ(n_enc, 1);
	KT_ASSERT_EQ(n_ctrl_set, 1);
	KT_ASSERT_ER(ts_hid_stat(&s1), E_OK);
	KT_ASSERT_EQ(s1.keyboards, s0.keyboards + 1);

	/* a key through the interrupt channel: 0xA1, report 1, the boot report */
	while ( ts_hid_read(&e, TMO_POL) >= E_OK ) ;
	knl_memset(r, 0, sizeof(r));
	r[0] = 5;
	r[1] = 0x20;
	r[2] = 14;
	r[4] = 10;
	r[6] = 0x41;
	KT_ASSERT(sizeof(r) >= 8);
	{
		UB	m[18];

		knl_memset(m, 0, sizeof(m));
		knl_memcpy(m, r, 8);
		m[2] = 14;
		m[4] = 10;
		m[8] = 0xa1;
		m[9] = 1;
		m[12] = 0x04;		/* A: the third byte of keys, after modifiers and reserved */
		KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, m, 18), E_OK);
		pump(50);
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_KEY_DOWN);
		KT_ASSERT_EQ(e.code, 0x04);
	}

	bt_end(&s0);
	(void)ts_hid_setattr(&keep);
}

/* A report on the interrupt channel */
LOCAL void hid_in( CONST UB *r, INT n )
{
	UB	m[8 + 1 + 16];

	m[0] = 5;
	m[1] = 0x20;
	m[2] = (UB)( n + 1 + 4 );
	m[3] = 0;
	m[4] = (UB)( n + 1 );
	m[5] = 0;
	m[6] = 0x41;
	m[7] = 0;
	m[8] = 0xa1;
	knl_memcpy(m + 9, r, n);
	KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, m, n + 9), E_OK);
	pump(50);
}

/* The device paired with the given descriptor: answers whether all went as it should */
LOCAL BOOL bt_up( CONST UB *rd, INT rdlen )
{
	INT	i;

	qh = qt = 0;
	dev_rd = rd;
	dev_rdlen = rdlen;
	sdp_make();
	n_start = n_authreq = n_enc = n_lkneg = n_iocap = n_confirm = n_ctrl_set = n_ctrl_report = 0;
	n_sdp_ask = n_sdp_disc = 0;
	saw_conn = FALSE;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
	if ( !knl_bt_ready() ) {
		(void)knl_vhc_unplug(1);
		return FALSE;
	}
	KT_ASSERT_ER(knl_bt_pair(kb_addr), E_OK);
	pump(1500);
	/* more than one question: the answer came in pieces */
	KT_ASSERT(n_sdp_ask > 1);
	KT_ASSERT_EQ(n_sdp_disc, 1);
	KT_ASSERT_EQ(n_ctrl_set, 0);
	KT_ASSERT_EQ(n_ctrl_report, 1);
	return TRUE;
}

/* The report protocol: a keyboard, a mouse and consumer keys in one device, told by their IDs */
LOCAL void test_bt_report( void )
{
	static CONST UB	key_a[] = { 1, 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
	static CONST UB	key_off[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
	static CONST UB	mouse[] = { 2, 0, 5, 3 };
	T_HIDATTR	a, keep;
	T_HIDEV		e;
	T_HIDSTAT	s0, s1;

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	(void)ts_hid_setattr(&a);
	KT_ASSERT_ER(ts_hid_stat(&s0), E_OK);
	if ( bt_up(rd_combo, sizeof(rd_combo)) ) {
		KT_ASSERT_ER(ts_hid_stat(&s1), E_OK);
		KT_ASSERT_EQ(s1.keyboards, s0.keyboards + 1);
		KT_ASSERT_EQ(s1.pointers, s0.pointers + 1);
		while ( ts_hid_read(&e, TMO_POL) >= E_OK ) ;
		hid_in(key_a, sizeof(key_a));
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		if ( e.type == HID_EV_KEY_DOWN && e.code == 0xe1 ) KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_KEY_DOWN);
		KT_ASSERT_EQ(e.code, 0x04);
		KT_ASSERT((e.mods & 0x02) != 0);
		hid_in(key_off, sizeof(key_off));
		while ( ts_hid_read(&e, 100) >= E_OK ) ;
		hid_in(mouse, sizeof(mouse));
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_MOVE);
		bt_end(&s0);
	}
	(void)ts_hid_setattr(&keep);
}

/* The report protocol: a keyboard with no ID, a bit for each key */
LOCAL void test_bt_report_bits( void )
{
	UB		r[14];
	T_HIDATTR	a, keep;
	T_HIDEV		e;
	T_HIDSTAT	s0, s1;

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	(void)ts_hid_setattr(&a);
	KT_ASSERT_ER(ts_hid_stat(&s0), E_OK);
	if ( bt_up(rd_bitmap, sizeof(rd_bitmap)) ) {
		KT_ASSERT_ER(ts_hid_stat(&s1), E_OK);
		KT_ASSERT_EQ(s1.keyboards, s0.keyboards + 1);
		KT_ASSERT_EQ(s1.pointers, s0.pointers);
		while ( ts_hid_read(&e, TMO_POL) >= E_OK ) ;
		knl_memset(r, 0, sizeof(r));
		r[1 + ( 0x05 >> 3 )] = (UB)( 1 << ( 0x05 & 7 ) );	/* B */
		hid_in(r, sizeof(r));
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_KEY_DOWN);
		KT_ASSERT_EQ(e.code, 0x05);
		knl_memset(r, 0, sizeof(r));
		hid_in(r, sizeof(r));
		bt_end(&s0);
	}
	(void)ts_hid_setattr(&keep);
}

/* The device's object: kept, and taken away for those that come after; the controller gone */
LOCAL void bt_end( CONST T_HIDSTAT *s0 )
{
	T_HIDEV		e;
	T_HIDSTAT	s1;
	INT		i, n;
	TS_UUID		*ids = NULL;
	char		idb[TS_UUID_STRLEN + 1];
	CONST char	*conf = knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb);

	/* kept in 機器の設定, which lasts, not in dev, whose links follow the devices there are */
	n = knl_conf_links_get(conf, CF_ROLE_BTDEV, NULL, 0);
	tm_printf((UB *)"  機器の設定 has %d devices kept\n", n);
	if ( n > 0 ) ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	KT_ASSERT(ids != NULL);
	if ( ids != NULL ) n = knl_conf_links_get(conf, CF_ROLE_BTDEV, ids, n);
	{
		BOOL	found = FALSE;

		for ( i = 0; ids != NULL && i < n; i++ ) {
			UB	meta[2048];
			SZ	asz = 0;
			ID	key = ob_opn_obj(&ids[i], OB_OP_ATRRD);

			if ( key <= 0 ) continue;
			if ( ob_get_atr(key, meta, sizeof(meta) - 1, &asz) >= E_OK ) {
				SZ	k;

				for ( k = 0; k + 7 <= asz; k++ ) {
					if ( same_bytes(meta + k, "\"btdev\"", 7) ) {
						found = TRUE;
						break;
					}
				}
			}
			(void)ob_cls_obj(key);
			if ( found ) {
				KT_ASSERT_ER(knl_conf_link_drop(conf, CF_ROLE_BTDEV, &ids[i]), E_OK);
				KT_ASSERT_EQ(knl_conf_link_drop(conf, CF_ROLE_BTDEV, &ids[i]), E_NOEXS);
				(void)ob_del_obj(&ids[i]);
				break;
			}
		}
		KT_ASSERT(found);
		if ( ids != NULL ) Kfree(ids);
	}
	KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
	for ( i = 0; i < 60; i++ ) {
		(void)ts_hid_read(&e, 20);
		(void)ts_hid_stat(&s1);
		if ( s1.keyboards == s0->keyboards && s1.pointers == s0->pointers ) break;
	}
	KT_ASSERT_EQ(s1.keyboards, s0->keyboards);
	KT_ASSERT_EQ(s1.pointers, s0->pointers);
}

/* ---------------------------------------------------------------- the sample data of pairing */

/* The specification's numbers, written the most significant byte first as it writes them */
LOCAL void turn( UB *to, CONST UB *from, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) to[i] = from[n - 1 - i];
}

LOCAL void test_bt_crypto( void )
{
	static CONST UB	cmac_key[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
	static CONST UB	cmac_empty[16] = { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28, 0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 };
	static CONST UB	cmac_msg[16] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
	static CONST UB	cmac_16[16] = { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c };
	/* ah */
	static CONST UB	irk[16] = { 0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec };
	static CONST UB	ah_r[3] = { 0x94, 0x81, 0x70 };
	static CONST UB	ah_x[3] = { 0xaa, 0xfb, 0x0d };
	/* c1 and s1 */
	static CONST UB	zero[16] = { 0 };
	static CONST UB	c1_r[16] = { 0xe0, 0x2e, 0x70, 0xc6, 0x4e, 0x27, 0x88, 0x63, 0x0e, 0x6f, 0xad, 0x56, 0x21, 0xd5, 0x83, 0x57 };
	static CONST UB	preq[7] = { 0x01, 0x01, 0x00, 0x00, 0x10, 0x07, 0x07 };
	static CONST UB	pres[7] = { 0x02, 0x03, 0x00, 0x00, 0x08, 0x00, 0x05 };
	static CONST UB	ra[6] = { 0xb6, 0xb5, 0xb4, 0xb3, 0xb2, 0xb1 };
	static CONST UB	ia[6] = { 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1 };
	static CONST UB	c1_x[16] = { 0x86, 0x3b, 0xf1, 0xbe, 0xc5, 0x4d, 0xa7, 0xd2, 0xea, 0x88, 0x89, 0x87, 0xef, 0x3f, 0x1e, 0x1e };
	static CONST UB	s1_r1[16] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
	static CONST UB	s1_r2[16] = { 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99 };
	static CONST UB	s1_x[16] = { 0x62, 0xa0, 0x6d, 0x79, 0xae, 0x16, 0x42, 0x5b, 0x9b, 0xf4, 0xb0, 0xe8, 0xf0, 0xe1, 0x1f, 0x9a };
	/* f4, f5, f6, g2 */
	static CONST UB	u[32] = {
		0xe6, 0x9d, 0x35, 0x0e, 0x48, 0x01, 0x03, 0xcc, 0xdb, 0xfd, 0xf4, 0xac, 0x11, 0x91, 0xf4, 0xef,
		0xb9, 0xa5, 0xf9, 0xe9, 0xa7, 0x83, 0x2c, 0x5e, 0x2c, 0xbe, 0x97, 0xf2, 0xd2, 0x03, 0xb0, 0x20 };
	static CONST UB	v[32] = {
		0xfd, 0xc5, 0x7f, 0xf4, 0x49, 0xdd, 0x4f, 0x6b, 0xfb, 0x7c, 0x9d, 0xf1, 0xc2, 0x9a, 0xcb, 0x59,
		0x2a, 0xe7, 0xd4, 0xee, 0xfb, 0xfc, 0x0a, 0x90, 0x9a, 0xbb, 0xf6, 0x32, 0x3d, 0x8b, 0x18, 0x55 };
	static CONST UB	x[16] = { 0xab, 0xae, 0x2b, 0x71, 0xec, 0xb2, 0xff, 0xff, 0x3e, 0x73, 0x77, 0xd1, 0x54, 0x84, 0xcb, 0xd5 };
	static CONST UB	y[16] = { 0xcf, 0xc4, 0x3d, 0xff, 0xf7, 0x83, 0x65, 0x21, 0x6e, 0x5f, 0xa7, 0x25, 0xcc, 0xe7, 0xe8, 0xa6 };
	static CONST UB	f4_x[16] = { 0x2d, 0x87, 0x74, 0xa9, 0xbe, 0xa1, 0xed, 0xf1, 0x1c, 0xbd, 0xa9, 0x07, 0xf1, 0x16, 0xc9, 0xf2 };
	static CONST UB	w[32] = {
		0x98, 0xa6, 0xbf, 0x73, 0xf3, 0x34, 0x8d, 0x86, 0xf1, 0x66, 0xf8, 0xb4, 0x13, 0x6b, 0x79, 0x99,
		0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec };
	static CONST UB	a1[7] = { 0xce, 0xbf, 0x37, 0x37, 0x12, 0x56, 0x00 };
	static CONST UB	a2[7] = { 0xc1, 0xcf, 0x2d, 0x70, 0x13, 0xa7, 0x00 };
	static CONST UB	f5_ltk[16] = { 0x38, 0x0a, 0x75, 0x94, 0xb5, 0x22, 0x05, 0x98, 0x23, 0xcd, 0xd7, 0x69, 0x11, 0x79, 0x86, 0x69 };
	static CONST UB	f5_mk[16] = { 0x20, 0x6e, 0x63, 0xce, 0x20, 0x6a, 0x3f, 0xfd, 0x02, 0x4a, 0x08, 0xa1, 0x76, 0xf1, 0x65, 0x29 };
	static CONST UB	f6_r[16] = { 0xc8, 0x0f, 0x2d, 0x0c, 0xd2, 0x42, 0xda, 0x08, 0x54, 0xbb, 0x53, 0xb4, 0x3b, 0x34, 0xa3, 0x12 };
	static CONST UB	f6_io[3] = { 0x02, 0x01, 0x01 };
	static CONST UB	f6_x[16] = { 0x61, 0x8f, 0x95, 0xda, 0x09, 0x0b, 0x6c, 0xd2, 0xc5, 0xe8, 0xd0, 0x9c, 0x98, 0x73, 0xc4, 0xe3 };
	/* the curve: the debug keys, and the secret between two keys */
	static CONST UB	priv_a[32] = {
		0x3f, 0x49, 0xf6, 0xd4, 0xa3, 0xc5, 0x5f, 0x38, 0x74, 0xc9, 0xb3, 0xe3, 0xd2, 0x10, 0x3f, 0x50,
		0x4a, 0xff, 0x60, 0x7b, 0xeb, 0x40, 0xb7, 0x99, 0x58, 0x99, 0xb8, 0xa6, 0xcd, 0x3c, 0x1a, 0xbd };
	static CONST UB	pub_ax[32] = {
		0x20, 0xb0, 0x03, 0xd2, 0xf2, 0x97, 0xbe, 0x2c, 0x5e, 0x2c, 0x83, 0xa7, 0xe9, 0xf9, 0xa5, 0xb9,
		0xef, 0xf4, 0x91, 0x11, 0xac, 0xf4, 0xfd, 0xdb, 0xcc, 0x03, 0x01, 0x48, 0x0e, 0x35, 0x9d, 0xe6 };
	static CONST UB	pub_ay[32] = {
		0xdc, 0x80, 0x9c, 0x49, 0x65, 0x2a, 0xeb, 0x6d, 0x63, 0x32, 0x9a, 0xbf, 0x5a, 0x52, 0x15, 0x5c,
		0x76, 0x63, 0x45, 0xc2, 0x8f, 0xed, 0x30, 0x24, 0x74, 0x1c, 0x8e, 0xd0, 0x15, 0x89, 0xd2, 0x8b };
	static CONST UB	priv_b[32] = {
		0x55, 0x18, 0x8b, 0x3d, 0x32, 0xf6, 0xbb, 0x9a, 0x90, 0x0a, 0xfc, 0xfb, 0xee, 0xd4, 0xe7, 0x2a,
		0x59, 0xcb, 0x9a, 0xc2, 0xf1, 0x9d, 0x7c, 0xfb, 0x6b, 0x4f, 0xdd, 0x49, 0xf4, 0x7f, 0xc5, 0xfd };
	static CONST UB	dh_ab[32] = {
		0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05, 0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b,
		0x99, 0x79, 0x6b, 0x13, 0xb4, 0xf8, 0x66, 0xf1, 0x86, 0x8d, 0x34, 0xf3, 0x73, 0xbf, 0xa6, 0x98 };
	UB	o[16], o2[16], pa[32], pb[32], pub[64], pub2[64], dh[32], t[32];

	knl_bts_cmac(cmac_key, NULL, 0, o);
	KT_ASSERT(same_bytes(o, cmac_empty, 16));
	knl_bts_cmac(cmac_key, cmac_msg, 16, o);
	KT_ASSERT(same_bytes(o, cmac_16, 16));
	knl_bts_ah(irk, ah_r, o);
	KT_ASSERT(same_bytes(o, ah_x, 3));
	knl_bts_c1(zero, c1_r, preq, pres, 1, ia, 0, ra, o);
	KT_ASSERT(same_bytes(o, c1_x, 16));
	knl_bts_s1(zero, s1_r1, s1_r2, o);
	KT_ASSERT(same_bytes(o, s1_x, 16));
	knl_bts_f4(u, v, x, 0, o);
	KT_ASSERT(same_bytes(o, f4_x, 16));
	knl_bts_f5(w, x, y, a1, a2, o, o2);
	KT_ASSERT(same_bytes(o, f5_mk, 16));
	KT_ASSERT(same_bytes(o2, f5_ltk, 16));
	knl_bts_f6(f5_mk, x, y, f6_r, f6_io, a1, a2, o);
	KT_ASSERT(same_bytes(o, f6_x, 16));
	KT_ASSERT_EQ(knl_bts_g2(u, v, x, y), 0x2f9ed5ba);

	turn(pa, priv_a, 32);
	KT_ASSERT_ER(knl_bts_p256_key(pa, pub, FALSE), E_OK);
	turn(t, pub_ax, 32);
	KT_ASSERT(same_bytes(pub, t, 32));
	turn(t, pub_ay, 32);
	KT_ASSERT(same_bytes(pub + 32, t, 32));
	turn(pb, priv_b, 32);
	KT_ASSERT_ER(knl_bts_p256_key(pb, pub2, FALSE), E_OK);
	KT_ASSERT_ER(knl_bts_p256_dh(pb, pub, dh), E_OK);
	turn(t, dh_ab, 32);
	KT_ASSERT(same_bytes(dh, t, 32));
	KT_ASSERT_ER(knl_bts_p256_dh(pa, pub2, dh), E_OK);
	KT_ASSERT(same_bytes(dh, t, 32));
	/* a point off the curve is refused */
	pub[32] ^= 1;
	KT_ASSERT_ER(knl_bts_p256_dh(pb, pub, dh), E_PAR);
}

/* The same frames here as on a host, where another decoder took them for what went in */
LOCAL void test_bt_sbc( void )
{
	SBC_ENC	*e = (SBC_ENC *)Kmalloc(sizeof(SBC_ENC));
	short	pcm[SBC_FRAME_SAMPLES * 2];
	UB	f[SBC_FRAME_MAX];
	UW	h = 2166136261U;
	INT	i, k, n, t = 0;

	KT_ASSERT(e != NULL);
	if ( e == NULL ) return;
	sbc_enc_init(e, 0, 1, 53);
	KT_ASSERT_EQ(sbc_enc_frame_len(e), 119);
	for ( k = 0; k < 40; k++ ) {
		for ( i = 0; i < SBC_FRAME_SAMPLES; i++, t++ ) {
			pcm[2 * i] = (short)( ( t * 37 ) % 4000 - 2000 );
			pcm[2 * i + 1] = (short)( ( t * 113 ) % 9000 - 4500 );
		}
		n = sbc_enc_frame(e, pcm, f);
		KT_ASSERT_EQ(n, 119);
		KT_ASSERT_EQ(f[0], 0x9c);
		for ( i = 0; i < n; i++ ) h = ( h ^ f[i] ) * 16777619U;
	}
	tm_printf((UB *)"  40 frames hash to %08x\n", h);
	KT_ASSERT_EQ(h, SBC_KAT_HASH);
	Kfree(e);
}

/* ---------------------------------------------------------------- an LE keyboard and mouse */

#define H_LE		6

LOCAL CONST UB	le_addr[6] = { 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6 };	/* random, static */
LOCAL CONST UB	beacon_addr[6] = { 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xc6 };	/* a device not for input */
LOCAL CONST UB	le_id[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };	/* its identity, public */
LOCAL CONST UB	le_irk[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
LOCAL CONST UB	le_ltk[16] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60 };
LOCAL CONST UB	le_rnd[8] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8 };
#define LE_EDIV		0x4321

LOCAL CONST UB	host_addr[6] = { 1, 2, 3, 4, 5, 6 };

/* Handles of its HID service */
#define G_SVC_FIRST	0x10
#define G_MAP		0x12
#define G_MODE		0x14
#define G_KBD		0x16
#define G_KBD_CCC	0x17
#define G_KBD_REF	0x18
#define G_MOUSE		0x1a
#define G_MOUSE_CCC	0x1b
#define G_MOUSE_REF	0x1c
#define G_FEAT		0x1e
#define G_FEAT_REF	0x1f
#define G_SVC_LAST	0x1f

typedef struct {
	BOOL	sc;			/* it can do Secure Connections */
	BOOL	bad_confirm;		/* its numbers do not fit */
	BOOL	bad_key;		/* its public key is no point of the curve */
	BOOL	connected, up_gatt;
	UB	peer_addr[6];		/* the address it is connecting from */
	UB	peer_type;
	UB	preq[7], pres[7];
	UB	mconf[16], srand[16], na[16], nb[16], mackey[16], key[16];
	UB	priv[32], pub[64], pa[64], dh[32];
	UB	want_key[16];
	BOOL	have_want;
	INT	n_req, n_start_enc, n_failed, n_failed_why, n_conn, n_disc;
	INT	n_mode, n_ccc, n_blob, n_scan_passive, n_scan_active;
	BOOL	enc_ok;
	UH	last_ediv;
	UB	last_rnd[8];
} MOCKLE;

LOCAL MOCKLE	m;
LOCAL BOOL	mock_adv;		/* a scan finds an LE device */

/* An L2CAP frame from the device, in LE packets of 27 bytes */
LOCAL void le_send( UH cid, CONST UB *d, INT n )
{
	UB	f[300], b[4 + 27];
	INT	total = n + 4, at = 0;

	f[0] = (UB)n;
	f[1] = (UB)( n >> 8 );
	f[2] = (UB)cid;
	f[3] = (UB)( cid >> 8 );
	knl_memcpy(f + 4, d, n);
	while ( at < total ) {
		INT	k = ( total - at > 27 ) ? 27 : total - at;

		b[0] = H_LE;
		b[1] = ( at == 0 ) ? 0x20 : 0x10;
		b[2] = (UB)k;
		b[3] = 0;
		knl_memcpy(b + 4, f + at, k);
		KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, b, k + 4), E_OK);
		at += k;
	}
}

LOCAL void smp_key_dist( void )
{
	UB	b[17];

	if ( !m.sc ) {
		b[0] = 0x06;
		knl_memcpy(b + 1, le_ltk, 16);
		le_send(6, b, 17);
		b[0] = 0x07;
		b[1] = (UB)LE_EDIV;
		b[2] = (UB)( LE_EDIV >> 8 );
		knl_memcpy(b + 3, le_rnd, 8);
		le_send(6, b, 11);
	}
	b[0] = 0x08;
	knl_memcpy(b + 1, le_irk, 16);
	le_send(6, b, 17);
	b[0] = 0x09;
	b[1] = 0;
	knl_memcpy(b + 2, le_id, 6);
	le_send(6, b, 8);
}

/* The device's side of pairing: what the same functions make from what this end sent */
LOCAL void smp_serve( CONST UB *f, INT n )
{
	static CONST UB	tk[16] = { 0 };
	UB		b[65], o[16], a[7], r[7], io[3];

	switch ( f[0] ) {
	case 0x01:
		m.n_req++;
		KT_ASSERT_EQ(n, 7);
		KT_ASSERT_EQ(f[1], 3);		/* no input, no output */
		KT_ASSERT_EQ(f[3] & 1, 1);	/* bonding */
		knl_memcpy(m.preq, f, 7);
		m.pres[0] = 0x02;
		m.pres[1] = 3;
		m.pres[2] = 0;
		m.pres[3] = (UB)( 0x01 | ( m.sc ? 0x08 : 0 ) );
		m.pres[4] = 16;
		m.pres[5] = 0;
		m.pres[6] = (UB)( ( f[6] & 3 ) );
		le_send(6, m.pres, 7);
		break;
	case 0x03:				/* the older pairing: its confirm */
		if ( m.sc ) break;
		knl_memcpy(m.mconf, f + 1, 16);
		knl_memset(m.srand, 0x77, 16);
		b[0] = 0x03;
		knl_bts_c1(tk, m.srand, m.preq, m.pres, 0, host_addr, 1, m.peer_addr, b + 1);
		if ( m.bad_confirm ) b[1] ^= 1;
		le_send(6, b, 17);
		break;
	case 0x04:
		if ( !m.sc ) {
			knl_bts_c1(tk, f + 1, m.preq, m.pres, 0, host_addr, 1, m.peer_addr, o);
			KT_ASSERT(same_bytes(o, m.mconf, 16));
			b[0] = 0x04;
			knl_memcpy(b + 1, m.srand, 16);
			le_send(6, b, 17);
			knl_bts_s1(tk, m.srand, f + 1, m.want_key);
			m.have_want = TRUE;
		} else {
			knl_memcpy(m.na, f + 1, 16);
			b[0] = 0x04;
			knl_memcpy(b + 1, m.nb, 16);
			le_send(6, b, 17);
			knl_memcpy(a, host_addr, 6);
			a[6] = 0;
			knl_memcpy(r, m.peer_addr, 6);
			r[6] = 1;
			knl_bts_f5(m.dh, m.na, m.nb, a, r, m.mackey, m.want_key);
			m.have_want = TRUE;
		}
		break;
	case 0x0c:				/* Secure Connections: its key, and the confirm of its nonce */
		KT_ASSERT_EQ(n, 65);
		knl_memcpy(m.pa, f + 1, 64);
		KT_ASSERT_ER(knl_bts_p256_key(m.priv, m.pub, TRUE), E_OK);
		KT_ASSERT_ER(knl_bts_p256_dh(m.priv, m.pa, m.dh), E_OK);
		b[0] = 0x0c;
		knl_memcpy(b + 1, m.pub, 64);
		if ( m.bad_key ) b[33] ^= 1;
		le_send(6, b, 65);
		if ( m.bad_key ) break;
		knl_memset(m.nb, 0x33, 16);
		b[0] = 0x03;
		knl_bts_f4(m.pub, m.pa, m.nb, 0, b + 1);
		if ( m.bad_confirm ) b[1] ^= 1;
		le_send(6, b, 17);
		break;
	case 0x0d: {
		static CONST UB	zero[16] = { 0 };

		knl_memcpy(a, host_addr, 6);
		a[6] = 0;
		knl_memcpy(r, m.peer_addr, 6);
		r[6] = 1;
		io[0] = m.preq[1];
		io[1] = m.preq[2];
		io[2] = m.preq[3];
		knl_bts_f6(m.mackey, m.na, m.nb, zero, io, a, r, o);
		KT_ASSERT(same_bytes(o, f + 1, 16));
		io[0] = m.pres[1];
		io[1] = m.pres[2];
		io[2] = m.pres[3];
		b[0] = 0x0d;
		knl_bts_f6(m.mackey, m.nb, m.na, zero, io, r, a, b + 1);
		le_send(6, b, 17);
		break;
	}
	case 0x05:
		m.n_failed++;
		m.n_failed_why = f[1];
		break;
	default:
		break;
	}
}

/* Its services, as a server of MTU 23 says them: a few at a time */
LOCAL void att_err( UB op, UH h, UB why )
{
	UB	b[5];

	b[0] = 0x01;
	b[1] = op;
	b[2] = (UB)h;
	b[3] = (UB)( h >> 8 );
	b[4] = why;
	le_send(4, b, 5);
}

LOCAL void put16( UB *p, UH v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void att_serve( CONST UB *f, INT n )
{
	static CONST struct { UH decl, val, uuid; } chars[] = {
		{ 0x11, G_MAP, 0x2a4b }, { 0x13, G_MODE, 0x2a4e }, { 0x15, G_KBD, 0x2a4d },
		{ 0x19, G_MOUSE, 0x2a4d }, { 0x1d, G_FEAT, 0x2a4d }
	};
	static CONST struct { UH h, uuid; } descs[] = {
		{ G_KBD_CCC, 0x2902 }, { G_KBD_REF, 0x2908 }, { G_MOUSE_CCC, 0x2902 }, { G_MOUSE_REF, 0x2908 }, { G_FEAT_REF, 0x2908 }
	};
	UB	b[64];
	UH	first = 0, last = 0, h;
	INT	i, k = 2;

	if ( n >= 5 ) {
		first = (UH)( f[1] | ( f[2] << 8 ) );
		last = (UH)( f[3] | ( f[4] << 8 ) );
	}
	h = (UH)( ( n >= 3 ) ? ( f[1] | ( f[2] << 8 ) ) : 0 );
	switch ( f[0] ) {
	case 0x02:
		b[0] = 0x03;
		put16(b + 1, 23);
		le_send(4, b, 3);
		break;
	case 0x10:
		b[0] = 0x11;
		b[1] = 6;
		if ( first <= 1 ) {
			put16(b + 2, 1);
			put16(b + 4, 0x0f);
			put16(b + 6, 0x1800);
			k = 8;
		}
		if ( first <= G_SVC_FIRST ) {
			put16(b + k, G_SVC_FIRST);
			put16(b + k + 2, G_SVC_LAST);
			put16(b + k + 4, 0x1812);
			k += 6;
		}
		if ( k == 2 ) att_err(0x10, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x08:
		b[0] = 0x09;
		b[1] = 7;
		for ( i = 0; i < (INT)( sizeof(chars) / sizeof(chars[0]) ) && k + 7 <= 2 + 2 * 7; i++ ) {
			if ( chars[i].decl < first || chars[i].decl > last ) continue;
			put16(b + k, chars[i].decl);
			b[k + 2] = 0x12;
			put16(b + k + 3, chars[i].val);
			put16(b + k + 5, chars[i].uuid);
			k += 7;
		}
		if ( k == 2 ) att_err(0x08, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x04:
		b[0] = 0x05;
		b[1] = 1;
		for ( i = 0; i < (INT)( sizeof(descs) / sizeof(descs[0]) ) && k + 4 <= 2 + 2 * 4; i++ ) {
			if ( descs[i].h < first || descs[i].h > last ) continue;
			put16(b + k, descs[i].h);
			put16(b + k + 2, descs[i].uuid);
			k += 4;
		}
		if ( k == 2 ) att_err(0x04, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x0a:
		b[0] = 0x0b;
		if ( h == G_KBD_REF || h == G_MOUSE_REF || h == G_FEAT_REF ) {
			b[1] = ( h == G_KBD_REF ) ? 1 : ( h == G_MOUSE_REF ) ? 2 : 3;
			b[2] = ( h == G_FEAT_REF ) ? 3 : 1;
			le_send(4, b, 3);
		} else if ( h == G_MAP ) {
			INT	l = ( sizeof(rd_combo) > 22 ) ? 22 : (INT)sizeof(rd_combo);

			knl_memcpy(b + 1, rd_combo, l);
			le_send(4, b, l + 1);
		} else {
			att_err(0x0a, h, 0x01);
		}
		break;
	case 0x0c: {
		INT	off = f[3] | ( f[4] << 8 ), l = (INT)sizeof(rd_combo) - off;

		m.n_blob++;
		if ( l > 22 ) l = 22;
		b[0] = 0x0d;
		knl_memcpy(b + 1, rd_combo + off, l);
		le_send(4, b, l + 1);
		break;
	}
	case 0x12:
		if ( h == G_KBD_CCC || h == G_MOUSE_CCC ) {
			KT_ASSERT_EQ(f[3], 1);
			m.n_ccc++;
		}
		b[0] = 0x13;
		le_send(4, b, 1);
		break;
	case 0x52:
		if ( h == G_MODE ) {
			KT_ASSERT_EQ(f[3], 1);
			m.n_mode++;
		}
		break;
	default:
		break;
	}
}

LOCAL void le_meta_conn( CONST UB *peer, UB type )
{
	UB	e[21];

	e[0] = 0x3e;
	e[1] = 19;
	e[2] = 0x01;
	e[3] = 0;
	e[4] = H_LE;
	e[5] = 0;
	e[6] = 0;		/* the central */
	e[7] = type;
	knl_memcpy(e + 8, peer, 6);
	e[14] = 24;
	e[15] = 0;
	e[16] = e[17] = 0;
	e[18] = 200;
	e[19] = 0;
	e[20] = 0;
	ev(e, 21);
}

LOCAL void le_adv( CONST UB *peer, UB evt, CONST UB *ad, INT n )
{
	UB	e[48];

	e[0] = 0x3e;
	e[1] = (UB)( 12 + n );
	e[2] = 0x02;
	e[3] = 1;
	e[4] = evt;
	e[5] = 1;
	knl_memcpy(e + 6, peer, 6);
	e[12] = (UB)n;
	knl_memcpy(e + 13, ad, n);
	e[13 + n] = 0xd0;
	ev(e, 14 + n);
}

/* What the controller does of the LE commands; FALSE for others */
LOCAL BOOL serve_le( UH op, CONST UB *p )
{
	static CONST UB	ad_kb[] = { 0x02, 0x01, 0x05, 0x03, 0x03, 0x12, 0x18, 0x03, 0x19, 0xc1, 0x03 };
	static CONST UB	rsp_kb[] = { 0x06, 0x09, 'L', 'E', 'k', 'b', 'd' };
	static CONST UB	ad_beacon[] = { 0x02, 0x01, 0x05, 0x03, 0x03, 0x0f, 0x18 };
	static CONST UB	lebuf[3] = { 27, 0, 4 };
	UB	e[8];

	if ( lea_cmd(op, p) ) return TRUE;
	switch ( op ) {
	case 0x0c6d: case 0x2001:
		n_start++;
		cmd_done(op, NULL, 0);
		return TRUE;
	case 0x2002:
		n_start++;
		cmd_done(op, lebuf, 3);
		return TRUE;
	case 0x200b:
		if ( p[0] == 1 ) m.n_scan_active++;
		else m.n_scan_passive++;
		cmd_done(op, NULL, 0);
		return TRUE;
	case 0x200c:
		cmd_done(op, NULL, 0);
		if ( p[0] == 1 && mock_adv && m.n_scan_active > 0 && !m.connected ) {
			le_adv(le_addr, 0x00, ad_kb, sizeof(ad_kb));
			le_adv(le_addr, 0x04, rsp_kb, sizeof(rsp_kb));
			le_adv(beacon_addr, 0x00, ad_beacon, sizeof(ad_beacon));
		}
		return TRUE;
	case 0x200d:
		cmd_status(op);
		m.peer_type = p[5];
		knl_memcpy(m.peer_addr, p + 6, 6);
		m.connected = TRUE;
		m.n_conn++;
		le_meta_conn(p + 6, p[5]);
		return TRUE;
	case 0x0406:		/* the link dropped: the test says when it is gone */
		cmd_status(op);
		return TRUE;
	case 0x200e: case 0x2013:
		cmd_status(op);
		return TRUE;
	case 0x2019: {
		BOOL	ok;

		m.n_start_enc++;
		m.last_ediv = (UH)( p[10] | ( p[11] << 8 ) );
		knl_memcpy(m.last_rnd, p + 2, 8);
		cmd_status(op);
		ok = (BOOL)( m.have_want && same_bytes(p + 12, m.want_key, 16) );
		m.enc_ok = ok;
		e[0] = 0x08;
		e[1] = 4;
		e[2] = ok ? 0 : 0x06;
		e[3] = H_LE;
		e[4] = 0;
		e[5] = ok ? 1 : 0;
		ev(e, 6);
		if ( ok && m.n_req > 0 && m.have_want && !m.up_gatt ) {
			m.up_gatt = TRUE;
			smp_key_dist();
		}
		return TRUE;
	}
	default:
		return FALSE;
	}
}

/* An L2CAP frame on the LE connection */
LOCAL void le_serve( CONST UB *b, INT n )
{
	INT	cid = b[6] | ( b[7] << 8 );

	if ( cid == 6 ) smp_serve(b + 8, n - 8);
	else if ( cid == 4 && lea_dev ) la_att(b + 8, n - 8);
	else if ( cid == 4 ) att_serve(b + 8, n - 8);
}

LOCAL void le_gone( void )
{
	UB	e[6];

	m.connected = FALSE;
	e[0] = 0x05;
	e[1] = 4;
	e[2] = 0;
	e[3] = H_LE;
	e[4] = 0;
	e[5] = 0x13;
	ev(e, 6);
}

/* An LE device found, paired as it can, its reports read; left connected */
LOCAL void le_pair_up( BOOL sc )
{
	T_BTFOUND	f[4];
	INT		i, n, k;

	qh = qt = 0;
	acl_have = 0;
	knl_memset(&m, 0, sizeof(m));
	m.sc = sc;
	mock_adv = TRUE;
	dev_rd = NULL;
	n_start = 0;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	pump(300);
	n = knl_bt_found(f, 4);
	/*
	 * The classic keyboard, the LE one by its name, and the device that
	 * takes a connection and says nothing of input (its battery service
	 * alone): listed as well, the pairing to find out what it is
	 */
	KT_ASSERT_EQ(n, 3);
	for ( i = 0, k = 0; i < n; i++ ) {
		if ( f[i].le != 0 && same_bytes(f[i].addr, le_addr, 6) ) {
			KT_ASSERT_EQ(f[i].atype, 1);
			KT_ASSERT_EQ(f[i].name[0], 'L');
			k |= 1;
		} else if ( f[i].le != 0 && same_bytes(f[i].addr, beacon_addr, 6) ) {
			k |= 2;
		}
	}
	KT_ASSERT_EQ(k, 3);
	KT_ASSERT_ER(knl_bt_pair(le_addr), E_OK);
}

LOCAL void le_clean( CONST T_HIDSTAT *s0 )
{
	if ( m.connected ) le_gone();
	pump(200);
	bt_end(s0);
	mock_adv = FALSE;
}

LOCAL void le_reports( void )
{
	static CONST UB	key[] = { 0x1b, G_KBD, 0, 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
	static CONST UB	key_off[] = { 0x1b, G_KBD, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	static CONST UB	mouse[] = { 0x1b, G_MOUSE, 0, 0, 5, 3 };
	T_HIDEV		e;

	while ( ts_hid_read(&e, TMO_POL) >= E_OK ) ;
	le_send(4, key, sizeof(key));
	pump(50);
	KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
	if ( e.type == HID_EV_KEY_DOWN && e.code == 0xe1 ) KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
	KT_ASSERT_EQ(e.type, HID_EV_KEY_DOWN);
	KT_ASSERT_EQ(e.code, 0x04);
	le_send(4, key_off, sizeof(key_off));
	pump(50);
	while ( ts_hid_read(&e, 100) >= E_OK ) ;
	le_send(4, mouse, sizeof(mouse));
	pump(50);
	KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
	KT_ASSERT_EQ(e.type, HID_EV_MOVE);
}

LOCAL void hid_quiet( T_HIDATTR *keep, T_HIDSTAT *s0 )
{
	T_HIDATTR	a;

	KT_ASSERT_ER(ts_hid_getattr(keep), E_OK);
	a = *keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	(void)ts_hid_setattr(&a);
	KT_ASSERT_ER(ts_hid_stat(s0), E_OK);
}

LOCAL void gatt_seen( CONST T_HIDSTAT *s0, INT times )
{
	T_HIDSTAT	s1;

	KT_ASSERT_EQ(m.n_mode, times);
	KT_ASSERT_EQ(m.n_ccc, 2 * times);	/* the two input reports, not the feature one */
	KT_ASSERT(m.n_blob >= times);		/* the map came in pieces */
	KT_ASSERT_ER(ts_hid_stat(&s1), E_OK);
	KT_ASSERT_EQ(s1.keyboards, s0->keyboards + 1);
	KT_ASSERT_EQ(s1.pointers, s0->pointers + 1);
}

/* The older pairing, Just Works: a key from a TK of zero, and the keys it gives */
LOCAL void test_le_legacy( void )
{
	T_HIDATTR	keep;
	T_HIDSTAT	s0;

	hid_quiet(&keep, &s0);
	le_pair_up(FALSE);
	pump(1500);
	KT_ASSERT_EQ(m.n_req, 1);
	KT_ASSERT_EQ(m.n_start_enc, 1);
	KT_ASSERT(m.enc_ok);
	gatt_seen(&s0, 1);
	le_reports();
	le_clean(&s0);
	(void)ts_hid_setattr(&keep);
}

/* Secure Connections, Just Works; then the same device back, by a private address */
LOCAL void test_le_sc( void )
{
	T_HIDATTR	keep;
	T_HIDSTAT	s0;
	UB		rpa[6], h[3];
	static CONST UB	ad[] = { 0x02, 0x01, 0x05 };
	INT		i;

	hid_quiet(&keep, &s0);
	le_pair_up(TRUE);
	for ( i = 0; i < 80 && m.n_ccc < 2; i++ ) pump(50);
	pump(200);
	KT_ASSERT_EQ(m.n_req, 1);
	KT_ASSERT_EQ(m.n_start_enc, 1);
	KT_ASSERT(m.enc_ok);
	KT_ASSERT_EQ(m.last_ediv, 0);
	gatt_seen(&s0, 1);
	le_reports();

	/* it goes away: this end listens without asking anything of what it hears */
	le_gone();
	pump(1200);
	KT_ASSERT(m.n_scan_passive >= 1);
	/* a private address its identity key resolves, for one that no other key does */
	rpa[3] = 0x11;
	rpa[4] = 0x22;
	rpa[5] = 0x45;
	knl_bts_ah(le_irk, rpa + 3, h);
	knl_memcpy(rpa, h, 3);
	le_adv(rpa, 0x00, ad, sizeof(ad));
	for ( i = 0; i < 80 && m.n_ccc < 4; i++ ) pump(50);
	pump(200);
	KT_ASSERT_EQ(m.n_conn, 2);
	KT_ASSERT(same_bytes(m.peer_addr, rpa, 6));
	KT_ASSERT_EQ(m.n_req, 1);		/* not paired again */
	KT_ASSERT_EQ(m.n_start_enc, 2);
	KT_ASSERT(m.enc_ok);
	gatt_seen(&s0, 2);
	le_reports();
	le_clean(&s0);
	(void)ts_hid_setattr(&keep);
}

/* What must not pass: a confirm that does not fit, a key off the curve */
LOCAL void le_refuse_run( void );

LOCAL void test_le_refused( void )
{
	T_HIDATTR	keep;
	T_HIDSTAT	s0, s1;
	INT		round;

	hid_quiet(&keep, &s0);
	for ( round = 0; round < 3; round++ ) {
		BOOL	sc = (BOOL)( round != 0 );

		qh = qt = 0;
		acl_have = 0;
		knl_memset(&m, 0, sizeof(m));
		m.sc = sc;
		m.bad_confirm = (BOOL)( round != 2 );
		m.bad_key = (BOOL)( round == 2 );
		le_refuse_run();
		KT_ASSERT_EQ(m.n_failed, 1);
		KT_ASSERT_EQ(m.n_failed_why, ( round == 2 ) ? 0x0a : 0x04);
		KT_ASSERT_EQ(m.n_start_enc, 0);
		KT_ASSERT_ER(ts_hid_stat(&s1), E_OK);
		KT_ASSERT_EQ(s1.keyboards, s0.keyboards);
	}
	(void)ts_hid_setattr(&keep);
}

/* ---------------------------------------------------------------- earphones */

LOCAL BOOL	av_mode;
LOCAL INT	av_discover, av_caps, av_config, av_open, av_start, av_suspend, av_media;
LOCAL UW	av_ts;
LOCAL UH	av_seq;
LOCAL INT	av_bad;
LOCAL UB	av_cfg[4];
LOCAL INT	av_frames;		/* SBC frames come */
LOCAL T_HIDSTAT	av_hid0;		/* the input layer's counts before the earphones */
LOCAL INT	av_max_len;		/* the longest packet of the stream */

#define CID_DEV_AVS	0x73
#define CID_DEV_AVM	0x74

/* The frame's own check, from the frame */
LOCAL UB sbc_crc( CONST UB *f, INT nbits )
{
	UB	crc = 0x0f;
	INT	i, at;

	for ( i = 0; i < 16 + nbits; i++ ) {
		INT	byte = ( i < 16 ) ? 1 + i / 8 : 4 + ( i - 16 ) / 8;

		at = ( i < 16 ) ? i & 7 : ( i - 16 ) & 7;
		{
			INT	in = ( f[byte] >> ( 7 - at ) ) & 1, top = ( crc >> 7 ) & 1;

			crc = (UB)( crc << 1 );
			if ( top ^ in ) crc ^= 0x1d;
		}
	}
	return crc;
}

LOCAL void av_reply( CONST UB *f, CONST UB *r, INT n )
{
	UB	b[64];

	b[0] = 5;
	b[1] = 0x20;
	b[2] = (UB)( n + 2 + 4 );
	b[3] = 0;
	b[4] = (UB)( n + 2 );
	b[5] = 0;
	b[6] = 0x43;
	b[7] = 0;
	b[8] = (UB)( ( f[0] & 0xf0 ) | 2 );
	b[9] = f[1];
	if ( n > 0 ) knl_memcpy(b + 10, r, n);
	KT_ASSERT_ER(knl_vhc_in(1, EP_ACL, b, n + 10), E_OK);
}

LOCAL void av_serve( CONST UB *f, INT n )
{
	static CONST UB	seps[] = {
		( 3 << 2 ), ( 0 << 4 ) | ( 0 << 3 ),		/* a source: not for this end */
		( 4 << 2 ) | 2, ( 0 << 4 ) | ( 1 << 3 ),	/* a sink in use */
		( 2 << 2 ), ( 0 << 4 ) | ( 1 << 3 )		/* the sink */
	};
	static CONST UB	caps[] = { 1, 0, 7, 6, 0, 0, 0xff, 0xff, 2, 64 };

	switch ( f[1] & 0x3f ) {
	case 0x01:
		av_discover++;
		av_reply(f, seps, sizeof(seps));
		break;
	case 0x02:
		av_caps++;
		KT_ASSERT_EQ(f[2], 2 << 2);
		av_reply(f, caps, sizeof(caps));
		break;
	case 0x03:
		av_config++;
		KT_ASSERT_EQ(n, 14);
		KT_ASSERT_EQ(f[2], 2 << 2);
		KT_ASSERT_EQ(f[6], 7);
		knl_memcpy(av_cfg, f + 10, 4);
		av_reply(f, NULL, 0);
		break;
	case 0x06:
		av_open++;
		av_reply(f, NULL, 0);
		break;
	case 0x07:
		av_start++;
		av_reply(f, NULL, 0);
		break;
	case 0x09:
		av_suspend++;
		av_reply(f, NULL, 0);
		break;
	default:
		break;
	}
}

/* A packet of the stream: its RTP header in order, and frames that hold together */
LOCAL void av_media_in( CONST UB *f, INT n )
{
	INT	i, nf;
	UH	seq = (UH)( ( f[2] << 8 ) | f[3] );
	UW	ts = ( (UW)f[4] << 24 ) | ( (UW)f[5] << 16 ) | ( (UW)f[6] << 8 ) | f[7];

	if ( f[0] != 0x80 || f[1] != 96 ) av_bad++;
	if ( av_media > 0 && ( seq != (UH)( av_seq + 1 ) || ts != av_ts ) ) av_bad++;
	nf = f[12] & 0x0f;
	if ( n != 13 + nf * 119 || nf == 0 ) av_bad++;
	for ( i = 0; i < nf && 13 + ( i + 1 ) * 119 <= n; i++ ) {
		CONST UB	*s = f + 13 + i * 119;

		if ( s[0] != 0x9c || s[1] != 0xbd || s[2] != 53 ) av_bad++;
		if ( s[3] != sbc_crc(s, 8 + 64) ) av_bad++;
	}
	av_seq = seq;
	av_ts = ts + (UW)nf * 128;
	av_media++;
	av_frames += nf;
	if ( n > av_max_len ) av_max_len = n;
}

LOCAL void a2dp_run( UH media_mtu )
{
	T_BTFOUND	f[4];
	SDOut		o[8];
	CONST UB	*id = NULL;
	UW		rate = 0;
	INT		i, n, ch = 0, at = -1;

	qh = qt = 0;
	acl_have = 0;
	KT_ASSERT_ER(ts_hid_stat(&av_hid0), E_OK);	/* before the remote control's slot */
	knl_memset(&m, 0, sizeof(m));
	knl_memset(mch, 0, sizeof(mch));
	mock_media_mtu = media_mtu;
	mock_adv = FALSE;
	av_mode = TRUE;
	mock_cod = 0x240404;		/* audio, wearable headset */
	mock_acl_len = 1021;
	av_discover = av_caps = av_config = av_open = av_start = av_suspend = av_media = av_bad = 0;
	av_frames = av_max_len = 0;
	dev_rd = rd_bitmap;
	dev_rdlen = sizeof(rd_bitmap);
	sdp_make();
	n_start = n_authreq = n_enc = n_lkneg = n_iocap = n_confirm = n_ctrl_set = n_ctrl_report = 0;
	n_sdp_ask = n_sdp_disc = 0;
	saw_conn = FALSE;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	pump(300);
	n = knl_bt_found(f, 4);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT_ER(knl_bt_pair(kb_addr), E_OK);
	pump(1500);
	KT_ASSERT_EQ(n_enc, 1);
	KT_ASSERT(n_sdp_ask >= 1);
	KT_ASSERT_EQ(av_discover, 1);
	KT_ASSERT_EQ(av_caps, 1);		/* the free sink alone is asked */
	KT_ASSERT_EQ(av_config, 1);
	KT_ASSERT_EQ(av_open, 1);
	KT_ASSERT_EQ(n_ctrl_set + n_ctrl_report, 0);	/* it is no input device */
	/* 44.1 kHz, joint stereo; 16 blocks, 8 subbands, loudness; bit pool up to 53 */
	KT_ASSERT_EQ(av_cfg[0], 0x21);
	KT_ASSERT_EQ(av_cfg[1], 0x15);
	KT_ASSERT_EQ(av_cfg[3], 53);

	/* an output the sound device lists */
	n = knl_snd_outputs(o, 8);
	for ( i = 0; i < n && i < 8; i++ ) {
		if ( o[i].kind == SND_BE_BT ) at = i;
	}
	KT_ASSERT(at >= 0);
	if ( at >= 0 ) {
		id = o[at].id;
		tm_printf((UB *)"  output %s \"%s\"\n", id, o[at].name);
		knl_usb_lock();
		KT_ASSERT_ER(knl_bta_hw.play_start(id, &rate, &ch), E_OK);
		knl_usb_unlock();
		KT_ASSERT_EQ(rate, 44100);
		KT_ASSERT_EQ(ch, 2);
		pump(100);
		KT_ASSERT_EQ(av_start, 1);
		for ( i = 0; i < 100; i++ ) {
			knl_usb_lock();
			knl_bta_hw.poll();
			knl_usb_unlock();
			pump(10);
		}
		tm_printf((UB *)"  %d packets of the stream in about a second, %d bytes the longest\n", av_media, av_max_len);
		KT_ASSERT(av_media >= 10);
		KT_ASSERT_EQ(av_bad, 0);
		KT_ASSERT(av_max_len <= 1100 - 4);
		knl_usb_lock();
		knl_bta_hw.play_stop();
		knl_usb_unlock();
		pump(100);
		KT_ASSERT_EQ(av_suspend, 1);
	}
	bt_end(&av_hid0);
	av_mode = FALSE;
	mock_cod = 0x000540;
	mock_acl_len = 64;
	mock_media_mtu = 0;
	dev_rd = NULL;
}

LOCAL void test_a2dp( void )
{
	a2dp_run(0);
}

/* A device that takes packets longer than this end sends: as many frames as one packet of this end holds */
LOCAL void test_a2dp_mtu( void )
{
	a2dp_run(2048);
}

/* ---------------------------------------------------------------- earphones that set up the stream */

LOCAL UB	avi_tl;
LOCAL INT	avi_bad;		/* answers that were not as they should be */
LOCAL INT	avi_host_disc;		/* this end's own Discover, which it should not send */
LOCAL INT	avi_suspend;
LOCAL BOOL	avi_set, avi_media_up, avi_started;
LOCAL BOOL	avi_caps_sbc;

/* A command of the device's on its own signalling channel */
LOCAL void avi_cmd( UB sig, CONST UB *d, INT n )
{
	UB	b[32];
	MCH	*c = mch_of(CID_DEV_AVS2);

	if ( c == NULL || n > 30 ) return;
	avi_tl = (UB)( ( avi_tl + 1 ) & 15 );
	b[0] = (UB)( avi_tl << 4 );
	b[1] = sig;
	if ( n > 0 ) knl_memcpy(b + 2, d, n);
	cl_send(c->ours, b, n + 2);
}

/* What this end says on the device's signalling channel */
LOCAL void avi_in( CONST UB *f, INT n )
{
	static CONST UB	cfg[] = { 1 << 2, 2 << 2, 1, 0, 7, 6, 0, 0, 0x21, 0x15, 2, 53 };
	UB		s1 = 1 << 2;
	INT		at;

	if ( n < 2 ) return;
	if ( ( f[0] & 3 ) == 0 ) {		/* a command of this end's */
		UB	r[2];
		MCH	*c = mch_of(CID_DEV_AVS2);

		if ( ( f[1] & 0x3f ) == 0x01 ) avi_host_disc++;
		if ( ( f[1] & 0x3f ) == 0x09 ) avi_suspend++;
		r[0] = (UB)( ( f[0] & 0xf0 ) | 2 );
		r[1] = f[1];
		if ( c != NULL ) cl_send(c->ours, r, 2);
		return;
	}
	if ( ( f[0] & 3 ) != 2 || ( f[0] >> 4 ) != avi_tl ) {
		tm_printf((UB *)"  this end answered %02x %02x\n", f[0], f[1]);
		avi_bad++;
		return;
	}
	switch ( f[1] & 0x3f ) {
	case 0x01:				/* one stream end: a source of audio, free */
		if ( n != 4 || ( f[2] >> 2 ) != 1 || ( f[2] & 2 ) != 0 || ( f[3] >> 4 ) != 0 || ( f[3] & 8 ) != 0 ) avi_bad++;
		avi_cmd(0x0c, &s1, 1);
		break;
	case 0x0c:				/* its capabilities: the transport, and SBC as this end makes it */
		for ( at = 2; at + 2 <= n; at += 2 + f[at + 1] ) {
			if ( f[at] == 7 && f[at + 1] == 6 && f[at + 2] == 0 && f[at + 3] == 0 ) {
				avi_caps_sbc = (BOOL)( ( f[at + 4] & 0x21 ) == 0x21 && ( f[at + 5] & 0x15 ) == 0x15 && f[at + 7] >= 53 );
			}
		}
		avi_cmd(0x03, cfg, sizeof(cfg));
		break;
	case 0x03:
		avi_set = TRUE;
		avi_cmd(0x06, &s1, 1);
		break;
	case 0x06:
		dev_open(0x0019, CID_DEV_AVM2);
		break;
	case 0x07:
		avi_started = TRUE;
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the device asking this end's records */

LOCAL UB	sdpc_got[1200];
LOCAL INT	sdpc_n, sdpc_asks, sdpc_handles;
LOCAL BOOL	sdpc_done;
LOCAL UB	sdpc_pdu;
LOCAL UH	sdpc_uuid, sdpc_max, sdpc_tid;

LOCAL void sdpc_ask( CONST UB *cont, INT nc )
{
	UB	b[40];
	INT	k = 5;
	MCH	*c = mch_of(CID_DEV_SDPC);

	if ( c == NULL ) return;
	sdpc_asks++;
	sdpc_tid++;
	b[0] = sdpc_pdu;
	b[1] = (UB)( sdpc_tid >> 8 );
	b[2] = (UB)sdpc_tid;
	b[k++] = 0x35;
	b[k++] = 3;
	b[k++] = 0x19;
	b[k++] = (UB)( sdpc_uuid >> 8 );
	b[k++] = (UB)sdpc_uuid;
	b[k++] = (UB)( sdpc_max >> 8 );
	b[k++] = (UB)sdpc_max;
	if ( sdpc_pdu == 0x06 ) {		/* every attribute */
		b[k++] = 0x35;
		b[k++] = 5;
		b[k++] = 0x0a;
		b[k++] = 0x00;
		b[k++] = 0x00;
		b[k++] = 0xff;
		b[k++] = 0xff;
	}
	b[k++] = (UB)nc;
	if ( nc > 0 ) knl_memcpy(b + k, cont, nc);
	k += nc;
	b[3] = 0;
	b[4] = (UB)( k - 5 );
	cl_send(c->ours, b, k);
}

LOCAL void sdpc_in( CONST UB *f, INT n )
{
	INT	cnt, at;

	if ( n < 5 || f[1] != (UB)( sdpc_tid >> 8 ) || f[2] != (UB)sdpc_tid ) {
		avi_bad++;
		return;
	}
	if ( f[0] == 0x03 && n >= 10 ) {		/* the handles */
		cnt = ( f[7] << 8 ) | f[8];
		sdpc_handles += cnt;
		at = 9 + 4 * cnt;
	} else if ( f[0] == 0x07 && n >= 8 ) {
		cnt = ( f[5] << 8 ) | f[6];
		if ( 7 + cnt >= n || sdpc_n + cnt > (INT)sizeof(sdpc_got) || cnt > sdpc_max ) {
			avi_bad++;
			return;
		}
		knl_memcpy(sdpc_got + sdpc_n, f + 7, cnt);
		sdpc_n += cnt;
		at = 7 + cnt;
	} else {
		tm_printf((UB *)"  this end's records answered %02x\n", f[0]);
		sdpc_done = TRUE;
		return;
	}
	if ( at < n && f[at] > 0 && at + 1 + f[at] <= n ) {
		sdpc_ask(f + at + 1, f[at]);
		return;
	}
	sdpc_done = TRUE;
}

/* A question to this end's records, answered whole */
LOCAL BOOL sdpc_run( UB pdu, UH uuid, UH max )
{
	INT	i;

	sdpc_pdu = pdu;
	sdpc_uuid = uuid;
	sdpc_max = max;
	sdpc_n = sdpc_asks = sdpc_handles = 0;
	sdpc_done = FALSE;
	sdpc_ask(NULL, 0);
	for ( i = 0; i < 40 && !sdpc_done; i++ ) pump(20);
	return sdpc_done;
}

/* Whether the bytes are among what came */
LOCAL BOOL sdpc_has( CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i + n <= sdpc_n; i++ ) {
		if ( same_bytes(sdpc_got + i, b, n) ) return TRUE;
	}
	return FALSE;
}

/* The records of the list of lists that came */
LOCAL INT sdpc_records( void )
{
	INT	at, n = 0, len;

	if ( sdpc_n < 3 || ( sdpc_got[0] != 0x35 && sdpc_got[0] != 0x36 ) ) return -1;
	at = ( sdpc_got[0] == 0x35 ) ? 2 : 3;
	while ( at < sdpc_n ) {
		if ( sdpc_got[at] == 0x35 ) len = 2 + sdpc_got[at + 1];
		else if ( sdpc_got[at] == 0x36 ) len = 3 + ( ( sdpc_got[at + 1] << 8 ) | sdpc_got[at + 2] );
		else return -1;
		at += len;
		n++;
	}
	return ( at == sdpc_n ) ? n : -1;
}

LOCAL void hfm_sabm0( void );

LOCAL void chan_ready( UB dev )
{
	UB	s1 = 0;

	(void)s1;
	if ( dev == CID_DEV_AVS2 ) avi_cmd(0x01, NULL, 0);
	else if ( dev == CID_DEV_AVM2 ) avi_media_up = TRUE;
	else if ( dev == 0x79 ) hfm_sabm0();
}

LOCAL void avm_in( CONST UB *f, INT n );
LOCAL void hfm_in( CONST UB *f, INT n );
LOCAL void hfm_sabm0( void );
LOCAL void hfm_sdp_in( CONST UB *q, INT n );

LOCAL void dev_frame( UB dev, CONST UB *f, INT n )
{
	if ( dev == CID_DEV_AVS2 ) avi_in(f, n);
	else if ( dev == CID_DEV_SDPC ) sdpc_in(f, n);
	else if ( dev == 0x78 ) avm_in(f, n);
	else if ( dev == 0x79 || dev == 0x7b ) hfm_in(f, n);
	else if ( dev == 0x7c ) hfm_sdp_in(f, n);
}

/* Earphones found and paired; the device's own setting up as the test says */
LOCAL BOOL av_pair( void )
{
	T_BTFOUND	f[4];
	INT		i;

	qh = qt = 0;
	acl_have = 0;
	KT_ASSERT_ER(ts_hid_stat(&av_hid0), E_OK);
	knl_memset(&m, 0, sizeof(m));
	knl_memset(mch, 0, sizeof(mch));
	mock_adv = FALSE;
	av_mode = TRUE;
	mock_cod = 0x240404;
	mock_acl_len = 1021;
	av_discover = av_caps = av_config = av_open = av_start = av_suspend = av_media = av_bad = 0;
	av_frames = av_max_len = 0;
	avi_bad = avi_host_disc = avi_suspend = 0;
	avi_set = avi_media_up = avi_started = avi_caps_sbc = FALSE;
	dev_rd = rd_bitmap;
	dev_rdlen = sizeof(rd_bitmap);
	sdp_make();
	n_start = n_authreq = n_enc = n_lkneg = n_iocap = n_confirm = n_ctrl_set = n_ctrl_report = 0;
	n_sdp_ask = n_sdp_disc = 0;
	saw_conn = FALSE;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
	if ( !knl_bt_ready() ) return FALSE;
	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	pump(300);
	(void)knl_bt_found(f, 4);
	KT_ASSERT_ER(knl_bt_pair(kb_addr), E_OK);
	pump(1500);
	KT_ASSERT_EQ(n_enc, 1);
	return TRUE;
}

LOCAL void av_unpair( void )
{
	bt_end(&av_hid0);
	av_mode = FALSE;
	av_dev_first = FALSE;
	mock_cod = 0x000540;
	mock_acl_len = 64;
	dev_rd = NULL;
	knl_memset(mch, 0, sizeof(mch));
}

/*
 * Earphones that set their stream up themselves, as they do when they
 * connect: this end answers each command, takes their configuration,
 * does not set up a stream of its own beside it, and plays at the pace
 * of the clock when the earphones start the stream.
 */
LOCAL void test_a2dp_dev( void )
{
	SDOut	o[8];
	UW	rate = 0, t0, t1;
	INT	i, n, ch = 0, at = -1, due;
	UB	s1 = 1 << 2;

	av_dev_first = TRUE;
	if ( av_pair() ) {
		pump(1000);			/* past the time this end waits for the device */
		KT_ASSERT(avi_set);
		KT_ASSERT(avi_caps_sbc);
		KT_ASSERT(avi_media_up);
		KT_ASSERT_EQ(avi_bad, 0);
		KT_ASSERT_EQ(avi_host_disc, 0);
		KT_ASSERT_EQ(av_discover, 0);
		avi_cmd(0x07, &s1, 1);		/* the earphones start it: a button pressed */
		pump(100);
		KT_ASSERT(avi_started);
		n = knl_snd_outputs(o, 8);
		for ( i = 0; i < n && i < 8; i++ ) {
			if ( o[i].kind == SND_BE_BT ) at = i;
		}
		KT_ASSERT(at >= 0);
		if ( at >= 0 ) {
			t0 = knl_usb_ms();
			knl_usb_lock();
			KT_ASSERT_ER(knl_bta_hw.play_start(o[at].id, &rate, &ch), E_OK);
			knl_usb_unlock();
			KT_ASSERT_EQ(rate, 44100);
			for ( i = 0; i < 100; i++ ) {
				knl_usb_lock();
				knl_bta_hw.poll();
				knl_usb_unlock();
				pump(10);
			}
			t1 = knl_usb_ms();
			/* what is due from the start, a packet and what the start waited over */
			due = (INT)( ( (UD)( t1 - t0 + 100 ) * 44100 ) / ( 1000 * 128 ) ) + 16;
			tm_printf((UB *)"  %d packets, %d frames in %d ms; %d due at most\n", av_media, av_frames, (INT)( t1 - t0 ), due);
			KT_ASSERT(av_media >= 10);
			KT_ASSERT(av_frames <= due);
			KT_ASSERT_EQ(av_bad, 0);
			knl_usb_lock();
			knl_bta_hw.play_stop();
			knl_usb_unlock();
			pump(200);
			KT_ASSERT_EQ(avi_suspend, 1);
		}
		av_unpair();
	}
}

/* This end's service records, as a device asks them: whole, and in pieces */
LOCAL void test_sdp_serve( void )
{
	static CONST UB	src[] = { 0x19, 0x11, 0x0a };
	static CONST UB	avdtp[] = { 0x09, 0x00, 0x19 };
	static CONST UB	a2dp[] = { 0x19, 0x11, 0x0d };
	static CONST UB	tg[] = { 0x19, 0x11, 0x0c };
	static CONST UB	ct[] = { 0x19, 0x11, 0x0f };
	static CONST UB	avctp[] = { 0x09, 0x00, 0x17 };
	INT		i;

	if ( av_pair() ) {
		dev_open(0x0001, CID_DEV_SDPC);
		for ( i = 0; i < 20 && ( mch_of(CID_DEV_SDPC)->up != 3 ); i++ ) pump(20);
		KT_ASSERT_EQ(mch_of(CID_DEV_SDPC)->res, 0);
		KT_ASSERT_EQ(mch_of(CID_DEV_SDPC)->up, 3);
		/* the source of A2DP, every attribute, in pieces of 48 bytes */
		KT_ASSERT(sdpc_run(0x06, 0x110a, 48));
		tm_printf((UB *)"  A2DP: %d bytes in %d answers\n", sdpc_n, sdpc_asks);
		KT_ASSERT(sdpc_asks > 1);
		KT_ASSERT_EQ(sdpc_records(), 1);
		KT_ASSERT(sdpc_has(src, 3));
		KT_ASSERT(sdpc_has(avdtp, 3));
		KT_ASSERT(sdpc_has(a2dp, 3));
		/* AVRCP: the target and the controller, both of the remote control's class */
		KT_ASSERT(sdpc_run(0x06, 0x110e, 600));
		KT_ASSERT_EQ(sdpc_asks, 1);
		KT_ASSERT_EQ(sdpc_records(), 2);
		KT_ASSERT(sdpc_has(tg, 3));
		KT_ASSERT(sdpc_has(ct, 3));
		KT_ASSERT(sdpc_has(avctp, 3));
		/* the handles alone; none for a service this end does not have */
		KT_ASSERT(sdpc_run(0x02, 0x110a, 8));
		KT_ASSERT_EQ(sdpc_handles, 1);
		KT_ASSERT(sdpc_run(0x02, 0x1124, 8));
		KT_ASSERT_EQ(sdpc_handles, 0);
		KT_ASSERT_EQ(avi_bad, 0);
		av_unpair();
	}
}

/* ---------------------------------------------------------------- LE Audio */

/*
 * A controller that can be the central of CISes, and LE Audio earphones:
 * PACS, ASCS with two sink ASEs (left and right), CSIS (one of a set of
 * two, its SIRK encrypted), VCS. The other of the set advertises its set
 * identifier and refuses the connection.
 */
LOCAL CONST UB	lea_addr[6] = { 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6 };		/* random, static */
LOCAL CONST UB	lea_mate[6] = { 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6 };		/* the other of the set */
LOCAL CONST UB	lea_sirk[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
LOCAL CONST UB	lea_prand[3] = { 0x11, 0x22, 0x45 };	/* its two top bits 0 and 1, as an RSI's random part has */

#define LA_PAC		0x22
#define LA_PAC_CCC	0x23
#define LA_LOC		0x25
#define LA_ASE1		0x32
#define LA_ASE1_CCC	0x33
#define LA_ASE2		0x35
#define LA_ASE2_CCC	0x36
#define LA_CP		0x38
#define LA_CP_CCC	0x39
#define LA_SIRK		0x42
#define LA_SIZE		0x44
#define LA_RANK		0x46
#define LA_VOL		0x52
#define LA_VOL_CCC	0x53
#define LA_VOL_CP	0x55
#define LA_FLAGS	0x57
#define LA_CIS0		0x60		/* the handles the controller gives the CISes */
#define LA_SDU_KEEP	8

typedef struct {
	UB	state, cig, cis;
	UB	freq;
	UW	alloc;
	UH	oct;
	UW	interval, pd;
	UH	max_sdu;
	UB	framing, phy;
} LAASE;

LOCAL struct {
	INT	n_feat, n_bufv2, n_host, n_ext_scan, n_ext_conn, n_legacy_scan, n_mate_conn, n_mate_adv;
	INT	n_cig, cig_cis, n_cis, n_path, n_remove, n_cis_drop;
	INT	n_ccc, n_cp, n_codec, n_qos, n_enable, n_disable, n_vol;
	BOOL	scan_active;
	UW	cig_interval;
	UH	cig_sdu[2];
	UB	lemask[8];
	LAASE	ase[2];
	UB	vol, vol_count;
	INT	sdus[2], iso_bad;
	UH	last_seq[2];
	INT	nkeep;
	UB	keep[LA_SDU_KEEP][100];
} la;

LOCAL void la_put24( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
}

LOCAL UW la_get24( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 );
}

/* What it advertises: its name, ASCS's announcement, and its set identifier (or the other's) */
LOCAL INT la_ad( UB *ad, BOOL rsi )
{
	static CONST UB	head[] = {
		0x02, 0x01, 0x06,
		0x07, 0x09, 'L', 'E', 'b', 'u', 'd', 's',
		0x09, 0x16, 0x4e, 0x18, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00
	};
	INT		n = sizeof(head);

	knl_memcpy(ad, head, n);
	if ( rsi ) {
		ad[n++] = 7;
		ad[n++] = 0x2e;
		knl_bts_ah(lea_sirk, lea_prand, ad + n);
		knl_memcpy(ad + n + 3, lea_prand, 3);
		n += 6;
	}
	return n;
}

/* An extended advertising report, not of a legacy PDU */
LOCAL void la_ext_adv( CONST UB *peer, UH et, CONST UB *ad, INT n )
{
	UB	e[28 + 64];

	knl_memset(e, 0, sizeof(e));
	e[0] = 0x3e;
	e[1] = (UB)( 2 + 24 + n );
	e[2] = 0x0d;
	e[3] = 1;
	e[4] = (UB)et;
	e[5] = (UB)( et >> 8 );
	e[6] = 1;
	knl_memcpy(e + 7, peer, 6);
	e[13] = 1;
	e[14] = 1;
	e[16] = 0x7f;
	e[17] = 0xd0;
	e[27] = (UB)n;
	knl_memcpy(e + 28, ad, n);
	ev(e, 28 + n);
}

LOCAL void la_note( UH h, CONST UB *v, INT n )
{
	UB	b[64];

	b[0] = 0x1b;
	put16(b + 1, h);
	knl_memcpy(b + 3, v, n);
	le_send(4, b, n + 3);
}

/* An ASE's value, and its notification */
LOCAL INT la_ase_val( INT k, UB *v )
{
	LAASE	*a = &la.ase[k];
	INT	n = 2;

	v[0] = (UB)( k + 1 );
	v[1] = a->state;
	switch ( a->state ) {
	case 1:
		v[2] = 0;			/* unframed PDUs taken */
		v[3] = 0x02;			/* 2M preferred */
		v[4] = 2;
		put16(v + 5, 10);
		la_put24(v + 7, 10000);
		la_put24(v + 10, 40000);
		la_put24(v + 13, 40000);
		la_put24(v + 16, 40000);
		v[19] = 0x06;
		knl_memset(v + 20, 0, 4);
		v[24] = 0;
		n = 25;
		break;
	case 2:
		v[2] = a->cig;
		v[3] = a->cis;
		la_put24(v + 4, a->interval);
		v[7] = a->framing;
		v[8] = a->phy;
		put16(v + 9, a->max_sdu);
		v[11] = 2;
		put16(v + 12, 10);
		la_put24(v + 14, a->pd);
		n = 17;
		break;
	case 3: case 4:
		v[2] = a->cig;
		v[3] = a->cis;
		v[4] = 0;
		n = 5;
		break;
	default:
		break;
	}
	return n;
}

LOCAL void la_ase_note( INT k )
{
	UB	v[32];
	INT	n = la_ase_val(k, v);

	la_note(( k == 0 ) ? LA_ASE1 : LA_ASE2, v, n);
}

/* An operation on the ASE control point: its answer, then the ASE's new state */
LOCAL void la_cp( CONST UB *v, INT n )
{
	UB	r[5];
	INT	k;

	la.n_cp++;
	if ( n < 3 || v[1] != 1 || v[2] < 1 || v[2] > 2 ) return;
	k = v[2] - 1;
	r[0] = v[0];
	r[1] = 1;
	r[2] = v[2];
	r[3] = r[4] = 0;
	la_note(LA_CP, r, 5);
	switch ( v[0] ) {
	case 0x01: {
		/* ASE_ID, target latency and PHY, the codec's id (its coding format first), the length, the configuration */
		INT	i = 0, cl = v[10];
		CONST UB *cf = v + 11;

		la.n_codec++;
		KT_ASSERT_EQ(v[5], 0x06);
		while ( i < cl && i + 1 + cf[i] <= cl ) {
			if ( cf[i + 1] == 0x01 ) la.ase[k].freq = cf[i + 2];
			if ( cf[i + 1] == 0x03 ) la.ase[k].alloc = (UW)cf[i + 2] | ( (UW)cf[i + 3] << 8 ) | ( (UW)cf[i + 4] << 16 ) | ( (UW)cf[i + 5] << 24 );
			if ( cf[i + 1] == 0x04 ) la.ase[k].oct = (UH)( cf[i + 2] | ( cf[i + 3] << 8 ) );
			i += 1 + cf[i];
		}
		la.ase[k].state = 1;
		break;
	}
	case 0x02:
		la.n_qos++;
		KT_ASSERT_EQ(n, 18);
		la.ase[k].cig = v[3];
		la.ase[k].cis = v[4];
		la.ase[k].interval = la_get24(v + 5);
		la.ase[k].framing = v[8];
		la.ase[k].phy = v[9];
		la.ase[k].max_sdu = (UH)( v[10] | ( v[11] << 8 ) );
		la.ase[k].pd = la_get24(v + 15);
		la.ase[k].state = 2;
		break;
	case 0x03:
		la.n_enable++;
		KT_ASSERT(n >= 8 && v[3] == 4 && v[5] == 0x02 && v[6] == 0x04);	/* for media */
		la.ase[k].state = 3;
		break;
	case 0x05:
		la.n_disable++;
		la.ase[k].state = 2;	/* a sink ASE goes straight back */
		break;
	default:
		return;
	}
	la_ase_note(k);
}

/* The earphones' ATT: their services, as a server of MTU 100 says them */
LOCAL void la_att( CONST UB *f, INT n )
{
	static CONST struct { UH first, last, uuid; } svcs[] = {
		{ 0x01, 0x0f, 0x1800 }, { 0x20, 0x25, 0x1850 }, { 0x30, 0x39, 0x184e }, { 0x40, 0x46, 0x1846 }, { 0x50, 0x57, 0x1844 }
	};
	static CONST struct { UH decl; UB props; UH val, uuid; } chars[] = {
		{ 0x21, 0x12, LA_PAC, 0x2bc9 }, { 0x24, 0x02, LA_LOC, 0x2bca },
		{ 0x31, 0x12, LA_ASE1, 0x2bc4 }, { 0x34, 0x12, LA_ASE2, 0x2bc4 }, { 0x37, 0x1c, LA_CP, 0x2bc6 },
		{ 0x41, 0x02, LA_SIRK, 0x2b84 }, { 0x43, 0x02, LA_SIZE, 0x2b85 }, { 0x45, 0x02, LA_RANK, 0x2b87 },
		{ 0x51, 0x12, LA_VOL, 0x2b7d }, { 0x54, 0x08, LA_VOL_CP, 0x2b7e }, { 0x56, 0x02, LA_FLAGS, 0x2b7f }
	};
	static CONST UH	cccds[] = { LA_PAC_CCC, LA_ASE1_CCC, LA_ASE2_CCC, LA_CP_CCC, LA_VOL_CCC };
	static CONST UB	pac[] = {
		1, 0x06, 0, 0, 0, 0, 19,
		3, 0x01, 0x84, 0x00,		/* 16 and 48 kHz */
		2, 0x02, 0x02,			/* 10 ms */
		2, 0x03, 0x01,			/* one channel */
		5, 0x04, 40, 0, 155, 0,		/* 40 to 155 bytes */
		2, 0x05, 1,
		0
	};
	UB	b[104];
	UH	first = 0, last = 0, h;
	INT	i, k = 2;

	if ( n >= 5 ) {
		first = (UH)( f[1] | ( f[2] << 8 ) );
		last = (UH)( f[3] | ( f[4] << 8 ) );
	}
	h = (UH)( ( n >= 3 ) ? ( f[1] | ( f[2] << 8 ) ) : 0 );
	switch ( f[0] ) {
	case 0x02:
		b[0] = 0x03;
		put16(b + 1, 100);
		le_send(4, b, 3);
		break;
	case 0x10:
		b[0] = 0x11;
		b[1] = 6;
		for ( i = 0; i < (INT)( sizeof(svcs) / sizeof(svcs[0]) ); i++ ) {
			if ( svcs[i].first < first ) continue;
			put16(b + k, svcs[i].first);
			put16(b + k + 2, svcs[i].last);
			put16(b + k + 4, svcs[i].uuid);
			k += 6;
		}
		if ( k == 2 ) att_err(0x10, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x08:
		b[0] = 0x09;
		b[1] = 7;
		for ( i = 0; i < (INT)( sizeof(chars) / sizeof(chars[0]) ); i++ ) {
			if ( chars[i].decl < first || chars[i].decl > last ) continue;
			put16(b + k, chars[i].decl);
			b[k + 2] = chars[i].props;
			put16(b + k + 3, chars[i].val);
			put16(b + k + 5, chars[i].uuid);
			k += 7;
		}
		if ( k == 2 ) att_err(0x08, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x04:
		b[0] = 0x05;
		b[1] = 1;
		for ( i = 0; i < (INT)( sizeof(cccds) / sizeof(cccds[0]) ); i++ ) {
			if ( cccds[i] < first || cccds[i] > last ) continue;
			put16(b + k, cccds[i]);
			put16(b + k + 2, 0x2902);
			k += 4;
		}
		if ( k == 2 ) att_err(0x04, first, 0x0a);
		else le_send(4, b, k);
		break;
	case 0x0a:
		b[0] = 0x0b;
		k = 1;
		switch ( h ) {
		case LA_PAC:	knl_memcpy(b + 1, pac, sizeof(pac)); k += sizeof(pac); break;
		case LA_LOC:	b[1] = 0x03; b[2] = b[3] = b[4] = 0; k += 4; break;
		case LA_ASE1:	k += la_ase_val(0, b + 1); break;
		case LA_ASE2:	k += la_ase_val(1, b + 1); break;
		case LA_SIRK:
			/* encrypted with the LTK of the link, as the server does on LE */
			b[1] = 0x00;
			knl_btbap_sef(m.want_key, lea_sirk, b + 2);
			k += 17;
			break;
		case LA_SIZE:	b[1] = 2; k++; break;
		case LA_RANK:	b[1] = 1; k++; break;
		case LA_VOL:	b[1] = la.vol; b[2] = 0; b[3] = la.vol_count; k += 3; break;
		case LA_FLAGS:	b[1] = 0; k++; break;
		default:	att_err(0x0a, h, 0x01); return;
		}
		le_send(4, b, k);
		break;
	case 0x12:
		if ( h == LA_VOL_CP ) {
			KT_ASSERT(n >= 6 && f[3] == 0x04);
			if ( f[4] != la.vol_count ) {
				att_err(0x12, h, 0x80);
				break;
			}
			la.vol = f[5];
			la.vol_count++;
			la.n_vol++;
		} else {
			KT_ASSERT_EQ(f[3], 1);
			la.n_ccc++;
		}
		b[0] = 0x13;
		le_send(4, b, 1);
		if ( h == LA_VOL_CP ) {
			b[0] = la.vol;
			b[1] = 0;
			b[2] = la.vol_count;
			la_note(LA_VOL, b, 3);
		}
		break;
	case 0x52:
		if ( h == LA_CP ) la_cp(f + 3, n - 3);
		break;
	default:
		break;
	}
}

/* The controller's side of the CIG, its CISes and the extended scan */
LOCAL BOOL lea_cmd( UH op, CONST UB *p )
{
	UB	e[40];
	INT	i;

	switch ( op ) {
	case 0x2001:
		knl_memcpy(la.lemask, p, 8);
		return FALSE;
	case 0x2003: {
		/* encryption; and extended advertising (bit 12) and CIS central (bit 28) of the chip that has them */
		UB	f[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };

		la.n_feat++;
		if ( lea_chip ) {
			f[1] = 0x10;
			f[3] = 0x10;
		}
		cmd_done(op, f, 8);
		return TRUE;
	}
	case 0x2060: {
		static CONST UB	b2[6] = { 27, 0, 4, 251, 0, 8 };

		la.n_bufv2++;
		cmd_done(op, b2, 6);
		return TRUE;
	}
	case 0x2074:
		la.n_host++;
		KT_ASSERT(p[0] == 32 && p[1] == 1);
		cmd_done(op, NULL, 0);
		return TRUE;
	case 0x200c:
		if ( !lea_dev ) return FALSE;
		la.n_legacy_scan++;
		cmd_done(op, NULL, 0);
		if ( p[0] == 1 && m.n_scan_active > 0 && !m.connected ) {
			UB	ad[31];

			le_adv(lea_addr, 0x00, ad, la_ad(ad, FALSE));
		}
		return TRUE;
	case 0x2041:
		KT_ASSERT_EQ(p[2], 0x01);
		la.scan_active = (BOOL)( p[3] == 1 );
		if ( p[3] == 1 ) m.n_scan_active++;
		else m.n_scan_passive++;
		cmd_done(op, NULL, 0);
		return TRUE;
	case 0x2042:
		la.n_ext_scan++;
		cmd_done(op, NULL, 0);
		if ( p[0] == 1 && lea_dev && la.scan_active && !m.connected ) {
			UB	ad[40];

			la_ext_adv(lea_addr, 0x0001, ad, la_ad(ad, TRUE));
		} else if ( p[0] == 1 && lea_dev && m.connected && la.n_mate_adv < 2 ) {
			UB	ad[40];

			la.n_mate_adv++;
			la_ext_adv(lea_mate, 0x0001, ad, la_ad(ad, TRUE));
		}
		return TRUE;
	case 0x2043:
		la.n_ext_conn++;
		cmd_status(op);
		if ( same_bytes(p + 3, lea_mate, 6) ) {
			/* the other of the set: not this time */
			la.n_mate_conn++;
			knl_memset(e, 0, 21);
			e[0] = 0x3e;
			e[1] = 19;
			e[2] = 0x01;
			e[3] = 0x3e;
			e[7] = p[2];
			knl_memcpy(e + 8, p + 3, 6);
			ev(e, 21);
			return TRUE;
		}
		m.peer_type = p[2];
		knl_memcpy(m.peer_addr, p + 3, 6);
		m.connected = TRUE;
		m.n_conn++;
		le_meta_conn(p + 3, p[2]);
		return TRUE;
	case 0x2062:
		la.n_cig++;
		la.cig_interval = la_get24(p + 1);
		la.cig_cis = p[14];
		e[0] = p[0];
		e[1] = p[14];
		for ( i = 0; i < p[14] && i < 2; i++ ) {
			la.cig_sdu[i] = (UH)( p[15 + 9 * i + 1] | ( p[15 + 9 * i + 2] << 8 ) );
			put16(e + 2 + 2 * i, (UH)( LA_CIS0 + p[15 + 9 * i] ));
		}
		cmd_done(op, e, 2 + 2 * i);
		return TRUE;
	case 0x2064:
		la.n_cis++;
		cmd_status(op);
		for ( i = 0; i < p[0] && i < 2; i++ ) {
			UH	h = (UH)( p[1 + 4 * i] | ( p[2 + 4 * i] << 8 ) );
			INT	k;

			KT_ASSERT_EQ(p[3 + 4 * i], H_LE);
			knl_memset(e, 0, sizeof(e));
			e[0] = 0x3e;
			e[1] = 29;
			e[2] = 0x19;
			put16(e + 4, h);
			e[18] = e[19] = 0x02;
			e[20] = 3;
			e[21] = 1;
			e[23] = e[24] = 1;
			put16(e + 25, 100);
			put16(e + 29, 8);
			ev(e, 31);
			/* the server, the receiver of a sink ASE, starts it once its CIS is there */
			for ( k = 0; k < 2; k++ ) {
				if ( la.ase[k].state == 3 && LA_CIS0 + la.ase[k].cis == h ) {
					la.ase[k].state = 4;
					la_ase_note(k);
				}
			}
		}
		return TRUE;
	case 0x206e:
		la.n_path++;
		KT_ASSERT(p[2] == 0 && p[3] == 0 && p[4] == 0x03);
		cmd_done(op, p, 2);
		return TRUE;
	case 0x2065:
		la.n_remove++;
		cmd_done(op, p, 1);
		return TRUE;
	case 0x0406: {
		UH	h = (UH)( p[0] | ( p[1] << 8 ) );

		if ( h != LA_CIS0 && h != LA_CIS0 + 1 ) return FALSE;
		la.n_cis_drop++;
		cmd_status(op);
		e[0] = 0x05;
		e[1] = 4;
		e[2] = 0;
		put16(e + 3, h);
		e[5] = p[2];
		ev(e, 6);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

/* An ISO data packet: an SDU whole, its sequence in order; the room back */
LOCAL BOOL lea_iso_in( CONST UB *b, INT n )
{
	UH	h = (UH)( b[0] | ( ( b[1] & 0x0f ) << 8 ) );
	INT	k = h - LA_CIS0, pb = ( b[1] >> 4 ) & 3, load, slen;
	UH	seq;
	UB	e[7];

	if ( n < 8 || ( h != LA_CIS0 && h != LA_CIS0 + 1 ) ) return FALSE;
	e[0] = 0x13;
	e[1] = 5;
	e[2] = 1;
	put16(e + 3, h);
	e[5] = 1;
	e[6] = 0;
	ev(e, 7);
	load = b[2] | ( ( b[3] & 0x3f ) << 8 );
	seq = (UH)( b[4] | ( b[5] << 8 ) );
	slen = b[6] | ( ( b[7] & 0x0f ) << 8 );
	if ( pb != 2 || ( b[1] & 0x40 ) != 0 || n != load + 4 || slen + 4 != load || slen != 100 ) la.iso_bad++;
	if ( la.sdus[k] > 0 && seq != (UH)( la.last_seq[k] + 1 ) ) la.iso_bad++;
	la.last_seq[k] = seq;
	la.sdus[k]++;
	if ( k == 0 && la.nkeep < LA_SDU_KEEP && slen == 100 ) knl_memcpy(la.keep[la.nkeep++], b + 8, 100);
	return TRUE;
}

/* ---- LC3 itself: encoded and decoded back, in a task that may use the floating point registers */

#define KT_LC3_FRAMES	12
#define KT_LC3_NS	480
LOCAL lc3_encoder_mem_48k_t	kt_enc_mem;
LOCAL lc3_decoder_mem_48k_t	kt_dec_mem;
LOCAL short	kt_in[KT_LC3_FRAMES * KT_LC3_NS], kt_out[KT_LC3_FRAMES * KT_LC3_NS];
LOCAL UB	kt_frame[100];
LOCAL INT	kt_lc3_bad, kt_sdu_bad, kt_delay;
LOCAL ID	kt_lc3_caller;

LOCAL void kt_lc3_task( INT stacd, void *exinf )
{
	lc3_encoder_t	e;
	lc3_decoder_t	d;
	INT		f;

	(void)stacd;
	(void)exinf;
	e = lc3_setup_encoder(10000, 48000, 0, &kt_enc_mem);
	d = lc3_setup_decoder(10000, 48000, 0, &kt_dec_mem);
	kt_delay = lc3_delay_samples(10000, 48000);
	for ( f = 0; f < KT_LC3_FRAMES; f++ ) {
		if ( lc3_encode(e, LC3_PCM_FORMAT_S16, kt_in + f * KT_LC3_NS, 1, 100, kt_frame) != 0 ) kt_lc3_bad++;
		if ( lc3_decode(d, kt_frame, 100, LC3_PCM_FORMAT_S16, kt_out + f * KT_LC3_NS, 1) != 0 ) kt_lc3_bad++;
	}
	/* what came on the CIS decodes as LC3 frames of 48 kHz */
	d = lc3_setup_decoder(10000, 48000, 0, &kt_dec_mem);
	for ( f = 0; f < la.nkeep; f++ ) {
		if ( lc3_decode(d, la.keep[f], 100, LC3_PCM_FORMAT_S16, kt_out, 1) != 0 ) kt_sdu_bad++;
	}
	(void)tk_wup_tsk(kt_lc3_caller);
	tk_exd_tsk();
}

/*
 * A triangle of 1 kHz at 48 kHz through the codec at 80 kbit/s: what
 * comes back, after the codec's delay, is near what went in (the error's
 * energy under a thirtieth of the signal's). The SDUs the streams carried
 * decode too.
 */
LOCAL void la_lc3_check( void )
{
	T_CTSK	ct;
	ID	tid;
	INT	i;
	UD	sig = 0, err = 0;

	for ( i = 0; i < KT_LC3_FRAMES * KT_LC3_NS; i++ ) {
		INT	ph = i % 48;

		kt_in[i] = (short)( ( ph < 24 ) ? -8000 + ( 16000 * ph ) / 24 : 8000 - ( 16000 * ( ph - 24 ) ) / 24 );
	}
	kt_lc3_bad = kt_sdu_bad = 0;
	kt_lc3_caller = tk_get_tid();
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)kt_lc3_task;
	ct.itskpri = 20;
	ct.stksz = 32 * 1024;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) return;
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	KT_ASSERT_ER(tk_slp_tsk(60000), E_OK);
	tk_dly_tsk(10);
	for ( i = 2 * KT_LC3_NS; i < KT_LC3_FRAMES * KT_LC3_NS; i++ ) {
		W	x = kt_in[i - kt_delay], dd = kt_out[i] - x;

		sig += (UD)( x * x );
		err += (UD)( dd * dd );
	}
	tm_printf((UB *)"  LC3: delay %d samples, signal to error %d to 1; %d SDUs of the CIS decoded\n",
		  kt_delay, (INT)( ( err > 0 ) ? sig / err : 0 ), la.nkeep);
	KT_ASSERT_EQ(kt_lc3_bad, 0);
	KT_ASSERT(err * 30 < sig);
	KT_ASSERT_EQ(kt_sdu_bad, 0);
}

/* The functions of CSIS against the sample data of the specification */
LOCAL void test_lea_crypto( void )
{
	/* written the lowest byte first */
	static CONST UB	sirk[16] = { 0xcd, 0xcc, 0x72, 0xdd, 0x86, 0x8c, 0xcd, 0xce, 0x22, 0xfd, 0xa1, 0x21, 0x09, 0x7d, 0x7d, 0x45 };
	static CONST UB	k[16] = { 0xd9, 0xce, 0xe5, 0x3c, 0x22, 0xc6, 0x1e, 0x06, 0x6f, 0x69, 0x48, 0xd4, 0x9b, 0x1b, 0x6e, 0x67 };
	static CONST UB	enc[16] = { 0x46, 0xd3, 0x5f, 0xf2, 0xd5, 0x62, 0x25, 0x7e, 0xa0, 0x24, 0x35, 0xe1, 0x35, 0x38, 0x0a, 0x17 };
	static CONST UB	r[3] = { 0x63, 0xf5, 0x69 };
	static CONST UB	hash[3] = { 0xda, 0x48, 0x19 };
	UB		o[16], back[16];

	knl_btbap_sef(k, sirk, o);
	KT_ASSERT(same_bytes(o, enc, 16));
	knl_btbap_sef(k, o, back);
	KT_ASSERT(same_bytes(back, sirk, 16));
	knl_bts_ah(sirk, r, o);			/* sih is ah with the SIRK */
	KT_ASSERT(same_bytes(o, hash, 3));
}

LOCAL void la_start( BOOL chip )
{
	INT	i;

	qh = qt = 0;
	acl_have = 0;
	knl_memset(&m, 0, sizeof(m));
	knl_memset(&la, 0, sizeof(la));
	m.sc = TRUE;
	mock_adv = FALSE;
	dev_rd = NULL;
	lea_chip = chip;
	lea_dev = TRUE;
	la.vol = 0x80;
	la.vol_count = 5;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT(knl_bt_ready());
}

LOCAL void la_end( void )
{
	T_HIDSTAT	s0;

	if ( m.connected ) le_gone();
	pump(300);
	KT_ASSERT_ER(ts_hid_stat(&s0), E_OK);
	bt_end(&s0);
	lea_chip = lea_dev = FALSE;
}

LOCAL INT la_find( void )
{
	T_BTFOUND	f[4];
	INT		i, n, at = -1;

	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	pump(300);
	n = knl_bt_found(f, 4);
	for ( i = 0; i < n && i < 4; i++ ) {
		if ( f[i].le != 0 && same_bytes(f[i].addr, lea_addr, 6) ) at = i;
	}
	KT_ASSERT(at >= 0);
	if ( at >= 0 ) KT_ASSERT_EQ(f[at].name[0], 'L');
	return at;
}

/*
 * The earphones on a controller of CISes: found by the extended scan,
 * paired, their services read, two streams of 48 kHz configured and a
 * CIG of two CISes; played: enabled, the CISes made, their data paths, and
 * LC3 SDUs on each every 10 ms; the volume; stopped. The other of the set
 * is known by its set identifier, the SIRK having been decrypted.
 */
LOCAL void test_lea_stream( void )
{
	SDOut	o[8];
	CONST UB *id = NULL;
	UW	rate = 0;
	INT	i, n, ch = 0, at = -1;
	SDVol	v;

	la_start(TRUE);
	KT_ASSERT_EQ(la.n_feat, 1);
	KT_ASSERT_EQ(la.n_bufv2, 1);
	KT_ASSERT_EQ(la.n_host, 1);
	KT_ASSERT(( la.lemask[1] & 0x10 ) != 0 && ( la.lemask[3] & 0x01 ) != 0);
	(void)la_find();
	KT_ASSERT(la.n_ext_scan >= 1);
	KT_ASSERT_EQ(la.n_legacy_scan, 0);
	KT_ASSERT_ER(knl_bt_pair(lea_addr), E_OK);
	for ( i = 0; i < 120 && ( la.ase[0].state != 2 || la.ase[1].state != 2 ); i++ ) pump(50);
	pump(200);
	KT_ASSERT_EQ(la.n_ext_conn - la.n_mate_conn, 1);
	KT_ASSERT_EQ(m.n_req, 1);
	KT_ASSERT(m.enc_ok);
	KT_ASSERT_EQ(la.n_ccc, 5);
	KT_ASSERT_EQ(la.n_codec, 2);
	for ( i = 0; i < 2; i++ ) {
		KT_ASSERT_EQ(la.ase[i].freq, 0x08);		/* 48 kHz */
		KT_ASSERT_EQ(la.ase[i].oct, 100);
		KT_ASSERT_EQ(la.ase[i].state, 2);
		KT_ASSERT_EQ(la.ase[i].interval, 10000);
		KT_ASSERT_EQ(la.ase[i].max_sdu, 100);
		KT_ASSERT_EQ(la.ase[i].pd, 40000);
		KT_ASSERT_EQ(la.ase[i].phy, 0x02);
		KT_ASSERT_EQ(la.ase[i].cis, i);
	}
	KT_ASSERT_EQ(la.ase[0].alloc, 1);
	KT_ASSERT_EQ(la.ase[1].alloc, 2);
	KT_ASSERT_EQ(la.n_cig, 1);
	KT_ASSERT_EQ(la.cig_cis, 2);
	KT_ASSERT_EQ(la.cig_interval, 10000);
	KT_ASSERT(la.cig_sdu[0] == 100 && la.cig_sdu[1] == 100);
	/* the other earphone: its set identifier resolved, a connection tried */
	KT_ASSERT(la.n_mate_conn >= 1);

	n = knl_snd_outputs(o, 8);
	for ( i = 0; i < n && i < 8; i++ ) {
		if ( o[i].kind == SND_BE_BT && o[i].id[0] == 'l' && o[i].id[1] == 'e' ) at = i;
	}
	KT_ASSERT(at >= 0);
	if ( at >= 0 ) {
		id = o[at].id;
		tm_printf((UB *)"  output %s \"%s\"\n", id, o[at].name);
		knl_usb_lock();
		KT_ASSERT_ER(knl_lea_hw.play_start(id, &rate, &ch), E_OK);
		knl_usb_unlock();
		KT_ASSERT_EQ(rate, 48000);
		KT_ASSERT_EQ(ch, 2);
		for ( i = 0; i < 40 && ( la.ase[0].state != 4 || la.ase[1].state != 4 || la.n_path < 2 ); i++ ) pump(20);
		KT_ASSERT_EQ(la.n_enable, 2);
		KT_ASSERT_EQ(la.n_cis, 1);
		KT_ASSERT_EQ(la.n_path, 2);
		for ( i = 0; i < 100; i++ ) {
			knl_usb_lock();
			knl_lea_hw.poll();
			knl_usb_unlock();
			pump(10);
		}
		tm_printf((UB *)"  %d and %d SDUs on the two CISes in about a second\n", la.sdus[0], la.sdus[1]);
		KT_ASSERT(la.sdus[0] >= 20 && la.sdus[1] >= 20);
		KT_ASSERT(la.sdus[0] - la.sdus[1] <= 1 && la.sdus[1] - la.sdus[0] <= 1);
		KT_ASSERT_EQ(la.iso_bad, 0);
		/* the volume, by VCP */
		v.lv = v.rv = 0x4000;
		knl_usb_lock();
		KT_ASSERT_ER(knl_lea_hw.set_level(DN_SDVOLMAIN, &v), E_OK);
		knl_usb_unlock();
		pump(100);
		KT_ASSERT_EQ(la.n_vol, 1);
		KT_ASSERT_EQ(la.vol, 0x40);
		knl_usb_lock();
		knl_lea_hw.play_stop();
		knl_usb_unlock();
		pump(200);
		KT_ASSERT_EQ(la.n_disable, 2);
		KT_ASSERT_EQ(la.n_cis_drop, 2);
		KT_ASSERT(la.ase[0].state == 2 && la.ase[1].state == 2);
	}
	la_lc3_check();
	la_end();
}

/* A controller without CISes: the same earphones by the legacy scan, and nothing of LE Audio asked */
LOCAL void test_lea_nocis( void )
{
	la_start(FALSE);
	KT_ASSERT_EQ(la.n_feat, 1);
	KT_ASSERT_EQ(la.n_bufv2, 0);
	KT_ASSERT_EQ(la.n_host, 0);
	KT_ASSERT(( la.lemask[1] & 0x10 ) == 0 && ( la.lemask[3] & 0x01 ) == 0);
	(void)la_find();
	KT_ASSERT_EQ(la.n_ext_scan, 0);
	KT_ASSERT(la.n_legacy_scan >= 1);
	KT_ASSERT_ER(knl_bt_pair(lea_addr), E_OK);
	pump(2000);
	KT_ASSERT_EQ(m.n_conn, 1);
	KT_ASSERT(m.enc_ok);
	KT_ASSERT_EQ(la.n_ccc, 0);
	KT_ASSERT_EQ(la.n_cp, 0);
	KT_ASSERT_EQ(la.n_cig, 0);
	la_end();
}

/* ---------------------------------------------------------------- the remote control */

#define CID_DEV_AVC	0x78		/* AVCTP of the device's opening */
#define CID_DEV_RFC	0x79		/* RFCOMM of the device's opening */
#define CID_DEV_RFC2	0x7b		/* and of this end's */
#define CID_DEV_SDPHF	0x7c		/* this end asking the device's hands-free service */

LOCAL INT	avm_caps, avm_reg, avm_vol, avm_vol_n, avm_bad;
LOCAL UB	avm_reg_tl, avm_rs, avm_tl;
LOCAL BOOL	avm_unit;

LOCAL void avm_send( UB tl, BOOL rsp, CONST UB *avc, INT n )
{
	UB	b[3 + 32];
	MCH	*c = mch_of(CID_DEV_AVC);

	if ( c == NULL || n > 32 ) return;
	b[0] = (UB)( ( tl << 4 ) | ( rsp ? 2 : 0 ) );
	b[1] = 0x11;
	b[2] = 0x0e;
	knl_memcpy(b + 3, avc, n);
	cl_send(c->ours, b, n + 3);
}

/* An answer of AVRCP's own from the earphones */
LOCAL void avm_vendor( UB tl, UB rs, UB pdu, CONST UB *prm, INT n )
{
	UB	d[10 + 16];

	d[0] = rs;
	d[1] = 0x48;
	d[2] = 0x00;
	d[3] = 0x00;
	d[4] = 0x19;
	d[5] = 0x58;
	d[6] = pdu;
	d[7] = 0;
	d[8] = 0;
	d[9] = (UB)n;
	knl_memcpy(d + 10, prm, n);
	avm_send(tl, TRUE, d, n + 10);
}

/* The earphones as AVRCP's target of the volume: what this end asks of them */
LOCAL void avm_in( CONST UB *f, INT n )
{
	UB	tl = (UB)( f[0] >> 4 ), r[4];

	if ( n < 6 || f[1] != 0x11 || f[2] != 0x0e ) {
		avm_bad++;
		return;
	}
	if ( ( f[0] & 2 ) != 0 ) {		/* an answer to the earphones' command */
		avm_rs = f[3];
		if ( f[5] == 0x30 ) avm_unit = (BOOL)( f[3] == 0x0c && n >= 11 && f[7] == 0x48 );
		return;
	}
	if ( f[5] != 0x00 || n < 13 ) {
		avm_bad++;
		return;
	}
	switch ( f[9] ) {
	case 0x10:
		avm_caps++;
		r[0] = 0x03;
		r[1] = 1;
		r[2] = 0x0d;
		avm_vendor(tl, 0x0c, 0x10, r, 3);
		break;
	case 0x31:
		if ( n < 14 || f[13] != 0x0d ) avm_bad++;
		avm_reg++;
		avm_reg_tl = tl;
		r[0] = 0x0d;
		r[1] = 0x40;
		avm_vendor(tl, 0x0f, 0x31, r, 2);
		break;
	case 0x50:
		avm_vol = f[13];
		avm_vol_n++;
		r[0] = f[13];
		avm_vendor(tl, 0x09, 0x50, r, 1);
		break;
	default:
		avm_bad++;
		break;
	}
}

/* A button of the earphones, pressed or let go */
LOCAL void avm_button( UB op, BOOL released )
{
	UB	d[5];

	d[0] = 0x00;
	d[1] = 0x48;
	d[2] = 0x7c;
	d[3] = (UB)( op | ( released ? 0x80 : 0 ) );
	d[4] = 0;
	avm_tl = (UB)( ( avm_tl + 1 ) & 15 );
	avm_rs = 0xff;
	avm_send(avm_tl, FALSE, d, 5);
	pump(50);
}

/* ---------------------------------------------------------------- the hands-free unit */

LOCAL BOOL	hfm_init;		/* the earphones started the session */
LOCAL UB	hfm_dlci, hfm_pn_cl;
LOCAL BOOL	hfm_open, hfm_msc_ok, hfm_msc_in, hfm_slc;
LOCAL INT	hfm_bad, hfm_ok, hfm_step, hfm_err;
LOCAL char	hfm_got[600];
LOCAL INT	hfm_n;
LOCAL UB	hfm_ch;			/* their channel in their records, 0 none */
LOCAL INT	hfm_sdp_asks;

LOCAL UB hfm_fcs( CONST UB *b, INT n )
{
	UB	f = 0xff;
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		f ^= b[i];
		for ( k = 0; k < 8; k++ ) f = ( f & 1 ) ? (UB)( ( f >> 1 ) ^ 0xe0 ) : (UB)( f >> 1 );
	}
	return (UB)( 0xff - f );
}

LOCAL void hfm_frame( UB dlci, UB ctrl, BOOL cmd, INT credits, CONST UB *d, INT n )
{
	UB	b[8 + 64];
	INT	k = 0;
	MCH	*c = mch_of(hfm_init ? CID_DEV_RFC : CID_DEV_RFC2);
	UB	ours = ( c != NULL ) ? c->ours : 0x47;

	if ( n > 64 ) return;
	b[k++] = (UB)( ( dlci << 2 ) | ( ( cmd ? hfm_init : !hfm_init ) ? 2 : 0 ) | 1 );
	b[k++] = ctrl;
	b[k++] = (UB)( ( n << 1 ) | 1 );
	if ( credits >= 0 ) b[k++] = (UB)credits;
	knl_memcpy(b + k, d, n);
	k += n;
	b[k] = hfm_fcs(b, ( ( ctrl & 0xef ) == 0xef ) ? 2 : 3);
	cl_send(ours, b, k + 1);
}

LOCAL void hfm_mux( UB type, BOOL cmd, CONST UB *v, INT n )
{
	UB	b[2 + 10];

	b[0] = (UB)( type | ( cmd ? 2 : 0 ) | 1 );
	b[1] = (UB)( ( n << 1 ) | 1 );
	knl_memcpy(b + 2, v, n);
	hfm_frame(0, 0xef, TRUE, -1, b, n + 2);
}

/* The session, as the earphones start it */
LOCAL void hfm_sabm0( void )
{
	hfm_frame(0, 0x3f, TRUE, -1, NULL, 0);
}

LOCAL void hfm_at( CONST char *t )
{
	hfm_frame(hfm_dlci, 0xff, TRUE, 3, (CONST UB *)t, (INT)knl_strlen(t));
}

/* The service level connection, a command each time the one before is answered OK */
LOCAL void hfm_next( void )
{
	static CONST char *CONST seq[] = { "AT+BRSF=16\r", "AT+CIND=?\r", "AT+CIND?\r", "AT+CMER=3,0,0,1\r" };

	if ( hfm_step < 4 ) hfm_at(seq[hfm_step++]);
	else hfm_slc = TRUE;
}

LOCAL void hfm_opened( void )
{
	UB	v[2];

	hfm_open = TRUE;
	v[0] = (UB)( ( hfm_dlci << 2 ) | 3 );
	v[1] = 0x8d;
	hfm_mux(0xe0, TRUE, v, 2);
	hfm_step = 0;
	hfm_next();
}

LOCAL void hfm_in( CONST UB *f, INT n )
{
	UB	dlci, ctrl;
	INT	len, hdr = 3, credits = -1;

	if ( n < 4 || ( f[2] & 1 ) == 0 ) {
		hfm_bad++;
		return;
	}
	dlci = (UB)( f[0] >> 2 );
	ctrl = (UB)( f[1] & 0xef );
	len = f[2] >> 1;
	if ( ctrl == 0xef && ( f[1] & 0x10 ) != 0 && dlci != 0 ) credits = f[hdr++];
	if ( hdr + len + 1 != n || f[n - 1] != hfm_fcs(f, ( ctrl == 0xef ) ? 2 : 3) ) {
		hfm_bad++;
		return;
	}
	(void)credits;
	f += hdr;
	switch ( ctrl ) {
	case 0x2f:				/* SABM: the session or the hands-free channel */
		hfm_frame(dlci, 0x73, FALSE, -1, NULL, 0);
		if ( dlci != 0 ) {
			hfm_dlci = dlci;
			hfm_opened();
		}
		break;
	case 0x63:				/* UA */
		if ( dlci == 0 ) {
			static CONST UB	pn[8] = { 2, 0xf0, 0, 0, 127, 0, 0, 7 };

			hfm_mux(0x80, TRUE, pn, 8);
		} else if ( dlci == hfm_dlci ) {
			hfm_opened();
		}
		break;
	case 0xef:
		if ( dlci == 0 && len >= 2 ) {
			UB	type = (UB)( f[0] & 0xfc );
			BOOL	cmd = (BOOL)( ( f[0] & 2 ) != 0 );

			if ( type == 0x80 && !cmd && len >= 10 ) {		/* PN answered: the channel */
				hfm_pn_cl = f[3];
				hfm_dlci = (UB)( f[2] & 0x3f );
				hfm_frame(hfm_dlci, 0x3f, TRUE, -1, NULL, 0);
			} else if ( type == 0x80 && cmd && len >= 10 ) {	/* this end's PN: credits taken */
				UB	v[8];

				knl_memcpy(v, f + 2, 8);
				hfm_pn_cl = (UB)( ( v[1] == 0xf0 ) ? 0xe0 : 0 );
				v[1] = hfm_pn_cl;
				if ( ( v[4] | ( v[5] << 8 ) ) > 127 ) {
					v[4] = 127;		/* frames of a one-byte length */
					v[5] = 0;
				}
				v[7] = 7;
				hfm_mux(0x80, FALSE, v, 8);
			} else if ( type == 0xe0 && cmd && len >= 4 ) {
				hfm_msc_in = TRUE;
				hfm_mux(0xe0, FALSE, f + 2, 2);
			} else if ( type == 0xe0 && !cmd ) {
				hfm_msc_ok = TRUE;
			}
		} else if ( dlci == hfm_dlci && len > 0 ) {
			INT	i;

			for ( i = 0; i < len && hfm_n < (INT)sizeof(hfm_got) - 1; i++ ) hfm_got[hfm_n++] = (char)f[i];
			hfm_got[hfm_n] = 0;
			if ( len == 6 && same_bytes(f, "\r\nOK\r\n", 6) ) {
				hfm_ok++;
				hfm_next();
			} else if ( len == 9 && same_bytes(f, "\r\nERROR\r\n", 9) ) {
				hfm_err++;
			}
		}
		break;
	default:
		break;
	}
}

/* The device's hands-free record, as this end asks it: its protocols and features */
LOCAL void hfm_sdp_in( CONST UB *q, INT n )
{
	UB	b[40];
	INT	k = 0, i;
	static CONST UB	rec_head[] = { 0x35, 25, 0x35, 23, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00,
				       0x35, 0x05, 0x19, 0x00, 0x03, 0x08 };
	static CONST UB	rec_tail[] = { 0x09, 0x03, 0x11, 0x09, 0x00, 0x10 };
	MCH		*c;

	hfm_sdp_asks++;
	if ( n < 10 || q[0] != 0x06 || q[8] != 0x11 || q[9] != 0x1e ) {
		hfm_bad++;
		return;
	}
	b[k++] = 0x07;
	b[k++] = q[1];
	b[k++] = q[2];
	b[k++] = 0;
	b[k++] = 2 + 27 + 1;
	b[k++] = 0;
	b[k++] = 27;
	for ( i = 0; i < (INT)sizeof(rec_head); i++ ) b[k++] = rec_head[i];
	b[k++] = hfm_ch;
	for ( i = 0; i < (INT)sizeof(rec_tail); i++ ) b[k++] = rec_tail[i];
	b[k++] = 0;
	c = NULL;
	(void)c;
	cl_send(0x48, b, k);
}

/* ---------------------------------------------------------------- the voice link */

#define H_SCO		7
LOCAL INT	sco_setups, sco_refuse, sco_disc, sco_tx, sco_tx_bad, sco_accepts;
LOCAL UH	sco_lat[4];
LOCAL UB	sco_buf[64];
LOCAL INT	sco_have;

/* The controller's side of the voice link: asked, refused as the test says, and taken down */
LOCAL BOOL sco_cmd( UH op, CONST UB *p )
{
	UB	e[20];

	if ( op == 0x0428 || op == 0x0429 ) {
		BOOL	fail = (BOOL)( op == 0x0428 && sco_refuse > 0 );

		if ( op == 0x0428 ) {
			if ( sco_setups < 4 ) sco_lat[sco_setups] = (UH)( p[10] | ( p[11] << 8 ) );
			sco_setups++;
			if ( p[12] != 0x60 || p[13] != 0x00 || p[2] != 0x40 || p[3] != 0x1f ) sco_tx_bad++;
		} else {
			sco_accepts++;
		}
		if ( fail ) sco_refuse--;
		cmd_status(op);
		knl_memset(e, 0, sizeof(e));
		e[0] = 0x2c;
		e[1] = 17;
		e[2] = (UB)( fail ? 0x1a : 0x00 );
		e[3] = H_SCO;
		e[4] = 0;
		knl_memcpy(e + 5, kb_addr, 6);
		e[11] = 0x02;
		e[12] = 6;
		e[13] = 2;
		e[14] = 60;
		e[16] = 60;
		e[18] = 0x02;
		ev(e, 19);
		return TRUE;
	}
	if ( op == 0x0406 && p[0] == H_SCO && p[1] == 0 ) {
		sco_disc++;
		cmd_status(op);
		e[0] = 0x05;
		e[1] = 4;
		e[2] = 0;
		e[3] = H_SCO;
		e[4] = 0;
		e[5] = 0x16;
		ev(e, 6);
		return TRUE;
	}
	return FALSE;
}

/* What this end sends of the voice: SCO packets of 48 bytes in USB packets of 17 */
LOCAL void sco_out( CONST UB *d, INT n )
{
	INT	i;

	if ( n != 17 ) sco_tx_bad++;
	for ( i = 0; i < n; i++ ) {
		if ( sco_have < (INT)sizeof(sco_buf) ) sco_buf[sco_have++] = d[i];
		if ( sco_have == 3 && ( sco_buf[0] != H_SCO || ( sco_buf[1] & 0x0f ) != 0 || sco_buf[2] != 48 ) ) {
			sco_tx_bad++;
			sco_have = 0;
		} else if ( sco_have == 51 ) {
			sco_tx++;
			sco_have = 0;
		}
	}
}

/* An SCO packet of the microphone's, in the USB packets it comes in */
LOCAL void sco_in( INT k )
{
	UB	b[51];
	INT	i;

	b[0] = H_SCO;
	b[1] = 0;
	b[2] = 48;
	for ( i = 0; i < 24; i++ ) {
		b[3 + 2 * i] = (UB)( k + i );
		b[4 + 2 * i] = 0x10;
	}
	for ( i = 0; i < 3; i++ ) KT_ASSERT_ER(knl_vhc_in(1, EP_SCO_IN, b + 17 * i, 17), E_OK);
}

IMPORT void knl_usb_kick( void );

LOCAL void hfm_clear( void )
{
	hfm_init = FALSE;
	hfm_dlci = hfm_pn_cl = 0;
	hfm_open = hfm_msc_ok = hfm_msc_in = hfm_slc = FALSE;
	hfm_bad = hfm_ok = hfm_step = hfm_err = hfm_n = 0;
	hfm_got[0] = 0;
	hfm_sdp_asks = 0;
	sco_setups = sco_disc = sco_tx = sco_tx_bad = sco_accepts = sco_have = 0;
	sco_refuse = 0;
	avm_caps = avm_reg = avm_vol_n = avm_bad = 0;
	avm_vol = -1;
}

/* Whether the output of the hands-free voice is listed */
LOCAL BOOL hfp_output( UB *id )
{
	SDOut	o[8];
	INT	i, n = knl_snd_outputs(o, 8);

	for ( i = 0; i < n && i < 8; i++ ) {
		INT	l = (INT)knl_strlen((CONST char *)o[i].id);

		if ( o[i].kind == SND_BE_BT && l > 4 && same_bytes(o[i].id + l - 4, "-hfp", 4) ) {
			if ( id != NULL ) knl_memcpy(id, o[i].id, SND_ID_MAX);
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * The remote control: the earphones open it, the buttons are keys of the
 * consumer page, and the volume goes both ways with the mixer's whole.
 */
LOCAL void test_avrcp( void )
{
	SDMix		m0, m;
	SDMixSet	s;
	T_HIDATTR	a, keep;
	T_HIDEV		e;
	INT		i;
	UB		d[8];

	hfm_clear();
	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	(void)ts_hid_setattr(&a);
	(void)knl_snd_mix_get(&m0, 0);
	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = s.rv = 80;
	(void)knl_snd_mix_set(&s, FALSE);
	avc_dev_first = TRUE;
	if ( av_pair() ) {
		for ( i = 0; i < 40 && avm_vol_n == 0; i++ ) pump(50);
		KT_ASSERT_EQ(mch_of(CID_DEV_AVC)->res, 0);
		KT_ASSERT_EQ(avm_caps, 1);
		KT_ASSERT_EQ(avm_reg, 1);
		/* the mixer's 80 to the earphones, as 0 to 127 */
		KT_ASSERT_EQ(avm_vol, ( 80 * 127 + 50 ) / 100);
		/* the earphones' own change: the mixer follows, and is told again */
		d[0] = 0x0d;
		d[1] = 0x20;
		avm_vendor(avm_reg_tl, 0x0d, 0x31, d, 2);
		pump(200);
		(void)knl_snd_mix_get(&m, 0);
		KT_ASSERT_EQ(m.lv, ( 0x20 * 100 + 63 ) / 127);
		KT_ASSERT_EQ(avm_reg, 2);
		KT_ASSERT_EQ(avm_vol_n, 1);	/* not sent back: it is theirs */
		/* the buttons */
		while ( ts_hid_read(&e, TMO_POL) >= E_OK ) ;
		avm_button(0x44, FALSE);
		KT_ASSERT_EQ(avm_rs, 0x09);
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_KEY_DOWN);
		KT_ASSERT_EQ(e.code, HID_KEY_PLAYPAUSE);
		avm_button(0x44, TRUE);
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.type, HID_EV_KEY_UP);
		avm_button(0x4b, FALSE);
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.code, HID_KEY_NEXT);
		avm_button(0x4b, TRUE);
		while ( ts_hid_read(&e, 100) >= E_OK ) ;
		avm_button(0x4c, FALSE);
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		KT_ASSERT_EQ(e.code, HID_KEY_PREV);
		avm_button(0x4c, TRUE);
		while ( ts_hid_read(&e, 100) >= E_OK ) ;
		avm_button(0x7e, FALSE);	/* none this end has */
		KT_ASSERT_EQ(avm_rs, 0x08);
		/* what the unit is */
		d[0] = 0x01;
		d[1] = 0xff;
		d[2] = 0x30;
		d[3] = d[4] = d[5] = d[6] = d[7] = 0xff;
		avm_unit = FALSE;
		avm_send(3, FALSE, d, 8);
		pump(50);
		KT_ASSERT(avm_unit);
		KT_ASSERT_EQ(avm_bad, 0);
		av_unpair();
	}
	avc_dev_first = FALSE;
	s.lv = m0.lv;
	s.rv = m0.rv;
	s.mute = m0.mute;
	(void)knl_snd_mix_set(&s, FALSE);
	(void)ts_hid_setattr(&keep);
}

LOCAL BOOL has_text( CONST char *h, CONST char *t )
{
	INT	n = (INT)knl_strlen(t), i, m = (INT)knl_strlen(h);

	for ( i = 0; i + n <= m; i++ ) {
		if ( same_bytes(h + i, t, n) ) return TRUE;
	}
	return FALSE;
}

/* The service level connection, the volume, and the voice both ways */
LOCAL void hfp_voice( void )
{
	T_BTHSTAT	st;
	SDMix		m;
	UW		rate = 0;
	INT		ch = 0, i;
	UB		id[SND_ID_MAX];

	KT_ASSERT(hfm_slc);
	KT_ASSERT(hfm_msc_ok);
	KT_ASSERT(hfm_msc_in);
	KT_ASSERT_EQ(hfm_pn_cl, 0xe0);
	KT_ASSERT_EQ(hfm_bad, 0);
	KT_ASSERT_EQ(hfm_err, 0);
	KT_ASSERT(has_text(hfm_got, "+BRSF: "));
	KT_ASSERT(has_text(hfm_got, "(\"call\",(0,1))"));
	KT_ASSERT(has_text(hfm_got, "+CIND: 1,0,0,0,5,0,5"));
	KT_ASSERT(hfp_output(id));
	knl_usb_lock();
	KT_ASSERT(knl_bta_hw.can_record());
	knl_usb_unlock();
	/* the earphones' gain: the mixer's whole with it */
	hfm_at("AT+VGS=7\r");
	pump(100);
	(void)knl_snd_mix_get(&m, 0);
	KT_ASSERT_EQ(m.lv, ( 7 * 100 + 7 ) / 15);
	/* the microphone: the link asked, the first set refused and the next taken */
	sco_refuse = 1;
	knl_usb_lock();
	KT_ASSERT_ER(knl_bta_hw.rec_start(16000, &rate, &ch), E_OK);
	knl_usb_unlock();
	KT_ASSERT_EQ(rate, 8000);
	KT_ASSERT_EQ(ch, 1);
	pump(300);
	KT_ASSERT_EQ(sco_setups, 2);
	KT_ASSERT_EQ(sco_lat[0], 0x000a);	/* S3: the earphones do not say S4 */
	KT_ASSERT_EQ(sco_lat[1], 0x0007);	/* S1 */
	knl_bth_stat(&st);
	KT_ASSERT_EQ(st.alt, 2);
	KT_ASSERT_EQ(st.handle, H_SCO);
	for ( i = 0; i < 30; i++ ) {
		if ( i % 3 == 0 ) sco_in(i);
		knl_usb_kick();
		tk_dly_tsk(4);
	}
	pump(50);
	knl_bth_stat(&st);
	tm_printf((UB *)"  voice: %d SCO packets out, %d in, %d samples to the sound device\n", sco_tx, st.rx_packets, st.rx_frames);
	KT_ASSERT(sco_tx >= 3);
	KT_ASSERT_EQ(sco_tx_bad, 0);
	KT_ASSERT(st.rx_packets >= 8);
	KT_ASSERT_EQ(st.rx_frames, st.rx_packets * 24);
	/* the voice output while the link is up, and the stream's output standing aside */
	knl_usb_lock();
	KT_ASSERT_ER(knl_bta_hw.play_start(id, &rate, &ch), E_OK);
	knl_usb_unlock();
	KT_ASSERT_EQ(rate, 8000);
	knl_usb_lock();
	knl_bta_hw.play_stop();
	knl_bta_hw.rec_stop();
	knl_usb_unlock();
	/* nothing uses it: taken down a while after */
	pump(2200);
	KT_ASSERT_EQ(sco_disc, 1);
}

/* Earphones that open the hands-free channel themselves */
LOCAL void test_hfp( void )
{
	static CONST UB	ag[] = { 0x19, 0x11, 0x1f };
	static CONST UB	chan[] = { 0x19, 0x00, 0x03, 0x08, 0x01 };
	SDMix		m0;
	SDMixSet	s;
	INT		i;

	/* the check sum against what 07.10 gives for SABM and UA on DLCI 0 */
	{
		static CONST UB	sabm[3] = { 0x03, 0x3f, 0x01 }, ua[3] = { 0x03, 0x73, 0x01 };

		KT_ASSERT_EQ(hfm_fcs(sabm, 3), 0x1c);
		KT_ASSERT_EQ(hfm_fcs(ua, 3), 0xd7);
	}
	hfm_clear();
	hfm_ch = 0;
	hfm_init = TRUE;
	rfc_dev_first = TRUE;
	(void)knl_snd_mix_get(&m0, 0);
	if ( av_pair() ) {
		/* this end's audio gateway, in its records */
		dev_open(0x0001, CID_DEV_SDPC);
		for ( i = 0; i < 20 && ( mch_of(CID_DEV_SDPC)->up != 3 ); i++ ) pump(20);
		KT_ASSERT(sdpc_run(0x02, 0x111f, 8));
		KT_ASSERT_EQ(sdpc_handles, 1);
		KT_ASSERT(sdpc_run(0x06, 0x111f, 600));
		KT_ASSERT(sdpc_has(ag, 3));
		KT_ASSERT(sdpc_has(chan, 5));
		for ( i = 0; i < 40 && !hfm_slc; i++ ) pump(50);
		hfp_voice();
		av_unpair();
	}
	rfc_dev_first = FALSE;
	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = m0.lv;
	s.rv = m0.rv;
	s.mute = m0.mute;
	(void)knl_snd_mix_set(&s, FALSE);
}

/* This end opens the earphones' hands-free channel, which their records name */
LOCAL void test_hfp_ag_first( void )
{
	SDMix		m0;
	SDMixSet	s;
	INT		i;

	hfm_clear();
	hfm_ch = 5;
	(void)knl_snd_mix_get(&m0, 0);
	if ( av_pair() ) {
		for ( i = 0; i < 80 && !hfm_slc; i++ ) pump(50);
		KT_ASSERT_EQ(hfm_sdp_asks, 1);
		KT_ASSERT_EQ(hfm_dlci, 5 << 1);
		hfp_voice();
		av_unpair();
	}
	hfm_ch = 0;
	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = m0.lv;
	s.rv = m0.rv;
	s.mute = m0.mute;
	(void)knl_snd_mix_set(&s, FALSE);
}

/* ---------------------------------------------------------------- Intel's kinds */

LOCAL CONST UB bt_ddesc_intel[18] = {
	18, 1, 0x00, 0x02, 0xe0, 1, 1, 64, 0x87, 0x80, 0x29, 0x00, 0x00, 0x01, 1, 2, 0, 1
};
LOCAL CONST T_VHCDEV bt_dev_intel = {
	bt_ddesc_intel, bt_cfg, sizeof(bt_cfg), { "TessronOS test", "Bluetooth test", NULL, NULL },
	{ NULL, NULL, NULL, NULL }, { 0, 0, 0, 0 },
	bt_out, bt_klass
};

/* What the adapter's object says, -1 no object */
LOCAL INT bt_state( T_BTSTATE *st )
{
	TS_UUID	u;
	SZ	asz = 0;
	ID	k;
	ER	er;

	(void)ts_str_to_uuid(BT_DEV_UUID, &u);
	k = ob_opn_obj(&u, OB_OP_READ);
	if ( k <= 0 ) return -1;
	er = ob_rea_rec(k, OB_BT_STATE, 0, st, sizeof(*st), &asz);
	(void)ob_cls_obj(k);
	return ( er >= E_OK && asz == (SZ)sizeof(*st) ) ? (INT)st->state : -1;
}

/* A controller that says nothing, or refuses its reset: there, not ready, and it says so */
LOCAL void test_bt_stuck( void )
{
	T_BTSTATE	st;
	T_BTCMD		cmd;
	INT		i, s = -1, k;

	for ( k = 0; k < 3; k++ ) {
		qh = qt = 0;
		acl_have = 0;
		mock_silent = (BOOL)( k < 2 );
		mock_reset_fail = (BOOL)( k == 2 );
		intel_answer = NULL;
		KT_ASSERT_ER(knl_vhc_plug(1, ( k == 1 ) ? &bt_dev_intel : &bt_dev), E_OK);
		pump(1000);
		KT_ASSERT(!knl_bt_ready());
		if ( k < 2 ) KT_ASSERT_EQ(bt_state(&st), BT_ST_STARTING);
		for ( i = 0; i < 300; i++ ) {
			s = bt_state(&st);
			if ( s == BT_ST_NOANSWER ) break;
			pump(50);
		}
		KT_ASSERT_EQ(s, BT_ST_NOANSWER);
		KT_ASSERT_EQ(st.op, 0x0c03);
		KT_ASSERT_EQ(st.status, ( k == 2 ) ? 0x01 : 0xffff);
		KT_ASSERT_EQ(st.vid, ( k == 1 ) ? 0x8087 : 0x1234);
		KT_ASSERT(!knl_bt_ready());
		knl_memset(&cmd, 0, sizeof(cmd));
		cmd.op = BT_C_SCAN;
		{
			TS_UUID	u;
			SZ	asz = 0;
			ID	key;

			(void)ts_str_to_uuid(BT_DEV_UUID, &u);
			key = ob_opn_obj(&u, OB_OP_WRITE);
			KT_ASSERT(key > 0);
			if ( key > 0 ) {
				KT_ASSERT_ER(ob_wri_rec(key, OB_BT_CMD, 0, &cmd, sizeof(cmd), &asz), E_OBJ);
				(void)ob_cls_obj(key);
			}
		}
		KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
		pump(800);
		KT_ASSERT_EQ(bt_state(&st), -1);	/* no adapter: no object */
	}
	mock_silent = mock_reset_fail = FALSE;
}

/* ---------------------------------------------------------------- the firmware's load */

LOCAL CONST char	fw_sfi_uuid[] = "01a0d8c4-54f0-7b01-8c10-01f0a2b354f0";
LOCAL CONST char	fw_ddc_uuid[] = "01a0d8c4-54f2-7b03-8c12-03f2a2b354f2";
#define FW_BOOT_ADDR	0x00102030
#define FW_TOTAL	( 644 + 608 )
LOCAL UB	fw_file[FW_TOTAL];
LOCAL INT	fw_got[4], fw_got_bytes;
LOCAL UB	fw_body[1000];
LOCAL BOOL	fw_refuse;

LOCAL ER fw_make( CONST char *id, CONST UB *d, INT n )
{
	static CONST char	js[] = "{\"name\":\"試験のファームウェア\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	static CONST char	note[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>test</p></document></tad>";
	T_OBCRE	c;
	TS_UUID	u, got;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	(void)ts_str_to_uuid(id, &u);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.uuid = u;
	c.json = (CONST UB *)js;
	c.jsonsz = (SZ)( sizeof(js) - 1 );
	er = ob_cre_obj(&c, &got);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	er = ( key > 0 ) ? E_OK : (ER)key;
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, note, (SZ)( sizeof(note) - 1 ), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, d, n, &asz);
	if ( key > 0 ) (void)ob_cls_obj(key);
	return er;
}

LOCAL void fw_rm( CONST char *id )
{
	TS_UUID	u;

	(void)ts_str_to_uuid(id, &u);
	(void)ob_del_obj(&u);
}

/* A file of the form: its header, the boot address, and commands of a filler to make its length */
LOCAL void fw_build( void )
{
	INT	i, at = 644;

	for ( i = 0; i < FW_TOTAL; i++ ) fw_file[i] = (UB)( i * 7 );
	fw_file[8] = 0;
	fw_file[9] = 0;
	fw_file[10] = 1;
	fw_file[11] = 0;
	fw_file[at++] = 0x0e;
	fw_file[at++] = 0xfc;
	fw_file[at++] = 4;
	fw_file[at++] = (UB)FW_BOOT_ADDR;
	fw_file[at++] = (UB)( FW_BOOT_ADDR >> 8 );
	fw_file[at++] = (UB)( FW_BOOT_ADDR >> 16 );
	fw_file[at++] = (UB)( FW_BOOT_ADDR >> 24 );
	while ( at < FW_TOTAL ) {
		INT	l = ( FW_TOTAL - at - 3 > 120 ) ? 120 : FW_TOTAL - at - 3;

		fw_file[at++] = 0x34;
		fw_file[at++] = 0xfc;
		fw_file[at++] = (UB)l;
		at += l;
	}
}

/* A secure send on the bulk endpoint, with the boot loader's answer there too */
LOCAL void fw_frag( CONST UB *b, INT n )
{
	static CONST UB	cc[6] = { 0x0e, 0x04, 0x01, 0x09, 0xfc, 0x00 };
	INT		t = b[3], l = n - 4;

	KT_ASSERT_EQ(b[2], n - 3);
	KT_ASSERT(l <= 252);
	if ( t <= 3 ) fw_got[t] += l;
	if ( t == 1 && fw_got_bytes + l <= (INT)sizeof(fw_body) ) {
		knl_memcpy(fw_body + fw_got_bytes, b + 4, l);
		KT_ASSERT_EQ(( fw_got[1] < FW_TOTAL - 644 ) ? ( fw_got[1] % 4 == 0 || l == 252 ) : 1, 1);
	}
	if ( t == 1 ) fw_got_bytes += l;
	acl_pieces(cc, 6);
	if ( fw_got_bytes == FW_TOTAL - 644 ) {
		UB	e[7];

		e[0] = 0xff;
		e[1] = 5;
		e[2] = 0x06;
		e[3] = fw_refuse ? 1 : 0;
		e[4] = 0x09;
		e[5] = 0xfc;
		e[6] = 0;
		ev(e, 7);
	}
}

LOCAL void test_bt_fwload( void )
{
	static CONST UB	ddc[] = { 3, 0x01, 0x02, 0x33, 2, 0x05, 0x06 };
	T_BTSTATE	st;
	INT		i, s = -1, round;
	ER		er;

	knl_bti_fw_wait_ms = 0;
	fw_build();
	fw_rm(fw_sfi_uuid);		/* a run cut short leaves them on the disk */
	fw_rm(fw_ddc_uuid);
	/* a WIFI=1 image links the real firmware under these names, and it cannot be replaced */
	er = fw_make(fw_sfi_uuid, fw_file, FW_TOTAL);
	if ( er == E_OBJ ) KT_SKIP("the real firmware is on the system volume (run with WIFI=0)");
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(fw_make(fw_ddc_uuid, ddc, sizeof(ddc)), E_OK);
	for ( round = 0; round < 2; round++ ) {
		qh = qt = 0;
		acl_have = 0;
		n_start = 0;
		fw_refuse = (BOOL)( round == 1 );
		fw_booted = FALSE;
		fw_boot_mode = TRUE;
		n_ddc = 0;
		fw_boot_seen = 0;
		fw_got[0] = fw_got[1] = fw_got[2] = fw_got[3] = 0;
		fw_got_bytes = 0;
		n_intel_ver = n_intel_boot = 0;
		KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev_intel), E_OK);
		for ( i = 0; i < 100 && !knl_bt_ready() && s != BT_ST_FWFAILED; i++ ) {
			pump(50);
			s = bt_state(&st);
		}
		KT_ASSERT_EQ(fw_got[0], 128);
		KT_ASSERT_EQ(fw_got[3], 256);
		KT_ASSERT_EQ(fw_got[2], 256);
		KT_ASSERT_EQ(fw_got_bytes, FW_TOTAL - 644);
		KT_ASSERT(same_bytes(fw_body, fw_file + 644, FW_TOTAL - 644));
		KT_ASSERT_EQ(n_intel_boot, 1);
		if ( round == 0 ) {
			KT_ASSERT(knl_bt_ready());
			KT_ASSERT_EQ(fw_boot_seen, FW_BOOT_ADDR);
			KT_ASSERT_EQ(n_intel_ver, 2);
			KT_ASSERT_EQ(n_ddc, 2);
			KT_ASSERT_EQ(n_start, 13);
			KT_ASSERT_EQ(bt_state(&st), BT_ST_READY);
		} else {
			KT_ASSERT(!knl_bt_ready());
			KT_ASSERT_EQ(bt_state(&st), BT_ST_FWFAILED);
			KT_ASSERT_EQ(st.file[0], 'i');
			KT_ASSERT_EQ(n_start, 0);
		}
		KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
		pump(800);
		s = -1;
	}
	fw_boot_mode = FALSE;
	fw_rm(fw_sfi_uuid);
	fw_rm(fw_ddc_uuid);
	knl_bti_fw_wait_ms = 30000;
}

/* It is told the version with the parameter of the newer form; what the machine does then */
LOCAL void test_bt_intel( void )
{

	static CONST UB tlv_boot[] = { 0, 0x10, 4, 0x00, 0x04, 0x37, 0x00, 0x12, 4, 0x00, 0x13, 0x2a, 0x10, 0x1c, 1, 0x01 };
	static CONST UB tlv_oper[] = { 0, 0x10, 4, 0x00, 0x04, 0x37, 0x00, 0x1c, 1, 0x03, 0x1f, 2, 0x34, 0x12 };
	static CONST UB old_boot[] = { 0, 0x37, 0x0b, 0x10, 0x06, 0x00, 0x20, 0x11, 0x13, 0x00 };
	static CONST UB old_oper[] = { 0, 0x37, 0x0b, 0x10, 0x23, 0x10, 0x20, 0x11, 0x13, 0x00 };
	static CONST struct { CONST UB *r; INT n; BOOL boot; BOOL old; } kinds[] = {
		{ tlv_boot, sizeof(tlv_boot), TRUE, FALSE },
		{ old_boot, sizeof(old_boot), TRUE, TRUE },
		{ tlv_oper, sizeof(tlv_oper), FALSE, FALSE },
		{ old_oper, sizeof(old_oper), FALSE, TRUE }
	};
	INT	k, i;

	knl_bti_fw_wait_ms = 0;
	for ( k = 0; k < 4; k++ ) {
		qh = qt = 0;
		acl_have = 0;
		n_start = 0;
		intel_answer = kinds[k].r;
		intel_answer_n = kinds[k].n;
		n_intel_ver = n_intel_boot = 0;
		KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev_intel), E_OK);
		for ( i = 0; i < 40 && !knl_bt_ready(); i++ ) pump(50);
		KT_ASSERT_EQ(n_intel_ver, 1);
		if ( kinds[k].boot ) {
			T_BTSTATE	st;

			pump(700);
			KT_ASSERT(!knl_bt_ready());
			KT_ASSERT_EQ(bt_state(&st), BT_ST_NOFIRMWARE);
			KT_ASSERT_EQ(n_start, 0);	/* nothing else was asked of the boot loader */
			KT_ASSERT_EQ(n_intel_boot, kinds[k].old ? 1 : 0);
		} else {
			KT_ASSERT(knl_bt_ready());
			KT_ASSERT_EQ(n_start, 13);
			KT_ASSERT_EQ(n_intel_boot, 0);
		}
		KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
		pump(300);
	}
	intel_answer = NULL;
}

LOCAL void le_refuse_run( void )
{
	T_BTFOUND	f[4];
	INT		i;

	mock_adv = TRUE;
	dev_rd = NULL;
	KT_ASSERT_ER(knl_vhc_plug(1, &bt_dev), E_OK);
	for ( i = 0; i < 100 && !knl_bt_ready(); i++ ) pump(50);
	KT_ASSERT_ER(knl_bt_scan(1), E_OK);
	pump(300);
	(void)knl_bt_found(f, 4);
	KT_ASSERT_ER(knl_bt_pair(le_addr), E_OK);
	for ( i = 0; i < 60 && m.n_failed == 0; i++ ) pump(50);
	if ( m.connected ) le_gone();
	pump(100);
	KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
	pump(300);
	mock_adv = FALSE;
}
#endif

EXPORT void ktest_bt( void )
{
	tm_printf((UB *)"\n[bt] Bluetooth (design 6.8)\n");
#if defined(_QEMU_VIRT_) || defined(X64)
	KT_RUN(test_bt_pair);
	KT_RUN(test_bt_report);
	KT_RUN(test_bt_report_bits);
	KT_RUN(test_bt_crypto);
	KT_RUN(test_bt_sbc);
	KT_RUN(test_le_legacy);
	KT_RUN(test_le_sc);
	KT_RUN(test_le_refused);
	KT_RUN(test_a2dp);
	KT_RUN(test_a2dp_mtu);
	KT_RUN(test_a2dp_dev);
	KT_RUN(test_sdp_serve);
	KT_RUN(test_avrcp);
	KT_RUN(test_hfp);
	KT_RUN(test_hfp_ag_first);
	KT_RUN(test_lea_crypto);
	KT_RUN(test_lea_stream);
	KT_RUN(test_lea_nocis);
	KT_RUN(test_bt_intel);
	KT_RUN(test_bt_fwload);
	KT_RUN(test_bt_stuck);
#endif
}
