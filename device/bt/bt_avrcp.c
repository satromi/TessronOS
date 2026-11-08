/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_avrcp.c
 *	The remote control of earphones (design 6.8): AVRCP 1.4 on AVCTP,
 *	this end its target for the buttons and its controller for the
 *	volume.
 *
 *	The buttons (play, pause, stop, the next and the previous track, the
 *	volume) come as PASS THROUGH commands, pressed and let go. Each is a
 *	key of the consumer page through a slot of the input layer of its
 *	own, as a keyboard's media keys are: play and pause the Play/Pause
 *	key, the tracks Scan Next and Scan Previous, and so on, so that
 *	whatever takes those keys of a keyboard takes the earphones' too.
 *
 *	The volume: earphones that tell the change of their volume
 *	(GetCapabilities lists the event) are given the volume of the
 *	mixer's whole as SetAbsoluteVolume, and a change they tell sets the
 *	whole. The stream is then sent at full scale and the earphones turn
 *	it down, as they would for a telephone: the mixer does not turn it
 *	down a second time (knl_snd_whole_outside). An absolute volume is
 *	0 to 127, as linear as the mixer's 0 to 100.
 *
 *	What the earphones ask of this end as the target of a player is
 *	answered so that they go on: the play status, which changes with the
 *	stream, and that no track is known.
 *
 *	Earphones mostly open the channel themselves after the stream is set
 *	up; when they have not a while after, this end opens it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/bt.h>
#include <ts/snd.h>
#include "../usb/usbdev.h"
#include "../snd/sndhw.h"
#include "btint.h"
#include "btaudio.h"

IMPORT INT  knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );
IMPORT void knl_hid_ext_detach( INT hd );
IMPORT void knl_hid_ext_report( INT hd, CONST UB *r, INT len );

/* AVCTP: the label, the packet type, command or response, and the profile it is for */
#define AVCTP_RSP		0x02
#define AVCTP_IPID		0x01
#define PID_AVRCP		0x110e

/* AV/C */
#define CT_CONTROL		0x00
#define CT_STATUS		0x01
#define CT_NOTIFY		0x03
#define RS_NOT_IMPL		0x08
#define RS_ACCEPTED		0x09
#define RS_REJECTED		0x0a
#define RS_STABLE		0x0c
#define RS_CHANGED		0x0d
#define RS_INTERIM		0x0f
#define SUBUNIT_PANEL		( 0x09 << 3 )
#define SUBUNIT_UNIT		0xff
#define OP_VENDOR		0x00
#define OP_UNIT_INFO		0x30
#define OP_SUBUNIT_INFO		0x31
#define OP_PASS_THROUGH		0x7c

/* The company ID of the Bluetooth SIG, which AVRCP's own commands carry */
#define BT_SIG_0		0x00
#define BT_SIG_1		0x19
#define BT_SIG_2		0x58

/* AVRCP's PDUs */
#define PDU_GET_CAPS		0x10
#define PDU_INFORM_CHARSET	0x17
#define PDU_INFORM_BATTERY	0x18
#define PDU_ELEMENT_ATTRS	0x20
#define PDU_PLAY_STATUS		0x30
#define PDU_REGISTER		0x31
#define PDU_SET_ABS_VOL		0x50
#define CAP_COMPANY		0x02
#define CAP_EVENTS		0x03
#define EV_PLAYBACK		0x01
#define EV_TRACK		0x02
#define EV_VOLUME		0x0d
#define ERR_INVALID_CMD		0x00
#define ERR_INVALID_PARAM	0x01
#define PLAY_STOPPED		0x00
#define PLAY_PLAYING		0x01

