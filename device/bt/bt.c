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

LOCAL void ring_put( BTC *c, UB kind, DMABUF *b, INT off, INT len )
{
	UW		next = ( c->tail + 1 ) % RING_N;
	RINGITEM	*r;

	if ( next == c->head ) {
		return;				/* the oldest kept: a later one would make no sense without it */
	}
	r = &c->ring[c->tail];
	if ( len > RING_LEN ) len = RING_LEN;
	knl_dmabuf_read(b, off, r->b, len);
	r->kind = kind;
	r->len = (UH)len;
	c->tail = next;
	(void)tk_sig_sem(c->sem, 1);
}

LOCAL void ev_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	if ( err == 0 && len > 0 ) ring_put((BTC *)ctx, 0, b, off, len);
}

LOCAL void acl_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	if ( err == 0 && len > 0 ) ring_put((BTC *)ctx, 1, b, off, len);
}

/*
 * What a way other than USB has had from the controller, a whole event
 * or ACL packet, into the ring in pieces it holds. From an interrupt
 * handler too: it is the ring's only writer.
 */
EXPORT void knl_btx_ring_in( BTC *c, UB kind, CONST UB *b, INT len )
{
	while ( len > 0 ) {
		UW		next = ( c->tail + 1 ) % RING_N;
		INT		k = ( len > RING_LEN ) ? RING_LEN : len;
		RINGITEM	*r;

		if ( next == c->head ) break;		/* the oldest kept, as ring_put does */
		r = &c->ring[c->tail];
		knl_memcpy(r->b, b, k);
		r->kind = kind;
		r->len = (UH)k;
		__sync_synchronize();		/* the item before the place that says it is there */
		c->tail = next;
		b += k;
		len -= k;
	}
	(void)tk_sig_sem(c->sem, 1);
}

/* ---------------------------------------------------------------- sending */

EXPORT UW knl_btx_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return t.lo;
}

LOCAL void cmd_now( BTC *c, CONST UB *cmd, INT n )
{
	UB	buf[64];
	ER	er;

	knl_memcpy(buf, cmd, n);
	c->cmd_op = (UH)( cmd[0] | ( cmd[1] << 8 ) );
	c->cmd_busy = TRUE;
	c->cmd_ms = knl_btx_now();
	er = c->gone ? E_IO : c->tr->cmd(c, buf, n);
	if ( er < E_OK ) {
		tm_printf((UB *)"bt: command %04x not taken (%d)\n", c->cmd_op, (INT)er);
		c->cmd_busy = FALSE;
	}
}

/* A command, sent now or when those before it have answered */
EXPORT void knl_btx_cmd( BTC *c, UH op, CONST UB *p, INT n )
{
	UB	b[64];

	b[0] = (UB)op;
	b[1] = (UB)( op >> 8 );
	b[2] = (UB)n;
	if ( n > 0 ) knl_memcpy(b + 3, p, n);
	if ( !c->cmd_busy ) {
		cmd_now(c, b, n + 3);
	} else if ( ( c->qt + 1 ) % NQCMD != c->qh ) {
		knl_memcpy(c->q[c->qt], b, n + 3);
		c->qn[c->qt] = (UB)( n + 3 );
		c->qt = ( c->qt + 1 ) % NQCMD;
	} else {
		tm_printf((UB *)"bt: command %04x dropped: too many waiting\n", op);
	}
}

LOCAL void cmd_answered( BTC *c )
{
	c->cmd_busy = FALSE;
	if ( c->qh != c->qt ) {
		INT	h = c->qh;

		c->qh = ( c->qh + 1 ) % NQCMD;
		cmd_now(c, c->q[h], c->qn[h]);
	}
}

/*
 * What is waiting goes out while the controller has room, each frame in
 * as many packets as its packet size takes. Called with no lock held.
 */
