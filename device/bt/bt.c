/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt.c
 *	Bluetooth on USB: the controller's HCI, the pairing of a device, and
 *	its keys and buttons taken through L2CAP as HID (design 6.8).
 *
 *	A USB controller (class 0xE0, subclass 1, protocol 1) takes commands
 *	as class requests to the device, gives events on an interrupt
 *	endpoint and ACL data on a bulk pair. The streams' callbacks run with
 *	the USB manager's lock held and cannot wait, so what comes is put in
 *	a ring, and one task per controller does all the work: it is the only
 *	one that sends and looks at the state, and the calls from outside
 *	(knl_bt_scan, knl_bt_pair) leave it a request.
 *
 *	A command is sent when the one before has answered. The start is a
 *	table of commands worked through as their answers come.
 *
 *	Pairing asks for no number: this end says it has neither a display
 *	nor a keyboard, so Secure Simple Pairing is "just works"; a device of
 *	the older pairing is offered the PIN 0000. A link key given is kept
 *	in the device's object (bt_keep), and given back when asked.
 *
 *	A device's report descriptor is asked of its service records (SDP)
 *	once the link is encrypted, and it is read in the report protocol as
 *	the descriptor says: a keyboard's reports, whatever their layout, go
 *	to the input layer in the boot protocol's eight bytes, and the rest
 *	of the device (a pointer, the consumer keys) to a slot of its own
 *	that reads them by the descriptor. A device that gives no descriptor
 *	the input layer can use is read in the boot protocol, whose reports
 *	are all that a keyboard and a mouse must speak: a report comes on the
 *	interrupt channel as 0xA1, a report ID (1 keyboard, 2 mouse) and the
 *	boot protocol's bytes. Which it is then comes from its Class of Device.
 *
 *	LE devices and earphones have their own files, bt_le.c and bt_a2dp.c,
 *	on what is here: the commands, and L2CAP frames going out in order as
 *	the controller says it has room for their packets.
 *
 *	A search is the inquiry with an LE scan beside it, then a Remote Name
 *	Request, one at a time, of each device the inquiry found that did not
 *	tell its name: what a person chooses by is its name. A pairing on
 *	BR/EDR first stops what is left of the search, as a page waits behind
 *	an inquiry or a name being asked.
 *
 *	One device may be found twice, on LE and on BR/EDR. Earphones of LE
 *	Audio on a controller that cannot carry it are paired on BR/EDR, for
 *	A2DP and HFP: chosen on LE, by their BR/EDR side when it is the same
 *	address or the one device of sound found with the same name; or, once
 *	paired on LE and found to serve nothing used here, by their identity
 *	address when it is public and their flags do not say BR/EDR is not
 *	supported, as a device of both has the one public address for both.
 *
 *	A controller on a UART (bt_uart.c, the Raspberry Pi 5's own) comes
 *	through knl_btx_open into the same task: only the way a command and an
 *	ACL packet go out and what comes back comes in differ (BTTRANS).
 *
 *	Everything that happens goes to the console ("bt: ..."): most of this
 *	can be seen working only on real controllers.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/sysdef.h>
#include <ts/conf.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "../../peripheral_kernel/obj/obj.h"
#include <ts/board.h>
#include "btint.h"

IMPORT INT  knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );
IMPORT void knl_hid_ext_detach( INT hd );
IMPORT void knl_hid_ext_report( INT hd, CONST UB *r, INT len );

#define USB_CLASS_WIRELESS	0xE0
#define USB_VID_INTEL		0x8087
#define REQ_HCI_COMMAND		0x00

#define EV_MAX			64
#define ACL_MAX			64
#define ACL_IN_XFER		64
#define BT_TSK_PRI		20
#define CMD_TMO_MS		5000	/* a command with no answer is given up, the start going on */
#define INQ_UNIT_MS		1280
#define NAME_TMO_MS		10000	/* a name asked is given up: past the page timeout of 5.12 s with room */

/* Commands: ogf << 10 | ocf */
#define C_INQUIRY	0x0401
#define C_INQ_CANCEL	0x0402
#define C_CREATE_CONN	0x0405
#define C_ACCEPT	0x0409
#define C_LK_REPLY	0x040b
#define C_LK_NEG	0x040c
#define C_PIN_REPLY	0x040d
#define C_AUTH		0x0411
#define C_ENCRYPT	0x0413
#define C_NAME_REQ	0x0419
#define C_NAME_CANCEL	0x041a
#define C_IOCAP_REPLY	0x042b
#define C_CONFIRM_REPLY	0x042c
#define C_SET_MASK	0x0c01
#define C_RESET		0x0c03
#define C_WRITE_NAME	0x0c13
#define C_SCAN_ENABLE	0x0c1a
#define C_WRITE_COD	0x0c24
#define C_INQ_MODE	0x0c45
#define C_SSP_MODE	0x0c56
#define C_READ_VER	0x1001
#define C_READ_FEATURES	0x1003
#define C_READ_ADDR	0x1009
#define C_READ_BUF	0x1005
#define C_LE_HOST	0x0c6d

/* Events */
#define E_INQ_DONE	0x01
#define E_INQ_RESULT	0x02
#define E_CONN_DONE	0x03
#define E_CONN_REQ	0x04
#define E_DISCONN	0x05
#define E_AUTH_DONE	0x06
#define E_NAME_DONE	0x07
#define E_ENC_CHANGE	0x08
#define E_CMD_DONE	0x0e
#define E_CMD_STATUS	0x0f
#define E_PIN_REQ	0x16
#define E_LK_REQ	0x17
#define E_LK_NOTE	0x18
#define E_INQ_RSSI	0x22
#define E_EXT_INQ	0x2f
#define E_IOCAP_REQ	0x31
#define E_CONFIRM_REQ	0x33
#define E_NUM_DONE	0x13
#define E_LE_META	0x3e
#define E_VENDOR	0xff

/* L2CAP */
#define CID_SIG		0x0001
#define PSM_SDP		0x0001
#define PSM_HID_CTRL	0x0011
#define PSM_HID_INTR	0x0013
#define PSM_AVDTP	0x0019
#define L_CONN_REQ	0x02
#define L_CONN_RSP	0x03
#define L_CONF_REQ	0x04
#define L_CONF_RSP	0x05
#define L_DISC_REQ	0x06
#define L_DISC_RSP	0x07
#define L_INFO_REQ	0x0a
#define L_INFO_RSP	0x0b
#define CID_CTRL	0x0040		/* our ends of the two channels */
#define CID_INTR	0x0041
#define CID_SDP		0x0042

/* HID over L2CAP */
#define HIDP_DATA_IN		0xa1
#define HIDP_SET_PROTO_BOOT	0x70
#define HIDP_SET_PROTO_REPORT	0x71
#define HIDP_RID_KBD		1
#define HIDP_RID_MOUSE		2

/* What the class of device's minor bits say of a peripheral */
#define COD_MAJOR(c)		( ( (c) >> 8 ) & 0x1f )
#define COD_MAJOR_PERIPH	5
#define COD_MINOR_KBD		0x40

/* Service discovery: what is asked, and how far it has come */
#define SDP_PDU_ERROR		0x01
#define SDP_PDU_SSA_REQ		0x06
#define SDP_PDU_SSA_RSP		0x07
#define SDP_UUID_HID		0x1124
#define SDP_UUID_SINK		0x110b
#define SDP_ATTR_PROTOCOLS	0x0004
#define COD_MAJOR_AUDIO		4
#define SDP_ATTR_HID_DESC	0x0206
#define SDP_DESC_REPORT		0x22
#define SDP_MAX_ANSWER		512	/* bytes of attributes in one answer */
#define SDP_CONT_MAX		16	/* the continuation state, as the protocol limits it */
#define SDP_TMO_MS		5000	/* a device that does not answer is read in the boot protocol */
#define SDP_NONE		0
#define SDP_RUN			1
#define SDP_DONE		2

/* HID usage pages and usages that tell the parts of a device apart */
#define UP_DESKTOP		0x01
#define UP_KEYS			0x07
#define UP_DIGITIZER		0x0d
#define UP_CONSUMER		0x0c
#define USG_POINTER		0x01
#define USG_MOUSE		0x02
#define USG_KEYBOARD		0x06
#define USG_KEYPAD		0x07
#define USG_FIRST_MOD		0xe0
#define USG_LAST_MOD		0xe7
#define BOOT_KEYS		6	/* the keys one boot protocol report holds */

IMPORT ID knl_obfile_first_vol( void );

LOCAL BTC	*bt_c = NULL;		/* the controller: one is used */

/* Whether the n bytes at a and b are the same */
EXPORT BOOL same_bt_bytes( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- streams */

/*
 * The ring: one writer at a time under rlk (a USB stream's callback, or
 * the UART's interrupt handler), one reader, the controller's task, which
 * alone makes it bigger, under rlk as well. An item's head is its kind
 * and its length in 24 bits; its bytes follow, perhaps around the end.
 */
#define RI_HEAD		4

LOCAL void ring_byte( BTC *c, UW at, UB v )
{
	c->rb[at & ( c->rcap - 1 )] = v;
}

/*
 * Room for an item of len bytes, with rlk held: its head written, where
 * its bytes go (two runs, the second perhaps empty). FALSE when it does
 * not fit, counted for the task to say; it is then made bigger.
 */
LOCAL BOOL ring_room( BTC *c, UB kind, INT len, UB **p_a, INT *p_na, UB **p_b )
{
	UW	used = c->tail - c->head;
	UW	at, off;

	if ( c->rb == NULL || len < 0 || len > 0xffffff || used + RI_HEAD + (UW)len > c->rcap ) {
		c->rlost++;
		c->rgrow = TRUE;
		return FALSE;
	}
	at = c->tail;
	ring_byte(c, at, kind);
	ring_byte(c, at + 1, (UB)len);
	ring_byte(c, at + 2, (UB)( len >> 8 ));
	ring_byte(c, at + 3, (UB)( len >> 16 ));
	off = ( at + RI_HEAD ) & ( c->rcap - 1 );
	*p_a = c->rb + off;
	*p_na = ( off + (UW)len > c->rcap ) ? (INT)( c->rcap - off ) : len;
	*p_b = c->rb;
	if ( used + RI_HEAD + (UW)len > c->rcap / 2 ) c->rgrow = TRUE;
	return TRUE;
}

/* The item made known, its bytes there before the place that says so */
LOCAL void ring_done( BTC *c, INT len )
{
	__sync_synchronize();
	c->tail += RI_HEAD + (UW)len;
}

LOCAL void ring_put( BTC *c, UB kind, DMABUF *b, INT off, INT len )
{
	UB	*pa, *pb;
	INT	na;
	UINT	im;

	ISpinLock(&c->rlk, &im);
	if ( ring_room(c, kind, len, &pa, &na, &pb) ) {
		knl_dmabuf_read(b, off, pa, na);
		if ( na < len ) knl_dmabuf_read(b, off + na, pb, len - na);
		ring_done(c, len);
	}
	ISpinUnlock(&c->rlk, &im);
	c->last_busy = knl_usb_ms();
	(void)tk_sig_sem(c->sem, 1);
}

LOCAL void ev_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	if ( err == 0 && len > 0 ) ring_put((BTC *)ctx, RK_EVENT, b, off, len);
}

LOCAL void acl_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	if ( err == 0 && len > 0 ) ring_put((BTC *)ctx, RK_ACL, b, off, len);
}

/*
 * What a way other than USB has had from the controller, a whole event
 * or ACL packet, into the ring as one item. From an interrupt handler
 * too.
 */
EXPORT void knl_btx_ring_in( BTC *c, UB kind, CONST UB *b, INT len )
{
	UB	*pa, *pb;
	INT	na;
	UINT	im;

	if ( len <= 0 ) return;
	ISpinLock(&c->rlk, &im);
	if ( ring_room(c, kind, len, &pa, &na, &pb) ) {
		knl_memcpy(pa, b, na);
		if ( na < len ) knl_memcpy(pb, b + na, len - na);
		ring_done(c, len);
	}
	ISpinUnlock(&c->rlk, &im);
	(void)tk_sig_sem(c->sem, 1);
}

/* The ring made, at the start: FALSE when there is no room for it */
EXPORT BOOL knl_btx_ring_make( BTC *c )
{
	InitSpinLock(&c->rlk);
	c->rb = (UB *)Kmalloc(RING_FIRST);
	if ( c->rb == NULL ) return FALSE;
	c->rcap = RING_FIRST;
	c->head = c->tail = 0;
	return TRUE;
}

/*
 * In the controller's task: the ring made twice as big when it was half
 * full or something did not fit, what is in it kept in order; and what
 * was lost said.
 */
EXPORT void knl_btx_ring_grow( BTC *c )
{
	UB	*nb, *ob;
	UW	used, i, lost;
	UINT	im;

	if ( !c->rgrow ) return;
	nb = ( c->rcap * 2 <= RING_LIMIT ) ? (UB *)Kmalloc(c->rcap * 2) : NULL;
	ISpinLock(&c->rlk, &im);
	lost = c->rlost;
	c->rlost = 0;
	c->rgrow = FALSE;
	ob = NULL;
	if ( nb != NULL ) {
		used = c->tail - c->head;
		for ( i = 0; i < used; i++ ) nb[i] = c->rb[( c->head + i ) & ( c->rcap - 1 )];
		ob = c->rb;
		c->rb = nb;
		c->rcap *= 2;
		c->head = 0;
		c->tail = used;
	}
	ISpinUnlock(&c->rlk, &im);
	if ( ob != NULL ) Kfree(ob);
	if ( lost > 0 ) {
		tm_printf((UB *)"bt: %d packets from the controller lost: the ring of %d bytes was full%s\n",
			  (INT)lost, (INT)c->rcap / ( nb != NULL ? 2 : 1 ), ( nb != NULL ) ? ", made bigger" :
			  ( c->rcap * 2 > RING_LIMIT ) ? ", which is as big as it may be" : ", and no room to make it bigger");
	}
}

/*
 * The next item of the ring, whole in c->ri: its kind, its bytes and
 * their length. FALSE when there is none. One whose room cannot be had
 * is let go and said.
 */
EXPORT BOOL knl_btx_ring_take( BTC *c, UB *p_kind, CONST UB **p_b, INT *p_len )
{
	for ( ;; ) {
		UW	h = c->head, m, i;
		INT	len;
		UB	kind;

		if ( h == c->tail ) return FALSE;
		__sync_synchronize();		/* the item's bytes after the place that said it is there */
		m = c->rcap - 1;
		kind = c->rb[h & m];
		len = c->rb[( h + 1 ) & m] | ( c->rb[( h + 2 ) & m] << 8 ) | ( c->rb[( h + 3 ) & m] << 16 );
		if ( len > c->ri_cap ) {
			INT	cap = ( c->ri_cap > 0 ) ? c->ri_cap : 512;
			UB	*nb;

			while ( cap < len ) cap *= 2;
			nb = (UB *)Kmalloc((SZ)cap);
			if ( nb == NULL ) {
				tm_printf((UB *)"bt: a packet of %d bytes from the controller let go: no room for it\n", len);
				c->head = h + RI_HEAD + (UW)len;
				continue;
			}
			if ( c->ri != NULL ) Kfree(c->ri);
			c->ri = nb;
			c->ri_cap = cap;
		}
		for ( i = 0; i < (UW)len; i++ ) c->ri[i] = c->rb[( h + RI_HEAD + i ) & m];
		__sync_synchronize();
		c->head = h + RI_HEAD + (UW)len;
		*p_kind = kind;
		*p_b = c->ri;
		*p_len = len;
		return TRUE;
	}
}

/* ---------------------------------------------------------------- sending */

EXPORT UW knl_btx_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return t.lo;
}

