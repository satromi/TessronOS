/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_a2dp.c
 *	Sound to earphones (design 6.8): this end the source of A2DP, the
 *	earphones a sink it finds with AVDTP, and an output of the sound
 *	device like any other (sndhw.h). And the channels of L2CAP the rest
 *	of the audio side rides on (btaudio.h): this end's service records,
 *	the remote control, RFCOMM under the hands-free profile.
 *
 *	A device whose Class of Device says audio, once its link is encrypted,
 *	is asked for its sink service (SDP), then for its stream ends and what
 *	they can do. One that can take SBC is set up and its media channel
 *	opened, and from then on it is an output; the stream starts when the
 *	sound device plays on it, and is suspended when it stops. Only one
 *	device plays at a time, and only one is set up: the first to get there.
 *
 *	Earphones that connect by themselves set the stream up themselves:
 *	they open the signalling channel, find this end's stream end (one
 *	source, SEID 1), ask what it can do, configure it and open it, and
 *	open the media channel. This end answers each of those as the
 *	acceptor, and does not set up a stream of its own while they do: when
 *	the signalling channel is theirs, it waits AV_WAIT_MS for their first
 *	command before it starts its own. A stream they start, with a button,
 *	is begun at the pace of the clock as one this end starts, and is held
 *	a while for the sound device to play on it before it is suspended.
 *
 *	The sound device calls this backend with the USB manager's lock held,
 *	on its own task, and that lock must not be waited on with the
 *	controller's task's work in hand. So the two sides meet in a ring of
 *	encoded frames, one writer and one reader, and in flags: poll encodes
 *	as time passes, the controller's task takes whole packets of them to
 *	the air and does all of the signalling.
 *
 *	The frames are made at the pace of the clock here. A sink does not say
 *	how fast it plays, and holds a little buffer of its own: if the clocks
 *	differ by a little, it deals with that, as with any source. A packet
 *	holds as many frames as both the device's MTU and one frame of this
 *	end (TX_MAX) take.
 *
 *	While the hands-free profile has a voice link up, the earphones play
 *	that and not the stream: the stream is suspended, and the output says
 *	it cannot sound, so that the sound device plays on the voice instead.
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
#include "sbc_enc.h"

#define PSM_AVDTP		0x0019

#define AV_DISCOVER		0x01
#define AV_GET_CAPS		0x02
#define AV_SET_CONFIG		0x03
#define AV_GET_CONFIG		0x04
#define AV_RECONFIG		0x05
#define AV_OPEN			0x06
#define AV_START		0x07
#define AV_CLOSE		0x08
#define AV_SUSPEND		0x09
#define AV_ABORT		0x0a
#define AV_SECURITY		0x0b
#define AV_GET_ALL_CAPS		0x0c
#define AV_DELAY_REPORT		0x0d

#define AVM_CMD			0
#define AVM_GEN_REJECT		1
#define AVM_ACCEPT		2
#define AVM_REJECT		3

#define AV_ERR_BAD_ACP_SEID	0x12
#define AV_ERR_SEP_IN_USE	0x13
#define AV_ERR_SEP_NOT_IN_USE	0x14
#define AV_ERR_NOT_SUPPORTED	0x19
#define AV_ERR_BAD_CONFIG	0x29
#define AV_ERR_BAD_STATE	0x31

#define AV_SEP_AUDIO		0
#define AV_SEP_SINK		1
#define AV_CAP_TRANSPORT	1
#define AV_CAP_CODEC		7
#define AV_CODEC_SBC		0

#define AV_SEID_OURS		1

/* What the codec's capabilities say, in their four bytes */
#define SBC_R44			0x20
#define SBC_R48			0x10
#define SBC_STEREO		0x02
#define SBC_JOINT		0x01
#define SBC_BLOCKS16		0x10
#define SBC_SUB8		0x04
#define SBC_LOUDNESS		0x01

#define BITPOOL_MIN		2
#define BITPOOL_WANT		53	/* the high quality the specification suggests at 44.1 kHz */

#define NSEP			8

#define AV_TMO_MS		10000
#define AV_WAIT_MS		2000	/* the time earphones that opened the channel have to start setting up */
#define AV_HOLD_MS		3000	/* a stream they started, kept this long for the sound device */

enum {
	V_NONE, V_DISCOVER, V_CAPS, V_CONFIG, V_CONF_ACP, V_OPEN, V_MEDIA, V_READY, V_STARTING, V_STREAM, V_SUSPENDING
};

#define RTP_HEAD		13	/* the RTP header and the byte that counts the frames */
#define RTP_PT			96
#define FRAME_SLOT		256
#define NSLOT			64	/* some 180 ms */
#define POLL_FRAMES		8	/* the most encoded in one look, not to hold the manager's lock long */
#define PER_PACKET_MAX		15	/* what the frame count's four bits hold */

