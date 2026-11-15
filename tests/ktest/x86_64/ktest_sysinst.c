/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_sysinst.c
 *	システム登録 (application/sysinst/si_core.c): the system put on an
 *	internal disk, beside Windows and over the whole disk
 *	(docs/tessronos/03-platform.md 3.10.10)
 *
 *	The machine is started from the test image (nvmea) with NVMe disks
 *	made by tools/tests/mkwindisk.py beside it (tools/x64/ktest64.sh
 *	NVME2=, NVME3=): each holds a GPT as Windows leaves one -- an EFI
 *	system partition with Windows' files, MSR, a basic data partition,
 *	a stretch nothing takes and the recovery partition at the end --
 *	told apart by the name of the basic data partition, KTWIN-C to be
 *	put beside, KTWIN-E to be erased, KTWIN-R to be made TessronOS's
 *	(NVME4=). Then:
 *
 *	  - the disk started from is never offered, nor the plan made on
 *	    it; a way that cannot be taken says why;
 *	  - over the whole of KTWIN-E: every partition there was is in the
 *	    plan as one that goes; afterwards the table has the ESP and the
 *	    TessronOS partition, both copies pass, the ESP is a FAT32 volume
 *	    and the volume's superblock is the system volume's;
 *	  - beside Windows on KTWIN-C: a TessronOS partition in the free
 *	    stretch, the entries there were the same, the bytes of MSR, the
 *	    basic data and the recovery partitions the same (CRC before and
 *	    after), Windows' files on the ESP the same, \EFI\TESSRON\ with
 *	    the loader and the kernel as /boot has them; the copy as large
 *	    as its partition, of the system volume's UUID, and checked
 *	    whole by the disk repair; the request for the boot entry on
 *	    /boot;
 *	  - KTWIN-R's basic data partition made TessronOS's: not the ESP,
 *	    MSR nor the recovery partition (si_replaceable); afterwards the
 *	    same four entries, the third where it was, of the TSFS type and
 *	    grown over the free stretch after it, the others' entries and
 *	    bytes the same, Windows' files on the ESP the same, the copy
 *	    checked whole.
 *
 *	  - the system put over the TessronOS left on KTWIN-C, keeping it
 *	    (SI_KEEP, 上書き): the person's object and link stay, the system's
 *	    own record made stale and the list of the system's objects are
 *	    the running system's again, the kernel on the ESP is /boot's, the
 *	    volume is checked whole; a newer system there is not put over,
 *	    and a disk with no TessronOS (or one erased whole) offers no
 *	    keeping.
 *
 *	A machine started from a disk the system was put on (its ESP has
 *	\EFI\TESSRON\INSTALL.TXT) only looks at itself: /boot and the system
 *	volume on that disk, the volume grown to its partition.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/fs.h>
#include <ts/blk.h>
#include <ts/gpt.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsck.h>
#include <ts/tsfs.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include "tstdlib.h"
#include "../ktest.h"
#include "../../../application/sysinst/si_core.h"

IMPORT unsigned char	knl_sysvol_name[];
IMPORT unsigned char	knl_bootvol_name[];

#define INSTALLED	"/boot/EFI/TESSRON/INSTALL.TXT"
#define REQ		"/boot/EFI/TESSRON/BOOTREG.TXT"
#define NFILE_WIN	3

LOCAL CONST char * CONST win_files[NFILE_WIN] = {
	"EFI/Microsoft/Boot/bootmgfw.efi", "EFI/Boot/bootx64.efi", "EFI/Microsoft/Boot/BCD"
};

LOCAL T_SIDISK	dk;
LOCAL T_SIPLAN	pl;
LOCAL UB	dc[L_DEVNM + 1], de[L_DEVNM + 1], dr[L_DEVNM + 1];	/* the disks of the three kinds */