/* A command sent, its room let go: the transport has taken its bytes when it returns */
LOCAL void cmd_now( BTC *c, BTCMDQ *q )
{
	ER	er;

	c->cmd_op = (UH)( q->b[0] | ( q->b[1] << 8 ) );
	c->cmd_busy = TRUE;
	c->cmd_ms = knl_btx_now();
	er = c->gone ? E_IO : c->tr->cmd(c, q->b, q->n);
	if ( er < E_OK ) {
		tm_printf((UB *)"bt: command %04x not taken (%d)\n", c->cmd_op, (INT)er);
		c->cmd_busy = FALSE;
	}
	Kfree(q);
}

/*
 * A command, sent now or when those before it have answered, in room of
 * its own as long as it is. Its parameters' length is one byte of the
 * command: E_PAR past that.
 */
EXPORT ER knl_btx_cmd( BTC *c, UH op, CONST UB *p, INT n )
{
	BTCMDQ	*q;

	if ( n < 0 || n > HCI_PARAM_MAX || ( n > 0 && p == NULL ) ) {
		tm_printf((UB *)"bt: command %04x of %d bytes of parameters refused: they take one byte's length\n", op, n);
		return E_PAR;
	}
	q = (BTCMDQ *)Kmalloc(offsetof(BTCMDQ, b) + 3 + (SZ)n);
	if ( q == NULL ) {
		tm_printf((UB *)"bt: command %04x not sent: no room for it\n", op);
		return E_NOMEM;
	}
	q->next = NULL;
	q->n = n + 3;
	q->b[0] = (UB)op;
	q->b[1] = (UB)( op >> 8 );
	q->b[2] = (UB)n;
	if ( n > 0 ) knl_memcpy(q->b + 3, p, n);
	if ( !c->cmd_busy && c->q_first == NULL ) {
		cmd_now(c, q);
	} else {
		if ( c->q_last != NULL ) c->q_last->next = q; else c->q_first = q;
		c->q_last = q;
	}
	return E_OK;
}

LOCAL void cmd_answered( BTC *c )
{
	BTCMDQ	*q;

	c->cmd_busy = FALSE;
	while ( !c->cmd_busy && ( q = c->q_first ) != NULL ) {
		c->q_first = q->next;
		if ( c->q_first == NULL ) c->q_last = NULL;
		cmd_now(c, q);
	}
}

/* Room for one packet going out of n bytes of data */
LOCAL BOOL txp_room( BTC *c, INT n )
{
	UB	*nb;

	if ( 4 + n <= c->txp_cap ) return TRUE;
	nb = (UB *)Kmalloc((SZ)( 4 + n ));
	if ( nb == NULL ) return FALSE;
	if ( c->txp != NULL ) Kfree(c->txp);
	c->txp = nb;
	c->txp_cap = 4 + n;
	return TRUE;
}

/*
 * What is waiting goes out while the controller has room, each frame in
 * as many packets as its packet size takes. Called with no lock held.
 */
EXPORT void knl_btx_tx_pump( BTC *c )
{
	BTFRAME	*f;

	while ( ( f = c->tx_first ) != NULL ) {
		BOOL	le = f->le;
		INT	max = le ? c->le_len : c->acl_len;
		INT	*room = le ? &c->le_room : &c->acl_room;
		INT	n = f->n - c->tx_off;
		UW	pb;
		ER	er;

		if ( *room <= 0 ) return;
		if ( max <= 0 ) max = 27;		/* not told: the least a controller takes (Core 5.4 Vol 4 Part E 7.8.2) */
		if ( n > max ) n = max;
		if ( !txp_room(c, n) ) {
			tm_printf((UB *)"bt: no room for a packet of %d bytes going out: tried again later\n", n);
			return;
		}
		/* a first piece flushable on the classic link, not on LE, which has no such thing */
		pb = ( c->tx_off > 0 ) ? 0x10 : le ? 0x00 : 0x20;
		c->txp[0] = (UB)f->handle;
		c->txp[1] = (UB)( ( f->handle >> 8 ) | pb );
		c->txp[2] = (UB)n;
		c->txp[3] = (UB)( n >> 8 );
		knl_memcpy(c->txp + 4, f->b + c->tx_off, n);
		( *room )--;
		er = c->gone ? E_IO : c->tr->acl(c, c->txp, n + 4);
		if ( er < E_OK ) tm_printf((UB *)"bt: ACL out on %d failed (%d)\n", f->handle, (INT)er);
		c->tx_off += n;
		if ( c->tx_off >= f->n ) {
			c->tx_off = 0;
			c->tx_first = f->next;
			if ( c->tx_first == NULL ) c->tx_last = NULL;
			c->tx_count--;
			Kfree(f);
		}
	}
}

/*
 * Whether a stream may put that many more frames to wait: the frames
 * waiting are not limited, but a stream (A2DP, RFCOMM) goes at the pace
 * the controller takes them, as its credits would have it.
 */
EXPORT BOOL knl_btx_tx_room( BTC *c, INT frames )
{
	return (BOOL)( c->tx_count + frames <= TX_PACE );
}

/* An L2CAP frame on the connection, of a head of hn bytes and data of n: E_NOMEM, said, when there is no room */
LOCAL ER l2_send2( BTC *c, PEER *p, UH cid, CONST UB *hd, INT hn, CONST UB *data, INT n )
{
	BTFRAME	*f;
	INT	all = hn + n;

	if ( all < 0 || all > 0xffff ) {
		tm_printf((UB *)"bt: a frame of %d bytes for channel %04x refused: L2CAP's length is 16 bits\n", all, cid);
		return E_PAR;
	}
	f = (BTFRAME *)Kmalloc(offsetof(BTFRAME, b) + 4 + (SZ)all);
	if ( f == NULL ) {
		tm_printf((UB *)"bt: a frame of %d bytes for channel %04x lost: no room for it\n", all, cid);
		return E_NOMEM;
	}
	f->next = NULL;
	f->handle = p->handle;
	f->le = (BOOL)( p->le && c->le_room >= 0 && c->le_len > 0 );
	f->n = all + 4;
	f->b[0] = (UB)all;
	f->b[1] = (UB)( all >> 8 );
	f->b[2] = (UB)cid;
	f->b[3] = (UB)( cid >> 8 );
	if ( hn > 0 ) knl_memcpy(f->b + 4, hd, hn);
	if ( n > 0 ) knl_memcpy(f->b + 4 + hn, data, n);
	if ( c->tx_last != NULL ) c->tx_last->next = f; else c->tx_first = f;
	c->tx_last = f;
	c->tx_count++;
	knl_btx_tx_pump(c);
	return E_OK;
}

EXPORT ER knl_btx_l2_send( BTC *c, PEER *p, UH cid, CONST UB *data, INT n )
{
	return l2_send2(c, p, cid, NULL, 0, data, n);
}

EXPORT void knl_btx_sig_send( BTC *c, PEER *p, UB code, UB ident, CONST UB *d, INT n )
{
	UB	h[4];

	h[0] = code;
	h[1] = ident;
	h[2] = (UB)n;
	h[3] = (UB)( n >> 8 );
	(void)l2_send2(c, p, CID_SIG, h, 4, d, n);
}

/* ---------------------------------------------------------------- the devices */

LOCAL BOOL same_addr( CONST UB *a, CONST UB *b )
{
	return same_bt_bytes(a, b, 6);
}

LOCAL PEER *peer_of_addr( BTC *c, CONST UB *a )
{
	INT	i;

	for ( i = 0; i < c->npeer; i++ ) {
		PEER	*p = c->peer[i];

		if ( p->used && same_addr(p->addr, a) ) return p;
	}
	return NULL;
}

LOCAL PEER *peer_of_handle( BTC *c, UH h )
{
	INT	i;

	for ( i = 0; i < c->npeer; i++ ) {
		PEER	*p = c->peer[i];

		if ( p->used && p->up && p->handle == h ) return p;
	}
	return NULL;
}

/*
 * A device's entry: the one of its address, else one let go, else a new
 * one. NULL only when there is no memory for it. The entries are never
 * let go, so that a pointer to one stays good; the table is made anew
 * twice as big when full, the old one kept for a reader that has it.
 */
EXPORT PEER *knl_btx_peer_new( BTC *c, CONST UB *a )
{
	INT	i;
	PEER	*p = peer_of_addr(c, a);

	if ( p != NULL ) return p;
	for ( i = 0; i < c->npeer && p == NULL; i++ ) {
		if ( !c->peer[i]->used ) p = c->peer[i];
	}
	if ( p != NULL ) {
		/* what it has room for stays with it, empty */
		UB	*sb = p->sdp_buf, *rd = p->rd;
		INT	sc = p->sdp_cap, rc = p->rd_cap, idx = p->idx;
		void	*bap = p->bap;

		knl_memset(p, 0, sizeof(*p));
		p->sdp_buf = sb;
		p->sdp_cap = sc;
		p->rd = rd;
		p->rd_cap = rc;
		p->idx = idx;
		p->bap = bap;
	} else {
		if ( c->npeer >= c->peer_cap ) {
			INT	cap = ( c->peer_cap > 0 ) ? c->peer_cap * 2 : 8;
			PEER	**nt = (PEER **)Kmalloc(sizeof(PEER *) * (SZ)cap);

			if ( nt == NULL ) {
				tm_printf((UB *)"bt: no room for the table of %d devices\n", cap);
				return NULL;
			}
			for ( i = 0; i < c->npeer; i++ ) nt[i] = c->peer[i];
			__sync_synchronize();
			c->peer = nt;
			c->peer_cap = cap;
		}
		p = (PEER *)Kcalloc(1, sizeof(PEER));
		if ( p == NULL ) {
			tm_printf((UB *)"bt: no room for another device\n");
			return NULL;
		}
		p->idx = c->npeer;
		c->peer[c->npeer] = p;
		__sync_synchronize();
		c->npeer++;
	}
	p->used = TRUE;
	knl_memcpy(p->addr, a, 6);
	p->handle = 0xffff;
	return p;
}

/* Room for n bytes of a device's report descriptor, what it has kept */
EXPORT BOOL knl_btx_rd_room( PEER *p, INT n )
{
	INT	cap;
	UB	*nb;

	if ( n <= p->rd_cap ) return TRUE;
	if ( n < 0 ) return FALSE;
	for ( cap = ( p->rd_cap > 0 ) ? p->rd_cap : 256; cap < n; cap *= 2 ) ;
	nb = (UB *)Kmalloc((SZ)cap);
	if ( nb == NULL ) return FALSE;
	if ( p->rdlen > 0 ) knl_memcpy(nb, p->rd, p->rdlen);
	if ( p->rd != NULL ) Kfree(p->rd);
	p->rd = nb;
	p->rd_cap = cap;
	return TRUE;
}

/* Room for n bytes of the answer of its service records */
LOCAL BOOL sdp_room( PEER *p, INT n )
{
	INT	cap;
	UB	*nb;

	if ( n <= p->sdp_cap ) return TRUE;
	for ( cap = ( p->sdp_cap > 0 ) ? p->sdp_cap : 1024; cap < n; cap *= 2 ) ;
	nb = (UB *)Kmalloc((SZ)cap);
	if ( nb == NULL ) return FALSE;
	if ( p->sdp_got > 0 ) knl_memcpy(nb, p->sdp_buf, p->sdp_got);
	if ( p->sdp_buf != NULL ) Kfree(p->sdp_buf);
	p->sdp_buf = nb;
	p->sdp_cap = cap;
	return TRUE;
}

/*
 * A paired device kept: its object, a real object read only by the
 * system, as its link key is in it, linked from 機器の設定 with the word
 * CF_ROLE_BTDEV (a setting of the machine's that lasts, as "dev" does
 * not: its links are made again from the devices there are). It is told
 * by its metadata (tessronos.btdev); record 1 is T_BTKEPT.
 */
LOCAL BOOL bt_is_obj( CONST TS_UUID *u )
{
	UB	meta[2048];
	SZ	asz = 0;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	yes = FALSE;

	if ( key <= 0 ) return FALSE;
	if ( ob_get_atr(key, meta, sizeof(meta) - 1, &asz) >= E_OK ) {
		SZ		i;
		static CONST char mark[] = "\"btdev\"";

		for ( i = 0; !yes && i + sizeof(mark) - 1 <= asz; i++ ) {
			yes = same_bt_bytes(meta + i, mark, sizeof(mark) - 1);
		}
	}
	(void)ob_cls_obj(key);
	return yes;
}

/* 機器の設定, the object the devices kept are linked from: its UUID as text in idb */
LOCAL CONST char *bt_conf( char *idb )
{
	return knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb);
}

/* The devices kept, as many as there are, in memory of their own (Kfree it); NULL none */
LOCAL TS_UUID *bt_links( INT *p_n )
{
	char	idb[TS_UUID_STRLEN + 1];
	CONST char *conf = bt_conf(idb);
	TS_UUID	*ids;
	INT	n;

	*p_n = 0;
	n = knl_conf_links_get(conf, CF_ROLE_BTDEV, NULL, 0);
	if ( n <= 0 ) return NULL;
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( ids == NULL ) return NULL;
	*p_n = knl_conf_links_get(conf, CF_ROLE_BTDEV, ids, n);
	if ( *p_n > n ) *p_n = n;
	return ids;
}

EXPORT ER knl_btx_keep( PEER *p )
{
	static CONST char meta[] =
		"{\"name\":\"Bluetooth機器\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		"\"recordCount\":2,\"editable\":false,\"deletable\":true,\"readable\":true,\"applist\":{},"
		"\"tessronos\":{\"btdev\":true,\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	static CONST char note[] = "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>"
				   "ペアリングしたBluetoothの機器です。</p>\n</document>\n</tad>\n";
	T_BTKEPT	k;
	T_OBPRT		prt;
	T_OBCRE		cr;
	TS_UUID		box, u, *ids;
	char		idb[TS_UUID_STRLEN + 1];
	INT		n = 0, i, recno = -1;
	SZ		asz = 0;
	ID		key;
	ER		er;

	knl_memset(&k, 0, sizeof(k));
	knl_memcpy(k.addr, p->addr, 6);
	knl_memcpy(k.key, p->key, 16);
	k.cod = p->cod;
	/* the whole name, and as much of it as the first form's field takes, to a character's boundary */
	knl_memcpy(k.lname, p->name, sizeof(k.lname));
	k.lname[BT_NAME_MAX] = 0;
	{
		INT	n;

		for ( n = 0; n < (INT)sizeof(k.name) - 1 && p->name[n] != 0; n++ ) ;
		if ( p->name[n] != 0 ) {
			while ( n > 0 && ( p->name[n] & 0xc0 ) == 0x80 ) n--;
		}
		knl_memcpy(k.name, p->name, n);
		k.name[n] = 0;
	}
	k.le = (UB)p->le;
	k.atype = p->atype;
	k.le_sc = (UB)p->le_sc;
	k.key_sz = p->key_sz;
	knl_memcpy(k.ltk, p->ltk, 16);
	knl_memcpy(k.rnd, p->rnd, 8);
	k.ediv = p->ediv;
	k.has_irk = (UB)p->has_irk;
	k.audio = (UB)p->audio;
	knl_memcpy(k.irk, p->irk, 16);
	if ( ts_str_to_uuid(bt_conf(idb), &box) < E_OK ) return E_SYS;
	/* the object it has already */
	ids = bt_links(&n);
	for ( i = 0; i < n; i++ ) {
		T_BTKEPT	o;

		if ( !bt_is_obj(&ids[i]) ) continue;
		key = ob_opn_obj(&ids[i], OB_OP_R | OB_OP_WRITE);
		if ( key <= 0 ) continue;
		knl_memset(&o, 0, sizeof(o));
		if ( ob_rea_rec(key, 1, 0, &o, sizeof(o), &asz) >= E_OK && same_addr(o.addr, p->addr) ) {
			er = ob_rpl_rec(key, 1, &k, sizeof(k));
			(void)ob_cls_obj(key);
			Kfree(ids);
			return er;
		}
		(void)ob_cls_obj(key);
	}
	if ( ids != NULL ) Kfree(ids);
	knl_memset(&prt, 0, sizeof(prt));
	prt.owner = ob_user_system;
	prt.group = ob_group_admin;
	prt.mode = 0600;
	knl_memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_FILE;
	cr.json = (CONST UB *)meta;
	cr.jsonsz = (SZ)( sizeof(meta) - 1 );
	cr.near = box;
	cr.prt = &prt;
	er = ob_cre_obj(&cr, &u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	er = ( key > 0 ) ? E_OK : ( key < 0 ) ? (ER)key : E_SYS;
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, note, (SZ)( sizeof(note) - 1 ), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, &k, sizeof(k), &asz);
	if ( key > 0 ) (void)ob_cls_obj(key);
	if ( er >= E_OK ) er = knl_conf_link_add(bt_conf(idb), CF_ROLE_BTDEV, &u, "");
	if ( er < E_OK ) (void)ob_del_obj(&u);
	return er;
}

