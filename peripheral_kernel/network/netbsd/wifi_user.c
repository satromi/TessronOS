/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifi_user.c (x64 PC)
 *	The hypercalls of the Wi-Fi card (design 9.6, 9.6.9).
 *
 *	OpenBSD's IEEE 802.11 layer and iwm or iwx (device/net/wifi/obsd,
 *	its PCI glue obsd_pci.c, design 9.6.9) reach the card through these, under
 *	the names rump's pci_user.h gives them (they were made for the rump
 *	kernel's PCI bus of NetBSD's iwm, which the x64 build no longer has):
 *
 *	- the configuration space and the windows (BARs) of the card, as
 *	  the kernel's PCI code reads them (peripheral_kernel/sysman/pcie.c);
 *	- memory the card reaches by DMA: physically whole pages below 4GB
 *	  from the page allocator, at their place in the direct map, so
 *	  that the address the driver is given is the kernel's own;
 *	- the card's one message interrupt (MSI), which wakes the thread
 *	  that runs the driver's handler (the layer's interrupt task).
 *
 *	And those of TessronOS's component: which card iwm(4) or iwx(4)
 *	will drive, the firmware read from its object on the system volume, and the
 *	frames the card received, given to the Wi-Fi manager (wifi.c).
 *
 *	A hypercall that waits (an interrupt, the disk) gives up the rump
 *	kernel's virtual processor while it waits, as the card's reads of
 *	neta_user.c do.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "sysman/pfalloc.h"
#include "tstdlib.h"
#include "rumpglue.h"

/* NetBSD's errno values the hypercalls answer with */
#define NB_ENOENT	2
#define NB_ENXIO	6
#define NB_ENOMEM	12
#define NB_EINVAL	22
#define NB_EOPNOTSUPP	45

/* The Wi-Fi manager (device/net/wifi/wifi.c) */
IMPORT void	knl_wifi_rx( CONST UB *frame, INT len );

/*
 * The cards OpenBSD's drivers know: iwm(4) (its iwm_devices[]: Intel's
 * 3160 to 8265) and iwx(4) (its iwx_devices[]: the Wi-Fi 6 AX200 and
 * later, whose registers and rings are of the next generation). Which
 * of the two drives the card is decided again by the layer from the
 * drivers' own tables (tswifi_obsd.c); here it says only which
 * registers the records of the card read.
 */
typedef struct {
	UH	device;
	UB	iwx;		/* iwx's: the next generation's registers */
} WIFI_ID;

LOCAL CONST WIFI_ID	wifi_ids[] = {
	{ 0x08b1, 0 }, { 0x08b2, 0 }, { 0x08b3, 0 }, { 0x08b4, 0 },	/* 7260, 3160 */
	{ 0x095a, 0 }, { 0x095b, 0 },					/* 7265 */
	{ 0x3165, 0 }, { 0x3166, 0 }, { 0x24fb, 0 },			/* 3165, 3168 */
	{ 0x24f3, 0 }, { 0x24f4, 0 }, { 0x24f5, 0 }, { 0x24f6, 0 },	/* 8260, 4165 */
	{ 0x24fd, 0 },							/* 8265 */
	{ 0x2723, 1 },							/* AX200 */
	{ 0x02f0, 1 }, { 0xa0f0, 1 }, { 0x34f0, 1 }, { 0x06f0, 1 },	/* AX201 */
	{ 0x43f0, 1 }, { 0x3df0, 1 }, { 0x4df0, 1 },
	{ 0x2725, 1 }, { 0x7e40, 1 },					/* AX210 */
	{ 0x2726, 1 }, { 0x51f0, 1 }, { 0x7a70, 1 }, { 0x7af0, 1 },	/* AX211 */
	{ 0x7f70, 1 }, { 0x54f0, 1 }, { 0x51f1, 1 }, { 0x7740, 1 }
};

LOCAL BOOL	w_iwx = FALSE;		/* the card found is one of iwx's */

/* The firmware a card asks for by name, and the object that holds it */
typedef struct {
	CONST char	*name;
	CONST char	*uuid;
} WIFI_FW;

LOCAL CONST WIFI_FW	wifi_fw[] = {
	{ "iwm-8265-36",		SYSDEF_FW_IWM8265_36 },	/* OpenBSD's iwm: API 36 */
	{ "iwx-cc-a0-77",		SYSDEF_FW_IWXCC_77 },	/* OpenBSD's iwx: the AX200's, API 77 */
};

/*
 * A hypercall that waits gives up the rump kernel's virtual processor
 * while it waits. With OpenBSD's layer (device/net/wifi/obsd, design
 * 9.6.9) the card is not in the rump kernel, and its caller holds no
 * virtual processor of it.
 */