LOCAL BOOL same_b( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*p = (CONST UB *)a, *q = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( p[i] != q[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL void part_name( UB *out, CONST UB *disk, INT unit )
{
	INT	n = (INT)knl_strlen((CONST char *)disk);

	knl_memcpy(out, disk, n);
	if ( unit >= 10 ) out[n++] = (UB)( '0' + unit / 10 );
	out[n++] = (UB)( '0' + unit % 10 );
	out[n] = 0;
}

/* The CRC and size of a file, of a path of the tree (vd < 0) or below a volume's root */
LOCAL ER file_crc( INT vd, CONST char *path, UW *p_crc, UD *p_size )
{
	UB	*buf;
	INT	fd, n;
	UW	crc = 0;
	UD	size = 0;

	fd = ( vd >= 0 ) ? fs_openat(vd, path, O_RDONLY) : fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		return fd;
	}
	buf = (UB *)Kmalloc(64 * 1024);
	if ( buf == NULL ) {
		fs_close(fd);
		return E_NOMEM;
	}
	while ( ( n = fs_read(fd, buf, 64 * 1024) ) > 0 ) {
		crc = si_crc32(crc, buf, (UD)n);
		size += (UD)n;
	}
	Kfree(buf);
	fs_close(fd);
	*p_crc = crc;
	*p_size = size;
	return ( n < 0 ) ? n : E_OK;
}

/* Both copies of a disk's table pass their CRCs */
LOCAL BOOL both_tables( CONST UB *disk, UD nsect )
{
	UB	*s;
	UB	h[92];
	ID	dd;
	SZ	asz;
	INT	k;
	BOOL	ok = TRUE;

	dd = tk_opn_dev(disk, TD_READ);
	if ( dd < E_OK ) {
		return FALSE;
	}
	s = (UB *)Kmalloc(512);
	for ( k = 0; k < 2 && ok && s != NULL; k++ ) {
		UW	crc;

		ok = (BOOL)( tk_srea_dev_d(dd, ( k == 0 ) ? 1 : (D)( nsect - 1 ), s, 512, &asz) >= E_OK
			     && same_b(s, "EFI PART", 8) );
		if ( !ok ) break;
		knl_memcpy(h, s, 92);
		crc = (UW)h[16] | ( (UW)h[17] << 8 ) | ( (UW)h[18] << 16 ) | ( (UW)h[19] << 24 );
		h[16] = h[17] = h[18] = h[19] = 0;
		ok = (BOOL)( si_crc32(0, h, 92) == crc );
	}
	if ( s != NULL ) Kfree(s);
	tk_cls_dev(dd, 0);
	return ok;
}

/* The disks mkwindisk.py made, by the name of their basic data partition */
LOCAL void find_disks( void )
{
	static CONST char * CONST names[] = { "nvmeb", "nvmec", "nvmed", NULL };
	INT	i, k;

	dc[0] = de[0] = dr[0] = 0;
	for ( i = 0; names[i] != NULL; i++ ) {
		if ( si_disk_read(names[i], &dk) < E_OK ) continue;
		for ( k = 0; k < dk.nparts; k++ ) {
			if ( knl_strcmp((CONST char *)dk.parts[k].name, "KTWIN-C") == 0 ) knl_strcpy((char *)dc, names[i]);
			if ( knl_strcmp((CONST char *)dk.parts[k].name, "KTWIN-E") == 0 ) knl_strcpy((char *)de, names[i]);
			if ( knl_strcmp((CONST char *)dk.parts[k].name, "KTWIN-R") == 0 ) knl_strcpy((char *)dr, names[i]);
		}
	}
	tm_printf((UB *)"  the disks to be put on: beside %s, over the whole %s, in a partition of %s\n",
		  ( dc[0] != 0 ) ? (char *)dc : "(none)", ( de[0] != 0 ) ? (char *)de : "(none)",
		  ( dr[0] != 0 ) ? (char *)dr : "(none)");
}

LOCAL void prog( void *ctx, INT step, UD done, UD total )
{
	INT	*last = (INT *)ctx;

	if ( step != *last ) {
		tm_printf((UB *)"  step %d\n", step);
		*last = step;
	}
}

/* ---------------------------------------------------------------- the plans */

LOCAL void test_plans( void )
{
	T_SIDISK	*all = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK) * SI_DISK_MAX);
	INT		n, i;

	KT_ASSERT(all != NULL);
	if ( all == NULL ) return;
	n = si_disks(all, SI_DISK_MAX);
	KT_ASSERT(n >= ( ( dc[0] != 0 ) ? 1 : 0 ) + ( ( de[0] != 0 ) ? 1 : 0 ));
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT(knl_strcmp((CONST char *)all[i].name, KT_DISK) != 0);	/* the one started from */
		KT_ASSERT(all[i].name[0] != 'u');				/* no USB disk */
	}
	Kfree(all);
	KT_ASSERT_ER(si_plan(KT_DISK, SI_COEXIST, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_BOOTDISK);
	KT_ASSERT_ER(si_plan(KT_DISK, SI_ERASE, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_BOOTDISK);
	KT_ASSERT_ER(si_source(&pl), E_OK);
	KT_ASSERT(pl.src_blocks > 0 && pl.src_used > 0 && pl.src_used <= pl.src_blocks);
	KT_ASSERT(pl.esp_bytes > 0);
	tm_printf((UB *)"  the source %s: %ld blocks, %ld taken; %ld bytes for the ESP\n", pl.src,
		  (D)pl.src_blocks, (D)pl.src_used, (D)pl.esp_bytes);
}

/* ---------------------------------------------------------------- over the whole disk */

LOCAL void test_erase( void )
{
	T_SIRES	res;
	UB	*s;
	ID	dd;
	SZ	asz;
	INT	last = -1, i;
	UD	ts_first;

	if ( de[0] == 0 ) KT_SKIP("no disk KTWIN-E (tools/x64/ktest64.sh NVME2=, tools/tests/mkwindisk.py)");
	KT_ASSERT_ER(si_disk_read((CONST char *)de, &dk), E_OK);
	KT_ASSERT_ER(si_plan((CONST char *)de, SI_ERASE | SI_KEEP, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_NOTSFS);			/* the disk goes whole */
	KT_ASSERT_ER(si_plan((CONST char *)de, SI_ERASE, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT_EQ(pl.nlost, dk.nparts);
	KT_ASSERT(pl.nlost >= 4);
	for ( i = 0; i < pl.nlost; i++ ) {
		tm_printf((UB *)"  goes: %d %s %ld MB \"%s\"\n", pl.lost[i].unit + 1, si_kind_name(pl.lost[i].kind),
			  (D)( ( pl.lost[i].last - pl.lost[i].first + 1 ) >> 11 ), pl.lost[i].name);
	}
	KT_ASSERT_EQ(pl.lost[0].kind, SI_K_ESP);
	KT_ASSERT_EQ(pl.lost[1].kind, SI_K_MSR);
	ts_first = pl.ts_first;

	KT_ASSERT_ER(si_run(&pl, prog, &last, &res), E_OK);
	KT_ASSERT_EQ(res.failed_step, -1);
	KT_ASSERT(res.copied > 0);
	KT_ASSERT(res.grow_later);			/* the driver keeps the partitions until the next start */

	KT_ASSERT_ER(si_disk_read((CONST char *)de, &dk), E_OK);
	KT_ASSERT(dk.gpt);
	KT_ASSERT_EQ(dk.nparts, 2);
	KT_ASSERT_EQ(dk.parts[0].kind, SI_K_ESP);
	KT_ASSERT_EQ(dk.parts[1].kind, SI_K_TSFS);
	KT_ASSERT_EQ(dk.parts[1].first, ts_first);
	KT_ASSERT(both_tables(de, dk.nsect));

	/* the ESP's boot sector, and the volume's superblock as the system volume's */
	s = (UB *)Kmalloc(4096);
	dd = tk_opn_dev(de, TD_READ);
	KT_ASSERT(s != NULL && dd >= E_OK);
	if ( s != NULL && dd >= E_OK ) {
		T_TSFSBLK_SB	sb;
		ID		vol = ts_vol_blk_of((CONST char *)knl_sysvol_name);

		KT_ASSERT_ER(tk_srea_dev_d(dd, (D)dk.parts[0].first, s, 512, &asz), E_OK);
		KT_ASSERT(same_b(s + 82, "FAT32   ", 8) && s[510] == 0x55 && s[511] == 0xAA);
		KT_ASSERT_ER(tk_srea_dev_d(dd, (D)dk.parts[1].first, s, 4096, &asz), E_OK);
		KT_ASSERT(same_b(s, TSFSBLK_MAGIC, 8));
		KT_ASSERT(vol > 0 && ts_ref_vol_blk(vol, &sb) >= E_OK && same_b(s + 24, sb.vol_uuid.b, 16));
	}
	if ( dd >= E_OK ) tk_cls_dev(dd, 0);
	if ( s != NULL ) Kfree(s);
}

/* ---------------------------------------------------------------- beside Windows */

LOCAL void test_coexist( void )
{
	T_SIDISK	*was = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK));
	T_SIRES		res;
	T_TSFSCK	ck;
	T_TSFSBLK_SB	sb, src;
	UB		esp[L_DEVNM + 1];
	UW		crc0[SI_PART_MAX], crc1, wcrc[NFILE_WIN], c1, c2;
	UD		wsz[NFILE_WIN], s1, s2;
	char		path[SI_NFILE][64];
	UD		size[SI_NFILE];
	INT		vd, i, k, last = -1, esp_i;
	ID		vol;

	if ( dc[0] == 0 ) KT_SKIP("no disk KTWIN-C (tools/x64/ktest64.sh NVME2=, tools/tests/mkwindisk.py)");
	KT_ASSERT(was != NULL);
	if ( was == NULL ) return;
	KT_ASSERT_ER(si_disk_read((CONST char *)dc, was), E_OK);
	KT_ASSERT(was->gpt && was->esp >= 0 && was->tsfs < 0);
	esp_i = was->esp;

	/* what must stay: every partition's bytes but the ESP's, and Windows' files on it */
	for ( i = 0; i < was->nparts; i++ ) {
		crc0[i] = 0;
		if ( i == esp_i ) continue;
		KT_ASSERT_ER(si_disk_crc((CONST char *)dc, was->parts[i].first,
					 was->parts[i].last - was->parts[i].first + 1, &crc0[i]), E_OK);
	}
	part_name(esp, dc, was->parts[esp_i].unit);
	vd = knl_fs_open_vol("fatfs", (CONST char *)esp, FS_MNT_RDONLY, 0);
	KT_ASSERT(vd >= 0);
	for ( k = 0; k < NFILE_WIN && vd >= 0; k++ ) {
		KT_ASSERT_ER(file_crc(vd, win_files[k], &wcrc[k], &wsz[k]), E_OK);
	}
	if ( vd >= 0 ) fs_close(vd);

	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST | SI_KEEP, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_NOTSFS);			/* no TessronOS there to keep */
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT(!pl.had_ts);
	KT_ASSERT_EQ(pl.ts_unit, -1);
	KT_ASSERT_EQ(pl.ts_first, was->free_first);
	KT_ASSERT_EQ(pl.ts_last, was->free_last);
	KT_ASSERT_EQ(pl.esp_unit, was->parts[esp_i].unit);
	KT_ASSERT_EQ(pl.nwin, 1);
	KT_ASSERT(!si_entry_first(&pl));		/* Windows stays: the entry goes after its */
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_ERASE, -1, &pl), E_OK);	/* the other way, looked at only */
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST, -1, &pl), E_OK);

	KT_ASSERT_ER(si_run(&pl, prog, &last, &res), E_OK);
	KT_ASSERT_EQ(res.failed_step, -1);
	KT_ASSERT(res.part[0] != 0);
	KT_ASSERT(res.entry_asked);
	KT_ASSERT(!res.grow_later);

	/* the table: the entries there were, and one more after them */
	KT_ASSERT_ER(si_disk_read((CONST char *)dc, &dk), E_OK);
	KT_ASSERT_EQ(dk.nparts, was->nparts + 1);
	KT_ASSERT(both_tables(dc, dk.nsect));
	for ( i = 0; i < was->nparts && i < dk.nparts; i++ ) {
		KT_ASSERT_EQ(dk.parts[i].first, was->parts[i].first);
		KT_ASSERT_EQ(dk.parts[i].last, was->parts[i].last);
		KT_ASSERT(same_b(dk.parts[i].type, was->parts[i].type, 16));
		KT_ASSERT(same_b(dk.parts[i].uniq, was->parts[i].uniq, 16));
		if ( i == esp_i ) continue;
		KT_ASSERT_ER(si_disk_crc((CONST char *)dc, was->parts[i].first,
					 was->parts[i].last - was->parts[i].first + 1, &crc1), E_OK);
		KT_ASSERT_EQ(crc1, crc0[i]);
		tm_printf((UB *)"  partition %d (%s): CRC %08x before, %08x after\n", i + 1,
			  si_kind_name(was->parts[i].kind), crc0[i], crc1);
	}
	k = dk.nparts - 1;
	KT_ASSERT_EQ(dk.parts[k].kind, SI_K_TSFS);
	KT_ASSERT_EQ(dk.parts[k].first, was->free_first);
	KT_ASSERT_EQ(dk.parts[k].last, was->free_last);
	KT_ASSERT(knl_strcmp((CONST char *)dk.parts[k].name, "TessronOS") == 0);

	/* the ESP: Windows' files the same, this system's added */
	vd = knl_fs_open_vol("fatfs", (CONST char *)esp, FS_MNT_RDONLY, 0);
	KT_ASSERT(vd >= 0);
	for ( k = 0; k < NFILE_WIN && vd >= 0; k++ ) {
		KT_ASSERT_ER(file_crc(vd, win_files[k], &c1, &s1), E_OK);
		KT_ASSERT_EQ(c1, wcrc[k]);
		KT_ASSERT_EQ(s1, wsz[k]);
	}
	KT_ASSERT_ER(si_boot_files(path, size), E_OK);
	if ( vd >= 0 ) {
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/BOOTX64.EFI", &c1, &s1), E_OK);
		KT_ASSERT_ER(file_crc(-1, path[SI_F_LOADER], &c2, &s2), E_OK);
		KT_ASSERT(c1 == c2 && s1 == s2);
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/tessronos.elf", &c1, &s1), E_OK);
		KT_ASSERT_ER(file_crc(-1, path[SI_F_KERNEL], &c2, &s2), E_OK);
		KT_ASSERT(c1 == c2 && s1 == s2);
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/INSTALL.TXT", &c1, &s1), E_OK);
		KT_ASSERT(s1 > 0);
		fs_close(vd);
	}

	/* the copy: the system volume's, as large as its partition, whole */
	vol = ts_vol_blk_of((CONST char *)knl_sysvol_name);
	KT_ASSERT(vol > 0 && ts_ref_vol_blk(vol, &src) >= E_OK);
	vol = ts_opn_vol_blk((CONST char *)res.part);
	KT_ASSERT(vol > 0);
	if ( vol > 0 ) {
		KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
		KT_ASSERT(same_b(sb.vol_uuid.b, src.vol_uuid.b, 16));
		KT_ASSERT(sb.total_blocks + 256 > ( pl.ts_last - pl.ts_first + 1 ) / TSFSBLK_SECT_PER_BLK);
		KT_ASSERT(sb.total_blocks > src.total_blocks);
		tm_printf((UB *)"  the copy %s: %ld blocks (the system volume %ld)\n", res.part,
			  (D)sb.total_blocks, (D)src.total_blocks);
		(void)ts_cls_vol_blk(vol);
	}
	knl_memset(&ck, 0, sizeof(ck));
	KT_ASSERT_ER(ob_chk_run((CONST char *)res.part, 0, &ck, NULL, NULL), E_OK);
	KT_ASSERT_EQ(ck.problems, 0);
	KT_ASSERT(ck.objects > 0);
	tm_printf((UB *)"  the copy checked: %d problems, %ld objects, %ld records\n", ck.problems,
		  (D)ck.objects, (D)ck.records);

	/* the loader is asked for the entry of this ESP */
	KT_ASSERT_ER(file_crc(-1, REQ, &c1, &s1), E_OK);
	KT_ASSERT(s1 > 0);
	Kfree(was);
}