/* The devices paired before, read back from their objects */
LOCAL void bt_recall( BTC *c )
{
	TS_UUID	*ids;
	INT	n = 0, i, got = 0;

	ids = bt_links(&n);
	for ( i = 0; i < n; i++ ) {
		T_BTKEPT	k;
		SZ		asz = 0;
		ID		key;
		PEER		*p;

		if ( !bt_is_obj(&ids[i]) ) continue;
		key = ob_opn_obj(&ids[i], OB_OP_R);
		if ( key <= 0 ) continue;
		knl_memset(&k, 0, sizeof(k));
		if ( ob_rea_rec(key, 1, 0, &k, sizeof(k), &asz) >= E_OK && asz >= BTKEPT_OLD_SIZE
		  && ( p = knl_btx_peer_new(c, k.addr) ) != NULL ) {
			knl_memcpy(p->key, k.key, 16);
			p->have_key = TRUE;
			p->cod = k.cod;
			knl_memset(p->name, 0, sizeof(p->name));
			if ( asz > (SZ)BTKEPT_LE_SIZE ) {
				knl_memcpy(p->name, k.lname, BT_NAME_MAX);	/* the whole name, of this form */
			} else {
				knl_memcpy(p->name, k.name, sizeof(k.name) - 1);
			}
			p->le = (BOOL)k.le;
			p->atype = k.atype;
			p->le_sc = (BOOL)k.le_sc;
			p->key_sz = k.key_sz;
			knl_memcpy(p->ltk, k.ltk, 16);
			knl_memcpy(p->rnd, k.rnd, 8);
			p->ediv = k.ediv;
			p->has_irk = (BOOL)k.has_irk;
			p->audio = (BOOL)( k.audio || COD_MAJOR(k.cod) == COD_MAJOR_AUDIO );
			knl_memcpy(p->irk, k.irk, 16);
			tm_printf((UB *)"bt: kept device %02x:%02x:%02x:%02x:%02x:%02x \"%s\", class %06x\n",
				  k.addr[5], k.addr[4], k.addr[3], k.addr[2], k.addr[1], k.addr[0], p->name, k.cod);
			got++;
		}
		(void)ob_cls_obj(key);
	}
	if ( got != n ) tm_printf((UB *)"bt: %d of the %d devices kept read back\n", got, n);
	if ( ids != NULL ) Kfree(ids);
}

/* ---------------------------------------------------------------- the report descriptor */

/* The bits at off of a report, up to 32 of them */
LOCAL UW report_bits( CONST UB *r, INT len, INT off, INT n )
{
	UW	v = 0;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		INT	at = off + i;

		if ( ( at >> 3 ) < len && ( r[at >> 3] & ( 1U << ( at & 7 ) ) ) != 0 ) v |= (UW)1 << i;
	}
	return v;
}

/*
 * What a device's descriptor says: which parts it has (by its top level
 * collections), and where the keyboard's report has its keys.
 */
EXPORT void knl_btx_rd_layout( CONST UB *rd, INT n, KBDLAY *l )
{
	enum { PART_NONE, PART_KBD, PART_OTHER } part = PART_NONE;
	INT	o[256];
	UW	page = 0, usage = 0, umin = 0, umax = 0, rsize = 0, rcount = 0;
	UW	rid = 0;
	INT	depth = 0, i = 0;

	knl_memset(l, 0, sizeof(*l));
	l->mod_off = l->arr_off = l->bm_off = -1;
	knl_memset(o, 0, sizeof(o));
	while ( i < n ) {
		UB	pre = rd[i++];
		INT	size = pre & 3, type = ( pre >> 2 ) & 3, tag = pre >> 4, k;
		UW	v = 0;

		if ( pre == 0xfe ) {			/* a long item, of no use here */
			if ( i + 1 >= n ) return;
			i += 2 + rd[i];
			continue;
		}
		if ( size == 3 ) size = 4;
		if ( i + size > n ) return;
		for ( k = 0; k < size; k++ ) v |= (UW)rd[i + k] << ( 8 * k );
		i += size;
		if ( type == 1 ) {
			switch ( tag ) {
			case 0:	page = v; break;
			case 7:	rsize = v; break;
			case 8:	rid = v & 0xff; break;
			case 9:	rcount = v; break;
			default: break;
			}
		} else if ( type == 2 ) {
			switch ( tag ) {
			case 0:	usage = v; break;
			case 1:	umin = v; break;
			case 2:	umax = v; break;
			default: break;
			}
		} else if ( type == 0 ) {
			if ( tag == 10 ) {
				if ( depth == 0 && v == 1 ) {
					UW	up = ( size == 4 ) ? ( usage >> 16 ) : page, u = usage & 0xffff;

					if ( up == UP_DESKTOP && ( u == USG_KEYBOARD || u == USG_KEYPAD ) ) {
						part = PART_KBD;
					} else if ( up == UP_CONSUMER ) {
						part = PART_OTHER;
						l->has_cc = TRUE;
					} else if ( ( up == UP_DESKTOP && ( u == USG_POINTER || u == USG_MOUSE ) )
						 || up == UP_DIGITIZER ) {
						part = PART_OTHER;
						l->has_ptr = TRUE;
					} else {
						part = PART_OTHER;
					}
				}
				depth++;
			} else if ( tag == 12 ) {
				if ( --depth <= 0 ) {
					depth = 0;
					part = PART_NONE;
				}
			} else if ( tag == 8 ) {
				/* a keyboard's keys, in its first report that has any */
				if ( part == PART_KBD && page == UP_KEYS && ( v & 1 ) == 0
				  && ( !l->has_kbd || l->rid == (INT)rid ) ) {
					BOOL	var = (BOOL)( ( v & 2 ) != 0 );

					if ( var && rsize == 1 && umin >= USG_FIRST_MOD && umax <= USG_LAST_MOD ) {
						if ( rcount == 8 ) l->mod_off = o[rid];
					} else if ( var && rsize == 1 ) {
						l->bm_off = o[rid];
						l->bm_min = (INT)umin;
						l->bm_n = (INT)rcount;
					} else if ( !var && rsize == 8 ) {
						l->arr_off = o[rid];
						l->arr_n = ( rcount > BOOT_KEYS ) ? BOOT_KEYS : (INT)rcount;
					}
					if ( l->mod_off >= 0 || l->bm_off >= 0 || l->arr_off >= 0 ) {
						l->has_kbd = TRUE;
						l->rid = (INT)rid;
					}
				}
				if ( rsize * rcount < 0x10000 ) o[rid] += (INT)( rsize * rcount );
			}
			usage = umin = umax = 0;
		}
	}
}

/* A keyboard's report as the boot protocol has it, r being what comes after the ID */
LOCAL void kbd_boot( CONST KBDLAY *l, CONST UB *r, INT len, UB *k )
{
	INT	i, n = 2;

	knl_memset(k, 0, 8);
	if ( l->mod_off >= 0 ) k[0] = (UB)report_bits(r, len, l->mod_off, 8);
	if ( l->arr_off >= 0 ) {
		for ( i = 0; i < l->arr_n; i++ ) {
			UB	u = (UB)report_bits(r, len, l->arr_off + 8 * i, 8);

			if ( u != 0 && n < 2 + BOOT_KEYS ) k[n++] = u;
		}
	}
	if ( l->bm_off >= 0 ) {
		for ( i = 0; i < l->bm_n; i++ ) {
			INT	u = l->bm_min + i;

			if ( report_bits(r, len, l->bm_off + i, 1) == 0 ) continue;
			if ( u >= USG_FIRST_MOD && u <= USG_LAST_MOD ) {
				k[0] |= (UB)( 1 << ( u - USG_FIRST_MOD ) );
			} else if ( n < 2 + BOOT_KEYS ) {
				k[n++] = (UB)u;
			}
		}
	}
}

/* The head of a data element of SDP: its type, and how long its head and its content are */
LOCAL BOOL de_head( CONST UB *b, INT n, INT *p_type, INT *p_hdr, INT *p_len )
{
	static CONST UB	fixed[5] = { 1, 2, 4, 8, 16 };
	INT		sz;

	if ( n < 1 ) return FALSE;
	*p_type = b[0] >> 3;
	sz = b[0] & 7;
	if ( *p_type == 0 ) {
		*p_hdr = 1;
		*p_len = 0;
	} else if ( sz < 5 ) {
		*p_hdr = 1;
		*p_len = fixed[sz];
	} else if ( sz == 5 ) {
		if ( n < 2 ) return FALSE;
		*p_hdr = 2;
		*p_len = b[1];
	} else if ( sz == 6 ) {
		if ( n < 3 ) return FALSE;
		*p_hdr = 3;
		*p_len = ( b[1] << 8 ) | b[2];
	} else {
		if ( n < 5 || b[1] != 0 || b[2] != 0 ) return FALSE;
		*p_hdr = 5;
		*p_len = ( b[3] << 8 ) | b[4];
	}
	return (BOOL)( *p_hdr + *p_len <= n );
}

#define DE_UINT		1
#define DE_STRING	4
#define DE_SEQ		6

/* The report descriptor among a HID service's descriptors: (type, bytes) pairs */
LOCAL BOOL de_report_desc( CONST UB *b, INT n, CONST UB **p_rd, INT *p_len )
{
	INT	t, h, l, t2, h2, l2, at, in;

	/* the list of pairs */
	for ( at = 0; at < n && de_head(b + at, n - at, &t, &h, &l); at += h + l ) {
		CONST UB *pair = b + at + h;

		if ( t != DE_SEQ || !de_head(pair, l, &t2, &h2, &l2) || t2 != DE_UINT || l2 != 1 ) continue;
		if ( pair[h2] != SDP_DESC_REPORT ) continue;
		in = h2 + l2;
		if ( !de_head(pair + in, l - in, &t2, &h2, &l2) || t2 != DE_STRING ) continue;
		*p_rd = pair + in + h2;
		*p_len = l2;
		return TRUE;
	}
	return FALSE;
}

/* The report descriptor in the answer: the lists of a record's (attribute, value) pairs */
LOCAL BOOL sdp_find_rd( CONST UB *b, INT n, CONST UB **p_rd, INT *p_len )
{
	INT	t, h, l, rt, rh, rl, at;

	if ( !de_head(b, n, &t, &h, &l) || t != DE_SEQ ) return FALSE;
	b += h;
	n = l;
	for ( at = 0; at < n && de_head(b + at, n - at, &rt, &rh, &rl); at += rh + rl ) {
		CONST UB *rec = b + at + rh;
		INT	in = 0;

		if ( rt != DE_SEQ ) continue;
		while ( in < rl && de_head(rec + in, rl - in, &t, &h, &l) ) {
			INT	vt, vh, vl, id;

			if ( t != DE_UINT || l != 2 ) break;
			id = ( rec[in + h] << 8 ) | rec[in + h + 1];
			in += h + l;
			if ( !de_head(rec + in, rl - in, &vt, &vh, &vl) ) break;
			if ( id == SDP_ATTR_HID_DESC && vt == DE_SEQ ) {
				return de_report_desc(rec + in + vh, vl, p_rd, p_len);
			}
			in += vh + vl;
		}
	}
	return FALSE;
}

/* ---------------------------------------------------------------- L2CAP */

EXPORT void knl_btx_l2_open( BTC *c, PEER *p, UH psm, UH scid )
{
	UB	d[4];

	d[0] = (UB)psm;
	d[1] = (UB)( psm >> 8 );
	d[2] = (UB)scid;
	d[3] = (UB)( scid >> 8 );
	knl_btx_sig_send(c, p, L_CONN_REQ, ++p->ident, d, 4);
}

LOCAL void l2_config( BTC *c, PEER *p, UH dcid )
{
	UB	d[8];

	d[0] = (UB)dcid;
	d[1] = (UB)( dcid >> 8 );
	d[2] = d[3] = 0;
	d[4] = 1;			/* the MTU this end takes */
	d[5] = 2;
	d[6] = (UB)L2_MTU;
	d[7] = (UB)( L2_MTU >> 8 );
	knl_btx_sig_send(c, p, L_CONF_REQ, ++p->ident, d, 8);
}

LOCAL void hid_ready( BTC *c, PEER *p )
{
	BOOL	kbd = (BOOL)( ( p->cod & COD_MINOR_KBD ) != 0 );
	UB	b;

	if ( p->hid_on || p->sdp != SDP_DONE ) return;
	p->hid_on = TRUE;
	p->report_proto = (BOOL)( p->lay.has_kbd || p->lay.has_ptr || p->lay.has_cc );
	b = p->report_proto ? HIDP_SET_PROTO_REPORT : HIDP_SET_PROTO_BOOT;
	if ( p->rcid_ctrl != 0 ) knl_btx_l2_send(c, p, p->rcid_ctrl, &b, 1);
	knl_usb_lock();
	if ( p->report_proto ) {
		if ( p->lay.has_kbd ) p->hd = knl_hid_ext_attach(TRUE, NULL, 0, (CONST UB *)"btkbd");
		if ( p->lay.has_ptr || p->lay.has_cc ) {
			p->hd2 = knl_hid_ext_attach(FALSE, p->rd, p->rdlen, (CONST UB *)"bthid");
		}
	} else {
		p->hd = knl_hid_ext_attach(kbd, NULL, 0, (CONST UB *)( kbd ? "btkbd" : "btmouse" ));
	}
	knl_usb_unlock();
	if ( p->report_proto ) {
		tm_printf((UB *)"bt: \"%s\" taken in the report protocol: keyboard slot %d, ID %d; other slot %d\n",
			  p->name, p->hd, p->lay.rid, p->hd2);
	} else {
		tm_printf((UB *)"bt: \"%s\" taken as a %s, boot protocol: input slot %d\n", p->name, kbd ? "keyboard" : "mouse", p->hd);
	}
}

/* The input layer's slots of a device let go. With the manager's lock held */
LOCAL void hid_let_go( PEER *p )
{
	if ( p->hd > 0 ) knl_hid_ext_detach(p->hd);
	if ( p->hd2 > 0 ) knl_hid_ext_detach(p->hd2);
	p->hd = p->hd2 = 0;
	p->hid_on = FALSE;
}

