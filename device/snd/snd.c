/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	snd.c
 *	The sound device "snda": channels, conversion, mixing and requests,
 *	apart from the hardware (design 10.15).
 *
 *	Every process that uses the device has a channel of its own for each
 *	unit it uses, found from the process of the task that made the
 *	request (all tasks of a program share it) and the unit: "snda" and
 *	each of "snda0" .. "snda3" are channels apart, so one program can,
 *	say, play on one and record on another. A kernel task that belongs
 *	to no process stands for itself. The device behaves toward each
 *	channel as a device of its own would: the mode, the state, the PCM
 *	buffer, the counts and the stop count are the channel's.
 *
 *	A channel's PCM buffer is a FIFO of the program's samples in the
 *	rate, format (mu-law, 8 bit unsigned, 16 bit signed) and channel
 *	count it set with DN_SDPCMMODE.
 *
 *	Playback: the program fills the FIFO with writes. For every piece of
 *	output the hardware asks for, each playing channel is converted to
 *	the hardware's rate and added in; one short of samples holds its
 *	level. A stream that can wait (a USB packet can go a little later)
 *	gets output only once some channel has the samples it takes, so a
 *	single program's stream follows its writes. At its stop count a
 *	channel adds silence until what was queued has been heard, then
 *	stops; a channel that stays short of samples for a while (an
 *	underrun) stops too. PCM_REPPLAY loops over what was written while
 *	stopped.
 *
 *	Recording: every frame of the hardware's input is converted to the
 *	recording channel's rate, format and channels and put into its FIFO,
 *	which the program drains with reads of data number 0 or 1. It stops
 *	by itself at the stop count or when the FIFO overflows.
 *
 *	Rate conversion: to a lower rate, the mean of the input samples since
 *	the previous output sample; to a higher one, a straight line between
 *	them.
 *
 *	Each FIFO has one writer and one reader, the request and the stream
 *	one way or the other; each moves only its own index, and a channel
 *	is set up completely before its state says it plays or records.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/proc.h>
#include "sndhw.h"

IMPORT void knl_usb_lock( void );
IMPORT void knl_usb_unlock( void );
IMPORT UW   knl_usb_ms( void );

#define NCH		8		/* channels in use at once */
#define BUF_DEFAULT	32768
#define BUF_MAX		(1024 * 1024)
#define REAP_MS		1000		/* ended processes are looked for */
#define IDLE_MS		3000		/* among channels unused this long */
#define DRAIN_MS	300		/* output queued ahead of what is heard */
#define STARVE_MS	500		/* a channel short this long stops */

#define MB()		Asm("dmb ish" ::: "memory")

/* why a channel stopped by itself */
#define WHY_ENDCNT	1
#define WHY_OVERRUN	2
#define WHY_UNDERRUN	3

LOCAL CONST UW rate_hz[6] = { 8000, 11025, 16000, 22050, 44100, 48000 };

typedef struct {
	BOOL		used;
	ID		key;		/* the process, or -task for a kernel task */
	INT		unit;		/* the unit it was opened as */
	UW		last_use;
	SDPcmMode	mode;
	volatile INT	ctl;		/* PCM_* */
	INT		bufsz;		/* the ring: one byte more than usable */
	UB		*fifo;
	volatile INT	head, tail;	/* write, read; equal: empty */
	volatile UW	count;		/* bytes made (recording) or taken */
	INT		endcnt;
	volatile BOOL	stop_req;	/* the service is to stop it */
	volatile INT	stop_why;
	/* playback */
	BOOL		rep;		/* repeated play */
	INT		rp;		/* its read position */
	BOOL		pinit;		/* the resampler primed */
	UW		pstep, pfrac;	/* program samples per stream sample, 16.16 */
	INT		cur_l, cur_r, nxt_l, nxt_r;
	volatile BOOL	draining;	/* past its stop count */
	INT		drain;		/* stream frames of silence to go */
	volatile BOOL	starved;	/* short of samples at the last mix */
	UW		starve_since;	/* 0 not */
	/* recording */
	UW		step, acc;	/* stream samples per program sample, 16.16 */
	BOOL		down;
	INT		sum_l, sum_r, nsum;
	INT		prev_l, prev_r;
	BOOL		have_prev;
} CHAN;

LOCAL CHAN		ch[NCH];
LOCAL CHAN *volatile	rec_ch = NULL;	/* the channel that records */
LOCAL CONST SNDHW	*play_hw = NULL;	/* the stream that plays */
LOCAL CONST SNDHW	*rec_hw = NULL;	/* and the one that records */
LOCAL UW		mix_rate = 48000;
LOCAL UW		rec_in_rate = 0;
LOCAL INT		rec_in_channels = 0;
LOCAL INT		play_channels = 0;
LOCAL SDStat		snd_stat;
LOCAL ID		snd_devid = 0;
LOCAL BOOL		snd_ready = FALSE;

