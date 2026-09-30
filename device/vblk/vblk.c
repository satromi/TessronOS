/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vblk.c
 *	virtio-blk over virtio-mmio (QEMU virt), design 10.4.
 *
 *	Each device found is registered with tk_def_dev() as "vblka",
 *	"vblkb", ... Subunit 0 is the whole device and subunits 1..n are the
 *	partitions read from the GPT or MBR, so "vblka" addresses the disk
 *	and "vblka0" the first partition.
 *
 *	Up to VBLK_SLOTS requests are in flight at once, each in a slot of
 *	its own: a run of descriptors, a header and a status. The completion
 *	interrupt walks the used ring and sets the event flag bit of each
 *	slot that came back, which the task that asked waits on, so the
 *	wait happens on the caller's processor while the handler may run on
 *	another (design 10.2, 11.10).
 */

#include <sys/machine.h>

#ifdef QEMU_VIRT

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include "sysman/pfalloc.h"

/* ---------------------------------------------------------------- virtio-mmio */
#define VIRTIO_MMIO_MAGIC		0x000	/* "virt" */
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW	0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG		0x100

#define VIRTIO_MAGIC_VALUE		0x74726976
#define VIRTIO_ID_BLOCK			2

#define VIRTIO_STATUS_ACKNOWLEDGE	1
#define VIRTIO_STATUS_DRIVER		2
#define VIRTIO_STATUS_DRIVER_OK		4
#define VIRTIO_STATUS_FEATURES_OK	8
#define VIRTIO_STATUS_FAILED		128

#define VIRTIO_F_VERSION_1		32	/* feature bit */
#define VIRTIO_BLK_F_RO			5
#define VIRTIO_BLK_F_FLUSH		9	/* the device takes VIRTIO_BLK_T_FLUSH */

/* Descriptor flags */
#define VRING_DESC_F_NEXT		1
#define VRING_DESC_F_WRITE		2

#define VBLK_QSIZE			64	/* descriptors: a request is a header, its pieces and a status */
#define VBLK_SLOTS			4	/* requests in flight at once */
#define VBLK_SLOT_DESC			( VBLK_QSIZE / VBLK_SLOTS )
#define VBLK_SLOT_DATA			( VBLK_SLOT_DESC - 2 )	/* pieces of one request */

typedef struct {
	UD	addr;
	UW	len;
	UH	flags;
	UH	next;
} VRING_DESC;

typedef struct {
	UH	flags;
	UH	idx;
	UH	ring[VBLK_QSIZE];
	UH	used_event;
} VRING_AVAIL;

typedef struct {
	UW	id;
	UW	len;
} VRING_USED_ELEM;

typedef struct {
	UH		flags;
	UH		idx;
	VRING_USED_ELEM	ring[VBLK_QSIZE];
	UH		avail_event;
} VRING_USED;

/* virtio-blk request header and status */
#define VIRTIO_BLK_T_IN			0
#define VIRTIO_BLK_T_OUT		1
#define VIRTIO_BLK_T_FLUSH		4
#define VIRTIO_BLK_S_OK			0

typedef struct {
	UW	type;
	UW	reserved;
	UD	sector;
} VBLK_REQ_HDR;

/*
 * The header and the status of one slot, in a cache line of their own:
 * cleaning the line of one slot must not write back over a status the
 * device has just put in another.
 */
typedef struct {
	VBLK_REQ_HDR	hdr;
	UB		status;
	UB		pad[64 - sizeof(VBLK_REQ_HDR) - 1];
} VBLK_SLOTAREA;

/*
 * The queue and the slots share one page so that a single allocation
 * covers everything the device reads or writes besides the caller's
 * buffer. The page comes from the linear map, so its physical address
 * is its virtual address minus KVA_BASE.
 */
