/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfspart.c
 *	TSFS partitions made and grown together with their volumes (design
 *	7.7), and the system volume grown at the start.
 *
 *	A grow writes the partition table first and the volume after: a
 *	partition larger than its volume is a volume that has room it does
 *	not use yet, and is grown the next time it is asked or the next
 *	start, while a volume larger than its partition would not open. The
 *	volume itself is grown so that a cut in the power at any point
 *	leaves either the volume as it was or one the next opening finishes
 *	growing (tsfsblk.c).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/gpt.h>
#include <ts/tsfsblk.h>

#ifndef CNF_SYSVOL_GROW_MB
#define CNF_SYSVOL_GROW_MB	8192
#endif

#define MB_SECT		2048ULL		/* sectors of a megabyte */
#define SLACK_BLOCKS	256		/* a partition this much larger than its volume has it grown */

LOCAL UD	sysvol_from = 0, sysvol_to = 0;

/* Blocks of a partition's device */
LOCAL ER dev_blocks( CONST char *part, UD *p_blocks )
{
	DiskInfo	info;
	SZ		asz;
	ID		dd;
	ER		er;

	dd = tk_opn_dev((CONST UB *)part, TD_READ);
	if ( dd < E_OK ) {
		return dd;
	}
	er = tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asz);
	tk_cls_dev(dd, 0);
	if ( er < E_OK ) {
		return er;
	}
	*p_blocks = (UD)info.blockcont_d / TSFSBLK_SECT_PER_BLK;
	return E_OK;
}

/* The volume on a partition grown to the partition: open, or opened for this */
LOCAL ER vol_grow( CONST char *part, UD *p_from, UD *p_to )
{
	T_TSFSBLK_SB	sb;
	UD		blocks = 0;
	ID		vol;
	BOOL		mine = FALSE;
	ER		er;

	er = dev_blocks(part, &blocks);
	if ( er < E_OK ) {
		return er;
	}
	vol = ts_vol_blk_of(part);
	if ( vol <= 0 ) {
		vol = ts_opn_vol_blk(part);
		if ( vol <= 0 ) {
			return ( vol < E_OK ) ? (ER)vol : E_OBJ;
		}
		mine = TRUE;
	}
	er = ts_ref_vol_blk(vol, &sb);
	*p_from = *p_to = sb.total_blocks;
	if ( er >= E_OK && blocks >= sb.total_blocks + SLACK_BLOCKS ) {
		tm_printf((UB *)"tsfs: %s: the volume of %ld blocks (state %x, incompat %lx) to its device's %ld\n",
			  part, (D)sb.total_blocks, (UINT)sb.state, (UD)sb.incompat, (D)blocks);
		er = ts_grow_vol_blk(vol, blocks, p_from, p_to);
		if ( er < E_OK ) {
			tm_printf((UB *)"tsfs: %s: not grown (%d)\n", part, (INT)er);
		}
	}
	if ( mine ) {
		ER	e2 = ts_cls_vol_blk(vol);

		if ( er >= E_OK && e2 < E_OK ) er = e2;
	}
	return er;
}

EXPORT ER knl_tsfs_part_grow( CONST char *part, UD bytes, UD *p_from, UD *p_to )
{
	T_GPTPART	gp;
	UD		last, from = 0, to = 0;
	ER		er;

	if ( part == NULL ) {
		return E_PAR;
	}
	er = knl_gpt_part(part, &gp);
	if ( er < E_OK ) {
		return er;
	}
	if ( !gp.tsfs ) {
		return E_OBJ;
	}
	last = gp.grow_last;
	if ( bytes > 0 ) {
		UD	want = gp.first + ( bytes / 512 ) / MB_SECT * MB_SECT - 1;

		if ( want < last ) last = want;
	}
	if ( last > gp.last ) {
		er = knl_gpt_grow(part, last);
		if ( er < E_OK ) {
			return er;
		}
	}
	er = vol_grow(part, &from, &to);
	if ( p_from != NULL ) *p_from = from * TSFSBLK_BLOCK_SIZE;
	if ( p_to != NULL ) *p_to = to * TSFSBLK_BLOCK_SIZE;
	if ( er >= E_OK && to == from && last <= gp.last ) {
		er = E_LIMIT;			/* nothing to grow into */
	}
	return er;
}

