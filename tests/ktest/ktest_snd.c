/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_snd.c
 *	The sound device "snda" (design 10.15).
 *
 *	On the test machine the hardware under it is a USB audio function
 *	behind a hub: full speed, playback only, a packet of 48 frames every
 *	millisecond on an isochronous OUT endpoint. So playback is tested end
 *	to end -- the program's samples converted, mixed, cut into packets
 *	and taken by the device at its own pace -- and recording through the
 *	device's test source, which stands in for hardware the machine does
 *	not have.
 *
 *	The Raspberry Pi 5's I2S backend cannot run here: SKIP. On the
 *	Raspberry Pi 5 itself the tests of playing, which are written for
 *	the USB audio function, are not run; opening, recording from the
 *	test source and closing are.
 *
 *	On the x64 PC, recording through Intel HDA is tested when QEMU is
 *	given its intel-hda and hda-duplex (tools/x64/ktest64.sh, EXTRA);
 *	without them it is skipped.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/snd.h>
#include <ts/usb.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/conf.h>

LOCAL ID	snd_dd = 0;

LOCAL ER attr_w( ID dd, W dn, W v )
{
	SZ	asz = 0;

	return tk_swri_dev(dd, dn, &v, sizeof(v), &asz);
}

LOCAL W attr_r( ID dd, W dn )
{
	W	v = -1;
	SZ	asz = 0;

	tk_srea_dev(dd, dn, &v, sizeof(v), &asz);

	return v;
}

LOCAL ER set_mode( ID dd, UINT rate, UINT fmt, UINT stereo )
{
	SDPcmMode	m;
	SZ		asz = 0;

	knl_memset(&m, 0, sizeof(m));
	m.sampling = rate;
	m.datafmt = fmt;
	m.stereo = stereo;

	return tk_swri_dev(dd, DN_SDPCMMODE, &m, sizeof(m), &asz);
}

LOCAL void get_stat( ID dd, SDStat *st )
{
	SZ	asz = 0;

	knl_memset(st, 0, sizeof(*st));
	tk_srea_dev(dd, DN_SDSTAT, st, sizeof(*st), &asz);
}

#if KT_QEMU_DEVS
/* A tone as a triangle wave: 16 bit, stereo, frames of it at rate */
LOCAL void tone16( H *buf, INT frames, INT period )
{
	INT	i;

	for ( i = 0; i < frames; i++ ) {
		INT	p = i % period;
		INT	v = ( p < period / 2 ) ? p : period - p;

		v = (v * 4 * 8000) / period - 8000;
		buf[i * 2] = (H)v;
		buf[i * 2 + 1] = (H)v;
	}
}
#endif /* KT_BOARD */

/* Wait for a channel to stop by itself, up to ms */
LOCAL BOOL wait_stop( ID dd, INT ms )
{
	INT	i;

	for ( i = 0; i < ms / 20; i++ ) {
		if ( attr_r(dd, DN_SDPCMCTL) == PCM_STOP ) {
			return TRUE;
		}
		tk_dly_tsk(20);
	}

	return FALSE;
}

/* ---------------------------------------------------------------- tests */

LOCAL void test_open( void )
{
	SDPcmInfo	in;
	SDStat		st;
	SDSel		sel;
	SZ		asz = 0;
	ER		er;

	snd_dd = tk_opn_dev((UB *)SND_DEVNM, TD_UPDATE);
	if ( snd_dd <= 0 ) KT_SKIP("no sound device");

	/* a read of size 0 says how large the data is */
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDPCMINFO, &in, 0, &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(SDPcmInfo));
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDPCMINFO, &in, sizeof(in), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(SDPcmInfo));
#if !KT_BOARD
	/* what the USB audio function of the QEMU machine offers */
	KT_ASSERT_EQ(in.spt[PCMST48K].splay, 1);
	KT_ASSERT_EQ(in.spt[PCMST8K].mplay, 1);
	KT_ASSERT_EQ(in.spt[PCMST44K].datafmt, 7);	/* mu-law, 8 and 16 bit */
#else
	/* on the board, what is plugged in, if anything: said, not required */
	tm_printf((UB *)"  48kHz: stereo %d mono %d, formats %x\n",
		  (INT)in.spt[PCMST48K].splay, (INT)in.spt[PCMST48K].mplay,
		  (INT)in.spt[PCMST48K].datafmt);
#endif
	KT_ASSERT_EQ(in.spt[6].datafmt, 0);		/* no such rate */

	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDSELOUT, &sel, sizeof(sel), &asz), E_OK);
#if !KT_BOARD
	KT_ASSERT_EQ(sel.pcm, 1);
#endif

	/* a new channel starts at 48kHz, 16 bit, stereo, stopped */
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_STOP);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMBUFSZ), 32768);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMENDCNT), PCM_COUNT_NONE);
	get_stat(snd_dd, &st);
	KT_ASSERT_EQ(st.backend, SND_BE_NONE);

	/* what is refused */
	KT_ASSERT_ER(set_mode(snd_dd, 7, PCMFmt16, 1), E_NOSPT);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, 9), E_PAR);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMBUFSZ, 0), E_PAR);
	KT_ASSERT(attr_w(snd_dd, -150, 0) < E_OK);
	/*
	 * nothing records on this machine, unless it has Intel HDA (the
	 * x64 PC, QEMU with intel-hda): then the recording is stopped
	 */
	er = attr_w(snd_dd, DN_SDPCMCTL, PCM_REC);
#if defined(X64)
	if ( er == E_OK ) {
		KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
		er = E_NOSPT;
	}
#endif
	KT_ASSERT_ER(er, E_NOSPT);
}

#if KT_QEMU_DEVS
/*
 * One second of a tone at 48kHz: the device takes it packet by packet,
 * a packet a millisecond, and the channel stops at its stop count once
 * what was queued has been heard.
 */
