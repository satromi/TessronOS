/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	snd_hda.c (x64 PC)
 *	Intel High Definition Audio: the controller on the PCI bus (class
 *	04.03) and the codecs on its link, as a backend of the sound device
 *	(sndhw.h, design 10.15).
 *
 *	The controller: reset, the command ring (CORB) and the response ring
 *	(RIRB) through which the codecs are spoken to, one output stream and
 *	one input stream, each a ring of NBLK blocks described by a buffer
 *	descriptor list that the controller goes round on its own. Nothing
 *	waits for its interrupts: the sound device's poll (every few ms
 *	while a stream runs) reads how far the controller has come (LPIB)
 *	and refills the blocks it has played, or hands on the ones it has
 *	recorded.
 *
 *	The codecs: walked as the specification lays them out, the same way
 *	for any maker's: the root node, its audio function group, its
 *	widgets (converters, mixers, selectors, pins), their connection
 *	lists and their pins' default configuration. Each output pin that is
 *	wired (a speaker, headphones, line out) is followed back through the
 *	connections to an output converter, and every widget on the way is
 *	powered, unmuted and selected; all the converters found take the one
 *	output stream, so that speaker and headphones play together. The
 *	input pins are reached the other way, by the shortest path from an
 *	input converter: the built-in microphone (fixed in the machine, as
 *	a laptop's digital microphone is) and the jack's (a headset's); else
 *	line in. A mixer before the converter takes only the input on the
 *	path, the others muted; a microphone in a jack is given its bias.
 *	While recording, the jack's path is taken when its pin senses that
 *	something is plugged in, the built-in one when not: the pin tells
 *	of it by an unsolicited response (tagged, in the RIRB among the
 *	answers to the verbs), or, when it cannot, is looked at every half
 *	second. The first recordings note in the console the path, its
 *	gains, the jack and the peaks of a few blocks. What a codec of a particular machine wants besides
 *	(pin configurations of its own, a GPIO for the speaker's amplifier,
 *	processing coefficients) has its place in hda_quirks(), to be filled
 *	from the machine's own report (HW.TXT, the DSDT).
 *
 *	Levels: DN_SDVOLMAIN and its kin set the output amplifiers on the
 *	paths (the converters' and the pins'), DN_SDVOLMIC and DN_SDGAININ
 *	the input ones, within the steps each amplifier has.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "../sndhw.h"

IMPORT UW	knl_usb_ms( void );

/* ---------------------------------------------------------------- the controller's registers */

#define GCAP		0x00
#define VMIN		0x02
#define VMAJ		0x03
#define GCTL		0x08
#define STATESTS	0x0e
#define INTCTL		0x20
#define CORBLBASE	0x40
#define CORBUBASE	0x44
#define CORBWP		0x48
#define CORBRP		0x4a
#define CORBCTL		0x4c
#define CORBSIZE	0x4e
#define RIRBLBASE	0x50
#define RIRBUBASE	0x54
#define RIRBWP		0x58
#define RINTCNT		0x5a
#define RIRBCTL		0x5c
#define RIRBSTS		0x5d
#define RIRBSIZE	0x5e
#define DPLBASE		0x70
#define DPUBASE		0x74

#define SD_BASE		0x80
#define SD_SIZE		0x20
#define SD_CTL		0x00		/* three bytes: SRST, RUN, ..., the stream's number */
#define SD_STS		0x03
#define SD_LPIB		0x04
#define SD_CBL		0x08
#define SD_LVI		0x0c
#define SD_FMT		0x12
#define SD_BDPL		0x18
#define SD_BDPU		0x1c

#define GCTL_CRST	0x01
#define GCTL_UNSOL	0x100		/* unsolicited responses taken into the RIRB */
#define SDCTL_SRST	0x01
#define SDCTL_RUN	0x02

/* ---------------------------------------------------------------- the codecs' verbs */

#define V_GET_PARAM	0xf00
#define V_GET_CONN	0xf02
#define V_SET_SELECT	0x701
#define V_SET_POWER	0x705
#define V_SET_STREAM	0x706
#define V_SET_PINCTL	0x707
#define V_SET_UNSOL	0x708
#define V_GET_PINSENSE	0xf09
#define V_SET_EAPD	0x70c
#define V_GET_CONFIG	0xf1c
#define V_SET_FORMAT	0x2		/* four bit verbs: sixteen bits of payload */
#define V_SET_AMP	0x3

#define P_VENDOR	0x00
#define P_NODES		0x04
#define P_FGTYPE	0x05
#define P_WCAPS		0x09
#define P_PINCAPS	0x0c
#define P_INAMP		0x0d
#define P_CONNLEN	0x0e
#define P_OUTAMP	0x12

#define W_OUT		0x0
#define W_IN		0x1
#define W_MIXER		0x2
#define W_SELECT	0x3
#define W_PIN		0x4

#define WC_INAMP	0x002
#define WC_OUTAMP	0x004
#define WC_AMPOVR	0x008
#define WC_UNSOL	0x080
#define WC_DIGITAL	0x200
#define WC_CONN		0x100
#define WC_POWER	0x400

#define PC_PRESENCE	0x004
#define PC_HP		0x008
#define PC_OUT		0x010
#define PC_IN		0x020
#define PC_EAPD		0x10000

#define PIN_OUT		0x40
#define PIN_IN		0x20
#define PIN_HP		0x80
#define PIN_VREF50	0x01
#define PIN_VREF80	0x04
#define PIN_VREF100	0x05
#define PC_VREF50	0x0200			/* the pin's capabilities: the bias it gives a microphone */
#define PC_VREF80	0x1000
#define PC_VREF100	0x2000

/* the pins' port connectivity (the default configuration's top two bits) */
#define PORT_JACK	0
#define PORT_NONE	1
#define PORT_FIXED	2
#define PORT_BOTH	3

#define TAG_JACK	0x01			/* the unsolicited response's tag of the microphone jack */

/* the pins' default devices */
#define DEV_LINEOUT	0x0
#define DEV_SPEAKER	0x1
#define DEV_HP		0x2
#define DEV_LINEIN	0x8
#define DEV_MIC		0xa

/* ---------------------------------------------------------------- sizes */

#define NBLK		8
#define BLK_FRAMES	256
#define FRAME_BYTES	4			/* 16 bits, two channels */
#define BLK_BYTES	( BLK_FRAMES * FRAME_BYTES )
#define RING_BYTES	( NBLK * BLK_BYTES )
#define RATE		48000
#define FMT_48K_16_2	0x0011			/* 48kHz, 16 bits, two channels */

#define MAXW		96			/* widgets of a function group looked at */
#define MAXCONN		16
#define MAXPATH		8
#define MAXOUT		4

#define CORB_N		256
#define RIRB_N		256

typedef struct {
	UB	type;
	UW	caps;
	UW	pincaps;
	UW	config;
	UW	ampin, ampout;			/* amplifier capabilities */
	INT	nconn;
	UH	conn[MAXCONN];
} WIDGET;

typedef struct {
	INT	n;
	UH	nid[MAXPATH];			/* from the pin (or converter) end */
	UB	sel[MAXPATH];			/* the connection taken at each */
} PATH;

LOCAL BOOL	hda_ok = FALSE;
LOCAL UBINT	base;
LOCAL UINT	cad;				/* the codec's address */
LOCAL UH	afg;				/* its audio function group */
LOCAL UW	codec_vendor;
LOCAL UH	wstart, wcount;
LOCAL WIDGET	*wid;
LOCAL PATH	outp[MAXOUT];
LOCAL INT	nout = 0;
/*
 * The input paths: to the built-in microphone (or the one input there
 * is) and to the jack's; while recording, the jack's is taken when
 * something is plugged in, the built-in one otherwise
 */
#define IN_INT		0
#define IN_JACK		1
LOCAL PATH	inp[2];
LOCAL BOOL	in_have[2];
LOCAL INT	in_cur = IN_INT;
LOCAL BOOL	has_in = FALSE;
LOCAL BOOL	jack_unsol = FALSE;		/* the jack tells of plugging itself */
LOCAL BOOL	jack_event = FALSE;		/* it has told, not looked at yet */
LOCAL BOOL	jack_in = FALSE;		/* something is in the jack */
LOCAL UW	jack_ms;			/* when the jack was last looked at */
LOCAL INT	rec_logs = 0;			/* recordings whose first samples were noted */
LOCAL INT	peak_n;				/* how many blocks' peaks are noted */
LOCAL UH	peak_l[8], peak_r[8];
LOCAL INT	iss, oss;			/* input and output stream descriptors */
LOCAL UW	*corb;
LOCAL UD	*rirb;
LOCAL UH	rirb_rp;
LOCAL UD	page0_pa, bdl_pa, out_pa, in_pa;
LOCAL UB	*bdl, *out_ring, *in_ring;
LOCAL BOOL	playing = FALSE, recording = FALSE;
LOCAL INT	out_done;			/* the oldest block played and not refilled */
LOCAL INT	in_done;			/* the next block to hand on */
LOCAL UH	vol_out = 0xc000, vol_in = 0xc000;
LOCAL UW	out_blocks, in_blocks;
LOCAL UB	out_name[SND_NAME_MAX];

LOCAL UB  r8( UINT o )  { return *(_UB *)( base + o ); }
LOCAL UH  r16( UINT o ) { return *(_UH *)( base + o ); }
LOCAL UW  r32( UINT o ) { return *(_UW *)( base + o ); }
LOCAL void w8( UINT o, UB v )  { *(_UB *)( base + o ) = v; }
LOCAL void w16( UINT o, UH v ) { *(_UH *)( base + o ) = v; }
LOCAL void w32( UINT o, UW v ) { *(_UW *)( base + o ) = v; }

LOCAL UINT sd_off( INT out )
{
	return SD_BASE + SD_SIZE * (UINT)( out ? iss : 0 );
}

/* Wait up to ms for (reg & mask) == want */
LOCAL BOOL wait8( UINT o, UB mask, UB want, INT ms )
{
	INT	i;

	for ( i = 0; i < ms * 10; i++ ) {
		if ( ( r8(o) & mask ) == want ) return TRUE;
		WaitUsec(100);
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the command rings */

/*
 * The next entry of the response ring, if the controller has written
 * one: TRUE with the response. An unsolicited one (the codec telling
 * of its own accord, bit 4 of the entry's upper half) is noted by its
 * tag and not handed back.
 */
LOCAL BOOL rirb_next( UW *resp, BOOL *unsol )
{
	UD	e;

	if ( ( r16(RIRBWP) & 0xff ) == rirb_rp ) {
		return FALSE;
	}
	rirb_rp = ( rirb_rp + 1 ) % RIRB_N;
	e = rirb[rirb_rp];
	w8(RIRBSTS, 0x05);
	*resp = (UW)e;
	*unsol = ( ( e >> 32 ) & 0x10 ) != 0;
	if ( *unsol && ( (UW)e >> 26 ) == TAG_JACK ) {
		jack_event = TRUE;
	}
	return TRUE;
}

/* The unsolicited responses that came while no verb waited */
LOCAL void rirb_drain( void )
{
	UW	resp;
	BOOL	unsol;

	while ( rirb_next(&resp, &unsol) ) ;
}

/* One verb to the codec, its answer; ~0 when it does not answer */
LOCAL UW cmd( UINT nid, UW verb_payload )
{
	UH	wp = ( r16(CORBWP) + 1 ) % CORB_N;
	INT	i;
	UW	resp;
	BOOL	unsol;

	rirb_drain();
	corb[wp] = ( (UW)cad << 28 ) | ( (UW)nid << 20 ) | verb_payload;
	DeviceBarrier();
	w16(CORBWP, wp);
	for ( i = 0; i < 2000; i++ ) {
		while ( rirb_next(&resp, &unsol) ) {
			if ( !unsol ) return resp;
		}
		WaitUsec(10);
	}
	return ~0U;
}

LOCAL UW verb( UINT nid, UINT v, UINT payload )
{
	return cmd(nid, ( (UW)v << 8 ) | ( payload & 0xff ));
}

LOCAL UW verb4( UINT nid, UINT v, UINT payload )
{
	return cmd(nid, ( (UW)v << 16 ) | ( payload & 0xffff ));
}

LOCAL UW param( UINT nid, UINT p )
{
	return verb(nid, V_GET_PARAM, p);
}

/* ---------------------------------------------------------------- the codec's widgets */

LOCAL WIDGET *w_of( UINT nid )
{
	return ( nid >= wstart && nid < wstart + wcount && nid - wstart < MAXW ) ? &wid[nid - wstart] : NULL;
}

LOCAL void read_widgets( void )
{
	UINT	nid, i;

	for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW; nid++ ) {
		WIDGET	*w = w_of(nid);
		UW	cl;

		w->caps = param(nid, P_WCAPS);
		w->type = (UB)( ( w->caps >> 20 ) & 0xf );
		w->ampout = ( w->caps & WC_AMPOVR ) ? param(nid, P_OUTAMP) : param(afg, P_OUTAMP);
		w->ampin = ( w->caps & WC_AMPOVR ) ? param(nid, P_INAMP) : param(afg, P_INAMP);
		if ( w->type == W_PIN ) {
			w->pincaps = param(nid, P_PINCAPS);
			w->config = verb(nid, V_GET_CONFIG, 0);
		}
		w->nconn = 0;
		if ( w->caps & WC_CONN ) {
			cl = param(nid, P_CONNLEN);
			if ( cl & 0x80 ) {
				/* long form: two entries a response */
				for ( i = 0; i < ( cl & 0x7f ) && w->nconn < MAXCONN; i += 2 ) {
					UW	r = verb(nid, V_GET_CONN, i);

					w->conn[w->nconn++] = (UH)( r & 0x7fff );
					if ( i + 1 < ( cl & 0x7f ) && w->nconn < MAXCONN ) w->conn[w->nconn++] = (UH)( ( r >> 16 ) & 0x7fff );
				}
			} else {
				for ( i = 0; i < ( cl & 0x7f ) && w->nconn < MAXCONN; i += 4 ) {
					UW	r = verb(nid, V_GET_CONN, i);
					UINT	k;

					for ( k = 0; k < 4 && i + k < ( cl & 0x7f ) && w->nconn < MAXCONN; k++ ) {
						w->conn[w->nconn++] = (UH)( ( r >> ( 8 * k ) ) & 0x7f );
					}
				}
			}
		}
	}
}

/* From nid back through what feeds it to a converter of the type wanted; depth first */
LOCAL BOOL find_path( UINT nid, UB want, PATH *p, INT depth )
{
	WIDGET	*w = w_of(nid);
	INT	i;

	if ( w == NULL || depth >= MAXPATH ) {
		return FALSE;
	}
	for ( i = 0; i < p->n; i++ ) {
		if ( p->nid[i] == nid ) return FALSE;	/* round in a circle */
	}
	p->nid[p->n] = (UH)nid;
	p->sel[p->n] = 0;
	p->n++;
	if ( w->type == want ) {
		return TRUE;
	}
	for ( i = 0; i < w->nconn; i++ ) {
		WIDGET	*c = w_of(w->conn[i]);

		if ( c == NULL ) continue;
		if ( c->type != want && c->type != W_MIXER && c->type != W_SELECT ) continue;
		p->sel[p->n - 1] = (UB)i;
		if ( find_path(w->conn[i], want, p, depth + 1) ) {
			return TRUE;
		}
	}
	p->n--;
	return FALSE;
}

/*
 * The input path the other way round: from an input converter back
 * through mixers and selectors to the pin wanted, in no more than
 * limit widgets
 */
LOCAL BOOL find_in_path( UINT nid, UINT pin, PATH *p, INT limit )
{
	WIDGET	*w = w_of(nid);
	INT	i;

	if ( w == NULL || p->n >= limit ) {
		return FALSE;
	}
	for ( i = 0; i < p->n; i++ ) {
		if ( p->nid[i] == nid ) return FALSE;
	}
	p->nid[p->n] = (UH)nid;
	p->sel[p->n] = 0;
	p->n++;
	if ( nid == pin ) {
		return TRUE;
	}
	if ( w->type != W_PIN ) {
		for ( i = 0; i < w->nconn; i++ ) {
			WIDGET	*c = w_of(w->conn[i]);

			if ( c == NULL || ( c->type != W_PIN && c->type != W_MIXER && c->type != W_SELECT ) ) continue;
			p->sel[p->n - 1] = (UB)i;
			if ( find_in_path(w->conn[i], pin, p, limit) ) {
				return TRUE;
			}
		}
	}
	p->n--;
	return FALSE;
}

/*
 * The shortest path from an input converter to the pin: the converter
 * prefer tried first at each length (so that both microphones share
 * one converter where they can, and switching between them is a matter
 * of the mixer's or selector's inputs), then the others in order
 */
LOCAL BOOL in_path_to( UINT pin, UINT prefer, PATH *p )
{
	INT	limit;
	UINT	adc;

	for ( limit = 2; limit <= MAXPATH; limit++ ) {
		p->n = 0;
		if ( prefer != 0 && find_in_path(prefer, pin, p, limit) ) {
			return TRUE;
		}
		for ( adc = wstart; adc < wstart + wcount && adc - wstart < MAXW; adc++ ) {
			if ( adc == prefer || w_of(adc)->type != W_IN ) continue;
			p->n = 0;
			if ( find_in_path(adc, pin, p, limit) ) {
				return TRUE;
			}
		}
	}
	p->n = 0;
	return FALSE;
}

/* An amplifier's gain for a level of 0..0xffff, within its steps (the level's middle at its 0 dB) */
LOCAL UINT gain_of( UW ampcaps, UH level )
{
	UINT	steps = ( ampcaps >> 8 ) & 0x7f, off = ampcaps & 0x7f, g;

	if ( steps == 0 ) return 0;
	if ( level >= 0x8000 ) {
		g = off + ( ( steps - off ) * ( level - 0x8000 ) + 0x4000 ) / 0x8000;
	} else {
		g = ( off * level + 0x4000 ) / 0x8000;
	}
	return ( g > steps ) ? steps : g;
}

/* Every amplifier on a path set: unmuted at the level, the inputs it takes on the way */
LOCAL void path_amps( PATH *p, UH level, BOOL out )
{
	INT	i;

	for ( i = 0; i < p->n; i++ ) {
		WIDGET	*w = w_of(p->nid[i]);
		UINT	mute = ( level == 0 ) ? 0x80 : 0;

		if ( w == NULL ) continue;
		if ( w->caps & WC_OUTAMP ) {
			verb4(p->nid[i], V_SET_AMP, 0x8000 | 0x3000 | mute | gain_of(w->ampout, out ? level : 0x8000));
		}
		if ( ( w->caps & WC_INAMP ) && !out && w->type == W_MIXER && i + 1 < p->n ) {
			INT	k;

			/*
			 * A mixer before an input converter takes one input
			 * only: the one on the path at the level, the others
			 * (the other pins, the loopback mixer) muted
			 */
			for ( k = 0; k < w->nconn; k++ ) {
				verb4(p->nid[i], V_SET_AMP, 0x4000 | 0x3000 | ( (UINT)k << 8 )
				      | ( ( k == p->sel[i] ) ? ( mute | gain_of(w->ampin, level) ) : 0x80 ));
			}
		} else if ( ( w->caps & WC_INAMP ) && ( w->nconn > 0 || ( !out && w->type == W_PIN ) ) ) {
			UINT	idx = ( w->type == W_MIXER || ( !out && w->type == W_SELECT ) ) ? p->sel[i] : 0;

			/* an input pin's own amplifier is the microphone's boost */
			verb4(p->nid[i], V_SET_AMP, 0x4000 | 0x3000 | ( idx << 8 ) | mute
			      | gain_of(w->ampin, out ? 0x8000 : level));
		}
	}
}

/*
 * The bias an analogue microphone in a jack needs (80% of the supply
 * where the pin gives it, else 50% or 100%); none for a built-in one,
 * a digital microphone or line in
 */
LOCAL UINT mic_bias( WIDGET *w )
{
	UINT	port = w->config >> 30;

	if ( ( ( w->config >> 20 ) & 0xf ) != DEV_MIC || port == PORT_FIXED || ( w->caps & WC_DIGITAL ) ) {
		return 0;
	}
	if ( w->pincaps & PC_VREF80 ) return PIN_VREF80;
	if ( w->pincaps & PC_VREF50 ) return PIN_VREF50;
	if ( w->pincaps & PC_VREF100 ) return PIN_VREF100;
	return 0;
}

/* A path made ready: powered, its selectors on the way, the pin's direction */
LOCAL void path_set( PATH *p, BOOL out )
{
	INT	i;

	for ( i = 0; i < p->n; i++ ) {
		WIDGET	*w = w_of(p->nid[i]);

		if ( w == NULL ) continue;
		if ( w->caps & WC_POWER ) verb(p->nid[i], V_SET_POWER, 0);
		if ( w->type != W_MIXER && w->nconn > 1 && i + 1 < p->n ) {
			verb(p->nid[i], V_SET_SELECT, p->sel[i]);
		}
		if ( w->type == W_PIN ) {
			UINT	ctl = out ? PIN_OUT : PIN_IN;

			if ( out && ( w->pincaps & PC_HP ) && ( ( w->config >> 20 ) & 0xf ) == DEV_HP ) ctl |= PIN_HP;
			if ( !out ) ctl |= mic_bias(w);
			verb(p->nid[i], V_SET_PINCTL, ctl);
			if ( w->pincaps & PC_EAPD ) verb(p->nid[i], V_SET_EAPD, 0x02);
		}
	}
	path_amps(p, out ? vol_out : vol_in, out);
}

/*
 * What a particular codec wants besides the specification's walk. The
 * codec's vendor and device are in codec_vendor (and in the console).
 * The Surface Go 2's Realtek ALC298 wants nothing more for its
 * speaker, its headphones and its two microphones: its built-in
 * digital microphone (pin 0x12) is an input pin like any other, reached
 * through the mixer before an input converter.
 */
LOCAL void hda_quirks( void )
{
}

LOCAL CONST char *dev_name( UINT d )
{
	switch ( d ) {
	case DEV_LINEOUT:	return "line out";
	case DEV_SPEAKER:	return "speaker";
	case DEV_HP:		return "headphones";
	case DEV_LINEIN:	return "line in";
	case DEV_MIC:		return "microphone";
	default:		return "other";
	}
}

/* A widget's connections in the console, after what is said of it */
LOCAL void log_widget( CONST char *what, UINT nid )
{
	WIDGET	*w = w_of(nid);
	UB	buf[MAXCONN * 5 + 1], *b = buf;
	INT	i;

	buf[0] = '\0';
	for ( i = 0; i < w->nconn; i++ ) {
		b += tm_sprintf(b, (UB *)" %d", w->conn[i]);
	}
	tm_printf((UB *)"TessronOS: hda: %s %d: caps %08x, amp in %08x, out %08x, takes%s\n",
		  what, nid, w->caps, w->ampin, w->ampout, ( w->nconn > 0 ) ? buf : (UB *)" nothing");
}

/*
 * What the input side is made of: the input converters, the mixers and
 * selectors before them, and the input pins' widget capabilities
 * (digital, telling of plugging by unsolicited responses)
 */
LOCAL void log_inputs( void )
{
	UINT	nid;
	INT	i;

	for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW; nid++ ) {
		WIDGET	*w = w_of(nid);

		if ( w->type != W_IN ) continue;
		log_widget("input converter", nid);
		for ( i = 0; i < w->nconn; i++ ) {
			WIDGET	*c = w_of(w->conn[i]);

			if ( c != NULL && ( c->type == W_MIXER || c->type == W_SELECT ) ) {
				log_widget(( c->type == W_MIXER ) ? "  mixer" : "  selector", w->conn[i]);
			}
		}
	}
	for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW; nid++ ) {
		WIDGET	*w = w_of(nid);

		if ( w->type == W_PIN && ( w->pincaps & PC_IN ) && ( w->config >> 30 ) != PORT_NONE ) {
			tm_printf((UB *)"TessronOS: hda: input pin %d: caps %08x (%s%s%s), amp in %08x\n", nid, w->caps,
				  ( w->caps & WC_DIGITAL ) ? "digital" : "analogue",
				  ( w->pincaps & PC_PRESENCE ) ? ", senses plugging" : "",
				  ( w->caps & WC_UNSOL ) ? ", tells of it" : "", w->ampin);
		}
	}
}

LOCAL void log_path( CONST char *what, PATH *p )
{
	UB	buf[MAXPATH * 6 + 1], *b = buf;
	INT	i;

	for ( i = 0; i < p->n; i++ ) {
		b += tm_sprintf(b, (UB *)( ( i == 0 ) ? "%d" : " > %d" ), p->nid[i]);
	}
	tm_printf((UB *)"TessronOS: hda: input path (%s): converter %s (pin)\n", what, buf);
}

/*
 * The input pins chosen and their paths made: the built-in microphone
 * (fixed in the machine) and the jack's (a microphone in a jack, a
 * headset's), else line in. The jack's is switched to only when the
 * pin can tell whether something is plugged in; an input alone is
 * always the one taken, whatever it is.
 */
LOCAL void in_setup( void )
{
	UINT	nid, dev, pin[2];
	INT	k;

	pin[IN_INT] = pin[IN_JACK] = 0;
	for ( dev = DEV_MIC; ; dev = DEV_LINEIN ) {
		for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW; nid++ ) {
			WIDGET	*w = w_of(nid);
			UINT	port = w->config >> 30;

			if ( w->type != W_PIN || ( w->pincaps & PC_IN ) == 0 || port == PORT_NONE
			  || ( ( w->config >> 20 ) & 0xf ) != dev ) continue;
			k = ( port == PORT_FIXED ) ? IN_INT : IN_JACK;
			if ( pin[k] == 0 ) pin[k] = nid;
		}
		if ( pin[IN_INT] != 0 || pin[IN_JACK] != 0 || dev == DEV_LINEIN ) break;
	}
	for ( k = 0; k < 2; k++ ) {
		in_have[k] = ( pin[k] != 0
			       && in_path_to(pin[k], ( k == IN_JACK && in_have[IN_INT] ) ? inp[IN_INT].nid[0] : 0, &inp[k]) );
		if ( pin[k] != 0 && !in_have[k] ) {
			tm_printf((UB *)"TessronOS: hda: no input converter reaches pin %d\n", pin[k]);
		}
	}
	if ( in_have[IN_INT] && in_have[IN_JACK] && ( w_of(pin[IN_JACK])->pincaps & PC_PRESENCE ) == 0 ) {
		tm_printf((UB *)"TessronOS: hda: pin %d cannot tell what is plugged in: pin %d records\n",
			  pin[IN_JACK], pin[IN_INT]);
		in_have[IN_JACK] = FALSE;
	}
	if ( in_have[IN_INT] ) {
		log_path("built in", &inp[IN_INT]);
	} else if ( in_have[IN_JACK] ) {
		inp[IN_INT] = inp[IN_JACK];		/* the jack's alone: always that one */
		in_have[IN_INT] = TRUE;
		in_have[IN_JACK] = FALSE;
		log_path("the only one", &inp[IN_INT]);
	}
	jack_unsol = in_have[IN_JACK] && ( w_of(pin[IN_JACK])->caps & WC_UNSOL ) != 0;
	if ( in_have[IN_JACK] ) {
		log_path(jack_unsol ? "jack, which tells of plugging" : "jack, looked at while recording", &inp[IN_JACK]);
	}
	has_in = in_have[IN_INT];
	in_cur = IN_INT;
}

