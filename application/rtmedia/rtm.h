/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtm.h
 *	実時間メディア: the process that sounds what a program of
 *	実時間制御TAD plays (design 15.6)
 *
 *	Three tasks. The main task reads the desktop's channel and does what
 *	it says; the loader reads and decodes the files it is asked to make
 *	ready, so that a long file being decoded holds up no note; the mixer
 *	makes the sound a few milliseconds at a time -- the files, the notes
 *	of the instruments, the sounds the record carries -- and writes it
 *	to the sound device "snda", keeping it a fixed time ahead of what is
 *	heard. A sound asked for at a time is put at the sample that will be
 *	heard then, plus that fixed time, so two sounds are as far apart as
 *	the program put them.
 *
 *	Floating point is used here (the process's tasks keep the FP
 *	registers), and a sine is read from a table, not worked out.
 */

#ifndef __RTM_H__
#define __RTM_H__

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/uapp.h>
#include <ts/snd.h>
#include <ts/rtad.h>
#include <ts/rtmedia.h>
#include <ts/time.h>

#define FS		48000		/* what is written to the device */
#define LATENCY_NS	80000000LL	/* every sound is heard this long after its time */
#define BLOCK		240		/* frames mixed at a time: 5 ms */
#define DEV_BUF		8192		/* bytes of the device's buffer: 43 ms */
#define MAX_VOICE	48
#define MAX_STREAM	32
#define MAX_EVQ		512		/* sounds waiting for their sample */
#define MAX_FILE	( 64 * 1024 * 1024 )	/* bytes of a file read whole */

/* A file made ready: its sound, 48 kHz, stereo, 16 bit */
typedef struct {
	BOOL		used;
	INT		gen;
	INT		kind;		/* RT_MK_* */
	H		*pcm;
	INT		frames;
	BOOL		loading;
} MEDIA;

/* What an instrument is made of */
#define IN_PIANO	0
#define IN_ORGAN	1
#define IN_GUITAR	2
#define IN_BASS		3
#define IN_STRINGS	4
#define IN_BRASS	5
#define IN_REED		6
#define IN_BELL		7
#define IN_SYNTH	8
#define IN_DRUMS	9
#define IN_FLUTE	10

#define W_SINE		0
#define W_SQUARE	1
#define W_SAW		2
#define W_TRIANGLE	3
#define W_NOISE		4

typedef struct {
	INT	family;			/* IN_* */
	INT	wave;			/* IN_SYNTH: W_* */
	float	attack, decay, sustain, release;	/* seconds, and the level held */
	float	gain;
} INSTR;

/* A voice: a file playing, a note, a sound of the record */
#define V_FILE		1
#define V_NOTE		2
#define V_ONCE		3		/* PCM of its own, played once */

typedef struct {
	BOOL		used;
	INT		kind;
	INT		inst, gen;	/* V_FILE: the medium */
	D		start;		/* the sample it begins at */
	BOOL		started;
	/* a file or a sound */
	H		*pcm;
	INT		frames;
	BOOL		own;		/* V_ONCE: the samples are the voice's */
	D		pos;		/* in 1/65536 frames */
	INT		first, last;	/* where it plays from, to (frames) */
	BOOL		loop;
	INT		loop_a, loop_b;
	INT		fade_in, fade_out;	/* frames */
	float		vol, pan, rate;
	float		vol_now;	/* moved toward vol, against clicks */
	BOOL		muted, paused;
	INT		played;		/* frames sounded since the start */
	D		end_at;		/* the sample where it ended, to tell when heard; -1 */
	D		told_at;	/* the sample of the last place told */
	/* a note */
	INT		chan, key, vel;
	INSTR		ins;
	float		freq;
	D		off_at;		/* the sample its key is let go */
	INT		age;		/* frames since it began */
	INT		rel;		/* frames since it was let go, -1 held */
	UW		ph[4];		/* phases, 32 bit turns */
	float		*ks;		/* a plucked string's delay line */
	INT		ks_len, ks_at;
	UINT		noise;
	float		lp, hp;		/* a drum's filters */
	float		env;		/* the level it has come to */
	float		rlev, rgain;	/* the level when let go, and what is left of it */
	float		amp;		/* from its velocity */
	float		bend;		/* times the frequency, from the channel's pitch bend */
} VOICE;

/* What a channel of MIDI is set to */
typedef struct {
	INT	program;
	float	volume, expr, pan, bend;
	BOOL	sustain;
	INSTR	ins;
	BOOL	fixed;			/* the stream says its instrument */
} CHAN;

/* rtm_main.c */
IMPORT void rtm_answer( UINT op, INT inst, INT gen, D v, INT w, INT h );
IMPORT void rtm_log( CONST char *s );

/* rtm_mix.c */
IMPORT ER   mix_start( PRI pri );
IMPORT void mix_stop( void );
IMPORT void mix_lock( void );
IMPORT void mix_unlock( void );
IMPORT D    mix_sample_at( D ns );		/* the sample heard at that time plus the latency */
IMPORT void mix_play( MEDIA *m, INT inst, INT gen, D at, CONST RTM_CMD *c );
IMPORT void mix_file_ctl( INT inst, UINT op, D v );
IMPORT void mix_note( INT stream, INT chan, INT key, INT vel, D dur_us, D at );
IMPORT void mix_midi( INT stream, CONST UB msg[3], D at );
IMPORT void mix_once( H *pcm, INT frames, float vol, D at );
IMPORT void mix_stream( INT number, CONST UB *device, CONST UB *params );
IMPORT void mix_silence( void );

/* rtm_dec.c */
IMPORT ER   dec_any( CONST UB *data, SZ size, CONST UB *name, CONST UB *format, H **p_pcm, INT *p_frames );
IMPORT ER   dec_raw( CONST UB *data, SZ size, INT rate, INT channels, INT bits, H **p_pcm, INT *p_frames );

/* rtm_synth.c */
IMPORT void syn_init( void );
IMPORT void syn_instr( INSTR *ins, INT family );
IMPORT BOOL syn_device( INSTR *ins, CONST UB *device, CONST UB *params, INT *p_chan );
IMPORT void syn_gm( INSTR *ins, INT program );
IMPORT void syn_start( VOICE *v );
IMPORT BOOL syn_render( VOICE *v, float *l, float *r, INT n, D at );	/* FALSE when it is over */

/* rtm_libc.c */
IMPORT void rtm_libc_init( void );

#endif /* __RTM_H__ */