EXPORT void knl_btx_tx_pump( BTC *c )
{
	UB	b[4 + TX_MAX];

	while ( c->tx_h != c->tx_t ) {
		INT	h = c->tx_h;
		BOOL	le = c->tx[h].le;
		INT	max = le ? c->le_len : c->acl_len;
		INT	*room = le ? &c->le_room : &c->acl_room;
		INT	n = c->tx[h].n - c->tx_off;
		UW	pb;
		ER	er;

		if ( *room <= 0 ) return;
		if ( n > max ) n = max;
		/* a first piece flushable on the classic link, not on LE, which has no such thing */
		pb = ( c->tx_off > 0 ) ? 0x10 : le ? 0x00 : 0x20;
		b[0] = (UB)c->tx[h].handle;
		b[1] = (UB)( ( c->tx[h].handle >> 8 ) | pb );
		b[2] = (UB)n;
		b[3] = (UB)( n >> 8 );
		knl_memcpy(b + 4, c->tx[h].b + c->tx_off, n);
		( *room )--;
		er = c->gone ? E_IO : c->tr->acl(c, b, n + 4);
		if ( er < E_OK ) tm_printf((UB *)"bt: ACL out on %d failed (%d)\n", c->tx[h].handle, (INT)er);
		c->tx_off += n;
		if ( c->tx_off >= c->tx[h].n ) {
			c->tx_off = 0;
			c->tx_h = ( h + 1 ) % TX_N;
		}
	}
}

/* Whether that many more frames would wait without one being lost */
EXPORT BOOL knl_btx_tx_room( BTC *c, INT frames )
{
	return (BOOL)( ( ( c->tx_h - c->tx_t - 1 + 2 * TX_N ) % TX_N ) >= frames );
}

/* An L2CAP frame on the connection */
EXPORT void knl_btx_l2_send( BTC *c, PEER *p, UH cid, CONST UB *data, INT n )
{
	INT	t = c->tx_t, next = ( t + 1 ) % TX_N;

	if ( n + 4 > TX_MAX ) {
		tm_printf((UB *)"bt: a frame of %d bytes for channel %04x lost: longer than %d\n", n, cid, TX_MAX - 4);
		return;
	}
	if ( next == c->tx_h ) {
		tm_printf((UB *)"bt: a frame for channel %04x lost: too many waiting\n", cid);
		return;
	}
	c->tx[t].handle = p->handle;
	c->tx[t].le = (BOOL)( p->le && c->le_room >= 0 && c->le_len > 0 );
	c->tx[t].n = n + 4;
	c->tx[t].b[0] = (UB)n;
	c->tx[t].b[1] = (UB)( n >> 8 );
	c->tx[t].b[2] = (UB)cid;
	c->tx[t].b[3] = (UB)( cid >> 8 );
	knl_memcpy(c->tx[t].b + 4, data, n);
	c->tx_t = next;
	knl_btx_tx_pump(c);
}

EXPORT void knl_btx_sig_send( BTC *c, PEER *p, UB code, UB ident, CONST UB *d, INT n )
{
	UB	b[4 + 32];

	b[0] = code;
	b[1] = ident;
	b[2] = (UB)n;
	b[3] = 0;
	if ( n > 0 ) knl_memcpy(b + 4, d, n);
	knl_btx_l2_send(c, p, CID_SIG, b, n + 4);
}

/* ---------------------------------------------------------------- the devices */

LOCAL BOOL same_addr( CONST UB *a, CONST UB *b )
{
	return same_bt_bytes(a, b, 6);
}

LOCAL PEER *peer_of_addr( BTC *c, CONST UB *a )
{
	INT	i;

	for ( i = 0; i < NPEER; i++ ) {
		if ( c->peer[i].used && same_addr(c->peer[i].addr, a) ) return &c->peer[i];
	}
	return NULL;
}

LOCAL PEER *peer_of_handle( BTC *c, UH h )
{
	INT	i;

	for ( i = 0; i < NPEER; i++ ) {
		if ( c->peer[i].used && c->peer[i].up && c->peer[i].handle == h ) return &c->peer[i];
	}
	return NULL;
}