/* The question to the service records: the HID service's descriptors, from where the answer stopped */
LOCAL void sdp_ask( BTC *c, PEER *p, CONST UB *cont, INT nc )
{
	UB	b[5 + 13 + SDP_CONT_MAX];
	INT	n = 5;
	UH	uuid = p->audio ? SDP_UUID_SINK : SDP_UUID_HID;
	UH	attr = p->audio ? SDP_ATTR_PROTOCOLS : SDP_ATTR_HID_DESC;

	p->sdp_asked = TRUE;
	p->sdp_tid++;
	b[0] = SDP_PDU_SSA_REQ;
	b[1] = (UB)( p->sdp_tid >> 8 );
	b[2] = (UB)p->sdp_tid;
	b[n++] = 0x35;			/* the service: a list of one UUID */
	b[n++] = 3;
	b[n++] = 0x19;
	b[n++] = (UB)( uuid >> 8 );
	b[n++] = (UB)uuid;
	b[n++] = (UB)( SDP_MAX_ANSWER >> 8 );
	b[n++] = (UB)SDP_MAX_ANSWER;
	b[n++] = 0x35;			/* the attributes: a list of one */
	b[n++] = 3;
	b[n++] = 0x09;
	b[n++] = (UB)( attr >> 8 );
	b[n++] = (UB)attr;
	b[n++] = (UB)nc;
	if ( nc > 0 ) knl_memcpy(b + n, cont, nc);
	n += nc;
	b[3] = (UB)( ( n - 5 ) >> 8 );
	b[4] = (UB)( n - 5 );
	knl_btx_l2_send(c, p, p->rcid_sdp, b, n);
}

LOCAL void chan_up( BTC *c, PEER *p );

/* The service records are done with: the HID channels may go on */
LOCAL void sdp_end( BTC *c, PEER *p )
{
	if ( p->sdp != SDP_RUN ) return;
	p->sdp = SDP_DONE;
	if ( p->rcid_sdp != 0 ) {
		UB	r[4];

		r[0] = (UB)p->rcid_sdp;
		r[1] = (UB)( p->rcid_sdp >> 8 );
		r[2] = (UB)CID_SDP;
		r[3] = (UB)( CID_SDP >> 8 );
		knl_btx_sig_send(c, p, L_DISC_REQ, ++p->ident, r, 4);
		p->rcid_sdp = 0;
	}
	if ( p->audio ) {
		knl_bta_open(c, p);
		return;
	}
	if ( p->rdlen == 0 ) tm_printf((UB *)"bt: \"%s\": no report descriptor, boot protocol\n", p->name);
	if ( p->rcid_ctrl == 0 ) knl_btx_l2_open(c, p, PSM_HID_CTRL, CID_CTRL);
	else chan_up(c, p);
}

/* An answer of the service records, perhaps one piece of several */
LOCAL void sdp_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	INT	cnt, nc;

	if ( p->sdp != SDP_RUN ) return;
	if ( n < 7 || f[0] != SDP_PDU_SSA_RSP ) {
		tm_printf((UB *)"bt: \"%s\": the service records answered %02x\n", p->name, ( n > 0 ) ? f[0] : 0);
		sdp_end(c, p);
		return;
	}
	cnt = ( f[5] << 8 ) | f[6];
	if ( 7 + cnt >= n ) {
		tm_printf((UB *)"bt: \"%s\": an answer of the service records cut short (%d bytes said, %d came)\n", p->name, cnt, n - 8);
		sdp_end(c, p);
		return;
	}
	if ( !sdp_room(p, p->sdp_got + cnt) ) {
		tm_printf((UB *)"bt: \"%s\": no room for %d bytes of its service records\n", p->name, p->sdp_got + cnt);
		sdp_end(c, p);
		return;
	}
	knl_memcpy(p->sdp_buf + p->sdp_got, f + 7, cnt);
	p->sdp_got += cnt;
	nc = f[7 + cnt];
	if ( nc > 0 ) {
		if ( nc > SDP_CONT_MAX || 8 + cnt + nc > n ) {
			sdp_end(c, p);
			return;
		}
		sdp_ask(c, p, f + 8 + cnt, nc);
		return;
	}
	if ( p->audio ) {
		knl_bta_sdp_answer(c, p, p->sdp_got);
	} else {
		CONST UB	*rd = NULL;
		INT		len = 0;

		p->rdlen = 0;
		if ( sdp_find_rd(p->sdp_buf, p->sdp_got, &rd, &len) && len > 0 && knl_btx_rd_room(p, len) ) {
			knl_memcpy(p->rd, rd, len);
			p->rdlen = len;
			knl_btx_rd_layout(p->rd, p->rdlen, &p->lay);
			tm_printf((UB *)"bt: \"%s\": report descriptor of %d bytes: keyboard %d (ID %d), pointer %d, consumer %d\n",
				  p->name, len, p->lay.has_kbd, p->lay.rid, p->lay.has_ptr, p->lay.has_cc);
		} else if ( len > 0 ) {
			tm_printf((UB *)"bt: \"%s\": no room for a report descriptor of %d bytes\n", p->name, len);
		}
	}
	sdp_end(c, p);
}

/* Once the link is encrypted: the descriptor, unless it is known from before */
LOCAL void sdp_start( BTC *c, PEER *p )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	p->sdp = SDP_RUN;
	p->sdp_asked = FALSE;
	p->sdp_got = 0;
	p->sdp_due = t.lo + SDP_TMO_MS;
	knl_btx_l2_open(c, p, PSM_SDP, CID_SDP);
}

/* The bits of the state kept for each of the device's channels, and its end of it */
LOCAL UB chan_bit( UH ours )
{
	return CID_IS_X(ours) ? 0 : ( ours == CID_CTRL ) ? CHB_CTRL : ( ours == CID_SDP ) ? CHB_SDP
	     : ( ours == CID_AV_SIG ) ? CHB_AV_SIG : ( ours == CID_AV_MEDIA ) ? CHB_AV_MEDIA : CHB_INTR;
}

LOCAL UH *chan_rcid( PEER *p, UH ours )
{
	return CID_IS_X(ours) ? &p->rcid_x[ours - CID_X_FIRST] : ( ours == CID_CTRL ) ? &p->rcid_ctrl : ( ours == CID_SDP ) ? &p->rcid_sdp
	     : ( ours == CID_AV_SIG ) ? &p->rcid_av : ( ours == CID_AV_MEDIA ) ? &p->rcid_media : &p->rcid_intr;
}

LOCAL void chan_up( BTC *c, PEER *p )
{
	knl_bta_x_up(c, p);
	if ( ( p->cfg_in & 4 ) && ( p->cfg_out & 4 ) && p->sdp == SDP_RUN && !p->sdp_asked ) {
		sdp_ask(c, p, NULL, 0);
	}
	if ( p->audio ) {
		knl_bta_chan_up(c, p, (UB)( p->cfg_in & p->cfg_out ));
		return;
	}
	if ( ( p->cfg_in & 1 ) && ( p->cfg_out & 1 ) && p->rcid_intr == 0 ) {
		/* the control channel is up: ours opens the interrupt one, unless the device has */
		knl_btx_l2_open(c, p, PSM_HID_INTR, CID_INTR);
	}
	if ( ( p->cfg_in & 2 ) && ( p->cfg_out & 2 ) && ( p->cfg_in & 1 ) && ( p->cfg_out & 1 ) ) {
		hid_ready(c, p);
	}
}

LOCAL void sig_in( BTC *c, PEER *p, CONST UB *d, INT n )
{
	while ( n >= 4 ) {
		UB	code = d[0], ident = d[1];
		INT	len = d[2] | ( d[3] << 8 );
		UB	r[12];

		if ( len + 4 > n ) return;
		/* a command shorter than its fields is passed over: they would be read past what came */
		if ( len < ( ( code == L_CONN_RSP ) ? 6 : ( code == L_CONN_REQ || code == L_DISC_REQ ) ? 4
			   : ( code == L_CONF_REQ || code == L_CONF_RSP || code == L_INFO_REQ ) ? 2 : 0 ) ) {
			tm_printf((UB *)"bt: \"%s\": a signalling command %02x of %d bytes, too short, passed over\n", p->name, code, len);
			d += len + 4;
			n -= len + 4;
			continue;
		}
		switch ( code ) {
		case L_CONN_REQ: {
			UH	psm = (UH)( d[4] | ( d[5] << 8 ) ), scid = (UH)( d[6] | ( d[7] << 8 ) ), ours;

			/* of the two channels of AVDTP, the signalling comes first */
			ours = ( psm == PSM_HID_CTRL ) ? CID_CTRL
			     : ( psm == PSM_AVDTP ) ? ( ( p->rcid_av == 0 ) ? CID_AV_SIG : CID_AV_MEDIA ) : CID_INTR;
			r[0] = (UB)ours;
			r[1] = (UB)( ours >> 8 );
			r[2] = (UB)scid;
			r[3] = (UB)( scid >> 8 );
			r[4] = r[5] = r[6] = r[7] = 0;
			if ( psm == PSM_AVDTP && p->audio ) {
				*chan_rcid(p, ours) = scid;
				knl_btx_sig_send(c, p, L_CONN_RSP, ident, r, 8);
				l2_config(c, p, scid);
				break;
			}
			if ( psm != PSM_HID_CTRL && psm != PSM_HID_INTR && psm != PSM_AVDTP
			  && ( ours = knl_bta_accept(c, p, psm) ) != 0 ) {
				/* the audio side's: this end's records, the remote control, RFCOMM */
				r[0] = (UB)ours;
				r[1] = (UB)( ours >> 8 );
				*chan_rcid(p, ours) = scid;
				knl_btx_sig_send(c, p, L_CONN_RSP, ident, r, 8);
				l2_config(c, p, scid);
				break;
			}
			if ( psm != PSM_HID_CTRL && psm != PSM_HID_INTR ) {
				r[0] = r[1] = 0;
				r[4] = 2;	/* the PSM is not one served */
				knl_btx_sig_send(c, p, L_CONN_RSP, ident, r, 8);
				break;
			}
			if ( psm == PSM_HID_CTRL ) p->rcid_ctrl = scid; else p->rcid_intr = scid;
			knl_btx_sig_send(c, p, L_CONN_RSP, ident, r, 8);
			l2_config(c, p, scid);
			break;
		}
		case L_CONN_RSP: {
			UH	dcid = (UH)( d[4] | ( d[5] << 8 ) ), scid = (UH)( d[6] | ( d[7] << 8 ) );
			UH	res = (UH)( d[8] | ( d[9] << 8 ) );

			if ( res == 1 ) break;	/* pending: another answer comes */
			if ( res != 0 ) {
				tm_printf((UB *)"bt: \"%s\": channel %04x refused (%d)\n", p->name, scid, res);
				if ( scid == CID_SDP ) sdp_end(c, p);
				if ( CID_IS_X(scid) ) knl_bta_chan_closed(c, p, scid);
				break;
			}
			*chan_rcid(p, scid) = dcid;
			l2_config(c, p, dcid);
			break;
		}
		case L_CONF_REQ: {
			UH	dcid = (UH)( d[4] | ( d[5] << 8 ) );
			UH	peer_end = *chan_rcid(p, dcid);
			INT	o = 8;

			/* its options: what is wanted of them is the MTU it takes on the media channel */
			while ( ( dcid == CID_AV_MEDIA || CID_IS_X(dcid) ) && o + 2 <= len + 4 ) {
				if ( ( d[o] & 0x7f ) == 1 && d[o + 1] == 2 && o + 4 <= len + 4 ) {
					UH	mtu = (UH)( d[o + 2] | ( d[o + 3] << 8 ) );

					/* every device takes 48 bytes on a channel (Core 5.4 Vol 3 Part A 5.1): less is not one to go by */
					if ( mtu < 48 ) mtu = 48;

					if ( dcid == CID_AV_MEDIA ) p->media_mtu = mtu;
					else p->mtu_x[dcid - CID_X_FIRST] = mtu;
				}
				o += 2 + d[o + 1];
			}
			r[0] = (UB)peer_end;
			r[1] = (UB)( peer_end >> 8 );
			r[2] = r[3] = r[4] = r[5] = 0;
			knl_btx_sig_send(c, p, L_CONF_RSP, ident, r, 6);
			p->cfg_in |= chan_bit(dcid);
			if ( CID_IS_X(dcid) ) p->cfgx_in |= (UB)( 1 << ( dcid - CID_X_FIRST ) );
			chan_up(c, p);
			break;
		}
		case L_CONF_RSP: {
			UH	scid = (UH)( d[4] | ( d[5] << 8 ) );

			p->cfg_out |= chan_bit(scid);
			if ( CID_IS_X(scid) ) p->cfgx_out |= (UB)( 1 << ( scid - CID_X_FIRST ) );
			chan_up(c, p);
			break;
		}
		case L_DISC_REQ:
			knl_bta_chan_closed(c, p, (UH)( d[4] | ( d[5] << 8 ) ));
			r[0] = d[4];
			r[1] = d[5];
			r[2] = d[6];
			r[3] = d[7];
			knl_btx_sig_send(c, p, L_DISC_RSP, ident, r, 4);
			break;
		case L_INFO_REQ:
			r[0] = d[4];
			r[1] = d[5];
			r[2] = 1;	/* not supported */
			r[3] = 0;
			knl_btx_sig_send(c, p, L_INFO_RSP, ident, r, 4);
			break;
		default:
			break;
		}
		d += len + 4;
		n -= len + 4;
	}
}

/* A report as HID has it, with its ID if the device numbers them, into the input layer */
EXPORT void knl_btx_report_in( PEER *p, CONST UB *r, INT m )
{
	knl_usb_lock();
	if ( !p->report_proto ) {
		if ( p->hd > 0 ) knl_hid_ext_report(p->hd, r + 1, m - 1);
	} else if ( p->hd > 0 && ( p->lay.rid == 0 || r[0] == p->lay.rid ) ) {
		UB	k[8];
		INT	skip = ( p->lay.rid != 0 ) ? 1 : 0;

		kbd_boot(&p->lay, r + skip, m - skip, k);
		knl_hid_ext_report(p->hd, k, 8);
	} else if ( p->hd2 > 0 ) {
		knl_hid_ext_report(p->hd2, r, m);
	}
	knl_usb_unlock();
}

/* One whole L2CAP frame */
LOCAL void l2_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	INT	len, cid;

	if ( n < 4 ) return;
	len = f[0] | ( f[1] << 8 );
	cid = f[2] | ( f[3] << 8 );
	if ( len + 4 > n ) return;
	f += 4;
	if ( cid == CID_SIG ) {
		sig_in(c, p, f, len);
	} else if ( cid == CID_SDP ) {
		sdp_in(c, p, f, len);
	} else if ( cid == CID_INTR && len >= 2 && f[0] == HIDP_DATA_IN ) {
		knl_btx_report_in(p, f + 1, len - 1);
	} else if ( cid == CID_ATT || cid == CID_LE_SIG || cid == CID_SMP ) {
		if ( p->le ) knl_btl_l2_in(c, p, cid, f, len);
	} else if ( cid == CID_AV_SIG ) {
		knl_bta_sig_in(c, p, f, len);
	} else if ( CID_IS_X(cid) ) {
		knl_bta_x_in(c, p, (UH)cid, f, len);
	}
}

LOCAL void acl_in( BTC *c, CONST UB *b, INT n )
{
	UH	h;
	INT	pb, len;
	PEER	*p;

	if ( n < 4 ) return;
	h = (UH)( ( b[0] | ( b[1] << 8 ) ) & 0x0fff );
	pb = ( b[1] >> 4 ) & 3;
	len = b[2] | ( b[3] << 8 );
	p = peer_of_handle(c, h);
	if ( p == NULL || len + 4 > n ) return;
	b += 4;
	if ( pb != 1 ) {
		p->acl_have = 0;
		p->acl_want = ( len >= 2 ) ? ( b[0] | ( b[1] << 8 ) ) + 4 : 0;
	}
	if ( p->acl_have + len > (INT)sizeof(p->acl) ) {
		p->acl_have = p->acl_want = 0;	/* longer than the MTU this end said: nothing here wants it */
		return;
	}
	knl_memcpy(p->acl + p->acl_have, b, len);
	p->acl_have += len;
	if ( p->acl_want > 0 && p->acl_have >= p->acl_want ) {
		l2_in(c, p, p->acl, p->acl_have);
		p->acl_have = p->acl_want = 0;
	}
}

