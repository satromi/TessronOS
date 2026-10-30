/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	beep.c
 *	The buzzer and the clicks (design 16.5.23)
 *
 *	The system's sounds are objects of the sound box: ブザー音, 押下音
 *	and 離し音 (SYSDEF_SND_*). One is played by reading it and writing it
 *	to the sound device's object, which gives each opening a channel of
 *	its own and mixes it with what else plays. An object whose record 1
 *	is a WAV file (RIFF, linear PCM) plays that; one without says in its
 *	metadata the tone the system makes for it (tessronos.sound.tone: hz,
 *	ms), a square wave eased in and out, which is what ユーザ環境設定
 *	changes. The speaker switched off (LK_SPEAKER 0) plays nothing.
 *
 *	A task of its own plays them, so that asking for one never waits:
 *	the last asked is played, one at a time.
 *
 *	This is the one way the system's sounds are played: the clicks and
 *	the buzzer of the window layer, and 〈試聴〉 of ユーザ環境設定 and of
 *	システム環境設定, which ask through the system object's record 8
 *	(PLAY and what, obsys.c). They all go to the sound device as one
 *	channel of this task, so they sound on the output the system has
 *	chosen (design 6.5.1). wm_beep_try is the asking that waits: it plays
 *	even with the speaker switched off, and answers whether the sound
 *	started -- E_NOSPT when no output can sound -- so a settings
 *	accessory can say so rather than stay silent. WM_BEEP_TONE plays an
 *	object's tone even when its record 1 holds a WAV file (the tone is
 *	what ユーザ環境設定 sets); WM_BEEP_TEST is half a second of 880Hz.
 *	The channel is stopped before each sound, so a sound cut short by a
 *	later one leaves nothing that would refuse the next.
 *
 *	The volume keys (HID_KEY_VOLUP, HID_KEY_VOLDOWN; a machine's own
 *	volume buttons are given as them) change the whole's volume of the
 *	sound device's mixer here, as 音量ミキサー's 全体 does (knl_wm_volume_key).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/look.h>
#include <ts/snd.h>
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <tm/tmonitor.h>
#include "obj/obj.h"

#define TONE_RATE	48000
#define SOUND_MAX	( 512 * 1024 )		/* bytes of a sound played */
#define TONE_AMP	6000
#define BEEP_TICK_MS	100			/* WM_BEEP_TICK: the buzzer's tone, short */
#define BEEP_LONG_MS	600			/* WM_BEEP_LONG: and long */
#define TEST_HZ		880			/* WM_BEEP_TEST */
#define TEST_MS		500
#define TRY_TMO		3000			/* the most wm_beep_try waits for the start */

#define FLG_ASK		0x01			/* beep_flg: wm_beep asked */
#define FLG_TRY		0x02			/* and wm_beep_try */

LOCAL ID	beep_tsk = 0;
LOCAL ID	beep_flg = 0;
LOCAL volatile UINT beep_what;
LOCAL UB	*beep_buf = NULL;
LOCAL ID	try_sem = 0;			/* one wm_beep_try at a time */
LOCAL ID	try_flg = 0;			/* its answer */
LOCAL volatile UINT try_what;
LOCAL volatile ER try_er;

LOCAL CONST char *CONST beep_obj[3] = { SYSDEF_SND_BUZZER, SYSDEF_SND_PRESS, SYSDEF_SND_RELEASE };

/* A little-endian number in a WAV file */
LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UH le16( CONST UB *p )
{
	return (UH)( p[0] | ( p[1] << 8 ) );
}

LOCAL BOOL tag_is( CONST UB *p, CONST char *tag )
{
	return (BOOL)( p[0] == (UB)tag[0] && p[1] == (UB)tag[1] && p[2] == (UB)tag[2] && p[3] == (UB)tag[3] );
}

/* The rate code the sound device takes for a rate, -1 for one it does not */
LOCAL INT rate_code( UW rate )
{
	switch ( rate ) {
	case 8000:	return PCMST8K;
	case 11025:	return PCMST11K;
	case 16000:	return PCMST16K;
	case 22050:	return PCMST22K;
	case 44100:	return PCMST44K;
	case 48000:	return PCMST48K;
	default:	return -1;
	}
}

/*
 * A WAV file's samples: where they start in buf, how many bytes, and the
 * form the device is to take them in. FALSE when it is not one the
 * device plays.
 */
