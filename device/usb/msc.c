/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	msc.c
 *	USB disks: mass storage over bulk-only transport (design 10.14).
 *
 *	Every command is three steps on the two bulk endpoints: a 31 byte
 *	Command Block Wrapper out, the data in or out, and a 13 byte Command
 *	Status Wrapper in. The command itself is SCSI:
 *	    INQUIRY		what it is, and whether its medium comes out
 *	    TEST UNIT READY	whether there is a medium to use
 *	    REQUEST SENSE	why the last command failed
 *	    READ CAPACITY	how many blocks, and how large (16 for a disk
 *				of more than 2^32 blocks)
 *	    READ / WRITE	(10, or 16 beyond 2^32 blocks)
 *
 *	A device that stalls the data stage has its endpoint cleared and the
 *	status is read anyway; one that loses step (a phase error, or a
 *	status that does not answer the command) is put back by the class
 *	reset and both endpoints cleared.
 *
 *	Each disk is registered with tk_def_dev() as "uda", "udb", ... "udz",
 *	"udaa", "udab" ... (the place it takes in the table, its letters
 *	counted as the columns of a spreadsheet are), in
 *	the way of the other disks (design 10.6): subunit 0 is the whole
 *	disk and the partitions of its GPT or MBR follow, so "uda0" is the
 *	first partition. The object layer is told whenever one comes or goes,
 *	and when the medium of a removable one goes in or out.
 *
 *	A CD-ROM drive (peripheral type 5) is a disk as well: its blocks are
 *	of 2048 bytes, it is read only, and it has no partitions of its own
 *	(what is on the disc is read from the whole disk).
 *
 *	A disk taken out while it is open stays registered, answering every
 *	request with an error, until the last one closes it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/ob.h>
#include "usbdev.h"

#define MSC_FIRST		8	/* places the table starts with; it doubles when full */
#define MSC_SUBCLASS_SCSI	0x06
#define MSC_PROTO_BOT		0x50
#define MSC_REQ_RESET		0xff
#define MSC_XFER_MAX		65536	/* bytes per command: the bulk buffer */
#define MSC_CMD_TMO		30000	/* ms a command may take, its three stages together */
#define MSC_RESET_TMO		5000	/* ms for the class reset request */
#define MSC_RESET_WAIT		6000	/* ms the device is given after the class reset */
#define MSC_RETRIES		5	/* a command sent again after the device was reset */
#define MSC_HIST		8	/* commands remembered, told when the transport fails */
#define MSC_SETTLE_TRIES	5	/* TEST UNIT READY after a reset, for the unit attentions it brings */
#define MEDIA_POLL_MS		5000	/* a removable medium is looked for */

#define BE32(p)		( ( (UW)(p)[0] << 24 ) | ( (UW)(p)[1] << 16 ) | ( (UW)(p)[2] << 8 ) | (UW)(p)[3] )

#define CBW_SIG			0x43425355	/* "USBC" */
#define CSW_SIG			0x53425355	/* "USBS" */

#define SCSI_TEST_UNIT_READY	0x00
#define SCSI_REQUEST_SENSE	0x03
#define SCSI_INQUIRY		0x12
#define SCSI_READ_CAPACITY10	0x25
#define SCSI_READ10		0x28
#define SCSI_WRITE10		0x2a
#define SCSI_READ16		0x88
#define SCSI_WRITE16		0x8a
#define SCSI_SERVICE_IN16	0x9e
#define SCSI_MODE_SENSE6	0x1a
#define SCSI_SYNC_CACHE10	0x35

#define SCSI_TYPE_DISK		0x00
#define SCSI_TYPE_CDROM		0x05

typedef struct {
	BOOL	used;
	BOOL	gone;		/* the device went; the name waits to go */
	BOOL	registered;
	USBDEV	*dev;
	USBEP	ep_in, ep_out;
	INT	ifno;
	UW	tag;
	UD	nblocks;
	UW	blksz;
	BOOL	removable;
	BOOL	readonly;
	BOOL	wce;		/* the device says its write cache is on (a caching mode page with WCE) */
	BOOL	fua;		/* and its writes may ask to go through that cache (DPOFUA) */
	BOOL	cdrom;		/* a CD-ROM drive */
	BOOL	medium;		/* a medium is in */
	BOOL	notify_media;	/* the object layer is to hear of it */
	UW	last_poll;
	T_PARTTBL tbl;
	UB	devnm[8];
	INT	no;		/* its place in the table, what its name says */
	ID	devid;
	UB	vendor[9], model[17];
	/* how long the requests took, for USB.TXT (knl_msc_report): count, ms in all, the longest */
	UW	n_rd, n_wr, n_sync;
	UW	ms_rd, ms_wr, ms_sync;
	UW	max_rd, max_wr, max_sync;
	/* the requests that failed, for the console and USB.TXT */
	UW	n_fail;
	UB	sense[3];	/* the last sense: key, ASC, ASCQ */
	BOOL	sync_said;	/* told once that SYNCHRONIZE CACHE was refused */
	/* the transport's recovery (msc_recover) */
	BOOL	recovering;	/* within it: its own commands are not recovered again */
	BOOL	expect_ua;	/* a reset was made: the unit attention it brings is not an error */
	UW	n_recover;	/* recoveries made, for USB.TXT */
	/* the last commands, oldest at hist_next, for the console when the transport fails */
	struct {
		UB	op;
		UD	lba;
		UW	len;	/* bytes of data */
		INT	res;	/* the status, or the transport's error */
		UW	ms;
	} hist[MSC_HIST];
	INT	hist_next;
	/* a test's pretended failures (knl_msc_fault): commands that time out, and how it is recovered */
	INT	fault;
	BOOL	fault_bot;
} MSCDEV;

#define MSC_FAIL_SAID	16	/* failed requests told on the console, each disk */