#ifdef WIFI_OBSD
#define W_UNSCHED(n)	( (void)( n = 0 ) )
#define W_SCHED(n)	( (void)( n ) )
#else
#define W_UNSCHED(n)	knl_rump_unsched(&n)
#define W_SCHED(n)	knl_rump_sched(n)
#endif

/* ---------------------------------------------------------------- the card */

/* The first card on the bus that iwm or iwx drives (the Wi-Fi manager asks too, before the rump kernel is up) */
EXPORT BOOL knl_wifi_pci_find( T_PCIDEV *out )
{
	T_PCIDEV	d;
	INT		i, k;

	for ( i = 0; ts_pcie_get(i, &d) == E_OK; i++ ) {
		if ( d.vendor != 0x8086 ) continue;
		for ( k = 0; k < (INT)( sizeof(wifi_ids) / sizeof(wifi_ids[0]) ); k++ ) {
			if ( d.device == wifi_ids[k].device ) {
				w_iwx = ( wifi_ids[k].iwx != 0 );
				*out = d;
				return TRUE;
			}
		}
	}
	return FALSE;
}

LOCAL UD	w_sink_pa = 0;		/* the page an address the card may not reach is turned into */
LOCAL UW	w_refused = 0;		/* how many such addresses there were */
LOCAL void	sink_take( void );

/* The card iwm or iwx will drive: its bus, and vendor and device in one word; 0 or ENXIO */
EXPORT int rumpcomp_tswifi_card( int *bus, unsigned int *id )
{
	T_PCIDEV	d;

	if ( !knl_wifi_pci_find(&d) ) return NB_ENXIO;
	sink_take();			/* before the card can be given any address */
	if ( w_sink_pa == 0 ) return NB_ENXIO;
	(void)ts_pcie_enable(&d);	/* as every driver does: the card and the path to it */
	*bus = (int)d.bus;
	*id = ( (UW)d.vendor << 16 ) | d.device;
	tm_printf((UB *)"wifi: card %04x:%04x at %d:%d.%d, BAR0 %lx, for %s\n",
		  d.vendor, d.device, d.bus, d.dev, d.fn, (unsigned long)d.bar[0], w_iwx ? "iwx" : "iwm");
	return 0;
}

/* ---------------------------------------------------------------- the bus */

EXPORT void *rumpcomp_pci_map( unsigned long addr, unsigned long len )
{
	(void)len;
	return (void *)DEV_BASE(addr);
}

EXPORT int rumpcomp_pci_confread( unsigned bus, unsigned dev, unsigned fun, int reg, unsigned int *v )
{
	*v = ts_pcie_cfg_read(bus, dev, fun, (UINT)reg & 0xffc, 4);
	return 0;
}

EXPORT int rumpcomp_pci_confwrite( unsigned bus, unsigned dev, unsigned fun, int reg, unsigned int v )
{
	ts_pcie_cfg_write(bus, dev, fun, (UINT)reg & 0xffc, v, 4);
	return 0;
}

/* ---------------------------------------------------------------- the interrupt */

#define WIFI_NIRQ	2

typedef struct {
	UINT	cookie;
	UINT	intno;
	ID	sem;
} WIFI_IRQ;

LOCAL WIFI_IRQ	irqs[WIFI_NIRQ];
LOCAL INT	nirq = 0;

/*
 * What is kept of the card's interrupts for KERNEL.TXT (knl_wifi_irq_say):
 * how many came, how many woke the thread that runs the handler, and the
 * card's interrupt registers as each of the last few found them
 */
#define CSR_INT		0x008		/* the causes, written back to clear them */
#define CSR_INT_MASK	0x00c		/* the causes that raise the message */
#define CSR_FH_INT	0x010		/* the DMA engines' causes */
#define CSR_GP_CNTRL	0x024
#define CSR_ICT		0x0a0		/* the interrupt cause table in memory, bit 31 on */
#define FH_SRVC_TX_CFG	0x1e20		/* the service channel the firmware is loaded through */
#define FH_TX_STATUS	0x1eb0
#define WIFI_SEEN_N	8

IMPORT UD	knl_lockup_now( void );		/* lockup.c */

typedef struct {
	UD	at;
	UW	cause, mask, fh;
	UH	ict_n;			/* entries of the interrupt cause table not 0 */
	UH	ict_first;		/* the first of them, and what it holds */
	UW	ict_val;
} WIFI_SEEN;

/*
 * The card's registers and the memory it shares with the host, for the
 * record of the command queue (knl_wifi_cmdq_say): its FH and scheduler
 * registers, read with the card kept awake, and the TFD ring, the RX
 * status and the interrupt cause table the registers point at
 */
