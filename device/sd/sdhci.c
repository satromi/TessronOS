/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sdhci.c
 *	SD host controller of the Raspberry Pi 5 (design 10.6, 4.2).
 *
 *	The registers follow the SD Host Controller specification; the
 *	block sits at 0x10_00ff_f000 with SPI 273. The bus is four bits
 *	wide and the clock is left at the identification speed raised to
 *	25MHz.
 *
 *	Data moves one of three ways. ADMA2 walks a table of descriptors,
 *	each naming a length and an address, so one command carries as much
 *	as the table can describe. The single address mode takes one address
 *	and asks for the next at every buffer boundary. The programmed path
 *	moves words through the data port and needs no memory the controller
 *	can reach at all. Which one is used comes from the capability
 *	register, which says whether the controller has the descriptor mode
 *	and whether its addresses are 64 bits wide; CNF_SD_DMA_MODE forces
 *	the choice. The programmed path stays and stays selectable because
 *	it is the only one the others can be compared against when the board
 *	is first tried, and a transfer no descriptor table can describe - a
 *	buffer outside the linear map of memory, or one needing more
 *	descriptors than the table holds - falls back to it on its own.
 *
 *	The controller masters the SoC's own bus rather than sitting behind
 *	the PCIe bridge, so an address written into a descriptor is the
 *	physical address with nothing added to it. Nothing on this board is
 *	cache coherent with a bus master, so the table and the data are
 *	cleaned out of the cache before a transfer and invalidated after it.
 *
 *	The card is registered as the disk device "sda" with its partitions
 *	as subunits, the same shape the virtio block driver uses, so the
 *	file system layer above does not know which one it is talking to.
 *
 *	The same driver runs under QEMU on the SD host controller the
 *	machine offers as a PCI function (sdhci-pci, class 08 05), whose
 *	registers are the same standard ones behind the function's first
 *	window. That is where the commands, the three ways of moving data
 *	and the partition offsets are tested against a card image
 *	(tests/ktest/ktest_sd.c). On the board itself it is not yet
 *	verified (docs/private/checklists/phase7.md items 19, 20, 34a to 34e).
 */

#include <sys/machine.h>

#if defined(RPI5) || defined(QEMU_VIRT)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#ifdef RPI5
#include <ts/gpio.h>
#else
#include <ts/pcie.h>
#endif
#include "sysman/pfalloc.h"
#include "sysdepend.h"

#ifdef QEMU_VIRT
/* the registers are wherever the bus walk put the function's window */
LOCAL UBINT	sd_base;
#define SDHCI_BASE		sd_base
#define PCI_CLASS_SDHCI		0x08		/* base system peripheral */
#define PCI_SUBCLASS_SDHCI	0x05		/* SD host controller */
#endif

/* ---------------------------------------------------------------- registers */
#define SDHCI_ARGUMENT2		(SDHCI_BASE + 0x00)
#define SDHCI_SDMA_ADDRESS	(SDHCI_BASE + 0x00)	/* the same word, in the single address mode */
#define SDHCI_BLOCK_SIZE	(SDHCI_BASE + 0x04)	/* 16 bit */
#define SDHCI_BLOCK_COUNT	(SDHCI_BASE + 0x06)	/* 16 bit */
#define SDHCI_ARGUMENT		(SDHCI_BASE + 0x08)
#define SDHCI_TRANSFER_MODE	(SDHCI_BASE + 0x0c)	/* 16 bit */
#define SDHCI_COMMAND		(SDHCI_BASE + 0x0e)	/* 16 bit */
#define SDHCI_RESPONSE(i)	(SDHCI_BASE + 0x10 + (i) * 4)
#define SDHCI_BUFFER		(SDHCI_BASE + 0x20)
#define SDHCI_PRESENT_STATE	(SDHCI_BASE + 0x24)
#define SDHCI_HOST_CONTROL	(SDHCI_BASE + 0x28)	/* 8 bit */
#define SDHCI_POWER_CONTROL	(SDHCI_BASE + 0x29)	/* 8 bit */
#define SDHCI_CLOCK_CONTROL	(SDHCI_BASE + 0x2c)	/* 16 bit */
#define SDHCI_TIMEOUT_CONTROL	(SDHCI_BASE + 0x2e)	/* 8 bit */
#define SDHCI_SOFTWARE_RESET	(SDHCI_BASE + 0x2f)	/* 8 bit */
#define SDHCI_INT_STATUS	(SDHCI_BASE + 0x30)
#define SDHCI_INT_ENABLE	(SDHCI_BASE + 0x34)
#define SDHCI_SIGNAL_ENABLE	(SDHCI_BASE + 0x38)
#define SDHCI_HOST_CONTROL2	(SDHCI_BASE + 0x3e)	/* 16 bit */
#define SDHCI_CAPABILITIES	(SDHCI_BASE + 0x40)
#define SDHCI_CAPABILITIES1	(SDHCI_BASE + 0x44)
#define SDHCI_ADMA_ERROR	(SDHCI_BASE + 0x54)	/* 8 bit */
#define SDHCI_ADMA_ADDRESS	(SDHCI_BASE + 0x58)	/* 64 bit; the low word alone when addresses are 32 bit */

/* Present state */
#define STATE_CMD_INHIBIT	0x00000001
#define STATE_DAT_INHIBIT	0x00000002
#define STATE_CARD_INSERTED	0x00010000
#define STATE_BUF_WRITE_EN	0x00000400
#define STATE_BUF_READ_EN	0x00000800

/* Interrupt status (the low half is normal, the high half is error) */
#define INT_CMD_COMPLETE	0x00000001
#define INT_XFER_COMPLETE	0x00000002
#define INT_DMA			0x00000008	/* a buffer boundary was reached */
#define INT_BUF_WRITE_READY	0x00000010
#define INT_BUF_READ_READY	0x00000020
#define INT_CARD_INSERT		0x00000040
#define INT_ERROR		0x00008000
#define INT_ERROR_MASK		0xFFFF0000
#define INT_ADMA_ERROR		0x02000000	/* the descriptor table was refused */

/* Software reset */
#define RESET_ALL		0x01
#define RESET_CMD		0x02
#define RESET_DATA		0x04

/* Clock control */
#define CLOCK_INTERNAL_EN	0x0001
#define CLOCK_INTERNAL_STABLE	0x0002
#define CLOCK_CARD_EN		0x0004