/*
 * A disk's entry is made when a disk first takes its place, and kept for
 * the next: the table holds what has been plugged in at once, and the
 * entries are as large as a partition table is. The table is as large
 * as the disks plugged in at once need, made twice as large when they
 * fill it; a larger table is made whole before it is put in place, and
 * the one before is not freed, so the manager's task may walk it while
 * another attaches a disk. A disk's name ("uda" to "udz", then "udaa"
 * on) is its place.
 */
typedef struct {
	INT	cap;
	MSCDEV	*e[];
} MSCTBL;

LOCAL MSCTBL	*msc_tbl = NULL;

#define MSC_N		( ( msc_tbl != NULL ) ? msc_tbl->cap : 0 )

/* The entry at a place, NULL while none was made there */
#define MSC_AT(i)	( msc_tbl->e[(i)] )
LOCAL BOOL	msc_notify = FALSE;	/* a disk came or went */

/* ---------------------------------------------------------------- transport */

/*
 * The Bulk-Only Mass Storage Reset: the class request, the device given
 * MSC_RESET_WAIT to recover, then the halt cleared on the bulk IN and
 * the bulk OUT endpoint (each also starting over on the controller's
 * side, knl_usb_clear_halt). The order and the times are those of the
 * usual hosts. E_OK when all three went through.
 */
LOCAL ER bot_reset( MSCDEV *m )
{
	ER	er, e1, e2;

	er = knl_usb_control_tmo(m->dev, USB_RT_CLASS | USB_RT_INTERFACE, MSC_REQ_RESET,
				 0, (UH)m->ifno, NULL, 0, NULL, MSC_RESET_TMO);
	if ( er < E_OK ) {
		return er;
	}
	knl_usb_wait(MSC_RESET_WAIT);
	e1 = knl_usb_clear_halt(m->dev, m->ep_in.addr);
	e2 = knl_usb_clear_halt(m->dev, m->ep_out.addr);

	return ( e1 < E_OK ) ? e1 : e2;
}

/* The commands remembered, oldest first, on the console */
LOCAL void hist_say( MSCDEV *m )
{
	INT	i, k;

	tm_printf((UB *)"usb: %s: the last commands, oldest first (command, block, bytes, answer, ms):", m->devnm);
	for ( i = 0; i < MSC_HIST; i++ ) {
		k = ( m->hist_next + i ) % MSC_HIST;
		if ( m->hist[k].op == 0 && m->hist[k].len == 0 && m->hist[k].ms == 0 && m->hist[k].res == 0 ) {
			continue;
		}
		tm_printf((UB *)" %02x %ld %u %d %u;", m->hist[k].op, (D)m->hist[k].lba,
			  m->hist[k].len, m->hist[k].res, m->hist[k].ms);
	}
	tm_printf((UB *)"\n");
}

LOCAL void msc_settle( MSCDEV *m );

/*
 * The device brought back after the transport failed: a reset of its
 * port first (the device addressed and configured again where it is
 * plugged in), and when that cannot be done the Bulk-Only reset, as
 * the usual hosts do. The unit attentions the reset brings are then
 * taken (msc_settle), and the command is sent again by the caller. One
 * that still comes after them is taken as part of the reset as well.
 */
LOCAL BOOL msc_recover( MSCDEV *m, INT why )
{
	ER	er;

	m->recovering = TRUE;
	m->n_recover++;
	hist_say(m);
	if ( m->fault_bot ) {
		er = E_NOSPT;			/* a test's: the port reset taken as not done */
	} else {
		er = knl_usb_reset_device(m->dev);
	}
	tm_printf((UB *)"usb: %s: the transport failed (%d): the port reset (%d)\n", m->devnm, why, (INT)er);
	if ( er < E_OK && !m->gone ) {
		er = bot_reset(m);
		tm_printf((UB *)"usb: %s: the Bulk-Only reset (%d)\n", m->devnm, (INT)er);
	}
	if ( er >= E_OK && !m->gone ) {
		msc_settle(m);
	}
	m->expect_ua = TRUE;
	m->recovering = FALSE;

	return ( er >= E_OK );
}

LOCAL ER bulk( MSCDEV *m, BOOL in, UB *data, INT len, INT *actual, TMO tmo )
{
	USBHC	*hc = m->dev->hc;

	return hc->ops->bulk(hc, m->dev, in ? &m->ep_in : &m->ep_out, data, len,
			     actual, tmo);
}

/* What is left of a command's time, at least a millisecond */
LOCAL TMO left_of( UW end )
{
	INT	d = (INT)( end - knl_usb_ms() );

	return ( d > 0 ) ? (TMO)d : 1;
}

/*
 * One command: len bytes of data in or out, its three stages within
 * MSC_CMD_TMO. Answers the status the device gave (0 passed, 1 failed),
 * or an error of the transport, after which the device has to be
 * brought back (msc_recover). *moved is how much data went.
 */
