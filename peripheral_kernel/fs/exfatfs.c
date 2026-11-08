/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	exfatfs.c
 *	exFAT file system implementation (fimp), design 7.6.
 *
 *	The volume: a boot region, one table of clusters (FAT), and the
 *	cluster heap, in which the bitmap of clusters in use, the up-case
 *	table and all the directories lie as files do. A directory is a list
 *	of 32 byte entries; a file is a set of them: the file entry, the
 *	stream entry (where its clusters are, its lengths) and the entries of
 *	its name, with a checksum over the set in the file entry.
 *
 *	A file's clusters are either one unbroken run, the table not used
 *	(NoFatChain), or a chain in the table. New clusters extend the run
 *	for as long as the next cluster is free; the first one that is not
 *	writes the chain for what there is, and the file goes on as a chain.
 *	Bytes past ValidDataLength read as zeros, so a file made longer
 *	without being written costs nothing.
 *
 *	A volume is written only when that is safe. It is served for reading
 *	alone when it carries the mark of not having been closed
 *	(VolumeDirty) or of a medium failure, has two tables (TexFAT), is of a
 *	major revision other than 1, or its up-case table does not match its
 *	checksum: nothing here puts such a volume right, and a guess could
 *	lose what is on it.
 *
 *	Changes go out in order: the data, the bitmap, the table, the
 *	directory entries; when clusters are freed, the entry that names them
 *	goes first. VolumeDirty is set before the first change and cleared
 *	once all changes are on the medium and no file is open for writing,
 *	so a cut in between leaves the mark for fsck.exfat or Windows to act on.
 *
 *	One volume is served at a time through a mutex.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/proc.h>
#include "fsname.h"

#define SECT			512
#define SECT_SHIFT		9
#define ENT			32

/* Boot sector */
#define BS_VOLLEN		72
#define BS_FATOFF		80
#define BS_FATLEN		84
#define BS_HEAPOFF		88
#define BS_CLUSCNT		92
#define BS_ROOT			96
#define BS_REV			104
#define BS_FLAGS		106
#define BS_SECTSH		108
#define BS_SPCSH		109
#define BS_NFATS		110
#define BS_PCT			112
#define VF_ACTIVEFAT		0x0001
#define VF_DIRTY		0x0002
#define VF_MEDIAFAIL		0x0004

/* Entry types */
#define ET_END			0x00
#define ET_BITMAP		0x81
#define ET_UPCASE		0x82
#define ET_LABEL		0x83
#define ET_FILE			0x85
#define ET_STREAM		0xC0
#define ET_NAME			0xC1
#define ET_INUSE		0x80

#define ATTR_DIR		0x0010
#define ATTR_ARC		0x0020

#define SF_ALLOC		0x01		/* AllocationPossible */
#define SF_NOFAT		0x02		/* NoFatChain */

#define CL_BAD			0xFFFFFFF7U
#define CL_END			0xFFFFFFFFU

#define NAME_MAX16		255
#define NAME_PER_ENT		15
#define SET_MAX			32		/* entries of a set handled; a name takes at most 19 */

/* Kinds of a cached sector, in the order they go to the medium */
#define CC_DATA			0
#define CC_BITMAP		1
#define CC_FAT			2
#define CC_DIR			3
#define CC_KINDS		4

#define CACHE_SLOTS		128
#define RUN_MAX			2048		/* sectors of one request */
#define FLUSH_MS		5000

typedef struct {
	UD	sect;
	UD	used;
	UB	dirty;
	UB	kind;
	UB	data[SECT];
} CSLOT;

/* Where the clusters of a file or a directory are */
typedef struct {
	UW	first;		/* 0: none yet */
	BOOL	nofat;
	UD	nclus;		/* how many it has */
	UD	hint_n;		/* a place along the chain: the hint_n'th cluster (from 0) is hint_c */
	UW	hint_c;
} EXS;

/* One file or directory in use */
typedef struct enode {
	struct enode *next;
	INT	refs;
	INT	writers;
	BOOL	root;
	UW	pfirst;		/* the directory its entries are in */
	BOOL	pnofat;
	UD	idx;		/* which entry of it the set begins at */
	EXS	s;
	UD	size;		/* DataLength */
	UD	valid;		/* ValidDataLength */
	UW	attr;
	UW	mtime_raw;	/* as read */
	UB	mtime_10ms, mtime_off;
	BOOL	edirty;		/* the entries are behind */
	BOOL	touched;	/* written: its time is now */
} ENODE;

typedef struct exvol {
	struct exvol *lnext;
	ID	dd, mtxid;
	UW	fat_off, fat_len, heap_off, clus_cnt, root_first;
	UD	vol_len;
	UINT	spc_sh;			/* sectors in a cluster, as a shift */
	UD	csize;
	UW	fat_active;		/* 0, or a table's length to step past (the second table) */
	BOOL	ro;			/* served for reading: unsafe, or asked for */
	BOOL	marked;			/* VolumeDirty is on, by us */
	BOOL	ioerr;
	UD	last_write;
	EXS	root;
	UD	bm_size;		/* bytes of the bitmap */
	UW	bm_first;
	UD	bm_hint_n;		/* a place in its chain */
	UW	bm_hint_c;
	UD	free_cnt;
	BOOL	free_known;
	UW	alloc_hint;
	UH	*upcase;		/* 65536 entries */
	CSLOT	*c;
	UD	c_tick;
	INT	c_cur;
	INT	c_dirty_n[CC_KINDS];
	ENODE	*nodes;
	UB	set[SET_MAX * ENT];	/* an entry set being worked on */
	UH	nm16[NAME_MAX16 + 1];
	UH	nm16b[NAME_MAX16 + 1];
} EXVOL;

/* What a look in a directory found */
typedef struct {
	UD	idx;
	INT	nent;		/* entries of the set */
	UW	attr;
	UW	first;
	BOOL	nofat;
	UD	size, valid;
	BOOL	others;		/* secondaries besides the stream and the name */
} EXENT;

/* A directory as a path leads to it */
typedef struct {
	BOOL	root;
	EXS	s;
	UW	pfirst;
	BOOL	pnofat;
	UD	idx;
	UD	size;
} EXDIR;

LOCAL UH rd16( CONST UB *p ) { return (UH)( p[0] | ( p[1] << 8 ) ); }
LOCAL UW rd32( CONST UB *p ) { return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 ); }
LOCAL UD rd64( CONST UB *p ) { return (UD)rd32(p) | ( (UD)rd32(p + 4) << 32 ); }
LOCAL void wr16( UB *p, UH v ) { p[0] = (UB)v; p[1] = (UB)( v >> 8 ); }
LOCAL void wr32( UB *p, UW v ) { wr16(p, (UH)v); wr16(p + 2, (UH)( v >> 16 )); }
LOCAL void wr64( UB *p, UD v ) { wr32(p, (UW)v); wr32(p + 4, (UW)( v >> 32 )); }

LOCAL UD now_ms( void )
{
	SYSTIM	t;

	if ( tk_get_otm(&t) < E_OK ) {
		return 0;
	}
	return ( (UD)(UW)t.hi << 32 ) | t.lo;
}

/* ---------------------------------------------------------------- the medium */

LOCAL ER dev_read( EXVOL *v, UD sect, void *buf, UD n )
{
	SZ	asz;

	return ( tk_srea_dev_d(v->dd, (D)sect, buf, (SZ)n * SECT, &asz) < E_OK ) ? EX_IO : EX_OK;
}

LOCAL ER dev_write( EXVOL *v, UD sect, CONST void *buf, UD n )
{
	SZ	asz;

	if ( tk_swri_dev_d(v->dd, (D)sect, buf, (SZ)n * SECT, &asz) < E_OK ) {
		v->ioerr = TRUE;
		return EX_IO;
	}
	v->last_write = now_ms();
	return EX_OK;
}

/* Everything changed to the medium, in the order of the kinds */
LOCAL ER c_flush_kinds( EXVOL *v, INT upto )
{
	INT	k, i;
	ER	er = EX_OK;

	for ( k = 0; k <= upto; k++ ) {
		if ( v->c_dirty_n[k] == 0 ) continue;
		for ( i = 0; i < CACHE_SLOTS; i++ ) {
			CSLOT	*s = &v->c[i];

			if ( s->dirty && s->kind == k ) {
				if ( dev_write(v, s->sect, s->data, 1) < EX_OK ) {
					er = EX_IO;
				}
				s->dirty = FALSE;
				v->c_dirty_n[k]--;
			}
		}
	}
	return er;
}

LOCAL ER c_flush( EXVOL *v )
{
	return c_flush_kinds(v, CC_KINDS - 1);
}

/* The sector in the cache. Its pointer holds until the next call. 'load' FALSE: it will be written whole */
LOCAL ER c_get( EXVOL *v, UD sect, INT kind, BOOL load, UB **pp )
{
	INT	i, pick = -1;
	CSLOT	*s;

	for ( i = 0; i < CACHE_SLOTS; i++ ) {
		s = &v->c[i];
		if ( s->sect == sect ) {
			s->used = ++v->c_tick;
			v->c_cur = i;
			*pp = s->data;
			return EX_OK;
		}
		if ( pick < 0 || s->used < v->c[pick].used ) pick = i;
	}
	s = &v->c[pick];
	if ( s->dirty ) {
		/* all of it and in order: what is left behind must not be ahead of what it names */
		if ( c_flush(v) < EX_OK ) {
			return EX_IO;
		}
	}
	s->sect = ~(UD)0;
	if ( load ) {
		if ( dev_read(v, sect, s->data, 1) < EX_OK ) {
			return EX_IO;
		}
	} else {
		knl_memset(s->data, 0, SECT);
	}
	s->sect = sect;
	s->kind = (UB)kind;
	s->used = ++v->c_tick;
	v->c_cur = pick;
	*pp = s->data;
	return EX_OK;
}

LOCAL void c_mark( EXVOL *v, INT kind )
{
	CSLOT	*s = &v->c[v->c_cur];

	if ( !s->dirty ) {
		s->dirty = TRUE;
		s->kind = (UB)kind;
		v->c_dirty_n[kind]++;
	}
}

