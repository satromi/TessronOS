/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sndtest.c (Raspberry Pi 5)
 *	Checks on the board of the sound QEMU does not have (docs/tessronos/
 *	06-device.md 6.5): the I2S's clock and pace, the HDMI sockets, the
 *	USB audio functions, and moving the stream between them.
 *
 *	They run once in a task of their own some seconds after the system
 *	has come up, and write "sndtest:" lines to the console: one a check,
 *	then each check's result again in a list and a summary. Each check
 *	plays a quiet 440Hz tone for a few seconds on the output it is
 *	about, so the person at the board hears each output in turn; each
 *	has its own time limit, and one that fails lets the next run. The
 *	output chosen before is chosen again at the end. make SNDTEST=1 puts
 *	them in.
 *
 *	  outputs	the outputs there are, listed
 *	  i2s		clk_i2s counted is 3072kHz (within 1%) and the ring goes
 *			at 187.5 blocks a second (within 3%) over two seconds
 *	  hdmi0, hdmi1	the same pace on each socket that can sound; one that
 *			cannot (no screen, a DVI one, one without sound) is
 *			said and passed over
 *	  usb		a USB audio function plugged in takes 48000 frames a
 *			second (within 5%)
 *	  move		while one channel plays on, the stream is moved through
 *			every output that can sound and back to "auto"; the
 *			channel goes on playing and its count goes on
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/snd.h>
#include "sysdepend.h"
#include "../../../device/snd/sndhw.h"

IMPORT UW knl_usb_ms( void );

#ifndef CNF_SNDTEST
#define CNF_SNDTEST	0
#endif

#define TEST_PRI	30
#define TEST_STKSZ	16384
#define TEST_DELAY_MS	25000		/* after the desktop has come up and taken its settings */
#define PLAY_MS		2000		/* the pace is measured over this */
#define SETTLE_MS	500		/* after a start, before measuring */
#define NRESULT		12
#define TONE_FRAMES	48000		/* a second, played over and over */

/* Every output, ids and names whole, for the caller to Kfree; NULL when there is no memory */
LOCAL SDOuts *outs_read( void )
{
	SDOuts	*l = NULL;
	SZ	room = 0, need;

	while ( ( need = knl_snd_outputs_v(l, room) ) > room ) {
		if ( l != NULL ) Kfree(l);
		room = need;
		l = (SDOuts *)Kmalloc(room);
		if ( l == NULL ) return NULL;
	}
	if ( need < 0 ) {
		if ( l != NULL ) Kfree(l);
		return NULL;
	}
	return l;
}

/* The output chosen and the one playing, whole, for the caller to Kfree; NULL when there is no memory */
LOCAL SDOutSelV *sel_read( void )
{
	SDOutSelV	*s = NULL;
	SZ		room = 0, need;

	while ( ( need = knl_snd_sel_v(s, room) ) > room ) {
		if ( s != NULL ) Kfree(s);
		room = need;
		s = (SDOutSelV *)Kmalloc(room);
		if ( s == NULL ) return NULL;
	}
	return s;
}

LOCAL ID	dd = 0;
LOCAL H		*tone = NULL;
LOCAL INT	npass = 0, nrun = 0, nres = 0;
LOCAL char	res_name[NRESULT][16];
LOCAL INT	res_ok[NRESULT];		/* 1 ok, 0 FAIL, -1 skipped */

LOCAL void note( CONST char *name, INT ok, CONST char *fmt, INT a, INT b, INT c )
{
	char	what[160];

	tm_sprintf((UB *)what, (CONST UB *)fmt, a, b, c);
	if ( ok >= 0 ) {
		nrun++;
		if ( ok ) npass++;
	}
	if ( nres < NRESULT ) {
		knl_strncpy(res_name[nres], name, sizeof(res_name[0]) - 1);
		res_ok[nres++] = ok;
	}
	tm_printf((UB *)"sndtest: %s %s: %s\n", name, ( ok > 0 ) ? "ok" : ( ok == 0 ) ? "FAIL" : "skipped", what);
}