LOCAL void test_play( void )
{
	SDStat		s0, s1;
	T_USBSTAT	u0, u1;
	H		*pcm;
	SZ		asz = 0;
	INT		bytes = 48000 * 4, sent = 0;
	UD		t0 = 0, t1 = 0;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	pcm = (H *)Kmalloc((SZ)bytes);
	KT_ASSERT(pcm != NULL);
	if ( pcm == NULL ) return;
	tone16(pcm, 48000, 109);			/* about 440Hz */

	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMBUFSZ, bytes), E_OK);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMBUFSZ), bytes);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, bytes, &asz), E_OK);
	KT_ASSERT_EQ(asz, bytes);
	/* full: a write finds no room */
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 1, pcm, 64, &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, bytes), E_OK);

	get_stat(snd_dd, &s0);
	ts_usb_stat(&u0);
	ts_get_mono(&t0);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	/* playing: a second start is refused, and so is a new mode */
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_BUSY);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST8K, PCMFmt16, 0), E_BUSY);
	tk_dly_tsk(200);
	get_stat(snd_dd, &s1);
	KT_ASSERT_EQ(s1.backend, SND_BE_USB);
	KT_ASSERT_EQ(s1.out_rate, 48000);
	KT_ASSERT_EQ(s1.out_channels, 2);
	KT_ASSERT_EQ(s1.playing, 1);

	KT_ASSERT(wait_stop(snd_dd, 4000));
	ts_get_mono(&t1);
	sent = attr_r(snd_dd, DN_SDPCMCNT);
	get_stat(snd_dd, &s1);
	ts_usb_stat(&u1);
	tm_printf((UB*)"  %d bytes in %d ms: %d frames to the device, %d packets "
		  "(%d missed, %d errors, %d underruns), %d short\n", sent,
		  (INT)((t1 - t0) / 1000000), (INT)(s1.out_frames - s0.out_frames),
		  (INT)(u1.iso_packets - u0.iso_packets),
		  (INT)(u1.iso_missed - u0.iso_missed),
		  (INT)(u1.iso_errors - u0.iso_errors),
		  (INT)(u1.out_underruns - u0.out_underruns),
		  (INT)(s1.out_short - s0.out_short));
	KT_ASSERT_EQ(sent, bytes);
	/* the second of samples and the silence after it went to the device */
	KT_ASSERT(s1.out_frames - s0.out_frames >= 48000);
	KT_ASSERT(u1.iso_packets - u0.iso_packets >= 1000);
	KT_ASSERT_EQ(u1.iso_errors - u0.iso_errors, 0);
	KT_ASSERT_EQ(s1.underruns - s0.underruns, 0);
	/* it took the device's time: a second at least, not a burst */
	KT_ASSERT((t1 - t0) >= 900000000ULL);
	Kfree(pcm);

	/* the stream stops with the last channel */
	tk_dly_tsk(100);
	get_stat(snd_dd, &s1);
	KT_ASSERT_EQ(s1.backend, SND_BE_NONE);
}

/*
 * Two channels at once, of different rates and formats: the one opened
 * as "snda" and another as "snda0". Both are converted and mixed into
 * the one stream.
 */
LOCAL void test_mix( void )
{
	ID	dd2;
	H	*pcm;
	static UB	ulaw[8000];
	SDStat	st;
	SZ	asz = 0;
	INT	i, maxplay = 0;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	dd2 = tk_opn_dev((UB *)"snda0", TD_UPDATE);
	KT_ASSERT(dd2 > 0);
	if ( dd2 <= 0 ) return;
	pcm = (H *)Kmalloc(24000 * 4);
	KT_ASSERT(pcm != NULL);
	if ( pcm == NULL ) {
		tk_cls_dev(dd2, 0);
		return;
	}
	tone16(pcm, 24000, 80);
	for ( i = 0; i < 8000; i++ ) {
		ulaw[i] = ( (i / 10) & 1 ) ? 0x80 : 0x00;	/* loud mu-law square */
	}

	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMBUFSZ, 24000 * 4), E_OK);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, 24000 * 4, &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 24000 * 4), E_OK);

	KT_ASSERT_ER(set_mode(dd2, PCMST8K, PCMFmt8ulaw, 0), E_OK);
	KT_ASSERT_ER(tk_swri_dev(dd2, 0, ulaw, sizeof(ulaw), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(ulaw));
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMENDCNT, sizeof(ulaw)), E_OK);

	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMCTL, PCM_PLAY), E_OK);
	for ( i = 0; i < 30; i++ ) {
		get_stat(snd_dd, &st);
		if ( (INT)st.playing > maxplay ) {
			maxplay = (INT)st.playing;
		}
		tk_dly_tsk(20);
	}
	KT_ASSERT_EQ(maxplay, 2);
	KT_ASSERT(st.channels >= 2);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	KT_ASSERT(wait_stop(dd2, 3000));
	KT_ASSERT_EQ(attr_r(dd2, DN_SDPCMCNT), (W)sizeof(ulaw));
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCNT), 24000 * 4);
	Kfree(pcm);
	KT_ASSERT_ER(tk_cls_dev(dd2, 0), E_OK);
}

/*
 * More channels than the table first has room for (16). A channel is a
 * program's (a task's here) on a unit, so each of KT_CHANS tasks opens
 * the unit and holds its channel while all the others hold theirs; then
 * each plays what it was given to the end and closes.
 */
#define KT_CHANS	20

LOCAL ID	ch_open, ch_go, ch_done;
LOCAL INT	ch_ok[KT_CHANS];

LOCAL void ch_task( INT i, void *exinf )
{
	static UB	ulaw[800];
	SZ		asz = 0;
	ID		dd;
	INT		k, ok = -1;

	for ( k = 0; k < (INT)sizeof(ulaw); k++ ) ulaw[k] = ( (k / 10) & 1 ) ? 0x80 : 0x00;
	dd = tk_opn_dev((UB *)"snda0", TD_UPDATE);
	/* its channel is made at its first request */
	if ( dd > 0 && set_mode(dd, PCMST8K, PCMFmt8ulaw, 0) >= E_OK ) ok = 0;
	tk_sig_sem(ch_open, 1);
	tk_wai_sem(ch_go, 1, 10000);
	/* what went wrong, when it did: the step's number, negative */
	if ( ok == 0 ) ok = ( tk_swri_dev(dd, 0, ulaw, sizeof(ulaw), &asz) < E_OK ) ? -3
			  : ( attr_w(dd, DN_SDPCMENDCNT, sizeof(ulaw)) < E_OK ) ? -4
			  : ( attr_w(dd, DN_SDPCMCTL, PCM_PLAY) < E_OK ) ? -5
			  : !wait_stop(dd, 10000) ? -6
			  : ( attr_r(dd, DN_SDPCMCNT) != (W)sizeof(ulaw) ) ? -7 : 1;
	ch_ok[i] = ok;
	if ( dd > 0 ) {
		(void)attr_w(dd, DN_SDPCMCTL, PCM_STOP);	/* none left playing for the tests after */
		tk_cls_dev(dd, 0);
	}
	tk_sig_sem(ch_done, 1);
	tk_ext_tsk();
}

LOCAL void test_channels( void )
{
	T_CSEM	cs;
	T_CTSK	ct;
	ID	tid[KT_CHANS];
	INT	i, n = 0, good = 0;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.maxsem = KT_CHANS;
	ch_open = tk_cre_sem(&cs);
	ch_go = tk_cre_sem(&cs);
	ch_done = tk_cre_sem(&cs);
	KT_ASSERT(ch_open > 0 && ch_go > 0 && ch_done > 0);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ch_task;
	ct.itskpri = 20;
	ct.stksz = 8 * 1024;
	for ( i = 0; i < KT_CHANS; i++ ) {
		ch_ok[i] = -2;
		tid[i] = tk_cre_tsk(&ct);
		if ( tid[i] > 0 && tk_sta_tsk(tid[i], i) >= E_OK ) n++;
	}
	KT_ASSERT_EQ(n, KT_CHANS);
	/* every one holds its channel at once */
	for ( i = 0; i < n; i++ ) (void)tk_wai_sem(ch_open, 1, 10000);
	for ( i = 0; i < n; i++ ) KT_ASSERT_EQ(ch_ok[i], -2);
	(void)tk_sig_sem(ch_go, n);
	for ( i = 0; i < n; i++ ) (void)tk_wai_sem(ch_done, 1, 20000);
	for ( i = 0; i < n; i++ ) {
		if ( ch_ok[i] == 1 ) good++;
		else tm_printf((UB *)"  channel %d: step %d\n", i, ch_ok[i]);
	}
	KT_ASSERT_EQ(good, KT_CHANS);
	tk_dly_tsk(20);
	for ( i = 0; i < KT_CHANS; i++ ) if ( tid[i] > 0 ) (void)tk_del_tsk(tid[i]);
	(void)tk_del_sem(ch_open);
	(void)tk_del_sem(ch_go);
	(void)tk_del_sem(ch_done);
}