/* Host control 1 */
#define CTRL_4BITBUS		0x02
#define CTRL_CD_TEST		0xC0		/* card detect from the test level, which says inserted */
#define CTRL_DMA_MASK		0x18		/* which kind of transfer the controller runs */
#define CTRL_DMA_SDMA		0x00
#define CTRL_DMA_ADMA2_32	0x10
#define CTRL_DMA_ADMA2_64	0x18

/*
 * Capabilities. The controller says here which transfer modes it has and
 * how wide the addresses it masters with are.
 */
#define CAP_ADMA2		0x00080000	/* the descriptor table mode */
#define CAP_SDMA		0x00400000	/* the single address mode */
#define CAP_64BIT		0x10000000	/* addresses are 64 bits wide */

/* Transfer mode */
#define XFER_DMA_EN		0x0001
#define XFER_BLKCNT_EN		0x0002
#define XFER_AUTO_CMD12		0x0004
#define XFER_READ		0x0010
#define XFER_MULTI		0x0020

/* Command flags */
#define CMD_RESP_NONE		0x0000
#define CMD_RESP_136		0x0001
#define CMD_RESP_48		0x0002
#define CMD_RESP_48_BUSY	0x0003
#define CMD_CRC_CHECK		0x0008
#define CMD_INDEX_CHECK		0x0010
#define CMD_DATA_PRESENT	0x0020

#define CMD(index, flags)	(((index) << 8) | (flags))

/* SD commands */
#define CMD_GO_IDLE		CMD(0,  CMD_RESP_NONE)
#define CMD_ALL_SEND_CID	CMD(2,  CMD_RESP_136 | CMD_CRC_CHECK)
#define CMD_SEND_REL_ADDR	CMD(3,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SELECT_CARD		CMD(7,  CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SEND_IF_COND	CMD(8,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SEND_CSD		CMD(9,  CMD_RESP_136 | CMD_CRC_CHECK)
#define CMD_STOP_XFER		CMD(12, CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SET_BLOCKLEN	CMD(16, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_READ_SINGLE		CMD(17, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_READ_MULTI		CMD(18, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_WRITE_SINGLE	CMD(24, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_WRITE_MULTI		CMD(25, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_APP_CMD		CMD(55, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define ACMD_SET_BUS_WIDTH	CMD(6,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define ACMD_SEND_OP_COND	CMD(41, CMD_RESP_48)

#define OCR_VOLTAGE		0x00FF8000	/* 2.7 to 3.6 volts */
#define OCR_HCS			0x40000000	/* high capacity supported */
#define OCR_BUSY		0x80000000	/* the card has finished */

/* Card status (R1) and the write protection the card data holds */
#define R1_CARD_IS_LOCKED	0x02000000	/* a password is set and not given */
#define CSD_PERM_WP		0x00002000	/* bit 13: for good */
#define CSD_TMP_WP		0x00001000	/* bit 12: until cleared */
#define SD_WP_TMP		0x01
#define SD_WP_PERM		0x02
#define SD_WP_LOCKED		0x04

#define SD_SPIN			1000000		/* polling attempts */

/*
 * How long a transfer may take. The waits spin a little and then sleep a
 * millisecond at a time, so that a slow card is waited for by time, not
 * by how fast the processor reads a register: a card may take a quarter
 * of a second over one block it is programming, and much longer over a
 * large write when it has to erase first.
 */
#define SD_QUICK_SPIN		20000		/* reads before the first sleep */
#define SD_INT_MS		1000		/* one command or one block */
#define SD_XFER_MS(n)		( 1000 + (INT)(n) * 2 )	/* a whole transfer of n blocks */

#define CMD_SEND_STATUS		CMD(13, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define R1_STATE(r)		( ( (r) >> 9 ) & 0xf )
#define R1_STATE_TRAN		4
#define R1_READY_FOR_DATA	0x00000100

/*
 * Block size register: the high bits say how far the single address mode
 * runs before it stops and asks for the address of the rest. The largest
 * step there is keeps the interruptions rare.
 */
#define BLKSZ_SDMA_BOUND_512K	0x7000
#define SDMA_BOUND		0x80000

/*
 * ADMA2 descriptors. A descriptor is a set of attribute bits, a length
 * and an address: two words when addresses are 32 bits wide, three when
 * they are 64. They are written word by word rather than through a
 * structure so that no padding can get between the fields.
 */
#define ADMA_ATTR_VALID		0x0001
#define ADMA_ATTR_END		0x0002		/* the last one of the table */
#define ADMA_ATTR_INT		0x0004
#define ADMA_ATTR_TRAN		0x0020		/* move data, as against link or skip */

/*
 * One descriptor carries at most 32KB. The field could say 64KB (as 0),
 * but not every controller reads a zero length that way, and the table
 * has room enough either way.
 */
#define ADMA_MAX_LEN		0x8000
#define ADMA_DESC_MAX		256		/* 8MB in one transfer, 3KB of table */

/*
 * A buffer the controller moves data into or out of starts on a cache
 * line. A read into one that shares its first or last line with other
 * data loses: whatever is written to that other data while the transfer
 * runs brings the line back into the cache, and the invalidate after the
 * transfer writes the line, stale half and all, over what the controller
 * put there. Such a buffer goes through the driver's own bounce buffer,
 * whose lines are its alone.
 */
#define DMA_ALIGN		64
#define BOUNCE_ORDER		4		/* 64KB */
#define BOUNCE_SECT		( ( PAGE_SIZE << BOUNCE_ORDER ) / BLK_SECTOR_SIZE )

/*
 * Sectors in one command. The block count register is 16 bits wide; a
 * larger request is sent as several commands.
 */
#define SD_XFER_MAX		8192

/* How data moves */
#define SD_DMA_NONE		0		/* through the data port */
#define SD_DMA_SDMA		1		/* one address, renewed at each boundary */
#define SD_DMA_ADMA2_32		2		/* descriptor table, 32 bit addresses */
#define SD_DMA_ADMA2_64		3		/* descriptor table, 64 bit addresses */

/*
 * Which of them to use. A negative value takes what the capability
 * register offers; 0 to 3 force one, which is how the programmed path is
 * kept selectable for comparison on a board.
 */
#ifndef CNF_SD_DMA_MODE
#define CNF_SD_DMA_MODE		(-1)
#endif

typedef struct {
	UD	nsect;			/* sectors of the card */
	UW	rca;			/* relative address, in the high half */
	BOOL	hc;			/* block addressed (SDHC and up) */
	BOOL	present;
	UB	wp;			/* SD_WP_*: why the card takes no writes, 0 when it does */
	INT	dma_mode;		/* SD_DMA_* */
	UW	*desc;			/* the descriptor table, when there is one */
	UD	desc_pa;
	UB	*bounce;		/* for buffers off a cache line, NULL without */
	ID	mtxid;
	ID	devid;
	T_PARTTBL tbl;
	UB	devnm[8];
} SDCARD;

