/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	si_core.c
 *	システム登録: the system the PC was started from put on an internal
 *	disk (si_core.h, docs/tessronos/03-platform.md 3.10.10).
 *
 *	What is written, and where:
 *
 *	  SI_COEXIST	the partition table only through knl_gpt_add (one
 *			entry added after those in use: the partitions
 *			there were keep their numbers and their bytes); the
 *			volume through the new partition's own device, which
 *			ends where the partition ends; the ESP's files
 *			through the FAT file system, under \EFI\TESSRON\.
 *	  SI_REPLACE	the chosen partition's entry only, through
 *			knl_gpt_retype (the TSFS type, a new unique GUID;
 *			where it lies and its number stay) and knl_gpt_grow
 *			when a free stretch follows it; then as SI_COEXIST.
 *			What the partition held is written over by the copy,
 *			nothing else of the disk is.
 *	  SI_ERASE	the whole disk opened alone (TD_EXCL: nothing of it
 *			in use, and nothing opens it meanwhile), a new table
 *			written whole, the volume and a FAT32 volume made
 *			here at their places. The driver keeps the
 *			partitions it read until the next start
 *			(knl_part_rescan does not let one in use move): the
 *			volume is grown to its partition then.
 *
 *	The copy of the volume is a dump: with the journal frozen and what
 *	it holds put in place (knl_jrnl_freeze, ts_sync_blk), the medium is
 *	the volume as one moment left it. The blocks before the first group
 *	(the superblocks and the journal), each group's own blocks, and
 *	every block its map says is taken are read from the partition's
 *	device and written in runs; a CRC of each run is kept, and the copy
 *	is read back against them once the volume is thawed.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/gpt.h>
#include <ts/fs.h>
#include <ts/dt.h>
#include <ts/uuid.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/ob.h>
#include "tstdlib.h"
#include "si_core.h"

IMPORT unsigned char	knl_sysvol_name[];
IMPORT unsigned char	knl_bootvol_name[];

#define SECT		512
#define BLK_SECT	TSFSBLK_SECT_PER_BLK
#define MB_SECT		2048ULL
#define ENT_N		128		/* entries of a table made here */
#define ENT_SZ		128
#define ENT_SECT	( ENT_N * ENT_SZ / SECT )
#define RUN_MAX		256		/* blocks of one request of the copy, at most */
#define FREEZE_TRIES	6

#define BOOT_DIR	"/boot/EFI/TESSRON"
#define REQ_PATH	BOOT_DIR "/BOOTREG.TXT"
#define LOADER_MARK	"TessronOS loader"

/* The disks a PC has inside, in the order the kernel looks at them */
LOCAL CONST char * CONST cands[] = {
	"nvmea", "nvmeb", "nvmec", "nvmed",
	"sataa", "satab", "satac", "satad",
	NULL
};

/* ---------------------------------------------------------------- small things */

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UD le64( CONST UB *p )
{
	return (UD)le32(p) | ( (UD)le32(p + 4) << 32 );
}

LOCAL void put16( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)( v >> 8 );
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