LOCAL void walk_codec( void )
{
	UW	n = param(0, P_NODES);
	UINT	f, nid, rank;
	INT	i;

	codec_vendor = param(0, P_VENDOR);
	afg = 0;
	for ( f = ( n >> 16 ) & 0xff; f < ( ( n >> 16 ) & 0xff ) + ( n & 0xff ); f++ ) {
		if ( ( param(f, P_FGTYPE) & 0xff ) == 1 ) {
			afg = (UH)f;
			break;
		}
	}
	if ( afg == 0 ) {
		tm_printf((UB *)"TessronOS: hda: codec %d (%08x) has no audio function group\n", cad, codec_vendor);
		return;
	}
	verb(afg, V_SET_POWER, 0);
	n = param(afg, P_NODES);
	wstart = (UH)( ( n >> 16 ) & 0xff );
	wcount = (UH)( n & 0xff );
	read_widgets();
	for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW; nid++ ) {
		WIDGET	*w = w_of(nid);

		if ( w->type == W_PIN ) {
			tm_printf((UB *)"TessronOS: hda: pin %d: config %08x (%s%s), caps %08x, %d connections\n",
				  nid, w->config, dev_name(( w->config >> 20 ) & 0xf),
				  ( ( w->config >> 30 ) == 1 ) ? ", not wired" : "", w->pincaps, w->nconn);
		}
	}
	log_inputs();
	/* outputs: the speaker first, headphones, line out */
	nout = 0;
	for ( rank = 0; rank < 3 && nout < MAXOUT; rank++ ) {
		UINT	want = ( rank == 0 ) ? DEV_SPEAKER : ( rank == 1 ) ? DEV_HP : DEV_LINEOUT;

		for ( nid = wstart; nid < wstart + wcount && nid - wstart < MAXW && nout < MAXOUT; nid++ ) {
			WIDGET	*w = w_of(nid);

			if ( w->type != W_PIN || ( w->pincaps & PC_OUT ) == 0 || ( w->config >> 30 ) == 1
			  || ( ( w->config >> 20 ) & 0xf ) != want ) continue;
			outp[nout].n = 0;
			if ( find_path(nid, W_OUT, &outp[nout], 0) ) {
				if ( nout == 0 ) {
					tm_sprintf(out_name, (UB *)"HDA %s", dev_name(want));
				}
				nout++;
			}
		}
	}
	for ( i = 0; i < nout; i++ ) {
		tm_printf((UB *)"TessronOS: hda: output path %d: pin %d ... converter %d (%d widgets)\n", i,
			  outp[i].nid[0], outp[i].nid[outp[i].n - 1], outp[i].n);
	}
	in_setup();
	hda_quirks();
}

