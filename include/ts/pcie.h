/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pcie.h
 *	PCI Express host bridge (design 4.6, 10.4, phase 8)
 *
 *	Configuration space is reached the enhanced way: one megabyte per
 *	bus, laid out so that the address of a register is the base plus the
 *	bus, device, function and offset packed together. No port
 *	instructions are involved, which is what makes the same code work on
 *	a machine with no such instructions at all.
 *
 *	A kernel started without firmware finds the bridges and the cards
 *	but no addresses: nothing has given the base registers anywhere to
 *	live. So the walk also hands out the windows, from the range the
 *	board says belongs to the bus.
 */

#ifndef __TS_PCIE_H__
#define __TS_PCIE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Registers every function has */
#define PCI_VENDOR_ID		0x00
#define PCI_DEVICE_ID		0x02
#define PCI_COMMAND		0x04
#define PCI_STATUS		0x06
#define PCI_REVISION		0x08
#define PCI_PROG_IF		0x09
#define PCI_SUBCLASS		0x0a
#define PCI_CLASS		0x0b
#define PCI_CACHE_LINE		0x0c
#define PCI_HEADER_TYPE		0x0e
#define PCI_BAR0		0x10
#define PCI_PRIMARY_BUS		0x18
#define PCI_SECONDARY_BUS	0x19
#define PCI_SUBORDINATE_BUS	0x1a
#define PCI_CAP_PTR		0x34
#define PCI_INT_LINE		0x3c
#define PCI_INT_PIN		0x3d

#define PCI_CMD_IO		0x0001
#define PCI_CMD_MEMORY		0x0002
#define PCI_CMD_MASTER		0x0004
#define PCI_CMD_INTX_OFF	0x0400

#define PCI_HDR_MULTI		0x80
#define PCI_HDR_TYPE_MASK	0x7f
#define PCI_HDR_NORMAL		0x00
#define PCI_HDR_BRIDGE		0x01

#define PCI_BAR_IO		0x0001
#define PCI_BAR_TYPE_MASK	0x0006
#define PCI_BAR_TYPE_64		0x0004
#define PCI_BAR_PREFETCH	0x0008

/* Capabilities that are looked for by number */
#define PCI_CAP_MSI		0x05
#define PCI_CAP_MSIX		0x11
#define PCI_CAP_EXPRESS		0x10

#define PCI_MAX_BAR		6

/* What the walk found about one function */
typedef struct {
	UH	vendor;
	UH	device;
	UH	bus;			/* on x64 with its segment above the low eight bits (PCI_BUS_IN) */
	UB	dev, fn;
	UB	class_code;		/* base class */
	UB	subclass;
	UB	prog_if;
	UB	header;
	UB	int_pin;		/* 0 when it raises no line */
	UINT	intno;			/* what that line is, 0 when none */
	UD	bar[PCI_MAX_BAR];	/* where each window went, 0 if unused */
	UD	bar_size[PCI_MAX_BAR];
	UB	bar_is_io[PCI_MAX_BAR];
	UB	bar_is_64[PCI_MAX_BAR];
} T_PCIDEV;

/*
 * Walk the bus. Called once while the system comes up; answers how many
 * functions were found, or an error.
 */
IMPORT INT knl_pcie_init( void );

/*
 * The same walk, for a board that had to bring its own controller up
 * first and so knows the range of addresses itself.
 */
IMPORT INT knl_pcie_scan( UD mmio_base, UD mmio_size );

/*
 * A board whose configuration space is not one flat window provides
 * these two and defines PCIE_CFG_BOARD; the walk then reaches every
 * function through them.
 */
IMPORT UW   knl_pcie_cfg_in( UINT bus, UINT dev, UINT fn, UINT off, INT width );
IMPORT void knl_pcie_cfg_out( UINT bus, UINT dev, UINT fn, UINT off,
			      UW val, INT width );

/*
 * The functions the walk found. ts_pcie_find answers the index-th one
 * whose class, subclass and interface match; a subclass or interface of
 * 0xff matches anything.
 */
IMPORT INT ts_pcie_count( void );
IMPORT ER  ts_pcie_get( INT index, T_PCIDEV *dev );
IMPORT ER  ts_pcie_find( UINT class_code, UINT subclass, UINT prog_if,
			 INT index, T_PCIDEV *dev );

/* Let a function answer on its windows and reach memory by itself */
IMPORT ER  ts_pcie_enable( CONST T_PCIDEV *dev );

/* Where a window ended up, as the kernel sees it */
IMPORT UBINT ts_pcie_bar_base( CONST T_PCIDEV *dev, INT bar );

/* Read and write configuration space of one function */
IMPORT UW  ts_pcie_cfg_read( UINT bus, UINT dev, UINT fn, UINT off, INT width );
IMPORT void ts_pcie_cfg_write( UINT bus, UINT dev, UINT fn, UINT off,
			       UW val, INT width );

