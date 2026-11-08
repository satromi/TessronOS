/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	nicring.h
 *	What the LAN drivers do with their descriptor rings and bulk transfers
 *	as plain functions of memory, with no register and no kernel service
 *	in them, so that a test can play the part of the chip
 *	(tests/ktest/ktest_nicring.c).
 *
 *	rtl_*	the Realtek PCIe chips: a slot belongs to the chip while its OWN
 *		bit is set, and the last slot of a ring says so (EOR). The
 *		RTL8111/8168 and the RTL8125 differ in where the fields of the
 *		receive descriptor lie and in the bits of its frame marks, and
 *		both count the frame check sequence in the length.
 *	igc_*	the Intel I225/I226: a slot is the chip's up to a tail register,
 *		and the chip overwrites the buffer's address when it writes a
 *		slot back, so that it is written again each time.
 *	ure_*	the frames of a transfer of the USB RTL8152/8153: each after a
 *		header of 24 bytes, at 8 byte boundaries.
 *	axen_*	the frames of a transfer of the USB AX88179: each at 8 byte
 *		boundaries, the headers in a table at the end of the transfer.
 *	axe_*	the frames of the USB AX88772: each after a header of 4 bytes
 *		(the length and its complement), at 2 byte boundaries, and a
 *		frame may go on into the next transfer.
 */

#ifndef __NICRING_H__
#define __NICRING_H__


/* A frame that may be sent or received: its header at the least */
#define NIC_FRAME_MIN	14
#define NIC_FRAME_PAD	60		/* shorter frames are padded with zeros */
#define NIC_CRC_LEN	4

/* What taking a slot gives: the length of a good frame, or one of these */
#define NIC_RX_NONE	(-1)		/* the chip still has it */
#define NIC_RX_BAD	0		/* a frame with errors, or in pieces: to be dropped */

/* ---------------------------------------------------------------- the Realtek rings */

#define RTL_OWN		0x80000000U
#define RTL_EOR		0x40000000U
#define RTL_TX_FS	0x20000000U
#define RTL_TX_LS	0x10000000U
#define RTL_RX_ERRSUM	0x00100000U
#define RTL_RX_LENMASK	0x00003fffU

/* The RTL8111/8168: one descriptor for both directions, its frame marks as sending has them */
#define RE_RX_FS	0x20000000U
#define RE_RX_LS	0x10000000U
/* The RTL8125 */
#define RGE_RX_FS	0x02000000U
#define RGE_RX_LS	0x01000000U

typedef struct {
	volatile UW	cmdsts;
	volatile UW	vlan;
	volatile UD	addr;
} RE_DESC;

typedef struct {
	volatile UW	rsv[4];
	volatile UD	addr;
	volatile UW	extsts;
	volatile UW	cmdsts;
} RGE_RXD;

typedef struct {
	volatile UW	cmdsts;
	volatile UW	extsts;
	volatile UD	addr;
	volatile UW	rsv[4];
} RGE_TXD;

/* The cmdsts of a receive slot given to the chip */
static __inline__ UW rtl_rx_word( UINT i, UINT n, UINT bufsz )
{
	return RTL_OWN | ( ( i == n - 1 ) ? RTL_EOR : 0 ) | bufsz;
}

/* What a receive word says: the frame's length without its check sequence, or NIC_RX_* */
static __inline__ INT rtl_rx_len( UW w, UW first, UW last )
{
	INT	len;

	if ( w & RTL_OWN ) return NIC_RX_NONE;
	if ( ( w & ( first | last ) ) != ( first | last ) || ( w & RTL_RX_ERRSUM ) ) return NIC_RX_BAD;
	len = (INT)( w & RTL_RX_LENMASK ) - NIC_CRC_LEN;
	return ( len >= NIC_FRAME_MIN ) ? len : NIC_RX_BAD;
}

/* The cmdsts of a transmit slot given to the chip: one buffer, one frame */
static __inline__ UW rtl_tx_word( UINT i, UINT n, UINT len )
{
	return RTL_OWN | RTL_TX_FS | RTL_TX_LS | ( ( i == n - 1 ) ? RTL_EOR : 0 ) | len;
}