/* ---------------------------------------------------------------- the streams */

LOCAL void stream_reset( INT out )
{
	UINT	sd = sd_off(out);

	w8(sd + SD_CTL, 0);
	(void)wait8(sd + SD_CTL, SDCTL_RUN, 0, 10);
	w8(sd + SD_CTL, SDCTL_SRST);
	(void)wait8(sd + SD_CTL, SDCTL_SRST, SDCTL_SRST, 10);
	w8(sd + SD_CTL, 0);
	(void)wait8(sd + SD_CTL, SDCTL_SRST, 0, 10);
}

/* The ring's blocks in the stream's buffer descriptor list, and the stream set up (not run) */
LOCAL void stream_setup( INT out, UINT tag )
{
	UINT	sd = sd_off(out);
	UB	*l = bdl + ( out ? 0 : 2048 );
	UD	lpa = bdl_pa + ( out ? 0 : 2048 ), rpa = out ? out_pa : in_pa;
	INT	i;

	stream_reset(out);
	for ( i = 0; i < NBLK; i++ ) {
		UD	a = rpa + (UD)i * BLK_BYTES;

		*(UD *)( l + 16 * i ) = a;
		*(UW *)( l + 16 * i + 8 ) = BLK_BYTES;
		*(UW *)( l + 16 * i + 12 ) = 0;
	}
	DeviceBarrier();
	w32(sd + SD_BDPL, (UW)lpa);
	w32(sd + SD_BDPU, (UW)( lpa >> 32 ));
	w32(sd + SD_CBL, RING_BYTES);
	w16(sd + SD_LVI, NBLK - 1);
	w16(sd + SD_FMT, FMT_48K_16_2);
	w8(sd + SD_CTL + 2, (UB)( tag << 4 ));
}