LOCAL ER vrec_put( ID vol, CONST char *us, INT recno, CONST void *buf, SZ size );
/* The list of a newer system's objects: a disk with it is not put over */
LOCAL CONST char	newer[] = "{\"format\":1,\"kind\":\"system\",\"id\":\"tessronos\",\"version\":\"R9.999\","
			  "\"machine\":\"x64\",\"arch\":\"x86_64\",\"objects\":[],\"keep\":[],\"links\":[]}";

/* ---------------------------------------------------------------- in a partition */

LOCAL void test_replace( void )
{
	T_SIDISK	*was = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK));
	T_SIRES		res;
	T_TSFSCK	ck;
	UB		esp[L_DEVNM + 1];
	UW		crc0[SI_PART_MAX], crc1, wcrc[NFILE_WIN], c1;
	UD		wsz[NFILE_WIN], s1;
	INT		vd, i, k, last = -1, esp_i, dat = -1;

	if ( dr[0] == 0 ) KT_SKIP("no disk KTWIN-R (tools/x64/ktest64.sh NVME4=, tools/tests/mkwindisk.py)");
	KT_ASSERT(was != NULL);
	if ( was == NULL ) return;
	KT_ASSERT_ER(si_disk_read((CONST char *)dr, was), E_OK);
	KT_ASSERT(was->gpt && was->esp >= 0 && was->tsfs < 0);
	esp_i = was->esp;
	for ( i = 0; i < was->nparts; i++ ) {
		if ( was->parts[i].kind == SI_K_BASIC ) dat = i;
	}
	KT_ASSERT(dat >= 0);
	if ( dat < 0 ) {
		Kfree(was);
		return;
	}

	/* the partitions that are kept are not offered */
	for ( i = 0; i < was->nparts; i++ ) {
		KT_ASSERT_EQ(si_replaceable(&was->parts[i]), (BOOL)( i == dat ));
		if ( i == dat ) continue;
		KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE, was->parts[i].unit, &pl), E_OK);
		KT_ASSERT_EQ(pl.why, SI_R_KEEP);
	}

	for ( i = 0; i < was->nparts; i++ ) {
		crc0[i] = 0;
		if ( i == esp_i || i == dat ) continue;
		KT_ASSERT_ER(si_disk_crc((CONST char *)dr, was->parts[i].first,
					 was->parts[i].last - was->parts[i].first + 1, &crc0[i]), E_OK);
	}
	part_name(esp, dr, was->parts[esp_i].unit);
	vd = knl_fs_open_vol("fatfs", (CONST char *)esp, FS_MNT_RDONLY, 0);
	KT_ASSERT(vd >= 0);
	for ( k = 0; k < NFILE_WIN && vd >= 0; k++ ) {
		KT_ASSERT_ER(file_crc(vd, win_files[k], &wcrc[k], &wsz[k]), E_OK);
	}
	if ( vd >= 0 ) fs_close(vd);

	/* the basic data partition, grown over the free stretch after it; not kept, being Windows's */
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE | SI_KEEP, was->parts[dat].unit, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_NOTSFS);
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE, was->parts[dat].unit, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT_EQ(pl.ts_unit, was->parts[dat].unit);
	KT_ASSERT_EQ(pl.ts_first, was->parts[dat].first);
	KT_ASSERT_EQ(pl.ts_last, was->free_last);
	KT_ASSERT_EQ(pl.nlost, 1);
	KT_ASSERT_EQ(pl.nwin, 0);
	KT_ASSERT(si_entry_first(&pl));			/* no Windows left to start */
	tm_printf((UB *)"  %s%d: %ld to %ld sectors\n", dr, pl.ts_unit,
		  (D)( was->parts[dat].last - was->parts[dat].first + 1 ), (D)( pl.ts_last - pl.ts_first + 1 ));

	KT_ASSERT_ER(si_run(&pl, prog, &last, &res), E_OK);
	KT_ASSERT_EQ(res.failed_step, -1);
	KT_ASSERT(res.entry_asked);
	KT_ASSERT(!res.grow_later);

	KT_ASSERT_ER(si_disk_read((CONST char *)dr, &dk), E_OK);
	KT_ASSERT_EQ(dk.nparts, was->nparts);
	KT_ASSERT(both_tables(dr, dk.nsect));
	for ( i = 0; i < was->nparts && i < dk.nparts; i++ ) {
		KT_ASSERT_EQ(dk.parts[i].entry, was->parts[i].entry);
		KT_ASSERT_EQ(dk.parts[i].first, was->parts[i].first);
		if ( i == dat ) {
			KT_ASSERT_EQ(dk.parts[i].kind, SI_K_TSFS);
			KT_ASSERT_EQ(dk.parts[i].last, pl.ts_last);
			KT_ASSERT(knl_strcmp((CONST char *)dk.parts[i].name, "TessronOS") == 0);
			KT_ASSERT(!same_b(dk.parts[i].uniq, was->parts[i].uniq, 16));
			continue;
		}
		KT_ASSERT_EQ(dk.parts[i].last, was->parts[i].last);
		KT_ASSERT(same_b(dk.parts[i].type, was->parts[i].type, 16));
		KT_ASSERT(same_b(dk.parts[i].uniq, was->parts[i].uniq, 16));
		if ( i == esp_i ) continue;
		KT_ASSERT_ER(si_disk_crc((CONST char *)dr, was->parts[i].first,
					 was->parts[i].last - was->parts[i].first + 1, &crc1), E_OK);
		KT_ASSERT_EQ(crc1, crc0[i]);
		tm_printf((UB *)"  partition %d (%s): CRC %08x before, %08x after\n", i + 1,
			  si_kind_name(was->parts[i].kind), crc0[i], crc1);
	}
	vd = knl_fs_open_vol("fatfs", (CONST char *)esp, FS_MNT_RDONLY, 0);
	KT_ASSERT(vd >= 0);
	for ( k = 0; k < NFILE_WIN && vd >= 0; k++ ) {
		KT_ASSERT_ER(file_crc(vd, win_files[k], &c1, &s1), E_OK);
		KT_ASSERT_EQ(c1, wcrc[k]);
		KT_ASSERT_EQ(s1, wsz[k]);
	}
	if ( vd >= 0 ) {
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/tessronos.elf", &c1, &s1), E_OK);
		fs_close(vd);
	}
	knl_memset(&ck, 0, sizeof(ck));
	KT_ASSERT_ER(ob_chk_run((CONST char *)res.part, 0, &ck, NULL, NULL), E_OK);
	KT_ASSERT_EQ(ck.problems, 0);
	tm_printf((UB *)"  the copy %s checked: %d problems, %ld objects; volume %ld -> %ld bytes\n", res.part,
		  ck.problems, (D)ck.objects, (D)res.vol_from, (D)res.vol_to);

	/* put there again "beside Windows": the partition written again, and still first, no Windows being there */
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_COEXIST, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT_EQ(pl.ts_unit, was->parts[dat].unit);
	KT_ASSERT_EQ(pl.nwin, 0);
	KT_ASSERT(si_entry_first(&pl));

	/* kept, the partition chosen being TessronOS's now; with a newer system there, not (left so for the UI test) */
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE | SI_KEEP, was->parts[dat].unit, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT_EQ(pl.ts_last, dk.parts[dat].last);
	{
		ID	vol = ts_opn_vol((CONST char *)res.part, TSFS_STORE_BLK);

		KT_ASSERT(vol > 0);
		if ( vol > 0 ) {
			KT_ASSERT_ER(vrec_put(vol, SYSDEF_SYSTEM_BOX, 1, newer, (SZ)knl_strlen(newer)), E_OK);
			(void)ts_cls_vol(vol);
		}
	}
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE | SI_KEEP, was->parts[dat].unit, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_NEWER);
	KT_ASSERT_ER(si_plan((CONST char *)dr, SI_REPLACE, was->parts[dat].unit, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);		/* 初期化 is still there to choose */
	KT_ASSERT(pl.had_ts);
	Kfree(was);
}