/* A channel that runs out of samples stops by itself: an underrun */
LOCAL void test_underrun( void )
{
	static H	pcm[4800 * 2];
	SDStat	s0, s1;
	SZ	asz = 0;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	tone16(pcm, 4800, 50);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);	/* none */
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, sizeof(pcm), &asz), E_OK);
	get_stat(snd_dd, &s0);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	get_stat(snd_dd, &s1);
	KT_ASSERT_EQ(s1.underruns - s0.underruns, 1);
	/*
	 * All of it was played but what was too little for one packet: a
	 * stream that can wait sends a packet only once it is full.
	 */
	KT_ASSERT(attr_r(snd_dd, DN_SDPCMCNT) > (W)sizeof(pcm) - 48 * 4);
	KT_ASSERT(attr_r(snd_dd, DN_SDPCMCNT) <= (W)sizeof(pcm));
}

/* Room in the channel's FIFO: a write of size 0 */
LOCAL INT room( ID dd )
{
	UB	b[4];
	SZ	asz = 0;

	tk_swri_dev(dd, 0, b, 0, &asz);

	return (INT)asz;
}

/* Bytes taken by a write of data number dn */
LOCAL INT put( ID dd, W dn, CONST H *pcm, INT bytes )
{
	SZ	asz = 0;

	if ( tk_swri_dev(dd, dn, (void *)pcm, bytes, &asz) < E_OK ) {
		return -1;
	}

	return (INT)asz;
}

/* All that was queued was played, but for less than a packet at the end */
#define PLAYED_ALL(n, bytes)	( (n) > (W)(bytes) - 48 * 4 && (n) <= (W)(bytes) )

/*
 * A stream queued in several writes of data number 0 before it starts,
 * as a program that writes what it has as it comes does: only the first
 * write after the stop starts at the top of the buffer, the others go
 * on after it, and every byte written is played. (Each write starting
 * at the top again would leave only the last, and the count would never
 * reach what the program wrote.)
 */
LOCAL void test_prime( void )
{
	static H	pcm[1024 * 2];
	SDStat	s0, s1;
	INT	bufsz, i;
	W	n;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	tone16(pcm, 1024, 64);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);	/* none */
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	bufsz = attr_r(snd_dd, DN_SDPCMBUFSZ);
	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT_EQ(put(snd_dd, 0, pcm, sizeof(pcm)), (INT)sizeof(pcm));
	}
	KT_ASSERT_EQ(room(snd_dd), bufsz - 3 * (INT)sizeof(pcm));
	get_stat(snd_dd, &s0);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	get_stat(snd_dd, &s1);
	n = attr_r(snd_dd, DN_SDPCMCNT);
	tm_printf((UB *)"  3 writes of %d bytes before the start: %d bytes played\n",
		  (INT)sizeof(pcm), n);
	KT_ASSERT(PLAYED_ALL(n, 3 * sizeof(pcm)));
	KT_ASSERT_EQ(s1.underruns - s0.underruns, 1);
}

/*
 * Stopping and starting again: a stream stopped with samples still in
 * the buffer leaves them behind when the next one begins with data
 * number 0; data number 1 goes on after it; a writer that pauses for
 * less than an underrun takes (the buffer running empty meanwhile)
 * keeps playing and goes on where it was.
 */
LOCAL void test_restart( void )
{
	H	*pcm;
	SDStat	s0, s1;
	INT	bufsz;
	W	n, ctl;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	pcm = (H *)Kmalloc(4800 * 4);
	KT_ASSERT(pcm != NULL);
	if ( pcm == NULL ) return;
	tone16(pcm, 4800, 100);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	bufsz = attr_r(snd_dd, DN_SDPCMBUFSZ);

	/* stopped with 70ms of it left */
	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 4800 * 4), 4800 * 4);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	tk_dly_tsk(30);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT(room(snd_dd) < bufsz);

	/* the next stream: number 0 at the top, number 1 after it */
	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 960 * 4), 960 * 4);
	KT_ASSERT_EQ(room(snd_dd), bufsz - 960 * 4);
	KT_ASSERT_EQ(put(snd_dd, 1, pcm, 960 * 4), 960 * 4);
	KT_ASSERT_EQ(room(snd_dd), bufsz - 2 * 960 * 4);
	get_stat(snd_dd, &s0);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	n = attr_r(snd_dd, DN_SDPCMCNT);
	tm_printf((UB *)"  after a stop with samples left: %d of %d bytes played\n", n, 2 * 960 * 4);
	KT_ASSERT(PLAYED_ALL(n, 2 * 960 * 4));

	/* a writer that pauses: 100ms of samples, 250ms of nothing, 100ms more */
	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 4800 * 4), 4800 * 4);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	tk_dly_tsk(350);
	ctl = attr_r(snd_dd, DN_SDPCMCTL);
	KT_ASSERT_EQ(ctl, PCM_PLAY);
	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 4800 * 4), 4800 * 4);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	get_stat(snd_dd, &s1);
	n = attr_r(snd_dd, DN_SDPCMCNT);
	tm_printf((UB *)"  a writer that paused 250ms: %d of %d bytes played, %d underruns\n",
		  n, 2 * 4800 * 4, (INT)(s1.underruns - s0.underruns));
	KT_ASSERT(PLAYED_ALL(n, 2 * 4800 * 4));
	/* one underrun for each of the two streams, at their ends; none in the pause */
	KT_ASSERT_EQ(s1.underruns - s0.underruns, 2);
	Kfree(pcm);
}

/*
 * One writer giving way to another: "snda0" starts while "snda" plays,
 * "snda" stops, "snda0" plays on to its end; the stream stops with the
 * last channel and starts again for the next writer, whose count is its
 * own from zero.
 */