LOCAL void stream_run( INT out, BOOL on )
{
	UINT	sd = sd_off(out);

	w8(sd + SD_CTL, on ? SDCTL_RUN : 0);
}

LOCAL INT stream_block( INT out )
{
	UW	pos = r32(sd_off(out) + SD_LPIB);

	return (INT)( ( pos % RING_BYTES ) / BLK_BYTES );
}

LOCAL void out_fill( INT k )
{
	UB	*b = out_ring + k * BLK_BYTES;

	if ( knl_snd_mix(b, BLK_FRAMES, RATE, 2, FALSE) < BLK_FRAMES ) {
		knl_memset(b, 0, BLK_BYTES);
	}
	out_blocks++;
}

/* ---------------------------------------------------------------- backend */

LOCAL INT hda_outputs( SDOut *o, INT max )
{
	if ( !hda_ok || nout == 0 || max < 1 ) {
		return 0;
	}
	knl_snd_out_set(o, (CONST UB *)"hda", out_name, SND_BE_HDA, SND_OF_LEVEL);
	return 1;
}

LOCAL BOOL hda_can_record( void )
{
	return ( hda_ok && has_in );
}

LOCAL ER hda_play_start( CONST UB *id, UW *rate, INT *channels )
{
	INT	i;

	if ( !hda_ok || nout == 0 ) {
		return E_NOEXS;
	}
	for ( i = 0; i < nout; i++ ) {
		WIDGET	*w = w_of(outp[i].nid[outp[i].n - 1]);

		path_set(&outp[i], TRUE);
		verb(outp[i].nid[outp[i].n - 1], V_SET_STREAM, ( 1 << 4 ) | 0);
		verb4(outp[i].nid[outp[i].n - 1], V_SET_FORMAT, FMT_48K_16_2);
		(void)w;
	}
	stream_setup(TRUE, 1);
	for ( i = 0; i < NBLK; i++ ) out_fill(i);
	out_done = 0;
	out_blocks = 0;
	stream_run(TRUE, TRUE);
	playing = TRUE;
	*rate = RATE;
	*channels = 2;
	tm_printf((UB *)"TessronOS: hda plays at %d Hz (%d blocks of %d frames)\n", RATE, NBLK, BLK_FRAMES);
	return E_OK;
}