LOCAL ER attr_w( W dn, W v )
{
	SZ	asz = 0;

	return tk_swri_dev(dd, dn, &v, sizeof(v), &asz);
}

LOCAL W attr_r( W dn )
{
	W	v = -1;
	SZ	asz = 0;

	tk_srea_dev(dd, dn, &v, sizeof(v), &asz);

	return v;
}

LOCAL ER choose( CONST UB *id )
{
	SZ	asz = 0;

	return tk_swri_dev(dd, DN_SDOUTSEL, (void *)id, (SZ)knl_strlen((CONST char *)id) + 1, &asz);
}


/* The tone playing over and over, from the top */
LOCAL ER tone_start( void )
{
	SDPcmMode	m;
	SZ		asz = 0;

	attr_w(DN_SDPCMCTL, PCM_STOP);
	knl_memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	tk_swri_dev(dd, DN_SDPCMMODE, &m, sizeof(m), &asz);
	attr_w(DN_SDPCMENDCNT, 0);
	attr_w(DN_SDPCMBUFSZ, TONE_FRAMES * 4);
	tk_swri_dev(dd, 0, tone, TONE_FRAMES * 4, &asz);

	return attr_w(DN_SDPCMCTL, PCM_REPPLAY);
}

LOCAL void tone_stop( void )
{
	attr_w(DN_SDPCMCTL, PCM_STOP);
	tk_dly_tsk(200);
}

