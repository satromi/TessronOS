/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_bcm.h
 *	A controller on a UART (design 6.8): the H4 framing (bt_h4.c), and
 *	the start of a Broadcom/Cypress controller with its patch (bt_bcm.c).
 *	Both are plain code over bytes, so that the tests run them on any
 *	machine; the UART itself is the board's (bt_uart.c).
 */

#ifndef __BT_BCM_H__
#define __BT_BCM_H__

/* The packet types of H4: the byte before each packet on the line */
#define H4_CMD			0x01
#define H4_ACL			0x02
#define H4_SCO			0x03
#define H4_EVENT		0x04
#define H4_ISO			0x05

/*
 * The room for a packet at the start, before the controller has said how
 * long its packets are: an event, whose length is one byte. A task makes
 * it as long as the controller's answers say (knl_bth4_room); the
 * interrupt handler never allocates.
 */
#define BTH4_FIRST		( 2 + 255 )

/*
 * What comes from the controller, a byte at a time: each whole packet
 * (without its type byte) goes to out. A byte that is not a known type
 * where a type belongs is passed over; a packet longer than the room,
 * longer than the controller said its packets are, is not one the
 * specification allows: it is passed over to its end and counted, so
 * that the next one is still found.
 */
typedef void (*BTH4_OUT)( void *ctx, UB type, CONST UB *p, INT len );

typedef struct {
	UB	type;			/* of the packet coming, 0 between packets */
	INT	hlen;			/* its header's length */
	INT	have;			/* its bytes held */
	INT	want;			/* its whole length, header included; 0 while the header is coming */
	INT	skip;			/* the bytes of a packet too long still to pass over */
	UW	frames;			/* whole packets given */
	UW	bad_type;		/* bytes passed over where a type belonged */
	UW	too_long;		/* packets passed over */
	T_SPLOCK lk;			/* the feeding, against the room made longer */
	UB	*b;			/* the packet coming */
	INT	cap;			/* and its room: the longest packet taken whole */
} BTH4;

/* Ready for a stream, its room BTH4_FIRST at least, kept from before: E_NOMEM when it cannot be had */
IMPORT ER   knl_bth4_init( BTH4 *h );
/* In a task: room for packets of n bytes, what is coming kept; E_NOMEM when it cannot be had */
IMPORT ER   knl_bth4_room( BTH4 *h, INT n );
IMPORT void knl_bth4_feed( BTH4 *h, CONST UB *b, INT n, BTH4_OUT out, void *ctx );
/* The header length of a type, 0 for one not known */
IMPORT INT  knl_bth4_hlen( UB type );
/* A packet with its type byte before it, into out: its length, or E_PAR when its header does not say n */
IMPORT INT  knl_bth4_pack( UB type, CONST UB *pkt, INT n, UB *out, INT max );
/* Whether the n bytes are one packet of the type, as its header says */
IMPORT BOOL knl_bth4_ok( UB type, CONST UB *pkt, INT n );

/*
 * A patch file (.hcd): HCI commands one after the other, each its
 * opcode, its length and its parameters, all of the vendor's group,
 * the last Launch RAM. The number of commands, or what is wrong and
 * where (*p_at, its offset).
 */
#define BTB_HCD_SHORT		(-1)	/* a command runs past the end */
#define BTB_HCD_NOTVENDOR	(-2)	/* a command not of the vendor's group */
#define BTB_HCD_NOLAUNCH	(-3)	/* it does not end with Launch RAM */
#define BTB_HCD_EMPTY		(-4)

IMPORT INT  knl_btb_hcd_check( CONST UB *d, INT n, INT *p_at );

/* How the controller is started: the board's side of it */
typedef struct {
	UW	init_baud;		/* the controller's speed after power on, and after its patch starts */
	UW	oper_baud;		/* the speed to run at; 0 stays at init_baud */
	UB	bdaddr[6];		/* the board's own address, low byte first; all zero none */
	ER	(*speed)( BTC *c, UW baud );	/* the host's end of the line to that speed */
	BOOL	sco_hci;		/* SCO to the host on the HCI rather than to the chip's PCM pins */
} BTBCFG;

/*
 * Before the ordinary start, in the controller's task. TRUE when that may
 * go on: the patch started, or there was none to give and the controller
 * runs its own; FALSE when the controller is left, the reason in c->st.
 * knl_btb_run is the same with the patch's bytes given (NULL none).
 */
IMPORT BOOL knl_btb_load( BTC *c, CONST BTBCFG *cfg );
IMPORT BOOL knl_btb_run( BTC *c, CONST BTBCFG *cfg, CONST UB *fw, INT n, CONST char *file );

#endif /* __BT_BCM_H__ */
