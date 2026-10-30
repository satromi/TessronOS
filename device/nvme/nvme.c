/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	nvme.c
 *	NVM Express (design 10.4): the solid state disks of a PC, the
 *	Surface Go 2's among them, and QEMU's nvme for trying them.
 *
 *	Each controller found on the bus (class 01h, subclass 08h, interface
 *	02h) is registered with tk_def_dev() as "nvmea", "nvmeb", ... with
 *	its first namespace: subunit 0 the whole namespace, 1..n its
 *	partitions from the GPT or MBR, as the other disks are ("nvmea0" the
 *	first partition). The device's blocks are the namespace's logical
 *	blocks, of 512 or 4096 bytes (LBADS); the partition table is read
 *	only on blocks of 512 bytes, which is what the readers of GPT and MBR
 *	take.
 *
 *	A controller gets the admin queue pair and one I/O queue pair, both
 *	small, and one interrupt for both (MSI-X, else MSI; with neither the
 *	completions are looked for every millisecond). A command is in
 *	flight at a time on each queue: the I/O of a disk is handed out by
 *	the file systems one request at a time anyway. The data goes through
 *	a buffer of the driver's, physically contiguous and below 4GB, which
 *	one PRP list describes, or straight from and to the caller's memory
 *	when that is the kernel's.
 *
 *	A command not answered in NV_TMO_MS, or a controller that says it
 *	failed (CSTS.CFS), has the controller reset before the request fails
 *	with E_IO: until the controller is disabled it may still write into
 *	the memory the command named, which the caller would have back. The
 *	queues are made again and the requests after it go on. A controller
 *	whose registers read all ones is gone, and its requests fail at once.
 *
 *	A controller that asks for a host memory buffer (HMPRE) is given one
 *	and keeps it; it is taken back (EHM cleared) before a reset and
 *	before the shutdown. Before the machine goes off or starts again,
 *	every controller is shut down as NVMe 1.4 7.6.2 says
 *	(knl_nvme_shutdown, after the records are written).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/syslib.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "nvme.h"

#if defined(PCIE_ECAM_PA) || defined(PCIE_CFG_BOARD)

IMPORT void	knl_int_count_at( UINT intno, volatile UW *cnt );	/* kernel/tkernel/inttarget.c */

/* Controller registers */
#define NV_CAP			0x00
#define NV_VS			0x08
#define NV_INTMS		0x0c
#define NV_INTMC		0x10
#define NV_CC			0x14
#define NV_CSTS			0x1c
#define NV_AQA			0x24
#define NV_ASQ			0x28
#define NV_ACQ			0x30
#define NV_DBL			0x1000

#define CAP_MQES(c)		( (UW)( (c) & 0xffff ) )
#define CAP_TO(c)		( (UW)( ( (c) >> 24 ) & 0xff ) )	/* 500ms units */
#define CAP_DSTRD(c)		( (UW)( ( (c) >> 32 ) & 0xf ) )
#define CAP_CSS_NVM(c)		( (UW)( ( (c) >> 37 ) & 1 ) )
#define CAP_MPSMIN(c)		( (UW)( ( (c) >> 48 ) & 0xf ) )		/* 2^(12 + n) bytes */

#define CC_EN			0x00000001
#define CC_SHN_MASK		( 3U << 14 )
#define CC_SHN_NORMAL		( 1U << 14 )
#define CC_IOSQES		( 6U << 16 )	/* 64 byte submissions */
#define CC_IOCQES		( 4U << 20 )	/* 16 byte completions */
#define CSTS_RDY		0x00000001
#define CSTS_CFS		0x00000002
#define CSTS_SHST_MASK		( 3U << 2 )
#define CSTS_SHST_DONE		( 2U << 2 )
#define NV_GONE			0xffffffffU	/* what a register reads once the device is off the bus */

/* Commands */
#define ADM_DELETE_SQ		0x00
#define ADM_CREATE_SQ		0x01
#define ADM_DELETE_CQ		0x04
#define ADM_CREATE_CQ		0x05
#define ADM_IDENTIFY		0x06
#define ADM_SET_FEATURES	0x09
#define FEAT_HMB		0x0d		/* host memory buffer */
#define IO_FLUSH		0x00
#define IO_WRITE		0x01
#define IO_READ			0x02

#define NV_QSIZE		32		/* entries of each queue */
#define NV_BUF_PAGES		32		/* the driver's buffer: 128KB */
#define NV_TMO_MS		10000		/* a command's answer */
#define NV_LOOK_MS		10		/* the queue looked at when its interrupt has not come */
#define NV_HMB_OFF_MS		1000		/* the buffer taken back before a reset */
#define NV_SHN_MIN_MS		5000		/* the shutdown: RTD3E when longer, ... */
#define NV_SHN_MAX_MS		60000		/* ... but no longer than this */

#define NV_HMB_MAX		( 128ULL << 20 )	/* the most given, whatever the controller prefers */
#define NV_HMB_ORDER		9		/* pieces of 2MB, smaller when memory is short */
#define NV_HMB_DESC_N		( PAGE_SIZE / 16 )	/* descriptors of the one list page */

typedef struct {
	UW	cdw0;		/* opcode, CID */
	UW	nsid;
	UD	rsvd;
	UD	mptr;
	UD	prp1;
	UD	prp2;
	UW	cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} NV_SQE;

typedef struct {
	UW	dw0;
	UW	dw1;
	UH	sqhd;
	UH	sqid;
	UH	cid;
	UH	status;		/* bit 0: phase */
} NV_CQE;

typedef struct {
	NV_SQE		*sq;		/* linear map */
	volatile NV_CQE	*cq;
	UD		sq_pa, cq_pa;
	UH		sq_tail, cq_head;
	UH		sq_head;	/* as the last completion said */
	UH		phase;
	UH		cid;
	UINT		qid;
	ID		mtxid;		/* one command at a time */
} NV_QUEUE;

typedef struct {
	T_PCIDEV	pci;
	UBINT		base;
	UW		dstrd;
	UW		to_ms;		/* CAP.TO: how long CSTS.RDY may take to follow CC.EN */
	INT		irqkind;	/* PCI_IRQ_* */
	UINT		intno;		/* 0: polled */
	ID		flgid;
	NV_QUEUE	adm, io;
	UB		*buf;		/* the data buffer and its PRP list */
	UD		buf_pa;
	UD		*prpl;
	UD		prpl_pa;
	UW		nsid;
	UD		capacity;	/* logical blocks */
	UW		blksz;		/* bytes of a logical block */
	UW		max_sect;	/* blocks of one command through the buffer */
	UW		max_dsect;	/* blocks of one command straight from the caller's memory */
	UW		rtd3e;		/* microseconds the controller says its shutdown takes, 0 not said */
	ID		devid;
	T_PARTTBL	tbl;
	UB		devnm[8];
	ID		bufmtx;		/* the driver's buffer, one request at a time */
	volatile INT	state;		/* NV_* */
	BOOL		closing;	/* the machine goes off: no request taken */

	/* the host memory buffer: a list page of descriptors, the pieces kept for good */
	UD		*hmb_desc;
	UD		hmb_desc_pa;
	UW		hmb_n;		/* descriptors */
	UD		hmb_bytes;
	BOOL		hmb_on;

	/* counts, for the records */
	volatile UW	nint;
	UW		ncmd, nlate, ntmo, nreset, nstray;
	UW		int_mark;	/* the next count of interrupts said */
	BOOL		late_told, silent_told;
	UW		shst_ms;
#if USE_KTEST
	BOOL		drop_next;
#endif
} NV_DEV;

