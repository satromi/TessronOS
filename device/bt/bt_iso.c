/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_iso.c
 *	What the controller can do of LE (its LE features), and its
 *	isochronous channels (design 6.8): a CIG of up to two CISes, each
 *	going to a device on its LE link, and the SDUs sent on them.
 *
 *	LE Audio is taken only of a controller that says it can be the
 *	central of CISes (bit 28 of its LE features) and gives buffers for
 *	ISO data; this end then sets the host's support of CISes (bit 32).
 *	A controller that cannot is asked nothing of all this, and the rest of
 *	the driver goes on as before.
 *
 *	On USB the ISO data packets go on the same bulk endpoint as the ACL
 *	ones, and are told apart by their handle; a way to the controller
 *	that cannot carry them (BTTRANS's iso) leaves LE Audio unused. The controller counts them
 *	apart too: what it says it has done of a CIS's handle is room for ISO
 *	packets, not ACL ones.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "btint.h"
#include "bt_lea.h"

/* Bits of the LE features (Core 6.B.4.6) */
#define FEAT_EXT_ADV		12
#define FEAT_CIS_CENTRAL	28
#define FEAT_CIS_PERIPH		29
#define FEAT_ISO_BROADCAST	30
#define FEAT_SYNC_RECEIVER	31

/* Commands */
#define C_LE_SET_CIG		0x2062
#define C_LE_CREATE_CIS		0x2064
#define C_LE_REMOVE_CIG		0x2065
#define C_LE_ISO_PATH		0x206e

#define HCI_REMOTE_USER_ENDED	0x13
#define CIG_ID			0

/* The PB flag of an ISO data packet: where its piece is in the SDU */
#define ISO_PB_FIRST		0
#define ISO_PB_CONT		1
#define ISO_PB_WHOLE		2
#define ISO_PB_LAST		3
#define ISO_SDU_HEAD		4	/* the sequence number and the SDU's length, in the first piece */

#define CODING_TRANSPARENT	0x03	/* the controller passes the SDUs on as they are */

LOCAL BTC	*iso_c = NULL;			/* the controller the handles below are of */
LOCAL UH	iso_cis[LEA_NSTREAM];		/* the CIG's CISes, 0xffff none */
LOCAL INT	iso_ncis = 0;

LOCAL BOOL le_feat( BTC *c, INT bit )
{
	return (BOOL)( ( c->le_feat[bit >> 3] >> ( bit & 7 ) ) & 1 );
}

LOCAL void put16( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void put24( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
}

LOCAL BOOL is_cis( BTC *c, UH h )
{
	INT	i;

	if ( c != iso_c ) return FALSE;
	for ( i = 0; i < iso_ncis; i++ ) {
		if ( iso_cis[i] == h ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- from bt.c */

/* A command answered: those of the start, and of the CIG */
EXPORT void knl_btiso_cmd_done( BTC *c, UH op, INT st, CONST UB *r, INT n )
{
	switch ( op ) {
	case C_LE_READ_FEAT:
		c->has_ext_adv = c->has_cis = FALSE;
		if ( st != 0 || n < 8 ) {
			tm_printf((UB *)"bt: CIS central: unknown, LE features not read (%02x): LE Audio not used\n", st);
			break;
		}
		knl_memcpy(c->le_feat, r, 8);
		c->has_ext_adv = le_feat(c, FEAT_EXT_ADV);
		c->has_cis = le_feat(c, FEAT_CIS_CENTRAL);
		if ( c->has_cis && c->tr->iso == NULL ) {
			c->has_cis = FALSE;
			tm_printf((UB *)"bt: the way to the controller carries no ISO data\n");
		}
		tm_printf((UB *)"bt: LE features %02x %02x %02x %02x %02x %02x %02x %02x: extended advertising %d, CIS peripheral %d, broadcaster %d, synchronized receiver %d\n",
			  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], c->has_ext_adv,
			  le_feat(c, FEAT_CIS_PERIPH), le_feat(c, FEAT_ISO_BROADCAST), le_feat(c, FEAT_SYNC_RECEIVER));
		tm_printf((UB *)"bt: CIS central %s\n", c->has_cis ? "supported: LE Audio used" : "not supported: LE Audio not used");
		break;
	case C_LE_READ_BUF_V2:
		if ( st == 0 && n >= 6 ) {
			c->iso_len = (UH)( r[3] | ( r[4] << 8 ) );
			c->iso_room = r[5];
			tm_printf((UB *)"bt: ISO packets of %d bytes, %d at a time\n", c->iso_len, c->iso_room);
		}
		if ( st != 0 || n < 6 || c->iso_len <= ISO_SDU_HEAD || c->iso_room <= 0 ) {
			c->has_cis = FALSE;
			tm_printf((UB *)"bt: no buffers for ISO data (%02x): LE Audio not used\n", st);
		}
		break;
	case C_LE_HOST_FEAT:
		if ( st != 0 ) {
			c->has_cis = FALSE;
			tm_printf((UB *)"bt: the host's support of CISes refused (%02x): LE Audio not used\n", st);
		}
		break;
	case C_LE_SET_CIG: {
		UH	h[LEA_NSTREAM];
		INT	i, k = 0;

		if ( st == 0 && n >= 2 ) {
			for ( i = 0; i < r[1] && i < LEA_NSTREAM && 2 + 2 * i + 2 <= n; i++ ) {
				h[k++] = (UH)( r[2 + 2 * i] | ( r[3 + 2 * i] << 8 ) );
			}
			iso_c = c;
			iso_ncis = k;
			for ( i = 0; i < k; i++ ) iso_cis[i] = h[i];
		}
		tm_printf((UB *)"bt: CIG %d: status %02x, %d CIS\n", CIG_ID, st, k);
		knl_btlea_cig_done(c, st, k, h);
		break;
	}
	case C_LE_ISO_PATH:
		knl_btlea_path_done(c, (UH)( ( n >= 2 ) ? ( r[0] | ( r[1] << 8 ) ) : 0xffff ), st);
		break;
	case C_LE_REMOVE_CIG:
		if ( st == 0 && c == iso_c ) iso_ncis = 0;
		knl_btlea_cig_removed(c, st);
		break;
	default:
		break;
	}
}

/* A command that did not start */
EXPORT void knl_btiso_cmd_status( BTC *c, UH op, INT st )
{
	if ( op == C_LE_CREATE_CIS && st != 0 ) knl_btlea_cis_failed(c, st);
}

/* The packets the controller has done of a handle: TRUE when it is a CIS's */
EXPORT BOOL knl_btiso_pkts_done( BTC *c, UH h, INT done )
{
	if ( !is_cis(c, h) ) return FALSE;
	c->iso_room += done;
	return TRUE;
}

/* A link gone: TRUE when it was a CIS */
EXPORT BOOL knl_btiso_disconn( BTC *c, UH h, INT reason )
{
	if ( !is_cis(c, h) ) return FALSE;
	knl_btlea_cis_down(c, h, reason);
	return TRUE;
}

/* The LE event that a CIS is up, or could not be */
EXPORT void knl_btiso_cis_est( BTC *c, CONST UB *d, INT len )
{
	UH	h;

	if ( len < 28 ) return;
	h = (UH)( d[1] | ( d[2] << 8 ) );
	tm_printf((UB *)"bt: CIS %d: status %02x, latency %d us, PHY %d, BN %d, FT %d, PDU %d bytes, ISO interval %d\n",
		  h, d[0], d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), d[15], d[18], d[20],
		  d[22] | ( d[23] << 8 ), d[26] | ( d[27] << 8 ));
	knl_btlea_cis_up(c, h, d[0]);
}

/* ---------------------------------------------------------------- for bt_lea.c */

/*
 * The CIG: n CISes from this end to the devices, an SDU every interval
 * us. Nothing comes back on them: what goes to the central has no SDU,
 * and is given the same interval and latency only as the command asks
 * for values in their ranges.
 */
EXPORT void knl_btiso_cig( BTC *c, UW interval, UB framing, UH latency, INT n,
			   CONST UB *cis_id, CONST UH *max_sdu, CONST UB *phy, CONST UB *rtn )
{
	UB	r[15 + 9 * LEA_NSTREAM];
	INT	i, k = 15;

	if ( n > LEA_NSTREAM ) n = LEA_NSTREAM;
	r[0] = CIG_ID;
	put24(r + 1, interval);
	put24(r + 4, interval);
	r[7] = 0;			/* the worst clock accuracy: 251 to 500 ppm */
	r[8] = 0;			/* sequential packing */
	r[9] = framing;
	put16(r + 10, latency);
	put16(r + 12, latency);
	r[14] = (UB)n;
	for ( i = 0; i < n; i++ ) {
		r[k] = cis_id[i];
		put16(r + k + 1, max_sdu[i]);
		put16(r + k + 3, 0);
		r[k + 5] = phy[i];
		r[k + 6] = phy[i];
		r[k + 7] = rtn[i];
		r[k + 8] = 0;
		k += 9;
	}
	knl_btx_cmd(c, C_LE_SET_CIG, r, k);
}

EXPORT void knl_btiso_cig_remove( BTC *c )
{
	UB	r[1];

	r[0] = CIG_ID;
	knl_btx_cmd(c, C_LE_REMOVE_CIG, r, 1);
}

EXPORT void knl_btiso_cis_create( BTC *c, INT n, CONST UH *cis, CONST UH *acl )
{
	UB	r[1 + 4 * LEA_NSTREAM];
	INT	i;

	if ( n > LEA_NSTREAM ) n = LEA_NSTREAM;
	r[0] = (UB)n;
	for ( i = 0; i < n; i++ ) {
		put16(r + 1 + 4 * i, cis[i]);
		put16(r + 3 + 4 * i, acl[i]);
	}
	knl_btx_cmd(c, C_LE_CREATE_CIS, r, 1 + 4 * n);
}

/* The CIS's data from the host by HCI, the codec being this end's */
EXPORT void knl_btiso_path( BTC *c, UH cis )
{
	UB	r[13];

	knl_memset(r, 0, sizeof(r));
	put16(r, cis);
	r[2] = 0;			/* in: from the host to the controller */
	r[3] = 0;			/* by HCI */
	r[4] = CODING_TRANSPARENT;
	knl_btx_cmd(c, C_LE_ISO_PATH, r, 13);
}

EXPORT void knl_btiso_drop( BTC *c, UH cis )
{
	UB	r[3];

	put16(r, cis);
	r[2] = HCI_REMOTE_USER_ENDED;
	knl_btx_cmd(c, C_DISCONNECT, r, 3);
}

/*
 * An SDU on the CIS, in as many packets as the controller's ISO packets
 * take, with no time stamp: the controller sends it at the next
 * interval. FALSE, nothing sent, while it has no room for all of them.
 */
EXPORT BOOL knl_btiso_send( BTC *c, UH cis, UH seq, CONST UB *sdu, INT n )
{
	UB	b[4 + ISO_SDU_HEAD + LEA_SDU_MAX];
	INT	max = c->iso_len, off = 0, pkts;

	if ( max <= ISO_SDU_HEAD || n > LEA_SDU_MAX || n <= 0 ) return FALSE;
	pkts = ( n + ISO_SDU_HEAD + max - 1 ) / max;
	if ( c->iso_room < pkts ) return FALSE;
	while ( off < n ) {
		BOOL	first = (BOOL)( off == 0 );
		INT	room = first ? max - ISO_SDU_HEAD : max;
		INT	k = ( n - off > room ) ? room : n - off, at = 4;
		BOOL	last = (BOOL)( off + k >= n );
		INT	pb = ( first && last ) ? ISO_PB_WHOLE : first ? ISO_PB_FIRST : last ? ISO_PB_LAST : ISO_PB_CONT;
		INT	load = k + ( first ? ISO_SDU_HEAD : 0 );
		ER	er;

		b[0] = (UB)cis;
		b[1] = (UB)( ( ( cis >> 8 ) & 0x0f ) | ( pb << 4 ) );
		b[2] = (UB)load;
		b[3] = (UB)( ( load >> 8 ) & 0x3f );
		if ( first ) {
			put16(b + 4, seq);
			b[6] = (UB)n;
			b[7] = (UB)( ( n >> 8 ) & 0x0f );
			at = 8;
		}
		knl_memcpy(b + at, sdu + off, k);
		c->iso_room--;
		er = c->gone ? E_IO : c->tr->iso(c, b, at + k);
		if ( er < E_OK ) {
			tm_printf((UB *)"bt: ISO out on %d failed (%d)\n", cis, (INT)er);
			return TRUE;		/* the SDU is lost, not sent again late */
		}
		off += k;
	}
	return TRUE;
}