LOCAL void test_switch( void )
{
	ID	dd2;
	H	*pcm;
	SDStat	st;
	W	n;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	dd2 = tk_opn_dev((UB *)"snda0", TD_UPDATE);
	KT_ASSERT(dd2 > 0);
	if ( dd2 <= 0 ) return;
	pcm = (H *)Kmalloc(4800 * 4);
	KT_ASSERT(pcm != NULL);
	if ( pcm == NULL ) {
		tk_cls_dev(dd2, 0);
		return;
	}
	tone16(pcm, 4800, 60);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMENDCNT, 0), E_OK);
	KT_ASSERT_ER(set_mode(dd2, PCMST48K, PCMFmt16, 1), E_OK);

	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 4800 * 4), 4800 * 4);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	tk_dly_tsk(40);
	KT_ASSERT_EQ(put(dd2, 0, pcm, 4800 * 4), 4800 * 4);
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	get_stat(snd_dd, &st);
	KT_ASSERT_EQ(st.backend, SND_BE_USB);
	KT_ASSERT_EQ(st.playing, 1);
	KT_ASSERT(wait_stop(dd2, 3000));
	n = attr_r(dd2, DN_SDPCMCNT);
	KT_ASSERT(PLAYED_ALL(n, 4800 * 4));
	tk_dly_tsk(100);
	get_stat(snd_dd, &st);
	KT_ASSERT_EQ(st.backend, SND_BE_NONE);

	/* the first writer again, a new stream */
	KT_ASSERT_EQ(put(snd_dd, 0, pcm, 2400 * 4), 2400 * 4);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	get_stat(snd_dd, &st);
	KT_ASSERT_EQ(st.backend, SND_BE_USB);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	n = attr_r(snd_dd, DN_SDPCMCNT);
	tm_printf((UB *)"  the second writer %d bytes, the first again %d of %d\n",
		  attr_r(dd2, DN_SDPCMCNT), n, 2400 * 4);
	KT_ASSERT(PLAYED_ALL(n, 2400 * 4));
	Kfree(pcm);
	KT_ASSERT_ER(tk_cls_dev(dd2, 0), E_OK);
}
#endif /* KT_BOARD */

/*
 * Recording, from the device's test source: 48kHz stereo in, 16kHz mono
 * 16 bit out. A second of input is a third as many frames, each the
 * mean of left and right.
 */
LOCAL void test_record( void )
{
	UB	*in;
	static UB	out[4096];
	SZ	asz = 0;
	INT	i, got = 0, bad = 0, n;
	W	bytes;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	in = (UB *)Kmalloc(4800 * 4);
	KT_ASSERT(in != NULL);
	if ( in == NULL ) return;
	for ( i = 0; i < 4800; i++ ) {
		in[i * 4]     = (UB)(1000 & 0xff);		/* left 1000 */
		in[i * 4 + 1] = (UB)(1000 >> 8);
		in[i * 4 + 2] = (UB)(3000 & 0xff);		/* right 3000 */
		in[i * 4 + 3] = (UB)(3000 >> 8);
	}
	knl_snd_loopback(TRUE);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST16K, PCMFmt16, 0), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMBUFSZ, 65536), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REC), E_OK);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_REC);

	for ( i = 0; i < 10; i++ ) {
		knl_snd_feed(in, 4800, 2);		/* a second, in tenths */
	}
	/* what came in is there to read: a third of the frames, mono */
	KT_ASSERT_ER(tk_srea_dev(snd_dd, 0, out, 0, &asz), E_OK);
	bytes = (W)asz;
	tm_printf((UB*)"  48000 frames in, %d bytes recorded\n", bytes);
	KT_ASSERT(bytes >= 31990 && bytes <= 32010);
	while ( got < bytes ) {
		KT_ASSERT_ER(tk_srea_dev(snd_dd, 1, out, sizeof(out), &asz), E_OK);
		n = (INT)asz;
		if ( n <= 0 ) break;
		for ( i = 0; i + 1 < n; i += 2 ) {
			H	v = (H)(out[i] | (out[i + 1] << 8));

			if ( v != 2000 ) bad++;
		}
		got += n;
	}
	KT_ASSERT_EQ(got, bytes);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCNT), bytes);

	/* a stop count ends it by itself; 8 bit mu-law this time */
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST8K, PCMFmt8ulaw, 1), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 1000), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REC), E_OK);
	knl_snd_feed(in, 4800, 2);
	KT_ASSERT(wait_stop(snd_dd, 1000));
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCNT), 1000);
	KT_ASSERT_ER(tk_srea_dev(snd_dd, 0, out, sizeof(out), &asz), E_OK);
	KT_ASSERT_EQ(asz, 1000);
	KT_ASSERT(out[0] == out[2] && out[0] != out[1]);	/* left and right apart */

	knl_snd_loopback(FALSE);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	Kfree(in);
}

#if KT_QEMU_DEVS
/* Room for the outputs a test looks at: more than it ever plugs in */
#define KT_OUTS		32

/* The outputs there are now: how many, and the one of kind with port (NULL any) */
LOCAL INT outs( SDOut *o, INT max )
{
	SZ	asz = 0;

	if ( tk_srea_dev(snd_dd, DN_SDOUTS, o, (SZ)sizeof(SDOut) * max, &asz) < E_OK ) {
		return 0;
	}

	return (INT)(asz / (SZ)sizeof(SDOut));
}

/* The id of the USB output whose id ends in port (its hub port, as ".1"), into id; FALSE none */
LOCAL BOOL usb_out( CONST char *port, UB *id )
{
	SDOut	o[KT_OUTS];
	INT	n = outs(o, KT_OUTS), k, a, b;

	for ( k = 0; k < n; k++ ) {
		a = (INT)knl_strlen((CONST char *)o[k].id);
		b = (INT)knl_strlen(port);
		if ( o[k].kind == SND_BE_USB && a > b
		  && knl_strcmp((CONST char *)o[k].id + a - b, port) == 0 ) {
			knl_strcpy((char *)id, (CONST char *)o[k].id);
			return TRUE;
		}
	}

	return FALSE;
}

