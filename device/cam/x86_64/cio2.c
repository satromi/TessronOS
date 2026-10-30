/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cio2.c (x64 PC)
 *	The notebook's own camera behind Intel's IPU3 as a camera of the
 *	camera device "cama" (camx.h, include/ts/cam.h): the CSI-2 receiver
 *	CIO2 (PCI 8086:9D32), the sensor the ACPI namespace names for it and
 *	its power controller, and the raw pictures made into NV12 and YUY2
 *	in software (../cam_isp.c). The IPU3's image processor (the ImgU,
 *	8086:1919) is not used.
 *
 *	The sensors: devices of the namespace with the IDs of the sensors
 *	(INT33BE the OV5693 in front, INT347A the OV8865 at the back and
 *	INT347E the OV7251 for the infrared, on the Surface Go 2); only the
 *	OV5693 is driven. A sensor's SSDB (a buffer of 108 bytes) gives the
 *	CSI-2 port of the receiver it is wired to (byte 28), its lanes (29),
 *	how it is turned in the case (84), its input clock (86, 4 bytes) and
 *	the number of its power controller (90); its _CRS its I2C address and
 *	bus; its _DEP its power controller (an INT3472, int3472.c) and its
 *	bus. The sensor is powered once as the system starts, its ID read
 *	and said, and powered off again; it is powered while it streams.
 *
 *	The receiver: four ports (a CSI-2 receiver and a MIPI back end each,
 *	0x400 apart) and 20 DMA channels, of which channel 0 is used. A
 *	channel writes frames through its FBPT, a ring of 32 frame buffers of
 *	8 entries of 16 bytes (one page): the first entry of a buffer has its
 *	control word (VALID, interrupt on completion and on start), the
 *	frame's number and the offset of its data in the first page; the
 *	second the bytes written and the pages of the buffer and the bytes
 *	of its last page that may be written; every entry the page frame
 *	number of a list of pointers (LOP), a page of 1024 page frame numbers
 *	of the buffer's pages. The hardware clears VALID when a frame is in.
 *	The entries of a buffer after its last LOP point to a LOP of a dummy
 *	page. Here the 32 buffers of the ring take 4 frame buffers in turn,
 *	so each frame buffer is written again only 4 frames later, and a
 *	buffer the hardware has finished is given back to it at once; the
 *	newest frame finished is the one made into a picture.
 *
 *	A frame is IPU3's packed 10-bit raw: 25 pixels in 32 bytes, lines of
 *	DIV_ROUND_UP(width, 50) * 64 bytes. The port's receiver is set up
 *	for the lanes and for the sensor's link frequency (the settle and
 *	termination counts of the clock lane and the data lanes, in 1/16 ns:
 *	16 * (A + B * UI) with the UI 1 / (2 * the link frequency), A and B
 *	95 and -8 for the clock lane's settle, 85 and -2 for the data lanes',
 *	0 and 0 for the terminations), its back end for RAW10 (data type
 *	0x2B) of virtual channel 0, the pixel formatter of the port for 10
 *	bits a pixel packed in 64 bytes.
 *
 *	The interrupt is MSI. Its handler acknowledges what came (INT_STS,
 *	and for an output error or an error of the receivers their own
 *	status), keeps the bits and counts and wakes the camera's task, which
 *	gives the finished buffers back, makes the newest into a picture,
 *	sets the exposure the pictures ask for and says, a few times only,
 *	what happened (the first frame and its first pixels, the errors).
 *
 *	Settings (cmdline.txt): "cam=off" leaves the cameras out; "cam=bars"
 *	has the sensor give its colour bars; "cam_bayer=rggb" (grbg, gbrg,
 *	bggr) another order of the colours.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/syslib.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include <ts/cam.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "../../../kernel/sysdepend/acpi/acpi.h"
#include "../cam_isp.h"
#include "camx.h"

IMPORT void	knl_int_count_at( UINT intno, volatile UW *cnt );	/* kernel/tkernel/inttarget.c */

#define CIO2_DEVICE		0x9d32
#define CIO2_MMIO_SIZE		0x10000

/* a port's: its receiver, its MIPI back end, its interrupts */
#define PIPE(n)			( (n) * 0x400 )
#define CSIRX_ENABLE		0x000
#define CSIRX_NLANES		0x004
#define CSIRX_STATUS		0x018
#define CSIRX_DLANE_HS		0x01c
#define CSIRX_DLANE_LP		0x020
#define CSIRX_TERMEN(l)		( 0x02c + 8 * (l) )	/* l -1 for the clock lane */
#define CSIRX_SETTLE(l)		( 0x030 + 8 * (l) )
#define MIPIBE_ENABLE		0x100
#define MIPIBE_STATUS		0x104
#define MIPIBE_COMP_FORMAT(v)	( 0x108 + 4 * (v) )
#define MIPIBE_FORCE_RAW8	0x120
#define MIPIBE_LUT_DISREGARD	0x168
#define MIPIBE_SP_LUT(v)	( 0x174 + 4 * (v) )
#define MIPIBE_LP_LUT(m)	( 0x184 + 4 * (m) )
#define LP_LUT_DISREGARD	0x1
#define LP_LUT_SID_SHIFT	1
#define LP_LUT_VC_SHIFT		5
#define LP_LUT_TYPE_SHIFT	7
#define IRQCTRL_EDGE		0x300
#define IRQCTRL_MASK		0x304
#define IRQCTRL_STATUS		0x308
#define IRQCTRL_CLEAR		0x30c
#define IRQCTRL_ENABLE		0x310
#define IRQCTRL_LEVEL_NOT_PULSE	0x314
#define IRQCTRL_ALL		0x3ffff

