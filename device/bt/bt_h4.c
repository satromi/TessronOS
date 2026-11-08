/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_h4.c
 *	H4, the HCI on a UART (design 6.8): each packet goes with a byte
 *	before it that says its type, and its own header says its length.
 *	The line has nothing else to find a packet's start by, so a byte
 *	lost puts what follows out of step; the controller's line has flow
 *	control, which keeps bytes from being lost.
 *
 *	knl_bth4_feed runs in the UART's interrupt handler: it only copies
 *	and counts.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "btint.h"
#include "bt_bcm.h"

EXPORT INT knl_bth4_hlen( UB type )
{
	switch ( type ) {
	case H4_CMD:	return 3;	/* opcode, length */
	case H4_ACL:	return 4;	/* handle and flags, a 16-bit length */
	case H4_SCO:	return 3;	/* handle and flags, length */
	case H4_EVENT:	return 2;	/* code, length */
	case H4_ISO:	return 4;	/* handle and flags, a 14-bit length */
	default:	return 0;
	}
}

/* The whole length of a packet whose header is at h */
LOCAL INT h4_len( UB type, CONST UB *h )
{
	switch ( type ) {
	case H4_CMD:	return 3 + h[2];
	case H4_ACL:	return 4 + ( h[2] | ( h[3] << 8 ) );
	case H4_SCO:	return 3 + h[2];
	case H4_EVENT:	return 2 + h[1];
	case H4_ISO:	return 4 + ( ( h[2] | ( h[3] << 8 ) ) & 0x3fff );
	default:	return 0;
	}
}

EXPORT void knl_bth4_init( BTH4 *h )
{
	h->type = 0;
	h->hlen = h->have = h->want = h->skip = 0;
	h->frames = h->bad_type = h->too_long = 0;
}

EXPORT void knl_bth4_feed( BTH4 *h, CONST UB *b, INT n, BTH4_OUT out, void *ctx )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		UB	x = b[i];

		if ( h->skip > 0 ) {
			h->skip--;
			continue;
		}
		if ( h->type == 0 ) {
			INT	hl = knl_bth4_hlen(x);

			if ( hl == 0 ) {
				h->bad_type++;
				continue;
			}
			h->type = x;
			h->hlen = hl;
			h->have = h->want = 0;
			continue;
		}
		h->b[h->have++] = x;
		if ( h->want == 0 && h->have == h->hlen ) {
			h->want = h4_len(h->type, h->b);
			if ( h->want > BTH4_MAX ) {
				/* passed over to its end: what comes after it is still a packet */
				h->too_long++;
				h->skip = h->want - h->have;
				h->type = 0;
				continue;
			}
		}
		if ( h->want != 0 && h->have == h->want ) {
			h->frames++;
			out(ctx, h->type, h->b, h->have);
			h->type = 0;
		}
	}
}

EXPORT INT knl_bth4_pack( UB type, CONST UB *pkt, INT n, UB *out, INT max )
{
	INT	hl = knl_bth4_hlen(type);

	if ( hl == 0 || n < hl || h4_len(type, pkt) != n || n + 1 > max ) return E_PAR;
	out[0] = type;
	knl_memcpy(out + 1, pkt, n);
	return n + 1;
}
