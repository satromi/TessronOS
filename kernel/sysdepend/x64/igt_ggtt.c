/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_ggtt.c (x64 PC)
 *	The global graphics translation table of Intel's integrated
 *	graphics of the 9th generation, kept in one place for both drivers
 *	of the device (igt_ggtt.h).
 *
 *	The ranges taken are kept in an array sorted by address, at most
 *	GGTT_NRANGE of them: both drivers take few and large ones (a
 *	surface, a context's image with its ring), and what is placed page
 *	by page lives in the render engine's own page tables, never here.
 *
 *	An entry of the table is 64 bits: the page's physical address and
 *	bit 0, present. The table lies in the first BAR from 8MB on and is
 *	written through the device window (uncached); the GPU keeps a cache
 *	of it, flushed by writing GFX_FLSH_CNTL after the entries.
 *
 *	The mappable part is read entry by entry when this starts, and the
 *	runs of present entries the firmware left are entered as ranges of
 *	their own ("firmware"), so that the screen the GOP set up stays where
 *	it is, whichever driver comes first. The part above it is not read
 *	(a million uncached reads), and the firmware is not known to map
 *	anything there.
 *
 *	The table's memory is not cleared at power on on every machine, and
 *	what it held then reads as entries: thousands of short runs, far
 *	more than the array holds. So the runs are kept as at most
 *	GGTT_FW_KEEP ranges, the closest ones joined (the holes between them
 *	taken as the firmware's too), and the rest of the array is left to
 *	the drivers. When no free room of the size asked for is left, a
 *	range is placed over the firmware's ranges, except the one at the
 *	start of the space, where the GOP's screen lies: after the boot
 *	services have ended, nothing but the display plane reads the table.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "igt_ggtt.h"

#define GGTT_NRANGE	128
#define GGTT_FW_KEEP	16		/* the firmware's runs kept as at most this many ranges */
#define GGTT_TBL_OFF	( 8UL * 1024 * 1024 )
#define GGTT_FLUSH	0x101008
#define GGTT_PRESENT	1ULL
#define GGTT_ADDR	0x0000007ffffff000ULL

typedef struct {
	UD	off, size;
	UB	owner;
	char	name[16];
} RANGE;

LOCAL T_SPLOCK		gt_lock;
LOCAL volatile INT	gt_state;	/* 0 not looked, 1 being looked at, 2 there, -1 none */
LOCAL T_IGTDEV		gt_dev;
LOCAL RANGE		gt_rng[GGTT_NRANGE];
LOCAL INT		gt_nrng;
LOCAL UD		gt_scratch;	/* a page of zeroes cleared entries point at */
LOCAL UD		gt_fw_runs;	/* runs of present entries the firmware left */
LOCAL UD		gt_fw_stolen;	/* present entries that point into stolen memory */
LOCAL UD		gt_fw_long, gt_fw_long_at;	/* the longest run: pages, and its first */

/* Device identifiers of the 9th generation's larger parts (not the low power Broxton and Gemini Lake) */
LOCAL BOOL gen9_id( UH id )
{
	switch ( id & 0xff00 ) {
	case 0x1900:		/* Skylake */
	case 0x5900:		/* Kaby Lake, Amber Lake */
	case 0x3e00:		/* Coffee Lake, Whiskey Lake */
	case 0x9b00:		/* Comet Lake */
		return TRUE;
	case 0x8700:		/* Amber Lake */
		return ( id == 0x87c0 || id == 0x87ca );
	default:
		return FALSE;
	}
}

Inline volatile UD *pte_at( UD off )
{
	return (volatile UD *)( gt_dev.mmio + GGTT_TBL_OFF + ( off >> 12 ) * 8 );
}

LOCAL void flush( void )
{
	DeviceBarrier();
	*(volatile UW *)( gt_dev.mmio + GGTT_FLUSH ) = 1;
	(void)*(volatile UW *)( gt_dev.mmio + GGTT_FLUSH );
}

/* A range entered at its place in the sorted array; under the lock */
LOCAL ER rng_add( UD off, UD size, UINT owner, CONST char *name )
{
	INT	i, k;

	if ( gt_nrng >= GGTT_NRANGE ) return E_LIMIT;
	for ( i = 0; i < gt_nrng && gt_rng[i].off < off; i++ ) ;
	for ( k = gt_nrng; k > i; k-- ) gt_rng[k] = gt_rng[k - 1];
	gt_rng[i].off = off;
	gt_rng[i].size = size;
	gt_rng[i].owner = (UB)owner;
	for ( k = 0; name != NULL && name[k] != '\0' && k < 15; k++ ) gt_rng[i].name[k] = name[k];
	gt_rng[i].name[k] = '\0';
	gt_nrng++;
	return E_OK;
}

/*
 * The firmware's runs of present entries in the mappable part, entered
 * as taken: at most GGTT_FW_KEEP ranges, each new run beyond them joined
 * with its neighbour across the smallest hole there is
 */
LOCAL void fw_ranges( void )
{
	UD	n = gt_dev.mappable >> 12, i, start = 0;
	UD	rs[GGTT_FW_KEEP + 1], re[GGTT_FW_KEEP + 1];	/* pages: first, and past the last */
	INT	nr = 0, k, j;
	BOOL	in = FALSE;

	gt_fw_runs = gt_fw_stolen = gt_fw_long = gt_fw_long_at = 0;
	for ( i = 0; i <= n; i++ ) {
		UD	e = ( i < n ) ? *pte_at(i << 12) : 0;
		BOOL	p = ( e & GGTT_PRESENT ) != 0;

		if ( p ) {
			gt_dev.fw_pages++;
			if ( gt_dev.stolen_size != 0 && ( e & GGTT_ADDR ) >= gt_dev.stolen_pa
			  && ( e & GGTT_ADDR ) < gt_dev.stolen_pa + gt_dev.stolen_size ) {
				gt_fw_stolen++;
			}
		}
		if ( p && !in ) {
			start = i;
			in = TRUE;
		} else if ( !p && in ) {
			in = FALSE;
			gt_fw_runs++;
			if ( i - start > gt_fw_long ) {
				gt_fw_long = i - start;
				gt_fw_long_at = start;
			}
			rs[nr] = start;
			re[nr] = i;
			nr++;
			if ( nr > GGTT_FW_KEEP ) {
				for ( k = 0, j = 1; j < nr - 1; j++ ) {
					if ( rs[j + 1] - re[j] < rs[k + 1] - re[k] ) k = j;
				}
				re[k] = re[k + 1];
				for ( j = k + 1; j < nr - 1; j++ ) {
					rs[j] = rs[j + 1];
					re[j] = re[j + 1];
				}
				nr--;
			}
		}
	}
	for ( k = 0; k < nr; k++ ) {
		(void)rng_add(rs[k] << 12, ( re[k] - rs[k] ) << 12, IGT_GGTT_FIRMWARE, "firmware");
	}
}

/* The firmware's ranges as kept, for KERNEL.TXT */
LOCAL void fw_say( void )
{
	INT	i;

	tm_printf((UB *)"igt: the firmware's entries: %ld present in %ld runs (%ld into stolen memory), "
		  "the longest %ld pages at %lx; kept as these ranges:\n", (UD)gt_dev.fw_pages, gt_fw_runs,
		  gt_fw_stolen, gt_fw_long, gt_fw_long_at << 12);
	for ( i = 0; i < gt_nrng; i++ ) {
		if ( gt_rng[i].owner == IGT_GGTT_FIRMWARE ) {
			tm_printf((UB *)"igt:   %08lx-%08lx (%ld pages)\n", gt_rng[i].off,
				  gt_rng[i].off + gt_rng[i].size, gt_rng[i].size >> 12);
		}
	}
}

LOCAL ER look( void )
{
	T_PCIDEV	d;
	PFRAME		*pf;
	UINT		ggms, gms;
	INT		i;

	for ( i = 0; ; i++ ) {
		if ( ts_pcie_find(0x03, 0xff, 0xff, i, &d) < E_OK ) return E_NOEXS;
		if ( d.vendor == 0x8086 && d.bus == 0 && d.dev == 2 && d.fn == 0 ) break;
	}
	if ( !gen9_id(d.device) ) return E_NOEXS;
	if ( d.bar[0] == 0 || d.bar_size[0] < 16UL * 1024 * 1024 || d.bar[2] == 0 ) return E_NOEXS;

	gt_dev.pci = d;
	gt_dev.mmio = (UBINT)DEV_BASE(d.bar[0]);
	gt_dev.gmadr_pa = d.bar[2];
	gt_dev.mappable = d.bar_size[2];
	gt_dev.ggc = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x50, 4);
	gt_dev.bdsm = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x5c, 4);
	ggms = ( gt_dev.ggc >> 6 ) & 3;
	if ( ggms == 0 ) return E_NOEXS;			/* no table */
	/* 2, 4 or 8MB of entries, each mapping 4KB */
	gt_dev.ggtt_size = ( ( 1UL << ggms ) * 1024 * 1024 / 8 ) * 4096;
	gms = ( gt_dev.ggc >> 8 ) & 0xff;
	gt_dev.stolen_size = ( gms < 0xf0 ) ? (UD)gms * 32 * 1024 * 1024 : (UD)( gms - 0xf0 + 1 ) * 4 * 1024 * 1024;
	gt_dev.stolen_pa = gt_dev.bdsm & 0xfff00000U;
	if ( gt_dev.mappable > gt_dev.ggtt_size ) gt_dev.mappable = gt_dev.ggtt_size;

	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) return E_NOMEM;
	gt_scratch = knl_pf_to_pa(pf);

	fw_ranges();
	tm_printf((UB *)"igt: %04x:%04x rev %02x, GGTT %ld MB (mappable %ld MB at %lx), stolen %ld MB at %lx, "
		  "%d pages the firmware mapped in %d range(s)\n", d.vendor, d.device,
		  ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x08, 1), gt_dev.ggtt_size >> 20, gt_dev.mappable >> 20,
		  gt_dev.gmadr_pa, gt_dev.stolen_size >> 20, gt_dev.stolen_pa, gt_dev.fw_pages, gt_nrng);
	fw_say();
	return E_OK;
}