/* ---------------------------------------------------------------- HCI */

/* The start given up: the adapter stays there, saying why, and asks nothing more */
LOCAL void start_stuck( BTC *c, UH op, UH status )
{
	tm_printf((UB *)"bt: the start stopped at step %d: command %04x %s %02x; the adapter is not usable\n",
		  c->step, op, ( status == 0xffff ) ? "got no answer," : "answered status", status & 0xff);
	c->st.state = BT_ST_NOANSWER;
	c->st.op = op;
	c->st.status = status;
	c->step = -1;
}

/* The start: each sent when the one before has answered */
LOCAL void start_next( BTC *c )
{
	/*
	 * Bit 46, the sixth byte's 0x40, lets the extended inquiry results
	 * through: with the inquiry mode below a device that tells its name
	 * comes in that event only, and a controller drops it when masked.
	 * Bit 61, the last byte's 0x20, lets the LE events through.
	 */
	static CONST UB	mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x5f, 0xff, 0x20 };
	/* a connection, a report of what advertises, a change of parameters */
	static CONST UB	lemask[8] = { 0x1f, 0, 0, 0, 0, 0, 0, 0 };
	/* a computer, a desktop one; its services capturing (bit 19), audio (21) and telephony (22) */
	static CONST UB	cod[3] = { 0x04, 0x01, 0x68 };
	UB		v[248];

	switch ( c->step++ ) {
	case 0: case 1:
		start_next(c);
		break;
	case 2:
		knl_btx_cmd(c, C_RESET, NULL, 0);
		break;
	case 3:	knl_btx_cmd(c, C_READ_VER, NULL, 0); break;
	case 4:	knl_btx_cmd(c, C_READ_FEATURES, NULL, 0); break;
	case 5:	knl_btx_cmd(c, C_READ_ADDR, NULL, 0); break;
	case 6:	knl_btx_cmd(c, C_READ_BUF, NULL, 0); break;
	case 7:	knl_btx_cmd(c, C_SET_MASK, mask, 8); break;
	case 8:	v[0] = 1; knl_btx_cmd(c, C_SSP_MODE, v, 1); break;
	case 9:	v[0] = 2; knl_btx_cmd(c, C_INQ_MODE, v, 1); break;	/* results with the signal's strength and their names */
	case 10: knl_btx_cmd(c, C_WRITE_COD, cod, 3); break;
	case 11: v[0] = 2; knl_btx_cmd(c, C_SCAN_ENABLE, v, 1); break;	/* a kept device may connect; nobody finds this one */
	case 12:
		if ( c->has_le ) {
			v[0] = 1;
			v[1] = 0;
			knl_btx_cmd(c, C_LE_HOST, v, 2);
		} else {
			c->step = 18;
			start_next(c);
		}
		break;
	case 13: knl_btx_cmd(c, C_LE_READ_FEAT, NULL, 0); break;	/* what it can do of LE (bt_iso.c) */
	case 14:
		/* and the extended advertising reports (bit 12) and the CISes made (bit 24), if it has them */
		knl_memcpy(v, lemask, 8);
		if ( c->has_ext_adv ) v[1] |= 0x10;
		if ( c->has_cis ) v[3] |= 0x01;
		knl_btx_cmd(c, C_LE_SET_MASK, v, 8);
		break;
	case 15: knl_btx_cmd(c, C_LE_READ_BUF, NULL, 0); break;
	case 16:
		if ( c->has_cis ) knl_btx_cmd(c, C_LE_READ_BUF_V2, NULL, 0);	/* the ISO buffers */
		else start_next(c);
		break;
	case 17:
		if ( c->has_cis ) {
			v[0] = 32;		/* the host's support of CISes */
			v[1] = 1;
			knl_btx_cmd(c, C_LE_HOST_FEAT, v, 2);
		} else {
			start_next(c);
		}
		break;
	case -1:
		c->step = -1;		/* a controller left in its boot loader goes no further */
		break;
	default:
		if ( !c->ready && same_bt_bytes(c->addr, "\0\0\0\0\0\0", 6) ) {
			/* it never said its address: its commands are not working, whatever else it answered */
			start_stuck(c, C_READ_ADDR, c->addr_status);
			break;
		}
		if ( !c->ready ) {
			c->ready = TRUE;
			c->st.state = BT_ST_READY;
			tm_printf((UB *)"bt: controller ready, address %02x:%02x:%02x:%02x:%02x:%02x\n",
				  c->addr[5], c->addr[4], c->addr[3], c->addr[2], c->addr[1], c->addr[0]);
		}
		break;
	}
}

/* The device the search found at that address, NULL none */
EXPORT BTFOUND *knl_btx_found_at( BTC *c, CONST UB *addr )
{
	INT	i;

	for ( i = 0; i < c->nfound; i++ ) {
		if ( same_addr(c->found[i]->f.addr, addr) ) return c->found[i];
	}
	return NULL;
}

/*
 * A device the search found: the entry of its address, once however
 * often it is seen, else a new one at the end, as many as there are.
 * NULL only when there is no memory for it, said on the console.
 */
EXPORT BTFOUND *knl_btx_found_add( BTC *c, CONST UB *addr, UW cod, CONST UB *name, INT nlen )
{
	BTFOUND	*e = knl_btx_found_at(c, addr);
	T_BTFOUND *f;

	if ( e == NULL ) {
		INT	i = c->nfound;

		if ( i >= c->found_made ) {
			/* an entry not had before: the table made bigger first if it is full */
			if ( c->found_made >= c->found_cap ) {
				INT	cap = ( c->found_cap > 0 ) ? c->found_cap * 2 : 32, k;
				BTFOUND	**nt = (BTFOUND **)Kmalloc(sizeof(BTFOUND *) * (SZ)cap);

				if ( nt == NULL ) {
					tm_printf((UB *)"bt: no room for the table of %d devices found\n", cap);
					return NULL;
				}
				for ( k = 0; k < c->found_made; k++ ) nt[k] = c->found[k];
				__sync_synchronize();
				c->found = nt;		/* the old table is kept: a reader may be in it */
				c->found_cap = cap;
			}
			e = (BTFOUND *)Kmalloc(sizeof(BTFOUND));
			if ( e == NULL ) {
				tm_printf((UB *)"bt: no room for device %d found\n", i + 1);
				return NULL;
			}
			c->found[c->found_made++] = e;
		}
		e = c->found[i];
		knl_memset(e, 0, sizeof(*e));
		knl_memcpy(e->f.addr, addr, 6);
		__sync_synchronize();		/* the entry before the count that shows it */
		c->nfound = i + 1;
		tm_printf((UB *)"bt: found %02x:%02x:%02x:%02x:%02x:%02x, class %06x\n",
			  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], cod);
	}
	f = &e->f;
	if ( cod != 0 ) f->cod = cod;		/* LE has none, and does not take away what the inquiry gave */
	if ( name != NULL && nlen > 0 ) {
		BOOL	first = (BOOL)( f->name[0] == 0 );

		/* a name is BT_NAME_MAX bytes at most: one longer is not one the specification has */
		if ( nlen > BT_NAME_MAX ) {
			tm_printf((UB *)"bt: a name of %d bytes, longer than %d: its first %d taken\n", nlen, BT_NAME_MAX, BT_NAME_MAX);
			nlen = BT_NAME_MAX;
		}
		knl_memcpy(f->name, name, nlen);
		f->name[nlen] = 0;
		if ( first ) {
			tm_printf((UB *)"bt: %02x:%02x:%02x:%02x:%02x:%02x is \"%s\"\n",
				  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], f->name);
		}
	}
	return e;
}

/* A device the inquiry found, and how to page it: what a name asked of it takes */
LOCAL void found_page( BTC *c, CONST UB *addr, UW cod, UB psrm, UH clk )
{
	BTFOUND	*e = knl_btx_found_add(c, addr, cod, NULL, 0);

	if ( e == NULL ) return;
	e->hint |= FH_BREDR;
	e->psrm = psrm;
	e->clk = (UH)( clk | 0x8000 );		/* bit 15: the offset is valid */
}

/* The found entry of that address, -1 none */
LOCAL INT found_of( BTC *c, CONST UB *addr )
{
	INT	i;

	for ( i = 0; i < c->nfound; i++ ) {
		if ( same_bt_bytes(c->found[i]->f.addr, addr, 6) ) return i;
	}
	return -1;
}

/*
 * The BR/EDR side of earphones found on LE: a device of sound (its Class
 * of Device) that an inquiry found at the same address, or else the one
 * such device found under the same name. Two of that name are no answer:
 * either could be it.
 */
LOCAL INT bredr_twin( BTC *c, CONST UB *addr, CONST UB *name )
{
	INT	i, k = -1, n = 0;

	i = found_of(c, addr);
	if ( i >= 0 && ( c->found[i]->hint & FH_BREDR ) != 0 && COD_MAJOR(c->found[i]->f.cod) == COD_MAJOR_AUDIO ) return i;
	if ( name == NULL || name[0] == 0 ) return -1;
	for ( i = 0; i < c->nfound; i++ ) {
		T_BTFOUND	*f = &c->found[i]->f;
		INT		j;

		if ( ( c->found[i]->hint & FH_BREDR ) == 0 || COD_MAJOR(f->cod) != COD_MAJOR_AUDIO ) continue;
		for ( j = 0; j < (INT)sizeof(f->name) && f->name[j] == name[j] && name[j] != 0; j++ ) ;
		if ( j < (INT)sizeof(f->name) && f->name[j] == name[j] ) {
			k = i;
			n++;
		}
	}
	return ( n == 1 ) ? k : -1;
}

/* The next name the search asks, of a device the inquiry found that did not tell it; the search over when none */
LOCAL void name_next( BTC *c )
{
	INT	i;
	UB	r[10];

	c->naming = FALSE;
	for ( i = 0; i < c->nfound; i++ ) {
		T_BTFOUND	*f = &c->found[i]->f;

		if ( ( c->found[i]->hint & ( FH_BREDR | FH_NAMED ) ) != FH_BREDR || f->name[0] != 0 ) continue;
		c->found[i]->hint |= FH_NAMED;
		knl_memcpy(r, f->addr, 6);
		r[6] = c->found[i]->psrm;
		r[7] = 0;
		r[8] = (UB)c->found[i]->clk;
		r[9] = (UB)( c->found[i]->clk >> 8 );
		knl_memcpy(c->name_addr, f->addr, 6);
		c->naming = TRUE;
		c->name_due = knl_btx_now() + NAME_TMO_MS;
		knl_btx_cmd(c, C_NAME_REQ, r, 10);
		return;
	}
	if ( c->scanning ) {
		c->scanning = FALSE;
		tm_printf((UB *)"bt: search over, %d found (after %d ms)\n", c->nfound, (INT)( knl_btx_now() - c->scan_at ));
	}
}

/* What is left of the search stopped, for a page: the inquiry and the name being asked */
LOCAL void search_stop( BTC *c )
{
	if ( c->inq_on ) {
		c->inq_on = FALSE;
		knl_btx_cmd(c, C_INQ_CANCEL, NULL, 0);
		knl_btl_scan_end(c);
	}
	if ( c->naming ) {
		c->naming = FALSE;
		knl_btx_cmd(c, C_NAME_CANCEL, c->name_addr, 6);
	}
	if ( c->scanning ) {
		c->scanning = FALSE;
		tm_printf((UB *)"bt: search stopped for a connection, %d found\n", c->nfound);
	}
}

/*
 * A pairing: on LE when the device was found there alone, on BR/EDR when
 * an inquiry found it or bredr says so. audio says it is for sound
 * whatever its class says; name is what to call it when nothing else
 * tells, NULL none. The BR/EDR side of earphones is taken
 * in place of LE Audio this controller cannot carry.
 */
LOCAL void pair_start( BTC *c, CONST UB *addr, BOOL bredr, BOOL audio, CONST UB *name )
{
	PEER	*p = knl_btx_peer_new(c, addr);
	INT	k = found_of(c, addr), tw;
	UB	r[13];

	if ( p == NULL ) {
		tm_printf((UB *)"bt: no room for another device\n");
		return;
	}
	if ( p->up ) {
		tm_printf((UB *)"bt: \"%s\" is connected already\n", p->name);
		return;
	}
	if ( k >= 0 ) {
		T_BTFOUND	*f = &c->found[k]->f;

		if ( f->cod != 0 ) p->cod = f->cod;
		if ( f->name[0] != 0 ) knl_memcpy(p->name, f->name, sizeof(p->name));
		p->hint = c->found[k]->hint;
		/* found on LE alone, or on both where LE is what it is for */
		if ( !bredr && f->le ) {
			p->le = TRUE;
			p->atype = f->atype;
		}
	}
	if ( name != NULL && name[0] != 0 && p->name[0] == 0 ) knl_memcpy(p->name, name, sizeof(p->name) - 1);
	if ( bredr && p->le ) {
		p->le = FALSE;
		p->have_key = FALSE;		/* an LE bond of the same address is not this one */
	}
	if ( audio ) p->audio = TRUE;
	if ( p->le && !c->has_cis && ( ( p->hint & ( FH_LEAUDIO | FH_AUDIO ) ) != 0 || COD_MAJOR(p->cod) == COD_MAJOR_AUDIO )
	  && ( tw = bredr_twin(c, addr, p->name) ) >= 0 ) {
		/* earphones whose LE Audio this controller cannot carry, their BR/EDR side found: that side */
		UB	nm[BT_NAME_MAX + 1];

		knl_memcpy(nm, p->name, sizeof(nm));
		tm_printf((UB *)"bt: \"%s\": LE Audio, which this controller cannot carry; paired on BR/EDR as %02x:%02x:%02x:%02x:%02x:%02x\n",
			  p->name, c->found[tw]->f.addr[5], c->found[tw]->f.addr[4], c->found[tw]->f.addr[3],
			  c->found[tw]->f.addr[2], c->found[tw]->f.addr[1], c->found[tw]->f.addr[0]);
		if ( k >= 0 ) c->found[k]->note = BT_NOTE_BREDR;
		if ( !p->have_key ) p->used = FALSE;
		pair_start(c, c->found[tw]->f.addr, TRUE, TRUE, nm);
		return;
	}
	if ( p->le ) {
		knl_memcpy(p->cur_addr, p->addr, 6);
		p->cur_type = p->atype;
		knl_btl_connect(c, p);
		return;
	}
	if ( COD_MAJOR(p->cod) == COD_MAJOR_AUDIO || ( p->hint & ( FH_LEAUDIO | FH_AUDIO ) ) != 0 ) p->audio = TRUE;
	search_stop(c);
	knl_memcpy(r, addr, 6);
	r[6] = 0x18;			/* the packets DM1, DH1 ... */
	r[7] = 0xcc;
	r[8] = ( k >= 0 && ( c->found[k]->hint & FH_BREDR ) != 0 ) ? c->found[k]->psrm : 1;	/* page scan repetition, R1 not known */
	r[9] = 0;
	r[10] = ( k >= 0 ) ? (UB)c->found[k]->clk : 0;
	r[11] = ( k >= 0 ) ? (UB)( c->found[k]->clk >> 8 ) : 0;
	r[12] = 1;			/* a role change allowed */
	tm_printf((UB *)"bt: \"%s\": connecting on BR/EDR%s\n", p->name, p->audio ? ", for sound" : "");
	knl_btx_cmd(c, C_CREATE_CONN, r, 13);
	if ( p->name[0] == 0 ) {
		knl_memcpy(r, addr, 6);
		r[6] = r[8];
		r[7] = 0;
		r[8] = r[10];
		r[9] = r[11];
		knl_btx_cmd(c, C_NAME_REQ, r, 10);
	}
}

