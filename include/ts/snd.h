/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	snd.h
 *	The sound device "snda" (design 10.15)
 *
 *	PCM playback and recording with the data numbers and structures of
 *	the B-right/V sound device, so a program written for that device
 *	works here unchanged. Structures are little endian.
 *
 *	Data number 0 or 1:
 *	  read	 recorded bytes since the previous read (0 while playing)
 *	  write	 bytes to play, into the free part of the PCM buffer; while
 *		 stopped, number 0 starts at the top of the buffer and number
 *		 1 goes on after the previous write
 *	A read or write of size 0 answers the bytes there are (read) or the
 *	room there is (write). A negative number is an attribute below; a
 *	read of size 0 answers its size.
 *
 *	Samples: 8 bit formats one byte a sample, 16 bit little endian;
 *	stereo interleaves left then right. The device converts any rate,
 *	format and channel count below to what the hardware takes.
 *
 *	Every process has a channel of its own per unit ("snda", "snda0" ..
 *	"snda3"): its own mode, state, buffer and counts. The channels that
 *	play are mixed into the one stream the hardware plays, so several
 *	programs can sound at once. One channel at a time records.
 *
 *	The hardware behind it is the first USB audio function plugged in,
 *	or, on the Raspberry Pi 5, the I2S interface of the header pins.
 */

#ifndef __TS_SND_H__
#define __TS_SND_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SND_DEVNM		"snda"

/*
 * The kind of device a sound device registers as (T_DDEV.devatr). The
 * T-Kernel kinds have none for sound; this one is TessronOS's, above the
 * ones T-Kernel names, and is what makes the device manager of the
 * object layer see it as a sound device (design 18.7).
 */
#define TDK_SOUND		0x0080U
#define SND_UNITS		4		/* subunits: snda0 .. snda3 */

/* Attribute data numbers */
#define DN_SDPCMINFO		(-110)	/* R : SDPcmInfo, what each rate supports */
#define DN_SDPCMMODE		(-111)	/* RW: SDPcmMode */
#define DN_SDPCMCTL		(-112)	/* RW: W, SDPcmCtl */
#define DN_SDPCMCNT		(-113)	/* R : W, bytes processed since the start */
#define DN_SDPCMENDCNT		(-114)	/* RW: W, stop after this many bytes */
#define DN_SDPCMBUFSZ		(-115)	/* RW: W, PCM buffer size in bytes */
#define DN_SDSELIN		(-120)	/* RW: SDSel, input selector */
#define DN_SDSELOUT		(-121)	/* RW: SDSel, output selector */
#define DN_SDGAININ		(-122)	/* RW: SDVol */
#define DN_SDGAINOUT		(-123)
#define DN_SDVOLMAIN		(-124)
#define DN_SDVOLTREBLE		(-125)
#define DN_SDVOLBASS		(-126)
#define DN_SDVOLPCM		(-127)
#define DN_SDVOLBEEP		(-128)
#define DN_SDVOLMIC		(-129)
#define DN_SDVOLLINE		(-130)
#define DN_SDVOLCD		(-131)
#define DN_SDVOLMUSIC		(-132)
#define DN_SDVOLAUX1		(-133)
#define DN_SDVOLAUX2		(-134)
/* TessronOS: SDStat, what the device and the hardware under it have done */
#define DN_SDSTAT		(-140)

/* DN_SDPCMINFO: one entry per sampling rate code */
typedef struct {
	UW	datafmt:16;	/* bit (1 << PcmDataFmt): the format is taken */
	UW	mplay:1;	/* mono playback */
	UW	mrecord:1;	/* mono recording */
	UW	splay:1;	/* stereo playback */
	UW	srecord:1;	/* stereo recording */
	UW	rsv:12;
} SDPcmSpt;

typedef struct {
	SDPcmSpt spt[16];
} SDPcmInfo;

/* DN_SDPCMMODE */
typedef struct {
	UW	sampling:4;	/* PcmSampling */
	UW	datafmt:4;	/* PcmDataFmt */
	UW	stereo:1;
	UW	rsv:23;
} SDPcmMode;

/* PcmDataFmt */
#define PCMFmt8ulaw		0	/* 8 bit mu-law */
#define PCMFmt8			1	/* 8 bit linear, unsigned */
#define PCMFmt16		2	/* 16 bit linear, signed */

/* PcmSampling */
#define PCMST8K			0	/*  8.0   kHz */
#define PCMST11K		1	/* 11.025 kHz */
#define PCMST16K		2	/* 16.0   kHz */
#define PCMST22K		3	/* 22.05  kHz */
#define PCMST44K		4	/* 44.1   kHz */
#define PCMST48K		5	/* 48.0   kHz */

/* DN_SDPCMCTL */
#define PCM_STOP		0
#define PCM_PLAY		1
#define PCM_REC			2
#define PCM_REPPLAY		3	/* play the buffer over and over */

/* The stop count that means none: the byte count runs on and wraps */
#define PCM_COUNT_NONE		0x3fffffff

/*
 * DN_SDSELIN / DN_SDSELOUT: what is connected, and 16 bits up, whether
 * that can be changed
 */
typedef struct {
	UW	pcm:1;
	UW	beep:1;
	UW	mic:1;
	UW	line:1;
	UW	cd:1;
	UW	music:1;
	UW	aux1:1;
	UW	aux2:1;
	UW	rsv1:8;
	UW	m_pcm:1;
	UW	m_beep:1;
	UW	m_mic:1;
	UW	m_line:1;
	UW	m_cd:1;
	UW	m_music:1;
	UW	m_aux1:1;
	UW	m_aux2:1;
	UW	rsv2:8;
} SDSel;

/* Gains and volumes: 0 the least, 0x8000 the middle, 0xFFFF the most */
typedef struct {
	UH	lv;
	UH	rv;
} SDVol;

/* DN_SDSTAT */
typedef struct {
	UW	backend;	/* SND_BE_*: what plays now, 0 nothing */
	UW	out_rate;	/* the rate the hardware plays at, 0 stopped */
	UW	out_channels;
	UW	in_rate;	/* and records at */
	UW	in_channels;
	UW	out_frames;	/* frames given to the hardware */
	UW	out_packets;	/* pieces of output it asked for */
	UW	out_short;	/* pieces made while no channel had samples */
	UW	in_frames;	/* frames recorded */
	UW	channels;	/* channels in use */
	UW	playing;	/* channels that play */
	UW	underruns;	/* channels stopped for want of samples */
} SDStat;

#define SND_BE_NONE		0
#define SND_BE_USB		1	/* a USB audio function */
#define SND_BE_I2S		2	/* the RP1's I2S (Raspberry Pi 5) */

/*
 * Register "snda" (design 10.15). The hardware may come and go later: a
 * USB audio function is looked for when a channel starts.
 */
IMPORT ER   knl_snd_init( void );

/* Called by the USB manager's task every few milliseconds while active */
IMPORT void knl_snd_poll( void );
IMPORT BOOL knl_snd_active( void );

/*
 * For a test: feed recorded frames as if the hardware had delivered
 * them (16 bit, channels per frame, at the rate the recording started).
 */
IMPORT void knl_snd_capture( CONST UB *src, INT frames, INT channels );

/*
 * For a test: recording takes what knl_snd_capture() is handed, as if it
 * came from hardware at 48kHz in stereo, instead of from a device.
 */
IMPORT void knl_snd_loopback( BOOL on );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SND_H__ */