#define CSR_RESET		0x020
#define CSR_HW_REV		0x028
#define CSR_GIO			0x03c
#define GP_MAC_ACCESS_REQ	0x00000008
#define GP_CLOCK_READY		0x00000001
#define GP_GOING_TO_SLEEP	0x00000010
#define HBUS_PRPH_RADDR		0x448
#define HBUS_PRPH_RDAT		0x450
#define FH_CBBC(q)		( 0x19d0 + 4 * (q) )	/* a queue's TFD ring, >> 8 (queues 0 to 15) */
#define FH_RX_STTS_WPTR		0x1bc0			/* the RX status, >> 4 */
#define FH_RX_RBDCB_BASE	0x1bc4
#define FH_RX_WPTR		0x1bc8
#define FH_RX_RDPTR		0x1bcc
#define FH_RX_CONFIG		0x1c00
#define FH_RX_STATUS		0x1c44
#define FH_TX_CONFIG(c)		( 0x1d00 + 0x20 * (c) )
#define FH_TX_CHICKEN		0x1e98
#define SCD_BASE		0xa02c00
#define CMD_QUEUE		9			/* the firmware's command queue (before DQA) */
#define TFD_SIZE		128

LOCAL T_PCIDEV		wcard;			/* the card whose message is set up */
LOCAL UBINT		wregs = 0;		/* its first BAR */
LOCAL UINT		wmsi = 0;		/* where its MSI capability is */
LOCAL volatile UW	w_nint = 0, w_nwake = 0;
LOCAL WIFI_SEEN		w_seen[WIFI_SEEN_N];
LOCAL BOOL		w_watch = FALSE;
LOCAL UW		w_dma_n = 0;		/* DMA memory given to the card: how many pieces, and where */
LOCAL UD		w_dma_lo = 0, w_dma_hi = 0;

IMPORT void	knl_vtd_faults( CONST char *why );	/* vtd.c: what the IOMMU stopped */

Inline UW wreg( UW off )
{
	return ( wregs != 0 ) ? *(volatile UW *)( wregs + off ) : 0xffffffff;
}

LOCAL BOOL ram_has( UD pa );

Inline void wwr( UW off, UW v )
{
	UBINT	b = wregs;

	if ( b != 0 ) *(volatile UW *)( b + off ) = v;
}

/* Host memory the card was pointed at, read through the direct map; NULL when it is not RAM */
LOCAL volatile UB *host_at( UD pa, UD len )
{
	if ( pa == 0 || !ram_has(pa) || !ram_has(pa + len - 1) ) return NULL;
	return (volatile UB *)PA2VA(pa);
}

/* The interrupt cause table (ICT): how many entries are not 0, the first of them and its value */
LOCAL void ict_look( UH *p_n, UH *p_first, UW *p_val )
{
	UW	reg = wreg(CSR_ICT);
	volatile UW *t;
	INT	i, n = 0;

	*p_n = 0;
	*p_first = 0;
	*p_val = 0;
	if ( ( reg & 0x80000000U ) == 0 ) return;		/* not in use */
	t = (volatile UW *)host_at((UD)( reg & 0x0fffffffU ) << 12, 4096);
	if ( t == NULL ) return;
	for ( i = 0; i < 1024; i++ ) {
		if ( t[i] != 0 ) {
			if ( n == 0 ) {
				*p_first = (UH)i;
				*p_val = t[i];
			}
			n++;
		}
	}
	*p_n = (UH)n;
}

/* The card awake for reads of its FH and scheduler registers: TRUE when it is, *p_mine when this woke it */
LOCAL BOOL card_wake( BOOL *p_mine )
{
	UW	gp = wreg(CSR_GP_CNTRL);
	INT	i;

	*p_mine = FALSE;
	if ( wregs == 0 ) return FALSE;
	if ( ( gp & GP_MAC_ACCESS_REQ ) == 0 ) {
		wwr(CSR_GP_CNTRL, gp | GP_MAC_ACCESS_REQ);
		*p_mine = TRUE;
	}
	for ( i = 0; i < 1500; i++ ) {
		if ( ( wreg(CSR_GP_CNTRL) & ( GP_CLOCK_READY | GP_GOING_TO_SLEEP ) ) == GP_CLOCK_READY ) return TRUE;
		WaitUsec(10);
	}
	return FALSE;
}