/* The operations of PASS THROUGH, and the key of the consumer page each is */
LOCAL CONST struct { UB op; UH usage; } op_keys[] = {
	{ 0x44, 0x00b0 },		/* PLAY: Play */
	{ 0x46, 0x00b1 },		/* PAUSE: Pause */
	{ 0x45, 0x00b7 },		/* STOP: Stop */
	{ 0x4b, 0x00b5 },		/* FORWARD: Scan Next Track */
	{ 0x4c, 0x00b6 },		/* BACKWARD: Scan Previous Track */
	{ 0x41, 0x00e9 },		/* VOLUME UP */
	{ 0x42, 0x00ea },		/* VOLUME DOWN */
	{ 0x43, 0x00e2 },		/* MUTE */
	{ 0x49, 0x00b3 },		/* FAST FORWARD */
	{ 0x48, 0x00b4 }		/* REWIND */
};

/* A slot of consumer keys: one 16-bit usage at a time, no report ID */
LOCAL CONST UB cc_rd[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
	0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
	0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
	0xc0
};

#define AVR_TMO_MS		3000
#define AVR_OPEN_AFTER_MS	2000	/* the earphones' time to open the channel themselves */
#define VOL_ABS_MAX		127

typedef struct {
	BTC	*c;
	PEER	*p;
	BOOL	up;
	BOOL	opening;		/* this end asked for the channel */
	UW	open_at;		/* when to ask, 0 not */
	INT	hd;			/* the input layer's slot, 0 none */
	UH	held;			/* the key held down, 0 none */
	UB	tl;			/* this end's transaction label */
	UB	wait_pdu;		/* the answer waited for, 0 none */
	UW	due;
	BOOL	absvol;			/* the earphones take an absolute volume */
	BOOL	registered;		/* and will tell its change */
	BOOL	synced;			/* the mixer's whole given them once */
	INT	vol_sent;		/* what was sent last, -1 nothing */
	INT	vol_dev;		/* what they said last, -1 nothing */
	BOOL	playing;		/* the stream plays */
	INT	play_tl;		/* the label of their asking to be told the play status, -1 none */
} AVR;

LOCAL AVR	avr = { NULL, NULL, FALSE, FALSE, 0, 0, 0, 0, 0, 0, FALSE, FALSE, FALSE, -1, -1, FALSE, -1 };

LOCAL INT vol_abs( UW v )
{
	return (INT)( ( v * VOL_ABS_MAX + SND_VOL_MAX / 2 ) / SND_VOL_MAX );
}

LOCAL UW vol_mix( INT a )
{
	return (UW)( ( a * SND_VOL_MAX + VOL_ABS_MAX / 2 ) / VOL_ABS_MAX );
}

/* ---------------------------------------------------------------- sending */

LOCAL void avctp_send( UB tl, BOOL rsp, CONST UB *d, INT n )
{
	UB	b[3 + 64];

	if ( !avr.up || n > 64 ) return;
	b[0] = (UB)( ( tl << 4 ) | ( rsp ? AVCTP_RSP : 0 ) );
	b[1] = (UB)( PID_AVRCP >> 8 );
	b[2] = (UB)PID_AVRCP;
	knl_memcpy(b + 3, d, n);
	knl_btx_l2_send(avr.c, avr.p, avr.p->rcid_x[XI_AVCTP], b, n + 3);
}

/* A command or an answer of AVRCP's own: the head of VENDOR DEPENDENT, the PDU and its parameters */
LOCAL void vendor_send( UB tl, BOOL rsp, UB ctype, UB pdu, CONST UB *prm, INT n )
{
	UB	d[10 + 32];

	if ( n > 32 ) return;
	d[0] = ctype;
	d[1] = SUBUNIT_PANEL;
	d[2] = OP_VENDOR;
	d[3] = BT_SIG_0;
	d[4] = BT_SIG_1;
	d[5] = BT_SIG_2;
	d[6] = pdu;
	d[7] = 0;			/* a single packet */
	d[8] = (UB)( n >> 8 );
	d[9] = (UB)n;
	if ( n > 0 ) knl_memcpy(d + 10, prm, n);
	avctp_send(tl, rsp, d, n + 10);
}