EXPORT ER knl_igt_dev( T_IGTDEV *dev )
{
	UINT	imask;
	BOOL	mine = FALSE;

	ISpinLock(&gt_lock, &imask);
	if ( gt_state == 0 ) {
		gt_state = 1;
		mine = TRUE;
	}
	ISpinUnlock(&gt_lock, &imask);
	if ( mine ) {
		gt_state = ( look() >= E_OK ) ? 2 : -1;
	}
	while ( gt_state == 1 ) {
		tk_dly_tsk(1);			/* the other driver is looking */
	}
	if ( gt_state != 2 ) return E_NOEXS;
	if ( dev != NULL ) *dev = gt_dev;
	return E_OK;
}

/*
 * Room for 'size' bytes on 'align' within [lo, hi): free, or with
 * over_fw over the firmware's ranges too, all but the one at 0. Under
 * the lock.
 */
LOCAL BOOL room( UD lo, UD hi, UD size, UD align, BOOL over_fw, UD *p_at )
{
	/* the page at 0 is never handed out: 0 means "no address" to the drivers */
	UD	at = ( lo == 0 ) ? align : ( ( lo + align - 1 ) & ~( align - 1 ) );
	INT	i;

	for ( i = 0; i <= gt_nrng; i++ ) {
		RANGE	*r = ( i < gt_nrng ) ? &gt_rng[i] : NULL;
		UD	next;

		if ( r != NULL && over_fw && r->owner == IGT_GGTT_FIRMWARE && r->off != 0 ) continue;
		if ( r != NULL && r->off + r->size <= at ) continue;
		next = ( r != NULL && r->off < hi ) ? r->off : hi;
		if ( at + size <= next ) {
			*p_at = at;
			return TRUE;
		}
		if ( r == NULL ) break;
		at = ( r->off + r->size + align - 1 ) & ~( align - 1 );
		if ( at >= hi ) break;
	}
	return FALSE;
}