/* the whole receiver's */
#define FB_HPLL_FREQ		0x1008
#define ISCLK_RATIO		0x100c
#define CGC			0x1400
#define D0I3C			0x1408
#define D0I3C_RR		0x8
#define INT_STS			0x1414
#define INT_STS_EXT_OE		0x1418
#define INT_EN			0x1420
#define INT_EN_EXT_OE		0x1424
#define PBM_ARB_CTRL		0x1460
#define PBM_WMCTRL1		0x1464
#define PBM_WMCTRL2		0x1468
#define PBM_FOPN_ABORT		0x1474
#define LTRCTRL			0x1480
#define LTRVAL23		0x1484
#define LTRVAL01		0x1488
#define CDMABA(n)		( 0x1500 + 0x10 * (n) )
#define CDMARI(n)		( 0x1504 + 0x10 * (n) )
#define CDMAC0(n)		( 0x1508 + 0x10 * (n) )
#define CDMAC1(n)		( 0x150c + 0x10 * (n) )
#define PXM_PXF_FMT_CFG0(n)	( 0x1700 + 0x30 * (n) )
#define PXM_FRF_CFG(n)		( 0x1720 + 0x30 * (n) )
#define PXM_SID2BID0(n)		( 0x1724 + 0x30 * (n) )
#define INT_STS_EXT_IE		0x17e4
#define INT_EN_EXT_IE		0x17e8

#define INT_IOC(d)		( 1U << ( (d) < 4 ? (d) : ( (d) >> 1 ) + 2 ) )
#define INT_IOC_MASK		0x7ff
#define INT_IOS(d)		( 1U << ( ( (d) >> 1 ) + 12 ) )
#define INT_IOS_MASK		( 0x3ffU << 12 )
#define INT_IOIE		( 1U << 22 )
#define INT_IOOE		( 1U << 23 )
#define INT_IOIRQ		( 1U << 24 )
#define EXT_IE_IRQ(p)		( 0x80U << ( 8 * (p) ) )

#define CDMAC0_LEN_SHIFT	0
#define CDMAC0_WIDTH_SHIFT	8
#define CDMAC0_INTR_ON_FS	( 1U << 26 )
#define CDMAC0_INTR_ON_FE	( 1U << 27 )
#define CDMAC0_UPDATE_FIFO_FULL	( 1U << 28 )
#define CDMAC0_DMA_EN		( 1U << 30 )
#define CDMAC0_DMA_HALTED	( 1U << 31 )
#define CDMAC1_LINENUMUPDATE	( 1U << 16 )

#define PXF_BPP_10		( 1U << 2 )
#define PXF_PCK_64B		0
#define FRF_CRC_TH		( 16U << 3 )
#define FRF_MSK_ECC_DPHY_NR	( 1U << 8 )
#define FRF_MSK_ECC_RE		( 1U << 9 )
#define FRF_MSK_ECC_DPHY_NE	( 1U << 10 )
#define FRF_ABORT		( 1U << 2 )
#define FOPN_ABORT(p)		( 0x1U << ( 8 * (p) ) )

#define CGC_PRIM_TGE		( 1U << 1 )
#define CGC_SIDE_TGE		( 1U << 2 )
#define CGC_XOSC_TGE		( 1U << 3 )
#define CGC_D3I3_TGE		( 1U << 5 )
#define CGC_CSI2_INTERFRAME_TGE	( 1U << 6 )
#define CGC_CSI2_PORT_DCGE	( 1U << 8 )
#define CGC_SIDE_DCGE		( 1U << 10 )
#define CGC_PRIM_DCGE		( 1U << 11 )
#define CGC_ROSC_DCGE		( 1U << 12 )
#define CGC_XOSC_DCGE		( 1U << 13 )
#define CGC_HOLDOFF		( 3U << 20 )
#define CGC_CSI_HOLDOFF		( 5U << 24 )

#define MIPI_RAW10		0x2b

/* the ring */
#define NSLOT			32		/* frame buffers of the FBPT */
#define NLOPS			8		/* its entries for each */
#define LOP_ENTRIES		1024
#define NBUF			4		/* the frame buffers behind them */
#define BUF_ORDER		9		/* 2 MB each, enough for a frame of the mode */
#define FBPT_VALID		0x1
#define FBPT_IOC		0x2
#define FBPT_IOS		0x4
#define DMA_CHAN		0

#define FLG_FRAME		0x1

/* the formats and sizes offered: all made from the sensor's one mode */
#define NMODES			6
LOCAL CONST struct { UB fmt; UH w, h; } offer[NMODES] = {
	{ CAMFMT_NV12, 640, 480 },  { CAMFMT_YUY2, 640, 480 },
	{ CAMFMT_NV12, 1280, 720 }, { CAMFMT_YUY2, 1280, 720 },
	{ CAMFMT_NV12, 1280, 960 }, { CAMFMT_YUY2, 1280, 960 },
};

/* the sensors the namespace may name */
LOCAL CONST struct { CONST char *hid; CONST char *model; BOOL driven; } sensors[] = {
	{ "INT33BE", "OV5693", TRUE },
	{ "INT347A", "OV8865", FALSE },
	{ "INT347E", "OV7251", FALSE },
};

typedef struct {
	char		path[48];
	CONST char	*model;
	UB		ssdb[108];
	INT		ssdb_len;
	UB		port, lanes, degree, clid;
	UW		mclk;
	unsigned int	addr, hz;
	char		ctrl[48];
	T_CAMPWR	pwr;
	T_OV5693	ov;
} SENSOR;

typedef struct {
	T_PCIDEV	pci;
	UBINT		base;
	UINT		intno;
	INT		irqkind;
	SENSOR		sen;
	T_CAMSRC	src;
	T_CAMMODE	modes[NMODES];
	INT		mode;
	BOOL		streaming;
	ID		tsk, flg, mtx;
	/* the DMA's memory */
	volatile UW	*fbpt;
	UD		fbpt_pa;
	UD		dummy_pa, dummy_lop_pa;
	UW		*dummy_lop;
	UB		*buf[NBUF];
	UD		buf_pa[NBUF];
	UW		*lop[NBUF];
	UD		lop_pa[NBUF];
	INT		bpl, flen, npages;
	INT		first;		/* the slot the hardware fills next, as far as the task knows */
	/* what the interrupt saw */
	volatile UW	nint, nioc, nios, noe, nie;
	volatile UW	oe, ie, csi[4];
	/* the pictures */
	T_CAMISP	*isp;
	UB		*out;
	UW		frames, made, said_err, said_none;
	UW		ae_frames;
	SYSTIM		t0;
} CIO2;