typedef struct {
	BTC		*c;
	PEER		*p;
	UB		st;
	UB		tid;
	UB		sig;		/* the command that is out */
	UW		due;
	UW		wait_until;	/* the earphones' turn to set up, 0 not */
	UB		sep[NSEP];	/* the device's sinks */
	INT		nsep, cur_sep;
	UB		seid;		/* the device's stream end, set up with this end's */
	UB		caps[4];
	BOOL		rate48, joint;
	UB		bitpool;
	/* the stream */
	volatile BOOL	want_play;
	BOOL		played;		/* the sound device has played on it since it started */
	UW		started_at;
	SBC_ENC		enc;
	INT		flen, per_packet;
	UW		start_ms;
	UD		made;		/* frames made since it started */
	UH		seq;
	UW		ts;
	volatile UW	head, tail;	/* the ring: the reader moves head, the writer tail */
	UW		lost;		/* frames there was no room for */
	UW		packets;
	UB		ring[NSLOT][FRAME_SLOT];
	UB		out[TX_MAX];
} AV;

LOCAL AV	*av = NULL;		/* the one device set up, with its stream */
LOCAL AV	*av_room = NULL;
LOCAL BOOL	play_voice = FALSE;	/* the sound device plays on the hands-free output */

LOCAL void av_sig( AV *a, UB sig, CONST UB *d, INT n )
{
	UB	b[16];

	a->tid = (UB)( ( a->tid + 1 ) & 15 );
	a->sig = sig;
	a->due = knl_btx_now() + AV_TMO_MS;
	b[0] = (UB)( ( a->tid << 4 ) | AVM_CMD );
	b[1] = sig;
	if ( n > 0 ) knl_memcpy(b + 2, d, n);
	knl_btx_l2_send(a->c, a->p, a->p->rcid_av, b, n + 2);
}

LOCAL void av_seid( AV *a, UB sig, UB st )
{
	UB	s = (UB)( a->seid << 2 );

	a->st = st;
	av_sig(a, sig, &s, 1);
}

LOCAL void av_give_up( AV *a, CONST char *why )
{
	tm_printf((UB *)"bt: \"%s\": no sound: %s\n", a->p->name, why);
	a->st = V_NONE;
	a->sig = 0;
	a->due = 0;
}

/* The ring and the counts from nothing, the clock from now: with the manager's lock */
LOCAL void stream_begin( AV *a )
{
	a->head = a->tail = 0;
	a->made = 0;
	a->start_ms = knl_usb_ms();
	a->packets = a->lost = 0;
	a->ts = 0;
	a->st = V_STREAM;
}

/* ---------------------------------------------------------------- the acceptor */

/* This end's capabilities: the transport, and SBC as the encoder makes it */
LOCAL INT caps_ours( UB *d )
{
	d[0] = AV_CAP_TRANSPORT;
	d[1] = 0;
	d[2] = AV_CAP_CODEC;
	d[3] = 6;
	d[4] = AV_SEP_AUDIO << 4;
	d[5] = AV_CODEC_SBC;
	d[6] = SBC_R44 | SBC_R48 | SBC_STEREO | SBC_JOINT;
	d[7] = SBC_BLOCKS16 | SBC_SUB8 | SBC_LOUDNESS;
	d[8] = BITPOOL_MIN;
	d[9] = BITPOOL_WANT;
	return 10;
}

/* The configuration the earphones give this end's stream end: FALSE for one it cannot make */
LOCAL BOOL config_take( AV *a, CONST UB *f, INT n )
{
	INT	at = 0;
	BOOL	codec = FALSE;

	while ( at + 2 <= n && at + 2 + f[at + 1] <= n ) {
		INT		l = f[at + 1];
		CONST UB	*v = f + at + 2;

		if ( f[at] == AV_CAP_CODEC ) {
			UB	r = (UB)( v[2] & 0xf0 ), m = (UB)( v[2] & 0x0f );

			if ( l < 6 || ( v[0] >> 4 ) != AV_SEP_AUDIO || v[1] != AV_CODEC_SBC ) return FALSE;
			if ( ( r != SBC_R44 && r != SBC_R48 ) || ( m != SBC_STEREO && m != SBC_JOINT )
			  || v[3] != ( SBC_BLOCKS16 | SBC_SUB8 | SBC_LOUDNESS )
			  || v[5] < BITPOOL_MIN || v[5] > BITPOOL_WANT || v[4] > v[5] ) return FALSE;
			a->rate48 = (BOOL)( r == SBC_R48 );
			a->joint = (BOOL)( m == SBC_JOINT );
			a->bitpool = v[5];
			knl_memcpy(a->caps, v + 2, 4);
			codec = TRUE;
		} else if ( f[at] != AV_CAP_TRANSPORT ) {
			return FALSE;		/* recovery, protection and the rest: none here */
		}
		at += 2 + l;
	}
	return codec;
}

