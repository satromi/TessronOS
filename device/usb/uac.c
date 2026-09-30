/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uac.c
 *	USB audio, class 1.0: recording (microphones, the audio of cameras)
 *	and playback (headphones, speakers, DACs), as a backend of the sound
 *	device (design 10.14, 10.15).
 *
 *	The AudioControl interface (class 1, subclass 1) has the terminals
 *	and the feature units they feed. The recording volume is the feature
 *	unit fed by a microphone kind of input terminal (0x02xx), the
 *	playback volume the one fed by the USB streaming terminal (0x0101);
 *	both are volume control selector 2 on the master channel or channel
 *	1.
 *
 *	An AudioStreaming interface (subclass 2) has no bandwidth on its
 *	setting 0; every other setting carries a Type I PCM format (channels,
 *	bytes and bits a sample, the rates it takes) on one isochronous
 *	endpoint, IN to record and OUT to play. A 16 bit setting is chosen,
 *	at the rate asked for when it has it, else the nearest above (the
 *	sound device converts the rate). Playback sends the nominal number
 *	of samples in every packet, which adaptive and synchronous sinks
 *	take; the feedback endpoint of an asynchronous one is not read.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/snd.h>
#include "usbdev.h"
#include "../snd/sndhw.h"

/*
 * A device came or went since the object layer was last told: the sound
 * device's medium, as its object sees it (design 18.7). Told outside the
 * manager's lock, by knl_uac_notify.
 */
LOCAL BOOL	uac_changed = FALSE;

#define AUDIO_SC_CONTROL	0x01
#define AUDIO_SC_STREAMING	0x02
#define AC_HEADER		0x01
#define AC_INPUT_TERMINAL	0x02
#define AC_FEATURE_UNIT		0x06
#define AS_GENERAL		0x01
#define AS_FORMAT_TYPE		0x02
#define EP_GENERAL		0x01

#define UAC_SET_CUR		0x01
#define UAC_GET_CUR		0x81
#define UAC_GET_MIN		0x82
#define UAC_GET_MAX		0x83
#define FU_VOLUME		0x02
#define EP_SAMPLING_FREQ	0x01

#define UAC_MAX			2
#define UAC_ALTS		8
#define UAC_RATES		8
#define TERM_USB_STREAMING	0x0101

#define OUT_RATE		48000	/* the playback rate asked for */
#define PERIOD_MAX		32000	/* the longest packet period taken (us) */

typedef struct {
	INT	alt;
	INT	channels, subframe, bits;
	BOOL	cont;		/* a continuous range rmin .. rmax */
	INT	nrates;
	UW	rates[UAC_RATES];
	UW	rmin, rmax;
	BOOL	freq_ctl;	/* the endpoint takes SAMPLING_FREQ_CONTROL */
	USBEP	ep;
} UACALT;

/* One direction: recording (IN) or playback (OUT) */
typedef struct {
	INT	as_if;		/* its streaming interface, -1 none */
	INT	fu_id, fu_ch;	/* its volume feature unit, 0 none */
	H	vol_min, vol_max, vol_cur;
	INT	nalt;
	UACALT	alt[UAC_ALTS];
	INT	cur;		/* the setting streaming, -1 idle */
	UW	rate;
	USBPIPE	*pipe;
	INT	pkt_max;	/* recording: one sample more than nominal */
} UACDIR;

typedef struct {
	BOOL	used;
	USBDEV	*dev;
	INT	ac_if;
	INT	bcd;		/* bcdADC */
	UACDIR	cap, play;
} UAC;

LOCAL UAC	uac_pool[UAC_MAX];

/* The format of each stream while it runs */
typedef struct {
	UW	rate;
	INT	channels;
	INT	subframe;	/* bytes a sample: 2 */
	UW	period_us;	/* what one packet covers */
	INT	max_bytes;	/* the largest packet */
} UACFMT;

LOCAL UAC	*cap_uac, *out_uac;	/* the functions the streams run on */
LOCAL UACFMT	cap_fmt, out_fmt;
LOCAL UW	pkt_acc;		/* the samples left over, in 1/200000 */

/* ---------------------------------------------------------------- parsing */

