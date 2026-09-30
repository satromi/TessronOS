/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	blk.h
 *	Block devices and partition tables (design 10.6, 11.9)
 *
 *	A block driver registers itself with tk_def_dev() following the
 *	T-Kernel/SM disk device rules: data is addressed in logical blocks,
 *	the whole device is subunit 0 and each partition is a subunit of
 *	its own (1 .. nsub). Attribute number TDN_DISKINFO returns the
 *	geometry of the unit that was opened.
 */

#ifndef __TS_BLK_H__
#define __TS_BLK_H__

#ifdef __cplusplus
extern "C" {
#endif

#define BLK_MAX_PART		8		/* partitions kept per device */
#define BLK_SECTOR_SIZE		512

/*
 * Attribute number (write, no data): what the device holds in its own
 * cache is put onto the medium before the request completes. A device
 * that keeps no such cache completes it at once; a driver that does not
 * know the number answers E_PAR (design 11.14.2).
 */
#define TDN_FLUSH		(-24)

/*
 * Disk information, returned for attribute number TDN_DISKINFO
 * (T-Kernel/SM 5.3.8 disk device rules)
 */
typedef struct {
	UINT	protect:1;	/* write protected */
	UINT	removable:1;	/* medium can be taken out */
	W	blocksize;	/* bytes in a logical block */
	W	blockcont;	/* logical blocks of the unit */
} DiskInfo;

/*
 * Partition of a block device (start and length in sectors)
 */
typedef struct {
	UD	start;
	UD	nsect;
	UB	type_guid[16];		/* GPT type GUID, 0 for an MBR entry */
	UB	part_guid[16];		/* GPT unique GUID */
	UW	mbr_type;		/* MBR partition type, 0 for a GPT entry */
	BOOL	valid;
} T_PARTITION;

/*
 * Partition table of one device
 */
typedef struct {
	INT		n;			/* entries in use */
	BOOL		gpt;			/* TRUE: GPT, FALSE: MBR */
	T_PARTITION	part[BLK_MAX_PART];
} T_PARTTBL;

/*
 * Read sectors for the partition scanner. Returns E_OK or an error;
 * reads nsect sectors of BLK_SECTOR_SIZE from sector 'start' into 'buf'.
 */
typedef ER (*BLK_READFN)( void *exinf, UD start, UD nsect, void *buf );

/*
 * Read the partition table of a device (kernel/sysman/part.c)
 *	GPT first: the protective MBR is accepted and the GPT header at
 *	sector 1 is used. A disk without a valid GPT is read as MBR.
 *	Returns the number of partitions found, or an error.
 */
IMPORT INT knl_read_parttbl( BLK_READFN readfn, void *exinf, UD nsect_total, T_PARTTBL *tbl );

/*
 * GUID of a TessronOS (TSFS) partition, design 11.6:
 *	54465331-0000-4000-8000-5446532d5631
 */
IMPORT CONST UB knl_tsfs_type_guid[16];

/* Basic data partition (FAT), EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
IMPORT CONST UB knl_basic_data_guid[16];

/*
 * virtio-blk driver (device/vblk/vblk.c): registers every block device
 * the machine provides as "vblka", "vblkb", ... Returns how many were
 * found.
 */
IMPORT INT knl_vblk_init( void );

/*
 * SD host controller (device/sd/sdhci.c; Raspberry Pi 5, and sdhci-pci
 * under QEMU). Registers the card as "sda" with its partitions as
 * subunits. Answers 1 when a card was registered, 0 when there is none.
 */
IMPORT INT knl_sd_init( void );

/*
 * Force the way the SD driver moves data (0 the data port, 1 the single
 * address mode, 2 and 3 the descriptor table with 32 and 64 bit
 * addresses), for a test; a negative value only asks. Answers the way in
 * use before, or E_NOSPT when the controller lacks the one asked for.
 */
IMPORT INT knl_sd_dma_mode( INT mode );

#ifdef __cplusplus
}
#endif

#endif /* __TS_BLK_H__ */