/*
 * The controllers, as many as are found: each made when it is found and
 * kept, the table of them twice as large when it fills. An old table is
 * not given back, so that the interrupt handler, which reads the table
 * without a lock, never reads memory given back; nv_ndev grows only
 * after the table holds the new one.
 */
LOCAL NV_DEV	**nv_dev = NULL;
LOCAL INT	nv_ndev = 0, nv_cap = 0;

/* The table's next entry, made; NULL when memory is short */
LOCAL NV_DEV *nv_new( void )
{
	NV_DEV	**t;
	INT	n;

	if ( nv_ndev >= nv_cap ) {
		n = ( nv_cap > 0 ) ? nv_cap * 2 : 4;
		t = (NV_DEV **)Kcalloc((SZ)n, sizeof(NV_DEV *));
		if ( t == NULL ) return NULL;
		if ( nv_dev != NULL ) knl_memcpy(t, nv_dev, sizeof(NV_DEV *) * (SZ)nv_cap);
		__atomic_store_n(&nv_dev, t, __ATOMIC_RELEASE);
		nv_cap = n;
	}
	if ( nv_dev[nv_ndev] == NULL ) {
		nv_dev[nv_ndev] = (NV_DEV *)Kcalloc(1, sizeof(NV_DEV));
	}
	return nv_dev[nv_ndev];
}

/* The name of the n-th disk: "nvmea" ... "nvmez", then "nvmeaa" ... */
LOCAL void nv_name( UB *nm, INT n )
{
	nm[0] = 'n'; nm[1] = 'v'; nm[2] = 'm'; nm[3] = 'e';
	if ( n < 26 ) {
		nm[4] = (UB)( 'a' + n );
		nm[5] = '\0';
	} else {
		nm[4] = (UB)( 'a' + ( n / 26 - 1 ) % 26 );
		nm[5] = (UB)( 'a' + n % 26 );
		nm[6] = '\0';
	}
}

#define NV_FLG_DONE	0x0001

Inline UW nv_rd( NV_DEV *d, UW off ) { return in_w(d->base + off); }
Inline void nv_wr( NV_DEV *d, UW off, UW v ) { out_w(d->base + off, v); }
Inline UD nv_rd64( NV_DEV *d, UW off ) { return (UD)in_w(d->base + off) | ( (UD)in_w(d->base + off + 4) << 32 ); }
Inline void nv_wr64( NV_DEV *d, UW off, UD v ) { out_w(d->base + off, (UW)v); out_w(d->base + off + 4, (UW)( v >> 32 )); }

Inline UW nv_le32( CONST UB *p ) { return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 ); }
Inline UD nv_le64( CONST UB *p ) { return (UD)nv_le32(p) | ( (UD)nv_le32(p + 4) << 32 ); }

/* The milliseconds of the system's clock: the waits are counted by it, not by how often they slept */
LOCAL UW nv_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return t.lo;
}

/* The doorbell of a queue's submission tail (cq = FALSE) or completion head */
Inline UW nv_db( NV_DEV *d, UINT qid, BOOL cq )
{
	return NV_DBL + ( 2 * qid + ( cq ? 1 : 0 ) ) * ( 4U << d->dstrd );
}

LOCAL void nv_inthdr( UINT intno, UW iar )
{
	INT	i;

	NV_DEV	**t = __atomic_load_n(&nv_dev, __ATOMIC_ACQUIRE);
	INT	n = __atomic_load_n(&nv_ndev, __ATOMIC_ACQUIRE);

	for ( i = 0; i < n; i++ ) {
		if ( t[i]->intno == intno ) {
			__atomic_add_fetch(&t[i]->nint, 1, __ATOMIC_RELAXED);
			tk_set_flg(t[i]->flgid, NV_FLG_DONE);
		}
	}
	EndOfInt(intno);
}

/* ---------------------------------------------------------------- the counts said */

/*
 * The interrupts that arrived, for the console (and so KERNEL.TXT): the
 * first, then each tenfold; and once, when a hundred commands were
 * answered with none, that the interrupts do not arrive
 */
LOCAL void nv_tally( NV_DEV *d )
{
	UW	n = d->nint;

	if ( d->intno == 0 ) return;
	if ( n >= d->int_mark ) {
		tm_printf((UB *)"nvme: %s: %u interrupts arrived (%s, interrupt %d, PRC %d); %u commands,"
			  " %u of them found without their interrupt\n", d->devnm, n, ts_pcie_irq_name(d->irqkind),
			  d->intno, (INT)GetIntTarget(d->intno), d->ncmd, d->nlate);
		while ( d->int_mark <= n && d->int_mark < 1000000000U ) d->int_mark *= 10;
	} else if ( n == 0 && d->ncmd >= 100 && !d->silent_told ) {
		d->silent_told = TRUE;
		tm_printf((UB *)"nvme: %s: no interrupt arrived in %u commands (%s, interrupt %d):"
			  " each was found by the look every %d ms\n", d->devnm, d->ncmd,
			  ts_pcie_irq_name(d->irqkind), d->intno, NV_LOOK_MS);
	}
}

/* A completion found after a wait for its interrupt ran out: said the first time */
LOCAL void nv_late( NV_DEV *d )
{
	d->nlate++;
	if ( !d->late_told ) {
		d->late_told = TRUE;
		tm_printf((UB *)"nvme: %s: a completion was there with no interrupt for it after %d ms"
			  " (%s, interrupt %d, %u interrupts so far)\n", d->devnm, NV_LOOK_MS,
			  ts_pcie_irq_name(d->irqkind), d->intno, d->nint);
	}
}

/* ---------------------------------------------------------------- commands */

/*
 * A command on a queue whose lock the caller holds, and its completion
 * waited for, tmo milliseconds at most: E_OK, or E_IO when the controller
 * answered with an error. When it did not answer -- in time, or because
 * it failed (CSTS.CFS) or is gone -- or the queue is full of commands it
 * did not take back, *p_broken is set as well: the controller must be
 * reset before the memory the command named is used again.
 */