/* ---------------------------------------------------------------- the TessronOS there kept */

#define KP_MINE		"01a0d8c4-5fb0-7b01-8c01-00000005fb01"	/* the person's, on the disk put on */
#define KP_TEXT		"THE PERSON'S OBJECT ON THE DISK"
#define KP_STALE	"an old version of the system's own record"

/* A record of an object of a volume of the native store whole, the caller's to Kfree */
LOCAL UB *vrec( ID vol, CONST char *us, INT recno, SZ *p_size )
{
	TS_UUID	u;
	T_RREC	r[TSFS_MAX_REC / 8];
	UB	*b = NULL;
	SZ	got = 0, asz;
	INT	cnt = 0, i;
	ID	od;

	*p_size = 0;
	(void)ts_str_to_uuid(us, &u);
	od = ts_opn_obj(vol, &u, TFO_READ);
	if ( od <= 0 ) return NULL;
	if ( ts_lst_rec(od, r, TSFS_MAX_REC / 8, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && r[i].recno != recno; i++ ) ;
		if ( i < cnt && ( b = (UB *)Kmalloc((SZ)r[i].size + 1) ) != NULL ) {
			while ( got < (SZ)r[i].size && ts_rea_rec(od, recno, got, b + got, (SZ)r[i].size - got, &asz) >= E_OK
				&& asz > 0 ) {
				got += asz;
			}
			b[got] = 0;
			*p_size = got;
		}
	}
	ts_cls_obj(od);
	return b;
}

