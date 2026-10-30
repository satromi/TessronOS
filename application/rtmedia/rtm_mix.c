/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtm_mix.c
 *	The mixer of 実時間メディア and the sound device (design 15.6.2)
 *
 *	The device "snda" is kept playing all the while something is to
 *	sound, fed BLOCK frames at a time as fast as it takes them, so that
 *	what is written is always about a buffer ahead of what is heard.
 *	The frames are counted from the start; the device says how many it
 *	has played, and from that and the clock comes the time each frame
 *	is heard. A sound asked for at time T goes in at the frame heard at
 *	T plus LATENCY_NS: later than any frame already written, and the
 *	same distance from every other sound as the program put it.
 *
 *	The end of a file, and where in it is being heard, is told when it
 *	is heard rather than when it was mixed.
 */

#include "rtm.h"
#include <stdlib.h>
#include <string.h>

static ID	snd = -1;		/* the device */
static ID	lk = -1;		/* the voices, one task at a time */
static ID	tid = -1;
static volatile BOOL	stop = FALSE;
static BOOL	running = FALSE;
static D	written = 0;		/* frames given to the device since it started */
static D	t0 = 0;			/* ns: when frame 0 is heard */
static D	heard = 0;		/* frames heard, as last asked */
static D	idle_since = 0;

static VOICE	voice[MAX_VOICE];

/* MIDI messages waiting for their frame */
typedef struct {
	D	at;
	INT	stream;
	UB	msg[3];
} EVQ;
static EVQ	evq[MAX_EVQ];
static INT	nevq = 0;

/* What each stream plays on, and its sixteen channels */
static struct {
	BOOL	used;
	INT	number;
	INSTR	ins;
	INT	chan;			/* its notes' channel, 1 .. 16 */
	CHAN	ch[16];
} st[MAX_STREAM];

/* Something heard to be told */
typedef struct {
	D	at;			/* the frame */
	UINT	op;
	INT	inst, gen;
} TELL;
static TELL	tell[64];
static INT	ntell = 0;

static D now_ns( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (D)ns;
}

EXPORT void mix_lock( void )
{
	if ( lk > 0 ) (void)tk_wai_sem(lk, 1, TMO_FEVR);
}

EXPORT void mix_unlock( void )
{
	if ( lk > 0 ) (void)tk_sig_sem(lk, 1);
}

/* ---------------------------------------------------------------- the device */

static ER snd_w( INT rec, INT v )
{
	SZ	asz = 0;
	W	w = v;

	return ob_wri_rec(snd, rec, 0, &w, sizeof(w), &asz);
}

static INT snd_r( INT rec )
{
	SZ	asz = 0;
	W	w = 0;

	if ( ob_rea_rec(snd, rec, 0, &w, sizeof(w), &asz) < E_OK ) return -1;
	return (INT)w;
}

/*
 * Every object of a kind, however many: asked a page at a time, each
 * page from the last object of the one before. How many, the list in
 * *p_ids (free it).
 */
static INT objs_all( UINT type, UINT sub, TS_UUID **p_ids )
{
	TS_UUID	*ids = NULL, *ni;
	INT	n = 0, got;

	*p_ids = NULL;
	for ( ;; ) {
		ni = (TS_UUID *)realloc(ids, sizeof(TS_UUID) * (size_t)( n + 64 ));
		if ( ni == NULL ) break;
		ids = ni;
		got = 0;
		if ( ob_lst_obj(type, sub, ( n > 0 ) ? &ids[n - 1] : NULL, ids + n, 64, &got) < E_OK ) break;
		n += got;
		if ( got < 64 ) break;
	}
	*p_ids = ids;
	return n;
}