LOCAL BOOL wav_parse( CONST UB *buf, INT len, INT *p_off, INT *p_n, SDPcmMode *m )
{
	INT	at = 12;
	BOOL	fmt = FALSE;

	if ( len < 44 || !tag_is(buf, "RIFF") || !tag_is(buf + 8, "WAVE") ) {
		return FALSE;
	}
	while ( at + 8 <= len ) {
		UW	sz = le32(buf + at + 4);

		if ( tag_is(buf + at, "fmt ") && sz >= 16 && at + 8 + 16 <= len ) {
			UH	kind = le16(buf + at + 8), ch = le16(buf + at + 10), bits = le16(buf + at + 22);
			INT	rc = rate_code(le32(buf + at + 12));

			if ( kind != 1 || ( ch != 1 && ch != 2 ) || ( bits != 8 && bits != 16 ) || rc < 0 ) {
				return FALSE;
			}
			knl_memset(m, 0, sizeof(*m));
			m->sampling = (UW)rc;
			m->datafmt = ( bits == 16 ) ? PCMFmt16 : PCMFmt8;
			m->stereo = ( ch == 2 );
			fmt = TRUE;
		} else if ( tag_is(buf + at, "data") && fmt ) {
			*p_off = at + 8;
			*p_n = (INT)( ( at + 8 + (INT)sz > len ) ? len - ( at + 8 ) : (INT)sz );
			return (BOOL)( *p_n > 0 );
		}
		at += 8 + (INT)( ( sz + 1 ) & ~1U );
	}
	return FALSE;
}

/*
 * The tone an object's metadata says: a square wave of hz for ms, into
 * buf; for len_ms above nought, that long instead
 */
LOCAL INT tone_make( ID key, UB *buf, SDPcmMode *m, INT len_ms, INT test_hz )
{
	UB	*j = NULL;
	SZ	asz = 0;
	D	hz = 2000, ms = 100;
	INT	p, n, i, half, k, len;
	H	*pcm = (H *)buf;

	len = ( key > 0 ) ? knl_ob_atr_all(key, 0, &j, NULL) : E_NOEXS;
	asz = ( len > 0 ) ? (SZ)len : 0;
	if ( len >= 0 ) {
		p = knl_oj_path(j, (INT)asz, "tessronos", "sound");
		p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "tone") : -1;
		if ( p >= 0 ) {
			INT	q = knl_oj_member(j, (INT)asz, p, "hz");

			if ( q >= 0 ) (void)knl_oj_num(j, (INT)asz, q, &hz);
			q = knl_oj_member(j, (INT)asz, p, "ms");
			if ( q >= 0 ) (void)knl_oj_num(j, (INT)asz, q, &ms);
		}
	}
	if ( j != NULL ) Kfree(j);
	if ( len_ms > 0 ) ms = len_ms;
	if ( test_hz > 0 ) hz = test_hz;
	if ( hz < 50 ) hz = 50;
	if ( hz > 12000 ) hz = 12000;
	if ( ms < 5 ) ms = 5;
	n = (INT)( TONE_RATE / 1000 * ms );
	if ( n * 2 > SOUND_MAX ) n = SOUND_MAX / 2;
	half = (INT)( TONE_RATE / hz / 2 );
	if ( half < 1 ) half = 1;
	for ( i = 0; i < n; i++ ) {
		INT	a = ( ( i / half ) & 1 ) ? -TONE_AMP : TONE_AMP;

		/* the first and last millisecond eased, so that it does not snap */
		k = ( i < 48 ) ? i : ( n - i < 48 ) ? n - i : 48;
		pcm[i] = (H)( a * k / 48 );
	}
	knl_memset(m, 0, sizeof(*m));
	m->sampling = PCMST48K;
	m->datafmt = PCMFmt16;
	m->stereo = 0;
	return n * 2;
}

/* The start's answer, to the one that waits in wm_beep_try */
LOCAL void answer( BOOL tell, ER er )
{
	if ( tell ) {
		try_er = er;
		tk_set_flg(try_flg, 1);
	}
}

/*
 * A sound played through the sound device's object, to its end: the
 * object uuid's WAV file, or its tone (tone set, or no WAV there); for
 * len_ms above nought, its tone that long whatever it holds; with uuid
 * NULL, the test tone. tell: the start's answer is waited for.
 */