LOCAL INT bot_once( MSCDEV *m, CONST UB *cdb, INT cdblen, UB *data, INT len,
		    BOOL in, INT *moved )
{
	UB	cbw[31], csw[13];
	INT	done = 0, n = 0, i;
	UW	t0 = knl_usb_ms(), end = t0 + MSC_CMD_TMO;
	ER	er;

	if ( moved != NULL ) {
		*moved = 0;
	}
	if ( m->gone ) {
		return E_IO;
	}
	if ( m->fault > 0 && !m->recovering ) {
		/* a test's: the status asked for with no command sent, which no device answers */
		m->fault--;
		er = bulk(m, TRUE, csw, sizeof(csw), &n, 500);
		return ( er < E_OK ) ? er : E_IO;
	}
	knl_memset(cbw, 0, sizeof(cbw));
	PUT32(cbw, CBW_SIG);
	m->tag++;
	PUT32(cbw + 4, m->tag);
	PUT32(cbw + 8, (UW)len);
	cbw[12] = ( in && len > 0 ) ? 0x80 : 0x00;
	cbw[13] = 0;				/* LUN 0 */
	cbw[14] = (UB)cdblen;
	for ( i = 0; i < cdblen; i++ ) {
		cbw[15 + i] = cdb[i];
	}
	er = bulk(m, FALSE, cbw, sizeof(cbw), &n, left_of(end));
	if ( er < E_OK ) {
		return er;
	}

	/* the data, in pieces the controller's buffer holds */
	while ( done < len ) {
		INT	piece = len - done;

		if ( piece > MSC_XFER_MAX ) {
			piece = MSC_XFER_MAX;
		}
		n = 0;
		er = bulk(m, in, data + done, piece, &n, left_of(end));
		if ( er == E_OBJ ) {
			/* it stalled the data: clear it, the status still comes */
			knl_usb_clear_halt(m->dev, in ? m->ep_in.addr : m->ep_out.addr);
			break;
		}
		if ( er < E_OK ) {
			return er;
		}
		done += n;
		if ( n < piece ) {
			break;			/* a short packet ends the data */
		}
	}
	if ( moved != NULL ) {
		*moved = done;
	}

	/* the status, once more after a stall */
	n = 0;
	er = bulk(m, TRUE, csw, sizeof(csw), &n, left_of(end));
	if ( er == E_OBJ ) {
		knl_usb_clear_halt(m->dev, m->ep_in.addr);
		er = bulk(m, TRUE, csw, sizeof(csw), &n, left_of(end));
	}
	if ( er < E_OK || n < 13 || GET32(csw) != CSW_SIG || GET32(csw + 4) != m->tag ) {
		USB_LOG("usb: disk: status %d, %d bytes, %08x tag %08x (%08x)\n",
			(INT)er, n, GET32(csw), GET32(csw + 4), m->tag);
		return ( er < E_OK ) ? er : E_IO;
	}
	if ( csw[12] == 2 ) {			/* the device lost step */
		USB_LOG("usb: %s: phase error in the status of command %02x\n", m->devnm, cdb[0]);
		return E_IO;
	}

	return csw[12];
}

/*
 * A command, sent again up to MSC_RETRIES times after the device was
 * brought back when the transport failed. Each is remembered.
 */
LOCAL INT bot_cmd( MSCDEV *m, CONST UB *cdb, INT cdblen, UB *data, INT len,
		   BOOL in, INT *moved )
{
	INT	st, k, tries = 0;
	UW	t0;

	for ( ;; ) {
		t0 = knl_usb_ms();
		st = bot_once(m, cdb, cdblen, data, len, in, moved);
		k = m->hist_next;
		m->hist[k].op = cdb[0];
		m->hist[k].lba = ( cdblen == 16 ) ? ( (UD)BE32(cdb + 2) << 32 ) | BE32(cdb + 6)
			       : ( cdblen == 10 ) ? BE32(cdb + 2) : 0;
		m->hist[k].len = (UW)len;
		m->hist[k].res = st;
		m->hist[k].ms = knl_usb_ms() - t0;
		m->hist_next = ( k + 1 ) % MSC_HIST;
		if ( st == 0 ) {
			m->expect_ua = FALSE;
		}
		if ( st >= 0 || m->gone || m->recovering || tries >= MSC_RETRIES ) {
			return st;
		}
		tries++;
		if ( !msc_recover(m, st) ) {
			return st;
		}
	}
}

/* Why the last command failed: the sense key, 0 when nothing is wrong */
LOCAL INT scsi_sense( MSCDEV *m, UB *asc )
{
	UB	cdb[6], s[18];
	INT	n;

	knl_memset(cdb, 0, sizeof(cdb));
	knl_memset(s, 0, sizeof(s));
	cdb[0] = SCSI_REQUEST_SENSE;
	cdb[4] = sizeof(s);
	if ( bot_cmd(m, cdb, 6, s, sizeof(s), TRUE, &n) != 0 || n < 14 ) {
		return -1;
	}
	if ( asc != NULL ) {
		*asc = s[12];
	}
	m->sense[0] = (UB)( s[2] & 0x0f );
	m->sense[1] = s[12];
	m->sense[2] = s[13];

	return s[2] & 0x0f;
}

/* Whether a medium is there and ready: answers TRUE when it is */
LOCAL BOOL scsi_ready( MSCDEV *m, INT tries )
{
	UB	cdb[6];
	INT	i;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_TEST_UNIT_READY;
	for ( i = 0; i < tries; i++ ) {
		INT	st = bot_cmd(m, cdb, 6, NULL, 0, FALSE, NULL);

		if ( st == 0 ) {
			return TRUE;
		}
		if ( st < 0 ) {
			return FALSE;
		}
		/* a unit attention after power-up is normal: it is asked again */
		if ( scsi_sense(m, NULL) == 2 && i > 0 ) {
			return FALSE;		/* not ready: no medium */
		}
		if ( i + 1 < tries ) {
			knl_usb_wait(100);
		}
	}

	return FALSE;
}

LOCAL ER scsi_capacity( MSCDEV *m )
{
	UB	cdb[16], d[32];
	INT	n = 0;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_READ_CAPACITY10;
	if ( bot_cmd(m, cdb, 10, d, 8, TRUE, &n) != 0 || n < 8 ) {
		scsi_sense(m, NULL);
		return E_IO;
	}
	m->nblocks = (UD)(((UW)d[0] << 24) | ((UW)d[1] << 16) | ((UW)d[2] << 8) | d[3]) + 1;
	m->blksz = ((UW)d[4] << 24) | ((UW)d[5] << 16) | ((UW)d[6] << 8) | d[7];

	if ( m->nblocks == 0x100000000ULL ) {
		/* more than 2^32 blocks: the long form says how many */
		knl_memset(cdb, 0, sizeof(cdb));
		cdb[0] = SCSI_SERVICE_IN16;
		cdb[1] = 0x10;			/* READ CAPACITY (16) */
		cdb[13] = 32;
		if ( bot_cmd(m, cdb, 16, d, 32, TRUE, &n) == 0 && n >= 12 ) {
			INT	i;
			UD	last = 0;

			for ( i = 0; i < 8; i++ ) {
				last = (last << 8) | d[i];
			}
			m->nblocks = last + 1;
			m->blksz = ((UW)d[8] << 24) | ((UW)d[9] << 16)
				 | ((UW)d[10] << 8) | d[11];
		}
	}
	if ( m->blksz == 0 || m->blksz > 4096 || (m->blksz & (m->blksz - 1)) != 0 ) {
		return E_NOSPT;
	}

	return E_OK;
}