static ER dev_open( void )
{
	TS_UUID		*list;
	INT		cnt, i;
	SDPcmMode	m;
	SZ		asz = 0;

	if ( snd > 0 ) return E_OK;
	/* the first sound device that opens, of every one there is */
	cnt = objs_all(OB_T_DEVICE, OB_S_SOUND, &list);
	for ( i = 0; i < cnt && snd <= 0; i++ ) {
		snd = ob_opn_obj(&list[i], OB_OP_R | OB_OP_WRITE);
	}
	free(list);
	if ( snd <= 0 ) {
		snd = -1;
		return E_NOEXS;
	}
	(void)snd_w(OB_SND_CTL, PCM_STOP);
	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	(void)ob_wri_rec(snd, OB_SND_MODE, 0, &m, sizeof(m), &asz);
	(void)snd_w(OB_SND_BUFSZ, DEV_BUF);
	(void)snd_w(OB_SND_ENDCNT, 0);
	return E_OK;
}

/* Begun: a buffer of silence, then played; frame 0 is heard about now */
static void dev_start( void )
{
	static H	zero[DEV_BUF / 2];
	SZ		asz = 0;

	if ( snd <= 0 && dev_open() < E_OK ) return;
	memset(zero, 0, sizeof(zero));
	(void)snd_w(OB_SND_CTL, PCM_STOP);
	(void)ob_wri_rec(snd, OB_SND_PCM, 0, zero, DEV_BUF / 2, &asz);
	written = (D)asz / 4;
	heard = 0;
	if ( snd_w(OB_SND_CTL, PCM_PLAY) >= E_OK ) {
		running = TRUE;
		t0 = now_ns();
	}
}

static void dev_stop( void )
{
	if ( snd > 0 ) (void)snd_w(OB_SND_CTL, PCM_STOP);
	running = FALSE;
}

/* How far the device has got, and from that when frame 0 was heard */
static void dev_where( void )
{
	INT	cnt;
	D	t, est;

	if ( !running ) return;
	cnt = snd_r(OB_SND_CNT);
	if ( cnt < 0 ) return;
	heard = (D)cnt / 4;
	t = now_ns();
	est = t - heard * 1000000000LL / FS;
	/* a little each time, so the jitter of the reading does not shake it */
	t0 += ( est - t0 ) / 8;
}

EXPORT D mix_sample_at( D ns )
{
	D	s;

	if ( !running ) return written;
	if ( ns <= 0 ) return written;
	s = ( ns + LATENCY_NS - t0 ) * FS / 1000000000LL;
	return ( s < written ) ? written : s;
}

/* ---------------------------------------------------------------- voices */

static VOICE *voice_new( void )
{
	INT	i, oldest = -1;

	for ( i = 0; i < MAX_VOICE; i++ ) {
		if ( !voice[i].used ) {
			memset(&voice[i], 0, sizeof(voice[i]));
			voice[i].used = TRUE;
			voice[i].end_at = -1;
			voice[i].off_at = -1;
			return &voice[i];
		}
		if ( voice[i].kind == V_NOTE && ( oldest < 0 || voice[i].age > voice[oldest].age ) ) oldest = i;
	}
	if ( oldest < 0 ) return NULL;
	/* the oldest note gives way */
	if ( voice[oldest].ks != NULL ) free(voice[oldest].ks);
	memset(&voice[oldest], 0, sizeof(voice[oldest]));
	voice[oldest].used = TRUE;
	voice[oldest].end_at = -1;
	voice[oldest].off_at = -1;
	return &voice[oldest];
}

static void voice_free( VOICE *v )
{
	if ( v->ks != NULL ) free(v->ks);
	if ( v->own && v->pcm != NULL ) free(v->pcm);
	memset(v, 0, sizeof(*v));
}

static INT us_frames( D us )
{
	return (INT)( us * FS / 1000000 );
}

