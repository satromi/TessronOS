/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	snd_rp1.c
 *	Board sound of the Raspberry Pi 5: I2S on the header pins, for a DAC
 *	board (design 10.15).
 *
 *	The Pi 5 has no analogue output of its own. What it has is the RP1's
 *	I2S interface on GPIO 18 (bit clock), 19 (frame clock), 20 (data in)
 *	and 21 (data out), where DAC boards of the PCM5102A kind sit. Those
 *	take their clocks from the Pi, so the interface that produces the
 *	clocks, I2S0, is the one used.
 *
 *	The pieces, all inside the RP1 and reached through its window:
 *
 *	  the audio PLL	    run at 1536MHz from the 50MHz crystal and
 *			    divided by 25 to 61.44MHz
 *	  clk_i2s	    the PLL divided by 20: 3.072MHz, the bit clock of
 *			    48kHz with two 32 bit slots a frame
 *	  I2S0		    a DesignWare I2S block in its clock producing
 *			    mode, one stereo channel, 32 bit words
 *	  the DMA	    a DesignWare AXI DMA controller; one channel walks
 *			    a ring of linked blocks into the transmit FIFO,
 *			    paced by I2S0's request line
 *
 *	The ring is 16 blocks of 256 frames, 85ms in all, its last linked
 *	block pointing back at the first. Nothing interrupts: the USB
 *	manager's task, which runs every few milliseconds while sound
 *	streams, reads which block the DMA is on and refills the ones it has
 *	finished with freshly mixed samples (silence when no program has
 *	any), so the ring never runs dry of valid blocks while the looks
 *	keep up.
 *
 *	The controller writes each linked block back when it has finished
 *	it, with its valid mark cleared, and stops at a block not valid with
 *	the error noted in the channel's status. So every block refilled is
 *	marked valid again, and a look that comes later than the ring lasts
 *	finds the DMA waiting at the oldest block not refilled. Then, and
 *	whenever the channel notes any other error or the DMA is seen not to
 *	move for STUCK_MS, the channel is stopped for certain (disabled, and
 *	aborted if it will not), the ring built and filled anew and the DMA
 *	started again from its first block; the registers that tell why are
 *	printed. A stop of the DMA costs at most the time until the next
 *	look, not the rest of the stream.
 *
 *	The DMA sees memory through the same window as the other RP1 bus
 *	masters, at the physical address plus RP1_DMA_BUS_OFFSET, and I2S0's
 *	FIFO at the RP1's own address for it.
 *
 *	Only playback; the data in pin is not used.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/usb.h>
#include <ts/gpio.h>
#include "sysdepend.h"
#include "sndhw.h"
#include "../usb/usbdev.h"

/* ---------------------------------------------------------------- clocks */

#define CLK_BASE		(RP1_WINDOW_BASE + 0x018000)
#define PLL_AUDIO_CS		(CLK_BASE + 0x0c000)
#define PLL_AUDIO_PWR		(CLK_BASE + 0x0c004)
#define PLL_AUDIO_FBDIV_INT	(CLK_BASE + 0x0c008)
#define PLL_AUDIO_FBDIV_FRAC	(CLK_BASE + 0x0c00c)
#define PLL_AUDIO_PRIM		(CLK_BASE + 0x0c010)
#define CLK_I2S_CTRL		(CLK_BASE + 0x000b4)
#define CLK_I2S_DIV_INT		(CLK_BASE + 0x000b8)
#define CLK_I2S_DIV_FRAC	(CLK_BASE + 0x000bc)	/* only read, to be printed */
#define CLK_I2S_SEL		(CLK_BASE + 0x000c0)

/*
 * The frequency counters: eight sets of registers 0x20 apart, each
 * counting one of its sources against clk_slow_sys (the 50MHz crystal)
 * and answering in kHz with five bits of fraction.
 */
#define FC_BASE(n)		(CLK_BASE + 0x0021c + (UBINT)(n) * 0x20)
#define FC_REF_KHZ		0x00
#define FC_MIN_KHZ		0x04
#define FC_MAX_KHZ		0x08
#define FC_DELAY		0x0c
#define FC_INTERVAL		0x10
#define FC_SRC			0x14
#define FC_STATUS		0x18
#define FC_RESULT		0x1c
#define FC_DONE			0x00000010
#define FC_RUNNING		0x00000100
#define FC_REF			50000		/* kHz */
#define FC_WAIT_MS		20
/* the counter and its source for each clock looked at */
#define FC_PLL_AUDIO		4, 2
#define FC_PLL_AUDIO_SEC	6, 2
#define FC_CLK_I2S		4, 4

