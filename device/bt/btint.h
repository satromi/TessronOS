/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btint.h
 *	What the parts of the Bluetooth driver share (design 6.8): bt.c, the
 *	controller and the classic link; bt_le.c, LE pairing and HID over
 *	GATT; bt_a2dp.c, sound to earphones. All of it runs in the
 *	controller's one task, except what bt_a2dp.c says of its stream.
 */

#ifndef __BTINT_H__
#define __BTINT_H__

#define RING_N			32
#define RING_LEN		( 300 )
#define NPEER			8
#define NFOUND			BT_FOUND_MAX
#define NQCMD			16
#define L2_MTU			672
#define TX_N			16	/* L2CAP frames waiting for the controller's room */
#define TX_MAX			1100

#define HID_RD_MAX		1024
#define SDP_BUF			1400

/* Commands: ogf << 10 | ocf */
#define C_DISCONNECT		0x0406
#define C_LE_SET_MASK		0x2001
#define C_LE_READ_BUF		0x2002
#define C_LE_SCAN_PARAM		0x200b
#define C_LE_SCAN_ENABLE	0x200c
#define C_LE_CREATE_CONN	0x200d
#define C_LE_CONN_CANCEL	0x200e
#define C_LE_CONN_UPDATE	0x2013
#define C_LE_START_ENC		0x2019
#define C_LE_READ_FEAT		0x2003
#define C_LE_EXT_SCAN_PARAM	0x2041
#define C_LE_EXT_SCAN_ENABLE	0x2042
#define C_LE_EXT_CREATE_CONN	0x2043
#define C_LE_READ_BUF_V2	0x2060
#define C_LE_HOST_FEAT		0x2074

/* Channels of L2CAP */
#define CID_ATT			0x0004
#define CID_LE_SIG		0x0005
#define CID_SMP			0x0006
#define CID_AV_SIG		0x0043
#define CID_AV_MEDIA		0x0044
/* The audio side's other channels (btaudio.h), this end's ends of them */
#define CID_X_FIRST		0x0045
#define CID_SDP_SRV		0x0045	/* a device asking this end's service records */
#define CID_AVCTP		0x0046	/* the remote control */
#define CID_RFCOMM		0x0047	/* RFCOMM, under the hands-free profile */
#define CID_SDP_HF		0x0048	/* this end asking where the device's hands-free service is */
#define NCIDX			4
#define CID_IS_X(cid)		( (cid) >= CID_X_FIRST && (cid) < CID_X_FIRST + NCIDX )

#define BT_ADDR_BYTES		6

/* The channel bits of cfg_in and cfg_out */
#define CHB_CTRL		1
#define CHB_INTR		2
#define CHB_SDP			4
#define CHB_AV_SIG		8
#define CHB_AV_MEDIA		16

/* Where a report in the report protocol has a keyboard's keys, in bits after any ID */
typedef struct {
	BOOL	has_kbd, has_ptr, has_cc;
	INT	rid;			/* the keyboard's report ID, 0 none */
	INT	mod_off;		/* the eight modifiers, -1 none */
	INT	arr_off, arr_n;		/* an array of 8-bit usages, -1 none */
	INT	bm_off, bm_min, bm_n;	/* a bit for each usage from bm_min, -1 none */
} KBDLAY;

typedef struct {
	UB	len_hi;			/* unused room kept with the bytes */
	UB	kind;			/* 0 event, 1 ACL */
	UH	len;
	UB	b[RING_LEN];
} RINGITEM;

/* A device that was paired, or is being; its object keeps it across a restart */
typedef struct {
	BOOL	used;
	UB	addr[6];
	UB	key[16];
	BOOL	have_key;
	UW	cod;
	UB	name[32];
	/* connected */
	UH	handle;			/* 0xffff none */
	BOOL	up;
	UH	rcid_ctrl, rcid_intr;	/* the device's ends of the channels */
	UB	cfg_done;		/* bit 0 ctrl both ways, 1 intr: as below */
	UB	cfg_in, cfg_out;	/* CHB_*: config answered that way */
	UB	ident;
	INT	hd;			/* the input layer's slot, 0 none */
	INT	hd2;			/* the slot of what is not its keyboard, in the report protocol */
	BOOL	hid_on;			/* the slots taken */
	BOOL	report_proto;		/* read in the report protocol */
	UB	sdp;			/* SDP_* */
	BOOL	sdp_asked;
	UH	rcid_sdp;
	UH	sdp_tid;
	UW	sdp_due;		/* when to stop waiting, in the low word of the operating time */
	INT	sdp_got;
	UB	sdp_buf[SDP_BUF];
	UB	rd[HID_RD_MAX];		/* its report descriptor, kept while the machine is on */
	INT	rdlen;
	KBDLAY	lay;
	UB	acl[L2_MTU + 8];	/* an L2CAP frame coming in pieces */
	INT	acl_have, acl_want;
	/* LE: kept */
	BOOL	le;
	UB	atype;			/* its address's type: 0 public, 1 random */
	UB	ltk[16];
	UB	rnd[8];
	UH	ediv;
	UB	irk[16];
	BOOL	has_irk;
	UB	key_sz;
	BOOL	le_sc;			/* the keys came of Secure Connections */
	/* LE: this connection */
	void	*les;			/* its state, bt_le.c */
	UB	cur_addr[6];		/* the address it comes from now, perhaps a private one */
	UB	cur_type;
	/* earphones */
	BOOL	audio;
	UH	rcid_av, rcid_media;
	UH	media_mtu;		/* what the device takes on the media channel */
	UH	rcid_x[NCIDX];		/* the device's ends of the audio side's other channels */
	UH	mtu_x[NCIDX];		/* and what it takes on each */
	UB	cfgx_in, cfgx_out;	/* a bit for each: config answered that way */
	UB	x_on;			/* and told up */
} PEER;

