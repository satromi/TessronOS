/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ahci.c (x64 PC)
 *	Serial ATA disks behind an AHCI controller (PCI class 01h, subclass
 *	06h, interface 01h): the SSD of a notebook PC that has a SATA one
 *	(some ThinkPad X1 Yoga), and QEMU q35's ICH9.
 *
 *	Every port with an ATA disk on it (its link up, its signature an
 *	ATA one) is registered with tk_def_dev() as "sataa", "satab", ...:
 *	subunit 0 the whole disk, 1..n its partitions from the GPT or MBR,
 *	as the other disks are ("sataa0" the first partition). It is a disk
 *	object then, as an NVMe disk is.
 *
 *	A port is spoken to through one command slot, a command at a time:
 *	the file systems hand out the I/O of a disk one request at a time
 *	anyway. The data goes through a buffer of the driver's, physically
 *	contiguous and below 4GB, described by one PRD entry. The commands
 *	are the 48 bit ones (READ DMA EXT, WRITE DMA EXT, FLUSH CACHE EXT).
 *	Completion is looked for, not waited for with an interrupt.
 *
 *	The controller is left as the firmware set it (no reset of the
 *	HBA); only the ports with a disk are stopped, given their memory
 *	and started again.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/pcie.h>
#include <ts/pm.h>
#include "sysman/pfalloc.h"

#ifdef X64

/* The controller's registers (ABAR) */
#define CAP		0x00
#define GHC		0x04
#define PI		0x0c
#define VS		0x10
#define GHC_AE		0x80000000U

/* A port's, from 0x100 + 0x80 n */
#define PX(n)		( 0x100 + 0x80 * (n) )
#define P_CLB		0x00
#define P_CLBU		0x04
#define P_FB		0x08
#define P_FBU		0x0c
#define P_IS		0x10
#define P_IE		0x14
#define P_CMD		0x18
#define P_TFD		0x20
#define P_SIG		0x24
#define P_SSTS		0x28
#define P_SERR		0x30
#define P_CI		0x38

#define CMD_ST		0x0001
#define CMD_FRE		0x0010
#define CMD_FR		0x4000
#define CMD_CR		0x8000
#define TFD_BSY		0x80
#define TFD_DRQ		0x08
#define TFD_ERR		0x01
#define IS_TFES		0x40000000U

#define SIG_ATA		0x00000101U

#define ATA_IDENTIFY	0xec
#define ATA_READ_EXT	0x25
#define ATA_WRITE_EXT	0x35
#define ATA_WRITE_FUA	0x3d		/* WRITE DMA FUA EXT */
#define ATA_FLUSH_EXT	0xea

#define AH_BUF_PAGES	32			/* the driver's buffer: 128KB */
#define AH_MAX_SECT	( AH_BUF_PAGES * PAGE_SIZE / BLK_SECTOR_SIZE )
#define AH_TMO_MS	10000

typedef struct {
	UBINT		abar;
	INT		port;
	UB		*mem;			/* the command list, the FIS area, the command table */
	UD		mem_pa;
	UB		*buf;
	UD		buf_pa;
	UD		capacity;		/* sectors */
	ID		mtxid;
	ID		devid;
	T_PARTTBL	tbl;
	UB		devnm[8];
	UB		model[41];
	BOOL		s64;			/* the controller reaches memory past 4GB */
	BOOL		fua;			/* the disk takes WRITE DMA FUA EXT */
} AH_DEV;

/*
 * The disks, as many as there are on the ports: each made when it is
 * found and kept, the table of them twice as large when it fills.
 */
LOCAL AH_DEV	**ah_dev = NULL;
LOCAL INT	ah_ndev = 0, ah_cap = 0;

/* The table's next entry, made; NULL when memory is short */
LOCAL AH_DEV *ah_new( void )
{
	AH_DEV	**t;
	INT	n;

	if ( ah_ndev >= ah_cap ) {
		n = ( ah_cap > 0 ) ? ah_cap * 2 : 4;
		t = (AH_DEV **)Kcalloc((SZ)n, sizeof(AH_DEV *));
		if ( t == NULL ) return NULL;
		if ( ah_dev != NULL ) knl_memcpy(t, ah_dev, sizeof(AH_DEV *) * (SZ)ah_cap);
		ah_dev = t;
		ah_cap = n;
	}
	if ( ah_dev[ah_ndev] == NULL ) {
		ah_dev[ah_ndev] = (AH_DEV *)Kcalloc(1, sizeof(AH_DEV));
	}
	return ah_dev[ah_ndev];
}