/* The objects a device was kept in, if any: deleted, with their links from 機器の設定. le: 1 an LE bond's, 0 a BR/EDR one's, -1 either */
LOCAL void bt_unkeep( CONST UB *addr, INT le )
{
	TS_UUID	*ids;
	char	idb[TS_UUID_STRLEN + 1];
	INT	n = 0, i;

	if ( ( ids = bt_links(&n) ) == NULL ) return;
	for ( i = 0; i < n; i++ ) {
		T_BTKEPT	o;
		SZ		asz = 0;
		ID		key;
		BOOL		mine;

		if ( !bt_is_obj(&ids[i]) ) continue;
		key = ob_opn_obj(&ids[i], OB_OP_R);
		if ( key <= 0 ) continue;
		knl_memset(&o, 0, sizeof(o));
		mine = (BOOL)( ob_rea_rec(key, 1, 0, &o, sizeof(o), &asz) >= E_OK && same_addr(o.addr, addr)
			     && ( le < 0 || ( o.le != 0 ) == ( le != 0 ) ) );
		(void)ob_cls_obj(key);
		if ( mine ) {
			(void)knl_conf_link_drop(bt_conf(idb), CF_ROLE_BTDEV, &ids[i]);
			(void)ob_del_obj(&ids[i]);
		}
	}
	Kfree(ids);
}

/*
 * An LE device none of whose services is used here, its link being let
 * go: not kept, and forgotten if it was kept from before, so that it is
 * not listened for and connected to again. Earphones have their BR/EDR
 * side paired once the link is gone, when it is known: for one that is of
 * sound (lea: its services are those of LE Audio; or what it advertised),
 * its identity address when it is public and its flags do not say BR/EDR
 * is not supported; for any, a device of sound an inquiry found at its
 * address or under its name. What became of it is said in the list.
 */
EXPORT void knl_btx_le_unused( BTC *c, PEER *p, BOOL lea )
{
	CONST UB	*to = NULL;
	UB		note = BT_NOTE_NOUSE;
	INT		k, tw;

	if ( ( lea || ( p->hint & ( FH_LEAUDIO | FH_AUDIO ) ) != 0 )
	  && p->atype == 0 && ( p->hint & ( FH_FLAGS | FH_NO_BREDR ) ) == FH_FLAGS ) {
		to = p->addr;
	} else if ( ( tw = bredr_twin(c, p->addr, p->name) ) >= 0 ) {
		to = c->found[tw]->f.addr;		/* a device of sound found on BR/EDR under its address or its name */
	}
	note = ( to != NULL ) ? BT_NOTE_BREDR : lea ? BT_NOTE_LEA : BT_NOTE_NOUSE;
	bt_unkeep(p->addr, 1);			/* kept on a link before, perhaps more than once */
	p->have_key = FALSE;
	p->keep_wait = FALSE;
	p->key_sz = 0;
	p->has_irk = FALSE;
	knl_memset(p->ltk, 0, sizeof(p->ltk));
	knl_memset(p->irk, 0, sizeof(p->irk));
	if ( ( k = found_of(c, p->cur_addr) ) >= 0 ) c->found[k]->note = note;
	if ( ( k = found_of(c, p->addr) ) >= 0 && c->found[k]->f.le ) c->found[k]->note = note;
	if ( to != NULL ) {
		c->fb_on = TRUE;
		c->fb_handle = p->handle;
		knl_memcpy(c->fb_addr, to, 6);
		knl_memcpy(c->fb_name, p->name, sizeof(c->fb_name));
		tm_printf((UB *)"bt: \"%s\": not kept on LE; its BR/EDR side %02x:%02x:%02x:%02x:%02x:%02x is paired once the link is gone\n",
			  p->name, to[5], to[4], to[3], to[2], to[1], to[0]);
	} else {
		tm_printf((UB *)"bt: \"%s\": not kept, and not connected to again%s\n", p->name,
			  lea ? ": its BR/EDR side is not known (search again with the earphones ready to pair)" : "");
	}
}

/*
 * The longest packet the controller says it sends, to the way it is
 * reached: an event, an ACL packet of either link, an SCO packet, an ISO
 * packet, each with its header.
 */
LOCAL void rx_size( BTC *c )
{
	INT	n = 2 + 255;

	if ( 4 + c->acl_len > n ) n = 4 + c->acl_len;
	if ( 4 + c->le_len > n ) n = 4 + c->le_len;
	if ( 3 + c->sco_mtu > n ) n = 3 + c->sco_mtu;
	if ( 4 + c->iso_len > n ) n = 4 + c->iso_len;
	if ( c->tr->rx_size != NULL ) c->tr->rx_size(c, n);
}

/*
 * The fewest bytes of parameters an event has that is read here: one
 * shorter is passed over, as its fields would be read past what came.
 */
LOCAL INT ev_min( UB code )
{
	switch ( code ) {
	case E_CMD_DONE:	return 3;	/* the packets it takes, the opcode */
	case E_CMD_STATUS:	return 4;
	case E_INQ_RESULT:	return 1;
	case E_NUM_DONE:	return 1;
	case E_CONN_REQ:	return 10;
	case E_CONN_DONE:	return 11;
	case E_AUTH_DONE:	return 3;
	case E_ENC_CHANGE:	return 4;
	case E_LK_REQ:		return 6;
	case E_LK_NOTE:		return 23;
	case E_PIN_REQ:		return 6;
	case E_IOCAP_REQ:	return 6;
	case E_CONFIRM_REQ:	return 6;
	case E_NAME_DONE:	return 7;
	case E_DISCONN:		return 4;
	case E_LE_META:		return 1;
	default:		return 0;
	}
}

LOCAL void ev_in( BTC *c, CONST UB *e, INT n )
{
	UB	code;
	INT	len;
	CONST UB *d;
	UB	r[32];
	PEER	*p;
	INT	i;

	if ( n < 2 ) return;
	code = e[0];
	len = e[1];
	d = e + 2;
	if ( len + 2 > n ) return;
	if ( len < ev_min(code) ) {
		tm_printf((UB *)"bt: an event %02x of %d bytes, too short, passed over\n", code, len);
		return;
	}
	switch ( code ) {
	case E_CMD_DONE: {
		UH	op = (UH)( d[1] | ( d[2] << 8 ) );
		UB	st = ( len >= 4 ) ? d[3] : 0;
		INT	ret = len - 3;		/* the return parameters, from the status */

		if ( st != 0 ) tm_printf((UB *)"bt: command %04x answered with status %02x\n", op, st);
		if ( op == C_READ_ADDR && st == 0 && ret >= 7 ) knl_memcpy(c->addr, d + 4, 6);
		if ( op == C_READ_ADDR ) c->addr_status = st;
		if ( op == C_READ_VER && st == 0 && ret >= 9 ) {
			tm_printf((UB *)"bt: HCI version %d, revision %04x, LMP %d.%04x, maker %04x\n",
				  d[4], d[5] | ( d[6] << 8 ), d[7], d[10] | ( d[11] << 8 ), d[8] | ( d[9] << 8 ));
		}
		if ( op == C_READ_FEATURES && st == 0 && ret >= 9 ) {
			knl_memcpy(c->lmp, d + 4, 8);
			c->has_le = (BOOL)( ( d[8] >> 6 ) & 1 );
			/* byte 4 bit 6 LE, byte 6 bit 3 Secure Simple Pairing: what the controller can be asked for */
			tm_printf((UB *)"bt: features %02x %02x %02x %02x %02x %02x %02x %02x: LE %d, simple pairing %d\n",
				  d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], ( d[8] >> 6 ) & 1, ( d[10] >> 3 ) & 1);
		}
		if ( op == C_READ_BUF && st == 0 && ret >= 8 ) {
			c->acl_len = (UH)( d[4] | ( d[5] << 8 ) );
			c->acl_room = d[7] | ( d[8] << 8 );
			c->sco_mtu = d[6];
			c->sco_num = (UH)( d[9] | ( d[10] << 8 ) );
			tm_printf((UB *)"bt: ACL packets of %d bytes, %d at a time; SCO packets of %d bytes, %d at a time\n",
				  c->acl_len, c->acl_room, c->sco_mtu, c->sco_num);
		}
		if ( op == C_LE_READ_BUF && st == 0 && ret >= 4 ) {
			c->le_len = (UH)( d[4] | ( d[5] << 8 ) );
			c->le_room = d[6];
			tm_printf((UB *)"bt: LE packets of %d bytes, %d at a time\n", c->le_len, c->le_room);
		}
		knl_btiso_cmd_done(c, op, st, d + 4, ( ret > 1 ) ? ret - 1 : 0);
		if ( st == 0 && ( op == C_READ_BUF || op == C_LE_READ_BUF || op == C_LE_READ_BUF_V2 ) ) rx_size(c);
		cmd_answered(c);
		/* a reset refused is what a controller that cannot run says: nothing after it would work */
		if ( !c->ready && op == C_RESET && st != 0 ) start_stuck(c, op, st);
		if ( !c->ready ) start_next(c);
		break;
	}
	case E_CMD_STATUS:
		if ( d[0] != 0 ) tm_printf((UB *)"bt: command %04x not started, status %02x\n", d[2] | ( d[3] << 8 ), d[0]);
		if ( d[0] != 0 && (UH)( d[2] | ( d[3] << 8 ) ) == C_INQUIRY ) {
			c->inq_on = FALSE;
			c->scanning = FALSE;
			knl_btl_scan_end(c);
		}
		if ( d[0] != 0 && (UH)( d[2] | ( d[3] << 8 ) ) == C_NAME_REQ && c->naming ) name_next(c);
		knl_btiso_cmd_status(c, (UH)( d[2] | ( d[3] << 8 ) ), d[0]);
		knl_bta_event(c, code, d, len);
		cmd_answered(c);
		break;
	case E_INQ_RESULT:
		/* the addresses, then the page scan repetition modes, two reserved bytes, the classes and the clock offsets */
		if ( 1 + 14 * d[0] > len ) {
			tm_printf((UB *)"bt: an inquiry result of %d devices in %d bytes, cut short, passed over\n", d[0], len);
			break;
		}
		for ( i = 0; i < d[0]; i++ ) {
			CONST UB *a = d + 1 + 6 * i;
			CONST UB *cl = d + 1 + 6 * d[0] + 3 * d[0] + 3 * i;
			CONST UB *ck = d + 1 + 12 * d[0] + 2 * i;

			found_page(c, a, cl[0] | ( cl[1] << 8 ) | ( (UW)cl[2] << 16 ), d[1 + 6 * d[0] + i], (UH)( ck[0] | ( ck[1] << 8 ) ));
		}
		break;
	case E_INQ_RSSI:
		if ( len >= 15 ) found_page(c, d + 1, d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), d[7], (UH)( d[12] | ( d[13] << 8 ) ));
		break;
	case E_EXT_INQ:
		if ( len >= 15 ) {
			INT	at = 15;

			found_page(c, d + 1, d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), d[7], (UH)( d[12] | ( d[13] << 8 ) ));
			/* its name, among what it tells */
			while ( at < len && d[at] != 0 ) {
				INT	l = d[at];

				if ( at + l >= len ) break;
				if ( d[at + 1] == 0x08 || d[at + 1] == 0x09 ) {
					(void)knl_btx_found_add(c, d + 1, d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), d + at + 2, l - 1);
				}
				at += l + 1;
			}
		}
		break;
	case E_LE_META:
		knl_btl_event(c, d, len);
		break;
	case E_NUM_DONE:
		for ( i = 0; i < d[0] && 1 + 4 * ( i + 1 ) <= len; i++ ) {
			UH	h = (UH)( d[1 + 4 * i] | ( d[2 + 4 * i] << 8 ) );
			INT	done = d[3 + 4 * i] | ( d[4 + 4 * i] << 8 );

			if ( knl_btiso_pkts_done(c, h, done) ) continue;	/* a CIS's: room for ISO packets */
			p = peer_of_handle(c, h);
			if ( p != NULL && p->le && c->le_len > 0 ) c->le_room += done;
			else c->acl_room += done;
		}
		knl_btx_tx_pump(c);
		break;
	case E_VENDOR:
		if ( c->intel ) knl_bti_log_bytes("Intel event", d, len);
		break;
	case E_INQ_DONE:
		knl_btl_scan_end(c);
		/* the status: 0 when the inquiry ran its length, an error code when the controller ended it */
		tm_printf((UB *)"bt: scan over, %d found (status %02x, after %d ms)\n",
			  c->nfound, ( len > 0 ) ? d[0] : 0xff, (INT)( knl_btx_now() - c->scan_at ));
		/* the names the devices did not tell, unless a pairing stopped the search */
		if ( c->inq_on ) {
			c->inq_on = FALSE;
			name_next(c);
		}
		break;
	case E_CONN_REQ: {
		if ( len >= 10 && d[9] != 1 ) {
			knl_bta_event(c, code, d, len);		/* a voice link, not an ACL one */
			break;
		}
		p = peer_of_addr(c, d);
		knl_memcpy(r, d, 6);
		if ( p != NULL ) {
			r[6] = 1;		/* it takes the central's role, as a keyboard reconnecting wants */
			knl_btx_cmd(c, C_ACCEPT, r, 7);
		} else {
			tm_printf((UB *)"bt: a connection from a device not paired, left\n");
		}
		break;
	}
	case E_CONN_DONE: {
		UH	h = (UH)( d[1] | ( d[2] << 8 ) );

		p = peer_of_addr(c, d + 3);
		tm_printf((UB *)"bt: connection %d to %02x:..:%02x: status %02x\n", h, d[8], d[3], d[0]);
		if ( p == NULL ) break;
		if ( d[0] != 0 ) {
			p->up = FALSE;
			break;
		}
		p->handle = h;
		p->up = TRUE;
		p->cfg_in = p->cfg_out = 0;
		p->rcid_ctrl = p->rcid_intr = p->rcid_sdp = 0;
		p->rcid_av = p->rcid_media = 0;
		p->media_mtu = L2_MTU;
		knl_memset(p->rcid_x, 0, sizeof(p->rcid_x));
		for ( i = 0; i < NCIDX; i++ ) p->mtu_x[i] = L2_MTU;
		p->cfgx_in = p->cfgx_out = p->x_on = 0;
		if ( COD_MAJOR(p->cod) == COD_MAJOR_AUDIO ) p->audio = TRUE;
		p->hd = p->hd2 = 0;
		p->hid_on = FALSE;
		p->sdp = ( p->rdlen > 0 ) ? SDP_DONE : SDP_NONE;
		r[0] = (UB)h;
		r[1] = (UB)( h >> 8 );
		knl_btx_cmd(c, C_AUTH, r, 2);
		break;
	}
	case E_AUTH_DONE: {
		UH	h = (UH)( d[1] | ( d[2] << 8 ) );

		p = peer_of_handle(c, h);
		tm_printf((UB *)"bt: authenticated %d: status %02x\n", h, d[0]);
		if ( p == NULL || d[0] != 0 ) break;
		r[0] = (UB)h;
		r[1] = (UB)( h >> 8 );
		r[2] = 1;
		knl_btx_cmd(c, C_ENCRYPT, r, 3);
		break;
	}
	case E_ENC_CHANGE: {
		UH	h = (UH)( d[1] | ( d[2] << 8 ) );

		p = peer_of_handle(c, h);
		tm_printf((UB *)"bt: encryption on %d: status %02x, %d\n", h, d[0], d[3]);
		if ( p != NULL && p->le ) {
			knl_btl_enc_change(c, p, d[0], d[3]);
		} else if ( p != NULL && d[0] == 0 && d[3] != 0 ) {
			if ( p->audio ) sdp_start(c, p);
			else if ( p->sdp == SDP_NONE ) sdp_start(c, p);
			else if ( p->sdp == SDP_DONE && p->rcid_ctrl == 0 ) knl_btx_l2_open(c, p, PSM_HID_CTRL, CID_CTRL);
		}
		break;
	}
	case E_LK_REQ:
		p = peer_of_addr(c, d);
		knl_memcpy(r, d, 6);
		if ( p != NULL && p->have_key ) {
			knl_memcpy(r + 6, p->key, 16);
			knl_btx_cmd(c, C_LK_REPLY, r, 22);
		} else {
			knl_btx_cmd(c, C_LK_NEG, r, 6);
		}
		break;
	case E_LK_NOTE:
		p = peer_of_addr(c, d);
		if ( p != NULL ) {
			ER	er;

			knl_memcpy(p->key, d + 6, 16);
			p->have_key = TRUE;
			er = knl_btx_keep(p);
			tm_printf((UB *)"bt: link key of %02x:..:%02x given, kind %d; kept (%d)\n", d[5], d[0], d[22], (INT)er);
		}
		break;
	case E_PIN_REQ:
		knl_memcpy(r, d, 6);
		r[6] = 4;
		knl_memset(r + 7, 0, 16);
		knl_memcpy(r + 7, "0000", 4);
		tm_printf((UB *)"bt: the older pairing: PIN 0000 offered\n");
		knl_btx_cmd(c, C_PIN_REPLY, r, 23);
		break;
	case E_IOCAP_REQ:
		knl_memcpy(r, d, 6);
		r[6] = 3;			/* neither a display nor a keyboard */
		r[7] = 0;			/* no data out of band */
		r[8] = 4;			/* general bonding, no protection from a man in the middle */
		knl_btx_cmd(c, C_IOCAP_REPLY, r, 9);
		break;
	case E_CONFIRM_REQ:
		knl_memcpy(r, d, 6);
		knl_btx_cmd(c, C_CONFIRM_REPLY, r, 6);
		break;
	case E_NAME_DONE: {
		INT	nl;

		/* the name, up to its zero, in as many bytes as came */
		for ( nl = 0; 7 + nl < len && d[7 + nl] != 0; nl++ ) ;
		p = peer_of_addr(c, d + 1);
		if ( p != NULL && d[0] == 0 ) {
			knl_memset(p->name, 0, sizeof(p->name));
			for ( i = 0; i < (INT)sizeof(p->name) - 1 && i < nl; i++ ) p->name[i] = d[7 + i];
			if ( p->have_key && !p->keep_wait ) (void)knl_btx_keep(p);
		}
		if ( d[0] == 0 && nl > 0 && found_of(c, d + 1) >= 0 ) {
			(void)knl_btx_found_add(c, d + 1, 0, d + 7, nl);
		} else if ( d[0] != 0 ) {
			tm_printf((UB *)"bt: the name of %02x:%02x:%02x:%02x:%02x:%02x not had, status %02x\n",
				  d[6], d[5], d[4], d[3], d[2], d[1], d[0]);
		}
		if ( c->naming && same_addr(c->name_addr, d + 1) ) name_next(c);
		break;
	}
	case E_DISCONN: {
		UH	h = (UH)( d[1] | ( d[2] << 8 ) );

		p = peer_of_handle(c, h);
		tm_printf((UB *)"bt: connection %d gone, reason %02x\n", h, d[3]);
		if ( knl_btiso_disconn(c, h, d[3]) ) break;
		if ( p == NULL ) knl_bta_event(c, code, d, len);	/* a voice link's, perhaps */
		if ( p != NULL ) {
			if ( p->le ) knl_btl_down(c, p);
			if ( p->audio ) knl_bta_down(c, p);
			p->up = FALSE;
			if ( p->hid_on ) {
				knl_usb_lock();
				hid_let_go(p);
				knl_usb_unlock();
			}
			/* a bond of this link whose services were never used goes with it: it was not kept */
			if ( p->keep_wait ) {
				p->keep_wait = FALSE;
				p->have_key = FALSE;
			}
			/* a device not paired, or let go unkept, takes no room: one found again is made anew */
			if ( !p->have_key ) p->used = FALSE;
		}
		if ( c->fb_on && c->fb_handle == h ) {
			/* its LE link gone: its BR/EDR side paired in its place */
			c->fb_on = FALSE;
			pair_start(c, c->fb_addr, TRUE, TRUE, c->fb_name);
		}
		break;
	}
	default:
		knl_bta_event(c, code, d, len);
		break;
	}
}