LOCAL void av_answer( AV *a, CONST UB *f, UB type, CONST UB *d, INT n )
{
	UB	b[16];

	b[0] = (UB)( ( f[0] & 0xf0 ) | type );
	b[1] = (UB)( f[1] & 0x3f );
	if ( n > 0 ) knl_memcpy(b + 2, d, n);
	knl_btx_l2_send(a->c, a->p, a->p->rcid_av, b, n + 2);
}

LOCAL void av_refuse( AV *a, CONST UB *f, CONST UB *d, INT n )
{
	av_answer(a, f, AVM_REJECT, d, n);
	tm_printf((UB *)"bt: \"%s\": asks %02x of this end, refused (%02x)\n", a->p->name, f[1] & 0x3f, d[n - 1]);
}

/* A command of the device's: what this end does, and no more */
LOCAL void av_serve( AV *a, CONST UB *f, INT n )
{
	UB	d[12];
	UB	sig = (UB)( f[1] & 0x3f );
	UB	acp = ( n > 2 ) ? (UB)( f[2] >> 2 ) : 0;
	BOOL	set = (BOOL)( a->st >= V_CONF_ACP && a->st != V_CONFIG );

	a->wait_until = 0;			/* the earphones are setting up: this end does not */
	switch ( sig ) {
	case AV_DISCOVER:
		d[0] = (UB)( ( AV_SEID_OURS << 2 ) | ( set ? 2 : 0 ) );
		d[1] = (UB)( ( AV_SEP_AUDIO << 4 ) | ( 0 << 3 ) );	/* a source */
		av_answer(a, f, AVM_ACCEPT, d, 2);
		break;
	case AV_GET_CAPS:
	case AV_GET_ALL_CAPS:
		if ( acp != AV_SEID_OURS ) {
			d[0] = AV_ERR_BAD_ACP_SEID;
			av_refuse(a, f, d, 1);
			break;
		}
		av_answer(a, f, AVM_ACCEPT, d, caps_ours(d));
		break;
	case AV_SET_CONFIG:
		d[0] = 0;
		if ( n < 4 || acp != AV_SEID_OURS ) {
			d[1] = AV_ERR_BAD_ACP_SEID;
		} else if ( set || a->st == V_CONFIG || a->st == V_OPEN ) {
			d[1] = AV_ERR_SEP_IN_USE;
		} else if ( !config_take(a, f + 4, n - 4) ) {
			d[0] = AV_CAP_CODEC;
			d[1] = AV_ERR_BAD_CONFIG;
		} else {
			/* taken: what this end had begun of its own is left */
			a->seid = (UB)( f[3] >> 2 );
			a->st = V_CONF_ACP;
			a->sig = 0;
			a->due = 0;
			av_answer(a, f, AVM_ACCEPT, NULL, 0);
			tm_printf((UB *)"bt: \"%s\" sets up the stream: SBC %s at %d Hz, bit pool %d\n", a->p->name,
				  a->joint ? "joint stereo" : "stereo", a->rate48 ? 48000 : 44100, a->bitpool);
			break;
		}
		av_refuse(a, f, d, 2);
		break;
	case AV_GET_CONFIG:
		if ( !set ) {
			d[0] = AV_ERR_SEP_NOT_IN_USE;
			av_refuse(a, f, d, 1);
			break;
		}
		(void)caps_ours(d);
		knl_memcpy(d + 6, a->caps, 4);
		av_answer(a, f, AVM_ACCEPT, d, 10);
		break;
	case AV_RECONFIG:
		d[0] = 0;
		d[1] = AV_ERR_NOT_SUPPORTED;
		av_refuse(a, f, d, 2);
		break;
	case AV_OPEN:
		if ( a->st != V_CONF_ACP ) {
			d[0] = AV_ERR_BAD_STATE;
			av_refuse(a, f, d, 1);
			break;
		}
		a->st = V_MEDIA;		/* the earphones open the media channel next */
		av_answer(a, f, AVM_ACCEPT, NULL, 0);
		break;
	case AV_START:
		if ( a->st == V_READY || a->st == V_STARTING ) {
			/* the sink starts it, a button on the earphones; or both at once */
			knl_usb_lock();
			stream_begin(a);
			a->played = a->want_play;
			a->started_at = knl_btx_now();
			knl_usb_unlock();
			a->sig = 0;
			a->due = 0;
			knl_btr_playing(TRUE);
			tm_printf((UB *)"bt: \"%s\" started the stream\n", a->p->name);
		}
		if ( a->st == V_STREAM ) {
			av_answer(a, f, AVM_ACCEPT, NULL, 0);
		} else {
			d[0] = acp;
			d[1] = AV_ERR_BAD_STATE;
			av_refuse(a, f, d, 2);
		}
		break;
	case AV_SUSPEND:
		if ( a->st == V_STREAM || a->st == V_SUSPENDING ) {
			a->st = V_READY;
			a->sig = 0;
			a->due = 0;
			knl_btr_playing(FALSE);
		}
		av_answer(a, f, AVM_ACCEPT, NULL, 0);
		break;
	case AV_CLOSE:
	case AV_ABORT:
		a->st = V_NONE;
		a->sig = 0;
		a->due = 0;
		knl_btr_playing(FALSE);
		av_answer(a, f, AVM_ACCEPT, NULL, 0);
		break;
	case AV_DELAY_REPORT:
		av_answer(a, f, AVM_ACCEPT, NULL, 0);
		break;
	case AV_SECURITY:
		d[0] = AV_ERR_NOT_SUPPORTED;
		av_refuse(a, f, d, 1);
		break;
	default:
		av_answer(a, f, AVM_GEN_REJECT, NULL, 0);
		tm_printf((UB *)"bt: \"%s\": asks %02x of this end, not known\n", a->p->name, sig);
		break;
	}
}