LOCAL BOOL rate_ok( CONST UACALT *a, UW r )
{
	INT	i;

	if ( a->cont ) {
		return ( r >= a->rmin && r <= a->rmax );
	}
	for ( i = 0; i < a->nrates; i++ ) {
		if ( a->rates[i] == r ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* The lowest rate of the setting at or above r, 0 if none */
LOCAL UW rate_above( CONST UACALT *a, UW r )
{
	UW	best = 0;
	INT	i;

	if ( a->cont ) {
		return ( r <= a->rmax ) ? ( r < a->rmin ? a->rmin : r ) : 0;
	}
	for ( i = 0; i < a->nrates; i++ ) {
		if ( a->rates[i] >= r && ( best == 0 || a->rates[i] < best ) ) {
			best = a->rates[i];
		}
	}

	return best;
}

LOCAL UW rate_highest( CONST UACALT *a )
{
	UW	best = 0;
	INT	i;

	if ( a->cont ) {
		return a->rmax;
	}
	for ( i = 0; i < a->nrates; i++ ) {
		if ( a->rates[i] > best ) {
			best = a->rates[i];
		}
	}

	return best;
}

LOCAL void parse_streaming( UAC *u, UB *id, UB *end )
{
	UACALT	a;
	UACDIR	*dir;
	UB	*p;
	BOOL	pcm = FALSE, in = FALSE, have_ep = FALSE;

	knl_memset(&a, 0, sizeof(a));
	a.alt = id[3];
	for ( p = knl_usb_next_desc(id, end); p != NULL && p[1] != USB_DT_INTERFACE;
	      p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_CS_INTERFACE && p[2] == AS_GENERAL && p[0] >= 7 ) {
			pcm = ( GET16(p + 5) == 0x0001 );
		}
		if ( p[1] == USB_DT_CS_INTERFACE && p[2] == AS_FORMAT_TYPE && p[0] >= 8
		  && p[3] == 1 ) {
			INT	k, n = p[7];

			a.channels = p[4];
			a.subframe = p[5];
			a.bits     = p[6];
			if ( n == 0 && p[0] >= 14 ) {
				a.cont = TRUE;
				a.rmin = GET24(p + 8);
				a.rmax = GET24(p + 11);
			} else {
				for ( k = 0; k < n && k < UAC_RATES && 8 + 3 * k + 3 <= p[0]; k++ ) {
					a.rates[a.nrates++] = GET24(p + 8 + 3 * k);
				}
			}
		}
		if ( p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == USB_EP_ISOC
		  && !have_ep ) {
			knl_usb_ep_from_desc(&a.ep, u->dev, p, NULL);
			in = ( (p[2] & 0x80) != 0 );
			have_ep = TRUE;
		}
		if ( p[1] == USB_DT_CS_ENDPOINT && p[2] == EP_GENERAL && p[0] >= 3 ) {
			a.freq_ctl = ( (p[3] & 1) != 0 );
		}
	}
	if ( !pcm || !have_ep || a.channels == 0 || a.ep.size == 0 ) {
		return;
	}
	dir = in ? &u->cap : &u->play;
	if ( dir->nalt >= UAC_ALTS || ( dir->as_if >= 0 && dir->as_if != id[2] ) ) {
		return;
	}
	dir->as_if = id[2];
	dir->alt[dir->nalt++] = a;
}

/* The feature unit fed by terminal term, with a volume control */
LOCAL void find_volume( UACDIR *dir, UB *id, UB *end, INT term )
{
	UB	*p;

	for ( p = knl_usb_next_desc(id, end); p != NULL && p[1] != USB_DT_INTERFACE;
	      p = knl_usb_next_desc(p, end) ) {
		INT	cs, ch;

		if ( p[1] != USB_DT_CS_INTERFACE || p[2] != AC_FEATURE_UNIT || p[0] < 7 ) {
			continue;
		}
		if ( term != 0 && p[4] != term ) {
			continue;
		}
		cs = p[5];
		for ( ch = 0; ch < 2 && 6 + (ch + 1) * cs <= p[0] - 1; ch++ ) {
			if ( cs > 0 && (p[6 + ch * cs] & 0x02) != 0 ) {
				dir->fu_id = p[3];
				dir->fu_ch = ch;
				return;
			}
		}
	}
}

LOCAL void parse_control( UAC *u, UB *id, UB *end )
{
	UB	*p;
	INT	mic = 0, usb = 0;

	u->ac_if = id[2];
	for ( p = knl_usb_next_desc(id, end); p != NULL && p[1] != USB_DT_INTERFACE;
	      p = knl_usb_next_desc(p, end) ) {
		if ( p[1] != USB_DT_CS_INTERFACE ) {
			continue;
		}
		if ( p[2] == AC_HEADER && p[0] >= 8 ) {
			u->bcd = GET16(p + 3);
		}
		if ( p[2] == AC_INPUT_TERMINAL && p[0] >= 12 ) {
			if ( (GET16(p + 4) & 0xff00) == 0x0200 && mic == 0 ) mic = p[3];
			if ( GET16(p + 4) == TERM_USB_STREAMING && usb == 0 ) usb = p[3];
		}
	}
	if ( mic != 0 ) find_volume(&u->cap, id, end, mic);
	if ( usb != 0 ) find_volume(&u->play, id, end, usb);
}

/* ---------------------------------------------------------------- controls */

LOCAL ER fu_volume( UAC *u, UACDIR *dir, UB req, H *v )
{
	UB	b[2];
	INT	n = 0;
	ER	er;
	UH	wv = (UH)((FU_VOLUME << 8) | dir->fu_ch);
	UH	wi = (UH)((dir->fu_id << 8) | u->ac_if);

	if ( req == UAC_SET_CUR ) {
		PUT16(b, (UH)*v);
		return knl_usb_control(u->dev, USB_RT_CLASS | USB_RT_INTERFACE,
				       UAC_SET_CUR, wv, wi, b, 2, NULL);
	}
	er = knl_usb_control(u->dev, USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE,
			     req, wv, wi, b, 2, &n);
	if ( er >= E_OK && n >= 2 ) {
		*v = (H)GET16(b);
	}

	return er;
}

/*
 * The level 0 .. 0xFFFF, or -1 when that direction has no volume. The
 * device's range may have 0xFFFF steps, so the products are unsigned.
 */
LOCAL INT uac_get_volume( UAC *u, BOOL out )
{
	UACDIR	*d;
	H	v = 0;
	INT	cur;

	if ( u == NULL ) {
		return -1;
	}
	d = out ? &u->play : &u->cap;
	if ( d->fu_id == 0 || d->vol_max <= d->vol_min ) {
		return -1;
	}
	if ( fu_volume(u, d, UAC_GET_CUR, &v) >= E_OK ) {
		d->vol_cur = v;
	}
	cur = d->vol_cur;
	if ( cur < d->vol_min ) cur = d->vol_min;
	if ( cur > d->vol_max ) cur = d->vol_max;

	return (INT)((UW)(cur - d->vol_min) * 0xffff / (UW)((INT)d->vol_max - d->vol_min));
}

LOCAL ER uac_set_volume( UAC *u, BOOL out, INT level )
{
	UACDIR	*d;
	H	v;

	if ( u == NULL ) {
		return E_NOSPT;
	}
	d = out ? &u->play : &u->cap;
	if ( d->fu_id == 0 || d->vol_max <= d->vol_min ) {
		return E_NOSPT;
	}
	if ( level < 0 ) level = 0;
	if ( level > 0xffff ) level = 0xffff;
	v = (H)(d->vol_min + (INT)((UW)((INT)d->vol_max - d->vol_min) * (UW)level / 0xffff));
	if ( fu_volume(u, d, UAC_SET_CUR, &v) < E_OK ) {
		return E_IO;
	}
	d->vol_cur = v;

	return E_OK;
}

LOCAL void load_volume( UAC *u, UACDIR *d )
{
	H	v = 0;

	if ( d->fu_id == 0 ) {
		return;
	}
	if ( fu_volume(u, d, UAC_GET_MIN, &v) >= E_OK ) d->vol_min = v;
	if ( fu_volume(u, d, UAC_GET_MAX, &v) >= E_OK ) d->vol_max = v;
	if ( fu_volume(u, d, UAC_GET_CUR, &v) >= E_OK ) d->vol_cur = v;
}

/* ---------------------------------------------------------------- attach */

EXPORT BOOL knl_uac_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( dev->cfg == NULL ) {
		return FALSE;
	}
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[5] == USB_CLASS_AUDIO
		  && p[6] == AUDIO_SC_STREAMING ) {
			return TRUE;
		}
	}

	return FALSE;
}