LOCAL ER claim_locked( UD off, UD size, UINT owner, CONST char *name );

EXPORT ER knl_igt_ggtt_alloc( UD size, UD align, UINT flags, UINT owner, CONST char *name, UD *p_off )
{
	UD	at, pass_lo[2], pass_hi[2];
	INT	pass, npass, over;
	UINT	imask;
	BOOL	over_fw = FALSE;
	ER	er = E_NOMEM;

	if ( knl_igt_dev(NULL) < E_OK ) return E_NOEXS;
	if ( size == 0 || ( size & 0xfff ) != 0 || p_off == NULL ) return E_PAR;
	if ( align < 4096 ) align = 4096;
	if ( ( align & ( align - 1 ) ) != 0 ) return E_PAR;

	/* a mappable range in the mappable part; any other above it first, then anywhere */
	if ( flags & IGT_GGTT_MAPPABLE ) {
		pass_lo[0] = 0;
		pass_hi[0] = gt_dev.mappable;
		npass = 1;
	} else {
		pass_lo[0] = gt_dev.mappable;
		pass_hi[0] = gt_dev.ggtt_size;
		pass_lo[1] = 0;
		pass_hi[1] = gt_dev.mappable;
		npass = 2;
	}

	/* free room first; then over the firmware's ranges */
	ISpinLock(&gt_lock, &imask);
	for ( over = 0; over < 2 && er < E_OK; over++ ) {
		for ( pass = 0; pass < npass && er < E_OK; pass++ ) {
			if ( !room(pass_lo[pass], pass_hi[pass], size, align, over != 0, &at) ) continue;
			er = ( over == 0 ) ? rng_add(at, size, owner, name) : claim_locked(at, size, owner, name);
			if ( er >= E_OK ) {
				*p_off = at;
				over_fw = ( over != 0 );
			}
		}
	}
	ISpinUnlock(&gt_lock, &imask);
	if ( over_fw ) {
		tm_printf((UB *)"igt: GGTT %08lx-%08lx (%s) placed over the firmware's entries: no free room\n",
			  *p_off, *p_off + size, ( name != NULL && name[0] != '\0' ) ? name : "-");
	}
	return er;
}