/* The name of the n-th disk: the stem and "a" ... "z", then "aa" ... */
LOCAL void ah_name( UB *nm, INT n )
{
	INT	k = 0;
	CONST char *stem = "sata";

	while ( stem[k] != '\0' ) { nm[k] = (UB)stem[k]; k++; }
	if ( n < 26 ) {
		nm[k++] = (UB)( 'a' + n );
	} else {
		nm[k++] = (UB)( 'a' + ( n / 26 - 1 ) % 26 );
		nm[k++] = (UB)( 'a' + n % 26 );
	}
	nm[k] = '\0';
}


LOCAL UW  ar( AH_DEV *d, UW off ) { return in_w(d->abar + PX(d->port) + off); }
LOCAL void aw( AH_DEV *d, UW off, UW v ) { out_w(d->abar + PX(d->port) + off, v); }

LOCAL void *pages( UINT order, UD *p_pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) {
		return NULL;
	}
	*p_pa = knl_pf_to_pa(pf);
	return PA2VA(*p_pa);
}

/* Wait up to ms for (reg & mask) == want */
LOCAL BOOL await( AH_DEV *d, UW off, UW mask, UW want, INT ms )
{
	INT	i;

	for ( i = 0; i < ms; i++ ) {
		if ( ( ar(d, off) & mask ) == want ) return TRUE;
		tk_dly_tsk(1);
	}
	return ( ( ar(d, off) & mask ) == want );
}

/*
 * Memory a command's data moves straight to or from, as the physical
 * region table names it: up to AH_PRD_MAX runs, each of an even number
 * of bytes from an even address (AHCI 1.3.1, 4.2.3.3).
 */
#define AH_PRD_MAX	128			/* what the command table's page holds after the FIS */
#define AH_DMAX_SECT	8192			/* sectors of one command straight from memory: 4MB */

typedef struct {
	UD	pa[AH_PRD_MAX];
	UW	len[AH_PRD_MAX];
	INT	n;
} AH_PRDS;

LOCAL ER ah_cmdp( AH_DEV *d, UB cmd, UD lba, UW count, CONST AH_PRDS *pr, BOOL wr );

/*
 * One command in slot 0: its register FIS, bytes of data in or out of
 * the driver's buffer; the port's task file error or a time out
 */
LOCAL ER ah_cmd( AH_DEV *d, UB cmd, UD lba, UW count, UW bytes, BOOL wr )
{
	AH_PRDS	pr;

	pr.n = 0;
	if ( bytes > 0 ) {
		pr.pa[0] = d->buf_pa;
		pr.len[0] = bytes;
		pr.n = 1;
	}
	return ah_cmdp(d, cmd, lba, count, &pr, wr);
}

/*
 * The runs of the caller's memory one command can move, whole sectors of
 * at most `bytes`: 0 when the memory is not the kernel's, not on an even
 * address, or not reachable by a controller of 32-bit addresses.
 */
LOCAL UD ah_map( AH_DEV *d, UB *buf, UD bytes, AH_PRDS *pr )
{
	void	*pa;
	UD	at = 0;
	INT	r;

	pr->n = 0;
	if ( ( (UBINT)buf & 1 ) != 0 ) {
		return 0;
	}
	while ( at < bytes && pr->n < AH_PRD_MAX ) {
		UD	want = bytes - at;

		if ( want > 0x400000 ) want = 0x400000;		/* 4MB a region */
		pa = NULL;
		r = ConvPhysicalAddress(buf + at, (INT)want, &pa);
		if ( r <= 0 || pa == NULL ) break;
		if ( !d->s64 && (UD)(UBINT)pa + (UD)r > 0x100000000ULL ) break;
		if ( pr->n > 0 && pr->pa[pr->n - 1] + pr->len[pr->n - 1] == (UD)(UBINT)pa
		  && (UD)pr->len[pr->n - 1] + (UD)r <= 0x400000 ) {
			pr->len[pr->n - 1] += (UW)r;		/* the run goes on */
		} else {
			pr->pa[pr->n] = (UD)(UBINT)pa;
			pr->len[pr->n] = (UW)r;
			pr->n++;
		}
		at += (UD)r;
	}
	at -= at % BLK_SECTOR_SIZE;
	/* the regions cut to the whole sectors */
	{
		UD	sum = 0;
		INT	i;

		for ( i = 0; i < pr->n; i++ ) {
			if ( sum + pr->len[i] >= at ) {
				pr->len[i] = (UW)( at - sum );
				pr->n = ( pr->len[i] > 0 ) ? i + 1 : i;
				break;
			}
			sum += pr->len[i];
		}
	}
	return at;
}