/* A record of an object of a volume of the native store replaced, added when it is the next */
LOCAL ER vrec_put( ID vol, CONST char *us, INT recno, CONST void *buf, SZ size )
{
	TS_UUID	u;
	T_ROBJ	ro;
	INT	n;
	ID	od;
	ER	er;

	(void)ts_str_to_uuid(us, &u);
	od = ts_opn_obj(vol, &u, TFO_READ | TFO_WRITE);
	if ( od <= 0 ) return ( od < 0 ) ? (ER)od : E_SYS;
	er = ts_ref_obj(vol, &u, &ro);
	if ( er >= E_OK && recno == ro.nrec ) er = ts_ins_rec(od, -1, OB_RT_SYSDATA, 0, &n);
	if ( er >= E_OK ) er = ts_rpl_rec(od, recno, buf, size);
	ts_cls_obj(od);
	return er;
}

/* Whether a record of the volume is the running system's same record */
LOCAL BOOL same_as_running( ID vol, CONST char *us, INT recno )
{
	TS_UUID	u;
	UB	*a, *b;
	SZ	na = 0, nb = 0;
	BOOL	ok;

	(void)ts_str_to_uuid(us, &u);
	a = vrec(vol, us, recno, &na);
	b = om_obj_record(&u, recno, &nb);
	ok = (BOOL)( a != NULL && b != NULL && na == nb && same_b(a, b, (INT)na) );
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);
	return ok;
}

