/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lanpci.h (x64 PC)
 *	What the PCIe LAN drivers have in common (lanpci.c): the memory of
 *	the rings, sending and receiving a copy at a time, the interrupt,
 *	the machine's sleep, the link's line on the console, and the card as
 *	the selection of netsel.c sees it. A driver gives what differs by its
 *	chip.
 */

#ifndef __LANPCI_H__
#define __LANPCI_H__

#include <ts/net.h>
#include <ts/pcie.h>
#include <ts/pm.h>
#include "../netsel.h"

#define LAN_NRX		64
#define LAN_NTX		32
#define LAN_BUF		2048

typedef struct t_lan	T_LAN;

/* What depends on the chip */
typedef struct {
	/* From its reset to receiving and sending, on the rings; at start-up and after a sleep */
	void	(*hw_start)( T_LAN *l );
	void	(*hw_stop)( T_LAN *l );
	/* The interrupts it may raise: on, and acknowledged; TRUE when the link is why */
	void	(*irq_on)( T_LAN *l );
	void	(*irq_off)( T_LAN *l );
	BOOL	(*irq_ack)( T_LAN *l );
	/* The link: its speed in Mb/s and whether it is full duplex; FALSE while it is down */
	BOOL	(*link)( T_LAN *l, INT *p_mbps, BOOL *p_fdx );
	/* Slot i of the ring: the frame's length or NIC_RX_*, and given back to the chip */
	INT	(*rx_take)( T_LAN *l, UINT i );
	void	(*rx_give)( T_LAN *l, UINT i );
	/* Slot i can take a frame; the frame of len bytes in its buffer sent */
	BOOL	(*tx_free)( T_LAN *l, UINT i );
	void	(*tx_go)( T_LAN *l, UINT i, UINT len );
} T_LANOPS;

struct t_lan {
	/* Set by the driver before knl_lan_start */
	CONST T_LANOPS	*ops;
	CONST char	*card;		/* its name among the adapters, as NET_ADAPTER names it */
	CONST char	*chip;		/* what it is, for the console and the adapters' list */
	T_PCIDEV	pci;
	UBINT		base;
	UW		rev;		/* the chip's revision, as the console says it */
	UB		mac[NET_MAC_LEN];
	INT		irq_kind;	/* what ts_pcie_irq_alloc or the driver had: PCI_IRQ_* */
	UINT		intno;

	/* The rings */
	void		*rx, *tx;
	UD		rx_pa, tx_pa;
	UB		*rxbuf, *txbuf;
	UD		rxbuf_pa, txbuf_pa;
	UINT		rx_next, tx_next;

	/* Kept by lanpci.c */
	BOOL		ok;
	volatile BOOL	asleep;
	ID		flg, tx_mtx, rx_mtx;
	UD		n_sent, n_recv, n_drop;
	volatile UW	n_irq;
	BOOL		link_was;
	INT		mbps_was;
	T_PMDEV		pm;
	T_NETDRV	drv;
};

/* The driver's interrupt handler passes here */
IMPORT void	knl_lan_inthdr( T_LAN *l, UINT n );

/*
 * Memory for the rings, the card started, its interrupt (asked for by the
 * driver beforehand, intno 0 when it is to be polled) and its sleep set up,
 * and the card given to the selection as number c
 */
IMPORT INT	knl_lan_start( T_LAN *l, INT c );

/* The line every driver says, here and at each change: "net: <chip> <id> link ..." */
IMPORT void	knl_lan_say( T_LAN *l );

/* A chip the driver matched by its ID and does not know, for the machine's record */
IMPORT void	knl_lan_refuse( CONST char *chip, CONST T_PCIDEV *pci, UW rev, CONST char *why );

#endif /* __LANPCI_H__ */