/* ---------------------------------------------------------------- the device object */

LOCAL INT bo_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	(void)t;
	(void)max;
	return n;
}

LOCAL INT bo_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(j, n, max, ",\"virtual\":true");
}

/* What a device's connection is doing: set up for what it is used for, or not yet */
LOCAL UB peer_use( BTC *c, PEER *p )
{
	if ( !p->up ) return BT_USE_NONE;
	if ( p->hid_on ) return BT_USE_INPUT;
	if ( knl_bta_ready(p) || ( p->le && knl_btbap_ready(c, p) ) ) return BT_USE_SOUND;
	return BT_USE_SETUP;
}

/*
 * One line into buf at at, its name after its fields, unless *p_full or
 * it does not fit in size (then *p_full, so that no later line is put
 * after a gap): the bytes it takes.
 */
LOCAL INT line_put( UB *buf, INT size, INT at, CONST T_BTLINE *one, CONST UB *name, BOOL *p_full )
{
	INT	nl, k;

	for ( nl = 0; nl < BT_NAME_MAX && name[nl] != 0; nl++ ) ;
	k = BT_LINE_SIZE(nl);
	if ( buf == NULL || *p_full || at + k > size ) {
		*p_full = TRUE;
		return k;
	}
	knl_memset(buf + at, 0, k);
	knl_memcpy(buf + at, one, BT_LINE_HEAD);
	((T_BTLINE *)( buf + at ))->size = (UH)k;
	((T_BTLINE *)( buf + at ))->namelen = (UH)nl;
	knl_memcpy(buf + at + BT_LINE_HEAD, name, nl);
	return k;
}

/*
 * The lines of the list: the devices paired, then those the search found
 * that are not among them, each with what its connection is doing and
 * what became of a try with it. A device being paired is a found one
 * whose connection is up. As many as there are: the whole lines size
 * takes into buf (NULL for none), how many bytes those are in *p_put;
 * the bytes of all of them answered. From another task too: each count
 * is taken before its table, which then has them all.
 */
LOCAL INT bt_lines( BTC *c, UB *buf, INT size, INT *p_put )
{
	T_BTLINE	one;
	PEER		* volatile *pt;
	BTFOUND		* volatile *ft;
	INT		i, k, n = 0, np, nf, put = 0, len;
	BOOL		full = FALSE;

	np = c->npeer;
	nf = c->nfound;
	__sync_synchronize();
	pt = c->peer;
	ft = c->found;
	for ( i = 0; i < np; i++ ) {
		PEER	*p = pt[i];

		if ( !p->used || !p->have_key || p->keep_wait ) continue;
		knl_memset(&one, 0, sizeof(one));
		knl_memcpy(one.addr, p->addr, 6);
		one.paired = 1;
		one.use = peer_use(c, p);
		one.connected = (UB)( one.use == BT_USE_INPUT || one.use == BT_USE_SOUND );
		one.cod = p->cod;
		one.bearer = p->le ? BT_BEARER_LE : BT_BEARER_BREDR;
		len = line_put(buf, size, n, &one, p->name, &full);
		if ( !full ) put = n + len;
		n += len;
	}
	for ( i = 0; i < nf; i++ ) {
		BTFOUND		*e = ft[i];
		T_BTFOUND	*f = &e->f;
		BOOL		had = FALSE;

		knl_memset(&one, 0, sizeof(one));
		for ( k = 0; k < np; k++ ) {
			PEER	*p = pt[k];

			if ( !p->used ) continue;
			if ( p->have_key && !p->keep_wait && same_addr(p->addr, f->addr) ) had = TRUE;
			if ( p->up && ( same_addr(p->addr, f->addr) || ( p->le && same_addr(p->cur_addr, f->addr) ) ) ) {
				one.use = peer_use(c, p);
			}
		}
		if ( had ) continue;
		knl_memcpy(one.addr, f->addr, 6);
		one.cod = f->cod;
		one.connected = (UB)( one.use == BT_USE_INPUT || one.use == BT_USE_SOUND );
		one.bearer = (UB)( ( ( e->hint & FH_BREDR ) ? BT_BEARER_BREDR : 0 ) | ( f->le ? BT_BEARER_LE : 0 ) );
		one.note = e->note;
		len = line_put(buf, size, n, &one, f->name, &full);
		if ( !full ) put = n + len;
		n += len;
	}
	if ( p_put != NULL ) *p_put = put;
	return n;
}

/*
 * A record read: OB_BT_LIST is the lines, all of them, as bytes from off
 * as far as size takes; a reader whose room is full reads again with
 * more (knl_ob_rea_all, ui_rec_all).
 */
LOCAL ER bo_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	BTC		*c = (BTC *)ctx;
	UB		*l;
	INT		all, put = 0;
	SZ		k = 0;

	(void)pos;
	if ( recno == OB_BT_STATE ) {
		if ( off != 0 || size < (SZ)sizeof(T_BTSTATE) ) {
			if ( p_asize != NULL ) *p_asize = 0;
			return E_OK;
		}
		*(T_BTSTATE *)buf = c->st;
		((T_BTSTATE *)buf)->scanning = (UB)( c->scanning ? 1 : 0 );
		if ( p_asize != NULL ) *p_asize = (SZ)sizeof(T_BTSTATE);
		return E_OK;
	}
	if ( recno != OB_BT_LIST ) return E_NOEXS;
	if ( off < 0 || size < 0 ) return E_PAR;
	/* as many as there are now, and room for a few that come meanwhile */
	all = bt_lines(c, NULL, 0, NULL) + 8 * BT_LINE_SIZE(BT_NAME_MAX);
	l = (UB *)Kmalloc((SZ)all);
	if ( l == NULL ) return E_NOMEM;
	(void)bt_lines(c, l, all, &put);
	if ( off < (D)put ) {
		k = (SZ)put - (SZ)off;
		if ( k > size ) k = size;
		if ( buf != NULL && k > 0 ) knl_memcpy(buf, l + off, (INT)k);
		else k = 0;
	}
	Kfree(l);
	if ( p_asize != NULL ) *p_asize = k;
	return E_OK;
}

LOCAL ER bo_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	CONST T_BTCMD	*m = (CONST T_BTCMD *)buf;
	BTC		*c = (BTC *)ctx;
	ER		er = E_PAR;

	(void)pos;
	(void)off;
	if ( recno != OB_BT_CMD ) return E_NOEXS;
	if ( size < (SZ)sizeof(*m) ) return E_PAR;
	if ( !c->ready ) return E_OBJ;
	switch ( m->op ) {
	case BT_C_SCAN:		er = knl_bt_scan(10);		break;
	case BT_C_PAIR:		er = knl_bt_pair(m->addr);	break;
	case BT_C_FORGET:
		knl_memcpy(c->forget_addr, m->addr, 6);
		c->req_forget = TRUE;
		er = tk_sig_sem(c->sem, 1);
		break;
	default:
		break;
	}
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

/* Anyone at the machine may pair a keyboard: it is how one who has none gets to type */
LOCAL CONST T_OBDVOPS bo_ops = {
	OB_S_CHAR, "bluetooth", OB_BT_NREC, 0666, 0, 0,
	bo_text, bo_attr, bo_rea, bo_wri, NULL, NULL
};

/* The device's object taken away as well as its record: what the machine forgets */
LOCAL void bt_forget( BTC *c )
{
	PEER	*p = peer_of_addr(c, c->forget_addr);
	UB	r[3];

	if ( p == NULL ) return;
	if ( p->up ) {
		r[0] = (UB)p->handle;
		r[1] = (UB)( p->handle >> 8 );
		r[2] = 0x13;			/* the person ended it */
		knl_btx_cmd(c, C_DISCONNECT, r, 3);
	}
	bt_unkeep(p->addr, -1);
	tm_printf((UB *)"bt: forgot %02x:..:%02x\n", p->addr[5], p->addr[0]);
	p->have_key = FALSE;
	p->keep_wait = FALSE;
	if ( !p->up ) p->used = FALSE;
}

/* ---------------------------------------------------------------- the task */

/*
 * The endpoints give a packet at a time; what is above takes a whole
 * event, a whole ACL packet. A piece may end one and begin the next. An
 * event's length is one byte, so c->ev holds any; an ACL packet's room
 * is had as long as its header says.
 */
LOCAL void ev_piece( BTC *c, CONST UB *b, INT n )
{
	while ( n > 0 ) {
		INT	want = ( c->ev_have < 2 ) ? 2 : 2 + c->ev[1];
		INT	k = want - c->ev_have;

		if ( k > n ) k = n;
		knl_memcpy(c->ev + c->ev_have, b, k);
		c->ev_have += k;
		b += k;
		n -= k;
		if ( c->ev_have >= 2 && c->ev_have == 2 + c->ev[1] ) {
			ev_in(c, c->ev, c->ev_have);
			c->ev_have = 0;
		}
	}
}

LOCAL void acl_piece( BTC *c, CONST UB *b, INT n )
{
	while ( n > 0 ) {
		INT	want = ( c->acl_have < 4 ) ? 4 : 4 + ( c->acl[2] | ( c->acl[3] << 8 ) );
		INT	k = want - c->acl_have;

		if ( want > c->acl_cap ) {
			INT	cap = ( c->acl_cap > 0 ) ? c->acl_cap : 1024;
			UB	*nb;

			while ( cap < want ) cap *= 2;
			nb = (UB *)Kmalloc((SZ)cap);
			if ( nb == NULL ) {
				/* its bytes passed over as they come, so that the next packet is in step */
				tm_printf((UB *)"bt: an ACL packet of %d bytes let go: no room for it\n", want);
				c->acl_cap = 0;
				if ( c->acl != NULL ) Kfree(c->acl);
				c->acl = NULL;
				c->acl_have = 0;
				return;
			}
			if ( c->acl_have > 0 ) knl_memcpy(nb, c->acl, c->acl_have);
			if ( c->acl != NULL ) Kfree(c->acl);
			c->acl = nb;
			c->acl_cap = cap;
		}
		if ( k > n ) k = n;
		knl_memcpy(c->acl + c->acl_have, b, k);
		c->acl_have += k;
		b += k;
		n -= k;
		/* whole when it has as many bytes as its header, now there, says */
		if ( c->acl_have >= 4 && c->acl_have == 4 + ( c->acl[2] | ( c->acl[3] << 8 ) ) ) {
			acl_in(c, c->acl, c->acl_have);
			c->acl_have = 0;
		}
	}
}