/* The bare receive and transmit rings: slots empty for the chip, or for sending */
static __inline__ void re_ring_init( RE_DESC *rx, UINT nrx, UD rxpa, UINT bufsz, RE_DESC *tx, UINT ntx )
{
	UINT	i;

	for ( i = 0; i < ntx; i++ ) {
		tx[i].addr = 0;
		tx[i].vlan = 0;
		tx[i].cmdsts = ( i == ntx - 1 ) ? RTL_EOR : 0;
	}
	for ( i = 0; i < nrx; i++ ) {
		rx[i].addr = rxpa + (UD)i * bufsz;
		rx[i].vlan = 0;
		rx[i].cmdsts = rtl_rx_word(i, nrx, bufsz);
	}
}

static __inline__ void rge_ring_init( RGE_RXD *rx, UINT nrx, UD rxpa, UINT bufsz, RGE_TXD *tx, UINT ntx )
{
	UINT	i, k;

	for ( i = 0; i < ntx; i++ ) {
		tx[i].addr = 0;
		tx[i].extsts = 0;
		for ( k = 0; k < 4; k++ ) tx[i].rsv[k] = 0;
		tx[i].cmdsts = ( i == ntx - 1 ) ? RTL_EOR : 0;
	}
	for ( i = 0; i < nrx; i++ ) {
		for ( k = 0; k < 4; k++ ) rx[i].rsv[k] = 0;
		rx[i].addr = rxpa + (UD)i * bufsz;
		rx[i].extsts = 0;
		rx[i].cmdsts = rtl_rx_word(i, nrx, bufsz);
	}
}

/* ---------------------------------------------------------------- the Intel rings */

#define IGC_RX_DD	0x00000001U
#define IGC_RX_EOP	0x00000002U
#define IGC_RX_ERR	0x80000000U	/* the frame had errors */
#define IGC_TX_DD	0x00000001U
#define IGC_TX_CMD	0x22300000U	/* an advanced data descriptor: the FCS added */
#define IGC_TX_LAST	0x09000000U	/* the frame's last buffer, its status reported */
#define IGC_TX_PAYSHIFT	14

typedef struct {
	volatile UD	addr;		/* written back: the chip's own values */
	volatile UW	status;		/* before it is written back: the header's address */
	volatile UH	len;
	volatile UH	vlan;
} IGC_RXD;

typedef struct {
	volatile UD	addr;
	volatile UW	cmd;
	volatile UW	status;		/* before it is written back: the frame's length, shifted */
} IGC_TXD;

static __inline__ void igc_rx_give( IGC_RXD *d, UD pa )
{
	d->addr = pa;
	d->status = 0;
	d->len = 0;
	d->vlan = 0;
}

static __inline__ INT igc_rx_len( CONST IGC_RXD *d )
{
	UW	st = d->status;
	INT	len;

	if ( ( st & IGC_RX_DD ) == 0 ) return NIC_RX_NONE;
	if ( ( st & IGC_RX_EOP ) == 0 || ( st & IGC_RX_ERR ) ) return NIC_RX_BAD;
	len = d->len;
	return ( len >= NIC_FRAME_MIN ) ? len : NIC_RX_BAD;
}

static __inline__ void igc_tx_put( IGC_TXD *d, UD pa, UINT len )
{
	d->addr = pa;
	d->status = (UW)len << IGC_TX_PAYSHIFT;
	d->cmd = IGC_TX_CMD | IGC_TX_LAST | len;
}

/* The slot can take a frame: never used, or written back by the chip */
static __inline__ BOOL igc_tx_free( CONST IGC_TXD *d )
{
	return d->cmd == 0 || ( d->status & IGC_TX_DD ) != 0;
}

/* ---------------------------------------------------------------- the USB transfers */

/* One frame of a transfer, delivered to the caller */
typedef void (*NIC_FRAMEFN)( void *ctx, CONST UB *frame, INT len );

#define NIC_LE32(p)	( (UW)(p)[0] | ( (UW)(p)[1] << 8 ) | ( (UW)(p)[2] << 16 ) | ( (UW)(p)[3] << 24 ) )
#define NIC_PUT32(p, v)	( (p)[0] = (UB)(v), (p)[1] = (UB)( (v) >> 8 ), (p)[2] = (UB)( (v) >> 16 ), (p)[3] = (UB)( (v) >> 24 ) )

#define URE_RX_HDR	24
#define URE_RX_ALIGN	8
#define URE_RX_LENMASK	0x7fffU
#define URE_TX_HDR	8
#define URE_TX_FIRSTLAST 0xc0000000U

/*
 * The frames of one transfer of the RTL8152/8153. Answers how many were
 * good; *p_err counts what could not be understood. A length past the end
 * of the transfer stops the walk: what follows it cannot be found.
 */
