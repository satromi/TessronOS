/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_hfp.c
 *	The hands-free profile (design 6.8): this end the audio gateway, as
 *	a telephone is, the earphones the hands-free unit; their microphone
 *	an input of the sound device and their voice channel an output.
 *
 *	The service level connection is AT commands on the RFCOMM channel
 *	(bt_rfcomm.c): the earphones say their features (AT+BRSF) and are
 *	told this end's, ask what the indicators are and their values
 *	(AT+CIND), and turn their reports on (AT+CMER); with that the
 *	connection is up. This end has no calls: the indicators say a
 *	network and no call, and what asks for a call is answered ERROR.
 *	The other commands earphones send to set themselves up are taken.
 *
 *	The voice goes on a synchronous link, eSCO or SCO, set up when the
 *	sound device plays on the output or records from the microphone,
 *	and taken down a while after both have stopped. This end does not
 *	negotiate codecs, so the voice is CVSD at 8 kHz, which every
 *	hands-free unit has. The link is asked with the parameter sets the
 *	profile names for CVSD, the best first (S4 when the earphones say
 *	they take it, S3, S1, and SCO's D1 last), each next one when the
 *	one before is refused.
 *
 *	The samples of the voice go over USB, on the isochronous endpoints
 *	of the controller's second interface, in its alternate setting for
 *	one voice channel of 16-bit samples: an SCO packet, its head of
 *	three bytes and 48 bytes of samples, is three USB packets of 17
 *	bytes, one a millisecond, so that the stream out is paced by the
 *	USB frames themselves. The packets in are put back together by
 *	their heads, and their samples given to the sound device.
 *
 *	The earphones' speaker gain (AT+VGS) and the mixer's whole are kept
 *	the same while the voice plays, as AVRCP's absolute volume is.
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

/* HCI */
#define C_SETUP_SYNC		0x0428
#define C_ACCEPT_SYNC		0x0429
#define C_REJECT_SYNC		0x042a
#define E_DISCONN		0x05
#define E_CONN_REQ		0x04
#define E_CMD_STATUS		0x0f
#define E_SYNC_DONE		0x2c
#define E_SYNC_CHANGED		0x2d
#define LINK_SCO		0x00
#define LINK_ESCO		0x02
#define VOICE_CVSD_16		0x0060	/* linear input, 2's complement, 16-bit samples, CVSD in the air */
#define VOICE_BW		8000	/* bytes a second each way */
#define REJ_LIMITED		0x0d

/* The HF's features (AT+BRSF) */
#define HF_REMOTE_VOL		0x0010
#define HF_ESCO_S4		0x0200
#define AG_FEATURES		0

/* The voice interface on USB */
#define VOICE_IF		1
#define VOICE_ALT		2	/* one voice channel of 16-bit samples */
#define SCO_MAX			60

#define AT_MAX			128
#define OPEN_AFTER_MS		1500	/* the earphones' time to open the channel themselves */
#define SETUP_TMO_MS		10000
#define IDLE_MS			1500	/* the link kept this long after the last use */
#define RETRY_MS		5000	/* and not asked again for this long after it failed */
#define VGS_MAX			15

/* CVSD's parameter sets: the latest packet, retransmission, and the packet types */
LOCAL CONST struct { UH lat; UB retx; UH pkt; CONST char *name; } psets[] = {
	{ 0x000c, 0x02, 0x0380, "S4" },		/* 2-EV3 */
	{ 0x000a, 0x01, 0x0380, "S3" },		/* 2-EV3 */
	{ 0x0007, 0x01, 0x03c8, "S1" },		/* EV3 */
	{ 0xffff, 0xff, 0x03c4, "D1" }		/* SCO, HV3 */
};
#define NPSET	( (INT)( sizeof(psets) / sizeof(psets[0]) ) )

enum { V_NONE, V_SETUP, V_UP, V_DOWNING };

typedef struct {
	BTC		*c;
	PEER		*p;
	BOOL		dlc;			/* the RFCOMM channel is up */
	BOOL		slc;			/* the service level connection is */
	UW		hf_feat;
	UB		open_ch;		/* the earphones' channel to open, 0 none */
	UW		open_at;
	char		line[AT_MAX];
	INT		nl;
	INT		vgs_sent;		/* the gain the earphones have, as far as this end knows */
	/* the voice link */
	UB		vst;			/* V_* */
	UH		sh;
	INT		pset;
	BOOL		incoming;
	UW		due;
	UW		idle_from;
	UW		failed_at;		/* when it last could not be set up, 0 not */
	/* the sound device's side, with the manager's lock */
	volatile BOOL	want_play, want_rec;
	/* USB */
	USBPIPE		*pin, *pout;
	UB		alt;
	INT		sco_len;
	UB		tx[3 + SCO_MAX];
	INT		tx_at, tx_len;
	UB		rx[3 + 255];
	INT		rx_have;
	T_BTHSTAT	st;
} HFP;

LOCAL HFP	hf;

/* ---------------------------------------------------------------- AT */

LOCAL void at_send( CONST char *s )
{
	UB	b[AT_MAX + 4];
	INT	n = (INT)knl_strlen(s);

	if ( n > AT_MAX ) n = AT_MAX;
	b[0] = '\r';
	b[1] = '\n';
	knl_memcpy(b + 2, s, n);
	b[n + 2] = '\r';
	b[n + 3] = '\n';
	knl_btf_send(hf.c, b, n + 4);
}

LOCAL BOOL at_is( CONST char *l, CONST char *pre )
{
	while ( *pre != 0 ) {
		char	a = *l++;

		if ( a >= 'a' && a <= 'z' ) a = (char)( a - 'a' + 'A' );
		if ( a != *pre++ ) return FALSE;
	}
	return TRUE;
}

LOCAL INT at_num( CONST char *s )
{
	INT	v = 0;

	while ( *s == ' ' ) s++;
	while ( *s >= '0' && *s <= '9' ) v = v * 10 + ( *s++ - '0' );
	return v;
}

LOCAL INT gain_of( UW v )
{
	return (INT)( ( v * VGS_MAX + SND_VOL_MAX / 2 ) / SND_VOL_MAX );
}

/* The earphones' speaker gain set by their buttons: the mixer's whole with it */
LOCAL void vgs_in( INT g )
{
	SDMixSet	s;
	SDMix		m;

	if ( g < 0 || g > VGS_MAX ) return;
	(void)knl_snd_mix_get(&m, 0);
	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = s.rv = (UW)( ( g * SND_VOL_MAX + VGS_MAX / 2 ) / VGS_MAX );
	s.mute = m.mute;
	(void)knl_snd_mix_set(&s, TRUE);
	hf.vgs_sent = gain_of(s.lv);
}

LOCAL void at_line( CONST char *l )
{
	CONST char	*rest;
	char		t[48];

	if ( !at_is(l, "AT") ) return;
	rest = l + 2;
	if ( at_is(rest, "+BRSF=") ) {
		hf.hf_feat = (UW)at_num(rest + 6);
		(void)tm_sprintf((UB *)t, (UB *)"+BRSF: %d", AG_FEATURES);
		at_send(t);
		at_send("OK");
	} else if ( at_is(rest, "+CIND=?") ) {
		at_send("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),"
			"(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))");
		at_send("OK");
	} else if ( at_is(rest, "+CIND?") ) {
		at_send("+CIND: 1,0,0,0,5,0,5");
		at_send("OK");
	} else if ( at_is(rest, "+CMER=") ) {
		at_send("OK");
		if ( !hf.slc ) {
			hf.slc = TRUE;
			hf.failed_at = 0;
			hf.vgs_sent = -1;
			tm_printf((UB *)"bt: \"%s\": hands-free up, its features %04x\n", hf.p->name, hf.hf_feat);
		}
	} else if ( at_is(rest, "+CHLD=?") ) {
		at_send("+CHLD: (0)");
		at_send("OK");
	} else if ( at_is(rest, "+VGS=") ) {
		vgs_in(at_num(rest + 5));
		at_send("OK");
	} else if ( at_is(rest, "+COPS?") ) {
		at_send("+COPS: 0,0,\"TessronOS\"");
		at_send("OK");
	} else if ( at_is(rest, "+VGM=") || at_is(rest, "+BAC=") || at_is(rest, "+CMEE=") || at_is(rest, "+CLIP=")
		 || at_is(rest, "+CCWA=") || at_is(rest, "+NREC=") || at_is(rest, "+BIA=") || at_is(rest, "+XAPL=")
		 || at_is(rest, "+IPHONEACCEV=") || at_is(rest, "+XEVENT=") || at_is(rest, "+CSRSF=")
		 || at_is(rest, "+BTRH?") || at_is(rest, "+CNUM") || at_is(rest, "+CLCC") || at_is(rest, "+COPS=")
		 || at_is(rest, "+CKPD=") ) {
		at_send("OK");
	} else {
		tm_printf((UB *)"bt: \"%s\": hands-free: %s refused\n", hf.p->name, l);
		at_send("ERROR");
	}
}

EXPORT void knl_bth_at_in( BTC *c, PEER *p, CONST UB *d, INT n )
{
	INT	i;

	(void)c;
	if ( hf.p != p || !hf.dlc ) return;
	for ( i = 0; i < n; i++ ) {
		if ( d[i] == '\r' ) {
			hf.line[hf.nl] = 0;
			if ( hf.nl > 0 ) at_line(hf.line);
			hf.nl = 0;
		} else if ( d[i] != '\n' && hf.nl < AT_MAX - 1 ) {
			hf.line[hf.nl++] = (char)d[i];
		}
	}
}

/* ---------------------------------------------------------------- the RFCOMM channel */

/* The earphones' hands-free service known: its channel opened a while after, unless they open one */
EXPORT void knl_bth_hf_found( BTC *c, PEER *p, UB ch, UH features )
{
	(void)features;
	if ( ch == 0 || knl_btf_busy(p) ) return;
	if ( hf.p != NULL && hf.p != p && hf.dlc ) return;
	hf.c = c;
	hf.p = p;
	hf.open_ch = ch;
	hf.open_at = ( knl_btx_now() + OPEN_AFTER_MS ) | 1;
}

EXPORT void knl_bth_dlc_up( BTC *c, PEER *p )
{
	if ( hf.p != NULL && hf.p != p && hf.slc ) return;
	hf.c = c;
	hf.p = p;
	hf.dlc = TRUE;
	hf.slc = FALSE;
	hf.nl = 0;
	hf.open_at = 0;
}

LOCAL void voice_close( void );

EXPORT void knl_bth_dlc_down( BTC *c, PEER *p )
{
	UB	r[3];

	(void)c;
	if ( hf.p != p ) return;
	if ( hf.slc ) tm_printf((UB *)"bt: \"%s\": hands-free down\n", p->name);
	hf.dlc = hf.slc = FALSE;
	if ( hf.vst == V_UP || hf.vst == V_SETUP ) {
		if ( hf.vst == V_UP ) {
			r[0] = (UB)hf.sh;
			r[1] = (UB)( hf.sh >> 8 );
			r[2] = 0x13;
			knl_btx_cmd(hf.c, C_DISCONNECT, r, 3);
			hf.vst = V_DOWNING;
		}
	}
}

/* ---------------------------------------------------------------- the voice on USB */

/* The packets of the voice out, one each time the controller takes a USB packet: with the manager's lock */
LOCAL INT sco_fill( void *ctx, UB *dst, INT max )
{
	INT	n;

	(void)ctx;
	if ( hf.vst != V_UP || hf.sco_len <= 0 ) return 0;
	if ( hf.tx_at >= hf.tx_len ) {
		hf.tx[0] = (UB)hf.sh;
		hf.tx[1] = (UB)( ( hf.sh >> 8 ) & 0x0f );
		hf.tx[2] = (UB)hf.sco_len;
		if ( !hf.want_play || knl_snd_mix(hf.tx + 3, hf.sco_len / 2, 8000, 1, FALSE) <= 0 ) {
			knl_memset(hf.tx + 3, 0, hf.sco_len);
		}
		hf.tx_len = 3 + hf.sco_len;
		hf.tx_at = 0;
		hf.st.tx_packets++;
	}
	n = hf.tx_len - hf.tx_at;
	if ( n > max ) n = max;
	knl_memcpy(dst, hf.tx + hf.tx_at, n);
	hf.tx_at += n;
	return n;
}

/* The pieces in, put back together by the SCO heads: with the manager's lock */
LOCAL void sco_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	UB	t[256];
	INT	k = 0;

	(void)ctx;
	if ( err != 0 ) {
		hf.rx_have = 0;
		return;
	}
	if ( len <= 0 || hf.vst != V_UP ) return;
	if ( len > (INT)sizeof(t) ) len = sizeof(t);
	knl_dmabuf_read(b, off, t, len);
	while ( k < len ) {
		INT	want, take;

		want = ( hf.rx_have < 3 ) ? 3 : 3 + hf.rx[2];
		take = want - hf.rx_have;
		if ( take > len - k ) take = len - k;
		knl_memcpy(hf.rx + hf.rx_have, t + k, take);
		hf.rx_have += take;
		k += take;
		if ( hf.rx_have == 3 + hf.rx[2] && hf.rx_have >= 3 ) {
			INT	ns = hf.rx[2] / 2;

			if ( ( ( hf.rx[0] | ( hf.rx[1] << 8 ) ) & 0x0fff ) == hf.sh && ns > 0 ) {
				hf.st.rx_packets++;
				if ( hf.want_rec ) {
					knl_snd_capture(hf.rx + 3, ns, 1);
					hf.st.rx_frames += (UW)ns;
				}
			}
			hf.rx_have = 0;
		}
	}
}