LOCAL ER nv_issue( NV_DEV *d, NV_QUEUE *q, NV_SQE *c, UW *p_dw0, UW tmo, BOOL *p_broken )
{
	volatile NV_CQE	*e;
	UH		cid;
	UINT		ptn;
	UW		t0, csts;
	BOOL		looked = FALSE;		/* the last wait ran out without an interrupt */

	*p_broken = FALSE;
	if ( (UH)( ( q->sq_tail + 1 ) % NV_QSIZE ) == q->sq_head ) {
		tm_printf((UB *)"nvme: %s: queue %d is full of commands not taken back\n", d->devnm, q->qid);
		*p_broken = TRUE;
		return E_IO;
	}
	cid = ++q->cid;
	c->cdw0 = ( c->cdw0 & 0xffff ) | ( (UW)cid << 16 );
	q->sq[q->sq_tail] = *c;
	q->sq_tail = (UH)( ( q->sq_tail + 1 ) % NV_QSIZE );
	DeviceBarrier();
#if USE_KTEST
	if ( d->drop_next && q == &d->io ) {
		d->drop_next = FALSE;		/* never seen by the controller: a command lost */
	} else
#endif
	nv_wr(d, nv_db(d, q->qid, FALSE), q->sq_tail);

	t0 = nv_ms();
	for ( ;; ) {
		e = &q->cq[q->cq_head];
		if ( ( e->status & 1 ) == q->phase ) {
			UH	st = e->status >> 1, ecid = e->cid;

			if ( p_dw0 != NULL && ecid == cid ) *p_dw0 = e->dw0;
			q->sq_head = (UH)( e->sqhd % NV_QSIZE );
			q->cq_head = (UH)( ( q->cq_head + 1 ) % NV_QSIZE );
			if ( q->cq_head == 0 ) {
				q->phase ^= 1;
			}
			nv_wr(d, nv_db(d, q->qid, TRUE), q->cq_head);
			if ( ecid != cid ) {
				d->nstray++;		/* not this command's */
				continue;
			}
			d->ncmd++;
			if ( looked && d->intno != 0 ) nv_late(d);
			nv_tally(d);
			if ( st != 0 ) {
				tm_printf((UB *)"nvme: %s command %02x failed: status %03x\n",
					  d->devnm, c->cdw0 & 0xff, st & 0x7ff);
				return E_IO;
			}
			return E_OK;
		}
		if ( looked ) {
			csts = nv_rd(d, NV_CSTS);
			if ( csts == NV_GONE || ( csts & CSTS_CFS ) != 0 ) {
				tm_printf((UB *)"nvme: %s: %s while command %02x was out (CSTS %08x)\n", d->devnm,
					  ( csts == NV_GONE ) ? "the controller is gone (its registers read all ones)"
							      : "the controller says it failed (CSTS.CFS)",
					  c->cdw0 & 0xff, csts);
				*p_broken = TRUE;
				return E_IO;
			}
			if ( (UW)( nv_ms() - t0 ) >= tmo ) {
				d->ntmo++;
				tm_printf((UB *)"nvme: %s: command %02x (id %d) on queue %d not answered in %d ms"
					  " (CSTS %08x, %u interrupts so far)\n", d->devnm, c->cdw0 & 0xff, cid,
					  q->qid, tmo, csts, d->nint);
				*p_broken = TRUE;
				return E_IO;
			}
		}
		if ( d->intno != 0 ) {
			looked = ( tk_wai_flg(d->flgid, NV_FLG_DONE, TWF_ORW | TWF_BITCLR, &ptn, NV_LOOK_MS) == E_TMOUT );
		} else {
			tk_dly_tsk(1);
			looked = TRUE;
		}
	}
}

/* An admin command, the admin queue's lock held by the caller */
LOCAL ER nv_admin( NV_DEV *d, NV_SQE *c, UW *p_dw0, UW tmo )
{
	BOOL	broken;
	ER	er = nv_issue(d, &d->adm, c, p_dw0, tmo, &broken);

	return ( broken && er >= E_OK ) ? E_IO : er;
}

LOCAL void nv_recover( NV_DEV *d );

/*
 * A command on the I/O queue: its error, E_IO when it was not answered,
 * and then only after the controller was reset, so that nothing it was
 * told is written after the caller has its memory back
 */
LOCAL ER nv_cmd( NV_DEV *d, NV_SQE *c )
{
	BOOL	broken = FALSE;
	ER	er = E_IO;

	tk_loc_mtx(d->io.mtxid, TMO_FEVR);
	if ( d->state == NV_LIVE ) {
		er = nv_issue(d, &d->io, c, NULL, NV_TMO_MS, &broken);
		if ( broken ) d->state = NV_FAILED;
	}
	tk_unl_mtx(d->io.mtxid);
	if ( broken ) {
		nv_recover(d);
	}
	return er;
}

/* The PRP list of n pages of the buffer */
LOCAL void nv_prp( NV_DEV *d, NV_SQE *c, UD bytes )
{
	UD	n = ( bytes + PAGE_SIZE - 1 ) / PAGE_SIZE, i;

	c->prp1 = d->buf_pa;
	if ( n <= 1 ) {
		c->prp2 = 0;
	} else if ( n == 2 ) {
		c->prp2 = d->buf_pa + PAGE_SIZE;
	} else {
		for ( i = 1; i < n; i++ ) {
			d->prpl[i - 1] = d->buf_pa + i * PAGE_SIZE;
		}
		c->prp2 = d->prpl_pa;
	}
}

/*
 * The caller's memory handed to the controller as it is: PRP1 the first
 * page from where the bytes start (on a double word), the list the whole
 * pages after it, each looked up, so that memory made of pages mapped
 * together (a buffer of a megabyte or more) needs no copy. Answers the
 * bytes the PRPs cover, whole blocks, up to `bytes`; 0 when the memory
 * is not the kernel's (a process's, which goes through the buffer) or
 * not on a double word.
 */
#define NV_PRPL_N	( PAGE_SIZE / sizeof(UD) )	/* entries of the list page */

LOCAL UD nv_map( NV_DEV *d, UB *buf, UD bytes, NV_SQE *c )
{
	void	*pa = NULL;
	UD	first, at, n = 0;

	if ( ( (UBINT)buf & 3 ) != 0 || ConvPhysicalAddress(buf, 1, &pa) <= 0 || pa == NULL ) {
		return 0;
	}
	c->prp1 = (UD)(UBINT)pa;
	c->prp2 = 0;
	first = PAGE_SIZE - (UD)( (UBINT)buf % PAGE_SIZE );
	if ( bytes <= first ) {
		return bytes;
	}
	for ( at = first; at < bytes && n < NV_PRPL_N; at += PAGE_SIZE ) {
		pa = NULL;
		if ( ConvPhysicalAddress(buf + at, 1, &pa) <= 0 || pa == NULL ) break;
		d->prpl[n++] = (UD)(UBINT)pa;
	}
	if ( at > bytes ) at = bytes;
	at -= at % d->blksz;
	if ( at <= first ) {
		/* only the first page: the blocks that end in it */
		at = first - first % d->blksz;
		return at;
	}
	n = ( at - first + PAGE_SIZE - 1 ) / PAGE_SIZE;	/* pages after the first */
	c->prp2 = ( n == 1 ) ? d->prpl[0] : d->prpl_pa;
	return at;
}

/*
 * Blocks from 'lba' on: straight from and to the caller's memory in
 * commands of max_dsect when it is the kernel's, else through the
 * buffer in commands of max_sect.
 */
#define NV_FUA		0x40000000U	/* CDW12: written through to the medium (Force Unit Access) */

LOCAL ER nv_xfer( NV_DEV *d, BOOL wr, UD lba, UB *buf, UD nblk, UW fua )
{
	ER	er = E_OK;

	while ( nblk > 0 && er == E_OK ) {
		UD	n = ( nblk < d->max_dsect ) ? nblk : d->max_dsect;
		UD	bytes;
		BOOL	direct;
		NV_SQE	c;

		knl_memset(&c, 0, sizeof(c));
		bytes = nv_map(d, buf, n * d->blksz, &c);
		direct = (BOOL)( bytes >= d->blksz );
		if ( direct ) {
			n = bytes / d->blksz;
		} else {
			n = ( nblk < d->max_sect ) ? nblk : d->max_sect;
			bytes = n * d->blksz;
		}
		c.cdw0 = wr ? IO_WRITE : IO_READ;
		c.nsid = d->nsid;
		c.cdw10 = (UW)lba;
		c.cdw11 = (UW)( lba >> 32 );
		c.cdw12 = (UW)( n - 1 ) | ( wr ? fua : 0 );
		if ( !direct ) {
			if ( wr ) {
				knl_memcpy(d->buf, buf, (SZ)bytes);
			}
			nv_prp(d, &c, bytes);
		}
		er = nv_cmd(d, &c);
		if ( er == E_OK && !wr && !direct ) {
			knl_memcpy(buf, d->buf, (SZ)bytes);
		}
		lba += n;
		buf += bytes;
		nblk -= n;
	}
	return er;
}