typedef struct {
	VRING_DESC	desc[VBLK_QSIZE];
	VRING_AVAIL	avail;
	UB		pad[64 - (sizeof(VRING_AVAIL) % 64)];
	VRING_USED	used;
	UB		pad2[64 - (sizeof(VRING_USED) % 64)];
	VBLK_SLOTAREA	slot[VBLK_SLOTS];
} VBLK_SHARED;

#define VBLK_MAX_DEV		4

typedef struct {
	UBINT		base;			/* virtio-mmio registers */
	UINT		intno;
	VBLK_SHARED	*sh;			/* shared area (linear map) */
	UD		sh_pa;
	UH		last_used;		/* used ring index consumed */
	UD		capacity;		/* sectors */
	BOOL		readonly;
	BOOL		flush;			/* it keeps a cache that can be emptied */
	ID		mtxid;			/* the slots and the avail ring */
	ID		semid;			/* free slots */
	ID		flgid;			/* completion: one bit a slot */
	BOOL		busy[VBLK_SLOTS];
	ID		devid;
	T_PARTTBL	tbl;
	UB		devnm[8];
} VBLK_DEV;

LOCAL VBLK_DEV	vblk_dev[VBLK_MAX_DEV];
LOCAL INT	vblk_ndev = 0;

#define VBLK_FLG_DONE		0x0001

/* ---------------------------------------------------------------- helpers */

LOCAL void vblk_out( VBLK_DEV *d, UW off, UW val )
{
	out_w(d->base + off, val);
}

LOCAL UW vblk_in( VBLK_DEV *d, UW off )
{
	return in_w(d->base + off);
}

/*
 * Clean and invalidate a range so that the device sees what was written
 * and the CPU does not keep stale lines of what the device wrote. The
 * emulator is coherent, real hardware may not be.
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

/* ---------------------------------------------------------------- interrupt */

/* The slots whose requests came back, taken off the used ring; only
   the interrupt handler moves `last_used` */
LOCAL UINT vblk_reap( VBLK_DEV *d )
{
	VBLK_SHARED	*sh = d->sh;
	UINT		done = 0;
	UW		id;

	dcache_flush(&sh->used, sizeof(sh->used));
	while ( d->last_used != sh->used.idx ) {
		id = sh->used.ring[d->last_used % VBLK_QSIZE].id;
		if ( id < VBLK_QSIZE ) {
			done |= 1U << ( id / VBLK_SLOT_DESC );
		}
		d->last_used++;
	}
	return done;
}

LOCAL void vblk_inthdr( UINT intno, UW iar )
{
	UINT	done;
	INT	i;

	for ( i = 0; i < vblk_ndev; i++ ) {
		VBLK_DEV *d = &vblk_dev[i];

		if ( d->intno != intno ) continue;
		vblk_out(d, VIRTIO_MMIO_INTERRUPT_ACK, vblk_in(d, VIRTIO_MMIO_INTERRUPT_STATUS));
		done = vblk_reap(d);
		if ( done != 0 ) {
			tk_set_flg(d->flgid, done);
		}
	}
	EndOfInt(intno);
}

/* ---------------------------------------------------------------- transfer */

/* A free slot, waited for when all are in flight */
LOCAL INT vblk_slot_get( VBLK_DEV *d, TMO tmout )
{
	INT	k;
	ER	er;

	er = tk_wai_sem(d->semid, 1, tmout);
	if ( er < E_OK ) {
		return er;
	}
	tk_loc_mtx(d->mtxid, TMO_FEVR);
	for ( k = 0; k < VBLK_SLOTS && d->busy[k]; k++ ) ;
	d->busy[k] = TRUE;			/* the semaphore says one is free */
	tk_unl_mtx(d->mtxid);
	tk_clr_flg(d->flgid, ~( 1U << k ));
	return k;
}

LOCAL void vblk_slot_put( VBLK_DEV *d, INT k )
{
	tk_loc_mtx(d->mtxid, TMO_FEVR);
	d->busy[k] = FALSE;
	tk_unl_mtx(d->mtxid);
	tk_sig_sem(d->semid, 1);
}