LOCAL void card_sleep( BOOL mine )
{
	if ( mine ) {
		wwr(CSR_GP_CNTRL, wreg(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
	}
}

LOCAL UW prph_rd( UW addr )
{
	wwr(HBUS_PRPH_RADDR, ( addr & 0x000fffffU ) | ( 3U << 24 ));
	return wreg(HBUS_PRPH_RDAT);
}

/*
 * The command queue as it stands: the scheduler's view of it, the TFDs
 * the driver wrote and the commands they carry, the FH's TX and RX
 * channels, the RX status the card last wrote, and the interrupt cause
 * table. Read only, so that it can be asked at any time.
 */
LOCAL void cmdq_say( void )
{
	BOOL		mine, awake;
	UD		ring, stat;
	volatile UB	*t, *c, *st;
	INT		i, k;
	UH		in, ifirst;
	UW		ival;

	if ( wregs == 0 ) return;
	awake = card_wake(&mine);
	tm_printf((UB *)"wifi: cmdq: card %s; HW_REV %08x RESET %08x GIO %08x GP_CNTRL %08x\n",
		  awake ? "awake" : "NOT awake (the reads below may be 5a5a or a5a5)", wreg(CSR_HW_REV), wreg(CSR_RESET),
		  wreg(CSR_GIO), wreg(CSR_GP_CNTRL));
	if ( w_iwx ) {
		/*
		 * iwx's cards have no scheduler or FH channels of this kind: their
		 * queues are the firmware's (TVQM) and the rings the driver's
		 * context information points at; only the interrupt cause table
		 */
		card_sleep(mine);
		ict_look(&in, &ifirst, &ival);
		tm_printf((UB *)"wifi: cmdq: ICT %08x: %d entries not 0, the first #%d = %08x\n", wreg(CSR_ICT), in, ifirst,
			  ival);
		return;
	}
	tm_printf((UB *)"wifi: cmdq: SCD SRAM %08x DRAM %08x TXFACT %08x GP %08x EN %08x AGGR %08x CHAINEXT %08x; "
		  "queue %d: WRPTR %08x RDPTR %08x STATUS %08x\n", prph_rd(SCD_BASE), prph_rd(SCD_BASE + 0x8),
		  prph_rd(SCD_BASE + 0x10), prph_rd(SCD_BASE + 0x1a8), prph_rd(SCD_BASE + 0x254), prph_rd(SCD_BASE + 0x248),
		  prph_rd(SCD_BASE + 0x244), CMD_QUEUE, prph_rd(SCD_BASE + 0x18 + CMD_QUEUE * 4),
		  prph_rd(SCD_BASE + 0x68 + CMD_QUEUE * 4), prph_rd(SCD_BASE + 0x10c + CMD_QUEUE * 4));
	tm_printf((UB *)"wifi: cmdq: FH TX config 0-7 %08x %08x %08x %08x %08x %08x %08x %08x, status %08x, chicken %08x\n",
		  wreg(FH_TX_CONFIG(0)), wreg(FH_TX_CONFIG(1)), wreg(FH_TX_CONFIG(2)), wreg(FH_TX_CONFIG(3)),
		  wreg(FH_TX_CONFIG(4)), wreg(FH_TX_CONFIG(5)), wreg(FH_TX_CONFIG(6)), wreg(FH_TX_CONFIG(7)),
		  wreg(FH_TX_STATUS), wreg(FH_TX_CHICKEN));
	tm_printf((UB *)"wifi: cmdq: FH RX status base %08x ring %08x WPTR %08x RDPTR %08x config %08x status %08x\n",
		  wreg(FH_RX_STTS_WPTR), wreg(FH_RX_RBDCB_BASE), wreg(FH_RX_WPTR), wreg(FH_RX_RDPTR), wreg(FH_RX_CONFIG),
		  wreg(FH_RX_STATUS));
	ring = (UD)wreg(FH_CBBC(CMD_QUEUE)) << 8;
	stat = (UD)wreg(FH_RX_STTS_WPTR) << 4;
	card_sleep(mine);

	/* the TFDs the driver wrote (index 0 on), and the head of the command each carries */
	t = host_at(ring, TFD_SIZE * 8);
	tm_printf((UB *)"wifi: cmdq: queue %d's TFD ring at %lx%s\n", CMD_QUEUE, ring, ( t == NULL ) ? ": not RAM" : "");
	for ( i = 0; t != NULL && i < 8; i++ ) {
		volatile UB *d = t + i * TFD_SIZE;
		UW	lo = d[4] | ( d[5] << 8 ) | ( d[6] << 16 ) | ( (UW)d[7] << 24 );
		UH	hl = (UH)( d[8] | ( d[9] << 8 ) );
		UD	pa = (UD)lo | ( (UD)( hl & 0xf ) << 32 );

		if ( d[3] == 0 && lo == 0 ) break;		/* not written yet */
		c = host_at(pa, 8);
		tm_printf((UB *)"wifi: cmdq:   TFD %d: %d TB, TB0 at %lx, %d bytes; command %02x flags %02x idx %d qid %d%s\n", i,
			  d[3] & 0x1f, pa, hl >> 4, ( c != NULL ) ? c[0] : 0, ( c != NULL ) ? c[1] : 0, ( c != NULL ) ? c[2] : 0,
			  ( c != NULL ) ? c[3] : 0, ( c == NULL ) ? " (not RAM)" : "");
	}

	/* what the card last said it wrote to the RX ring */
	st = host_at(stat, 8);
	if ( st != NULL ) {
		tm_printf((UB *)"wifi: cmdq: RX status at %lx: closed RB %d, closed frame %d, finished RB %d\n", stat,
			  ( st[0] | ( st[1] << 8 ) ) & 0xfff, st[2] | ( st[3] << 8 ), ( st[4] | ( st[5] << 8 ) ) & 0xfff);
	}
	ict_look(&in, &ifirst, &ival);
	tm_printf((UB *)"wifi: cmdq: ICT %08x: %d entries not 0, the first #%d = %08x\n", wreg(CSR_ICT), in, ifirst, ival);
	if ( in > 1 ) {
		volatile UW *it = (volatile UW *)host_at((UD)( wreg(CSR_ICT) & 0x0fffffffU ) << 12, 4096);

		for ( i = 0, k = 0; it != NULL && i < 1024 && k < 16; i++ ) {
			if ( it[i] != 0 ) {
				tm_printf((UB *)"wifi: cmdq:   ICT #%d = %08x\n", i, it[i]);
				k++;
			}
		}
	}
}

LOCAL void wifi_inthdr( UINT n )
{
	INT	i;
	UW	k = w_nint++;
	WIFI_SEEN *s = &w_seen[k % WIFI_SEEN_N];

	s->at = knl_lockup_now();
	s->cause = wreg(CSR_INT);
	s->mask = wreg(CSR_INT_MASK);
	s->fh = wreg(CSR_FH_INT);
	ict_look(&s->ict_n, &s->ict_first, &s->ict_val);
	for ( i = 0; i < nirq; i++ ) {
		if ( irqs[i].intno == n ) {
			(void)tk_sig_sem(irqs[i].sem, 1);
			break;
		}
	}
	EndOfInt(n);
}

/*
 * The card's interrupt as it stands: what came, the message as the card
 * holds it, the processor it goes to, and the card's own registers
 */
EXPORT void knl_wifi_irq_say( CONST char *why )
{
	UD	now = knl_lockup_now(), frq = ( knl_cntfrq != 0 ) ? knl_cntfrq : 1;
	UW	ctrl, cmd, lo, hi = 0, data, maskb = 0, pend = 0;
	UINT	b = wcard.bus, d = wcard.dev, f = wcard.fn, data_off;
	INT	i, k;
	SZ	cnt;

	if ( nirq == 0 ) {
		tm_printf((UB *)"wifi: irq (%s): no message interrupt was set up\n", why);
		return;
	}
	cmd = ts_pcie_cfg_read(b, d, f, PCI_COMMAND, 4);
	tm_printf((UB *)"wifi: irq (%s): interrupt %d to PRC %d, %u taken, %u woke the handler's thread; "
		  "PCI command %04x status %04x\n", why, irqs[0].intno, (INT)GetIntTarget(irqs[0].intno),
		  (UINT)w_nint, (UINT)w_nwake, cmd & 0xffff, cmd >> 16);
	if ( wmsi != 0 ) {
		ctrl = ts_pcie_cfg_read(b, d, f, wmsi + 2, 2);
		lo = ts_pcie_cfg_read(b, d, f, wmsi + 4, 4);
		if ( ctrl & 0x0080 ) {
			hi = ts_pcie_cfg_read(b, d, f, wmsi + 8, 4);
			data_off = 0x0c;
		} else {
			data_off = 0x08;
		}
		data = ts_pcie_cfg_read(b, d, f, wmsi + data_off, 2);
		if ( ctrl & 0x0100 ) {
			maskb = ts_pcie_cfg_read(b, d, f, wmsi + data_off + 4, 4);
			pend = ts_pcie_cfg_read(b, d, f, wmsi + data_off + 8, 4);
		}
		tm_printf((UB *)"wifi: irq: MSI at %02x: control %04x, address %08x %08x, data %04x, mask %08x, "
			  "pending %08x\n", wmsi, ctrl, hi, lo, data, maskb, pend);
	}
	{
		/* the card's link: ASPM (L0s, L1) and its state, and LTR, which the card's power saving follows */
		UINT	pe = ts_pcie_cap_find(&wcard, PCI_CAP_EXPRESS);

		if ( pe != 0 ) {
			tm_printf((UB *)"wifi: irq: PCIe device control %04x, link control %04x status %04x, "
				  "device control 2 %04x\n", ts_pcie_cfg_read(b, d, f, pe + 0x08, 2),
				  ts_pcie_cfg_read(b, d, f, pe + 0x10, 2), ts_pcie_cfg_read(b, d, f, pe + 0x12, 2),
				  ts_pcie_cfg_read(b, d, f, pe + 0x28, 2));
		}
	}
	if ( w_iwx ) {
		tm_printf((UB *)"wifi: irq: card CSR_INT %08x INT_MASK %08x FH_INT %08x GP_CNTRL %08x ICT %08x\n",
			  wreg(CSR_INT), wreg(CSR_INT_MASK), wreg(CSR_FH_INT), wreg(CSR_GP_CNTRL), wreg(CSR_ICT));
	} else {
		tm_printf((UB *)"wifi: irq: card CSR_INT %08x INT_MASK %08x FH_INT %08x GP_CNTRL %08x ICT %08x; "
			  "FH service channel %08x, TX status %08x\n", wreg(CSR_INT), wreg(CSR_INT_MASK), wreg(CSR_FH_INT),
			  wreg(CSR_GP_CNTRL), wreg(CSR_ICT), wreg(FH_SRVC_TX_CFG), wreg(FH_TX_STATUS));
	}
	tm_printf((UB *)"wifi: irq: DMA memory: %u pieces within %lx-%lx; %u addresses refused\n", (UINT)w_dma_n, w_dma_lo,
		  w_dma_hi, (UINT)w_refused);
	knl_vtd_faults(why);
	cmdq_say();
	cnt = ( w_nint < WIFI_SEEN_N ) ? (SZ)w_nint : WIFI_SEEN_N;
	for ( i = 0; i < (INT)cnt; i++ ) {
		k = (INT)( ( w_nint - cnt + (UW)i ) % WIFI_SEEN_N );
		tm_printf((UB *)"wifi: irq:   #%u %ld ms ago: CSR_INT %08x INT_MASK %08x FH_INT %08x; ICT %d not 0, "
			  "first #%d = %08x\n", (UINT)( w_nint - cnt + (UW)i ), (D)( ( now - w_seen[k].at ) * 1000 / frq ),
			  w_seen[k].cause, w_seen[k].mask, w_seen[k].fh, w_seen[k].ict_n, w_seen[k].ict_first,
			  w_seen[k].ict_val);
	}
}

/*
 * Once, while the driver loads the firmware and sends its first
 * commands: the interrupt's and the command queue's state 3 s and 8 s
 * after the firmware was read
 */
LOCAL void wifi_watch_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	tk_dly_tsk(3000);
	knl_wifi_irq_say("3 s after the firmware was read");
	tk_dly_tsk(5000);
	knl_wifi_irq_say("8 s after the firmware was read");
	tk_exd_tsk();
}

LOCAL void wifi_watch_start( void )
{
	T_CTSK	ct;
	ID	t;

	if ( w_watch ) return;
	w_watch = TRUE;
	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)wifi_watch_task;
	ct.itskpri = 30;
	ct.stksz = 16384;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) (void)tk_sta_tsk(t, 0);
}