LOCAL void vendor_cmd( UB ctype, UB pdu, CONST UB *prm, INT n )
{
	avr.tl = (UB)( ( avr.tl + 1 ) & 15 );
	avr.wait_pdu = pdu;
	avr.due = knl_btx_now() + AVR_TMO_MS;
	vendor_send(avr.tl, FALSE, ctype, pdu, prm, n);
}

LOCAL void vendor_reject( UB tl, UB pdu, UB why )
{
	vendor_send(tl, TRUE, RS_REJECTED, pdu, &why, 1);
}

LOCAL void send_volume( INT a )
{
	UB	v = (UB)( a & 0x7f );

	avr.vol_sent = a;
	vendor_cmd(CT_CONTROL, PDU_SET_ABS_VOL, &v, 1);
}

LOCAL void register_volume( void )
{
	UB	prm[5] = { EV_VOLUME, 0, 0, 0, 0 };

	vendor_cmd(CT_NOTIFY, PDU_REGISTER, prm, 5);
}

/* ---------------------------------------------------------------- the buttons */

LOCAL void key_report( UH usage )
{
	UB	r[2];

	r[0] = (UB)usage;
	r[1] = (UB)( usage >> 8 );
	knl_usb_lock();
	if ( avr.hd > 0 ) knl_hid_ext_report(avr.hd, r, 2);
	knl_usb_unlock();
}