LOCAL SDCARD	sd;
LOCAL INT	sd_step;	/* how far sd_init_card came, for the message when it fails */

/* ---------------------------------------------------------------- low level */

/*
 * Every register is reached with 32 bit accesses only. The narrower
 * registers of the specification share words; one is written by reading
 * its word, changing its bytes and writing the word back. Two words need
 * care. The transfer mode and the command share one, and writing the
 * command starts it, so the mode is kept aside and goes out in the same
 * write as the command. The word of the clock control also holds the
 * software reset bits, which are written back as zero so that changing
 * the clock never starts a reset.
 */
LOCAL UH	sd_xfer_mode;		/* the transfer mode for the next command */

#define SD_WORD(r)		((r) & ~(UBINT)3)
#define SD_SHIFT(r)		(((r) & 3) * 8)
#define SD_RESET_BITS		0xFF000000	/* byte 3 of the word at 0x2c */

LOCAL UW sd_in8( UBINT r )
{
	return ( in_w(SD_WORD(r)) >> SD_SHIFT(r) ) & 0xff;
}

LOCAL UW sd_in16( UBINT r )
{
	return ( in_w(SD_WORD(r)) >> SD_SHIFT(r) ) & 0xffff;
}

LOCAL UW sd_keep( UBINT r, UW w )
{
	return ( SD_WORD(r) == SD_WORD(SDHCI_SOFTWARE_RESET) ) ? ( w & ~(UW)SD_RESET_BITS ) : w;
}

LOCAL void sd_out8( UBINT r, UW v )
{
	UW	w = sd_keep(r, in_w(SD_WORD(r)));

	w &= ~( (UW)0xff << SD_SHIFT(r) );
	out_w(SD_WORD(r), w | ( ( v & 0xff ) << SD_SHIFT(r) ));
}

LOCAL void sd_out16( UBINT r, UW v )
{
	UW	w;

	if ( r == SDHCI_TRANSFER_MODE ) {
		sd_xfer_mode = (UH)v;		/* goes out with the command */
		return;
	}
	if ( r == SDHCI_COMMAND ) {
		out_w(SDHCI_TRANSFER_MODE, ( ( v & 0xffff ) << 16 ) | sd_xfer_mode);
		sd_xfer_mode = 0;
		return;
	}
	w = sd_keep(r, in_w(SD_WORD(r)));
	w &= ~( (UW)0xffff << SD_SHIFT(r) );
	out_w(SD_WORD(r), w | ( ( v & 0xffff ) << SD_SHIFT(r) ));
}

LOCAL void sd_out64( UBINT r, UD v )
{
	out_w(r, (UW)v);
	out_w(r + 4, (UW)( v >> 32 ));
}

LOCAL UD sd_in64( UBINT r )
{
	return (UD)in_w(r) | ( (UD)in_w(r + 4) << 32 );
}

LOCAL ER wait_state( UW mask, INT spin )
{
	INT	i;

	for ( i = 0; i < spin; i++ ) {
		if ( (in_w(SDHCI_PRESENT_STATE) & mask) == 0 ) return E_OK;
	}
	return E_TMOUT;
}

/* a millisecond's sleep, or a moment's pause where the caller cannot sleep */
LOCAL void sd_nap( void )
{
	if ( tk_dly_tsk(1) != E_OK ) {
		Asm("yield");
	}
}

LOCAL ER wait_int_ms( UW mask, UW *p_status, INT ms )
{
	UW	st;
	INT	i, slept = 0;

	for (;;) {
		for ( i = 0; i < SD_QUICK_SPIN; i++ ) {
			st = in_w(SDHCI_INT_STATUS);
			if ( (st & INT_ERROR_MASK) != 0 ) {
				out_w(SDHCI_INT_STATUS, st);	/* clear and report */
				if ( p_status != NULL ) *p_status = st;
				return E_IO;
			}
			if ( (st & mask) != 0 ) {
				out_w(SDHCI_INT_STATUS, st & mask);
				if ( p_status != NULL ) *p_status = st;
				return E_OK;
			}
		}
		if ( slept++ >= ms ) {
			return E_TMOUT;
		}
		sd_nap();
	}
}

LOCAL ER wait_int( UW mask, UW *p_status )
{
	return wait_int_ms(mask, p_status, SD_INT_MS);
}

/*
 * Wait until the controller has finished with the last transfer and the
 * card with the last write. The block size and count, the transfer mode
 * and the DMA addresses may not be written before that: while the data
 * lines are in use the controller keeps its old values, and the next
 * command would run with the last one's direction and length. A card
 * may take a quarter of a second over programming what it was sent.
 */
LOCAL ER sd_wait_idle( void )
{
	INT	i;

	for ( i = 0; i < 1000; i++ ) {
		if ( wait_state(STATE_CMD_INHIBIT | STATE_DAT_INHIBIT, 10000) == E_OK ) {
			return E_OK;
		}
		if ( tk_dly_tsk(1) != E_OK ) {
			Asm("yield");
		}
	}
	return E_TMOUT;
}

/*
 * One command. 'flags' carries the response kind and whether data follows.
 */
LOCAL ER sd_cmd( UH cmd, UW arg, UW *resp )
{
	ER	er;

	er = wait_state(STATE_CMD_INHIBIT | (( (cmd & CMD_DATA_PRESENT) != 0 ) ? STATE_DAT_INHIBIT : 0),
			SD_SPIN);
	if ( er < E_OK ) {
		return er;
	}
	/*
	 * Nothing left over from an earlier command may be taken for this
	 * one's: a command that holds the card busy (R1b) ends with the
	 * same transfer complete bit a data transfer ends with, and a wait
	 * that found it already set would return before its own transfer
	 * had moved anything.
	 */
	out_w(SDHCI_INT_STATUS, 0xFFFFFFFF);
	out_w(SDHCI_ARGUMENT, arg);
	sd_out16(SDHCI_COMMAND, cmd);

	er = wait_int(INT_CMD_COMPLETE, NULL);
	if ( er < E_OK ) {
		return er;
	}
	if ( (cmd & 3) == CMD_RESP_48_BUSY && (cmd & CMD_DATA_PRESENT) == 0 ) {
		/* the end of the busy time, which not every controller reports */
		(void)wait_int(INT_XFER_COMPLETE, NULL);
		(void)wait_state(STATE_DAT_INHIBIT, SD_SPIN);
	}
	if ( resp == NULL ) {
		/* nothing to read */
	} else if ( (cmd & 3) == CMD_RESP_136 ) {
		/*
		 * The controller keeps bits 127:8 of a long response in bits
		 * 119:0 of the four registers (the CRC is not kept). Shifted
		 * back up a byte, resp[3] holds bits 127:96 and resp[0]
		 * bits 31:0, with the CRC's byte zero.
		 */
		UW	r[4];
		INT	i;

		for ( i = 0; i < 4; i++ ) {
			r[i] = in_w(SDHCI_RESPONSE(i));
		}
		resp[3] = (r[3] << 8) | (r[2] >> 24);
		resp[2] = (r[2] << 8) | (r[1] >> 24);
		resp[1] = (r[1] << 8) | (r[0] >> 24);
		resp[0] = r[0] << 8;
	} else {
		resp[0] = in_w(SDHCI_RESPONSE(0));
	}

	return E_OK;
}