/*
 * The card's message interrupt set up for the cookie the bus gave;
 * 0, or ENXIO when the card has no message to give
 */
EXPORT int rumpcomp_pci_irq_map( unsigned bus, unsigned dev, unsigned fun, int intrline, unsigned cookie )
{
	T_PCIDEV	d;
	T_CSEM		cs;
	WIFI_IRQ	*q;
	UINT		intno;
	INT		i;
	ER		er;

	(void)intrline;
	if ( nirq >= WIFI_NIRQ ) return NB_ENXIO;
	for ( i = 0; ts_pcie_get(i, &d) == E_OK; i++ ) {
		if ( d.bus == bus && d.dev == dev && d.fn == fun ) break;
	}
	if ( ts_pcie_get(i, &d) != E_OK ) return NB_ENXIO;
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 0x7fffffff;
	q = &irqs[nirq];
	q->sem = tk_cre_sem(&cs);
	if ( q->sem <= 0 ) return NB_ENOMEM;
	er = ts_pcie_msi_plain_alloc(&d, &intno);
	if ( er < E_OK ) {
		tm_printf((UB *)"wifi: no message interrupt for %d:%d.%d (%d)\n", bus, dev, fun, (INT)er);
		tk_del_sem(q->sem);
		return NB_ENXIO;
	}
	q->cookie = cookie;
	q->intno = intno;
	if ( nirq == 0 ) {
		wcard = d;
		wregs = ( d.bar[0] != 0 ) ? (UBINT)DEV_BASE(d.bar[0]) : 0;
		wmsi = ts_pcie_cap_find(&d, PCI_CAP_MSI);
	}
	nirq++;
	knl_define_inthdr((INT)intno, TA_HLNG, (FP)wifi_inthdr);
	EnableInt(intno, INTPRI_DEVICE);
	tm_printf((UB *)"wifi: message interrupt %d\n", (INT)intno);
	return 0;
}