/* ---------------------------------------------------------------- the initiator */

LOCAL void ask_caps( AV *a )
{
	UB	s;

	if ( a->cur_sep >= a->nsep ) {
		av_give_up(a, "no stream end takes SBC as this end makes it");
		return;
	}
	s = (UB)( a->sep[a->cur_sep++] << 2 );
	a->st = V_CAPS;
	a->seid = (UB)( s >> 2 );
	av_sig(a, AV_GET_CAPS, &s, 1);
}

/* What a sink can do, and the setting for it: FALSE when it cannot take what is made here */
LOCAL BOOL caps_look( AV *a, CONST UB *f, INT n )
{
	INT	at = 0;

	while ( at + 2 <= n && at + 2 + f[at + 1] <= n ) {
		INT		l = f[at + 1];
		CONST UB	*v = f + at + 2;

		if ( f[at] == AV_CAP_CODEC && l >= 6 && ( v[0] >> 4 ) == AV_SEP_AUDIO && v[1] == AV_CODEC_SBC ) {
			INT	pool;

			if ( ( v[2] & ( SBC_R44 | SBC_R48 ) ) == 0 || ( v[2] & ( SBC_STEREO | SBC_JOINT ) ) == 0
			  || ( v[3] & SBC_BLOCKS16 ) == 0 || ( v[3] & SBC_SUB8 ) == 0 || ( v[3] & SBC_LOUDNESS ) == 0 ) {
				return FALSE;
			}
			a->rate48 = (BOOL)( ( v[2] & SBC_R44 ) == 0 );
			a->joint = (BOOL)( ( v[2] & SBC_JOINT ) != 0 );
			pool = ( v[5] < BITPOOL_WANT ) ? v[5] : BITPOOL_WANT;
			if ( pool < v[4] ) pool = v[4];
			a->bitpool = (UB)pool;
			a->caps[0] = (UB)( ( a->rate48 ? SBC_R48 : SBC_R44 ) | ( a->joint ? SBC_JOINT : SBC_STEREO ) );
			a->caps[1] = SBC_BLOCKS16 | SBC_SUB8 | SBC_LOUDNESS;
			a->caps[2] = v[4];
			a->caps[3] = (UB)pool;
			return TRUE;
		}
		at += 2 + l;
	}
	return FALSE;
}

LOCAL void set_config( AV *a )
{
	UB	d[12];

	d[0] = (UB)( a->seid << 2 );
	d[1] = AV_SEID_OURS << 2;
	d[2] = AV_CAP_TRANSPORT;
	d[3] = 0;
	d[4] = AV_CAP_CODEC;
	d[5] = 6;
	d[6] = AV_SEP_AUDIO << 4;
	d[7] = AV_CODEC_SBC;
	knl_memcpy(d + 8, a->caps, 4);
	a->st = V_CONFIG;
	av_sig(a, AV_SET_CONFIG, d, 12);
}

LOCAL void discover( AV *a )
{
	a->st = V_DISCOVER;
	av_sig(a, AV_DISCOVER, NULL, 0);
}

/* The one device of the stream: its room, kept for those after it */
LOCAL AV *av_take( BTC *c, PEER *p )
{
	AV	*a;

	if ( av != NULL ) return ( av->p == p ) ? av : NULL;
	if ( av_room == NULL ) av_room = (AV *)Kcalloc(1, sizeof(AV));
	if ( av_room == NULL ) return NULL;
	a = av_room;
	a->c = c;
	a->p = p;
	a->st = V_NONE;
	a->sig = 0;
	a->due = 0;
	a->wait_until = 0;
	a->want_play = FALSE;
	av = a;
	return a;
}