LOCAL void lock( void )   { knl_usb_lock(); }
LOCAL void unlock( void ) { knl_usb_unlock(); }

/* a / b in 16.16 fixed point, for sample rates */
LOCAL UW ratio16( UW a, UW b )
{
	while ( b > 0xffff ) {
		a >>= 1;
		b >>= 1;
	}

	return ((a / b) << 16) + (((a % b) << 16) / b);
}

/* ---------------------------------------------------------------- backends */

/*
 * A recording source for tests: with it on, recording takes whatever
 * knl_snd_capture() is handed, as if it came at 48kHz in stereo.
 */
LOCAL BOOL	loop_on = FALSE;
LOCAL BOOL	loop_run = FALSE;

LOCAL BOOL loop_can_play( void )   { return FALSE; }
LOCAL BOOL loop_can_record( void ) { return loop_on; }
LOCAL ER   loop_play_start( UW *rate, INT *channels ) { return E_NOSPT; }
LOCAL void loop_play_stop( void )  { }
LOCAL BOOL loop_play_running( void ) { return FALSE; }
LOCAL void loop_poll( void )       { }

LOCAL ER loop_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	*dev_rate = 48000;
	*channels = 2;
	loop_run = TRUE;

	return E_OK;
}

LOCAL void loop_rec_stop( void )    { loop_run = FALSE; }
LOCAL BOOL loop_rec_running( void ) { return ( loop_run && loop_on ); }
LOCAL ER   loop_get_level( INT dn, SDVol *v )       { return E_NOSPT; }
LOCAL ER   loop_set_level( INT dn, CONST SDVol *v ) { return E_NOSPT; }

LOCAL CONST SNDHW loop_hw = {
	SND_BE_NONE,
	loop_can_play, loop_can_record,
	loop_play_start, loop_play_stop, loop_play_running,
	loop_rec_start, loop_rec_stop, loop_rec_running,
	loop_poll, loop_get_level, loop_set_level
};

EXPORT void knl_snd_loopback( BOOL on )
{
	loop_on = on;
}

/* The hardware to play on: a USB audio function first */
LOCAL CONST SNDHW *hw_play( void )
{
	if ( knl_uac_hw.can_play() ) {
		return &knl_uac_hw;
	}
#ifdef RPI5
	if ( knl_i2s_hw.can_play() ) {
		return &knl_i2s_hw;
	}
#endif

	return NULL;
}

LOCAL CONST SNDHW *hw_record( void )
{
	if ( loop_on ) {
		return &loop_hw;
	}
	if ( knl_uac_hw.can_record() ) {
		return &knl_uac_hw;
	}
#ifdef RPI5
	if ( knl_i2s_hw.can_record() ) {
		return &knl_i2s_hw;
	}
#endif

	return NULL;
}

/* ---------------------------------------------------------------- channels */

LOCAL BOOL playing( CONST CHAN *c )
{
	return ( c->ctl == PCM_PLAY || c->ctl == PCM_REPPLAY );
}

LOCAL BOOL limited( CONST CHAN *c )
{
	return ( c->endcnt < PCM_COUNT_NONE );
}

/* The streams run while a channel needs them */
LOCAL void follow( void )
{
	INT	i;
	BOOL	play = FALSE;

	for ( i = 0; i < NCH; i++ ) {
		if ( ch[i].used && playing(&ch[i]) ) {
			play = TRUE;
		}
	}
	if ( play_hw != NULL && !play ) {
		play_hw->play_stop();
		play_hw = NULL;
	}
	if ( rec_hw != NULL && rec_ch == NULL ) {
		rec_hw->rec_stop();
		rec_hw = NULL;
	}
}

LOCAL void chan_stop( CHAN *c )
{
	if ( c->stop_why == WHY_UNDERRUN ) {
		snd_stat.underruns++;
	}
	c->ctl = PCM_STOP;
	c->stop_req = FALSE;
	c->draining = FALSE;
	if ( rec_ch == c ) {
		rec_ch = NULL;
	}
	follow();
}

LOCAL void chan_free( CHAN *c )
{
	chan_stop(c);
	if ( c->fifo != NULL ) {
		Kfree(c->fifo);
		c->fifo = NULL;
	}
	c->used = FALSE;
}

/* Who is asking: its process, or the task itself outside any process */
LOCAL ID requester( void )
{
	ID	pid = ts_get_pid();

	return ( pid > 0 ) ? pid : -tk_get_tid();
}

LOCAL BOOL requester_gone( ID key )
{
	if ( key > 0 ) {
		T_RPRC	r;

		return ( ts_ref_prc(key, &r) < E_OK );
	} else {
		T_RTSK	r;

		return ( tk_ref_tsk(-key, &r) < E_OK );
	}
}