/*
 * A paired device kept in its object. What LE added comes after the
 * fields of the first form, so that a record of that form is still read.
 */
typedef struct {
	UB	addr[6];
	UB	key[16];
	UW	cod;
	UB	name[32];
	/* LE and audio */
	UB	le, atype, le_sc, key_sz;
	UB	ltk[16];
	UB	rnd[8];
	UH	ediv;
	UB	has_irk, audio;
	UB	irk[16];
} T_BTKEPT;
#define BTKEPT_OLD_SIZE		60	/* its size before the LE fields */

/*
 * How the controller is reached: on USB (bt.c), or on a UART in H4
 * (bt_uart.c). A command is its opcode, its length and its parameters;
 * an ACL packet its header and its data. What comes back goes into the
 * ring, as knl_btx_ring_in puts it. prep runs in the controller's task
 * before the ordinary start, NULL for nothing: FALSE leaves the
 * controller, the reason in its state.
 */
struct bt_ctl;
typedef struct {
	ER	(*cmd)( struct bt_ctl *c, UB *b, INT n );
	ER	(*acl)( struct bt_ctl *c, UB *b, INT n );
	BOOL	(*prep)( struct bt_ctl *c );
	ER	(*iso)( struct bt_ctl *c, UB *b, INT n );	/* an ISO data packet, NULL where none can go */
} BTTRANS;

typedef struct bt_ctl {
	CONST BTTRANS	*tr;
	void		*tr_ctx;	/* what the way it is reached keeps of its own */
	USBDEV		*dev;
	USBPIPE		*ev_pipe, *acl_pipe;
	USBEP		ep_out;
	BOOL		gone;
	ID		sem, tsk;
	RINGITEM	ring[RING_N];
	volatile UW	head, tail;
	UB		ev[260];	/* an event coming in pieces */
	INT		ev_have;
	UB		acl[1100];	/* and a piece of ACL data */
	INT		acl_have;
	/* the start */
	INT		step;
	BOOL		ready;
	BOOL		cmd_busy;
	UH		cmd_op;
	UW		cmd_ms;		/* when the command went */
	/* what a command waiting its turn will say: a small queue */
	UB		q[NQCMD][64];
	UB		qn[NQCMD];
	INT		qh, qt;
	UB		addr[6];
	BOOL		intel;
	UB		addr_status;	/* what asking its address answered */
	BOOL		has_le;
	T_BTSTATE	st;		/* what the device object tells */
	/* requests from outside */
	volatile INT	req_scan_s;
	volatile BOOL	req_pair, req_forget;
	UB		pair_addr[6], forget_addr[6];
	BOOL		obj_up;
	BOOL		recalled;	/* the devices kept have been read */
	BOOL		scanning;
	UW		scan_at;	/* when the scan was asked of the controller (ms) */
	INT		nfound;
	T_BTFOUND	found[NFOUND];
	PEER		peer[NPEER];
	/* what goes out: frames in order, as the controller has room for their packets */
	UH		acl_len, le_len;	/* the data of one packet */
	INT		acl_room, le_room;	/* the packets it can hold now */
	struct {
		UH	handle;
		BOOL	le;
		INT	n;
		UB	b[TX_MAX];
	}		tx[TX_N];
	INT		tx_h, tx_t, tx_off;
	/* LE */
	BOOL		le_scan_on, le_scan_active;
	BOOL		le_user_scan;
	PEER		*le_conn;	/* the connection being made */
	UW		le_conn_due;
	BOOL		le_conn_cancelled;
	INT		le_other;	/* devices seen that do not say they are for input */
	/* LE: what the controller can do, and its ISO packets (bt_iso.c) */
	UB		le_feat[8];
	BOOL		has_ext_adv;	/* extended advertising: the scan and the connection by the extended commands */
	BOOL		has_cis;	/* the central of CISes, with buffers for them: LE Audio */
	UH		iso_len;	/* the data of one ISO packet */
	INT		iso_room;	/* the ISO packets it can hold now */
} BTC;