/* An application command is a CMD55 followed by the command itself */
LOCAL ER sd_acmd( UH cmd, UW arg, UW *resp )
{
	ER	er = sd_cmd(CMD_APP_CMD, sd.rca, NULL);

	if ( er < E_OK ) {
		return er;
	}
	return sd_cmd(cmd, arg, resp);
}

LOCAL ER sd_reset( UB what )
{
	INT	i;

	out_w(SD_WORD(SDHCI_SOFTWARE_RESET), ( in_w(SD_WORD(SDHCI_SOFTWARE_RESET)) & ~(UW)SD_RESET_BITS ) | ( (UW)what << 24 ));
	for ( i = 0; i < SD_SPIN; i++ ) {
		if ( (sd_in8(SDHCI_SOFTWARE_RESET) & what) == 0 ) return E_OK;
	}
	return E_TMOUT;
}

/*
 * The divider of the base clock that gives at most 'hz'. The 8 bit
 * divided clock mode of the specification is used.
 */
LOCAL ER sd_set_clock( UW hz )
{
	UW	base, div;
	UH	val;
	INT	i;

	base = (in_w(SDHCI_CAPABILITIES) >> 8) & 0xff;	/* MHz */
	if ( base == 0 ) base = 100;			/* the field may read zero */
	base *= 1000000;

	for ( div = 1; div < 2048; div <<= 1 ) {
		if ( base / div <= hz ) break;
	}
	div >>= 1;					/* the register holds div/2 */

	sd_out16(SDHCI_CLOCK_CONTROL, 0);
	val = (UH)(((div & 0xff) << 8) | ((div & 0x300) >> 2) | CLOCK_INTERNAL_EN);
	sd_out16(SDHCI_CLOCK_CONTROL, val);

	for ( i = 0; i < SD_SPIN; i++ ) {
		if ( (sd_in16(SDHCI_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE) != 0 ) break;
	}
	if ( i >= SD_SPIN ) {
		return E_TMOUT;
	}
	sd_out16(SDHCI_CLOCK_CONTROL, (UH)(val | CLOCK_CARD_EN));

	return E_OK;
}

/*
 * After a transfer that failed or ran out of time: the controller's
 * command and data sides are reset, the card is told to stop whatever
 * transfer it is still in (CMD12; a card that is in none refuses it,
 * which does no harm), and it is waited for until it is back in the
 * transfer state and ready for data. Without this, a card left in the
 * middle of a write refuses every command after it.
 */
LOCAL void sd_recover( ER why )
{
	UW	resp[4];
	INT	i;

	sd_reset(RESET_CMD);
	sd_reset(RESET_DATA);
	(void)sd_cmd(CMD_STOP_XFER, 0, NULL);
	for ( i = 0; i < SD_INT_MS; i++ ) {
		resp[0] = 0;
		if ( sd_cmd(CMD_SEND_STATUS, sd.rca, resp) >= E_OK
		  && R1_STATE(resp[0]) == R1_STATE_TRAN
		  && (resp[0] & R1_READY_FOR_DATA) != 0 ) {
			break;
		}
		sd_nap();
	}
	tm_printf((UB*)"TessronOS: sd a transfer failed (%d); card status %08x after %d ms\n",
		  (INT)why, resp[0], i);
}

/* ---------------------------------------------------------------- transfer */

/*
 * Clean and invalidate a range, so that the controller sees what was
 * written and the processor keeps no stale line of what the controller
 * wrote. Nothing on this board is coherent with a bus master.
 */
LOCAL void dcache_flush( CONST void *p, UD len )
{
	UBINT	a = (UBINT)p & ~63ULL, end = (UBINT)p + len;

	Asm("dsb sy" ::: "memory");
	for ( ; a < end; a += 64 ) {
		Asm("dc civac, %0" :: "r"(a) : "memory");
	}
	Asm("dsb sy" ::: "memory");
}

/*
 * Only a buffer in the linear map of memory has a physical address that
 * subtraction finds, and only such a buffer is one unbroken run of
 * physical memory. Anything else has to go through the data port.
 */
LOCAL BOOL buf_is_linear( CONST void *p, UD len )
{
	UBINT	a = (UBINT)p;

	return ( a >= KVA_BASE && a + len > a && a + len <= DEV_VA_BASE );
}

/*
 * Set the block size and count, say what the transfer does, and send the
 * command that starts it. 'extra_mode' carries the transfer mode bits
 * that belong to the way the data moves, 'extra_blksz' the buffer
 * boundary of the single address mode.
 */
LOCAL ER sd_xfer_begin( UD start, UD nsect, BOOL write, UH extra_mode, UH extra_blksz )
{
	UW	arg = (UW)(( sd.hc ) ? start : start * BLK_SECTOR_SIZE);
	UH	mode;

	out_w(SDHCI_BLOCK_SIZE, (UW)(BLK_SECTOR_SIZE | extra_blksz) | ( (UW)nsect << 16 ));

	/*
	 * The block count runs down on a single block too, so that no
	 * transfer leaves a count behind: some controllers take a write of
	 * the single address register with blocks outstanding as the
	 * go-ahead for another transfer.
	 */
	mode = (( write ) ? 0 : XFER_READ) | XFER_BLKCNT_EN;
	if ( nsect > 1 ) {
		mode |= XFER_MULTI | XFER_AUTO_CMD12;
	}
	sd_out16(SDHCI_TRANSFER_MODE, (UH)(mode | extra_mode));

	return sd_cmd(( write ) ? (( nsect > 1 ) ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE)
			        : (( nsect > 1 ) ? CMD_READ_MULTI : CMD_READ_SINGLE),
		      arg, NULL);
}

/*
 * Move whole blocks through the data port, one word at a time. This path
 * asks nothing of the controller but the data register, which is what
 * makes it the one the others are measured against.
 */
LOCAL ER sd_xfer_pio( UD start, void *buf, UD nsect, BOOL write )
{
	UB	*p = (UB *)buf;
	UW	st, w;
	UD	i;
	ER	er;

	er = sd_xfer_begin(start, nsect, write, 0, 0);
	if ( er < E_OK ) {
		return er;
	}

	for ( i = 0; i < nsect; i++ ) {
		UD	k;

		er = wait_int(( write ) ? INT_BUF_WRITE_READY : INT_BUF_READ_READY, &st);
		if ( er < E_OK ) {
			sd_recover(er);
			return er;
		}
		/* byte by byte in memory: the buffer need not be word aligned */
		for ( k = 0; k < BLK_SECTOR_SIZE / 4; k++, p += 4 ) {
			if ( write ) {
				w = (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
				out_w(SDHCI_BUFFER, w);
			} else {
				w = in_w(SDHCI_BUFFER);
				p[0] = (UB)w;
				p[1] = (UB)(w >> 8);
				p[2] = (UB)(w >> 16);
				p[3] = (UB)(w >> 24);
			}
		}
	}

	er = wait_int_ms(INT_XFER_COMPLETE, NULL, SD_XFER_MS(nsect));
	if ( er < E_OK ) {
		sd_recover(er);
	}

	return er;
}

/*
 * Describe one unbroken run of physical memory as a descriptor table.
 * Each entry carries at most 65536 bytes, which the 16 bit length field
 * writes as zero; the last one is marked so the controller stops there.
 * Answers how many entries were used, or E_NOSPT when the table is too
 * small for the transfer.
 */
LOCAL ER adma_build( UD pa, UD len )
{
	UW	*d = sd.desc, *last = NULL;
	INT	n = 0;
	INT	step = ( sd.dma_mode == SD_DMA_ADMA2_64 ) ? 3 : 2;

	while ( len > 0 ) {
		UD	chunk = ( len > ADMA_MAX_LEN ) ? ADMA_MAX_LEN : len;

		if ( n >= ADMA_DESC_MAX ) {
			return E_NOSPT;
		}
		d[0] = (UW)(ADMA_ATTR_VALID | ADMA_ATTR_TRAN)
		     | ((UW)(chunk & 0xffff) << 16);
		d[1] = (UW)pa;
		if ( step == 3 ) {
			d[2] = (UW)(pa >> 32);
		}
		last = d;
		d  += step;
		pa += chunk;
		len -= chunk;
		n++;
	}
	if ( last == NULL ) {
		return E_PAR;
	}
	last[0] |= ADMA_ATTR_END;
	dcache_flush(sd.desc, (UD)n * (UD)step * 4);

	return (ER)n;
}

/*
 * Wait for a transfer the controller runs by itself. In the single
 * address mode it stops at every buffer boundary and wants the address
 * of the rest, which is what its own register then holds. A refused
 * descriptor table is reported separately from a refused transfer, and
 * the controller leaves the address of the descriptor it stopped on
 * where it can be read out.
 */
LOCAL ER sd_dma_wait( UD nsect )
{
	UW	st;
	INT	i = 0, slept = 0;

	for ( ;; i++ ) {
		if ( i >= SD_QUICK_SPIN ) {
			if ( slept++ >= SD_XFER_MS(nsect) ) {
				break;
			}
			sd_nap();
			i = 0;
		}
		st = in_w(SDHCI_INT_STATUS);

		if ( (st & INT_ERROR_MASK) != 0 ) {
			out_w(SDHCI_INT_STATUS, st);
			if ( (st & INT_ADMA_ERROR) != 0 ) {
				UD	at = sd_in64(SDHCI_ADMA_ADDRESS);

				tm_printf((UB*)"TessronOS: sd ADMA error %02x, descriptor at %08x%08x\n",
					  (UW)sd_in8(SDHCI_ADMA_ERROR),
					  (UW)(at >> 32), (UW)at);
			}
			return E_IO;
		}
		if ( (st & INT_DMA) != 0 ) {
			out_w(SDHCI_INT_STATUS, INT_DMA);
			out_w(SDHCI_SDMA_ADDRESS, in_w(SDHCI_SDMA_ADDRESS));
			continue;
		}
		if ( (st & INT_XFER_COMPLETE) != 0 ) {
			out_w(SDHCI_INT_STATUS, st & INT_XFER_COMPLETE);
			return E_OK;
		}
	}

	return E_TMOUT;
}

/*
 * Let the controller fetch the data itself. Answers E_NOSPT when this
 * buffer cannot be reached the chosen way, which sends the transfer
 * through the data port instead.
 */
LOCAL ER sd_xfer_dma( UD start, void *buf, UD nsect, BOOL write )
{
	UD	len = nsect * BLK_SECTOR_SIZE;
	UD	pa;
	UB	ctrl;
	UH	blksz = 0;
	ER	er;

	if ( !buf_is_linear(buf, len) || ((UBINT)buf & (DMA_ALIGN - 1)) != 0 ) {
		return E_NOSPT;
	}
	pa = (UD)VA2PA((UBINT)buf);
	ctrl = (UB)(sd_in8(SDHCI_HOST_CONTROL) & ~CTRL_DMA_MASK);

	switch ( sd.dma_mode ) {
	case SD_DMA_ADMA2_64:
		er = adma_build(pa, len);
		if ( er < E_OK ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_ADMA2_64));
		sd_out64(SDHCI_ADMA_ADDRESS, sd.desc_pa);
		break;

	case SD_DMA_ADMA2_32:
		if ( (pa + len) > 0x100000000ULL ) {
			return E_NOSPT;		/* the addresses do not reach it */
		}
		er = adma_build(pa, len);
		if ( er < E_OK ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_ADMA2_32));
		out_w(SDHCI_ADMA_ADDRESS, (UW)sd.desc_pa);
		break;

	case SD_DMA_SDMA:
		/* the single address register is 32 bits wide */
		if ( (pa + len) > 0x100000000ULL ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_SDMA));
		out_w(SDHCI_SDMA_ADDRESS, (UW)pa);
		blksz = BLKSZ_SDMA_BOUND_512K;
		break;

	default:
		return E_NOSPT;
	}

	dcache_flush(buf, len);

	er = sd_xfer_begin(start, nsect, write, XFER_DMA_EN, blksz);
	if ( er < E_OK ) {
		sd_recover(er);
		return er;
	}
	er = sd_dma_wait(nsect);
	if ( er < E_OK ) {
		sd_recover(er);
		return er;
	}
	if ( !write ) {
		dcache_flush(buf, len);	/* drop what was read ahead of the transfer */
	}

	return E_OK;
}

