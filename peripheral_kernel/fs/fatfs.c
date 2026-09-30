/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fatfs.c
 *	FAT file system implementation (fimp), design 12.2.2.
 *
 *	FAT12, FAT16 and FAT32 are handled. Which of them a volume is follows
 *	from its number of clusters alone: fewer than 4085 is FAT12, fewer
 *	than 65525 FAT16, anything more FAT32. SD cards and USB sticks come
 *	in all three, the small ones as FAT12 or FAT16.
 *
 *	The three differ in the width of an entry of the table of clusters
 *	(12, 16 or 32 bits; twelve bit entries share bytes, and one may start
 *	in the last byte of a sector and end in the next) and in the root
 *	directory: on FAT12 and FAT16 it is a fixed area between the tables
 *	and the data with room for a set number of entries, on FAT32 a chain
 *	of clusters like any other directory. Cluster 0 stands for the root
 *	throughout, as it does in the ".." entries on the disk.
 *
 *	Long names (VFAT) are read and written: a name that does not fit the
 *	short 8.3 form is stored in the entries before it as UTF-16, with a
 *	generated short alias. Names are compared without regard to case.
 *
 *	One volume is served at a time through a mutex; the scratch sector
 *	of the volume is used for every directory and table access.
 *
 *	A power cut leaves the volume in a state the next mount puts right.
 *	The table goes to the disk before the directory entries that name
 *	its clusters, an entry is erased before its clusters are freed, and
 *	a rename marks the entry it moves away from and the one it moves to
 *	(fat_rename), so that a volume cut off at any sector is either as
 *	it was or holds what fat_check needs to finish the change or undo
 *	it. The volume carries a mark of being in use from the first change
 *	after it is mounted until it is unmounted (FAT16 and FAT32 in entry
 *	1 of the table, FAT12 in the boot sector); a volume mounted with
 *	the mark still on was not unmounted, and fat_check looks it over.
 *
 *	The free clusters are counted once and then kept count of. FAT32
 *	keeps the count and where to look for a free cluster in its FSInfo
 *	sector, which is read at mount and written back on sync and unmount.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/blk.h>
#include <ts/proc.h>

#define SECTOR_SIZE		BLK_SECTOR_SIZE
#define DIRENT_SIZE		32
#define DIRENT_PER_SECT		(SECTOR_SIZE / DIRENT_SIZE)

#define ATTR_READ_ONLY		0x01
#define ATTR_HIDDEN		0x02
#define ATTR_SYSTEM		0x04
#define ATTR_VOLUME_ID		0x08
#define ATTR_DIRECTORY		0x10
#define ATTR_ARCHIVE		0x20
#define ATTR_LONG_NAME		0x0F

#define FAT16_MIN_CLUS		4085		/* fewer than this is FAT12 */
#define FAT32_MIN_CLUS		65525		/* fewer than this is FAT16 */

#define CLUS_FREE		0
#define CLUS_MASK		0x0FFFFFFF	/* the 28 bits a FAT32 entry uses */

/*
 * Entry 1 of the table: the volume was unmounted cleanly, and no disk
 * error was met. FAT12 has no room for them; its boot sector has a
 * state byte instead, whose lowest bit says the volume is in use.
 */
#define FAT16_CLEAN		0x8000
#define FAT16_NOERR		0x4000
#define FAT32_CLEAN		0x08000000
#define FAT32_NOERR		0x04000000
#define BS_STATE_OFF		37		/* FAT12 and FAT16 */
#define BS_STATE_INUSE		0x01

/* FSInfo (FAT32) */
#define FSI_LEAD_SIG		0x41615252
#define FSI_STRUC_SIG		0x61417272
#define FSI_TRAIL_SIG		0xAA550000
#define FSI_FREE_OFF		488
#define FSI_NEXT_OFF		492
#define FSI_UNKNOWN		0xFFFFFFFF
#define FSI_WRITE_MS		1000		/* written on close at most this often */

/*
 * Sectors kept in memory. A directory is read an entry at a time, and
 * finding the next sector of a directory means reading the table of
 * clusters; with one sector kept, the table's sector and the
 * directory's took turns throwing each other out, and every name looked
 * up read the disk once for each step through the directory. Making a
 * file in a directory of a few hundred names took a quarter of a second
 * that way. Sixty-four sectors hold the table's sectors and the
 * directory's together.
 *
 * What is changed stays here until it is flushed -- when a file is
 * closed, when a directory entry is written, and before any sector is
 * written straight to the disk -- which is when it was written before,
 * with one sector kept.
 */
#define FC_SLOTS		64

/*
 * Directories whose place in their chain is kept (FATVOL.dh), and
 * directories whose names are kept (FATVOL.dx).
 */
#define FAT_DH_SLOTS		4
#define FAT_DX_SLOTS		4

/*
 * A name of a directory, as a hash, and where its group of entries is.
 * A name with a long form is kept twice, long and short, since either
 * finds it.
 */
typedef struct {
	UW	hash;
	UW	first;			/* the first entry of the group */
	UW	last;			/* its short entry */
} DXNAME;

typedef struct {
	UD	dir;			/* the directory's first cluster, 0 for none */
	DXNAME	*nm;
	INT	n, max;
	UW	free_from;		/* no entry before this one is free */
	UD	used;
} DXDIR;

typedef struct {
	UD	sect;			/* ~0 when it holds nothing */
	BOOL	dirty;
	UD	used;			/* when last taken, to choose one to reuse */
	UB	data[SECTOR_SIZE];
} FCSLOT;

typedef struct {
	ID	dd;			/* block device of the partition */
	ID	mtxid;
	UW	fat_bits;		/* 12, 16 or 32 */
	UD	eoc;			/* an entry this or more ends a chain */
	UD	eoc_mark;		/* what is written to end a chain */
	UW	sec_per_clus;
	UD	fat_sect;		/* first sector of FAT 1 */
	UW	num_fats;
	UD	fat_sects;		/* sectors of one FAT */
	UD	root_clus;		/* first cluster of the root directory (FAT32) */
	UD	root_sect;		/* first sector of the fixed root (FAT12/16) */
	UD	root_sects;		/* its sectors; 0 on FAT32 */
	UD	data_sect;		/* first sector of the data area */
	UD	nclus;			/* clusters in the data area */
	UD	free_hint;		/* where to look for a free cluster */
	UD	free_cnt;		/* free clusters, when free_known */
	BOOL	free_known;
	UD	fsi_sect;		/* the FSInfo sector, 0 for none */
	BOOL	fsi_dirty;		/* free_cnt or free_hint moved since it was written */
	UD	fsi_ms;			/* when it was written last */
	BOOL	rdonly;			/* mounted read-only: nothing is written */
	BOOL	inuse;			/* the mark of being in use is on the disk */
	UINT	bs_state;		/* offset of the state byte of the boot sector, 0 for none */
	BOOL	ioerr;			/* a write failed */
	CONST UB *devnm;		/* the device's name, from the mount */
	BOOL	dirty;			/* the sector in buf has been changed */
	UB	*buf;			/* the sector in hand: a slot's data */
	UD	buf_sect;		/* sector held in buf, ~0 when invalid */
	FCSLOT	*fc;			/* FC_SLOTS of them */
	INT	fc_cur;			/* the slot buf is, -1 for none */
	UD	fc_tick;
	BOOL	no_zero;		/* a cluster taken now is written whole: not zeroed */

	/*
	 * Where the last component a path walk resolved sits in its
	 * directory: the first entry of its group (long name entries
	 * included), the short entry, and the directory. Deleting and
	 * renaming need them. Valid while the mutex is held.
	 */
	UD	walk_first_idx;
	UD	walk_last_idx;
	UD	walk_dirclus;

	/*
	 * Where the last few directories were got to: the idx-th cluster
	 * of the directory that starts at 'first'. A directory is read an
	 * entry at a time, and each entry's sector was found by walking
	 * the chain from the directory's first cluster, so that reading a
	 * directory of n clusters took n * n / 2 steps through the table.
	 * With the place kept, going on through it costs one step per
	 * cluster. Every place is forgotten when a chain is cut (a free or
	 * an end mark set), since it may have been on the part cut off.
	 */
	struct {
		UD	first;		/* 0 for none */
		UD	idx;
		UD	clus;
		UD	used;
	}	dh[FAT_DH_SLOTS];
	UD	dh_tick;

	/*
	 * The names of the last few directories looked in (dir_lookup),
	 * made by reading the directory once and kept up to date by every
	 * entry group written or erased. A store keeps a few hundred
	 * objects of several files each in one directory, and every name
	 * looked up, and every name found not to be there, read the whole
	 * directory -- more sectors than are kept, so from the disk each
	 * time. With the names kept, a lookup reads the one group whose
	 * hash matches, and a name that is not there reads nothing. They
	 * are only a copy of what is on the disk: a directory whose chain
	 * is freed drops its names, and the look over at mount drops all.
	 */
	DXDIR	dx[FAT_DX_SLOTS];
	UD	dx_tick;

	/*
	 * The files open on the volume. Each keeps where its directory
	 * entry is, to put the size there after a write; renaming moves
	 * the entry, and the open files that point at it are told where
	 * it went.
	 */
	T_FILE	**ofile;
	INT	nofile;
	INT	maxofile;
} FATVOL;

/*
 * Sectors read or written in one request when they lie one after
 * another on the disk: a file is read and written a run of clusters at
 * a time, not a sector at a time.
 */
#define FAT_RUN_MAX		256

/* open files a volume has room to note at first; more doubles the room */
#define FAT_OFILE_INIT		16

/* ---------------------------------------------------------------- little endian */

LOCAL UH rd16( CONST UB *p )
{
	return (UH)((UW)p[0] | ((UW)p[1] << 8));
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL void wr16( UB *p, UH v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8);
}

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8); p[2] = (UB)(v >> 16); p[3] = (UB)(v >> 24);
}

/* ---------------------------------------------------------------- sector I/O */

LOCAL UD now_ms( void )
{
	SYSTIM	tim;

	if ( tk_get_otm(&tim) < E_OK ) {
		return 0;
	}
	return ((UD)(UW)tim.hi << 32) | tim.lo;
}

#ifdef USE_KTEST
/*
 * A power cut for the tests. Armed on a device with the number of
 * sectors still to be written, it lets that many through and drops
 * every write after them; knl_fat_cut_hit tells that one was dropped.
 * knl_fat_checks counts the looks over a volume at mount.
 */
EXPORT UB	knl_fat_cut_dev[FS_DEVNM_MAX];
EXPORT INT	knl_fat_cut_left = -1;		/* -1: not armed */
EXPORT BOOL	knl_fat_cut_hit = FALSE;
EXPORT UW	knl_fat_checks = 0;

/* How many of the next 'n' sectors of the volume may still be written */
LOCAL UD cut_allow( FATVOL *v, UD n )
{
	INT	i;

	if ( knl_fat_cut_left < 0 || v->devnm == NULL ) {
		return n;
	}
	for ( i = 0; i < FS_DEVNM_MAX; i++ ) {
		if ( knl_fat_cut_dev[i] != v->devnm[i] ) return n;
		if ( v->devnm[i] == 0 ) break;
	}
	if ( (UD)knl_fat_cut_left >= n ) {
		knl_fat_cut_left -= (INT)n;
		return n;
	}
	n = (UD)knl_fat_cut_left;
	knl_fat_cut_left = 0;
	knl_fat_cut_hit = TRUE;

	return n;
}
#endif

LOCAL ER sect_read( FATVOL *v, UD sect, void *buf )
{
	SZ	asize;

	return tk_srea_dev(v->dd, (W)sect, buf, SECTOR_SIZE, &asize);
}

/* 'n' sectors one after another */
LOCAL ER sects_read( FATVOL *v, UD sect, void *buf, UD n )
{
	SZ	asize;

	return tk_srea_dev(v->dd, (W)sect, buf, (SZ)n * SECTOR_SIZE, &asize);
}

LOCAL ER sects_write( FATVOL *v, UD sect, CONST void *buf, UD n )
{
	SZ	asize;
	ER	er;

#ifdef USE_KTEST
	{
		UD	ok = cut_allow(v, n);

		if ( ok < n ) {
			if ( ok > 0 ) {
				tk_swri_dev(v->dd, (W)sect, buf, (SZ)ok * SECTOR_SIZE, &asize);
			}
			return E_IO;
		}
	}
#endif
	er = tk_swri_dev(v->dd, (W)sect, buf, (SZ)n * SECTOR_SIZE, &asize);
	if ( er < E_OK ) {
		v->ioerr = TRUE;
	}
	return er;
}

LOCAL ER sect_write( FATVOL *v, UD sect, CONST void *buf )
{
	return sects_write(v, sect, buf, 1);
}

/* Whether the sector in hand was changed goes with its slot */
LOCAL void fc_park( FATVOL *v )
{
	if ( v->fc_cur >= 0 ) {
		v->fc[v->fc_cur].dirty = v->dirty;
	}
}

/* The slot holding a sector, or -1 */
LOCAL INT fc_find( FATVOL *v, UD sect )
{
	INT	i;

	for ( i = 0; i < FC_SLOTS; i++ ) {
		if ( v->fc[i].sect == sect ) {
			return i;
		}
	}

	return -1;
}

/*
 * A sector about to be written straight to the disk, or known to be
 * changed there, is not kept: what is kept of it would be out of date.
 */
LOCAL void fc_forget( FATVOL *v, UD sect )
{
	INT	i;

	fc_park(v);
	i = fc_find(v, sect);
	if ( i < 0 ) {
		return;
	}
	v->fc[i].sect = ~0ULL;
	v->fc[i].dirty = FALSE;
	if ( i == v->fc_cur ) {
		v->fc_cur = -1;
		v->buf_sect = ~0ULL;
		v->dirty = FALSE;
	}
}

/* Sector in hand: taken from the slots, or read into the one used longest ago */
LOCAL ER buf_load( FATVOL *v, UD sect )
{
	ER	er;
	INT	i, pick = 0;

	if ( v->buf_sect == sect && v->fc_cur >= 0 ) {
		return EX_OK;
	}
	fc_park(v);
	i = fc_find(v, sect);
	if ( i < 0 ) {
		for ( i = 0; i < FC_SLOTS; i++ ) {
			if ( v->fc[i].sect == ~0ULL ) {
				pick = i;
				break;
			}
			if ( v->fc[i].used < v->fc[pick].used ) {
				pick = i;
			}
		}
		i = pick;
		if ( v->fc[i].dirty && v->fc[i].sect != ~0ULL ) {
			er = sect_write(v, v->fc[i].sect, v->fc[i].data);
			if ( er < E_OK ) return EX_IO;
			v->fc[i].dirty = FALSE;
		}
		er = sect_read(v, sect, v->fc[i].data);
		if ( er < E_OK ) {
			v->fc[i].sect = ~0ULL;
			v->fc_cur = -1;
			v->buf_sect = ~0ULL;
			v->dirty = FALSE;
			return EX_IO;
		}
		v->fc[i].sect = sect;
		v->fc[i].dirty = FALSE;
	}
	v->fc[i].used = ++v->fc_tick;
	v->fc_cur = i;
	v->buf = v->fc[i].data;
	v->buf_sect = sect;
	v->dirty = v->fc[i].dirty;

	return EX_OK;
}

LOCAL BOOL sect_in_fat( FATVOL *v, UD sect )
{
	return ( sect >= v->fat_sect && sect < v->fat_sect + (UD)v->num_fats * v->fat_sects );
}

/*
 * Everything changed, written. The sectors of the table go first, so
 * that an entry written with them never names a cluster the table on
 * the disk does not have yet, and from the last to the first: a chain
 * grows towards higher clusters as a rule, so the new end is marked
 * before the entry that leads to it. A cut between them leaves a
 * cluster taken that nothing names, which fat_check frees.
 */
LOCAL ER buf_flush( FATVOL *v )
{
	ER	er;
	INT	i, pick;

	fc_park(v);
	for (;;) {
		pick = -1;
		for ( i = 0; i < FC_SLOTS; i++ ) {
			if ( v->fc[i].dirty && v->fc[i].sect != ~0ULL && sect_in_fat(v, v->fc[i].sect)
			  && ( pick < 0 || v->fc[i].sect > v->fc[pick].sect ) ) {
				pick = i;
			}
		}
		if ( pick < 0 ) break;
		er = sect_write(v, v->fc[pick].sect, v->fc[pick].data);
		if ( er < E_OK ) return EX_IO;
		v->fc[pick].dirty = FALSE;
	}
	for ( i = 0; i < FC_SLOTS; i++ ) {
		if ( v->fc[i].dirty && v->fc[i].sect != ~0ULL ) {
			er = sect_write(v, v->fc[i].sect, v->fc[i].data);
			if ( er < E_OK ) return EX_IO;
			v->fc[i].dirty = FALSE;
		}
	}
	v->dirty = FALSE;
	return EX_OK;
}