/*
 * The request of slot k, `ndesc` descriptors long, handed to the device
 * and waited for. A request that does not come back in time keeps its
 * slot, which the device may still write into.
 */
LOCAL ER vblk_run( VBLK_DEV *d, INT k, INT ndesc, TMO tmout, BOOL *p_lost )
{
	VBLK_SHARED	*sh = d->sh;
	UINT		flgptn;
	UH		idx;
	ER		er;

	dcache_flush(&sh->desc[k * VBLK_SLOT_DESC], sizeof(VRING_DESC) * (UD)ndesc);
	dcache_flush(&sh->slot[k], sizeof(VBLK_SLOTAREA));

	tk_loc_mtx(d->mtxid, TMO_FEVR);
	idx = sh->avail.idx;
	sh->avail.ring[idx % VBLK_QSIZE] = (UH)( k * VBLK_SLOT_DESC );	/* head descriptor */
	Asm("dmb ishst" ::: "memory");			/* the ring entry before the index */
	sh->avail.idx = idx + 1;
	dcache_flush(&sh->avail, sizeof(sh->avail));
	vblk_out(d, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
	tk_unl_mtx(d->mtxid);

	er = tk_wai_flg(d->flgid, 1U << k, TWF_ORW | TWF_BITCLR, &flgptn, tmout);
	if ( er < E_OK ) {
		*p_lost = TRUE;
		return er;
	}
	dcache_flush(&sh->slot[k], sizeof(VBLK_SLOTAREA));
	return ( sh->slot[k].status == VIRTIO_BLK_S_OK ) ? E_OK : E_IO;
}

/*
 * One request in slot k: the header, as many pieces of the buffer as
 * the slot holds, and the status.
 *
 * The card reads and writes memory itself, so what goes in a descriptor
 * is a physical address. A buffer in the kernel's own linear area has
 * one, found by subtraction; a buffer made of pages mapped together
 * elsewhere -- which is what a font, a picture or anything else of a
 * few megabytes is -- has a different physical address for every page
 * of it, and subtracting would name memory belonging to nobody. The
 * card would then be given an address it writes nothing useful to, the
 * request would come back saying it succeeded, and the buffer would
 * still hold whatever it held before.
 *
 * So the buffer is walked and handed over in as many pieces as it is
 * physically in, one descriptor each. The card treats a chain as one
 * run of bytes, so the pieces need not fall on sector boundaries. What
 * does not fit is left for the next request, cut at a sector so that
 * each request is whole sectors. The sectors done are put in *p_done.
 */
LOCAL ER vblk_xfer1( VBLK_DEV *d, INT k, UW type, UD sector, UB *buf, UD nsect,
		     TMO tmout, UD *p_done, BOOL *p_lost )
{
	VBLK_SHARED	*sh = d->sh;
	VRING_DESC	*dsc = &sh->desc[k * VBLK_SLOT_DESC];
	UH		base = (UH)( k * VBLK_SLOT_DESC );
	UD		left = nsect * BLK_SECTOR_SIZE, at = 0, cut;
	INT		n = 0;			/* data descriptors used */
	ER		er;

	sh->slot[k].hdr.type     = type;
	sh->slot[k].hdr.reserved = 0;
	sh->slot[k].hdr.sector   = sector;
	sh->slot[k].status       = 0xff;

	/* header (device reads) */
	dsc[0].addr  = d->sh_pa + ((UBINT)&sh->slot[k].hdr - (UBINT)sh);
	dsc[0].len   = sizeof(VBLK_REQ_HDR);
	dsc[0].flags = VRING_DESC_F_NEXT;
	dsc[0].next  = base + 1;

	/* the data, in as many pieces as it is in and the slot holds */
	while ( left > 0 && n < VBLK_SLOT_DATA ) {
		void	*pa = NULL;
		INT	run = ConvPhysicalAddress(buf + at, (INT)( ( left < 0x40000000 ) ? left : 0x40000000 ), &pa);

		if ( run <= 0 || pa == NULL ) {
			return E_PAR;		/* not memory the card can reach */
		}
		dsc[1 + n].addr  = (UD)(UBINT)pa;
		dsc[1 + n].len   = (UW)run;
		dsc[1 + n].flags = VRING_DESC_F_NEXT
				 | ((type == VIRTIO_BLK_T_IN) ? VRING_DESC_F_WRITE : 0);
		dsc[1 + n].next  = (UH)( base + 2 + n );
		at += (UD)run;
		left -= (UD)run;
		n++;
	}
	if ( left > 0 ) {
		/* the slot is full: this request ends at the last whole sector */
		if ( at < BLK_SECTOR_SIZE ) {
			return E_PAR;
		}
		cut = at % BLK_SECTOR_SIZE;
		while ( cut > 0 ) {
			if ( dsc[n].len > cut ) {
				dsc[n].len -= (UW)cut;
				cut = 0;
			} else {
				cut -= dsc[n].len;
				n--;
			}
		}
		at -= at % BLK_SECTOR_SIZE;
	}

	/* status (device writes) */
	dsc[1 + n].addr  = d->sh_pa + ((UBINT)&sh->slot[k].status - (UBINT)sh);
	dsc[1 + n].len   = 1;
	dsc[1 + n].flags = VRING_DESC_F_WRITE;
	dsc[1 + n].next  = 0;
	dsc[n].next      = (UH)( base + 1 + n );

	if ( type == VIRTIO_BLK_T_OUT ) {
		dcache_flush(buf, at);
	}
	er = vblk_run(d, k, n + 2, tmout, p_lost);
	if ( er < E_OK ) {
		return er;
	}
	if ( type == VIRTIO_BLK_T_IN ) {
		dcache_flush(buf, at);
	}
	*p_done = at / BLK_SECTOR_SIZE;

	return E_OK;
}

/*
 * The device's cache onto the medium: a request of a header and a
 * status with no data. A device that did not offer the feature writes
 * through, and there is nothing to ask of it.
 */
LOCAL ER vblk_flush( VBLK_DEV *d, TMO tmout )
{
	VBLK_SHARED	*sh = d->sh;
	VRING_DESC	*dsc;
	BOOL		lost = FALSE;
	INT		k;
	ER		er;

	if ( !d->flush ) {
		return E_OK;
	}
	k = vblk_slot_get(d, tmout);
	if ( k < 0 ) {
		return (ER)k;
	}
	dsc = &sh->desc[k * VBLK_SLOT_DESC];
	sh->slot[k].hdr.type     = VIRTIO_BLK_T_FLUSH;
	sh->slot[k].hdr.reserved = 0;
	sh->slot[k].hdr.sector   = 0;
	sh->slot[k].status       = 0xff;
	dsc[0].addr  = d->sh_pa + ((UBINT)&sh->slot[k].hdr - (UBINT)sh);
	dsc[0].len   = sizeof(VBLK_REQ_HDR);
	dsc[0].flags = VRING_DESC_F_NEXT;
	dsc[0].next  = (UH)( k * VBLK_SLOT_DESC + 1 );
	dsc[1].addr  = d->sh_pa + ((UBINT)&sh->slot[k].status - (UBINT)sh);
	dsc[1].len   = 1;
	dsc[1].flags = VRING_DESC_F_WRITE;
	dsc[1].next  = 0;

	er = vblk_run(d, k, 2, tmout, &lost);
	if ( !lost ) {
		vblk_slot_put(d, k);
	}
	return er;
}

/*
 * The sectors from 'sector' on, in as many requests as the buffer's
 * pieces need, one slot held throughout
 */
LOCAL ER vblk_xfer( VBLK_DEV *d, UW type, UD sector, void *buf, UD nsect, TMO tmout )
{
	UB	*p = (UB *)buf;
	UD	done;
	BOOL	lost = FALSE;
	INT	k;
	ER	er = E_OK;

	k = vblk_slot_get(d, tmout);
	if ( k < 0 ) {
		return (ER)k;
	}
	while ( nsect > 0 ) {
		done = 0;
		er = vblk_xfer1(d, k, type, sector, p, nsect, tmout, &done, &lost);
		if ( er < E_OK ) {
			break;
		}
		sector += done;
		p += done * BLK_SECTOR_SIZE;
		nsect -= done;
	}
	if ( !lost ) {
		vblk_slot_put(d, k);
	}
	return er;
}

/*
 * Partition scanner callback (whole device, at start-up)
 */
LOCAL ER vblk_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	return vblk_xfer((VBLK_DEV *)exinf, VIRTIO_BLK_T_IN, start, buf, nsect, 5000);
}

