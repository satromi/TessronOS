#!/usr/bin/env python3
"""Make an SD card image for the Raspberry Pi 5 and 4 (design 13.6).

Layout (512 byte sectors, GPT):
  0      protective MBR
  1      GPT header
  2..33  partition entry array
  2048   partition 1, basic data (FAT32): what the boot loader reads
         (config.txt, the device tree, tessronos.img) and /boot's files
  ...    partition 2, TessronOS (TSFS) type GUID: the system volume as
         tools/mktsfs made it, attached at start as sda1 (CNF_OB_SYSVOL).
         When it is the last partition, the first start grows it and its
         volume into the rest of the card, up to CNF_SYSVOL_GROW_MB, and
         moves the backup table to the card's end (design 7.7)
  ...    partition 3, only with --scratch-mb: the scratch type GUID, empty.
         The in-kernel tests (make KTEST=1) format it and write its
         sectors; they find it by that type and touch nothing else of
         the card below the file system (sda2)
  end-33 backup entry array, end  backup header

Usage: mksd.py --sys SYSVOL [--boot-mb N] [--scratch-mb N] [--log-mb N] [--esp] <output> <file for the FAT root ...>

--log-mb N adds a partition for the records the PC's loader and kernel write
(tools/x64, kernel/sysdepend/x64/bootlog.c): N MB of FAT32 labelled TSLOG, of
the basic data type and named TSLOG, which Windows shows as a drive of its
own even on a removable disk (the EFI system partition it does not show). It
lies right after partition 1, so that the system volume stays the last
partition and may grow; its entry is the last of the table, so that the
other partitions keep their numbers (nvmea0 /boot, nvmea1 the system).

--esp gives partition 1 the type of an EFI system partition, for a PC whose
UEFI firmware starts EFI/BOOT/BOOTX64.EFI from it (tools/x64); a DEST may
then name a directory of a directory (EFI/BOOT/BOOTX64.EFI).

A file is placed under its own base name; give it the name the boot
loader or the kernel looks for (tessronos.img, INIT.ELF ...), or name it
DEST=SOURCE to place SOURCE as DEST, which may be dir/name for a directory
of the root (the Raspberry Pi 4 firmware's overlays/disable-bt.dtbo).
"""
import os
import struct
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkdisk import (SECTOR, ENTRIES, ENTRY_SIZE, TSFS_GUID, BASIC_DATA_GUID,  # noqa: E402
                    crc32, gpt_header)
from mkfat import fat32_format                                                # noqa: E402

# The partition the tests may format: "TSFS-SC" where the system's type has "TSFS-V1"
SCRATCH_GUID = uuid.UUID("54465331-0000-4000-8000-5446532d5343")
# The EFI system partition
ESP_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")