IMPORT BOOL same_bt_bytes( CONST void *a, CONST void *b, INT n );

/* bt.c */
IMPORT void knl_btx_cmd( BTC *c, UH op, CONST UB *p, INT n );
IMPORT void knl_btx_l2_send( BTC *c, PEER *p, UH cid, CONST UB *data, INT n );
IMPORT BOOL knl_btx_tx_room( BTC *c, INT frames );
IMPORT void knl_btx_tx_pump( BTC *c );
IMPORT void knl_btx_sig_send( BTC *c, PEER *p, UB code, UB ident, CONST UB *d, INT n );
IMPORT void knl_btx_l2_open( BTC *c, PEER *p, UH psm, UH scid );
IMPORT PEER *knl_btx_peer_new( BTC *c, CONST UB *a );
IMPORT ER   knl_btx_keep( PEER *p );
IMPORT void knl_btx_rd_layout( CONST UB *rd, INT n, KBDLAY *l );
IMPORT void knl_btx_report_in( PEER *p, CONST UB *r, INT m );
IMPORT T_BTFOUND *knl_btx_found_add( BTC *c, CONST UB *addr, UW cod, CONST UB *name, INT nlen );
IMPORT UW   knl_btx_now( void );
IMPORT void knl_btx_ring_in( BTC *c, UB kind, CONST UB *b, INT len );
IMPORT BTC  *knl_btx_open( CONST BTTRANS *tr, void *ctx );

/* bt_intel.c */
IMPORT BOOL knl_bti_load( BTC *c );
IMPORT void knl_bti_log_bytes( CONST char *what, CONST UB *b, INT n );

/* bt_le.c */
IMPORT void knl_btl_event( BTC *c, CONST UB *d, INT len );
IMPORT void knl_btl_l2_in( BTC *c, PEER *p, INT cid, CONST UB *f, INT len );
IMPORT void knl_btl_enc_change( BTC *c, PEER *p, INT status, INT on );
IMPORT void knl_btl_down( BTC *c, PEER *p );
IMPORT void knl_btl_scan_start( BTC *c );
IMPORT void knl_btl_scan_end( BTC *c );
IMPORT void knl_btl_connect( BTC *c, PEER *p );
IMPORT void knl_btl_tick( BTC *c );

/* bt_a2dp.c */
IMPORT void knl_bta_sdp_answer( BTC *c, PEER *p, INT got );
IMPORT void knl_bta_open( BTC *c, PEER *p );
IMPORT void knl_bta_chan_up( BTC *c, PEER *p, UB bit );
IMPORT void knl_bta_chan_closed( BTC *c, PEER *p, UH ours );
IMPORT void knl_bta_sig_in( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_bta_down( BTC *c, PEER *p );
IMPORT void knl_bta_gone( BTC *c );
IMPORT void knl_bta_tick( BTC *c );
IMPORT UH   knl_bta_accept( BTC *c, PEER *p, UH psm );
IMPORT void knl_bta_x_up( BTC *c, PEER *p );
IMPORT void knl_bta_x_in( BTC *c, PEER *p, UH cid, CONST UB *f, INT n );
IMPORT void knl_bta_event( BTC *c, UB code, CONST UB *d, INT len );

/* bt_iso.c */
IMPORT void knl_btiso_cmd_done( BTC *c, UH op, INT st, CONST UB *r, INT n );
IMPORT void knl_btiso_cmd_status( BTC *c, UH op, INT st );
IMPORT BOOL knl_btiso_pkts_done( BTC *c, UH h, INT done );
IMPORT BOOL knl_btiso_disconn( BTC *c, UH h, INT reason );
IMPORT void knl_btiso_cis_est( BTC *c, CONST UB *d, INT len );

/* bt_bap.c: the services of LE Audio */
IMPORT void knl_btbap_reset( BTC *c, PEER *p );
IMPORT void knl_btbap_svc( BTC *c, PEER *p, UH uuid, UH first, UH last );
IMPORT BOOL knl_btbap_start( BTC *c, PEER *p, UH mtu );
IMPORT BOOL knl_btbap_att_in( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_btbap_down( BTC *c, PEER *p );
IMPORT BOOL knl_btbap_rsi( BTC *c, CONST UB *ad, INT n );
IMPORT BOOL knl_btbap_seek( BTC *c );

/* bt_lea.c: sound to LE Audio earphones */
IMPORT void knl_btlea_tick( BTC *c );
IMPORT void knl_btlea_gone( BTC *c );

#endif /* __BTINT_H__ */
