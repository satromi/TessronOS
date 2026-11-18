/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	snd_hdmi.c
 *	Sound through the HDMI sockets of the Raspberry Pi 5 (design 10.15).
 *
 *	The firmware brings the picture up on HDMI0, HDMI1 or both and goes
 *	on driving it (disp_rpi5.c); nothing of the picture is touched
 *	here. What HDMI sound needs besides the picture is in the same
 *	blocks of the BCM2712 and is this file's alone:
 *
 *	  the MAI		  the audio input of each HDMI controller, in
 *				  the "hd" block shared by the two: a FIFO of
 *				  IEC 60958 subframes, the sample clock made
 *				  from the 108MHz clock of the DVP block, and
 *				  the format it is told
 *	  the HDMI core		  the audio packets: which channels, the clock
 *				  regeneration numbers N and CTS, and the AUDIO
 *				  InfoFrame in the packet RAM
 *	  a channel of the	  the 40 bit DMA ("dma40", channels 6 to 10 are
 *	  DMA			  the ARM's): a ring of control blocks walking
 *				  the samples into the MAI's FIFO, paced by the
 *				  MAI's request line (DREQ 10 for HDMI0, 17 for
 *				  HDMI1)
 *
 *	The samples go out as IEC 60958 subframes, two 32 bit words a frame:
 *	the preamble in bits 0-3 (the block start's 8 on the first left
 *	subframe of every 192 frames, else 2 for left and 4 for right), the
 *	16 bit sample at the top of the 24 bit audio field (bits 12-27), the
 *	channel status bit in bit 30 and even parity over bits 4-30 in bit
 *	31. The channel status says consumer PCM, no copyright, 48kHz, 16
 *	bits.
 *
 *	Which sockets are there is asked of the firmware once, at the start:
 *	the displays it drives and their ids (2 HDMI0, 7 HDMI1), the screen's
 *	EDID (its name, and whether its CEA extension says it takes sound)
 *	and the pixel clock, which the CTS is made from. A socket the
 *	firmware does not drive is not touched at all: its registers may
 *	have no clock; a firmware that does not say which it drives has no
 *	HDMI sound offered. A screen the HDMI controller sends DVI to, or whose
 *	EDID has no sound, or a socket whose hot plug line says nothing is
 *	in it, is an output that cannot sound now (SND_OF_NOSOUND).
 *
 *	The ring is 16 blocks of 256 frames, as the I2S's; the DMA never
 *	stops by itself, so the look (every few milliseconds, from the USB
 *	manager's task) refills the blocks behind the one the DMA is on. A
 *	look that comes later than three quarters of the ring takes the
 *	ring as played through and refills all of it but the block the DMA
 *	is on, which is heard as a repeat of old samples rather than a stop.
 *
 *	Not yet verified on hardware.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/usb.h>
#include "sysdepend.h"
#include "sndhw.h"
#include "../usb/usbdev.h"

#ifndef CNF_SND_HDMI
#define CNF_SND_HDMI		1
#endif

/* ---------------------------------------------------------------- blocks */

#define HD_PA			0x107c720000ULL		/* the MAIs of both controllers */
#define DVP_PA			0x107c700000ULL
#define HVS_PA			0x107c580000ULL
#define DMA_PA			0x1000010000ULL		/* channel n at + n * 0x100 */

LOCAL CONST UD	core_pa[2] = { 0x107c701400ULL, 0x107c706400ULL };
LOCAL CONST UD	ram_pa[2]  = { 0x107c703800ULL, 0x107c708800ULL };
LOCAL CONST UW	mai_off[2] = { 0x10, 0x30 };	/* MAI_CTL of each in the hd block */
LOCAL CONST UW	dreq[2]    = { 10, 17 };
LOCAL CONST UW	disp_id[2] = { 2, 7 };		/* the firmware's numbers of the displays */

/* the MAI, from its MAI_CTL */
#define MAI_CTL			0x00
#define MAI_THR			0x04
#define MAI_FMT			0x08
#define MAI_DATA		0x0c
#define MAI_SMP			0x10

#define MAI_CTL_RESET		0x00000001
#define MAI_CTL_ERRORF		0x00000002
#define MAI_CTL_ERRORE		0x00000004
#define MAI_CTL_ENABLE		0x00000008
#define MAI_CTL_CHNUM(n)	((UW)(n) << 4)
#define MAI_CTL_FLUSH		0x00000200
#define MAI_CTL_EMPTY		0x00000400
#define MAI_CTL_FULL		0x00000800
#define MAI_CTL_WHOLSMP		0x00001000
#define MAI_CTL_CHALIGN		0x00002000
#define MAI_CTL_DLATE		0x00008000

#define MAI_FMT_48000		(9U << 8)
#define MAI_FMT_PCM		(2U << 16)

/* the HDMI core */
#define HDMI_MAI_CHANNEL_MAP	0x0a4
#define HDMI_MAI_CONFIG		0x0a8
#define HDMI_AUDIO_PACKET	0x0c0
#define HDMI_RAM_PACKET_CONFIG	0x0c4
#define HDMI_RAM_PACKET_STATUS	0x0cc
#define HDMI_CRP_CFG		0x0d0
#define HDMI_CTS_0		0x0d4
#define HDMI_CTS_1		0x0d8
#define HDMI_SCHEDULER_CONTROL	0x0e8
#define HDMI_HOTPLUG		0x1c8

#define MAI_CONFIG_BIT_REVERSE	0x04000000
#define MAI_CONFIG_FORMAT_REVERSE 0x08000000
#define AUDIO_ZERO_ON_FLAT	0x20000000
#define AUDIO_ZERO_ON_INACTIVE	0x01000000
#define AUDIO_B_FRAME_ID(n)	((UW)(n) << 10)
#define RAM_PACKET_ENABLE	0x00010000
#define CRP_EXTERNAL_CTS	0x01000000
#define SCHED_MODE_HDMI		0x00000001
#define HOTPLUG_CONNECTED	0x00000001

#define PACKET_STRIDE		0x24
#define PACKET_AUDIO		4		/* the AUDIO InfoFrame, type 0x84 */

/* the DVP block: its resets, and the 108MHz clocks of the two */
#define DVP_SW_INIT		0x04
#define DVP_MISC_CONFIG		0x08
LOCAL CONST UW	dvp_gate[2]  = { 0x08, 0x10 };	/* set: the clock is off */
#define AUDIO_CLOCK		108000000

/* the scaler's version: D0 parts lay MAI_THR out otherwise */
#define HVS_VERSION_D0		0x54

/* the DMA channel, 40 bit */
#define DMA_CH			6
#define CH_CS			0x00
#define CH_CB			0x04
#define CH_DEBUG		0x0c
#define CH_TI			0x10
#define CH_SRC			0x14

#define CS_ACTIVE		0x00000001
#define CS_PROT			0x00000300
#define CS_QOS(n)		((UW)(n) << 16)
#define CS_PANIC_QOS(n)		((UW)(n) << 20)
#define CS_TRANSACTIONS		0x02000000
#define DEBUG_RESET		0x00800000

#define TI_WAIT_RESP		0x00000004
#define TI_PER_MAP(n)		((UW)(n) << 9)
#define TI_D_DREQ		0x00008000
#define XI_BURST_LEN(n)		((UW)(n) << 8)
#define XI_INC			0x00001000
#define XI_SIZE_128		0x00004000

/* ---------------------------------------------------------------- the ring */

#define NBLK			16
#define BLK_FRAMES		256
#define BLK_BYTES		(BLK_FRAMES * 8)	/* two subframes a frame */
#define OUT_RATE		48000
#define CB_SIZE			32
#define RING_MS			(NBLK * BLK_FRAMES * 1000 / OUT_RATE)
#define STUCK_MS		100
#define REPORT_MS		60000
#define RATE_MS			5000
#define RATE_EARLY_MS		30000

/*
 * A stream starts with its first samples at once, and goes on in
 * silence this long after its last channel stops, so that a sound soon
 * after finds the screen still listening; after that it stops, and the
 * next sound opens it again at once. A screen or television may let
 * the sound through only some hundreds of milliseconds after the audio
 * packets begin, so a short sound then can be lost: the sound device
 * can be told to keep the stream going for good (knl_snd_keep).
 */
#define HDMI_LEAD_MS		0
#define HDMI_TAIL_MS		5000

/*
 * While it plays, the socket is looked at this often: one unplugged,
 * sent DVI, or whose timing changed (another mode) ends the stream, so
 * that the sound device moves it and starts it again with the timing
 * as it is.
 */
#define CHECK_MS		1000
#define HDMI_HORZA		0x0ec
#define HDMI_VERTA0		0x0f4
#define HDMI_VERTB0		0x0f8

/* what the firmware says of the two sockets */
typedef struct {
	BOOL	driven;			/* the firmware shows a picture on it */
	BOOL	edid;			/* its EDID could be read */
	BOOL	sound;			/* the EDID says the screen takes sound */
	UW	clock_khz;		/* the pixel clock, 0 not known */
	UB	name[16];		/* the screen's name */
} HDMIOUT;

LOCAL HDMIOUT	out[2];
LOCAL BOOL	hdmi_ok = FALSE;	/* the ring is there */
LOCAL BOOL	d0 = FALSE;
LOCAL INT	cur = -1;		/* the socket playing, -1 none */
LOCAL DMABUF	ring;
LOCAL UW	*cbs;			/* the control blocks, a page */
LOCAL UD	cbs_pa;
LOCAL INT	last_k, seen_k;
LOCAL UW	moved_ms, look_ms, start_ms, rate_ms, report_ms;
LOCAL UW	nblocks, rate_blocks, pace_blocks, nlate, nstuck;
LOCAL UW	check_ms;		/* the socket last looked at */
LOCAL UW	timing[3];		/* its timing when the stream started */
LOCAL BOOL	lost = FALSE;		/* the screen changed: the stream is to end */
LOCAL INT	iec_frame;		/* the frame's place in the channel status block */

LOCAL CONST UB	chan_status[5] = { 0x04, 0x82, 0x00, 0x02, 0x02 };

LOCAL UW rd( UD pa )
{
	return *(volatile UW *)DEV_BASE(pa);
}

LOCAL void wr( UD pa, UW v )
{
	*(volatile UW *)DEV_BASE(pa) = v;
	DeviceBarrier();
}

/* a block's place, through its page, and its physical address */
LOCAL UW *blk_addr( INT i )
{
	return (UW *)(ring.virt[i / 2] + (i % 2) * BLK_BYTES);
}

LOCAL UD blk_pa( INT i )
{
	return ring.bus[i / 2] - RP1_DMA_BUS_OFFSET + (UD)(i % 2) * BLK_BYTES;
}

/* ---------------------------------------------------------------- the firmware */

#define TAG_GET_EDID_DISPLAY	0x00030023U
#define TAG_GET_NUM_DISPLAYS	0x00040013U
#define TAG_GET_DISPLAY_ID	0x00040016U
#define TAG_GET_DISPLAY_TIMING	0x00040017U
#define TAG_ANSWERED		0x80000000U

LOCAL UW	fwm[64] __attribute__((aligned(64)));

/*
 * One tag of nval words of room, the first nask of them asked with v
 * (copied in): the answer's words at fwm + 5, or NULL when it was not
 * answered.
 */
LOCAL UW *fw_ask( UW tag, INT nval, CONST UW *v, INT nask )
{
	INT	i;

	if ( nval + 6 > (INT)(sizeof(fwm) / sizeof(fwm[0])) ) {
		return NULL;
	}
	knl_memset(fwm, 0, sizeof(fwm));
	fwm[0] = (UW)(nval + 6) * 4;
	fwm[1] = 0;
	fwm[2] = tag;
	fwm[3] = (UW)nval * 4;
	fwm[4] = (UW)nask * 4;
	for ( i = 0; i < nask; i++ ) {
		fwm[5 + i] = v[i];
	}
	fwm[5 + nval] = 0;			/* the end tag */
	if ( knl_mbox_property(fwm) < E_OK || (fwm[4] & TAG_ANSWERED) == 0 ) {
		return NULL;
	}

	return fwm + 5;
}

/*
 * Block blk of display id's EDID into e (128 bytes): asked with the
 * block and the display, answered with the block, a word and the
 * bytes. Taken only when its bytes add up to 0, as every EDID block's
 * do, and the first block has its header.
 */
LOCAL BOOL edid_block( UW id, UW blk, UB *e )
{
	UW	v[2];
	UW	*a;
	INT	i, sum = 0;

	v[0] = blk;
	v[1] = id;
	a = fw_ask(TAG_GET_EDID_DISPLAY, 2 + 32, v, 2);
	if ( a == NULL ) {
		return FALSE;
	}
	knl_memcpy(e, a + 2, 128);
	for ( i = 0; i < 128; i++ ) {
		sum += e[i];
	}
	if ( (sum & 0xff) != 0 ) {
		return FALSE;
	}

	return ( blk != 0 || ( e[0] == 0x00 && e[1] == 0xff && e[7] == 0x00 ) );
}

/*
 * What the EDID says: the screen's name (the descriptor 0xFC), and
 * whether a CEA extension says it takes basic audio or has an audio
 * data block. A screen with no CEA extension is a DVI one: no sound.
 */
LOCAL void edid_read( INT h )
{
	UB	e[128];
	INT	i, k, n, next;

	out[h].edid = edid_block(disp_id[h], 0, e);
	if ( !out[h].edid ) {
		return;
	}
	for ( i = 0x36; i <= 0x6c; i += 18 ) {
		if ( e[i] == 0 && e[i + 1] == 0 && e[i + 3] == 0xfc ) {
			for ( k = 0; k < 13 && k < (INT)sizeof(out[h].name) - 1 && e[i + 5 + k] != 0x0a; k++ ) {
				out[h].name[k] = e[i + 5 + k];
			}
			while ( k > 0 && out[h].name[k - 1] == ' ' ) {
				k--;
			}
			out[h].name[k] = 0;
		}
	}
	n = e[0x7e];
	for ( k = 1; k <= n && k <= 3; k++ ) {
		if ( !edid_block(disp_id[h], (UW)k, e) || e[0] != 0x02 ) {
			continue;
		}
		if ( e[1] >= 2 && (e[3] & 0x40) != 0 ) {
			out[h].sound = TRUE;		/* basic audio */
		}
		for ( i = 4; i < e[2] && i < 127; i = next ) {
			next = i + 1 + (e[i] & 0x1f);
			if ( (e[i] >> 5) == 1 && (e[i] & 0x1f) >= 3 ) {
				out[h].sound = TRUE;	/* an audio data block */
			}
		}
	}
}

/* The pixel clock of display id, in kHz; 0 when the firmware does not say */
LOCAL UW timing_khz( UW id )
{
	UW	v[1];
	UW	*a;

	v[0] = id & 0xff;			/* the display, in the first byte */
	a = fw_ask(TAG_GET_DISPLAY_TIMING, 9, v, 1);

	return ( a != NULL ) ? a[1] : 0;
}

/* Which sockets the firmware drives */
LOCAL void displays_find( void )
{
	UW	v[1];
	UW	*a;
	INT	n, i, h;

	a = fw_ask(TAG_GET_NUM_DISPLAYS, 1, NULL, 0);
	n = ( a != NULL ) ? (INT)a[0] : -1;
	for ( i = 0; i < n && i < 8; i++ ) {
		v[0] = (UW)i;
		a = fw_ask(TAG_GET_DISPLAY_ID, 1, v, 1);
		if ( a == NULL ) {
			continue;
		}
		for ( h = 0; h < 2; h++ ) {
			if ( a[0] == disp_id[h] ) {
				out[h].driven = TRUE;
			}
		}
	}
	if ( n < 0 ) {
		/*
		 * No answer: which socket shows the picture is not known, and the
		 * registers of one that does not may have no clock, so neither is
		 * touched.
		 */
		tm_printf((UB *)"TessronOS: hdmi sound: the firmware does not say its displays; "
			  "no HDMI sound\n");
	}
}

/* ---------------------------------------------------------------- the samples */

/* One subframe: the sample, the channel status bit, parity and the preamble */
LOCAL UW subframe( INT s, INT right )
{
	UW	w = (UW)(UH)(H)s << 12;
	INT	f = iec_frame;

	if ( f / 8 < (INT)sizeof(chan_status) && (chan_status[f / 8] & (1 << (f % 8))) != 0 ) {
		w |= 0x40000000;
	}
	if ( __builtin_parity(w & 0x7ffffff0) ) {
		w |= 0x80000000;
	}

	return w | ( right ? 0x04 : ( f == 0 ) ? 0x08 : 0x02 );
}

/* One block of mixed samples, as subframes */
LOCAL void blk_fill( INT i )
{
	H	pcm[BLK_FRAMES * 2];
	UW	*w = blk_addr(i);
	INT	k;

	knl_snd_mix((UB *)pcm, BLK_FRAMES, OUT_RATE, 2, FALSE);
	for ( k = 0; k < BLK_FRAMES; k++ ) {
		w[k * 2] = subframe(pcm[k * 2], 0);
		w[k * 2 + 1] = subframe(pcm[k * 2 + 1], 1);
		iec_frame = ( iec_frame + 1 ) % 192;
	}
	USB_MB();
	nblocks++;
}

/* ---------------------------------------------------------------- the DMA */

#define CH_PA			(DMA_PA + (UD)DMA_CH * 0x100)

/* The channel stopped and reset, when it runs */
LOCAL void dma_halt( void )
{
	INT	i;

	if ( rd(CH_PA + CH_CB) == 0 ) {
		return;
	}
	wr(CH_PA + CH_CS, rd(CH_PA + CH_CS) & ~CS_ACTIVE);
	for ( i = 0; i < 1000 && (rd(CH_PA + CH_CS) & CS_TRANSACTIONS) != 0; i++ ) {
		;
	}
	wr(CH_PA + CH_CS, CS_PROT);
	wr(CH_PA + CH_DEBUG, rd(CH_PA + CH_DEBUG) | DEBUG_RESET);
}

/* The control blocks: each a block into the MAI's FIFO, the last back to the first */
LOCAL void cbs_build( INT h )
{
	UD	dst = HD_PA + mai_off[h] + MAI_DATA;
	INT	i;

	for ( i = 0; i < NBLK; i++ ) {
		UW	*c = cbs + i * (CB_SIZE / 4);
		UD	src = blk_pa(i);
		UD	next = cbs_pa + (UD)((i + 1) % NBLK) * CB_SIZE;

		knl_memset(c, 0, CB_SIZE);
		c[0] = TI_WAIT_RESP | TI_D_DREQ | TI_PER_MAP(dreq[h]);
		c[1] = (UW)src;
		c[2] = (UW)(src >> 32) | XI_INC | XI_SIZE_128 | XI_BURST_LEN(3);
		c[3] = (UW)dst;
		c[4] = (UW)(dst >> 32) | XI_BURST_LEN(3);
		c[5] = BLK_BYTES;
		c[6] = (UW)(next >> 5);
	}
	USB_MB();
}

/* The DMA onto the ring from its first block, every block filled */
LOCAL void dma_start( INT h )
{
	INT	i;

	dma_halt();
	for ( i = 0; i < NBLK; i++ ) {
		blk_fill(i);
	}
	cbs_build(h);
	last_k = seen_k = 0;
	moved_ms = knl_usb_ms();
	wr(CH_PA + CH_CB, (UW)(cbs_pa >> 5));
	wr(CH_PA + CH_CS, CS_ACTIVE | CS_PROT | CS_QOS(10) | CS_PANIC_QOS(15));
}

/* The block the DMA is on: from the control block it holds, -1 none of ours */
LOCAL INT dma_block( void )
{
	UD	cb = (UD)rd(CH_PA + CH_CB) << 5;

	if ( cb < cbs_pa || cb >= cbs_pa + NBLK * CB_SIZE ) {
		return -1;
	}

	return (INT)((cb - cbs_pa) / CB_SIZE);
}

/* ---------------------------------------------------------------- the HDMI side */

LOCAL UD mai( INT h, UW reg )
{
	return HD_PA + mai_off[h] + reg;
}

/* The AUDIO InfoFrame: two channels, everything else as the stream says */
LOCAL void infoframe( INT h, BOOL on )
{
	UD	base = ram_pa[h] + PACKET_AUDIO * PACKET_STRIDE;
	UB	f[14];
	UW	cfg;
	INT	i, sum = 0;

	cfg = rd(core_pa[h] + HDMI_RAM_PACKET_CONFIG) & ~(1U << PACKET_AUDIO);
	wr(core_pa[h] + HDMI_RAM_PACKET_CONFIG, cfg);
	for ( i = 0; i < 100 && (rd(core_pa[h] + HDMI_RAM_PACKET_STATUS) & (1U << PACKET_AUDIO)) != 0; i++ ) {
		tk_dly_tsk(1);
	}
	if ( !on ) {
		return;
	}
	knl_memset(f, 0, sizeof(f));
	f[0] = 0x84;				/* type */
	f[1] = 0x01;				/* version */
	f[2] = 10;				/* length */
	f[4] = 0x01;				/* PCM as the stream says, 2 channels */
	for ( i = 0; i < 14; i++ ) {
		sum += f[i];
	}
	f[3] = (UB)(0x100 - (sum & 0xff));
	wr(base + 0x00, (UW)f[0] | ((UW)f[1] << 8) | ((UW)f[2] << 16));
	wr(base + 0x04, (UW)f[3] | ((UW)f[4] << 8) | ((UW)f[5] << 16) | ((UW)f[6] << 24));
	wr(base + 0x08, (UW)f[7] | ((UW)f[8] << 8) | ((UW)f[9] << 16));
	wr(base + 0x0c, (UW)f[10] | ((UW)f[11] << 8) | ((UW)f[12] << 16) | ((UW)f[13] << 24));
	for ( i = 0x10; i < PACKET_STRIDE; i += 4 ) {
		wr(base + (UW)i, 0);
	}
	wr(core_pa[h] + HDMI_RAM_PACKET_CONFIG, cfg | RAM_PACKET_ENABLE | (1U << PACKET_AUDIO));
}

/*
 * The MAI and the audio packets set up for 48kHz stereo PCM: the sample
 * clock from 108MHz (N/M = 2250/1), N = 6144 and CTS = the pixel clock
 * in kHz, which together give the screen 48kHz again.
 */
LOCAL void audio_up( INT h )
{
	UW	cts = ( out[h].clock_khz != 0 ) ? out[h].clock_khz : 148500;

	/* the 108MHz clock of the controller on */
	wr(DVP_PA + DVP_MISC_CONFIG, rd(DVP_PA + DVP_MISC_CONFIG) & ~dvp_gate[h]);
	wr(mai(h, MAI_CTL), MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
	wr(mai(h, MAI_SMP), ((UW)(AUDIO_CLOCK / OUT_RATE) << 8) | 0);
	wr(mai(h, MAI_CTL), MAI_CTL_CHNUM(2) | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN | MAI_CTL_ENABLE);
	wr(mai(h, MAI_FMT), MAI_FMT_48000 | MAI_FMT_PCM);
	if ( d0 ) {
		wr(mai(h, MAI_THR), (0x10U << 23) | (0x10U << 15) | (0x1cU << 7) | 0x1cU);
	} else {
		wr(mai(h, MAI_THR), (0x10U << 24) | (0x10U << 16) | (0x1cU << 8) | 0x1cU);
	}
	wr(core_pa[h] + HDMI_MAI_CONFIG, MAI_CONFIG_BIT_REVERSE | MAI_CONFIG_FORMAT_REVERSE | 0x3);
	wr(core_pa[h] + HDMI_MAI_CHANNEL_MAP, (0U << 0) | (1U << 4));
	wr(core_pa[h] + HDMI_AUDIO_PACKET, AUDIO_ZERO_ON_FLAT | AUDIO_ZERO_ON_INACTIVE
				       | AUDIO_B_FRAME_ID(8) | 0x3);
	wr(core_pa[h] + HDMI_CRP_CFG, CRP_EXTERNAL_CTS | 6144);
	wr(core_pa[h] + HDMI_CTS_0, cts);
	wr(core_pa[h] + HDMI_CTS_1, cts);
	infoframe(h, TRUE);
}

LOCAL void audio_down( INT h )
{
	wr(mai(h, MAI_CTL), MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
	infoframe(h, FALSE);
	wr(mai(h, MAI_CTL), MAI_CTL_RESET);
	wr(mai(h, MAI_CTL), MAI_CTL_ERRORF);
	wr(mai(h, MAI_CTL), MAI_CTL_FLUSH);
}

/* Whether socket h can sound now: driven, sent HDMI rather than DVI, plugged, the screen willing */
LOCAL BOOL can_sound( INT h )
{
	if ( !out[h].driven ) {
		return FALSE;
	}
	if ( (rd(core_pa[h] + HDMI_SCHEDULER_CONTROL) & SCHED_MODE_HDMI) == 0 ) {
		return FALSE;
	}
	if ( (rd(core_pa[h] + HDMI_HOTPLUG) & HOTPLUG_CONNECTED) == 0 ) {
		return FALSE;
	}

	return ( !out[h].edid || out[h].sound );
}

/* ---------------------------------------------------------------- backend */

LOCAL ER hdmi_outs( SNDOUTS *l )
{
	INT	h;
	ER	er = E_OK;

	for ( h = 0; h < 2 && hdmi_ok && er >= E_OK; h++ ) {
		if ( !out[h].driven ) {
			continue;
		}
		er = knl_snd_out_add(l, (CONST UB *)( h == 0 ? "hdmi0" : "hdmi1" ), out[h].name,
				     SND_BE_HDMI, can_sound(h) ? 0 : SND_OF_NOSOUND);
	}

	return er;
}

LOCAL BOOL hdmi_can_record( void )
{
	return FALSE;
}

/* The socket's timing registers, to see a change of mode by */
LOCAL void timing_seen( INT h, UW *t )
{
	t[0] = rd(core_pa[h] + HDMI_HORZA);
	t[1] = rd(core_pa[h] + HDMI_VERTA0);
	t[2] = rd(core_pa[h] + HDMI_VERTB0);
}

LOCAL ER hdmi_play_start( CONST UB *id, UW *rate, INT *channels )
{
	INT	h;

	if ( !hdmi_ok || id == NULL ) {
		return E_NOEXS;
	}
	h = ( knl_strcmp((CONST char *)id, "hdmi1") == 0 ) ? 1
	  : ( knl_strcmp((CONST char *)id, "hdmi0") == 0 ) ? 0 : -1;
	if ( h < 0 || !can_sound(h) ) {
		return E_NOEXS;
	}
	nblocks = rate_blocks = pace_blocks = nlate = nstuck = 0;
	iec_frame = 0;
	look_ms = start_ms = rate_ms = report_ms = check_ms = knl_usb_ms();
	lost = FALSE;
	/* the pixel clock as it is now (the CTS is made of it), and the timing to watch */
	{
		UW	khz = timing_khz(disp_id[h]);

		if ( khz != 0 ) {
			out[h].clock_khz = khz;
		}
	}
	timing_seen(h, timing);
	audio_up(h);
	dma_start(h);
	cur = h;
	*rate = OUT_RATE;
	*channels = 2;
	tm_printf((UB *)"TessronOS: hdmi%d plays at %d Hz (%d blocks of %d frames); pixel clock %u kHz, "
		  "mai ctl %08x, sched %08x, packets %08x/%08x\n",
		  h, OUT_RATE, NBLK, BLK_FRAMES, out[h].clock_khz, rd(mai(h, MAI_CTL)),
		  rd(core_pa[h] + HDMI_SCHEDULER_CONTROL), rd(core_pa[h] + HDMI_RAM_PACKET_CONFIG),
		  rd(core_pa[h] + HDMI_RAM_PACKET_STATUS));

	return E_OK;
}

LOCAL void hdmi_play_stop( void )
{
	if ( cur < 0 ) {
		return;
	}
	dma_halt();
	audio_down(cur);
	tm_printf((UB *)"TessronOS: hdmi%d stops: %u blocks, %u late looks, %u restarts\n",
		  cur, nblocks, nlate, nstuck);
	cur = -1;
}

LOCAL BOOL hdmi_play_running( void )
{
	return ( cur >= 0 && !lost );
}

LOCAL ER hdmi_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	return E_NOSPT;
}

LOCAL void hdmi_rec_stop( void )
{
}

LOCAL BOOL hdmi_rec_running( void )
{
	return FALSE;
}

/*
 * Every few milliseconds while the stream runs: the blocks from last_k
 * up to the one before the block the DMA is on are played, and refilled.
 */
LOCAL void hdmi_poll( void )
{
	UW	now, gap;
	INT	k, n;

	if ( cur < 0 || lost ) {
		return;
	}
	now = knl_usb_ms();
	gap = now - look_ms;
	look_ms = now;
	if ( (INT)(now - check_ms) >= CHECK_MS ) {
		UW	t[3];

		check_ms = now;
		timing_seen(cur, t);
		if ( !can_sound(cur) || t[0] != timing[0] || t[1] != timing[1] || t[2] != timing[2] ) {
			/* unplugged, DVI, or another mode: the stream ends, to be started again */
			tm_printf((UB *)"TessronOS: hdmi%d: the screen changed (%s), the stream ends\n", cur,
				  can_sound(cur) ? "its timing" : "no sound now");
			lost = TRUE;
			return;
		}
	}
	k = dma_block();
	if ( k < 0 || ( k == seen_k && (INT)(now - moved_ms) >= STUCK_MS ) ) {
		/* not on the ring, or not moving: started again */
		nstuck++;
		if ( nstuck <= 5 || nstuck % 50 == 0 ) {
			tm_printf((UB *)"TessronOS: hdmi%d: the DMA %s (%u times): cs %08x, cb %08x, "
				  "debug %08x, mai ctl %08x; started again\n", cur,
				  ( k < 0 ) ? "is off the ring" : "has not moved", nstuck,
				  rd(CH_PA + CH_CS), rd(CH_PA + CH_CB), rd(CH_PA + CH_DEBUG),
				  rd(mai(cur, MAI_CTL)));
		}
		audio_up(cur);
		dma_start(cur);
		return;
	}
	if ( k != seen_k ) {
		seen_k = k;
		moved_ms = now;
	}
	if ( gap * 4 >= RING_MS * 3 ) {
		/* so late the DMA may have gone round: all of it but the block it is on */
		nlate++;
		n = ( k - last_k + NBLK ) % NBLK;
		if ( n == 0 ) {
			n = NBLK - 1;
		}
	} else {
		n = ( k - last_k + NBLK ) % NBLK;
	}
	rate_blocks += (UW)n;
	while ( n-- > 0 ) {
		blk_fill(last_k);
		last_k = ( last_k + 1 ) % NBLK;
	}
	if ( (INT)(now - rate_ms) >= (( (INT)(now - start_ms) < RATE_EARLY_MS ) ? RATE_MS : REPORT_MS) ) {
		UW	ms = now - rate_ms;
		UW	x10 = ( ms > 0 ) ? (UW)((UD)rate_blocks * 10000 / ms) : 0;

		pace_blocks += rate_blocks;
		tm_printf((UB *)"TessronOS: hdmi%d: %u blocks in %u ms, %u.%u a second (187.5 at %d Hz); "
			  "%u late looks\n", cur, rate_blocks, ms, x10 / 10, x10 % 10, OUT_RATE, nlate);
		rate_ms = now;
		rate_blocks = 0;
	}
}

LOCAL ER hdmi_get_level( INT dn, SDVol *v )
{
	return E_NOSPT;
}

LOCAL ER hdmi_set_level( INT dn, CONST SDVol *v )
{
	return E_NOSPT;
}

EXPORT CONST SNDHW knl_hdmi_hw = {
	SND_BE_HDMI,
	NULL,
	hdmi_can_record,
	hdmi_play_start,
	hdmi_play_stop,
	hdmi_play_running,
	hdmi_rec_start,
	hdmi_rec_stop,
	hdmi_rec_running,
	hdmi_poll,
	hdmi_get_level,
	hdmi_set_level,
	HDMI_LEAD_MS,
	HDMI_TAIL_MS,
	hdmi_outs
};

/*
 * For the checks on the board (sndtest.c): the socket playing, the
 * blocks the DMA finished and the milliseconds since the stream started.
 * -1 when none plays.
 */
EXPORT INT knl_hdmi_pace( UW *blocks, UW *ms )
{
	if ( cur < 0 ) {
		return -1;
	}
	*blocks = pace_blocks + rate_blocks;
	*ms = knl_usb_ms() - start_ms;

	return cur;
}

/*
 * At the start, after the screen: which sockets the firmware drives and
 * what their screens say, and the memory the DMA walks, taken once.
 */
EXPORT ER knl_hdmi_init( void )
{
#if CNF_SND_HDMI
	INT	h;

	knl_memset(out, 0, sizeof(out));
	displays_find();
	if ( !out[0].driven && !out[1].driven ) {
		tm_printf((UB *)"TessronOS: hdmi sound: no HDMI display\n");
		return E_NOEXS;
	}
	d0 = ( (rd(HVS_PA) & 0xff) == HVS_VERSION_D0 );
	for ( h = 0; h < 2; h++ ) {
		if ( !out[h].driven ) {
			continue;
		}
		edid_read(h);
		out[h].clock_khz = timing_khz(disp_id[h]);
		tm_printf((UB *)"TessronOS: hdmi%d: screen \"%s\", EDID %s, sound %s, pixel clock %u kHz, "
			  "%s, hot plug %s, dvp %08x/%08x\n", h, out[h].name,
			  out[h].edid ? "read" : "not read", out[h].sound ? "yes" : "no", out[h].clock_khz,
			  (rd(core_pa[h] + HDMI_SCHEDULER_CONTROL) & SCHED_MODE_HDMI) ? "HDMI" : "DVI",
			  (rd(core_pa[h] + HDMI_HOTPLUG) & HOTPLUG_CONNECTED) ? "in" : "out",
			  rd(DVP_PA + DVP_SW_INIT), rd(DVP_PA + DVP_MISC_CONFIG));
	}
	if ( knl_dmabuf_alloc(&ring, NBLK * BLK_BYTES) < E_OK ) {
		return E_NOMEM;
	}
	cbs = (UW *)knl_usb_page_alloc(&cbs_pa);
	if ( cbs == NULL ) {
		knl_dmabuf_free(&ring);
		return E_NOMEM;
	}
	cbs_pa -= RP1_DMA_BUS_OFFSET;
	hdmi_ok = TRUE;

	return E_OK;
#else
	return E_NOSPT;
#endif
}

#endif /* RPI5 */