LOCAL ER sd_xfer( UD start, void *buf, UD nsect, BOOL write )
{
	UB	*p = (UB *)buf;
	UD	n;
	ER	er;

	while ( nsect > 0 ) {
		BOOL	pio = ( sd.dma_mode == SD_DMA_NONE );

		er = sd_wait_idle();
		if ( er < E_OK ) {
			return er;
		}

		n = ( nsect > SD_XFER_MAX ) ? SD_XFER_MAX : nsect;
		if ( sd.dma_mode == SD_DMA_SDMA && buf_is_linear(p, BLK_SECTOR_SIZE) ) {
			/*
			 * The single address mode is given runs that end at or
			 * before the next buffer boundary, so it never stops
			 * half way to ask for the rest (a pause not every
			 * controller resumes from). A sector that would straddle
			 * the boundary goes through the data port.
			 */
			UD	pa = (UD)VA2PA((UBINT)p);
			UD	fit = ( SDMA_BOUND - ( pa & ( SDMA_BOUND - 1 ) ) ) / BLK_SECTOR_SIZE;

			if ( fit == 0 ) {
				n = 1;
				pio = TRUE;
			} else if ( n > fit ) {
				n = fit;
			}
		}
		er = E_NOSPT;
		if ( !pio && ((UBINT)p & (DMA_ALIGN - 1)) != 0 && sd.bounce != NULL ) {
			/* off a cache line: through the bounce buffer */
			if ( n > BOUNCE_SECT ) n = BOUNCE_SECT;
			if ( write ) {
				knl_memcpy(sd.bounce, p, (SZ)( n * BLK_SECTOR_SIZE ));
			}
			er = sd_xfer_dma(start, sd.bounce, n, write);
			if ( er >= E_OK && !write ) {
				knl_memcpy(p, sd.bounce, (SZ)( n * BLK_SECTOR_SIZE ));
			}
		} else if ( !pio ) {
			er = sd_xfer_dma(start, p, n, write);
		}
		if ( er == E_NOSPT ) {
			er = sd_xfer_pio(start, p, n, write);
		}
		if ( er < E_OK ) {
			return er;
		}
		start += n;
		p += n * BLK_SECTOR_SIZE;
		nsect -= n;
	}
	return E_OK;
}