/* The requester's channel, set up at its first request (NULL: all taken) */
LOCAL CHAN *chan_of( INT unit )
{
	ID	key = requester();
	CHAN	*c = NULL;
	INT	i;

	for ( i = 0; i < NCH && c == NULL; i++ ) {
		if ( ch[i].used && ch[i].key == key && ch[i].unit == unit ) {
			c = &ch[i];
		}
	}
	for ( i = 0; i < NCH && c == NULL; i++ ) {
		if ( !ch[i].used ) {
			c = &ch[i];
			knl_memset(c, 0, sizeof(*c));
			c->key = key;
			c->unit = unit;
			c->bufsz = BUF_DEFAULT + 1;
			c->endcnt = PCM_COUNT_NONE;
			c->mode.sampling = PCMST48K;
			c->mode.datafmt = PCMFmt16;
			c->mode.stereo = 1;
			c->used = TRUE;
		}
	}
	if ( c != NULL ) {
		c->last_use = knl_usb_ms();
	}

	return c;
}

/* ---------------------------------------------------------------- formats */

LOCAL UB ulaw( INT s )
{
	INT	sign = (s >> 8) & 0x80, exp, mant, mask;

	if ( sign != 0 ) {
		s = -s;
	}
	if ( s > 32635 ) {
		s = 32635;
	}
	s += 0x84;
	for ( exp = 7, mask = 0x4000; exp > 0 && (s & mask) == 0; exp--, mask >>= 1 ) {
		;
	}
	mant = (s >> (exp + 3)) & 0x0f;

	return (UB)~(sign | (exp << 4) | mant);
}

LOCAL INT unulaw( UB u )
{
	INT	s;

	u = (UB)~u;
	s = ((((INT)u & 0x0f) << 3) + 0x84) << ((u >> 4) & 7);
	s -= 0x84;

	return ( (u & 0x80) != 0 ) ? -s : s;
}

LOCAL INT frame_bytes( CONST CHAN *c )
{
	INT	b = ( c->mode.datafmt == PCMFmt16 ) ? 2 : 1;

	return c->mode.stereo ? 2 * b : b;
}

LOCAL INT fifo_used( CONST CHAN *c )
{
	INT	used = c->head - c->tail;

	return ( used < 0 ) ? used + c->bufsz : used;
}

LOCAL ER fifo_alloc( CHAN *c )
{
	if ( c->fifo == NULL ) {
		c->fifo = (UB *)Kmalloc((SZ)c->bufsz);
		if ( c->fifo == NULL ) {
			return E_NOMEM;
		}
	}

	return E_OK;
}

LOCAL INT clamp16( INT s )
{
	return ( s > 32767 ) ? 32767 : ( s < -32768 ) ? -32768 : s;
}

/* ---------------------------------------------------------------- recording */

LOCAL void rec_end( CHAN *c, INT why )
{
	c->ctl = PCM_STOP;
	c->stop_why = why;
	c->stop_req = TRUE;
}

/* One frame into the recording channel's FIFO */
LOCAL void emit( CHAN *c, INT l, INT r )
{
	UB	o[4];
	INT	n = 0, i;

	if ( c->ctl != PCM_REC ) {
		return;
	}
	if ( !c->mode.stereo ) {
		l = (l + r) / 2;
	}
	for ( i = 0; i < ( c->mode.stereo ? 2 : 1 ); i++ ) {
		INT	s = clamp16(( i == 0 ) ? l : r);

		switch ( c->mode.datafmt ) {
		case PCMFmt16:	o[n++] = (UB)s; o[n++] = (UB)(s >> 8); break;
		case PCMFmt8:	o[n++] = (UB)((s >> 8) + 128); break;
		default:	o[n++] = ulaw(s); break;
		}
	}
	if ( limited(c) && c->count + (UW)n > (UW)c->endcnt ) {
		rec_end(c, WHY_ENDCNT);
		return;
	}
	if ( c->bufsz - 1 - fifo_used(c) < n ) {
		rec_end(c, WHY_OVERRUN);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		c->fifo[c->head] = o[i];
		c->head = ( c->head + 1 == c->bufsz ) ? 0 : c->head + 1;
	}
	MB();
	c->count += (UW)n;
	if ( limited(c) && c->count >= (UW)c->endcnt ) {
		rec_end(c, WHY_ENDCNT);
	}
}