/* ---------------------------------------------------------------- driver interface */

/*
 * Range of the opened unit: subunit 0 is the whole device, 1..n are the
 * partitions.
 */
LOCAL ER vblk_unit_range( VBLK_DEV *d, INT unitno, UD *p_start, UD *p_nsect )
{
	if ( unitno == 0 ) {
		*p_start = 0;
		*p_nsect = d->capacity;
		return E_OK;
	}
	if ( unitno > d->tbl.n || !d->tbl.part[unitno - 1].valid ) {
		return E_NOEXS;
	}
	*p_start = d->tbl.part[unitno - 1].start;
	*p_nsect = d->tbl.part[unitno - 1].nsect;

	return E_OK;
}

LOCAL ER vblk_open( ID devid, UINT omode, void *exinf )
{
	VBLK_DEV *d = (VBLK_DEV *)exinf;

	if ( d->readonly && (omode & TD_WRITE) != 0 ) {
		return E_RONLY;
	}
	return E_OK;
}

LOCAL ER vblk_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

/*
 * Attribute data (start < 0): only TDN_DISKINFO is answered.
 */
LOCAL ER vblk_attr( VBLK_DEV *d, INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nsect;
	ER		er;

	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ ) {
		return E_PAR;
	}
	if ( req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = vblk_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		return er;
	}

	info = (DiskInfo *)req->buf;
	info->protect   = d->readonly;
	info->removable = FALSE;
	info->blocksize = BLK_SECTOR_SIZE;
	info->blockcont = (W)nsect;
	req->asize = sizeof(DiskInfo);

	return E_OK;
}