#define PLL_CS_LOCK		0x80000000
#define PLL_CS_REFDIV_1		0x00000001
#define PLL_CS_REFDIV_MASK	0x0000003f
#define PLL_PWR_DSMPD		0x00000004	/* the fraction is not used */
#define PLL_PWR_ALL		0x0000003f	/* every part powered down */
#define PLL_PRIM_DIV1(n)	((UW)(n) << 16)
#define PLL_PRIM_DIV2(n)	((UW)(n) << 12)
#define PLL_PRIM_DIVS		0x00077000
#define CLK_CTRL_ENABLE		0x00000800
#define CLK_CTRL_AUXSRC(n)	((UW)(n) << 5)
#define CLK_CTRL_AUXSRC_MASK	0x000003e0
#define CLK_AUX_PLL_AUDIO	1

/* 50MHz x 30.72 = 1536MHz; /25 = 61.44MHz; /20 = 3.072MHz */
#define PLL_FBDIV_INT		30
#define PLL_FBDIV_FRAC		12079596	/* 0.72 in 24 bits */
#define PLL_DIV1		5
#define PLL_DIV2		5
#define I2S_CLK_DIV		20

/* ---------------------------------------------------------------- I2S0 */

#define I2S_BASE		(RP1_WINDOW_BASE + 0x0a0000)
#define I2S_BUS			0xc0400a0000ULL	/* where the DMA sees it */
#define I2S_IER			0x000
#define I2S_ITER		0x008
#define I2S_CER			0x00c
#define I2S_CCR			0x010
#define I2S_TXFFR		0x018
#define I2S_RER(n)		(0x028 + (n) * 0x40)
#define I2S_TER(n)		(0x02c + (n) * 0x40)
#define I2S_TCR0		0x034
#define I2S_ISR0		0x038
#define I2S_IMR(n)		(0x03c + (n) * 0x40)
#define I2S_TFCR0		0x04c
#define I2S_NPAIR		4		/* stereo channel pairs the block may have */
#define I2S_TXDMA		0x1c8
#define I2S_COMP_PARAM_1	0x1f4
#define I2S_COMP_TYPE		0x1fc
#define I2S_DMACR		0x200

#define I2S_TYPE_DW		0x445701a0	/* the DesignWare block's mark */
#define CCR_WSS_32		0x10		/* 32 clocks a slot */
#define TCR_32BIT		5
#define DMACR_TXCH0		0x00000100	/* reads back as 0 on the RP1 */
#define DMACR_TX		0x00020000

/*
 * ISR0, as printed: bit 4 (0x10) the transmit FIFO at or below its
 * threshold, that is, waiting for the DMA; bit 5 (0x20) a word written
 * to a full FIFO. Running dry is not marked: the interface then sends
 * what it has, and the DAC hears a gap.
 */

/* ---------------------------------------------------------------- DMA */

#define DMA_BASE		(RP1_WINDOW_BASE + 0x188000)
#define DMAC_CFG		0x010
#define DMAC_CHEN		0x018		/* enable 7:0, its write enable 15:8 */
#define DMAC_CHEN_H		0x01c		/* abort 7:0 (bits 39:32), its write enable 15:8 */
#define DMA_CH			7		/* the last of its eight channels */
#define CH_BASE			(DMA_BASE + 0x100 + DMA_CH * 0x100)
#define CH_CTL_L		0x018
#define CH_CTL_H		0x01c
#define CH_CFG_L		0x020
#define CH_CFG_H		0x024
#define CH_LLP_L		0x028
#define CH_LLP_H		0x02c
#define CH_STATUS_L		0x030		/* items of the current block done */
#define CH_INTSTATUS_ENA	0x080
#define CH_INTSTATUS		0x088
#define CH_INTSIGNAL_ENA	0x090
#define CH_INTCLEAR		0x098

/* the channel's status bits: the errors, and a linked block found not valid among them */
#define INT_BLOCK_DONE		0x00000001
#define INT_LLI_INVALID		0x00002000
#define INT_ERRORS		0x003f7fe0

#define DREQ_I2S0_TX		0x20

/* control word of each block */
#define CTL_SRC_INC		(0 << 4)
#define CTL_DST_NOINC		(1 << 6)
#define CTL_SRC_WIDTH32		(2 << 8)
#define CTL_DST_WIDTH32		(2 << 11)
#define CTL_SRC_MSIZE4		(1 << 14)
#define CTL_DST_MSIZE4		(1 << 18)
#define CTLH_LLI_VALID		0x80000000

/* configuration, in the layout of a controller of more than 16 targets */
#define CFGL_MULTBLK_LL		0x0000000f	/* linked lists both ways */
#define CFGL_DST_PER(n)		((UW)(n) << 11)
#define CFGH_TT_MEM_TO_PER	1
#define CFGH_PRIORITY(n)	((UW)(n) << 20)