/* One frame at the input's rate */
LOCAL void input( CHAN *c, INT l, INT r )
{
	if ( c->down ) {
		c->sum_l += l;
		c->sum_r += r;
		c->nsum++;
		c->acc += 0x10000;
		while ( c->acc >= c->step && c->nsum > 0 ) {
			emit(c, c->sum_l / c->nsum, c->sum_r / c->nsum);
			c->sum_l = c->sum_r = c->nsum = 0;
			c->acc -= c->step;
		}
		return;
	}
	if ( !c->have_prev ) {
		c->prev_l = l;
		c->prev_r = r;
		c->have_prev = TRUE;
		return;
	}
	while ( c->acc <= 0x10000 ) {
		INT	a = (INT)(c->acc >> 2);	/* 14 bits: no overflow below */

		emit(c, c->prev_l + (l - c->prev_l) * a / 0x4000,
		     c->prev_r + (r - c->prev_r) * a / 0x4000);
		c->acc += c->step;
	}
	c->acc -= 0x10000;
	c->prev_l = l;
	c->prev_r = r;
}

EXPORT void knl_snd_capture( CONST UB *src, INT frames, INT channels )
{
	CHAN	*c = rec_ch;
	INT	i, fb = 2 * channels;

	if ( c == NULL || c->ctl != PCM_REC || channels <= 0 || frames <= 0 ) {
		return;
	}
	snd_stat.in_frames += (UW)frames;
	for ( i = 0; i < frames; i++, src += fb ) {
		INT	l = (H)(src[0] | (src[1] << 8));
		INT	r = ( channels > 1 ) ? (H)(src[2] | (src[3] << 8)) : l;

		input(c, l, r);
	}
}

/* ---------------------------------------------------------------- playback */

LOCAL void decode( CONST CHAN *c, CONST UB *p, INT *l, INT *r )
{
	INT	i, s[2];

	s[1] = 0;
	for ( i = 0; i < ( c->mode.stereo ? 2 : 1 ); i++ ) {
		switch ( c->mode.datafmt ) {
		case PCMFmt16:	s[i] = (H)(p[0] | (p[1] << 8)); p += 2; break;
		case PCMFmt8:	s[i] = ((INT)p[0] - 128) << 8; p++; break;
		default:	s[i] = unulaw(p[0]); p++; break;
		}
	}
	*l = s[0];
	*r = c->mode.stereo ? s[1] : s[0];
}

/* The channel's next frame; FALSE when there is none */
LOCAL BOOL pull( CHAN *c, INT *l, INT *r )
{
	UB	f[4];
	INT	fb = frame_bytes(c), i;

	if ( c->draining ) {
		return FALSE;
	}
	if ( c->rep ) {
		if ( c->head < fb ) {
			return FALSE;
		}
		knl_memcpy(f, c->fifo + c->rp, fb);
		c->rp += fb;
		if ( c->rp + fb > c->head ) {
			c->rp = 0;
		}
	} else {
		if ( fifo_used(c) < fb ) {
			return FALSE;
		}
		for ( i = 0; i < fb; i++ ) {
			f[i] = c->fifo[c->tail];
			c->tail = ( c->tail + 1 == c->bufsz ) ? 0 : c->tail + 1;
		}
	}
	decode(c, f, l, r);
	c->count += (UW)fb;
	if ( limited(c) && c->count >= (UW)c->endcnt ) {
		c->drain = (INT)(mix_rate / 100 * (UW)DRAIN_MS / 10);
		c->stop_why = WHY_ENDCNT;
		c->draining = TRUE;
	}

	return TRUE;
}

/* One frame of a playing channel at the stream's rate */
LOCAL void next( CHAN *c, INT *l, INT *r )
{
	INT	a;

	if ( c->draining ) {
		*l = *r = 0;
		return;
	}
	a = (INT)(c->pfrac >> 2);
	*l = c->cur_l + (c->nxt_l - c->cur_l) * a / 0x4000;
	*r = c->cur_r + (c->nxt_r - c->cur_r) * a / 0x4000;
	c->pfrac += c->pstep;
	while ( c->pfrac >= 0x10000 ) {
		c->cur_l = c->nxt_l;
		c->cur_r = c->nxt_r;
		pull(c, &c->nxt_l, &c->nxt_r);	/* short: the level is held */
		c->pfrac -= 0x10000;
	}
}

/* The channel has what frames stream frames take, or needs nothing */
LOCAL BOOL chan_ready( CONST CHAN *c, INT frames )
{
	INT	pulls, have;

	if ( c->draining || c->rep ) {
		return TRUE;
	}
	pulls = (INT)((c->pfrac + (UW)frames * c->pstep) >> 16);
	have = fifo_used(c);
	/* all of it up to the stop count is in: the end, silence after it */

	return ( have >= pulls * frame_bytes(c)
	      || ( limited(c) && c->count + (UW)have >= (UW)c->endcnt ) );
}

