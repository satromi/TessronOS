/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_rfcomm.c
 *	RFCOMM under the hands-free profile (design 6.8): one session with
 *	the earphones on L2CAP, and in it the one channel that carries HFP's
 *	AT commands.
 *
 *	The session's frames are those of TS 07.10 as RFCOMM takes them:
 *	the address (the DLCI, command or response, and the bit that ends
 *	it), the control (SABM, UA, DM, DISC, UIH), the length and the check
 *	sum, which is over the address and the control of a UIH frame and
 *	over the length too of the others. The multiplexer's channel, DLCI
 *	0, carries the parameters of a channel (PN), the state of its lines
 *	(MSC) and what else 07.10 asks there; what this end does not have is
 *	answered "not supported" (NSC).
 *
 *	Either end may start the session: the earphones open the channel of
 *	this end's audio gateway (RFC_CH_AG), or this end opens their
 *	hands-free channel, which their service records name. The one that
 *	starts it is its initiator, which decides the C/R bit of each frame:
 *	a command from the initiator, and an answer from the other end, have
 *	it set. A channel opened by the initiator has the direction bit 0
 *	in its DLCI, one opened by the other end 1.
 *
 *	The flow is by credits when both say so in PN, as earphones do: each
 *	UIH frame with data takes one of the credits the other end gave, and
 *	this end gives more as it takes frames.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "../snd/sndhw.h"
#include "btint.h"
#include "btaudio.h"

/* Control: the frame's type, and the P/F bit */
#define F_SABM			0x2f
#define F_UA			0x63
#define F_DM			0x0f
#define F_DISC			0x43
#define F_UIH			0xef
#define F_PF			0x10

/* The multiplexer's messages, their type with the EA bit and C/R as a command */
#define M_PN			0x80
#define M_MSC			0xe0
#define M_RPN			0x90
#define M_RLS			0x50
#define M_TEST			0x20
#define M_FCON			0xa0
#define M_FCOFF			0x60
#define M_NSC			0x10
#define M_CR			0x02
#define M_EA			0x01

#define CL_CREDITS_ASK		0xf0	/* PN: credits, asked */
#define CL_CREDITS_YES		0xe0	/* and answered */
#define V24_SIGNALS		0x8d	/* ready to communicate, ready to receive, data valid */

#define N1_MINE			330	/* the longest information this end takes in a frame */
#define N1_DEFAULT		127
#define CREDITS_GIVE		7
#define CREDITS_LOW		3
#define RFC_TMO_MS		10000
#define TXQ_MAX			1024

enum { S_NONE, S_L2, S_SABM0, S_MUX, S_DISC0 };
enum { D_NONE, D_PN, D_SABM, D_OPEN };

typedef struct {
	BTC	*c;
	PEER	*p;
	UB	st;			/* S_* */
	BOOL	initiator;
	BOOL	l2_ours;		/* the L2CAP channel is of this end's asking */
	UB	open_ch;		/* the earphones' channel this end opens, 0 none */
	UB	dlci;			/* the hands-free channel's, 0 none */
	UB	dst;			/* D_* */
	BOOL	cfc;			/* the flow by credits */
	INT	tx_cred, rx_cred;
	INT	n1;			/* the longest information a frame has */
	BOOL	fc_off;			/* the earphones said to stop */
	BOOL	msc_sent;
	UW	due;
	UB	txq[TXQ_MAX];		/* what waits for credits */
	INT	txn;
} RFC;

LOCAL RFC	rfc;
LOCAL UB	crc_tab[256];
LOCAL BOOL	crc_made = FALSE;

/* The check sum of 07.10: CRC-8 of x^8 + x^2 + x + 1, its bits taken from the lowest */
LOCAL void crc_make( void )
{
	INT	i, k;

	for ( i = 0; i < 256; i++ ) {
		UB	r = (UB)i;

		for ( k = 0; k < 8; k++ ) r = ( r & 1 ) ? (UB)( ( r >> 1 ) ^ 0xe0 ) : (UB)( r >> 1 );
		crc_tab[i] = r;
	}
	crc_made = TRUE;
}