LOCAL CIO2	*cio2 = NULL;

LOCAL UW  rd( CIO2 *c, UW o ) { return in_w(c->base + o); }
LOCAL void wr( CIO2 *c, UW o, UW v ) { out_w(c->base + o, v); }

/* ---------------------------------------------------------------- settings */

/* The value of a word key=value of the start-up settings into v: FALSE when there is none */
LOCAL BOOL setting( CONST char *key, char *v, INT max )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k, n;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; key[k] != '\0' && c[k] == key[k]; k++ ) ;
		if ( key[k] != '\0' || c[k] != '=' ) continue;
		for ( n = 0, c += k + 1; *c != '\0' && *c != ' ' && n < max - 1; n++ ) v[n] = *c++;
		v[n] = '\0';
		return TRUE;
	}
	return FALSE;
}

LOCAL BOOL setting_is( CONST char *key, CONST char *want )
{
	char	v[16];

	return setting(key, v, sizeof(v)) && knl_strcmp(v, want) == 0;
}

/* ---------------------------------------------------------------- the interrupt */

LOCAL void cio2_inthdr( UINT intno )
{
	CIO2	*c = cio2;
	UW	st;
	INT	n, p;

	if ( c != NULL && c->intno == intno ) {
		for ( n = 0, st = rd(c, INT_STS); st != 0 && n < 8; n++, st = rd(c, INT_STS) ) {
			wr(c, INT_STS, st);
			if ( st & INT_IOOE ) {
				UW	oe = rd(c, INT_STS_EXT_OE);

				wr(c, INT_STS_EXT_OE, oe);
				c->oe |= oe;
				c->noe++;
			}
			if ( st & INT_IOC_MASK ) c->nioc++;
			if ( st & INT_IOS_MASK ) c->nios++;
			if ( st & ( INT_IOIE | INT_IOIRQ ) ) {
				UW	ie = rd(c, INT_STS_EXT_IE);

				for ( p = 0; p < 4; p++ ) {
					if ( ie & EXT_IE_IRQ(p) ) {
						UW	s = rd(c, PIPE(p) + IRQCTRL_STATUS);

						wr(c, PIPE(p) + IRQCTRL_CLEAR, s);
						c->csi[p] |= s;
					}
				}
				wr(c, INT_STS_EXT_IE, ie);
				c->ie |= ie;
				c->nie++;
			}
		}
		c->nint++;
		tk_set_flg(c->flg, FLG_FRAME);
	}
	EndOfInt(intno);
}

/* ---------------------------------------------------------------- the ring */

LOCAL void *dma_pages( UINT order, UD *pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) return NULL;
	*pa = knl_pf_to_pa(pf);
	return PA2VA(*pa);
}

/* The memory of the ring, made the first time a stream starts and kept */
LOCAL ER ring_alloc( CIO2 *c )
{
	INT	i, k;
	void	*dummy;

	if ( c->fbpt != NULL ) return E_OK;
	c->bpl = cam_isp_ipu3_bpl(OV5693_WIDTH);
	c->flen = c->bpl * OV5693_HEIGHT;
	c->npages = ( c->flen + 4095 ) / 4096;
	if ( c->npages > LOP_ENTRIES - 1 || c->flen > ( 4096 << BUF_ORDER ) ) return E_SYS;
	dummy = dma_pages(0, &c->dummy_pa);
	c->dummy_lop = (UW *)dma_pages(0, &c->dummy_lop_pa);
	c->fbpt = (volatile UW *)dma_pages(0, &c->fbpt_pa);
	if ( dummy == NULL || c->dummy_lop == NULL || c->fbpt == NULL ) return E_NOMEM;
	for ( k = 0; k < LOP_ENTRIES; k++ ) c->dummy_lop[k] = (UW)( c->dummy_pa >> 12 );
	for ( i = 0; i < NBUF; i++ ) {
		c->buf[i] = (UB *)dma_pages(BUF_ORDER, &c->buf_pa[i]);
		c->lop[i] = (UW *)dma_pages(0, &c->lop_pa[i]);
		if ( c->buf[i] == NULL || c->lop[i] == NULL ) return E_NOMEM;
		/* the buffer's pages, then the dummy page for whatever the hardware reaches past them */
		for ( k = 0; k < LOP_ENTRIES; k++ ) {
			c->lop[i][k] = (UW)( ( ( k < c->npages ) ? c->buf_pa[i] + (UD)k * 4096 : c->dummy_pa ) >> 12 );
		}
	}
	if ( ( c->isp = (T_CAMISP *)Kmalloc(sizeof(T_CAMISP)) ) == NULL ) return E_NOMEM;
	if ( ( c->out = (UB *)Kmalloc(1280 * 960 * 2) ) == NULL ) return E_NOMEM;
	tm_printf((UB *)"cio2: ring: FBPT at %lx, %d slots over %d buffers of %d bytes (%d pages, lines of %d bytes), "
		  "buffer 0 at %lx\n", c->fbpt_pa, NSLOT, NBUF, c->flen, c->npages, c->bpl, c->buf_pa[0]);
	return E_OK;
}

/* The slot's entries set for its buffer and given to the hardware */
LOCAL void slot_arm( CIO2 *c, INT s )
{
	volatile UW	*e = c->fbpt + s * NLOPS * 4;
	INT		b = s % NBUF, k, last = c->flen % 4096;

	e[0] = 0;
	e[1] = 0;
	e[2] = 0;				/* the data from the start of the first page */
	e[3] = (UW)( c->lop_pa[b] >> 12 );
	e[4] = 0;
	e[5] = 0;
	e[6] = (UW)( ( ( last != 0 ) ? last : 4096 ) - 1 ) | ( (UW)c->npages << 16 );
	e[7] = (UW)( c->dummy_lop_pa >> 12 );	/* the first entry past the buffer's LOP: a valid LOP */
	for ( k = 2; k < NLOPS; k++ ) e[k * 4 + 3] = (UW)( c->dummy_lop_pa >> 12 );
	__sync_synchronize();
	e[0] = FBPT_VALID | FBPT_IOC | FBPT_IOS;
}

