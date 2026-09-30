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
 *	Each disk is registered with tk_def_dev() as "uda", "udb", ... in
 *	the way of the other disks (design 10.6): subunit 0 is the whole
 *	disk and the partitions of its GPT or MBR follow, so "uda0" is the
 *	first partition. The object layer is told whenever one comes or goes,
 *	and when the medium of a removable one goes in or out.
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

#define MSC_MAX			8
#define MSC_SUBCLASS_SCSI	0x06
#define MSC_PROTO_BOT		0x50
#define MSC_REQ_RESET		0xff
#define MSC_XFER_MAX		65536	/* bytes per command: the bulk buffer */
#define MSC_TMO			5000	/* ms for a data stage */
#define MEDIA_POLL_MS		5000	/* a removable medium is looked for */

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
	BOOL	medium;		/* a medium is in */
	BOOL	notify_media;	/* the object layer is to hear of it */
	UW	last_poll;
	T_PARTTBL tbl;
	UB	devnm[8];
	ID	devid;
	UB	vendor[9], model[17];
} MSCDEV;

LOCAL MSCDEV	msc_tbl[MSC_MAX];
LOCAL BOOL	msc_notify = FALSE;	/* a disk came or went */

/* ---------------------------------------------------------------- transport */

/* The class reset, then both endpoints cleared: the device starts over */
LOCAL void bot_reset( MSCDEV *m )
{
	knl_usb_control(m->dev, USB_RT_CLASS | USB_RT_INTERFACE, MSC_REQ_RESET,
			0, (UH)m->ifno, NULL, 0, NULL);
	knl_usb_clear_halt(m->dev, m->ep_in.addr);
	knl_usb_clear_halt(m->dev, m->ep_out.addr);
}

LOCAL ER bulk( MSCDEV *m, BOOL in, UB *data, INT len, INT *actual, TMO tmo )
{
	USBHC	*hc = m->dev->hc;

	return hc->ops->bulk(hc, m->dev, in ? &m->ep_in : &m->ep_out, data, len,
			     actual, tmo);
}

/*
 * One command: len bytes of data in or out. Answers the status the
 * device gave (0 passed, 1 failed), or an error of the transport.
 * *moved is how much data went.
 */