/* ---------------------------------------------------------------- the ring */

#define NBLK			16
#define BLK_FRAMES		256
#define BLK_BYTES		(BLK_FRAMES * 8)	/* two 32 bit words a frame */
#define OUT_RATE		48000
#define LLI_SIZE		64

#define STUCK_MS		100	/* the DMA on one block this long: it does not move */
#define HALT_MS			5	/* a channel given this long to stop */
#define REPORT_MS		60000	/* how often a stream that had trouble says so */
#define RATE_MS			5000	/* how often the pace of the ring is told */
#define RATE_EARLY_MS		30000	/* that often for this long after a start, then every REPORT_MS */

/* why the DMA was started again */
#define WHY_USED		0	/* it waited at a block not refilled: a late look */
#define WHY_ERROR		1	/* some other error in the channel's status */
#define WHY_STUCK		2	/* it did not move */
#define NWHY			3

LOCAL BOOL	i2s_ok = FALSE;
LOCAL BOOL	running = FALSE;
LOCAL DMABUF	ring;			/* NBLK blocks, two to a page */
LOCAL UW	*lli;			/* the linked blocks, a page */
LOCAL UD	lli_bus;
LOCAL INT	last_k;			/* the oldest block played and not refilled */
LOCAL INT	seen_k;			/* the DMA's linked block at the last look */
LOCAL UW	moved_ms;		/* when it was last seen to move */
LOCAL UW	look_ms;		/* the last look */
LOCAL UW	gap_max;		/* the longest time between two looks since the last report */
LOCAL UW	report_ms;		/* the last report */
LOCAL UW	nblocks;		/* blocks refilled since the stream started */
LOCAL INT	restarts[NWHY];		/* since the stream started */
LOCAL INT	restarts_rep;		/* all of them at the last report */
LOCAL INT	nrestart;		/* all of them */
LOCAL UW	start_ms;		/* when the stream started */
LOCAL UW	rate_ms;		/* the last report of the pace */
LOCAL UW	rate_blocks;		/* blocks the DMA finished since then */
LOCAL UW	pace_blocks;		/* and before then, since the stream started */
LOCAL UW	ter_reset[I2S_NPAIR];	/* TER of each pair before the first set-up */
LOCAL BOOL	ter_seen = FALSE;

LOCAL UW rd( UBINT a )
{
	return *(volatile UW *)a;
}

LOCAL void wr( UBINT a, UW v )
{
	*(volatile UW *)a = v;
	DeviceBarrier();
}

LOCAL UB *blk_addr( INT i )
{
	return ring.virt[i / 2] + (i % 2) * BLK_BYTES;
}

LOCAL UD blk_bus( INT i )
{
	return ring.bus[i / 2] + (UD)(i % 2) * BLK_BYTES;
}

/*
 * One block of mixed samples, 16 bit, widened to the 32 bit words the
 * interface sends, the sample in the upper half. With no program's
 * samples to mix it is silence (or a held level), never nothing.
 */
LOCAL void blk_fill( INT i )
{
	H	pcm[BLK_FRAMES * 2];
	UW	*w = (UW *)blk_addr(i);
	INT	k;

	knl_snd_mix((UB *)pcm, BLK_FRAMES, OUT_RATE, 2, FALSE);
	for ( k = 0; k < BLK_FRAMES * 2; k++ ) {
		w[k] = (UW)((UW)(UH)pcm[k] << 16);
	}
	USB_MB();
	nblocks++;
}

/* ---------------------------------------------------------------- set-up */

