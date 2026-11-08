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
 *	  write	 bytes to play, into the free part of the PCM buffer; the
 *		 first write after a stop starts a new stream, at the top of
 *		 the buffer when it is number 0 (what the last stream left
 *		 unplayed is dropped); every other write goes on after the
 *		 previous one
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
 *	The hardware behind it is the output chosen (DN_SDOUTSEL), or with
 *	"auto" chosen, the first USB audio function plugged in, else, on the
 *	Raspberry Pi 5, the I2S interface of the header pins. The Raspberry
 *	Pi 5 has its two HDMI sockets as outputs too, taken only when chosen.
 *	An output chosen that is not there (a USB device taken out) leaves
 *	the sound on what "auto" would take, and it goes back to the chosen
 *	one when that comes again; a change of output, chosen or forced,
 *	moves the stream without stopping a channel.
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
/* TessronOS: R : SDOut[], "auto" and every output there is now */
#define DN_SDOUTS		(-141)
/* TessronOS: RW: SDOutSel, the output chosen; written, it is taken at once */
#define DN_SDOUTSEL		(-142)

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
#define SND_BE_HDMI		3	/* an HDMI socket (Raspberry Pi 5) */
#define SND_BE_HDA		4	/* Intel High Definition Audio (the x64 PC) */
#define SND_BE_BT		5	/* earphones over Bluetooth (A2DP) */

/*
 * The outputs (DN_SDOUTS, DN_SDOUTSEL). An output is named by an id
 * that stays the same from one start to the next:
 *	auto		  what is there, in the order above
 *	i2s		  the I2S interface on GPIO 18-21 (a DAC board)
 *	hdmi0, hdmi1	  the HDMI sockets
 *	usb:VVVV:PPPP@P	  a USB audio function: its vendor and product ids
 *			  in hex and where it is plugged in (P: the
 *			  controller from 0, a '-', its root port from 1,
 *			  then the hub ports, as 0-2.4)
 * A USB function chosen and plugged into another socket is still the
 * one chosen: with no function at the port named, the first of the same
 * ids is taken.
 *
 * DN_SDOUTS answers "auto" and every output there is, an SDOut each, and
 * as many as there are: a read of size 0 is told the size, and a read
 * with less room has the first ones.
 */
/*
 * The structures this system adds (SDOut, SDOutSel, SDMix ...) are made
 * of INT and UINT, which are 32 bits in a program and in the kernel
 * alike; W and UW are not, in a program built without config.h first.
 */
#define SND_ID_MAX		32
#define SND_NAME_MAX		64
#define SND_ID_AUTO		"auto"

typedef struct {
	UB	id[SND_ID_MAX];		/* as above, NUL ended */
	UB	name[SND_NAME_MAX];	/* UTF-8: what is attached -- a USB function's
					   product, a screen's name -- "" when not known */
	UINT	kind;			/* SND_BE_*; SND_BE_NONE for "auto" */
	UINT	flags;			/* SND_OF_* */
	UINT	rate;			/* while it plays: the rate and channels */
	UINT	channels;
} SDOut;

/*
 * Why the machine's own sound may not be heard (the x64 PC; the system
 * object's sound record, HW_NOTE): what is wired through the audio DSP has no
 * driver here, and the HD Audio registers do not reach it (design 6.5.8)
 */
#define SND_NOTE_SPK_DSP	0x0001	/* the speakers are on SoundWire or I2S behind the DSP */
#define SND_NOTE_MIC_DSP	0x0002	/* the built-in microphones are digital ones behind the DSP */
#define SND_NOTE_NOCODEC	0x0004	/* the controller is there and no codec answers on its link */
#define SND_NOTE_DISPLAY	0x0008	/* only the display's codec answers: sound goes to a screen alone */
#define SND_NOTE_SOUNDWIRE	0x0010	/* with SPK_DSP: on SoundWire */
#define SND_NOTE_I2S		0x0020	/* with SPK_DSP: on I2S */