/*
 * The driver's buffer is one; it is held for the whole of a request,
 * the copying as well
 */
LOCAL ER nv_rw( NV_DEV *d, BOOL wr, UD lba, void *buf, UD nblk, UW fua )
{
	ER	er;

	tk_loc_mtx(d->bufmtx, TMO_FEVR);
	er = d->closing ? E_IO : nv_xfer(d, wr, lba, (UB *)buf, nblk, fua);
	tk_unl_mtx(d->bufmtx);
	return er;
}

/* The partition table's reader: sectors of 512 bytes, which are the blocks when it is called */
LOCAL ER nv_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	return nv_rw((NV_DEV *)exinf, FALSE, start, buf, nsect, 0);
}

LOCAL ER nv_flush( NV_DEV *d )
{
	NV_SQE	c;

	if ( d->closing ) return E_IO;
	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = IO_FLUSH;
	c.nsid = d->nsid;
	return nv_cmd(d, &c);
}

/* ---------------------------------------------------------------- driver interface */

LOCAL ER nv_unit_range( NV_DEV *d, INT unitno, UD *p_start, UD *p_nsect )
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

LOCAL ER nv_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

LOCAL ER nv_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER nv_attr( NV_DEV *d, INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nsect;
	ER		er;

	if ( req->start == TDN_IOMAX && req->cmd == TDC_READ && req->size >= (SZ)sizeof(UW) ) {
		/* the most one command moves straight from memory (design 7.3) */
		*(UW *)req->buf = (UW)d->max_dsect * d->blksz;
		req->asize = sizeof(UW);
		return E_OK;
	}
	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ || req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = nv_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		return er;
	}
	info = (DiskInfo *)req->buf;
	info->protect   = FALSE;
	info->removable = FALSE;
	info->blocksize = (W)d->blksz;
	/* the count in a W stops at 2^31 - 1 blocks; the whole count is in blockcont_d */
	info->blockcont = ( nsect > 0x7FFFFFFFULL ) ? 0x7FFFFFFF : (W)nsect;
	info->blockcont_d = (D)nsect;
	req->asize = sizeof(DiskInfo);
	return E_OK;
}

LOCAL ER nv_define( NV_DEV *d );

LOCAL ER nv_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	NV_DEV	*d = (NV_DEV *)exinf;
	INT	unitno = (INT)( req->devid & 0xff );
	UD	start, nsect, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;

	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		req->error = nv_flush(d);
		return E_OK;
	}
	if ( req->start == TDN_PARTSCAN && req->cmd == TDC_WRITE ) {
		INT	n = ( unitno != 0 ) ? E_PAR : ( d->blksz != BLK_SECTOR_SIZE ) ? E_NOSPT
			  : knl_part_rescan(nv_readfn, d, d->capacity, &d->tbl);

		req->error = ( n < E_OK ) ? n : nv_define(d);
		return E_OK;
	}
	if ( req->start == TDN_FUAWRITE && req->cmd == TDC_WRITE ) {
		/* blocks written through to the medium: every NVMe controller takes FUA */
		CONST T_DEVFUA	*fw = (CONST T_DEVFUA *)req->buf;

		if ( req->size < (SZ)sizeof(T_DEVFUA) || fw->size <= 0 || ( fw->size % d->blksz ) != 0
		  || fw->start < 0 || nv_unit_range(d, unitno, &start, &nsect) < E_OK
		  || (UD)fw->start + (UD)fw->size / d->blksz > nsect ) {
			req->error = E_PAR;
			return E_OK;
		}
		req->error = nv_rw(d, TRUE, start + (UD)fw->start, (void *)fw->buf,
				   (UD)fw->size / d->blksz, NV_FUA);
		if ( req->error == E_OK ) req->asize = req->size;
		return E_OK;
	}
	if ( req->start < 0 ) {
		req->error = nv_attr(d, unitno, req);
		return E_OK;
	}
	er = nv_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		req->error = er;
		return E_OK;
	}
	if ( ( req->size % d->blksz ) != 0 ) {
		req->error = E_PAR;
		return E_OK;
	}
	nreq = (UD)req->size / d->blksz;
	if ( (UD)req->start >= nsect || (UD)req->start + nreq > nsect ) {
		req->error = E_PAR;
		return E_OK;
	}
	er = nv_rw(d, ( req->cmd == TDC_WRITE ), start + (UD)req->start, req->buf, nreq, 0);
	if ( er == E_OK ) {
		req->asize = req->size;
	}
	req->error = er;
	return E_OK;
}

/* Requests complete inside execfn, so the wait function only reports them */
LOCAL INT nv_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER nv_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT nv_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* The device under its name, the partitions as its subunits; again when they change */
LOCAL ER nv_define( NV_DEV *d )
{
	T_DDEV	ddev;
	ID	id;

	ddev.exinf   = d;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_DISK | TDK_UNDEF;
	ddev.nsub    = d->tbl.n;
	ddev.blksz   = (W)d->blksz;
	ddev.openfn  = (FP)nv_open;
	ddev.closefn = (FP)nv_close;
	ddev.execfn  = (FP)nv_exec;
	ddev.waitfn  = (FP)nv_wait;
	ddev.abortfn = (FP)nv_abort;
	ddev.eventfn = (FP)nv_event;

	id = tk_def_dev(d->devnm, &ddev, NULL);
	if ( id > 0 ) {
		d->devid = id;
	}
	return ( id > 0 ) ? E_OK : (ER)id;
}

/* ---------------------------------------------------------------- the controller started */

/* Pages below 4GB, zeroed, and their physical address */
LOCAL void *nv_pages( UINT order, UD *p_pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) {
		return NULL;
	}
	*p_pa = knl_pf_to_pa(pf);
	return PA2VA(*p_pa);
}

LOCAL ER nv_queue_mem( NV_QUEUE *q, UINT qid )
{
	T_CMTX	cmtx;

	q->sq = (NV_SQE *)nv_pages(0, &q->sq_pa);
	q->cq = (volatile NV_CQE *)nv_pages(0, &q->cq_pa);
	if ( q->sq == NULL || q->cq == NULL ) {
		return E_NOMEM;
	}
	q->cid = 0;
	q->qid = qid;
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	q->mtxid = tk_cre_mtx(&cmtx);
	return ( q->mtxid > 0 ) ? E_OK : (ER)q->mtxid;
}

/* A queue as the controller has it after a reset: empty, both ends at 0 */
LOCAL void nv_queue_clear( NV_QUEUE *q )
{
	knl_memset(q->sq, 0, PAGE_SIZE);
	knl_memset((void *)q->cq, 0, PAGE_SIZE);
	q->sq_tail = q->sq_head = q->cq_head = 0;
	q->phase = 1;
}