LOCAL BOOL same_n( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL zero16( CONST UB *g )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( g[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* The CRC-32 of IEEE 802.3, as the GPT and the tests have it */
EXPORT UW si_crc32( UW crc, CONST UB *p, UD n )
{
	UW	c = ~crc;
	INT	k;

	while ( n-- > 0 ) {
		c ^= *p++;
		for ( k = 0; k < 8; k++ ) {
			c = ( c & 1 ) ? ( c >> 1 ) ^ 0xEDB88320U : ( c >> 1 );
		}
	}
	return ~c;
}

LOCAL INT copy_str( UB *to, INT max, CONST char *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != '\0' && n < max - 1 ) {
		to[n] = (UB)s[n];
		n++;
	}
	to[n] = 0;
	return n;
}

/* A partition's device: the disk's name and the subunit */
LOCAL void name_of( UB *out, CONST UB *disk, INT unit )
{
	INT	n = 0;

	while ( disk[n] != 0 && n < L_DEVNM - 3 ) {
		out[n] = disk[n];
		n++;
	}
	if ( unit >= 100 ) out[n++] = (UB)( '0' + unit / 100 );
	if ( unit >= 10 ) out[n++] = (UB)( '0' + unit / 10 % 10 );
	out[n++] = (UB)( '0' + unit % 10 );
	out[n] = 0;
}

/* The disk of a partition's device: the name without the number at its end */
LOCAL void disk_of( UB *out, CONST UB *part )
{
	INT	n = 0;

	while ( part[n] != 0 && n < L_DEVNM ) {
		out[n] = part[n];
		n++;
	}
	while ( n > 0 && out[n - 1] >= '0' && out[n - 1] <= '9' ) n--;
	out[n] = 0;
}

LOCAL ER rd( ID dd, UD lba, UD n, void *buf )
{
	SZ	asz;

	return tk_srea_dev_d(dd, (D)lba, buf, (SZ)( n * SECT ), &asz);
}

LOCAL ER wr( ID dd, UD lba, UD n, CONST void *buf )
{
	SZ	asz;

	return tk_swri_dev_d(dd, (D)lba, buf, (SZ)( n * SECT ), &asz);
}

LOCAL void dev_flush( ID dd )
{
	SZ	asz;

	(void)tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asz);
}

/* The most bytes one request of a device moves, or 0 when it does not say */
LOCAL UW io_max( ID dd )
{
	UW	n = 0;
	SZ	asz;

	if ( tk_srea_dev(dd, TDN_IOMAX, &n, sizeof(n), &asz) < E_OK ) {
		return 0;
	}
	return n;
}

/* Memory a device can read and write, on a page */
LOCAL UB *io_buf( SZ size, void **p_raw )
{
	UB	*raw = (UB *)Kmalloc(size + 4096);

	*p_raw = raw;
	return ( raw == NULL ) ? NULL : (UB *)( ( (UBINT)raw + 4095 ) & ~(UBINT)4095 );
}

/* ---------------------------------------------------------------- partition types */

/* The types, as they lie in an entry */
LOCAL CONST UB	g_esp[16] = {
	0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};
LOCAL CONST UB	g_msr[16] = {
	0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d, 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae
};
LOCAL CONST UB	g_winre[16] = {
	0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d, 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac
};
LOCAL CONST UB	g_linux[16] = {
	0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};

LOCAL INT kind_of( CONST UB *type )
{
	if ( same_n(type, g_esp, 16) ) return SI_K_ESP;
	if ( same_n(type, g_msr, 16) ) return SI_K_MSR;
	if ( same_n(type, knl_basic_data_guid, 16) ) return SI_K_BASIC;
	if ( same_n(type, g_winre, 16) ) return SI_K_WINRE;
	if ( same_n(type, knl_tsfs_type_guid, 16) ) return SI_K_TSFS;
	if ( same_n(type, g_linux, 16) ) return SI_K_LINUX;
	return SI_K_OTHER;
}

EXPORT CONST char *si_kind_name( INT kind )
{
	switch ( kind ) {
	case SI_K_ESP:		return "EFIシステム";
	case SI_K_MSR:		return "Microsoft予約";
	case SI_K_BASIC:	return "基本データ";
	case SI_K_WINRE:	return "Windows回復";
	case SI_K_TSFS:		return "TessronOS";
	case SI_K_LINUX:	return "Linux";
	default:		return "その他";
	}
}

/*
 * A Windows volume, Linux or TessronOS; not one the firmware or the
 * maker needs (bit 0, "required"), nor a basic data one Windows is
 * told to leave without a letter (bit 63), as a maker's recovery image
 * is kept
 */
EXPORT BOOL si_replaceable( CONST T_SIPART *p )
{
	if ( ( p->attr & 1ULL ) != 0 ) {
		return FALSE;
	}
	switch ( p->kind ) {
	case SI_K_BASIC:	return (BOOL)( ( p->attr & ( 1ULL << 63 ) ) == 0 );
	case SI_K_LINUX:
	case SI_K_TSFS:		return TRUE;
	default:		return FALSE;
	}
}

/* A partition's name: UTF-16LE in the entry, to UTF-8 (the plane of 16 bits only) */
LOCAL void name_utf8( CONST UB *e, UB *out )
{
	INT	i, n = 0;

	for ( i = 0; i < GPT_NAME_MAX; i++ ) {
		UW	c = (UW)e[56 + 2 * i] | ( (UW)e[57 + 2 * i] << 8 );

		if ( c == 0 ) break;
		if ( c < 0x80 ) {
			out[n++] = (UB)c;
		} else if ( c < 0x800 ) {
			out[n++] = (UB)( 0xC0 | ( c >> 6 ) );
			out[n++] = (UB)( 0x80 | ( c & 0x3F ) );
		} else if ( c < 0xD800 || c > 0xDFFF ) {
			out[n++] = (UB)( 0xE0 | ( c >> 12 ) );
			out[n++] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
			out[n++] = (UB)( 0x80 | ( c & 0x3F ) );
		}
	}
	out[n] = 0;
}

/* ---------------------------------------------------------------- the disks */

/* The disk the system was started from: that of /boot */
LOCAL void boot_disk( UB *out )
{
	disk_of(out, (CONST UB *)knl_bootvol_name);
}

/* A table's header that passes, and its entries that pass too: the entries in `ent` (Kfree) */
LOCAL UB *hdr_take( ID dd, CONST UB *h, UD my, UD nsect, UW *p_nent, UW *p_entsz )
{
	UB	tmp[92];
	UW	size = le32(h + 12), nent = le32(h + 80), entsz = le32(h + 84);
	UD	elba = le64(h + 72), esect;
	UB	*ent;

	if ( !same_n(h, (CONST UB *)"EFI PART", 8) || size != 92 || le64(h + 24) != my ) {
		return NULL;
	}
	knl_memcpy(tmp, h, 92);
	put32(tmp + 16, 0);
	if ( si_crc32(0, tmp, 92) != le32(h + 16) ) {
		return NULL;
	}
	if ( entsz < 128 || entsz % 8 != 0 || nent == 0 || (UD)nent * entsz > 128 * 1024
	  || elba < 2 || elba >= nsect ) {
		return NULL;
	}
	esect = ( (UD)nent * entsz + SECT - 1 ) / SECT;
	ent = (UB *)Kcalloc(esect, SECT);
	if ( ent == NULL ) {
		return NULL;
	}
	if ( rd(dd, elba, esect, ent) < E_OK || si_crc32(0, ent, (UD)nent * entsz) != le32(h + 88) ) {
		Kfree(ent);
		return NULL;
	}
	*p_nent = nent;
	*p_entsz = entsz;
	return ent;
}

EXPORT ER si_disk_read( CONST char *disk, T_SIDISK *pk )
{
	DiskInfo	info;
	T_GPTFREE	fr[16];
	UB		*s = NULL, *ent = NULL;
	UW		nent = 0, entsz = 0, i;
	SZ		asz;
	ID		dd;
	INT		n, k;
	ER		er = E_OK;

	knl_memset(pk, 0, sizeof(*pk));
	pk->esp = pk->tsfs = -1;
	(void)copy_str(pk->name, sizeof(pk->name), disk);
	dd = tk_opn_dev((CONST UB *)disk, TD_READ);
	if ( dd < E_OK ) {
		return E_NOEXS;
	}
	if ( tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asz) < E_OK || info.blocksize != SECT
	  || info.blockcont_d < (D)( 64 * MB_SECT ) ) {
		er = E_NOSPT;
		goto out;
	}
	pk->nsect = (UD)info.blockcont_d;
	s = (UB *)Kmalloc(SECT * 2);
	if ( s == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	if ( rd(dd, 0, 2, s) < E_OK ) {
		er = E_IO;
		goto out;
	}
	pk->blank = TRUE;
	for ( k = 0; k < SECT * 2; k++ ) {
		if ( s[k] != 0 ) pk->blank = FALSE;
	}
	ent = hdr_take(dd, s + SECT, 1, pk->nsect, &nent, &entsz);
	if ( ent == NULL && rd(dd, pk->nsect - 1, 1, s + SECT) >= E_OK ) {
		ent = hdr_take(dd, s + SECT, pk->nsect - 1, pk->nsect, &nent, &entsz);
	}
	pk->gpt = (BOOL)( ent != NULL );
	for ( i = 0; ent != NULL && i < nent; i++ ) {
		UB		*e = ent + (UD)i * entsz;
		T_SIPART	*p;
		UD		f = le64(e + 32), l = le64(e + 40);

		/* the partitions as part.c numbers them: the entries in use that lie on the disk */
		if ( zero16(e) || l < f || l >= pk->nsect ) {
			continue;
		}
		if ( pk->nparts >= SI_PART_MAX ) {
			break;
		}
		p = &pk->parts[pk->nparts];
		p->unit = pk->nparts;
		p->entry = (INT)i;
		p->first = f;
		p->last = l;
		knl_memcpy(p->type, e, 16);
		knl_memcpy(p->uniq, e + 16, 16);
		p->attr = le64(e + 48);
		p->kind = kind_of(e);
		name_utf8(e, p->name);
		if ( p->kind == SI_K_BASIC && rd(dd, f, 1, s) >= E_OK && same_n(s + 3, (CONST UB *)"-FVE-FS-", 8) ) {
			p->bitlocker = TRUE;
			pk->bitlocker = TRUE;
		}
		if ( p->kind == SI_K_ESP && pk->esp < 0 ) pk->esp = pk->nparts;
		if ( p->kind == SI_K_TSFS && pk->tsfs < 0 ) pk->tsfs = pk->nparts;
		pk->nparts++;
	}
	/* the largest free stretch, on whole megabytes, as a partition made there would have it */
	if ( pk->gpt || pk->blank ) {
		n = knl_gpt_free(disk, fr, 16, NULL, NULL);
		for ( k = 0; k < n; k++ ) {
			if ( fr[k].last - fr[k].first > pk->free_last - pk->free_first || pk->free_last == 0 ) {
				pk->free_first = fr[k].first;
				pk->free_last = fr[k].last;
			}
		}
	}
out:
	if ( ent != NULL ) Kfree(ent);
	if ( s != NULL ) Kfree(s);
	tk_cls_dev(dd, 0);
	return er;
}

EXPORT INT si_disks( T_SIDISK *buf, INT max )
{
	UB	boot[L_DEVNM + 1];
	INT	i, n = 0;

	boot_disk(boot);
	for ( i = 0; cands[i] != NULL && n < max; i++ ) {
		if ( knl_strcmp(cands[i], (CONST char *)boot) == 0 ) {
			continue;		/* the one the system runs from */
		}
		if ( si_disk_read(cands[i], &buf[n]) >= E_OK ) {
			n++;
		}
	}
	return n;
}

/* Whether a partition of the disk is in use: a volume of objects open on it, or a file system mounted */
LOCAL BOOL in_use( CONST T_SIDISK *dk, INT except )
{
	T_FSMNT	*mt;
	UB	dev[L_DEVNM + 1];
	INT	i, k, n;
	BOOL	used = FALSE;

	for ( i = 0; i < dk->nparts && !used; i++ ) {
		name_of(dev, dk->name, dk->parts[i].unit);
		if ( i != except && ts_vol_blk_of((CONST char *)dev) > 0 ) used = TRUE;
	}
	n = fs_mounts(NULL, 0);
	if ( used || n <= 0 ) {
		return used;
	}
	mt = (T_FSMNT *)Kmalloc(sizeof(T_FSMNT) * (SZ)n);
	if ( mt == NULL ) {
		return TRUE;
	}
	n = fs_mounts(mt, n);
	for ( k = 0; k < n && !used; k++ ) {
		for ( i = 0; i < dk->nparts; i++ ) {
			name_of(dev, dk->name, dk->parts[i].unit);
			if ( i != except && knl_strcmp((CONST char *)mt[k].dev, (CONST char *)dev) == 0 ) used = TRUE;
		}
		if ( knl_strcmp((CONST char *)mt[k].dev, (CONST char *)dk->name) == 0 ) used = TRUE;
	}
	Kfree(mt);
	return used;
}

/* ---------------------------------------------------------------- the source */

/* Whether a file holds the bytes of s */
LOCAL BOOL file_has( CONST char *path, CONST char *s, UD *p_size )
{
	T_FSTAT	st;
	UB	*buf;
	INT	fd, n, k, m = (INT)knl_strlen(s);
	BOOL	got = FALSE;

	if ( fs_stat(path, &st) < EX_OK || ( st.mode & FS_IFMT ) != FS_IFREG || st.size == 0
	  || st.size > ( 16ULL << 20 ) ) {
		return FALSE;
	}
	buf = (UB *)Kmalloc((SZ)st.size);
	if ( buf == NULL ) {
		return FALSE;
	}
	fd = fs_open(path, O_RDONLY);
	n = ( fd >= 0 ) ? fs_read(fd, buf, (SZ)st.size) : -1;
	if ( fd >= 0 ) fs_close(fd);
	for ( k = 0; n > m && k <= n - m && !got; k++ ) {
		got = same_n(buf + k, (CONST UB *)s, m);
	}
	Kfree(buf);
	*p_size = st.size;
	return got;
}

EXPORT ER si_boot_files( char (*path)[64], UD *size )
{
	static CONST char * CONST loader[] = { BOOT_DIR "/BOOTX64.EFI", "/boot/EFI/BOOT/BOOTX64.EFI", NULL };
	static CONST char * CONST kernel[] = { BOOT_DIR "/tessronos.elf", "/boot/tessronos.elf", NULL };
	static CONST char * CONST cmdline[] = { BOOT_DIR "/cmdline.txt", "/boot/cmdline.txt", NULL };
	CONST char * CONST	*list[SI_NFILE] = { loader, kernel, cmdline };
	T_FSTAT			st;
	INT			f, i;

	for ( f = 0; f < SI_NFILE; f++ ) {
		path[f][0] = 0;
		size[f] = 0;
		for ( i = 0; list[f][i] != NULL && path[f][0] == 0; i++ ) {
			if ( f == SI_F_LOADER ) {
				/* the loader that is this system's: a Windows ESP has one of the same name */
				if ( file_has(list[f][i], LOADER_MARK, &size[f]) ) {
					(void)copy_str((UB *)path[f], 64, list[f][i]);
				}
			} else if ( fs_stat(list[f][i], &st) >= EX_OK && ( st.mode & FS_IFMT ) == FS_IFREG ) {
				(void)copy_str((UB *)path[f], 64, list[f][i]);
				size[f] = st.size;
			}
		}
	}
	return ( path[SI_F_LOADER][0] != 0 && path[SI_F_KERNEL][0] != 0 ) ? E_OK : E_NOEXS;
}

EXPORT ER si_source( T_SIPLAN *pl )
{
	T_TSFSBLK_SB	sb;
	char		path[SI_NFILE][64];
	UD		size[SI_NFILE];
	ID		vol;
	INT		f;

	(void)copy_str(pl->src, sizeof(pl->src), (CONST char *)knl_sysvol_name);
	if ( pl->src[0] == 0 ) {
		return E_NOEXS;
	}
	vol = ts_vol_blk_of((CONST char *)pl->src);
	if ( vol <= 0 || ts_ref_vol_blk(vol, &sb) < E_OK ) {
		return E_NOEXS;
	}
	pl->src_blocks = sb.total_blocks;
	pl->src_used = sb.total_blocks - sb.free_blocks;
	if ( si_boot_files(path, size) < E_OK ) {
		return E_NOEXS;
	}
	pl->esp_bytes = 0;
	for ( f = 0; f < SI_NFILE; f++ ) {
		pl->esp_bytes += ( size[f] + 4095 ) / 4096 * 4096;
	}
	pl->esp_bytes += 4096;			/* INSTALL.TXT */
	return E_OK;
}

/* ---------------------------------------------------------------- the plan */

/* Bytes free on an ESP, with what this system's files take there now */
LOCAL ER esp_room( CONST UB *dev, UD *p_free )
{
	static CONST char * CONST mine[] = {
		"EFI/TESSRON/BOOTX64.EFI", "EFI/TESSRON/tessronos.elf", "EFI/TESSRON/cmdline.txt", NULL
	};
	T_FSSTAT	fst;
	T_FSTAT		st;
	INT		vd, i;
	ER		er;

	vd = knl_fs_open_vol("fatfs", (CONST char *)dev, FS_MNT_RDONLY, 0);
	if ( vd < 0 ) {
		return vd;
	}
	er = fs_fstatvfs(vd, &fst);
	if ( er >= EX_OK ) {
		*p_free = fst.bfree * fst.bsize;
		for ( i = 0; mine[i] != NULL; i++ ) {
			if ( fs_statat(vd, mine[i], &st) >= EX_OK ) *p_free += st.size;
		}
	}
	fs_close(vd);
	return er;
}

EXPORT ER si_plan( CONST char *disk, INT mode, INT unit, T_SIPLAN *pl )
{
	T_SIDISK	*dk;
	UB		boot[L_DEVNM + 1], dev[L_DEVNM + 1];
	UD		need, lu;
	INT		i;
	ER		er;

	knl_memset(pl, 0, sizeof(*pl));
	pl->mode = mode;
	pl->ts_unit = pl->esp_unit = pl->esp_entry = -1;
	(void)copy_str(pl->disk, sizeof(pl->disk), disk);
	dk = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK));
	if ( dk == NULL ) {
		return E_NOMEM;
	}
	boot_disk(boot);
	er = si_disk_read(disk, dk);
	if ( er < E_OK ) {
		pl->why = SI_R_NODISK;
		goto out;
	}
	pl->nsect = dk->nsect;
	if ( knl_strcmp(disk, (CONST char *)boot) == 0 ) {
		pl->why = SI_R_BOOTDISK;
		goto out;
	}
	if ( si_source(pl) < E_OK ) {
		pl->why = SI_R_NOSYS;
		goto out;
	}
	need = pl->src_blocks * BLK_SECT;		/* sectors the volume takes */

	if ( mode == SI_COEXIST || mode == SI_REPLACE ) {
		if ( !dk->gpt ) {
			pl->why = SI_R_NOGPT;
			goto out;
		}
		if ( dk->esp < 0 ) {
			pl->why = SI_R_NOESP;
			goto out;
		}
		pl->esp_entry = dk->parts[dk->esp].entry;
		pl->esp_unit = dk->parts[dk->esp].unit;
		pl->esp_first = dk->parts[dk->esp].first;
		pl->esp_last = dk->parts[dk->esp].last;
		knl_memcpy(pl->esp_uniq, dk->parts[dk->esp].uniq, 16);
		if ( in_use(dk, dk->esp) ) {
			pl->why = SI_R_INUSE;
			goto out;
		}
		if ( mode == SI_REPLACE ) {
			T_GPTPART	gp;
			CONST T_SIPART	*p = NULL;

			for ( i = 0; i < dk->nparts; i++ ) {
				if ( dk->parts[i].unit == unit ) p = &dk->parts[i];
			}
			if ( p == NULL || !si_replaceable(p) ) {
				pl->why = SI_R_KEEP;
				goto out;
			}
			pl->lost[0] = *p;
			pl->nlost = 1;
			/* as far as the free stretch right after it goes */
			pl->ts_unit = p->unit;
			pl->ts_first = p->first;
			pl->ts_last = p->last;
			name_of(dev, dk->name, p->unit);
			if ( knl_gpt_part((CONST char *)dev, &gp) >= E_OK && gp.grow_last > p->last ) {
				pl->ts_last = gp.grow_last;
			}
		} else if ( dk->tsfs >= 0 ) {
			/* a system put here before: its partition written again */
			pl->ts_unit = dk->parts[dk->tsfs].unit;
			pl->ts_first = dk->parts[dk->tsfs].first;
			pl->ts_last = dk->parts[dk->tsfs].last;
		} else {
			pl->ts_first = dk->free_first;
			pl->ts_last = dk->free_last;
		}
		if ( pl->ts_last == 0 || pl->ts_last - pl->ts_first + 1 < need ) {
			pl->why = SI_R_NOROOM;
			goto out;
		}
		name_of(dev, dk->name, pl->esp_unit);
		if ( esp_room(dev, &pl->esp_free) < E_OK || pl->esp_free < pl->esp_bytes + SI_ESP_SLACK ) {
			pl->why = SI_R_ESPFULL;
			goto out;
		}
	} else {
		TS_UUID	u;
		UD	mb = ( dk->nsect >= ( 32ULL << 30 ) / SECT ) ? SI_ESP_MB_LARGE : SI_ESP_MB;

		if ( in_use(dk, -1) ) {
			pl->why = SI_R_INUSE;
			goto out;
		}
		pl->esp_entry = 0;
		pl->esp_unit = 0;
		pl->esp_first = MB_SECT;
		pl->esp_last = pl->esp_first + mb * MB_SECT - 1;
		pl->ts_first = pl->esp_last + 1;
		lu = dk->nsect - 1 - ENT_SECT - 1;	/* the last sector before the backup table */
		pl->ts_last = ( lu + 1 ) / MB_SECT * MB_SECT - 1;
		if ( pl->ts_last <= pl->ts_first || pl->ts_last - pl->ts_first + 1 < need
		  || pl->esp_bytes + SI_ESP_SLACK > mb * MB_SECT * SECT / 2 ) {
			pl->why = SI_R_SMALL;
			goto out;
		}
		for ( i = 0; i < dk->nparts && i < SI_PART_MAX; i++ ) {
			pl->lost[i] = dk->parts[i];
		}
		pl->nlost = dk->nparts;
		(void)ts_gen_uuid(&u);
		knl_memcpy(pl->disk_guid, u.b, 16);
		(void)ts_gen_uuid(&u);
		knl_memcpy(pl->esp_uniq, u.b, 16);
		(void)ts_gen_uuid(&u);
		knl_memcpy(pl->ts_uniq, u.b, 16);
	}
	/* the Windows volumes that stay: none (erased, or the last one made TessronOS's) puts the entry first */
	pl->nwin = 0;
	for ( i = 0; i < dk->nparts && mode != SI_ERASE; i++ ) {
		if ( dk->parts[i].kind != SI_K_BASIC ) continue;
		if ( mode == SI_REPLACE && dk->parts[i].unit == pl->ts_unit ) continue;
		pl->nwin++;
	}
	pl->why = SI_R_OK;