EXPORT ER knl_uac_attach( USBDEV *dev )
{
	UAC	*u = NULL;
	UB	*p, *end = dev->cfg + dev->cfglen;
	INT	i;

	for ( i = 0; i < UAC_MAX; i++ ) {
		if ( !uac_pool[i].used ) {
			u = &uac_pool[i];
			break;
		}
	}
	if ( u == NULL ) {
		return E_LIMIT;
	}
	knl_memset(u, 0, sizeof(*u));
	u->dev = dev;
	u->ac_if = -1;
	u->cap.as_if = u->play.as_if = -1;
	u->cap.cur = u->play.cur = -1;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] != USB_DT_INTERFACE || p[0] < 9 || p[5] != USB_CLASS_AUDIO ) {
			continue;
		}
		if ( p[6] == AUDIO_SC_CONTROL && u->ac_if < 0 ) {
			parse_control(u, p, end);
		} else if ( p[6] == AUDIO_SC_STREAMING && p[3] != 0 ) {
			parse_streaming(u, p, end);
		}
	}
	if ( u->cap.nalt == 0 && u->play.nalt == 0 ) {
		return E_NOSPT;			/* no PCM setting */
	}
	load_volume(u, &u->cap);
	load_volume(u, &u->play);
	u->used = TRUE;
	dev->audio = u;
	uac_changed = TRUE;
	USB_LOG("usb: audio %x.%02x: %d recording, %d playback settings\n",
		u->bcd >> 8, u->bcd & 0xff, u->cap.nalt, u->play.nalt);

	return E_OK;
}