EXPORT void mix_play( MEDIA *m, INT inst, INT gen, D at, CONST RTM_CMD *c )
{
	VOICE	*v;
	INT	i;

	mix_lock();
	/* one voice for each medium */
	for ( i = 0; i < MAX_VOICE; i++ ) {
		if ( voice[i].used && voice[i].kind == V_FILE && voice[i].inst == inst ) voice_free(&voice[i]);
	}
	v = voice_new();
	if ( v != NULL ) {
		v->kind = V_FILE;
		v->inst = inst;
		v->gen = gen;
		v->pcm = m->pcm;
		v->frames = m->frames;
		v->first = us_frames(c->u.play.start + c->u.play.skip);
		if ( v->first > m->frames ) v->first = m->frames;
		v->last = ( c->u.play.dur > 0 ) ? us_frames(c->u.play.start + c->u.play.dur) : m->frames;
		if ( v->last > m->frames || v->last <= 0 ) v->last = m->frames;
		v->pos = (D)v->first << 16;
		v->loop = (BOOL)c->u.play.loop;
		v->loop_a = us_frames(c->u.play.loop_start);
		v->loop_b = ( c->u.play.loop_end > 0 ) ? us_frames(c->u.play.loop_end) : v->last;
		if ( v->loop_b > v->last ) v->loop_b = v->last;
		if ( v->loop_a >= v->loop_b ) v->loop_a = 0;
		v->fade_in = us_frames(c->u.play.fade_in);
		v->fade_out = us_frames(c->u.play.fade_out);
		v->vol = (float)c->u.play.volume / 1000000.0f;
		v->vol_now = v->vol;
		v->pan = (float)c->u.play.pan / 1000000.0f;
		v->rate = ( c->u.play.rate > 0 ) ? (float)c->u.play.rate / 1000000.0f : 1.0f;
		v->muted = (BOOL)c->u.play.muted;
		v->start = mix_sample_at(at);
		v->told_at = v->start;
	}
	idle_since = 0;
	mix_unlock();
	if ( tid > 0 ) (void)tk_wup_tsk(tid);
}

EXPORT void mix_file_ctl( INT inst, UINT op, D val )
{
	INT	i;

	mix_lock();
	for ( i = 0; i < MAX_VOICE; i++ ) {
		VOICE	*v = &voice[i];

		if ( !v->used || v->kind != V_FILE || v->inst != inst ) continue;
		switch ( op ) {
		case RTM_PAUSE:		v->paused = TRUE; break;
		case RTM_RESUME:	v->paused = FALSE; break;
		case RTM_STOP:
		case RTM_CLOSE:		voice_free(v); break;
		case RTM_SEEK:		v->pos = (D)us_frames(val) << 16; break;
		case RTM_VOLUME:	v->vol = (float)val / 1000000.0f; break;
		case RTM_PAN:		v->pan = (float)val / 1000000.0f; break;
		case RTM_RATE:		v->rate = ( val > 0 ) ? (float)val / 1000000.0f : 1.0f; break;
		case RTM_MUTE:		v->muted = (BOOL)( val != 0 ); break;
		default:		break;
		}
	}
	mix_unlock();
}

static INT stream_of( INT number )
{
	INT	i, free_at = -1;

	for ( i = 0; i < MAX_STREAM; i++ ) {
		if ( st[i].used && st[i].number == number ) return i;
		if ( !st[i].used && free_at < 0 ) free_at = i;
	}
	if ( free_at < 0 ) return 0;
	/* a stream nobody described: the piano on channel 1 */
	st[free_at].used = TRUE;
	st[free_at].number = number;
	st[free_at].chan = 1;
	syn_instr(&st[free_at].ins, IN_PIANO);
	for ( i = 0; i < 16; i++ ) {
		syn_instr(&st[free_at].ch[i].ins, ( i == 9 ) ? IN_DRUMS : IN_PIANO);
		st[free_at].ch[i].volume = 1.0f;
		st[free_at].ch[i].expr = 1.0f;
		st[free_at].ch[i].bend = 1.0f;
	}
	return free_at;
}

EXPORT void mix_stream( INT number, CONST UB *device, CONST UB *params )
{
	INT	s, i, chan = 1;
	INSTR	ins;

	mix_lock();
	s = stream_of(number);
	(void)syn_device(&ins, device, params, &chan);
	if ( chan < 1 || chan > 16 ) chan = 1;
	st[s].ins = ins;
	st[s].chan = chan;
	for ( i = 0; i < 16; i++ ) {
		st[s].ch[i].ins = ( i == 9 ) ? st[s].ch[i].ins : ins;
		st[s].ch[i].fixed = TRUE;
	}
	if ( chan == 10 ) syn_instr(&st[s].ch[9].ins, IN_DRUMS);
	mix_unlock();
}