out:
	Kfree(dk);
	return ( er == E_NOMEM ) ? er : E_OK;
}

/* ---------------------------------------------------------------- a table made whole */

EXPORT ER si_gpt_write( ID dd, UD nsect, CONST UB *disk_guid, CONST T_SINEWPART *p, INT n )
{
	UB	*ent, *h, *mbr;
	UD	lu = nsect - 1 - ENT_SECT - 1, back_ent = nsect - 1 - ENT_SECT;
	UW	ecrc;
	INT	i, k, m;
	ER	er;

	if ( n <= 0 || n > ENT_N ) {
		return E_PAR;
	}
	ent = (UB *)Kcalloc(ENT_SECT + 2, SECT);
	if ( ent == NULL ) {
		return E_NOMEM;
	}
	h = ent + ENT_SECT * SECT;
	mbr = h + SECT;
	for ( i = 0; i < n; i++ ) {
		UB	*e = ent + i * ENT_SZ;

		if ( p[i].first < 2 + ENT_SECT || p[i].last > lu || p[i].last < p[i].first ) {
			Kfree(ent);
			return E_PAR;
		}
		knl_memcpy(e, p[i].type, 16);
		knl_memcpy(e + 16, p[i].uniq, 16);
		put64(e + 32, p[i].first);
		put64(e + 40, p[i].last);
		for ( k = 0, m = 0; p[i].name != NULL && p[i].name[k] != '\0' && m < GPT_NAME_MAX; k++, m++ ) {
			e[56 + 2 * m] = (UB)p[i].name[k];	/* the names made here are of ASCII */
		}
	}
	ecrc = si_crc32(0, ent, ENT_N * ENT_SZ);

	/* the protective MBR: one partition of type 0xEE over the disk */
	knl_memset(mbr, 0, SECT);
	mbr[446 + 2] = 0x02;
	mbr[446 + 4] = 0xEE;
	mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
	put32(mbr + 446 + 8, 1);
	put32(mbr + 446 + 12, ( nsect - 1 > 0xFFFFFFFFULL ) ? 0xFFFFFFFFU : (UW)( nsect - 1 ));
	mbr[510] = 0x55; mbr[511] = 0xAA;

	knl_memset(h, 0, SECT);
	knl_memcpy(h, "EFI PART", 8);
	put32(h + 8, 0x00010000);
	put32(h + 12, 92);
	put64(h + 40, 2 + ENT_SECT);
	put64(h + 48, lu);
	knl_memcpy(h + 56, disk_guid, 16);
	put32(h + 80, ENT_N);
	put32(h + 84, ENT_SZ);
	put32(h + 88, ecrc);

	/* the backup at the end of the disk, then the table at its start */
	put64(h + 24, nsect - 1);
	put64(h + 32, 1);
	put64(h + 72, back_ent);
	put32(h + 16, 0);
	put32(h + 16, si_crc32(0, h, 92));
	er = wr(dd, back_ent, ENT_SECT, ent);
	if ( er >= E_OK ) er = wr(dd, nsect - 1, 1, h);
	if ( er >= E_OK ) {
		dev_flush(dd);
		put64(h + 24, 1);
		put64(h + 32, nsect - 1);
		put64(h + 72, 2);
		put32(h + 16, 0);
		put32(h + 16, si_crc32(0, h, 92));
		er = wr(dd, 2, ENT_SECT, ent);
	}
	if ( er >= E_OK ) er = wr(dd, 1, 1, h);
	if ( er >= E_OK ) er = wr(dd, 0, 1, mbr);
	dev_flush(dd);
	Kfree(ent);
	return er;
}

