/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtad.h
 *	実時間制御TAD: the engine that plays the <realtime> part of an
 *	xmlTAD record (design 15)
 *
 *	The engine reads the elements of a <realtime> once into a program
 *	and runs it against a clock it is given. It keeps the state of
 *	everything the program moves -- the shapes, texts, pictures and
 *	videos on the screen, the sounds and their media -- and tells
 *	whoever plays it what to sound and what to load, through the
 *	functions of an RT_HOST. It draws nothing, reads no file, makes no
 *	system call and uses no floating point, so the desktop (in the
 *	kernel), a process and the kernel's tests run the same code
 *	(lib/librtad).
 *
 *	Times are microseconds of the clock the caller passes in (RT_TIME);
 *	every other quantity -- a length, an alpha, an angle, a volume -- is
 *	kept in millionths (RT_V), so 0.5 is 500000. Values are read from
 *	the record's decimal text without rounding through a float.
 *
 *	The record must stay parsed for as long as the program runs: the
 *	program points into its attribute values rather than copying them.
 */

#ifndef __TS_RTAD_H__
#define __TS_RTAD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/tad.h>
#include <ts/dp.h>

typedef D	RT_TIME;		/* microseconds */
typedef D	RT_V;			/* millionths */

#define RT_NEVER	((RT_TIME)0x7FFFFFFFFFFFFFFFLL)
#define RT_ONE		((RT_V)1000000)

typedef struct rt	RT;		/* a program and its run */


/* ---------------------------------------------------------------- what is on the screen */

#define RT_OB_SHAPE	1		/* a shape of a figure, by its id */
#define RT_OB_TEXT	2		/* a text box (<document>), by the id of its <text> */
#define RT_OB_IMAGE	3		/* a picture: of a figure, or an <image> of the program */
#define RT_OB_VIDEO	4		/* a <video> */

/* The properties a control, an interpolation or a keyframe moves */
#define RT_P_LEFT	0		/* the box, in the units of the scene */
#define RT_P_TOP	1
#define RT_P_RIGHT	2
#define RT_P_BOTTOM	3
#define RT_P_ALPHA	4		/* 0 .. RT_ONE */
#define RT_P_ROT	5		/* degrees, clockwise about the middle */
#define RT_P_SX		6		/* scale across and down, RT_ONE life size */
#define RT_P_SY		7
#define RT_NP		8

/* What changed since the host last looked (RT_OBJ.dirty) */
#define RT_D_BOX	0x0001
#define RT_D_LOOK	0x0002		/* alpha, turn, scale, flip */
#define RT_D_SHOWN	0x0004
#define RT_D_Z		0x0008
#define RT_D_PICT	0x0010		/* another picture (imageControl replace) */

typedef struct {
	CONST UB	*id;		/* its id, or a picture's href when it has none */
	UINT		kind;		/* RT_OB_* */
	RT_V		p[RT_NP];
	INT		z;		/* its place in the order things are drawn */
	BOOL		shown;
	BOOL		flip_h, flip_v;
	BOOL		bound;		/* the host has said where it is */
	CONST UB	*href;		/* a picture's file, a video's poster */
	INT		media;		/* a video: its media, else -1 */
	UINT		dirty;		/* RT_D_* */
	CONST T_TADNODE	*nd;		/* the element it was first met in */
	RT_V		base[RT_NP];	/* as the record has it, for fill="none" */
	INT		base_z;
	BOOL		base_shown;
	CONST UB	*base_href;
	void		*user;		/* the host's */
} RT_OBJ;

/* ---------------------------------------------------------------- media */

#define RT_MK_AUDIO	1
#define RT_MK_VIDEO	2
#define RT_MK_PCM	3		/* a <pcm> that names a file or carries its data */

#define RT_MS_IDLE	0		/* declared, nothing asked yet */
#define RT_MS_LOADING	1
#define RT_MS_READY	2		/* loaded, not playing */
#define RT_MS_PLAYING	3
#define RT_MS_PAUSED	4
#define RT_MS_ENDED	5
#define RT_MS_STOPPED	6
#define RT_MS_ERROR	7

#define RT_TR_TIME	0		/* trigger="time" */
#define RT_TR_MANUAL	1
#define RT_TR_LOAD	2
#define RT_TR_VISIBLE	3

#define RT_PRE_NONE	0
#define RT_PRE_META	1
#define RT_PRE_AUTO	2

/* Why a medium failed (<onerror type>) */
#define RT_ERR_NOTFOUND	0x01
#define RT_ERR_DECODE	0x02
#define RT_ERR_NETWORK	0x04
#define RT_ERR_TIMEOUT	0x08
#define RT_ERR_ABORTED	0x10
#define RT_ERR_UNSUPP	0x20		/* a format nothing here plays (<fallback condition="unsupported">) */
#define RT_ERR_ALL	0x3F