/* Waits outside the rump kernel for the interrupt of the cookie; 0 */
EXPORT int rumpcomp_tswifi_intr_wait( unsigned int cookie )
{
	INT	i, n;

	for ( i = 0; i < nirq && irqs[i].cookie != cookie; i++ ) ;
	if ( i == nirq ) return NB_EINVAL;
	W_UNSCHED(n);
	(void)tk_wai_sem(irqs[i].sem, 1, TMO_FEVR);
	w_nwake++;
	W_SCHED(n);
	return 0;
}

/* ---------------------------------------------------------------- DMA */

LOCAL INT order_of( UD size )
{
	INT	o = 0;

	while ( ( (UD)PAGE_SIZE << o ) < size && o < PF_MAX_ORDER ) o++;
	return ( ( (UD)PAGE_SIZE << o ) < size ) ? -1 : o;
}

/*
 * Memory for the card: whole pages below 4GB, aligned to their own
 * size (which covers what the driver asks for), cleared
 */
EXPORT int rumpcomp_pci_dmalloc( unsigned long size, unsigned long align, unsigned long *pap, unsigned long *vap )
{
	PFRAME	*pf;
	INT	o = order_of(( align > size ) ? align : size);
	UD	pa;

	if ( o < 0 ) return NB_ENOMEM;
	pf = knl_alloc_pages((UINT)o, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) return NB_ENOMEM;
	pa = knl_pf_to_pa(pf);
	/* for the record: where the card's DMA goes (the large ones, the firmware's chunk among them, one by one) */
	w_dma_n++;
	if ( w_dma_lo == 0 || pa < w_dma_lo ) w_dma_lo = pa;
	if ( pa + ( (UD)PAGE_SIZE << o ) > w_dma_hi ) w_dma_hi = pa + ( (UD)PAGE_SIZE << o );
	if ( size >= 16 * 1024 ) {
		tm_printf((UB *)"wifi: DMA memory %lu bytes (align %lu) at %lx\n", size, align, (unsigned long)pa);
	}
	*pap = (unsigned long)pa;
	*vap = (unsigned long)PA2VA(pa);
	return 0;
}