LOCAL ER ah_cmdp( AH_DEV *d, UB cmd, UD lba, UW count, CONST AH_PRDS *pr, BOOL wr )
{
	UW	*hdr = (UW *)d->mem;			/* the command list: slot 0 */
	UB	*tbl = d->mem + 1024 + 256;		/* the command table, 128 byte aligned */
	UD	tbl_pa = d->mem_pa + 1024 + 256;
	UB	*fis = tbl;
	UW	*prd = (UW *)( tbl + 0x80 );
	INT	t;

	knl_memset(tbl, 0, 0x80 + 16 * (SZ)( pr->n > 0 ? pr->n : 1 ));
	fis[0] = 0x27;				/* host to device */
	fis[1] = 0x80;				/* a command */
	fis[2] = cmd;
	fis[4] = (UB)lba;
	fis[5] = (UB)( lba >> 8 );
	fis[6] = (UB)( lba >> 16 );
	fis[7] = 0x40;				/* LBA */
	fis[8] = (UB)( lba >> 24 );
	fis[9] = (UB)( lba >> 32 );
	fis[10] = (UB)( lba >> 40 );
	fis[12] = (UB)count;
	fis[13] = (UB)( count >> 8 );
	hdr[0] = 5 | ( wr ? 0x40 : 0 ) | ( (UW)pr->n << 16 );
	hdr[1] = 0;
	hdr[2] = (UW)tbl_pa;
	hdr[3] = (UW)( tbl_pa >> 32 );
	for ( t = 0; t < pr->n; t++ ) {
		prd[t * 4 + 0] = (UW)pr->pa[t];
		prd[t * 4 + 1] = (UW)( pr->pa[t] >> 32 );
		prd[t * 4 + 2] = 0;
		prd[t * 4 + 3] = pr->len[t] - 1;
	}
	DeviceBarrier();
	aw(d, P_IS, ~0U);
	aw(d, P_CI, 1);
	for ( t = 0; t < AH_TMO_MS * 20; t++ ) {
		if ( ( ar(d, P_CI) & 1 ) == 0 ) break;
		if ( ar(d, P_IS) & IS_TFES ) break;
		if ( t < 200 ) WaitUsec(50); else tk_dly_tsk(1);
	}
	if ( ( ar(d, P_IS) & IS_TFES ) || ( ar(d, P_TFD) & TFD_ERR ) ) {
		tm_printf((UB *)"ahci: %s command %02x at %d failed: task file %04x, error %08x\n",
			  d->devnm, cmd, (UW)lba, ar(d, P_TFD), ar(d, P_SERR));
		aw(d, P_SERR, ~0U);
		/* the port started again, to take the next command */
		aw(d, P_CMD, ar(d, P_CMD) & ~CMD_ST);
		(void)await(d, P_CMD, CMD_CR, 0, 500);
		aw(d, P_IS, ~0U);
		aw(d, P_CMD, ar(d, P_CMD) | CMD_ST);
		return E_IO;
	}
	if ( ar(d, P_CI) & 1 ) {
		return E_TMOUT;
	}
	return E_OK;
}

LOCAL ER ah_rw( AH_DEV *d, BOOL wr, UD sector, UB *buf, UD nsect, BOOL fua )
{
	ER	er = E_OK;
	UB	wcmd = fua ? ATA_WRITE_FUA : ATA_WRITE_EXT;

	tk_loc_mtx(d->mtxid, TMO_FEVR);
	while ( nsect > 0 && er == E_OK ) {
		AH_PRDS	pr;
		UD	n = ( nsect < AH_DMAX_SECT ) ? nsect : AH_DMAX_SECT;
		UD	bytes = ah_map(d, buf, n * BLK_SECTOR_SIZE, &pr);

		if ( bytes >= BLK_SECTOR_SIZE ) {
			/* straight from and to the caller's memory */
			n = bytes / BLK_SECTOR_SIZE;
			er = ah_cmdp(d, wr ? wcmd : ATA_READ_EXT, sector, (UW)n, &pr, wr);
		} else {
			n = ( nsect < AH_MAX_SECT ) ? nsect : AH_MAX_SECT;
			bytes = n * BLK_SECTOR_SIZE;
			if ( wr ) knl_memcpy(d->buf, buf, (SZ)bytes);
			er = ah_cmd(d, wr ? wcmd : ATA_READ_EXT, sector, (UW)n, (UW)bytes, wr);
			if ( er == E_OK && !wr ) knl_memcpy(buf, d->buf, (SZ)bytes);
		}
		sector += n;
		buf += bytes;
		nsect -= n;
	}
	tk_unl_mtx(d->mtxid);
	return er;
}