static void note_on( INT s, INT ch, INT key, INT vel, D at, D off )
{
	VOICE	*v = voice_new();
	CHAN	*c = &st[s].ch[ch];

	if ( v == NULL ) return;
	v->kind = V_NOTE;
	v->chan = ch;
	v->key = key;
	v->vel = vel;
	v->ins = c->ins;
	v->ins.gain *= c->volume * c->expr;
	v->pan = c->pan;
	v->bend = c->bend;
	v->start = at;
	v->off_at = off;
	syn_start(v);
}

EXPORT void mix_note( INT stream, INT chan, INT key, INT vel, D dur_us, D at )
{
	INT	s, ch;
	D	from;

	mix_lock();
	s = stream_of(stream);
	ch = ( chan >= 1 && chan <= 16 ) ? chan - 1 : st[s].chan - 1;
	from = mix_sample_at(at);
	note_on(s, ch, key, vel, from, from + dur_us * FS / 1000000);
	idle_since = 0;
	mix_unlock();
	if ( tid > 0 ) (void)tk_wup_tsk(tid);
}

EXPORT void mix_midi( INT stream, CONST UB msg[3], D at )
{
	mix_lock();
	if ( nevq < MAX_EVQ ) {
		evq[nevq].at = mix_sample_at(at);
		evq[nevq].stream = stream;
		memcpy(evq[nevq].msg, msg, 3);
		nevq++;
	}
	idle_since = 0;
	mix_unlock();
	if ( tid > 0 ) (void)tk_wup_tsk(tid);
}

/* One MIDI message, at its frame */
static void midi_do( INT s, CONST UB *m, D at )
{
	INT	ch = m[0] & 0x0F, i;
	CHAN	*c = &st[s].ch[ch];

	switch ( m[0] & 0xF0 ) {
	case 0x90:
		if ( m[2] > 0 ) {
			note_on(s, ch, m[1] & 0x7F, m[2] & 0x7F, at, -1);
			break;
		}
		/* velocity 0: let go */
		/* fall through */
	case 0x80:
		for ( i = 0; i < MAX_VOICE; i++ ) {
			VOICE	*v = &voice[i];

			if ( v->used && v->kind == V_NOTE && v->chan == ch && v->key == ( m[1] & 0x7F )
			  && v->off_at < 0 && !c->sustain ) {
				v->off_at = at;
			}
		}
		break;
	case 0xB0:
		switch ( m[1] ) {
		case 7:		c->volume = m[2] / 127.0f; break;
		case 10:	c->pan = ( m[2] - 64 ) / 64.0f; break;
		case 11:	c->expr = m[2] / 127.0f; break;
		case 64:	c->sustain = (BOOL)( m[2] >= 64 ); break;
		case 120:
		case 123:
			for ( i = 0; i < MAX_VOICE; i++ ) {
				if ( voice[i].used && voice[i].kind == V_NOTE && voice[i].chan == ch ) voice[i].off_at = at;
			}
			break;
		default:	break;
		}
		break;
	case 0xC0:
		if ( ch != 9 ) syn_gm(&c->ins, m[1] & 0x7F);
		c->program = m[1] & 0x7F;
		break;
	case 0xE0: {
		INT	b = ( ( m[2] & 0x7F ) << 7 | ( m[1] & 0x7F ) ) - 8192;

		/* two semitones either way */
		c->bend = 1.0f + b / 8192.0f * 0.12246f;
		for ( i = 0; i < MAX_VOICE; i++ ) {
			if ( voice[i].used && voice[i].kind == V_NOTE && voice[i].chan == ch ) voice[i].bend = c->bend;
		}
		break;
	}
	default:
		break;
	}
}

EXPORT void mix_once( H *pcm, INT frames, float vol, D at )
{
	VOICE	*v;

	mix_lock();
	v = voice_new();
	if ( v != NULL ) {
		v->kind = V_ONCE;
		v->own = TRUE;
		v->pcm = pcm;
		v->frames = frames;
		v->last = frames;
		v->vol = v->vol_now = vol;
		v->rate = 1.0f;
		v->start = mix_sample_at(at);
	} else {
		free(pcm);
	}
	idle_since = 0;
	mix_unlock();
	if ( tid > 0 ) (void)tk_wup_tsk(tid);
}