LOCAL void hda_play_stop( void )
{
	INT	i;

	if ( !playing ) {
		return;
	}
	stream_run(TRUE, FALSE);
	for ( i = 0; i < nout; i++ ) {
		verb(outp[i].nid[outp[i].n - 1], V_SET_STREAM, 0);
	}
	playing = FALSE;
	tm_printf((UB *)"TessronOS: hda stops: %u blocks played\n", out_blocks);
}

LOCAL BOOL hda_play_running( void )
{
	return playing;
}

/* Whether something is in the microphone jack, as the pin senses it */
LOCAL BOOL jack_sense( void )
{
	UW	r = verb(inp[IN_JACK].nid[inp[IN_JACK].n - 1], V_GET_PINSENSE, 0);

	jack_ms = knl_usb_ms();
	return ( r != ~0U && ( r & 0x80000000U ) != 0 );
}

/* The gains a path's amplifiers were given, for the console */
LOCAL void log_take( CONST char *why, PATH *p )
{
	UB	buf[MAXPATH * 24 + 1], *b = buf;
	INT	i;

	buf[0] = '\0';
	for ( i = 0; i < p->n; i++ ) {
		WIDGET	*w = w_of(p->nid[i]);

		if ( ( w->caps & WC_INAMP ) && ( ( w->ampin >> 8 ) & 0x7f ) != 0 ) {
			b += tm_sprintf(b, (UB *)" %d:%d/%d(0dB %d)", p->nid[i], gain_of(w->ampin, vol_in),
					( w->ampin >> 8 ) & 0x7f, w->ampin & 0x7f);
		}
	}
	tm_printf((UB *)"TessronOS: hda: %s: pin %d through converter %d, jack %s, level %04x, gains%s\n", why,
		  p->nid[p->n - 1], p->nid[0], !in_have[IN_JACK] ? "not used" : jack_in ? "plugged in" : "empty",
		  vol_in, ( buf[0] != '\0' ) ? buf : (UB *)" none");
}