EXPORT void rumpcomp_pci_dmafree( unsigned long va, unsigned long size )
{
	INT	o = order_of(size);

	if ( va != 0 && o >= 0 ) {
		knl_free_pages(knl_pa_to_pf(VA2PA(va)), (UINT)o);
	}
}

/* rump's layout of a segment (pci_user.h) */
typedef struct {
	unsigned long	ds_pa;
	unsigned long	ds_len;
	unsigned long	ds_vacookie;
} RC_DMASEG;

/* Memory from rumpcomp_pci_dmalloc is mapped already: one segment of it, at its place */
EXPORT int rumpcomp_pci_dmamem_map( RC_DMASEG *dss, unsigned long nseg, unsigned long size, void **vap )
{
	(void)size;
	if ( nseg != 1 ) return NB_EOPNOTSUPP;
	*vap = (void *)dss[0].ds_vacookie;
	return 0;
}

/*
 * The address the card is given for a buffer of the rump kernel's.
 * rump's bus_dma takes a run of contiguous virtual addresses for a run
 * of contiguous memory, which holds for the two kinds of address the
 * rump kernel's memory has: the kernel's direct map of RAM (the page
 * allocator, Kmalloc) and the kernel's image. Anything else (a mapping
 * of scattered pages, a device's window, a bad pointer) has no physical
 * address that way; nor has an address outside RAM or beyond what the
 * card reaches (36 bits). Such a one is not given to the card: it is
 * told, counted, and the card is given the page set aside for it
 * instead, where whatever the card writes does no harm.
 */
#define CARD_DMA_LIMIT	( 1ULL << 36 )

LOCAL BOOL ram_has( UD pa )
{
	INT	i;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		if ( pa >= knl_mem_range[i].start && pa < knl_mem_range[i].start + knl_mem_range[i].size ) return TRUE;
	}
	return FALSE;
}