LOCAL ER ah_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	return ah_rw((AH_DEV *)exinf, FALSE, start, (UB *)buf, nsect, FALSE);
}

LOCAL ER ah_flush( AH_DEV *d )
{
	ER	er;

	tk_loc_mtx(d->mtxid, TMO_FEVR);
	er = ah_cmd(d, ATA_FLUSH_EXT, 0, 0, 0, FALSE);
	tk_unl_mtx(d->mtxid);
	return er;
}

/* ---------------------------------------------------------------- driver interface */

LOCAL ER ah_unit_range( AH_DEV *d, INT unitno, UD *p_start, UD *p_nsect )
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

LOCAL ER ah_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

LOCAL ER ah_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER ah_attr( AH_DEV *d, INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nsect;
	ER		er;

	if ( req->start == TDN_IOMAX && req->cmd == TDC_READ && req->size >= (SZ)sizeof(UW) ) {
		/* the most one command moves straight from memory (design 7.3) */
		*(UW *)req->buf = AH_DMAX_SECT * BLK_SECTOR_SIZE;
		req->asize = sizeof(UW);
		return E_OK;
	}
	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ || req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = ah_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		return er;
	}
	info = (DiskInfo *)req->buf;
	info->protect   = FALSE;
	info->removable = FALSE;
	info->blocksize = BLK_SECTOR_SIZE;
	/* the count in a W stops at 2^31 - 1 sectors (1TB); the whole count is in blockcont_d */
	info->blockcont = ( nsect > 0x7FFFFFFFULL ) ? 0x7FFFFFFF : (W)nsect;
	info->blockcont_d = (D)nsect;
	req->asize = sizeof(DiskInfo);
	return E_OK;
}

LOCAL ER ah_define( AH_DEV *d );

LOCAL ER ah_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	AH_DEV	*d = (AH_DEV *)exinf;
	INT	unitno = (INT)( req->devid & 0xff );
	UD	start, nsect, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;
	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		req->error = ah_flush(d);
		return E_OK;
	}
	if ( req->start == TDN_PARTSCAN && req->cmd == TDC_WRITE ) {
		INT	n = ( unitno != 0 ) ? E_PAR : knl_part_rescan(ah_readfn, d, d->capacity, &d->tbl);

		req->error = ( n < E_OK ) ? n : ah_define(d);
		return E_OK;
	}
	if ( req->start == TDN_FUAWRITE && req->cmd == TDC_WRITE ) {
		/* blocks written through to the medium, when the disk says it can */
		CONST T_DEVFUA	*fw = (CONST T_DEVFUA *)req->buf;

		if ( !d->fua || req->size < (SZ)sizeof(T_DEVFUA) || fw->size <= 0
		  || ( fw->size % BLK_SECTOR_SIZE ) != 0 || fw->start < 0
		  || ah_unit_range(d, unitno, &start, &nsect) < E_OK
		  || (UD)fw->start + (UD)fw->size / BLK_SECTOR_SIZE > nsect ) {
			req->error = E_PAR;
			return E_OK;
		}
		req->error = ah_rw(d, TRUE, start + (UD)fw->start, (UB *)fw->buf,
				   (UD)fw->size / BLK_SECTOR_SIZE, TRUE);
		if ( req->error == E_OK ) req->asize = req->size;
		return E_OK;
	}
	if ( req->start < 0 ) {
		req->error = ah_attr(d, unitno, req);
		return E_OK;
	}
	er = ah_unit_range(d, unitno, &start, &nsect);
	if ( er < E_OK ) {
		req->error = er;
		return E_OK;
	}
	if ( ( req->size % BLK_SECTOR_SIZE ) != 0 ) {
		req->error = E_PAR;
		return E_OK;
	}
	nreq = (UD)req->size / BLK_SECTOR_SIZE;
	if ( (UD)req->start >= nsect || (UD)req->start + nreq > nsect ) {
		req->error = E_PAR;
		return E_OK;
	}
	er = ah_rw(d, ( req->cmd == TDC_WRITE ), start + (UD)req->start, (UB *)req->buf, nreq, FALSE);
	if ( er == E_OK ) {
		req->asize = req->size;
	}
	req->error = er;
	return E_OK;
}