EXPORT PEER *knl_btx_peer_new( BTC *c, CONST UB *a )
{
	INT	i;
	PEER	*p = peer_of_addr(c, a);

	if ( p != NULL ) return p;
	for ( i = 0; i < NPEER; i++ ) {
		p = &c->peer[i];
		if ( !p->used ) {
			knl_memset(p, 0, sizeof(*p));
			p->used = TRUE;
			knl_memcpy(p->addr, a, 6);
			p->handle = 0xffff;
			return p;
		}
	}
	return NULL;
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
	knl_memcpy(k.name, p->name, sizeof(k.name));
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
			knl_memcpy(p->name, k.name, sizeof(p->name));
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
				  k.addr[5], k.addr[4], k.addr[3], k.addr[2], k.addr[1], k.addr[0], k.name, k.cod);
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
	if ( 7 + cnt >= n || p->sdp_got + cnt > SDP_BUF ) {
		tm_printf((UB *)"bt: \"%s\": an answer of the service records too long or cut short (%d, %d)\n", p->name, cnt, p->sdp_got);
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

		if ( sdp_find_rd(p->sdp_buf, p->sdp_got, &rd, &len) && len > 0 && len <= HID_RD_MAX ) {
			knl_memcpy(p->rd, rd, len);
			p->rdlen = len;
			knl_btx_rd_layout(p->rd, p->rdlen, &p->lay);
			tm_printf((UB *)"bt: \"%s\": report descriptor of %d bytes: keyboard %d (ID %d), pointer %d, consumer %d\n",
				  p->name, len, p->lay.has_kbd, p->lay.rid, p->lay.has_ptr, p->lay.has_cc);
		} else if ( len > HID_RD_MAX ) {
			tm_printf((UB *)"bt: \"%s\": a report descriptor of %d bytes left\n", p->name, len);
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

EXPORT T_BTFOUND *knl_btx_found_add( BTC *c, CONST UB *addr, UW cod, CONST UB *name, INT nlen )
{
	INT		i;
	T_BTFOUND	*f = NULL;

	for ( i = 0; i < c->nfound; i++ ) {
		if ( same_addr(c->found[i].addr, addr) ) f = &c->found[i];
	}
	if ( f == NULL ) {
		if ( c->nfound >= NFOUND ) return NULL;
		f = &c->found[c->nfound++];
		knl_memset(f, 0, sizeof(*f));
		knl_memcpy(f->addr, addr, 6);
		tm_printf((UB *)"bt: found %02x:%02x:%02x:%02x:%02x:%02x, class %06x\n",
			  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], cod);
	}
	f->cod = cod;
	if ( name != NULL && nlen > 0 ) {
		if ( nlen > (INT)sizeof(f->name) - 1 ) nlen = (INT)sizeof(f->name) - 1;
		knl_memcpy(f->name, name, nlen);
		f->name[nlen] = 0;
	}
	return f;
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
	switch ( code ) {
	case E_CMD_DONE: {
		UH	op = (UH)( d[1] | ( d[2] << 8 ) );
		UB	st = d[3];

		if ( st != 0 ) tm_printf((UB *)"bt: command %04x answered with status %02x\n", op, st);
		if ( op == C_READ_ADDR && st == 0 ) knl_memcpy(c->addr, d + 4, 6);
		if ( op == C_READ_ADDR ) c->addr_status = st;
		if ( op == C_READ_VER && st == 0 ) {
			tm_printf((UB *)"bt: HCI version %d, revision %04x, LMP %d.%04x, maker %04x\n",
				  d[4], d[5] | ( d[6] << 8 ), d[7], d[10] | ( d[11] << 8 ), d[8] | ( d[9] << 8 ));
		}
		if ( op == C_READ_FEATURES && st == 0 ) {
			c->has_le = (BOOL)( ( d[8] >> 6 ) & 1 );
			/* byte 4 bit 6 LE, byte 6 bit 3 Secure Simple Pairing: what the controller can be asked for */
			tm_printf((UB *)"bt: features %02x %02x %02x %02x %02x %02x %02x %02x: LE %d, simple pairing %d\n",
				  d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], ( d[8] >> 6 ) & 1, ( d[10] >> 3 ) & 1);
		}
		if ( op == C_READ_BUF && st == 0 ) {
			c->acl_len = (UH)( d[4] | ( d[5] << 8 ) );
			c->acl_room = d[7] | ( d[8] << 8 );
			tm_printf((UB *)"bt: ACL packets of %d bytes, %d at a time\n", c->acl_len, c->acl_room);
			if ( c->acl_len > TX_MAX ) c->acl_len = TX_MAX;
		}
		if ( op == C_LE_READ_BUF && st == 0 ) {
			c->le_len = (UH)( d[4] | ( d[5] << 8 ) );
			c->le_room = d[6];
			tm_printf((UB *)"bt: LE packets of %d bytes, %d at a time\n", c->le_len, c->le_room);
		}
		knl_btiso_cmd_done(c, op, st, d + 4, len - 4);
		cmd_answered(c);
		/* a reset refused is what a controller that cannot run says: nothing after it would work */
		if ( !c->ready && op == C_RESET && st != 0 ) start_stuck(c, op, st);
		if ( !c->ready ) start_next(c);
		break;
	}
	case E_CMD_STATUS:
		if ( d[0] != 0 ) tm_printf((UB *)"bt: command %04x not started, status %02x\n", d[2] | ( d[3] << 8 ), d[0]);
		if ( d[0] != 0 && (UH)( d[2] | ( d[3] << 8 ) ) == C_INQUIRY ) c->scanning = FALSE;
		knl_btiso_cmd_status(c, (UH)( d[2] | ( d[3] << 8 ) ), d[0]);
		knl_bta_event(c, code, d, len);
		cmd_answered(c);
		break;
	case E_INQ_RESULT:
		for ( i = 0; i < d[0] && 1 + 14 * ( i + 1 ) <= len; i++ ) {
			CONST UB *a = d + 1 + 6 * i;
			CONST UB *cl = d + 1 + 6 * d[0] + 3 * d[0] + 3 * i;

			(void)knl_btx_found_add(c, a, cl[0] | ( cl[1] << 8 ) | ( (UW)cl[2] << 16 ), NULL, 0);
		}
		break;
	case E_INQ_RSSI:
		if ( len >= 15 ) (void)knl_btx_found_add(c, d + 1, d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), NULL, 0);
		break;
	case E_EXT_INQ:
		if ( len >= 15 ) {
			INT	at = 15;

			(void)knl_btx_found_add(c, d + 1, d[9] | ( d[10] << 8 ) | ( (UW)d[11] << 16 ), NULL, 0);
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
		c->scanning = FALSE;
		/* the status: 0 when the inquiry ran its length, an error code when the controller ended it */
		tm_printf((UB *)"bt: scan over, %d found (status %02x, after %d ms)\n",
			  c->nfound, ( len > 0 ) ? d[0] : 0xff, (INT)( knl_btx_now() - c->scan_at ));
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
	case E_NAME_DONE:
		p = peer_of_addr(c, d + 1);
		if ( p != NULL && d[0] == 0 ) {
			knl_memset(p->name, 0, sizeof(p->name));
			for ( i = 0; i < (INT)sizeof(p->name) - 1 && d[7 + i] != 0 && 7 + i < len; i++ ) p->name[i] = d[7 + i];
			if ( p->have_key ) (void)knl_btx_keep(p);
		}
		break;
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

LOCAL ER bo_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	BTC		*c = (BTC *)ctx;
	T_BTLINE	*l = (T_BTLINE *)buf, one;
	INT		i, k, n = 0, max = (INT)( size / (SZ)sizeof(T_BTLINE) );

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
	if ( off != 0 ) {
		if ( p_asize != NULL ) *p_asize = 0;
		return E_OK;
	}
	for ( i = 0; i < NPEER && n < max; i++ ) {
		PEER	*p = &c->peer[i];

		if ( !p->used || !p->have_key ) continue;
		knl_memset(&one, 0, sizeof(one));
		knl_memcpy(one.addr, p->addr, 6);
		one.paired = 1;
		one.connected = (UB)p->up;
		one.cod = p->cod;
		knl_memcpy(one.name, p->name, sizeof(one.name));
		l[n++] = one;
	}
	for ( i = 0; i < c->nfound && n < max; i++ ) {
		BOOL	had = FALSE;

		for ( k = 0; k < NPEER; k++ ) {
			had = had || ( c->peer[k].used && c->peer[k].have_key && same_addr(c->peer[k].addr, c->found[i].addr) );
		}
		if ( had ) continue;
		knl_memset(&one, 0, sizeof(one));
		knl_memcpy(one.addr, c->found[i].addr, 6);
		one.cod = c->found[i].cod;
		knl_memcpy(one.name, c->found[i].name, sizeof(one.name));
		l[n++] = one;
	}
	if ( p_asize != NULL ) *p_asize = (SZ)n * (SZ)sizeof(T_BTLINE);
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
	TS_UUID	*ids;
	char	idb[TS_UUID_STRLEN + 1];
	INT	n = 0, i;
	UB	r[3];

	if ( p == NULL ) return;
	if ( p->up ) {
		r[0] = (UB)p->handle;
		r[1] = (UB)( p->handle >> 8 );
		r[2] = 0x13;			/* the person ended it */
		knl_btx_cmd(c, C_DISCONNECT, r, 3);
	}
	if ( ( ids = bt_links(&n) ) != NULL ) {
		for ( i = 0; i < n; i++ ) {
			T_BTKEPT	o;
			SZ		asz = 0;
			ID		key;
			BOOL		mine = FALSE;

			if ( !bt_is_obj(&ids[i]) ) continue;
			key = ob_opn_obj(&ids[i], OB_OP_R);
			if ( key <= 0 ) continue;
			mine = (BOOL)( ob_rea_rec(key, 1, 0, &o, sizeof(o), &asz) >= E_OK && same_addr(o.addr, p->addr) );
			(void)ob_cls_obj(key);
			if ( mine ) {
				(void)knl_conf_link_drop(bt_conf(idb), CF_ROLE_BTDEV, &ids[i]);
				(void)ob_del_obj(&ids[i]);
				break;
			}
		}
		Kfree(ids);
	}
	tm_printf((UB *)"bt: forgot %02x:..:%02x\n", p->addr[5], p->addr[0]);
	p->have_key = FALSE;
	if ( !p->up ) p->used = FALSE;
}

/* ---------------------------------------------------------------- the task */

/* The endpoints give a packet at a time; what is above takes a whole event, a whole ACL packet */
LOCAL void ev_piece( BTC *c, CONST UB *b, INT n )
{
	if ( c->ev_have + n > (INT)sizeof(c->ev) ) c->ev_have = 0;
	knl_memcpy(c->ev + c->ev_have, b, n);
	c->ev_have += n;
	if ( c->ev_have >= 2 && c->ev_have >= c->ev[1] + 2 ) {
		ev_in(c, c->ev, c->ev_have);
		c->ev_have = 0;
	}
}

LOCAL void acl_piece( BTC *c, CONST UB *b, INT n )
{
	if ( c->acl_have + n > (INT)sizeof(c->acl) ) c->acl_have = 0;
	knl_memcpy(c->acl + c->acl_have, b, n);
	c->acl_have += n;
	if ( c->acl_have >= 4 && c->acl_have >= ( c->acl[2] | ( c->acl[3] << 8 ) ) + 4 ) {
		acl_in(c, c->acl, c->acl_have);
		c->acl_have = 0;
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
		PEER		*p = knl_btx_peer_new(c, c->pair_addr);
		INT		i;

		c->req_pair = FALSE;
		if ( p == NULL ) {
			tm_printf((UB *)"bt: no room for another device\n");
			return;
		}
		for ( i = 0; i < c->nfound; i++ ) {
			if ( same_addr(c->found[i].addr, c->pair_addr) ) {
				p->cod = c->found[i].cod;
				knl_memcpy(p->name, c->found[i].name, sizeof(p->name));
				if ( c->found[i].le ) {
					p->le = TRUE;
					p->atype = c->found[i].atype;
				}
			}
		}
		if ( p->le ) {
			knl_memcpy(p->cur_addr, p->addr, 6);
			p->cur_type = p->atype;
			knl_btl_connect(c, p);
			return;
		}
		knl_memcpy(r, c->pair_addr, 6);
		r[6] = 0x18;			/* the packets DM1, DH1 ... */
		r[7] = 0xcc;
		r[8] = 1;			/* page scan repetition R1 */
		r[9] = 0;
		r[10] = r[11] = 0;
		r[12] = 1;			/* a role change allowed */
		knl_btx_cmd(c, C_CREATE_CONN, r, 13);
		if ( p->name[0] == 0 ) {
			knl_memcpy(r, c->pair_addr, 6);
			r[6] = 1;
			r[7] = 0;
			r[8] = r[9] = 0;
			knl_btx_cmd(c, C_NAME_REQ, r, 10);
		}
	}
}

/* A device that has not answered about its records is read as if it had none */
LOCAL void sdp_overdue( BTC *c )
{
	SYSTIM	t;
	INT	i;

	(void)tk_get_otm(&t);
	for ( i = 0; i < NPEER; i++ ) {
		PEER	*p = &c->peer[i];

		if ( p->used && p->up && p->sdp == SDP_RUN && (W)( t.lo - p->sdp_due ) >= 0 ) {
			tm_printf((UB *)"bt: \"%s\": the service records did not answer\n", p->name);
			sdp_end(c, p);
		}
	}
}

LOCAL void bt_task( INT stacd, void *exinf )
{
	BTC	*c = (BTC *)exinf;

	(void)stacd;
	requests(c);			/* the object first: a load takes seconds, and the settings tell of it */
	if ( ( !c->intel || knl_bti_load(c) ) && ( c->tr->prep == NULL || c->tr->prep(c) ) ) start_next(c);
	while ( !c->gone ) {
		(void)tk_wai_sem(c->sem, 1, 500);
		while ( c->head != c->tail ) {
			RINGITEM	*r = &c->ring[c->head];

			if ( r->kind == 0 ) ev_piece(c, r->b, r->len);
			else acl_piece(c, r->b, r->len);
			c->head = ( c->head + 1 ) % RING_N;
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
		requests(c);
		sdp_overdue(c);
		knl_btl_tick(c);
		knl_bta_tick(c);
	}
	if ( c->obj_up ) (void)knl_obdev_del((CONST UB *)"Bluetooth");
	tk_exd_tsk();
}

/* ---------------------------------------------------------------- the USB side */

LOCAL ER usb_cmd( BTC *c, UB *b, INT n )
{
	ER	er;

	knl_usb_lock();
	er = c->gone ? E_IO : knl_usb_control(c->dev, USB_RT_CLASS, REQ_HCI_COMMAND, 0, 0, b, (UH)n, NULL);
	knl_usb_unlock();
	return er;
}

LOCAL ER usb_acl( BTC *c, UB *b, INT n )
{
	INT	got = 0;
	ER	er;

	knl_usb_lock();
	er = c->gone ? E_IO : c->dev->hc->ops->bulk(c->dev->hc, c->dev, &c->ep_out, b, n, &got, 1000);
	knl_usb_unlock();
	return er;
}

/* ISO data goes on the same bulk endpoint as ACL data */
LOCAL CONST BTTRANS	usb_tr = { usb_cmd, usb_acl, NULL, usb_acl };

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
	c->tr = &usb_tr;
	c->dev = dev;
	c->intel = (BOOL)( GET16(dev->ddesc + 8) == USB_VID_INTEL );
	c->st.state = BT_ST_STARTING;
	c->st.vid = (UH)GET16(dev->ddesc + 8);
	c->st.pid = (UH)GET16(dev->ddesc + 10);
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RING_N * 2;
	c->sem = tk_cre_sem(&cs);
	if ( c->sem <= 0 ) {
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
	for ( i = 0; i < NPEER; i++ ) {
		if ( c->peer[i].used ) hid_let_go(&c->peer[i]);
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
	c->tr = tr;
	c->tr_ctx = ctx;
	c->st.state = BT_ST_STARTING;
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = RING_N * 2;
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
		Kfree(c);
		return NULL;
	}
	bt_c = c;
	knl_usb_unlock();
	(void)tk_sta_tsk(c->tsk, 0);
	return c;
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
	INT	i;

	if ( c == NULL ) return E_NOEXS;
	for ( i = 0; i < c->nfound && i < max; i++ ) buf[i] = c->found[i];
	return c->nfound;
}

EXPORT BOOL knl_bt_ready( void )
{
	return (BOOL)( bt_c != NULL && bt_c->ready );
}