/* CSTS.RDY waited for to be `on`, CAP.TO at most by the clock */
LOCAL ER nv_wait_rdy( NV_DEV *d, BOOL on )
{
	UW	t0 = nv_ms(), s;

	for ( ;; ) {
		s = nv_rd(d, NV_CSTS);
		if ( s == NV_GONE ) {
			return E_NOEXS;
		}
		if ( on && ( s & CSTS_CFS ) != 0 ) {
			return E_IO;
		}
		if ( ( ( s & CSTS_RDY ) != 0 ) == on ) {
			return E_OK;
		}
		if ( (UW)( nv_ms() - t0 ) >= d->to_ms ) {
			return E_TMOUT;
		}
		tk_dly_tsk(1);
	}
}

/*
 * The controller disabled (a controller reset: nothing it was told is
 * carried out after CSTS.RDY falls). One still coming up from being
 * enabled (CC.EN 1, CSTS.RDY 0) is let become ready first, as clearing
 * CC.EN before that is not defined. Registers that read all ones: gone.
 */
LOCAL ER nv_disable( NV_DEV *d )
{
	UW	cc = nv_rd(d, NV_CC), s = nv_rd(d, NV_CSTS);

	if ( cc == NV_GONE || s == NV_GONE ) {
		return E_NOEXS;
	}
	if ( ( cc & CC_EN ) != 0 && ( s & CSTS_RDY ) == 0 ) {
		(void)nv_wait_rdy(d, TRUE);
	}
	nv_wr(d, NV_CC, cc & ~( CC_EN | CC_SHN_MASK ));
	return nv_wait_rdy(d, FALSE);
}

/*
 * The controller reset and enabled again on empty admin queues; both
 * queues as it then has them. Polled, its interrupts are masked as well.
 */
LOCAL ER nv_start_admin( NV_DEV *d )
{
	ER	er = nv_disable(d);

	if ( er < E_OK ) {
		return er;
	}
	nv_queue_clear(&d->adm);
	nv_queue_clear(&d->io);
	nv_wr(d, NV_AQA, ( ( NV_QSIZE - 1 ) << 16 ) | ( NV_QSIZE - 1 ));
	nv_wr64(d, NV_ASQ, d->adm.sq_pa);
	nv_wr64(d, NV_ACQ, d->adm.cq_pa);
	nv_wr(d, NV_CC, CC_IOSQES | CC_IOCQES | CC_EN);		/* NVM command set, pages of 4KB */
	er = nv_wait_rdy(d, TRUE);
	if ( er >= E_OK && d->intno == 0 ) {
		nv_wr(d, NV_INTMS, 0xffffffffU);
	}
	return er;
}

/* ---------------------------------------------------------------- the host memory buffer */

/*
 * The buffer the controller prefers (HMPRE, at most NV_HMB_MAX), in pieces
 * of 2MB or smaller ones when memory is short, none smaller than HMMINDS
 * and no more of them than HMMAXD; nothing when less than HMMIN could be
 * had. The pieces are kept for good: the controller is given them again
 * after a reset, and a controller that does not answer cannot write into
 * memory handed to something else.
 */
LOCAL void nv_hmb_alloc( NV_DEV *d, UW hmpre, UW hmmin, UW hmminds, UW hmmaxd )
{
	UD	want = (UD)hmpre * PAGE_SIZE, least = (UD)hmmin * PAGE_SIZE, got = 0;
	UD	piece_min = ( hmminds != 0 ) ? (UD)hmminds * PAGE_SIZE : PAGE_SIZE;
	UINT	order = NV_HMB_ORDER, maxn = NV_HMB_DESC_N, i;
	PFRAME	*pf;

	if ( hmpre == 0 ) {
		return;
	}
	if ( want > NV_HMB_MAX ) want = NV_HMB_MAX;
	if ( want < least || piece_min > ( (UD)PAGE_SIZE << PF_MAX_ORDER ) ) {
		tm_printf((UB *)"nvme: %s: the host memory buffer it asks for is not given"
			  " (at least %u KB, in pieces of %u KB)\n", d->devnm, hmmin * 4, hmminds * 4);
		return;
	}
	if ( hmmaxd != 0 && hmmaxd < maxn ) maxn = hmmaxd;
	while ( order < PF_MAX_ORDER && ( (UD)PAGE_SIZE << order ) < piece_min ) order++;
	d->hmb_desc = (UD *)nv_pages(0, &d->hmb_desc_pa);
	if ( d->hmb_desc == NULL ) {
		return;
	}
	while ( got < want && d->hmb_n < maxn ) {
		while ( order > 0 && ( (UD)PAGE_SIZE << order ) > want - got
		     && ( (UD)PAGE_SIZE << ( order - 1 ) ) >= piece_min ) {
			order--;
		}
		pf = knl_alloc_pages(order, ZONE_NORMAL, 0);
		if ( pf == NULL ) {
			if ( order == 0 || ( (UD)PAGE_SIZE << ( order - 1 ) ) < piece_min ) break;
			order--;
			continue;
		}
		d->hmb_desc[2 * d->hmb_n] = knl_pf_to_pa(pf);			/* BADD */
		d->hmb_desc[2 * d->hmb_n + 1] = (UD)1 << order;		/* BSIZE, in pages */
		d->hmb_n++;
		got += (UD)PAGE_SIZE << order;
	}
	if ( got < least || got == 0 ) {
		for ( i = 0; i < d->hmb_n; i++ ) {
			UD	pages = d->hmb_desc[2 * i + 1];
			UINT	o = 0;

			while ( ( (UD)1 << o ) < pages ) o++;
			knl_free_pages(knl_pa_to_pf(d->hmb_desc[2 * i]), o);
		}
		tm_printf((UB *)"nvme: %s: no host memory buffer: %ld KB had of the %u KB at least it takes\n",
			  d->devnm, (D)( got >> 10 ), hmmin * 4);
		d->hmb_n = 0;
		return;
	}
	d->hmb_bytes = got;
}

/*
 * The buffer given to the controller (EHM set; MR as well when it is the
 * memory it had before a reset, untouched since) or taken back. The
 * admin queue's lock held.
 */
LOCAL ER nv_hmb_set( NV_DEV *d, BOOL on, BOOL again, UW tmo )
{
	NV_SQE	c;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_SET_FEATURES;
	c.cdw10 = FEAT_HMB;
	if ( on ) {
		c.cdw11 = 0x1 | ( again ? 0x2 : 0 );			/* EHM, MR */
		c.cdw12 = (UW)( d->hmb_bytes / PAGE_SIZE );		/* HSIZE */
		c.cdw13 = (UW)d->hmb_desc_pa;				/* HMDLLA */
		c.cdw14 = (UW)( d->hmb_desc_pa >> 32 );			/* HMDLUA */
		c.cdw15 = d->hmb_n;					/* HMDLEC */
	}
	er = nv_admin(d, &c, NULL, tmo);
	d->hmb_on = ( er >= E_OK ) ? on : FALSE;
	return er;
}

/*
 * After the admin queue is up: the host memory buffer given, then the
 * I/O queue pair made, completions first. The admin queue's lock held.
 */