/* ---------------------------------------------------------------- FAT table */

LOCAL UD clus_to_sect( FATVOL *v, UD clus )
{
	return v->data_sect + (clus - 2) * v->sec_per_clus;
}

/*
 * An entry this or more ends a chain. The one below the lowest of them
 * marks a bad cluster; like every value past the last cluster, it is
 * not a cluster of the volume, and a chain that meets it ends there.
 */
LOCAL BOOL clus_is_eoc( FATVOL *v, UD c )
{
	return ( c >= v->eoc );
}

LOCAL BOOL clus_valid( FATVOL *v, UD c )
{
	return ( c >= 2 && c < v->nclus + 2 );
}

/*
 * Byte 'off' of copy 'copy' of the table, in the sector in hand. A
 * twelve bit entry is read and written a byte at a time: its two bytes
 * may lie in two sectors.
 */
LOCAL ER fat_byte( FATVOL *v, UW copy, UD off, UB **pp )
{
	ER	er;

	er = buf_load(v, v->fat_sect + (UD)copy * v->fat_sects + off / SECTOR_SIZE);
	if ( er < EX_OK ) return er;
	*pp = v->buf + off % SECTOR_SIZE;

	return EX_OK;
}

LOCAL ER fat_get( FATVOL *v, UD clus, UD *p_val )
{
	UB	*p;
	UW	w;
	ER	er;

	if ( !clus_valid(v, clus) ) {
		return EX_IO;
	}

	switch ( v->fat_bits ) {
	  case 12: {
		UD	off = clus + clus / 2;

		er = fat_byte(v, 0, off, &p);
		if ( er < EX_OK ) return er;
		w = *p;
		er = fat_byte(v, 0, off + 1, &p);
		if ( er < EX_OK ) return er;
		w |= (UW)*p << 8;
		/* an odd cluster has the upper twelve bits of the pair */
		*p_val = ( (clus & 1) != 0 ) ? (w >> 4) : (w & 0x0FFF);
		break;
	  }
	  case 16:
		er = fat_byte(v, 0, clus * 2, &p);
		if ( er < EX_OK ) return er;
		*p_val = rd16(p);
		break;
	  default:
		er = fat_byte(v, 0, clus * 4, &p);
		if ( er < EX_OK ) return er;
		*p_val = rd32(p) & CLUS_MASK;
		break;
	}

	return EX_OK;
}

/* Every place kept in a directory's chain, forgotten */
LOCAL void dh_forget( FATVOL *v )
{
	INT	i;

	for ( i = 0; i < FAT_DH_SLOTS; i++ ) {
		v->dh[i].first = 0;
	}
}

/* The key a directory's names are kept under: the root is its cluster on FAT32 */
LOCAL UD dx_key( FATVOL *v, UD dirclus )
{
	return ( dirclus == 0 && v->root_sects == 0 ) ? v->root_clus : dirclus;
}

/* The names kept of one directory, or of every one (dir 0), dropped */
LOCAL void dx_drop( FATVOL *v, UD dir )
{
	INT	i;

	for ( i = 0; i < FAT_DX_SLOTS; i++ ) {
		if ( v->dx[i].nm != NULL && ( dir == 0 || v->dx[i].dir == dir ) ) {
			Kfree(v->dx[i].nm);
			v->dx[i].nm = NULL;
			v->dx[i].dir = 0;
			v->dx[i].n = 0;
			v->dx[i].max = 0;
		}
	}
}

LOCAL ER fat_set( FATVOL *v, UD clus, UD val )
{
	UB	*p;
	UW	i;
	ER	er;

	if ( !clus_valid(v, clus) ) {
		return EX_IO;
	}

	/* every copy of the table is kept identical */
	for ( i = 0; i < v->num_fats; i++ ) {
		switch ( v->fat_bits ) {
		  case 12: {
			UD	off = clus + clus / 2;

			val &= 0x0FFF;
			/* the half byte shared with the neighbour is kept */
			er = fat_byte(v, i, off, &p);
			if ( er < EX_OK ) return er;
			*p = ( (clus & 1) != 0 ) ? (UB)((*p & 0x0F) | ((val << 4) & 0xF0)) : (UB)val;
			v->dirty = TRUE;
			er = fat_byte(v, i, off + 1, &p);
			if ( er < EX_OK ) return er;
			*p = ( (clus & 1) != 0 ) ? (UB)(val >> 4) : (UB)((*p & 0xF0) | (val >> 8));
			v->dirty = TRUE;
			break;
		  }
		  case 16:
			er = fat_byte(v, i, clus * 2, &p);
			if ( er < EX_OK ) return er;
			wr16(p, (UH)val);
			v->dirty = TRUE;
			break;
		  default:
			er = fat_byte(v, i, clus * 4, &p);
			if ( er < EX_OK ) return er;
			/* the top four bits of an entry are reserved */
			wr32(p, (rd32(p) & ~CLUS_MASK) | (UW)(val & CLUS_MASK));
			v->dirty = TRUE;
			break;
		}
	}
	if ( val == CLUS_FREE || clus_is_eoc(v, val) ) {
		dh_forget(v);			/* a chain was cut, or ends anew */
	}

	return EX_OK;
}

/*
 * Take a free cluster, mark it as the end of a chain and zero its data.
 */
LOCAL ER clus_alloc( FATVOL *v, UD *p_clus )
{
	UD	c, val, start = v->free_hint, n = 0, s;
	UB	*zero;
	ER	er;

	for ( c = start; n < v->nclus; n++ ) {
		if ( c < 2 || c >= v->nclus + 2 ) c = 2;
		er = fat_get(v, c, &val);
		if ( er < EX_OK ) return er;
		if ( val == CLUS_FREE ) break;
		c++;
	}
	if ( n >= v->nclus ) {
		/* the whole table was looked at: the count is known now */
		v->free_cnt = 0;
		v->free_known = TRUE;
		v->fsi_dirty = TRUE;
		return EX_NOSPC;
	}

	er = fat_set(v, c, v->eoc_mark);
	if ( er < EX_OK ) return er;
	v->free_hint = c + 1;
	if ( v->free_known ) {
		/* a count read from FSInfo that says none is left was wrong */
		if ( v->free_cnt == 0 ) v->free_known = FALSE;
		else v->free_cnt--;
	}
	v->fsi_dirty = TRUE;

	zero = (UB *)Kmalloc(SECTOR_SIZE);
	if ( zero == NULL ) return EX_NOMEM;
	knl_memset(zero, 0, SECTOR_SIZE);
	/*
	 * A freed cluster may still have sectors kept from the file it
	 * belonged to, changed and not yet written. They are dropped, not
	 * written: written later, they would land on the new file.
	 */
	er = EX_OK;
	for ( s = 0; s < v->sec_per_clus && er >= EX_OK; s++ ) {
		fc_forget(v, clus_to_sect(v, c) + s);
		if ( !v->no_zero && sect_write(v, clus_to_sect(v, c) + s, zero) < E_OK ) er = EX_IO;
	}
	Kfree(zero);
	if ( er < EX_OK ) return er;

	*p_clus = c;

	return EX_OK;
}

/*
 * Free a whole chain starting at 'clus'
 */
LOCAL ER chain_free( FATVOL *v, UD clus )
{
	UD	next;
	ER	er;

	dx_drop(v, clus);		/* a directory going: its names with it */

	while ( clus_valid(v, clus) ) {
		er = fat_get(v, clus, &next);
		if ( er < EX_OK ) return er;
		if ( next == CLUS_FREE ) break;		/* not part of a chain */
		er = fat_set(v, clus, CLUS_FREE);
		if ( er < EX_OK ) return er;
		if ( clus < v->free_hint ) v->free_hint = clus;
		if ( v->free_known ) v->free_cnt++;
		v->fsi_dirty = TRUE;
		clus = next;
	}

	return EX_OK;
}

/* The free clusters, counted from the table */
LOCAL ER free_count( FATVOL *v )
{
	UD	c, val, n = 0;
	ER	er;

	for ( c = 2; c < v->nclus + 2; c++ ) {
		er = fat_get(v, c, &val);
		if ( er < EX_OK ) return er;
		if ( val == CLUS_FREE ) n++;
	}
	v->free_cnt = n;
	v->free_known = TRUE;
	v->fsi_dirty = TRUE;

	return EX_OK;
}

/*
 * Entry 1 of the table as it is, all its bits (FAT16 and FAT32), and
 * written to every copy of the table.
 */
LOCAL ER fat1_get( FATVOL *v, UW *p_val )
{
	UB	*p;
	ER	er;

	er = fat_byte(v, 0, ( v->fat_bits == 16 ) ? 2 : 4, &p);
	if ( er < EX_OK ) return er;
	*p_val = ( v->fat_bits == 16 ) ? rd16(p) : rd32(p);

	return EX_OK;
}

LOCAL ER fat1_set( FATVOL *v, UW val )
{
	UB	*p;
	UW	i;
	ER	er;

	for ( i = 0; i < v->num_fats; i++ ) {
		er = fat_byte(v, i, ( v->fat_bits == 16 ) ? 2 : 4, &p);
		if ( er < EX_OK ) return er;
		if ( v->fat_bits == 16 ) wr16(p, (UH)val);
		else wr32(p, val);
		v->dirty = TRUE;
	}

	return EX_OK;
}

/*
 * The mark of being in use, put on (inuse TRUE) or taken off, and
 * written at once. Taking it off also says whether a write failed while
 * the volume was mounted (FAT16 and FAT32); a volume on which one did
 * keeps the mark, to be looked over at the next mount. A FAT12 volume
 * without the state byte has nothing to carry the mark.
 */
LOCAL ER vol_mark( FATVOL *v, BOOL inuse )
{
	UW	w, clean, noerr;
	ER	er;

	if ( v->rdonly ) {
		return EX_OK;
	}
	if ( !inuse && v->ioerr ) {
		inuse = TRUE;
	}
	if ( v->fat_bits != 12 ) {
		clean = ( v->fat_bits == 16 ) ? FAT16_CLEAN : FAT32_CLEAN;
		noerr = ( v->fat_bits == 16 ) ? FAT16_NOERR : FAT32_NOERR;
		er = fat1_get(v, &w);
		if ( er < EX_OK ) return er;
		w = ( inuse ) ? ( w & ~clean ) : ( w | clean );
		if ( v->ioerr ) w &= ~noerr;
		er = fat1_set(v, w);
		if ( er < EX_OK ) return er;
	} else if ( v->bs_state != 0 ) {
		er = buf_load(v, 0);
		if ( er < EX_OK ) return er;
		if ( inuse ) v->buf[v->bs_state] |= BS_STATE_INUSE;
		else v->buf[v->bs_state] &= (UB)~BS_STATE_INUSE;
		v->dirty = TRUE;
	}
	er = buf_flush(v);
	if ( er >= EX_OK ) {
		v->inuse = inuse;
	}

	return er;
}

/* Before the first change after the volume was mounted */
LOCAL ER vol_touch( FATVOL *v )
{
	if ( v->inuse || v->rdonly ) {
		return EX_OK;
	}
	return vol_mark(v, TRUE);
}

/*
 * The free count and where to look for a free cluster, written to
 * FSInfo. A count not known is written as unknown.
 */
LOCAL ER fsi_write( FATVOL *v )
{
	UB	*p;
	ER	er;

	if ( v->fsi_sect == 0 || v->rdonly || !v->fsi_dirty ) {
		return EX_OK;
	}
	er = buf_load(v, v->fsi_sect);
	if ( er < EX_OK ) return er;
	p = v->buf;
	if ( rd32(p) != FSI_LEAD_SIG || rd32(p + 484) != FSI_STRUC_SIG ) {
		v->fsi_sect = 0;		/* no longer one: left alone */
		return EX_OK;
	}
	wr32(p + FSI_FREE_OFF, ( v->free_known ) ? (UW)v->free_cnt : FSI_UNKNOWN);
	wr32(p + FSI_NEXT_OFF, clus_valid(v, v->free_hint) ? (UW)v->free_hint : FSI_UNKNOWN);
	v->dirty = TRUE;
	er = buf_flush(v);
	if ( er >= EX_OK ) {
		v->fsi_dirty = FALSE;
		v->fsi_ms = now_ms();
	}

	return er;
}

/*
 * The cluster holding offset 'n' of a chain. When 'grow' is true the chain
 * is extended as needed.
 */
LOCAL ER chain_nth( FATVOL *v, UD first, UD n, BOOL grow, UD *p_clus )
{
	UD	c = first, next;
	ER	er;

	if ( !clus_valid(v, c) ) {
		return EX_IO;
	}
	while ( n > 0 ) {
		er = fat_get(v, c, &next);
		if ( er < EX_OK ) return er;
		if ( clus_is_eoc(v, next) || !clus_valid(v, next) ) {
			if ( !grow ) return EX_NOENT;
			er = clus_alloc(v, &next);
			if ( er < EX_OK ) return er;
			er = fat_set(v, c, next);
			if ( er < EX_OK ) return er;
		}
		c = next;
		n--;
	}
	*p_clus = c;

	return EX_OK;
}

/*
 * The cluster that holds the n-th cluster's worth of a file, carrying
 * on from where that file got to last time.
 *
 * A file read or written from beginning to end asks for cluster 0, then
 * 1, then 2, and walking to each from the first cluster reads the whole
 * table again every time. With the hint, going forward costs one step
 * per cluster however large the file is; going backwards, or to a file
 * that has been wound back, starts from the beginning as before.
 */
LOCAL ER file_clus( FATVOL *v, T_FILE *f, UD n, BOOL grow, UD *p_clus )
{
	UD	from = f->ino, step = n;
	ER	er;

	if ( f->hint_clus != 0 && f->hint_idx <= n
	  && clus_valid(v, f->hint_clus) ) {
		from = f->hint_clus;
		step = n - f->hint_idx;
	}
	er = chain_nth(v, from, step, grow, p_clus);
	if ( er >= EX_OK ) {
		f->hint_idx  = n;
		f->hint_clus = *p_clus;
	}

	return er;
}

/* ---------------------------------------------------------------- directories */

/*
 * The k-th cluster of the directory that starts at 'first', from the
 * place kept for it when that is not past k (FATVOL.dh)
 */
LOCAL ER dir_clus( FATVOL *v, UD first, UD k, BOOL grow, UD *p_clus )
{
	UD	from = first, step = k;
	INT	i, hit = -1, pick = 0;
	ER	er;

	for ( i = 0; i < FAT_DH_SLOTS; i++ ) {
		if ( v->dh[i].first == first ) {
			hit = i;
			break;
		}
		if ( v->dh[i].used < v->dh[pick].used ) {
			pick = i;
		}
	}
	if ( hit >= 0 && v->dh[hit].idx <= k ) {
		from = v->dh[hit].clus;
		step = k - v->dh[hit].idx;
	}
	er = chain_nth(v, from, step, grow, p_clus);
	if ( er < EX_OK ) {
		return er;
	}
	/* taken after the walk: a chain made longer forgets every place */
	if ( hit < 0 || v->dh[hit].first != first ) {
		hit = pick;
	}
	v->dh[hit].first = first;
	v->dh[hit].idx = k;
	v->dh[hit].clus = *p_clus;
	v->dh[hit].used = ++v->dh_tick;

	return EX_OK;
}

/*
 * The n-th sector of a directory. 'dirclus' 0 means the root: on FAT12
 * and FAT16 that is the fixed area, which cannot grow, on FAT32 the
 * chain from root_clus.
 */
LOCAL ER dir_sect( FATVOL *v, UD dirclus, UD n, BOOL grow, UD *p_sect )
{
	UD	clus;
	ER	er;

	if ( dirclus == 0 ) {
		if ( v->root_sects != 0 ) {
			if ( n >= v->root_sects ) {
				return ( grow ) ? EX_NOSPC : EX_NOENT;
			}
			*p_sect = v->root_sect + n;
			return EX_OK;
		}
		dirclus = v->root_clus;		/* 0 stands for the root */
	}
	er = dir_clus(v, dirclus, n / v->sec_per_clus, grow, &clus);
	if ( er < EX_OK ) return er;
	*p_sect = clus_to_sect(v, clus) + (n % v->sec_per_clus);

	return EX_OK;
}