/* What the device says on the signalling channel */
EXPORT void knl_bta_sig_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	AV	*a;
	INT	type, i;

	if ( n < 2 || ( f[0] & 0x0c ) != 0 ) return;	/* a message in pieces: none of these is that long */
	type = f[0] & 3;
	a = av_take(c, p);
	if ( a == NULL ) return;
	if ( type == AVM_CMD ) {
		av_serve(a, f, n);
		return;
	}
	if ( a->sig == 0 || ( f[1] & 0x3f ) != a->sig || ( f[0] >> 4 ) != a->tid ) return;
	a->due = 0;
	a->sig = 0;
	if ( type != AVM_ACCEPT ) {
		if ( a->st == V_CAPS ) {
			ask_caps(a);		/* another stream end, perhaps */
		} else if ( a->st == V_STARTING ) {
			tm_printf((UB *)"bt: \"%s\": refused to start, %02x\n", p->name, ( n > 2 ) ? f[n - 1] : 0);
			a->st = V_READY;
			a->want_play = FALSE;
		} else if ( a->st == V_SUSPENDING ) {
			a->st = V_READY;
		} else {
			tm_printf((UB *)"bt: \"%s\": refused %02x, %02x\n", p->name, f[1] & 0x3f, ( n > 2 ) ? f[n - 1] : 0);
			av_give_up(a, "its stream was not set up");
		}
		return;
	}
	switch ( a->st ) {
	case V_DISCOVER:
		a->nsep = 0;
		for ( i = 2; i + 2 <= n && a->nsep < NSEP; i += 2 ) {
			BOOL	in_use = (BOOL)( ( f[i] & 2 ) != 0 );

			if ( !in_use && ( f[i + 1] >> 4 ) == AV_SEP_AUDIO && ( ( f[i + 1] >> 3 ) & 1 ) == AV_SEP_SINK ) {
				a->sep[a->nsep++] = (UB)( f[i] >> 2 );
			}
		}
		a->cur_sep = 0;
		ask_caps(a);
		break;
	case V_CAPS:
		if ( caps_look(a, f + 2, n - 2) ) {
			set_config(a);
		} else {
			ask_caps(a);
		}
		break;
	case V_CONFIG:
		av_seid(a, AV_OPEN, V_OPEN);
		break;
	case V_OPEN:
		a->st = V_MEDIA;
		if ( p->rcid_media == 0 ) knl_btx_l2_open(c, p, PSM_AVDTP, CID_AV_MEDIA);
		else knl_bta_chan_up(c, p, (UB)( p->cfg_in & p->cfg_out ));
		break;
	case V_STARTING:
		knl_usb_lock();
		stream_begin(a);
		a->played = TRUE;
		knl_usb_unlock();
		knl_btr_playing(TRUE);
		tm_printf((UB *)"bt: \"%s\": the stream started\n", p->name);
		break;
	case V_SUSPENDING:
		a->st = V_READY;
		knl_btr_playing(FALSE);
		tm_printf((UB *)"bt: \"%s\": the stream suspended, %u packets, %u frames lost\n", p->name, a->packets, a->lost);
		break;
	default:
		break;
	}
}

/* The device's service records looked at: only the log says what came */
EXPORT void knl_bta_sdp_answer( BTC *c, PEER *p, INT got )
{
	(void)c;
	tm_printf((UB *)"bt: \"%s\": its audio sink service: %d bytes\n", p->name, got);
}

/* Its services are known: on to its stream ends, and to where its hands-free service is */
EXPORT void knl_bta_open( BTC *c, PEER *p )
{
	AV	*a;

	if ( av != NULL && av->p != p ) {
		tm_printf((UB *)"bt: \"%s\" left without sound: another device has the stream\n", p->name);
		return;
	}
	a = av_take(c, p);
	if ( a == NULL ) return;
	knl_btsd_hf_ask(c, p);
	if ( p->rcid_av == 0 ) {
		knl_btx_l2_open(c, p, PSM_AVDTP, CID_AV_SIG);
	} else if ( a->st == V_NONE ) {
		/* the earphones opened it: their turn first */
		a->wait_until = ( knl_btx_now() + AV_WAIT_MS ) | 1;
	}
}