/*
 * After a reset a device reports what the reset did as unit attentions,
 * one for each command it is sent, before it carries out any other: the
 * power on or reset (29/xx), and a card reader also the card it found
 * again (28/00, the medium may have changed). They are taken here, with
 * TEST UNIT READY and REQUEST SENSE, before the command that failed is
 * sent again, so that it is not answered with one of them; a device
 * still becoming ready (02/04/01) is waited for. A medium said to have
 * changed is the same one when its capacity is. Another is taken as
 * the old one gone, for the media poll to bring the new one in.
 */
LOCAL void msc_settle( MSCDEV *m )
{
	UB	cdb[6], asc;
	INT	i, st = 0, key, ua = 0;
	BOOL	changed = FALSE;
	UD	nblocks = m->nblocks;
	UW	blksz = m->blksz;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_TEST_UNIT_READY;
	for ( i = 0; i < MSC_SETTLE_TRIES; i++ ) {
		st = bot_cmd(m, cdb, 6, NULL, 0, FALSE, NULL);
		if ( st <= 0 ) {
			break;			/* ready, or the transport failed again */
		}
		asc = 0;
		key = scsi_sense(m, &asc);
		if ( key == 6 ) {
			ua++;
			if ( asc == 0x28 ) {
				changed = TRUE;
			}
			continue;
		}
		if ( key == 2 && asc == 0x04 && m->sense[2] == 0x01 ) {
			knl_usb_wait(100);
			continue;
		}
		break;				/* not ready, no medium: the command says so */
	}
	if ( changed && m->medium && !m->gone ) {
		if ( scsi_capacity(m) < E_OK || m->nblocks != nblocks || m->blksz != blksz ) {
			tm_printf((UB *)"usb: %s: another medium after the reset (%ld blocks of %u, was %ld of %u)\n",
				  m->devnm, (D)m->nblocks, m->blksz, (D)nblocks, blksz);
			m->medium = FALSE;
			m->notify_media = TRUE;
		}
	}
	tm_printf((UB *)"usb: %s: after the reset: %d unit attentions, test unit ready %d, medium %s\n",
		  m->devnm, ua, st, !m->medium ? "gone" : changed ? "the same" : "kept");
}

/*
 * Whether the medium is write protected, from the mode parameter header
 * of every page asked for at once (page 0x3f in 192 bytes: the caching
 * page 8 alone is not asked for, since many USB devices fail on it).
 * The write cache is taken as on only when a caching page among those
 * answered says so: page 8 with WCE, or page 6 (RBC) without its
 * write-cache-disable bit. A device that names no such page, or does
 * not answer, writes through, and is sent no SYNCHRONIZE CACHE; a write
 * of it is on the medium when the device says it is done. FUA (DPOFUA
 * in the header) is used only with a write cache on, and not on a
 * medium that is write protected.
 */
LOCAL BOOL scsi_protected( MSCDEV *m )
{
	UB	cdb[6], d[192];
	INT	n = 0, len, at;
	BOOL	wp;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_MODE_SENSE6;
	cdb[2] = 0x3f;				/* every page */
	cdb[4] = sizeof(d);			/* 192: the length many devices take only */
	m->wce = FALSE;
	m->fua = FALSE;
	if ( bot_cmd(m, cdb, 6, d, sizeof(d), TRUE, &n) != 0 || n < 4 ) {
		scsi_sense(m, NULL);		/* many do not answer it */
		return FALSE;
	}
	wp = (BOOL)( (d[2] & 0x80) != 0 );
	len = d[0] + 1;				/* the mode data length does not count itself */
	if ( len > n ) len = n;
	for ( at = 4 + d[3]; at < len; ) {
		UB	page = (UB)( d[at] & 0x3f );

		if ( page == 8 || page == 6 ) {
			if ( len - at > 2 ) {
				m->wce = ( page == 8 ) ? (BOOL)( (d[at + 2] & 0x04) != 0 )
						       : (BOOL)( (d[at + 2] & 0x01) == 0 );
			}
			break;
		}
		if ( (d[at] & 0x40) != 0 && len - at > 3 ) {
			at += 4 + ( (INT)d[at + 2] << 8 ) + d[at + 3];	/* a subpage */
		} else if ( (d[at] & 0x40) == 0 && len - at > 1 ) {
			at += 2 + d[at + 1];
		} else {
			break;
		}
	}
	if ( wp ) {
		m->wce = FALSE;			/* nothing to empty onto a medium not written */
	}
	m->fua = (BOOL)( m->wce && (d[2] & 0x10) != 0 );

	return wp;
}

/*
 * A read or a write the device failed, told on the console (the first
 * MSC_FAIL_SAID of each disk) with what it said: the transport's error,
 * or the command's status and the sense that followed, and how much
 * moved. Counted for USB.TXT.
 */