/*
 * Pick how data moves. The capability register says whether the
 * controller has the descriptor mode and whether its addresses are 64
 * bits wide. The table itself has to be memory the controller can reach,
 * and one page of it describes more than any request the layers above
 * make; without that page the single address mode is taken instead.
 */
LOCAL void sd_bounce_alloc( void )
{
	PFRAME	*pf = knl_alloc_pages(BOUNCE_ORDER, ZONE_DMA32, KAF_DMA32);

	sd.bounce = ( pf != NULL ) ? (UB *)PA2VA(knl_pf_to_pa(pf)) : NULL;
}

LOCAL void sd_dma_select( void )
{
	UW	caps = in_w(SDHCI_CAPABILITIES);

#if CNF_SD_DMA_MODE < 0
	if ( (caps & CAP_ADMA2) != 0 ) {
		sd.dma_mode = ( (caps & CAP_64BIT) != 0 )
			    ? SD_DMA_ADMA2_64 : SD_DMA_ADMA2_32;
	} else if ( (caps & CAP_SDMA) != 0 ) {
		sd.dma_mode = SD_DMA_SDMA;
	} else {
		sd.dma_mode = SD_DMA_NONE;
	}
#else
	sd.dma_mode = CNF_SD_DMA_MODE;
#endif

	if ( sd.dma_mode == SD_DMA_ADMA2_32 || sd.dma_mode == SD_DMA_ADMA2_64 ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

		if ( pf == NULL ) {
			sd.dma_mode = ( (caps & CAP_SDMA) != 0 )
				    ? SD_DMA_SDMA : SD_DMA_NONE;
		} else {
			sd.desc_pa = knl_pf_to_pa(pf);
			sd.desc    = (UW *)PA2VA(sd.desc_pa);
		}
	}
}

/*
 * Force the way data moves, for a test that runs every one of them over
 * the same card. A way the controller does not have, or the descriptor
 * modes without their table, answer E_NOSPT and change nothing. Answers
 * the way that was in use.
 */
EXPORT INT knl_sd_dma_mode( INT mode )
{
	UW	caps;
	INT	old = sd.dma_mode;

	if ( mode < 0 ) {
		return old;
	}
	caps = in_w(SDHCI_CAPABILITIES);
	switch ( mode ) {
	case SD_DMA_NONE:
		break;
	case SD_DMA_SDMA:
		if ( (caps & CAP_SDMA) == 0 ) return E_NOSPT;
		break;
	case SD_DMA_ADMA2_32:
		if ( (caps & CAP_ADMA2) == 0 || sd.desc == NULL ) return E_NOSPT;
		break;
	case SD_DMA_ADMA2_64:
		if ( (caps & CAP_ADMA2) == 0 || (caps & CAP_64BIT) == 0
		  || sd.desc == NULL ) return E_NOSPT;
		break;
	default:
		return E_PAR;
	}
	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	sd.dma_mode = mode;
	tk_unl_mtx(sd.mtxid);
	return old;
}

LOCAL CONST UB *sd_dma_name( void )
{
	switch ( sd.dma_mode ) {
	case SD_DMA_SDMA:	return (CONST UB*)"SDMA";
	case SD_DMA_ADMA2_32:	return (CONST UB*)"ADMA2/32";
	case SD_DMA_ADMA2_64:	return (CONST UB*)"ADMA2/64";
	default:		return (CONST UB*)"PIO";
	}
}

/* ---------------------------------------------------------------- card */

/*
 * Bring the card up: idle, voltage, address, select, four bit bus.
 */