/* Up to ms for the USB output at hub port port to come (want) or go (!want) */
LOCAL BOOL wait_usb_out( CONST char *port, BOOL want, INT ms )
{
	UB	id[SND_ID_MAX];

	for ( ; ms > 0; ms -= 50 ) {
		if ( usb_out(port, id) == want ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}

	return FALSE;
}

LOCAL void sel_get( SDOutSel *s )
{
	SZ	asz = 0;

	knl_memset(s, 0, sizeof(*s));
	tk_srea_dev(snd_dd, DN_SDOUTSEL, s, sizeof(*s), &asz);
}

LOCAL ER sel_put( CONST char *id )
{
	SZ	asz = 0;

	return tk_swri_dev(snd_dd, DN_SDOUTSEL, (void *)id, (SZ)knl_strlen(id) + 1, &asz);
}

/* Up to ms for the stream to play on id */
LOCAL BOOL wait_now( CONST UB *id, INT ms )
{
	SDOutSel	s;

	for ( ; ms > 0; ms -= 20 ) {
		sel_get(&s);
		if ( knl_strcmp((CONST char *)s.now, (CONST char *)id) == 0 ) {
			return TRUE;
		}
		tk_dly_tsk(20);
	}

	return FALSE;
}

LOCAL void outputs_by_records( CONST UB *id1 );

#define QMP_AUD2_ADD	"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-audio\"," \
			"\"bus\":\"xhci.0\",\"port\":\"4.4\",\"audiodev\":\"snd0\",\"id\":\"aud2\"}}\n"
#define QMP_AUD2_DEL	"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"aud2\"}}\n"
#define QMP_AUD1_ADD	"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-audio\"," \
			"\"bus\":\"xhci.0\",\"port\":\"4.1\",\"audiodev\":\"snd0\",\"id\":\"aud1\"}}\n"
#define QMP_AUD1_DEL	"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"aud1\"}}\n"

/*
 * The outputs, and choosing one: "auto" and the USB audio function of
 * the machine are listed; a second function is plugged in at 4.4 and
 * chosen, then the first while the stream plays, which moves the
 * stream without stopping the channel; the chosen one taken out leaves
 * the sound on the other, and put back, takes it again.
 */
LOCAL void test_outputs( void )
{
	static SDOut	o[KT_OUTS];
	SDOutSel	s;
	UB		id1[SND_ID_MAX], id2[SND_ID_MAX];
	H		*pcm;
	SZ		asz = 0;
	INT		n, bytes = 4800 * 4;
	W		c0, c1;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	n = outs(o, KT_OUTS);
	KT_ASSERT(n >= 2);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)o[0].id, SND_ID_AUTO), 0);
	KT_ASSERT((o[0].flags & SND_OF_CHOSEN) != 0);
	KT_ASSERT(usb_out(".1", id1));
	tm_printf((UB *)"  %d outputs; the machine's: %s \"%s\"\n", n, id1, o[1].name);
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, SND_ID_AUTO), 0);
	KT_ASSERT_EQ(s.now[0], 0);
	KT_ASSERT_ER(sel_put("speaker"), E_PAR);
	KT_ASSERT_ER(sel_put(""), E_PAR);

	/* a second function, chosen before anything plays */
	tm_printf((UB *)QMP_AUD2_ADD);
	if ( !wait_usb_out(".4", TRUE, 8000) ) {
		KT_SKIP("no QEMU monitor: no second USB audio function");
	}
	KT_ASSERT(usb_out(".4", id2));
	KT_ASSERT_ER(sel_put((CONST char *)id2), E_OK);
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, (CONST char *)id2), 0);

	pcm = (H *)Kmalloc((SZ)bytes);
	KT_ASSERT(pcm != NULL);
	if ( pcm == NULL ) return;
	tone16(pcm, 4800, 100);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, bytes, &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REPPLAY), E_OK);
	KT_ASSERT(wait_now(id2, 500));

	/* the first chosen while it plays: the stream moves, the channel plays on */
	c0 = attr_r(snd_dd, DN_SDPCMCNT);
	KT_ASSERT_ER(sel_put((CONST char *)id1), E_OK);
	KT_ASSERT(wait_now(id1, 500));
	tk_dly_tsk(200);
	c1 = attr_r(snd_dd, DN_SDPCMCNT);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_REPPLAY);
	KT_ASSERT(c1 != c0);
	n = outs(o, KT_OUTS);
	KT_ASSERT(n >= 3);

	/* the chosen one taken out: on to the other; put back: on to it again */
	tm_printf((UB *)QMP_AUD1_DEL);
	KT_ASSERT(wait_usb_out(".1", FALSE, 8000));
	KT_ASSERT(wait_now(id2, 2000));
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_REPPLAY);
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, (CONST char *)id1), 0);	/* still chosen */
	tm_printf((UB *)QMP_AUD1_ADD);
	KT_ASSERT(wait_usb_out(".1", TRUE, 8000));
	KT_ASSERT(wait_now(id1, 2000));
	c0 = attr_r(snd_dd, DN_SDPCMCNT);
	tk_dly_tsk(200);
	KT_ASSERT(attr_r(snd_dd, DN_SDPCMCNT) != c0);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_REPPLAY);

	/* back to "auto", and the second function taken out again */
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(sel_put(SND_ID_AUTO), E_OK);
	tm_printf((UB *)QMP_AUD2_DEL);
	KT_ASSERT(wait_usb_out(".4", FALSE, 8000));
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, SND_ID_AUTO), 0);
	Kfree(pcm);
	outputs_by_records(id1);
}

/* Whether text holds key, a tab and val as a line of its own */
LOCAL BOOL has_line( CONST UB *t, INT len, CONST char *key, CONST char *val )
{
	char	line[96];
	INT	n, i;

	n = tm_sprintf((UB *)line, (CONST UB *)"%s\t%s\n", key, val);
	for ( i = 0; i + n <= len; i++ ) {
		INT	k;

		if ( i > 0 && t[i - 1] != '\n' ) {
			continue;
		}
		for ( k = 0; k < n && t[i + k] == (UB)line[k]; k++ ) {
			;
		}
		if ( k == n ) {
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * The same through the system object's record of sound (written SELECT
 * and an id, that one is chosen) and the machine's settings' SOUND_OUT,
 * which is what the start takes.
 */
LOCAL void outputs_by_records( CONST UB *id1 )
{
	UB		*t;
	SZ		asz = 0;
	SDOutSel	s;
	ID		k;
	char		was[CF_VAL_MAX];
	BOOL		had;
	CONST char	*w;

	t = (UB *)Kmalloc(4096);
	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	k = ob_opn_obj(&ob_uuid_system, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_rea_rec(k, OB_SYS_SOUND, 0, t, 4095, &asz), E_OK);
		KT_ASSERT(has_line(t, (INT)asz, "SELECT", SND_ID_AUTO));
		KT_ASSERT(has_line(t, (INT)asz, "OUT", SND_ID_AUTO));
		KT_ASSERT(has_line(t, (INT)asz, "OUT", (CONST char *)id1));
		KT_ASSERT(has_line(t, (INT)asz, "OUT_KIND", "usb"));
		KT_ASSERT(has_line(t, (INT)asz, "OUTPUT", "none"));
		tm_sprintf(t, (CONST UB *)"SELECT %s", id1);
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, t, (SZ)knl_strlen((CONST char *)t), &asz), E_OK);
		sel_get(&s);
		KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, (CONST char *)id1), 0);
		w = "SELECT nowhere";
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_PAR);
		w = "SELECT auto";
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_OK);
		/* the system's sounds, on the output chosen: the write says it sounds */
		w = "PLAY test";
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_OK);
		KT_ASSERT(wait_now(id1, 500));
		w = "PLAY buzzer tone";
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_OK);
		w = "PLAY fanfare";
		KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_PAR);
		tk_dly_tsk(800);
		ob_cls_obj(k);
	}
	Kfree(t);

	/* the sound device's object shows the outputs, and does not choose */
	{
		TS_UUID	dev;
		INT	cnt = 0;
		SDOut	o[KT_OUTS];

		if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &dev, 1, &cnt) >= E_OK && cnt > 0 ) {
			k = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
			KT_ASSERT(k > 0);
			if ( k > 0 ) {
				KT_ASSERT_ER(ob_rea_rec(k, OB_SND_OUTS, 0, o, sizeof(o), &asz), E_OK);
				KT_ASSERT(asz >= (SZ)sizeof(SDOut) * 2);
				KT_ASSERT_EQ(knl_strcmp((CONST char *)o[0].id, SND_ID_AUTO), 0);
				KT_ASSERT_ER(ob_rea_rec(k, OB_SND_OUTSEL, 0, &s, sizeof(s), &asz), E_OK);
				KT_ASSERT_EQ(asz, sizeof(s));
				KT_ASSERT_ER(ob_wri_rec(k, OB_SND_OUTSEL, 0, id1, SND_ID_MAX, &asz), E_RONLY);
				ob_cls_obj(k);
			}
		}
	}

	had = wm_dev_conf_get("SOUND_OUT", was, sizeof(was));
	if ( wm_dev_conf_put("SOUND_OUT", (CONST char *)id1) < E_OK ) KT_SKIP("no machine settings object");
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, (CONST char *)id1), 0);
	KT_ASSERT_ER(wm_dev_conf_put("SOUND_OUT", SND_ID_AUTO), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	sel_get(&s);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)s.id, SND_ID_AUTO), 0);
	KT_ASSERT_ER(wm_dev_conf_put("SOUND_OUT", had ? was : ""), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_ER(sel_put(SND_ID_AUTO), E_OK);

	/* with nowhere to sound, the system's sound says so rather than stay silent */
	tm_printf((UB *)QMP_AUD1_DEL);
	if ( wait_usb_out(".1", FALSE, 8000) ) {
		k = ob_opn_obj(&ob_uuid_system, OB_OP_R | OB_OP_WRITE);
		if ( k > 0 ) {
			w = "PLAY test";
			KT_ASSERT_ER(ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz), E_NOSPT);
			ob_cls_obj(k);
		}
		tm_printf((UB *)QMP_AUD1_ADD);
		KT_ASSERT(wait_usb_out(".1", TRUE, 8000));
	}
}
#endif /* KT_BOARD */