/* Sectors about to be written straight: the copies kept are out of date */
LOCAL void c_forget( EXVOL *v, UD sect, UD n )
{
	INT	i;

	for ( i = 0; i < CACHE_SLOTS; i++ ) {
		CSLOT	*s = &v->c[i];

		if ( s->sect >= sect && s->sect < sect + n ) {
			if ( s->dirty ) {
				s->dirty = FALSE;
				v->c_dirty_n[s->kind]--;
			}
			s->sect = ~(UD)0;
			s->used = 0;
		}
	}
}

/* The mark set before the first change. Not in the cache: it must be out before anything else */
LOCAL ER vol_touch( EXVOL *v )
{
	UB	*p;
	ER	er;

	if ( v->ro || v->ioerr ) {
		return EX_ROFS;
	}
	if ( v->marked ) {
		return EX_OK;
	}
	er = c_get(v, 0, CC_DATA, TRUE, &p);
	if ( er < EX_OK ) return er;
	wr16(p + BS_FLAGS, (UH)( rd16(p + BS_FLAGS) | VF_DIRTY ));
	er = dev_write(v, 0, p, 1);
	if ( er >= EX_OK ) v->marked = TRUE;
	return er;
}

/* Everything out, the mark cleared */
LOCAL ER vol_clean( EXVOL *v )
{
	UB	*p;
	ER	er;

	er = c_flush(v);
	if ( er < EX_OK || !v->marked || v->ioerr ) {
		return er;
	}
	er = c_get(v, 0, CC_DATA, TRUE, &p);
	if ( er < EX_OK ) return er;
	wr16(p + BS_FLAGS, (UH)( rd16(p + BS_FLAGS) & ~VF_DIRTY ));
	p[BS_PCT] = v->free_known ? (UB)( ( (UD)v->clus_cnt - v->free_cnt ) * 100 / v->clus_cnt ) : 0xFF;
	er = dev_write(v, 0, p, 1);
	if ( er >= EX_OK ) v->marked = FALSE;
	return er;
}

/* ---------------------------------------------------------------- clusters */

LOCAL UD clus_sect( EXVOL *v, UW c )
{
	return (UD)v->heap_off + ( (UD)( c - 2 ) << v->spc_sh );
}

LOCAL BOOL clus_ok( EXVOL *v, UW c )
{
	return (BOOL)( c >= 2 && (UD)c < (UD)v->clus_cnt + 2 );
}

LOCAL ER fat_get( EXVOL *v, UW c, UW *pv )
{
	UB	*p;
	UD	off = (UD)c * 4;
	ER	er;

	er = c_get(v, (UD)v->fat_off + v->fat_active + ( off >> SECT_SHIFT ), CC_FAT, TRUE, &p);
	if ( er < EX_OK ) return er;
	*pv = rd32(p + ( off & ( SECT - 1 ) ));
	return EX_OK;
}

LOCAL ER fat_set( EXVOL *v, UW c, UW val )
{
	UB	*p;
	UD	off = (UD)c * 4;
	ER	er;

	er = c_get(v, (UD)v->fat_off + ( off >> SECT_SHIFT ), CC_FAT, TRUE, &p);
	if ( er < EX_OK ) return er;
	wr32(p + ( off & ( SECT - 1 ) ), val);
	c_mark(v, CC_FAT);
	return EX_OK;
}

/* The cluster n along a chain, from a place known */
LOCAL ER chain_walk( EXVOL *v, UW first, UD n, UD *hn, UW *hc, UW *pc )
{
	UW	c = first;
	UD	i = 0;
	ER	er;

	if ( *hc != 0 && *hn <= n ) {
		c = *hc;
		i = *hn;
	}
	for ( ; i < n; i++ ) {
		UW	next;

		er = fat_get(v, c, &next);
		if ( er < EX_OK ) return er;
		if ( !clus_ok(v, next) ) return EX_IO;
		c = next;
	}
	*hn = n;
	*hc = c;
	*pc = c;
	return EX_OK;
}

LOCAL ER s_clus( EXVOL *v, EXS *s, UD n, UW *pc )
{
	if ( s->first == 0 || n >= s->nclus ) {
		return EX_IO;
	}
	if ( s->nofat ) {
		if ( !clus_ok(v, s->first) || !clus_ok(v, (UW)( s->first + n )) ) return EX_IO;
		*pc = (UW)( s->first + n );
		return EX_OK;
	}
	return chain_walk(v, s->first, n, &s->hint_n, &s->hint_c, pc);
}

/* The bitmap's byte that holds a cluster's bit, as a sector and an offset */
LOCAL ER bm_at( EXVOL *v, UW c, UD *psect, UINT *poff )
{
	UD	byte = (UD)( c - 2 ) / 8, nc = byte / v->csize;
	UW	bc;
	ER	er;

	er = chain_walk(v, v->bm_first, nc, &v->bm_hint_n, &v->bm_hint_c, &bc);
	if ( er < EX_OK ) return er;
	*psect = clus_sect(v, bc) + ( ( byte % v->csize ) >> SECT_SHIFT );
	*poff = (UINT)( byte & ( SECT - 1 ) );
	return EX_OK;
}

LOCAL ER bm_test( EXVOL *v, UW c, BOOL *pset )
{
	UD	sect;
	UINT	off;
	UB	*p;
	ER	er;

	er = bm_at(v, c, &sect, &off);
	if ( er >= EX_OK ) er = c_get(v, sect, CC_BITMAP, TRUE, &p);
	if ( er < EX_OK ) return er;
	*pset = (BOOL)( ( p[off] >> ( ( c - 2 ) & 7 ) ) & 1 );
	return EX_OK;
}

LOCAL ER bm_put( EXVOL *v, UW c, BOOL on )
{
	UD	sect;
	UINT	off;
	UB	*p, bit = (UB)( 1 << ( ( c - 2 ) & 7 ) );
	ER	er;

	er = bm_at(v, c, &sect, &off);
	if ( er >= EX_OK ) er = c_get(v, sect, CC_BITMAP, TRUE, &p);
	if ( er < EX_OK ) return er;
	if ( on == ( ( p[off] & bit ) != 0 ) ) {
		return on ? EX_IO : EX_OK;	/* taken twice: the bitmap and the caller disagree */
	}
	p[off] = (UB)( on ? ( p[off] | bit ) : ( p[off] & ~bit ) );
	c_mark(v, CC_BITMAP);
	if ( v->free_known ) {
		v->free_cnt += on ? (UD)-1 : 1;
	}
	return EX_OK;
}

/* Counted from the bitmap once, then kept count of */
LOCAL ER free_count( EXVOL *v )
{
	UD	left = v->clus_cnt, freec = 0, at = 0;
	ER	er = EX_OK;

	if ( v->free_known ) {
		return EX_OK;
	}
	while ( left > 0 ) {
		UD	sect;
		UINT	off, i;
		UB	*p;

		er = bm_at(v, (UW)( at + 2 ), &sect, &off);
		if ( er >= EX_OK ) er = c_get(v, sect, CC_BITMAP, TRUE, &p);
		if ( er < EX_OK ) return er;
		for ( i = 0; i < SECT && left > 0; i++ ) {
			UINT	b;

			for ( b = 0; b < 8 && left > 0; b++, left-- ) {
				if ( ( p[i] & ( 1 << b ) ) == 0 ) freec++;
			}
		}
		at += SECT * 8;
	}
	v->free_cnt = freec;
	v->free_known = TRUE;
	return EX_OK;
}

/* A free cluster, from the hint on and round; marked taken */
LOCAL ER clus_alloc( EXVOL *v, UW *pc )
{
	UD	n, cnt = v->clus_cnt;
	UD	at = ( v->alloc_hint >= 2 && (UD)v->alloc_hint < cnt + 2 ) ? (UD)v->alloc_hint - 2 : 0;
	ER	er;

	for ( n = 0; n < cnt; ) {
		UD	sect;
		UINT	off, b;
		UB	*p;

		er = bm_at(v, (UW)( at + 2 ), &sect, &off);
		if ( er >= EX_OK ) er = c_get(v, sect, CC_BITMAP, TRUE, &p);
		if ( er < EX_OK ) return er;
		if ( p[off] == 0xFF ) {
			UD	step = 8 - ( at & 7 );

			n += step;
			at += step;
		} else {
			b = (UINT)( at & 7 );
			if ( ( p[off] & ( 1 << b ) ) == 0 ) {
				er = bm_put(v, (UW)( at + 2 ), TRUE);
				if ( er < EX_OK ) return er;
				*pc = (UW)( at + 2 );
				v->alloc_hint = (UW)( at + 3 );
				return EX_OK;
			}
			n++;
			at++;
		}
		if ( at >= cnt ) at = 0;
	}
	return EX_NOSPC;
}

/* What the table must say of a run that was held without it */
LOCAL ER run_to_chain( EXVOL *v, EXS *s )
{
	UD	i;
	ER	er;

	for ( i = 0; i + 1 < s->nclus; i++ ) {
		er = fat_set(v, (UW)( s->first + i ), (UW)( s->first + i + 1 ));
		if ( er < EX_OK ) return er;
	}
	if ( s->nclus > 0 ) {
		er = fat_set(v, (UW)( s->first + s->nclus - 1 ), CL_END);
		if ( er < EX_OK ) return er;
	}
	s->nofat = FALSE;
	s->hint_c = 0;
	return EX_OK;
}

/* One more cluster at the end. '*pc' is it */
LOCAL ER s_grow1( EXVOL *v, EXS *s, UW *pc )
{
	UW	c, last = 0;
	BOOL	taken = TRUE;
	ER	er;

	if ( s->nclus >= 0xFFFFFFF0U ) {
		return EX_FBIG;
	}
	if ( s->nclus > 0 ) {
		er = s_clus(v, s, s->nclus - 1, &last);
		if ( er < EX_OK ) return er;
		if ( clus_ok(v, (UW)( last + 1 )) ) {
			er = bm_test(v, (UW)( last + 1 ), &taken);
			if ( er < EX_OK ) return er;
		}
	}
	if ( !taken ) {
		c = (UW)( last + 1 );
		er = bm_put(v, c, TRUE);
	} else {
		er = clus_alloc(v, &c);
	}
	if ( er < EX_OK ) return er;

	if ( s->nclus == 0 ) {
		s->first = c;
		s->nofat = TRUE;
	} else if ( s->nofat && c != last + 1 ) {
		er = run_to_chain(v, s);
		if ( er < EX_OK ) return er;
	}
	if ( !s->nofat ) {
		er = fat_set(v, c, CL_END);
		if ( er >= EX_OK && s->nclus > 0 ) {
			er = fat_set(v, last, c);
		}
		if ( er < EX_OK ) return er;
	}
	s->nclus++;
	*pc = c;
	return EX_OK;
}

