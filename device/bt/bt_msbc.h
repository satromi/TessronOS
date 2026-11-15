/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_msbc.h
 *	The hands-free profile's wide band speech on the voice link (design
 *	6.8, bt_msbc.c): each mSBC frame goes with the H2 header before it,
 *	the sync 0x01 and a byte with a sequence number of 2 bits, and a
 *	byte of padding after, 60 bytes in all, one in each eSCO packet of
 *	the T1 and T2 settings.
 */

#ifndef __BT_MSBC_H__
#define __BT_MSBC_H__

#include "sbc_enc.h"

#define H2_LEN			60	/* the header, the frame, the padding */
#define H2_SYNC			0x01

/* What goes out: the encoder and the next sequence number */
typedef struct {
	SBC_ENC		enc;
	UB		sn;
} MSBC_TX;

/* What comes in, as bytes the packets have, which need not start where a frame does */
typedef void (*MSBC_OUT)( void *ctx, CONST H *pcm, INT n );

typedef struct {
	SBC_DEC		dec;
	UB		b[2 * H2_LEN];
	INT		have;
	BOOL		pad;		/* a frame's byte of padding may come next */
	INT		sn;		/* the sequence number the next frame should have, -1 not known */
	INT		lost_bytes;	/* the bytes of packets that came broken, not yet made up for */
	INT		lost_run;	/* frames made up one after the other */
	H		last[MSBC_SAMPLES];	/* the last frame given, for what is made up */
	UW		frames;		/* frames decoded */
	UW		filled;		/* frames made up for ones lost */
	UW		bad;		/* frames found whose check failed */
	UW		skipped;	/* bytes passed over looking for a header */
} MSBC_RX;

IMPORT void knl_msbc_tx_init( MSBC_TX *t );
/* MSBC_SAMPLES samples into the H2_LEN bytes of out */
IMPORT void knl_msbc_tx_frame( MSBC_TX *t, CONST H *pcm, UB *out );

IMPORT void knl_msbc_rx_init( MSBC_RX *r );
/*
 * The data of a packet: good FALSE when its status says it was not
 * received right. Each frame found gives its samples to out; one that
 * is lost, by its packet, its check or the sequence numbers, is made up
 * from the frame before it, fading, and becomes silence after a few.
 */
IMPORT void knl_msbc_rx_feed( MSBC_RX *r, CONST UB *b, INT n, BOOL good, MSBC_OUT out, void *ctx );

/* The second byte of the header for a sequence number 0 to 3, and the number of one; -1 not one */
IMPORT UB   knl_msbc_h2_byte( INT sn );
IMPORT INT  knl_msbc_h2_sn( UB b );

#endif /* __BT_MSBC_H__ */