EXPORT ER knl_tsfs_part_make( CONST char *disk, UD first, UD bytes, CONST char *label,
			      UB *p_part, INT max )
{
	T_GPTFREE	fr[32];
	UD		last;
	INT		n, i;
	ER		er;

	if ( disk == NULL || p_part == NULL ) {
		return E_PAR;
	}
	n = knl_gpt_free(disk, fr, 32, NULL, NULL);
	if ( n < E_OK ) {
		return n;
	}
	for ( i = 0; i < n && fr[i].first != first; i++ ) ;
	if ( i == n ) {
		return E_PAR;			/* not where a free stretch starts */
	}
	last = fr[i].last;
	if ( bytes > 0 ) {
		UD	want = first + ( bytes / 512 ) / MB_SECT * MB_SECT - 1;

		if ( want < first || want > last ) {
			return E_PAR;
		}
		last = want;
	}
	if ( ( last - first + 1 ) / TSFSBLK_SECT_PER_BLK < TSFSBLK_MIN_BLOCKS ) {
		return E_PAR;			/* too small to be a volume */
	}
	er = knl_gpt_add(disk, first, last, knl_tsfs_type_guid, label, p_part, max);
	if ( er < E_OK ) {
		return er;
	}
	er = ts_format_blk((CONST char *)p_part, label);
	tm_printf((UB *)"tsfs: %s made, %d MB \"%s\" (%d)\n", p_part,
		  (INT)( ( last - first + 1 ) / MB_SECT ), ( label != NULL ) ? label : "", (INT)er);
	return er;
}

EXPORT void knl_sysvol_grow( CONST char *part )
{
	T_GPTPART	gp;
	UD		limit = (UD)CNF_SYSVOL_GROW_MB * MB_SECT, size, from = 0, to = 0, bytes;
	CONST char	*why = NULL;
	ER		er;

	er = knl_gpt_part(part, &gp);
	if ( er < E_OK ) {
		tm_printf((UB *)"sysvol: %s not grown (no GPT entry: %d)\n", part, (INT)er);
		return;
	}
	size = gp.last - gp.first + 1;
	bytes = 0;
	{
		T_TSFSBLK_SB	sb;
		UD		dev = 0;
		ID		vol = ts_vol_blk_of(part);

		/* where it starts from: the table, the partition's device, and its volume when it is open */
		(void)dev_blocks(part, &dev);
		tm_printf((UB *)"sysvol: %s: sectors %ld to %ld of %ld in the table, %ld blocks on its device",
			  part, (D)gp.first, (D)gp.last, (D)gp.nsect, (D)dev);
		if ( vol > 0 && ts_ref_vol_blk(vol, &sb) >= E_OK ) {
			tm_printf((UB *)", its volume %ld blocks", (D)sb.total_blocks);
		}
		tm_printf((UB *)"\n");
	}
	if ( !gp.tsfs ) {
		why = "not a TSFS partition";
	} else if ( size >= limit ) {
		why = "at its limit";
	} else if ( !gp.tail ) {
		why = "not the last partition";
	} else if ( gp.grow_last <= gp.last ) {
		why = "no room after it";
	} else {
		bytes = limit * 512;
	}
	/* the partition stays when it may not grow; its volume is still
	   brought up to it, after a cut in the power between the two */
	if ( bytes == 0 ) {
		bytes = size * 512;
	} else {
		/* said before it starts: a grow that does not come back is seen on the screen console */
		tm_printf((UB *)"sysvol: %s growing from %d MB to %d MB\n", part, (INT)( size / MB_SECT ),
			  (INT)( limit / MB_SECT ));
	}
	er = ( gp.tsfs ) ? knl_tsfs_part_grow(part, bytes, &from, &to) : E_OBJ;
	sysvol_from = from;
	sysvol_to = to;
	if ( er >= E_OK && to > from ) {
		tm_printf((UB *)"sysvol: grown %d MB -> %d MB\n", (INT)( from >> 20 ), (INT)( to >> 20 ));
	} else if ( why != NULL && er < E_OK && er != E_LIMIT ) {
		/* the partition may not grow, and bringing its volume up to it failed */
		tm_printf((UB *)"sysvol: %d MB, not grown (%s); its volume of %d MB not brought up to it (%d)\n",
			  (INT)( size / MB_SECT ), why, (INT)( from >> 20 ), (INT)er);
	} else if ( why != NULL ) {
		tm_printf((UB *)"sysvol: %d MB, not grown (%s)\n", (INT)( size / MB_SECT ), why);
	} else {
		tm_printf((UB *)"sysvol: %d MB, not grown (%d)\n", (INT)( size / MB_SECT ), (INT)er);
	}
}

EXPORT void knl_sysvol_grown( UD *p_from, UD *p_to )
{
	if ( p_from != NULL ) *p_from = sysvol_from;
	if ( p_to != NULL ) *p_to = sysvol_to;
}