/* All but the first 'keep' clusters given back */
LOCAL ER s_cut( EXVOL *v, EXS *s, UD keep )
{
	UD	i;
	UW	c, next;
	ER	er;

	if ( keep >= s->nclus ) {
		return EX_OK;
	}
	if ( s->nofat ) {
		for ( i = keep; i < s->nclus; i++ ) {
			er = bm_put(v, (UW)( s->first + i ), FALSE);
			if ( er < EX_OK ) return er;
		}
	} else {
		er = s_clus(v, s, keep, &c);
		if ( er < EX_OK ) return er;
		if ( keep > 0 ) {
			UW	before;

			er = s_clus(v, s, keep - 1, &before);
			if ( er >= EX_OK ) er = fat_set(v, before, CL_END);
			if ( er < EX_OK ) return er;
		}
		for ( i = keep; i < s->nclus; i++ ) {
			er = fat_get(v, c, &next);
			if ( er >= EX_OK ) er = bm_put(v, c, FALSE);
			if ( er < EX_OK ) return er;
			if ( i + 1 < s->nclus ) {
				if ( !clus_ok(v, next) ) return EX_IO;
				c = next;
			}
		}
	}
	s->nclus = keep;
	s->hint_c = 0;
	if ( keep == 0 ) {
		s->first = 0;
		s->nofat = FALSE;
	}
	return EX_OK;
}

/* ---------------------------------------------------------------- time */

#define T1970			473385600ULL	/* seconds from 1970 to the origin, 1985 */

LOCAL UW ts_of_tron( UD t )
{
	UD	sec = t + T1970, days = sec / 86400, secs = sec % 86400;
	D	era, doe, yoe, y, doy, mp, d, mo;

	days += 719468;
	era = (D)days / 146097;
	doe = (D)days - era * 146097;
	yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
	y = yoe + era * 400;
	doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
	mp = ( 5 * doy + 2 ) / 153;
	d = doy - ( 153 * mp + 2 ) / 5 + 1;
	mo = mp + ( mp < 10 ? 3 : -9 );
	if ( mo <= 2 ) y++;
	if ( y < 1980 || y > 2107 ) {
		return (UW)( 46U << 25 | 1U << 21 | 1U << 16 );
	}
	return (UW)( ( (UD)( y - 1980 ) << 25 ) | ( (UD)mo << 21 ) | ( (UD)d << 16 )
		   | ( ( secs / 3600 ) << 11 ) | ( ( ( secs / 60 ) % 60 ) << 5 ) | ( ( secs % 60 ) / 2 ) );
}

