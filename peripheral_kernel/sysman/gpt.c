/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpt.c
 *	GPT partitions made and made larger (design 7.7).
 *
 *	The table is read from the whole disk: the header at sector 1 and
 *	its entries, both checked against their CRCs, else the backup header
 *	at the last sector and its entries. Nothing is written to a disk
 *	neither copy of whose table passes.
 *
 *	Every write puts the whole table down twice, in this order:
 *
 *		1. the backup entries at the end of the disk, then the backup
 *		   header in the last sector;
 *		2. the entries at the start, then the header in sector 1;
 *		3. the protective MBR's size, when it says less than the disk.
 *
 *	The device is told to empty its cache after each. Until the header
 *	of 2 is down, the header at sector 1 and its entries are the table
 *	there was, and the backup it names is where it was: a disk whose
 *	power goes before then reads as before. A card written with an image
 *	smaller than the card has its backup table in the middle of the card,
 *	right after the last partition; the first write moves it to the end,
 *	and the old one is left in the free stretch, where a partition grown
 *	or made later writes over it.
 *
 *	The driver is then asked to read the table again (TDN_PARTSCAN),
 *	and the device manager's list of disks follows (knl_obdev_changed).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/gpt.h>
#include <ts/uuid.h>
#include <ts/ob.h>

#define SECT		BLK_SECTOR_SIZE
#define GPT_SIG		"EFI PART"
#define HDR_SIZE	92
#define ENT_MAX_BYTES	( 128 * 1024 )	/* an entry array larger than this is not written */

/* the header */
#define H_REV		8
#define H_SIZE		12
#define H_CRC		16
#define H_MY		24
#define H_ALT		32
#define H_FIRST		40
#define H_LAST		48
#define H_GUID		56
#define H_ENTLBA	72
#define H_NENT		80
#define H_ENTSZ		84
#define H_ENTCRC	88

/* an entry */
#define E_TYPE		0
#define E_UNIQ		16
#define E_FIRST		32
#define E_LAST		40
#define E_ATTR		48
#define E_NAME		56

typedef struct {
	ID	dd;
	UD	nsect;
	UB	hdr[SECT];		/* the header, as the start of the disk will have it */
	UB	*ent;
	UW	nent, entsz;
	UD	esect;			/* sectors of the entries */
	BOOL	fresh;			/* there was no table: one is made */
} GPT;

LOCAL UW	gpt_cut_after = 0;	/* writes still to go before a pretended cut */
LOCAL BOOL	gpt_cut_on = FALSE;
LOCAL UW	gpt_writes = 0;

/* ---------------------------------------------------------------- small things */

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UD le64( CONST UB *p )
{
	return (UD)le32(p) | ((UD)le32(p + 4) << 32);
}

LOCAL void put32( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)( v >> 8 ); p[2] = (UB)( v >> 16 ); p[3] = (UB)( v >> 24 );
}

LOCAL void put64( UB *p, UD v )
{
	put32(p, (UW)v);
	put32(p + 4, (UW)( v >> 32 ));
}

/* The CRC-32 of IEEE 802.3, as the GPT has it */
LOCAL UW crc32( CONST UB *p, UD n )
{
	UW	c = 0xFFFFFFFFU;
	INT	k;

	while ( n-- > 0 ) {
		c ^= *p++;
		for ( k = 0; k < 8; k++ ) {
			c = ( c & 1 ) ? ( c >> 1 ) ^ 0xEDB88320U : ( c >> 1 );
		}
	}
	return ~c;
}