LOCAL void msc_fail( MSCDEV *m, BOOL wr, UD lba, UW nblk, INT st, BOOL sensed, INT moved )
{
	m->n_fail++;
	if ( m->n_fail > MSC_FAIL_SAID ) {
		return;
	}
	if ( sensed ) {
		tm_printf((UB *)"usb: %s: %s of %d blocks at %ld failed: status %d, sense %02x/%02x/%02x, %d bytes moved\n",
			  m->devnm, wr ? "write" : "read", (INT)nblk, (D)lba, st,
			  m->sense[0], m->sense[1], m->sense[2], moved);
	} else {
		tm_printf((UB *)"usb: %s: %s of %d blocks at %ld failed: %s %d, %d bytes moved\n",
			  m->devnm, wr ? "write" : "read", (INT)nblk, (D)lba,
			  ( st < 0 ) ? "transport" : "status", st, moved);
	}
}

/* Read or write nblk blocks from block lba; fua: the write through the device's cache */
LOCAL ER scsi_rw( MSCDEV *m, BOOL wr, UD lba, UB *buf, UW nblk, BOOL fua )
{
	UB	cdb[16];
	INT	len, moved = 0, st, cdblen;

	knl_memset(cdb, 0, sizeof(cdb));
	if ( lba + nblk > 0xffffffffULL ) {
		cdb[0] = wr ? SCSI_WRITE16 : SCSI_READ16;
		cdb[2] = (UB)(lba >> 56);
		cdb[3] = (UB)(lba >> 48);
		cdb[4] = (UB)(lba >> 40);
		cdb[5] = (UB)(lba >> 32);
		cdb[6] = (UB)(lba >> 24);
		cdb[7] = (UB)(lba >> 16);
		cdb[8] = (UB)(lba >> 8);
		cdb[9] = (UB)lba;
		cdb[10] = (UB)(nblk >> 24);
		cdb[11] = (UB)(nblk >> 16);
		cdb[12] = (UB)(nblk >> 8);
		cdb[13] = (UB)nblk;
		cdblen = 16;
	} else {
		cdb[0] = wr ? SCSI_WRITE10 : SCSI_READ10;
		cdb[2] = (UB)(lba >> 24);
		cdb[3] = (UB)(lba >> 16);
		cdb[4] = (UB)(lba >> 8);
		cdb[5] = (UB)lba;
		cdb[7] = (UB)(nblk >> 8);
		cdb[8] = (UB)nblk;
		cdblen = 10;
	}
	if ( wr && fua ) {
		cdb[1] |= 0x08;			/* FUA */
	}
	len = (INT)(nblk * m->blksz);
	st = bot_cmd(m, cdb, cdblen, buf, len, !wr, &moved);
	if ( st < 0 ) {
		msc_fail(m, wr, lba, nblk, st, FALSE, moved);
		return st;
	}
	if ( st != 0 ) {
		UB	asc = 0;
		INT	key = scsi_sense(m, &asc);

		if ( key == 6 && m->expect_ua ) {
			/* the unit attention of the reset just made: the command again */
			m->expect_ua = FALSE;
			st = bot_cmd(m, cdb, cdblen, buf, len, !wr, &moved);
			if ( st == 0 && moved == len ) {
				return E_OK;
			}
			if ( st < 0 ) {
				msc_fail(m, wr, lba, nblk, st, FALSE, moved);
				return st;
			}
			key = ( st != 0 ) ? scsi_sense(m, &asc) : 0;
		}

		msc_fail(m, wr, lba, nblk, st, TRUE, moved);
		if ( key == 2 || asc == 0x3a ) {
			return E_NOMDA;		/* not ready, or no medium */
		}
		if ( key == 7 ) {
			return E_RONLY;		/* data protect */
		}
		return E_IO;
	}
	if ( moved != len ) {
		msc_fail(m, wr, lba, nblk, st, FALSE, moved);
		return E_IO;
	}

	return E_OK;
}

/*
 * The device's cache onto the medium (SYNCHRONIZE CACHE for the whole
 * medium). A device that refuses the command keeps no cache worth
 * asking about, and is answered as done.
 */
LOCAL ER scsi_sync( MSCDEV *m )
{
	UB	cdb[10];
	INT	st;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_SYNC_CACHE10;
	st = bot_cmd(m, cdb, 10, NULL, 0, FALSE, NULL);
	if ( st < 0 ) {
		return E_IO;
	}
	if ( st != 0 ) {
		(void)scsi_sense(m, NULL);	/* the condition is cleared */
		if ( !m->sync_said ) {
			m->sync_said = TRUE;
			tm_printf((UB *)"usb: %s: SYNCHRONIZE CACHE refused (sense %02x/%02x/%02x): "
				  "taken as a device without a cache to empty\n", m->devnm,
				  m->sense[0], m->sense[1], m->sense[2]);
		}
	}
	return E_OK;
}

/* ---------------------------------------------------------------- disk device */

/* A request in blocks, cut to what one command carries */
LOCAL ER msc_xfer( MSCDEV *m, BOOL wr, UD lba, UB *buf, UD nblk, BOOL fua )
{
	UW	per = MSC_XFER_MAX / m->blksz;
	ER	er = E_OK;

	while ( nblk > 0 && er >= E_OK ) {
		UW	n = ( nblk > per ) ? per : (UW)nblk;

		er = scsi_rw(m, wr, lba, buf, n, fua);
		lba += n;
		buf += (UBINT)n * m->blksz;
		nblk -= n;
	}

	return er;
}

/* The partition scanner reads 512 byte sectors of the whole disk */
LOCAL ER msc_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	return msc_xfer((MSCDEV *)exinf, FALSE, start, (UB *)buf, nsect, FALSE);
}

LOCAL ER unit_range( MSCDEV *m, INT unitno, UD *p_start, UD *p_nblk )
{
	if ( unitno == 0 ) {
		*p_start = 0;
		*p_nblk = m->nblocks;
		return E_OK;
	}
	if ( unitno > m->tbl.n || !m->tbl.part[unitno - 1].valid ) {
		return E_NOEXS;
	}
	*p_start = m->tbl.part[unitno - 1].start;
	*p_nblk = m->tbl.part[unitno - 1].nsect;

	return E_OK;
}