EXPORT INT knl_snd_mix( UB *dst, INT frames, UW rate, INT channels, BOOL wait )
{
	CHAN	*act[NCH];
	INT	n = 0, i, k, o = 0;
	BOOL	ready = FALSE;

	if ( frames <= 0 || rate == 0 || channels <= 0 ) {
		return 0;
	}
	mix_rate = rate;
	for ( i = 0; i < NCH; i++ ) {
		CHAN	*c = &ch[i];

		if ( !c->used || !playing(c) ) {
			continue;
		}
		if ( !c->pinit ) {
			c->pstep = ratio16(rate_hz[c->mode.sampling], rate);
			c->pfrac = 0;
			c->cur_l = c->cur_r = c->nxt_l = c->nxt_r = 0;
			if ( pull(c, &c->cur_l, &c->cur_r) ) {
				pull(c, &c->nxt_l, &c->nxt_r);
			}
			c->pinit = TRUE;
		}
		c->starved = !chan_ready(c, frames);
		if ( !c->starved ) {
			ready = TRUE;
		}
		act[n++] = c;
	}
	snd_stat.out_packets++;
	if ( wait && !ready ) {
		return 0;
	}
	if ( !ready ) {
		snd_stat.out_short++;
	}
	for ( i = 0; i < frames; i++ ) {
		INT	sl = 0, sr = 0, l, r;

		for ( k = 0; k < n; k++ ) {
			next(act[k], &l, &r);
			sl += l;
			sr += r;
		}
		sl = clamp16(sl);
		sr = clamp16(sr);
		if ( channels == 1 ) {
			INT	m = (sl + sr) / 2;

			dst[o++] = (UB)m;
			dst[o++] = (UB)(m >> 8);
		} else {
			dst[o++] = (UB)sl;
			dst[o++] = (UB)(sl >> 8);
			dst[o++] = (UB)sr;
			dst[o++] = (UB)(sr >> 8);
			for ( k = 2; k < channels; k++ ) {
				dst[o++] = 0;
				dst[o++] = 0;
			}
		}
	}
	for ( k = 0; k < n; k++ ) {
		CHAN	*c = act[k];

		if ( c->draining && !c->stop_req && (c->drain -= frames) <= 0 ) {
			c->stop_req = TRUE;
		}
	}
	snd_stat.out_frames += (UW)frames;

	return frames;
}

/* ---------------------------------------------------------------- control */

LOCAL ER start_rec( CHAN *c )
{
	UW	out = rate_hz[c->mode.sampling], in = 0;
	INT	nch = 0;
	CONST SNDHW *hw;
	ER	er;

	if ( rec_ch != NULL && rec_ch != c ) {
		return E_BUSY;			/* another program records */
	}
	hw = hw_record();
	if ( hw == NULL ) {
		return E_NOSPT;
	}
	er = fifo_alloc(c);
	if ( er < E_OK ) {
		return er;
	}
	/* rec_ch == c: its recording ended and was not stopped yet */
	rec_ch = NULL;
	if ( rec_hw != NULL ) {
		rec_hw->rec_stop();
		rec_hw = NULL;
	}
	c->head = c->tail = 0;
	c->count = 0;
	c->stop_req = FALSE;
	c->stop_why = 0;
	er = hw->rec_start(out, &in, &nch);
	if ( er < E_OK ) {
		return er;
	}
	rec_hw = hw;
	if ( in == 0 ) {
		in = out;
	}
	rec_in_rate = in;
	rec_in_channels = nch;
	c->down = ( in >= out );
	c->step = ratio16(in, out);
	c->acc = 0;
	c->sum_l = c->sum_r = c->nsum = 0;
	c->have_prev = FALSE;
	MB();
	c->ctl = PCM_REC;
	rec_ch = c;

	return E_OK;
}

LOCAL ER start_play( CHAN *c, INT how )
{
	ER	er;

	if ( play_hw == NULL && hw_play() == NULL ) {
		return E_NOSPT;
	}
	er = fifo_alloc(c);
	if ( er < E_OK ) {
		return er;
	}
	c->count = 0;
	c->stop_req = FALSE;
	c->stop_why = 0;
	c->draining = FALSE;
	c->rp = 0;
	c->rep = ( how == PCM_REPPLAY );
	c->pinit = FALSE;			/* primed at its first mix */
	c->starved = FALSE;
	c->starve_since = 0;
	MB();
	c->ctl = how;
	if ( play_hw == NULL ) {
		CONST SNDHW	*hw = hw_play();
		UW		rate = 0;

		er = hw->play_start(&rate, &play_channels);
		if ( er < E_OK ) {
			c->ctl = PCM_STOP;
			return er;
		}
		play_hw = hw;
		mix_rate = rate;
	}

	return E_OK;
}

/*
 * Every few milliseconds from the USB manager's task, with its lock
 * held: the backends are served, a stream the hardware lost stops its
 * channels, and channels that ended, underran or whose process ended
 * are stopped or let go.
 */