LOCAL UB fcs_of( CONST UB *b, INT n )
{
	UB	f = 0xff;
	INT	i;

	if ( !crc_made ) crc_make();
	for ( i = 0; i < n; i++ ) f = crc_tab[f ^ b[i]];
	return (UB)( 0xff - f );
}

/* ---------------------------------------------------------------- frames out */

/* The C/R bit of a frame: a command from the initiator, an answer from the other end */
LOCAL UB addr_of( UB dlci, BOOL command )
{
	BOOL	cr = command ? rfc.initiator : !rfc.initiator;

	return (UB)( ( dlci << 2 ) | ( cr ? 2 : 0 ) | 1 );
}

LOCAL void frame( UB dlci, UB ctrl, BOOL command, INT credits, CONST UB *d, INT n )
{
	UB	b[6 + N1_MINE + 1];
	INT	k = 0, hdr;

	if ( n > N1_MINE || rfc.p == NULL || rfc.p->rcid_x[XI_RFCOMM] == 0 ) return;
	b[k++] = addr_of(dlci, command);
	b[k++] = ctrl;
	if ( n < 128 ) {
		b[k++] = (UB)( ( n << 1 ) | 1 );
	} else {
		b[k++] = (UB)( n << 1 );
		b[k++] = (UB)( n >> 7 );
	}
	hdr = k;
	if ( credits >= 0 ) b[k++] = (UB)credits;
	if ( n > 0 ) knl_memcpy(b + k, d, n);
	k += n;
	b[k] = fcs_of(b, ( ( ctrl & ~F_PF ) == F_UIH ) ? 2 : hdr);
	k++;
	knl_btx_l2_send(rfc.c, rfc.p, rfc.p->rcid_x[XI_RFCOMM], b, k);
}

/* A message on the multiplexer's channel */
LOCAL void mux_msg( UB type, BOOL command, CONST UB *d, INT n )
{
	UB	b[2 + 16];

	b[0] = (UB)( type | ( command ? M_CR : 0 ) | M_EA );
	b[1] = (UB)( ( n << 1 ) | 1 );
	if ( n > 0 ) knl_memcpy(b + 2, d, n);
	frame(0, F_UIH, TRUE, -1, b, n + 2);
}

LOCAL void send_pn( UB dlci, BOOL command, UB cl, UB prio, INT n1, UB k )
{
	UB	d[8];

	d[0] = (UB)( dlci & 0x3f );
	d[1] = cl;
	d[2] = (UB)( prio & 0x3f );
	d[3] = 0;
	d[4] = (UB)n1;
	d[5] = (UB)( n1 >> 8 );
	d[6] = 0;
	d[7] = (UB)( k & 7 );
	mux_msg(M_PN, command, d, 8);
}

LOCAL void send_msc( BOOL command )
{
	UB	d[2];

	d[0] = (UB)( ( rfc.dlci << 2 ) | 3 );
	d[1] = V24_SIGNALS;
	mux_msg(M_MSC, command, d, 2);
}

/* Credits given back once the earphones are short of them */
LOCAL void credits_look( void )
{
	if ( rfc.cfc && rfc.dst == D_OPEN && rfc.rx_cred < CREDITS_LOW ) {
		INT	give = CREDITS_GIVE - rfc.rx_cred;

		rfc.rx_cred += give;
		frame(rfc.dlci, F_UIH | F_PF, TRUE, give, NULL, 0);
	}
}

/* What waits goes, as the credits let it, a frame of n1 at most each */
LOCAL void tx_pump( void )
{
	while ( rfc.txn > 0 && rfc.dst == D_OPEN && !rfc.fc_off && ( !rfc.cfc || rfc.tx_cred > 0 ) ) {
		INT	k = ( rfc.txn > rfc.n1 ) ? rfc.n1 : rfc.txn;

		if ( !knl_btx_tx_room(rfc.c, 2) ) return;
		if ( rfc.cfc ) rfc.tx_cred--;
		frame(rfc.dlci, F_UIH, TRUE, -1, rfc.txq, k);
		rfc.txn -= k;
		if ( rfc.txn > 0 ) knl_memcpy(rfc.txq, rfc.txq + k, rfc.txn);
	}
}

