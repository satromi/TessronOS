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

/*
 * What comes from the controller waits in a ring of bytes, each item a
 * head of four bytes (its kind, its length in 24 bits) and its bytes. The
 * ring starts at RING_FIRST and is doubled by the controller's task once
 * it is half full; RING_LIMIT is what it may take, so that a controller
 * gone wrong does not take all the memory: past it what does not fit is
 * counted and said on the console.
 */
#define RING_FIRST		( 16 * 1024 )
#define RING_LIMIT		( 1024 * 1024 )
#define RING_SEM_MAX		0x7fffffff	/* the ring's semaphore counts every item */
#define HCI_PARAM_MAX		255	/* a command's parameters: their length is one byte (Core 5.4 Vol 4 Part E 5.4.1) */
#define L2_MTU			672
#define TX_PACE			15	/* frames waiting past which a stream waits for the controller (knl_btx_tx_room) */
#define ATT_VAL_MAX		512	/* an attribute's value (Core 5.4 Vol 3 Part F 3.2.9) */

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

/* The kinds of what comes in the ring */
#define RK_EVENT		0
#define RK_ACL			1
#define RK_SCO			2

/* A device that was paired, or is being; its object keeps it across a restart */
typedef struct {
	BOOL	used;
	INT	idx;			/* its place in the controller's table, which does not change */
	UB	addr[6];
	UB	key[16];
	BOOL	have_key;
	UW	cod;
	UB	name[BT_NAME_MAX + 1];
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
	UB	*sdp_buf;		/* the answer of its service records, as long as it comes */
	INT	sdp_cap;
	UB	*rd;			/* its report descriptor, kept while the machine is on */
	INT	rdlen, rd_cap;
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
	UB	hint;			/* FH_*: what its advertising said of it */
	BOOL	keep_wait;		/* paired on this link: kept once a service of it is used */
	/* earphones */
	BOOL	audio;
	UH	rcid_av, rcid_media;
	UH	media_mtu;		/* what the device takes on the media channel */
	UH	rcid_x[NCIDX];		/* the device's ends of the audio side's other channels */
	UH	mtu_x[NCIDX];		/* and what it takes on each */
	UB	cfgx_in, cfgx_out;	/* a bit for each: config answered that way */
	UB	x_on;			/* and told up */
	void	*bap;			/* its LE Audio services (bt_bap.c), NULL none yet */
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
	/* its whole name: name above has as much of it as fits, for a kernel of the forms before */
	UB	lname[BT_NAME_MAX + 1];
} T_BTKEPT;
#define BTKEPT_OLD_SIZE		60	/* its size before the LE fields */
#define BTKEPT_LE_SIZE		offsetof(T_BTKEPT, lname)	/* and before the whole name */

/*
 * What is known of a device found, beside its T_BTFOUND: where it was
 * seen, what its advertising said, and whether its name has been asked.
 */
#define FH_BREDR		0x01	/* an inquiry found it */
#define FH_LE			0x02	/* it advertises on LE */
#define FH_FLAGS		0x04	/* its advertising had the flags (AD type 1) */
#define FH_NO_BREDR		0x08	/* and they say BR/EDR is not supported */
#define FH_LEAUDIO		0x10	/* it says it serves LE Audio: ASCS, PACS or CAS */
#define FH_AUDIO		0x20	/* its appearance is one of sound: an audio sink, earphones */
#define FH_NAMED		0x40	/* its name has been asked of it (Remote Name Request) */

typedef struct {
	T_BTFOUND	f;
	UB		hint;		/* FH_* */
	UB		note;		/* BT_NOTE_*: what became of a try with it */
	UB		psrm;		/* its page scan repetition mode, from the inquiry */
	UH		clk;		/* and its clock offset */
} BTFOUND;

/* A command waiting its turn, and an L2CAP frame waiting for the controller's room */
typedef struct btcmdq {
	struct btcmdq	*next;
	INT		n;
	UB		b[3];		/* the opcode and the length, then the parameters: n in all */
} BTCMDQ;

typedef struct btframe {
	struct btframe	*next;
	UH		handle;
	BOOL		le;
	INT		n;
	UB		b[4];		/* the L2CAP header, then the rest: n in all */
} BTFRAME;

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
	ER	(*sco)( struct bt_ctl *c, UB *b, INT n );	/* an SCO data packet on the HCI, NULL where none can go */
	void	(*rx_size)( struct bt_ctl *c, INT n );	/* the longest packet the controller sends is n bytes; NULL nothing to do */
} BTTRANS;

/* Where a voice link's packets go (knl_bth_way) */
#define VW_NONE			0	/* nowhere: no voice */
#define VW_USB			1	/* the isochronous endpoints of the voice interface */
#define VW_HCI			2	/* SCO data packets, as the transport has them */