LOCAL ER vblk_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	VBLK_DEV *d = (VBLK_DEV *)exinf;
	INT	unitno = (INT)(req->devid & 0xff);
	UD	start, nsect, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;

	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		req->error = vblk_flush(d, tmout);
		return E_OK;
	}
	if ( req->start < 0 ) {
		req->error = vblk_attr(d, unitno, req);
		return E_OK;
	}

	er = vblk_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		req->error = er;
		return E_OK;
	}
	if ( (req->size % BLK_SECTOR_SIZE) != 0 ) {
		req->error = E_PAR;
		return E_OK;
	}
	nreq = (UD)req->size / BLK_SECTOR_SIZE;
	if ( (UD)req->start >= nsect || (UD)req->start + nreq > nsect ) {
		req->error = E_PAR;
		return E_OK;
	}
	if ( req->cmd == TDC_WRITE && d->readonly ) {
		req->error = E_RONLY;
		return E_OK;
	}

	er = vblk_xfer(d, ( req->cmd == TDC_READ ) ? VIRTIO_BLK_T_IN : VIRTIO_BLK_T_OUT,
			start + (UD)req->start, req->buf, nreq, tmout);

	if ( er == E_OK ) {
		req->asize = req->size;
	}
	req->error = er;

	return E_OK;
}

/*
 * Requests complete inside execfn, so the wait function only reports them.
 */
LOCAL INT vblk_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER vblk_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT vblk_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* ---------------------------------------------------------------- start-up */

/*
 * Bring one virtio-mmio slot up as a block device.
 */