/* ---------------------------------------------------------------- the receiver */

/* A count of the receiver's timing in 1/16 ns: 16 * (a + b * UI) for the link frequency hz */
LOCAL W rx_timing( W a, W b, UW hz )
{
	D	r = (D)16 * b * 500000000LL;

	return (W)( r / (D)hz + 16 * a );
}

LOCAL void hw_init( CIO2 *c )
{
	UW	pb = PIPE(c->sen.port);
	INT	i;
	UW	hz = OV5693_LINK_HZ;
	W	clk_settle = rx_timing(95, -8, hz), dat_settle = rx_timing(85, -2, hz);

	wr(c, pb + CSIRX_TERMEN(-1), (UW)rx_timing(0, 0, hz));
	wr(c, pb + CSIRX_SETTLE(-1), (UW)clk_settle);
	for ( i = 0; i < c->sen.lanes; i++ ) {
		wr(c, pb + CSIRX_TERMEN(i), (UW)rx_timing(0, 0, hz));
		wr(c, pb + CSIRX_SETTLE(i), (UW)dat_settle);
	}
	wr(c, PBM_WMCTRL1, ( 4U << 0 ) | ( 16U << 8 ) | ( 21U << 16 ));
	wr(c, PBM_WMCTRL2, ( 40U << 0 ) | ( 22U << 8 ) | ( 2U << 16 ) | ( 1U << 24 ) | ( 1U << 29 ));
	wr(c, PBM_ARB_CTRL, ( 0U << 0 ) | ( 1U << 7 ) | ( 2U << 8 ) | ( 480U << 16 ));
	wr(c, pb + CSIRX_DLANE_HS, 0xff);
	wr(c, pb + CSIRX_DLANE_LP, 0xffffff);
	wr(c, FB_HPLL_FREQ, 0x2);
	wr(c, ISCLK_RATIO, 0xc);

	/* the back end: short packets of the four channels passed, long ones disregarded but RAW10 of channel 0 */
	for ( i = 0; i < 4; i++ ) wr(c, pb + MIPIBE_SP_LUT(i), 1);
	for ( i = 0; i < 16; i++ ) wr(c, pb + MIPIBE_LP_LUT(i), LP_LUT_DISREGARD);
	wr(c, pb + MIPIBE_LUT_DISREGARD, 1);

	wr(c, INT_EN_EXT_IE, 0xffffffff);
	wr(c, pb + IRQCTRL_MASK, IRQCTRL_ALL);
	wr(c, pb + IRQCTRL_ENABLE, IRQCTRL_ALL);
	wr(c, pb + IRQCTRL_EDGE, 0);
	wr(c, pb + IRQCTRL_LEVEL_NOT_PULSE, 0);
	wr(c, INT_EN_EXT_OE, 0x8f0fffff);
	wr(c, INT_EN, INT_IOIRQ | INT_IOC(DMA_CHAN) | INT_IOS(DMA_CHAN));

	wr(c, PXM_PXF_FMT_CFG0(c->sen.port), PXF_BPP_10 | PXF_PCK_64B);
	wr(c, pb + MIPIBE_LP_LUT(0), ( 0U << LP_LUT_SID_SHIFT ) | ( 0U << LP_LUT_VC_SHIFT )
				     | ( (UW)MIPI_RAW10 << LP_LUT_TYPE_SHIFT ));
	wr(c, pb + MIPIBE_COMP_FORMAT(0), 0);
	wr(c, pb + MIPIBE_FORCE_RAW8, 0);
	wr(c, PXM_SID2BID0(c->sen.port), 0);

	wr(c, pb + CSIRX_NLANES, c->sen.lanes);
	wr(c, CGC, CGC_PRIM_TGE | CGC_SIDE_TGE | CGC_XOSC_TGE | CGC_D3I3_TGE | CGC_CSI2_INTERFRAME_TGE
		   | CGC_CSI2_PORT_DCGE | CGC_SIDE_DCGE | CGC_PRIM_DCGE | CGC_ROSC_DCGE | CGC_XOSC_DCGE
		   | CGC_HOLDOFF | CGC_CSI_HOLDOFF);
	wr(c, LTRCTRL, 1U << 16);
	wr(c, LTRVAL01, ( 175U << 0 ) | ( 2U << 10 ) | ( 90U << 16 ) | ( 2U << 26 ));
	wr(c, LTRVAL23, ( 90U << 0 ) | ( 2U << 10 ) | ( 90U << 16 ) | ( 2U << 26 ));

	for ( i = 0; i < 20; i++ ) {
		wr(c, CDMABA(i), 0);
		wr(c, CDMAC0(i), 0);
		wr(c, CDMAC1(i), 0);
	}
	for ( i = 0; i < NSLOT; i++ ) slot_arm(c, i);
	c->first = 0;
	wr(c, CDMABA(DMA_CHAN), (UW)( c->fbpt_pa >> 12 ));
	wr(c, CDMAC0(DMA_CHAN), ( (UW)( NSLOT - 1 ) << CDMAC0_LEN_SHIFT ) | ( ( NLOPS / 4U ) << CDMAC0_WIDTH_SHIFT )
				| CDMAC0_INTR_ON_FE | CDMAC0_UPDATE_FIFO_FULL | CDMAC0_DMA_EN | CDMAC0_INTR_ON_FS
				| CDMAC0_DMA_HALTED);
	wr(c, CDMAC1(DMA_CHAN), CDMAC1_LINENUMUPDATE);
	wr(c, PBM_FOPN_ABORT, 0);
	wr(c, PXM_FRF_CFG(c->sen.port), FRF_CRC_TH | FRF_MSK_ECC_DPHY_NR | FRF_MSK_ECC_RE | FRF_MSK_ECC_DPHY_NE);

	wr(c, pb + IRQCTRL_CLEAR, IRQCTRL_ALL);
	wr(c, INT_STS_EXT_OE, 0xffffffff);
	wr(c, INT_STS_EXT_IE, 0xffffffff);
	wr(c, INT_STS, 0xffffffff);
	/* the back end before the receiver in front of it */
	wr(c, pb + MIPIBE_ENABLE, 1);
	wr(c, pb + CSIRX_ENABLE, 1);
	tm_printf((UB *)"cio2: port %d, %d lanes, link %d Hz: settle clock %d data %d (1/16 ns), RAW10 to DMA %d, "
		  "CDMAC0 %08x\n", c->sen.port, c->sen.lanes, hz, clk_settle, dat_settle, DMA_CHAN, rd(c, CDMAC0(DMA_CHAN)));
}