typedef struct {
	CONST UB	*id;
	UINT		kind;		/* RT_MK_* */
	UINT		state;		/* RT_MS_* */
	CONST T_TADNODE	*nd;		/* the element */
	CONST UB	*href;		/* the file it plays now: its own or a fallback's */
	CONST UB	*format;
	INT		nfall;		/* fallbacks tried */
	INT		tries;		/* times the file was tried again (onerror retry) */
	UINT		err;		/* RT_ERR_* of the last failure */
	BOOL		autoplay;
	UINT		trigger;	/* RT_TR_* */
	UINT		preload;	/* RT_PRE_* */
	RT_V		volume, pan, rate;
	RT_TIME		start, dur;	/* where in the file to begin, how much (0 all) */
	BOOL		loop;
	RT_TIME		loop_start, loop_end;
	RT_TIME		fade_in, fade_out;
	RT_TIME		timeout;	/* to wait for it to load, 0 for ever */
	RT_TIME		total;		/* totalDuration, 0 not said */
	BOOL		muted;
	INT		track;		/* audioTrack */
	CONST UB	*onended;
	CONST UB	*title, *artist, *album;
	INT		trackno;
	INT		sample_rate, channels, bits;	/* what the record says of it */
	INT		obj;		/* a video: its display object, else -1 */
	RT_TIME		pos;		/* where it is in the file, as last told */
	RT_TIME		asked;		/* when it was last asked to load (for timeout) */
	BOOL		play_on_load;	/* to start once loaded */
	RT_TIME		play_at;	/* the time it was to have started */
	RT_TIME		skip;		/* how far into it to start (a seek of the program) */
	INT		gen;		/* counts its starts, so an old instance's word is not taken */
	BOOL		loaded;		/* the host has it ready */
	RT_TIME		retry_at;	/* <onerror retry>: when to try again, 0 not */
	INT		thr;		/* the thread whose element started it, -1 */
	void		*user;		/* the host's */
} RT_MEDIA;

/* What the engine asks of the media (RT_HOST.media) */
#define RT_MO_LOAD	1		/* get the file ready: href, format, preload */
#define RT_MO_PLAY	2		/* from start (+ skip), with every setting */
#define RT_MO_PAUSE	3
#define RT_MO_RESUME	4
#define RT_MO_STOP	5
#define RT_MO_SEEK	6		/* v: microseconds into the file */
#define RT_MO_VOLUME	7		/* v: RT_ONE the loudest */
#define RT_MO_PAN	8		/* v: -RT_ONE left .. RT_ONE right */
#define RT_MO_RATE	9		/* v: RT_ONE as recorded */
#define RT_MO_MUTE	10		/* v: 1 muted, 0 not */
#define RT_MO_CLOSE	11		/* not wanted any more */

/* What the media tell the engine (rt_media_event) */
#define RT_ME_LOADED	1		/* v: its length in microseconds, 0 not known */
#define RT_ME_PLAYING	2
#define RT_ME_PAUSED	3
#define RT_ME_ENDED	4
#define RT_ME_ERROR	5		/* v: RT_ERR_* */
#define RT_ME_TIME	6		/* v: where it is, microseconds into the file */

/* ---------------------------------------------------------------- sound */

#define RT_SND_NOTE	1
#define RT_SND_MIDI	2
#define RT_SND_PCM	3		/* <pcm> carrying its data: nd is the element */

#define RT_MIDI_MAX	16

typedef struct {
	UINT		kind;
	RT_TIME		at;		/* when it is to sound, on the engine's clock */
	INT		stream;		/* the stream its group says, 0 none */
	INT		pitch;		/* a note: MIDI number */
	INT		velocity;
	INT		channel;	/* 1 .. 16, 0 the stream's */
	RT_TIME		dur;
	INT		nmsg;		/* MIDI: messages, status and two data each */
	UB		msg[RT_MIDI_MAX][3];
	CONST T_TADNODE	*nd;
} RT_SND;

/* A <stream>: what device the events of a group go to */
typedef struct {
	INT		number;
	CONST UB	*device;	/* <deviceName>, e.g. "inst:Piano", "MIDI0:", "audio:" */
	CONST UB	*params;	/* <deviceParams> */
	CONST UB	*data;		/* <streamData>, Base64 as written */
} RT_STREAM;

/* ---------------------------------------------------------------- input */

#define RT_IN_CLICK	1
#define RT_IN_DBLCLICK	2
#define RT_IN_KEYDOWN	3
#define RT_IN_KEYUP	4
#define RT_IN_MOVE	5
#define RT_IN_SWIPE_L	6
#define RT_IN_SWIPE_R	7
#define RT_IN_SWIPE_U	8
#define RT_IN_SWIPE_D	9
#define RT_IN_RESIZE	10
#define RT_IN_VISIBLE	11		/* shown (x 1) or hidden (x 0) */