/*
 * talk: put the interface back on its idle setting. Not when the device
 * is gone, where the request would only run into its timeout.
 */
LOCAL void stop_dir( UAC *u, UACDIR *d, BOOL talk )
{
	if ( u == NULL || d->cur < 0 ) {
		return;
	}
	if ( d->pipe != NULL ) {
		u->dev->hc->ops->stream_close(u->dev->hc, d->pipe);
	}
	d->pipe = NULL;
	d->cur = -1;
	if ( talk ) {
		knl_usb_quiesce(u->dev, d->as_if);
	}
}

/* The device is gone: its streams close without a word to it */
EXPORT void knl_uac_detach( USBDEV *dev )
{
	UAC	*u = (UAC *)dev->audio;

	if ( u == NULL || !u->used ) {
		return;
	}
	stop_dir(u, &u->cap, FALSE);
	stop_dir(u, &u->play, FALSE);
	u->used = FALSE;
	dev->audio = NULL;
	uac_changed = TRUE;
}

/* Outside the manager's lock: whether sound has somewhere to go now */
EXPORT void knl_uac_notify( void )
{
	BOOL	any = FALSE;
	INT	i;

	if ( !uac_changed ) {
		return;
	}
	uac_changed = FALSE;
	for ( i = 0; i < UAC_MAX; i++ ) {
		if ( uac_pool[i].used ) {
			any = TRUE;
		}
	}
	knl_obdev_media((CONST UB *)SND_DEVNM, any);
}

LOCAL UAC *uac_capture( void )
{
	INT	i;

	for ( i = 0; i < UAC_MAX; i++ ) {
		if ( uac_pool[i].used && uac_pool[i].cap.nalt > 0 ) {
			return &uac_pool[i];
		}
	}

	return NULL;
}

LOCAL UAC *uac_playback( void )
{
	INT	i;

	for ( i = 0; i < UAC_MAX; i++ ) {
		if ( uac_pool[i].used && uac_pool[i].play.nalt > 0 ) {
			return &uac_pool[i];
		}
	}

	return NULL;
}

/* ---------------------------------------------------------------- streams */

/*
 * The packet period in microseconds: 2^(bInterval-1) microframes at high
 * speed, bInterval frames otherwise; audio uses 1ms or less, and more
 * than 32ms is taken as 32ms.
 */
LOCAL UW period_us( UAC *u, CONST UACALT *a )
{
	INT	b = ( a->ep.interval < 1 ) ? 1 : a->ep.interval;

	if ( u->dev->speed >= USB_SPEED_HIGH ) {
		return ( b > 9 ) ? 32000 : 125UL << (b - 1);
	}

	return ( b > 32 ) ? 32000 : 1000UL * (UW)b;
}