#define SND_OF_CHOSEN		0x0001	/* the one chosen */
#define SND_OF_PLAYING		0x0002	/* the one the stream plays on now */
#define SND_OF_NOSOUND		0x0004	/* there, but cannot sound now (a screen that
					   takes no sound, nothing plugged in) */
#define SND_OF_LEVEL		0x0008	/* has a volume (DN_SDVOLMAIN) */

typedef struct {
	UB	id[SND_ID_MAX];		/* written: what to choose; read: what was chosen */
	UB	now[SND_ID_MAX];	/* read: what plays now, "" when nothing does */
} SDOutSel;

/*
 * The mixer (DN_SDMIXER, DN_SDMIXSET). Each program's channels are
 * scaled by the program's volume before they are added together, and
 * the sum by the whole's volume, left and right apart; a sum beyond
 * what 16 bits hold is bent softly back rather than cut. A volume is 0
 * to SND_VOL_MAX and scales the samples by its square (50 by a quarter,
 * 12dB down), which the ear takes as even steps; a mute silences
 * without losing the volume.
 *
 * A program is its program object: the volume set for it holds for
 * every process started from it, now and from the next start on (the
 * machine's settings' lines SOUND_VOL and SOUND_APP, uconf.c). A
 * process started from a file has its own, for as long as it runs; the
 * system's own sounds (kernel tasks: the buzzer and the clicks) are one
 * more, pid 0.
 */
#define DN_SDMIXER		(-143)	/* R : SDMix, the whole and the programs that use the device */
#define DN_SDMIXSET		(-144)	/* W : SDMixSet, a volume and mute set */
/* TessronOS: RW: SDMon, what a channel's recording takes from what is played */
#define DN_SDMONITOR		(-145)

#define SND_VOL_MAX		100

typedef struct {
	UB	prog[16];		/* the program object's UUID; zeros: none */
	INT	pid;			/* a process of it, 0 the system's own */
	UB	name[SND_NAME_MAX];	/* UTF-8, for a person */
	UINT	vol;			/* 0 .. SND_VOL_MAX */
	UINT	mute;
	UINT	channels;		/* channels it has open */
	UINT	playing;		/* of them, playing */
	UINT	level;			/* the loudest it sounded of late, 0 .. 32767 */
} SDMixApp;

typedef struct {
	UINT	lv, rv;			/* the whole, left and right: 0 .. SND_VOL_MAX */
	UINT	mute;
	UINT	level_l, level_r;	/* the loudest it sounded of late, 0 .. 32767 */
	UINT	napp;
	SDMixApp app[];			/* napp of them */
} SDMix;

/*
 * DN_SDMIXER answers SND_MIX_SIZE(napp) bytes, every program that has
 * channels now: a read of size 0 is told the size, and a read with less
 * room has the whole and the first programs.
 */
#define SND_MIX_SIZE(napp)	( sizeof(SDMix) + sizeof(SDMixApp) * (SZ)(napp) )

#define SND_MIX_ALL		0	/* SDMixSet.what: the whole, lv and rv */
#define SND_MIX_APP		1	/* a program, lv */

typedef struct {
	UINT	what;			/* SND_MIX_* */
	UB	prog[16];		/* SND_MIX_APP: the program object, or zeros ... */
	INT	pid;			/* ... and then the process (0 the system's own) */
	UINT	lv, rv;
	UINT	mute;
} SDMixSet;

/*
 * The monitor (DN_SDMONITOR): a channel set to it, while stopped, records
 * what the programs play instead of an input, as long as PCM_REC runs.
 * The samples are taken after each program's volume and before the whole's
 * volume, so that turning the machine's sound down or muting it changes
 * what is heard here and not what is taken. Any number of channels can
 * monitor at once, beside the one that records an input, and a monitor
 * neither keeps the stream going nor starts it: while nothing is played on
 * an output, it has nothing to read. A reader too slow for it loses the
 * newest frames; the recording does not stop for that.
 */