/*
 * The system put over the TessronOS that test_coexist put on KTWIN-C,
 * keeping it: the person's object made there, a link the person added to
 * システム, a record of the system's own made stale and the list of the
 * system's objects taken away (a disk put on before it was kept). After
 * it, the person's object and link are there, the stale record and the
 * list are the running system's, the kernel on its ESP is /boot's, and
 * the volume is checked whole. A list of a newer system there is refused.
 */
LOCAL void test_keep( void )
{
	T_SIDISK	*now = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK));
	T_SIRES		res;
	T_TSFSCK	ck;
	TS_UUID		mine, box;
	T_TAD		*doc = NULL;
	UB		part[L_DEVNM + 1], esp[L_DEVNM + 1], *b, *out;
	char		path[SI_NFILE][64];
	UD		size[SI_NFILE], s1, s2;
	UW		c1, c2;
	SZ		n = 0, osz = 0;
	TS_UUID		ids[64];
	INT		last = -1, k, nl, vd;
	ID		vol;
	BOOL		linked;
	T_FSTAT		st;

	if ( dc[0] == 0 ) KT_SKIP("no disk KTWIN-C (tools/x64/ktest64.sh NVME2=, tools/tests/mkwindisk.py)");
	KT_ASSERT(now != NULL);
	if ( now == NULL ) return;
	KT_ASSERT_ER(si_disk_read((CONST char *)dc, now), E_OK);
	if ( now->tsfs < 0 ) {
		Kfree(now);
		KT_SKIP("no TessronOS on KTWIN-C (test_coexist did not put it there)");
	}
	part_name(part, dc, now->parts[now->tsfs].unit);
	part_name(esp, dc, now->parts[now->esp].unit);
	(void)ts_str_to_uuid(KP_MINE, &mine);
	(void)ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &box);

	/* the running system keeps the list of its objects */
	b = om_obj_record(&box, 1, &n);
	KT_ASSERT(b != NULL && n > 0);
	if ( b != NULL ) Kfree(b);

	/* what the person did there, and a disk put on before the list was kept */
	vol = ts_opn_vol((CONST char *)part, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) {
		Kfree(now);
		return;
	}
	{
		static CONST char	meta[] = "{\"name\":\"登録先の利用者の実身\"}";

		KT_ASSERT_ER(knl_tsfs_cre_as(vol, &mine, (CONST UB *)meta, (INT)knl_strlen(meta)), E_OK);
	}
	KT_ASSERT_ER(vrec_put(vol, KP_MINE, 0, KP_TEXT, (SZ)knl_strlen(KP_TEXT)), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &mine), E_OK);
	b = vrec(vol, SYSDEF_SYSTEM_BOX, 0, &n);
	KT_ASSERT(b != NULL && tad_parse(b, n, NULL, &doc) >= E_OK && doc != NULL);
	if ( b != NULL ) Kfree(b);
	if ( doc != NULL ) {
		KT_ASSERT_EQ(om_tad_link_add(doc, &mine), 1);
		(void)tad_write_mem(doc, NULL, 0, &osz);
		out = (UB *)Kmalloc(osz + 1);
		KT_ASSERT(out != NULL && tad_write_mem(doc, out, osz + 1, &osz) >= E_OK);
		if ( out != NULL ) {
			KT_ASSERT_ER(vrec_put(vol, SYSDEF_SYSTEM_BOX, 0, out, osz), E_OK);
			Kfree(out);
		}
		tad_free(doc);
	}
	KT_ASSERT_ER(vrec_put(vol, SYSDEF_BOX_SYSINST, 0, KP_STALE, (SZ)knl_strlen(KP_STALE)), E_OK);
	{
		ID	od = ts_opn_obj(vol, &box, TFO_READ | TFO_WRITE);

		KT_ASSERT(od > 0);
		if ( od > 0 ) {
			KT_ASSERT_ER(ts_del_rec(od, 1), E_OK);
			ts_cls_obj(od);
		}
	}
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);

	/* both ways offered for it: 上書き and 初期化 */
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT(pl.had_ts);
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_ERASE | SI_KEEP, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_NOTSFS);
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST | SI_KEEP, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT(pl.keep && pl.nobj > 0 && pl.ver[0] == 'R' && pl.tver[0] == 0);
	KT_ASSERT(pl.vol_need > 0 && pl.vol_need <= pl.vol_free);
	tm_printf((UB *)"  kept %s: %s over %s, %d objects, %ld KB of %ld KB free\n", part, pl.ver,
		  ( pl.tver[0] != 0 ) ? (char *)pl.tver : "(no list)", pl.nobj, (D)( pl.vol_need >> 10 ),
		  (D)( pl.vol_free >> 10 ));

	KT_ASSERT_ER(si_run(&pl, prog, &last, &res), E_OK);
	KT_ASSERT_EQ(res.failed_step, -1);
	KT_ASSERT_EQ(res.copied, 0);
	KT_ASSERT(res.nobj > 0);
	KT_ASSERT(res.entry_asked);
	KT_ASSERT(knl_strcmp((CONST char *)res.part, (CONST char *)part) == 0);

	/* the table as it was */
	KT_ASSERT_ER(si_disk_read((CONST char *)dc, &dk), E_OK);
	KT_ASSERT_EQ(dk.nparts, now->nparts);
	KT_ASSERT(both_tables(dc, dk.nsect));

	/* what the person had stays; the system's own and the list are the running system's */
	vol = ts_opn_vol((CONST char *)part, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol > 0 ) {
		b = vrec(vol, KP_MINE, 0, &n);
		KT_ASSERT(b != NULL && n == (SZ)knl_strlen(KP_TEXT) && same_b(b, KP_TEXT, (INT)n));
		if ( b != NULL ) Kfree(b);
		b = vrec(vol, SYSDEF_SYSTEM_BOX, 0, &n);
		nl = ( b != NULL ) ? om_store_links(b, n, ids, 64) : 0;
		for ( linked = FALSE, k = 0; k < nl; k++ ) {
			if ( ts_uuid_cmp(&ids[k], &mine) == 0 ) linked = TRUE;
		}
		KT_ASSERT(linked);
		if ( b != NULL ) Kfree(b);
		KT_ASSERT(same_as_running(vol, SYSDEF_BOX_SYSINST, 0));
		KT_ASSERT(same_as_running(vol, SYSDEF_SYSTEM_BOX, 1));
		KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	}
	knl_memset(&ck, 0, sizeof(ck));
	KT_ASSERT_ER(ob_chk_run((CONST char *)part, 0, &ck, NULL, NULL), E_OK);
	KT_ASSERT_EQ(ck.problems, 0);
	tm_printf((UB *)"  kept %s checked: %d problems, %ld objects\n", part, ck.problems, (D)ck.objects);

	/* the ESP: the kernel /boot has, the note of the install, nothing left half put */
	KT_ASSERT_ER(si_boot_files(path, size), E_OK);
	vd = knl_fs_open_vol("fatfs", (CONST char *)esp, FS_MNT_RDONLY, 0);
	KT_ASSERT(vd >= 0);
	if ( vd >= 0 ) {
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/tessronos.elf", &c1, &s1), E_OK);
		KT_ASSERT_ER(file_crc(-1, path[SI_F_KERNEL], &c2, &s2), E_OK);
		KT_ASSERT(c1 == c2 && s1 == s2);
		KT_ASSERT_ER(file_crc(vd, "EFI/TESSRON/BOOTX64.EFI", &c1, &s1), E_OK);
		KT_ASSERT_ER(file_crc(-1, path[SI_F_LOADER], &c2, &s2), E_OK);
		KT_ASSERT(c1 == c2 && s1 == s2);
		KT_ASSERT(fs_statat(vd, "EFI/TESSRON/INSTALL.TXT", &st) >= EX_OK);
		KT_ASSERT(fs_statat(vd, "EFI/TESSRON/tessronos.elf.new", &st) < EX_OK);
		fs_close(vd);
	}

	/* now with the list: the version there is this one's; a newer one there is not put over */
	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST | SI_KEEP, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
	KT_ASSERT(knl_strcmp((CONST char *)pl.tver, (CONST char *)pl.ver) == 0);
	vol = ts_opn_vol((CONST char *)part, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol > 0 ) {
		b = vrec(vol, SYSDEF_SYSTEM_BOX, 1, &n);
		KT_ASSERT_ER(vrec_put(vol, SYSDEF_SYSTEM_BOX, 1, newer, (SZ)knl_strlen(newer)), E_OK);
		(void)ts_cls_vol(vol);
		KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST | SI_KEEP, -1, &pl), E_OK);
		KT_ASSERT_EQ(pl.why, SI_R_NEWER);
		vol = ts_opn_vol((CONST char *)part, TSFS_STORE_BLK);
		if ( vol > 0 && b != NULL ) KT_ASSERT_ER(vrec_put(vol, SYSDEF_SYSTEM_BOX, 1, b, n), E_OK);
		if ( vol > 0 ) (void)ts_cls_vol(vol);
		if ( b != NULL ) Kfree(b);
	}
	Kfree(now);
}