/* A 16 bit setting of d for rate, and the interface switched to it */
LOCAL ER pick_setting( UAC *u, UACDIR *d, UW rate, INT *pick, UW *p_rate )
{
	INT	i, k = -1;
	UW	r = 0;
	UB	f[3];
	UACALT	*a;
	ER	er;

	for ( i = 0; i < d->nalt && k < 0; i++ ) {
		if ( d->alt[i].subframe == 2 && rate_ok(&d->alt[i], rate) ) {
			k = i;
			r = rate;
		}
	}
	for ( i = 0; i < d->nalt; i++ ) {
		UW	ra = rate_above(&d->alt[i], rate);

		if ( k < 0 && d->alt[i].subframe == 2 && ra != 0 && ( r == 0 || ra < r ) ) {
			k = i;
			r = ra;
		}
	}
	if ( k < 0 ) {
		for ( i = 0; i < d->nalt; i++ ) {
			if ( d->alt[i].subframe == 2 && rate_highest(&d->alt[i]) > r ) {
				k = i;
				r = rate_highest(&d->alt[i]);
			}
		}
	}
	if ( k < 0 ) {
		return E_NOSPT;
	}
	a = &d->alt[k];
	er = knl_usb_set_interface(u->dev, d->as_if, a->alt);
	if ( er < E_OK ) {
		return er;
	}
	if ( a->freq_ctl || a->cont || a->nrates > 1 ) {
		f[0] = (UB)r;
		f[1] = (UB)(r >> 8);
		f[2] = (UB)(r >> 16);
		knl_usb_control(u->dev, USB_RT_CLASS | USB_RT_ENDPOINT, UAC_SET_CUR,
				EP_SAMPLING_FREQ << 8, a->ep.addr, f, 3, NULL);
	}
	*pick = k;
	*p_rate = r;

	return E_OK;
}

/* A recorded packet: frames of 16 bit samples */
LOCAL void cap_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	UAC	*u = (UAC *)ctx;
	UB	buf[1024];
	INT	fb;

	if ( u->cap.cur < 0 || err != 0 || len <= 0 ) {
		return;
	}
	/* a packet differs from the nominal count by one sample at most */
	if ( len > u->cap.pkt_max ) {
		len = u->cap.pkt_max;
	}
	if ( len > (INT)sizeof(buf) ) {
		len = sizeof(buf);
	}
	knl_dmabuf_read(b, off, buf, len);
	fb = cap_fmt.channels * 2;
	if ( fb > 0 ) {
		knl_snd_capture(buf, len / fb, cap_fmt.channels);
	}
}

/*
 * A packet to play: the nominal number of frames for its period, carried
 * over in 1/200000 of a frame. 0 when no channel has the samples yet.
 */
LOCAL INT out_fill( void *ctx, UB *dst, INT max )
{
	INT	fb = out_fmt.channels * 2, frames;
	UW	us = out_fmt.period_us, a;

	if ( fb <= 0 ) {
		return 0;
	}
	if ( us == 0 ) us = 1000;
	if ( us > PERIOD_MAX ) us = PERIOD_MAX;
	a = pkt_acc + out_fmt.rate / 5 * us;
	frames = (INT)(a / 200000);
	if ( frames * fb > max ) {
		frames = max / fb;
	}
	if ( frames < 1 ) {
		frames = 1;
	}
	if ( knl_snd_mix(dst, frames, out_fmt.rate, out_fmt.channels, TRUE) == 0 ) {
		return 0;
	}
	pkt_acc = ( a >= (UW)frames * 200000 ) ? a - (UW)frames * 200000 : 0;
	if ( pkt_acc >= 200000 ) {
		pkt_acc = 0;			/* a packet cut to max */
	}

	return frames * fb;
}

LOCAL ER uac_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	UACDIR	*d;
	UACALT	*a;
	INT	k;
	UW	r, n;
	ER	er;

	cap_uac = uac_capture();
	if ( cap_uac == NULL ) {
		return E_NOSPT;
	}
	d = &cap_uac->cap;
	if ( d->cur >= 0 ) {
		return E_BUSY;
	}
	er = pick_setting(cap_uac, d, rate, &k, &r);
	if ( er < E_OK ) {
		cap_uac = NULL;
		return er;
	}
	a = &d->alt[k];
	d->rate = r;
	n = (r / 5 * period_us(cap_uac, a) + 199999) / 200000;	/* rounded up */
	d->pkt_max = (INT)(n + 1) * a->channels * a->subframe;
	if ( d->pkt_max > a->ep.esit ) {
		d->pkt_max = a->ep.esit;
	}
	cap_fmt.rate = r;
	cap_fmt.channels = a->channels;
	cap_fmt.subframe = a->subframe;
	cap_fmt.period_us = period_us(cap_uac, a);
	cap_fmt.max_bytes = a->ep.esit;
	d->cur = k;
	er = cap_uac->dev->hc->ops->stream_open(cap_uac->dev->hc, cap_uac->dev,
			&a->ep, a->ep.esit, cap_xfer, cap_uac, &d->pipe);
	if ( er < E_OK ) {
		d->cur = -1;
		knl_usb_set_interface(cap_uac->dev, d->as_if, 0);
		cap_uac = NULL;
		return er;
	}
	*dev_rate = r;
	*channels = a->channels;

	return E_OK;
}

