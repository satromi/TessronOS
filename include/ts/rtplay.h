/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtplay.h
 *	The player of 実時間制御TAD, in the program that plays it (design
 *	15.5, lib/librtplay)
 *
 *	A program that plays a record of 実時間制御TAD -- MPEG Player, スラ
 *	イド -- links the player and plays the record in a part of its own
 *	window: the scene is laid out by the drawing of documents and
 *	figures linked into the program (include/ts/tvport.h, built with
 *	TV_USER), drawn into pictures of the program's memory (dp_open_mem)
 *	and laid on the window's environment (dp_put_argb). The program of
 *	the record runs in a task of the player's own, woken at the time of
 *	its next event and at each frame while anything moves; sound is
 *	made by the process 実時間メディア and video by MPEG Player started
 *	for it (ts/rtmedia.h), both children of the program, which end by
 *	themselves when the program does, however it ends.
 *
 *	What the program does to the player is said with rtp_do: an
 *	operation (RTP_*), and in the state how the program of the record
 *	goes. A program that is not linked with the POSIX threads (newlib
 *	alone) links lib/librtplay/rtp_mlock.c too, so that the C library's
 *	memory is taken by one task at a time.
 */

#ifndef __TS_RTPLAY_H__
#define __TS_RTPLAY_H__

#include <tk/typedef.h>
#include <ts/uuid.h>
#include <ts/tad.h>
#include <ts/dp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtplay RTPLAY;

/* Where and how it is played (rtp_open) */
typedef struct {
	INT	gid;			/* the window's drawing environment (wm_obj_gid) */
	ID	kw;			/* the window's key: what is drawn is flushed through it */
	ID	port;			/* the window's channel: the panels' notices come there */
	T_DPRECT area;			/* of the work area: the scene is laid out in it */
	UW	ground;			/* round the scene in the area */
	UW	paper;			/* under the scene; TAD_COL_NONE the object's (black when it has none) */
	BOOL	slide;			/* played as a slide: what the record asks of the slides is kept */
	INT	tw, th;			/* a slide: the screen it is made for; 0 0 what the record says */
	INT	slide_no;		/* a slide: its number from 1, which the record reads */
	/*
	 * The player's task started as one of the program's: body(arg) run
	 * until it returns. NULL: a task of the kernel's (tk_cre_tsk) at the
	 * priority of the task that opens it. A program of POSIX threads
	 * gives one that makes a thread, for the C library's locks.
	 */
	ER	(*spawn)( void (*body)( void *arg ), void *arg );
} RTP_OPEN;

/* What is done to it (rtp_do) */
#define RTP_STATE	0		/* nothing: only how it goes */
#define RTP_TOGGLE	1		/* 再生 / 一時停止; one waiting to be begun begins */
#define RTP_STOP	2		/* back to the start, and held there */
#define RTP_RESTART	3		/* 先頭から */
#define RTP_SEEK	4		/* a ms forward, back when less than 0 */
#define RTP_TRACK	5		/* the medium of the playlist a after (before) the one playing */
#define RTP_AREA	6		/* the scene laid out again in a by b of the work area's corner */
#define RTP_REDRAW	7		/* the ground and the scene drawn again */
#define RTP_GROUND	8		/* 背景色変更: chosen in a panel */
#define RTP_LIST	9		/* プレイリスト表示: a panel, the medium chosen played */
#define RTP_PRESS	10		/* the first button down at a, b of the work area */
#define RTP_RELEASE	11		/* and up there */
#define RTP_KEY		12		/* key a (a HID usage) down, b RTP_KEY_* */
#define RTP_MOVE	13		/* the pointer at a, b (outside the scene: it left it) */

#define RTP_KEY_UP	0x0001		/* up, not down */
#define RTP_KEY_MINE	0x0002		/* the program's own key: given only when the record takes it */

/* How it goes */
#define RTP_S_PLAYING	0x0001
#define RTP_S_PAUSED	0x0002
#define RTP_S_WAITING	0x0004		/* it waits to be begun (autoplay="false") */
#define RTP_S_DONE	0x0008		/* it has run to its end */
#define RTP_S_TAKEN	0x0010		/* PRESS, RELEASE, KEY: a trigger or a wait of it took it */
#define RTP_S_GO	0x0020		/* a slide: the record asked for another slide */
#define RTP_S_CLOSED	0x0040		/* a panel: the window was closed while it was up */

#define RTP_TEXT_MAX	192

typedef struct {
	UINT	flags;			/* RTP_S_* */
	INT	ms;			/* the record's time */
	INT	media;			/* the media of its playlist */
	UINT	go;			/* RTP_S_GO: RT_ACT_NEXT, RT_ACT_PREV, RT_ACT_INDEX (ts/rtad.h) */
	INT	go_n;			/* RT_ACT_INDEX: the slide's index */
	UB	now[RTP_TEXT_MAX];	/* the title and the artist of the one playing, UTF-8 */
} RTP_HOW;

/* Whether a record holds a <realtime> */
IMPORT BOOL    rtp_has( CONST T_TAD *rec );

/* The scene a record is made for: its figure's view, or 1920 by 1080 */
IMPORT void    rtp_scene_size( CONST T_TAD *rec, INT *p_w, INT *p_h );

/*
 * Record recno of obj played in the window as o says: NULL when it holds
 * no <realtime> or there is not the memory for it. The ground and the
 * scene are drawn at once.
 */
IMPORT RTPLAY *rtp_open( CONST TS_UUID *obj, INT recno, CONST RTP_OPEN *o );

/* Stopped, its helpers ended, and let go of */
IMPORT void    rtp_close( RTPLAY *p );

/* op done (RTP_*), and how it goes then in *st; E_PAR for an op not known */
IMPORT ER      rtp_do( RTPLAY *p, UINT op, INT a, INT b, RTP_HOW *st );

/* Where the scene stands in the window */
IMPORT void    rtp_fit( RTPLAY *p, T_DPRECT *r );

#ifdef __cplusplus
}
#endif

#endif /* __TS_RTPLAY_H__ */