LOCAL void hw_exit( CIO2 *c )
{
	UW	pb = PIPE(c->sen.port);
	INT	i;

	wr(c, pb + IRQCTRL_MASK, 0);
	wr(c, pb + IRQCTRL_ENABLE, 0);
	wr(c, pb + CSIRX_ENABLE, 0);
	wr(c, pb + MIPIBE_ENABLE, 0);
	wr(c, CDMAC0(DMA_CHAN), 0);
	for ( i = 0; i < 500 && ( rd(c, CDMAC0(DMA_CHAN)) & CDMAC0_DMA_HALTED ) == 0; i++ ) tk_dly_tsk(4);
	if ( i == 500 ) tm_printf((UB *)"cio2: DMA %d did not halt\n", DMA_CHAN);
	for ( i = 0; i < 4; i++ ) {
		wr(c, PXM_FRF_CFG(i), rd(c, PXM_FRF_CFG(i)) | FRF_ABORT);
		wr(c, PBM_FOPN_ABORT, rd(c, PBM_FOPN_ABORT) | FOPN_ABORT(i));
	}
	wr(c, INT_EN, 0);
}

/* The receiver's state said: what to look at when no frame comes */
LOCAL void say_state( CIO2 *c, CONST char *when )
{
	UW	pb = PIPE(c->sen.port);

	tm_printf((UB *)"cio2: %s: interrupts %d (completed %d, started %d, output errors %d, receiver errors %d); "
		  "INT_STS %08x EXT_OE %08x EXT_IE %08x; port %d: CSIRX status %08x lanes HS %02x LP %06x, "
		  "MIPI back end %08x, IRQ status %05x; CDMAC0 %08x CDMARI %08x FOPN %08x\n",
		  when, c->nint, c->nioc, c->nios, c->noe, c->nie, rd(c, INT_STS), rd(c, INT_STS_EXT_OE),
		  rd(c, INT_STS_EXT_IE), c->sen.port, rd(c, pb + CSIRX_STATUS), rd(c, pb + CSIRX_DLANE_HS),
		  rd(c, pb + CSIRX_DLANE_LP), rd(c, pb + MIPIBE_STATUS), rd(c, pb + IRQCTRL_STATUS),
		  rd(c, CDMAC0(DMA_CHAN)), rd(c, CDMARI(DMA_CHAN)), rd(c, PBM_FOPN_ABORT));
}

/* ---------------------------------------------------------------- the pictures */

/* The exposure the pictures ask for: lines first, then gain (scale: 256 for as now) */
LOCAL void exposure_to( CIO2 *c, UW scale )
{
	T_OV5693	*s = &c->sen.ov;
	UD		total = (UD)s->exposure * s->gain * scale / 256;
	UW		maxe = knl_ov5693_max_exposure(s), e, g;

	if ( total < 4 * 8 ) total = 4 * 8;
	if ( total / 8 <= maxe ) {
		e = (UW)( total / 8 );
		g = 8;
	} else {
		e = maxe;
		g = (UW)( ( total + e - 1 ) / e );
	}
	/* a change of less than 1/32 is left, as is any while the last one may not have taken effect */
	if ( c->ae_frames < 3 ) return;
	if ( (UD)e * g * 32 > (UD)s->exposure * s->gain * 31 && (UD)e * g * 31 < (UD)s->exposure * s->gain * 32 ) return;
	(void)knl_ov5693_exposure(s, e, g);
	c->ae_frames = 0;
}

LOCAL void say_frame( CIO2 *c, INT slot, UW bytes, UW fnum )
{
	CONST UB	*raw = c->buf[slot % NBUF];
	UH		px[8];
	UD		n = c->isp->ncell ? c->isp->ncell : 1;
	SYSTIM		t;
	UD		ms;

	cam_isp_ipu3_unpack(raw, px, 8);
	tk_get_tim(&t);
	ms = ( ( (UD)t.hi << 32 ) | t.lo ) - ( ( (UD)c->t0.hi << 32 ) | c->t0.lo );
	tm_printf((UB *)"cio2: frame %d after %d ms: slot %d, %d bytes (%d expected), its number %d; first pixels "
		  "%d %d %d %d %d %d %d %d; means R %d G %d B %d (black %d), saturated %d of %d; exposure %d gain %d, "
		  "white balance %d %d %d\n", c->frames, (INT)ms, slot, bytes, c->flen, fnum,
		  px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7],
		  (INT)( c->isp->sum[0] / n ), (INT)( c->isp->sum[1] / n ), (INT)( c->isp->sum[2] / n ), c->isp->black,
		  c->isp->nsat, c->isp->ncell, c->sen.ov.exposure, c->sen.ov.gain, c->isp->wb[0], c->isp->wb[1], c->isp->wb[2]);
}

/* What the interrupt kept, said when it changed (eight times at most a stream) */
LOCAL void say_errors( CIO2 *c )
{
	UW		oe = c->oe, ie = c->ie, csi = c->csi[c->sen.port];
	INT		p;

	if ( oe == 0 && ie == 0 && csi == 0 ) return;
	c->oe = c->ie = 0;
	for ( p = 0; p < 4; p++ ) c->csi[p] = 0;
	if ( c->said_err++ >= 8 ) return;
	tm_printf((UB *)"cio2: errors at frame %d: DMA output %08x, receivers %08x (port %d: ECC %d, D-PHY %d, CRC %d, "
		  "inter-frame data %d, packet short %d long %d), port %d IRQ %05x\n", c->frames, oe, ie, c->sen.port,
		  ( ie >> ( 8 * c->sen.port ) ) & 0x05 ? 1 : 0, ( ie >> ( 8 * c->sen.port ) ) & 0x02 ? 1 : 0,
		  ( ie >> ( 8 * c->sen.port ) ) & 0x08 ? 1 : 0, ( ie >> ( 8 * c->sen.port ) ) & 0x10 ? 1 : 0,
		  ( ie >> ( 8 * c->sen.port ) ) & 0x20 ? 1 : 0, ( ie >> ( 8 * c->sen.port ) ) & 0x40 ? 1 : 0,
		  c->sen.port, csi);
}

