/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_igfx_aux.c (x64 PC)
 *	The DisplayPort AUX channels of Intel's integrated graphics of the
 *	9th generation, by DDI port: native AUX reads and writes of a
 *	receiver's DPCD (docs/tessronos/06-device.md 6.10).
 *
 *	Each DDI's AUX channel: A, B, C and D have their own (DP_AUX_CH_CTL at
 *	0x64010 + 0x100 * channel, five data registers after it); DDI E has
 *	none of its own and uses A's unless the VBT names another (the
 *	caller passes the channel then). A transaction: the channel not busy
 *	(waited for up to 3 ms), the request's bytes loaded into the data
 *	registers four at a time, most significant first, the control written
 *	with the request's size, the 1600 us timeout and the 9th generation's
 *	32 sync pulses (precharge and sync), then busy waited out (up to
 *	10 ms) and the done and error bits cleared; a timeout or a receive
 *	error is tried again after 400 us, five tries in all. The reply's
 *	first byte is the reply code: ACK, NACK, DEFER. A native read or write
 *	moves at most 16 bytes; a DEFER is asked again after 500 us, up to 32
 *	times, as is a NACK of a write.
 *
 *	The panel's AUX needs its power, which the firmware left on with the
 *	panel lit; an external port's needs its DDI's IO power well (disp_ext.c
 *	asks for those).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <tk/tkernel.h>
#include "sysdepend.h"

IMPORT UBINT	knl_igfx_mmio( void );			/* disp_igfx.c */

#define AUX_CTL_A		0x64010
#define AUX_CTL(ch)		( AUX_CTL_A + 0x100 * (ch) )
#define AUX_DATA(ch, i)		( AUX_CTL(ch) + 4 + 4 * (i) )
#define AUX_BUSY		( 1U << 31 )
#define AUX_DONE		( 1U << 30 )
#define AUX_TIMEOUT_ERR		( 1U << 28 )
#define AUX_TIME_1600US		( 3U << 26 )
#define AUX_RECV_ERR		( 1U << 25 )
#define AUX_SIZE(n)		( (UW)(n) << 20 )
#define AUX_SYNC_SKL		( ( 32U - 1 ) << 5 | ( 32U - 1 ) )

#define AUX_NATIVE_WRITE	0x8
#define AUX_NATIVE_READ		0x9
#define AUX_REPLY_MASK		0x30
#define AUX_REPLY_ACK		0x00
#define AUX_REPLY_NACK		0x10
#define AUX_REPLY_DEFER		0x20
#define AUX_DATA_MAX		20
#define AUX_TRIES		5
#define AUX_DPCD_RETRIES	32
#define AUX_DPCD_CHUNK		16

/* The AUX channel of each DDI port, A to E (E's own is none: A's) */
LOCAL CONST UB	ddi_aux_ch[5] = { 0, 1, 2, 3, 0 };

EXPORT INT knl_igfx_aux_ch( INT port )
{
	return ( port >= 0 && port < 5 ) ? ddi_aux_ch[port] : -1;
}

/*
 * One AUX transaction on channel ch: ntx bytes sent, the reply (its first
 * byte the reply code) into rx, at most max; how many bytes came, or an
 * error: E_BUSY the channel stayed busy, E_TMOUT no answer, E_IO a reply
 * that did not come whole
 */
