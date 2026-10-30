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

	KT_ASSERT_ER(si_plan((CONST char *)dc, SI_COEXIST, -1, &pl), E_OK);
	KT_ASSERT_EQ(pl.why, SI_R_OK);
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

	/* the basic data partition, grown over the free stretch after it */
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
	Kfree(was);
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
	KT_RUN(test_replace);
}