#define SND_MON_OFF		0	/* an input, as usual */
#define SND_MON_ALL		1	/* every program */
#define SND_MON_EXCEPT		2	/* every program but the process pid */
#define SND_MON_ONLY		3	/* the process pid alone */

typedef struct {
	UINT	mode;			/* SND_MON_* */
	INT	pid;
} SDMon;

/*
 * The mixer from inside the kernel: set (keep: and kept for the next
 * start, as a write of DN_SDMIXSET is), and read without the programs'
 * names.
 */
IMPORT ER   knl_snd_mix_set( CONST SDMixSet *s, BOOL keep );
IMPORT INT  knl_snd_mix_get( SDMix *m, INT max );	/* room for max programs; how many there are */
/* The settings of the programs that have an object, as the keeper keeps them: up to max into s, and how many there are */
IMPORT INT  knl_snd_mix_kept( SDMixSet *s, INT max );
/* What keeps the mixer's settings: called a second after they last changed */
IMPORT void knl_snd_mix_keeper( void (*fn)( void ) );
#ifdef USE_KTEST
/* For a test: the last frames the mixer gave the hardware, stereo, the newest last */
IMPORT INT  knl_snd_tap( H *dst, INT frames );
/* For a test: the caller's channel of a unit taken as a channel of the program prog */
IMPORT void knl_snd_tag( INT unit, CONST UB *prog );
#endif

/*
 * Register "snda" (design 10.15). The hardware may come and go later: a
 * USB audio function is looked for when a channel starts.
 */
IMPORT ER   knl_snd_init( void );

/* Called by the USB manager's task every few milliseconds while active */
IMPORT void knl_snd_poll( void );
IMPORT BOOL knl_snd_active( void );

/*
 * Called while a task waits for a USB transfer with the manager's lock
 * held: a stream not on USB (the I2S) is served meanwhile
 */
IMPORT void knl_snd_service( void );

/*
 * Recorded frames as the hardware delivered them (16 bit, channels per
 * frame, at the rate the recording started), with the USB manager's
 * lock held (the drivers call it from their streams, which have it).
 */
IMPORT void knl_snd_capture( CONST UB *src, INT frames, INT channels );

/*
 * For a test: the same, from a task that does not hold the manager's
 * lock (it is taken here), so that the recording cannot end while the
 * frames go in.
 */
IMPORT void knl_snd_feed( CONST UB *src, INT frames, INT channels );

/*
 * For a test: recording takes what knl_snd_capture() is handed, as if it
 * came from hardware at 48kHz in stereo, instead of from a device.
 */
IMPORT void knl_snd_loopback( BOOL on );

/*
 * The output chosen, by its id (SND_ID_AUTO for the order above):
 * taken at once, the stream moved when it plays. The machine's settings
 * line SOUND_OUT holds it from one start to the next (uconf.c).
 * E_PAR for an id of no known form.
 */
IMPORT ER   knl_snd_select( CONST UB *id );

/*
 * The stream kept going (on) on the output the choice gives, silence
 * sent while nothing plays; it stops and starts again only when the
 * output changes or goes. Off (as it starts): the stream stops when
 * nothing has played for the output's while (sndhw.h, tail_ms). The
 * machine's settings line SOUND_KEEP holds it (uconf.c); the system
 * object's record 8 shows it as KEEP and takes "KEEP 1" or "KEEP 0".
 */
IMPORT ER   knl_snd_keep( BOOL on );
IMPORT BOOL knl_snd_keeping( void );
/* SND_NOTE_* as found at start-up; 0 on the machines where it does not arise */
IMPORT UW   knl_snd_note( void );

/*
 * The outputs as DN_SDOUTS has them: up to max of them into o, and how
 * many there are. When that is more than max, ask again with more room;
 * o NULL and max 0 asks only how many.
 */
IMPORT INT  knl_snd_outputs( SDOut *o, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SND_H__ */