EXPORT void knl_snd_poll( void )
{
	LOCAL UW	last_reap = 0;
	UW		now;
	INT		i;

	if ( !snd_ready ) {
		return;
	}
	now = knl_usb_ms();
	if ( play_hw != NULL ) {
		play_hw->poll();
		if ( !play_hw->play_running() ) {
			play_hw = NULL;
			for ( i = 0; i < NCH; i++ ) {
				if ( ch[i].used && playing(&ch[i]) ) {
					ch[i].ctl = PCM_STOP;
				}
			}
		}
	}
	if ( rec_hw != NULL ) {
		if ( rec_hw != play_hw ) {
			rec_hw->poll();
		}
		if ( !rec_hw->rec_running() ) {
			rec_hw = NULL;
			if ( rec_ch != NULL ) {
				rec_ch->ctl = PCM_STOP;
				rec_ch = NULL;
			}
		}
	}
	for ( i = 0; i < NCH; i++ ) {
		CHAN	*c = &ch[i];

		if ( !c->used ) {
			continue;
		}
		if ( c->ctl == PCM_PLAY && !c->draining ) {
			if ( !c->starved ) {
				c->starve_since = 0;
			} else if ( c->starve_since == 0 ) {
				c->starve_since = now | 1;
			} else if ( (INT)(now - c->starve_since) >= STARVE_MS ) {
				c->stop_why = WHY_UNDERRUN;
				c->stop_req = TRUE;
			}
		}
		if ( c->stop_req ) {
			chan_stop(c);
		}
	}
	/* channels of programs that have ended, and idle ones of kernel tasks */
	if ( (INT)(now - last_reap) >= REAP_MS ) {
		last_reap = now;
		for ( i = 0; i < NCH; i++ ) {
			CHAN	*c = &ch[i];

			if ( !c->used || (INT)(now - c->last_use) < IDLE_MS ) {
				continue;
			}
			if ( requester_gone(c->key) || ( c->key < 0 && c->ctl == PCM_STOP ) ) {
				chan_free(c);
			}
		}
	}
	follow();
}

EXPORT BOOL knl_snd_active( void )
{
	return ( play_hw != NULL || rec_hw != NULL );
}

/* ---------------------------------------------------------------- requests */

LOCAL BOOL is_level( INT dn )
{
	return ( dn <= DN_SDGAININ && dn >= DN_SDVOLAUX2 );
}

/* An attribute answered: what fits of it, or its size to a read of 0 */
LOCAL void reply( T_DEVREQ *req, CONST void *p, INT size )
{
	if ( req->size <= 0 ) {
		req->asize = size;
		return;
	}
	if ( req->size < size ) {
		size = (INT)req->size;
	}
	knl_memcpy(req->buf, p, size);
	req->asize = size;
}

LOCAL ER proc_read( CHAN *c, T_DEVREQ *req )
{
	ER	er;

	switch ( req->start ) {
	case 0:
	case 1: {
		/* recorded bytes, out of the ring in up to two pieces */
		INT	n = 0, want = (INT)req->size, h = c->head;

		if ( playing(c) || c->fifo == NULL ) {
			return E_OK;
		}
		if ( want <= 0 ) {
			req->asize = fifo_used(c);
			return E_OK;
		}
		while ( n < want && c->tail != h ) {
			INT	end = ( h > c->tail ) ? h : c->bufsz;
			INT	k = end - c->tail;

			if ( k > want - n ) {
				k = want - n;
			}
			knl_memcpy((UB *)req->buf + n, c->fifo + c->tail, k);
			n += k;
			c->tail = ( c->tail + k == c->bufsz ) ? 0 : c->tail + k;
		}
		req->asize = n;
		return E_OK;
	}
	case DN_SDPCMINFO: {
		SDPcmInfo	in;
		BOOL		rec, play;
		INT		i;

		lock();
		rec = ( hw_record() != NULL );
		play = ( play_hw != NULL || hw_play() != NULL );
		unlock();
		knl_memset(&in, 0, sizeof(in));
		if ( rec || play ) {
			for ( i = PCMST8K; i <= PCMST48K; i++ ) {
				in.spt[i].datafmt = (1 << PCMFmt8ulaw) | (1 << PCMFmt8)
						  | (1 << PCMFmt16);
				in.spt[i].mrecord = in.spt[i].srecord = rec;
				in.spt[i].mplay = in.spt[i].splay = play;
			}
		}
		reply(req, &in, sizeof(in));
		return E_OK;
	}
	case DN_SDPCMMODE:
		reply(req, &c->mode, sizeof(c->mode));
		return E_OK;
	case DN_SDPCMCTL: {
		W	v = c->ctl;

		reply(req, &v, sizeof(v));
		return E_OK;
	}
	case DN_SDPCMCNT: {
		W	v = (W)c->count;

		reply(req, &v, sizeof(v));
		return E_OK;
	}
	case DN_SDPCMENDCNT: {
		W	v = c->endcnt;

		reply(req, &v, sizeof(v));
		return E_OK;
	}
	case DN_SDPCMBUFSZ: {
		W	v = c->bufsz - 1;

		reply(req, &v, sizeof(v));
		return E_OK;
	}
	case DN_SDSELIN:
	case DN_SDSELOUT: {
		SDSel	s;

		knl_memset(&s, 0, sizeof(s));
		lock();
		if ( req->start == DN_SDSELIN ) {
			s.mic = ( hw_record() != NULL );
		} else {
			s.pcm = ( play_hw != NULL || hw_play() != NULL );
		}
		unlock();
		reply(req, &s, sizeof(s));
		return E_OK;
	}
	case DN_SDSTAT: {
		SDStat	st;
		INT	i;

		lock();
		st = snd_stat;
		st.backend = ( play_hw != NULL ) ? (UW)play_hw->kind
			   : ( rec_hw != NULL ) ? (UW)rec_hw->kind : SND_BE_NONE;
		st.out_rate = ( play_hw != NULL ) ? mix_rate : 0;
		st.out_channels = ( play_hw != NULL ) ? (UW)play_channels : 0;
		st.in_rate = ( rec_hw != NULL ) ? rec_in_rate : 0;
		st.in_channels = ( rec_hw != NULL ) ? (UW)rec_in_channels : 0;
		st.channels = st.playing = 0;
		for ( i = 0; i < NCH; i++ ) {
			if ( ch[i].used ) {
				st.channels++;
				if ( playing(&ch[i]) ) {
					st.playing++;
				}
			}
		}
		unlock();
		reply(req, &st, sizeof(st));
		return E_OK;
	}
	default:
		if ( is_level((INT)req->start) ) {
			SDVol	v;
			CONST SNDHW *hw;

			knl_memset(&v, 0, sizeof(v));
			lock();
			hw = ( play_hw != NULL ) ? play_hw : hw_play();
			if ( hw == NULL ) {
				hw = hw_record();
			}
			er = ( hw != NULL ) ? hw->get_level((INT)req->start, &v) : E_NOSPT;
			unlock();
			if ( er < E_OK ) {
				return er;
			}
			reply(req, &v, sizeof(v));
			return E_OK;
		}
		return E_PAR;
	}
}

