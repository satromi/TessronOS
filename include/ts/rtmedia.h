/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtmedia.h
 *	What the player of 実時間制御TAD (lib/librtplay, in the program that
 *	plays the record) says to the processes that make its sound and its
 *	pictures move (design 15.6)
 *
 *	Whatever sounds -- a file of audio, the notes and MIDI messages of
 *	the program, the PCM a record carries -- is done by the process
 *	実時間メディア (application/rtmedia), and a video by MPEG Player
 *	started in its own way for it (--embed), each a child of the program
 *	that plays the record. Each is started with an RTM_ARG as its
 *	start-up argument and is told what to do by messages on a channel
 *	the player made; it answers on another, and a video puts its frames
 *	where the player reads them. Given its host, it watches the process
 *	it plays for (OB_E_EXIT, told on the channel it reads) and, when
 *	that ends however it ends, deletes the channels and the memory it
 *	was given and ends too.
 *
 *	The messages are fixed records, little endian, one a channel
 *	message (at most OB_CH_MSG_MAX bytes). Times are nanoseconds of the
 *	monotonic clock both sides read (ts_get_mono); a sound is to be
 *	heard at its time plus the latency the process keeps, the same for
 *	every sound, so the time between two sounds is kept to the sample.
 */

#ifndef __TS_RTMEDIA_H__
#define __TS_RTMEDIA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

#define RTM_MAGIC	0x4D54525AU	/* "ZRTM" */
#define RTM_PROG	"rtmedia"	/* the program id of 実時間メディア */
#define RTM_VIDEO_PROG	"mpeg-player"	/* the program that decodes video, started with --embed */
#define RTM_VIDEO_UUID	"01a0f638-4c5a-78a9-9bbd-adfbafa3466a"	/* its program object (a package's) */
#define RTM_EMBED	"--embed"

/* The start-up argument */
typedef struct {
	UINT	magic;			/* RTM_MAGIC */
	UINT	flags;
	TS_UUID	cmd;			/* the channel it reads */
	TS_UUID	reply;			/* the channel it answers on */
	TS_UUID	frames;		/* a video: the memory object its frames go to */
	UB	embed[8];		/* RTM_EMBED, for a program that has a window of its own otherwise */
	TS_UUID	host;			/* the process object of the program it plays for; all 0 none */
} RTM_ARG;

/*
 * The number the helper asks the host's OB_E_EXIT to be told with, on
 * the channel of its commands: it stands where a command's op does, and
 * is none of theirs
 */
#define RTM_HOST_ID	0x5854524DU	/* "MRTX" */

/* ---------------------------------------------------------------- what it is told */

#define RTM_OPEN	1		/* get a medium ready */
#define RTM_PLAY	2
#define RTM_PAUSE	3
#define RTM_RESUME	4
#define RTM_STOP	5
#define RTM_SEEK	6		/* v: microseconds into the file */
#define RTM_VOLUME	7		/* v: millionths */
#define RTM_PAN		8
#define RTM_RATE	9
#define RTM_MUTE	10		/* v: 1, 0 */
#define RTM_CLOSE	11
#define RTM_NOTE	12		/* a note of a stream's instrument */
#define RTM_MIDI	13		/* MIDI messages for a stream */
#define RTM_STREAM	14		/* what a stream plays on: its device's name and parameters */
#define RTM_PCM		15		/* PCM bytes the record carried, from a memory object */
#define RTM_SILENCE	16		/* every note, every medium, stopped */
#define RTM_SIZE	17		/* a video: the size of the box its frames are made for */
#define RTM_QUIT	18

#define RTM_NAME	96		/* bytes of a resource's name, a format, a device's name */
#define RTM_PARAMS	160

/* Where a medium is: a resource of an object ("_0_0.mp3"), or a record of it (recno >= 0) */
typedef struct {
	TS_UUID	obj;
	INT	recno;			/* -1: the resource 'name' */
	UB	name[RTM_NAME];
	UB	format[16];		/* "mp3", "wav" ...; empty: from the name */
} RTM_SRC;

typedef struct {
	UINT	op;			/* RTM_* */
	INT	inst;			/* the medium, as the engine numbers them */
	INT	gen;			/* its start, given back in the answers */
	INT	stream;
	D	at;			/* when it is to be heard (ns, monotonic); 0 at once */
	D	v;
	union {
		struct {		/* RTM_OPEN */
			RTM_SRC	src;
			INT	kind;		/* RT_MK_* */
			INT	preload;
		} open;
		struct {		/* RTM_PLAY: every setting it is played with */
			D	start, dur;	/* microseconds into the file, how much (0 all) */
			D	skip;		/* and how much more of it is past already */
			D	volume, pan, rate;	/* millionths */
			D	loop_start, loop_end;
			D	fade_in, fade_out;
			INT	loop, muted, track;
		} play;
		struct {		/* RTM_NOTE */
			INT	pitch, velocity, channel;
			D	dur;		/* microseconds */
		} note;
		struct {		/* RTM_MIDI */
			INT	n;
			UB	msg[16][3];
		} midi;
		struct {		/* RTM_STREAM */
			INT	number;
			UB	device[RTM_NAME];
			UB	params[RTM_PARAMS];
		} stream;
		struct {		/* RTM_PCM */
			TS_UUID	mem;		/* a memory object: record 0 holds the bytes */
			INT	rate, channels, bits;
			INT	wav;		/* the bytes are a whole WAVE file */
		} pcm;
		struct {		/* RTM_SIZE */
			INT	w, h;
		} size;
	} u;
} RTM_CMD;

/* ---------------------------------------------------------------- what it answers */

#define RTM_A_LOADED	1		/* v: its length, microseconds; w, h a video's size */
#define RTM_A_PLAYING	2
#define RTM_A_PAUSED	3
#define RTM_A_ENDED	4
#define RTM_A_ERROR	5		/* v: RT_ERR_* */
#define RTM_A_TIME	6		/* v: where it is, microseconds into the file */
#define RTM_A_FRAME	7		/* a video: a new frame in the memory object (v: its number) */
#define RTM_A_READY	8		/* the process is up and reads its channel */

typedef struct {
	UINT	op;			/* RTM_A_* */
	INT	inst, gen;
	INT	w, h;
	D	v;
} RTM_ANS;

/*
 * A video's frames: the memory object's record 0 holds this head and
 * two frames after it, each w by h pixels of 0x00rrggbb. The process
 * writes the one 'shown' does not name and then names it; the desktop
 * reads the one named. 'seq' counts the frames.
 */
typedef struct {
	UINT	magic;			/* RTM_MAGIC */
	INT	w, h;			/* what the frames are */
	INT	shown;			/* 0 or 1: the frame to read */
	INT	seq;
	INT	rsv[3];
} RTM_FRAMES;

#define RTM_FRAMES_MAX	( 1280 * 720 )	/* pixels of one frame, at most */

#ifdef __cplusplus
}
#endif

#endif /* __TS_RTMEDIA_H__ */