/*
 * A name in the short 8.3 form. Lower case is kept through the two case
 * bits of the entry, as Windows does, so "readme.txt" needs no long name
 * entry; a name whose base or extension mixes cases does, and is refused
 * here. p_ntflags may be NULL.
 */
#define NT_LOWER_BASE		0x08
#define NT_LOWER_EXT		0x10

/*
 * Two more bits of that byte are on only while a rename is under way
 * (fat_rename): the entry being moved away from, and the entry it is
 * moving to. Other bits of the byte are left as they are.
 */
#define NT_REN_OLD		0x40
#define NT_REN_NEW		0x80

LOCAL BOOL short_char( UB c );
LOCAL BOOL alias_taken( FATVOL *v, UD dirclus, CONST UB *name83 );
LOCAL UW name_hash( CONST UB *name, INT len );

LOCAL ER name_to_83( CONST char *name, INT len, UB *out, UB *p_ntflags )
{
	INT	i, j, dot = -1;
	INT	base_lo = 0, base_up = 0, ext_lo = 0, ext_up = 0;
	UB	flags = 0;

	for ( i = 0; i < 11; i++ ) out[i] = ' ';
	if ( p_ntflags != NULL ) *p_ntflags = 0;
	if ( len <= 0 || len > 12 ) {
		return EX_INVAL;
	}
	for ( i = 0; i < len; i++ ) {
		if ( name[i] == '.' ) dot = i;
	}
	if ( dot == 0 ) {
		/* "." and ".." are given as they are */
		if ( len == 1 ) { out[0] = '.'; return EX_OK; }
		if ( len == 2 && name[1] == '.' ) { out[0] = '.'; out[1] = '.'; return EX_OK; }
		return EX_INVAL;
	}
	if ( dot < 0 ) dot = len;
	if ( dot > 8 || len - dot - 1 > 3 ) {
		return EX_INVAL;
	}

	for ( i = 0; i < dot; i++ ) {
		char c = name[i];
		if ( c >= 'a' && c <= 'z' ) { base_lo++; c = (char)(c - 'a' + 'A'); }
		else if ( c >= 'A' && c <= 'Z' ) base_up++;
		if ( !short_char((UB)c) ) return EX_INVAL;
		out[i] = (UB)c;
	}
	for ( j = 0, i = dot + 1; i < len; i++, j++ ) {
		char c = name[i];
		if ( c >= 'a' && c <= 'z' ) { ext_lo++; c = (char)(c - 'a' + 'A'); }
		else if ( c >= 'A' && c <= 'Z' ) ext_up++;
		if ( !short_char((UB)c) ) return EX_INVAL;
		out[8 + j] = (UB)c;
	}

	if ( base_lo > 0 && base_up > 0 ) return EX_INVAL;	/* mixed: needs a long name */
	if ( ext_lo > 0 && ext_up > 0 ) return EX_INVAL;
	if ( base_lo > 0 ) flags |= NT_LOWER_BASE;
	if ( ext_lo > 0 ) flags |= NT_LOWER_EXT;
	if ( p_ntflags != NULL ) *p_ntflags = flags;

	return EX_OK;
}

/*
 * "README  TXT" -> "README.TXT"
 */
LOCAL void name_from_83( CONST UB *e, UB *out )
{
	UB	flags = e[12];
	INT	i, n = 0;

	for ( i = 0; i < 8 && e[i] != ' '; i++ ) {
		UB c = e[i];
		if ( (flags & NT_LOWER_BASE) != 0 && c >= 'A' && c <= 'Z' ) c = (UB)(c - 'A' + 'a');
		out[n++] = c;
	}
	if ( e[8] != ' ' ) {
		out[n++] = '.';
		for ( i = 8; i < 11 && e[i] != ' '; i++ ) {
			UB c = e[i];
			if ( (flags & NT_LOWER_EXT) != 0 && c >= 'A' && c <= 'Z' ) c = (UB)(c - 'A' + 'a');
			out[n++] = c;
		}
	}
	out[n] = 0;
}