EXPORT INT knl_igfx_aux_xfer( INT ch, CONST UB *tx, INT ntx, UB *rx, INT max )
{
	UBINT	m = knl_igfx_mmio();
	UW	w, st = 0;
	INT	i, k, n, t;

	if ( m == 0 || ch < 0 || ch > 3 || ntx < 1 || ntx > AUX_DATA_MAX ) return E_PAR;
	for ( i = 0; i < 30 && ( in_w(m + AUX_CTL(ch)) & AUX_BUSY ) != 0; i++ ) WaitUsec(100);
	if ( in_w(m + AUX_CTL(ch)) & AUX_BUSY ) return E_BUSY;
	for ( t = 0; t < AUX_TRIES; t++ ) {
		for ( i = 0; i < AUX_DATA_MAX / 4; i++ ) {
			w = 0;
			for ( k = 0; k < 4; k++ ) {
				if ( i * 4 + k < ntx ) w |= (UW)tx[i * 4 + k] << ( 24 - 8 * k );
			}
			out_w(m + AUX_DATA(ch, i), w);
		}
		out_w(m + AUX_CTL(ch), AUX_BUSY | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR | AUX_TIME_1600US
		      | AUX_SIZE(ntx) | AUX_SYNC_SKL);
		for ( i = 0; i < 100; i++ ) {
			WaitUsec(100);
			st = in_w(m + AUX_CTL(ch));
			if ( ( st & AUX_BUSY ) == 0 ) break;
		}
		out_w(m + AUX_CTL(ch), st | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR);
		if ( st & ( AUX_TIMEOUT_ERR | AUX_RECV_ERR ) ) {
			WaitUsec(400);
			continue;
		}
		if ( st & AUX_DONE ) break;
	}
	if ( ( st & AUX_DONE ) == 0 || ( st & AUX_BUSY ) != 0 ) return E_BUSY;
	if ( st & AUX_RECV_ERR ) return E_IO;
	if ( st & AUX_TIMEOUT_ERR ) return E_TMOUT;
	n = (INT)( ( st >> 20 ) & 0x1f );
	if ( n == 0 || n > AUX_DATA_MAX ) return E_BUSY;
	if ( n > max ) n = max;
	w = 0;
	for ( i = 0; i < n; i++ ) {
		if ( i % 4 == 0 ) w = in_w(m + AUX_DATA(ch, i / 4));
		rx[i] = (UB)( w >> ( 24 - 8 * ( i % 4 ) ) );
	}
	return n;
}

/* One native request of at most 16 bytes, asked again on DEFER (and on a write's NACK) */
LOCAL INT dpcd_once( INT ch, BOOL wr, UW addr, UB *buf, INT len )
{
	UB	tx[4 + AUX_DPCD_CHUNK], rx[AUX_DATA_MAX];
	INT	n, r, ntx = 4;

	tx[0] = (UB)( ( ( wr ? AUX_NATIVE_WRITE : AUX_NATIVE_READ ) << 4 ) | ( ( addr >> 16 ) & 0xf ) );
	tx[1] = (UB)( addr >> 8 );
	tx[2] = (UB)addr;
	tx[3] = (UB)( len - 1 );
	if ( wr ) {
		knl_memcpy(tx + 4, buf, len);
		ntx += len;
	}
	for ( r = 0; r < AUX_DPCD_RETRIES; r++ ) {
		n = knl_igfx_aux_xfer(ch, tx, ntx, rx, sizeof(rx));
		if ( n < E_OK ) return n;
		switch ( rx[0] & AUX_REPLY_MASK ) {
		case AUX_REPLY_ACK:
			if ( wr ) return len;
			if ( n < 2 ) return E_IO;
			knl_memcpy(buf, rx + 1, n - 1);
			return n - 1;
		case AUX_REPLY_NACK:
			if ( !wr ) return E_IO;
			break;
		default:
			break;
		}
		WaitUsec(500);
	}
	return E_TMOUT;
}

/* len bytes of the DPCD from addr into buf over channel ch: how many were read, or an error */
EXPORT INT knl_igfx_dpcd_read( INT ch, UW addr, UB *buf, INT len )
{
	INT	got = 0, n;

	while ( got < len ) {
		n = dpcd_once(ch, FALSE, addr + got, buf + got, ( len - got > AUX_DPCD_CHUNK ) ? AUX_DPCD_CHUNK : len - got);
		if ( n < E_OK ) return ( got > 0 ) ? got : n;
		if ( n == 0 ) break;
		got += n;
	}
	return got;
}

/* len bytes of buf written to the DPCD at addr over channel ch: how many, or an error */
EXPORT INT knl_igfx_dpcd_write( INT ch, UW addr, CONST UB *buf, INT len )
{
	UB	t[AUX_DPCD_CHUNK];
	INT	done = 0, n, k;

	while ( done < len ) {
		k = ( len - done > AUX_DPCD_CHUNK ) ? AUX_DPCD_CHUNK : len - done;
		knl_memcpy(t, buf + done, k);
		n = dpcd_once(ch, TRUE, addr + done, t, k);
		if ( n < E_OK ) return ( done > 0 ) ? done : n;
		done += n;
	}
	return done;
}

#endif /* X64 */
