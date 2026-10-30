/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fsprobe.c
 *	What is on a disk or a partition, told from its first blocks
 *	(design 7.6)
 *
 *	A volume is mounted with the fimp of its own kind, not the one a
 *	program happens to name, so the kind is read from the medium: the
 *	boot sector of FAT and of the other systems that put theirs at
 *	sector 0 (exFAT, NTFS), the superblock of TSFS at block 0 and of
 *	ext2/3/4 at byte 1024, the volume descriptors of ISO 9660 from
 *	sector 16 of 2048 bytes, the system header of the BTRON file system
 *	somewhere in the first 64 KB, and a partition table, which says the
 *	volumes are in the partitions and not on the whole disk.
 *
 *	Nothing is written and nothing is mounted: the device is opened for
 *	reading for as long as the look takes.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/blk.h>
#include <ts/tsfsblk.h>
#include <ts/crc32c.h>

#define PROBE_BYTES	( 64 * 1024 )	/* read from the start: the ISO 9660 descriptors of a disc are in it too */
#define BTRON_SCAN	( 64 * 1024 )	/* where the BTRON system header may be */
#define ISO_SECT	2048
#define ISO_VD_FIRST	16
#define TSFS_SB_GEN	16		/* in the TSFS superblock: its generation */
#define TSFS_SB_VOLUUID	24		/* the volume's UUID */
#define TSFS_SB_LABEL	40
#define TSFS_SB_TOPUUID	224		/* the object it opens with */
#define TSFS_SB_CRC	4092

LOCAL UW le16( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 );
}

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UD le64( CONST UB *p )
{
	return (UD)le32(p) | ( (UD)le32(p + 4) << 32 );
}

LOCAL UW be16( CONST UB *p )
{
	return ( (UW)p[0] << 8 ) | (UW)p[1];
}