/* Channels that have come up, as the configuration of each is done both ways */
EXPORT void knl_bta_chan_up( BTC *c, PEER *p, UB bits )
{
	AV	*a = av;
	INT	mtu;

	if ( a == NULL || a->p != p ) return;
	if ( ( bits & CHB_AV_SIG ) && a->st == V_NONE && p->rcid_av != 0 && a->wait_until == 0 && a->sig == 0 ) {
		discover(a);
	}
	if ( ( bits & CHB_AV_MEDIA ) && a->st == V_MEDIA ) {
		/* a packet as long as both the device and this end take */
		mtu = ( p->media_mtu < TX_MAX - 4 ) ? p->media_mtu : TX_MAX - 4;
		knl_usb_lock();
		sbc_enc_init(&a->enc, a->rate48, a->joint, a->bitpool);
		a->flen = sbc_enc_frame_len(&a->enc);
		a->per_packet = ( mtu - RTP_HEAD ) / a->flen;
		if ( a->per_packet > PER_PACKET_MAX ) a->per_packet = PER_PACKET_MAX;
		if ( a->per_packet < 1 ) a->per_packet = 1;
		a->st = V_READY;
		knl_usb_unlock();
		tm_printf((UB *)"bt: \"%s\": a sound output: SBC %s at %d Hz, bit pool %d, %d bytes a frame, %d frames a packet (MTU %d)\n",
			  p->name, a->joint ? "joint stereo" : "stereo", a->rate48 ? 48000 : 44100, a->bitpool, a->flen,
			  a->per_packet, p->media_mtu);
		knl_btr_want(c, p);
	}
}

EXPORT void knl_bta_chan_closed( BTC *c, PEER *p, UH ours )
{
	AV	*a = av;

	if ( ours == CID_AV_MEDIA ) {
		p->rcid_media = 0;
		p->cfg_in &= (UB)~CHB_AV_MEDIA;
		p->cfg_out &= (UB)~CHB_AV_MEDIA;
		if ( a != NULL && a->p == p && a->st >= V_READY ) {
			a->st = V_NONE;
			knl_btr_playing(FALSE);
		}
	} else if ( ours == CID_AV_SIG ) {
		p->rcid_av = 0;
		p->cfg_in &= (UB)~CHB_AV_SIG;
		p->cfg_out &= (UB)~CHB_AV_SIG;
		if ( a != NULL && a->p == p ) {
			a->st = V_NONE;
			a->sig = 0;
			a->due = 0;
			knl_btr_playing(FALSE);
		}
	} else if ( CID_IS_X(ours) ) {
		UB	bit = (UB)( 1 << XI(ours) );

		p->rcid_x[XI(ours)] = 0;
		p->cfgx_in &= (UB)~bit;
		p->cfgx_out &= (UB)~bit;
		p->x_on &= (UB)~bit;
		if ( ours == CID_AVCTP ) knl_btr_closed(c, p);
		else if ( ours == CID_RFCOMM ) knl_btf_closed(c, p);
		else if ( ours == CID_SDP_HF ) knl_btsd_hf_closed(c, p);
	}
}

EXPORT void knl_bta_down( BTC *c, PEER *p )
{
	knl_btsd_hf_closed(c, p);
	knl_btr_down(c, p);
	knl_btf_down(c, p);
	knl_bth_down(c, p);
	if ( av != NULL && av->p == p ) {
		knl_usb_lock();
		av->st = V_NONE;
		av = NULL;
		knl_usb_unlock();
		knl_btr_playing(FALSE);
	}
}

/*
 * The controller gone, with the manager's lock held: no disconnection event
 * will come, and the sound device must not go on listing its earphone as an
 * output, or choose it and play to nothing.
 */
EXPORT void knl_bta_gone( BTC *c )
{
	if ( av != NULL && av->c == c ) {
		av->st = V_NONE;
		av = NULL;
	}
	knl_bth_gone(c);
	knl_btr_gone(c);
	knl_btf_gone(c);
	knl_btsd_gone(c);
	play_voice = FALSE;
	knl_snd_whole_outside(FALSE);
}

/* ---------------------------------------------------------------- the audio side's other channels */

/* A channel a device asks for: this end's end of it, 0 refused */
EXPORT UH knl_bta_accept( BTC *c, PEER *p, UH psm )
{
	switch ( psm ) {
	case PSM_SDP_A:
		return ( p->rcid_x[XI_SDP_SRV] == 0 ) ? CID_SDP_SRV : 0;
	case PSM_AVCTP:
		return ( p->audio && knl_btr_accept(c, p) ) ? CID_AVCTP : 0;
	case PSM_RFCOMM:
		return ( p->audio && knl_btf_accept(c, p) ) ? CID_RFCOMM : 0;
	default:
		return 0;
	}
}

EXPORT void knl_bta_x_open( BTC *c, PEER *p, UH psm, UH cid )
{
	knl_btx_l2_open(c, p, psm, cid);
}

/* A channel of the audio side's closed by this end */
EXPORT void knl_bta_x_close( BTC *c, PEER *p, UH cid )
{
	UB	r[4];
	UH	rc = p->rcid_x[XI(cid)];
	UB	bit = (UB)( 1 << XI(cid) );

	if ( rc != 0 ) {
		r[0] = (UB)rc;
		r[1] = (UB)( rc >> 8 );
		r[2] = (UB)cid;
		r[3] = (UB)( cid >> 8 );
		knl_btx_sig_send(c, p, 0x06, ++p->ident, r, 4);	/* a disconnection request */
	}
	p->rcid_x[XI(cid)] = 0;
	p->cfgx_in &= (UB)~bit;
	p->cfgx_out &= (UB)~bit;
	p->x_on &= (UB)~bit;
}