static __inline__ INT ure_rx_walk( CONST UB *buf, INT len, NIC_FRAMEFN fn, void *ctx, INT *p_err )
{
	INT	at = 0, good = 0;

	while ( len - at >= URE_RX_HDR ) {
		INT	plen = (INT)( NIC_LE32(buf + at) & URE_RX_LENMASK ), adv;

		at += URE_RX_HDR;
		if ( plen <= NIC_CRC_LEN || plen > len - at ) {
			(*p_err)++;
			break;
		}
		if ( plen - NIC_CRC_LEN < NIC_FRAME_MIN ) {
			(*p_err)++;
		} else {
			fn(ctx, buf + at, plen - NIC_CRC_LEN);
			good++;
		}
		adv = ( plen + URE_RX_ALIGN - 1 ) & ~( URE_RX_ALIGN - 1 );
		at += adv;
	}
	return good;
}

/* A frame made ready for sending at out: the bytes to send */
static __inline__ INT ure_tx_wrap( UB *out, CONST UB *frame, INT len, void (*copy)( void *, CONST void *, INT ) )
{
	NIC_PUT32(out, (UW)len | URE_TX_FIRSTLAST);
	NIC_PUT32(out + 4, 0);
	copy(out + URE_TX_HDR, frame, len);
	return URE_TX_HDR + len;
}

#define AXEN_RX_MAXPKT	200		/* more in one transfer is taken for garbage */
#define AXEN_RX_LENSHIFT 16
#define AXEN_RX_LENMASK	0x1fffU
#define AXEN_RX_BAD	0xa0000000U	/* the frame was dropped, or its check sequence failed */
#define AXEN_TX_HDR	8
#define AXEN_TX_PADMARK	0x80008000U

/*
 * The frames of one transfer of the AX88179: the frames at 8 byte
 * boundaries, each with 0xee 0xee before it, then a table of their
 * headers, and last a word with the table's place and the number of
 * frames. pad is what a frame's length holds besides the frame: the two
 * marks and, on the AX88179 itself, a trailer of 4.
 */
static __inline__ INT axen_rx_walk( CONST UB *buf, INT len, INT pad, NIC_FRAMEFN fn, void *ctx, INT *p_err )
{
	UW	last;
	INT	off, count, at = 0, good = 0;

	if ( len < 4 ) {
		(*p_err)++;
		return 0;
	}
	last = NIC_LE32(buf + len - 4);
	off = (INT)( last >> 16 );
	count = (INT)( last & 0xffff );
	if ( off + 4 > len || count > AXEN_RX_MAXPKT ) {
		(*p_err)++;
		return 0;
	}
	for ( ; count > 0; count--, off += 4 ) {
		UW	h;
		INT	plen;

		if ( off + 4 > len - 4 ) {
			(*p_err)++;
			break;
		}
		h = NIC_LE32(buf + off);
		plen = (INT)( ( h >> AXEN_RX_LENSHIFT ) & AXEN_RX_LENMASK );
		if ( plen == 0 ) continue;		/* a dummy entry of the later revisions */
		if ( at + plen > len || buf[at] != 0xee || buf[at + 1] != 0xee ) {
			(*p_err)++;
			break;
		}
		if ( ( h & AXEN_RX_BAD ) || plen - pad < NIC_FRAME_MIN ) {
			(*p_err)++;
		} else {
			fn(ctx, buf + at + 2, plen - pad);
			good++;
		}
		at += ( plen + 7 ) & ~7;
	}
	return good;
}

/*
 * A frame made ready for sending at out: the bytes to send. A transfer
 * of a whole number of the bus's blocks would not end: a header of no
 * length is added, which the chip takes as padding.
 */
static __inline__ INT axen_tx_wrap( UB *out, CONST UB *frame, INT len, INT boundary,
				    void (*copy)( void *, CONST void *, INT ) )
{
	INT	n = AXEN_TX_HDR + len;

	NIC_PUT32(out, (UW)len);
	NIC_PUT32(out + 4, 0);
	copy(out + AXEN_TX_HDR, frame, len);
	if ( ( n % boundary ) == 0 ) {
		NIC_PUT32(out + n, 0);
		NIC_PUT32(out + n + 4, AXEN_TX_PADMARK);
		n += AXEN_TX_HDR;
	}
	return n;
}

#define AXE_HDR		4
#define AXE_LENMASK	0x07ffU		/* the 11 bits of a length and of its complement */
#define AXE_TX_PAD	0xffff0000U	/* a header of no length: the chip skips it */