LOCAL BOOL same_bytes( CONST UB *p, CONST char *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( p[i] != (UB)s[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL INT str_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL void put_str( UB *dst, INT max, CONST char *s )
{
	INT	i;

	for ( i = 0; i < max - 1 && s[i] != 0; i++ ) dst[i] = (UB)s[i];
	dst[i] = 0;
}

/* A label written in a fixed field padded with spaces, as ASCII */
LOCAL void put_label( UB *dst, INT max, CONST UB *src, INT n )
{
	INT	i, k = 0;

	while ( n > 0 && ( src[n - 1] == ' ' || src[n - 1] == 0 ) ) n--;
	for ( i = 0; i < n && k < max - 1; i++ ) {
		dst[k++] = ( src[i] >= 0x20 && src[i] < 0x7F ) ? src[i] : (UB)'_';
	}
	dst[k] = 0;
}

/*
 * A FAT boot sector, as the FAT fimp takes one: sectors of 512 bytes, a
 * cluster a power of two of them, the media byte F0 or F8 and above.
 * The kind follows from the number of clusters.
 */
LOCAL BOOL fat_of( CONST UB *b, T_FSPROBE *p )
{
	UW	bps = le16(b + 11), spc = b[13], rsvd = le16(b + 14), nfat = b[16];
	UW	rootent = le16(b + 17), tot = le16(b + 19), fatsz = le16(b + 22);
	UW	data, nclus;

	if ( le16(b + 510) != 0xAA55 && !( b[0] == 0xEB || b[0] == 0xE9 ) ) {
		return FALSE;
	}
	if ( tot == 0 ) tot = le32(b + 32);
	if ( fatsz == 0 ) fatsz = le32(b + 36);
	if ( bps != 512 || spc == 0 || ( spc & ( spc - 1 ) ) != 0 || rsvd == 0 || nfat == 0
	  || fatsz == 0 || tot == 0 || ( b[21] != 0xF0 && b[21] < 0xF8 ) ) {
		return FALSE;
	}
	data = rsvd + nfat * fatsz + ( rootent * 32 + 511 ) / 512;
	if ( tot <= data ) {
		return FALSE;
	}
	nclus = ( tot - data ) / spc;
	if ( nclus < 4085 ) {
		p->kind = FS_KIND_FAT12;
		put_str(p->name, sizeof(p->name), "FAT12");
	} else if ( nclus < 65525 ) {
		p->kind = FS_KIND_FAT16;
		put_str(p->name, sizeof(p->name), "FAT16");
	} else {
		p->kind = FS_KIND_FAT32;
		put_str(p->name, sizeof(p->name), "FAT32");
	}
	put_str(p->fimp, sizeof(p->fimp), "fatfs");
	if ( p->kind == FS_KIND_FAT32 ) {
		if ( b[66] == 0x29 ) put_label(p->label, sizeof(p->label), b + 71, 11);
	} else if ( b[38] == 0x29 ) {
		put_label(p->label, sizeof(p->label), b + 43, 11);
	}
	if ( same_bytes(p->label, "NO NAME", 8) ) {
		p->label[0] = 0;
	}
	return TRUE;
}

/* The BTRON file system's system header at one place: the disk ID, either way round, and a format of it */
LOCAL BOOL btron_at( CONST UB *h )
{
	UW	id = le16(h), fmt;

	if ( id == 0x42FE || id == 0x52FE ) {		/* the semi-standard form, low byte first */
		fmt = le16(h + 2);
	} else if ( be16(h) == 0x42FE || be16(h) == 0x52FE ) {	/* the standard form */
		fmt = be16(h + 2);
	} else {
		return FALSE;
	}
	return (BOOL)( fmt >= 0x6400 && fmt <= 0x6402 );
}

/*
 * The kind of what the bytes read from the start hold. 'len' is how
 * many there are, 'blksz' the device's block, 'parted' whether the
 * device is a whole disk whose partitions are its subunits.
 */
LOCAL void probe_bytes( CONST UB *b, INT len, INT blksz, BOOL parted, T_FSPROBE *p )
{
	INT	off, i;

	/*
	 * The native TSFS: its superblock is block 0 and block 1, written in
	 * turn; the good copy of the higher generation is the one now.
	 */
	if ( len >= 2 * 4096 && ( same_bytes(b, TSFSBLK_MAGIC, 8) || same_bytes(b + 4096, TSFSBLK_MAGIC, 8) ) ) {
		CONST UB *sb = NULL;

		for ( i = 0; i < 2; i++ ) {
			CONST UB *c = b + i * 4096;

			if ( same_bytes(c, TSFSBLK_MAGIC, 8) && le32(c + TSFS_SB_CRC) == ts_crc32c(c, TSFS_SB_CRC)
			  && ( sb == NULL || le64(c + TSFS_SB_GEN) > le64(sb + TSFS_SB_GEN) ) ) {
				sb = c;
			}
		}
		p->kind = FS_KIND_TSFS;
		put_str(p->name, sizeof(p->name), "TSFS");
		if ( sb != NULL ) {
			for ( i = 0; i < TSFSBLK_LABEL_MAX && i < (INT)sizeof(p->label) - 1
				     && sb[TSFS_SB_LABEL + i] != 0; i++ ) {
				p->label[i] = sb[TSFS_SB_LABEL + i];
			}
			p->label[i] = 0;
			knl_memcpy(p->vuuid.b, sb + TSFS_SB_VOLUUID, 16);
			knl_memcpy(p->top.b, sb + TSFS_SB_TOPUUID, 16);
		}
		return;
	}
	if ( len >= 512 && blksz == 512 ) {
		if ( same_bytes(b + 3, "EXFAT   ", 8) ) {
			p->kind = FS_KIND_EXFAT;
			put_str(p->name, sizeof(p->name), "exFAT");
			return;
		}
		if ( same_bytes(b + 3, "NTFS    ", 8) ) {
			p->kind = FS_KIND_NTFS;
			put_str(p->name, sizeof(p->name), "NTFS");
			return;
		}
		if ( fat_of(b, p) ) {
			return;
		}
	}
	if ( len >= 1024 + 128 && le16(b + 1024 + 56) == 0xEF53 ) {
		p->kind = FS_KIND_EXT;
		put_str(p->name, sizeof(p->name), "ext2/3/4");
		put_label(p->label, sizeof(p->label), b + 1024 + 120, 16);
		return;
	}
	/* ISO 9660: the volume descriptors; a BTRON one carries a partition of the BTRON file system */
	for ( i = ISO_VD_FIRST; ( i + 1 ) * ISO_SECT <= len; i++ ) {
		CONST UB *d = b + i * ISO_SECT;

		if ( !same_bytes(d + 1, "CD001", 5) ) break;
		if ( d[0] == 3 && same_bytes(d + 8, "BTRON SPECIFIED FILE SYSTEM", 27) ) {
			p->kind = FS_KIND_BTRON;
			put_str(p->name, sizeof(p->name), "BTRON CD-ROM");
			return;
		}
		if ( d[0] == 1 && p->kind == FS_KIND_NONE ) {
			p->kind = FS_KIND_ISO9660;
			put_str(p->name, sizeof(p->name), "ISO 9660");
			put_label(p->label, sizeof(p->label), d + 40, 32);
		}
		if ( d[0] == 255 ) break;
	}
	if ( p->kind != FS_KIND_NONE ) {
		return;
	}
	/* a partition image of the BTRON file system: its header at a sector of the first 64 KB */
	for ( off = 512; off + 128 <= len && off < BTRON_SCAN; off += 512 ) {
		if ( btron_at(b + off) ) {
			p->kind = FS_KIND_BTRON;
			put_str(p->name, sizeof(p->name), "BTRON");
			return;
		}
	}
	/* a partition table: the volumes are in the partitions */
	if ( parted && len >= 512 && le16(b + 510) == 0xAA55 ) {
		p->kind = FS_KIND_PARTED;
		put_str(p->name, sizeof(p->name), "partitioned");
		return;
	}
	for ( i = 0; i < len && b[i] == 0; i++ ) ;
	if ( i >= len ) {
		p->kind = FS_KIND_BLANK;
		put_str(p->name, sizeof(p->name), "blank");
		return;
	}
	p->kind = FS_KIND_UNKNOWN;
	put_str(p->name, sizeof(p->name), "unknown");
}

EXPORT ER knl_fs_probe( CONST char *devnm, T_FSPROBE *p )
{
	T_RDEV		rd;
	DiskInfo	di;
	UB		*buf;
	SZ		asz = 0;
	INT		len, n, k;
	ID		dd;
	ER		er;

	if ( devnm == NULL || p == NULL ) {
		return E_PAR;
	}
	knl_memset(p, 0, sizeof(*p));
	if ( tk_ref_dev((CONST UB *)devnm, &rd) < E_OK ) {
		return E_NOEXS;
	}
	dd = tk_opn_dev((CONST UB *)devnm, TD_READ);
	if ( dd < E_OK ) {
		return (ER)dd;
	}
	er = tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz);
	if ( er < E_OK || di.blocksize <= 0 || di.blockcont_d <= 0 ) {
		tk_cls_dev(dd, 0);
		return ( er < E_OK ) ? er : E_NOMDA;
	}
	len = PROBE_BYTES;
	if ( di.blockcont_d * di.blocksize < len ) {
		len = (INT)( di.blockcont_d * di.blocksize );
	}
	len -= len % di.blocksize;
	buf = (UB *)Kmalloc((SZ)len + 4096);
	if ( buf == NULL ) {
		tk_cls_dev(dd, 0);
		return E_NOMEM;
	}
	knl_memset(buf, 0, (SZ)len + 4096);
	n = len;
	er = tk_srea_dev(dd, 0, buf, (SZ)n, &asz);
	if ( er < E_OK && n > 4096 ) {
		/* a medium shorter than it says, or one that fails far in: the first blocks alone */
		n = 4096 - 4096 % di.blocksize;
		er = tk_srea_dev(dd, 0, buf, (SZ)n, &asz);
	}
	tk_cls_dev(dd, 0);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	/* a whole disk: one whose name ends in a letter has its partitions as subunits */
	k = str_len(devnm);
	probe_bytes(buf, n, di.blocksize,
		    (BOOL)( rd.nsub > 0 && k > 0 && !( devnm[k - 1] >= '0' && devnm[k - 1] <= '9' ) ), p);
	Kfree(buf);
	p->bytes = (UD)di.blockcont_d * (UD)di.blocksize;
	p->rdonly = (BOOL)di.protect;

	return E_OK;
}

/* The fimp a kind is mounted with, "" for none */
EXPORT CONST char *knl_fs_kind_fimp( UINT kind )
{
	return ( kind == FS_KIND_FAT12 || kind == FS_KIND_FAT16 || kind == FS_KIND_FAT32 ) ? "fatfs" : "";
}