/*
 * A range at a place of the caller's choosing taken over: free, or
 * within what the firmware left (the GOP's screen, which the display
 * goes on showing). What of the firmware's runs it covers is theirs no
 * more; the rest of them stays the firmware's.
 */
LOCAL ER claim_locked( UD off, UD size, UINT owner, CONST char *name )
{
	UD	end = off + size;
	INT	i;
	ER	er = E_OK;

	for ( i = 0; i < gt_nrng; i++ ) {
		RANGE	*r = &gt_rng[i];

		if ( r->off < end && off < r->off + r->size && r->owner != IGT_GGTT_FIRMWARE ) {
			er = E_OBJ;		/* someone has it */
			break;
		}
	}
	for ( i = 0; er >= E_OK && i < gt_nrng; ) {
		RANGE	*r = &gt_rng[i];
		UD	rend = r->off + r->size;

		if ( !( r->off < end && off < rend ) ) {
			i++;
			continue;
		}
		if ( r->off < off && rend > end ) {		/* the claim in its middle: split */
			r->size = off - r->off;
			er = rng_add(end, rend - end, IGT_GGTT_FIRMWARE, "firmware");
			break;
		}
		if ( r->off < off ) {
			r->size = off - r->off;
			i++;
		} else if ( rend > end ) {
			r->size = rend - end;
			r->off = end;
			i++;
		} else {
			INT	k;

			for ( k = i; k < gt_nrng - 1; k++ ) gt_rng[k] = gt_rng[k + 1];
			gt_nrng--;
		}
	}
	if ( er >= E_OK ) er = rng_add(off, size, owner, name);
	return er;
}