#if KT_QEMU_DEVS
/* A level held in both sides, stereo 16 bit, frames of it */
LOCAL void dc16( H *buf, INT frames, INT l, INT r )
{
	INT	i;

	for ( i = 0; i < frames; i++ ) {
		buf[i * 2] = (H)l;
		buf[i * 2 + 1] = (H)r;
	}
}

/* What the mixer gave lately, a mean of the last 256 frames, after ms */
LOCAL void heard( INT ms, INT *l, INT *r )
{
	static H	t[256 * 2];
	INT		i, n, sl = 0, sr = 0;

	tk_dly_tsk(ms);
	n = knl_snd_tap(t, 256);
	for ( i = 0; i < n; i++ ) {
		sl += t[i * 2];
		sr += t[i * 2 + 1];
	}
	*l = ( n > 0 ) ? sl / n : 0;
	*r = ( n > 0 ) ? sr / n : 0;
}

LOCAL ER mixset( UW what, CONST UB *prog, UW lv, UW rv, UW mute )
{
	SDMixSet	s;
	SZ		asz = 0;

	knl_memset(&s, 0, sizeof(s));
	s.what = what;
	if ( prog != NULL ) knl_memcpy(s.prog, prog, 16);
	s.lv = lv;
	s.rv = rv;
	s.mute = mute;
	return tk_swri_dev(snd_dd, DN_SDMIXSET, &s, sizeof(s), &asz);
}

LOCAL BOOL same16( CONST UB *x, CONST UB *y )
{
	INT	i;

	for ( i = 0; i < 16 && x[i] == y[i]; i++ ) {
		;
	}
	return ( i == 16 );
}

#define NEAR(a, b)	( (a) - (b) <= 8 && (b) - (a) <= 8 )