/* The second interface's setting for the voice, and its endpoints: FALSE when the controller has none */
LOCAL BOOL voice_eps( USBDEV *dev, USBEP *in, USBEP *out )
{
	UB	*p, *end = dev->cfg + dev->cfglen;
	BOOL	here = FALSE, got_in = FALSE, got_out = FALSE;

	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			here = (BOOL)( p[2] == VOICE_IF && p[3] == VOICE_ALT );
			continue;
		}
		if ( !here || p[1] != USB_DT_ENDPOINT || p[0] < 7 || ( p[3] & 3 ) != USB_EP_ISOC ) continue;
		if ( ( p[2] & 0x80 ) != 0 ) {
			knl_usb_ep_from_desc(in, dev, p, NULL);
			got_in = TRUE;
		} else {
			knl_usb_ep_from_desc(out, dev, p, NULL);
			got_out = TRUE;
		}
	}
	return (BOOL)( got_in && got_out );
}

/* The link up: the voice interface set and its streams opened */
LOCAL BOOL voice_open( void )
{
	USBDEV	*dev = hf.c->dev;
	USBEP	in, out;
	ER	er;

	if ( dev == NULL || dev->cfg == NULL || !voice_eps(dev, &in, &out) ) {
		tm_printf((UB *)"bt: the controller has no voice interface on USB: no voice\n");
		return FALSE;
	}
	knl_usb_lock();
	hf.tx_at = hf.tx_len = 0;
	hf.rx_have = 0;
	hf.sco_len = 3 * out.size - 3;
	if ( hf.sco_len > SCO_MAX ) hf.sco_len = SCO_MAX;
	hf.sco_len &= ~1;
	er = hf.c->gone ? E_IO : knl_usb_set_interface(dev, VOICE_IF, VOICE_ALT);
	if ( er >= E_OK ) er = dev->hc->ops->stream_open(dev->hc, dev, &in, in.size, sco_xfer, &hf, &hf.pin);
	if ( er >= E_OK ) er = dev->hc->ops->stream_open_out(dev->hc, dev, &out, sco_fill, &hf, &hf.pout);
	if ( er >= E_OK ) hf.alt = VOICE_ALT;
	knl_usb_unlock();
	if ( er < E_OK ) {
		tm_printf((UB *)"bt: the voice interface not opened (%d)\n", (INT)er);
		voice_close();
		return FALSE;
	}
	tm_printf((UB *)"bt: voice on USB: setting %d, packets of %d bytes, %d samples an SCO packet\n",
		  VOICE_ALT, out.size, hf.sco_len / 2);
	return TRUE;
}