/*
 * The input path taken: its widgets set, the converter on the input
 * stream (and the one before, if another, off it)
 */
LOCAL void in_take( INT k, INT old )
{
	if ( old >= 0 && inp[old].nid[0] != inp[k].nid[0] ) {
		verb(inp[old].nid[0], V_SET_STREAM, 0);
	}
	in_cur = k;
	path_set(&inp[k], FALSE);
	verb(inp[k].nid[0], V_SET_STREAM, ( 2 << 4 ) | 0);
	verb4(inp[k].nid[0], V_SET_FORMAT, FMT_48K_16_2);
}

/* While recording: the jack's microphone when something is plugged in, the built-in one when not */
LOCAL void jack_look( void )
{
	INT	k;

	jack_event = FALSE;
	jack_in = jack_sense();
	k = jack_in ? IN_JACK : IN_INT;
	if ( k != in_cur ) {
		in_take(k, in_cur);
		log_take(jack_in ? "plugged in" : "unplugged", &inp[k]);
	}
}

/* The first samples of a recording, as the peaks of a few of its blocks (the first recordings only) */
LOCAL CONST UH	peak_at[8] = { 0, 1, 2, 3, 50, 100, 200, 400 };

LOCAL void peak_log( void )
{
	UB	buf[8 * 18 + 1], *b = buf;
	INT	i;

	for ( i = 0; i < peak_n; i++ ) {
		b += tm_sprintf(b, (UB *)" %d:%d/%d", peak_at[i], peak_l[i], peak_r[i]);
	}
	tm_printf((UB *)"TessronOS: hda: recorded peaks (block:left/right of 32767):%s\n", buf);
	peak_n = -1;
}