/* The finished slots given back; the newest made into a picture */
LOCAL void frames_in( CIO2 *c )
{
	INT	n, newest = -1;
	UW	bytes = 0, fnum = 0;
	CONST T_CAMMODE	*m;
	INT	len;

	for ( n = 0; n < NSLOT; n++ ) {
		volatile UW	*e = c->fbpt + c->first * NLOPS * 4;

		if ( e[0] & FBPT_VALID ) break;
		newest = c->first;
		fnum = e[1] >> 16;
		bytes = e[5];
		c->frames++;
		slot_arm(c, c->first);
		c->first = ( c->first + 1 ) % NSLOT;
	}
	if ( newest < 0 || c->mode < 0 ) return;
	m = &c->modes[c->mode];
	len = cam_isp_frame(c->isp, c->buf[newest % NBUF], c->bpl, OV5693_WIDTH, OV5693_HEIGHT, c->out, m->format,
			    m->width, m->height);
	if ( len > 0 ) {
		knl_cam_frame(&c->src, c->out, len, 0);
		c->made++;
	}
	c->ae_frames++;
	exposure_to(c, cam_isp_auto(c->isp));
	if ( c->made == 1 || c->made == 30 || c->made == 300 ) say_frame(c, newest, bytes, fnum);
}

LOCAL void cio2_task( INT stacd, void *exinf )
{
	CIO2	*c = (CIO2 *)exinf;
	UINT	ptn;
	ER	er;

	for ( ;; ) {
		er = tk_wai_flg(c->flg, FLG_FRAME, TWF_ORW | TWF_BITCLR, &ptn, 1000);
		tk_loc_mtx(c->mtx, TMO_FEVR);
		if ( c->streaming ) {
			if ( er == E_TMOUT && c->frames == 0 && c->said_none < 2 ) {
				c->said_none++;
				say_state(c, c->said_none == 1 ? "no frame in a second" : "still no frame");
				knl_ov5693_say(&c->sen.ov, "no frame");
			}
			frames_in(c);
			say_errors(c);
		}
		tk_unl_mtx(c->mtx);
	}
}

/* ---------------------------------------------------------------- the camera */

LOCAL ER cio2_set_mode( T_CAMSRC *s, T_CAMSETMODE *m )
{
	CIO2	*c = (CIO2 *)s->priv;

	c->mode = m->mode;
	m->altSetting = 0;
	m->maxPayload = s->modes[m->mode].maxBytes;
	m->maxFrame = s->modes[m->mode].maxBytes;
	return E_OK;
}

LOCAL ER cio2_start( T_CAMSRC *s )
{
	CIO2	*c = (CIO2 *)s->priv;
	char	v[8];
	INT	bayer = ISP_BGGR;
	ER	er;

	tk_loc_mtx(c->mtx, TMO_FEVR);
	er = ring_alloc(c);
	if ( er < E_OK ) {
		tm_printf((UB *)"cio2: no memory for the ring (%d)\n", (INT)er);
		tk_unl_mtx(c->mtx);
		return er;
	}
	er = knl_campwr_on(&c->sen.pwr);
	if ( er >= E_OK ) er = knl_ov5693_setup(&c->sen.ov);
	if ( er < E_OK ) {
		tm_printf((UB *)"cio2: the sensor does not start (%d)\n", (INT)er);
		knl_campwr_off(&c->sen.pwr);
		tk_unl_mtx(c->mtx);
		return E_IO;
	}
	if ( setting("cam_bayer", v, sizeof(v)) ) {
		bayer = !knl_strcmp(v, "rggb") ? ISP_RGGB : !knl_strcmp(v, "grbg") ? ISP_GRBG
		      : !knl_strcmp(v, "gbrg") ? ISP_GBRG : ISP_BGGR;
	}
	cam_isp_init(c->isp, bayer, OV5693_BLACK);
	c->nint = c->nioc = c->nios = c->noe = c->nie = 0;
	c->oe = c->ie = c->csi[0] = c->csi[1] = c->csi[2] = c->csi[3] = 0;
	c->frames = c->made = c->said_err = c->said_none = c->ae_frames = 0;
	(void)tk_clr_flg(c->flg, 0);
	tk_get_tim(&c->t0);
	hw_init(c);
	er = knl_ov5693_stream(&c->sen.ov, TRUE);
	c->streaming = TRUE;
	knl_ov5693_say(&c->sen.ov, "streaming");
	tm_printf((UB *)"cio2: streaming %dx%d %s from the sensor's %dx%d (stream on: %d)\n",
		  c->modes[c->mode].width, c->modes[c->mode].height,
		  c->modes[c->mode].format == CAMFMT_NV12 ? "NV12" : "YUY2", OV5693_WIDTH, OV5693_HEIGHT, (INT)er);
	tk_unl_mtx(c->mtx);
	return E_OK;
}

LOCAL void cio2_stop( T_CAMSRC *s )
{
	CIO2	*c = (CIO2 *)s->priv;

	tk_loc_mtx(c->mtx, TMO_FEVR);
	if ( c->streaming ) {
		c->streaming = FALSE;
		(void)knl_ov5693_stream(&c->sen.ov, FALSE);
		hw_exit(c);
		knl_campwr_off(&c->sen.pwr);
		tm_printf((UB *)"cio2: stopped: %d frames in, %d pictures made; interrupts %d (completed %d, started %d, "
			  "output errors %d, receiver errors %d)\n", c->frames, c->made, c->nint, c->nioc, c->nios,
			  c->noe, c->nie);
	}
	tk_unl_mtx(c->mtx);
}

/* ---------------------------------------------------------------- finding it all */