LOCAL void voice_close_locked( void )
{
	USBDEV	*dev = ( hf.c != NULL ) ? hf.c->dev : NULL;

	if ( dev != NULL ) {
		if ( hf.pout != NULL ) dev->hc->ops->stream_close(dev->hc, hf.pout);
		if ( hf.pin != NULL ) dev->hc->ops->stream_close(dev->hc, hf.pin);
		if ( hf.alt != 0 && !hf.c->gone ) (void)knl_usb_quiesce(dev, VOICE_IF);
	}
	hf.pin = hf.pout = NULL;
	hf.alt = 0;
	hf.sco_len = 0;
}

LOCAL void voice_close( void )
{
	knl_usb_lock();
	voice_close_locked();
	knl_usb_unlock();
}

/* ---------------------------------------------------------------- the link */

LOCAL void setup_send( void )
{
	UB	r[17];
	UH	h = hf.p->handle;

	r[0] = (UB)h;
	r[1] = (UB)( h >> 8 );
	r[2] = (UB)VOICE_BW;
	r[3] = (UB)( VOICE_BW >> 8 );
	r[4] = r[5] = 0;
	r[6] = (UB)VOICE_BW;
	r[7] = (UB)( VOICE_BW >> 8 );
	r[8] = r[9] = 0;
	r[10] = (UB)psets[hf.pset].lat;
	r[11] = (UB)( psets[hf.pset].lat >> 8 );
	r[12] = (UB)VOICE_CVSD_16;
	r[13] = (UB)( VOICE_CVSD_16 >> 8 );
	r[14] = psets[hf.pset].retx;
	r[15] = (UB)psets[hf.pset].pkt;
	r[16] = (UB)( psets[hf.pset].pkt >> 8 );
	hf.due = knl_btx_now() + SETUP_TMO_MS;
	knl_btx_cmd(hf.c, C_SETUP_SYNC, r, 17);
	tm_printf((UB *)"bt: \"%s\": asks a voice link, %s\n", hf.p->name, psets[hf.pset].name);
}

