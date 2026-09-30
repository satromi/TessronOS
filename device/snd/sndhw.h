/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sndhw.h
 *	What the sound device asks of the hardware under it (design 10.15).
 *
 *	The device itself (snd.c) knows nothing of the hardware: it keeps a
 *	channel per process, converts and mixes, and answers the requests.
 *	A backend plays one stream of 16 bit samples and records another.
 *	Output is pulled: the backend asks knl_snd_mix() for every piece of
 *	output just before it needs it. Input is pushed: the backend hands
 *	what it recorded to knl_snd_capture().
 *
 *	Everything here is called with the USB manager's lock held, which is
 *	the sound device's lock too: the USB audio streams run in the
 *	manager's task, and the other backends are served from the same
 *	task's knl_snd_poll().
 */

#ifndef __SNDHW_H__
#define __SNDHW_H__

#include <ts/snd.h>

typedef struct {
	INT	kind;				/* SND_BE_* */
	BOOL	(*can_play)( void );
	BOOL	(*can_record)( void );
	/*
	 * Output: from here on the backend asks knl_snd_mix() for its
	 * samples, perhaps already before this returns.
	 */
	ER	(*play_start)( UW *rate, INT *channels );
	void	(*play_stop)( void );
	BOOL	(*play_running)( void );
	/* Input near rate Hz; the rate and channels it comes in at */
	ER	(*rec_start)( UW rate, UW *dev_rate, INT *channels );
	void	(*rec_stop)( void );
	BOOL	(*rec_running)( void );
	/* every few milliseconds while a stream runs */
	void	(*poll)( void );
	/* the levels it has (DN_SDVOL*, DN_SDGAIN*); E_NOSPT for one it has not */
	ER	(*get_level)( INT dn, SDVol *v );
	ER	(*set_level)( INT dn, CONST SDVol *v );
} SNDHW;

/*
 * The playback stream: frames frames of 16 bit little endian samples,
 * channels per frame, at rate Hz, into dst; every playing channel is
 * converted and added in. Answers the frames made: frames, or 0 when
 * wait is set and no channel has the samples yet.
 */
IMPORT INT knl_snd_mix( UB *dst, INT frames, UW rate, INT channels, BOOL wait );

/* The backends there are */
IMPORT CONST SNDHW	knl_uac_hw;
#ifdef RPI5
IMPORT CONST SNDHW	knl_i2s_hw;
IMPORT ER knl_i2s_init( void );
#endif

#endif /* __SNDHW_H__ */