typedef struct {
	UINT		kind;		/* RT_IN_* */
	INT		x, y;		/* the pointer, in the units of the scene */
	CONST char	*key;		/* a key's name: "Space", "ArrowLeft", "a", "5" */
	UINT		code;		/* and its HID usage */
} RT_IN;

/* ---------------------------------------------------------------- the host */

/* What the engine wants done (RT_HOST.action) */
#define RT_ACT_NEXT	1		/* slide:next */
#define RT_ACT_PREV	2		/* slide:prev */
#define RT_ACT_INDEX	3		/* slide:index:N (n) */
#define RT_ACT_DONE	4		/* every <realData> has played to its end */

typedef struct {
	void	*arg;
	void	*(*alloc)( void *arg, SZ size );	/* zeroed */
	void	(*free)( void *arg, void *p );
	/* the media: inst is the index of rt_media(); v as the op says */
	void	(*media)( void *arg, RT *rt, INT inst, UINT op, RT_V v );
	/* a note, a MIDI message, a sound the record carries, to sound at s->at */
	void	(*sound)( void *arg, RT *rt, CONST RT_SND *s );
	void	(*action)( void *arg, RT *rt, UINT act, INT n );
	/* a line for the console, without its end */
	void	(*log)( void *arg, CONST char *s );
} RT_HOST;

/* ---------------------------------------------------------------- the calls */

/*
 * The program of a <realtime> element: E_NOEXS when there is none,
 * E_LIMIT past what one program may hold. 'rt_find' gives the
 * <realtime> of a record: the root of a 実時間TAD, or one inside its
 * <document> or <figure>.
 */
IMPORT CONST T_TADNODE *rt_find( CONST T_TAD *doc );
IMPORT ER   rt_new( CONST T_TADNODE *realtime, CONST RT_HOST *host, RT **p_rt );
IMPORT void rt_delete( RT *rt );

/*
 * The display objects. The program knows which ids it moves; the host
 * that lays out the record says where each of them is (rt_bind) before
 * the run starts. The pictures and videos the program brings in itself
 * are made when it is read, not shown until their event.
 */
IMPORT BOOL rt_wants( CONST RT *rt, CONST UB *id );
IMPORT INT  rt_bind( RT *rt, CONST UB *id, UINT kind, CONST T_DPRECT *box, INT z,
		     CONST T_TADNODE *nd );
IMPORT INT  rt_nobj( CONST RT *rt );
IMPORT RT_OBJ *rt_obj( RT *rt, INT i );
IMPORT INT  rt_obj_find( CONST RT *rt, CONST UB *id );
/* the hrefs an imageControl replace may put on an object (by its id, *p_ref), one after another; NULL at the end */
IMPORT CONST UB *rt_replace_href( CONST RT *rt, INT i, CONST UB **p_ref );

IMPORT INT  rt_nmedia( CONST RT *rt );
IMPORT RT_MEDIA *rt_media( RT *rt, INT i );
IMPORT INT  rt_nstream( CONST RT *rt );
IMPORT CONST RT_STREAM *rt_stream( CONST RT *rt, INT i );
IMPORT CONST RT_STREAM *rt_stream_of( CONST RT *rt, INT number );

/* Whether the program sounds at all: notes, MIDI, PCM or audio */
IMPORT BOOL rt_sounds( CONST RT *rt );

/* What <realtime> says: autoplay (TRUE plays at once), preload, loop */
IMPORT BOOL rt_autoplay( CONST RT *rt );
IMPORT BOOL rt_loops( CONST RT *rt );
IMPORT UINT rt_preload( CONST RT *rt );

/*
 * Running. rt_start begins at 'now' on the clock given, rt_step does
 * everything due up to 'now' and answers when it next has something to
 * do (RT_NEVER: only an event from outside will wake it). While
 * anything moves smoothly the answer is the next frame (RT_FRAME_US).
 * rt_pause holds the program's time still and rt_resume lets it go on
 * from where it stood. rt_seek starts it again and runs it forward to
 * 'to' microseconds of program time without sounding what is past.
 */
#define RT_FRAME_US	16667		/* a frame at 60 a second */

IMPORT void    rt_start( RT *rt, RT_TIME now );
/*
 * A <realtime> or <realData> that does not play by itself (autoplay
 * "false") stands waiting after rt_start, its scene shown, until the
 * person starts it: rt_begin.
 */