LOCAL void peak_note( CONST UB *blk )
{
	INT	i, l = 0, r = 0;

	if ( peak_n < 0 || peak_n >= 8 || in_blocks != peak_at[peak_n] ) {
		return;
	}
	for ( i = 0; i < BLK_FRAMES; i++ ) {
		INT	a = (H)( blk[i * 4] | ( blk[i * 4 + 1] << 8 ) );
		INT	c = (H)( blk[i * 4 + 2] | ( blk[i * 4 + 3] << 8 ) );

		if ( a < 0 ) a = -a;
		if ( c < 0 ) c = -c;
		if ( a > l ) l = a;
		if ( c > r ) r = c;
	}
	peak_l[peak_n] = (UH)l;
	peak_r[peak_n] = (UH)r;
	if ( ++peak_n == 8 ) {
		peak_log();
	}
}

LOCAL ER hda_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	if ( !hda_ok || !has_in ) {
		return E_NOEXS;
	}
	jack_in = FALSE;
	jack_event = FALSE;
	if ( in_have[IN_JACK] ) {
		/* told of plugging while recording, else looked at every so often */
		if ( jack_unsol ) {
			verb(inp[IN_JACK].nid[inp[IN_JACK].n - 1], V_SET_UNSOL, 0x80 | TAG_JACK);
		}
		jack_in = jack_sense();
	}
	in_take(jack_in ? IN_JACK : IN_INT, -1);
	stream_setup(FALSE, 2);
	in_done = 0;
	in_blocks = 0;
	stream_run(FALSE, TRUE);
	recording = TRUE;
	*dev_rate = RATE;
	*channels = 2;
	tm_printf((UB *)"TessronOS: hda records at %d Hz\n", RATE);
	peak_n = -1;
	if ( rec_logs < 3 ) {
		rec_logs++;
		peak_n = 0;
		log_take("records", &inp[in_cur]);
	}
	return E_OK;
}

LOCAL void hda_rec_stop( void )
{
	if ( !recording ) {
		return;
	}
	stream_run(FALSE, FALSE);
	verb(inp[in_cur].nid[0], V_SET_STREAM, 0);
	if ( jack_unsol ) {
		verb(inp[IN_JACK].nid[inp[IN_JACK].n - 1], V_SET_UNSOL, 0);
	}
	recording = FALSE;
	if ( peak_n > 0 ) {
		peak_log();
	}
	tm_printf((UB *)"TessronOS: hda stops recording: %u blocks\n", in_blocks);
}

LOCAL BOOL hda_rec_running( void )
{
	return recording;
}

/*
 * Every few milliseconds: the blocks behind the controller refilled
 * (the one it is in may still be read), the blocks it has recorded
 * handed on, and the microphone jack looked at when it has told of
 * plugging (or every JACK_MS when it cannot tell)
 */
#define JACK_MS		500

LOCAL void hda_poll( void )
{
	INT	k;

	if ( playing ) {
		k = stream_block(TRUE);
		while ( out_done != k ) {
			out_fill(out_done);
			out_done = ( out_done + 1 ) % NBLK;
		}
	}
	rirb_drain();
	if ( recording ) {
		k = stream_block(FALSE);
		while ( in_done != k ) {
			peak_note(in_ring + in_done * BLK_BYTES);
			knl_snd_capture(in_ring + in_done * BLK_BYTES, BLK_FRAMES, 2);
			in_blocks++;
			in_done = ( in_done + 1 ) % NBLK;
		}
		if ( in_have[IN_JACK] && ( jack_event || ( !jack_unsol && (INT)( knl_usb_ms() - jack_ms ) >= JACK_MS ) ) ) {
			jack_look();
		}
	}
}

LOCAL BOOL out_level( INT dn )
{
	return ( dn == DN_SDVOLMAIN || dn == DN_SDVOLPCM || dn == DN_SDGAINOUT );
}