LOCAL INT ah_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER ah_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT ah_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

LOCAL ER ah_define( AH_DEV *d )
{
	T_DDEV	ddev;
	ID	id;

	ddev.exinf   = d;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_DISK | TDK_UNDEF;
	ddev.nsub    = d->tbl.n;
	ddev.blksz   = BLK_SECTOR_SIZE;
	ddev.openfn  = (FP)ah_open;
	ddev.closefn = (FP)ah_close;
	ddev.execfn  = (FP)ah_exec;
	ddev.waitfn  = (FP)ah_wait;
	ddev.abortfn = (FP)ah_abort;
	ddev.eventfn = (FP)ah_event;
	id = tk_def_dev(d->devnm, &ddev, NULL);
	if ( id > 0 ) {
		d->devid = id;
	}
	return ( id > 0 ) ? E_OK : (ER)id;
}

/* ---------------------------------------------------------------- start-up */

/* A port with a disk: stopped, given its memory, started, the disk identified */
/* The port stopped, its lists given to it and started: at start-up and after a sleep */
LOCAL ER ah_port_start( AH_DEV *d )
{
	aw(d, P_CMD, ar(d, P_CMD) & ~CMD_ST);
	if ( !await(d, P_CMD, CMD_CR, 0, 500) ) return E_IO;
	aw(d, P_CMD, ar(d, P_CMD) & ~CMD_FRE);
	if ( !await(d, P_CMD, CMD_FR, 0, 500) ) return E_IO;
	aw(d, P_CLB, (UW)d->mem_pa);
	aw(d, P_CLBU, (UW)( d->mem_pa >> 32 ));
	aw(d, P_FB, (UW)( d->mem_pa + 1024 ));
	aw(d, P_FBU, (UW)( ( d->mem_pa + 1024 ) >> 32 ));
	aw(d, P_SERR, ~0U);
	aw(d, P_IS, ~0U);
	aw(d, P_IE, 0);
	aw(d, P_CMD, ar(d, P_CMD) | CMD_FRE);
	if ( !await(d, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000) ) return E_IO;
	aw(d, P_CMD, ar(d, P_CMD) | CMD_ST);
	return E_OK;
}

LOCAL ER ah_port( AH_DEV *d )
{
	UW	*w;
	T_CMTX	cm;
	INT	i;
	ER	er;

	d->mem = (UB *)pages(0, &d->mem_pa);
	d->buf = (UB *)pages(5, &d->buf_pa);
	if ( d->mem == NULL || d->buf == NULL ) return E_NOMEM;
	er = ah_port_start(d);
	if ( er < E_OK ) return er;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	d->mtxid = tk_cre_mtx(&cm);
	if ( d->mtxid <= 0 ) return E_LIMIT;

	if ( ah_cmd(d, ATA_IDENTIFY, 0, 0, 512, FALSE) < E_OK ) return E_IO;
	w = (UW *)d->buf;
	{
		UH	*h = (UH *)d->buf;

		if ( ( h[83] & ( 1U << 10 ) ) == 0 ) {
			tm_printf((UB *)"ahci: port %d: no 48 bit addressing; not taken\n", d->port);
			return E_NOSPT;
		}
		d->capacity = (UD)h[100] | ( (UD)h[101] << 16 ) | ( (UD)h[102] << 32 ) | ( (UD)h[103] << 48 );
		/*
		 * WRITE DMA FUA EXT: word 84 valid (bits 15:14 are 01) and its
		 * bit 6 set; with 48 bit addressing, checked above
		 */
		d->fua = (BOOL)( ( h[84] >> 14 ) == 1 && ( h[84] & ( 1U << 6 ) ) != 0 );
		for ( i = 0; i < 20; i++ ) {
			d->model[2 * i] = (UB)( h[27 + i] >> 8 );
			d->model[2 * i + 1] = (UB)h[27 + i];
		}
		d->model[40] = 0;
		for ( i = 39; i >= 0 && d->model[i] == ' '; i-- ) d->model[i] = 0;
	}
	(void)w;
	return E_OK;
}

