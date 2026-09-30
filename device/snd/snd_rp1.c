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
 *	The ring is 16 blocks of 256 frames, 85ms in all, and loops on
 *	itself, so the DMA never stops. Nothing interrupts: the USB
 *	manager's task, which runs every few milliseconds while sound
 *	streams, reads which block the DMA is on and refills the ones it has
 *	finished with freshly mixed samples. The DMA sees memory through the
 *	same window as the other RP1 bus masters, at the physical address
 *	plus RP1_DMA_BUS_OFFSET, and I2S0's FIFO at the RP1's own address
 *	for it.
 *
 *	Only playback; the data in pin is not used.
 *
 *	NOT VERIFIED ON HARDWARE. No board has played through this code
 *	yet; the register offsets and bit positions below are the ones to
 *	check first when one does not.
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
#define CLK_I2S_SEL		(CLK_BASE + 0x000c0)

#define PLL_CS_LOCK		0x80000000
#define PLL_CS_REFDIV_1		0x00000001
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
#define I2S_TER0		0x02c
#define I2S_TCR0		0x034
#define I2S_IMR0		0x03c
#define I2S_TFCR0		0x04c
#define I2S_TXDMA		0x1c8
#define I2S_COMP_PARAM_1	0x1f4
#define I2S_COMP_TYPE		0x1fc
#define I2S_DMACR		0x200

#define I2S_TYPE_DW		0x445701a0	/* the DesignWare block's mark */
#define CCR_WSS_32		0x10		/* 32 clocks a slot */
#define TCR_32BIT		5
#define DMACR_TXCH0		0x00000100
#define DMACR_TX		0x00020000

/* ---------------------------------------------------------------- DMA */

#define DMA_BASE		(RP1_WINDOW_BASE + 0x188000)
#define DMAC_CFG		0x010
#define DMAC_CHEN		0x018
#define DMA_CH			7		/* the last of its eight channels */
#define CH_BASE			(DMA_BASE + 0x100 + DMA_CH * 0x100)
#define CH_CTL_L		0x018
#define CH_CTL_H		0x01c
#define CH_CFG_L		0x020
#define CH_CFG_H		0x024
#define CH_LLP_L		0x028
#define CH_LLP_H		0x02c
#define CH_INTSTATUS_ENA	0x080
#define CH_INTCLEAR		0x098

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

LOCAL BOOL	i2s_ok = FALSE;
LOCAL BOOL	running = FALSE;
LOCAL DMABUF	ring;			/* NBLK blocks, two to a page */
LOCAL UW	*lli;			/* the linked blocks, a page */
LOCAL UD	lli_bus;
LOCAL INT	last_k;			/* the DMA's block at the last look */

LOCAL UW rd( UBINT a )
{
	return *(volatile UW *)a;
}

LOCAL void wr( UBINT a, UW v )
{
	*(volatile UW *)a = v;
	Asm("dsb sy" ::: "memory");
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
 * interface sends, the sample in the upper half.
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
}

/* ---------------------------------------------------------------- set-up */