/* ---------------------------------------------------------------- the files of the ESP */

/* One file put on an ESP: from a file of /boot, or from memory */
typedef struct {
	CONST char	*dst;		/* below the ESP's root, "EFI/TESSRON/tessronos.elf" */
	char		src[64];	/* a path of /boot, "" for mem */
	CONST UB	*mem;
	UD		size;
} SIFILE;

#define SI_MAXFILE	6

/* What the ESP gets: this system's loader, kernel and settings, and the note of the install */
LOCAL INT esp_files( SIFILE *f, BOOL fallback, CONST char *note )
{
	char	path[SI_NFILE][64];
	UD	size[SI_NFILE];
	INT	n = 0;

	knl_memset(f, 0, sizeof(SIFILE) * SI_MAXFILE);
	if ( si_boot_files(path, size) < E_OK ) {
		return E_NOEXS;
	}
	if ( fallback ) {
		f[n].dst = "EFI/BOOT/BOOTX64.EFI";
		(void)copy_str((UB *)f[n].src, 64, path[SI_F_LOADER]);
		f[n].size = size[SI_F_LOADER];
		n++;
	}
	f[n].dst = "EFI/TESSRON/BOOTX64.EFI";
	(void)copy_str((UB *)f[n].src, 64, path[SI_F_LOADER]);
	f[n].size = size[SI_F_LOADER];
	n++;
	f[n].dst = "EFI/TESSRON/tessronos.elf";
	(void)copy_str((UB *)f[n].src, 64, path[SI_F_KERNEL]);
	f[n].size = size[SI_F_KERNEL];
	n++;
	if ( path[SI_F_CMDLINE][0] != 0 ) {
		f[n].dst = "EFI/TESSRON/cmdline.txt";
		(void)copy_str((UB *)f[n].src, 64, path[SI_F_CMDLINE]);
		f[n].size = size[SI_F_CMDLINE];
		n++;
	}
	f[n].dst = "EFI/TESSRON/INSTALL.TXT";
	f[n].src[0] = 0;
	f[n].mem = (CONST UB *)note;
	f[n].size = knl_strlen(note);
	n++;
	return n;
}

/* The bytes of a file, `len` at a time into buf: answers how many came, 0 at its end */
typedef struct {
	CONST SIFILE	*f;
	INT		fd;
	UD		at;
} SIREAD;

LOCAL ER fread_open( SIREAD *r, CONST SIFILE *f )
{
	r->f = f;
	r->at = 0;
	r->fd = -1;
	if ( f->src[0] != 0 ) {
		r->fd = fs_open(f->src, O_RDONLY);
		if ( r->fd < 0 ) return E_IO;
	}
	return E_OK;
}

LOCAL INT fread_some( SIREAD *r, UB *buf, INT len )
{
	INT	n, got = 0;

	if ( r->fd < 0 ) {
		UD	left = r->f->size - r->at;

		n = ( (UD)len < left ) ? len : (INT)left;
		knl_memcpy(buf, r->f->mem + r->at, n);
		r->at += (UD)n;
		return n;
	}
	while ( got < len ) {
		n = fs_read(r->fd, buf + got, len - got);
		if ( n < 0 ) return E_IO;
		if ( n == 0 ) break;
		got += n;
	}
	r->at += (UD)got;
	return got;
}

LOCAL void fread_close( SIREAD *r )
{
	if ( r->fd >= 0 ) fs_close(r->fd);
	r->fd = -1;
}

/* The files written through the FAT file system of an ESP there is, below \EFI\TESSRON\ */
LOCAL ER esp_put_fs( CONST UB *dev, CONST SIFILE *f, INT nf, SI_PROG cb, void *ctx )
{
	SIREAD	r;
	UB	*buf;
	UD	total = 0, done = 0;
	INT	vd, fd, i, n;
	ER	er = E_OK;

	for ( i = 0; i < nf; i++ ) total += f[i].size;
	buf = (UB *)Kmalloc(64 * 1024);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	vd = knl_fs_open_vol("fatfs", (CONST char *)dev, 0, 0);
	if ( vd < 0 ) {
		Kfree(buf);
		return vd;
	}
	(void)fs_mkdirat(vd, "EFI");
	(void)fs_mkdirat(vd, "EFI/TESSRON");
	for ( i = 0; i < nf && er >= E_OK; i++ ) {
		er = fread_open(&r, &f[i]);
		if ( er < E_OK ) break;
		fd = fs_openat(vd, f[i].dst, O_WRONLY | O_CREAT | O_TRUNC);
		if ( fd < 0 ) {
			er = fd;
			fread_close(&r);
			break;
		}
		while ( ( n = fread_some(&r, buf, 64 * 1024) ) > 0 ) {
			if ( fs_write(fd, buf, n) != n ) {
				er = E_IO;
				break;
			}
			done += (UD)n;
			if ( cb != NULL ) cb(ctx, SI_ST_ESP, done, total);
		}
		if ( n < 0 ) er = n;
		if ( fs_fsync(fd) < EX_OK && er >= E_OK ) er = E_IO;
		if ( fs_close(fd) < EX_OK && er >= E_OK ) er = E_IO;
		fread_close(&r);
		tm_printf((UB *)"sysinst: %s: %s from %s (%d)\n", dev, f[i].dst,
			  ( f[i].src[0] != 0 ) ? f[i].src : "memory", (INT)er);
	}
	fs_close(vd);
	(void)fs_sync();
	Kfree(buf);
	return er;
}

/* ---------------------------------------------------------------- a FAT32 volume made */

#define FAT_RSVD	32
#define FAT_EOC		0x0FFFFFFFU
#define DIR_MAX		8		/* directories of the ESP made here */

typedef struct {
	CONST char	*path;		/* "", "EFI", "EFI/BOOT", "EFI/TESSRON" */
	UW		clus;
	INT		parent;		/* index, -1 for the root */
	UB		*ent;		/* its cluster, being filled */
	INT		nent;
} FATDIR;

typedef struct {
	ID	dd;
	UD	first;			/* the volume's first sector */
	UW	spc;			/* sectors of a cluster */
	UD	fatsz;			/* sectors of one table */
	UD	data;			/* the first sector of cluster 2 */
	UW	nclus;			/* clusters of the data */
	UW	*fat;
	UW	next;			/* the next cluster to take */
	UW	date, time;
	FATDIR	dir[DIR_MAX];
	INT	ndir;
} FATMK;

LOCAL UD clus_sect( FATMK *m, UW c )
{
	return m->data + (UD)( c - 2 ) * m->spc;
}

/* A run of clusters taken and chained */
LOCAL UW take( FATMK *m, UW n )
{
	UW	c = m->next, i;

	if ( n == 0 || (UD)m->next + n > (UD)m->nclus + 2 ) {
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		m->fat[c + i] = ( i == n - 1 ) ? FAT_EOC : c + i + 1;
	}
	m->next += n;
	return c;
}