LOCAL ER sd_init_card( void )
{
	UW	resp[4], ocr = 0;
	INT	i;
	ER	er;

	/*
	 * The card detect switch of the socket is wired to an always-on
	 * GPIO (active low), not necessarily to the controller, whose own
	 * bit may then say there is no card. The GPIO decides; the
	 * controller is told the card is in through its detect test level,
	 * as it may refuse to power the bus otherwise.
	 */
#ifdef RPI5
	if ( gpio_read(0, GIO_AON_SD_CD) == 1
	  && (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		return E_NOEXS;
	}
#else
	if ( (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		return E_NOEXS;
	}
#endif
	sd_step = 1;
	er = sd_reset(RESET_ALL);
	if ( er < E_OK ) return er;
	if ( (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		sd_out8(SDHCI_HOST_CONTROL, sd_in8(SDHCI_HOST_CONTROL) | CTRL_CD_TEST);
	}

	sd_out8(SDHCI_POWER_CONTROL, 0x0e);		/* 3.3 volts */
	sd_out8(SDHCI_POWER_CONTROL, 0x0f);		/* and on */
	sd_out8(SDHCI_TIMEOUT_CONTROL, 0x0e);		/* the longest timeout */
	out_w(SDHCI_INT_ENABLE, 0xFFFFFFFF);		/* status bits, not signals */
	out_w(SDHCI_SIGNAL_ENABLE, 0);			/* polled, no interrupt yet */

	sd_step = 2;
	er = sd_set_clock(400000);			/* identification speed */
	if ( er < E_OK ) return er;

	sd_step = 3;
	sd.rca = 0;
	er = sd_cmd(CMD_GO_IDLE, 0, NULL);
	if ( er < E_OK ) return er;

	/* 2.7 to 3.6 volts, check pattern 0xAA */
	sd_step = 4;
	er = sd_cmd(CMD_SEND_IF_COND, 0x1AA, resp);
	if ( er < E_OK || (resp[0] & 0xff) != 0xAA ) {
		return E_NOSPT;				/* not an SD 2.0 card */
	}

	sd_step = 5;
	for ( i = 0; i < 1000; i++ ) {
		er = sd_acmd(ACMD_SEND_OP_COND, OCR_VOLTAGE | OCR_HCS, resp);
		if ( er < E_OK ) return er;
		ocr = resp[0];
		if ( (ocr & OCR_BUSY) != 0 ) break;
		tk_dly_tsk(1);
	}
	if ( (ocr & OCR_BUSY) == 0 ) {
		return E_TMOUT;
	}
	sd.hc = ( (ocr & OCR_HCS) != 0 );

	sd_step = 6;
	er = sd_cmd(CMD_ALL_SEND_CID, 0, resp);
	if ( er < E_OK ) return er;

	er = sd_cmd(CMD_SEND_REL_ADDR, 0, resp);
	if ( er < E_OK ) return er;
	sd.rca = resp[0] & 0xFFFF0000;

	/* the size, from the card specific data */
	er = sd_cmd(CMD_SEND_CSD, sd.rca, resp);
	if ( er < E_OK ) return er;
	{
		UW	csd_ver = (resp[3] >> 30) & 3;

		if ( csd_ver == 1 ) {
			/* version 2: (C_SIZE + 1) * 512KB */
			UD c_size = ((UD)(resp[1] >> 16) | ((UD)(resp[2] & 0x3f) << 16));
			sd.nsect = (c_size + 1) * 1024;
		} else {
			UD c_size = (((UD)(resp[2] & 0x3ff) << 2) | (resp[1] >> 30));
			UD mult   = 1UL << (((resp[1] >> 15) & 7) + 2);
			UD rdblk  = 1UL << ((resp[2] >> 16) & 0x0f);
			sd.nsect = (c_size + 1) * mult * rdblk / BLK_SECTOR_SIZE;
		}
		/*
		 * A card that says it is write protected is written to by no
		 * one: the protection is its own, set by a command, and a
		 * write it refused would come back as an error half way
		 * through what a file system meant to write.
		 */
		sd.wp = 0;
		if ( (resp[0] & CSD_TMP_WP) != 0 )  sd.wp |= SD_WP_TMP;
		if ( (resp[0] & CSD_PERM_WP) != 0 ) sd.wp |= SD_WP_PERM;
	}

	sd_step = 7;
	er = sd_cmd(CMD_SELECT_CARD, sd.rca, resp);
	if ( er < E_OK ) return er;
	if ( (resp[0] & R1_CARD_IS_LOCKED) != 0 ) {
		sd.wp |= SD_WP_LOCKED;
	}
	if ( sd.wp != 0 ) {
		tm_printf((UB*)"TessronOS: sd the card is%s%s%s: nothing is written to it\n",
			  ( sd.wp & SD_WP_TMP ) ? " write protected (temporary)" : "",
			  ( sd.wp & SD_WP_PERM ) ? " write protected (permanent)" : "",
			  ( sd.wp & SD_WP_LOCKED ) ? " locked by a password" : "");
	}

	er = sd_acmd(ACMD_SET_BUS_WIDTH, 2, NULL);	/* four bits */
	if ( er >= E_OK ) {
		sd_out8(SDHCI_HOST_CONTROL, sd_in8(SDHCI_HOST_CONTROL) | CTRL_4BITBUS);
	}
	er = sd_cmd(CMD_SET_BLOCKLEN, BLK_SECTOR_SIZE, NULL);
	if ( er < E_OK ) return er;

	sd_step = 8;
	er = sd_set_clock(25000000);			/* full speed */
	if ( er < E_OK ) return er;

	sd.present = TRUE;

	return E_OK;
}

/*
 * Before anything is written: read the start of the card through the
 * data port and through the way chosen, and compare. Reading changes
 * nothing on the card, and a way that does not bring back what the data
 * port does is not used, so that it never gets to write.
 */
#define SD_CHECK_SECT		64		/* 32KB: the partition table and more */

LOCAL void sd_dma_check( void )
{
	PFRAME	*pf;
	UB	*a, *b;
	UD	i;
	ER	er1, er2;
	INT	mode = sd.dma_mode;

	if ( mode == SD_DMA_NONE ) {
		return;
	}
	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_DMA32);	/* 64KB: two buffers of 32KB */
	if ( pf == NULL ) {
		sd.dma_mode = SD_DMA_NONE;
		tm_printf((UB*)"TessronOS: sd no memory to check %s; using the data port\n",
			  sd_dma_name());
		return;
	}
	a = (UB *)PA2VA(knl_pf_to_pa(pf));
	b = a + SD_CHECK_SECT * BLK_SECTOR_SIZE;
	for ( i = 0; i < SD_CHECK_SECT * BLK_SECTOR_SIZE; i++ ) {
		a[i] = 0x5A;
		b[i] = 0xA5;
	}

	sd.dma_mode = SD_DMA_NONE;
	er1 = sd_xfer(0, a, SD_CHECK_SECT, FALSE);
	sd.dma_mode = mode;
	er2 = sd_xfer(0, b, SD_CHECK_SECT, FALSE);
	for ( i = 0; i < SD_CHECK_SECT * BLK_SECTOR_SIZE && a[i] == b[i]; i++ ) ;

	if ( er1 < E_OK || er2 < E_OK || i < SD_CHECK_SECT * BLK_SECTOR_SIZE ) {
		tm_printf((UB*)"TessronOS: sd %s reads differ from the data port "
			  "(%d, %d, at byte %d); using the data port\n",
			  sd_dma_name(), (INT)er1, (INT)er2, (INT)i);
		sd.dma_mode = SD_DMA_NONE;
	}
	knl_free_pages(pf, 4);
}

/* ---------------------------------------------------------------- driver */

LOCAL ER sd_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	ER	er;

	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	er = sd_xfer(start, buf, nsect, FALSE);
	tk_unl_mtx(sd.mtxid);

	return er;
}

LOCAL ER sd_unit_range( INT unitno, UD *p_start, UD *p_nsect )
{
	if ( unitno == 0 ) {
		*p_start = 0;
		*p_nsect = sd.nsect;
		return E_OK;
	}
	if ( unitno > sd.tbl.n || !sd.tbl.part[unitno - 1].valid ) {
		return E_NOEXS;
	}
	*p_start = sd.tbl.part[unitno - 1].start;
	*p_nsect = sd.tbl.part[unitno - 1].nsect;

	return E_OK;
}

LOCAL ER sd_open( ID devid, UINT omode, void *exinf )
{
	return ( sd.present ) ? E_OK : E_NOMDA;
}

LOCAL ER sd_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER sd_attr( INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nsect;
	ER		er;

	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ
	  || req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = sd_unit_range(unitno, &start, &nsect);
	if ( er < E_OK ) return er;

	info = (DiskInfo *)req->buf;
	info->protect   = ( sd.wp != 0 );
	info->removable = TRUE;			/* the card can be taken out */
	info->blocksize = BLK_SECTOR_SIZE;
	info->blockcont = (W)nsect;
	req->asize = sizeof(DiskInfo);

	return E_OK;
}

LOCAL ER sd_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	INT	unitno = (INT)(req->devid & 0xff);
	UD	start, nsect, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;

	/*
	 * A write here completes when the card leaves its busy state, and
	 * the card's own cache (SD 6.0) is never turned on, so there is
	 * nothing more to put onto the medium.
	 */
	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		req->error = sd.present ? E_OK : E_NOMDA;
		return E_OK;
	}
	if ( req->start < 0 ) {
		req->error = sd_attr(unitno, req);
		return E_OK;
	}
	if ( !sd.present ) {
		req->error = E_NOMDA;
		return E_OK;
	}
	er = sd_unit_range(unitno, &start, &nsect);
	if ( er < E_OK ) { req->error = er; return E_OK; }

	if ( (req->size % BLK_SECTOR_SIZE) != 0 ) {
		req->error = E_PAR;
		return E_OK;
	}
	nreq = (UD)req->size / BLK_SECTOR_SIZE;
	if ( (UD)req->start >= nsect || (UD)req->start + nreq > nsect ) {
		req->error = E_PAR;
		return E_OK;
	}

	if ( req->cmd == TDC_WRITE && sd.wp != 0 ) {
		req->error = E_RONLY;
		return E_OK;
	}

	er = tk_loc_mtx(sd.mtxid, tmout);
	if ( er < E_OK ) { req->error = er; return E_OK; }

	er = sd_xfer(start + (UD)req->start, req->buf, nreq, ( req->cmd == TDC_WRITE ));
	tk_unl_mtx(sd.mtxid);

	if ( er == E_OK ) req->asize = req->size;
	req->error = er;

	return E_OK;
}