/* The audio PLL at 61.44MHz, left alone when it already is */
LOCAL ER pll_up( void )
{
	UW	prim;
	INT	i;

	if ( (rd(PLL_AUDIO_CS) & PLL_CS_LOCK) != 0
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

/* The ring of linked blocks, the last pointing back at the first */
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

LOCAL void dma_stop( void )
{
	/* the enable bit written as 0, with its write enable */
	wr(DMA_BASE + DMAC_CHEN, (UW)(1U << (8 + DMA_CH)));
}

LOCAL void i2s_stop( void )
{
	wr(I2S_BASE + I2S_DMACR, 0);
	wr(I2S_BASE + I2S_ITER, 0);
	wr(I2S_BASE + I2S_CER, 0);
	wr(I2S_BASE + I2S_IER, 0);
}

/* ---------------------------------------------------------------- backend */

LOCAL BOOL i2s_can_play( void )
{
	return i2s_ok;
}

LOCAL BOOL i2s_can_record( void )
{
	return FALSE;
}

LOCAL ER i2s_play_start( UW *rate, INT *channels )
{
	UW	comp1, depth;
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

	for ( i = 0; i < NBLK; i++ ) {
		blk_fill(i);
	}
	lli_build();
	last_k = 0;

	/* the interface: stopped, then set up for one stereo channel */
	i2s_stop();
	comp1 = rd(I2S_BASE + I2S_COMP_PARAM_1);
	depth = 1U << (1 + ((comp1 >> 2) & 3));
	wr(I2S_BASE + I2S_CCR, CCR_WSS_32);
	wr(I2S_BASE + I2S_TCR0, TCR_32BIT);
	wr(I2S_BASE + I2S_TFCR0, depth / 2 - 1);
	wr(I2S_BASE + I2S_IMR0, 0x33);		/* no interrupts */
	wr(I2S_BASE + I2S_TER0, 1);
	wr(I2S_BASE + I2S_TXFFR, 1);
	wr(I2S_BASE + I2S_DMACR, DMACR_TXCH0 | DMACR_TX);

	/* the DMA channel onto the ring */
	dma_stop();
	wr(DMA_BASE + DMAC_CFG, rd(DMA_BASE + DMAC_CFG) | 1);	/* the controller on */
	wr(CH_BASE + CH_CFG_L, CFGL_MULTBLK_LL | CFGL_DST_PER(DREQ_I2S0_TX));
	wr(CH_BASE + CH_CFG_H, CFGH_TT_MEM_TO_PER | CFGH_PRIORITY(DMA_CH));
	wr(CH_BASE + CH_INTSTATUS_ENA, 0);
	wr(CH_BASE + CH_INTCLEAR, 0xffffffff);
	wr(CH_BASE + CH_LLP_L, (UW)lli_bus);
	wr(CH_BASE + CH_LLP_H, (UW)(lli_bus >> 32));
	wr(DMA_BASE + DMAC_CHEN, (UW)((1U << DMA_CH) | (1U << (8 + DMA_CH))));

	/* and the interface running: it asks the DMA for words from here on */
	wr(I2S_BASE + I2S_IER, 1);
	wr(I2S_BASE + I2S_ITER, 1);
	wr(I2S_BASE + I2S_CER, 1);
	running = TRUE;
	*rate = OUT_RATE;
	*channels = 2;
	tm_printf((UB *)"TessronOS: i2s0 plays at %d Hz\n", OUT_RATE);

	return E_OK;
}

LOCAL void i2s_play_stop( void )
{
	if ( !running ) {
		return;
	}
	i2s_stop();
	dma_stop();
	running = FALSE;
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
 * The blocks the DMA has finished since the last look are filled again.
 * Its linked list pointer names the block it is on or the one it loads
 * next, so the block before that one may still be read and is left for
 * the next look. The looks come far more often than half the ring takes
 * to play, so a distance of more than half the ring is the DMA not yet
 * past the start, not a ring played nearly round.
 */
LOCAL void i2s_poll( void )
{
	UD	llp;
	INT	k, done;

	if ( !running ) {
		return;
	}
	llp = (UD)rd(CH_BASE + CH_LLP_L) | ((UD)rd(CH_BASE + CH_LLP_H) << 32);
	if ( llp < lli_bus || llp >= lli_bus + NBLK * LLI_SIZE ) {
		return;
	}
	k = (INT)((llp - lli_bus) / LLI_SIZE);
	done = (k + NBLK - 1) % NBLK;
	if ( (done - last_k + NBLK) % NBLK > NBLK / 2 ) {
		return;
	}
	while ( last_k != done ) {
		blk_fill(last_k);
		last_k = (last_k + 1) % NBLK;
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
	i2s_can_play,
	i2s_can_record,
	i2s_play_start,
	i2s_play_stop,
	i2s_play_running,
	i2s_rec_start,
	i2s_rec_stop,
	i2s_rec_running,
	i2s_poll,
	i2s_get_level,
	i2s_set_level
};

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