LOCAL BOOL zero16( CONST UB *g )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( g[i] != 0 ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL same_n( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

#define same16(a, b)	same_n((a), (b), 16)

LOCAL UD align_up( UD s )
{
	return ( s + GPT_ALIGN - 1 ) / GPT_ALIGN * GPT_ALIGN;
}

/* The last sector of the megabyte before the one `s` (one past an end) is in */
LOCAL UD align_end( UD s )
{
	return s / GPT_ALIGN * GPT_ALIGN;
}

/* "sda1" to the disk "sda" and the subunit 1; FALSE for a name with no number */
LOCAL BOOL split_name( CONST char *part, UB *disk, INT *p_unit )
{
	INT	n = 0, k, unit = 0;

	while ( part[n] != '\0' && n < L_DEVNM ) n++;
	for ( k = n; k > 0 && part[k - 1] >= '0' && part[k - 1] <= '9'; k-- ) ;
	if ( k == n || k == 0 ) {
		return FALSE;
	}
	knl_memcpy(disk, part, k);
	disk[k] = 0;
	for ( ; k < n; k++ ) unit = unit * 10 + ( part[k] - '0' );
	*p_unit = unit;
	return TRUE;
}

/* ---------------------------------------------------------------- the disk */

LOCAL ER rd( GPT *g, UD lba, UD n, void *buf )
{
	SZ	asz;

	return tk_srea_dev_d(g->dd, (D)lba, buf, (SZ)( n * SECT ), &asz);
}

LOCAL ER wr( GPT *g, UD lba, UD n, CONST void *buf )
{
	SZ	asz;

	gpt_writes++;
	if ( gpt_cut_on ) {
		if ( gpt_cut_after == 0 ) {
			return E_OK;		/* the power went: the medium keeps what it had */
		}
		gpt_cut_after--;
	}
	return tk_swri_dev_d(g->dd, (D)lba, buf, (SZ)( n * SECT ), &asz);
}

LOCAL void flush( GPT *g )
{
	SZ	asz;

	if ( !gpt_cut_on || gpt_cut_after > 0 ) {
		(void)tk_swri_dev(g->dd, TDN_FLUSH, NULL, 0, &asz);
	}
}

EXPORT void knl_gpt_cut( UW after, UW *p_writes )
{
	if ( p_writes != NULL ) {
		*p_writes = gpt_writes;
	}
	gpt_writes = 0;
	gpt_cut_on = (BOOL)( after > 0 );
	gpt_cut_after = after;
}

/* A header that passes, and the entries it names that pass too */
LOCAL BOOL hdr_take( GPT *g, CONST UB *h, UD my )
{
	UB	tmp[HDR_SIZE];
	UW	size = le32(h + H_SIZE), nent = le32(h + H_NENT), entsz = le32(h + H_ENTSZ);
	UD	elba = le64(h + H_ENTLBA), esect;

	if ( !same_n(h, (CONST UB *)GPT_SIG, 8) || size < HDR_SIZE || size > SECT || le64(h + H_MY) != my ) {
		return FALSE;
	}
	knl_memcpy(tmp, h, HDR_SIZE);
	put32(tmp + H_CRC, 0);
	if ( size != HDR_SIZE || crc32(tmp, HDR_SIZE) != le32(h + H_CRC) ) {
		return FALSE;		/* a larger header is not one this writes */
	}
	if ( entsz < 128 || entsz % 8 != 0 || nent == 0 || (UD)nent * entsz > ENT_MAX_BYTES
	  || elba < 2 || elba >= g->nsect ) {
		return FALSE;
	}
	esect = ( (UD)nent * entsz + SECT - 1 ) / SECT;
	g->ent = (UB *)Kcalloc(esect, SECT);
	if ( g->ent == NULL ) {
		return FALSE;
	}
	if ( rd(g, elba, esect, g->ent) < E_OK || crc32(g->ent, (UD)nent * entsz) != le32(h + H_ENTCRC) ) {
		Kfree(g->ent);
		g->ent = NULL;
		return FALSE;
	}
	g->nent = nent;
	g->entsz = entsz;
	g->esect = esect;
	knl_memcpy(g->hdr, h, SECT);
	return TRUE;
}

LOCAL void gpt_close( GPT *g )
{
	if ( g->ent != NULL ) Kfree(g->ent);
	g->ent = NULL;
	if ( g->dd > 0 ) tk_cls_dev(g->dd, 0);
	g->dd = 0;
}

/*
 * The table of a disk read. One with a valid copy, or none at all on a
 * disk whose first sectors hold nothing (`fresh`: a table of 128 entries
 * is set up in memory); E_NOSPT for an MBR, E_OBJ for a GPT that does
 * not pass.
 */
LOCAL ER gpt_open( CONST char *disk, GPT *g, UINT mode )
{
	DiskInfo	info;
	UB		*s;
	SZ		asz;
	INT		i;
	BOOL		blank = TRUE, sig;
	ER		er = E_OBJ;

	knl_memset(g, 0, sizeof(*g));
	g->dd = tk_opn_dev((CONST UB *)disk, mode);
	if ( g->dd < E_OK ) {
		er = g->dd;
		g->dd = 0;
		return er;
	}
	if ( tk_srea_dev(g->dd, TDN_DISKINFO, &info, sizeof(info), &asz) < E_OK
	  || info.blocksize != SECT || info.blockcont_d < 4 * GPT_ALIGN ) {
		gpt_close(g);
		return E_NOSPT;
	}
	g->nsect = (UD)info.blockcont_d;
	s = (UB *)Kmalloc(SECT * 2);
	if ( s == NULL ) {
		gpt_close(g);
		return E_NOMEM;
	}
	if ( rd(g, 0, 2, s) < E_OK ) {
		Kfree(s);
		gpt_close(g);
		return E_IO;
	}
	if ( hdr_take(g, s + SECT, 1) ) {
		er = E_OK;
	} else if ( rd(g, g->nsect - 1, 1, s + SECT) >= E_OK && hdr_take(g, s + SECT, g->nsect - 1) ) {
		/* the backup, as the start of the disk is to have it */
		put64(g->hdr + H_MY, 1);
		put64(g->hdr + H_ENTLBA, 2);
		er = E_OK;
	} else {
		sig = (BOOL)( s[510] == 0x55 && s[511] == 0xAA );
		for ( i = 0; i < SECT * 2; i++ ) {
			if ( s[i] != 0 ) blank = FALSE;
		}
		if ( same_n(s + SECT, (CONST UB *)GPT_SIG, 8) ) {
			er = E_OBJ;
		} else if ( sig || !blank ) {
			er = E_NOSPT;
		} else {
			/* nothing there: a table made from nothing */
			g->fresh = TRUE;
			g->nent = 128;
			g->entsz = 128;
			g->esect = 32;
			g->ent = (UB *)Kcalloc(g->esect, SECT);
			knl_memset(g->hdr, 0, SECT);
			knl_memcpy(g->hdr, GPT_SIG, 8);
			put32(g->hdr + H_REV, 0x00010000);
			put32(g->hdr + H_SIZE, HDR_SIZE);
			put64(g->hdr + H_MY, 1);
			put64(g->hdr + H_FIRST, 2 + g->esect);
			put64(g->hdr + H_ENTLBA, 2);
			put32(g->hdr + H_NENT, g->nent);
			put32(g->hdr + H_ENTSZ, g->entsz);
			{
				TS_UUID	u;

				(void)ts_gen_uuid(&u);
				knl_memcpy(g->hdr + H_GUID, u.b, 16);
			}
			er = ( g->ent != NULL ) ? E_OK : E_NOMEM;
		}
	}
	Kfree(s);
	if ( er < E_OK ) {
		gpt_close(g);
	}
	return er;
}

/* The last sector a partition may take: the backup at the end of the disk */
LOCAL UD last_usable( CONST GPT *g )
{
	return g->nsect - 1 - g->esect - 1;
}

LOCAL UB *entry( GPT *g, INT i )
{
	return g->ent + (UD)i * g->entsz;
}

LOCAL BOOL used( GPT *g, INT i )
{
	return (BOOL)!zero16(entry(g, i) + E_TYPE);
}

/* The entry of subunit `unit` (as part.c counts them), or -1 */
LOCAL INT entry_of_unit( GPT *g, INT unit )
{
	INT	i, u = 0;

	for ( i = 0; i < (INT)g->nent; i++ ) {
		UB	*e = entry(g, i);

		if ( !used(g, i) || le64(e + E_LAST) < le64(e + E_FIRST) || le64(e + E_LAST) >= g->nsect ) {
			continue;
		}
		if ( u == unit ) return i;
		u++;
	}
	return -1;
}

/* The subunit the entry `i` will be */
LOCAL INT unit_of_entry( GPT *g, INT at )
{
	INT	i, u = 0;

	for ( i = 0; i < at; i++ ) {
		UB	*e = entry(g, i);

		if ( used(g, i) && le64(e + E_LAST) >= le64(e + E_FIRST) && le64(e + E_LAST) < g->nsect ) u++;
	}
	return u;
}

/* The first sector of a partition after `after`, or past the last usable one */
LOCAL UD next_start( GPT *g, UD after, INT skip )
{
	UD	lim = last_usable(g) + 1, f;
	INT	i;

	for ( i = 0; i < (INT)g->nent; i++ ) {
		if ( i == skip || !used(g, i) ) continue;
		f = le64(entry(g, i) + E_FIRST);
		if ( f > after && f < lim ) lim = f;
	}
	return lim;
}

/* Whether [first, last] meets no partition */
LOCAL BOOL clear_of( GPT *g, UD first, UD last, INT skip )
{
	INT	i;

	for ( i = 0; i < (INT)g->nent; i++ ) {
		UB	*e = entry(g, i);

		if ( i == skip || !used(g, i) ) continue;
		if ( !( last < le64(e + E_FIRST) || first > le64(e + E_LAST) ) ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- writing */

LOCAL ER gpt_write( GPT *g )
{
	UB	*h, *mbr;
	UD	lu = last_usable(g), back_ent = g->nsect - 1 - g->esect;
	UW	ecrc = crc32(g->ent, (UD)g->nent * g->entsz);
	ER	er;

	h = (UB *)Kmalloc(SECT * 2);
	if ( h == NULL ) {
		return E_NOMEM;
	}
	mbr = h + SECT;

	/* the protective MBR of a disk that had nothing: before anything says GPT */
	if ( g->fresh ) {
		knl_memset(mbr, 0, SECT);
		mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02;
		mbr[446 + 4] = 0xEE;
		mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
		put32(mbr + 446 + 8, 1);
		put32(mbr + 446 + 12, ( g->nsect - 1 > 0xFFFFFFFFULL ) ? 0xFFFFFFFFU : (UW)( g->nsect - 1 ));
		mbr[510] = 0x55; mbr[511] = 0xAA;
		er = wr(g, 0, 1, mbr);
		if ( er < E_OK ) goto out;
	}

	put64(g->hdr + H_LAST, lu);
	put32(g->hdr + H_ENTCRC, ecrc);

	/* 1: the backup at the end of the disk */
	knl_memcpy(h, g->hdr, SECT);
	put64(h + H_MY, g->nsect - 1);
	put64(h + H_ALT, 1);
	put64(h + H_ENTLBA, back_ent);
	put32(h + H_CRC, 0);
	put32(h + H_CRC, crc32(h, HDR_SIZE));
	er = wr(g, back_ent, g->esect, g->ent);
	if ( er >= E_OK ) er = wr(g, g->nsect - 1, 1, h);
	if ( er < E_OK ) goto out;
	flush(g);

	/* 2: the entries at the start, then the header that names them */
	put64(g->hdr + H_MY, 1);
	put64(g->hdr + H_ALT, g->nsect - 1);
	put32(g->hdr + H_CRC, 0);
	put32(g->hdr + H_CRC, crc32(g->hdr, HDR_SIZE));
	er = wr(g, le64(g->hdr + H_ENTLBA), g->esect, g->ent);
	if ( er >= E_OK ) {
		flush(g);
		er = wr(g, 1, 1, g->hdr);
	}
	if ( er < E_OK ) goto out;
	flush(g);

	/* 3: the protective MBR said less than the disk had (a card larger than its image) */
	if ( !g->fresh && rd(g, 0, 1, mbr) >= E_OK && mbr[510] == 0x55 && mbr[511] == 0xAA
	  && mbr[446 + 4] == 0xEE ) {
		UW	want = ( g->nsect - 1 > 0xFFFFFFFFULL ) ? 0xFFFFFFFFU : (UW)( g->nsect - 1 );

		if ( le32(mbr + 446 + 12) < want ) {
			put32(mbr + 446 + 12, want);
			if ( wr(g, 0, 1, mbr) >= E_OK ) flush(g);
		}
	}

    out:
	Kfree(h);
	return er;
}

/* The driver reads the table again; the list of disks follows */
LOCAL ER rescan( GPT *g )
{
	SZ	asz;
	ER	er;

	if ( gpt_cut_on && gpt_cut_after == 0 ) {
		return E_OK;			/* a test's pretended cut: nothing changed on the medium */
	}
	er = tk_swri_dev(g->dd, TDN_PARTSCAN, NULL, 0, &asz);
	if ( er < E_OK ) {
		tm_printf((UB *)"gpt: the partitions not read again (%d)\n", (INT)er);
		return er;
	}
	knl_obdev_changed();
	return E_OK;
}

/* ---------------------------------------------------------------- what is offered */

EXPORT INT knl_gpt_free( CONST char *disk, T_GPTFREE *buf, INT max, UD *p_nsect, BOOL *p_fresh )
{
	GPT	g;
	UD	at, lim, f, l;
	INT	n = 0;
	ER	er;

	if ( disk == NULL || buf == NULL ) {
		return E_PAR;
	}
	er = gpt_open(disk, &g, TD_READ);
	if ( er < E_OK ) {
		return er;
	}
	if ( p_nsect != NULL ) *p_nsect = g.nsect;
	if ( p_fresh != NULL ) *p_fresh = g.fresh;
	at = le64(g.hdr + H_FIRST);
	lim = last_usable(&g);
	while ( at <= lim && n < max ) {
		INT	i;
		BOOL	moved = FALSE;

		/* a partition that takes `at`: on past it */
		for ( i = 0; i < (INT)g.nent; i++ ) {
			UB	*e = entry(&g, i);

			if ( used(&g, i) && le64(e + E_FIRST) <= at && le64(e + E_LAST) >= at ) {
				at = le64(e + E_LAST) + 1;
				moved = TRUE;
			}
		}
		if ( moved ) continue;
		l = next_start(&g, at, -1) - 1;
		f = align_up(at);
		if ( l + 1 > f && align_end(l + 1) >= f + GPT_ALIGN ) {
			buf[n].first = f;
			buf[n].last = align_end(l + 1) - 1;
			n++;
		}
		at = l + 1;
	}
	gpt_close(&g);
	return n;
}

EXPORT ER knl_gpt_part( CONST char *part, T_GPTPART *pk )
{
	GPT	g;
	UB	*e;
	INT	unit, i;
	UD	nx;
	ER	er;

	if ( part == NULL || pk == NULL ) {
		return E_PAR;
	}
	knl_memset(pk, 0, sizeof(*pk));
	if ( !split_name(part, pk->disk, &unit) ) {
		return E_PAR;
	}
	er = gpt_open((CONST char *)pk->disk, &g, TD_READ);
	if ( er < E_OK ) {
		return er;
	}
	i = g.fresh ? -1 : entry_of_unit(&g, unit);
	if ( i < 0 ) {
		gpt_close(&g);
		return E_NOEXS;
	}
	e = entry(&g, i);
	pk->entry = i;
	pk->unit = unit;
	pk->first = le64(e + E_FIRST);
	pk->last = le64(e + E_LAST);
	pk->nsect = g.nsect;
	knl_memcpy(pk->type, e + E_TYPE, 16);
	pk->tsfs = same16(e + E_TYPE, knl_tsfs_type_guid);
	nx = next_start(&g, pk->last, i);
	pk->tail = (BOOL)( nx == last_usable(&g) + 1 );
	pk->grow_last = pk->last;
	if ( nx > pk->last + 1 ) {
		UD	end = align_end(nx);		/* the megabyte the stretch ends in is left out */

		if ( end > pk->first && end - 1 > pk->last ) pk->grow_last = end - 1;
	}
	gpt_close(&g);
	return E_OK;
}

EXPORT ER knl_gpt_grow( CONST char *part, UD last )
{
	GPT	g;
	UB	disk[L_DEVNM + 1];
	UB	*e;
	INT	unit, i;
	ER	er;

	if ( part == NULL || !split_name(part, disk, &unit) ) {
		return E_PAR;
	}
	er = gpt_open((CONST char *)disk, &g, TD_UPDATE);
	if ( er < E_OK ) {
		return er;
	}
	i = g.fresh ? -1 : entry_of_unit(&g, unit);
	if ( i < 0 ) {
		gpt_close(&g);
		return E_NOEXS;
	}
	e = entry(&g, i);
	if ( last <= le64(e + E_LAST) || last > last_usable(&g)
	  || !clear_of(&g, le64(e + E_LAST) + 1, last, i) ) {
		gpt_close(&g);
		return E_PAR;
	}
	tm_printf((UB *)"gpt: %s grown from %d to %d sectors\n", part,
		  (INT)( le64(e + E_LAST) - le64(e + E_FIRST) + 1 ), (INT)( last - le64(e + E_FIRST) + 1 ));
	put64(e + E_LAST, last);
	er = gpt_write(&g);
	if ( er >= E_OK ) er = rescan(&g);
	gpt_close(&g);
	return er;
}

/* A partition's name in UTF-16, from UTF-8: what is past the plane of 16 bits is left out */
LOCAL void set_name( UB *e, CONST char *name )
{
	INT	k, m;

	knl_memset(e + E_NAME, 0, GPT_NAME_MAX * 2);
	for ( k = 0, m = 0; name != NULL && name[k] != '\0' && m < GPT_NAME_MAX; ) {
		UB	c = (UB)name[k];
		UW	ch;

		if ( c < 0x80 ) {
			ch = c; k += 1;
		} else if ( ( c & 0xE0 ) == 0xC0 && name[k + 1] != '\0' ) {
			ch = ( (UW)( c & 0x1F ) << 6 ) | ( (UB)name[k + 1] & 0x3F ); k += 2;
		} else if ( ( c & 0xF0 ) == 0xE0 && name[k + 1] != '\0' && name[k + 2] != '\0' ) {
			ch = ( (UW)( c & 0x0F ) << 12 ) | ( (UW)( (UB)name[k + 1] & 0x3F ) << 6 )
			   | ( (UB)name[k + 2] & 0x3F );
			k += 3;
		} else {
			k++;
			continue;
		}
		e[E_NAME + 2 * m] = (UB)ch;
		e[E_NAME + 2 * m + 1] = (UB)( ch >> 8 );
		m++;
	}
}

EXPORT ER knl_gpt_add( CONST char *disk, UD first, UD last, CONST UB *type, CONST char *name,
		       UB *p_part, INT max )
{
	GPT	g;
	TS_UUID	u;
	UB	*e;
	INT	i, at = 0, k;
	ER	er;

	if ( disk == NULL || type == NULL || zero16(type) ) {
		return E_PAR;
	}
	er = gpt_open(disk, &g, TD_UPDATE);
	if ( er < E_OK ) {
		return er;
	}
	if ( first < le64(g.hdr + H_FIRST) || last > last_usable(&g) || last < first
	  || !clear_of(&g, first, last, -1) ) {
		gpt_close(&g);
		return E_PAR;
	}
	/* after the last entry in use: the partitions there were keep their numbers */
	for ( i = 0; i < (INT)g.nent; i++ ) {
		if ( used(&g, i) ) at = i + 1;
	}
	if ( at >= (INT)g.nent || unit_of_entry(&g, at) >= BLK_MAX_PART ) {
		gpt_close(&g);
		return E_LIMIT;
	}
	e = entry(&g, at);
	knl_memset(e, 0, g.entsz);
	knl_memcpy(e + E_TYPE, type, 16);
	(void)ts_gen_uuid(&u);
	knl_memcpy(e + E_UNIQ, u.b, 16);
	put64(e + E_FIRST, first);
	put64(e + E_LAST, last);
	set_name(e, name);
	k = unit_of_entry(&g, at);
	tm_printf((UB *)"gpt: %s%d made, %d sectors at %d\n", disk, k, (INT)( last - first + 1 ), (INT)first);
	er = gpt_write(&g);
	if ( er >= E_OK ) er = rescan(&g);
	gpt_close(&g);
	if ( er >= E_OK && p_part != NULL && max > 0 ) {
		INT	n = 0;
		char	t[4];
		INT	j = 0;

		for ( n = 0; disk[n] != '\0' && n < max - 1; n++ ) p_part[n] = (UB)disk[n];
		do {
			t[j++] = (char)( '0' + k % 10 );
			k /= 10;
		} while ( k > 0 && j < 4 );
		while ( j > 0 && n < max - 1 ) p_part[n++] = (UB)t[--j];
		p_part[n] = 0;
	}
	return er;
}

EXPORT ER knl_gpt_retype( CONST char *part, CONST UB *type, CONST char *name, UB *p_uniq )
{
	GPT	g;
	TS_UUID	u;
	UB	disk[L_DEVNM + 1];
	UB	*e;
	INT	unit, i;
	ER	er;

	if ( part == NULL || type == NULL || zero16(type) || !split_name(part, disk, &unit) ) {
		return E_PAR;
	}
	er = gpt_open((CONST char *)disk, &g, TD_UPDATE);
	if ( er < E_OK ) {
		return er;
	}
	i = g.fresh ? -1 : entry_of_unit(&g, unit);
	if ( i < 0 ) {
		gpt_close(&g);
		return E_NOEXS;
	}
	e = entry(&g, i);
	knl_memcpy(e + E_TYPE, type, 16);
	(void)ts_gen_uuid(&u);
	knl_memcpy(e + E_UNIQ, u.b, 16);
	put64(e + E_ATTR, 0);
	set_name(e, name);
	tm_printf((UB *)"gpt: %s made another, %d sectors at %d\n", part,
		  (INT)( le64(e + E_LAST) - le64(e + E_FIRST) + 1 ), (INT)le64(e + E_FIRST));
	er = gpt_write(&g);
	if ( er >= E_OK ) er = rescan(&g);
	gpt_close(&g);
	if ( er >= E_OK && p_uniq != NULL ) knl_memcpy(p_uniq, u.b, 16);
	return er;
}