/* Those whose configuration is done both ways, each told once */
EXPORT void knl_bta_x_up( BTC *c, PEER *p )
{
	INT	i;
	UB	both = (UB)( p->cfgx_in & p->cfgx_out );

	for ( i = 0; i < NCIDX; i++ ) {
		UB	bit = (UB)( 1 << i );

		if ( ( both & bit ) == 0 || ( p->x_on & bit ) != 0 ) continue;
		p->x_on |= bit;
		switch ( CID_X_FIRST + i ) {
		case CID_AVCTP:		knl_btr_up(c, p);	break;
		case CID_RFCOMM:	knl_btf_up(c, p);	break;
		case CID_SDP_HF:	knl_btsd_hf_up(c, p);	break;
		default:		break;
		}
	}
}

EXPORT void knl_bta_x_in( BTC *c, PEER *p, UH cid, CONST UB *f, INT n )
{
	UB	bit = (UB)( 1 << XI(cid) );

	if ( ( p->x_on & bit ) == 0 ) return;
	switch ( cid ) {
	case CID_SDP_SRV:	knl_btsd_serve(c, p, f, n);	break;
	case CID_AVCTP:		knl_btr_in(c, p, f, n);		break;
	case CID_RFCOMM:	knl_btf_in(c, p, f, n);		break;
	case CID_SDP_HF:	knl_btsd_hf_in(c, p, f, n);	break;
	default:		break;
	}
}

/* The controller's events of the voice link */
EXPORT void knl_bta_event( BTC *c, UB code, CONST UB *d, INT len )
{
	knl_bth_event(c, code, d, len);
}

/* ---------------------------------------------------------------- each round */

/* Whole packets of what was encoded, while the controller is keeping up */
LOCAL void send_packets( AV *a )
{
	while ( a->st == V_STREAM && ( ( a->tail - a->head + NSLOT ) % NSLOT ) >= (UW)a->per_packet
	     && knl_btx_tx_room(a->c, 2) ) {
		UB	*o = a->out;
		INT	i, at = RTP_HEAD;

		a->seq++;
		o[0] = 0x80;
		o[1] = RTP_PT;
		o[2] = (UB)( a->seq >> 8 );
		o[3] = (UB)a->seq;
		o[4] = (UB)( a->ts >> 24 );
		o[5] = (UB)( a->ts >> 16 );
		o[6] = (UB)( a->ts >> 8 );
		o[7] = (UB)a->ts;
		o[8] = o[9] = o[10] = 0;
		o[11] = 1;
		o[12] = (UB)a->per_packet;
		for ( i = 0; i < a->per_packet; i++ ) {
			knl_memcpy(o + at, a->ring[a->head], a->flen);
			at += a->flen;
			a->head = ( a->head + 1 ) % NSLOT;
		}
		a->ts += (UW)( a->per_packet * SBC_FRAME_SAMPLES );
		a->packets++;
		knl_btx_l2_send(a->c, a->p, a->p->rcid_media, o, at);
	}
}

/* The mixer turns the whole down itself unless the earphones do */
LOCAL void outside_look( void )
{
	AV	*a = av;
	BOOL	stream = (BOOL)( !play_voice && a != NULL && a->want_play && knl_btr_absvol() );

	knl_snd_whole_outside((BOOL)( stream || ( play_voice && knl_bth_vol_outside() ) ));
}

/* Each round of the controller's task */
EXPORT void knl_bta_tick( BTC *c )
{
	AV	*a = av;

	knl_btsd_tick(c);
	knl_btr_tick(c);
	knl_btf_tick(c);
	knl_bth_tick(c);
	outside_look();
	if ( a == NULL || a->c != c ) return;
	if ( a->due != 0 && (W)( knl_btx_now() - a->due ) >= 0 ) {
		tm_printf((UB *)"bt: \"%s\": no answer to %02x at step %d\n", a->p->name, a->sig, a->st);
		av_give_up(a, "it did not answer");
		return;
	}
	if ( a->wait_until != 0 && (W)( knl_btx_now() - a->wait_until ) >= 0 ) {
		a->wait_until = 0;
		if ( a->st == V_NONE && a->p->rcid_av != 0 && a->sig == 0 ) discover(a);
	}
	if ( a->sig != 0 ) {
		/* a command is out: the next waits for its answer */
	} else if ( a->st == V_READY && a->want_play && !knl_bth_voice() ) {
		av_seid(a, AV_START, V_STARTING);
	} else if ( a->st == V_STREAM && knl_bth_voice() ) {
		av_seid(a, AV_SUSPEND, V_SUSPENDING);
	} else if ( a->st == V_STREAM && a->want_play ) {
		a->played = TRUE;
	} else if ( a->st == V_STREAM && ( a->played || (W)( knl_btx_now() - a->started_at ) >= AV_HOLD_MS ) ) {
		av_seid(a, AV_SUSPEND, V_SUSPENDING);
	}
	send_packets(a);
}