LOCAL void requests( BTC *c )
{
	UB	r[32];

	/* the object comes with the adapter, ready or not: the settings tell the two apart */
	if ( !c->obj_up ) {
		TS_UUID	u;

		/*
		 * A controller on a port the firmware left up comes before the
		 * layer of device objects has started, which answers E_PAR until
		 * then: asked again each round. Any other answer is final.
		 */
		if ( ts_str_to_uuid(BT_DEV_UUID, &u) >= E_OK ) {
			ER	er = knl_obdev_add((CONST UB *)"Bluetooth", &u, &bo_ops, c, NULL);

			if ( er != E_PAR ) c->obj_up = TRUE;
			if ( er < E_OK && er != E_PAR ) tm_printf((UB *)"bt: no device object (%d)\n", (INT)er);
		}
	}
	/* the devices kept are on a volume, which may come later than the controller too */
	if ( c->ready && !c->recalled && knl_obfile_first_vol() > 0 ) {
		c->recalled = TRUE;
		bt_recall(c);
	}
	if ( !c->ready ) return;
	if ( c->req_forget ) {
		c->req_forget = FALSE;
		bt_forget(c);
	}
	if ( c->req_scan_s > 0 && !c->scanning ) {
		INT	s = c->req_scan_s;

		c->req_scan_s = 0;
		c->nfound = 0;
		c->scanning = TRUE;
		c->inq_on = TRUE;
		c->scan_at = knl_btx_now();
		knl_btl_scan_start(c);
		r[0] = 0x33;			/* the general inquiry access code */
		r[1] = 0x8b;
		r[2] = 0x9e;
		r[3] = (UB)( ( s * 1000 ) / INQ_UNIT_MS + 1 );
		r[4] = 0;
		knl_btx_cmd(c, C_INQUIRY, r, 5);
	}
	if ( c->req_pair ) {
		c->req_pair = FALSE;
		pair_start(c, c->pair_addr, FALSE, FALSE, NULL);
	}
}

/* A device that has not answered about its records is read as if it had none */
LOCAL void sdp_overdue( BTC *c )
{
	SYSTIM	t;
	INT	i;

	(void)tk_get_otm(&t);
	for ( i = 0; i < c->npeer; i++ ) {
		PEER	*p = c->peer[i];

		if ( p->used && p->up && p->sdp == SDP_RUN && (W)( t.lo - p->sdp_due ) >= 0 ) {
			tm_printf((UB *)"bt: \"%s\": the service records did not answer\n", p->name);
			sdp_end(c, p);
		}
	}
}

LOCAL void usb_autosuspend( BTC *c );

LOCAL void bt_task( INT stacd, void *exinf )
{
	BTC	*c = (BTC *)exinf;

	(void)stacd;
	requests(c);			/* the object first: a load takes seconds, and the settings tell of it */
	if ( ( !c->intel || knl_bti_load(c) ) && ( c->tr->prep == NULL || c->tr->prep(c) ) ) start_next(c);
	while ( !c->gone ) {
		(void)tk_wai_sem(c->sem, 1, 500);
		for ( ;; ) {
			CONST UB	*b;
			INT		len;
			UB		kind;

			knl_btx_ring_grow(c);
			if ( !knl_btx_ring_take(c, &kind, &b, &len) ) break;
			if ( kind == RK_EVENT ) ev_piece(c, b, len);
			else if ( kind == RK_SCO ) knl_bth_sco_in(c, b, len);
			else acl_piece(c, b, len);
		}
		knl_btx_tx_pump(c);
		if ( c->cmd_busy && (W)( knl_btx_now() - c->cmd_ms ) >= CMD_TMO_MS ) {
			UH	op = c->cmd_op;

			tm_printf((UB *)"bt: command %04x not answered in %d ms\n", op, CMD_TMO_MS);
			cmd_answered(c);
			/* a vendor's command is only a question; without the others the adapter cannot run */
			if ( !c->ready && ( op >> 10 ) != 0x3f ) start_stuck(c, op, 0xffff);
			if ( !c->ready ) start_next(c);
		}
		if ( c->naming && (W)( knl_btx_now() - c->name_due ) >= 0 ) {
			/* a name the controller has not answered for: given up, and on to the next */
			tm_printf((UB *)"bt: the name of %02x:..:%02x not answered in %d ms\n", c->name_addr[5], c->name_addr[0], NAME_TMO_MS);
			knl_btx_cmd(c, C_NAME_CANCEL, c->name_addr, 6);
			name_next(c);
		}
		requests(c);
		sdp_overdue(c);
		knl_btl_tick(c);
		knl_bta_tick(c);
		usb_autosuspend(c);
	}
	if ( c->obj_up ) (void)knl_obdev_del((CONST UB *)"Bluetooth");
	tk_exd_tsk();
}

/* ---------------------------------------------------------------- the USB side */

/*
 * The adapter suspended while idle, as a USB device may be: allowed
 * unless the start-up setting says "btusb.enable_autosuspend=0"; after
 * AUTOSUSP_MS with nothing sent or received, while it has no connection,
 * no search (an inquiry or an active LE scan), no command, frame or event
 * waiting, and is up. Its remote wakeup is on while suspended, so that
 * an event (a connection asked for by a device, an advertisement of a
 * passive scan) brings it back; anything to send brings it back first.
 */
#define AUTOSUSP_MS	2000

LOCAL CONST BTTRANS	usb_tr;

LOCAL BOOL bt_idle( BTC *c )
{
	INT	i;

	if ( !c->ready || c->cmd_busy || c->q_first != NULL || c->tx_first != NULL || c->head != c->tail ) return FALSE;
	if ( c->scanning || c->inq_on || c->naming || c->le_scan_active || c->le_user_scan || c->le_conn != NULL || c->fb_on ) {
		return FALSE;
	}
	for ( i = 0; i < c->npeer; i++ ) {
		if ( c->peer[i]->used && c->peer[i]->handle != 0xffff ) return FALSE;
	}
	return (BOOL)( (W)( knl_usb_ms() - c->last_busy ) >= AUTOSUSP_MS );
}

LOCAL void usb_autosuspend( BTC *c )
{
	if ( !c->autosusp || c->tr != &usb_tr || c->dev == NULL || c->dev->suspended || !bt_idle(c) ) return;
	knl_usb_lock();
	if ( !c->gone && bt_idle(c) ) (void)knl_usb_dev_suspend(c->dev, TRUE);
	knl_usb_unlock();
}

/* With the manager's lock held: the adapter brought back before anything goes to it */
LOCAL ER usb_awake( BTC *c )
{
	c->last_busy = knl_usb_ms();
	return c->dev->suspended ? knl_usb_dev_resume(c->dev) : E_OK;
}

LOCAL ER usb_cmd( BTC *c, UB *b, INT n )
{
	ER	er;

	knl_usb_lock();
	er = c->gone ? E_IO : usb_awake(c);
	if ( er >= E_OK ) er = knl_usb_control(c->dev, USB_RT_CLASS, REQ_HCI_COMMAND, 0, 0, b, (UH)n, NULL);
	knl_usb_unlock();
	return er;
}

LOCAL ER usb_acl( BTC *c, UB *b, INT n )
{
	INT	got = 0;
	ER	er;

	knl_usb_lock();
	er = c->gone ? E_IO : usb_awake(c);
	if ( er >= E_OK ) er = c->dev->hc->ops->bulk(c->dev->hc, c->dev, &c->ep_out, b, n, &got, 1000);
	knl_usb_unlock();
	return er;
}

/* ISO data goes on the same bulk endpoint as ACL data */
LOCAL CONST BTTRANS	usb_tr = { usb_cmd, usb_acl, NULL, usb_acl, NULL, NULL };	/* SCO on the voice interface (bt_hfp.c) */

EXPORT BOOL knl_bt_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( dev->cfg == NULL ) return FALSE;
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[3] == 0
		  && p[5] == USB_CLASS_WIRELESS && p[6] == 1 && p[7] == 1 ) return TRUE;
	}
	return FALSE;
}

/* With the manager's lock held */
EXPORT ER knl_bt_attach( USBDEV *dev )
{
	UB	*p, *end = dev->cfg + dev->cfglen, *ev_d = NULL, *in_d = NULL, *out_d = NULL;
	BOOL	in = FALSE;
	BTC	*c;
	USBEP	ep;
	T_CSEM	cs;
	T_CTSK	ct;
	ER	er;

	if ( bt_c != NULL ) {
		USB_LOG("bt: a second controller left alone\n");
		return E_LIMIT;
	}
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			in = (BOOL)( p[3] == 0 && p[5] == USB_CLASS_WIRELESS && p[6] == 1 && p[7] == 1 && p[2] == 0 );
		}
		if ( !in || p[1] != USB_DT_ENDPOINT || p[0] < 7 ) continue;
		if ( ( p[3] & 3 ) == USB_EP_INTR && ( p[2] & 0x80 ) != 0 ) ev_d = p;
		if ( ( p[3] & 3 ) == USB_EP_BULK && ( p[2] & 0x80 ) != 0 ) in_d = p;
		if ( ( p[3] & 3 ) == USB_EP_BULK && ( p[2] & 0x80 ) == 0 ) out_d = p;
	}
	if ( ev_d == NULL || in_d == NULL || out_d == NULL ) {
		USB_LOG("bt: an interface without its endpoints\n");
		return E_NOEXS;
	}
	c = (BTC *)Kcalloc(1, sizeof(BTC));
	if ( c == NULL ) return E_NOMEM;
	if ( !knl_btx_ring_make(c) ) {
		Kfree(c);
		return E_NOMEM;
	}
	c->tr = &usb_tr;
	c->dev = dev;
	{
		UB	v[4];

		/* suspended while idle unless the start-up setting says "btusb.enable_autosuspend=0" */
		c->autosusp = !( knl_bootarg(knl_bootargs, "btusb.enable_autosuspend", v, sizeof(v))
				 && ( v[0] == '0' || v[0] == 'n' || v[0] == 'N' ) );
		c->last_busy = knl_usb_ms();
	}
	c->intel = (BOOL)( GET16(dev->ddesc + 8) == USB_VID_INTEL );
	c->st.state = BT_ST_STARTING;
	c->st.vid = (UH)GET16(dev->ddesc + 8);
	c->st.pid = (UH)GET16(dev->ddesc + 10);
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RING_SEM_MAX;
	c->sem = tk_cre_sem(&cs);
	if ( c->sem <= 0 ) {
		Kfree(c->rb);
		Kfree(c);
		return E_LIMIT;
	}
	knl_usb_ep_from_desc(&c->ep_out, dev, out_d, NULL);
	knl_usb_ep_from_desc(&ep, dev, ev_d, NULL);
	er = dev->hc->ops->stream_open(dev->hc, dev, &ep, ( ep.size < EV_MAX ) ? EV_MAX : ep.size, ev_xfer, c, &c->ev_pipe);
	if ( er >= E_OK ) {
		knl_usb_ep_from_desc(&ep, dev, in_d, NULL);
		er = dev->hc->ops->stream_open(dev->hc, dev, &ep, ( ep.size > ACL_IN_XFER ) ? ep.size : ACL_IN_XFER, acl_xfer, c, &c->acl_pipe);
	}
	if ( er < E_OK ) {
		USB_LOG("bt: its streams not opened (%d)\n", (INT)er);
		if ( c->ev_pipe != NULL ) dev->hc->ops->stream_close(dev->hc, c->ev_pipe);
		tk_del_sem(c->sem);
		Kfree(c->rb);
		Kfree(c);
		return er;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = c;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)bt_task;
	ct.itskpri = BT_TSK_PRI;
	ct.stksz = 32 * 1024;		/* the curve's arithmetic of LE pairing runs here */
	c->tsk = tk_cre_tsk(&ct);
	if ( c->tsk > 0 ) (void)tk_sta_tsk(c->tsk, 0);
	bt_c = c;
	USB_LOG("bt: controller %04x:%04x%s\n", GET16(dev->ddesc + 8), GET16(dev->ddesc + 10), c->intel ? " (Intel)" : "");
	return E_OK;
}

/* With the manager's lock held */
EXPORT void knl_bt_detach( USBDEV *dev )
{
	BTC	*c = bt_c;
	INT	i;

	if ( c == NULL || c->dev != dev ) return;
	c->gone = TRUE;
	if ( c->ev_pipe != NULL ) dev->hc->ops->stream_close(dev->hc, c->ev_pipe);
	if ( c->acl_pipe != NULL ) dev->hc->ops->stream_close(dev->hc, c->acl_pipe);
	for ( i = 0; i < c->npeer; i++ ) {
		if ( c->peer[i]->used ) hid_let_go(c->peer[i]);
	}
	knl_bta_gone(c);
	knl_btlea_gone(c);
	(void)tk_sig_sem(c->sem, 1);
	bt_c = NULL;
	USB_LOG("bt: controller gone\n");
	/* the task and the room are left: the task may be in a call that holds a pointer to them */
}

/* ---------------------------------------------------------------- other ways */

/*
 * A controller reached another way (bt_uart.c), its task started: NULL
 * when one is there already, or there is no room. Its prep comes first.
 */
EXPORT BTC *knl_btx_open( CONST BTTRANS *tr, void *ctx )
{
	BTC	*c;
	T_CSEM	cs;
	T_CTSK	ct;

	c = (BTC *)Kcalloc(1, sizeof(BTC));
	if ( c == NULL ) return NULL;
	if ( !knl_btx_ring_make(c) ) {
		Kfree(c);
		tm_printf((UB *)"bt: no room for the controller's ring\n");
		return NULL;
	}
	c->tr = tr;
	c->tr_ctx = ctx;
	c->st.state = BT_ST_STARTING;
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RING_SEM_MAX;
	c->sem = tk_cre_sem(&cs);
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = c;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)bt_task;
	ct.itskpri = BT_TSK_PRI;
	ct.stksz = 32 * 1024;
	c->tsk = ( c->sem > 0 ) ? tk_cre_tsk(&ct) : E_LIMIT;
	knl_usb_lock();			/* as against a USB controller coming at the same time */
	if ( bt_c != NULL || c->tsk <= 0 ) {
		knl_usb_unlock();
		tm_printf((UB *)"bt: %s\n", ( bt_c != NULL ) ? "a second controller left alone" : "no task for the controller");
		if ( c->tsk > 0 ) tk_del_tsk(c->tsk);
		if ( c->sem > 0 ) tk_del_sem(c->sem);
		Kfree(c->rb);
		Kfree(c);
		return NULL;
	}
	bt_c = c;
	knl_usb_unlock();
	(void)tk_sta_tsk(c->tsk, 0);
	return c;
}

EXPORT BTC *knl_btx_ctl( void )
{
	return bt_c;
}

/* ---------------------------------------------------------------- what others ask */

EXPORT ER knl_bt_scan( INT seconds )
{
	BTC	*c = bt_c;

	if ( c == NULL ) return E_NOEXS;
	if ( seconds < 1 ) seconds = 1;
	if ( seconds > 30 ) seconds = 30;
	c->req_scan_s = seconds;
	return tk_sig_sem(c->sem, 1);
}

EXPORT ER knl_bt_pair( CONST UB *addr )
{
	BTC	*c = bt_c;

	if ( c == NULL ) return E_NOEXS;
	knl_memcpy(c->pair_addr, addr, 6);
	c->req_pair = TRUE;
	return tk_sig_sem(c->sem, 1);
}

EXPORT INT knl_bt_found( T_BTFOUND *buf, INT max )
{
	BTC	*c = bt_c;
	BTFOUND	* volatile *tab;
	INT	i, n;

	if ( c == NULL ) return E_NOEXS;
	/* the count first, then the table: one made after the count has them all */
	n = c->nfound;
	__sync_synchronize();
	tab = c->found;
	for ( i = 0; i < n && i < max; i++ ) buf[i] = tab[i]->f;
	return n;
}

EXPORT INT knl_bt_lines( UB *buf, INT size )
{
	BTC	*c = bt_c;

	if ( c == NULL ) return E_NOEXS;
	return bt_lines(c, buf, size, NULL);
}

EXPORT BOOL knl_bt_ready( void )
{
	return (BOOL)( bt_c != NULL && bt_c->ready );
}