/* A short name of 8.3 in the entry's form, and the case bits of its NT byte; FALSE when it needs a long name */
LOCAL BOOL short_name( CONST char *name, UB *n83, UB *p_case )
{
	INT	i, k = 0, dot = -1, len = (INT)knl_strlen(name);
	BOOL	lo_b = FALSE, up_b = FALSE, lo_e = FALSE, up_e = FALSE;

	for ( i = 0; i < len; i++ ) {
		if ( name[i] == '.' ) dot = i;
	}
	if ( ( dot < 0 && len > 8 ) || ( dot >= 0 && ( dot > 8 || len - dot - 1 > 3 || dot == 0 ) ) ) {
		return FALSE;
	}
	knl_memset(n83, ' ', 11);
	for ( i = 0; i < len; i++ ) {
		char	c = name[i];
		BOOL	ext = (BOOL)( dot >= 0 && i > dot );

		if ( i == dot ) {
			k = 8;
			continue;
		}
		if ( c >= 'a' && c <= 'z' ) {
			if ( ext ) lo_e = TRUE; else lo_b = TRUE;
			c = (char)( c - 'a' + 'A' );
		} else if ( c >= 'A' && c <= 'Z' ) {
			if ( ext ) up_e = TRUE; else up_b = TRUE;
		}
		n83[k++] = (UB)c;
	}
	if ( ( lo_b && up_b ) || ( lo_e && up_e ) ) {
		return FALSE;			/* mixed: a long name */
	}
	*p_case = (UB)( ( lo_b ? 0x08 : 0 ) | ( lo_e ? 0x10 : 0 ) );
	return TRUE;
}

LOCAL UB lfn_sum( CONST UB *n83 )
{
	UB	s = 0;
	INT	i;

	for ( i = 0; i < 11; i++ ) {
		s = (UB)( ( ( s & 1 ) << 7 ) + ( s >> 1 ) + n83[i] );
	}
	return s;
}

LOCAL UB *dir_slot( FATMK *m, INT d )
{
	FATDIR	*dr = &m->dir[d];

	if ( ( dr->nent + 1 ) * 32 > (INT)( m->spc * SECT ) ) {
		return NULL;
	}
	return dr->ent + 32 * dr->nent++;
}

/* The short entry itself */
LOCAL ER dir_raw( FATMK *m, INT d, CONST UB *n83, UB attr, UB cs, UW clus, UD size )
{
	UB	*e = dir_slot(m, d);

	if ( e == NULL ) return E_LIMIT;
	knl_memset(e, 0, 32);
	knl_memcpy(e, n83, 11);
	e[11] = attr;
	e[12] = cs;
	put16(e + 14, m->time);
	put16(e + 16, m->date);
	put16(e + 18, m->date);
	put16(e + 20, (UW)( clus >> 16 ));
	put16(e + 22, m->time);
	put16(e + 24, m->date);
	put16(e + 26, (UW)( clus & 0xFFFF ));
	put32(e + 28, (UW)size);
	return E_OK;
}

/* An entry put in a directory: its long name first when its name is not of 8.3 */
LOCAL ER dir_add( FATMK *m, INT d, CONST char *name, UB attr, UW clus, UD size )
{
	UB	n83[11], cs = 0, *e;
	INT	len = (INT)knl_strlen(name), nl, i, k;

	if ( !short_name(name, n83, &cs) ) {
		static CONST UB	off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
		UB	sum;

		/* the alias: six of the stem, ~1, three of the extension, upper case */
		knl_memset(n83, ' ', 11);
		for ( i = 0, k = 0; i < len && name[i] != '.' && k < 6; i++ ) {
			char	c = name[i];

			n83[k++] = (UB)( ( c >= 'a' && c <= 'z' ) ? c - 'a' + 'A' : c );
		}
		n83[k++] = '~';
		n83[k] = '1';
		for ( i = len - 1; i > 0 && name[i] != '.'; i-- ) ;
		for ( k = 8, i = ( i > 0 ) ? i + 1 : len; i < len && k < 11; i++ ) {
			char	c = name[i];

			n83[k++] = (UB)( ( c >= 'a' && c <= 'z' ) ? c - 'a' + 'A' : c );
		}
		sum = lfn_sum(n83);
		nl = ( len + 1 + 12 ) / 13;		/* with the NUL, when it does not fill the last */
		if ( len % 13 == 0 ) nl = len / 13;
		for ( i = nl; i >= 1; i-- ) {
			e = dir_slot(m, d);
			if ( e == NULL ) return E_LIMIT;
			knl_memset(e, 0, 32);
			e[0] = (UB)( i | ( ( i == nl ) ? 0x40 : 0 ) );
			e[11] = 0x0F;
			e[13] = sum;
			for ( k = 0; k < 13; k++ ) {
				INT	at = ( i - 1 ) * 13 + k;
				UW	u = ( at < len ) ? (UB)name[at] : ( at == len ) ? 0x0000 : 0xFFFF;

				put16(e + off[k], u);
			}
		}
	}
	return dir_raw(m, d, n83, attr, cs, clus, size);
}

/* The directory of a path, made with its parents when it is not there */
LOCAL INT dir_get( FATMK *m, CONST char *path, INT plen )
{
	INT	i, cut, d;
	UW	c;
	ER	er;

	for ( i = 0; i < m->ndir; i++ ) {
		if ( (INT)knl_strlen(m->dir[i].path) == plen && same_n((CONST UB *)m->dir[i].path, (CONST UB *)path, plen) ) {
			return i;
		}
	}
	if ( m->ndir >= DIR_MAX ) {
		return E_LIMIT;
	}
	for ( cut = plen; cut > 0 && path[cut - 1] != '/'; cut-- ) ;
	d = dir_get(m, path, ( cut > 0 ) ? cut - 1 : 0);
	if ( d < 0 ) {
		return d;
	}
	c = take(m, 1);
	if ( c == 0 ) {
		return E_LIMIT;
	}
	i = m->ndir;
	m->dir[i].ent = (UB *)Kcalloc(m->spc, SECT);
	if ( m->dir[i].ent == NULL ) {
		return E_NOMEM;
	}
	m->ndir++;
	{
		/* its name kept for looking it up again: the part of the path it is */
		char	*p = (char *)Kmalloc((SZ)plen + 1);

		if ( p == NULL ) return E_NOMEM;
		knl_memcpy(p, path, plen);
		p[plen] = 0;
		m->dir[i].path = p;
	}
	m->dir[i].clus = c;
	m->dir[i].parent = d;
	m->dir[i].nent = 0;
	{
		char	nm[16];
		INT	k, n = 0;

		for ( k = cut; k < plen && n < 15; k++ ) nm[n++] = path[k];
		nm[n] = 0;
		/* "." and "..", the parent's cluster 0 for the root */
		er = dir_raw(m, i, (CONST UB *)".          ", 0x10, 0, c, 0);
		if ( er >= E_OK ) er = dir_raw(m, i, (CONST UB *)"..         ", 0x10, 0, ( d == 0 ) ? 0 : m->dir[d].clus, 0);
		if ( er >= E_OK ) er = dir_add(m, d, nm, 0x10, c, 0);
	}
	return ( er < E_OK ) ? er : i;
}