LOCAL BOOL name_eq( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 11; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

/* the high half of the first cluster is only FAT32's: it is reserved on the others */
LOCAL UD ent_clus( FATVOL *v, CONST UB *e )
{
	if ( v->fat_bits != 32 ) {
		return (UD)rd16(e + 26);
	}
	return (UD)rd16(e + 26) | ((UD)rd16(e + 20) << 16);
}

LOCAL void ent_set_clus( FATVOL *v, UB *e, UD c )
{
	wr16(e + 26, (UH)c);
	wr16(e + 20, (UH)(c >> 16));
}

LOCAL ER dir_find_83( FATVOL *v, UD dirclus, CONST UB *name83, UD *p_sect, UINT *p_off );
LOCAL void fat_now( UH *p_date, UH *p_time );

/*
 * Long names (VFAT). A long name is held in the entries just before the
 * short one, thirteen UTF-16 units each, in reverse order, every entry
 * carrying a checksum of the short name it belongs to.
 */
#define LFN_CHARS_PER_ENT	13
#define LFN_LAST		0x40
#define LFN_SEQ_MASK		0x3f
#define LFN_MAX_ENT		20		/* 260 characters */

LOCAL UB lfn_checksum( CONST UB *name83 )
{
	UB	sum = 0;
	INT	i;

	for ( i = 0; i < 11; i++ ) {
		sum = (UB)(((sum & 1) << 7) + (sum >> 1) + name83[i]);
	}
	return sum;
}

/* offsets of the thirteen units inside a long name entry */
LOCAL CONST UB lfn_off[LFN_CHARS_PER_ENT] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/*
 * One UTF-8 character to a code point. Returns the bytes used, or 0 for
 * a malformed sequence.
 */
LOCAL INT utf8_get( CONST UB *s, INT len, UW *p_cp )
{
	if ( len <= 0 ) return 0;
	if ( s[0] < 0x80 ) { *p_cp = s[0]; return 1; }
	if ( (s[0] & 0xE0) == 0xC0 && len >= 2 ) {
		*p_cp = ((UW)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		return 2;
	}
	if ( (s[0] & 0xF0) == 0xE0 && len >= 3 ) {
		*p_cp = ((UW)(s[0] & 0x0F) << 12) | ((UW)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		return 3;
	}
	if ( (s[0] & 0xF8) == 0xF0 && len >= 4 ) {
		*p_cp = ((UW)(s[0] & 0x07) << 18) | ((UW)(s[1] & 0x3F) << 12)
		      | ((UW)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		return 4;
	}
	return 0;
}

/* A code point as UTF-8. Returns the bytes written. */
LOCAL INT utf8_put( UB *out, INT room, UW cp )
{
	if ( cp < 0x80 ) {
		if ( room < 1 ) return 0;
		out[0] = (UB)cp;
		return 1;
	}
	if ( cp < 0x800 ) {
		if ( room < 2 ) return 0;
		out[0] = (UB)(0xC0 | (cp >> 6));
		out[1] = (UB)(0x80 | (cp & 0x3F));
		return 2;
	}
	if ( cp < 0x10000 ) {
		if ( room < 3 ) return 0;
		out[0] = (UB)(0xE0 | (cp >> 12));
		out[1] = (UB)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (UB)(0x80 | (cp & 0x3F));
		return 3;
	}
	/* outside the basic plane: from a surrogate pair */
	if ( room < 4 ) return 0;
	out[0] = (UB)(0xF0 | (cp >> 18));
	out[1] = (UB)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (UB)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (UB)(0x80 | (cp & 0x3F));
	return 4;
}

/*
 * UTF-8 to UTF-16. Characters outside the basic plane are written as a
 * surrogate pair. Returns the number of units, or -1 when the name does
 * not fit.
 */
LOCAL INT utf8_to_utf16( CONST UB *name, INT len, UH *out, INT max )
{
	INT	n = 0, i = 0, used;
	UW	cp;

	while ( i < len ) {
		used = utf8_get(name + i, len - i, &cp);
		if ( used == 0 ) return -1;
		i += used;
		if ( cp >= 0x10000 ) {
			if ( n + 2 > max ) return -1;
			cp -= 0x10000;
			out[n++] = (UH)(0xD800 | (cp >> 10));
			out[n++] = (UH)(0xDC00 | (cp & 0x3FF));
		} else {
			if ( n + 1 > max ) return -1;
			out[n++] = (UH)cp;
		}
	}
	return n;
}

LOCAL INT utf16_to_utf8( CONST UH *in, INT n, UB *out, INT max )
{
	INT	i, len = 0, used;
	UW	cp;

	for ( i = 0; i < n; i++ ) {
		cp = in[i];
		if ( cp >= 0xD800 && cp < 0xDC00 && i + 1 < n
		  && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000 ) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			i++;
		}
		used = utf8_put(out + len, max - 1 - len, cp);
		if ( used == 0 ) return -1;
		len += used;
	}
	out[len] = '\0';

	return len;
}

LOCAL UB upcase( UB c )
{
	return ( c >= 'a' && c <= 'z' ) ? (UB)(c - 'a' + 'A') : c;
}

/* case insensitive comparison of two names given as byte strings */
LOCAL BOOL name_eq_ci( CONST UB *a, INT alen, CONST UB *b, INT blen )
{
	INT	i;

	if ( alen != blen ) return FALSE;
	for ( i = 0; i < alen; i++ ) {
		if ( upcase(a[i]) != upcase(b[i]) ) return FALSE;
	}
	return TRUE;
}

/*
 * a character a short name may hold. A blank is not one: the entry pads
 * with blanks, so "fs (2).txt" kept short would read back as "fs.txt".
 */
LOCAL BOOL short_char( UB c )
{
	CONST char *bad = "\"*+,/:;<=>?[]|.";
	INT	i;

	if ( c <= 0x20 || c >= 0x80 ) return FALSE;
	for ( i = 0; bad[i] != '\0'; i++ ) {
		if ( c == (UB)bad[i] ) return FALSE;
	}
	return TRUE;
}

/*
 * Build a short alias for a long name: up to six characters of the stem,
 * "~n", and the first three of the extension. The first four tries
 * count 'n' up; after them two characters of the stem and four hex
 * digits of a hash of the whole name take the stem's place, as names
 * that begin alike -- the UUIDs a store names its files with, made in
 * the same hours -- would otherwise count through every alias taken
 * before them.
 */
LOCAL ER make_alias( FATVOL *v, UD dirclus, CONST UB *name, INT len, UB *out )
{
	static CONST char hex[] = "0123456789ABCDEF";
	UW	h = name_hash(name, len);
	INT	i, j, dot = -1, k, n;

	for ( i = 0; i < len; i++ ) {
		if ( name[i] == '.' ) dot = i;
	}
	if ( dot < 0 ) dot = len;

	for ( k = 1; k < 1000; k++ ) {
		INT	tail, stem;

		n = ( k <= 4 ) ? k : k - 4;
		tail = ( n < 10 ) ? 2 : ( n < 100 ) ? 3 : 4;
		stem = ( k <= 4 ) ? 8 - tail : 2;

		for ( i = 0; i < 11; i++ ) out[i] = ' ';

		for ( i = 0, j = 0; i < dot && j < stem; i++ ) {
			UB c = upcase(name[i]);
			if ( !short_char(c) ) c = '_';
			out[j++] = c;
		}
		if ( k > 4 ) {
			INT	room = 8 - tail - j;

			for ( i = 0; i < room && i < 4; i++ ) {
				out[j++] = (UB)hex[( h >> ( 12 - 4 * i ) ) & 0xF];
			}
		}
		out[j++] = '~';
		if ( n >= 100 ) out[j++] = (UB)('0' + (n / 100) % 10);
		if ( n >= 10 )  out[j++] = (UB)('0' + (n / 10) % 10);
		out[j++] = (UB)('0' + n % 10);

		for ( i = dot + 1, j = 0; i < len && j < 3; i++, j++ ) {
			UB c = upcase(name[i]);
			if ( !short_char(c) ) c = '_';
			out[8 + j] = c;
		}

		if ( !alias_taken(v, dirclus, out) ) {
			return EX_OK;
		}
	}

	return EX_EXIST;
}

/*
 * Find a short name in the directory. On success the entry's sector and
 * offset are returned and the scratch buffer holds that sector.
 */
LOCAL ER dir_find_83( FATVOL *v, UD dirclus, CONST UB *name83, UD *p_sect, UINT *p_off )
{
	UD	n, sect;
	UINT	i;
	ER	er;

	for ( n = 0; ; n++ ) {
		er = dir_sect(v, dirclus, n, FALSE, &sect);
		if ( er < EX_OK ) return EX_NOENT;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;

		for ( i = 0; i < DIRENT_PER_SECT; i++ ) {
			UB *e = v->buf + i * DIRENT_SIZE;

			if ( e[0] == 0x00 ) return EX_NOENT;		/* end of directory */
			if ( e[0] == 0xE5 ) continue;			/* deleted */
			if ( (e[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME ) continue;
			if ( (e[11] & ATTR_VOLUME_ID) != 0 ) continue;
			if ( name_eq(e, name83) ) {
				*p_sect = sect;
				*p_off  = i * DIRENT_SIZE;
				return EX_OK;
			}
		}
	}
}

/*
 * Walk a directory entry by entry, assembling long names.
 *	'idx' is the entry index to start at and is advanced past the entry
 *	returned. The short entry is left in the scratch buffer, with its
 *	sector in *p_sect and offset in *p_off; 'name' receives the name as
 *	UTF-8, long when the entry has one. 'first_idx' receives the index
 *	of the first entry of the group, so that a whole group can be
 *	erased.
 */
LOCAL ER dir_next( FATVOL *v, UD dirclus, UD *p_idx, UB *name, INT namemax,
		   UD *p_sect, UINT *p_off, UD *p_first_idx )
{
	UH	uni[LFN_MAX_ENT * LFN_CHARS_PER_ENT + 1];
	INT	unilen = 0;
	UB	want_sum = 0;
	BOOL	have_lfn = FALSE;
	UD	first = *p_idx;
	ER	er;

	for (;;) {
		UD	idx = *p_idx, sect;
		UINT	off = (UINT)(idx % DIRENT_PER_SECT) * DIRENT_SIZE;
		UB	*e;

		er = dir_sect(v, dirclus, idx / DIRENT_PER_SECT, FALSE, &sect);
		if ( er < EX_OK ) return EX_NOENT;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		e = v->buf + off;
		(*p_idx)++;

		if ( e[0] == 0x00 ) {
			return EX_NOENT;			/* end of directory */
		}
		if ( e[0] == 0xE5 ) {
			have_lfn = FALSE;
			unilen = 0;
			first = *p_idx;
			continue;
		}
		if ( (e[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME ) {
			INT	seq = e[0] & LFN_SEQ_MASK;
			INT	k, base;

			if ( (e[0] & LFN_LAST) != 0 ) {
				have_lfn = TRUE;
				want_sum = e[13];
				unilen = seq * LFN_CHARS_PER_ENT;
				if ( seq > LFN_MAX_ENT || unilen > (INT)(sizeof(uni) / sizeof(uni[0])) - 1 ) {
					have_lfn = FALSE;
				}
				first = *p_idx - 1;
			}
			if ( have_lfn && seq >= 1 && seq <= LFN_MAX_ENT ) {
				base = (seq - 1) * LFN_CHARS_PER_ENT;
				for ( k = 0; k < LFN_CHARS_PER_ENT; k++ ) {
					uni[base + k] = rd16(e + lfn_off[k]);
				}
			}
			continue;
		}
		if ( (e[11] & ATTR_VOLUME_ID) != 0 ) {
			have_lfn = FALSE;
			unilen = 0;
			first = *p_idx;
			continue;
		}

		/* a short entry: it closes the group */
		if ( have_lfn && lfn_checksum(e) == want_sum ) {
			INT	k;

			for ( k = 0; k < unilen; k++ ) {
				if ( uni[k] == 0x0000 || uni[k] == 0xFFFF ) { unilen = k; break; }
			}
			if ( utf16_to_utf8(uni, unilen, name, namemax) < 0 ) {
				name_from_83(e, name);
			}
		} else {
			name_from_83(e, name);
			first = *p_idx - 1;
		}
		*p_sect = sect;
		*p_off  = off;
		if ( p_first_idx != NULL ) *p_first_idx = first;

		return EX_OK;
	}
}

/* A name's hash, without regard to case as names are compared */
LOCAL UW name_hash( CONST UB *name, INT len )
{
	UW	h = 2166136261U;
	INT	i;

	for ( i = 0; i < len; i++ ) {
		h = ( h ^ upcase(name[i]) ) * 16777619U;
	}
	return h;
}

LOCAL INT str_len( CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != '\0' ) n++;
	return n;
}

/* One name more in a directory's names; FALSE when there is no room for it */
LOCAL BOOL dx_put( DXDIR *x, CONST UB *name, INT len, UD first, UD last )
{
	if ( x->n >= x->max ) {
		INT	max = ( x->max > 0 ) ? x->max * 2 : 64;
		DXNAME	*nm = (DXNAME *)Kmalloc(sizeof(DXNAME) * (UINT)max);

		if ( nm == NULL ) {
			return FALSE;
		}
		if ( x->nm != NULL ) {
			knl_memcpy(nm, x->nm, (INT)sizeof(DXNAME) * x->n);
			Kfree(x->nm);
		}
		x->nm = nm;
		x->max = max;
	}
	x->nm[x->n].hash = name_hash(name, len);
	x->nm[x->n].first = (UW)first;
	x->nm[x->n].last = (UW)last;
	x->n++;
	return TRUE;
}

/* A group's names: the long one, and the short one when it differs */
LOCAL BOOL dx_put_group( DXDIR *x, CONST UB *lname, INT llen, CONST UB *ent, UD first, UD last )
{
	UB	sname[13];
	INT	slen;

	name_from_83(ent, sname);
	slen = str_len(sname);
	if ( !dx_put(x, lname, llen, first, last) ) {
		return FALSE;
	}
	if ( slen != llen || !name_eq_ci(sname, slen, lname, llen) ) {
		return dx_put(x, sname, slen, first, last);
	}
	return TRUE;
}

/* The names kept of a directory, or NULL */
LOCAL DXDIR *dx_find( FATVOL *v, UD dirclus )
{
	UD	key = dx_key(v, dirclus);
	INT	i;

	for ( i = 0; i < FAT_DX_SLOTS; i++ ) {
		if ( v->dx[i].nm != NULL && v->dx[i].dir == key ) {
			return &v->dx[i];
		}
	}
	return NULL;
}

/*
 * The names of a directory: those kept, or read now into the slot used
 * longest ago. NULL when they cannot be had; the caller then reads the
 * directory itself.
 */
LOCAL DXDIR *dx_get( FATVOL *v, UD dirclus )
{
	UB	*name;
	DXDIR	*x = dx_find(v, dirclus);
	UD	idx = 0, sect, first;
	UINT	off;
	INT	i, pick = 0;
	ER	er;

	if ( x != NULL ) {
		x->used = ++v->dx_tick;
		return x;
	}
	for ( i = 0; i < FAT_DX_SLOTS; i++ ) {
		if ( v->dx[i].nm == NULL ) {
			pick = i;
			break;
		}
		if ( v->dx[i].used < v->dx[pick].used ) {
			pick = i;
		}
	}
	x = &v->dx[pick];
	if ( x->nm != NULL ) {
		dx_drop(v, x->dir);
	}
	name = (UB *)Kmalloc(FS_NAME_MAX);
	if ( name == NULL ) {
		return NULL;
	}
	x->dir = dx_key(v, dirclus);
	x->nm = NULL;
	x->n = 0;
	x->max = 0;
	x->free_from = 0;
	for (;;) {
		er = dir_next(v, dirclus, &idx, name, FS_NAME_MAX, &sect, &off, &first);
		if ( er == EX_NOENT ) {
			break;			/* read to the end */
		}
		if ( er < EX_OK || idx > 0xFFFF
		  || !dx_put_group(x, name, str_len(name), v->buf + off, first, idx - 1) ) {
			Kfree(name);
			if ( x->nm != NULL ) Kfree(x->nm);
			x->nm = NULL;
			x->dir = 0;
			return NULL;
		}
	}
	Kfree(name);
	if ( x->nm == NULL ) {			/* an empty directory: room for the first */
		x->nm = (DXNAME *)Kmalloc(sizeof(DXNAME) * 8);
		if ( x->nm == NULL ) {
			x->dir = 0;
			return NULL;
		}
		x->max = 8;
	}
	x->used = ++v->dx_tick;
	return x;
}

/*
 * Find a name, long or short, in a directory. Only the groups whose
 * hash is the name's are read, when the directory's names are kept.
 */
LOCAL ER dir_lookup( FATVOL *v, UD dirclus, CONST UB *name, INT len,
		     UD *p_sect, UINT *p_off, UD *p_first_idx, UD *p_last_idx )
{
	UB	found[FS_NAME_MAX];
	DXDIR	*x;
	UD	idx = 0;
	UW	h = name_hash(name, len);
	INT	i = 0, flen;
	ER	er = EX_NOENT;

	x = dx_get(v, dirclus);
	for (;;) {
		if ( x != NULL ) {
			while ( i < x->n && x->nm[i].hash != h ) i++;
			if ( i >= x->n ) {
				er = EX_NOENT;
				break;
			}
			idx = x->nm[i++].first;
		}
		er = dir_next(v, dirclus, &idx, found, FS_NAME_MAX, p_sect, p_off, p_first_idx);
		if ( er < EX_OK ) {
			if ( x != NULL && er == EX_NOENT ) continue;
			break;
		}
		flen = str_len(found);
		if ( !name_eq_ci(found, flen, name, len) ) {
			/* an entry with a long name answers to its short alias too */
			name_from_83(v->buf + *p_off, found);
			flen = str_len(found);
			if ( !name_eq_ci(found, flen, name, len) ) continue;
		}
		if ( p_last_idx != NULL ) *p_last_idx = idx - 1;
		er = buf_load(v, *p_sect);	/* the caller reads the entry */
		break;
	}
	return er;
}

/* Whether a short name is already in the directory */
LOCAL BOOL alias_taken( FATVOL *v, UD dirclus, CONST UB *name83 )
{
	UB	ent[DIRENT_SIZE], sname[13];
	DXDIR	*x = dx_get(v, dirclus);
	UD	sect;
	UINT	off;
	UW	h;
	INT	i;

	if ( x == NULL ) {
		return ( dir_find_83(v, dirclus, name83, &sect, &off) != EX_NOENT );
	}
	knl_memset(ent, 0, sizeof(ent));
	knl_memcpy(ent, name83, 11);
	name_from_83(ent, sname);
	h = name_hash(sname, str_len(sname));
	for ( i = 0; i < x->n; i++ ) {
		if ( x->nm[i].hash != h ) continue;
		if ( dir_sect(v, dirclus, x->nm[i].last / DIRENT_PER_SECT, FALSE, &sect) < EX_OK
		  || buf_load(v, sect) < EX_OK ) {
			return TRUE;		/* not known to be free */
		}
		if ( name_eq(v->buf + ( x->nm[i].last % DIRENT_PER_SECT ) * DIRENT_SIZE, name83) ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL ER dir_alloc_ent_n( FATVOL *v, UD dirclus, INT want, UD *p_idx )
{
	DXDIR	*x = dx_find(v, dirclus);
	UD	idx = 0, run_start = 0, sect;
	INT	run = 0;
	BOOL	seen = FALSE;
	ER	er;

	if ( x != NULL ) {
		idx = x->free_from;
	}
	for (;;) {
		UINT	off = (UINT)(idx % DIRENT_PER_SECT) * DIRENT_SIZE;
		UB	*e;

		er = dir_sect(v, dirclus, idx / DIRENT_PER_SECT, TRUE, &sect);
		if ( er < EX_OK ) return er;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		e = v->buf + off;

		if ( e[0] == 0x00 || e[0] == 0xE5 ) {
			if ( run == 0 ) run_start = idx;
			if ( !seen && x != NULL ) {
				x->free_from = (UW)idx;	/* the first free one */
				seen = TRUE;
			}
			run++;
			if ( run >= want ) {
				*p_idx = run_start;
				if ( x != NULL && x->free_from == run_start ) {
					x->free_from = (UW)( run_start + (UD)want );
				}
				return EX_OK;
			}
		} else {
			run = 0;
		}
		idx++;
	}
}

/*
 * Write an entry group: the long name entries followed by the short one.
 *	'tmpl', when not NULL, is an entry whose times the new one takes;
 *	'xnt' is added to its case bits. The long name entries are on the
 *	disk before the short one when the group spans sectors: a cut in
 *	between leaves entries that belong to nothing, never a short entry
 *	without its long name.
 */
LOCAL ER dir_create( FATVOL *v, UD dirclus, CONST UB *name, INT len, UB attr,
		     UD clus, UD size, CONST UB *tmpl, UB xnt, UD *p_sect, UINT *p_off )
{
	UH	uni[LFN_MAX_ENT * LFN_CHARS_PER_ENT + 1];
	UB	name83[11];
	UD	idx, sect, first_sect = ~0ULL;
	UINT	off;
	INT	unilen, nent, i, k;
	UB	sum, ntflags = 0;
	UH	date, time;
	ER	er;

	er = name_to_83((CONST char *)name, len, name83, &ntflags);
	if ( er >= EX_OK ) {
		nent = 0;			/* the short form is enough */
		unilen = 0;
	} else {
		unilen = utf8_to_utf16(name, len, uni, LFN_MAX_ENT * LFN_CHARS_PER_ENT);
		if ( unilen <= 0 ) {
			return EX_NAMETOOLONG;
		}
		er = make_alias(v, dirclus, name, len, name83);
		if ( er < EX_OK ) return er;
		nent = (unilen + LFN_CHARS_PER_ENT - 1) / LFN_CHARS_PER_ENT;

		/* pad the last entry: a NUL then 0xFFFF */
		for ( i = unilen; i < nent * LFN_CHARS_PER_ENT; i++ ) {
			uni[i] = ( i == unilen ) ? 0x0000 : 0xFFFF;
		}
	}

	er = dir_alloc_ent_n(v, dirclus, nent + 1, &idx);
	if ( er < EX_OK ) return er;

	sum = lfn_checksum(name83);
	fat_now(&date, &time);

	for ( i = nent; i >= 1; i-- ) {
		UD	e_idx = idx + (UD)(nent - i);

		er = dir_sect(v, dirclus, e_idx / DIRENT_PER_SECT, FALSE, &sect);
		if ( er < EX_OK ) return er;
		if ( i == nent ) first_sect = sect;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		off = (UINT)(e_idx % DIRENT_PER_SECT) * DIRENT_SIZE;
		{
			UB *e = v->buf + off;

			knl_memset(e, 0, DIRENT_SIZE);
			e[0]  = (UB)(( i == nent ) ? (i | LFN_LAST) : i);
			e[11] = ATTR_LONG_NAME;
			e[13] = sum;
			for ( k = 0; k < LFN_CHARS_PER_ENT; k++ ) {
				wr16(e + lfn_off[k], uni[(i - 1) * LFN_CHARS_PER_ENT + k]);
			}
		}
		v->dirty = TRUE;
	}

	{
		UD	e_idx = idx + (UD)nent;

		er = dir_sect(v, dirclus, e_idx / DIRENT_PER_SECT, FALSE, &sect);
		if ( er < EX_OK ) return er;
		if ( first_sect != ~0ULL && first_sect != sect ) {
			er = buf_flush(v);
			if ( er < EX_OK ) return er;
		}
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		off = (UINT)(e_idx % DIRENT_PER_SECT) * DIRENT_SIZE;
		{
			UB *e = v->buf + off;

			knl_memset(e, 0, DIRENT_SIZE);
			knl_memcpy(e, name83, 11);
			e[11] = attr;
			e[12] = (UB)(ntflags | xnt);	/* lower case base and extension */
			wr16(e + 14, time); wr16(e + 16, date);
			wr16(e + 22, time); wr16(e + 24, date);
			if ( tmpl != NULL ) {
				knl_memcpy(e + 13, tmpl + 13, 7);	/* creation time and date, access date */
				knl_memcpy(e + 22, tmpl + 22, 4);	/* time and date of the last write */
			}
			ent_set_clus(v, e, clus);
			wr32(e + 28, (UW)size);
		}
		v->dirty = TRUE;
	}

	*p_sect = sect;
	*p_off  = off;

	{
		DXDIR	*x = dx_find(v, dirclus);

		if ( x != NULL && ( idx + (UD)nent > 0xFFFF
				    || !dx_put_group(x, name, len, v->buf + off, idx, idx + (UD)nent) ) ) {
			dx_drop(v, x->dir);	/* no room to keep it: read it again next time */
		}
	}

	return buf_flush(v);
}

/*
 * Erase an entry group: every entry from first_idx up to and including
 * the short entry.
 */
LOCAL ER dir_erase( FATVOL *v, UD dirclus, UD first_idx, UD last_idx )
{
	DXDIR	*x = dx_find(v, dirclus);
	UD	idx, sect;
	INT	i;
	ER	er;

	if ( x != NULL ) {
		for ( i = 0; i < x->n; ) {
			if ( x->nm[i].last >= first_idx && x->nm[i].last <= last_idx ) {
				x->nm[i] = x->nm[--x->n];
			} else {
				i++;
			}
		}
		if ( first_idx < x->free_from ) {
			x->free_from = (UW)first_idx;
		}
	}

	for ( idx = first_idx; idx <= last_idx; idx++ ) {
		er = dir_sect(v, dirclus, idx / DIRENT_PER_SECT, FALSE, &sect);
		if ( er < EX_OK ) return er;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		v->buf[(idx % DIRENT_PER_SECT) * DIRENT_SIZE] = 0xE5;
		v->dirty = TRUE;
	}

	return buf_flush(v);
}

/*
 * A time as a FAT date and time pair: seconds since 1985-01-01 UTC, to
 * the civil date. FAT keeps the years 1980 to 2107 and seconds in twos;
 * FALSE for a time it cannot hold.
 */
LOCAL BOOL fat_of_tron( UD t, UH *p_date, UH *p_time )
{
	UD	sec, days, secs;
	D	era, doe, yoe, y, doy, mp, d, mo;

	sec = t + 473385600ULL;			/* to seconds since 1970-01-01 */
	days = sec / 86400;
	secs = sec % 86400;

	/* days since 1970-01-01 to a civil date */
	days += 719468;
	era = (D)days / 146097;
	doe = (D)days - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y   = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp  = (5 * doy + 2) / 153;
	d   = doy - (153 * mp + 2) / 5 + 1;
	mo  = mp + ( mp < 10 ? 3 : -9 );
	if ( mo <= 2 ) y++;

	if ( y < 1980 || y > 2107 ) {
		return FALSE;
	}
	*p_date = (UH)(((y - 1980) << 9) | (mo << 5) | d);
	*p_time = (UH)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
	return TRUE;
}

/*
 * Current time as a FAT date and time pair (the system time is
 * milliseconds since 1985-01-01 UTC).
 */
LOCAL void fat_now( UH *p_date, UH *p_time )
{
	SYSTIM	tim;

	*p_date = (UH)((2026 - 1980) << 9 | (1 << 5) | 1);	/* fallback */
	*p_time = 0;
	if ( tk_get_tim(&tim) < E_OK ) {
		return;
	}
	(void)fat_of_tron((((UD)(UW)tim.hi << 32) | tim.lo) / 1000, p_date, p_time);
}

/*
 * A FAT date and time pair as seconds since 1985-01-01 UTC. FAT keeps
 * dates from 1980; the years before the origin of the system time give
 * zero.
 */
LOCAL UD fat_to_tron( UH date, UH time )
{
	UD	y = 1980 + (date >> 9), m = (date >> 5) & 0xf, d = date & 0x1f;
	D	era, yoe, doy, doe, days;

	if ( m < 1 || m > 12 || d < 1 ) {
		return 0;
	}
	y -= ( m <= 2 ) ? 1 : 0;
	era = (D)y / 400;
	yoe = (D)y - era * 400;
	doy = (153 * (m + ( m > 2 ? -3 : 9 )) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = era * 146097 + doe - 724947;	/* 724947: 1985-01-01 counted from 0000-03-01 */
	if ( days < 0 ) {
		return 0;
	}

	return (UD)days * 86400 + ((time >> 11) * 3600) + (((time >> 5) & 0x3f) * 60)
	     + ((time & 0x1f) * 2);
}

/* ---------------------------------------------------------------- path walk */

/*
 * Walk a path to its entry. On success *p_clus is the first cluster,
 * *p_size the size and *p_attr the attributes; for the root itself the
 * cluster is 0 and the attribute ATTR_DIRECTORY.
 *	When p_parent is not NULL the walk stops at the last component and
 *	returns the parent's cluster plus a pointer to the last name.
 *	Where the last entry found sits is left in walk_first_idx,
 *	walk_last_idx and walk_dirclus of the volume.
 */
LOCAL ER path_walk( FATVOL *v, CONST char *path, UD *p_clus, UD *p_size, UINT *p_attr,
		    UD *p_sect, UINT *p_off, UD *p_parent, CONST char **p_last )
{
	UD	dirclus = 0, sect;
	UINT	off;
	CONST char *p = path, *comp;
	INT	len;
	ER	er;

	if ( p[0] != '/' ) {
		return EX_INVAL;
	}
	*p_clus = 0;
	*p_size = 0;
	*p_attr = ATTR_DIRECTORY;
	if ( p_sect != NULL ) { *p_sect = 0; *p_off = 0; }
	if ( p_parent != NULL ) { *p_parent = 0; *p_last = NULL; }

	for (;;) {
		while ( *p == '/' ) p++;
		if ( *p == '\0' ) {
			if ( p_parent != NULL && *p_last == NULL ) {
				return EX_INVAL;		/* the root has no name */
			}
			return EX_OK;
		}
		comp = p;
		while ( *p != '\0' && *p != '/' ) p++;
		len = (INT)(p - comp);

		/* trailing component and the caller wants the parent */
		if ( p_parent != NULL ) {
			CONST char *q = p;
			while ( *q == '/' ) q++;
			if ( *q == '\0' ) {
				*p_parent = dirclus;
				*p_last = comp;
				return EX_OK;
			}
		}

		if ( *p_attr != ATTR_DIRECTORY && !((*p_attr & ATTR_DIRECTORY) != 0) ) {
			return EX_NOTDIR;
		}
		v->walk_dirclus = dirclus;
		er = dir_lookup(v, dirclus, (CONST UB *)comp, len, &sect, &off,
				&v->walk_first_idx, &v->walk_last_idx);
		if ( er < EX_OK ) return er;

		{
			UB *e = v->buf + off;

			*p_attr = e[11];
			*p_size = rd32(e + 28);
			*p_clus = ent_clus(v, e);
			if ( p_sect != NULL ) { *p_sect = sect; *p_off = off; }
		}
		if ( (*p_attr & ATTR_DIRECTORY) == 0 && *p != '\0' ) {
			return EX_NOTDIR;
		}
		dirclus = *p_clus;
	}
}

/* ---------------------------------------------------------------- the look over at mount */

/*
 * fat_check: a volume that was not unmounted is looked over before it
 * is used, in this order.
 *
 *   1. A copy of the table that differs from the first is made the same
 *      as the first. buf_flush writes the first copy last, so a change
 *      in the first is in every copy, and a change only in the others
 *      is one whose directory entry was not written yet.
 *   2. The rename marks. When an entry marked as the one moved to is
 *      found, the rename reached the disk: the group marked as the one
 *      moved from is erased and the mark taken off. When none is, the
 *      mark on the old entry is taken off and the rename is undone.
 *   3. Every entry against its chain: a chain that meets a free, bad,
 *      out of range or already taken cluster ends before it; a file
 *      whose chain is shorter than its size is cut to the chain, one
 *      whose chain is longer loses the rest; a directory without a
 *      usable first cluster is erased; "." and ".." name the directory
 *      and its parent; long name entries that belong to no short entry
 *      are erased.
 *   4. Clusters taken in the table that no entry has are freed, and the
 *      free clusters counted.
 *
 * The walk goes CHK_DEPTH directories deep and through CHK_DIRS of
 * them at most, and reads a directory up to its 65536th entry. When it
 * does not reach every directory the marks are left and nothing is
 * taken for lost; so also when a bit a cluster cannot be had for
 * step 3. The free count is always made.
 */
#define CHK_DEPTH		64
#define CHK_DIRS		65536
#define CHK_MARKS		8
#define CHK_RUN			16		/* sectors of the table compared at a time */
#define DIR_MAX_ENT		65536

typedef struct {
	UD	dirclus;		/* 0: the root */
	UD	first, last;		/* the group: long name entries and the short one */
	UB	bits;
} CHKMARK;

typedef struct {
	UD	clus;			/* the directory, 0 for the root */
	UD	parent;
	UD	idx;			/* the next entry to look at */
} CHKLV;

typedef struct {
	UB	*own;			/* a bit a cluster: some entry has it */
	BOOL	fix;			/* step 3: entries and chains are put right */
	BOOL	dry;			/* with fix: counted in 'bad', not put right */
	UD	bad;
	UD	nfiles;
	BOOL	partial;		/* some directory was not looked at */
	UD	ndirs;
	INT	nmark;
	CHKMARK	mark[CHK_MARKS];
	CHKLV	lv[CHK_DEPTH];
} CHK;

LOCAL BOOL own_get( CHK *ck, UD c )
{
	return ( (ck->own[c >> 3] & (1 << (c & 7))) != 0 );
}

LOCAL void own_set( CHK *ck, UD c )
{
	ck->own[c >> 3] |= (UB)(1 << (c & 7));
}

/* Step 1 */
LOCAL ER chk_copies( FATVOL *v )
{
	UB	*a, *b;
	UD	s, n, k, base;
	UW	i;
	ER	er = EX_OK;

	if ( v->num_fats < 2 ) {
		return EX_OK;
	}
	a = (UB *)Kmalloc(CHK_RUN * SECTOR_SIZE);
	b = (UB *)Kmalloc(CHK_RUN * SECTOR_SIZE);
	if ( a == NULL || b == NULL ) {
		if ( a != NULL ) Kfree(a);
		if ( b != NULL ) Kfree(b);
		return EX_OK;
	}
	for ( s = 0; s < v->fat_sects && er >= EX_OK; s += n ) {
		n = v->fat_sects - s;
		if ( n > CHK_RUN ) n = CHK_RUN;
		if ( sects_read(v, v->fat_sect + s, a, n) < E_OK ) { er = EX_IO; break; }
		for ( i = 1; i < v->num_fats && er >= EX_OK; i++ ) {
			base = v->fat_sect + (UD)i * v->fat_sects + s;
			if ( sects_read(v, base, b, n) < E_OK ) { er = EX_IO; break; }
			for ( k = 0; k < n * SECTOR_SIZE && a[k] == b[k]; k++ ) ;
			if ( k < n * SECTOR_SIZE && sects_write(v, base, a, n) < E_OK ) er = EX_IO;
		}
	}
	Kfree(a);
	Kfree(b);

	return er;
}

/* A cluster a chain may go on to: of the volume, taken, not bad, and no other chain's */
LOCAL ER chk_usable( FATVOL *v, CHK *ck, UD c, BOOL *p_ok )
{
	UD	val;
	ER	er;

	*p_ok = FALSE;
	if ( !clus_valid(v, c) || own_get(ck, c) ) {
		return EX_OK;
	}
	er = fat_get(v, c, &val);
	if ( er < EX_OK ) return er;
	*p_ok = (BOOL)( val != CLUS_FREE && val != v->eoc - 1 );

	return EX_OK;
}

/*
 * The chain from 'c', its clusters owned. It ends before a cluster
 * that is not usable. *p_n is the clusters kept, 0 when the first one
 * is not usable.
 */
LOCAL ER chk_chain( FATVOL *v, CHK *ck, UD c, UD *p_n )
{
	UD	n = 0, next;
	BOOL	ok;
	ER	er;

	*p_n = 0;
	er = chk_usable(v, ck, c, &ok);
	if ( er < EX_OK || !ok ) return er;
	for (;;) {
		own_set(ck, c);
		n++;
		er = fat_get(v, c, &next);
		if ( er < EX_OK ) return er;
		if ( clus_is_eoc(v, next) ) break;
		er = chk_usable(v, ck, next, &ok);
		if ( er < EX_OK ) return er;
		if ( !ok ) {
			if ( ck->dry ) {
				ck->bad++;
				break;
			}
			er = fat_set(v, c, v->eoc_mark);
			if ( er < EX_OK ) return er;
			break;
		}
		c = next;
	}
	*p_n = n;

	return EX_OK;
}

/* A file's entry at (sect, off) and its chain made to agree */
LOCAL ER chk_file( FATVOL *v, CHK *ck, UD sect, UINT off )
{
	UD	c, size, n = 0, need, last, next, csize = (UD)v->sec_per_clus * SECTOR_SIZE;
	UB	*e;
	ER	er;

	er = buf_load(v, sect);
	if ( er < EX_OK ) return er;
	e = v->buf + off;
	c = ent_clus(v, e);
	size = rd32(e + 28);
	if ( c != 0 ) {
		er = chk_chain(v, ck, c, &n);
		if ( er < EX_OK ) return er;
	}
	need = ( size + csize - 1 ) / csize;
	ck->nfiles++;
	if ( ck->dry ) {
		if ( n != need || ( c != 0 && n == 0 ) ) {
			ck->bad++;
		}
		return EX_OK;
	}
	if ( n > need ) {
		if ( need == 0 ) {
			er = chain_free(v, c);
			n = 0;
		} else {
			er = chain_nth(v, c, need - 1, FALSE, &last);
			if ( er >= EX_OK ) er = fat_get(v, last, &next);
			if ( er >= EX_OK ) er = chain_free(v, next);
			if ( er >= EX_OK ) er = fat_set(v, last, v->eoc_mark);
		}
		if ( er < EX_OK ) return er;
	} else if ( n < need ) {
		size = n * csize;
	}
	if ( n == 0 ) {
		c = 0;
	}

	er = buf_load(v, sect);
	if ( er < EX_OK ) return er;
	e = v->buf + off;
	if ( ent_clus(v, e) != c || rd32(e + 28) != size ) {
		ent_set_clus(v, e, c);
		wr32(e + 28, (UW)size);
		v->dirty = TRUE;
	}

	return EX_OK;
}

LOCAL BOOL dot_ent( CONST UB *e, INT n )
{
	INT	i;

	for ( i = 0; i < 11; i++ ) {
		if ( e[i] != ( ( i < n ) ? '.' : ' ' ) ) return FALSE;
	}
	return TRUE;
}

/*
 * Walk every directory from the root. Without ck->fix the rename marks
 * are noted and nothing is changed (step 2); with it, step 3.
 */
LOCAL ER chk_walk( FATVOL *v, CHK *ck )
{
	CHKLV	*L;
	UD	idx, sect, grp, c, n, lfn_first = 0;
	INT	d = 0, k, lfn_want = -1;	/* sequence number looked for next; -1: no group */
	UB	lfn_sum = 0, *e;
	UINT	off = 0;
	BOOL	end;
	ER	er;

	ck->lv[0].clus = 0;
	ck->lv[0].parent = 0;
	ck->lv[0].idx = 0;
	ck->ndirs = 1;

	for (;;) {
		L = &ck->lv[d];
		idx = L->idx;
		end = TRUE;
		e = NULL;
		if ( idx < DIR_MAX_ENT
		  && dir_sect(v, L->clus, idx / DIRENT_PER_SECT, FALSE, &sect) >= EX_OK ) {
			er = buf_load(v, sect);
			if ( er < EX_OK ) return er;
			off = (UINT)(idx % DIRENT_PER_SECT) * DIRENT_SIZE;
			e = v->buf + off;
			end = (BOOL)( e[0] == 0x00 );
		}

		/* a group of long name entries that no short entry closes */
		if ( lfn_want >= 0 && ( end || e[0] == 0xE5
		  || ( (e[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME && (e[0] & LFN_LAST) != 0 )
		  || ( (e[11] & ATTR_LONG_NAME) != ATTR_LONG_NAME && (e[11] & ATTR_VOLUME_ID) != 0 ) ) ) {
			if ( ck->fix && ck->dry ) {
				ck->bad++;
			} else if ( ck->fix ) {
				er = dir_erase(v, L->clus, lfn_first, idx - 1);
				if ( er < EX_OK ) return er;
			}
			lfn_want = -1;
			if ( !end ) {
				er = buf_load(v, sect);
				if ( er < EX_OK ) return er;
				e = v->buf + off;
			}
		}
		if ( end ) {
			if ( d == 0 ) break;
			d--;
			continue;
		}
		L->idx = idx + 1;

		if ( e[0] == 0xE5 ) {
			continue;
		}
		if ( (e[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME ) {
			INT	seq = e[0] & LFN_SEQ_MASK;

			if ( (e[0] & LFN_LAST) != 0 && seq >= 1 && seq <= LFN_MAX_ENT ) {
				lfn_first = idx;
				lfn_sum = e[13];
				lfn_want = seq - 1;
			} else if ( lfn_want >= 1 && seq == lfn_want && e[13] == lfn_sum
				 && (e[0] & LFN_LAST) == 0 ) {
				lfn_want--;
			} else {
				if ( ck->fix && ck->dry ) {
					ck->bad++;
				} else if ( ck->fix ) {
					er = dir_erase(v, L->clus, ( lfn_want >= 0 ) ? lfn_first : idx, idx);
					if ( er < EX_OK ) return er;
				}
				lfn_want = -1;
			}
			continue;
		}
		if ( (e[11] & ATTR_VOLUME_ID) != 0 ) {
			continue;
		}

		/* a short entry: it closes the group before it */
		grp = idx;
		if ( lfn_want >= 0 ) {
			if ( lfn_want == 0 && lfn_checksum(e) == lfn_sum ) {
				grp = lfn_first;
			} else if ( ck->fix && ck->dry ) {
				ck->bad++;
			} else if ( ck->fix ) {
				er = dir_erase(v, L->clus, lfn_first, idx - 1);
				if ( er < EX_OK ) return er;
				er = buf_load(v, sect);
				if ( er < EX_OK ) return er;
				e = v->buf + off;
			}
			lfn_want = -1;
		}

		if ( dot_ent(e, 1) || dot_ent(e, 2) ) {
			/* "." names the directory itself, ".." its parent (0 for the root) */
			if ( ck->fix && d > 0 && idx < 2 ) {
				UD	want = ( idx == 0 ) ? L->clus : L->parent;

				c = ent_clus(v, e);
				if ( c != want && !( want == 0 && v->fat_bits == 32 && c == v->root_clus ) ) {
					if ( ck->dry ) {
						ck->bad++;
					} else {
						ent_set_clus(v, e, want);
						v->dirty = TRUE;
					}
				}
			}
			continue;
		}

		c = ent_clus(v, e);
		if ( !ck->fix ) {
			if ( (e[12] & (NT_REN_OLD | NT_REN_NEW)) != 0 ) {
				if ( ck->nmark < CHK_MARKS ) {
					CHKMARK *mk = &ck->mark[ck->nmark++];

					mk->dirclus = L->clus;
					mk->first = grp;
					mk->last = idx;
					mk->bits = (UB)(e[12] & (NT_REN_OLD | NT_REN_NEW));
				} else {
					ck->partial = TRUE;
				}
			}
			if ( (e[11] & ATTR_DIRECTORY) == 0 || !clus_valid(v, c) ) continue;
		} else if ( (e[11] & ATTR_DIRECTORY) == 0 ) {
			er = chk_file(v, ck, sect, off);
			if ( er < EX_OK ) return er;
			continue;
		} else {
			n = 0;
			if ( c != 0 ) {
				er = chk_chain(v, ck, c, &n);
				if ( er < EX_OK ) return er;
			}
			if ( n == 0 ) {
				if ( ck->dry ) {
					ck->bad++;
					continue;
				}
				er = dir_erase(v, L->clus, grp, idx);
				if ( er < EX_OK ) return er;
				continue;
			}
		}

		/* into the directory, unless it is one the walk is already in */
		if ( c == v->root_clus ) continue;
		for ( k = 0; k <= d && ck->lv[k].clus != c; k++ ) ;
		if ( k <= d ) continue;
		if ( d + 1 >= CHK_DEPTH || ck->ndirs >= CHK_DIRS ) {
			ck->partial = TRUE;
			continue;
		}
		ck->ndirs++;
		d++;
		ck->lv[d].clus = c;
		ck->lv[d].parent = L->clus;
		ck->lv[d].idx = 0;
	}

	return buf_flush(v);
}

/* Step 2 */
LOCAL ER chk_marks( FATVOL *v, CHK *ck )
{
	CHKMARK	*mk;
	UD	sect;
	BOOL	done = FALSE;
	INT	i;
	ER	er;

	for ( i = 0; i < ck->nmark; i++ ) {
		if ( (ck->mark[i].bits & NT_REN_NEW) != 0 ) done = TRUE;
	}
	for ( i = 0; i < ck->nmark; i++ ) {
		mk = &ck->mark[i];
		if ( done && (mk->bits & NT_REN_OLD) != 0 ) {
			er = dir_erase(v, mk->dirclus, mk->first, mk->last);
			if ( er < EX_OK ) return er;
			continue;
		}
		er = dir_sect(v, mk->dirclus, mk->last / DIRENT_PER_SECT, FALSE, &sect);
		if ( er < EX_OK ) return er;
		er = buf_load(v, sect);
		if ( er < EX_OK ) return er;
		v->buf[(mk->last % DIRENT_PER_SECT) * DIRENT_SIZE + 12] &= (UB)~(NT_REN_OLD | NT_REN_NEW);
		v->dirty = TRUE;
	}

	return buf_flush(v);
}

LOCAL ER fat_check( FATVOL *v )
{
	CHK	*ck;
	UD	c, val, nfree = 0;
	BOOL	lost;
	ER	er;

#ifdef USE_KTEST
	knl_fat_checks++;
#endif
	dx_drop(v, 0);			/* it changes directories behind the names kept */
	dh_forget(v);
	er = vol_touch(v);
	if ( er < EX_OK ) return er;
	er = chk_copies(v);
	if ( er < EX_OK ) return er;

	ck = (CHK *)Kmalloc(sizeof(CHK));
	if ( ck != NULL ) {
		knl_memset(ck, 0, sizeof(CHK));

		/* step 2 */
		er = chk_walk(v, ck);
		if ( er >= EX_OK && !ck->partial && ck->nmark > 0 ) {
			er = chk_marks(v, ck);
		}

		/* step 3 */
		if ( er >= EX_OK && !ck->partial ) {
			ck->own = (UB *)Kmalloc((SZ)(( v->nclus + 2 + 7 ) / 8));
		}
		if ( er >= EX_OK && ck->own != NULL ) {
			knl_memset(ck->own, 0, (SZ)(( v->nclus + 2 + 7 ) / 8));
			ck->fix = TRUE;
			if ( v->fat_bits == 32 ) {
				/* the root keeps its first cluster whatever the table says */
				er = fat_get(v, v->root_clus, &val);
				if ( er >= EX_OK && ( val == CLUS_FREE || val == v->eoc - 1 ) ) {
					er = fat_set(v, v->root_clus, v->eoc_mark);
				}
				if ( er >= EX_OK ) er = chk_chain(v, ck, v->root_clus, &c);
			}
			if ( er >= EX_OK ) er = chk_walk(v, ck);
		}
	}
	if ( er < EX_OK ) goto out;

	/* step 4 */
	lost = (BOOL)( ck != NULL && ck->own != NULL && !ck->partial );
	for ( c = 2; c < v->nclus + 2; c++ ) {
		er = fat_get(v, c, &val);
		if ( er < EX_OK ) goto out;
		if ( lost && val != CLUS_FREE && val != v->eoc - 1 && !own_get(ck, c) ) {
			er = fat_set(v, c, CLUS_FREE);
			if ( er < EX_OK ) goto out;
			val = CLUS_FREE;
		}
		if ( val == CLUS_FREE ) nfree++;
	}
	v->free_cnt = nfree;
	v->free_known = TRUE;
	v->fsi_dirty = TRUE;
	er = buf_flush(v);
	if ( er >= EX_OK ) er = fsi_write(v);

    out:
	if ( ck != NULL ) {
		if ( ck->own != NULL ) Kfree(ck->own);
		Kfree(ck);
	}
	return er;
}

#ifdef USE_KTEST
/*
 * The look over of the mount, made on a mounted volume as it stands and
 * without putting anything right (the tests): what is kept is written
 * out first, so that the disk holds what the volume holds, and nothing
 * else is written. 'm' is a mount of this fimp, held by the caller.
 */
EXPORT ER knl_fat_verify( T_MOUNT *m, void *arg )
{
	T_FATVFY *r = (T_FATVFY *)arg;
	FATVOL	*v = (FATVOL *)m->exinf;
	CHK	*ck = NULL;
	UB	*a = NULL, *b = NULL;
	UD	c, val, s, k, nfree = 0;
	UW	i, w;
	ER	er;

	if ( m->fimp != &knl_fimp_fat || v == NULL || r == NULL ) {
		return EX_INVAL;
	}
	knl_memset(r, 0, sizeof(*r));
	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = buf_flush(v);
	if ( er < EX_OK ) goto out;
	r->inuse = v->inuse;
	r->ioerr = v->ioerr;

	a = (UB *)Kmalloc(SECTOR_SIZE);
	b = (UB *)Kmalloc(SECTOR_SIZE);
	ck = (CHK *)Kmalloc(sizeof(CHK));
	if ( a == NULL || b == NULL || ck == NULL ) { er = EX_NOMEM; goto out; }

	/* the mark as the disk has it */
	if ( v->fat_bits != 12 ) {
		UD	off = ( v->fat_bits == 16 ) ? 2 : 4;

		if ( sect_read(v, v->fat_sect + off / SECTOR_SIZE, a) < E_OK ) { er = EX_IO; goto out; }
		w = ( v->fat_bits == 16 ) ? rd16(a + off % SECTOR_SIZE) : rd32(a + off % SECTOR_SIZE);
		r->mark = (BOOL)( (w & ( ( v->fat_bits == 16 ) ? FAT16_CLEAN : FAT32_CLEAN )) == 0 );
	} else if ( v->bs_state != 0 ) {
		if ( sect_read(v, 0, a) < E_OK ) { er = EX_IO; goto out; }
		r->mark = (BOOL)( (a[v->bs_state] & BS_STATE_INUSE) != 0 );
	} else {
		r->mark = TRUE;			/* nothing to carry it: always looked over */
	}

	/* the copies of the table, as the disk has them */
	for ( s = 0; s < v->fat_sects; s++ ) {
		if ( sect_read(v, v->fat_sect + s, a) < E_OK ) { er = EX_IO; goto out; }
		for ( i = 1; i < v->num_fats; i++ ) {
			if ( sect_read(v, v->fat_sect + (UD)i * v->fat_sects + s, b) < E_OK ) {
				er = EX_IO;
				goto out;
			}
			for ( k = 0; k < SECTOR_SIZE && a[k] == b[k]; k++ ) ;
			if ( k < SECTOR_SIZE ) r->copies++;
		}
	}

	/* the rename marks left (step 2), then what step 3 would put right */
	knl_memset(ck, 0, sizeof(CHK));
	er = chk_walk(v, ck);
	if ( er < EX_OK ) goto out;
	r->marks = (UINT)ck->nmark;
	r->partial = ck->partial;
	ck->own = (UB *)Kmalloc((SZ)(( v->nclus + 2 + 7 ) / 8));
	if ( ck->own == NULL ) { er = EX_NOMEM; goto out; }
	knl_memset(ck->own, 0, (SZ)(( v->nclus + 2 + 7 ) / 8));
	ck->fix = TRUE;
	ck->dry = TRUE;
	if ( v->fat_bits == 32 ) {
		er = fat_get(v, v->root_clus, &val);
		if ( er < EX_OK ) goto out;
		if ( val == CLUS_FREE || val == v->eoc - 1 ) {
			ck->bad++;
		} else {
			er = chk_chain(v, ck, v->root_clus, &c);
			if ( er < EX_OK ) goto out;
		}
	}
	ck->nmark = 0;
	er = chk_walk(v, ck);
	if ( er < EX_OK ) goto out;
	r->bad = (UINT)ck->bad;
	r->files = (UINT)ck->nfiles;

	/* step 4: clusters taken that no entry has, and the free count */
	for ( c = 2; c < v->nclus + 2; c++ ) {
		er = fat_get(v, c, &val);
		if ( er < EX_OK ) goto out;
		if ( val == CLUS_FREE ) {
			nfree++;
		} else if ( val != v->eoc - 1 && !own_get(ck, c) ) {
			r->lost++;
		}
	}
	r->freeok = (BOOL)( !v->free_known || v->free_cnt == nfree );

    out:
	tk_unl_mtx(v->mtxid);
	if ( ck != NULL ) {
		if ( ck->own != NULL ) Kfree(ck->own);
		Kfree(ck);
	}
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);

	return er;
}
#endif

/* ---------------------------------------------------------------- fimp calls */

LOCAL ER fat_mount( T_MOUNT *m )
{
	FATVOL	*v;
	UB	*bpb;
	UD	tot_sec, root_ents, data_sects;
	UW	bytes_per_sec, rsvd;
	UD	fat_sz;
	T_CMTX	cmtx;
	ER	er = EX_OK;

	v = (FATVOL *)Kmalloc(sizeof(FATVOL));
	if ( v == NULL ) {
		return EX_NOMEM;
	}
	knl_memset(v, 0, sizeof(FATVOL));
	v->dd = m->dd;
	v->buf_sect = ~0ULL;
	v->fc = (FCSLOT *)Kmalloc(sizeof(FCSLOT) * FC_SLOTS);
	if ( v->fc == NULL ) {
		Kfree(v);
		return EX_NOMEM;
	}
	{
		INT	i;

		for ( i = 0; i < FC_SLOTS; i++ ) {
			v->fc[i].sect = ~0ULL;
			v->fc[i].dirty = FALSE;
			v->fc[i].used = 0;
		}
	}
	v->fc_cur = -1;
	v->buf = v->fc[0].data;

	bpb = (UB *)Kmalloc(SECTOR_SIZE);
	if ( bpb == NULL ) { Kfree(v->fc); Kfree(v); return EX_NOMEM; }

	if ( sect_read(v, 0, bpb) < E_OK ) { er = EX_IO; goto err; }

	bytes_per_sec = rd16(bpb + 11);
	v->sec_per_clus = bpb[13];
	rsvd            = rd16(bpb + 14);
	v->num_fats     = bpb[16];
	root_ents       = rd16(bpb + 17);
	tot_sec         = rd16(bpb + 19);
	if ( tot_sec == 0 ) tot_sec = rd32(bpb + 32);
	fat_sz          = rd16(bpb + 22);
	if ( fat_sz == 0 ) fat_sz = rd32(bpb + 36);

	/*
	 * A sector that is not a boot sector of a FAT volume (the MBR of a
	 * whole disk, say) is told apart by these: a cluster is a power of
	 * two sectors, and the media byte is 0xF0 or 0xF8 and above.
	 */
	if ( bytes_per_sec != SECTOR_SIZE || v->sec_per_clus == 0 || rsvd == 0
	  || (v->sec_per_clus & (v->sec_per_clus - 1)) != 0
	  || ( bpb[21] != 0xF0 && bpb[21] < 0xF8 )
	  || v->num_fats == 0 || fat_sz == 0 || tot_sec == 0 ) {
		er = EX_INVAL;
		goto err;
	}

	/* the fixed root of FAT12 and FAT16 lies between the tables and the data */
	v->fat_sect   = rsvd;
	v->fat_sects  = fat_sz;
	v->root_sect  = rsvd + (UD)v->num_fats * fat_sz;
	v->root_sects = (root_ents * DIRENT_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
	v->data_sect  = v->root_sect + v->root_sects;
	if ( tot_sec <= v->data_sect ) { er = EX_INVAL; goto err; }
	data_sects    = tot_sec - v->data_sect;
	v->nclus      = data_sects / v->sec_per_clus;
	v->free_hint  = 2;

	/* the type follows from the number of clusters and from nothing else */
	if ( v->nclus < FAT16_MIN_CLUS ) {
		v->fat_bits = 12;
		v->eoc      = 0x0FF8;
		v->eoc_mark = 0x0FFF;
	} else if ( v->nclus < FAT32_MIN_CLUS ) {
		v->fat_bits = 16;
		v->eoc      = 0xFFF8;
		v->eoc_mark = 0xFFFF;
	} else {
		v->fat_bits = 32;
		v->eoc      = 0x0FFFFFF8;
		v->eoc_mark = CLUS_MASK;
	}

	/* a table shorter than the data area leaves the clusters past it unused */
	{
		UD	ents = fat_sz * SECTOR_SIZE * 8 / v->fat_bits;

		if ( ents < 3 ) { er = EX_INVAL; goto err; }
		if ( v->nclus + 2 > ents ) v->nclus = ents - 2;
	}

	if ( v->fat_bits == 32 ) {
		/* no fixed root area: the root is a chain like any directory */
		if ( root_ents != 0 ) { er = EX_INVAL; goto err; }
		v->root_clus = rd32(bpb + 44);
		if ( !clus_valid(v, v->root_clus) ) { er = EX_INVAL; goto err; }
	} else {
		if ( root_ents == 0 ) { er = EX_INVAL; goto err; }
		v->root_clus = 0;
	}

	/* the free count and where to look for a free cluster, from FSInfo */
	if ( v->fat_bits == 32 ) {
		UD	fsi = rd16(bpb + 48);

		if ( fsi >= 1 && fsi < rsvd && sect_read(v, fsi, bpb) >= E_OK
		  && rd32(bpb) == FSI_LEAD_SIG && rd32(bpb + 484) == FSI_STRUC_SIG
		  && rd32(bpb + 508) == FSI_TRAIL_SIG ) {
			UD	cnt = rd32(bpb + FSI_FREE_OFF), next = rd32(bpb + FSI_NEXT_OFF);

			v->fsi_sect = fsi;
			if ( cnt <= v->nclus ) {
				v->free_cnt = cnt;
				v->free_known = TRUE;
			}
			if ( clus_valid(v, next) ) {
				v->free_hint = next;
			}
		}
		if ( sect_read(v, 0, bpb) < E_OK ) { er = EX_IO; goto err; }
	}

	/*
	 * How the volume was left. FAT12 keeps it in the state byte of the
	 * boot sector when the boot sector has the extended fields; with
	 * nothing to keep it in, the volume is looked over at every mount
	 * (FAT12 volumes are small).
	 */
	v->rdonly = (BOOL)( (m->flags & FS_MNT_RDONLY) != 0 );
	v->devnm  = m->dev;
	if ( v->fat_bits == 12 && ( bpb[38] == 0x28 || bpb[38] == 0x29 ) ) {
		v->bs_state = BS_STATE_OFF;
	}
	{
		BOOL	check;

		if ( v->fat_bits != 12 ) {
			UW	w, clean, noerr;

			clean = ( v->fat_bits == 16 ) ? FAT16_CLEAN : FAT32_CLEAN;
			noerr = ( v->fat_bits == 16 ) ? FAT16_NOERR : FAT32_NOERR;
			if ( fat1_get(v, &w) < EX_OK ) { er = EX_IO; goto err; }
			v->inuse = (BOOL)( (w & clean) == 0 );
			check = (BOOL)( v->inuse || (w & noerr) == 0 );
		} else if ( v->bs_state != 0 ) {
			v->inuse = (BOOL)( (bpb[v->bs_state] & BS_STATE_INUSE) != 0 );
			check = v->inuse;
		} else {
			v->inuse = TRUE;
			check = TRUE;
		}
		if ( check && !v->rdonly ) {
			/* what was counted before the look over does not hold */
			v->free_known = FALSE;
			er = fat_check(v);
			if ( er < EX_OK ) goto err;
		}
	}

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	v->mtxid = tk_cre_mtx(&cmtx);
	if ( v->mtxid <= 0 ) { er = EX_NOMEM; goto err; }

	/* room for the open files taken now, not while files are opened */
	v->ofile = (T_FILE **)Kmalloc(sizeof(T_FILE *) * FAT_OFILE_INIT);
	if ( v->ofile == NULL ) { tk_del_mtx(v->mtxid); er = EX_NOMEM; goto err; }
	v->maxofile = FAT_OFILE_INIT;

	m->exinf = v;
	Kfree(bpb);

	return EX_OK;

    err:
	Kfree(bpb);
	Kfree(v->fc);
	Kfree(v);

	return er;
}

LOCAL ER fat_unmount( T_MOUNT *m )
{
	FATVOL	*v = (FATVOL *)m->exinf;

	if ( v == NULL ) return EX_OK;
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( buf_flush(v) >= EX_OK && fsi_write(v) >= EX_OK && v->inuse ) {
		vol_mark(v, FALSE);
	}
	tk_unl_mtx(v->mtxid);
	tk_del_mtx(v->mtxid);
	dx_drop(v, 0);
	if ( v->ofile != NULL ) Kfree(v->ofile);
	Kfree(v->fc);
	Kfree(v);
	m->exinf = NULL;

	return EX_OK;
}

/* A file opened on the volume, noted; the list grows as needed */
LOCAL ER ofile_add( FATVOL *v, T_FILE *f )
{
	T_FILE	**a;
	INT	n, i;

	if ( v->nofile >= v->maxofile ) {
		n = v->maxofile * 2;
		a = (T_FILE **)Kmalloc(sizeof(T_FILE *) * (SZ)n);
		if ( a == NULL ) return EX_NOMEM;
		for ( i = 0; i < v->nofile; i++ ) a[i] = v->ofile[i];
		if ( v->ofile != NULL ) Kfree(v->ofile);
		v->ofile = a;
		v->maxofile = n;
	}
	v->ofile[v->nofile++] = f;

	return EX_OK;
}

LOCAL void ofile_del( FATVOL *v, T_FILE *f )
{
	INT	i;

	for ( i = 0; i < v->nofile; i++ ) {
		if ( v->ofile[i] == f ) {
			v->ofile[i] = v->ofile[--v->nofile];
			return;
		}
	}
}

/* Whether a file open on the volume has its entry at (sect, off) */
LOCAL BOOL ofile_at( FATVOL *v, UD sect, UINT off )
{
	INT	i;

	for ( i = 0; i < v->nofile; i++ ) {
		if ( v->ofile[i]->dir_sect == sect && v->ofile[i]->dir_off == off ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL ER fat_open( T_MOUNT *m, CONST char *path, UINT oflags, T_FILE *f )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	clus, size, sect = 0, parent;
	UINT	attr, off = 0;
	CONST char *last;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = path_walk(v, path, &clus, &size, &attr, &sect, &off, NULL, NULL);

	if ( er == EX_NOENT && (oflags & O_CREAT) != 0 ) {
		INT	len;

		er = path_walk(v, path, &clus, &size, &attr, NULL, NULL, &parent, &last);
		if ( er < EX_OK ) goto exit;
		for ( len = 0; last[len] != '\0' && last[len] != '/'; len++ ) ;

		er = vol_touch(v);
		if ( er < EX_OK ) goto exit;
		er = dir_create(v, parent, (CONST UB *)last, len, ATTR_ARCHIVE, 0, 0, NULL, 0, &sect, &off);
		if ( er < EX_OK ) goto exit;

		clus = 0;
		size = 0;
		attr = ATTR_ARCHIVE;
	} else if ( er == EX_OK && (oflags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) ) {
		er = EX_EXIST;
		goto exit;
	}
	if ( er < EX_OK ) {
		goto exit;
	}

	if ( (attr & ATTR_DIRECTORY) != 0 ) {
		if ( (oflags & O_ACCMODE) != O_RDONLY ) { er = EX_ISDIR; goto exit; }
		f->mode = FS_IFDIR;
		f->size = 0;
	} else {
		if ( (oflags & O_DIRECTORY) != 0 ) { er = EX_NOTDIR; goto exit; }
		f->mode = FS_IFREG;
		f->size = size;
	}
	f->ino      = clus;
	f->dir_sect = sect;
	f->dir_off  = off;
	f->priv     = 0;

	if ( (oflags & O_TRUNC) != 0 && f->mode == FS_IFREG && f->size > 0 ) {
		/* the entry lets go of the chain before the chain is freed */
		er = vol_touch(v);
		if ( er < EX_OK ) goto exit;
		f->ino = 0;
		f->size = 0;
		if ( sect != 0 ) {
			er = buf_load(v, sect);
			if ( er < EX_OK ) goto exit;
			ent_set_clus(v, v->buf + off, 0);
			wr32(v->buf + off + 28, 0);
			v->dirty = TRUE;
			er = buf_flush(v);
			if ( er < EX_OK ) goto exit;
		}
		er = chain_free(v, clus);
		if ( er >= EX_OK ) er = buf_flush(v);
	}
	if ( er >= EX_OK ) {
		er = ofile_add(v, f);
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL ER fat_close( T_FILE *f )
{
	FATVOL	*v = (FATVOL *)f->mount->exinf;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	ofile_del(v, f);
	er = buf_flush(v);
	/* FSInfo follows the count now and then, not at every close */
	if ( er >= EX_OK && v->fsi_dirty && now_ms() - v->fsi_ms >= FSI_WRITE_MS ) {
		er = fsi_write(v);
	}
	tk_unl_mtx(v->mtxid);

	return er;
}

/*
 * Update the directory entry of an open file after a write.
 */
LOCAL ER fat_update_ent( FATVOL *v, T_FILE *f )
{
	UH	date, time;
	ER	er;

	if ( f->dir_sect == 0 ) {
		return EX_OK;
	}
	er = buf_load(v, f->dir_sect);
	if ( er < EX_OK ) return er;

	fat_now(&date, &time);
	ent_set_clus(v, v->buf + f->dir_off, f->ino);
	wr32(v->buf + f->dir_off + 28, (UW)f->size);
	wr16(v->buf + f->dir_off + 22, time);
	wr16(v->buf + f->dir_off + 24, date);
	v->dirty = TRUE;

	return buf_flush(v);
}

/*
 * Where the disk can move whole sectors of the buffer at 'p' straight
 * to or from, and how many of 'n' sectors lie there together (*p_n).
 * The kernel's own memory is handed over as it is. A process's memory
 * is handed over as the kernel's view of the same pages, one run of
 * pages that follow one another in memory at a time (knl_prc_dma): the
 * driver, and the task that serves the disk, need not be in the
 * process's space, and the driver keeps the caches right on that view
 * as on any buffer of the kernel's (design 10.8). 'into' is TRUE when
 * the disk writes the memory. NULL when not one whole sector can go
 * straight; that sector goes through the volume's buffer and is copied.
 */
LOCAL UB *dma_at( UB *p, UD n, BOOL into, UD *p_n )
{
	UD	at = 0, want = n * SECTOR_SIZE;
	void	*pa, *k = NULL;
	INT	r;

	*p_n = 0;
	if ( want > 0x40000000 ) want = 0x40000000;
	while ( at < want ) {
		pa = NULL;
		r = ConvPhysicalAddress(p + at, (INT)( want - at ), &pa);
		if ( r <= 0 || pa == NULL ) break;
		at += (UD)r;
	}
	if ( at > 0 ) {
		*p_n = at / SECTOR_SIZE;
		return p;
	}
	r = knl_prc_dma(p, (INT)want, into, &k);
	if ( r < SECTOR_SIZE || k == NULL ) {
		return NULL;
	}
	*p_n = (UD)r / SECTOR_SIZE;
	return (UB *)k;
}

/*
 * How many whole sectors from the one at file offset 'off' (sector
 * 'sect' of cluster 'clus') lie one after another on the disk, up to
 * 'want'. The chain is followed while each cluster is the one after the
 * last. For writing ('grow') the chain is made longer as it is followed,
 * and a cluster the run will cover whole is not zeroed first; 'left' is
 * how much is still to be written from 'off'. For reading, a sector
 * that is kept in a slot ends the run: the slot may hold what has not
 * been written yet.
 */
LOCAL UD sect_run( FATVOL *v, T_FILE *f, UD off, UD clus, UD sect, UD want, BOOL grow,
		   SZ left )
{
	UD	csize = (UD)v->sec_per_clus * SECTOR_SIZE;
	UD	run = 1, next;

	if ( want > FAT_RUN_MAX ) want = FAT_RUN_MAX;
	for ( off += SECTOR_SIZE; run < want; off += SECTOR_SIZE ) {
		if ( off % csize == 0 ) {
			v->no_zero = (BOOL)( grow && left - (SZ)run * SECTOR_SIZE >= (SZ)csize );
			if ( file_clus(v, f, off / csize, grow, &next) < EX_OK || next != clus + 1 ) {
				v->no_zero = FALSE;
				break;
			}
			v->no_zero = FALSE;
			clus = next;
		}
		if ( !grow ) {
			fc_park(v);
			if ( fc_find(v, sect + run) >= 0 ) break;
		}
		run++;
	}
	return run;
}

LOCAL INT fat_read( T_FILE *f, void *buf, SZ len )
{
	FATVOL	*v = (FATVOL *)f->mount->exinf;
	UB	*dst = (UB *)buf, *xfer;
	UD	clus, sect, csize, reach;
	SZ	done = 0;
	ER	er = EX_OK;

	if ( f->offset >= f->size ) {
		return 0;
	}
	if ( len > f->size - f->offset ) {
		len = f->size - f->offset;
	}
	csize = (UD)v->sec_per_clus * SECTOR_SIZE;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	while ( done < len ) {
		UD	coff = f->offset % csize;
		UD	soff = coff % SECTOR_SIZE;
		SZ	n = SECTOR_SIZE - soff;

		if ( n > len - done ) n = len - done;

		er = file_clus(v, f, f->offset / csize, FALSE, &clus);
		if ( er < EX_OK ) break;
		sect = clus_to_sect(v, clus) + coff / SECTOR_SIZE;

		if ( soff == 0 && n == SECTOR_SIZE
		     && ( xfer = dma_at(dst + done, (UD)( ( len - done ) / SECTOR_SIZE ), TRUE, &reach) ) != NULL
		     && reach > 0 ) {
			/* whole sectors: from its slot if one is kept, which
			   may hold what has not been written yet, else as many
			   as lie together in one request */
			INT	k;
			UD	run;

			fc_park(v);
			k = fc_find(v, sect);
			if ( k >= 0 ) {
				knl_memcpy(dst + done, v->fc[k].data, SECTOR_SIZE);
			} else {
				run = sect_run(v, f, f->offset, clus, sect, reach, FALSE, 0);
				if ( sects_read(v, sect, xfer, run) < E_OK ) {
					er = EX_IO;
					break;
				}
				n = (SZ)run * SECTOR_SIZE;
			}
		} else {
			er = buf_load(v, sect);
			if ( er < EX_OK ) break;
			knl_memcpy(dst + done, v->buf + soff, (INT)n);
		}
		done += n;
		f->offset += n;
	}

	tk_unl_mtx(v->mtxid);

	if ( done == 0 && er < EX_OK ) {
		return (INT)er;
	}
	return (INT)done;
}

LOCAL INT fat_write( T_FILE *f, CONST void *buf, SZ len )
{
	FATVOL	*v = (FATVOL *)f->mount->exinf;
	CONST UB *src = (CONST UB *)buf;
	UB	*xfer;
	UD	clus, sect, csize, reach;
	SZ	done = 0;
	ER	er = EX_OK;

	csize = (UD)v->sec_per_clus * SECTOR_SIZE;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	if ( f->ino == 0 ) {			/* first write to an empty file */
		v->no_zero = (BOOL)( f->offset == 0 && len >= (SZ)csize );
		er = clus_alloc(v, &clus);
		v->no_zero = FALSE;
		if ( er < EX_OK ) goto exit;
		f->ino = clus;
		f->hint_idx = 0;
		f->hint_clus = 0;
	}

	while ( done < len ) {
		UD	coff = f->offset % csize;
		UD	soff = coff % SECTOR_SIZE;
		SZ	n = SECTOR_SIZE - soff;

		if ( n > len - done ) n = len - done;

		/* a cluster this write covers whole need not be zeroed first */
		v->no_zero = (BOOL)( coff == 0 && len - done >= (SZ)csize );
		er = file_clus(v, f, f->offset / csize, TRUE, &clus);
		v->no_zero = FALSE;
		if ( er < EX_OK ) break;
		sect = clus_to_sect(v, clus) + coff / SECTOR_SIZE;

		if ( soff == 0 && n == SECTOR_SIZE
		     && ( xfer = dma_at((UB *)src + done, (UD)( ( len - done ) / SECTOR_SIZE ), FALSE,
					&reach) ) != NULL
		     && reach > 0 ) {
			/* whole sectors go straight out, as many as lie together;
			   a kept copy of any of them is stale */
			UD	run, k;

			run = sect_run(v, f, f->offset, clus, sect, reach, TRUE, len - done);
			for ( k = 0; k < run; k++ ) {
				fc_forget(v, sect + k);
			}
			if ( sects_write(v, sect, xfer, run) < E_OK ) { er = EX_IO; break; }
			n = (SZ)run * SECTOR_SIZE;
		} else {
			er = buf_load(v, sect);
			if ( er < EX_OK ) break;
			knl_memcpy(v->buf + soff, src + done, (INT)n);
			v->dirty = TRUE;
		}
		done += n;
		f->offset += n;
		if ( f->offset > f->size ) f->size = f->offset;
	}

	if ( done > 0 ) {
		ER e = fat_update_ent(v, f);
		if ( er >= EX_OK ) er = e;
	}

    exit:
	tk_unl_mtx(v->mtxid);

	if ( done == 0 && er < EX_OK ) {
		return (INT)er;
	}
	return (INT)done;
}

LOCAL ER fat_truncate( T_FILE *f, UD len )
{
	FATVOL	*v = (FATVOL *)f->mount->exinf;
	UD	csize, clus, next;
	ER	er = EX_OK;

	if ( f->mode != FS_IFREG ) {
		return EX_ISDIR;
	}
	csize = (UD)v->sec_per_clus * SECTOR_SIZE;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	/* the chain is about to change shape: what was remembered of it
	   means nothing afterwards */
	f->hint_idx = 0;
	f->hint_clus = 0;

	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	if ( len == 0 ) {
		/* the entry lets go of the chain before the chain is freed */
		clus = f->ino;
		f->ino = 0;
		f->size = 0;
		if ( f->offset > 0 ) f->offset = 0;
		er = fat_update_ent(v, f);
		if ( er >= EX_OK && clus != 0 ) {
			er = chain_free(v, clus);
			if ( er >= EX_OK ) er = buf_flush(v);
		}
		goto exit;
	} else if ( len < f->size ) {
		er = chain_nth(v, f->ino, (len - 1) / csize, FALSE, &clus);
		if ( er >= EX_OK ) {
			er = fat_get(v, clus, &next);
			if ( er >= EX_OK && clus_valid(v, next) ) {
				er = chain_free(v, next);
			}
			if ( er >= EX_OK ) {
				er = fat_set(v, clus, v->eoc_mark);
			}
		}
	}
	if ( er >= EX_OK ) {
		f->size = len;
		if ( f->offset > len ) f->offset = len;
		er = fat_update_ent(v, f);
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL ER fat_stat( T_MOUNT *m, CONST char *path, T_FSTAT *st )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	clus, size, sect;
	UINT	attr, off;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = path_walk(v, path, &clus, &size, &attr, &sect, &off, NULL, NULL);
	if ( er >= EX_OK ) {
		st->mode  = (( attr & ATTR_DIRECTORY ) != 0 ? FS_IFDIR : FS_IFREG) | FS_IRWXU;
		st->size  = (( attr & ATTR_DIRECTORY ) != 0) ? 0 : size;
		st->ino   = clus;
		st->mtime = 0;
		if ( sect != 0 && buf_load(v, sect) >= EX_OK ) {
			st->mtime = fat_to_tron(rd16(v->buf + off + 24), rd16(v->buf + off + 22));
		}
	}

	tk_unl_mtx(v->mtxid);

	return er;
}

/*
 * The time a file or a directory was last written, set: what a copy
 * brings from where it came. The root has no entry to hold it.
 */
LOCAL ER fat_utime( T_MOUNT *m, CONST char *path, UD mtime )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	clus, size, sect;
	UINT	attr, off;
	UH	date, time;
	ER	er;

	if ( !fat_of_tron(mtime, &date, &time) ) {
		return EX_INVAL;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = path_walk(v, path, &clus, &size, &attr, &sect, &off, NULL, NULL);
	if ( er >= EX_OK && sect == 0 ) er = EX_INVAL;
	if ( er >= EX_OK ) er = vol_touch(v);
	if ( er >= EX_OK ) er = buf_load(v, sect);
	if ( er >= EX_OK ) {
		wr16(v->buf + off + 22, time);
		wr16(v->buf + off + 24, date);
		v->dirty = TRUE;
		er = buf_flush(v);
	}

	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL INT fat_getdents( T_FILE *f, T_DIRENT *buf, INT nent )
{
	FATVOL	*v = (FATVOL *)f->mount->exinf;
	UD	sect, idx;
	UINT	off;
	INT	n = 0;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	idx = f->priv;
	while ( n < nent ) {
		er = dir_next(v, f->ino, &idx, buf[n].name, FS_NAME_MAX, &sect, &off, NULL);
		if ( er < EX_OK ) { er = EX_OK; break; }	/* end of directory */

		{
			UB *e = v->buf + off;

			buf[n].mode = (( e[11] & ATTR_DIRECTORY ) != 0) ? FS_IFDIR : FS_IFREG;
			buf[n].size = (( e[11] & ATTR_DIRECTORY ) != 0) ? 0 : rd32(e + 28);
			buf[n].ino  = ent_clus(v, e);
		}
		n++;
	}
	f->priv = idx;

	tk_unl_mtx(v->mtxid);

	if ( n == 0 && er < EX_OK ) {
		return (INT)er;
	}
	return n;
}

LOCAL ER fat_mkdir( T_MOUNT *m, CONST char *path )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	clus, size, parent, newclus, sect;
	UINT	attr, off;
	CONST char *last;
	UH	date, time;
	INT	len;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = path_walk(v, path, &clus, &size, &attr, NULL, NULL, NULL, NULL);
	if ( er >= EX_OK ) { er = EX_EXIST; goto exit; }

	er = path_walk(v, path, &clus, &size, &attr, NULL, NULL, &parent, &last);
	if ( er < EX_OK ) goto exit;
	for ( len = 0; last[len] != '\0' && last[len] != '/'; len++ ) ;

	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	er = clus_alloc(v, &newclus);
	if ( er < EX_OK ) goto exit;

	fat_now(&date, &time);

	/* "." and ".." in the new directory */
	er = buf_load(v, clus_to_sect(v, newclus));
	if ( er < EX_OK ) goto exit;
	knl_memset(v->buf, 0, SECTOR_SIZE);
	{
		UB *e = v->buf;

		knl_memset(e, ' ', 11); e[0] = '.';
		e[11] = ATTR_DIRECTORY;
		wr16(e + 22, time); wr16(e + 24, date);
		ent_set_clus(v, e, newclus);

		e += DIRENT_SIZE;
		knl_memset(e, ' ', 11); e[0] = '.'; e[1] = '.';
		e[11] = ATTR_DIRECTORY;
		wr16(e + 22, time); wr16(e + 24, date);
		ent_set_clus(v, e, parent);
	}
	v->dirty = TRUE;
	er = buf_flush(v);
	if ( er < EX_OK ) goto exit;

	/* the entry in the parent; with no room for it (a full fixed root)
	   the cluster goes back */
	er = dir_create(v, parent, (CONST UB *)last, len, ATTR_DIRECTORY, newclus, 0, NULL, 0, &sect, &off);
	if ( er < EX_OK ) {
		chain_free(v, newclus);
		buf_flush(v);
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

/*
 * EX_OK when a directory holds nothing but "." and "..", EX_NOTEMPTY
 * when it holds more.
 */
LOCAL ER dir_empty( FATVOL *v, UD clus )
{
	UD	n, s;
	UINT	i;

	for ( n = 0; ; n++ ) {
		if ( dir_sect(v, clus, n, FALSE, &s) < EX_OK ) break;
		if ( buf_load(v, s) < EX_OK ) return EX_IO;
		for ( i = 0; i < DIRENT_PER_SECT; i++ ) {
			UB *e = v->buf + i * DIRENT_SIZE;

			if ( e[0] == 0x00 ) return EX_OK;
			if ( e[0] == 0xE5 ) continue;
			if ( (e[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME ) continue;
			if ( e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')) ) continue;
			return EX_NOTEMPTY;
		}
	}

	return EX_OK;
}

/* The last component of a path and its length (trailing slashes left out) */
LOCAL CONST char *last_name( CONST char *path, INT *p_len )
{
	INT	end, beg;

	for ( end = 0; path[end] != '\0'; end++ ) ;
	while ( end > 0 && path[end - 1] == '/' ) end--;
	for ( beg = end; beg > 0 && path[beg - 1] != '/'; beg-- ) ;
	*p_len = end - beg;

	return path + beg;
}

/* "." or "..": entries every directory has, never removed nor moved */
LOCAL BOOL dot_name( CONST char *name, INT len )
{
	return ( ( len == 1 && name[0] == '.' )
	      || ( len == 2 && name[0] == '.' && name[1] == '.' ) );
}

/*
 * Remove a file (want_dir FALSE) or an empty directory (TRUE)
 */
LOCAL ER fat_remove( T_MOUNT *m, CONST char *path, BOOL want_dir )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	clus, size, sect, dirclus, first_idx, last_idx;
	UINT	attr, off;
	CONST char *nm;
	INT	len;
	ER	er;

	nm = last_name(path, &len);
	if ( dot_name(nm, len) ) {
		return EX_INVAL;
	}

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	er = path_walk(v, path, &clus, &size, &attr, &sect, &off, NULL, NULL);
	if ( er < EX_OK ) goto exit;
	if ( sect == 0 ) { er = EX_BUSY; goto exit; }		/* the root itself */
	dirclus   = v->walk_dirclus;
	first_idx = v->walk_first_idx;
	last_idx  = v->walk_last_idx;

	if ( want_dir ) {
		if ( (attr & ATTR_DIRECTORY) == 0 ) { er = EX_NOTDIR; goto exit; }
		if ( clus == v->root_clus ) { er = EX_BUSY; goto exit; }
		er = dir_empty(v, clus);
		if ( er < EX_OK ) goto exit;
	} else {
		if ( (attr & ATTR_DIRECTORY) != 0 ) { er = EX_ISDIR; goto exit; }
	}

	/* the short entry and the long name entries in front of it, then
	   the clusters: a cut in between leaves clusters nothing names */
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	er = dir_erase(v, dirclus, first_idx, last_idx);
	if ( er >= EX_OK && clus_valid(v, clus) ) {
		er = chain_free(v, clus);
		if ( er >= EX_OK ) er = buf_flush(v);
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL ER fat_unlink( T_MOUNT *m, CONST char *path )
{
	return fat_remove(m, path, FALSE);
}

LOCAL ER fat_rmdir( T_MOUNT *m, CONST char *path )
{
	return fat_remove(m, path, TRUE);
}

/*
 * The directory a directory lies in, read from its ".." entry: 0 for
 * the root.
 */
LOCAL ER dir_parent( FATVOL *v, UD dirclus, UD *p_parent )
{
	UB	*e;
	ER	er;

	if ( !clus_valid(v, dirclus) ) {
		return EX_IO;
	}
	er = buf_load(v, clus_to_sect(v, dirclus));
	if ( er < EX_OK ) return er;
	e = v->buf + DIRENT_SIZE;
	if ( e[0] != '.' || e[1] != '.' || e[2] != ' ' ) {
		return EX_IO;
	}
	*p_parent = ent_clus(v, e);
	if ( v->fat_bits == 32 && *p_parent == v->root_clus ) {
		*p_parent = 0;			/* the root named by its cluster */
	}

	return EX_OK;
}

/*
 * Rename within the volume, in one directory or from one to another.
 *
 *	The entry group of the source (its long name entries and the short
 *	entry) is written anew under the new name in the directory it goes
 *	to, with a short name made the same way as for a new file, and the
 *	old group is erased after; the first cluster, size, attributes and
 *	times go with it. A directory that changes parent has its ".."
 *	pointed at the new one.
 *
 *	A name that is already taken is replaced as POSIX asks: a file by a
 *	file, an empty directory by a directory. The entry that has the
 *	name takes over the cluster and size of the one moved and its old
 *	clusters are freed. A file or directory that is open cannot be
 *	replaced (EX_BUSY): its clusters would be freed under it. The open
 *	files of the entry moved follow it.
 *
 *	A directory cannot be moved into itself or below itself (EX_INVAL),
 *	nor can "." and ".." be moved or be the new name.
 *
 *	The disk is changed in this order, each step written before the
 *	next begins:
 *	  1. the entry moved gets the mark NT_REN_OLD;
 *	  2. the new group is written with the mark NT_REN_NEW on its short
 *	     entry (or the entry replaced takes over, with the mark);
 *	  3. the old group is erased;
 *	  4. a directory with a new parent has its ".." pointed at it;
 *	  5. the clusters of the entry replaced are freed;
 *	  6. the mark NT_REN_NEW is taken off.
 *	A cut before step 2 is on the disk leaves only NT_REN_OLD, and the
 *	next mount undoes the rename; a cut after it leaves NT_REN_NEW,
 *	and the next mount finishes it (fat_check).
 */
LOCAL ER ent_mark( FATVOL *v, UD sect, UINT off, UB bit, BOOL on )
{
	ER	er;

	er = buf_load(v, sect);
	if ( er < EX_OK ) return er;
	if ( on ) v->buf[off + 12] |= bit;
	else v->buf[off + 12] &= (UB)~bit;
	v->dirty = TRUE;

	return buf_flush(v);
}

LOCAL ER fat_rename( T_MOUNT *m, CONST char *from, CONST char *to )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	UD	sclus, ssize, ssect, sdir, sfirst, slast;
	UD	tclus, tsize, tsect, tdir, tfirst, tlast;
	UD	nsect = 0, d, n;
	UINT	sattr, tattr, soff, toff, noff = 0;
	CONST char *sname, *tname, *last;
	INT	slen, tlen, i;
	UB	sent[DIRENT_SIZE], *e;
	BOOL	replaced = FALSE;
	ER	er;

	sname = last_name(from, &slen);
	tname = last_name(to, &tlen);
	if ( dot_name(sname, slen) || dot_name(tname, tlen) ) {
		return EX_INVAL;
	}

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	/* the entry to move */
	er = path_walk(v, from, &sclus, &ssize, &sattr, &ssect, &soff, NULL, NULL);
	if ( er < EX_OK ) goto exit;
	if ( ssect == 0 ) { er = EX_BUSY; goto exit; }		/* the root itself */
	sdir   = v->walk_dirclus;
	sfirst = v->walk_first_idx;
	slast  = v->walk_last_idx;
	er = buf_load(v, ssect);
	if ( er < EX_OK ) goto exit;
	knl_memcpy(sent, v->buf + soff, DIRENT_SIZE);

	/* the directory it goes to */
	er = path_walk(v, to, &tclus, &tsize, &tattr, NULL, NULL, &tdir, &last);
	if ( er < EX_OK ) goto exit;

	/* a directory cannot go inside itself: none of the directories
	   above the new place may be the one moved */
	if ( (sattr & ATTR_DIRECTORY) != 0 ) {
		for ( d = tdir, n = 0; d != 0; n++ ) {
			if ( d == sclus ) { er = EX_INVAL; goto exit; }
			if ( n > v->nclus ) { er = EX_IO; goto exit; }	/* a loop */
			er = dir_parent(v, d, &d);
			if ( er < EX_OK ) goto exit;
		}
	}

	/* what already has the new name */
	er = dir_lookup(v, tdir, (CONST UB *)tname, tlen, &tsect, &toff, &tfirst, &tlast);
	if ( er == EX_OK && tdir == sdir && tlast == slast ) {
		/* the entry itself: a change of case only, or no change */
		if ( slen == tlen ) {
			for ( i = 0; i < slen && sname[i] == tname[i]; i++ ) ;
			if ( i == slen ) {
				er = EX_OK;
				goto exit;
			}
		}
	} else if ( er == EX_OK ) {
		e = v->buf + toff;
		tattr = e[11];
		tclus = ent_clus(v, e);
		if ( (sattr & ATTR_DIRECTORY) != 0 && (tattr & ATTR_DIRECTORY) == 0 ) {
			er = EX_NOTDIR;
			goto exit;
		}
		if ( (sattr & ATTR_DIRECTORY) == 0 && (tattr & ATTR_DIRECTORY) != 0 ) {
			er = EX_ISDIR;
			goto exit;
		}
		if ( ofile_at(v, tsect, toff) ) { er = EX_BUSY; goto exit; }
		if ( (tattr & ATTR_DIRECTORY) != 0 ) {
			er = dir_empty(v, tclus);
			if ( er < EX_OK ) goto exit;
		}
		replaced = TRUE;
	} else if ( er != EX_NOENT ) {
		goto exit;
	}

	/* 1 */
	er = buf_flush(v);
	if ( er >= EX_OK ) er = vol_touch(v);
	if ( er >= EX_OK ) er = ent_mark(v, ssect, soff, NT_REN_OLD, TRUE);
	if ( er < EX_OK ) goto exit;

	/* 2 */
	if ( replaced ) {
		/* the entry with the name takes the attributes, times, cluster
		   and size of the one moved; its name stays */
		er = buf_load(v, tsect);
		if ( er < EX_OK ) goto exit;
		e = v->buf + toff;
		e[11] = sent[11];
		e[12] |= NT_REN_NEW;
		knl_memcpy(e + 13, sent + 13, DIRENT_SIZE - 13);
		v->dirty = TRUE;
		er = buf_flush(v);
		if ( er < EX_OK ) goto exit;
		nsect = tsect;
		noff  = toff;
	} else {
		/* a new group under the new name, with the times of the old */
		er = dir_create(v, tdir, (CONST UB *)tname, tlen, sent[11], sclus, ssize,
				sent, NT_REN_NEW, &nsect, &noff);
		if ( er < EX_OK ) goto exit;
	}

	/* 3: the old group goes; a new one never lies on it */
	er = dir_erase(v, sdir, sfirst, slast);
	if ( er < EX_OK ) goto exit;

	/* 4: a directory with a new parent: its ".." names the new one */
	if ( (sattr & ATTR_DIRECTORY) != 0 && tdir != sdir && clus_valid(v, sclus) ) {
		er = buf_load(v, clus_to_sect(v, sclus));
		if ( er < EX_OK ) goto exit;
		e = v->buf + DIRENT_SIZE;
		if ( e[0] == '.' && e[1] == '.' && e[2] == ' ' ) {
			ent_set_clus(v, e, tdir);
			v->dirty = TRUE;
		}
		er = buf_flush(v);
		if ( er < EX_OK ) goto exit;
	}

	/* 5: what the entry replaced had */
	if ( replaced && clus_valid(v, tclus) && tclus != sclus ) {
		er = chain_free(v, tclus);
		if ( er >= EX_OK ) er = buf_flush(v);
		if ( er < EX_OK ) goto exit;
	}

	/* 6 */
	er = ent_mark(v, nsect, noff, NT_REN_NEW, FALSE);
	if ( er < EX_OK ) goto exit;

	/* open files of the entry moved write their size to the new one */
	for ( i = 0; i < v->nofile; i++ ) {
		if ( v->ofile[i]->dir_sect == ssect && v->ofile[i]->dir_off == soff ) {
			v->ofile[i]->dir_sect = nsect;
			v->ofile[i]->dir_off  = noff;
		}
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL ER fat_statvfs( T_MOUNT *m, T_FSSTAT *st )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);

	/* counted the first time it is asked for, kept count of after */
	if ( !v->free_known ) {
		er = free_count(v);
	}
	st->blocks = v->nclus;
	st->bfree  = v->free_cnt;
	st->bsize  = (UD)v->sec_per_clus * SECTOR_SIZE;

	tk_unl_mtx(v->mtxid);

	return er;
}

LOCAL ER fat_sync( T_MOUNT *m )
{
	FATVOL	*v = (FATVOL *)m->exinf;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = buf_flush(v);
	if ( er >= EX_OK ) er = fsi_write(v);
	tk_unl_mtx(v->mtxid);

	return er;
}

EXPORT CONST T_FIMP knl_fimp_fat = {
	.name     = "fatfs",
	.mount    = fat_mount,
	.unmount  = fat_unmount,
	.open     = fat_open,
	.close    = fat_close,
	.read     = fat_read,
	.write    = fat_write,
	.truncate = fat_truncate,
	.stat     = fat_stat,
	.getdents = fat_getdents,
	.mkdir    = fat_mkdir,
	.rmdir    = fat_rmdir,
	.unlink   = fat_unlink,
	.rename   = fat_rename,
	.statvfs  = fat_statvfs,
	.sync     = fat_sync,
	.utime    = fat_utime,
};