/* ---------------------------------------------------------------- the sound device's side */

LOCAL void id_of( CONST PEER *p, UB *id )
{
	(void)tm_sprintf(id, (UB *)"bt-%02x%02x%02x%02x%02x%02x", p->addr[5], p->addr[4], p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
}

LOCAL INT bta_outputs( SDOut *o, INT max )
{
	AV	*a = av;
	UB	id[SND_ID_MAX];
	INT	n = 0;

	if ( a != NULL && max > 0 && a->st >= V_READY ) {
		id_of(a->p, id);
		knl_snd_out_set(&o[0], id, a->p->name, SND_BE_BT, knl_bth_voice() ? SND_OF_NOSOUND : 0);
		n = 1;
	}
	return n + knl_bth_outputs(o + n, max - n);
}

LOCAL BOOL bta_can_record( void )
{
	return knl_bth_can_record();
}

LOCAL ER bta_play_start( CONST UB *id, UW *rate, INT *channels )
{
	AV	*a = av;
	UB	mine[SND_ID_MAX];

	if ( knl_bth_is_id(id) ) {
		ER	er = knl_bth_play_start(id, rate, channels);

		play_voice = (BOOL)( er >= E_OK );
		return er;
	}
	if ( a == NULL || a->st < V_READY || id == NULL ) return E_NOEXS;
	id_of(a->p, mine);
	if ( knl_strcmp((CONST char *)id, (CONST char *)mine) != 0 ) return E_NOEXS;
	*rate = a->rate48 ? 48000 : 44100;
	*channels = 2;
	play_voice = FALSE;
	a->want_play = TRUE;
	(void)tk_sig_sem(a->c->sem, 1);
	return E_OK;
}

LOCAL void bta_play_stop( void )
{
	AV	*a = av;

	if ( play_voice ) {
		knl_bth_play_stop();
		play_voice = FALSE;
	} else if ( a != NULL ) {
		a->want_play = FALSE;
		(void)tk_sig_sem(a->c->sem, 1);
	}
	knl_snd_whole_outside(FALSE);
}

LOCAL BOOL bta_play_running( void )
{
	AV	*a = av;

	if ( play_voice ) return knl_bth_play_running();
	return (BOOL)( a != NULL && a->st >= V_READY );
}

/* The frames that are due by the clock, from the mixer, into the ring */
LOCAL void bta_poll( void )
{
	AV	*a = av;
	short	pcm[SBC_FRAME_SAMPLES * 2];
	UD	due;
	INT	n = 0;

	if ( play_voice || a == NULL || a->st != V_STREAM || !a->want_play ) return;
	due = ( (UD)(UW)( knl_usb_ms() - a->start_ms ) * ( a->rate48 ? 48000 : 44100 ) ) / ( 1000 * SBC_FRAME_SAMPLES );
	while ( a->made < due && n < POLL_FRAMES ) {
		UW	next = ( a->tail + 1 ) % NSLOT;

		(void)knl_snd_mix((UB *)pcm, SBC_FRAME_SAMPLES, a->rate48 ? 48000 : 44100, 2, FALSE);
		a->made++;
		n++;
		if ( next == a->head ) {
			a->lost++;
			continue;
		}
		(void)sbc_enc_frame(&a->enc, pcm, a->ring[a->tail]);
		a->tail = next;
	}
	if ( n > 0 ) (void)tk_sig_sem(a->c->sem, 1);
}

LOCAL ER bta_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	return knl_bth_rec_start(rate, dev_rate, channels);
}

LOCAL void bta_rec_stop( void )		{ knl_bth_rec_stop(); }
LOCAL BOOL bta_rec_running( void )	{ return knl_bth_rec_running(); }
LOCAL ER   bta_get_level( INT dn, SDVol *v ) { (void)dn; (void)v; return E_NOSPT; }
LOCAL ER   bta_set_level( INT dn, CONST SDVol *v ) { (void)dn; (void)v; return E_NOSPT; }

/* A sink takes a moment to open its sound after the start, and a suspend would cut what follows */
EXPORT CONST SNDHW knl_bta_hw = {
	SND_BE_BT,
	bta_outputs, bta_can_record,
	bta_play_start, bta_play_stop, bta_play_running,
	bta_rec_start, bta_rec_stop, bta_rec_running,
	bta_poll, bta_get_level, bta_set_level,
	200, 2000
};