/* The receiver on the PCI bus, set up: FALSE when there is none */
LOCAL BOOL find_cio2( CIO2 *c )
{
	INT	i, n = ts_pcie_count();
	UINT	pm;

	for ( i = 0; i < n; i++ ) {
		if ( ts_pcie_get(i, &c->pci) == E_OK && c->pci.vendor == 0x8086 && c->pci.device == CIO2_DEVICE ) break;
	}
	if ( i == n ) return FALSE;
	pm = ts_pcie_cap_find(&c->pci, 0x01);
	if ( pm != 0 ) {
		UW	cs = ts_pcie_cfg_read(c->pci.bus, c->pci.dev, c->pci.fn, pm + 4, 2);

		if ( cs & 3 ) {
			ts_pcie_cfg_write(c->pci.bus, c->pci.dev, c->pci.fn, pm + 4, cs & ~(UW)0x8003, 2);
			tk_dly_tsk(10);
		}
	}
	ts_pcie_enable(&c->pci);
	c->base = ts_pcie_bar_base(&c->pci, 0);
	if ( c->base == 0 || c->pci.bar_size[0] < CIO2_MMIO_SIZE ) {
		tm_printf((UB *)"cio2: no memory window\n");
		return FALSE;
	}
	wr(c, D0I3C, D0I3C_RR);
	c->irqkind = ts_pcie_irq_alloc(&c->pci, &c->intno, FALSE);
	if ( ( c->irqkind == PCI_IRQ_MSIX || c->irqkind == PCI_IRQ_MSI ) && c->intno != 0 ) {
		knl_define_inthdr((INT)c->intno, TA_HLNG, (FP)cio2_inthdr);
		EnableInt(c->intno, INTPRI_DEVICE);
		knl_int_owner(c->intno, (UB *)"cama");
		knl_int_count_at(c->intno, &c->nint);
	} else {
		tm_printf((UB *)"cio2: no message interrupt (%d)\n", c->irqkind);
		return FALSE;
	}
	tm_printf((UB *)"TessronOS: cio2 8086:%04x at %d:%d.%d, registers at %lx, %s %d, D0i3 %x\n", c->pci.device,
		  c->pci.bus, c->pci.dev, c->pci.fn, (UD)c->pci.bar[0], ts_pcie_irq_name(c->irqkind), c->intno,
		  rd(c, D0I3C));
	return TRUE;
}

/* A sensor of the namespace: its SSDB, its I2C connection and its power controller read and said */
LOCAL ER read_sensor( SENSOR *s, void *dev, CONST char *model )
{
	char			dep[48], line[3 * 36 + 1];
	unsigned long long	adr = 0;
	INT			i, m;

	knl_memset(s, 0, sizeof(*s));
	s->model = model;
	(void)knl_acpica_path(dev, s->path, sizeof(s->path));
	s->ssdb_len = knl_acpica_buf(dev, "SSDB", s->ssdb, sizeof(s->ssdb));
	for ( i = 0; i < 3; i++ ) {
		INT	k;

		for ( k = i * 36, m = 0, line[0] = '\0'; k < i * 36 + 36 && k < s->ssdb_len; k++ ) {
			m += tm_sprintf((UB *)line + m, (UB *)" %02x", s->ssdb[k]);
		}
		tm_printf((UB *)"cio2: %s %s SSDB %d-%d (%d bytes):%s\n", s->path, model, i * 36, i * 36 + 35,
			  s->ssdb_len, line);
	}
	if ( s->ssdb_len < 95 ) {
		tm_printf((UB *)"cio2: %s: no SSDB (%d)\n", s->path, s->ssdb_len);
		return E_NOEXS;
	}
	s->port = s->ssdb[28];
	s->lanes = s->ssdb[29];
	s->degree = s->ssdb[84];
	s->mclk = (UW)s->ssdb[86] | ( (UW)s->ssdb[87] << 8 ) | ( (UW)s->ssdb[88] << 16 ) | ( (UW)s->ssdb[89] << 24 );
	s->clid = s->ssdb[90];
	tm_printf((UB *)"cio2: %s %s: version %d, SKU %d, CSI-2 port %d, %d lanes, highest lane rate %d, "
		  "input clock %d Hz (port %d), turned %d, power controller %d, ROM %d, VCM %d, flash %d, privacy LED %d, "
		  "platform %d.%d\n",
		  s->path, model, s->ssdb[0], s->ssdb[1], s->port, s->lanes,
		  (INT)( s->ssdb[70] | ( s->ssdb[71] << 8 ) | ( s->ssdb[72] << 16 ) | ( (UW)s->ssdb[73] << 24 ) ),
		  s->mclk, s->ssdb[94], s->degree, s->clid, s->ssdb[78], s->ssdb[79], s->ssdb[82], s->ssdb[83],
		  s->ssdb[80], s->ssdb[81]);
	if ( knl_acpica_i2c(dev, &s->addr, &s->hz, s->ctrl, sizeof(s->ctrl), NULL, NULL, 0, NULL, NULL) != 0 ) {
		tm_printf((UB *)"cio2: %s: no I2C connection\n", s->path);
		return E_NOEXS;
	}
	(void)knl_acpica_int(knl_acpica_handle(s->ctrl), "_ADR", &adr);
	tm_printf((UB *)"cio2: %s: I2C %02x on %s (%x), %d Hz\n", s->path, s->addr, s->ctrl, (UW)adr, s->hz);
	for ( i = 0; i < 4 && knl_acpica_dep(dev, i, dep, sizeof(dep)) == 0; i++ ) {
		tm_printf((UB *)"cio2: %s: _DEP %d: %s\n", s->path, i, dep);
		if ( s->pwr.kind == 0 && knl_acpica_buf(knl_acpica_handle(dep), "CLDB", (UB *)line, 32) >= 3 ) {
			(void)knl_campwr_find(&s->pwr, dep);
		}
	}
	if ( s->pwr.kind == 0 ) {
		/* no _DEP: the controller whose CLDB has the SSDB's number */
		void	*d;

		for ( i = 0; i < 8 && ( d = knl_acpica_device("INT3472", i) ) != NULL; i++ ) {
			UB	cl[32];

			if ( knl_acpica_buf(d, "CLDB", cl, sizeof(cl)) >= 3 && cl[2] == s->clid ) {
				(void)knl_acpica_path(d, dep, sizeof(dep));
				(void)knl_campwr_find(&s->pwr, dep);
				break;
			}
		}
	}
	s->ov.bus = knl_dwi2c_bus((UW)adr, knl_acpica_handle(s->ctrl), s->hz);
	s->ov.addr = (UH)s->addr;
	return ( s->ov.bus != NULL ) ? E_OK : E_NOEXS;
}