LOCAL ER vblk_setup( VBLK_DEV *d )
{
	PFRAME	*pf;
	UD	features;
	UW	qmax, status;

	/* Reset, then announce the driver */
	vblk_out(d, VIRTIO_MMIO_STATUS, 0);
	status = VIRTIO_STATUS_ACKNOWLEDGE;
	vblk_out(d, VIRTIO_MMIO_STATUS, status);
	status |= VIRTIO_STATUS_DRIVER;
	vblk_out(d, VIRTIO_MMIO_STATUS, status);

	/* Features: the modern interface only, plus read-only if offered */
	vblk_out(d, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
	features = vblk_in(d, VIRTIO_MMIO_DEVICE_FEATURES);
	vblk_out(d, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
	features |= (UD)vblk_in(d, VIRTIO_MMIO_DEVICE_FEATURES) << 32;

	if ( (features & (1ULL << VIRTIO_F_VERSION_1)) == 0 ) {
		vblk_out(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return E_NOSPT;
	}
	d->readonly = ( features & (1ULL << VIRTIO_BLK_F_RO) ) ? TRUE : FALSE;
	d->flush = ( features & (1ULL << VIRTIO_BLK_F_FLUSH) ) ? TRUE : FALSE;

	vblk_out(d, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
	vblk_out(d, VIRTIO_MMIO_DRIVER_FEATURES, ( d->readonly ? (1U << VIRTIO_BLK_F_RO) : 0 )
					       | ( d->flush ? (1U << VIRTIO_BLK_F_FLUSH) : 0 ));
	vblk_out(d, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
	vblk_out(d, VIRTIO_MMIO_DRIVER_FEATURES, 1U << (VIRTIO_F_VERSION_1 - 32));

	status |= VIRTIO_STATUS_FEATURES_OK;
	vblk_out(d, VIRTIO_MMIO_STATUS, status);
	if ( (vblk_in(d, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0 ) {
		vblk_out(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return E_NOSPT;
	}

	/* Queue 0 */
	vblk_out(d, VIRTIO_MMIO_QUEUE_SEL, 0);
	qmax = vblk_in(d, VIRTIO_MMIO_QUEUE_NUM_MAX);
	if ( qmax < VBLK_QSIZE ) {
		vblk_out(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return E_NOSPT;
	}

	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	d->sh_pa = knl_pf_to_pa(pf);
	d->sh    = (VBLK_SHARED *)PA2VA(d->sh_pa);
	d->last_used = 0;

	vblk_out(d, VIRTIO_MMIO_QUEUE_NUM, VBLK_QSIZE);
	vblk_out(d, VIRTIO_MMIO_QUEUE_DESC_LOW,    (UW)(d->sh_pa + ((UBINT)d->sh->desc - (UBINT)d->sh)));
	vblk_out(d, VIRTIO_MMIO_QUEUE_DESC_HIGH,   (UW)((d->sh_pa + ((UBINT)d->sh->desc - (UBINT)d->sh)) >> 32));
	vblk_out(d, VIRTIO_MMIO_QUEUE_DRIVER_LOW,  (UW)(d->sh_pa + ((UBINT)&d->sh->avail - (UBINT)d->sh)));
	vblk_out(d, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (UW)((d->sh_pa + ((UBINT)&d->sh->avail - (UBINT)d->sh)) >> 32));
	vblk_out(d, VIRTIO_MMIO_QUEUE_DEVICE_LOW,  (UW)(d->sh_pa + ((UBINT)&d->sh->used - (UBINT)d->sh)));
	vblk_out(d, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (UW)((d->sh_pa + ((UBINT)&d->sh->used - (UBINT)d->sh)) >> 32));
	vblk_out(d, VIRTIO_MMIO_QUEUE_READY, 1);

	/* Capacity is the first field of the block configuration space */
	d->capacity = (UD)vblk_in(d, VIRTIO_MMIO_CONFIG)
		    | ((UD)vblk_in(d, VIRTIO_MMIO_CONFIG + 4) << 32);

	status |= VIRTIO_STATUS_DRIVER_OK;
	vblk_out(d, VIRTIO_MMIO_STATUS, status);

	return E_OK;
}

/*
 * Scan the virtio-mmio slots, set up every block device and register it.
 */
EXPORT INT knl_vblk_init( void )
{
	T_DDEV	ddev;
	T_CMTX	cmtx;
	T_CSEM	csem;
	T_CFLG	cflg;
	INT	slot, i;

	for ( slot = 0; slot < VIRTIO_MMIO_NUM && vblk_ndev < VBLK_MAX_DEV; slot++ ) {
		VBLK_DEV *d = &vblk_dev[vblk_ndev];

		d->base  = VIRTIO_MMIO_BASE + (UBINT)slot * VIRTIO_MMIO_STRIDE;
		d->intno = VIRTIO_MMIO_INTNO(slot);

		if ( vblk_in(d, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC_VALUE ) continue;
		if ( vblk_in(d, VIRTIO_MMIO_VERSION) != 2 ) continue;
		if ( vblk_in(d, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_BLOCK ) continue;

		cmtx.exinf = NULL;
		cmtx.mtxatr = TA_TFIFO;
		d->mtxid = tk_cre_mtx(&cmtx);
		cflg.exinf = NULL;
		cflg.flgatr = TA_TFIFO | TA_WMUL;
		cflg.iflgptn = 0;
		d->flgid = tk_cre_flg(&cflg);
		csem.exinf = NULL;
		csem.sematr = TA_TFIFO | TA_FIRST;
		csem.isemcnt = VBLK_SLOTS;
		csem.maxsem = VBLK_SLOTS;
		d->semid = tk_cre_sem(&csem);
		if ( d->mtxid <= 0 || d->flgid <= 0 || d->semid <= 0 ) {
			return E_LIMIT;
		}

		if ( vblk_setup(d) < E_OK ) {
			continue;
		}

		/* Counted before the interrupt is enabled: the handler looks the
		   device up in this array, and the partition scan below already
		   needs completions. */
		vblk_ndev++;

		knl_define_inthdr((INT)d->intno, TA_HLNG, (FP)vblk_inthdr);
		EnableInt(d->intno, INTPRI_DEVICE);

		knl_read_parttbl(vblk_readfn, d, d->capacity, &d->tbl);

		d->devnm[0] = 'v'; d->devnm[1] = 'b'; d->devnm[2] = 'l';
		d->devnm[3] = 'k'; d->devnm[4] = (UB)('a' + (d - vblk_dev)); d->devnm[5] = '\0';

		ddev.exinf   = d;
		ddev.drvatr  = 0;
		ddev.devatr  = TDK_DISK | TDK_UNDEF;
		ddev.nsub    = d->tbl.n;
		ddev.blksz   = BLK_SECTOR_SIZE;
		ddev.openfn  = (FP)vblk_open;
		ddev.closefn = (FP)vblk_close;
		ddev.execfn  = (FP)vblk_exec;
		ddev.waitfn  = (FP)vblk_wait;
		ddev.abortfn = (FP)vblk_abort;
		ddev.eventfn = (FP)vblk_event;

		d->devid = tk_def_dev(d->devnm, &ddev, NULL);
		if ( d->devid <= 0 ) {
			DisableInt(d->intno);
			vblk_ndev--;
			return d->devid;
		}

		tm_printf((UB*)"TessronOS: %s %d sectors (%d MB)%s, %d partitions (%s)\n",
			d->devnm, (UW)d->capacity, (UW)(d->capacity >> 11),
			d->readonly ? ", read only" : "",
			d->tbl.n, d->tbl.gpt ? "GPT" : "MBR");
		for ( i = 0; i < d->tbl.n; i++ ) {
			tm_printf((UB*)"TessronOS:   %s%d %d+%d\n", d->devnm, i,
				(UW)d->tbl.part[i].start, (UW)d->tbl.part[i].nsect);
		}
	}

	return vblk_ndev;
}

#endif /* QEMU_VIRT */