LOCAL void setup_failed( CONST char *why, INT st )
{
	tm_printf((UB *)"bt: \"%s\": no voice link: %s (%02x)\n", hf.p->name, why, st);
	hf.vst = V_NONE;
	hf.due = 0;
	hf.failed_at = knl_btx_now() | 1;
}

/* The next parameter set, or none left */
LOCAL void setup_next( INT st )
{
	if ( !hf.incoming && hf.pset + 1 < NPSET && hf.p != NULL && hf.p->up ) {
		hf.pset++;
		setup_send();
	} else {
		setup_failed("refused", st);
	}
}

EXPORT void knl_bth_event( BTC *c, UB code, CONST UB *d, INT len )
{
	UB	r[21];

	switch ( code ) {
	case E_CONN_REQ:
		/* the earphones ask the voice link themselves */
		if ( len < 10 ) break;
		knl_memcpy(r, d, 6);
		if ( hf.slc && hf.c == c && hf.p != NULL && same_bt_bytes(hf.p->addr, d, 6) && hf.vst == V_NONE && c->dev != NULL ) {
			r[6] = (UB)VOICE_BW;
			r[7] = (UB)( VOICE_BW >> 8 );
			r[8] = r[9] = 0;
			r[10] = (UB)VOICE_BW;
			r[11] = (UB)( VOICE_BW >> 8 );
			r[12] = r[13] = 0;
			r[14] = r[15] = 0xff;			/* any latency */
			r[16] = (UB)VOICE_CVSD_16;
			r[17] = (UB)( VOICE_CVSD_16 >> 8 );
			r[18] = 0xff;				/* any retransmission */
			r[19] = 0x3f;				/* any packet type */
			r[20] = 0x00;
			hf.vst = V_SETUP;
			hf.incoming = TRUE;
			hf.due = knl_btx_now() + SETUP_TMO_MS;
			knl_btx_cmd(c, C_ACCEPT_SYNC, r, 21);
			tm_printf((UB *)"bt: \"%s\" asks a voice link (%s): taken\n", hf.p->name, ( d[9] == LINK_ESCO ) ? "eSCO" : "SCO");
		} else {
			r[6] = REJ_LIMITED;
			knl_btx_cmd(c, C_REJECT_SYNC, r, 7);
			tm_printf((UB *)"bt: a voice link asked, refused\n");
		}
		break;
	case E_CMD_STATUS:
		if ( len >= 4 && d[0] != 0 && hf.vst == V_SETUP ) {
			UH	op = (UH)( d[2] | ( d[3] << 8 ) );

			if ( op == C_SETUP_SYNC ) setup_next(d[0]);
			else if ( op == C_ACCEPT_SYNC ) setup_failed("not taken", d[0]);
		}
		break;
	case E_SYNC_DONE:
		if ( len < 17 || hf.vst != V_SETUP || hf.p == NULL || !same_bt_bytes(hf.p->addr, d + 3, 6) ) break;
		if ( d[0] != 0 ) {
			setup_next(d[0]);
			break;
		}
		hf.sh = (UH)( ( d[1] | ( d[2] << 8 ) ) & 0x0fff );
		hf.due = 0;
		hf.vst = V_UP;
		hf.idle_from = knl_btx_now();
		hf.st.handle = hf.sh;
		tm_printf((UB *)"bt: \"%s\": voice link %d up, %s, every %d slots, packets of %d bytes, air mode %d\n",
			  hf.p->name, hf.sh, ( d[9] == LINK_ESCO ) ? "eSCO" : "SCO", d[10], d[12] | ( d[13] << 8 ), d[16]);
		if ( !voice_open() ) {
			r[0] = (UB)hf.sh;
			r[1] = (UB)( hf.sh >> 8 );
			r[2] = 0x13;
			knl_btx_cmd(c, C_DISCONNECT, r, 3);
			hf.vst = V_DOWNING;
			hf.failed_at = knl_btx_now() | 1;
		}
		hf.st.alt = hf.alt;
		break;
	case E_SYNC_CHANGED:
		if ( len >= 9 && d[0] == 0 ) {
			tm_printf((UB *)"bt: voice link %d changed: every %d slots\n", ( d[1] | ( d[2] << 8 ) ) & 0x0fff, d[3]);
		}
		break;
	case E_DISCONN:
		if ( len >= 4 && hf.vst != V_NONE && ( ( d[1] | ( d[2] << 8 ) ) & 0x0fff ) == hf.sh ) {
			tm_printf((UB *)"bt: voice link %d gone, reason %02x\n", hf.sh, d[3]);
			voice_close();
			hf.vst = V_NONE;
			hf.sh = 0;
		}
		break;
	default:
		break;
	}
}