IMPORT BOOL    rt_waiting( CONST RT *rt );
IMPORT void    rt_begin( RT *rt, RT_TIME now );
IMPORT RT_TIME rt_step( RT *rt, RT_TIME now );
IMPORT void    rt_pause( RT *rt, RT_TIME now );
IMPORT void    rt_resume( RT *rt, RT_TIME now );
IMPORT void    rt_stop( RT *rt );
IMPORT void    rt_seek( RT *rt, RT_TIME to, RT_TIME now );
IMPORT BOOL    rt_running( CONST RT *rt );
IMPORT BOOL    rt_paused( CONST RT *rt );
IMPORT BOOL    rt_done( CONST RT *rt );
IMPORT RT_TIME rt_now( CONST RT *rt );		/* program time at the last step */

/* A medium played as a person chose it, the others of its kind playing stopped */
IMPORT void rt_play_only( RT *rt, INT inst, RT_TIME now );

/* What happened to a medium; answered at the next step */
IMPORT void rt_media_event( RT *rt, INT inst, INT gen, UINT ev, RT_V v, RT_TIME now );

/*
 * Something the person did. TRUE when a <trigger> or a <wait> of the
 * program took it; rt_would_take answers the same without doing it,
 * so a slide can leave a key it would otherwise use to the program.
 */
IMPORT BOOL rt_input( RT *rt, CONST RT_IN *in, RT_TIME now );
IMPORT BOOL rt_would_take( CONST RT *rt, CONST RT_IN *in );

/* The seed of <random> and of the random system variable: the same seed plays the same */
IMPORT void rt_seed( RT *rt, UINT seed );

/* A user variable (0x0001 .. 0x3FFF) and the system ones (0x4000 ..) */
IMPORT INT  rt_var( CONST RT *rt, INT id );
IMPORT void rt_set_var( RT *rt, INT id, INT v );
#define RT_SV_TIME	0x4000		/* program time in milliseconds, kept to 15 bits */
#define RT_SV_RANDOM	0x4001		/* a random number 0 .. 32767, new each read */
#define RT_SV_KEY	0x4002		/* the HID usage of the last key pressed */
#define RT_SV_X		0x4003		/* where the pointer was last, in the scene */
#define RT_SV_Y		0x4004
#define RT_SV_PLAYING	0x4005		/* media that are playing */
#define RT_SV_SLIDE	0x4006		/* the slide shown (1 the first), 0 outside a slide show */
#define RT_SV_BLOCK	0x4007		/* the <realData> playing, 1 the first */

/* ---------------------------------------------------------------- pieces, for the tests */

/*
 * An expression of <expr> run on its own: 'code' as the binary TAD
 * writes it (rt_expr_code), the variables those of rt. 16-bit arithmetic
 * as the format says; a division by 0 gives 0.
 */
#define RT_EX_CONST	0x8100		/* followed by the value */
#define RT_EX_MAX	64
IMPORT INT rt_expr_code( CONST T_TADNODE *expr, UH *code, INT max );
IMPORT INT rt_expr_eval( RT *rt, CONST UH *code, INT n );
IMPORT UINT rt_op_code( CONST UB *name );		/* "add" -> 0x8000, 0 none */
IMPORT CONST char *rt_op_name( UINT code );

/* A decimal written in the record, in millionths; FALSE when it is not one */
IMPORT BOOL rt_num( CONST UB *s, RT_V *p_v );
/* A whole number written in the record (decimal or 0x hexadecimal), or dflt */
IMPORT INT rt_int( CONST UB *s, INT dflt );

/* "A/B" or a decimal, as a fraction n/d */
IMPORT BOOL rt_ratio( CONST UB *s, D *p_n, D *p_d );

/* The shape of a change over time: p and the answer 0 .. 65536 */
#define RT_EASE_LINEAR	0
#define RT_EASE_SPLINE	1
#define RT_EASE_IN	2
#define RT_EASE_OUT	3
#define RT_EASE_INOUT	4
#define RT_EASE_STEP_S	5
#define RT_EASE_STEP_E	6
#define RT_EASE_BEZIER	7
typedef struct {
	UINT	kind;
	INT	x1, y1, x2, y2;		/* cubic-bezier, in 65536ths */
} RT_EASE;
IMPORT BOOL rt_ease_parse( CONST UB *s, RT_EASE *e );
IMPORT INT  rt_ease( CONST RT_EASE *e, INT p );

/* A note's name ("C4", "F#3", "Bb2") or number as MIDI: C4 is 60; -1 none */
IMPORT INT rt_pitch( CONST UB *s );

/*
 * A time value as the units in force say: the value in millionths as
 * written, the unit attribute when the element has one ("ms", "us",
 * "s", "tick"), for a thread with that unit state. Exposed for the
 * tests through a program: the time 'value' would come to in the first
 * thread's state of rt.
 */
IMPORT RT_TIME rt_time_of( RT *rt, CONST UB *value, CONST UB *unit );

/* Base64 into bytes: how many, or -1 */
IMPORT INT rt_base64( CONST UB *s, UB *out, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_RTAD_H__ */