EXPORT ER knl_igt_ggtt_claim( UD off, UD size, UINT owner, CONST char *name )
{
	UINT	imask;
	ER	er;

	if ( knl_igt_dev(NULL) < E_OK ) return E_NOEXS;
	if ( size == 0 || ( ( off | size ) & 0xfff ) != 0 || off + size > gt_dev.ggtt_size ) return E_PAR;
	ISpinLock(&gt_lock, &imask);
	er = claim_locked(off, size, owner, name);
	ISpinUnlock(&gt_lock, &imask);
	return er;
}

EXPORT ER knl_igt_ggtt_free( UD off )
{
	INT	i, k;
	UINT	imask;
	ER	er = E_NOEXS;

	ISpinLock(&gt_lock, &imask);
	for ( i = 0; i < gt_nrng; i++ ) {
		if ( gt_rng[i].off == off && gt_rng[i].owner != IGT_GGTT_FIRMWARE ) {
			for ( k = i; k < gt_nrng - 1; k++ ) gt_rng[k] = gt_rng[k + 1];
			gt_nrng--;
			er = E_OK;
			break;
		}
	}
	ISpinUnlock(&gt_lock, &imask);
	return er;
}

EXPORT ER knl_igt_ggtt_find( CONST char *name, UD *p_off, UD *p_size )
{
	INT	i, k;
	UINT	imask;
	ER	er = E_NOEXS;

	if ( name == NULL || name[0] == '\0' || knl_igt_dev(NULL) < E_OK ) return E_NOEXS;
	ISpinLock(&gt_lock, &imask);
	for ( i = 0; i < gt_nrng; i++ ) {
		for ( k = 0; k < 16 && name[k] == gt_rng[i].name[k] && name[k] != '\0'; k++ ) ;
		if ( k < 16 && name[k] == '\0' && gt_rng[i].name[k] == '\0' ) {
			if ( p_off != NULL ) *p_off = gt_rng[i].off;
			if ( p_size != NULL ) *p_size = gt_rng[i].size;
			er = E_OK;
			break;
		}
	}
	ISpinUnlock(&gt_lock, &imask);
	return er;
}

EXPORT void knl_igt_ggtt_set( UD off, CONST UD *pa, UD npages, BOOL contig )
{
	UD	i;

	if ( gt_state != 2 || off + ( npages << 12 ) > gt_dev.ggtt_size ) return;
	for ( i = 0; i < npages; i++ ) {
		UD	a = contig ? pa[0] + ( i << 12 ) : pa[i];

		*pte_at(off + ( i << 12 )) = ( a & GGTT_ADDR ) | GGTT_PRESENT;
	}
	flush();
}

EXPORT void knl_igt_ggtt_clear( UD off, UD npages )
{
	UD	i;

	if ( gt_state != 2 || off + ( npages << 12 ) > gt_dev.ggtt_size ) return;
	for ( i = 0; i < npages; i++ ) {
		*pte_at(off + ( i << 12 )) = ( gt_scratch & GGTT_ADDR ) | GGTT_PRESENT;
	}
	flush();
}

EXPORT UD knl_igt_ggtt_pte( UD off )
{
	if ( gt_state != 2 || off >= gt_dev.ggtt_size ) return 0;
	return *pte_at(off);
}

