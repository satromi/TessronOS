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
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/look.h>
#include <ts/snd.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "obj/obj.h"

#define TONE_RATE	48000
#define SOUND_MAX	( 512 * 1024 )		/* bytes of a sound played */
#define TONE_AMP	6000

LOCAL ID	beep_tsk = 0;
LOCAL ID	beep_flg = 0;
LOCAL volatile UINT beep_what;
LOCAL UB	*beep_buf = NULL;

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

/* The tone an object's metadata says: a square wave of hz for ms, into buf */
LOCAL INT tone_make( ID key, UB *buf, SDPcmMode *m )
{
	UB	*j = (UB *)Kmalloc(OB_META_MAX);
	SZ	asz = 0;
	D	hz = 2000, ms = 100;
	INT	p, n, i, half, k;
	H	*pcm = (H *)buf;

	if ( j == NULL ) {
		return 0;
	}
	if ( ob_get_atr(key, j, OB_META_MAX - 1, &asz) >= E_OK ) {
		p = knl_oj_path(j, (INT)asz, "tessronos", "sound");
		p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "tone") : -1;
		if ( p >= 0 ) {
			INT	q = knl_oj_member(j, (INT)asz, p, "hz");

			if ( q >= 0 ) (void)knl_oj_num(j, (INT)asz, q, &hz);
			q = knl_oj_member(j, (INT)asz, p, "ms");
			if ( q >= 0 ) (void)knl_oj_num(j, (INT)asz, q, &ms);
		}
	}
	Kfree(j);
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

/* A sound object played through the sound device's object, to its end */
LOCAL void play( CONST char *uuid )
{
	TS_UUID		u, dev;
	SDPcmMode	m;
	ID		key, dk;
	SZ		asz = 0;
	INT		off = 0, n = 0, i;
	W		v;

	if ( ts_str_to_uuid(uuid, &u) < E_OK || knl_obdev_uuid((CONST UB *)SND_DEVNM, &dev) < E_OK ) {
		return;
	}
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) {
		return;
	}
	if ( ob_rea_rec(key, 1, 0, beep_buf, SOUND_MAX, &asz) < E_OK || asz <= 0
	  || !wav_parse(beep_buf, (INT)( asz > SOUND_MAX ? SOUND_MAX : asz ), &off, &n, &m) ) {
		off = 0;
		n = tone_make(key, beep_buf, &m);
	}
	ob_cls_obj(key);
	if ( n <= 0 ) {
		return;
	}

	dk = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
	if ( dk < E_OK ) {
		return;
	}
	v = n;
	if ( ob_wri_rec(dk, OB_SND_MODE, 0, &m, sizeof(m), &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_BUFSZ, 0, &v, sizeof(v), &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_PCM, 0, beep_buf + off, n, &asz) >= E_OK
	  && ob_wri_rec(dk, OB_SND_ENDCNT, 0, &v, sizeof(v), &asz) >= E_OK ) {
		v = PCM_PLAY;
		if ( ob_wri_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz) >= E_OK ) {
			/* until it has played, and no longer than the longest may take */
			for ( i = 0; i < 400; i++ ) {
				tk_dly_tsk(10);
				if ( ob_rea_rec(dk, OB_SND_CTL, 0, &v, sizeof(v), &asz) < E_OK || v == PCM_STOP ) {
					break;
				}
			}
		}
	}
	ob_cls_obj(dk);
}

LOCAL void beep_task( INT stacd, void *exinf )
{
	UINT	ptn;

	for ( ;; ) {
		if ( tk_wai_flg(beep_flg, 1, TWF_ORW | TWF_CLR, &ptn, TMO_FEVR) < E_OK ) {
			tk_dly_tsk(100);
			continue;
		}
		if ( wm_num(LK_SPEAKER, 1) != 0 && beep_what <= WM_BEEP_RELEASE ) {
			play(beep_obj[beep_what]);
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

	if ( what > WM_BEEP_RELEASE ) {
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
	return tk_set_flg(beep_flg, 1);
}