/* Whether s holds the text w */
LOCAL BOOL str_has( CONST char *s, CONST char *w )
{
	INT	i, k;

	for ( i = 0; s[i] != 0; i++ ) {
		for ( k = 0; w[k] != 0 && s[i + k] == w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/*
 * The test's two programs taken out of what the machine's settings keep,
 * as it was before any run of the test, and the settings left still.
 * Each write of the settings is applied again by their watcher a little
 * later (uconf.c); a line of either program kept by an earlier run (the
 * second is left at 100, not muted, as the test ends) would set it back
 * over what the test sets while it runs, and one at 30 and muted would
 * start the machine so.
 */
#define KT_PA_KEPT	"0000000000a1 "
#define KT_PB_KEPT	"0000000000b2 "

/*
 * Until the watcher has applied a write of the machine's settings made
 * after the console was at *pos (it says so on the console first), and
 * a little more: up to 5 s. A fixed wait is not enough when the machine
 * is busy, the watcher being a task of low priority, and its apply,
 * coming in the middle of the test, would set the mixer back.
 */
#define WATCHED_SAID	"settings: the machine's were written"

LOCAL void watcher_applied( UD pos )
{
	static UB	buf[2048];
	INT		n, k, t, m = (INT)sizeof(WATCHED_SAID) - 1, i;

	for ( t = 0; t < 5000; t += 50 ) {
		while ( ( n = tm_log_read(&pos, buf, (INT)sizeof(buf)) ) > 0 ) {
			for ( i = 0; i + m <= n; i++ ) {
				for ( k = 0; k < m && buf[i + k] == (UB)WATCHED_SAID[k]; k++ ) ;
				if ( k == m ) {
					tk_dly_tsk(300);
					return;
				}
			}
			if ( n > m ) pos -= (UD)m;	/* a line cut between two reads */
		}
		tk_dly_tsk(50);
	}
}

LOCAL void drop_kept_pa( void )
{
	char	v[CF_VAL_MAX];
	UD	pos = ~(UD)0 >> 1;
	UB	c;

	if ( wm_dev_conf_get("SOUND_APP", v, sizeof(v)) && ( str_has(v, KT_PA_KEPT) || str_has(v, KT_PB_KEPT) ) ) {
		(void)tm_log_read(&pos, &c, 1);		/* where the console is now */
		KT_ASSERT_ER(wm_dev_conf_put("SOUND_APP", ""), E_OK);
		watcher_applied(pos);			/* the watcher, which applies them again */
	}
}

/*
 * The mixer, by the samples it gives: two channels of two programs (the
 * test's own channels of "snda" and "snda0", taken as two programs)
 * playing levels held, added; each program's volume and mute, the
 * whole's left and right apart and its mute; and sums beyond 16 bits
 * bent back below 32767 rather than cut. A volume scales by its square.
 */
LOCAL void test_mixer( void )
{
	static CONST UB	pa[16] = { 0x01, 0xa0, 0xd8, 0xc4, 0xee, 0x01, 0x70, 0x01, 0x80, 0, 0, 0, 0, 0, 0, 0xa1 };
	static CONST UB	pb[16] = { 0x01, 0xa0, 0xd8, 0xc4, 0xee, 0x02, 0x70, 0x02, 0x80, 0, 0, 0, 0, 0, 0, 0xb2 };
	SDMix	*m;
	H	*a, *b;
	ID	dd2;
	SZ	asz = 0;
	INT	l, r, k, na = 0, nb = 0;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	dd2 = tk_opn_dev((UB *)"snda0", TD_UPDATE);
	a = (H *)Kmalloc(4800 * 4);
	b = (H *)Kmalloc(4800 * 4);
	m = (SDMix *)Kmalloc(SND_MIX_SIZE(KT_OUTS));
	KT_ASSERT(dd2 > 0 && a != NULL && b != NULL && m != NULL);
	if ( dd2 <= 0 || a == NULL || b == NULL || m == NULL ) return;
	drop_kept_pa();

	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(set_mode(dd2, PCMST48K, PCMFmt16, 1), E_OK);
	knl_snd_tag(0, pa);
	knl_snd_tag(1, pb);
	dc16(a, 4800, 4000, 4000);
	dc16(b, 4800, 2000, -2000);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, a, 4800 * 4, &asz), E_OK);
	KT_ASSERT_ER(tk_swri_dev(dd2, 0, b, 4800 * 4, &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REPPLAY), E_OK);
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMCTL, PCM_REPPLAY), E_OK);
	knl_snd_tag(0, pa);
	knl_snd_tag(1, pb);

	/* added */
	heard(150, &l, &r);
	tm_printf((UB *)"  4000/4000 and 2000/-2000 added: %d %d\n", l, r);
	KT_ASSERT(NEAR(l, 6000) && NEAR(r, 2000));
	/* the first program at 50: a quarter of it */
	KT_ASSERT_ER(mixset(SND_MIX_APP, pa, 50, 50, 0), E_OK);
	heard(80, &l, &r);
	tm_printf((UB *)"  the first at 50: %d %d\n", l, r);
	KT_ASSERT(NEAR(l, 3000) && NEAR(r, -1000));
	/* the second muted */
	KT_ASSERT_ER(mixset(SND_MIX_APP, pb, 100, 100, 1), E_OK);
	heard(80, &l, &r);
	KT_ASSERT(NEAR(l, 1000) && NEAR(r, 1000));
	/* the whole's left at 50, its right as it was */
	KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 50, 100, 0), E_OK);
	heard(80, &l, &r);
	tm_printf((UB *)"  the whole's left at 50: %d %d\n", l, r);
	KT_ASSERT(NEAR(l, 250) && NEAR(r, 1000));
	/* the whole muted */
	KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 50, 100, 1), E_OK);
	heard(80, &l, &r);
	KT_ASSERT(l == 0 && r == 0);

	/* what the mixer says of them */
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDMIXER, m, SND_MIX_SIZE(KT_OUTS), &asz), E_OK);
	KT_ASSERT_EQ(asz, SND_MIX_SIZE(m->napp));
	KT_ASSERT(m->lv == 50 && m->rv == 100 && m->mute == 1);
	for ( k = 0; k < (INT)m->napp; k++ ) {
		tm_printf((UB *)"  program \"%s\" pid %d: volume %d mute %d, %d channels %d playing\n",
			  m->app[k].name, m->app[k].pid, m->app[k].vol, m->app[k].mute,
			  m->app[k].channels, m->app[k].playing);
		if ( same16(m->app[k].prog, pa) && m->app[k].vol == 50 && m->app[k].mute == 0 ) na++;
		if ( same16(m->app[k].prog, pb) && m->app[k].vol == 100 && m->app[k].mute == 1 ) nb++;
	}
	KT_ASSERT(na == 1 && nb == 1);
	KT_ASSERT_ER(mixset(5, NULL, 50, 50, 0), E_PAR);
	KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 101, 50, 0), E_PAR);

	/* loud sums: bent back, never cut */
	KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 100, 100, 0), E_OK);
	KT_ASSERT_ER(mixset(SND_MIX_APP, pa, 100, 100, 0), E_OK);
	KT_ASSERT_ER(mixset(SND_MIX_APP, pb, 100, 100, 1), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	dc16(a, 4800, 26000, -30000);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, a, 4800 * 4, &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REPPLAY), E_OK);
	knl_snd_tag(0, pa);
	heard(150, &l, &r);
	tm_printf((UB *)"  26000/-30000 alone: %d %d\n", l, r);
	KT_ASSERT(l > 24576 && l < 26000 && r < -24576 && r > -30000);
	KT_ASSERT_ER(mixset(SND_MIX_APP, pb, 100, 100, 0), E_OK);	/* and the second added */
	heard(80, &l, &r);
	tm_printf((UB *)"  with 2000/-2000 added: %d %d\n", l, r);
	KT_ASSERT(l > 25000 && l < 32767 && r < -25000 && r > -32767);

	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(attr_w(dd2, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_ER(tk_cls_dev(dd2, 0), E_OK);

	/* kept in the machine's settings a second after the last change, and taken again from them */
	{
		char	was_vol[CF_VAL_MAX], v[CF_VAL_MAX];
		BOOL	had = wm_dev_conf_get("SOUND_VOL", was_vol, sizeof(was_vol));

		if ( wm_dev_conf_apply() >= E_OK ) {
			KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 80, 60, 0), E_OK);
			KT_ASSERT_ER(mixset(SND_MIX_APP, pa, 30, 30, 1), E_OK);
			tk_dly_tsk(2000);
			KT_ASSERT(wm_dev_conf_get("SOUND_VOL", v, sizeof(v)));
			tm_printf((UB *)"  kept: SOUND_VOL %s\n", v);
			KT_ASSERT_EQ(knl_strcmp(v, "80 60 0"), 0);
			KT_ASSERT(wm_dev_conf_get("SOUND_APP", v, sizeof(v)));
			tm_printf((UB *)"  kept: SOUND_APP %s\n", v);
			/* changed here, then taken again from what was kept */
			KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 100, 100, 0), E_OK);
			KT_ASSERT_ER(wm_dev_conf_put("SOUND_VOL", "80 60 0"), E_OK);
			KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
			KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDMIXER, m, SND_MIX_SIZE(KT_OUTS), &asz), E_OK);
			KT_ASSERT(m->lv == 80 && m->rv == 60);
			/* put back as it was: the settings first, then the program's volume */
			KT_ASSERT_ER(wm_dev_conf_put("SOUND_VOL", had ? was_vol : "100 100 0"), E_OK);
			KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
			KT_ASSERT_ER(mixset(SND_MIX_APP, pa, 100, 100, 0), E_OK);
		}
	}
	KT_ASSERT_ER(mixset(SND_MIX_ALL, NULL, 100, 100, 0), E_OK);
	tk_dly_tsk(2500);			/* kept a second after the last change */
	drop_kept_pa();
	{
		char	v[CF_VAL_MAX];

		tm_printf((UB *)"  kept after: SOUND_APP %s\n",
			  wm_dev_conf_get("SOUND_APP", v, sizeof(v)) ? v : "(none)");
		KT_ASSERT(!wm_dev_conf_get("SOUND_APP", v, sizeof(v))
			  || ( !str_has(v, KT_PA_KEPT) && !str_has(v, KT_PB_KEPT) ));
	}
	Kfree(a);
	Kfree(b);
	Kfree(m);
}
#endif /* KT_BOARD */