/* Playback data, into the free part of the channel's FIFO */
LOCAL INT write_pcm( CHAN *c, T_DEVREQ *req )
{
	INT	n = 0, want = (INT)req->size, t;

	if ( c->ctl == PCM_REC || c->ctl == PCM_REPPLAY ) {
		return 0;
	}
	if ( want <= 0 ) {
		return c->bufsz - 1 - (( c->fifo != NULL ) ? fifo_used(c) : 0);
	}
	if ( fifo_alloc(c) < E_OK ) {
		return 0;
	}
	if ( c->ctl == PCM_STOP && req->start == 0 ) {
		c->head = c->tail = 0;
	}
	t = c->tail;
	while ( n < want ) {
		INT	used = c->head - t, room, k;

		if ( used < 0 ) {
			used += c->bufsz;
		}
		room = c->bufsz - 1 - used;
		if ( room <= 0 ) {
			break;
		}
		k = ( c->head >= t ) ? c->bufsz - c->head : t - 1 - c->head;
		if ( c->head >= t && t == 0 ) {
			k--;			/* a byte stays free at the wrap */
		}
		if ( k > room ) {
			k = room;
		}
		if ( k > want - n ) {
			k = want - n;
		}
		if ( k <= 0 ) {
			break;
		}
		knl_memcpy(c->fifo + c->head, (UB *)req->buf + n, k);
		n += k;
		MB();
		c->head = ( c->head + k == c->bufsz ) ? 0 : c->head + k;
	}

	return n;
}