LOCAL ER nv_start_io( NV_DEV *d, BOOL again )
{
	NV_SQE	c;
	ER	er;

	if ( d->hmb_n > 0 ) {
		er = nv_hmb_set(d, TRUE, again, NV_TMO_MS);
		if ( er < E_OK ) {
			tm_printf((UB *)"nvme: %s: the host memory buffer not taken (%d)\n", d->devnm, er);
		}
	}
	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_CREATE_CQ;
	c.prp1 = d->io.cq_pa;
	c.cdw10 = ( ( NV_QSIZE - 1 ) << 16 ) | 1;
	c.cdw11 = ( d->intno != 0 ) ? 0x3 : 0x1;	/* physically contiguous, interrupts (vector 0) */
	er = nv_admin(d, &c, NULL, NV_TMO_MS);
	if ( er < E_OK ) {
		return er;
	}
	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_CREATE_SQ;
	c.prp1 = d->io.sq_pa;
	c.cdw10 = ( ( NV_QSIZE - 1 ) << 16 ) | 1;
	c.cdw11 = ( 1U << 16 ) | 0x1;			/* completions on queue 1, physically contiguous */
	return nv_admin(d, &c, NULL, NV_TMO_MS);
}

/*
 * A controller that did not answer a command, or failed: reset and
 * started again, with both queues held so that no command is out
 * meanwhile. Its buffer taken back first while it still works. Not back
 * after it, or gone: its requests fail from then on, and it is left
 * disabled, so that it writes nothing more.
 */
LOCAL void nv_recover( NV_DEV *d )
{
	UW	s;
	ER	er;

	tk_loc_mtx(d->io.mtxid, TMO_FEVR);
	tk_loc_mtx(d->adm.mtxid, TMO_FEVR);
	if ( d->state == NV_FAILED ) {
		s = nv_rd(d, NV_CSTS);
		if ( s == NV_GONE ) {
			d->state = NV_DEAD;
			tm_printf((UB *)"nvme: %s: the controller is gone; its requests fail from now on\n", d->devnm);
		} else {
			d->nreset++;
			tm_printf((UB *)"nvme: %s: the controller reset (CSTS %08x, reset %u), its queues made again\n",
				  d->devnm, s, d->nreset);
			if ( d->hmb_on && ( s & CSTS_CFS ) == 0 ) {
				(void)nv_hmb_set(d, FALSE, FALSE, NV_HMB_OFF_MS);
			}
			d->hmb_on = FALSE;
			er = nv_start_admin(d);
			if ( er >= E_OK ) {
				er = nv_start_io(d, TRUE);
			}
			if ( er >= E_OK ) {
				d->state = NV_LIVE;
				tm_printf((UB *)"nvme: %s: back after the reset%s\n", d->devnm,
					  d->hmb_on ? ", with its host memory buffer" : "");
			} else {
				d->state = NV_DEAD;
				(void)nv_disable(d);
				tm_printf((UB *)"nvme: %s: not back after the reset (%d); its requests fail from now on\n",
					  d->devnm, er);
			}
		}
	}
	tk_unl_mtx(d->adm.mtxid);
	tk_unl_mtx(d->io.mtxid);
}

/* ---------------------------------------------------------------- start-up */

/* The printable part of an identify string, its trailing blanks cut */
LOCAL void nv_ident_str( UB *to, CONST UB *from, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		to[i] = ( from[i] >= 0x20 && from[i] < 0x7f ) ? from[i] : '?';
	}
	while ( i > 0 && to[i - 1] == ' ' ) i--;
	to[i] = '\0';
}

/*
 * What the controller says of itself: its name, the largest transfer
 * (MDTS), its namespaces (NN), its shutdown's time (RTD3E) and the host
 * memory buffer it would have; then namespace 1, its size and its
 * blocks. The admin queue's lock held.
 */
LOCAL ER nv_identify( NV_DEV *d, UW *p_hm )
{
	UB	*id = d->buf, *ns = d->buf, *lbaf, model[41], fw[9];
	NV_SQE	c;
	UW	vs, mdts, nn, vwc, flbas, lbads;
	UD	lim;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_IDENTIFY;
	c.prp1 = d->buf_pa;
	c.cdw10 = 1;					/* the controller */
	er = nv_admin(d, &c, NULL, NV_TMO_MS);
	if ( er < E_OK ) {
		return er;
	}
	nv_ident_str(model, id + 24, 40);
	nv_ident_str(fw, id + 64, 8);
	mdts = id[77];
	d->rtd3e = nv_le32(id + 88);
	nn = nv_le32(id + 516);
	vwc = id[525] & 1;
	p_hm[0] = nv_le32(id + 272);			/* HMPRE, pages */
	p_hm[1] = nv_le32(id + 276);			/* HMMIN */
	p_hm[2] = nv_le32(id + 316);			/* HMMINDS */
	p_hm[3] = (UW)id[320] | ( (UW)id[321] << 8 );	/* HMMAXD */
	vs = nv_rd(d, NV_VS);
	tm_printf((UB *)"nvme: %s: \"%s\" firmware %s, NVMe %d.%d, MDTS %d, %u namespaces,"
		  " %s, RTD3E %u us, host memory buffer %u KB preferred (%u KB at least)\n",
		  d->devnm, model, fw, vs >> 16, ( vs >> 8 ) & 0xff, mdts, nn,
		  vwc ? "a volatile write cache" : "no volatile write cache",
		  d->rtd3e, p_hm[0] * 4, p_hm[1] * 4);
	if ( nn == 0 ) {
		tm_printf((UB *)"nvme: %s: the controller has no namespace\n", d->devnm);
		return E_NOEXS;
	}
	/* MDTS: 2^n of the smallest page (4KB, CAP.MPSMIN 0), 0 no limit */
	lim = ( mdts == 0 || mdts >= 20 ) ? (UD)NV_PRPL_N * PAGE_SIZE : (UD)PAGE_SIZE << mdts;

	/* namespace 1 */
	d->nsid = 1;
	knl_memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_IDENTIFY;
	c.nsid = d->nsid;
	c.prp1 = d->buf_pa;
	c.cdw10 = 0;					/* a namespace */
	er = nv_admin(d, &c, NULL, NV_TMO_MS);
	if ( er < E_OK ) {
		return er;
	}
	d->capacity = nv_le64(ns + 0);			/* NSZE */
	flbas = ns[26] & 0xf;
	if ( d->capacity == 0 ) {
		tm_printf((UB *)"nvme: %s: namespace 1 is not there (no blocks)\n", d->devnm);
		return E_NOEXS;
	}
	if ( flbas > ns[25] ) {
		tm_printf((UB *)"nvme: %s: namespace 1 names LBA format %d of %d\n", d->devnm, flbas, ns[25] + 1);
		return E_NOSPT;
	}
	lbaf = ns + 128 + 4 * flbas;
	lbads = lbaf[2];
	if ( lbads < 9 || lbads > 12 || ( lbaf[0] | lbaf[1] ) != 0 ) {
		tm_printf((UB *)"nvme: %s: namespace 1 has blocks of 2^%d bytes with %d bytes of metadata;"
			  " only 512 to 4096 bytes without metadata are taken\n", d->devnm, lbads,
			  (INT)( lbaf[0] | ( lbaf[1] << 8 ) ));
		return E_NOSPT;
	}
	d->blksz = 1U << lbads;
	d->max_sect = (UW)( ( ( lim < (UD)NV_BUF_PAGES * PAGE_SIZE ) ? lim : (UD)NV_BUF_PAGES * PAGE_SIZE ) / d->blksz );
	d->max_dsect = (UW)( ( ( lim < (UD)NV_PRPL_N * PAGE_SIZE ) ? lim : (UD)NV_PRPL_N * PAGE_SIZE ) / d->blksz );
	return E_OK;
}