EXPORT void knl_btf_send( BTC *c, CONST UB *d, INT n )
{
	(void)c;
	if ( rfc.dst != D_OPEN ) return;
	if ( rfc.txn + n > TXQ_MAX ) {
		tm_printf((UB *)"bt: RFCOMM: %d bytes lost, the earphones take no more\n", n);
		return;
	}
	knl_memcpy(rfc.txq + rfc.txn, d, n);
	rfc.txn += n;
	tx_pump();
}

/* ---------------------------------------------------------------- the session */

LOCAL void dlc_down( void )
{
	BOOL	was = (BOOL)( rfc.dst == D_OPEN );

	rfc.dlci = 0;
	rfc.dst = D_NONE;
	rfc.txn = 0;
	rfc.msc_sent = FALSE;
	if ( was ) knl_bth_dlc_down(rfc.c, rfc.p);
}

LOCAL void session_end( BOOL close_l2 )
{
	PEER	*p = rfc.p;

	dlc_down();
	rfc.st = S_NONE;
	rfc.due = 0;
	rfc.open_ch = 0;
	if ( close_l2 && p != NULL && p->rcid_x[XI_RFCOMM] != 0 ) knl_bta_x_close(rfc.c, p, CID_RFCOMM);
}

LOCAL void dlc_open( void )
{
	rfc.dst = D_OPEN;
	rfc.due = 0;
	if ( rfc.n1 <= 0 ) rfc.n1 = N1_DEFAULT;
	tm_printf((UB *)"bt: \"%s\": RFCOMM channel %d open, %s, frames of %d\n", rfc.p->name, rfc.dlci >> 1,
		  rfc.cfc ? "by credits" : "no credits", rfc.n1);
	send_msc(TRUE);
	rfc.msc_sent = TRUE;
	knl_bth_dlc_up(rfc.c, rfc.p);
}

/* The earphones open the session: taken unless there is one */
EXPORT BOOL knl_btf_accept( BTC *c, PEER *p )
{
	if ( rfc.st != S_NONE || p->rcid_x[XI_RFCOMM] != 0 ) return FALSE;
	knl_memset(&rfc, 0, sizeof(rfc));
	rfc.c = c;
	rfc.p = p;
	rfc.st = S_L2;
	rfc.initiator = FALSE;
	rfc.due = knl_btx_now() + RFC_TMO_MS;
	return TRUE;
}

/* This end opens the earphones' hands-free channel, and the session for it if there is none */
EXPORT void knl_btf_open( BTC *c, PEER *p, UB ch )
{
	if ( rfc.st != S_NONE ) {
		if ( rfc.p == p && rfc.st == S_MUX && rfc.dst == D_NONE && rfc.initiator ) {
			rfc.open_ch = ch;
			rfc.dlci = (UB)( ch << 1 );
			rfc.dst = D_PN;
			send_pn(rfc.dlci, TRUE, CL_CREDITS_ASK, 0, N1_MINE, CREDITS_GIVE);
		}
		return;
	}
	knl_memset(&rfc, 0, sizeof(rfc));
	rfc.c = c;
	rfc.p = p;
	rfc.st = S_L2;
	rfc.initiator = TRUE;
	rfc.l2_ours = TRUE;
	rfc.open_ch = ch;
	rfc.due = knl_btx_now() + RFC_TMO_MS;
	knl_bta_x_open(c, p, PSM_RFCOMM, CID_RFCOMM);
}

EXPORT void knl_btf_up( BTC *c, PEER *p )
{
	(void)c;
	if ( rfc.p != p || rfc.st != S_L2 ) return;
	if ( rfc.initiator ) {
		rfc.st = S_SABM0;
		frame(0, F_SABM | F_PF, TRUE, -1, NULL, 0);
	}
}