EXPORT void mix_silence( void )
{
	INT	i;

	mix_lock();
	for ( i = 0; i < MAX_VOICE; i++ ) {
		if ( voice[i].used ) voice_free(&voice[i]);
	}
	nevq = 0;
	ntell = 0;
	mix_unlock();
}

/* ---------------------------------------------------------------- mixing */

static float	accl[BLOCK], accr[BLOCK];
static H	out[BLOCK * 2];

static void tell_later( D at, UINT op, INT inst, INT gen )
{
	if ( ntell < 64 ) {
		tell[ntell].at = at;
		tell[ntell].op = op;
		tell[ntell].inst = inst;
		tell[ntell].gen = gen;
		ntell++;
	}
}

/* A file's or a sound's frames from 'from' (an offset into the block) on */
static void file_render( VOICE *v, INT from, D s0 )
{
	INT	i;
	float	gl, gr;

	gl = ( v->pan <= 0 ) ? 1.0f : 1.0f - v->pan;
	gr = ( v->pan >= 0 ) ? 1.0f : 1.0f + v->pan;
	for ( i = from; i < BLOCK; i++ ) {
		INT	k = (INT)( v->pos >> 16 ), f = (INT)( v->pos & 0xFFFF );
		float	a, b, g;

		if ( v->paused ) return;
		if ( k >= v->last || ( v->loop && k >= v->loop_b ) ) {
			if ( v->loop && v->loop_b > v->loop_a ) {
				v->pos = (D)v->loop_a << 16;
				k = v->loop_a;
			} else {
				if ( v->kind == V_FILE ) {
					v->end_at = s0 + i;
					tell_later(v->end_at, RTM_A_ENDED, v->inst, v->gen);
				}
				v->used = FALSE;		/* freed after the block */
				return;
			}
		}
		/* toward the volume asked, a step each frame, against a click */
		v->vol_now += ( v->vol - v->vol_now ) * 0.002f;
		g = v->muted ? 0.0f : v->vol_now;
		if ( v->fade_in > 0 && v->played < v->fade_in ) g *= (float)v->played / v->fade_in;
		if ( v->fade_out > 0 && !v->loop && v->last - k < v->fade_out ) g *= (float)( v->last - k ) / v->fade_out;
		a = v->pcm[2 * k];
		b = v->pcm[2 * k + 1];
		if ( k + 1 < v->frames && f != 0 ) {
			a += ( v->pcm[2 * ( k + 1 )] - a ) * f / 65536.0f;
			b += ( v->pcm[2 * ( k + 1 ) + 1] - b ) * f / 65536.0f;
		}
		accl[i] += a / 32768.0f * g * gl;
		accr[i] += b / 32768.0f * g * gr;
		v->pos += (D)( v->rate * 65536.0f );
		v->played++;
	}
}

static void mix_block( D s0 )
{
	INT	i, k;

	memset(accl, 0, sizeof(accl));
	memset(accr, 0, sizeof(accr));
	/* the MIDI messages due in this block */
	for ( i = 0; i < nevq; ) {
		if ( evq[i].at < s0 + BLOCK ) {
			midi_do(stream_of(evq[i].stream), evq[i].msg, ( evq[i].at > s0 ) ? evq[i].at : s0);
			evq[i] = evq[--nevq];
		} else {
			i++;
		}
	}
	for ( k = 0; k < MAX_VOICE; k++ ) {
		VOICE	*v = &voice[k];
		INT	from;

		if ( !v->used || v->start >= s0 + BLOCK ) continue;
		from = ( v->start > s0 ) ? (INT)( v->start - s0 ) : 0;
		if ( v->kind == V_NOTE ) {
			if ( !syn_render(v, accl + from, accr + from, BLOCK - from, s0 + from) ) voice_free(v);
		} else {
			file_render(v, from, s0);
			if ( !v->used ) {
				v->used = TRUE;
				voice_free(v);
			}
		}
	}
	for ( i = 0; i < BLOCK; i++ ) {
		float	l = accl[i] * 32767.0f, r = accr[i] * 32767.0f;

		if ( l > 32767.0f ) l = 32767.0f;
		if ( l < -32768.0f ) l = -32768.0f;
		if ( r > 32767.0f ) r = 32767.0f;
		if ( r < -32768.0f ) r = -32768.0f;
		out[2 * i] = (H)l;
		out[2 * i + 1] = (H)r;
	}
}