LOCAL ER nv_setup( NV_DEV *d )
{
	UD	cap;
	UW	mqes, hm[4];
	ER	er;
	T_CFLG	cflg;
	T_CMTX	cmtx;

	ts_pcie_enable(&d->pci);
	d->base = ts_pcie_bar_base(&d->pci, 0);
	if ( d->base == 0 ) {
		return E_NOEXS;
	}
	cap = nv_rd64(d, NV_CAP);
	if ( cap == ~(UD)0 || nv_rd(d, NV_CSTS) == NV_GONE ) {
		tm_printf((UB *)"nvme: %s: its registers read all ones: not there\n", d->devnm);
		return E_NOEXS;
	}
	d->dstrd = CAP_DSTRD(cap);
	d->to_ms = CAP_TO(cap) * 500 + 500;
	mqes = CAP_MQES(cap) + 1;
	if ( mqes < NV_QSIZE || CAP_CSS_NVM(cap) == 0 || CAP_MPSMIN(cap) != 0 ) {
		tm_printf((UB *)"nvme: %s: CAP %08x%08x: queues of %d, NVM command set %d, pages from %d KB\n",
			  d->devnm, (UW)( cap >> 32 ), (UW)cap, mqes, CAP_CSS_NVM(cap), 4 << CAP_MPSMIN(cap));
		return E_NOSPT;
	}

	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	d->flgid = tk_cre_flg(&cflg);
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	d->bufmtx = tk_cre_mtx(&cmtx);
	if ( d->flgid <= 0 || d->bufmtx <= 0 ) {
		return E_LIMIT;
	}
	if ( nv_queue_mem(&d->adm, 0) < E_OK || nv_queue_mem(&d->io, 1) < E_OK ) {
		return E_NOMEM;
	}
	d->buf = (UB *)nv_pages(5, &d->buf_pa);		/* 32 pages */
	d->prpl = (UD *)nv_pages(0, &d->prpl_pa);
	if ( d->buf == NULL || d->prpl == NULL ) {
		return E_NOMEM;
	}

	/* disabled before its messages are turned on: one an earlier system left running sends none meanwhile */
	er = nv_disable(d);
	if ( er < E_OK ) {
		tm_printf((UB *)"nvme: %s: the controller did not stop (%d, CSTS %08x)\n", d->devnm, er, nv_rd(d, NV_CSTS));
		return er;
	}

	/*
	 * The interrupt, vector 0 of MSI-X (else MSI), which the admin queue
	 * uses and the I/O queue is given. With neither, the function's line
	 * is turned off as well and the controller's interrupts are masked
	 * (INTMS): the completions are looked for.
	 */
	d->irqkind = ts_pcie_irq_alloc(&d->pci, &d->intno, FALSE);
	if ( ( d->irqkind == PCI_IRQ_MSIX || d->irqkind == PCI_IRQ_MSI ) && d->intno != 0 ) {
		d->int_mark = 1;
		knl_define_inthdr((INT)d->intno, TA_HLNG, (FP)nv_inthdr);
		EnableInt(d->intno, INTPRI_DEVICE);
		knl_int_owner(d->intno, d->devnm);	/* the processor its object asks for (design 4.5) */
		knl_int_count_at(d->intno, &d->nint);	/* and its count in the object's attributes */
	} else {
		d->irqkind = PCI_IRQ_POLL;
		d->intno = 0;
		ts_pcie_cfg_write(d->pci.bus, d->pci.dev, d->pci.fn, PCI_COMMAND,
				  ts_pcie_cfg_read(d->pci.bus, d->pci.dev, d->pci.fn, PCI_COMMAND, 2) | PCI_CMD_INTX_OFF, 2);
	}

	/* the controller reset: whatever an earlier system left in it (queues, a host memory buffer) dropped */
	d->state = NV_INIT;
	tk_loc_mtx(d->adm.mtxid, TMO_FEVR);
	er = nv_start_admin(d);
	if ( er < E_OK ) {
		tm_printf((UB *)"nvme: %s: the controller did not become ready (%d, CSTS %08x)\n",
			  d->devnm, er, nv_rd(d, NV_CSTS));
	}
	if ( er >= E_OK ) {
		er = nv_identify(d, hm);
	}
	if ( er >= E_OK ) {
		nv_hmb_alloc(d, hm[0], hm[1], hm[2], hm[3]);
		er = nv_start_io(d, FALSE);
	}
	if ( er >= E_OK ) {
		d->state = NV_LIVE;
	}
	tk_unl_mtx(d->adm.mtxid);
	return er;
}

EXPORT INT knl_nvme_init( void )
{
	T_PCIDEV	p;
	INT		k, i;
	ER		er;

	for ( k = 0; ts_pcie_find(0x01, 0x08, 0x02, k, &p) == E_OK; k++ ) {
		NV_DEV	*d = nv_new();

		if ( d == NULL ) {
			tm_printf((UB *)"nvme: no memory for one more controller\n");
			break;
		}
		knl_memset(d, 0, sizeof(*d));
		d->pci = p;
		nv_name(d->devnm, nv_ndev);
		__atomic_add_fetch(&nv_ndev, 1, __ATOMIC_RELEASE);	/* before its interrupt can come */
		er = nv_setup(d);
		if ( er < E_OK ) {
			tm_printf((UB *)"nvme: %04x:%04x at %d:%d.%d not taken (%d)\n",
				  p.vendor, p.device, p.bus, p.dev, p.fn, er);
			if ( d->base != 0 ) (void)nv_disable(d);	/* nothing it was told carried out later */
			if ( d->intno != 0 ) DisableInt(d->intno);
			d->intno = 0;
			__atomic_sub_fetch(&nv_ndev, 1, __ATOMIC_RELEASE);	/* its entry is the next one's */
			continue;
		}
		if ( d->blksz == BLK_SECTOR_SIZE ) {
			knl_read_parttbl(nv_readfn, d, d->capacity, &d->tbl);
		} else {
			knl_memset(&d->tbl, 0, sizeof(d->tbl));
			tm_printf((UB *)"nvme: %s: blocks of %d bytes: its partition table is not read"
				  " (GPT and MBR are read on sectors of 512 bytes)\n", d->devnm, d->blksz);
		}
		er = nv_define(d);
		if ( er < E_OK ) {
			return er;
		}
		tm_printf((UB*)"TessronOS: %s %ld blocks of %d bytes (%d MB), %d partitions (%s), interrupt %d (%s)%s\n",
			d->devnm, (D)d->capacity, d->blksz, (UW)( ( d->capacity * d->blksz ) >> 20 ),
			d->tbl.n, d->tbl.gpt ? "GPT" : "MBR", d->intno, ts_pcie_irq_name(d->irqkind),
			d->hmb_on ? ", host memory buffer" : "");
		if ( d->hmb_n > 0 ) {
			tm_printf((UB *)"nvme: %s: host memory buffer of %ld KB in %d pieces %s\n", d->devnm,
				  (D)( d->hmb_bytes >> 10 ), d->hmb_n, d->hmb_on ? "given" : "NOT taken");
		} else {
			tm_printf((UB *)"nvme: %s: no host memory buffer\n", d->devnm);
		}
		for ( i = 0; i < d->tbl.n; i++ ) {
			tm_printf((UB*)"TessronOS:   %s%d %d+%d\n", d->devnm, i,
				(UW)d->tbl.part[i].start, (UW)d->tbl.part[i].nsect);
		}
	}
	return nv_ndev;
}