/*
 * The receiving side of the AX88772 between transfers. The chip fills the
 * bulk stream with header, frame, header, frame..., each frame padded to
 * an even length, and the end of a transfer may fall anywhere in that
 * stream, in a header as well as in a frame: what was taken of the one
 * not finished is kept here for the next transfer.
 */
typedef struct {
	INT	want;			/* the length of the frame being gathered; 0: a header is due */
	INT	have;			/* bytes of it gathered */
	INT	hlen;			/* bytes of the header gathered */
	BOOL	pad;			/* the byte that makes the frame's length even is still to come */
	UB	hdr[AXE_HDR];
	UB	frame[AXE_LENMASK + 1];
} AXE_RX;

static __inline__ void axe_bytes( UB *d, CONST UB *s, INT n )
{
	while ( n-- > 0 ) *d++ = *s++;
}

static __inline__ void axe_rx_reset( AXE_RX *st )
{
	st->want = st->have = st->hlen = 0;
	st->pad = FALSE;
}

/* The header holds a length and its complement: the stream is in step */
static __inline__ BOOL axe_hdr_ok( CONST UB *h )
{
	UW	len = (UW)h[0] | ( (UW)h[1] << 8 ), ilen = (UW)h[2] | ( (UW)h[3] << 8 );

	return ( ( len ^ ilen ) & AXE_LENMASK ) == AXE_LENMASK;
}

/*
 * One transfer of the AX88772 added to the stream. Answers how many
 * frames it finished; *p_err counts what could not be understood. A
 * header that fails its check means the place in the stream is lost: the
 * frame being gathered and the rest of the transfer are dropped, and the
 * next transfer is read as starting with a header. When a frame goes on
 * from the transfer before and this one holds the header after it, that
 * header is checked before the frame is given: a transfer lost between
 * the two would otherwise join pieces of two frames.
 */
static __inline__ INT axe_rx_feed( AXE_RX *st, CONST UB *buf, INT len, NIC_FRAMEFN fn, void *ctx, INT *p_err )
{
	INT	at = 0, good = 0, n;

	if ( st->want > 0 ) {
		INT	next = ( st->want - st->have ) + ( st->want & 1 );

		if ( next + AXE_HDR <= len && !axe_hdr_ok(buf + next) ) {
			(*p_err)++;
			axe_rx_reset(st);
			return 0;
		}
	}
	while ( at < len ) {
		if ( st->pad ) {
			st->pad = FALSE;
			at++;
			continue;
		}
		if ( st->want == 0 ) {
			n = AXE_HDR - st->hlen;
			if ( n > len - at ) n = len - at;
			axe_bytes(st->hdr + st->hlen, buf + at, n);
			st->hlen += n;
			at += n;
			if ( st->hlen < AXE_HDR ) break;
			st->hlen = 0;
			if ( !axe_hdr_ok(st->hdr) ) {
				(*p_err)++;
				axe_rx_reset(st);
				break;
			}
			st->want = (INT)( ( (UW)st->hdr[0] | ( (UW)st->hdr[1] << 8 ) ) & AXE_LENMASK );
			st->have = 0;
			if ( st->want == 0 ) (*p_err)++;
			continue;
		}
		n = st->want - st->have;
		if ( n > len - at ) n = len - at;
		axe_bytes(st->frame + st->have, buf + at, n);
		st->have += n;
		at += n;
		if ( st->have < st->want ) break;
		if ( st->want < NIC_FRAME_MIN ) {
			(*p_err)++;
		} else {
			fn(ctx, st->frame, st->want);
			good++;
		}
		st->pad = ( st->want & 1 ) != 0;
		st->want = st->have = 0;
	}
	return good;
}

/*
 * A frame made ready for sending at out: the bytes to send. As with the
 * AX88179, a transfer of a whole number of the bus's blocks would not
 * end: a header of no length is added.
 */
static __inline__ INT axe_tx_wrap( UB *out, CONST UB *frame, INT len, INT boundary,
				   void (*copy)( void *, CONST void *, INT ) )
{
	INT	n = AXE_HDR + len;

	NIC_PUT32(out, (UW)len | ( ( ~(UW)len & 0xffffU ) << 16 ));
	copy(out + AXE_HDR, frame, len);
	if ( ( n % boundary ) == 0 ) {
		NIC_PUT32(out + n, AXE_TX_PAD);
		n += AXE_HDR;
	}
	return n;
}

#endif /* __NICRING_H__ */