LOCAL ER msc_open( ID devid, UINT omode, void *exinf )
{
	MSCDEV	*m = (MSCDEV *)exinf;

	if ( m->gone ) {
		return E_NOEXS;
	}
	if ( m->readonly && (omode & TD_WRITE) != 0 ) {
		return E_RONLY;
	}

	return E_OK;
}

LOCAL ER msc_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER msc_attr( MSCDEV *m, INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nblk;
	ER		er;

	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ ) {
		return E_PAR;
	}
	if ( req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = unit_range(m, unitno, &start, &nblk);
	if ( er < E_OK ) {
		return er;
	}
	info = (DiskInfo *)req->buf;
	info->protect   = m->readonly;
	info->removable = m->removable;
	info->blocksize = (W)m->blksz;
	/* the count in a W stops at 2^31 - 1 sectors (1TB); the whole count is in blockcont_d */
	info->blockcont = ( nblk > 0x7FFFFFFFULL ) ? 0x7FFFFFFF : (W)nblk;
	info->blockcont_d = (D)nblk;
	req->asize = sizeof(DiskInfo);

	return E_OK;
}

LOCAL ER msc_register( MSCDEV *m );

LOCAL ER msc_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	MSCDEV	*m = (MSCDEV *)exinf;
	INT	unitno = (INT)(req->devid & 0xff);
	UD	start, nblk, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;

	knl_usb_lock();
	if ( m->gone || !m->used ) {
		req->error = E_IO;
		goto done;
	}
	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		UW	t0 = knl_usb_ms(), ms;

		/* a device without a write cache has its writes on the medium already */
		req->error = !m->medium ? E_NOMDA : m->wce ? scsi_sync(m) : E_OK;
		ms = knl_usb_ms() - t0;
		m->n_sync++;
		m->ms_sync += ms;
		if ( ms > m->max_sync ) m->max_sync = ms;
		goto done;
	}
	/* the partition table read again after it was written (gpt.c) */
	if ( req->start == TDN_PARTSCAN && req->cmd == TDC_WRITE ) {
		INT	n = ( unitno != 0 || !m->medium || m->blksz != BLK_SECTOR_SIZE ) ? E_PAR
			    : knl_part_rescan(msc_readfn, m, m->nblocks, &m->tbl);

		req->error = ( n < E_OK ) ? n : msc_register(m);
		goto done;
	}
	if ( req->start == TDN_FUAWRITE && req->cmd == TDC_WRITE ) {
		/* blocks written through to the medium, when the device says it can */
		CONST T_DEVFUA	*fw = (CONST T_DEVFUA *)req->buf;

		if ( !m->medium ) {
			req->error = E_NOMDA;
		} else if ( !m->fua || req->size < (SZ)sizeof(T_DEVFUA) || fw->size <= 0
			 || ( fw->size % m->blksz ) != 0 || fw->start < 0
			 || unit_range(m, unitno, &start, &nblk) < E_OK
			 || (UD)fw->start + (UD)fw->size / m->blksz > nblk ) {
			req->error = E_PAR;
		} else if ( m->readonly ) {
			req->error = E_RONLY;
		} else {
			UW	t0 = knl_usb_ms(), ms;

			req->error = msc_xfer(m, TRUE, start + (UD)fw->start, (UB *)fw->buf,
					      (UD)fw->size / m->blksz, TRUE);
			if ( req->error >= E_OK ) req->asize = req->size;
			ms = knl_usb_ms() - t0;
			m->n_wr++;
			m->ms_wr += ms;
			if ( ms > m->max_wr ) m->max_wr = ms;
		}
		goto done;
	}
	if ( req->start < 0 ) {
		req->error = msc_attr(m, unitno, req);
		goto done;
	}
	if ( !m->medium ) {
		req->error = E_NOMDA;
		goto done;
	}
	er = unit_range(m, unitno, &start, &nblk);
	if ( er < E_OK ) {
		req->error = er;
		goto done;
	}
	if ( (req->size % m->blksz) != 0 ) {
		req->error = E_PAR;
		goto done;
	}
	nreq = (UD)req->size / m->blksz;
	if ( (UD)req->start >= nblk || (UD)req->start + nreq > nblk ) {
		req->error = E_PAR;
		goto done;
	}
	if ( req->cmd == TDC_WRITE && m->readonly ) {
		req->error = E_RONLY;
		goto done;
	}
	{
		UW	t0 = knl_usb_ms(), ms;

		er = msc_xfer(m, req->cmd == TDC_WRITE, start + (UD)req->start,
			      (UB *)req->buf, nreq, FALSE);
		ms = knl_usb_ms() - t0;
		if ( req->cmd == TDC_WRITE ) {
			m->n_wr++;
			m->ms_wr += ms;
			if ( ms > m->max_wr ) m->max_wr = ms;
		} else {
			m->n_rd++;
			m->ms_rd += ms;
			if ( ms > m->max_rd ) m->max_rd = ms;
		}
	}
	if ( er >= E_OK ) {
		req->asize = req->size;
	}
	req->error = er;

    done:
	knl_usb_unlock();

	return E_OK;
}

/* Requests complete inside execfn, so the wait function only reports them */
LOCAL INT msc_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER msc_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT msc_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* The disk under its name, the partitions as its subunits */
LOCAL ER msc_register( MSCDEV *m )
{
	T_DDEV	ddev;
	ID	id;

	ddev.exinf   = m;
	ddev.drvatr  = 0;
	ddev.devatr  = m->cdrom ? TDK_DISK_CDROM : TDK_DISK_HD;
	ddev.nsub    = m->tbl.n;
	ddev.blksz   = (SZ)m->blksz;
	ddev.openfn  = (FP)msc_open;
	ddev.closefn = (FP)msc_close;
	ddev.execfn  = (FP)msc_exec;
	ddev.waitfn  = (FP)msc_wait;
	ddev.abortfn = (FP)msc_abort;
	ddev.eventfn = (FP)msc_event;
	id = tk_def_dev(m->devnm, &ddev, NULL);
	if ( id <= 0 ) {
		return id;
	}
	m->devid = id;
	m->registered = TRUE;
	msc_notify = TRUE;

	return E_OK;
}