/* A message of the multiplexer's channel */
LOCAL void mux_in( CONST UB *d, INT n )
{
	UB	type, len;
	BOOL	command;
	CONST UB *v;

	if ( n < 2 || ( d[1] & 1 ) == 0 ) return;	/* a length past 127 is no message of these */
	type = (UB)( d[0] & 0xfc );
	command = (BOOL)( ( d[0] & M_CR ) != 0 );
	len = (UB)( d[1] >> 1 );
	v = d + 2;
	if ( 2 + len > n ) return;
	switch ( type ) {
	case M_PN:
		if ( len < 8 ) return;
		if ( command ) {
			UB	dlci = (UB)( v[0] & 0x3f );
			INT	n1 = v[4] | ( v[5] << 8 );
			BOOL	cfc = (BOOL)( ( v[1] & 0xf0 ) == CL_CREDITS_ASK );

			if ( n1 > N1_MINE || n1 <= 0 ) n1 = N1_MINE;
			if ( ( dlci >> 1 ) == RFC_CH_AG && rfc.dst == D_NONE ) {
				rfc.cfc = cfc;
				rfc.n1 = n1;
				rfc.tx_cred = v[7] & 7;
				rfc.rx_cred = cfc ? CREDITS_GIVE : 0;
			}
			send_pn(dlci, FALSE, cfc ? CL_CREDITS_YES : 0, v[2], n1, cfc ? CREDITS_GIVE : 0);
		} else if ( rfc.dst == D_PN && ( v[0] & 0x3f ) == rfc.dlci ) {
			rfc.cfc = (BOOL)( ( v[1] & 0xf0 ) == CL_CREDITS_YES );
			rfc.n1 = v[4] | ( v[5] << 8 );
			if ( rfc.n1 > N1_MINE || rfc.n1 <= 0 ) rfc.n1 = N1_DEFAULT;
			rfc.tx_cred = rfc.cfc ? ( v[7] & 7 ) : 0;
			rfc.rx_cred = rfc.cfc ? CREDITS_GIVE : 0;
			rfc.dst = D_SABM;
			frame(rfc.dlci, F_SABM | F_PF, TRUE, -1, NULL, 0);
		}
		break;
	case M_MSC:
		if ( command && len >= 2 ) {
			UB	r[2];

			r[0] = v[0];
			r[1] = v[1];
			if ( ( v[1] & 0x02 ) != 0 && !rfc.cfc ) rfc.fc_off = TRUE;	/* FC: it can take nothing now */
			else if ( !rfc.cfc ) rfc.fc_off = FALSE;
			mux_msg(M_MSC, FALSE, r, 2);
			tx_pump();
		}
		break;
	case M_RPN:
		if ( command ) {
			UB	r[8];

			if ( len >= 8 ) {
				knl_memcpy(r, v, 8);
			} else {
				/* asked what the line is: 9600 baud, eight bits, no parity, no flow control */
				r[0] = v[0];
				r[1] = 0x03;
				r[2] = 0x03;
				r[3] = 0x00;
				r[4] = 0x11;
				r[5] = 0x13;
				r[6] = 0xff;
				r[7] = 0x3f;
			}
			mux_msg(M_RPN, FALSE, r, 8);
		}
		break;
	case M_RLS:
		if ( command && len >= 2 ) mux_msg(M_RLS, FALSE, v, 2);
		break;
	case M_TEST:
		if ( command && len <= 16 ) mux_msg(M_TEST, FALSE, v, len);
		break;
	case M_FCON:
	case M_FCOFF:
		if ( command ) {
			rfc.fc_off = (BOOL)( type == M_FCOFF );
			mux_msg(type, FALSE, NULL, 0);
			tx_pump();
		}
		break;
	default:
		if ( command ) {
			UB	r = d[0];

			mux_msg(M_NSC, FALSE, &r, 1);
		}
		break;
	}
}