EXPORT unsigned long rumpcomp_pci_virt_to_mach( void *va )
{
	UBINT	v = (UBINT)va;
	UD	pa = 0;
	BOOL	ok;

	if ( v >= KIMG_VA_BASE ) {
		pa = VA2PA(v);			/* the kernel's image, loaded whole */
		ok = TRUE;
	} else if ( v >= KVA_BASE && v < DEV_VA_BASE ) {
		pa = v - KVA_BASE;		/* the direct map */
		ok = TRUE;
	} else {
		ok = FALSE;
	}
	if ( ok && ( !ram_has(pa) || pa >= CARD_DMA_LIMIT ) ) ok = FALSE;
	if ( !ok ) {
		if ( w_refused++ < 16 ) {
			tm_printf((UB *)"wifi: DMA REFUSED: %p is not memory the card may reach (as %lx); the card is given "
				  "the sink page %lx instead (called from %p)\n", va, (unsigned long)pa,
				  (unsigned long)w_sink_pa, __builtin_return_address(0));
		}
		return (unsigned long)w_sink_pa;
	}
	return (unsigned long)pa;
}

/* The sink page, taken once before the card is driven */
LOCAL void sink_take( void )
{
	PFRAME	*pf;

	if ( w_sink_pa != 0 ) return;
	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf != NULL ) w_sink_pa = knl_pf_to_pa(pf);
}

/* ---------------------------------------------------------------- the firmware */

/*
 * The firmware the driver asks for, read whole from its object: 0 and
 * the bytes (Kmalloc), or ENOENT. Waits for the disk outside the rump
 * kernel.
 */
EXPORT int rumpcomp_tswifi_fw_open( CONST char *name, void **data, UD *size )
{
	TS_UUID		u;
	T_OBREC		r[4];
	INT		i, k, n, cnt = 0;
	ID		key = E_NOEXS;
	SZ		asz = 0;
	UD		sz = 0;
	UB		*buf = NULL;
	ER		er = E_NOEXS;

	for ( k = 0; k < (INT)( sizeof(wifi_fw) / sizeof(wifi_fw[0]) ); k++ ) {
		for ( i = 0; name[i] != '\0' && name[i] == wifi_fw[k].name[i]; i++ ) ;
		if ( name[i] == '\0' && wifi_fw[k].name[i] == '\0' ) break;
	}
	if ( k == (INT)( sizeof(wifi_fw) / sizeof(wifi_fw[0]) ) ) {
		tm_printf((UB *)"wifi: firmware %s: not one the system has\n", name);
		return NB_ENOENT;
	}
	W_UNSCHED(n);
	if ( ts_str_to_uuid(wifi_fw[k].uuid, &u) >= E_OK ) {
		key = ob_opn_obj(&u, OB_OP_R);		/* the record read, and the list of records for its size */
	}
	if ( key > 0 ) {
		er = ob_lst_rec(key, r, 4, &cnt);
		for ( i = 0; er >= E_OK && i < cnt; i++ ) {
			if ( r[i].recno == 1 ) sz = r[i].size;
		}
		if ( sz == 0 || sz > 8 * 1024 * 1024 ) {
			er = E_NOEXS;
		} else if ( ( buf = (UB *)Kmalloc((size_t)sz) ) == NULL ) {
			er = E_NOMEM;
		} else {
			er = ob_rea_rec(key, 1, 0, buf, (SZ)sz, &asz);
			if ( er >= E_OK && (UD)asz != sz ) er = E_IO;
		}
		ob_cls_obj(key);
	} else {
		er = (ER)key;
	}
	W_SCHED(n);
	if ( er < E_OK ) {
		if ( buf != NULL ) Kfree(buf);
		tm_printf((UB *)"wifi: firmware %s: the object %s could not be read (%d)\n",
			  name, wifi_fw[k].uuid, (INT)er);
		return ( er == E_NOMEM ) ? NB_ENOMEM : NB_ENOENT;
	}
	tm_printf((UB *)"wifi: firmware %s: %d bytes from the object %s\n", name, (INT)sz, wifi_fw[k].uuid);
	wifi_watch_start();
	*data = buf;
	*size = sz;
	return 0;
}

EXPORT void rumpcomp_tswifi_fw_close( void *data, UD size )
{
	(void)size;
	if ( data != NULL ) Kfree(data);
}

/* ---------------------------------------------------------------- frames and words */

/* A frame the card's interface received: to the manager, which does not wait */
EXPORT void rumpcomp_tswifi_input( CONST void *frame, UD len )
{
	knl_wifi_rx((CONST UB *)frame, (INT)len);
}

EXPORT void rumpcomp_tswifi_say( CONST char *s )
{
	tm_putstring((CONST UB *)s);
}

#endif /* X64 */