LOCAL INT sd_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER sd_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT sd_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

EXPORT INT knl_sd_init( void )
{
	T_DDEV	ddev;
	T_CMTX	cmtx;
	INT	i;
	ER	er;

	knl_memset(&sd, 0, sizeof(sd));

#ifdef QEMU_VIRT
	{
		T_PCIDEV	d;

		if ( ts_pcie_find(PCI_CLASS_SDHCI, PCI_SUBCLASS_SDHCI, 0xff, 0, &d) < E_OK
		  || d.bar[0] == 0 ) {
			return 0;		/* the machine has no SD slot */
		}
		ts_pcie_enable(&d);
		sd_base = ts_pcie_bar_base(&d, 0);
		if ( sd_base == 0 ) {
			return 0;
		}
	}
#endif

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	sd.mtxid = tk_cre_mtx(&cmtx);
	if ( sd.mtxid <= 0 ) {
		return (ER)sd.mtxid;
	}

	er = sd_init_card();
	if ( er < E_OK ) {
		tm_printf((UB*)"TessronOS: no SD card (%d at step %d, state %08x, caps %08x)\n",
			  er, sd_step, in_w(SDHCI_PRESENT_STATE), in_w(SDHCI_CAPABILITIES));
		return 0;
	}
	sd_dma_select();
	sd_bounce_alloc();
	sd_dma_check();

	knl_read_parttbl(sd_readfn, &sd, sd.nsect, &sd.tbl);

	sd.devnm[0] = 's'; sd.devnm[1] = 'd'; sd.devnm[2] = 'a'; sd.devnm[3] = '\0';

	ddev.exinf   = &sd;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_DISK | TDK_DISK_FLA;
	ddev.nsub    = sd.tbl.n;
	ddev.blksz   = BLK_SECTOR_SIZE;
	ddev.openfn  = (FP)sd_open;
	ddev.closefn = (FP)sd_close;
	ddev.execfn  = (FP)sd_exec;
	ddev.waitfn  = (FP)sd_wait;
	ddev.abortfn = (FP)sd_abort;
	ddev.eventfn = (FP)sd_event;

	sd.devid = tk_def_dev(sd.devnm, &ddev, NULL);
	if ( sd.devid <= 0 ) {
		return sd.devid;
	}

	tm_printf((UB*)"TessronOS: sda %d sectors (%d MB)%s, %d partitions (%s), %s\n",
		(UW)sd.nsect, (UW)(sd.nsect >> 11), sd.hc ? ", HC" : "",
		sd.tbl.n, sd.tbl.gpt ? "GPT" : "MBR", sd_dma_name());
	for ( i = 0; i < sd.tbl.n; i++ ) {
		tm_printf((UB*)"TessronOS:   sda%d %d+%d\n", i,
			(UW)sd.tbl.part[i].start, (UW)sd.tbl.part[i].nsect);
	}

	return 1;
}

#endif /* RPI5 || QEMU_VIRT */