/* Up to ms for the stream to play on id */
LOCAL BOOL wait_on( CONST UB *id, INT ms )
{
	SDOutSelV	*s;
	BOOL		on;

	for ( ; ms > 0; ms -= 50 ) {
		s = sel_read();
		on = ( s != NULL && knl_strcmp((CONST char *)SND_OUTSELV_NOW(s), (CONST char *)id) == 0 );
		if ( s != NULL ) Kfree(s);
		if ( on ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}

	return FALSE;
}

/* The output of id among the outputs: its flags, or -1 when it is not there */
LOCAL INT flags_of( CONST char *id )
{
	SDOuts	*l = outs_read();
	SDOutV	*e;
	UINT	k;
	INT	f = -1;

	for ( k = 0, e = ( l != NULL ) ? SND_OUTS_FIRST(l) : NULL; l != NULL && k < l->n; k++, e = SND_OUTV_NEXT(e) ) {
		if ( knl_strcmp((CONST char *)e->id, id) == 0 ) {
			f = (INT)e->flags;
			break;
		}
	}
	if ( l != NULL ) Kfree(l);

	return f;
}

/* ---------------------------------------------------------------- checks */

LOCAL void check_outputs( void )
{
	SDOuts	*l = outs_read();
	SDOutV	*e;
	UINT	k;
	INT	n = ( l != NULL ) ? (INT)l->n : 0;

	for ( k = 0, e = ( l != NULL ) ? SND_OUTS_FIRST(l) : NULL; l != NULL && k < l->n; k++, e = SND_OUTV_NEXT(e) ) {
		tm_printf((UB *)"sndtest:   %s \"%s\" kind %d flags %x\n", e->id, SND_OUTV_NAME(e),
			  (INT)e->kind, (INT)e->flags);
	}
	if ( l != NULL ) Kfree(l);
	note("outputs", ( n >= 2 && flags_of("i2s") >= 0 ) ? 1 : 0, "%d with auto, i2s there %d", n,
	     (INT)( flags_of("i2s") >= 0 ), 0);
}

/* 187.5 blocks a second within 3%, in tenths: 1819 .. 1931 */
LOCAL BOOL pace_ok( UW x10 )
{
	return ( x10 >= 1819 && x10 <= 1931 );
}

LOCAL void check_i2s( void )
{
	UW	b0, m0, b1, m1, khz = 0, x10;
	INT	ok;

	if ( choose((CONST UB *)"i2s") < E_OK || tone_start() < E_OK || !wait_on((CONST UB *)"i2s", 1000) ) {
		tone_stop();
		note("i2s", 0, "the stream does not play on it", 0, 0, 0);
		return;
	}
	tk_dly_tsk(SETTLE_MS);
	if ( !knl_i2s_pace(&b0, &m0, &khz) ) {
		tone_stop();
		note("i2s", 0, "not running", 0, 0, 0);
		return;
	}
	tk_dly_tsk(PLAY_MS);
	(void)knl_i2s_pace(&b1, &m1, &khz);
	tone_stop();
	x10 = ( m1 > m0 ) ? (UW)((UD)(b1 - b0) * 10000 / (m1 - m0)) : 0;
	ok = ( pace_ok(x10) && khz >= 3041 && khz <= 3103 );
	note("i2s", ok, "clk_i2s %d kHz (3072), %d.%d blocks a second (187.5)", (INT)khz,
	     (INT)(x10 / 10), (INT)(x10 % 10));
}

LOCAL void check_hdmi( INT h )
{
	CONST char	*id = ( h == 0 ) ? "hdmi0" : "hdmi1";
	INT		fl = flags_of(id), on;
	UW		b0, m0, b1, m1, x10;

	if ( fl < 0 ) {
		note(id, -1, "not driven by the firmware (no screen on it)", 0, 0, 0);
		return;
	}
	if ( (fl & SND_OF_NOSOUND) != 0 ) {
		note(id, -1, "its screen takes no sound, or it is sent DVI, or nothing is plugged in", 0, 0, 0);
		return;
	}
	if ( choose((CONST UB *)id) < E_OK || tone_start() < E_OK || !wait_on((CONST UB *)id, 1000) ) {
		tone_stop();
		note(id, 0, "the stream does not play on it", 0, 0, 0);
		return;
	}
	tk_dly_tsk(SETTLE_MS);
	on = knl_hdmi_pace(&b0, &m0);
	tk_dly_tsk(PLAY_MS);
	(void)knl_hdmi_pace(&b1, &m1);
	tone_stop();
	x10 = ( on == h && m1 > m0 ) ? (UW)((UD)(b1 - b0) * 10000 / (m1 - m0)) : 0;
	note(id, pace_ok(x10), "%d.%d blocks a second (187.5), socket %d playing", (INT)(x10 / 10),
	     (INT)(x10 % 10), on);
}

LOCAL void check_usb( void )
{
	SDOuts	*l = outs_read();
	SDOutV	*e = NULL;
	SDStat	s0, s1;
	SZ	asz = 0;
	UW	t0, t1, fps;
	UINT	k;
	BOOL	on;

	for ( k = 0, e = ( l != NULL ) ? SND_OUTS_FIRST(l) : NULL; l != NULL && k < l->n; k++, e = SND_OUTV_NEXT(e) ) {
		if ( e->kind == SND_BE_USB ) break;
	}
	if ( l == NULL || k >= l->n ) {
		if ( l != NULL ) Kfree(l);
		note("usb", -1, "no USB audio function plugged in", 0, 0, 0);
		return;
	}
	tm_printf((UB *)"sndtest:   usb: %s \"%s\"\n", e->id, SND_OUTV_NAME(e));
	on = ( choose(e->id) >= E_OK && tone_start() >= E_OK && wait_on(e->id, 1000) );
	Kfree(l);
	if ( !on ) {
		tone_stop();
		note("usb", 0, "the stream does not play on it", 0, 0, 0);
		return;
	}
	tk_dly_tsk(SETTLE_MS);
	tk_srea_dev(dd, DN_SDSTAT, &s0, sizeof(s0), &asz);
	t0 = knl_usb_ms();
	tk_dly_tsk(PLAY_MS);
	tk_srea_dev(dd, DN_SDSTAT, &s1, sizeof(s1), &asz);
	t1 = knl_usb_ms();
	tone_stop();
	fps = ( t1 > t0 ) ? (UW)((UD)(s1.out_frames - s0.out_frames) * 1000 / (t1 - t0)) : 0;
	note("usb", ( fps >= 45600 && fps <= 50400 ), "%d frames a second (48000), %d Hz %d channels",
	     (INT)fps, (INT)s1.out_rate, (INT)s1.out_channels);
}

/* The stream moved through every output that can sound while one channel plays on */
LOCAL void check_move( void )
{
	SDOuts	*l = outs_read();
	SDOutV	*e;
	UINT	k;
	INT	moved = 0, bad = 0;
	W	c0, c1;

	if ( l == NULL || tone_start() < E_OK ) {
		if ( l != NULL ) Kfree(l);
		tone_stop();
		note("move", 0, "nothing plays", 0, 0, 0);
		return;
	}
	for ( k = 0, e = SND_OUTS_FIRST(l); k < l->n; k++, e = SND_OUTV_NEXT(e) ) {
		if ( k == 0 || (e->flags & SND_OF_NOSOUND) != 0 ) {
			continue;		/* "auto", and what cannot sound */
		}
		c0 = attr_r(DN_SDPCMCNT);
		choose(e->id);
		if ( !wait_on(e->id, 1000) ) {
			tm_printf((UB *)"sndtest:   move: not on %s\n", e->id);
			bad++;
			continue;
		}
		tk_dly_tsk(1000);
		c1 = attr_r(DN_SDPCMCNT);
		if ( attr_r(DN_SDPCMCTL) != PCM_REPPLAY || c1 == c0 ) {
			tm_printf((UB *)"sndtest:   move: on %s the channel stopped\n", e->id);
			bad++;
		}
		moved++;
	}
	Kfree(l);
	choose((CONST UB *)SND_ID_AUTO);
	tk_dly_tsk(500);
	if ( attr_r(DN_SDPCMCTL) != PCM_REPPLAY ) {
		bad++;
	}
	tone_stop();
	note("move", ( bad == 0 && moved >= 1 ), "%d outputs gone through, %d wrong", moved, bad, 0);
}

/* ---------------------------------------------------------------- the task */

LOCAL void test_task( INT stacd, void *exinf )
{
	SDOutSelV	*was;
	INT		i;

	(void)stacd;
	(void)exinf;
	tk_dly_tsk(TEST_DELAY_MS);
	tm_printf((UB *)"sndtest: start\n");
	dd = tk_opn_dev((UB *)SND_DEVNM, TD_UPDATE);
	tone = (H *)Kmalloc(TONE_FRAMES * 4);
	if ( dd <= 0 || tone == NULL ) {
		tm_printf((UB *)"sndtest: no sound device (%d)\n", (INT)dd);
		tk_ext_tsk();
	}
	/* 440Hz, a triangle a quarter of the way up */
	for ( i = 0; i < TONE_FRAMES; i++ ) {
		INT	p = ( i * 440 ) % 48000;
		INT	v = ( p < 24000 ) ? p : 48000 - p;

		tone[i * 2] = tone[i * 2 + 1] = (H)(v * 8000 / 24000 - 4000);
	}
	was = sel_read();

	check_outputs();
	check_i2s();
	check_hdmi(0);
	check_hdmi(1);
	check_usb();
	check_move();

	choose(( was != NULL && was->id[0] != 0 ) ? was->id : (CONST UB *)SND_ID_AUTO);
	if ( was != NULL ) Kfree(was);
	tk_cls_dev(dd, 0);
	Kfree(tone);
	for ( i = 0; i < nres; i++ ) {
		tm_printf((UB *)"sndtest: result %s %s\n", res_name[i],
			  ( res_ok[i] > 0 ) ? "ok" : ( res_ok[i] == 0 ) ? "FAIL" : "skipped");
	}
	tm_printf((UB *)"sndtest: summary %d/%d passed, %d skipped\n", npass, nrun, nres - nrun);
	tk_ext_tsk();
}

EXPORT void knl_sndtest_start( void )
{
	T_CTSK	ctsk;
	ID	tid;

	if ( !CNF_SNDTEST ) {
		return;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)test_task;
	ctsk.itskpri = TEST_PRI;
	ctsk.stksz = TEST_STKSZ;
	tid = tk_cre_tsk(&ctsk);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		tm_printf((UB *)"sndtest: could not start (%d)\n", (INT)tid);
	}
}

#endif /* RPI5 */
