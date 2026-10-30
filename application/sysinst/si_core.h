/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	si_core.h
 *	システム登録: the system the PC was started from (a USB memory or
 *	a microSD card) put on an internal disk (docs/tessronos/03-platform.md
 *	3.10.6). Kernel side, x64 only.
 *
 *	Three ways:
 *
 *	  SI_COEXIST	beside Windows: a TessronOS partition made in the
 *			largest free stretch of the disk (or the one there
 *			is written again), and the loader, the kernel and
 *			the settings added under \EFI\TESSRON\ of the EFI
 *			system partition there is. Nothing of the other
 *			partitions is written, nor any file of the ESP but
 *			those under \EFI\TESSRON\.
 *	  SI_REPLACE	one partition chosen (a Windows volume, Linux, or
 *			TessronOS) made the TessronOS partition in its place,
 *			grown into the free stretch right after it; the ESP
 *			as in SI_COEXIST. The other partitions (the ESP, the
 *			Microsoft reserved one, Windows recovery) keep their
 *			numbers and their bytes.
 *	  SI_ERASE	the whole disk: a new GPT with an ESP (FAT32, made
 *			here, \EFI\BOOT\BOOTX64.EFI and \EFI\TESSRON\) and a
 *			TessronOS partition over the rest.
 *
 *	The system volume is copied block by block while its journal is
 *	frozen (knl_jrnl_freeze): every block its maps say is taken, the
 *	superblocks and the journal; the rest is not read. The copy keeps
 *	the volume's UUID, which every block's head carries. A partition
 *	larger than the volume has the volume grown to it, here when its
 *	device can be opened, else at the next start (knl_sysvol_grow).
 *
 *	The firmware is asked to start the copy by a request left on the
 *	ESP the system was started from (\EFI\TESSRON\BOOTREG.TXT): the
 *	loader reads it at the next start from there, while the boot
 *	services are there, writes Boot#### and BootOrder, sets BootNext
 *	and starts the machine again (tools/x64/uefi/loader.c).
 */

#ifndef __SI_CORE_H__
#define __SI_CORE_H__

#include <tk/tkernel.h>
#include <ts/blk.h>
#include <ts/gpt.h>

#define SI_COEXIST		0
#define SI_ERASE		1
#define SI_REPLACE		2

#define SI_DISK_MAX		8
#define SI_PART_MAX		32
#define SI_NAME_MAX		( GPT_NAME_MAX * 3 + 1 )	/* a partition's name in UTF-8 */

#define SI_ESP_MB		260		/* the ESP SI_ERASE makes on a disk below 32GB */
#define SI_ESP_MB_LARGE		512		/* and on a larger one */
#define SI_ESP_SLACK		( 4ULL << 20 )	/* bytes left on an ESP for the loader's records */

/* What a partition is, as its type says */
#define SI_K_OTHER		0
#define SI_K_ESP		1
#define SI_K_MSR		2		/* Microsoft reserved */
#define SI_K_BASIC		3		/* basic data: a Windows volume, or FAT */
#define SI_K_WINRE		4		/* Windows recovery */
#define SI_K_TSFS		5
#define SI_K_LINUX		6

/* One partition of a disk, as its table has it */
typedef struct {
	INT	unit;			/* its subunit, as part.c counts them */
	INT	entry;			/* its entry in the table */
	UD	first, last;		/* sectors */
	UB	type[16];
	UB	uniq[16];		/* its unique GUID */
	UD	attr;			/* its attributes */
	INT	kind;			/* SI_K_* */
	BOOL	bitlocker;		/* a basic data volume encrypted (-FVE-FS-) */
	UB	name[SI_NAME_MAX];	/* UTF-8 */
} T_SIPART;

/* A disk the system may be put on */
typedef struct {
	UB	name[L_DEVNM + 1];	/* "nvmea" */
	UD	nsect;			/* 512 byte sectors */
	BOOL	gpt;			/* a GPT that passes */
	BOOL	blank;			/* nothing in its first sectors */
	INT	nparts;
	INT	esp;			/* index in parts of its ESP, -1 none */
	INT	tsfs;			/* of its first TessronOS partition, -1 none */
	BOOL	bitlocker;		/* a volume of it is encrypted */
	UD	free_first, free_last;	/* its largest free stretch, 0 0 none */
	T_SIPART parts[SI_PART_MAX];
} T_SIDISK;

/* Why a way cannot be taken (si_plan) */
#define SI_R_OK			0
#define SI_R_NODISK		1	/* the disk is not there, or not one of 512 byte sectors */
#define SI_R_NOGPT		2	/* SI_COEXIST on a disk with no GPT (an MBR, or nothing) */
#define SI_R_NOESP		3	/* SI_COEXIST on a disk with no EFI system partition */
#define SI_R_NOROOM		4	/* no free stretch as large as the system volume */
#define SI_R_ESPFULL		5	/* the ESP has not the room for the files */
#define SI_R_SMALL		6	/* SI_ERASE on a disk smaller than the ESP and the volume */
#define SI_R_INUSE		7	/* a partition of the disk is in use */
#define SI_R_NOSYS		8	/* no system volume, or no loader or kernel on /boot */
#define SI_R_BOOTDISK		9	/* the disk the system runs from */
#define SI_R_KEEP		10	/* SI_REPLACE of a partition that is kept: the ESP, MSR, recovery, one not known */

