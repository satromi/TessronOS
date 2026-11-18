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
 *	A backend has one or more outputs, each named by its id (include/
 *	ts/snd.h): the USB audio backend one for every function plugged in,
 *	the HDMI one a socket each. The device picks the output, and the
 *	backend plays on the one it is given.
 *
 *	Everything here is called with the USB manager's lock held, which is
 *	the sound device's lock too: the USB audio streams run in the
 *	manager's task, and the other backends are served from the same
 *	task's knl_snd_poll().
 */

#ifndef __SNDHW_H__
#define __SNDHW_H__

#include <ts/snd.h>

/*
 * The outputs being gathered (snd.c): a backend adds each it has with
 * knl_snd_out_add(), its id and name whole. The room is the sound
 * device's and is kept from one gathering to the next, so that a
 * gathering while a stream plays takes no memory once it has had enough.
 */
typedef struct SNDOUTS SNDOUTS;

typedef struct {
	INT	kind;				/* SND_BE_* */
	/*
	 * The outputs it has now, in the fixed form, up to max of them:
	 * id, name, kind and the flags SND_OF_NOSOUND and SND_OF_LEVEL.
	 * For the backends written before outs; NULL in one that has outs.
	 */
	INT	(*outputs)( SDOut *o, INT max );
	BOOL	(*can_record)( void );
	/*
	 * Output on the output id: from here on the backend asks
	 * knl_snd_mix() for its samples, perhaps already before this
	 * returns.
	 */
	ER	(*play_start)( CONST UB *id, UW *rate, INT *channels );
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
	/*
	 * Silence before the first samples of a stream that has just
	 * started (a screen takes a while to let sound through), and how
	 * long the stream goes on in silence once the last channel has
	 * stopped, so that a short sound soon after is not cut by that
	 * wait again. 0 for neither.
	 */
	UW	lead_ms;
	UW	tail_ms;
	/*
	 * The outputs it has now, each added to l by knl_snd_out_add():
	 * id, name, kind and the flags SND_OF_NOSOUND and SND_OF_LEVEL.
	 * Cheap: it is asked every few hundred milliseconds while a stream
	 * plays. E_NOMEM when an add failed (knl_snd_out_add's answer).
	 * NULL: outputs is asked instead.
	 */
	ER	(*outs)( SNDOUTS *l );
} SNDHW;

/*
 * The playback stream: frames frames of 16 bit little endian samples,
 * channels per frame, at rate Hz, into dst; every playing channel is
 * converted and added in. Answers the frames made: frames, or 0 when
 * wait is set and no channel has the samples yet.
 */
IMPORT INT knl_snd_mix( UB *dst, INT frames, UW rate, INT channels, BOOL wait );

/*
 * The rate the input now comes at, when it changed while it records
 * (a voice link up in another codec than its start foresaw): with the
 * manager's lock held. Nothing when nothing records from a backend.
 */
IMPORT void knl_snd_in_rate( UW rate );

/*
 * The microphone's volume (0 .. SND_VOL_MAX) as a level of DN_SDVOLMIC,
 * SND_MIC_UNITY at the level's middle, and a level back as the nearest
 * volume: for a backend that has its hardware's gain told it in its own
 * steps (the earphones' +VGM)
 */
IMPORT UH knl_snd_mic_level( UW vol );
IMPORT UW knl_snd_mic_vol( UH level );

/* An output added to l, its id and name whole; E_NOMEM when there is no room for it */
IMPORT ER   knl_snd_out_add( SNDOUTS *l, CONST UB *id, CONST UB *name, UW kind, UW flags );

/*
 * An id and a name into an entry of the fixed form (SDOut), cut at a
 * whole letter to fit, SND_OF_CUT set when they did not: for the
 * backends that still answer outputs
 */
IMPORT void knl_snd_out_set( SDOut *o, CONST UB *id, CONST UB *name, UW kind, UW flags );

/* The backends there are */
IMPORT CONST SNDHW	knl_uac_hw;
IMPORT CONST SNDHW	knl_bta_hw;	/* device/bt/bt_a2dp.c */

/*
 * The output in use turns the whole's volume down itself (Bluetooth
 * earphones told the mixer's volume): the mixer then leaves the sum at
 * full scale, the mute still its own. Set by that backend while it plays.
 */
IMPORT void knl_snd_whole_outside( BOOL on );
IMPORT CONST SNDHW	knl_lea_hw;	/* device/bt/bt_lea.c: LE Audio */
#ifdef RPI5
IMPORT CONST SNDHW	knl_i2s_hw;
IMPORT ER knl_i2s_init( void );
IMPORT CONST SNDHW	knl_hdmi_hw;
IMPORT ER knl_hdmi_init( void );

/* For the checks on the board: how far the stream went, and clk_i2s counted */
IMPORT BOOL knl_i2s_pace( UW *blocks, UW *ms, UW *clk_khz );
IMPORT INT  knl_hdmi_pace( UW *blocks, UW *ms );
#endif
#ifdef X64
IMPORT CONST SNDHW	knl_hda_hw;	/* device/snd/x86_64/snd_hda.c */
IMPORT ER knl_hda_init( void );

/* What was found of the machine's sound at start-up, and what it comes to (SND_NOTE_*) */
typedef struct {
	BOOL	ctrl;		/* an audio controller was found */
	UH	codecs;		/* the codecs that answered on its link */
	BOOL	analog;		/* one of them is not a display's */
	BOOL	int_mic;	/* a fixed microphone is on that codec's pins */
	INT	nhlt_dmic;	/* endpoints of the ACPI table NHLT on digital microphones */
	INT	nhlt_ssp;	/* and on I2S (SSP) */
	INT	sdw_devs;	/* peripherals under SoundWire controllers in the namespace */
	BOOL	i2s_codec;	/* the namespace has a codec of those that only sit on I2S */
} HDA_FOUND;
IMPORT UW   knl_hda_judge( CONST HDA_FOUND *f );
IMPORT UW   knl_hda_note( void );
/* The endpoints of an NHLT table (whole, with its ACPI header) by their link; FALSE when it is not well formed */
IMPORT BOOL knl_hda_nhlt( CONST UB *t, INT len, INT *dmic, INT *ssp );
#endif

#endif /* __SNDHW_H__ */