/* ---------------------------------------------------------------- the machine going off */

LOCAL NV_DEV *nv_by_name( CONST UB *devnm )
{
	INT	i, k;

	for ( i = 0; devnm != NULL && i < nv_ndev; i++ ) {
		for ( k = 0; devnm[k] != '\0' && nv_dev[i]->devnm[k] == devnm[k]; k++ ) ;
		if ( devnm[k] == '\0' && nv_dev[i]->devnm[k] == '\0' ) return nv_dev[i];
	}
	return NULL;
}

EXPORT ER knl_nvme_stat( CONST UB *devnm, T_NVSTAT *st )
{
	NV_DEV	*d = nv_by_name(devnm);

	if ( d == NULL || st == NULL ) {
		return E_NOEXS;
	}
	st->state = d->state;
	st->irqkind = d->irqkind;
	st->intno = d->intno;
	st->nint = d->nint;
	st->ncmd = d->ncmd;
	st->nlate = d->nlate;
	st->ntmo = d->ntmo;
	st->nreset = d->nreset;
	st->blksz = d->blksz;
	st->capacity = d->capacity;
	st->hmb_bytes = d->hmb_bytes;
	st->hmb_on = d->hmb_on;
	st->shst_ms = d->shst_ms;
	return E_OK;
}

#if USE_KTEST
EXPORT ER knl_nvme_test_drop( CONST UB *devnm )
{
	NV_DEV	*d = nv_by_name(devnm);

	if ( d == NULL ) {
		return E_NOEXS;
	}
	d->drop_next = TRUE;
	return E_OK;
}
#endif

EXPORT void knl_nvme_report( void )
{
	INT	i;

	for ( i = 0; i < nv_ndev; i++ ) {
		NV_DEV	*d = nv_dev[i];

		tm_printf((UB *)"nvme: %s: %u commands, %u interrupts (%s, interrupt %d), %u found without"
			  " their interrupt, %u not answered, %u resets, host memory buffer %ld KB%s\n",
			  d->devnm, d->ncmd, d->nint, ts_pcie_irq_name(d->irqkind), d->intno, d->nlate,
			  d->ntmo, d->nreset, (D)( d->hmb_bytes >> 10 ), d->hmb_on ? " in use" : "");
	}
}

/* How long the shutdown may take: RTD3E when the controller says more than NV_SHN_MIN_MS */
LOCAL UW nv_shn_ms( NV_DEV *d )
{
	UW	ms = ( d->rtd3e + 999 ) / 1000;

	if ( ms < NV_SHN_MIN_MS ) ms = NV_SHN_MIN_MS;
	if ( ms > NV_SHN_MAX_MS ) ms = NV_SHN_MAX_MS;
	return ms;
}

/*
 * One controller shut down: its requests held off (the buffer's lock and
 * both queues' held to the end), the volatile cache flushed, the I/O
 * submission queue deleted and then its completion queue, the host
 * memory buffer taken back, and CC.SHN set to 01b with CC.EN left on;
 * CSTS.SHST 10b says the shutdown is complete
 */
LOCAL void nv_shutdown_one( NV_DEV *d )
{
	NV_SQE	c;
	BOOL	broken = FALSE;
	ER	fl = E_IO, dq = E_IO, hm = E_OK;
	UW	cc, s, t0, tmo;

	tk_loc_mtx(d->bufmtx, TMO_FEVR);
	tk_loc_mtx(d->io.mtxid, TMO_FEVR);
	tk_loc_mtx(d->adm.mtxid, TMO_FEVR);
	d->closing = TRUE;
	if ( d->state == NV_LIVE ) {
		knl_memset(&c, 0, sizeof(c));
		c.cdw0 = IO_FLUSH;
		c.nsid = d->nsid;
		fl = nv_issue(d, &d->io, &c, NULL, NV_TMO_MS, &broken);
		if ( !broken ) {
			knl_memset(&c, 0, sizeof(c));
			c.cdw0 = ADM_DELETE_SQ;
			c.cdw10 = 1;
			dq = nv_admin(d, &c, NULL, NV_TMO_MS);
			if ( dq >= E_OK ) {
				knl_memset(&c, 0, sizeof(c));
				c.cdw0 = ADM_DELETE_CQ;
				c.cdw10 = 1;
				dq = nv_admin(d, &c, NULL, NV_TMO_MS);
			}
			if ( d->hmb_on ) {
				hm = nv_hmb_set(d, FALSE, FALSE, NV_TMO_MS);
			}
		}
	}
	cc = nv_rd(d, NV_CC);
	s = nv_rd(d, NV_CSTS);
	if ( cc == NV_GONE || s == NV_GONE ) {
		tm_printf((UB *)"nvme: %s: gone: no shutdown to tell it\n", d->devnm);
	} else if ( ( cc & CC_EN ) == 0 || ( s & CSTS_RDY ) == 0 ) {
		tm_printf((UB *)"nvme: %s: not enabled (CC %08x, CSTS %08x): no shutdown to tell it\n", d->devnm, cc, s);
	} else {
		tmo = nv_shn_ms(d);
		nv_wr(d, NV_CC, ( cc & ~CC_SHN_MASK ) | CC_SHN_NORMAL);
		t0 = nv_ms();
		for ( ;; ) {
			s = nv_rd(d, NV_CSTS);
			if ( s == NV_GONE || ( s & CSTS_SHST_MASK ) == CSTS_SHST_DONE
			  || (UW)( nv_ms() - t0 ) >= tmo ) {
				break;
			}
			tk_dly_tsk(1);
		}
		d->shst_ms = (UW)( nv_ms() - t0 ) + 1;
		tm_printf((UB *)"nvme: %s: shutdown %s in %u ms (of %u; CSTS %08x): flush %d, I/O queues deleted %d,"
			  " host memory buffer taken back %d\n", d->devnm,
			  ( s != NV_GONE && ( s & CSTS_SHST_MASK ) == CSTS_SHST_DONE ) ? "complete" : "NOT complete",
			  d->shst_ms - 1, tmo, s, fl, dq, hm);
	}
	d->state = NV_OFF;
	tk_unl_mtx(d->adm.mtxid);
	tk_unl_mtx(d->io.mtxid);
	tk_unl_mtx(d->bufmtx);
}

EXPORT void knl_nvme_shutdown( void )
{
	INT	i;

	for ( i = 0; i < nv_ndev; i++ ) {
		nv_shutdown_one(nv_dev[i]);
	}
}

#else /* PCIE_ECAM_PA || PCIE_CFG_BOARD */

EXPORT void knl_nvme_report( void )
{
}

EXPORT void knl_nvme_shutdown( void )
{
}

#endif /* PCIE_ECAM_PA || PCIE_CFG_BOARD */