static BOOL busy( void )
{
	INT	i;

	if ( nevq > 0 || ntell > 0 ) return TRUE;
	for ( i = 0; i < MAX_VOICE; i++ ) {
		if ( voice[i].used ) return TRUE;
	}
	return FALSE;
}

/* What has been heard is told: the ends, and where each file is */
static void tell_heard( void )
{
	INT	i;
	D	h = heard;

	for ( i = 0; i < ntell; ) {
		if ( tell[i].at <= h ) {
			rtm_answer(tell[i].op, tell[i].inst, tell[i].gen, 0, 0, 0);
			tell[i] = tell[--ntell];
		} else {
			i++;
		}
	}
	for ( i = 0; i < MAX_VOICE; i++ ) {
		VOICE	*v = &voice[i];

		if ( v->used && v->kind == V_FILE && !v->paused && h - v->told_at >= FS / 4 ) {
			D	lag = written - h;
			D	at = ( v->pos >> 16 ) - lag;

			v->told_at = h;
			rtm_answer(RTM_A_TIME, v->inst, v->gen, ( at > 0 ? at : 0 ) * 1000000 / FS, 0, 0);
		}
	}
}

static void mix_task( INT stacd, void *exinf )
{
	SZ	pend = 0, sent = 0;
	D	last_where = 0;

	(void)stacd;
	(void)exinf;
	while ( !stop ) {
		D	now = now_ns();

		mix_lock();
		if ( !busy() && pend == 0 ) {
			if ( idle_since == 0 ) idle_since = now;
			if ( running && now - idle_since > 5000000000LL ) dev_stop();
			mix_unlock();
			(void)tk_slp_tsk(running ? 5 : 200);
			continue;
		}
		idle_since = 0;
		if ( !running ) {
			dev_start();
			if ( !running ) {
				mix_unlock();
				(void)tk_slp_tsk(500);
				continue;
			}
		}
		if ( now - last_where > 100000000LL ) {
			dev_where();
			last_where = now;
			if ( snd_r(OB_SND_CTL) == PCM_STOP ) {
				/* it ran dry and stopped: begun again, the clock with it */
				running = FALSE;
				dev_start();
			}
		}
		if ( pend == 0 ) {
			mix_block(written);
			pend = sizeof(out);
			sent = 0;
		}
		{
			SZ	asz = 0;

			if ( ob_wri_rec(snd, OB_SND_PCM, 0, (UB *)out + sent, pend - sent, &asz) < E_OK ) asz = 0;
			sent += asz;
			if ( sent >= pend ) {
				written += BLOCK;
				pend = 0;
			}
			tell_heard();
			mix_unlock();
			if ( asz == 0 ) (void)tk_dly_tsk(2);
		}
	}
	tk_ext_tsk();
}

EXPORT ER mix_start( PRI pri )
{
	T_CSEM	cs;
	T_CTSK	ct;
	ER	er;

	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 1;
	cs.maxsem = 1;
	lk = tk_cre_sem(&cs);
	if ( lk < E_OK ) return lk;
	(void)dev_open();
	memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG3;
	ct.task = (FP)mix_task;
	ct.itskpri = pri;		/* the process's own: the highest it may have */
	ct.stksz = 64 * 1024;
	tid = tk_cre_tsk(&ct);
	if ( tid < E_OK ) return tid;
	er = tk_sta_tsk(tid, 0);
	return er;
}

EXPORT void mix_stop( void )
{
	stop = TRUE;
	mix_silence();
	dev_stop();
}