/*
 * Give a function one message interrupt of its own, and answer the
 * number it will raise. A message is a write the card makes, so it needs
 * no wire and shares nothing with the other cards. E_NOSPT when the
 * function has no messages, E_NOEXS when the machine has nowhere to
 * send them.
 */
IMPORT ER  ts_pcie_msi_alloc( CONST T_PCIDEV *dev, UINT *p_intno );

#ifdef __x86_64__
/* The same through the plain message capability (MSI), for a function that has no MSI-X (ts_pcie_irq_alloc tries it) */
IMPORT ER  ts_pcie_msi_plain_alloc( CONST T_PCIDEV *dev, UINT *p_intno );
#endif

/*
 * One interrupt for a function, the way the function and the machine
 * allow, tried in this order: MSI-X, MSI (x64), its line (INTx; on x64
 * the GSI the ACPI _PRT tables name, taken only when no other handler
 * has it), and only when line is TRUE. Answers the kind (PCI_IRQ_*), or
 * an error; *p_intno is 0 for PCI_IRQ_POLL, when the driver polls.
 * The interrupt goes where SetIntTarget and the device's object say,
 * as any other does. The console is told which it was, and why the
 * earlier ones were not had.
 */
#define PCI_IRQ_POLL		0
#define PCI_IRQ_MSIX		1
#define PCI_IRQ_MSI		2
#define PCI_IRQ_INTX		3
IMPORT INT ts_pcie_irq_alloc( CONST T_PCIDEV *dev, UINT *p_intno, BOOL line );

/* "MSI-X", "MSI", "INTx" or "polled" */
IMPORT CONST char *ts_pcie_irq_name( INT kind );

/* The kind ts_pcie_irq_alloc gave the function and its number, E_NOEXS when it was not asked */
IMPORT INT ts_pcie_irq_kind( CONST T_PCIDEV *dev, UINT *p_intno );

#ifdef __x86_64__
/*
 * A segment: a bus behind a host function, with its own numbering and its
 * own configuration space. Its functions are in the same table as any
 * other's and are used the same way; their bus numbers have the segment
 * above the low eight bits, so that the bus of a function names the
 * segment too. The host says how to reach the space and what a function
 * in it has to write to raise an interrupt.
 */
#define PCI_BUS_SEG(b)		( (UINT)(b) >> 8 )
#define PCI_BUS_NUM(b)		( (UINT)(b) & 0xff )
#define PCI_BUS_IN(seg, b)	( ( (UINT)(seg) << 8 ) | (UINT)(b) )

#define PCI_DOMAIN_RANGES	2

typedef struct {
	void	*arg;
	UW	(*cfg_rd)( void *arg, UINT bus, UINT dev, UINT fn, UINT off, INT width );
	void	(*cfg_wr)( void *arg, UINT bus, UINT dev, UINT fn, UINT off, UW val, INT width );
	/*
	 * A number for a function's message, and the address and data it
	 * writes. *p_routed TRUE when the host turns that into its own
	 * message, so that the function's entry never changes after this
	 */
	ER	(*msg)( void *arg, CONST T_PCIDEV *dev, UINT *p_intno, UD *p_addr, UW *p_data, BOOL *p_routed );
	UINT	bus_first, bus_last;	/* the numbers of the segment's buses */
	INT	nrange;			/* where its functions' windows may go */
	UD	range_next[PCI_DOMAIN_RANGES];
	UD	range_end[PCI_DOMAIN_RANGES];
	UH	host_bus;		/* the function that is its host */
	UB	host_dev, host_fn;
	BOOL	read_before_handler;	/* see ts_pcie_irq_sync */
} T_PCIDOMAIN;

IMPORT INT  knl_pcie_domain_add( T_PCIDOMAIN *g );
IMPORT BOOL knl_pcie_is_domain_host( CONST T_PCIDEV *dev );
IMPORT ER   knl_pcie_msix_at( CONST T_PCIDEV *dev, UINT index, UINT *p_intno );
IMPORT ER   knl_pcie_msg_direct( UINT *p_intno, UD *p_addr, UW *p_data );
IMPORT void ts_pcie_irq_sync( CONST T_PCIDEV *dev );
#ifdef USE_KTEST
IMPORT void knl_pcie_hide( UINT bus, UINT dev, UINT fn );
IMPORT void knl_pcie_fake_amd( void );
#endif
#endif

/* The offset of a capability, or 0 when the function has none of it */
IMPORT UINT ts_pcie_cap_find( CONST T_PCIDEV *dev, UINT cap_id );

#ifdef __cplusplus
}
#endif

#endif /* __TS_PCIE_H__ */