#if KT_QEMU_DEVS
/* Up to ms for the stream to be (on) or not be (!on) playing on some output */
LOCAL BOOL wait_stream( BOOL on, INT ms )
{
	SDStat	st;

	for ( ; ms > 0; ms -= 20 ) {
		get_stat(snd_dd, &st);
		if ( ( st.backend != SND_BE_NONE ) == on ) {
			return TRUE;
		}
		tk_dly_tsk(20);
	}
	return FALSE;
}

LOCAL ER sys_sound( CONST char *w )
{
	ID	k = ob_opn_obj(&ob_uuid_system, OB_OP_R | OB_OP_WRITE);
	SZ	asz = 0;
	ER	er;

	if ( k <= 0 ) return (ER)k;
	er = ob_wri_rec(k, OB_SYS_SOUND, 0, w, (SZ)knl_strlen(w), &asz);
	ob_cls_obj(k);
	return er;
}

/*
 * The stream let stop, as it starts: with nothing playing it stops (the
 * USB function's while is none); kept going ("KEEP 1", or the machine's
 * settings' SOUND_KEEP): it starts with nothing to play, goes on in
 * silence after a sound, and stops again once let ("KEEP 0").
 */
LOCAL void test_keep( void )
{
	static H	pcm[4800 * 2];
	SZ	asz = 0;
	char	was[CF_VAL_MAX];
	BOOL	had;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	tone16(pcm, 4800, 100);
	KT_ASSERT(wait_stream(FALSE, 1000));
	KT_ASSERT(!knl_snd_keeping());

	/* let stop: a sound, and after it nothing */
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, sizeof(pcm)), E_OK);
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, sizeof(pcm), &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT(wait_stream(TRUE, 500));
	KT_ASSERT(wait_stop(snd_dd, 3000));
	KT_ASSERT(wait_stream(FALSE, 500));

	/* kept going: on with nothing to play, and on after a sound */
	KT_ASSERT_ER(sys_sound("KEEP 2"), E_PAR);
	KT_ASSERT_ER(sys_sound("KEEP 1"), E_OK);
	KT_ASSERT(knl_snd_keeping());
	KT_ASSERT(wait_stream(TRUE, 1000));
	tk_dly_tsk(1500);
	KT_ASSERT(wait_stream(TRUE, 20));
	KT_ASSERT_ER(tk_swri_dev(snd_dd, 0, pcm, sizeof(pcm), &asz), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_PLAY), E_OK);
	KT_ASSERT(wait_stop(snd_dd, 3000));
	tk_dly_tsk(1000);
	KT_ASSERT(wait_stream(TRUE, 20));
	tm_printf((UB *)"  kept going: the stream on with nothing playing\n");

	/* let stop again */
	KT_ASSERT_ER(sys_sound("KEEP 0"), E_OK);
	KT_ASSERT(!knl_snd_keeping());
	KT_ASSERT(wait_stream(FALSE, 1000));

	/* from the machine's settings, as at the start */
	had = wm_dev_conf_get("SOUND_KEEP", was, sizeof(was));
	if ( wm_dev_conf_put("SOUND_KEEP", "1") >= E_OK ) {
		KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
		KT_ASSERT(knl_snd_keeping());
		KT_ASSERT(wait_stream(TRUE, 1000));
		KT_ASSERT_ER(wm_dev_conf_put("SOUND_KEEP", had ? was : "0"), E_OK);
		KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	}
	(void)knl_snd_keep(FALSE);
	KT_ASSERT(wait_stream(FALSE, 1000));
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
}
#endif /* KT_BOARD */

/* The board sound of the Raspberry Pi 5 cannot be heard here */
#if defined(X64)
/*
 * Recording through Intel HDA (QEMU's intel-hda with hda-duplex, or the
 * PC's own codec): the input path made and the stream running, the
 * frames coming at the device's pace, the input level kept
 */
LOCAL void test_hda_record( void )
{
	static UB	buf[8192];
	SDStat		s0, s1;
	SDVol		v, r;
	SZ		asz = 0;
	ER		er;
	INT		got = 0, i;

	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	KT_ASSERT_ER(set_mode(snd_dd, PCMST48K, PCMFmt16, 1), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMBUFSZ, 65536), E_OK);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	get_stat(snd_dd, &s0);
	er = attr_w(snd_dd, DN_SDPCMCTL, PCM_REC);
	if ( er == E_NOSPT ) KT_SKIP("nothing records (QEMU without -device intel-hda -device hda-duplex)");
	KT_ASSERT_ER(er, E_OK);
	get_stat(snd_dd, &s1);
	if ( s1.backend != SND_BE_HDA ) {
		(void)attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP);
		KT_SKIP("recording goes through another backend");
	}
	KT_ASSERT_EQ(s1.in_rate, 48000);
	KT_ASSERT_EQ(s1.in_channels, 2);

	v.lv = v.rv = 0x9000;
	KT_ASSERT_ER(tk_swri_dev(snd_dd, DN_SDVOLMIC, &v, sizeof(v), &asz), E_OK);
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDVOLMIC, &r, sizeof(r), &asz), E_OK);
	KT_ASSERT_EQ(r.lv, 0x9000);

	/* half a second: the frames recorded, and what is read of them */
	for ( i = 0; i < 25; i++ ) {
		tk_dly_tsk(20);
		if ( tk_srea_dev(snd_dd, 1, buf, sizeof(buf), &asz) >= E_OK ) got += (INT)asz;
	}
	get_stat(snd_dd, &s1);
	tm_printf((UB *)"  hda: %u frames recorded in 0.5 s, %d bytes read\n", s1.in_frames - s0.in_frames, got);
	KT_ASSERT(s1.in_frames - s0.in_frames >= 12000);
	KT_ASSERT(got >= 12000 * 4);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_REC);

	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_STOP), E_OK);
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCTL), PCM_STOP);
	v.lv = v.rv = 0xc000;
	(void)tk_swri_dev(snd_dd, DN_SDVOLMIC, &v, sizeof(v), &asz);
}
#endif /* X64 */

LOCAL void test_i2s( void )
{
	KT_SKIP("RP1 I2S: Raspberry Pi 5 only, not verified on hardware");
}

LOCAL void test_close( void )
{
	if ( snd_dd <= 0 ) KT_SKIP("no sound device");
	KT_ASSERT_ER(tk_cls_dev(snd_dd, 0), E_OK);
	snd_dd = 0;
}

EXPORT void ktest_snd( void )
{
	KT_RUN(test_open);
	KT_RUN_QEMU_DEVS(test_play, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_mix, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_channels, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_underrun, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_prime, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_restart, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_switch, "written for the USB audio function of the QEMU machine");
	KT_RUN(test_record);
#if defined(X64)
	KT_RUN(test_hda_record);
#endif
	KT_RUN_QEMU_DEVS(test_outputs, "written for the USB audio functions of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_mixer, "written for the USB audio function of the QEMU machine");
	KT_RUN_QEMU_DEVS(test_keep, "written for the USB audio function of the QEMU machine");
	KT_RUN(test_i2s);
	KT_RUN(test_close);
}
