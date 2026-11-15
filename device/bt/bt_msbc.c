/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_msbc.c
 *	mSBC in H2 frames (design 6.8), what the hands-free profile's
 *	wide band speech puts on the voice link in transparent mode.
 *
 *	Out, each 7.5 ms of samples at 16 kHz is an mSBC frame of 57 bytes
 *	(sbc_enc.c) with the header 0x01 and the sequence number before it
 *	and a byte of padding after it: 60 bytes, one eSCO packet.
 *
 *	In, the bytes need not start where a frame does: the controller
 *	gives the packets as they came, and a packet lost or broken shifts
 *	nothing on the air but leaves a gap here. So the header is looked
 *	for in the bytes: 0x01, a second byte that is one of the four, and
 *	the mSBC sync 0xAD after them. A frame whose check fails, the data
 *	of a packet whose status says it was not received right, and a gap
 *	in the sequence numbers each count as frames lost. A lost frame is
 *	made up from the last good one, quieter each time, and silence after
 *	three: the voice goes on at its rate, and a short loss is not heard
 *	as a click.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "bt_msbc.h"

#define FILL_FADE	3		/* frames made up from the last one before silence */

/* The header's second byte: from the low bit 1000, then SN0 twice and SN1 twice */
LOCAL CONST UB	h2_b[4] = { 0x08, 0x38, 0xc8, 0xf8 };

EXPORT UB knl_msbc_h2_byte( INT sn )
{
	return h2_b[sn & 3];
}

EXPORT INT knl_msbc_h2_sn( UB b )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		if ( h2_b[i] == b ) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- out */

EXPORT void knl_msbc_tx_init( MSBC_TX *t )
{
	sbc_enc_init_msbc(&t->enc);
	t->sn = 0;
}

EXPORT void knl_msbc_tx_frame( MSBC_TX *t, CONST H *pcm, UB *out )
{
	out[0] = H2_SYNC;
	out[1] = knl_msbc_h2_byte(t->sn);
	t->sn = (UB)( ( t->sn + 1 ) & 3 );
	(void)sbc_enc_frame(&t->enc, (CONST short *)pcm, out + 2);
	out[2 + MSBC_LEN] = 0;
}

/* ---------------------------------------------------------------- in */

EXPORT void knl_msbc_rx_init( MSBC_RX *r )
{
	knl_memset(r, 0, sizeof(*r));
	sbc_dec_init(&r->dec);
	r->sn = -1;
}

/* A frame lost: the last one again, quieter, or silence */
LOCAL void fill( MSBC_RX *r, MSBC_OUT out, void *ctx )
{
	H	pcm[MSBC_SAMPLES];
	INT	i, k;

	r->lost_run++;
	k = ( r->lost_run <= FILL_FADE ) ? FILL_FADE + 1 - r->lost_run : 0;
	for ( i = 0; i < MSBC_SAMPLES; i++ ) pcm[i] = (H)( r->last[i] * k / ( FILL_FADE + 1 ) );
	r->filled++;
	if ( r->sn >= 0 ) r->sn = ( r->sn + 1 ) & 3;
	out(ctx, pcm, MSBC_SAMPLES);
}

/* The first k bytes held let go: a copy to the front, byte by byte as the two overlap */
LOCAL void drop( MSBC_RX *r, INT k )
{
	INT	i;

	if ( k <= 0 ) return;
	if ( k > r->have ) k = r->have;
	for ( i = 0; i < r->have - k; i++ ) r->b[i] = r->b[i + k];
	r->have -= k;
}

/* The frames in what is held; what is left is less than a frame, or a header's start */
LOCAL void frames( MSBC_RX *r, MSBC_OUT out, void *ctx )
{
	H	pcm[MSBC_SAMPLES];

	for ( ;; ) {
		INT	i, sn = -1, got, nch, len;

		/* the byte of padding after a frame: let go when it is there, not counted as passed over */
		if ( r->pad && r->have > 0 ) {
			if ( r->b[0] == 0 ) drop(r, 1);
			r->pad = FALSE;
		}
		for ( i = 0; i + 2 < r->have; i++ ) {
			if ( r->b[i] == H2_SYNC && r->b[i + 2] == MSBC_SYNC && ( sn = knl_msbc_h2_sn(r->b[i + 1]) ) >= 0 ) break;
		}
		if ( i + 2 >= r->have ) {
			/* no header: the last two bytes may be the start of one */
			if ( r->have > 2 ) {
				r->skipped += (UW)( r->have - 2 );
				drop(r, r->have - 2);
			}
			return;
		}
		r->skipped += (UW)i;
		drop(r, i);
		if ( r->have < 2 + MSBC_LEN ) return;
		got = sbc_dec_frame(&r->dec, r->b + 2, MSBC_LEN, (short *)pcm, &nch, &len);
		if ( got != MSBC_SAMPLES ) {
			r->bad++;
			fill(r, out, ctx);
		} else {
			/* the frames between the last and this one never came */
			if ( r->sn >= 0 ) {
				INT	gap = ( sn - r->sn ) & 3;

				while ( gap-- > 0 ) fill(r, out, ctx);
			}
			r->frames++;
			r->lost_run = 0;
			r->lost_bytes = 0;
			r->sn = ( sn + 1 ) & 3;
			knl_memcpy(r->last, pcm, sizeof(pcm));
			out(ctx, pcm, MSBC_SAMPLES);
		}
		drop(r, 2 + MSBC_LEN);
		r->pad = TRUE;
	}
}

EXPORT void knl_msbc_rx_feed( MSBC_RX *r, CONST UB *b, INT n, BOOL good, MSBC_OUT out, void *ctx )
{
	if ( !good ) {
		/* what was held goes with it: a frame that ran into the broken packet is lost too */
		r->lost_bytes += n + r->have;
		r->have = 0;
		r->pad = FALSE;
		while ( r->lost_bytes >= H2_LEN ) {
			fill(r, out, ctx);
			r->lost_bytes -= H2_LEN;
		}
		return;
	}
	while ( n > 0 ) {
		INT	k = (INT)sizeof(r->b) - r->have;

		if ( k > n ) k = n;
		knl_memcpy(r->b + r->have, b, k);
		r->have += k;
		b += k;
		n -= k;
		frames(r, out, ctx);
	}
}