/*
 * Over S3: the table lives in stolen memory, which keeps its contents in
 * self-refresh unless the firmware's resume touches it. The entries of
 * every range in use are copied before and compared after, and written
 * back where they differ; how many did is said.
 */
LOCAL UD	*gt_pm_copy = NULL;
LOCAL UD	gt_pm_n = 0;

EXPORT void knl_igt_ggtt_pm_save( void )
{
	INT	i;
	UD	k, n = 0;

	if ( gt_state != 2 ) return;
	for ( i = 0; i < gt_nrng; i++ ) n += gt_rng[i].size >> 12;
	if ( gt_pm_copy != NULL ) Kfree(gt_pm_copy);
	gt_pm_copy = (UD *)Kmalloc((SZ)( n * sizeof(UD) ));
	gt_pm_n = 0;
	if ( gt_pm_copy == NULL ) return;
	for ( i = 0; i < gt_nrng; i++ ) {
		for ( k = 0; k < gt_rng[i].size >> 12; k++ ) {
			gt_pm_copy[gt_pm_n++] = *pte_at(gt_rng[i].off + ( k << 12 ));
		}
	}
}

EXPORT void knl_igt_ggtt_pm_restore( void )
{
	INT	i;
	UD	k, n = 0, diff = 0;

	if ( gt_state != 2 || gt_pm_copy == NULL ) return;
	for ( i = 0; i < gt_nrng; i++ ) {
		for ( k = 0; k < gt_rng[i].size >> 12 && n < gt_pm_n; k++, n++ ) {
			volatile UD	*p = pte_at(gt_rng[i].off + ( k << 12 ));

			if ( *p != gt_pm_copy[n] ) {
				*p = gt_pm_copy[n];
				diff++;
			}
		}
	}
	flush();
	tm_printf((UB *)"igt: GGTT after the sleep: %ld of %ld entries had changed and were written back\n", diff, n);
}

#if USE_KTEST
/*
 * For the tests on a machine without the device: a table over memory
 * of the caller's ('mmio' standing for the first BAR, the entries from
 * 8MB on), read as the firmware would have left it; and back to how it
 * was
 */
LOCAL INT	gt_state_kept;

EXPORT void knl_igt_ggtt_fake( UBINT mmio, UD ggtt_size, UD mappable )
{
	PFRAME	*pf;

	gt_state_kept = gt_state;
	knl_memset(&gt_dev, 0, sizeof(gt_dev));
	gt_dev.mmio = mmio;
	gt_dev.ggtt_size = ggtt_size;
	gt_dev.mappable = mappable;
	gt_nrng = 0;
	if ( gt_scratch == 0 ) {
		pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
		if ( pf != NULL ) gt_scratch = knl_pf_to_pa(pf);
	}
	fw_ranges();
	gt_state = 2;
}

EXPORT void knl_igt_ggtt_unfake( void )
{
	gt_nrng = 0;
	knl_memset(&gt_dev, 0, sizeof(gt_dev));
	gt_state = gt_state_kept;
}

EXPORT UD knl_igt_ggtt_scratch( void )
{
	return gt_scratch;
}

/* How many ranges 'owner' has */
EXPORT INT knl_igt_ggtt_count( UINT owner )
{
	INT	i, k = 0;

	for ( i = 0; i < gt_nrng; i++ ) {
		if ( gt_rng[i].owner == owner ) k++;
	}
	return k;
}
#endif

EXPORT void knl_igt_ggtt_report( void )
{
	static CONST char	*own[] = { "?", "display", "render", "firmware" };
	INT	i;

	if ( gt_state != 2 ) return;
	for ( i = 0; i < gt_nrng; i++ ) {
		tm_printf((UB *)"igt: GGTT %08lx-%08lx %s %s\n", gt_rng[i].off, gt_rng[i].off + gt_rng[i].size,
			  own[gt_rng[i].owner & 3], gt_rng[i].name);
	}
}

#endif /* X64 */