typedef struct bt_ctl {
	CONST BTTRANS	*tr;
	void		*tr_ctx;	/* what the way it is reached keeps of its own */
	USBDEV		*dev;
	USBPIPE		*ev_pipe, *acl_pipe;
	USBEP		ep_out;
	BOOL		gone;
	ID		sem, tsk;
	T_SPLOCK	rlk;		/* the ring's writers, and its growing */
	UB		*rb;		/* the ring: items of a head and their bytes */
	UW		rcap;		/* its bytes, a power of two */
	volatile UW	head, tail;	/* where its items are, in bytes, running on past rcap */
	volatile BOOL	rgrow;		/* half full: the task makes it bigger */
	volatile UW	rlost;		/* items that did not fit */
	UB		*ri;		/* the item taken out of it, whole */
	INT		ri_cap;
	UB		ev[2 + 255];	/* an event coming in pieces: its length is one byte */
	INT		ev_have;
	UB		*acl;		/* and an ACL packet, as long as its header says */
	INT		acl_have, acl_cap;
	/* the start */
	INT		step;
	BOOL		ready;
	BOOL		cmd_busy;
	UH		cmd_op;
	UW		cmd_ms;		/* when the command went */
	UB		lmp[8];		/* its LMP features, as Read Local Supported Features answered */
	UB		sco_mtu;	/* the data of one SCO packet on the HCI (Read Buffer Size) */
	UH		sco_num;	/* and the packets it holds */
	BOOL		sco_hci;	/* its SCO goes to the host on the HCI, not to pins of its own (bt_bcm.c) */
	/* the commands waiting their turn, in order */
	BTCMDQ		*q_first, *q_last;
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
	BOOL		scanning;	/* a search: the inquiry, then the names it did not tell */
	BOOL		inq_on;		/* the inquiry itself */
	UW		scan_at;	/* when the scan was asked of the controller (ms) */
	/*
	 * The devices the search found, as many as it finds. Each is room
	 * of its own, never let go: a new search uses them again. The table
	 * is made anew twice as big when full, the old one kept, so that a
	 * reader in another task that took nfound and then found stays in it.
	 */
	volatile INT	nfound;
	BTFOUND		* volatile *found;
	INT		found_cap, found_made;
	BOOL		naming;		/* a Remote Name Request of the search is out */
	UB		name_addr[6];
	UW		name_due;
	/* a device whose BR/EDR side is paired once its LE link has gone */
	BOOL		fb_on;
	UH		fb_handle;
	UB		fb_addr[6];
	UB		fb_name[BT_NAME_MAX + 1];
	/* the devices paired or being, grown as the found ones are */
	volatile INT	npeer;
	PEER		* volatile *peer;
	INT		peer_cap;
	/* what goes out: frames in order, as the controller has room for their packets */
	UH		acl_len, le_len;	/* the data of one packet */
	INT		acl_room, le_room;	/* the packets it can hold now */
	BTFRAME		*tx_first, *tx_last;
	INT		tx_count, tx_off;
	UB		*txp;		/* one packet going out */
	INT		txp_cap;
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
	/* the USB device suspended while idle (bt.c) */
	BOOL		autosusp;	/* allowed */
	volatile UW	last_busy;	/* when something last went to it or came from it (ms) */
} BTC;

IMPORT BOOL same_bt_bytes( CONST void *a, CONST void *b, INT n );

/* bt.c */
IMPORT ER   knl_btx_cmd( BTC *c, UH op, CONST UB *p, INT n );	/* E_PAR past HCI_PARAM_MAX */
IMPORT ER   knl_btx_l2_send( BTC *c, PEER *p, UH cid, CONST UB *data, INT n );
IMPORT BOOL knl_btx_tx_room( BTC *c, INT frames );
IMPORT void knl_btx_tx_pump( BTC *c );
IMPORT void knl_btx_sig_send( BTC *c, PEER *p, UB code, UB ident, CONST UB *d, INT n );
IMPORT void knl_btx_l2_open( BTC *c, PEER *p, UH psm, UH scid );
IMPORT PEER *knl_btx_peer_new( BTC *c, CONST UB *a );
IMPORT ER   knl_btx_keep( PEER *p );
IMPORT void knl_btx_rd_layout( CONST UB *rd, INT n, KBDLAY *l );
IMPORT void knl_btx_report_in( PEER *p, CONST UB *r, INT m );
IMPORT BTFOUND *knl_btx_found_add( BTC *c, CONST UB *addr, UW cod, CONST UB *name, INT nlen );	/* NULL only with no memory for it */
IMPORT void knl_btx_le_unused( BTC *c, PEER *p, BOOL lea );
IMPORT UW   knl_btx_now( void );
IMPORT void knl_btx_ring_in( BTC *c, UB kind, CONST UB *b, INT len );
IMPORT BOOL knl_btx_ring_make( BTC *c );	/* FALSE: no room for it */
IMPORT BOOL knl_btx_ring_take( BTC *c, UB *p_kind, CONST UB **p_b, INT *p_len );
IMPORT void knl_btx_ring_grow( BTC *c );
IMPORT BOOL knl_btx_rd_room( PEER *p, INT n );	/* room for n bytes of its descriptor */
IMPORT BTFOUND *knl_btx_found_at( BTC *c, CONST UB *addr );
IMPORT BTC  *knl_btx_open( CONST BTTRANS *tr, void *ctx );
IMPORT BTC  *knl_btx_ctl( void );		/* the controller in use, NULL none: for the tests */

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
IMPORT BOOL knl_bta_ready( CONST PEER *p );	/* its stream set up: a sound output */

/* bt_hfp.c: the voice */
IMPORT void knl_bth_sco_in( BTC *c, CONST UB *b, INT len );	/* an SCO data packet that came on the HCI */
IMPORT INT  knl_bth_way( BTC *c );		/* VW_* */
IMPORT UH   knl_bth_sdp_features( BTC *c );	/* this end's features in its audio gateway's record */

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
IMPORT BOOL knl_btbap_has_lea( BTC *c, PEER *p );	/* PACS and ASCS among its services */
IMPORT BOOL knl_btbap_ready( BTC *c, PEER *p );	/* taken as LE Audio, its services read */

/* bt_lea.c: sound to LE Audio earphones */
IMPORT void knl_btlea_tick( BTC *c );
IMPORT void knl_btlea_gone( BTC *c );

#endif /* __BTINT_H__ */