LOCAL UD tron_of_ts( UW ts, UB ms10, UB off )
{
	D	y = 1980 + ( ts >> 25 ), m = ( ts >> 21 ) & 0xf, d = ( ts >> 16 ) & 0x1f;
	D	era, yoe, doy, doe, days, sec;

	if ( m < 1 || m > 12 || d < 1 ) {
		return 0;
	}
	y -= ( m <= 2 ) ? 1 : 0;
	era = y / 400;
	yoe = y - era * 400;
	doy = ( 153 * ( m + ( m > 2 ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = era * 146097 + doe - 724947;
	sec = days * 86400 + ( ( ts >> 11 ) & 0x1f ) * 3600 + ( ( ts >> 5 ) & 0x3f ) * 60
	    + ( ts & 0x1f ) * 2 + ms10 / 100;
	if ( ( off & 0x80 ) != 0 ) {
		/* the offset from UTC, in quarters of an hour, signed in seven bits */
		INT	q = off & 0x7f;

		if ( q >= 64 ) q -= 128;
		sec -= q * 900;
	}
	return ( sec < 0 ) ? 0 : (UD)sec;
}

/* ---------------------------------------------------------------- entries */

LOCAL UH set_checksum( CONST UB *set, INT nent )
{
	UH	c = 0;
	INT	i, n = nent * ENT;

	for ( i = 0; i < n; i++ ) {
		if ( i == 2 || i == 3 ) continue;
		c = (UH)( ( ( c & 1 ) ? 0x8000 : 0 ) + ( c >> 1 ) + set[i] );
	}
	return c;
}

LOCAL UH name_hash( EXVOL *v, CONST UH *nm, INT n )
{
	UH	h = 0;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		UH	u = v->upcase[nm[i]];

		h = (UH)( ( ( h & 1 ) ? 0x8000 : 0 ) + ( h >> 1 ) + ( u & 0xff ) );
		h = (UH)( ( ( h & 1 ) ? 0x8000 : 0 ) + ( h >> 1 ) + ( u >> 8 ) );
	}
	return h;
}

/* A pointer to entry idx of a directory, in the cache. Its pointer holds until the next call */
LOCAL ER ent_at( EXVOL *v, EXS *d, UD idx, BOOL dirty_it, UB **pp )
{
	UD	off = idx * ENT, nc = off / v->csize;
	UW	c;
	UB	*p;
	ER	er;

	er = s_clus(v, d, nc, &c);
	if ( er < EX_OK ) return er;
	er = c_get(v, clus_sect(v, c) + ( ( off % v->csize ) >> SECT_SHIFT ), CC_DIR, TRUE, &p);
	if ( er < EX_OK ) return er;
	if ( dirty_it ) c_mark(v, CC_DIR);
	*pp = p + ( off & ( SECT - 1 ) );
	return EX_OK;
}

/* The set at idx into v->set; FALSE in *pok when it is not one that holds together */
LOCAL ER set_read( EXVOL *v, EXS *d, UD idx, INT nent, BOOL *pok )
{
	INT	i;
	UB	*p;
	ER	er;

	*pok = FALSE;
	if ( nent < 3 || nent > SET_MAX || ( idx + (UD)nent ) * ENT > d->nclus * v->csize ) {
		return EX_OK;
	}
	for ( i = 0; i < nent; i++ ) {
		er = ent_at(v, d, idx + (UD)i, FALSE, &p);
		if ( er < EX_OK ) return er;
		knl_memcpy(v->set + i * ENT, p, ENT);
	}
	*pok = (BOOL)( v->set[0] == ET_FILE && v->set[ENT] == ET_STREAM
		    && rd16(v->set + 2) == set_checksum(v->set, nent) );
	return EX_OK;
}

/* v->set out at idx, its checksum made first */
LOCAL ER set_write( EXVOL *v, EXS *d, UD idx, INT nent )
{
	INT	i;
	UB	*p;
	ER	er;

	wr16(v->set + 2, set_checksum(v->set, nent));
	for ( i = 0; i < nent; i++ ) {
		er = ent_at(v, d, idx + (UD)i, TRUE, &p);
		if ( er < EX_OK ) return er;
		knl_memcpy(p, v->set + i * ENT, ENT);
	}
	return EX_OK;
}

LOCAL void ent_decode( EXVOL *v, UD idx, INT nent, EXENT *e )
{
	CONST UB *st = v->set + ENT;
	INT	i;

	e->idx = idx;
	e->nent = nent;
	e->attr = rd16(v->set + 4);
	e->first = rd32(st + 20);
	e->nofat = (BOOL)( ( st[1] & SF_NOFAT ) != 0 );
	e->size = rd64(st + 24);
	e->valid = rd64(st + 8);
	e->others = FALSE;
	for ( i = 2; i < nent; i++ ) {
		if ( v->set[i * ENT] != ET_NAME ) e->others = TRUE;
	}
}

LOCAL INT set_name( EXVOL *v, INT nent, UH *out )
{
	INT	len = v->set[ENT + 3], i, k = 0;

	for ( i = 2; i < nent && k < len; i++ ) {
		INT	j;

		if ( v->set[i * ENT] != ET_NAME ) continue;
		for ( j = 0; j < NAME_PER_ENT && k < len; j++ ) {
			out[k++] = rd16(v->set + i * ENT + 2 + j * 2);
		}
	}
	return k;
}

LOCAL UD clus_of_size( EXVOL *v, UD size )
{
	return ( size + v->csize - 1 ) / v->csize;
}

/*
 * The next set of a directory at or after *pidx. EX_NOENT at its end.
 * A set whose checksum does not hold is passed over.
 */
LOCAL ER set_next( EXVOL *v, EXS *d, UD *pidx, EXENT *e )
{
	UD	idx = *pidx, max = d->nclus * v->csize / ENT;
	UB	*p;
	ER	er;

	while ( idx < max ) {
		UB	type;
		INT	nent;
		BOOL	ok;

		er = ent_at(v, d, idx, FALSE, &p);
		if ( er < EX_OK ) return er;
		type = p[0];
		if ( type == ET_END ) break;
		if ( type != ET_FILE ) {
			idx++;
			continue;
		}
		nent = 1 + p[1];
		er = set_read(v, d, idx, nent, &ok);
		if ( er < EX_OK ) return er;
		if ( !ok ) {
			idx++;
			continue;
		}
		ent_decode(v, idx, nent, e);
		*pidx = idx + (UD)nent;
		return EX_OK;
	}
	*pidx = idx;
	return EX_NOENT;
}

LOCAL ER dir_find( EXVOL *v, EXS *d, CONST UH *nm, INT n, EXENT *e )
{
	UD	idx = 0;
	UH	h = name_hash(v, nm, n);
	ER	er;

	for ( ;; ) {
		INT	k, i;

		er = set_next(v, d, &idx, e);
		if ( er < EX_OK ) return er;
		if ( v->set[ENT + 3] != n || rd16(v->set + ENT + 4) != h ) continue;
		k = set_name(v, e->nent, v->nm16b);
		if ( k != n ) continue;
		for ( i = 0; i < n && v->upcase[nm[i]] == v->upcase[v->nm16b[i]]; i++ ) ;
		if ( i == n ) return EX_OK;
	}
}

/* ---------------------------------------------------------------- nodes */

LOCAL void s_from_ent( EXVOL *v, EXS *s, CONST EXENT *e )
{
	s->first = e->first;
	s->nofat = e->nofat;
	s->nclus = clus_of_size(v, e->size);
	s->hint_c = 0;
	s->hint_n = 0;
}

LOCAL ENODE *node_at( EXVOL *v, UW pfirst, UD idx )
{
	ENODE	*n;

	for ( n = v->nodes; n != NULL; n = n->next ) {
		if ( !n->root && n->pfirst == pfirst && n->idx == idx ) return n;
	}
	return NULL;
}

LOCAL ENODE *node_of_dir( EXVOL *v, UW first )
{
	ENODE	*n;

	for ( n = v->nodes; n != NULL; n = n->next ) {
		if ( ( n->attr & ATTR_DIR ) != 0 && n->s.first == first ) return n;
	}
	return NULL;
}

/* The entries of a node brought up to it */
LOCAL ER node_put( EXVOL *v, ENODE *n )
{
	EXS	d;
	UB	*p;
	INT	nent;
	BOOL	ok;
	UH	flags;
	ER	er;

	if ( n->root || !n->edirty ) {
		return EX_OK;
	}
	knl_memset(&d, 0, sizeof(d));
	d.first = n->pfirst;
	d.nofat = n->pnofat;
	d.nclus = ~(UW)0;
	er = ent_at(v, &d, n->idx, FALSE, &p);
	if ( er < EX_OK ) return er;
	nent = 1 + p[1];
	er = set_read(v, &d, n->idx, nent, &ok);
	if ( er < EX_OK ) return er;
	if ( !ok ) return EX_IO;
	flags = v->set[ENT + 1];
	flags = (UH)( ( flags & ~( SF_NOFAT | SF_ALLOC ) ) | SF_ALLOC | ( n->s.nofat ? SF_NOFAT : 0 ) );
	v->set[ENT + 1] = (UB)flags;
	wr64(v->set + ENT + 8, n->valid);
	wr32(v->set + ENT + 20, n->s.first);
	wr64(v->set + ENT + 24, n->size);
	if ( n->touched ) {
		SYSTIM	t;
		UW	ts;

		ts = ( tk_get_tim(&t) >= E_OK ) ? ts_of_tron(( ( (UD)(UW)t.hi << 32 ) | t.lo ) / 1000) : rd32(v->set + 12);
		wr32(v->set + 12, ts);
		v->set[21] = 0;
		v->set[23] = 0x80;
		wr32(v->set + 16, ts);		/* LastAccessed */
		v->set[24] = 0x80;
	}
	er = set_write(v, &d, n->idx, nent);
	if ( er >= EX_OK ) {
		n->edirty = FALSE;
		n->touched = FALSE;
	}
	return er;
}

LOCAL ER nodes_put( EXVOL *v )
{
	ENODE	*n;
	ER	er = EX_OK;

	for ( n = v->nodes; n != NULL; n = n->next ) {
		ER	e = node_put(v, n);

		if ( e < EX_OK ) er = e;
	}
	return er;
}

/* ---------------------------------------------------------------- paths */

/* One name of a path as UTF-16 in v->nm16, its length in *pn. '.'-free, checked for what exFAT forbids */
LOCAL ER name_conv( EXVOL *v, CONST char *s, INT len, INT *pn )
{
	INT	n, i;

	n = knl_fs_utf8_to_16((CONST UB *)s, len, v->nm16, NAME_MAX16);
	if ( n == -2 ) return EX_NAMETOOLONG;
	if ( n <= 0 ) return EX_INVAL;
	for ( i = 0; i < n; i++ ) {
		UH	c = v->nm16[i];

		if ( c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>'
		  || c == '?' || c == '\\' || c == '|' ) {
			return EX_INVAL;
		}
	}
	*pn = n;
	return EX_OK;
}

LOCAL void dir_root( EXVOL *v, EXDIR *d )
{
	knl_memset(d, 0, sizeof(*d));
	d->root = TRUE;
	d->s = v->root;
	d->size = v->root.nclus * v->csize;
}

LOCAL void dir_of_ent( EXVOL *v, EXDIR *d, CONST EXDIR *parent, CONST EXENT *e )
{
	d->root = FALSE;
	s_from_ent(v, &d->s, e);
	d->pfirst = parent->s.first;
	d->pnofat = parent->s.nofat;
	d->idx = e->idx;
	d->size = e->size;
}

/*
 * Down a path. All of it: *pe is the last name's entry (*pisroot for the
 * root itself), *pd the directory it is in. With p_last: all but the last
 * name, *pd the directory it is to be in, *p_last and *p_lastlen the name.
 * 'avoid' a directory (its first cluster) the walk must not go through.
 */
LOCAL ER path_walk( EXVOL *v, CONST char *path, EXDIR *pd, EXENT *pe, BOOL *pisroot,
		    CONST char **p_last, INT *p_lastlen, UW avoid )
{
	CONST char *p = path;
	EXDIR	cur;
	EXENT	e;
	BOOL	have = FALSE;
	ER	er;

	if ( *p != '/' ) return EX_INVAL;
	dir_root(v, &cur);
	if ( pisroot != NULL ) *pisroot = FALSE;
	for ( ;; ) {
		CONST char *comp;
		INT	len, n;

		while ( *p == '/' ) p++;
		if ( *p == 0 ) {
			break;
		}
		comp = p;
		while ( *p != 0 && *p != '/' ) p++;
		len = (INT)( p - comp );
		if ( len == 1 && comp[0] == '.' ) continue;
		if ( len == 2 && comp[0] == '.' && comp[1] == '.' ) return EX_INVAL;
		{
			CONST char *q = p;

			while ( *q == '/' ) q++;
			if ( *q == 0 && p_last != NULL ) {
				*p_last = comp;
				*p_lastlen = len;
				*pd = cur;
				return EX_OK;
			}
		}
		if ( have ) {
			EXDIR	nx;

			if ( ( e.attr & ATTR_DIR ) == 0 ) return EX_NOTDIR;
			dir_of_ent(v, &nx, &cur, &e);
			cur = nx;
			if ( avoid != 0 && cur.s.first == avoid ) return EX_INVAL;
		}
		er = name_conv(v, comp, len, &n);
		if ( er < EX_OK ) return er;
		er = dir_find(v, &cur.s, v->nm16, n, &e);
		if ( er < EX_OK ) return er;
		have = TRUE;
		/* with p_last, what was found is a directory on the way */
		if ( p_last != NULL ) {
			EXDIR	nx;

			if ( ( e.attr & ATTR_DIR ) == 0 ) return EX_NOTDIR;
			dir_of_ent(v, &nx, &cur, &e);
			cur = nx;
			if ( avoid != 0 && cur.s.first == avoid ) return EX_INVAL;
			have = FALSE;
		}
	}
	if ( p_last != NULL ) {
		return EX_INVAL;	/* the root has no name to make */
	}
	if ( !have ) {
		if ( pisroot != NULL ) *pisroot = TRUE;
		*pd = cur;
		return EX_OK;
	}
	*pd = cur;
	*pe = e;
	return EX_OK;
}

/* ---------------------------------------------------------------- making and erasing entries */

/* A directory one cluster longer, zeroed, and what names it says so */
LOCAL ER dir_grow( EXVOL *v, EXDIR *d )
{
	UW	c;
	UD	i;
	ER	er;

	er = s_grow1(v, &d->s, &c);
	if ( er < EX_OK ) return er;
	for ( i = 0; i < ( (UD)1 << v->spc_sh ); i++ ) {
		UB	*p;

		er = c_get(v, clus_sect(v, c) + i, CC_DIR, FALSE, &p);
		if ( er < EX_OK ) return er;
		knl_memset(p, 0, SECT);
		c_mark(v, CC_DIR);
	}
	d->size = d->s.nclus * v->csize;
	if ( d->root ) {
		v->root = d->s;
	} else {
		ENODE	*n = node_of_dir(v, d->s.first);
		EXS	pd;
		UB	*p;
		INT	nent;
		BOOL	ok;

		if ( n != NULL ) {
			n->s = d->s;
			n->size = n->valid = d->size;
		}
		knl_memset(&pd, 0, sizeof(pd));
		pd.first = d->pfirst;
		pd.nofat = d->pnofat;
		pd.nclus = ~(UW)0;
		er = ent_at(v, &pd, d->idx, FALSE, &p);
		if ( er < EX_OK ) return er;
		nent = 1 + p[1];
		er = set_read(v, &pd, d->idx, nent, &ok);
		if ( er < EX_OK ) return er;
		if ( !ok ) return EX_IO;
		v->set[ENT + 1] = (UB)( SF_ALLOC | ( d->s.nofat ? SF_NOFAT : 0 ) );
		wr64(v->set + ENT + 8, d->size);
		wr32(v->set + ENT + 20, d->s.first);
		wr64(v->set + ENT + 24, d->size);
		er = set_write(v, &pd, d->idx, nent);
	}
	return er;
}

/* Room for 'want' entries one after another; the directory grows when it must */
LOCAL ER dir_room( EXVOL *v, EXDIR *d, INT want, UD *pidx )
{
	UD	idx = 0, run = 0, start = 0, max;
	UB	*p;
	ER	er;

	for ( ;; ) {
		max = d->s.nclus * v->csize / ENT;
		if ( idx >= max ) {
			if ( max >= 256ULL * 1024 * 1024 / ENT ) return EX_NOSPC;	/* the limit of a directory */
			er = dir_grow(v, d);
			if ( er < EX_OK ) return er;
			continue;
		}
		er = ent_at(v, &d->s, idx, FALSE, &p);
		if ( er < EX_OK ) return er;
		if ( p[0] < ET_INUSE ) {
			if ( run == 0 ) start = idx;
			run++;
			if ( run >= (UD)want ) {
				*pidx = start;
				return EX_OK;
			}
		} else {
			run = 0;
		}
		idx++;
	}
}

/* A new set made in v->set: stream at offset ENT, name from nm/n; returns its entries */
LOCAL INT set_make( EXVOL *v, UW attr, UW first, BOOL nofat, UD size, UD valid, CONST UH *nm, INT n,
		    CONST UB *times )
{
	INT	nnames = ( n + NAME_PER_ENT - 1 ) / NAME_PER_ENT, nent = 2 + nnames, i;
	UB	*e0 = v->set, *e1 = v->set + ENT;

	knl_memset(v->set, 0, (SZ)nent * ENT);
	e0[0] = ET_FILE;
	e0[1] = (UB)( nent - 1 );
	wr16(e0 + 4, (UH)attr);
	if ( times != NULL ) {
		knl_memcpy(e0 + 8, times, 24 - 8 + 3);
	} else {
		SYSTIM	t;
		UW	ts = ( tk_get_tim(&t) >= E_OK ) ? ts_of_tron(( ( (UD)(UW)t.hi << 32 ) | t.lo ) / 1000)
							: ts_of_tron(0);

		wr32(e0 + 8, ts);
		wr32(e0 + 12, ts);
		wr32(e0 + 16, ts);
		e0[22] = e0[23] = e0[24] = 0x80;
	}
	e1[0] = ET_STREAM;
	e1[1] = (UB)( SF_ALLOC | ( nofat ? SF_NOFAT : 0 ) );
	e1[3] = (UB)n;
	wr16(e1 + 4, name_hash(v, nm, n));
	wr64(e1 + 8, valid);
	wr32(e1 + 20, first);
	wr64(e1 + 24, size);
	for ( i = 0; i < n; i++ ) {
		UB	*e = v->set + ( 2 + i / NAME_PER_ENT ) * ENT;

		e[0] = ET_NAME;
		wr16(e + 2 + ( i % NAME_PER_ENT ) * 2, nm[i]);
	}
	for ( i = 0; i < nnames; i++ ) {
		v->set[( 2 + i ) * ENT] = ET_NAME;
	}
	return nent;
}

LOCAL ER set_erase( EXVOL *v, EXS *d, UD idx, INT nent )
{
	INT	i;
	UB	*p;
	ER	er;

	for ( i = 0; i < nent; i++ ) {
		er = ent_at(v, d, idx + (UD)i, TRUE, &p);
		if ( er < EX_OK ) return er;
		p[0] &= (UB)~ET_INUSE;
	}
	return EX_OK;
}

LOCAL ER dir_empty( EXVOL *v, CONST EXENT *e )
{
	EXS	s;
	UD	idx = 0;
	EXENT	x;
	ER	er;

	s_from_ent(v, &s, e);
	er = set_next(v, &s, &idx, &x);
	if ( er == EX_NOENT ) return EX_OK;
	return ( er < EX_OK ) ? er : EX_NOTEMPTY;
}

/* ---------------------------------------------------------------- fimp calls */

LOCAL void vols_add( EXVOL *v );
LOCAL void vols_del( EXVOL *v );

/* The table of up-case: expanded to one entry per code, identity where it is compressed */
LOCAL ER upcase_load( EXVOL *v, UW first, UD bytes, UW want_sum, BOOL *pgood )
{
	UD	got = 0, code = 0, nc;
	UW	sum = 0, c = first;
	BOOL	skip = FALSE;
	UD	i;
	ER	er;

	*pgood = FALSE;
	for ( i = 0; i < 65536; i++ ) v->upcase[i] = (UH)i;
	if ( bytes == 0 || bytes > 2 * 65536 * 2 || ( bytes & 1 ) != 0 ) {
		return EX_OK;
	}
	nc = 0;
	while ( got < bytes ) {
		UD	s;

		if ( nc > 0 ) {
			UW	next;

			er = fat_get(v, c, &next);
			if ( er < EX_OK ) return er;
			if ( !clus_ok(v, next) ) return EX_OK;
			c = next;
		}
		nc++;
		for ( s = 0; s < ( (UD)1 << v->spc_sh ) && got < bytes; s++ ) {
			UB	*p;
			UINT	k;

			er = c_get(v, clus_sect(v, c) + s, CC_DIR, TRUE, &p);
			if ( er < EX_OK ) return er;
			for ( k = 0; k < SECT && got < bytes; k += 2, got += 2 ) {
				UH	u = rd16(p + k);

				sum = ( ( sum & 1 ) ? 0x80000000U : 0 ) + ( sum >> 1 ) + p[k];
				sum = ( ( sum & 1 ) ? 0x80000000U : 0 ) + ( sum >> 1 ) + p[k + 1];
				if ( skip ) {
					code += u;
					skip = FALSE;
				} else if ( u == 0xFFFF ) {
					skip = TRUE;
				} else if ( code < 65536 ) {
					v->upcase[code++] = u;
				}
			}
		}
	}
	*pgood = (BOOL)( sum == want_sum );
	return EX_OK;
}

LOCAL ER ex_mount( T_MOUNT *m )
{
	EXVOL	*v;
	UB	*b = NULL;
	UD	sum_i;
	UW	sum = 0;
	T_CMTX	cm;
	ER	er = EX_INVAL;
	UD	idx, max;
	UW	up_first = 0, up_sum = 0;
	UD	up_len = 0;
	UINT	i;
	UH	flags;
	BOOL	good, unsafe = FALSE;

	v = (EXVOL *)Kmalloc(sizeof(EXVOL));
	if ( v == NULL ) return EX_NOMEM;
	knl_memset(v, 0, sizeof(*v));
	v->dd = m->dd;
	v->mtxid = -1;
	v->c = (CSLOT *)Kmalloc(sizeof(CSLOT) * CACHE_SLOTS);
	v->upcase = (UH *)Kmalloc(65536 * sizeof(UH));
	b = (UB *)Kmalloc(SECT * 12);
	if ( v->c == NULL || v->upcase == NULL || b == NULL ) {
		er = EX_NOMEM;
		goto err;
	}
	for ( i = 0; i < CACHE_SLOTS; i++ ) {
		v->c[i].sect = ~(UD)0;
		v->c[i].used = 0;
		v->c[i].dirty = FALSE;
	}
	if ( dev_read(v, 0, b, 12) < EX_OK ) {
		er = EX_IO;
		goto err;
	}
	for ( i = 0; i < 8; i++ ) {
		if ( b[3 + i] != (UB)"EXFAT   "[i] ) goto err;
	}
	if ( rd16(b + 510) != 0xAA55 ) goto err;

	/* the boot region's checksum: of 11 sectors, the flags and the percentage left out */
	for ( sum_i = 0; sum_i < 11 * SECT; sum_i++ ) {
		if ( sum_i == BS_FLAGS || sum_i == BS_FLAGS + 1 || sum_i == BS_PCT ) continue;
		sum = ( ( sum & 1 ) ? 0x80000000U : 0 ) + ( sum >> 1 ) + b[sum_i];
	}
	if ( sum != rd32(b + 11 * SECT) ) {
		tm_printf((UB *)"exfatfs: the boot region's checksum does not hold\n");
		goto err;
	}
	if ( b[BS_SECTSH] != SECT_SHIFT || b[BS_SPCSH] > 25 - SECT_SHIFT ) {
		tm_printf((UB *)"exfatfs: a sector of %d bytes is not handled\n", 1 << b[BS_SECTSH]);
		er = EX_NOTSUP;
		goto err;
	}
	v->vol_len = rd64(b + BS_VOLLEN);
	v->fat_off = rd32(b + BS_FATOFF);
	v->fat_len = rd32(b + BS_FATLEN);
	v->heap_off = rd32(b + BS_HEAPOFF);
	v->clus_cnt = rd32(b + BS_CLUSCNT);
	v->root_first = rd32(b + BS_ROOT);
	v->spc_sh = b[BS_SPCSH];
	v->csize = (UD)SECT << v->spc_sh;
	flags = rd16(b + BS_FLAGS);
	if ( v->clus_cnt == 0 || v->clus_cnt > 0xFFFFFFF5U || !clus_ok(v, v->root_first)
	  || ( (UD)v->clus_cnt + 2 ) * 4 > (UD)v->fat_len * SECT
	  || (UD)v->heap_off + ( (UD)v->clus_cnt << v->spc_sh ) > v->vol_len ) {
		goto err;
	}
	if ( b[BS_NFATS] != 1 ) {
		unsafe = TRUE;
		if ( ( flags & VF_ACTIVEFAT ) != 0 ) v->fat_active = v->fat_len;
	}
	if ( b[BS_REV + 1] != 1 || ( flags & ( VF_DIRTY | VF_MEDIAFAIL ) ) != 0 ) {
		unsafe = TRUE;
	}

	er = tk_cre_mtx(( cm.exinf = NULL, cm.mtxatr = TA_TFIFO, &cm ));
	if ( er < E_OK ) {
		er = EX_NOMEM;
		goto err;
	}
	v->mtxid = er;

	/* the root: its length is its chain's */
	v->root.first = v->root_first;
	v->root.nofat = FALSE;
	{
		UW	c = v->root_first, next;
		UD	n = 1;

		for ( ;; ) {
			er = fat_get(v, c, &next);
			if ( er < EX_OK ) goto err;
			if ( next == CL_END ) break;
			if ( !clus_ok(v, next) || n >= v->clus_cnt ) {
				er = EX_IO;
				goto err;
			}
			c = next;
			n++;
		}
		v->root.nclus = n;
	}

	/* the bitmap and the up-case table are named in the root */
	max = v->root.nclus * v->csize / ENT;
	for ( idx = 0; idx < max; idx++ ) {
		UB	*p;

		er = ent_at(v, &v->root, idx, FALSE, &p);
		if ( er < EX_OK ) goto err;
		if ( p[0] == ET_END ) break;
		if ( p[0] == ET_BITMAP && ( p[1] & 1 ) == 0 ) {
			v->bm_first = rd32(p + 20);
			v->bm_size = rd64(p + 24);
		} else if ( p[0] == ET_UPCASE ) {
			up_sum = rd32(p + 4);
			up_first = rd32(p + 20);
			up_len = rd64(p + 24);
		}
	}
	if ( !clus_ok(v, v->bm_first) || v->bm_size < ( (UD)v->clus_cnt + 7 ) / 8 || !clus_ok(v, up_first) ) {
		tm_printf((UB *)"exfatfs: no bitmap or up-case table in the root\n");
		er = EX_INVAL;
		goto err;
	}
	er = upcase_load(v, up_first, up_len, up_sum, &good);
	if ( er < EX_OK ) goto err;
	if ( !good ) unsafe = TRUE;

	if ( unsafe && ( m->flags & FS_MNT_RDONLY ) == 0 ) {
		tm_printf((UB *)"exfatfs: %s: not safe to write (flags %x, tables %d, revision %d, up-case %s): read only\n",
			  m->dev, (UINT)flags, b[BS_NFATS], b[BS_REV + 1], good ? "good" : "bad");
	}
	v->ro = (BOOL)( unsafe || ( m->flags & FS_MNT_RDONLY ) != 0 );
	Kfree(b);
	m->exinf = v;
	vols_add(v);
	return EX_OK;

    err:
	if ( v->mtxid > 0 ) tk_del_mtx(v->mtxid);
	if ( b != NULL ) Kfree(b);
	if ( v->c != NULL ) Kfree(v->c);
	if ( v->upcase != NULL ) Kfree(v->upcase);
	Kfree(v);
	return ( er < EX_OK ) ? er : EX_INVAL;
}

LOCAL ER ex_unmount( T_MOUNT *m )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	ER	er = EX_OK;

	vols_del(v);
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( !v->ro ) {
		(void)nodes_put(v);
		er = vol_clean(v);
	}
	while ( v->nodes != NULL ) {
		ENODE	*n = v->nodes;

		v->nodes = n->next;
		Kfree(n);
	}
	tk_unl_mtx(v->mtxid);
	tk_del_mtx(v->mtxid);
	Kfree(v->c);
	Kfree(v->upcase);
	Kfree(v);
	m->exinf = NULL;
	return er;
}

LOCAL UD ino_of( CONST EXENT *e, UW pfirst )
{
	return ( e->first != 0 ) ? e->first : ( ( (UD)pfirst << 32 ) | e->idx );
}

LOCAL ENODE *node_new( EXVOL *v )
{
	ENODE	*n = (ENODE *)Kmalloc(sizeof(ENODE));

	if ( n != NULL ) {
		knl_memset(n, 0, sizeof(*n));
		n->refs = 1;
		n->next = v->nodes;
		v->nodes = n;
	}
	return n;
}

LOCAL void node_unuse( EXVOL *v, ENODE *n )
{
	ENODE	**pp;

	if ( --n->refs > 0 ) return;
	for ( pp = &v->nodes; *pp != n; pp = &(*pp)->next ) ;
	*pp = n->next;
	Kfree(n);
}

LOCAL ER ex_open( T_MOUNT *m, CONST char *path, UINT oflags, T_FILE *f )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	d;
	EXENT	e;
	BOOL	isroot;
	ENODE	*n = NULL;
	BOOL	wr = (BOOL)( ( oflags & O_ACCMODE ) != O_RDONLY );
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( wr && v->ro ) {
		er = EX_ROFS;
		goto exit;
	}
	er = path_walk(v, path, &d, &e, &isroot, NULL, NULL, 0);
	if ( er == EX_NOENT && ( oflags & O_CREAT ) != 0 ) {
		CONST char *last;
		INT	len, k;

		if ( v->ro ) {
			er = EX_ROFS;
			goto exit;
		}
		er = path_walk(v, path, &d, NULL, NULL, &last, &len, 0);
		if ( er < EX_OK ) goto exit;
		er = name_conv(v, last, len, &k);
		if ( er < EX_OK ) goto exit;
		er = vol_touch(v);
		if ( er < EX_OK ) goto exit;
		{
			INT	want = 2 + ( k + NAME_PER_ENT - 1 ) / NAME_PER_ENT;

			er = dir_room(v, &d, want, &e.idx);
			if ( er < EX_OK ) goto exit;
			/* looking for room used the cache only; v->nm16 is untouched */
			want = set_make(v, ATTR_ARC, 0, FALSE, 0, 0, v->nm16, k, NULL);
			er = set_write(v, &d.s, e.idx, want);
			if ( er < EX_OK ) goto exit;
			ent_decode(v, e.idx, want, &e);
		}
		isroot = FALSE;
	} else if ( er == EX_OK && ( oflags & ( O_CREAT | O_EXCL ) ) == ( O_CREAT | O_EXCL ) ) {
		er = EX_EXIST;
		goto exit;
	}
	if ( er < EX_OK ) goto exit;

	if ( isroot ) {
		if ( wr ) {
			er = EX_ISDIR;
			goto exit;
		}
		n = node_new(v);
		if ( n == NULL ) {
			er = EX_NOMEM;
			goto exit;
		}
		n->root = TRUE;
		n->s = v->root;
		n->attr = ATTR_DIR;
		f->mode = FS_IFDIR;
		f->size = 0;
		f->ino = v->root.first;
	} else {
		BOOL	isdir = (BOOL)( ( e.attr & ATTR_DIR ) != 0 );

		if ( isdir && wr ) {
			er = EX_ISDIR;
			goto exit;
		}
		if ( !isdir && ( oflags & O_DIRECTORY ) != 0 ) {
			er = EX_NOTDIR;
			goto exit;
		}
		n = node_at(v, d.s.first, e.idx);
		if ( n != NULL ) {
			n->refs++;
		} else {
			n = node_new(v);
			if ( n == NULL ) {
				er = EX_NOMEM;
				goto exit;
			}
			n->pfirst = d.s.first;
			n->pnofat = d.s.nofat;
			n->idx = e.idx;
			s_from_ent(v, &n->s, &e);
			n->size = e.size;
			n->valid = ( e.valid <= e.size ) ? e.valid : e.size;
			n->attr = e.attr;
		}
		if ( wr ) n->writers++;
		f->mode = isdir ? FS_IFDIR : FS_IFREG;
		f->size = isdir ? 0 : n->size;
		f->ino = ino_of(&e, d.s.first);
		if ( !isdir && ( oflags & O_TRUNC ) != 0 && n->size > 0 ) {
			er = vol_touch(v);
			if ( er >= EX_OK ) {
				EXS	gone = n->s;

				n->s.first = 0;
				n->s.nofat = FALSE;
				n->s.nclus = 0;
				n->s.hint_c = 0;
				n->size = n->valid = 0;
				n->edirty = n->touched = TRUE;
				er = node_put(v, n);
				if ( er >= EX_OK ) er = c_flush(v);
				if ( er >= EX_OK ) er = s_cut(v, &gone, 0);
				f->size = 0;
			}
			if ( er < EX_OK ) {
				if ( wr ) n->writers--;
				node_unuse(v, n);
				n = NULL;
				goto exit;
			}
		}
	}
	f->obj = n;
	f->priv = 0;
	f->dir_sect = 0;
	f->dir_off = 0;
	er = EX_OK;

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_close( T_FILE *f )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ENODE	*n = (ENODE *)f->obj;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( !v->ro ) {
		er = node_put(v, n);
		if ( er >= EX_OK ) er = c_flush(v);
	}
	if ( ( f->oflags & O_ACCMODE ) != O_RDONLY && n->writers > 0 ) {
		n->writers--;
	}
	node_unuse(v, n);
	f->obj = NULL;
	tk_unl_mtx(v->mtxid);
	return er;
}