/* The medium's size and partitions, read again when a medium goes in */
LOCAL void msc_medium( MSCDEV *m )
{
	knl_memset(&m->tbl, 0, sizeof(m->tbl));
	m->nblocks = 0;
	m->medium = scsi_ready(m, 5) && scsi_capacity(m) >= E_OK;
	if ( !m->medium ) {
		return;
	}
	if ( !m->cdrom && m->blksz == BLK_SECTOR_SIZE ) {
		UB	b0[BLK_SECTOR_SIZE];

		/* the first block read before the mode is asked: some devices answer it only after a read */
		(void)msc_xfer(m, FALSE, 0, b0, 1, FALSE);
	}
	m->wce = m->fua = FALSE;
	m->readonly = m->cdrom ? TRUE : scsi_protected(m);
	if ( m->blksz == BLK_SECTOR_SIZE && !m->cdrom ) {
		knl_read_parttbl(msc_readfn, m, m->nblocks, &m->tbl);
	}
}

/* ---------------------------------------------------------------- attach */

EXPORT BOOL knl_msc_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( dev->cfg == NULL ) {
		return FALSE;
	}
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[5] == USB_CLASS_MSC
		  && p[6] == MSC_SUBCLASS_SCSI && p[7] == MSC_PROTO_BOT ) {
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * The name of the disk at place n: "ud" and n in letters, a to z, then
 * aa to az, ba ... as the columns of a spreadsheet are counted.
 */
LOCAL void msc_name( INT n, UB *out )
{
	UB	t[4];
	INT	k = 0, i;

	n++;
	while ( n > 0 && k < (INT)sizeof(t) ) {
		n--;
		t[k++] = (UB)( 'a' + n % 26 );
		n /= 26;
	}
	out[0] = 'u';
	out[1] = 'd';
	for ( i = 0; i < k; i++ ) {
		out[2 + i] = t[k - 1 - i];
	}
	out[2 + k] = 0;
}

EXPORT ER knl_msc_attach( USBDEV *dev )
{
	MSCDEV	*m = NULL;
	UB	*p, *end = dev->cfg + dev->cfglen, *id = NULL;
	UB	cdb[6], inq[36];
	INT	i, n = 0;
	ER	er;

	for ( i = 0; ; i++ ) {
		if ( i == MSC_N ) {
			INT	cap = ( MSC_N > 0 ) ? MSC_N * 2 : MSC_FIRST, k;
			MSCTBL	*t = (MSCTBL *)Kcalloc(1, sizeof(MSCTBL) + sizeof(MSCDEV *) * (SZ)cap);

			if ( t == NULL ) {
				return E_NOMEM;
			}
			for ( k = 0; k < MSC_N; k++ ) {
				t->e[k] = MSC_AT(k);
			}
			t->cap = cap;
			__sync_synchronize();
			msc_tbl = t;
		}
		if ( MSC_AT(i) == NULL ) {
			MSCDEV	*n = (MSCDEV *)Kcalloc(1, sizeof(MSCDEV));

			if ( n == NULL ) {
				return E_NOMEM;
			}
			__sync_synchronize();
			msc_tbl->e[i] = n;
		}
		if ( !MSC_AT(i)->used ) {
			m = MSC_AT(i);
			break;			/* i is its place */
		}
	}
	knl_memset(m, 0, sizeof(*m));
	m->no = i;
	m->dev = dev;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			if ( id != NULL ) {
				break;			/* the next interface */
			}
			if ( p[5] == USB_CLASS_MSC && p[6] == MSC_SUBCLASS_SCSI
			  && p[7] == MSC_PROTO_BOT && p[3] == 0 ) {
				id = p;
				m->ifno = p[2];
			}
			continue;
		}
		if ( id != NULL && p[1] == USB_DT_ENDPOINT && p[0] >= 7
		  && (p[3] & 3) == USB_EP_BULK ) {
			UB	*comp = knl_usb_next_desc(p, end);

			if ( (p[2] & 0x80) != 0 ) {
				knl_usb_ep_from_desc(&m->ep_in, dev, p, comp);
			} else {
				knl_usb_ep_from_desc(&m->ep_out, dev, p, comp);
			}
		}
	}
	if ( m->ep_in.addr == 0 || m->ep_out.addr == 0 ) {
		USB_LOG("usb: disk: no bulk endpoints\n");
		return E_NOSPT;
	}
	m->used = TRUE;

	knl_memset(cdb, 0, sizeof(cdb));
	knl_memset(inq, 0, sizeof(inq));
	cdb[0] = SCSI_INQUIRY;
	cdb[4] = sizeof(inq);
	i = bot_cmd(m, cdb, 6, inq, sizeof(inq), TRUE, &n);
	if ( i != 0 || n < 36 || ( (inq[0] & 0x1f) != SCSI_TYPE_DISK && (inq[0] & 0x1f) != SCSI_TYPE_CDROM ) ) {
		USB_LOG("usb: disk: INQUIRY answered %d, %d bytes, type %d\n",
			i, n, inq[0] & 0x1f);
		/* not a disk this driver serves (a card reader's other LUN, a tape) */
		m->used = FALSE;
		return E_NOSPT;
	}
	m->cdrom = (BOOL)( (inq[0] & 0x1f) == SCSI_TYPE_CDROM );
	m->removable = (BOOL)( (inq[1] & 0x80) != 0 || m->cdrom );
	knl_memcpy(m->vendor, inq + 8, 8);
	knl_memcpy(m->model, inq + 16, 16);
	msc_medium(m);

	msc_name(m->no, m->devnm);
	er = msc_register(m);
	if ( er < E_OK ) {
		m->used = FALSE;
		return er;
	}
	m->last_poll = knl_usb_ms();
	dev->msc = m;
	knl_memcpy(dev->devnm, m->devnm, sizeof(m->devnm));
	USB_LOG("usb: %s: %s %s, %d blocks of %d%s%s%s, %d partitions, write cache %s\n",
		m->devnm, m->vendor, m->model, (UW)m->nblocks, (INT)m->blksz,
		m->cdrom ? ", CD-ROM" : "", m->removable ? ", removable" : "",
		m->readonly ? ", read only" : "", m->tbl.n,
		m->wce ? ( m->fua ? "on (FUA)" : "on" ) : "taken as off");

	return E_OK;
}