LOCAL void play( CONST char *uuid, INT len_ms, BOOL tone, BOOL tell )
{
	TS_UUID		u, dev;
	SDPcmMode	m;
	ID		key = 0, dk;
	SZ		asz = 0;
	INT		off = 0, n = 0, i;
	W		v;
	ER		er;

	if ( knl_obdev_uuid((CONST UB *)SND_DEVNM, &dev) < E_OK ) {
		answer(tell, E_NOEXS);
		return;
	}
	if ( uuid == NULL ) {
		n = tone_make(0, beep_buf, &m, TEST_MS, TEST_HZ);
	} else {
		if ( ts_str_to_uuid(uuid, &u) < E_OK || ( key = ob_opn_obj(&u, OB_OP_R) ) < E_OK ) {
			answer(tell, E_NOEXS);
			return;
		}
		if ( len_ms > 0 || tone || ob_rea_rec(key, 1, 0, beep_buf, SOUND_MAX, &asz) < E_OK || asz <= 0
		  || !wav_parse(beep_buf, (INT)( asz > SOUND_MAX ? SOUND_MAX : asz ), &off, &n, &m) ) {
			off = 0;
			n = tone_make(key, beep_buf, &m, len_ms, 0);
		}
		ob_cls_obj(key);
	}
	if ( n <= 0 ) {
		answer(tell, E_NOMEM);
		return;
	}

	dk = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
	if ( dk < E_OK ) {
		answer(tell, (ER)dk);
		return;
	}
	v = PCM_STOP;
	(void)ob_wri_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz);
	v = n;
	er = ob_wri_rec(dk, OB_SND_MODE, 0, &m, sizeof(m), &asz);
	if ( er >= E_OK ) er = ob_wri_rec(dk, OB_SND_BUFSZ, 0, &v, sizeof(v), &asz);
	if ( er >= E_OK ) er = ob_wri_rec(dk, OB_SND_PCM, 0, beep_buf + off, n, &asz);
	if ( er >= E_OK ) er = ob_wri_rec(dk, OB_SND_ENDCNT, 0, &v, sizeof(v), &asz);
	if ( er >= E_OK ) {
		v = PCM_PLAY;
		er = ob_wri_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz);
	}
	answer(tell, er);
	if ( er < E_OK ) {
		tm_printf((UB *)"wm: beep: not played (%d)\n", (INT)er);
	} else {
		/* until it has played, and no longer than the longest may take */
		for ( i = 0; i < 400; i++ ) {
			tk_dly_tsk(10);
			if ( ob_rea_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz) < E_OK || v == PCM_STOP ) {
				break;
			}
		}
	}
	ob_cls_obj(dk);
}

/* One sound asked for: a WM_BEEP_* with its WM_BEEP_TONE */
LOCAL void play_what( UINT what, BOOL tell )
{
	UINT	w = what & ~WM_BEEP_TONE;
	BOOL	tone = ( ( what & WM_BEEP_TONE ) != 0 );

	if ( w <= WM_BEEP_RELEASE ) {
		play(beep_obj[w], 0, tone, tell);
	} else if ( w == WM_BEEP_TICK ) {
		play(beep_obj[WM_BEEP_BUZZER], BEEP_TICK_MS, TRUE, tell);
	} else if ( w == WM_BEEP_LONG ) {
		play(beep_obj[WM_BEEP_BUZZER], BEEP_LONG_MS, TRUE, tell);
	} else if ( w == WM_BEEP_TEST ) {
		play(NULL, 0, TRUE, tell);
	} else {
		answer(tell, E_PAR);
	}
}

LOCAL void beep_task( INT stacd, void *exinf )
{
	UINT	ptn;

	for ( ;; ) {
		if ( tk_wai_flg(beep_flg, FLG_ASK | FLG_TRY, TWF_ORW | TWF_CLR, &ptn, TMO_FEVR) < E_OK ) {
			tk_dly_tsk(100);
			continue;
		}
		/* the one that waits for its answer first, whatever the speaker */
		if ( ( ptn & FLG_TRY ) != 0 ) {
			play_what(try_what, TRUE);
		}
		if ( ( ptn & FLG_ASK ) != 0 && wm_num(LK_SPEAKER, 1) != 0 ) {
			play_what(beep_what, FALSE);
		}
	}
}

