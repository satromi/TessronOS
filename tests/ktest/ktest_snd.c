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
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/snd.h>
#include <ts/usb.h>

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

#ifndef RPI5
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
#endif /* RPI5 */

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

	snd_dd = tk_opn_dev((UB *)SND_DEVNM, TD_UPDATE);
	if ( snd_dd <= 0 ) KT_SKIP("no sound device");

	/* a read of size 0 says how large the data is */
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDPCMINFO, &in, 0, &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(SDPcmInfo));
	KT_ASSERT_ER(tk_srea_dev(snd_dd, DN_SDPCMINFO, &in, sizeof(in), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(SDPcmInfo));
#ifndef RPI5
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
#ifndef RPI5
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
	/* nothing records on this machine */
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMCTL, PCM_REC), E_NOSPT);
}

#ifndef RPI5
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
	UB	ulaw[8000];
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

/* A channel that runs out of samples stops by itself: an underrun */
LOCAL void test_underrun( void )
{
	H	pcm[4800 * 2];
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
#endif /* RPI5 */

/*
 * Recording, from the device's test source: 48kHz stereo in, 16kHz mono
 * 16 bit out. A second of input is a third as many frames, each the
 * mean of left and right.
 */
LOCAL void test_record( void )
{
	UB	*in, out[4096];
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
		knl_snd_capture(in, 4800, 2);		/* a second, in tenths */
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
	knl_snd_capture(in, 4800, 2);
	KT_ASSERT(wait_stop(snd_dd, 1000));
	KT_ASSERT_EQ(attr_r(snd_dd, DN_SDPCMCNT), 1000);
	KT_ASSERT_ER(tk_srea_dev(snd_dd, 0, out, sizeof(out), &asz), E_OK);
	KT_ASSERT_EQ(asz, 1000);
	KT_ASSERT(out[0] == out[2] && out[0] != out[1]);	/* left and right apart */

	knl_snd_loopback(FALSE);
	KT_ASSERT_ER(attr_w(snd_dd, DN_SDPCMENDCNT, 0), E_OK);
	Kfree(in);
}

/* The board sound of the Raspberry Pi 5 cannot be heard here */
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
	KT_RUN_EXCEPT_RPI5(test_play, "written for the USB audio function of the QEMU machine");
	KT_RUN_EXCEPT_RPI5(test_mix, "written for the USB audio function of the QEMU machine");
	KT_RUN_EXCEPT_RPI5(test_underrun, "written for the USB audio function of the QEMU machine");
	KT_RUN(test_record);
	KT_RUN(test_i2s);
	KT_RUN(test_close);
}