/*
 * Whole sectors straight to or from a caller's memory, as much as the
 * memory lets go at once: its own address when the kernel can find where
 * it lies, a view of a process's pages otherwise. NULL when not a sector
 * can go that way, and it goes through the cache.
 */
LOCAL UB *dma_at( UB *p, UD n, BOOL into, UD *pn )
{
	UD	at = 0, want = n * SECT;
	void	*pa, *k = NULL;
	INT	r;

	*pn = 0;
	if ( want > (UD)RUN_MAX * SECT ) want = (UD)RUN_MAX * SECT;
	while ( at < want ) {
		pa = NULL;
		r = ConvPhysicalAddress(p + at, (INT)( want - at ), &pa);
		if ( r <= 0 || pa == NULL ) break;
		at += (UD)r;
	}
	if ( at >= SECT ) {
		*pn = at / SECT;
		return p;
	}
	r = knl_prc_dma(p, (INT)want, into, &k);
	if ( r < SECT || k == NULL ) {
		return NULL;
	}
	*pn = (UD)r / SECT;
	return (UB *)k;
}

/* How many sectors from file offset 'off' lie one after another, up to 'want' */
LOCAL UD sect_run( EXVOL *v, ENODE *n, UD off, UW c, UD want )
{
	UD	sects = ( (UD)1 << v->spc_sh ), in = ( off % v->csize ) >> SECT_SHIFT, run = sects - in;
	UD	nc = off / v->csize + 1;

	while ( run < want && nc < n->s.nclus ) {
		UW	next;

		if ( s_clus(v, &n->s, nc, &next) < EX_OK || next != c + 1 ) break;
		c = next;
		nc++;
		run += sects;
	}
	return ( run < want ) ? run : want;
}