/* A button pressed or let go: whether it is one this end takes */
LOCAL BOOL button( UB op, BOOL released )
{
	INT	i;

	for ( i = 0; i < (INT)( sizeof(op_keys) / sizeof(op_keys[0]) ); i++ ) {
		if ( op_keys[i].op != op ) continue;
		if ( !released ) {
			if ( avr.held != 0 && avr.held != op_keys[i].usage ) key_report(0);
			avr.held = op_keys[i].usage;
			key_report(avr.held);
		} else if ( avr.held == op_keys[i].usage ) {
			avr.held = 0;
			key_report(0);
		}
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- what the earphones ask */

LOCAL void vendor_in( UB tl, CONST UB *f, INT n )
{
	UB	pdu, r[16];
	CONST UB *prm;
	INT	plen;

	if ( n < 10 || f[3] != BT_SIG_0 || f[4] != BT_SIG_1 || f[5] != BT_SIG_2 ) {
		knl_memcpy(r, f, ( n > 10 ) ? 10 : n);
		r[0] = RS_NOT_IMPL;
		avctp_send(tl, TRUE, r, ( n > 10 ) ? 10 : n);
		return;
	}
	pdu = f[6];
	plen = ( f[8] << 8 ) | f[9];
	prm = f + 10;
	if ( 10 + plen > n ) {
		vendor_reject(tl, pdu, ERR_INVALID_PARAM);
		return;
	}
	switch ( pdu ) {
	case PDU_GET_CAPS:
		if ( plen >= 1 && prm[0] == CAP_COMPANY ) {
			r[0] = CAP_COMPANY;
			r[1] = 1;
			r[2] = BT_SIG_0;
			r[3] = BT_SIG_1;
			r[4] = BT_SIG_2;
			vendor_send(tl, TRUE, RS_STABLE, pdu, r, 5);
		} else if ( plen >= 1 && prm[0] == CAP_EVENTS ) {
			r[0] = CAP_EVENTS;
			r[1] = 2;
			r[2] = EV_PLAYBACK;
			r[3] = EV_TRACK;
			vendor_send(tl, TRUE, RS_STABLE, pdu, r, 4);
		} else {
			vendor_reject(tl, pdu, ERR_INVALID_PARAM);
		}
		break;
	case PDU_REGISTER:
		if ( plen >= 1 && prm[0] == EV_PLAYBACK ) {
			r[0] = EV_PLAYBACK;
			r[1] = avr.playing ? PLAY_PLAYING : PLAY_STOPPED;
			avr.play_tl = tl;
			vendor_send(tl, TRUE, RS_INTERIM, pdu, r, 2);
		} else if ( plen >= 1 && prm[0] == EV_TRACK ) {
			r[0] = EV_TRACK;
			knl_memset(r + 1, 0xff, 8);	/* no track chosen */
			vendor_send(tl, TRUE, RS_INTERIM, pdu, r, 9);
		} else {
			vendor_reject(tl, pdu, ERR_INVALID_PARAM);
		}
		break;
	case PDU_PLAY_STATUS:
		knl_memset(r, 0xff, 8);		/* the length and the place of a song not known */
		r[8] = avr.playing ? PLAY_PLAYING : PLAY_STOPPED;
		vendor_send(tl, TRUE, RS_STABLE, pdu, r, 9);
		break;
	case PDU_ELEMENT_ATTRS:
		r[0] = 0;			/* no attributes */
		vendor_send(tl, TRUE, RS_STABLE, pdu, r, 1);
		break;
	case PDU_INFORM_CHARSET:
	case PDU_INFORM_BATTERY:
		vendor_send(tl, TRUE, RS_ACCEPTED, pdu, NULL, 0);
		break;
	default:
		vendor_reject(tl, pdu, ERR_INVALID_CMD);
		break;
	}
}

LOCAL void command_in( UB tl, CONST UB *f, INT n )
{
	UB	r[8];

	if ( n < 3 ) return;
	switch ( f[2] ) {
	case OP_PASS_THROUGH:
		if ( n < 5 ) return;
		knl_memcpy(r, f, 5);
		r[0] = button((UB)( f[3] & 0x7f ), (BOOL)( ( f[3] & 0x80 ) != 0 )) ? RS_ACCEPTED : RS_NOT_IMPL;
		avctp_send(tl, TRUE, r, 5);
		break;
	case OP_UNIT_INFO:
		r[0] = RS_STABLE;
		r[1] = SUBUNIT_UNIT;
		r[2] = OP_UNIT_INFO;
		r[3] = 0x07;
		r[4] = SUBUNIT_PANEL;
		r[5] = BT_SIG_0;
		r[6] = BT_SIG_1;
		r[7] = BT_SIG_2;
		avctp_send(tl, TRUE, r, 8);
		break;
	case OP_SUBUNIT_INFO:
		r[0] = RS_STABLE;
		r[1] = SUBUNIT_UNIT;
		r[2] = OP_SUBUNIT_INFO;
		r[3] = 0x07;
		r[4] = SUBUNIT_PANEL;
		r[5] = r[6] = r[7] = 0xff;
		avctp_send(tl, TRUE, r, 8);
		break;
	case OP_VENDOR:
		vendor_in(tl, f, n);
		break;
	default:
		knl_memcpy(r, f, ( n > 8 ) ? 8 : n);
		r[0] = RS_NOT_IMPL;
		avctp_send(tl, TRUE, r, ( n > 8 ) ? 8 : n);
		break;
	}
}

/* ---------------------------------------------------------------- what the earphones answer */

LOCAL void answer_in( UB tl, CONST UB *f, INT n )
{
	UB	pdu, rs = (UB)( f[0] & 0x0f );
	CONST UB *prm;
	INT	plen, i;

	if ( n < 10 || f[2] != OP_VENDOR ) return;
	pdu = f[6];
	plen = ( f[8] << 8 ) | f[9];
	prm = f + 10;
	if ( 10 + plen > n ) return;
	if ( pdu == PDU_REGISTER && rs == RS_CHANGED && plen >= 2 && prm[0] == EV_VOLUME ) {
		/* a change of their own, or the echo of what this end set: told once, asked again */
		INT	v = prm[1] & 0x7f;

		avr.vol_dev = v;
		avr.registered = FALSE;		/* asked again at the next round */
		if ( v != avr.vol_sent ) {
			SDMixSet	s;
			SDMix		m;

			(void)knl_snd_mix_get(&m, 0);
			knl_memset(&s, 0, sizeof(s));
			s.what = SND_MIX_ALL;
			s.lv = s.rv = vol_mix(v);
			s.mute = m.mute;
			(void)knl_snd_mix_set(&s, TRUE);
			avr.vol_sent = vol_abs(s.lv);
			tm_printf((UB *)"bt: \"%s\": its volume %d, the mixer's whole %d\n", avr.p->name, v, s.lv);
		}
		return;
	}
	if ( tl != avr.tl || pdu != avr.wait_pdu ) return;
	avr.wait_pdu = 0;
	avr.due = 0;
	switch ( pdu ) {
	case PDU_GET_CAPS:
		avr.absvol = FALSE;
		if ( rs == RS_STABLE && plen >= 2 && prm[0] == CAP_EVENTS ) {
			for ( i = 0; i < prm[1] && 2 + i < plen; i++ ) {
				if ( prm[2 + i] == EV_VOLUME ) avr.absvol = TRUE;
			}
		}
		tm_printf((UB *)"bt: \"%s\": %s\n", avr.p->name,
			  avr.absvol ? "takes an absolute volume" : "keeps its own volume");
		if ( avr.absvol ) register_volume();
		break;
	case PDU_REGISTER:
		if ( rs == RS_INTERIM && plen >= 2 && prm[0] == EV_VOLUME ) {
			avr.registered = TRUE;
			avr.vol_dev = prm[1] & 0x7f;
			if ( !avr.synced ) avr.vol_sent = -1;	/* the mixer's whole to them at the next round */
			avr.synced = TRUE;
		} else {
			avr.absvol = FALSE;
		}
		break;
	case PDU_SET_ABS_VOL:
		if ( rs == RS_ACCEPTED && plen >= 1 ) avr.vol_dev = prm[0] & 0x7f;
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the channel */

/* The earphones open the channel: taken unless this end has one or is asking for it */
EXPORT BOOL knl_btr_accept( BTC *c, PEER *p )
{
	(void)c;
	if ( avr.opening && avr.p == p ) return FALSE;
	if ( avr.up || p->rcid_x[XI_AVCTP] != 0 ) return FALSE;
	avr.c = c;
	avr.p = p;
	avr.open_at = 0;
	return TRUE;
}

/* The stream is set up: the channel is opened a while after, unless the earphones open it first */
EXPORT void knl_btr_want( BTC *c, PEER *p )
{
	if ( avr.up || avr.opening ) return;
	avr.c = c;
	avr.p = p;
	avr.open_at = ( knl_btx_now() + AVR_OPEN_AFTER_MS ) | 1;
}

EXPORT void knl_btr_up( BTC *c, PEER *p )
{
	avr.c = c;
	avr.p = p;
	avr.up = TRUE;
	avr.opening = FALSE;
	avr.open_at = 0;
	avr.absvol = avr.registered = avr.synced = FALSE;
	avr.vol_sent = avr.vol_dev = -1;
	avr.wait_pdu = 0;
	avr.held = 0;
	avr.play_tl = -1;
	knl_usb_lock();
	if ( avr.hd <= 0 ) avr.hd = knl_hid_ext_attach(FALSE, cc_rd, sizeof(cc_rd), (CONST UB *)"btavrcp");
	knl_usb_unlock();
	tm_printf((UB *)"bt: \"%s\": the remote control is up, input slot %d\n", p->name, avr.hd);
	{
		UB	prm = CAP_EVENTS;

		vendor_cmd(CT_STATUS, PDU_GET_CAPS, &prm, 1);
	}
}

EXPORT void knl_btr_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	UB	tl;

	(void)c;
	if ( !avr.up || avr.p != p || n < 3 ) return;
	tl = (UB)( f[0] >> 4 );
	if ( ( f[0] & 0x0c ) != 0 ) return;		/* a packet of several: none of AVRCP's here is that long */
	if ( ( ( f[1] << 8 ) | f[2] ) != PID_AVRCP ) {
		UB	r[3];

		if ( ( f[0] & AVCTP_RSP ) != 0 ) return;
		r[0] = (UB)( ( f[0] & 0xf0 ) | AVCTP_RSP | AVCTP_IPID );
		r[1] = f[1];
		r[2] = f[2];
		knl_btx_l2_send(avr.c, p, p->rcid_x[XI_AVCTP], r, 3);
		return;
	}
	if ( ( f[0] & AVCTP_RSP ) != 0 ) answer_in(tl, f + 3, n - 3);
	else command_in(tl, f + 3, n - 3);
}

LOCAL void avr_end( void )
{
	knl_usb_lock();
	if ( avr.hd > 0 ) {
		if ( avr.held != 0 ) {
			UB	r[2] = { 0, 0 };

			knl_hid_ext_report(avr.hd, r, 2);
		}
		knl_hid_ext_detach(avr.hd);
	}
	avr.hd = 0;
	knl_usb_unlock();
	avr.up = avr.opening = FALSE;
	avr.open_at = 0;
	avr.held = 0;
	avr.absvol = avr.registered = FALSE;
	avr.wait_pdu = 0;
	avr.due = 0;
}

EXPORT void knl_btr_closed( BTC *c, PEER *p )
{
	(void)c;
	if ( avr.p != p ) return;
	if ( avr.up ) tm_printf((UB *)"bt: \"%s\": the remote control closed\n", p->name);
	avr_end();
}

EXPORT void knl_btr_down( BTC *c, PEER *p )
{
	knl_btr_closed(c, p);
	if ( avr.p == p ) avr.p = NULL;
}

/* The controller gone, with the manager's lock held: no disconnection comes */
EXPORT void knl_btr_gone( BTC *c )
{
	if ( avr.c != c ) return;
	if ( avr.hd > 0 ) knl_hid_ext_detach(avr.hd);
	avr.hd = 0;
	avr.up = avr.opening = FALSE;
	avr.open_at = 0;
	avr.held = 0;
	avr.absvol = avr.registered = FALSE;
	avr.wait_pdu = 0;
	avr.p = NULL;
	avr.c = NULL;
}

EXPORT BOOL knl_btr_absvol( void )
{
	return (BOOL)( avr.up && avr.absvol && avr.registered );
}

/* The stream plays or stops: earphones that asked are told */
EXPORT void knl_btr_playing( BOOL on )
{
	if ( avr.playing == on ) return;
	avr.playing = on;
	if ( avr.up && avr.play_tl >= 0 ) {
		UB	r[2];

		r[0] = EV_PLAYBACK;
		r[1] = on ? PLAY_PLAYING : PLAY_STOPPED;
		vendor_send((UB)avr.play_tl, TRUE, RS_CHANGED, PDU_REGISTER, r, 2);
		avr.play_tl = -1;
	}
}

EXPORT void knl_btr_tick( BTC *c )
{
	if ( avr.c != c || avr.p == NULL ) return;
	if ( avr.open_at != 0 && (W)( knl_btx_now() - avr.open_at ) >= 0 ) {
		avr.open_at = 0;
		if ( !avr.up && avr.p->up && avr.p->rcid_x[XI_AVCTP] == 0 ) {
			avr.opening = TRUE;
			knl_bta_x_open(c, avr.p, PSM_AVCTP, CID_AVCTP);
		}
	}
	if ( !avr.up ) return;
	if ( avr.wait_pdu != 0 && (W)( knl_btx_now() - avr.due ) >= 0 ) {
		if ( avr.wait_pdu == PDU_GET_CAPS || avr.wait_pdu == PDU_REGISTER ) avr.absvol = FALSE;
		avr.wait_pdu = 0;
	}
	if ( avr.wait_pdu == 0 && avr.absvol && !avr.registered && avr.synced ) register_volume();
	/* the mixer's whole to the earphones as it changes */
	if ( avr.wait_pdu == 0 && avr.absvol && avr.registered ) {
		SDMix	m;
		INT	a;

		(void)knl_snd_mix_get(&m, 0);
		a = vol_abs(m.lv);
		if ( a != avr.vol_sent ) send_volume(a);
	}
}