/* What will be done: si_plan fills it in, si_run does it */
typedef struct {
	INT	mode;			/* SI_COEXIST, SI_ERASE, SI_REPLACE */
	INT	why;			/* SI_R_* */
	UB	disk[L_DEVNM + 1];
	UD	nsect;
	/* the source */
	UB	src[L_DEVNM + 1];	/* the system volume's partition */
	UD	src_blocks;		/* of the volume, 4KB each */
	UD	src_used;		/* taken, as its groups count */
	UD	esp_bytes;		/* the files for the ESP */
	/* the ESP: the one there is (SI_COEXIST) or the one made */
	INT	esp_entry;		/* its entry in the table */
	INT	esp_unit;
	UD	esp_first, esp_last;
	UB	esp_uniq[16];
	UD	esp_free;		/* bytes free on it now (SI_COEXIST) */
	/* the TessronOS partition */
	INT	ts_unit;		/* one there is, written again; -1 one made */
	UD	ts_first, ts_last;
	/* SI_ERASE, SI_REPLACE: what goes (SI_REPLACE: the one partition, before it grows) */
	INT	nlost;
	INT	nwin;			/* basic data partitions (Windows' volumes) the disk keeps */
	T_SIPART lost[SI_PART_MAX];
	UB	disk_guid[16];		/* the new table's */
	UB	ts_uniq[16];
} T_SIPLAN;

/* The steps, as the progress says them */
#define SI_ST_CHECK		0	/* the plan looked at again */
#define SI_ST_TABLE		1	/* the partition table */
#define SI_ST_COPY		2	/* the system volume copied */
#define SI_ST_VERIFY		3	/* the copy read back */
#define SI_ST_GROW		4	/* the volume made as large as its partition */
#define SI_ST_ESP		5	/* the ESP's files */
#define SI_ST_ENTRY		6	/* the firmware's boot entry asked for */
#define SI_ST_DONE		7

/* What si_run tells as it goes: the step, and bytes done of the step's */
typedef void (*SI_PROG)( void *ctx, INT step, UD done, UD total );

/* What si_run did */
typedef struct {
	UB	part[L_DEVNM + 1];	/* the TessronOS partition's device, "" when it has none yet */
	UD	copied;			/* blocks copied */
	UD	vol_from, vol_to;	/* the volume's bytes before and after it was grown (0 0 not grown) */
	BOOL	grow_later;		/* grown at the next start */
	BOOL	entry_asked;		/* BOOTREG.TXT written */
	INT	failed_step;		/* SI_ST_* of a failure, -1 none */
} T_SIRES;

/*
 * The disks the system may be put on: NVMe and SATA, neither the one
 * the system was started from nor a USB one. Answers how many.
 */
IMPORT INT si_disks( T_SIDISK *buf, INT max );

/* One disk read: E_NOEXS when it is not there, E_NOSPT for sectors not of 512 bytes */
IMPORT ER si_disk_read( CONST char *disk, T_SIDISK *pk );

/* Whether the new boot entry goes first in BootOrder: no Windows is left on the disk to start */
IMPORT BOOL si_entry_first( CONST T_SIPLAN *pl );

/* The word a way is said with on the console and in INSTALL.TXT */
IMPORT CONST char *si_mode_name( INT mode );

/* The words a kind is shown with */
IMPORT CONST char *si_kind_name( INT kind );

/* What the system to copy is: its volume's partition, its size and the files of the ESP */
IMPORT ER si_source( T_SIPLAN *pl );

/* Whether a partition can be made the TessronOS one by SI_REPLACE */
IMPORT BOOL si_replaceable( CONST T_SIPART *p );

/*
 * The way looked at, with the partition (its subunit) SI_REPLACE makes
 * another: pl->why is SI_R_OK when it can be taken; E_OK either way,
 * but for a disk that cannot be read
 */
IMPORT ER si_plan( CONST char *disk, INT mode, INT unit, T_SIPLAN *pl );

/* Everything done as planned; the progress told to cb */
IMPORT ER si_run( T_SIPLAN *pl, SI_PROG cb, void *ctx, T_SIRES *res );

/*
 * The pieces, for the tests: a fresh GPT of the partitions given
 * written whole over a disk (both copies, the protective MBR); a FAT32
 * volume made in [first, last] of a disk with the files of the ESP;
 * the system volume copied onto [first, first + nsect) of an open
 * device; a CRC-32 of sectors of a disk.
 */
typedef struct {
	UD	first, last;
	CONST UB *type;
	UB	uniq[16];
	CONST char *name;
} T_SINEWPART;

IMPORT ER si_gpt_write( ID dd, UD nsect, CONST UB *disk_guid, CONST T_SINEWPART *p, INT n );
IMPORT ER si_fat_make( ID dd, UD first, UD last, BOOL fallback, CONST char *note );
IMPORT ER si_vol_copy( CONST char *src, ID dd, UD first, UD nsect, SI_PROG cb, void *ctx, UD *p_copied );
IMPORT UW si_crc32( UW crc, CONST UB *p, UD n );
IMPORT ER si_disk_crc( CONST char *dev, UD first, UD nsect, UW *p_crc );

/* The ESP's files of the running system (as si_run takes them): the path on /boot of each, "" none */
#define SI_F_LOADER	0
#define SI_F_KERNEL	1
#define SI_F_CMDLINE	2
#define SI_NFILE	3
IMPORT ER si_boot_files( char (*path)[64], UD *size );

#endif /* __SI_CORE_H__ */