/* The sensor powered and its ID read; reset and powerdown tried the other way round when it does not answer */
LOCAL ER probe_sensor( SENSOR *s )
{
	ER	er;
	INT	k;

	if ( s->pwr.kind != CAMPWR_DISCRETE ) {
		tm_printf((UB *)"cio2: %s: no power controller this can drive\n", s->path);
		return E_NOSPT;
	}
	if ( s->mclk != OV5693_MCLK ) {
		tm_printf((UB *)"cio2: %s: an input clock of %d Hz; the sensor's settings are for %d\n", s->path, s->mclk,
			  OV5693_MCLK);
	}
	for ( k = 0; k < 2; k++ ) {
		knl_campwr_say(&s->pwr, "before");
		er = knl_campwr_on(&s->pwr);
		knl_campwr_say(&s->pwr, "powered");
		if ( er >= E_OK ) er = knl_ov5693_probe(&s->ov);
		knl_campwr_off(&s->pwr);
		if ( er != E_IO ) break;
		if ( k == 0 ) knl_campwr_swap(&s->pwr);
	}
	if ( er == E_IO ) knl_campwr_swap(&s->pwr);	/* back as the _DSM says, neither answered */
	return er;
}

EXPORT INT knl_cio2_init( void )
{
	CIO2	*c;
	T_CFLG	cf;
	T_CMTX	cm;
	T_CTSK	ct;
	void	*dev = NULL;
	INT	i, k, n = 0;
	char	path[48];

	if ( !knl_acpica_ready() || setting_is("cam", "off") ) return 0;
	/* the sensors said, whichever are driven */
	for ( i = 0; i < (INT)( sizeof(sensors) / sizeof(sensors[0]) ); i++ ) {
		for ( k = 0; k < 4; k++ ) {
			void	*d = knl_acpica_device(sensors[i].hid, k);

			if ( d == NULL ) break;
			(void)knl_acpica_path(d, path, sizeof(path));
			tm_printf((UB *)"cio2: %s %s (%s)%s\n", path, sensors[i].hid, sensors[i].model,
				  sensors[i].driven ? "" : ": not driven");
			if ( sensors[i].driven && dev == NULL ) dev = d;
		}
	}
	if ( dev == NULL ) return 0;
	c = (CIO2 *)Kcalloc(1, sizeof(CIO2));
	if ( c == NULL ) return 0;
	c->mode = -1;
	if ( !find_cio2(c) ) {
		tm_printf((UB *)"cio2: no IPU3 CSI-2 receiver (8086:%04x)\n", CIO2_DEVICE);
		Kfree(c);
		return 0;
	}
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cm.ceilpri = 0;
	c->flg = tk_cre_flg(&cf);
	c->mtx = tk_cre_mtx(&cm);
	if ( c->flg <= 0 || c->mtx <= 0 ) return 0;
	cio2 = c;

	if ( read_sensor(&c->sen, dev, "OV5693") < E_OK ) return 0;
	c->sen.ov.bars = setting_is("cam", "bars");
	if ( c->sen.lanes == 0 || c->sen.lanes > 4 || c->sen.port > 3 ) {
		tm_printf((UB *)"cio2: %s: port %d with %d lanes cannot be\n", c->sen.path, c->sen.port, c->sen.lanes);
		return 0;
	}
	if ( c->sen.lanes != OV5693_LANES ) {
		/* the sensor sends on the first lanes of those wired: the receiver takes as many as it sends */
		tm_printf((UB *)"cio2: %s: the SSDB says %d lanes; the sensor's settings are for %d\n", c->sen.path,
			  c->sen.lanes, OV5693_LANES);
		if ( c->sen.lanes < OV5693_LANES ) return 0;
		c->sen.lanes = OV5693_LANES;
	}
	if ( probe_sensor(&c->sen) < E_OK ) {
		tm_printf((UB *)"cio2: %s: no OV5693 answered: no camera\n", c->sen.path);
		return 0;
	}

	for ( i = 0; i < NMODES; i++ ) {
		T_CAMMODE	*m = &c->modes[i];

		m->fmtIndex = (UB)( offer[i].fmt == CAMFMT_NV12 ? 1 : 2 );
		m->frmIndex = (UB)( i / 2 + 1 );
		m->format = offer[i].fmt;
		m->width = offer[i].w;
		m->height = offer[i].h;
		m->maxBytes = (UW)offer[i].w * offer[i].h * ( offer[i].fmt == CAMFMT_NV12 ? 3 : 4 ) / 2;
		m->defInterval = OV5693_INTERVAL;
		m->nintv = 1;
		m->interval[0] = OV5693_INTERVAL;
	}
	knl_strcpy((char *)c->src.info.product, "Front camera (OV5693)");
	knl_strcpy((char *)c->src.info.manufacturer, "OmniVision");
	c->src.modes = c->modes;
	c->src.nmodes = NMODES;
	c->src.set_mode = cio2_set_mode;
	c->src.start = cio2_start;
	c->src.stop = cio2_stop;
	c->src.priv = c;
	c->src.unit = -1;

	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = c;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)cio2_task;
	ct.itskpri = 14;
	ct.stksz = 8 * 1024;
	c->tsk = tk_cre_tsk(&ct);
	if ( c->tsk <= 0 ) return 0;
	tk_sta_tsk(c->tsk, 0);
	if ( knl_cam_attach(&c->src) >= 0 ) n++;
	tm_printf((UB *)"TessronOS: cio2: %s %s on port %d (%d lanes), %d modes, unit %d\n", c->sen.path, c->sen.model,
		  c->sen.port, c->sen.lanes, NMODES, c->src.unit);
	return n;
}

#endif /* X64 */