EXPORT ER si_fat_make( ID dd, UD first, UD last, BOOL fallback, CONST char *note )
{
	SIFILE	f[SI_MAXFILE];
	FATMK	m;
	TS_TIME	now = 0;
	TS_TM	tm;
	UB	*bs, *io = NULL;
	void	*raw = NULL;
	UD	nsect = last - first + 1, s, k;
	UW	free_c, cl;
	INT	nf, i, j;
	ER	er = E_OK;

	knl_memset(&m, 0, sizeof(m));
	nf = esp_files(f, fallback, note);
	if ( nf < 0 ) {
		return nf;
	}
	m.dd = dd;
	m.first = first;
	/* a cluster as large as leaves well over the 65525 FAT32 needs, at most 4KB */
	for ( m.spc = 8; m.spc > 1 && nsect / m.spc < 70000; m.spc >>= 1 ) ;
	for ( m.fatsz = 1;; ) {
		UD	cl2 = ( nsect - FAT_RSVD - 2 * m.fatsz ) / m.spc;
		UD	need = ( ( cl2 + 2 ) * 4 + SECT - 1 ) / SECT;

		if ( need <= m.fatsz ) break;
		m.fatsz = need;
	}
	m.nclus = (UW)( ( nsect - FAT_RSVD - 2 * m.fatsz ) / m.spc );
	if ( m.nclus < 65525 ) {
		return E_PAR;			/* too small for FAT32 */
	}
	m.data = first + FAT_RSVD + 2 * m.fatsz;
	m.fat = (UW *)Kcalloc((SZ)m.fatsz, SECT);
	bs = (UB *)Kcalloc(FAT_RSVD, SECT);
	io = io_buf(256 * 1024, &raw);
	if ( m.fat == NULL || bs == NULL || io == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	if ( dt_gettime(&now) >= E_OK && dt_localtime(&now, &tm) >= E_OK && tm.tm_year >= 80 ) {
		m.date = (UW)( ( ( tm.tm_year - 80 ) << 9 ) | ( ( tm.tm_mon + 1 ) << 5 ) | tm.tm_mday );
		m.time = (UW)( ( tm.tm_hour << 11 ) | ( tm.tm_min << 5 ) | ( tm.tm_sec / 2 ) );
	} else {
		m.date = (UW)( ( ( 2026 - 1980 ) << 9 ) | ( 1 << 5 ) | 1 );
	}
	m.fat[0] = 0x0FFFFFF8U;
	m.fat[1] = FAT_EOC;
	m.next = 2;

	/* the root at cluster 2, its label first */
	m.dir[0].path = "";
	m.dir[0].clus = take(&m, 1);
	m.dir[0].parent = -1;
	m.dir[0].ent = (UB *)Kcalloc(m.spc, SECT);
	m.ndir = 1;
	if ( m.dir[0].ent == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	knl_memcpy(m.dir[0].ent, "TESSRONOS  ", 11);
	m.dir[0].ent[11] = 0x08;
	put16(m.dir[0].ent + 22, m.time);
	put16(m.dir[0].ent + 24, m.date);
	m.dir[0].nent = 1;

	/* each file: its directory, its clusters, its entry */
	{
		UW	fc[SI_MAXFILE];

		for ( i = 0; i < nf && er >= E_OK; i++ ) {
			INT	plen = (INT)knl_strlen(f[i].dst), cut, d;
			UW	n = (UW)( ( f[i].size + m.spc * SECT - 1 ) / ( m.spc * SECT ) );

			for ( cut = plen; cut > 0 && f[i].dst[cut - 1] != '/'; cut-- ) ;
			d = dir_get(&m, f[i].dst, ( cut > 0 ) ? cut - 1 : 0);
			if ( d < 0 ) {
				er = d;
				break;
			}
			fc[i] = ( n > 0 ) ? take(&m, n) : 0;
			if ( n > 0 && fc[i] == 0 ) {
				er = E_LIMIT;
				break;
			}
			er = dir_add(&m, d, f[i].dst + cut, 0x20, fc[i], f[i].size);
		}

		/* the data of the files, a cluster's multiple at a time, the tail with zeros */
		for ( i = 0; i < nf && er >= E_OK; i++ ) {
			SIREAD	r;
			UD	at = clus_sect(&m, fc[i]), left = f[i].size;
			INT	got;

			if ( f[i].size == 0 ) continue;
			er = fread_open(&r, &f[i]);
			while ( er >= E_OK && left > 0 ) {
				INT	want = ( left < 256 * 1024 ) ? (INT)left : 256 * 1024;
				UD	ns;

				got = fread_some(&r, io, want);
				if ( got != want ) {
					er = E_IO;
					break;
				}
				ns = ( (UD)got + m.spc * SECT - 1 ) / ( m.spc * SECT ) * m.spc;
				knl_memset(io + got, 0, (SZ)( ns * SECT - (UD)got ));
				er = wr(dd, at, ns, io);
				at += ns;
				left -= (UD)got;
			}
			fread_close(&r);
		}
	}

	/* the directories */
	for ( j = 0; j < m.ndir && er >= E_OK; j++ ) {
		er = wr(dd, clus_sect(&m, m.dir[j].clus), m.spc, m.dir[j].ent);
	}

	/* the two tables */
	for ( free_c = 0, cl = 2; cl < m.nclus + 2; cl++ ) {
		if ( m.fat[cl] == 0 ) free_c++;
	}
	for ( j = 0; j < 2 && er >= E_OK; j++ ) {
		for ( s = 0; s < m.fatsz && er >= E_OK; s += k ) {
			k = m.fatsz - s;
			if ( k > 512 ) k = 512;
			er = wr(dd, first + FAT_RSVD + j * m.fatsz + s, k, (UB *)m.fat + s * SECT);
		}
	}

	/* the boot sector, FSInfo and their copies at 6 and 7, last */
	if ( er >= E_OK ) {
		UB	*b = bs, *fi = bs + SECT;
		TS_UUID	u;

		b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
		knl_memcpy(b + 3, "TESSRON ", 8);
		put16(b + 11, SECT);
		b[13] = (UB)m.spc;
		put16(b + 14, FAT_RSVD);
		b[16] = 2;
		b[21] = 0xF8;
		put16(b + 24, 63);
		put16(b + 26, 255);
		put32(b + 28, (UW)first);
		put32(b + 32, (UW)nsect);
		put32(b + 36, (UW)m.fatsz);
		put32(b + 44, 2);
		put16(b + 48, 1);
		put16(b + 50, 6);
		b[64] = 0x80;
		b[66] = 0x29;
		(void)ts_gen_uuid(&u);
		knl_memcpy(b + 67, u.b, 4);
		knl_memcpy(b + 71, "TESSRONOS  ", 11);
		knl_memcpy(b + 82, "FAT32   ", 8);
		b[510] = 0x55; b[511] = 0xAA;
		put32(fi, 0x41615252U);
		put32(fi + 484, 0x61417272U);
		put32(fi + 488, free_c);
		put32(fi + 492, m.next);
		put32(fi + 508, 0xAA550000U);
		knl_memcpy(bs + 6 * SECT, b, SECT);
		knl_memcpy(bs + 7 * SECT, fi, SECT);
		er = wr(dd, first, FAT_RSVD, bs);
	}
	dev_flush(dd);
	tm_printf((UB *)"sysinst: FAT32 at %ld, %ld sectors, %d per cluster, %d clusters, %d free, %d files (%d)\n",
		  (D)first, (D)nsect, (INT)m.spc, (INT)m.nclus, (INT)free_c, nf, (INT)er);
out:
	for ( j = 0; j < m.ndir; j++ ) {
		if ( m.dir[j].ent != NULL ) Kfree(m.dir[j].ent);
		if ( j > 0 && m.dir[j].path != NULL ) Kfree((void *)m.dir[j].path);
	}
	if ( m.fat != NULL ) Kfree(m.fat);
	if ( bs != NULL ) Kfree(bs);
	if ( raw != NULL ) Kfree(raw);
	return er;
}

/* ---------------------------------------------------------------- the volume copied */

/* The runs of the copy: where each is and the CRC of what was written, for reading it back */
typedef struct {
	UD	blk;
	UW	n;
	UW	crc;
} SIRUN;

typedef struct {
	ID	vol, sdd, dd;
	UD	first;			/* the target's first sector */
	UD	pend, npend;		/* the run being gathered */
	UW	chunk;			/* blocks of one request */
	BOOL	count_only;
	UD	total, done;
	UB	*buf;
	SIRUN	*run;
	INT	nrun, room;
	SI_PROG	cb;
	void	*ctx;
	ER	er;
} SICOPY;

LOCAL void run_out( SICOPY *c )
{
	SZ	asz;
	UD	n = c->npend;

	c->npend = 0;
	if ( n == 0 || c->er < E_OK ) {
		return;
	}
	if ( c->count_only ) {
		c->total += n;
		return;
	}
	if ( c->nrun >= c->room ) {
		INT	room = ( c->room == 0 ) ? 1024 : c->room * 2;
		SIRUN	*r = (SIRUN *)Kmalloc(sizeof(SIRUN) * (SZ)room);

		if ( r == NULL ) {
			c->er = E_NOMEM;
			return;
		}
		if ( c->run != NULL ) {
			knl_memcpy(r, c->run, sizeof(SIRUN) * (SZ)c->nrun);
			Kfree(c->run);
		}
		c->run = r;
		c->room = room;
	}
	c->er = tk_srea_dev_d(c->sdd, (D)( c->pend * BLK_SECT ), c->buf, (SZ)( n * TSFSBLK_BLOCK_SIZE ), &asz);
	if ( c->er >= E_OK ) {
		c->er = tk_swri_dev_d(c->dd, (D)( c->first + c->pend * BLK_SECT ), c->buf,
				      (SZ)( n * TSFSBLK_BLOCK_SIZE ), &asz);
	}
	if ( c->er < E_OK ) {
		tm_printf((UB *)"sysinst: the copy of blocks %ld to %ld failed (%d)\n", (D)c->pend,
			  (D)( c->pend + n - 1 ), (INT)c->er);
		return;
	}
	c->run[c->nrun].blk = c->pend;
	c->run[c->nrun].n = (UW)n;
	c->run[c->nrun].crc = si_crc32(0, c->buf, n * TSFSBLK_BLOCK_SIZE);
	c->nrun++;
	c->done += n;
	if ( c->cb != NULL ) c->cb(c->ctx, SI_ST_COPY, c->done * TSFSBLK_BLOCK_SIZE, c->total * TSFSBLK_BLOCK_SIZE);
}

/* One block more to copy: joined to the run when it follows it */
LOCAL void take_blk( SICOPY *c, UD blk, UD n )
{
	while ( n > 0 && c->er >= E_OK ) {
		if ( c->npend > 0 && ( c->pend + c->npend != blk || c->npend >= c->chunk ) ) {
			run_out(c);
		}
		if ( c->npend == 0 ) c->pend = blk;
		c->npend++;
		blk++;
		n--;
	}
}

/* Every block of the volume in use, in order */
LOCAL void walk( SICOPY *c, CONST T_TSFSBLK_SB *sb, UB *map )
{
	T_TSFSAGREF	a;
	UD		n, off, lo, hi, first0 = 0;
	UW		k;

	if ( ts_blk_group(c->vol, 0, &a) >= E_OK ) first0 = a.first;
	take_blk(c, 0, first0);			/* the superblocks and the journal */
	for ( n = 0; n < sb->ag_count && c->er >= E_OK; n++ ) {
		if ( ts_blk_group(c->vol, n, &a) < E_OK ) {
			c->er = E_OBJ;
			break;
		}
		take_blk(c, a.first, a.meta);
		for ( k = 0; k < a.map_blocks && c->er >= E_OK; k++ ) {
			BOOL	all = (BOOL)( ts_blk_map(c->vol, n, k, map) < E_OK );	/* a map that does not hold: all of it */

			lo = (UD)k * TSFSBLK_BLOCK_SIZE * 8;
			hi = lo + TSFSBLK_BLOCK_SIZE * 8;
			if ( lo < a.meta ) lo = a.meta;
			if ( hi > a.len ) hi = a.len;
			for ( off = lo; off < hi && c->er >= E_OK; off++ ) {
				UD	bit = off % ( TSFSBLK_BLOCK_SIZE * 8 );

				if ( all || ( map[bit / 8] & ( 1 << ( bit % 8 ) ) ) != 0 ) {
					take_blk(c, a.first + off, 1);
				}
			}
		}
	}
	run_out(c);
}

EXPORT ER si_vol_copy( CONST char *src, ID dd, UD first, UD nsect, SI_PROG cb, void *ctx, UD *p_copied )
{
	T_TSFSBLK_SB	sb;
	SICOPY		c;
	UB		*map = NULL;
	void		*raw = NULL;
	UW		m1, m2;
	INT		i, t;
	ER		er, fr = E_BUSY;

	knl_memset(&c, 0, sizeof(c));
	if ( p_copied != NULL ) *p_copied = 0;
	c.vol = ts_vol_blk_of(src);
	if ( c.vol <= 0 || ts_ref_vol_blk(c.vol, &sb) < E_OK ) {
		return E_NOEXS;
	}
	if ( nsect < sb.total_blocks * BLK_SECT ) {
		return E_LIMIT;
	}
	c.sdd = tk_opn_dev((CONST UB *)src, TD_READ);
	if ( c.sdd < E_OK ) {
		return c.sdd;
	}
	c.dd = dd;
	c.first = first;
	c.cb = cb;
	c.ctx = ctx;
	m1 = io_max(c.sdd);
	m2 = io_max(dd);
	c.chunk = RUN_MAX;
	if ( m1 != 0 && m1 / TSFSBLK_BLOCK_SIZE < c.chunk ) c.chunk = m1 / TSFSBLK_BLOCK_SIZE;
	if ( m2 != 0 && m2 / TSFSBLK_BLOCK_SIZE < c.chunk ) c.chunk = m2 / TSFSBLK_BLOCK_SIZE;
	if ( c.chunk < 1 ) c.chunk = 1;
	c.buf = io_buf((SZ)c.chunk * TSFSBLK_BLOCK_SIZE, &raw);
	map = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( c.buf == NULL || map == NULL ) {
		er = E_NOMEM;
		goto out;
	}

	/* no transaction begins, and every one before is on the medium in its place */
	for ( t = 0; t < FREEZE_TRIES && ( fr = knl_jrnl_freeze(c.vol, TRUE) ) == E_BUSY; t++ ) {
		tk_dly_tsk(500);
	}
	if ( fr < E_OK ) {
		tm_printf((UB *)"sysinst: %s could not be frozen (%d)\n", src, (INT)fr);
		er = fr;
		goto out;
	}
	er = ts_sync_blk(c.vol);
	if ( er >= E_OK ) er = ts_ref_vol_blk(c.vol, &sb);
	if ( er >= E_OK ) {
		c.count_only = TRUE;
		walk(&c, &sb, map);
		c.count_only = FALSE;
		er = c.er;
	}
	tm_printf((UB *)"sysinst: %s frozen: %ld of %ld blocks to copy, %d at a time\n", src, (D)c.total,
		  (D)sb.total_blocks, (INT)c.chunk);
	if ( er >= E_OK ) {
		walk(&c, &sb, map);
		er = c.er;
	}
	dev_flush(dd);
	(void)knl_jrnl_freeze(c.vol, FALSE);
	tm_printf((UB *)"sysinst: %s thawed: %ld blocks in %d runs copied (%d)\n", src, (D)c.done, c.nrun, (INT)er);

	/* read back */
	for ( i = 0; i < c.nrun && er >= E_OK; i++ ) {
		SZ	asz;

		er = tk_srea_dev_d(dd, (D)( first + c.run[i].blk * BLK_SECT ), c.buf,
				   (SZ)c.run[i].n * TSFSBLK_BLOCK_SIZE, &asz);
		if ( er >= E_OK && si_crc32(0, c.buf, (UD)c.run[i].n * TSFSBLK_BLOCK_SIZE) != c.run[i].crc ) {
			tm_printf((UB *)"sysinst: blocks %ld to %ld do not read back as written\n", (D)c.run[i].blk,
				  (D)( c.run[i].blk + c.run[i].n - 1 ));
			er = E_IO;
		}
		if ( cb != NULL && ( i % 16 == 15 || i == c.nrun - 1 ) ) cb(ctx, SI_ST_VERIFY, (UD)i + 1, (UD)c.nrun);
	}
	if ( p_copied != NULL ) *p_copied = c.done;
out:
	if ( c.run != NULL ) Kfree(c.run);
	if ( map != NULL ) Kfree(map);
	if ( raw != NULL ) Kfree(raw);
	tk_cls_dev(c.sdd, 0);
	return er;
}

EXPORT ER si_disk_crc( CONST char *dev, UD first, UD nsect, UW *p_crc )
{
	UB	*buf;
	void	*raw;
	UW	crc = 0;
	UD	at, n;
	ID	dd;
	ER	er = E_OK;

	dd = tk_opn_dev((CONST UB *)dev, TD_READ);
	if ( dd < E_OK ) {
		return dd;
	}
	buf = io_buf(256 * SECT, &raw);
	if ( buf == NULL ) {
		tk_cls_dev(dd, 0);
		return E_NOMEM;
	}
	for ( at = 0; at < nsect && er >= E_OK; at += n ) {
		n = nsect - at;
		if ( n > 256 ) n = 256;
		er = rd(dd, first + at, n, buf);
		crc = si_crc32(crc, buf, n * SECT);
	}
	*p_crc = crc;
	Kfree(raw);
	tk_cls_dev(dd, 0);
	return er;
}

/* ---------------------------------------------------------------- the whole */

EXPORT CONST char *si_mode_name( INT mode )
{
	return ( mode == SI_ERASE ) ? "erase" : ( mode == SI_REPLACE ) ? "replace" : "coexist";
}

LOCAL INT put_hex( char *s, INT n, CONST UB *b, INT len )
{
	static CONST char	hx[] = "0123456789abcdef";
	INT			i;

	for ( i = 0; i < len; i++ ) {
		s[n++] = hx[b[i] >> 4];
		s[n++] = hx[b[i] & 15];
	}
	s[n] = 0;
	return n;
}

/* The note of the install, INSTALL.TXT of the ESP: what was put where, and when */
LOCAL void make_note( CONST T_SIPLAN *pl, CONST UB *part, char *s, INT max )
{
	TS_TIME	now = 0;
	TS_TM	tm;

	knl_memset(&tm, 0, sizeof(tm));
	if ( dt_gettime(&now) >= E_OK ) (void)dt_localtime(&now, &tm);
	tm_sprintf((UB *)s, (UB *)"TessronOS installed by システム登録\r\nmode=%s\r\nfrom=%s\r\nto=%s\r\n"
		   "date=%04d-%02d-%02d %02d:%02d:%02d\r\n", si_mode_name(pl->mode),
		   pl->src, ( part[0] != 0 ) ? (CONST char *)part : (CONST char *)pl->disk,
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	(void)max;
}

EXPORT BOOL si_entry_first( CONST T_SIPLAN *pl )
{
	return (BOOL)( pl->mode == SI_ERASE || pl->nwin == 0 );
}

/*
 * The request the loader takes at the next start from this ESP: the
 * partition the new entry names (its entry's number from 1, where it
 * starts and how long it is, its unique GUID as the table holds it)
 * and the file of the loader there
 */
LOCAL ER ask_entry( CONST T_SIPLAN *pl )
{
	char	s[400];
	INT	n, fd;
	ER	er;

	n = (INT)tm_sprintf((UB *)s, (UB *)"TessronOS boot entry request\r\npart=%d\r\nstart=%ld\r\nsize=%ld\r\nuniq=",
			    pl->esp_entry + 1, (D)pl->esp_first, (D)( pl->esp_last - pl->esp_first + 1 ));
	n = put_hex(s, n, pl->esp_uniq, 16);
	n += (INT)tm_sprintf((UB *)s + n, (UB *)"\r\npath=\\EFI\\TESSRON\\BOOTX64.EFI\r\nfirst=%d\r\n",
			     si_entry_first(pl) ? 1 : 0);
	(void)fs_mkdir("/boot/EFI");
	(void)fs_mkdir(BOOT_DIR);
	fd = fs_open(REQ_PATH, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return fd;
	}
	er = ( fs_write(fd, s, n) == n ) ? E_OK : E_IO;
	if ( fs_fsync(fd) < EX_OK && er >= E_OK ) er = E_IO;
	fs_close(fd);
	(void)fs_sync();
	return er;
}

/* The plan looked at again just before: the disk as it was when it was made */
LOCAL BOOL still( CONST T_SIPLAN *pl )
{
	T_SIPLAN	*now = (T_SIPLAN *)Kmalloc(sizeof(T_SIPLAN));
	BOOL		same;

	if ( now == NULL ) {
		return FALSE;
	}
	same = (BOOL)( si_plan((CONST char *)pl->disk, pl->mode, pl->ts_unit, now) >= E_OK && now->why == SI_R_OK
		       && now->nsect == pl->nsect && now->ts_first == pl->ts_first && now->ts_last == pl->ts_last
		       && now->esp_first == pl->esp_first && now->ts_unit == pl->ts_unit
		       && now->src_blocks <= pl->src_blocks );
	Kfree(now);
	return same;
}

EXPORT ER si_run( T_SIPLAN *pl, SI_PROG cb, void *ctx, T_SIRES *res )
{
	UB	part[L_DEVNM + 1], esp[L_DEVNM + 1];
	char	note[400];
	SIFILE	f[SI_MAXFILE];
	ID	dd = 0;
	UD	from = 0, to = 0;
	INT	nf;
	ER	er;

	knl_memset(res, 0, sizeof(*res));
	res->failed_step = SI_ST_CHECK;
	part[0] = 0;
	if ( cb != NULL ) cb(ctx, SI_ST_CHECK, 0, 1);
	if ( pl->why != SI_R_OK || !still(pl) ) {
		tm_printf((UB *)"sysinst: %s is not as it was when the plan was made\n", pl->disk);
		return E_OBJ;
	}
	tm_printf((UB *)"sysinst: %s: %s, %s (%ld blocks, %ld taken) to sectors %ld to %ld\n", pl->disk,
		  si_mode_name(pl->mode), pl->src, (D)pl->src_blocks, (D)pl->src_used,
		  (D)pl->ts_first, (D)pl->ts_last);

	if ( pl->mode == SI_COEXIST || pl->mode == SI_REPLACE ) {
		/* the partition: added after those in use, the one there is, or the one chosen made another */
		res->failed_step = SI_ST_TABLE;
		if ( cb != NULL ) cb(ctx, SI_ST_TABLE, 0, 1);
		if ( pl->mode == SI_REPLACE ) {
			name_of(part, pl->disk, pl->ts_unit);
			if ( pl->lost[0].kind != SI_K_TSFS ) {
				er = knl_gpt_retype((CONST char *)part, knl_tsfs_type_guid, "TessronOS", NULL);
				if ( er < E_OK ) goto fail;
			}
			if ( pl->ts_last > pl->lost[0].last ) {
				er = knl_gpt_grow((CONST char *)part, pl->ts_last);
				if ( er < E_OK ) goto fail;
			}
		} else if ( pl->ts_unit < 0 ) {
			er = knl_gpt_add((CONST char *)pl->disk, pl->ts_first, pl->ts_last, knl_tsfs_type_guid,
					 "TessronOS", part, sizeof(part));
			if ( er < E_OK ) goto fail;
		} else {
			name_of(part, pl->disk, pl->ts_unit);
		}
		(void)copy_str(res->part, sizeof(res->part), (CONST char *)part);

		/* the volume, through the partition's own device */
		res->failed_step = SI_ST_COPY;
		dd = tk_opn_dev(part, TD_UPDATE);
		if ( dd < E_OK ) {
			er = dd;
			goto fail;
		}
		er = si_vol_copy((CONST char *)pl->src, dd, 0, pl->ts_last - pl->ts_first + 1, cb, ctx, &res->copied);
		tk_cls_dev(dd, 0);
		dd = 0;
		if ( er < E_OK ) goto fail;

		/* as large as the partition */
		res->failed_step = SI_ST_GROW;
		if ( cb != NULL ) cb(ctx, SI_ST_GROW, 0, 1);
		er = knl_tsfs_part_grow((CONST char *)part, ( pl->ts_last - pl->ts_first + 1 ) * SECT, &from, &to);
		if ( er == E_LIMIT ) er = E_OK;		/* as large already */
		if ( er < E_OK ) goto fail;
		res->vol_from = from;
		res->vol_to = to;

		/* the files, below \EFI\TESSRON\ of the ESP there is */
		res->failed_step = SI_ST_ESP;
		make_note(pl, part, note, sizeof(note));
		nf = esp_files(f, FALSE, note);
		if ( nf < 0 ) {
			er = nf;
			goto fail;
		}
		name_of(esp, pl->disk, pl->esp_unit);
		er = esp_put_fs(esp, f, nf, cb, ctx);
		if ( er < E_OK ) goto fail;
	} else {
		T_SINEWPART	np[2];
		SZ		asz;

		/* the whole disk, alone: nothing of it is in use, nor is opened meanwhile */
		res->failed_step = SI_ST_TABLE;
		if ( cb != NULL ) cb(ctx, SI_ST_TABLE, 0, 1);
		dd = tk_opn_dev(pl->disk, TD_UPDATE | TD_EXCL);
		if ( dd < E_OK ) {
			er = ( dd == E_BUSY ) ? E_BUSY : dd;
			goto fail;
		}
		knl_memset(np, 0, sizeof(np));
		np[0].first = pl->esp_first;
		np[0].last = pl->esp_last;
		np[0].type = g_esp;
		knl_memcpy(np[0].uniq, pl->esp_uniq, 16);
		np[0].name = "EFI system partition";
		np[1].first = pl->ts_first;
		np[1].last = pl->ts_last;
		np[1].type = knl_tsfs_type_guid;
		knl_memcpy(np[1].uniq, pl->ts_uniq, 16);
		np[1].name = "TessronOS";
		er = si_gpt_write(dd, pl->nsect, pl->disk_guid, np, 2);
		if ( er < E_OK ) goto fail;

		res->failed_step = SI_ST_COPY;
		er = si_vol_copy((CONST char *)pl->src, dd, pl->ts_first, pl->ts_last - pl->ts_first + 1, cb, ctx,
				 &res->copied);
		if ( er < E_OK ) goto fail;

		res->failed_step = SI_ST_ESP;
		if ( cb != NULL ) cb(ctx, SI_ST_ESP, 0, 1);
		name_of(part, pl->disk, 1);
		make_note(pl, part, note, sizeof(note));
		er = si_fat_make(dd, pl->esp_first, pl->esp_last, TRUE, note);
		tk_cls_dev(dd, 0);
		dd = 0;
		if ( er < E_OK ) goto fail;

		/* the driver reads the table again: it can, when the disk had none */
		res->failed_step = SI_ST_GROW;
		if ( cb != NULL ) cb(ctx, SI_ST_GROW, 0, 1);
		res->grow_later = TRUE;
		dd = tk_opn_dev(pl->disk, TD_UPDATE);
		if ( dd >= E_OK ) {
			er = tk_swri_dev(dd, TDN_PARTSCAN, NULL, 0, &asz);
			tk_cls_dev(dd, 0);
			dd = 0;
			if ( er >= E_OK ) {
				knl_obdev_changed();
				(void)copy_str(res->part, sizeof(res->part), (CONST char *)part);
				er = knl_tsfs_part_grow((CONST char *)part, ( pl->ts_last - pl->ts_first + 1 ) * SECT,
							&from, &to);
				if ( er >= E_OK || er == E_LIMIT ) {
					res->grow_later = FALSE;
					res->vol_from = from;
					res->vol_to = to;
				}
			}
			tm_printf((UB *)"sysinst: %s: the partitions read again: %s\n", pl->disk,
				  res->grow_later ? "not until the next start" : "done");
		}
	}

	res->failed_step = SI_ST_ENTRY;
	if ( cb != NULL ) cb(ctx, SI_ST_ENTRY, 0, 1);
	er = ask_entry(pl);
	res->entry_asked = (BOOL)( er >= E_OK );
	if ( er < E_OK ) {
		tm_printf((UB *)"sysinst: the boot entry could not be asked for (%d)\n", (INT)er);
		er = E_OK;			/* the system is there: chosen from the firmware's menu */
	}
	res->failed_step = -1;
	if ( cb != NULL ) cb(ctx, SI_ST_DONE, 1, 1);
	tm_printf((UB *)"sysinst: done: %s on %s, %ld blocks copied, volume %ld -> %ld bytes%s, entry %s\n",
		  pl->src, ( res->part[0] != 0 ) ? (CONST char *)res->part : (CONST char *)pl->disk,
		  (D)res->copied, (D)res->vol_from, (D)res->vol_to, res->grow_later ? " (grown at the next start)" : "",
		  res->entry_asked ? "asked" : "not asked");
	return E_OK;

fail:
	if ( dd > 0 ) tk_cls_dev(dd, 0);
	tm_printf((UB *)"sysinst: failed at step %d (%d)\n", res->failed_step, (INT)er);
	return er;
}

#endif /* X64 */