#ifdef X64
/*
 * The machine asleep: each disk's cache written out and its lock held by
 * the sleep's task, so that requests wait. On waking the controller is in
 * AHCI mode again and each port starts on the memory it has.
 */
LOCAL ER ah_pm_suspend( void *arg, INT how )
{
	INT	i;

	for ( i = 0; i < ah_ndev; i++ ) {
		(void)ah_flush(ah_dev[i]);
		tk_loc_mtx(ah_dev[i]->mtxid, TMO_FEVR);
	}
	return E_OK;
}

LOCAL void ah_pm_resume( void *arg, INT how )
{
	INT	i;

	for ( i = 0; i < ah_ndev; i++ ) {
		AH_DEV	*d = ah_dev[i];
		ER	er;

		out_w(d->abar + GHC, in_w(d->abar + GHC) | GHC_AE);
		er = ah_port_start(d);
		tm_printf((UB *)"ahci: port %d: back from the sleep: %d\n", d->port, er);
		tk_unl_mtx(d->mtxid);
	}
}

LOCAL CONST T_PMDEV	ah_pm = { "ahci", PM_ORD_DISK, ah_pm_suspend, ah_pm_resume, NULL };
#endif /* X64 */

EXPORT INT knl_ahci_init( void )
{
#ifdef X64
	(void)ts_pm_register(&ah_pm);
#endif

	T_PCIDEV	p;
	UBINT		abar;
	UW		pi;
	INT		k, port, i;
	ER		er;

	for ( k = 0; ts_pcie_find(0x01, 0x06, 0x01, k, &p) == E_OK; k++ ) {
		ts_pcie_enable(&p);
		abar = ts_pcie_bar_base(&p, 5);
		if ( abar == 0 ) continue;
		out_w(abar + GHC, in_w(abar + GHC) | GHC_AE);
		pi = in_w(abar + PI);
		tm_printf((UB *)"TessronOS: ahci %04x:%04x, version %x, %d ports (%08x), ports implemented %08x\n",
			  p.vendor, p.device, in_w(abar + VS), ( in_w(abar + CAP) & 0x1f ) + 1, in_w(abar + CAP), pi);
		for ( port = 0; port < 32; port++ ) {
			AH_DEV	*d = ah_new();
			UW	ssts, sig;

			if ( d == NULL ) {
				tm_printf((UB *)"ahci: no memory for one more disk\n");
				break;
			}
			if ( ( pi & ( 1U << port ) ) == 0 ) continue;
			ssts = in_w(abar + PX(port) + P_SSTS);
			sig = in_w(abar + PX(port) + P_SIG);
			if ( ( ssts & 0xf ) != 3 ) continue;		/* no device, or no link */
			if ( sig != SIG_ATA ) {
				tm_printf((UB *)"ahci: port %d: signature %08x (not a disk), left alone\n", port, sig);
				continue;
			}
			knl_memset(d, 0, sizeof(*d));
			d->abar = abar;
			d->port = port;
			d->s64 = (BOOL)( ( in_w(abar + CAP) & 0x80000000U ) != 0 );
			ah_name(d->devnm, ah_ndev);
			er = ah_port(d);
			if ( er < E_OK ) {
				tm_printf((UB *)"ahci: port %d not taken (%d)\n", port, er);
				continue;
			}
			ah_ndev++;
			knl_read_parttbl(ah_readfn, d, d->capacity, &d->tbl);
			if ( ah_define(d) < E_OK ) continue;
			tm_printf((UB *)"TessronOS: %s (port %d) \"%s\" %ld sectors (%d MB), %d partitions (%s)%s\n",
				  d->devnm, port, d->model, (D)d->capacity, (UW)( d->capacity >> 11 ),
				  d->tbl.n, d->tbl.gpt ? "GPT" : "MBR", d->fua ? ", FUA" : "");
			for ( i = 0; i < d->tbl.n; i++ ) {
				tm_printf((UB *)"TessronOS:   %s%d %d+%d\n", d->devnm, i,
					  (UW)d->tbl.part[i].start, (UW)d->tbl.part[i].nsect);
			}
		}
	}
	return ah_ndev;
}

#endif /* X64 */