/* The audio PLL at 61.44MHz, left alone when it already is */
LOCAL ER pll_up( void )
{
	UW	prim;
	INT	i;

	if ( (rd(PLL_AUDIO_CS) & PLL_CS_LOCK) != 0
	  && (rd(PLL_AUDIO_CS) & PLL_CS_REFDIV_MASK) == PLL_CS_REFDIV_1
	  && (rd(PLL_AUDIO_PWR) & PLL_PWR_ALL) == 0	/* all on, the fraction too */
	  && rd(PLL_AUDIO_FBDIV_INT) == PLL_FBDIV_INT
	  && rd(PLL_AUDIO_FBDIV_FRAC) == PLL_FBDIV_FRAC
	  && (rd(PLL_AUDIO_PRIM) & PLL_PRIM_DIVS)
	     == (PLL_PRIM_DIV1(PLL_DIV1) | PLL_PRIM_DIV2(PLL_DIV2)) ) {
		return E_OK;
	}
	wr(PLL_AUDIO_PWR, PLL_PWR_ALL);
	wr(PLL_AUDIO_FBDIV_INT, 0);
	wr(PLL_AUDIO_FBDIV_FRAC, 0);
	wr(PLL_AUDIO_CS, PLL_CS_REFDIV_1);
	wr(PLL_AUDIO_FBDIV_INT, PLL_FBDIV_INT);
	wr(PLL_AUDIO_FBDIV_FRAC, PLL_FBDIV_FRAC);
	wr(PLL_AUDIO_PWR, 0);			/* all of it on, the fraction too */
	for ( i = 0; i < 100 && (rd(PLL_AUDIO_CS) & PLL_CS_LOCK) == 0; i++ ) {
		tk_dly_tsk(1);
	}
	if ( (rd(PLL_AUDIO_CS) & PLL_CS_LOCK) == 0 ) {
		return E_IO;			/* it would not lock */
	}
	prim = rd(PLL_AUDIO_PRIM) & ~PLL_PRIM_DIVS;
	wr(PLL_AUDIO_PRIM, prim | PLL_PRIM_DIV1(PLL_DIV1) | PLL_PRIM_DIV2(PLL_DIV2));

	return E_OK;
}

/* clk_i2s from the audio PLL, divided down to the bit clock */
LOCAL void clk_up( void )
{
	UW	ctrl;

	wr(CLK_I2S_DIV_INT, I2S_CLK_DIV);
	ctrl = rd(CLK_I2S_CTRL) & ~CLK_CTRL_AUXSRC_MASK;
	wr(CLK_I2S_CTRL, ctrl | CLK_CTRL_AUXSRC(CLK_AUX_PLL_AUDIO));
	wr(CLK_I2S_CTRL, rd(CLK_I2S_CTRL) | CLK_CTRL_ENABLE);
}

/* GPIO 18 .. 21 to I2S0, their pads driving and listening */
#define PAD(n)			(RP1_PADS_BANK0_BASE + 4 + (UBINT)(n) * 4)
#define PAD_IN_ENABLE		0x00000040
#define PAD_OUT_DISABLE		0x00000080
#define FUNC_I2S0		2

LOCAL void pins_up( void )
{
	UINT	pin;

	for ( pin = 18; pin <= 21; pin++ ) {
		wr(PAD(pin), (rd(PAD(pin)) & ~PAD_OUT_DISABLE) | PAD_IN_ENABLE);
		rp1_gpio_set_func(pin, FUNC_I2S0);
	}
}

/* The ring of linked blocks, the last pointing back at the first, every one valid */
LOCAL void lli_build( void )
{
	INT	i;

	for ( i = 0; i < NBLK; i++ ) {
		UW	*l = lli + i * (LLI_SIZE / 4);
		UD	src = blk_bus(i);
		UD	dst = I2S_BUS + I2S_TXDMA;
		UD	next = lli_bus + (UD)((i + 1) % NBLK) * LLI_SIZE;

		knl_memset(l, 0, LLI_SIZE);
		l[0] = (UW)src;
		l[1] = (UW)(src >> 32);
		l[2] = (UW)dst;
		l[3] = (UW)(dst >> 32);
		l[4] = BLK_FRAMES * 2 - 1;	/* 32 bit transfers, less one */
		l[6] = (UW)next;
		l[7] = (UW)(next >> 32);
		l[8] = CTL_SRC_INC | CTL_DST_NOINC | CTL_SRC_WIDTH32 | CTL_DST_WIDTH32
		     | CTL_SRC_MSIZE4 | CTL_DST_MSIZE4;
		l[9] = CTLH_LLI_VALID;
	}
	USB_MB();
}

/* A linked block valid again, after the controller wrote it back with the mark cleared */
LOCAL void lli_valid( INT i )
{
	UW	*l = lli + i * (LLI_SIZE / 4);

	l[8] = CTL_SRC_INC | CTL_DST_NOINC | CTL_SRC_WIDTH32 | CTL_DST_WIDTH32
	     | CTL_SRC_MSIZE4 | CTL_DST_MSIZE4;
	l[9] = CTLH_LLI_VALID;
	USB_MB();
}

LOCAL BOOL ch_on( void )
{
	return ( rd(DMA_BASE + DMAC_CHEN) & (1U << DMA_CH) ) != 0;
}