/* Let the name go, if nobody has the disk open any more */
LOCAL void msc_unregister( MSCDEV *m )
{
	if ( m->registered && tk_def_dev(m->devnm, NULL, NULL) < E_OK ) {
		return;				/* still open: tried again later */
	}
	m->registered = FALSE;
	m->used = FALSE;
	m->notify_media = FALSE;	/* its going is told as the device's */
	msc_notify = TRUE;
}

EXPORT void knl_msc_detach( USBDEV *dev )
{
	MSCDEV	*m = (MSCDEV *)dev->msc;

	if ( m == NULL ) {
		return;
	}
	m->gone = TRUE;
	m->medium = FALSE;
	m->dev = NULL;
	dev->msc = NULL;
	/*
	 * Still open, the name stays until the last user closes it; the
	 * object layer is told the medium went meanwhile, so that a volume
	 * mounted on it lets go of the disk.
	 */
	m->notify_media = TRUE;
	msc_unregister(m);
}

/*
 * From the manager's task: names that were still open go once they are
 * closed, and a removable disk is asked now and then whether its medium
 * went in or out.
 */
EXPORT void knl_msc_poll( void )
{
	INT	i;
	UW	now = knl_usb_ms();

	for ( i = 0; i < MSC_N; i++ ) {
		MSCDEV	*m = MSC_AT(i);
		BOOL	was;

		if ( m == NULL || !m->used ) {
			continue;
		}
		if ( m->gone ) {
			msc_unregister(m);
			continue;
		}
		if ( !m->removable || (INT)(now - m->last_poll) < MEDIA_POLL_MS ) {
			continue;
		}
		m->last_poll = now;
		was = m->medium;
		if ( scsi_ready(m, 1) == was ) {
			continue;
		}
		msc_medium(m);
		tm_printf((UB *)"usb: %s: the medium %s (asked again: %s)\n", m->devnm,
			  was ? "did not answer ready" : "answered ready",
			  m->medium ? "there" : "not there");
		if ( m->medium != was ) {
			/* the partitions may differ: the subunits follow them */
			msc_register(m);
			m->notify_media = TRUE;
		}
	}
}

/* Outside the manager's lock: tell the object layer what changed */
EXPORT void knl_msc_notify( void )
{
	INT	i;

	if ( msc_notify ) {
		msc_notify = FALSE;
		knl_obdev_changed();
	}
	for ( i = 0; i < MSC_N; i++ ) {
		MSCDEV	*m = MSC_AT(i);

		if ( m != NULL && m->used && m->notify_media ) {
			m->notify_media = FALSE;
			knl_obdev_media(m->devnm, m->medium);
		}
	}
}

/*
 * For USB.TXT: each disk's reads, writes and flushes so far, how many,
 * how long in all and the longest (ms). Answers the length written.
 */
EXPORT INT knl_msc_report( char *buf, INT max )
{
	INT	i, n = 0;

	for ( i = 0; i < MSC_N && n < max - 200; i++ ) {
		MSCDEV	*m = MSC_AT(i);

		if ( m == NULL || !m->used ) {
			continue;
		}
		n += tm_sprintf((UB *)buf + n, (CONST UB *)"  disk %s%s: reads %u in %u ms (longest %u), writes %u in %u ms "
				"(longest %u), flushes %u in %u ms (longest %u), failed %u, recovered %u\n", m->devnm, m->wce ? ( m->fua ? " (write cache, FUA)" : " (write cache)" ) : "", m->n_rd, m->ms_rd, m->max_rd,
				m->n_wr, m->ms_wr, m->max_wr, m->n_sync, m->ms_sync, m->max_sync, m->n_fail, m->n_recover);
	}
	if ( n < max ) buf[n] = '\0';
	return n;
}

/*
 * For the tests: the next n commands of a disk time out on the
 * controller, a status read that nothing answers, the device then
 * brought back as when the transport fails; with bot, by the Bulk-Only
 * reset instead of the port reset. Answers the recoveries made so far.
 */
EXPORT INT knl_msc_fault( CONST char *devnm, INT n, BOOL bot )
{
	INT	i, k;

	for ( i = 0; i < MSC_N; i++ ) {
		MSCDEV	*m = MSC_AT(i);

		if ( m == NULL || !m->used ) {
			continue;
		}
		for ( k = 0; devnm[k] != '\0' && devnm[k] == (char)m->devnm[k]; k++ ) ;
		if ( devnm[k] == '\0' && m->devnm[k] == '\0' ) {
			m->fault = n;
			m->fault_bot = bot;
			return (INT)m->n_recover;
		}
	}
	return E_NOEXS;
}

/* Whether a removable disk wants its medium looked at */
EXPORT BOOL knl_msc_wants_poll( void )
{
	INT	i;

	for ( i = 0; i < MSC_N; i++ ) {
		if ( MSC_AT(i) != NULL && MSC_AT(i)->used && ( MSC_AT(i)->gone || MSC_AT(i)->removable ) ) {
			return TRUE;
		}
	}

	return FALSE;
}