LOCAL ER proc_write( CHAN *c, T_DEVREQ *req )
{
	W	v = 0;
	ER	er;

	req->asize = req->size;
	if ( req->start != 0 && req->start != 1 && req->size >= (SZ)sizeof(W) ) {
		knl_memcpy(&v, req->buf, sizeof(W));
	}
	switch ( req->start ) {
	case 0:
	case 1:
		req->asize = write_pcm(c, req);
		return E_OK;
	case DN_SDPCMMODE: {
		SDPcmMode	m;

		if ( req->size < (SZ)sizeof(m) ) {
			return E_PAR;
		}
		if ( c->ctl != PCM_STOP ) {
			return E_BUSY;
		}
		knl_memcpy(&m, req->buf, sizeof(m));
		if ( m.sampling > PCMST48K || m.datafmt > PCMFmt16 ) {
			return E_NOSPT;
		}
		c->mode = m;
		return E_OK;
	}
	case DN_SDPCMCTL:
		if ( req->size < (SZ)sizeof(W) ) {
			return E_PAR;
		}
		if ( v == PCM_STOP ) {
			lock();
			chan_stop(c);
			unlock();
			return E_OK;
		}
		if ( c->ctl != PCM_STOP ) {
			return E_BUSY;
		}
		if ( v != PCM_REC && v != PCM_PLAY && v != PCM_REPPLAY ) {
			return E_PAR;
		}
		lock();
		er = ( v == PCM_REC ) ? start_rec(c) : start_play(c, v);
		unlock();
		return er;
	case DN_SDPCMENDCNT:
		if ( req->size < (SZ)sizeof(W) ) {
			return E_PAR;
		}
		c->endcnt = ( v <= 0 || v > PCM_COUNT_NONE ) ? PCM_COUNT_NONE : v;
		if ( c->ctl != PCM_STOP && limited(c) && c->count >= (UW)c->endcnt ) {
			lock();
			chan_stop(c);
			unlock();
		}
		return E_OK;
	case DN_SDPCMBUFSZ:
		if ( req->size < (SZ)sizeof(W) ) {
			return E_PAR;
		}
		if ( c->ctl != PCM_STOP ) {
			return E_BUSY;
		}
		if ( v <= 0 ) {
			return E_PAR;
		}
		if ( v > BUF_MAX ) {
			return E_NOMEM;
		}
		lock();
		if ( c->fifo != NULL ) {
			Kfree(c->fifo);
			c->fifo = NULL;
		}
		c->bufsz = v + 1;
		c->head = c->tail = 0;
		er = fifo_alloc(c);
		if ( er < E_OK ) {
			c->bufsz = BUF_DEFAULT + 1;
		}
		unlock();
		return er;
	case DN_SDSELIN:
	case DN_SDSELOUT:
		/* connected, and not to be changed */
		return ( req->size < (SZ)sizeof(SDSel) ) ? E_PAR : E_OK;
	default:
		if ( is_level((INT)req->start) ) {
			SDVol	s;
			CONST SNDHW *hw;

			if ( req->size < (SZ)sizeof(s) ) {
				return E_PAR;
			}
			knl_memcpy(&s, req->buf, sizeof(s));
			lock();
			hw = ( play_hw != NULL ) ? play_hw : hw_play();
			if ( hw == NULL ) {
				hw = hw_record();
			}
			er = ( hw != NULL ) ? hw->set_level((INT)req->start, &s) : E_NOSPT;
			unlock();
			return er;
		}
		return E_PAR;
	}
}

/* ---------------------------------------------------------------- driver */

LOCAL ER snd_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

/*
 * The last close of a unit. The device manager passes on only the first
 * open and the last close of a unit, not each process's, so a channel is
 * made at its first request and let go here, or when its process ends.
 */
LOCAL ER snd_close( ID devid, UINT option, void *exinf )
{
	INT	unit = (INT)(devid - snd_devid);
	INT	i;

	lock();
	for ( i = 0; i < NCH; i++ ) {
		if ( ch[i].used && ch[i].unit == unit ) {
			chan_free(&ch[i]);
		}
	}
	unlock();

	return E_OK;
}

LOCAL ER snd_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	INT	unit = (INT)(req->devid - snd_devid);
	CHAN	*c;

	req->asize = 0;
	lock();
	c = chan_of(unit);
	unlock();
	if ( c == NULL ) {
		req->error = E_LIMIT;		/* too many programs */
		return E_OK;
	}
	req->error = ( req->cmd == TDC_READ ) ? proc_read(c, req) : proc_write(c, req);

	return E_OK;
}

LOCAL INT snd_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER snd_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

/* Suspend: every channel stops */
LOCAL INT snd_event( INT evttyp, void *evtinf, void *exinf )
{
	INT	i;

	if ( evttyp != TDV_SUSPEND ) {
		return E_OK;
	}
	lock();
	for ( i = 0; i < NCH; i++ ) {
		if ( ch[i].used ) {
			chan_stop(&ch[i]);
		}
	}
	unlock();

	return E_OK;
}

EXPORT ER knl_snd_init( void )
{
	T_DDEV	ddev;
	ID	id;

	if ( snd_ready ) {
		return E_OK;
	}
#ifdef RPI5
	knl_i2s_init();
#endif
	ddev.exinf   = NULL;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_SOUND;
	ddev.nsub    = SND_UNITS;
	ddev.blksz   = 1;
	ddev.openfn  = (FP)snd_open;
	ddev.closefn = (FP)snd_close;
	ddev.execfn  = (FP)snd_exec;
	ddev.waitfn  = (FP)snd_wait;
	ddev.abortfn = (FP)snd_abort;
	ddev.eventfn = (FP)snd_event;
	id = tk_def_dev((UB *)SND_DEVNM, &ddev, NULL);
	if ( id <= 0 ) {
		return id;
	}
	snd_devid = id;
	snd_ready = TRUE;

	return E_OK;
}