LOCAL ER beep_start( void )
{
	T_CFLG	cf;
	T_CTSK	ct;

	if ( beep_tsk > 0 ) {
		return E_OK;
	}
	beep_buf = (UB *)Kmalloc(SOUND_MAX);
	if ( beep_buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WSGL;
	beep_flg = tk_cre_flg(&cf);
	if ( beep_flg <= 0 ) {
		return (ER)beep_flg;
	}
	try_flg = tk_cre_flg(&cf);
	if ( try_flg <= 0 ) {
		return (ER)try_flg;
	}
	{
		T_CSEM	cs;

		knl_memset(&cs, 0, sizeof(cs));
		cs.sematr = TA_TFIFO | TA_FIRST;
		cs.isemcnt = 1;
		cs.maxsem = 1;
		try_sem = tk_cre_sem(&cs);
		if ( try_sem <= 0 ) {
			return (ER)try_sem;
		}
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)beep_task;
	ct.itskpri = 25;
	ct.stksz = 8 * 1024;
	beep_tsk = tk_cre_tsk(&ct);
	if ( beep_tsk <= 0 ) {
		return (ER)beep_tsk;
	}
	return tk_sta_tsk(beep_tsk, 0);
}

EXPORT ER wm_beep( UINT what )
{
	ER	er;

	if ( ( what & ~WM_BEEP_TONE ) > WM_BEEP_TEST ) {
		return E_PAR;
	}
	if ( wm_num(LK_SPEAKER, 1) == 0 ) {
		return E_OK;
	}
	er = beep_start();
	if ( er < E_OK ) {
		return er;
	}
	beep_what = what;
	return tk_set_flg(beep_flg, FLG_ASK);
}

EXPORT ER wm_beep_try( UINT what )
{
	UINT	ptn;
	ER	er;

	if ( ( what & ~WM_BEEP_TONE ) > WM_BEEP_TEST ) {
		return E_PAR;
	}
	er = beep_start();
	if ( er < E_OK ) {
		return er;
	}
	if ( tk_wai_sem(try_sem, 1, TRY_TMO) < E_OK ) {
		return E_TMOUT;
	}
	tk_clr_flg(try_flg, 0);
	try_er = E_TMOUT;
	try_what = what;
	tk_set_flg(beep_flg, FLG_TRY);
	er = ( tk_wai_flg(try_flg, 1, TWF_ORW | TWF_CLR, &ptn, TRY_TMO) >= E_OK ) ? try_er : E_TMOUT;
	tk_sig_sem(try_sem, 1);
	return er;
}

/*
 * A volume key pressed, or repeating while it is held (the window layer
 * takes it before any window, wm_read_event): the whole's volume a step
 * of VOL_STEP up or down, from the louder of left and right, within 0 ..
 * SND_VOL_MAX, both sides set to it. A mute is let go, as the volume is
 * set again. It is kept for the next start, as a write of DN_SDMIXSET
 * is, and the mixer tells of it (OB_E_CHANGE of OB_SND_MIX), which is
 * how 音量ミキサー shows it; the message line says it for a moment.
 * Answers the volume set, or -1 for a key that is not one of them (or
 * a mixer that took nothing).
 */
#define VOL_STEP	5

IMPORT void knl_msg_note( CONST char *utf8 );	/* msgline.c */

EXPORT INT knl_wm_volume_key( UINT code )
{
	SDMix		m;
	SDMixSet	s;
	INT		v, n;
	char		t[32];

	if ( code != HID_KEY_VOLUP && code != HID_KEY_VOLDOWN ) {
		return -1;
	}
	knl_memset(&m, 0, sizeof(m));
	(void)knl_snd_mix_get(&m, 0);
	v = (INT)( ( m.lv > m.rv ) ? m.lv : m.rv );
	v += ( code == HID_KEY_VOLUP ) ? VOL_STEP : -VOL_STEP;
	if ( v < 0 ) v = 0;
	if ( v > SND_VOL_MAX ) v = SND_VOL_MAX;
	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = s.rv = (UINT)v;
	s.mute = 0;
	if ( knl_snd_mix_set(&s, TRUE) < E_OK ) {
		return -1;
	}
	/* 音量 45% */
	n = 0;
	knl_memcpy(t, "音量 ", sizeof("音量 ") - 1);
	n += sizeof("音量 ") - 1;
	if ( v >= 100 ) t[n++] = (char)( '0' + v / 100 );
	if ( v >= 10 )  t[n++] = (char)( '0' + ( v / 10 ) % 10 );
	t[n++] = (char)( '0' + v % 10 );
	t[n++] = '%';
	t[n] = 0;
	knl_msg_note(t);
	return v;
}