LOCAL INT ex_read( T_FILE *f, void *buf, SZ len )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ENODE	*n = (ENODE *)f->obj;
	UB	*dst = (UB *)buf;
	SZ	done = 0;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( f->offset >= n->size ) {
		tk_unl_mtx(v->mtxid);
		return 0;
	}
	if ( len > (SZ)( n->size - f->offset ) ) len = (SZ)( n->size - f->offset );
	if ( len > 0x40000000 ) len = 0x40000000;
	while ( done < len ) {
		UD	off = f->offset;
		SZ	w;

		if ( off >= n->valid ) {
			/* never written */
			knl_memset(dst + done, 0, len - done);
			f->offset += (UD)( len - done );
			done = len;
			break;
		}
		{
			UD	soff = off & ( SECT - 1 ), reach;
			UW	c;
			UD	sect;
			UB	*x;

			w = SECT - (SZ)soff;
			if ( w > len - done ) w = len - done;
			if ( (UD)w > n->valid - off ) w = (SZ)( n->valid - off );
			er = s_clus(v, &n->s, off / v->csize, &c);
			if ( er < EX_OK ) break;
			sect = clus_sect(v, c) + ( ( off % v->csize ) >> SECT_SHIFT );
			if ( soff == 0 && w == SECT
			  && ( x = dma_at(dst + done, (UD)( ( len - done ) / SECT ), TRUE, &reach) ) != NULL && reach > 0 ) {
				UD	lim = ( n->valid - off ) / SECT, run;

				if ( reach > lim ) reach = lim;
				run = sect_run(v, n, off, c, reach);
				if ( v->c_dirty_n[CC_DATA] > 0 ) {
					er = c_flush_kinds(v, CC_DATA);
					if ( er < EX_OK ) break;
				}
				if ( dev_read(v, sect, x, run) < EX_OK ) {
					er = EX_IO;
					break;
				}
				w = (SZ)run * SECT;
			} else {
				UB	*p;

				er = c_get(v, sect, CC_DATA, TRUE, &p);
				if ( er < EX_OK ) break;
				knl_memcpy(dst + done, p + soff, w);
			}
		}
		done += w;
		f->offset += (UD)w;
	}
	tk_unl_mtx(v->mtxid);
	if ( done == 0 && er < EX_OK ) return (INT)er;
	return (INT)done;
}