/* ---------------------------------------------------------------- started from a copy */

LOCAL void test_booted_copy( void )
{
	UB	boot[L_DEVNM + 1], sys[L_DEVNM + 1];
	UD	from = 0, to = 0;
	INT	n;

	knl_strcpy((char *)boot, (CONST char *)knl_bootvol_name);
	knl_strcpy((char *)sys, (CONST char *)knl_sysvol_name);
	for ( n = (INT)knl_strlen((char *)boot); n > 0 && boot[n - 1] >= '0' && boot[n - 1] <= '9'; n-- ) ;
	boot[n] = 0;
	tm_printf((UB *)"  started from the copy on %s: /boot %s, system volume %s\n", boot, knl_bootvol_name, sys);
	KT_ASSERT(sys[0] != 0);
	KT_ASSERT(same_b(sys, boot, n) && sys[n] >= '0' && sys[n] <= '9');
	KT_ASSERT(ts_vol_blk_of((CONST char *)sys) > 0);
	knl_sysvol_grown(&from, &to);
	tm_printf((UB *)"  the system volume at this start: %ld -> %ld bytes\n", (D)from, (D)to);
	KT_ASSERT(to >= from && to > 0);
}

EXPORT void ktest_sysinst( void )
{
	T_FSTAT	st;

	if ( fs_stat(INSTALLED, &st) >= EX_OK ) {
		KT_RUN(test_booted_copy);
		return;
	}
	find_disks();
	KT_RUN(test_plans);
	KT_RUN(test_erase);
	KT_RUN(test_coexist);
	KT_RUN(test_keep);
	KT_RUN(test_replace);
}