LOCAL void uac_rec_stop( void )
{
	if ( cap_uac != NULL ) {
		stop_dir(cap_uac, &cap_uac->cap, TRUE);
	}
	cap_uac = NULL;
}

LOCAL BOOL uac_rec_running( void )
{
	return ( cap_uac != NULL && cap_uac->used && cap_uac->cap.cur >= 0 );
}

LOCAL ER uac_play_start( UW *rate, INT *channels )
{
	UACDIR	*d;
	UACALT	*a;
	INT	k;
	UW	r;
	ER	er;

	out_uac = uac_playback();
	if ( out_uac == NULL ) {
		return E_NOSPT;
	}
	d = &out_uac->play;
	if ( d->cur >= 0 ) {
		return E_BUSY;
	}
	er = pick_setting(out_uac, d, OUT_RATE, &k, &r);
	if ( er < E_OK ) {
		out_uac = NULL;
		return er;
	}
	a = &d->alt[k];
	/* the packets are sized from these before the first fill */
	out_fmt.rate = r;
	out_fmt.channels = a->channels;
	out_fmt.subframe = a->subframe;
	out_fmt.period_us = period_us(out_uac, a);
	out_fmt.max_bytes = a->ep.esit;
	pkt_acc = 0;
	d->rate = r;
	d->cur = k;
	*rate = r;
	*channels = a->channels;
	er = out_uac->dev->hc->ops->stream_open_out(out_uac->dev->hc, out_uac->dev,
			&a->ep, out_fill, out_uac, &d->pipe);
	if ( er < E_OK ) {
		d->cur = -1;
		knl_usb_set_interface(out_uac->dev, d->as_if, 0);
		out_uac = NULL;
		return er;
	}
	USB_LOG("usb: audio plays at %d Hz, %d channels, %d us a packet\n",
		(INT)r, a->channels, (INT)out_fmt.period_us);

	return E_OK;
}

LOCAL void uac_play_stop( void )
{
	if ( out_uac != NULL ) {
		stop_dir(out_uac, &out_uac->play, TRUE);
	}
	out_uac = NULL;
}

LOCAL BOOL uac_play_running( void )
{
	return ( out_uac != NULL && out_uac->used && out_uac->play.cur >= 0 );
}

LOCAL BOOL uac_can_play( void )
{
	return ( uac_playback() != NULL );
}

LOCAL BOOL uac_can_record( void )
{
	return ( uac_capture() != NULL );
}

LOCAL void uac_poll( void )
{
	/* the streams run from the controller's own service */
}

LOCAL BOOL out_level( INT dn )
{
	return ( dn == DN_SDVOLMAIN || dn == DN_SDVOLPCM || dn == DN_SDGAINOUT );
}

LOCAL BOOL in_level( INT dn )
{
	return ( dn == DN_SDVOLMIC || dn == DN_SDGAININ );
}

LOCAL ER uac_get_level( INT dn, SDVol *v )
{
	BOOL	out = out_level(dn);
	INT	l;

	if ( !out && !in_level(dn) ) {
		return E_NOSPT;
	}
	l = uac_get_volume(out ? uac_playback() : uac_capture(), out);
	if ( l < 0 ) {
		return E_NOSPT;
	}
	v->lv = v->rv = (UH)l;

	return E_OK;
}

LOCAL ER uac_set_level( INT dn, CONST SDVol *v )
{
	BOOL	out = out_level(dn);

	if ( !out && !in_level(dn) ) {
		return E_NOSPT;
	}

	return uac_set_volume(out ? uac_playback() : uac_capture(), out, v->lv);
}

EXPORT CONST SNDHW knl_uac_hw = {
	SND_BE_USB,
	uac_can_play,
	uac_can_record,
	uac_play_start,
	uac_play_stop,
	uac_play_running,
	uac_rec_start,
	uac_rec_stop,
	uac_rec_running,
	uac_poll,
	uac_get_level,
	uac_set_level
};