LOCAL BOOL in_level( INT dn )
{
	return ( dn == DN_SDVOLMIC || dn == DN_SDGAININ );
}

LOCAL ER hda_get_level( INT dn, SDVol *v )
{
	if ( out_level(dn) ) {
		v->lv = v->rv = vol_out;
	} else if ( in_level(dn) ) {
		v->lv = v->rv = vol_in;
	} else {
		return E_NOSPT;
	}
	return E_OK;
}

LOCAL ER hda_set_level( INT dn, CONST SDVol *v )
{
	INT	i;

	if ( out_level(dn) ) {
		vol_out = v->lv;
		for ( i = 0; i < nout && hda_ok; i++ ) path_amps(&outp[i], vol_out, TRUE);
	} else if ( in_level(dn) ) {
		vol_in = v->lv;
		if ( has_in && hda_ok ) path_amps(&inp[in_cur], vol_in, FALSE);
	} else {
		return E_NOSPT;
	}
	return E_OK;
}

EXPORT CONST SNDHW knl_hda_hw = {
	SND_BE_HDA,
	hda_outputs,
	hda_can_record,
	hda_play_start,
	hda_play_stop,
	hda_play_running,
	hda_rec_start,
	hda_rec_stop,
	hda_rec_running,
	hda_poll,
	hda_get_level,
	hda_set_level,
	0,
	0
};

/* ---------------------------------------------------------------- start-up */

LOCAL void *pages( UINT order, UD *p_pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) {
		return NULL;
	}
	*p_pa = knl_pf_to_pa(pf);
	return PA2VA(*p_pa);
}

/* The controller found on the bus brought up, its first codec walked */
EXPORT ER knl_hda_init( void )
{
	T_PCIDEV	p;
	UB		*pg0;
	UH		gcap, st;
	INT		i;

	if ( ts_pcie_find(0x04, 0x03, 0xff, 0, &p) < E_OK ) {
		return E_NOEXS;
	}
	ts_pcie_enable(&p);
	base = ts_pcie_bar_base(&p, 0);
	if ( base == 0 ) {
		return E_NOEXS;
	}
	pg0 = (UB *)pages(0, &page0_pa);
	bdl = (UB *)pages(0, &bdl_pa);
	out_ring = (UB *)pages(1, &out_pa);		/* two pages: the ring of RING_BYTES */
	in_ring = (UB *)pages(1, &in_pa);
	wid = (WIDGET *)Kcalloc(MAXW, sizeof(WIDGET));
	if ( pg0 == NULL || bdl == NULL || out_ring == NULL || in_ring == NULL || wid == NULL ) {
		return E_NOMEM;
	}
	corb = (UW *)pg0;
	rirb = (UD *)( pg0 + 1024 );

	/* out of reset, the codecs given time to say they are there */
	w32(GCTL, r32(GCTL) & ~(UW)GCTL_CRST);
	(void)wait8(GCTL, GCTL_CRST, 0, 100);
	w32(GCTL, r32(GCTL) | GCTL_CRST);
	if ( !wait8(GCTL, GCTL_CRST, GCTL_CRST, 100) ) {
		tm_printf((UB *)"TessronOS: hda: the controller does not come out of reset\n");
		return E_IO;
	}
	WaitUsec(1000);
	gcap = r16(GCAP);
	iss = ( gcap >> 8 ) & 0xf;
	oss = ( gcap >> 12 ) & 0xf;
	st = r16(STATESTS);
	w32(INTCTL, 0);
	w32(GCTL, r32(GCTL) | GCTL_UNSOL);	/* the jack's plugging told through the RIRB */

	/* the command and response rings, 256 entries each */
	w8(CORBCTL, 0);
	w8(RIRBCTL, 0);
	(void)wait8(CORBCTL, 0x02, 0, 10);
	(void)wait8(RIRBCTL, 0x02, 0, 10);
	w32(CORBLBASE, (UW)page0_pa);
	w32(CORBUBASE, (UW)( page0_pa >> 32 ));
	w8(CORBSIZE, 0x02);
	w16(CORBRP, 0x8000);
	for ( i = 0; i < 100 && ( r16(CORBRP) & 0x8000 ) == 0; i++ ) WaitUsec(10);
	w16(CORBRP, 0);
	for ( i = 0; i < 100 && ( r16(CORBRP) & 0x8000 ) != 0; i++ ) WaitUsec(10);
	w16(CORBWP, 0);
	w32(RIRBLBASE, (UW)( page0_pa + 1024 ));
	w32(RIRBUBASE, (UW)( ( page0_pa + 1024 ) >> 32 ));
	w8(RIRBSIZE, 0x02);
	w16(RIRBWP, 0x8000);
	w16(RINTCNT, 1);
	rirb_rp = 0;
	w8(CORBCTL, 0x02);
	w8(RIRBCTL, 0x03);		/* the response count noted in RIRBSTS (the interrupt stays off in INTCTL) */
	(void)wait8(CORBCTL, 0x02, 0x02, 10);

	tm_printf((UB *)"TessronOS: hda %04x:%04x at %x%08x, version %d.%d, %d input and %d output streams, "
		  "codecs %04x\n", p.vendor, p.device, (UW)( (UD)base >> 32 ), (UW)base, r8(VMAJ), r8(VMIN),
		  iss, oss, st);
	if ( oss == 0 || st == 0 ) {
		return E_NOEXS;
	}
	for ( cad = 0; cad < 15 && ( st & ( 1U << cad ) ) == 0; cad++ ) ;
	walk_codec();
	tm_printf((UB *)"TessronOS: hda: codec %d %04x:%04x, function group %d, widgets %d-%d, "
		  "%d output paths, %s input\n", cad, codec_vendor >> 16, codec_vendor & 0xffff, afg, wstart,
		  wstart + wcount - 1, nout, has_in ? "an" : "no");
	hda_ok = ( afg != 0 );
	return hda_ok ? E_OK : E_NOEXS;
}

#endif /* X64 */