EXPORT BOOL knl_bth_voice( void )
{
	return (BOOL)( hf.vst != V_NONE );
}

EXPORT BOOL knl_bth_vol_outside( void )
{
	return (BOOL)( hf.slc && hf.vst == V_UP && hf.want_play && ( hf.hf_feat & HF_REMOTE_VOL ) != 0 );
}

EXPORT void knl_bth_tick( BTC *c )
{
	UW	now = knl_btx_now();
	BOOL	want;
	UB	r[3];

	if ( hf.c != c || hf.p == NULL ) return;
	if ( hf.open_at != 0 && (W)( now - hf.open_at ) >= 0 ) {
		hf.open_at = 0;
		if ( !hf.dlc && hf.p->up && !knl_btf_busy(hf.p) ) {
			tm_printf((UB *)"bt: \"%s\": opens its hands-free channel %d\n", hf.p->name, hf.open_ch);
			knl_btf_open(c, hf.p, hf.open_ch);
		}
	}
	want = (BOOL)( hf.want_play || hf.want_rec );
	if ( !want && hf.failed_at != 0 && hf.vst == V_NONE ) hf.failed_at = 0;	/* asked anew next time */
	if ( hf.failed_at != 0 && (W)( now - hf.failed_at ) >= RETRY_MS ) hf.failed_at = 0;
	if ( hf.vst == V_SETUP && hf.due != 0 && (W)( now - hf.due ) >= 0 ) setup_failed("no answer", 0xff);
	if ( want && hf.slc && hf.vst == V_NONE && hf.failed_at == 0 && hf.p->up && c->dev != NULL ) {
		hf.incoming = FALSE;
		hf.pset = ( ( hf.hf_feat & HF_ESCO_S4 ) != 0 ) ? 0 : 1;
		hf.vst = V_SETUP;
		setup_send();
	}
	if ( hf.vst == V_UP ) {
		if ( want ) {
			hf.idle_from = now;
		} else if ( (W)( now - hf.idle_from ) >= IDLE_MS ) {
			r[0] = (UB)hf.sh;
			r[1] = (UB)( hf.sh >> 8 );
			r[2] = 0x13;
			knl_btx_cmd(c, C_DISCONNECT, r, 3);
			hf.vst = V_DOWNING;
		}
	}
	/* the mixer's whole to the earphones' speaker while the voice plays */
	if ( hf.slc && hf.vst == V_UP && ( hf.hf_feat & HF_REMOTE_VOL ) != 0 ) {
		SDMix	m;
		INT	g;

		(void)knl_snd_mix_get(&m, 0);
		g = gain_of(m.lv);
		if ( g != hf.vgs_sent ) {
			char	t[16];

			hf.vgs_sent = g;
			(void)tm_sprintf((UB *)t, (UB *)"+VGS: %d", g);
			at_send(t);
		}
	}
}