/* Up to HALT_MS for the channel to say it is off */
LOCAL BOOL ch_wait_off( void )
{
	UW	t0 = knl_usb_ms();

	while ( ch_on() ) {
		if ( (INT)(knl_usb_ms() - t0) > HALT_MS ) {
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * The channel stopped for certain: disabled (the enable bit written as
 * 0, with its write enable), and when it does not go off, aborted, which
 * drops what it was doing. Its status is cleared after.
 */
LOCAL void dma_halt( void )
{
	if ( ch_on() ) {
		wr(DMA_BASE + DMAC_CHEN, (UW)(1U << (8 + DMA_CH)));
		if ( !ch_wait_off() ) {
			wr(DMA_BASE + DMAC_CHEN_H, (UW)((1U << DMA_CH) | (1U << (8 + DMA_CH))));
			if ( !ch_wait_off() ) {
				tm_printf((UB *)"TessronOS: i2s0: the DMA channel does not stop\n");
			}
		}
	}
	wr(CH_BASE + CH_INTCLEAR, 0xffffffff);
}

/* The DMA channel onto a new ring, every block filled, from the first block */
LOCAL void dma_start( void )
{
	INT	i;

	dma_halt();
	for ( i = 0; i < NBLK; i++ ) {
		blk_fill(i);
	}
	lli_build();
	last_k = 0;
	seen_k = 0;
	moved_ms = knl_usb_ms();

	wr(DMA_BASE + DMAC_CFG, rd(DMA_BASE + DMAC_CFG) | 1);	/* the controller on */
	wr(CH_BASE + CH_CFG_L, CFGL_MULTBLK_LL | CFGL_DST_PER(DREQ_I2S0_TX));
	wr(CH_BASE + CH_CFG_H, CFGH_TT_MEM_TO_PER | CFGH_PRIORITY(DMA_CH));
	/* the errors noted in the status, for the look; none raises an interrupt */
	wr(CH_BASE + CH_INTSIGNAL_ENA, 0);
	wr(CH_BASE + CH_INTSTATUS_ENA, INT_ERRORS);
	wr(CH_BASE + CH_INTCLEAR, 0xffffffff);
	wr(CH_BASE + CH_LLP_L, (UW)lli_bus);
	wr(CH_BASE + CH_LLP_H, (UW)(lli_bus >> 32));
	wr(DMA_BASE + DMAC_CHEN, (UW)((1U << DMA_CH) | (1U << (8 + DMA_CH))));
}

LOCAL void i2s_stop( void )
{
	wr(I2S_BASE + I2S_DMACR, 0);
	wr(I2S_BASE + I2S_ITER, 0);
	wr(I2S_BASE + I2S_CER, 0);
	wr(I2S_BASE + I2S_IER, 0);
}

/*
 * The interface set up for one stereo channel, its FIFO emptied; not yet
 * running.
 *
 * The transmit and receive enables of every channel pair come out of
 * reset set, and the DMA register hands its words to the enabled
 * transmit pairs in turn. With all four pairs on, three frames of every
 * four go to pairs no pin carries, and the ring is used up four times
 * as fast as the frame clock runs. So every pair is turned off first and
 * only the first one on again.
 */
LOCAL void i2s_setup( void )
{
	UW	comp1, depth;
	INT	i;

	i2s_stop();
	if ( !ter_seen ) {
		for ( i = 0; i < I2S_NPAIR; i++ ) {
			ter_reset[i] = rd(I2S_BASE + I2S_TER(i));
		}
		ter_seen = TRUE;
	}
	for ( i = 0; i < I2S_NPAIR; i++ ) {
		wr(I2S_BASE + I2S_TER(i), 0);
		wr(I2S_BASE + I2S_RER(i), 0);
		wr(I2S_BASE + I2S_IMR(i), 0x33);	/* no interrupts */
	}
	comp1 = rd(I2S_BASE + I2S_COMP_PARAM_1);
	depth = 1U << (1 + ((comp1 >> 2) & 3));
	wr(I2S_BASE + I2S_CCR, CCR_WSS_32);
	wr(I2S_BASE + I2S_TCR0, TCR_32BIT);
	wr(I2S_BASE + I2S_TFCR0, depth / 2 - 1);
	wr(I2S_BASE + I2S_TER(0), 1);
	wr(I2S_BASE + I2S_TXFFR, 1);
	wr(I2S_BASE + I2S_DMACR, DMACR_TXCH0 | DMACR_TX);
}

/* ---------------------------------------------------------------- measuring */

/* A clock counted by frequency counter n from its source src: kHz x 32, 0 when it does not answer */
LOCAL UW fc_count( INT n, INT src )
{
	UBINT	b = FC_BASE(n);
	UW	t0, r;

	t0 = knl_usb_ms();
	while ( (rd(b + FC_STATUS) & FC_RUNNING) != 0 ) {
		if ( (INT)(knl_usb_ms() - t0) > FC_WAIT_MS ) {
			return 0;
		}
	}
	wr(b + FC_REF_KHZ, FC_REF);
	wr(b + FC_MIN_KHZ, 0);
	wr(b + FC_MAX_KHZ, 0x1ffffff);
	wr(b + FC_INTERVAL, 8);
	wr(b + FC_DELAY, 7);
	wr(b + FC_SRC, (UW)src);
	t0 = knl_usb_ms();
	while ( (rd(b + FC_STATUS) & FC_DONE) == 0 ) {
		if ( (INT)(knl_usb_ms() - t0) > FC_WAIT_MS ) {
			wr(b + FC_SRC, 0);
			return 0;
		}
	}
	r = rd(b + FC_RESULT);
	wr(b + FC_SRC, 0);

	return r;
}

/* The audio PLL's VCO in kHz, as its registers set it */
LOCAL UW pll_core_khz( void )
{
	UW	ref = rd(PLL_AUDIO_CS) & PLL_CS_REFDIV_MASK;
	UD	f;

	if ( ref == 0 ) {
		return 0;
	}
	f = (UD)rd(PLL_AUDIO_FBDIV_INT) << 24;
	if ( (rd(PLL_AUDIO_PWR) & PLL_PWR_DSMPD) == 0 ) {
		f += rd(PLL_AUDIO_FBDIV_FRAC) & 0xffffff;
	}

	return (UW)(((UD)FC_REF * f) >> 24) / ref;
}

/*
 * The clocks of the stream as counted, and the interface as it was
 * found and as it is now: printed when a stream starts, so a frame clock
 * that is not 48kHz shows where it comes from.
 */
LOCAL void clocks_print( void )
{
	UW	pa = fc_count(FC_PLL_AUDIO);
	UW	ps = fc_count(FC_PLL_AUDIO_SEC);
	UW	ci = fc_count(FC_CLK_I2S);
	UW	prim = rd(PLL_AUDIO_PRIM);
	UW	comp1 = rd(I2S_BASE + I2S_COMP_PARAM_1);

	tm_printf((UB *)"TessronOS: i2s0 clocks: pll_audio_core %u kHz (by its registers: cs %08x "
		  "pwr %08x fbdiv %u+%06x), pll_audio %u kHz (prim %u x %u), pll_audio_sec %u kHz, "
		  "clk_i2s %u kHz (3072 expected; ctrl %08x sel %08x div %u frac %08x)\n",
		  pll_core_khz(), rd(PLL_AUDIO_CS), rd(PLL_AUDIO_PWR),
		  rd(PLL_AUDIO_FBDIV_INT), rd(PLL_AUDIO_FBDIV_FRAC),
		  pa >> 5, (prim >> 16) & 7, (prim >> 12) & 7, ps >> 5,
		  ci >> 5, rd(CLK_I2S_CTRL), rd(CLK_I2S_SEL), rd(CLK_I2S_DIV_INT),
		  rd(CLK_I2S_DIV_FRAC));
	tm_printf((UB *)"TessronOS: i2s0 interface: comp1 %08x (%d transmit pairs), ccr %x, "
		  "ter %x/%x/%x/%x (%x/%x/%x/%x as found), tcr0 %x, tfcr0 %x, dmacr %08x\n",
		  comp1, (INT)((comp1 >> 9) & 3) + 1, rd(I2S_BASE + I2S_CCR),
		  rd(I2S_BASE + I2S_TER(0)), rd(I2S_BASE + I2S_TER(1)),
		  rd(I2S_BASE + I2S_TER(2)), rd(I2S_BASE + I2S_TER(3)),
		  ter_reset[0], ter_reset[1], ter_reset[2], ter_reset[3],
		  rd(I2S_BASE + I2S_TCR0), rd(I2S_BASE + I2S_TFCR0), rd(I2S_BASE + I2S_DMACR));
}

/*
 * The pace of the ring in real time: the blocks the DMA finished and the
 * time they took. At 48kHz that is 187.5 blocks a second.
 */
LOCAL void rate_print( UW now )
{
	UW	ms = now - rate_ms;
	UW	x10;

	if ( ms == 0 ) {
		return;
	}
	pace_blocks += rate_blocks;
	x10 = (UW)((UD)rate_blocks * 10000 / ms);
	tm_printf((UB *)"TessronOS: i2s0: %u blocks in %u ms, %u.%u a second (187.5 at %d Hz), "
		  "%u frames a second\n",
		  rate_blocks, ms, x10 / 10, x10 % 10, OUT_RATE,
		  (UW)((UD)rate_blocks * BLK_FRAMES * 1000 / ms));
	rate_ms = now;
	rate_blocks = 0;
}

/* The interface running: it asks the DMA for words from here on */
LOCAL void i2s_run( void )
{
	wr(I2S_BASE + I2S_IER, 1);
	wr(I2S_BASE + I2S_ITER, 1);
	wr(I2S_BASE + I2S_CER, 1);
}

/* ---------------------------------------------------------------- backend */

/* One output while the block is there: whether a DAC listens cannot be told */
LOCAL ER i2s_outs( SNDOUTS *l )
{
	if ( !i2s_ok ) {
		return E_OK;
	}

	return knl_snd_out_add(l, (CONST UB *)"i2s", (CONST UB *)"", SND_BE_I2S, 0);
}

LOCAL BOOL i2s_can_record( void )
{
	return FALSE;
}

LOCAL ER i2s_play_start( CONST UB *id, UW *rate, INT *channels )
{
	INT	i;
	ER	er;

	if ( !i2s_ok ) {
		return E_NOEXS;
	}
	er = pll_up();
	if ( er < E_OK ) {
		return er;
	}
	clk_up();
	pins_up();

	nblocks = 0;
	for ( i = 0; i < NWHY; i++ ) {
		restarts[i] = 0;
	}
	nrestart = restarts_rep = 0;
	gap_max = 0;
	look_ms = report_ms = start_ms = rate_ms = knl_usb_ms();
	rate_blocks = pace_blocks = 0;

	/* the interface stopped and set up, the DMA onto the ring, then the interface running */
	i2s_setup();
	dma_start();
	i2s_run();
	running = TRUE;
	*rate = OUT_RATE;
	*channels = 2;
	tm_printf((UB *)"TessronOS: i2s0 plays at %d Hz (%d blocks of %d frames)\n",
		  OUT_RATE, NBLK, BLK_FRAMES);
	clocks_print();

	return E_OK;
}

LOCAL void i2s_play_stop( void )
{
	if ( !running ) {
		return;
	}
	dma_halt();
	i2s_stop();
	running = FALSE;
	tm_printf((UB *)"TessronOS: i2s0 stops: %u blocks, the DMA started again %d times "
		  "(%d at a block not refilled, %d on an error, %d not moving); "
		  "the longest time between looks %u ms\n",
		  nblocks, nrestart, restarts[WHY_USED], restarts[WHY_ERROR],
		  restarts[WHY_STUCK], gap_max);
}

LOCAL BOOL i2s_play_running( void )
{
	return running;
}

LOCAL ER i2s_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	return E_NOSPT;
}

LOCAL void i2s_rec_stop( void )
{
}

LOCAL BOOL i2s_rec_running( void )
{
	return FALSE;
}

/*
 * The DMA started again from a new ring. What its channel and the
 * interface say is printed first (the first times, then now and then).
 * When it did not move, the interface is started again too: a request
 * line that never comes would keep it where it is.
 */
LOCAL void i2s_restart( INT why, INT k, UD llp, UW st, UW gap )
{
	LOCAL CONST char *CONST what[NWHY] = {
		"waits at a block not refilled", "notes an error", "has not moved"
	};

	restarts[why]++;
	nrestart++;
	if ( nrestart <= 5 || nrestart % 50 == 0 ) {
		tm_printf((UB *)"TessronOS: i2s0: the DMA %s (%d times): block %d, last refilled %d, "
			  "llp %08x%08x, channel %s, status %08x, ch %08x; i2s on %x/%x/%x, isr %08x, "
			  "dmacr %08x; clk %08x, pll %08x; %u ms since the last look (at most %u); "
			  "started again\n",
			  what[why], nrestart, k, ( last_k + NBLK - 1 ) % NBLK,
			  (UW)(llp >> 32), (UW)llp, ch_on() ? "on" : "off", st,
			  rd(CH_BASE + CH_STATUS_L),
			  rd(I2S_BASE + I2S_IER), rd(I2S_BASE + I2S_ITER), rd(I2S_BASE + I2S_CER),
			  rd(I2S_BASE + I2S_ISR0), rd(I2S_BASE + I2S_DMACR),
			  rd(CLK_I2S_CTRL), rd(PLL_AUDIO_CS), gap, gap_max);
	}
	if ( why == WHY_STUCK ) {
		dma_halt();
		i2s_setup();
		dma_start();
		i2s_run();
	} else {
		dma_start();
	}
}

/*
 * Every few milliseconds while the stream runs. The linked list pointer
 * names the block the DMA is on or the one it loads next, so the block
 * before it may still be read; the ones before that are finished and
 * refilled. The DMA cannot pass a block not refilled (it stops there),
 * so the blocks from last_k up to two short of the pointer are exactly
 * the ones it has finished since the last look, however long that was:
 * a pointer back at last_k is a DMA that has played the whole ring and
 * waits there, which its status says, and is not taken for a ring to
 * refill.
 */
LOCAL void i2s_poll( void )
{
	UD	llp;
	UW	now, gap, st;
	INT	k, n;

	if ( !running ) {
		return;
	}
	now = knl_usb_ms();
	gap = now - look_ms;
	look_ms = now;
	if ( gap > gap_max ) {
		gap_max = gap;
	}
	st = rd(CH_BASE + CH_INTSTATUS) & INT_ERRORS;
	llp = (UD)rd(CH_BASE + CH_LLP_L) | ((UD)rd(CH_BASE + CH_LLP_H) << 32);
	k = ( llp < lli_bus || llp >= lli_bus + NBLK * LLI_SIZE || (llp - lli_bus) % LLI_SIZE != 0 )
	    ? -1 : (INT)((llp - lli_bus) / LLI_SIZE);
	if ( st != 0 || k < 0 ) {
		i2s_restart(( st == INT_LLI_INVALID ) ? WHY_USED : WHY_ERROR, k, llp, st, gap);
		return;
	}
	if ( k != seen_k ) {
		seen_k = k;
		moved_ms = now;
	} else if ( (INT)(now - moved_ms) >= STUCK_MS ) {
		i2s_restart(WHY_STUCK, k, llp, st, gap);
		return;
	}
	if ( k != last_k ) {
		n = ( k - 1 - last_k + NBLK ) % NBLK;
		rate_blocks += (UW)n;
		while ( n-- > 0 ) {
			blk_fill(last_k);
			lli_valid(last_k);
			last_k = ( last_k + 1 ) % NBLK;
		}
	}
	/* the pace of the ring, often at first and then now and then */
	if ( (INT)(now - rate_ms) >= (( (INT)(now - start_ms) < RATE_EARLY_MS ) ? RATE_MS : REPORT_MS) ) {
		rate_print(now);
	}
	/* now and then, a stream that had trouble says how it goes */
	if ( (INT)(now - report_ms) >= REPORT_MS ) {
		if ( nrestart != restarts_rep || gap_max > 40 ) {
			tm_printf((UB *)"TessronOS: i2s0: %u blocks so far, the DMA started again %d times; "
				  "the longest time between looks %u ms\n", nblocks, nrestart, gap_max);
		}
		report_ms = now;
		restarts_rep = nrestart;
		gap_max = 0;
	}
}

LOCAL ER i2s_get_level( INT dn, SDVol *v )
{
	return E_NOSPT;
}

LOCAL ER i2s_set_level( INT dn, CONST SDVol *v )
{
	return E_NOSPT;
}

EXPORT CONST SNDHW knl_i2s_hw = {
	SND_BE_I2S,
	NULL,
	i2s_can_record,
	i2s_play_start,
	i2s_play_stop,
	i2s_play_running,
	i2s_rec_start,
	i2s_rec_stop,
	i2s_rec_running,
	i2s_poll,
	i2s_get_level,
	i2s_set_level,
	0,			/* a DAC sounds what it is given at once */
	2000,
	i2s_outs
};

/*
 * For the checks on the board (sndtest.c): the blocks the DMA finished
 * and the milliseconds since the stream started, and clk_i2s counted
 * now in kHz (0 when the counter does not answer). FALSE when no
 * stream plays.
 */
EXPORT BOOL knl_i2s_pace( UW *blocks, UW *ms, UW *clk_khz )
{
	if ( !running ) {
		return FALSE;
	}
	*blocks = pace_blocks + rate_blocks;
	*ms = knl_usb_ms() - start_ms;
	*clk_khz = fc_count(FC_CLK_I2S) >> 5;

	return TRUE;
}

/*
 * Whether the block is there: the RP1 window answers, and I2S0 carries
 * the DesignWare mark. The memory the DMA walks is taken now, once.
 */
EXPORT ER knl_i2s_init( void )
{
#if CNF_SND_I2S
	if ( rp1_gpio_read(0) < 0 ) {
		return E_NOEXS;			/* the RP1's window does not answer */
	}
	if ( rd(I2S_BASE + I2S_COMP_TYPE) != I2S_TYPE_DW ) {
		return E_NOEXS;
	}
	if ( knl_dmabuf_alloc(&ring, NBLK * BLK_BYTES) < E_OK ) {
		return E_NOMEM;
	}
	lli = (UW *)knl_usb_page_alloc(&lli_bus);
	if ( lli == NULL ) {
		knl_dmabuf_free(&ring);
		return E_NOMEM;
	}
	i2s_ok = TRUE;
	tm_printf((UB *)"TessronOS: i2s0 on GPIO 18-21 for a DAC board\n");

	return E_OK;
#else
	return E_NOSPT;
#endif
}

#endif /* RPI5 */