/* Bytes [from, to) of a file's clusters made zeros, through the cache */
LOCAL ER zero_range( EXVOL *v, ENODE *n, UD from, UD to )
{
	ER	er;

	while ( from < to ) {
		UD	soff = from & ( SECT - 1 ), w = SECT - soff;
		UW	c;
		UB	*p;

		if ( w > to - from ) w = to - from;
		er = s_clus(v, &n->s, from / v->csize, &c);
		if ( er >= EX_OK ) {
			er = c_get(v, clus_sect(v, c) + ( ( from % v->csize ) >> SECT_SHIFT ), CC_DATA, soff != 0 || w != SECT, &p);
		}
		if ( er < EX_OK ) return er;
		knl_memset(p + soff, 0, (SZ)w);
		c_mark(v, CC_DATA);
		from += w;
	}
	return EX_OK;
}

LOCAL INT ex_write( T_FILE *f, CONST void *buf, SZ len )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ENODE	*n = (ENODE *)f->obj;
	CONST UB *src = (CONST UB *)buf;
	SZ	done = 0;
	UD	off, end, want;
	ER	er;

	if ( len <= 0 ) return 0;
	if ( len > 0x40000000 ) len = 0x40000000;
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	if ( ( f->oflags & O_APPEND ) != 0 ) {
		f->offset = n->size;
	}
	off = f->offset;
	end = off + (UD)len;

	/* the clusters first: a volume that runs out takes what fits */
	want = clus_of_size(v, end);
	while ( n->s.nclus < want ) {
		UW	c;

		er = s_grow1(v, &n->s, &c);
		if ( er < EX_OK ) break;
	}
	if ( er < EX_OK ) {
		UD	have = n->s.nclus * v->csize;

		if ( er != EX_NOSPC || have <= off ) goto exit;
		len = (SZ)( have - off );
		end = have;
		er = EX_OK;
	}

	/* what lies between the last byte written and here is to read as zeros */
	if ( off > n->valid ) {
		er = zero_range(v, n, n->valid, off);
		if ( er < EX_OK ) goto exit;
	}
	while ( done < len ) {
		UD	soff = off & ( SECT - 1 ), reach;
		SZ	w = SECT - (SZ)soff;
		UW	c;
		UD	sect;
		UB	*x;

		if ( w > len - done ) w = len - done;
		er = s_clus(v, &n->s, off / v->csize, &c);
		if ( er < EX_OK ) break;
		sect = clus_sect(v, c) + ( ( off % v->csize ) >> SECT_SHIFT );
		if ( soff == 0 && w == SECT
		  && ( x = dma_at((UB *)src + done, (UD)( ( len - done ) / SECT ), FALSE, &reach) ) != NULL && reach > 0 ) {
			UD	run = sect_run(v, n, off, c, reach);

			c_forget(v, sect, run);
			if ( dev_write(v, sect, x, run) < EX_OK ) {
				er = EX_IO;
				break;
			}
			w = (SZ)run * SECT;
		} else {
			UB	*p;

			/* a sector no byte of which was valid is not worth reading */
			er = c_get(v, sect, CC_DATA, !( soff == 0 && off >= n->valid ), &p);
			if ( er < EX_OK ) break;
			knl_memcpy(p + soff, src + done, w);
			c_mark(v, CC_DATA);
		}
		done += w;
		off += (UD)w;
		f->offset = off;
		if ( off > n->valid ) n->valid = off;
		if ( off > n->size ) n->size = off;
	}
	if ( done > 0 || n->s.nclus != clus_of_size(v, n->size) ) {
		n->edirty = n->touched = TRUE;
	}
	/*
	 * Clusters taken beyond what was written (a request that failed part
	 * way) would be counted against a size that does not reach them
	 */
	if ( n->s.nclus > clus_of_size(v, n->size) ) {
		(void)s_cut(v, &n->s, clus_of_size(v, n->size));
	}
	f->size = n->size;

    exit:
	tk_unl_mtx(v->mtxid);
	if ( done == 0 && er < EX_OK ) return (INT)er;
	return (INT)done;
}

LOCAL ER ex_truncate( T_FILE *f, UD len )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ENODE	*n = (ENODE *)f->obj;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	if ( len < n->size ) {
		UD	keep = clus_of_size(v, len);
		EXS	s = n->s;

		n->size = len;
		if ( n->valid > len ) n->valid = len;
		if ( keep == 0 ) {
			n->s.first = 0;
			n->s.nofat = FALSE;
			n->s.nclus = 0;
			n->s.hint_c = 0;
		}
		n->edirty = n->touched = TRUE;
		/* the entry lets go of the clusters before they are freed */
		er = node_put(v, n);
		if ( er >= EX_OK ) er = c_flush(v);
		if ( er >= EX_OK ) {
			er = s_cut(v, &s, keep);
			if ( keep > 0 ) n->s = s;
		}
	} else if ( len > n->size ) {
		UD	want = clus_of_size(v, len);

		while ( n->s.nclus < want && er >= EX_OK ) {
			UW	c;

			er = s_grow1(v, &n->s, &c);
		}
		if ( er >= EX_OK ) {
			n->size = len;		/* the rest reads as zeros: ValidDataLength stays */
			n->edirty = n->touched = TRUE;
		} else {
			(void)s_cut(v, &n->s, clus_of_size(v, n->size));
		}
	}
	f->size = n->size;
	if ( er >= EX_OK ) er = c_flush(v);

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL void fill_stat( EXVOL *v, CONST EXENT *e, UW pfirst, T_FSTAT *st )
{
	ENODE	*n = node_at(v, pfirst, e->idx);
	UD	size = ( n != NULL ) ? n->size : e->size;

	st->mode = ( ( e->attr & ATTR_DIR ) != 0 ) ? FS_IFDIR | FS_IRWXU : FS_IFREG | FS_IRWXU;
	st->size = ( ( e->attr & ATTR_DIR ) != 0 ) ? 0 : size;
	st->mtime = tron_of_ts(rd32(v->set + 12), v->set[21], v->set[23]);
	st->ino = ino_of(e, pfirst);
}

LOCAL ER ex_stat( T_MOUNT *m, CONST char *path, T_FSTAT *st )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	d;
	EXENT	e;
	BOOL	isroot;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, path, &d, &e, &isroot, NULL, NULL, 0);
	if ( er >= EX_OK ) {
		if ( isroot ) {
			st->mode = FS_IFDIR | FS_IRWXU;
			st->size = 0;
			st->mtime = 0;
			st->ino = v->root.first;
		} else {
			fill_stat(v, &e, d.s.first, st);
		}
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_utime( T_MOUNT *m, CONST char *path, UD mtime )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	d;
	EXENT	e;
	BOOL	isroot;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, path, &d, &e, &isroot, NULL, NULL, 0);
	if ( er >= EX_OK ) {
		if ( isroot ) {
			er = EX_INVAL;
		} else {
			ENODE	*n = node_at(v, d.s.first, e.idx);

			er = vol_touch(v);
			if ( er >= EX_OK && n != NULL ) {
				/* what the file's writing owes the entry first, or it would be stamped over */
				er = node_put(v, n);
				if ( er >= EX_OK ) er = set_read(v, &d.s, e.idx, e.nent, &isroot);
			}
			if ( er >= EX_OK ) {
				wr32(v->set + 12, ts_of_tron(mtime));
				v->set[21] = 0;
				v->set[23] = 0x80;
				er = set_write(v, &d.s, e.idx, e.nent);
			}
			if ( er >= EX_OK ) er = c_flush(v);
		}
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL INT ex_getdents( T_FILE *f, T_DIRENT *buf, INT nent )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ENODE	*n = (ENODE *)f->obj;
	UD	idx = f->priv;
	INT	cnt = 0;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	while ( cnt < nent ) {
		EXENT	e;
		INT	k, b;

		er = set_next(v, &n->s, &idx, &e);
		if ( er == EX_NOENT ) {
			er = EX_OK;
			break;
		}
		if ( er < EX_OK ) break;
		k = set_name(v, e.nent, v->nm16b);
		b = knl_fs_utf16_to_8(v->nm16b, k, buf[cnt].name, FS_NAME_MAX - 1);
		if ( b < 0 ) continue;
		buf[cnt].name[b] = 0;
		buf[cnt].mode = ( ( e.attr & ATTR_DIR ) != 0 ) ? FS_IFDIR : FS_IFREG;
		{
			ENODE	*o = node_at(v, n->s.first, e.idx);

			buf[cnt].size = ( ( e.attr & ATTR_DIR ) != 0 ) ? 0 : ( o != NULL ) ? o->size : e.size;
		}
		buf[cnt].ino = ino_of(&e, n->s.first);
		cnt++;
	}
	f->priv = idx;
	tk_unl_mtx(v->mtxid);
	if ( cnt == 0 && er < EX_OK ) return (INT)er;
	return cnt;
}