/* A frame of the session */
EXPORT void knl_btf_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	UB	dlci, ctrl, type;
	INT	len, hdr, credits = 0;
	CONST UB *info;

	(void)c;
	if ( rfc.p != p || rfc.st == S_NONE || n < 4 ) return;
	dlci = (UB)( f[0] >> 2 );
	ctrl = f[1];
	type = (UB)( ctrl & ~F_PF );
	if ( ( f[2] & 1 ) != 0 ) {
		len = f[2] >> 1;
		hdr = 3;
	} else {
		if ( n < 5 ) return;
		len = ( f[2] >> 1 ) | ( f[3] << 7 );
		hdr = 4;
	}
	if ( type == F_UIH && ( ctrl & F_PF ) != 0 && dlci != 0 && rfc.cfc ) {
		credits = f[hdr];
		hdr++;
	}
	if ( hdr + len + 1 != n ) return;
	if ( f[n - 1] != fcs_of(f, ( type == F_UIH ) ? 2 : ( ( f[2] & 1 ) ? 3 : 4 )) ) {
		tm_printf((UB *)"bt: RFCOMM: a frame with a bad check sum left\n");
		return;
	}
	info = f + hdr;
	switch ( type ) {
	case F_SABM:
		if ( dlci == 0 ) {
			if ( rfc.st == S_L2 && !rfc.initiator ) rfc.st = S_MUX;
			frame(0, F_UA | F_PF, FALSE, -1, NULL, 0);
		} else if ( rfc.st == S_MUX && ( dlci >> 1 ) == RFC_CH_AG && rfc.dst == D_NONE ) {
			rfc.dlci = dlci;
			frame(dlci, F_UA | F_PF, FALSE, -1, NULL, 0);
			if ( rfc.n1 == 0 ) rfc.n1 = N1_DEFAULT;
			dlc_open();
		} else {
			frame(dlci, F_DM | F_PF, FALSE, -1, NULL, 0);
		}
		break;
	case F_UA:
		if ( dlci == 0 && rfc.st == S_SABM0 ) {
			rfc.st = S_MUX;
			rfc.due = 0;
			if ( rfc.open_ch != 0 ) {
				rfc.dlci = (UB)( rfc.open_ch << 1 );
				rfc.dst = D_PN;
				rfc.due = knl_btx_now() + RFC_TMO_MS;
				send_pn(rfc.dlci, TRUE, CL_CREDITS_ASK, 0, N1_MINE, CREDITS_GIVE);
			}
		} else if ( dlci == 0 && rfc.st == S_DISC0 ) {
			session_end(TRUE);
		} else if ( dlci != 0 && dlci == rfc.dlci && rfc.dst == D_SABM ) {
			dlc_open();
		}
		break;
	case F_DM:
		if ( dlci == 0 ) {
			tm_printf((UB *)"bt: \"%s\": RFCOMM: the session refused\n", p->name);
			session_end(TRUE);
		} else if ( dlci == rfc.dlci ) {
			tm_printf((UB *)"bt: \"%s\": RFCOMM: channel %d refused or gone\n", p->name, dlci >> 1);
			dlc_down();
		}
		break;
	case F_DISC:
		frame(dlci, F_UA | F_PF, FALSE, -1, NULL, 0);
		if ( dlci == 0 ) session_end(FALSE);
		else if ( dlci == rfc.dlci ) dlc_down();
		break;
	case F_UIH:
		if ( dlci == 0 ) {
			mux_in(info, len);
		} else if ( dlci == rfc.dlci && rfc.dst == D_OPEN ) {
			rfc.tx_cred += credits;
			if ( len > 0 ) {
				if ( rfc.cfc && rfc.rx_cred > 0 ) rfc.rx_cred--;
				knl_bth_at_in(rfc.c, p, info, len);
			}
			credits_look();
			tx_pump();
		}
		break;
	default:
		break;
	}
}

EXPORT void knl_btf_closed( BTC *c, PEER *p )
{
	(void)c;
	if ( rfc.p != p || rfc.st == S_NONE ) return;
	session_end(FALSE);
}

EXPORT void knl_btf_down( BTC *c, PEER *p )
{
	knl_btf_closed(c, p);
	if ( rfc.p == p ) rfc.p = NULL;
}

/* Whether a session of this end's or the earphones' is there or being made */
/* The controller gone: the session with it */
EXPORT void knl_btf_gone( BTC *c )
{
	if ( rfc.c == c ) knl_memset(&rfc, 0, sizeof(rfc));
}

EXPORT BOOL knl_btf_busy( PEER *p )
{
	return (BOOL)( rfc.p == p && rfc.st != S_NONE );
}

EXPORT void knl_btf_tick( BTC *c )
{
	if ( rfc.c != c || rfc.p == NULL || rfc.st == S_NONE ) return;
	tx_pump();
	if ( rfc.due != 0 && (W)( knl_btx_now() - rfc.due ) >= 0 ) {
		tm_printf((UB *)"bt: \"%s\": RFCOMM did not answer\n", rfc.p->name);
		session_end(TRUE);
	}
}