def fat32_blank(img, start, nsect, label):
    """An empty FAT32 volume of nsect sectors at img's sector start, with a volume label."""
    spc, rsvd, nfats = 1, 32, 2
    fatsz = 1
    while True:
        clusters = (nsect - rsvd - nfats * fatsz) // spc
        need = -(-(clusters + 2) * 4 // SECTOR)
        if need <= fatsz:
            break
        fatsz = need
    clusters = (nsect - rsvd - nfats * fatsz) // spc
    assert clusters >= 65525, "not enough clusters for FAT32: %d" % clusters
    name = label.encode("ascii").ljust(11)[:11]
    bpb = bytearray(SECTOR)
    bpb[0:3] = b"\xeb\x58\x90"
    bpb[3:11] = b"TESSRON "
    struct.pack_into("<HBHBHHBHHHII", bpb, 11,
                     SECTOR, spc, rsvd, nfats, 0, 0, 0xF8, 0, 63, 255, start, nsect)
    struct.pack_into("<IHHIHH", bpb, 36, fatsz, 0, 0, 2, 1, 6)
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into("<I", bpb, 67, 0x54534C47)
    bpb[71:82] = name
    bpb[82:90] = b"FAT32   "
    struct.pack_into("<H", bpb, 510, 0xAA55)
    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<II", fsinfo, 488, clusters - 1, 3)
    struct.pack_into("<H", fsinfo, 510, 0xAA55)
    base = start * SECTOR
    img[base:base + nsect * SECTOR] = bytes(nsect * SECTOR)
    img[base:base + SECTOR] = bpb
    img[base + SECTOR:base + 2 * SECTOR] = fsinfo
    img[base + 6 * SECTOR:base + 7 * SECTOR] = bpb
    img[base + 7 * SECTOR:base + 8 * SECTOR] = fsinfo
    fat = bytearray(SECTOR)
    struct.pack_into("<III", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)    # cluster 2 the root
    for k in range(nfats):
        off = base + (rsvd + k * fatsz) * SECTOR
        img[off:off + SECTOR] = fat
    root = bytearray(SECTOR)
    root[0:11] = name
    root[11] = 0x08                                     # the volume label
    off = base + (rsvd + nfats * fatsz) * SECTOR
    img[off:off + SECTOR] = root


def main():
    args = sys.argv[1:]
    sysvol = None
    boot_mb = 256
    scratch_mb = 0
    boot_guid = BASIC_DATA_GUID
    log_mb = 0
    while args and args[0].startswith("--"):
        if args[0] == "--sys":
            sysvol = args[1]
            args = args[2:]
        elif args[0] == "--boot-mb":
            boot_mb = int(args[1])
            args = args[2:]
        elif args[0] == "--scratch-mb":
            scratch_mb = int(args[1])
            args = args[2:]
        elif args[0] == "--log-mb":
            log_mb = int(args[1])
            args = args[2:]
        elif args[0] == "--esp":
            boot_guid = ESP_GUID
            args = args[1:]
        else:
            sys.exit("mksd: unknown option %s" % args[0])
    if sysvol is None or not args:
        sys.exit(__doc__)
    out, files = args[0], args[1:]

    entry_sectors = ENTRIES * ENTRY_SIZE // SECTOR
    first_usable = 2 + entry_sectors
    p1_start, p1_sectors = 2048, boot_mb * 2048
    plog_start, plog_sectors = p1_start + p1_sectors, log_mb * 2048
    p2_start = plog_start + plog_sectors
    sysvol_len = os.path.getsize(sysvol)
    p2_sectors = -(-sysvol_len // SECTOR // 2048) * 2048
    p3_start, p3_sectors = p2_start + p2_sectors, scratch_mb * 2048
    # the image ends one MB past the last partition, room for the backup table
    nsect = p3_start + p3_sectors + 2048
    last_usable = nsect - 1 - 1 - entry_sectors
    assert p3_start + p3_sectors <= last_usable + 1

    parts = [
        (boot_guid, p1_start, p1_start + p1_sectors - 1, "TessronOS boot"),
        (TSFS_GUID, p2_start, p2_start + p2_sectors - 1, "TessronOS system"),
    ]
    if p3_sectors > 0:
        parts.append((SCRATCH_GUID, p3_start, p3_start + p3_sectors - 1, "TessronOS scratch"))
    if plog_sectors > 0:
        parts.append((BASIC_DATA_GUID, plog_start, plog_start + plog_sectors - 1, "TSLOG"))
    entries = bytearray(ENTRIES * ENTRY_SIZE)
    for i, (type_guid, first, last, name) in enumerate(parts):
        e = struct.pack("<16s16sQQQ72s",
                        type_guid.bytes_le, uuid.uuid4().bytes_le,
                        first, last, 0, name.encode("utf-16-le").ljust(72, b"\0"))
        entries[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = e
    entries_crc = crc32(bytes(entries))
    disk_guid = uuid.uuid4()

    # what is built in memory: the tables and the boot partition; the system
    # volume, gigabytes of it, is copied into the file a piece at a time
    img = bytearray(p2_start * SECTOR)
    tail_lba = nsect - 1 - entry_sectors
    tail = bytearray((nsect - tail_lba) * SECTOR)

    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446,
                     0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff",
                     1, min(nsect - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr
    img[SECTOR:2 * SECTOR] = gpt_header(1, nsect - 1, first_usable, last_usable,
                                        2, entries_crc, disk_guid)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    backup_entry_lba = tail_lba
    tail[0:len(entries)] = entries
    tail[(nsect - 1 - tail_lba) * SECTOR:] = gpt_header(
        nsect - 1, 1, first_usable, last_usable, backup_entry_lba, entries_crc, disk_guid)

    fat32_format(img, p1_start, p1_sectors, files)
    if plog_sectors > 0:
        fat32_blank(img, plog_start, plog_sectors, "TSLOG")
    piece = 64 * 1024 * 1024
    with open(out, "wb") as f:
        f.truncate(nsect * SECTOR)
        f.write(img)
        with open(sysvol, "rb") as v:
            while True:
                b = v.read(piece)
                if not b:
                    break
                f.write(b)
        f.seek(tail_lba * SECTOR)
        f.write(tail)
    print("%s: %d MB, boot FAT32 %d MB, system volume %d MB%s%s"
          % (out, nsect * SECTOR // (1024 * 1024), boot_mb, p2_sectors // 2048,
             ", scratch %d MB" % scratch_mb if scratch_mb > 0 else "",
             ", records FAT32 %d MB (TSLOG)" % log_mb if log_mb > 0 else ""))


if __name__ == "__main__":
    main()