LOCAL ER ex_mkdir( T_MOUNT *m, CONST char *path )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	d;
	EXENT	e;
	CONST char *last;
	INT	len, k, want;
	UD	idx, i;
	UW	c;
	EXS	ns;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, path, &d, NULL, NULL, &last, &len, 0);
	if ( er < EX_OK ) goto exit;
	er = name_conv(v, last, len, &k);
	if ( er < EX_OK ) goto exit;
	er = dir_find(v, &d.s, v->nm16, k, &e);
	if ( er >= EX_OK ) {
		er = EX_EXIST;
		goto exit;
	}
	if ( er != EX_NOENT ) goto exit;
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	want = 2 + ( k + NAME_PER_ENT - 1 ) / NAME_PER_ENT;
	er = dir_room(v, &d, want, &idx);
	if ( er < EX_OK ) goto exit;

	knl_memset(&ns, 0, sizeof(ns));
	er = s_grow1(v, &ns, &c);
	if ( er < EX_OK ) goto exit;
	for ( i = 0; i < ( (UD)1 << v->spc_sh ); i++ ) {
		UB	*p;

		er = c_get(v, clus_sect(v, c) + i, CC_DIR, FALSE, &p);
		if ( er < EX_OK ) goto exit;
		knl_memset(p, 0, SECT);
		c_mark(v, CC_DIR);
	}
	want = set_make(v, ATTR_DIR, ns.first, ns.nofat, v->csize, v->csize, v->nm16, k, NULL);
	er = set_write(v, &d.s, idx, want);
	if ( er >= EX_OK ) er = c_flush(v);

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_remove( T_MOUNT *m, CONST char *path, BOOL want_dir )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	d;
	EXENT	e;
	EXS	s;
	BOOL	isroot;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, path, &d, &e, &isroot, NULL, NULL, 0);
	if ( er < EX_OK ) goto exit;
	if ( isroot ) {
		er = EX_BUSY;
		goto exit;
	}
	if ( want_dir && ( e.attr & ATTR_DIR ) == 0 ) {
		er = EX_NOTDIR;
		goto exit;
	}
	if ( !want_dir && ( e.attr & ATTR_DIR ) != 0 ) {
		er = EX_ISDIR;
		goto exit;
	}
	if ( node_at(v, d.s.first, e.idx) != NULL ) {
		er = EX_BUSY;
		goto exit;
	}
	if ( e.others ) {
		er = EX_NOTSUP;		/* entries it does not know may hold clusters of their own */
		goto exit;
	}
	s_from_ent(v, &s, &e);
	if ( want_dir ) {
		INT	nent = e.nent;
		UD	idx = e.idx;

		er = dir_empty(v, &e);
		if ( er < EX_OK ) goto exit;
		e.nent = nent;
		e.idx = idx;
	}
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;
	er = set_erase(v, &d.s, e.idx, e.nent);
	if ( er >= EX_OK ) er = c_flush(v);
	if ( er >= EX_OK && s.first != 0 ) er = s_cut(v, &s, 0);
	if ( er >= EX_OK ) er = c_flush(v);

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_unlink( T_MOUNT *m, CONST char *path )
{
	return ex_remove(m, path, FALSE);
}

LOCAL ER ex_rmdir( T_MOUNT *m, CONST char *path )
{
	return ex_remove(m, path, TRUE);
}

LOCAL ER ex_rename( T_MOUNT *m, CONST char *from, CONST char *to )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	EXDIR	df, dt;
	EXENT	ef, et;
	EXS	gone;
	BOOL	isroot, have_t = FALSE;
	CONST char *last;
	INT	len, k, want, nent;
	UD	idx;
	UB	times[19];
	UW	attr;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, from, &df, &ef, &isroot, NULL, NULL, 0);
	if ( er < EX_OK ) goto exit;
	if ( isroot ) {
		er = EX_BUSY;
		goto exit;
	}
	if ( node_at(v, df.s.first, ef.idx) != NULL ) {
		er = EX_BUSY;
		goto exit;
	}
	if ( ef.others ) {
		er = EX_NOTSUP;
		goto exit;
	}
	attr = ef.attr;
	knl_memcpy(times, v->set + 8, sizeof(times));

	/* not into itself */
	er = path_walk(v, to, &dt, NULL, NULL, &last, &len, ( attr & ATTR_DIR ) != 0 ? ef.first : 0);
	if ( er < EX_OK ) goto exit;
	if ( ( attr & ATTR_DIR ) != 0 && dt.s.first == ef.first ) {
		er = EX_INVAL;
		goto exit;
	}
	er = name_conv(v, last, len, &k);
	if ( er < EX_OK ) goto exit;
	er = dir_find(v, &dt.s, v->nm16, k, &et);
	if ( er >= EX_OK ) {
		if ( !( dt.s.first == df.s.first && et.idx == ef.idx ) ) {
			have_t = TRUE;
			if ( ( et.attr & ATTR_DIR ) != ( attr & ATTR_DIR ) ) {
				er = ( ( et.attr & ATTR_DIR ) != 0 ) ? EX_ISDIR : EX_NOTDIR;
				goto exit;
			}
			if ( node_at(v, dt.s.first, et.idx) != NULL ) {
				er = EX_BUSY;
				goto exit;
			}
			if ( et.others ) {
				er = EX_NOTSUP;
				goto exit;
			}
			if ( ( et.attr & ATTR_DIR ) != 0 ) {
				INT	n0 = et.nent;
				UD	i0 = et.idx;

				er = dir_empty(v, &et);
				if ( er < EX_OK ) goto exit;
				et.nent = n0;
				et.idx = i0;
			}
		}
	} else if ( er != EX_NOENT ) {
		goto exit;
	}
	er = vol_touch(v);
	if ( er < EX_OK ) goto exit;

	/* the name taken over first, then the new set, then the old one erased */
	if ( have_t ) {
		s_from_ent(v, &gone, &et);
		er = set_erase(v, &dt.s, et.idx, et.nent);
		if ( er < EX_OK ) goto exit;
	}
	want = 2 + ( k + NAME_PER_ENT - 1 ) / NAME_PER_ENT;
	/* the name is safe from the look at the old set: v->nm16 is not used for it */
	er = dir_room(v, &dt, want, &idx);
	if ( er < EX_OK ) goto exit;
	/* room in the old entries' own directory cannot be the old set's: they are still in use */
	nent = set_make(v, attr, ef.first, ef.nofat, ef.size, ef.valid, v->nm16, k, times);
	er = set_write(v, &dt.s, idx, nent);
	if ( er < EX_OK ) goto exit;
	if ( dt.s.first == df.s.first ) {
		df = dt;
	}
	er = set_erase(v, &df.s, ef.idx, ef.nent);
	if ( er >= EX_OK ) er = c_flush(v);
	if ( er >= EX_OK && have_t && gone.first != 0 ) {
		er = s_cut(v, &gone, 0);
		if ( er >= EX_OK ) er = c_flush(v);
	}

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_statvfs( T_MOUNT *m, T_FSSTAT *st )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = free_count(v);
	if ( er >= EX_OK ) {
		st->blocks = v->clus_cnt;
		st->bfree = v->free_cnt;
		st->bsize = v->csize;
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL BOOL any_writer( EXVOL *v )
{
	ENODE	*n;

	for ( n = v->nodes; n != NULL; n = n->next ) {
		if ( n->writers > 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL ER ex_sync( T_MOUNT *m )
{
	EXVOL	*v = (EXVOL *)m->exinf;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( !v->ro ) {
		er = nodes_put(v);
		if ( er >= EX_OK ) {
			er = any_writer(v) ? c_flush(v) : vol_clean(v);
		}
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

LOCAL ER ex_fsync( T_FILE *f )
{
	EXVOL	*v = (EXVOL *)f->mount->exinf;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( !v->ro ) {
		er = node_put(v, (ENODE *)f->obj);
		if ( er >= EX_OK ) er = c_flush(v);
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

/*
 * The flusher: what was left in the cache and the mark of being in use,
 * a few seconds after the last write and with no file open for writing,
 * so that a medium pulled out after a quiet moment is a clean one.
 */
LOCAL EXVOL	*ex_vols = NULL;
LOCAL ID	ex_vols_mtx = 0;
LOCAL ID	ex_flush_tsk = 0;

LOCAL void ex_flusher( INT stacd, void *exinf )
{
	EXVOL	*v;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		(void)tk_dly_tsk(1000);
		tk_loc_mtx(ex_vols_mtx, TMO_FEVR);
		for ( v = ex_vols; v != NULL; v = v->lnext ) {
			if ( !v->marked || v->ro || v->ioerr ) continue;
			tk_loc_mtx(v->mtxid, TMO_FEVR);
			if ( v->marked && now_ms() - v->last_write >= FLUSH_MS ) {
				if ( nodes_put(v) >= EX_OK ) {
					if ( any_writer(v) ) {
						(void)c_flush(v);
					} else {
						(void)vol_clean(v);
					}
				}
			}
			tk_unl_mtx(v->mtxid);
		}
		tk_unl_mtx(ex_vols_mtx);
	}
}

LOCAL void vols_add( EXVOL *v )
{
	T_CMTX	cm;
	T_CTSK	ct;

	if ( ex_vols_mtx <= 0 ) {
		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		ex_vols_mtx = tk_cre_mtx(&cm);
		if ( ex_vols_mtx <= 0 ) return;
	}
	tk_loc_mtx(ex_vols_mtx, TMO_FEVR);
	v->lnext = ex_vols;
	ex_vols = v;
	tk_unl_mtx(ex_vols_mtx);
	if ( ex_flush_tsk <= 0 ) {
		ct.exinf = NULL;
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)ex_flusher;
		ct.itskpri = CNF_MAX_TSKPRI - 1;
		ct.stksz = 8192;
		ex_flush_tsk = tk_cre_tsk(&ct);
		if ( ex_flush_tsk > 0 ) {
			(void)tk_sta_tsk(ex_flush_tsk, 0);
		} else {
			tm_printf((UB *)"exfatfs: the flusher could not be made (%d)\n", (INT)ex_flush_tsk);
		}
	}
}

LOCAL void vols_del( EXVOL *v )
{
	EXVOL	**pp;

	if ( ex_vols_mtx <= 0 ) return;
	tk_loc_mtx(ex_vols_mtx, TMO_FEVR);
	for ( pp = &ex_vols; *pp != NULL && *pp != v; pp = &(*pp)->lnext ) ;
	if ( *pp == v ) *pp = v->lnext;
	tk_unl_mtx(ex_vols_mtx);
}

EXPORT CONST T_FIMP knl_fimp_exfat = {
	.name     = "exfatfs",
	.mount    = ex_mount,
	.unmount  = ex_unmount,
	.open     = ex_open,
	.close    = ex_close,
	.read     = ex_read,
	.write    = ex_write,
	.truncate = ex_truncate,
	.stat     = ex_stat,
	.getdents = ex_getdents,
	.mkdir    = ex_mkdir,
	.rmdir    = ex_rmdir,
	.unlink   = ex_unlink,
	.rename   = ex_rename,
	.statvfs  = ex_statvfs,
	.sync     = ex_sync,
	.utime    = ex_utime,
	.fsync    = ex_fsync,
};