LOCAL INT bot_cmd( MSCDEV *m, CONST UB *cdb, INT cdblen, UB *data, INT len,
		   BOOL in, INT *moved )
{
	UB	cbw[31], csw[13];
	INT	done = 0, n = 0, i;
	ER	er;

	if ( moved != NULL ) {
		*moved = 0;
	}
	if ( m->gone ) {
		return E_IO;
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
	er = bulk(m, FALSE, cbw, sizeof(cbw), &n, 1000);
	if ( er < E_OK ) {
		bot_reset(m);
		return er;
	}

	/* the data, in pieces the controller's buffer holds */
	while ( done < len ) {
		INT	piece = len - done;

		if ( piece > MSC_XFER_MAX ) {
			piece = MSC_XFER_MAX;
		}
		n = 0;
		er = bulk(m, in, data + done, piece, &n, MSC_TMO);
		if ( er == E_OBJ ) {
			/* it stalled the data: clear it, the status still comes */
			knl_usb_clear_halt(m->dev, in ? m->ep_in.addr : m->ep_out.addr);
			break;
		}
		if ( er < E_OK ) {
			bot_reset(m);
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
	er = bulk(m, TRUE, csw, sizeof(csw), &n, 1000);
	if ( er == E_OBJ ) {
		knl_usb_clear_halt(m->dev, m->ep_in.addr);
		er = bulk(m, TRUE, csw, sizeof(csw), &n, 1000);
	}
	if ( er < E_OK || n < 13 || GET32(csw) != CSW_SIG || GET32(csw + 4) != m->tag ) {
		USB_LOG("usb: disk: status %d, %d bytes, %08x tag %08x (%08x)\n",
			(INT)er, n, GET32(csw), GET32(csw + 4), m->tag);
		bot_reset(m);
		return ( er < E_OK ) ? er : E_IO;
	}
	if ( csw[12] == 2 ) {			/* the device lost step */
		bot_reset(m);
		return E_IO;
	}

	return csw[12];
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

/* Whether the medium is write protected, from the mode parameter header */
LOCAL BOOL scsi_protected( MSCDEV *m )
{
	UB	cdb[6], d[4];
	INT	n = 0;

	knl_memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_MODE_SENSE6;
	cdb[2] = 0x3f;				/* every page */
	cdb[4] = sizeof(d);
	if ( bot_cmd(m, cdb, 6, d, sizeof(d), TRUE, &n) != 0 || n < 4 ) {
		scsi_sense(m, NULL);		/* many do not answer it */
		return FALSE;
	}

	return ( (d[2] & 0x80) != 0 );
}

/* Read or write nblk blocks from block lba */
LOCAL ER scsi_rw( MSCDEV *m, BOOL wr, UD lba, UB *buf, UW nblk )
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
	len = (INT)(nblk * m->blksz);
	st = bot_cmd(m, cdb, cdblen, buf, len, !wr, &moved);
	if ( st < 0 ) {
		return st;
	}
	if ( st != 0 ) {
		UB	asc = 0;
		INT	key = scsi_sense(m, &asc);

		if ( key == 2 || asc == 0x3a ) {
			return E_NOMDA;		/* not ready, or no medium */
		}
		if ( key == 7 ) {
			return E_RONLY;		/* data protect */
		}
		return E_IO;
	}

	return ( moved == len ) ? E_OK : E_IO;
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
	}
	return E_OK;
}

/* ---------------------------------------------------------------- disk device */

/* A request in blocks, cut to what one command carries */
LOCAL ER msc_xfer( MSCDEV *m, BOOL wr, UD lba, UB *buf, UD nblk )
{
	UW	per = MSC_XFER_MAX / m->blksz;
	ER	er = E_OK;

	while ( nblk > 0 && er >= E_OK ) {
		UW	n = ( nblk > per ) ? per : (UW)nblk;

		er = scsi_rw(m, wr, lba, buf, n);
		lba += n;
		buf += (UBINT)n * m->blksz;
		nblk -= n;
	}

	return er;
}

/* The partition scanner reads 512 byte sectors of the whole disk */
LOCAL ER msc_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	return msc_xfer((MSCDEV *)exinf, FALSE, start, (UB *)buf, nsect);
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
	info->blockcont = (W)nblk;
	req->asize = sizeof(DiskInfo);

	return E_OK;
}

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
		req->error = m->medium ? scsi_sync(m) : E_NOMDA;
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
	er = msc_xfer(m, req->cmd == TDC_WRITE, start + (UD)req->start,
		      (UB *)req->buf, nreq);
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
	ddev.devatr  = TDK_DISK_HD;
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
	m->readonly = scsi_protected(m);
	if ( m->blksz == BLK_SECTOR_SIZE ) {
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

EXPORT ER knl_msc_attach( USBDEV *dev )
{
	MSCDEV	*m = NULL;
	UB	*p, *end = dev->cfg + dev->cfglen, *id = NULL;
	UB	cdb[6], inq[36];
	INT	i, n = 0;
	ER	er;

	for ( i = 0; i < MSC_MAX; i++ ) {
		if ( !msc_tbl[i].used ) {
			m = &msc_tbl[i];
			break;
		}
	}
	if ( m == NULL ) {
		return E_LIMIT;
	}
	knl_memset(m, 0, sizeof(*m));
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
	if ( i != 0 || n < 36 || (inq[0] & 0x1f) != 0x00 ) {
		USB_LOG("usb: disk: INQUIRY answered %d, %d bytes, type %d\n",
			i, n, inq[0] & 0x1f);
		/* not a disk this driver serves (a CD-ROM, a card reader's other LUN) */
		m->used = FALSE;
		return E_NOSPT;
	}
	m->removable = ( (inq[1] & 0x80) != 0 );
	knl_memcpy(m->vendor, inq + 8, 8);
	knl_memcpy(m->model, inq + 16, 16);
	msc_medium(m);

	m->devnm[0] = 'u';
	m->devnm[1] = 'd';
	m->devnm[2] = (UB)('a' + (m - msc_tbl));
	m->devnm[3] = 0;
	er = msc_register(m);
	if ( er < E_OK ) {
		m->used = FALSE;
		return er;
	}
	m->last_poll = knl_usb_ms();
	dev->msc = m;
	knl_memcpy(dev->devnm, m->devnm, sizeof(m->devnm));
	USB_LOG("usb: %s: %s %s, %d blocks of %d%s%s, %d partitions\n",
		m->devnm, m->vendor, m->model, (UW)m->nblocks, (INT)m->blksz,
		m->removable ? ", removable" : "", m->readonly ? ", read only" : "",
		m->tbl.n);

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

	for ( i = 0; i < MSC_MAX; i++ ) {
		MSCDEV	*m = &msc_tbl[i];
		BOOL	was;

		if ( !m->used ) {
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
	for ( i = 0; i < MSC_MAX; i++ ) {
		MSCDEV	*m = &msc_tbl[i];

		if ( m->used && m->notify_media ) {
			m->notify_media = FALSE;
			knl_obdev_media(m->devnm, m->medium);
		}
	}
}

/* Whether a removable disk wants its medium looked at */
EXPORT BOOL knl_msc_wants_poll( void )
{
	INT	i;

	for ( i = 0; i < MSC_MAX; i++ ) {
		if ( msc_tbl[i].used && ( msc_tbl[i].gone || msc_tbl[i].removable ) ) {
			return TRUE;
		}
	}

	return FALSE;
}