/* The link to the earphones gone */
EXPORT void knl_bth_down( BTC *c, PEER *p )
{
	(void)c;
	if ( hf.p != p ) return;
	voice_close();
	knl_usb_lock();
	hf.vst = V_NONE;
	hf.dlc = hf.slc = FALSE;
	hf.open_at = 0;
	hf.p = NULL;
	knl_usb_unlock();
}

/* The controller gone, with the manager's lock held: its streams go with it */
EXPORT void knl_bth_gone( BTC *c )
{
	if ( hf.c != c ) return;
	voice_close_locked();
	hf.vst = V_NONE;
	hf.dlc = hf.slc = FALSE;
	hf.p = NULL;
	hf.c = NULL;
}

EXPORT void knl_bth_stat( T_BTHSTAT *s )
{
	*s = hf.st;
}

/* ---------------------------------------------------------------- the sound device's side */

LOCAL void hfp_id( CONST PEER *p, UB *id )
{
	(void)tm_sprintf(id, (UB *)"bt-%02x%02x%02x%02x%02x%02x-hfp", p->addr[5], p->addr[4], p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
}

EXPORT INT knl_bth_outputs( SDOut *o, INT max )
{
	UB	id[SND_ID_MAX], name[SND_NAME_MAX];

	if ( !hf.slc || hf.p == NULL || max < 1 || hf.c == NULL || hf.c->dev == NULL ) return 0;
	hfp_id(hf.p, id);
	(void)tm_sprintf(name, (UB *)"%s（通話）", hf.p->name);
	knl_snd_out_set(&o[0], id, name, SND_BE_BT, ( hf.failed_at != 0 ) ? SND_OF_NOSOUND : 0);
	return 1;
}

EXPORT BOOL knl_bth_is_id( CONST UB *id )
{
	UB	mine[SND_ID_MAX];

	if ( hf.p == NULL || id == NULL ) return FALSE;
	hfp_id(hf.p, mine);
	return (BOOL)( knl_strcmp((CONST char *)id, (CONST char *)mine) == 0 );
}

EXPORT ER knl_bth_play_start( CONST UB *id, UW *rate, INT *channels )
{
	if ( !hf.slc || !knl_bth_is_id(id) ) return E_NOEXS;
	*rate = 8000;
	*channels = 1;
	hf.want_play = TRUE;
	(void)tk_sig_sem(hf.c->sem, 1);
	return E_OK;
}

EXPORT void knl_bth_play_stop( void )
{
	hf.want_play = FALSE;
	if ( hf.c != NULL ) (void)tk_sig_sem(hf.c->sem, 1);
}

EXPORT BOOL knl_bth_play_running( void )
{
	return (BOOL)( hf.slc && hf.want_play && hf.failed_at == 0 );
}

EXPORT BOOL knl_bth_can_record( void )
{
	return (BOOL)( hf.slc && hf.c != NULL && hf.c->dev != NULL && hf.failed_at == 0 );
}

EXPORT ER knl_bth_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	(void)rate;
	if ( !knl_bth_can_record() ) return E_NOSPT;
	*dev_rate = 8000;
	*channels = 1;
	hf.want_rec = TRUE;
	(void)tk_sig_sem(hf.c->sem, 1);
	return E_OK;
}

EXPORT void knl_bth_rec_stop( void )
{
	hf.want_rec = FALSE;
	if ( hf.c != NULL ) (void)tk_sig_sem(hf.c->sem, 1);
}

EXPORT BOOL knl_bth_rec_running( void )
{
	return (BOOL)( hf.slc && hf.want_rec && hf.failed_at == 0 );
}
