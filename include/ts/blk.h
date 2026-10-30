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

#define BLK_MAX_PART		255		/* partitions kept per device, as many as a device has subunits (MAX_UNIT): GPT entries, or the MBR's primary and logical ones */
#define BLK_SECTOR_SIZE		512

/*
 * Attribute number (write, no data): what the device holds in its own
 * cache is put onto the medium before the request completes. A device
 * that keeps no such cache completes it at once; a driver that does not
 * know the number answers E_PAR (design 11.14.2).
 */
#define TDN_FLUSH		(-24)

/*
 * Attribute number (write to the whole disk, no data): the partition
 * table is read again and the partitions follow it, one made larger and
 * one added after the others (gpt.c writes them). The partitions there
 * were keep their numbers; a table that moves or takes away one of them
 * is not followed (E_OBJ). The device is registered again with its new
 * number of subunits.
 */
#define TDN_PARTSCAN		(-25)

/*
 * Attribute number (read, a UW): the bytes the device takes best in one
 * request -- what the file system asks of it at once when what it reads
 * or writes lies in one run on the medium (design 7.3). A driver that
 * does not know the number answers E_PAR, and the file system keeps to
 * its own default.
 */
#define TDN_IOMAX		(-26)

/*
 * A write that is on the medium when it completes, past the device's
 * own cache (Force Unit Access): written to TDN_FUAWRITE with a T_DEVFUA
 * that says where, what and how much (whole blocks of the unit). A
 * device that cannot answers E_PAR, and the caller writes and then
 * empties the cache (TDN_FLUSH). The journal of TSFS writes its batch
 * this way, which saves the second emptying of the cache (design 7.3).
 */
#define TDN_FUAWRITE		(-27)

typedef struct {
	D		start;		/* the first block of the unit */
	CONST void	*buf;
	SZ		size;		/* bytes, whole blocks */
} T_DEVFUA;

/*
 * Disk information, returned for attribute number TDN_DISKINFO
 * (T-Kernel/SM 5.3.8 disk device rules)
 */
typedef struct {
	UINT	protect:1;	/* write protected */
	UINT	removable:1;	/* medium can be taken out */
	W	blocksize;	/* bytes in a logical block */
	W	blockcont;	/* logical blocks of the unit, at most 2^31 - 1 */
	D	blockcont_d;	/* logical blocks of the unit, all of them (T-Kernel 2.0) */
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
 * The partition table of a device in use read again into `tbl`, for
 * TDN_PARTSCAN: the partitions it holds keep their places, and may only
 * grow or shrink; new ones are added after them; the count is stored
 * last, so a request reading the table meanwhile sees either. Answers the
 * number of partitions, or E_OBJ (nothing changed) when one of those in
 * `tbl` has moved or gone.
 */
IMPORT INT knl_part_rescan( BLK_READFN readfn, void *exinf, UD nsect_total, T_PARTTBL *tbl );

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

/*
 * The SD bus as the driver set it up: one of SD_SPEED_*, E_NOEXS
 * without a card; the clock in *p_hz.
 */
#define SD_SPEED_DEFAULT	0	/* 25MHz at 3.3V */
#define SD_SPEED_HS		1	/* the High Speed mode, 50MHz at 3.3V */
#define SD_SPEED_SDR12		2	/* UHS-I, at 1.8V */
#define SD_SPEED_SDR25		3
#define SD_SPEED_SDR50		4
#define SD_SPEED_DDR50		5
#define SD_SPEED_SDR104		6
IMPORT INT knl_sd_speed( UW *p_hz );

/*
 * How the SD driver ends a multiple block transfer: by CMD12 after it,
 * by SET_BLOCK_COUNT (CMD23) sent before it, or by the controller's own
 * Auto-CMD23. For a test, one of them forced, or SD_CMD23_DECIDE back
 * to what the card and the controller say; a negative value only asks.
 * Answers the way in use before, E_NOSPT for Auto-CMD23 on a controller
 * older than version 3.00, E_NOEXS without a card.
 */
#define SD_CMD23_NONE		0
#define SD_CMD23_MANUAL		1
#define SD_CMD23_AUTO		2
#define SD_CMD23_DECIDE		3
IMPORT INT knl_sd_cmd23( INT way );

/*
 * Bring the SD card up again from the start, for a test, with these
 * made to happen on a card that cannot really switch to 1.8V:
 * SD_TEST_UHS offers it the UHS-I bus speeds whatever the slot has,
 * SD_TEST_S18A takes it to have accepted 1.8V, SD_TEST_NO_CMD11 to have
 * switched without CMD11, SD_TEST_BAD_READ spoils the read that checks a
 * UHS-I bus speed. 0 brings it up as at start-up. Answers SD_SPEED_* or
 * an error.
 */
#define SD_TEST_UHS		0x01
#define SD_TEST_S18A		0x02
#define SD_TEST_NO_CMD11	0x04
#define SD_TEST_BAD_READ	0x08
IMPORT INT knl_sd_reinit( UW test );

/*
 * At shutdown and before a restart (Raspberry Pi 5): a card at 1.8V
 * powered off and its lines put back at 3.3V.
 */
IMPORT void knl_sd_finish( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_BLK_H__ */
