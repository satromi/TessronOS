#!/usr/bin/env python3
"""Make a GPT partitioned disk image for the TessronOS block device tests.

Layout (512 byte sectors):
  0      protective MBR
  1      GPT header
  2..33  partition entry array (128 entries x 128 bytes)
  2048   partition 1, TessronOS (TSFS) type GUID, 16MB, for the tests to format
  ...    partition 2, basic data (FAT) type GUID
  ...    partition 3, TessronOS (TSFS) type GUID: the system volume, when
         --sys gives an image of one (made by tools/mktsfs)
  ...    partition 4, basic data: a FAT16 volume of 6MB (tools/mkfat.py)
  ...    partition 5, basic data: a FAT12 volume of 2MB (tools/mkfat.py)
  end-33 backup entry array, end  backup header

Usage: mkdisk.py [--sys IMAGE] <output> [size_mb] [file to place in the FAT root ...]
"""

import binascii
import os
import struct
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfat import fat_format, fat32_format    # noqa: E402

SECTOR = 512
ENTRIES = 128
ENTRY_SIZE = 128

TSFS_GUID = uuid.UUID("54465331-0000-4000-8000-5446532d5631")
BASIC_DATA_GUID = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def gpt_header(my_lba, alt_lba, first, last, entry_lba, entries_crc, disk_guid):
    h = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0,
                     b"EFI PART", 0x00010000, 92, 0, 0,
                     my_lba, alt_lba, first, last,
                     disk_guid.bytes_le, entry_lba, ENTRIES, ENTRY_SIZE, entries_crc)
    struct.pack_into("<I", h, 16, crc32(bytes(h)))
    return bytes(h) + b"\0" * (SECTOR - 92)


def main():
    args = sys.argv[1:]
    sysvol = None
    if len(args) >= 2 and args[0] == "--sys":
        with open(args[1], "rb") as f:
            sysvol = f.read()
        args = args[2:]
    out = args[0]
    size_mb = int(args[1]) if len(args) > 1 else 96
    files = args[2:]
    nsect = size_mb * 1024 * 1024 // SECTOR

    entry_sectors = ENTRIES * ENTRY_SIZE // SECTOR          # 32
    first_usable = 2 + entry_sectors                        # 34
    last_usable = nsect - 1 - 1 - entry_sectors             # backup array + header

    p1_start, p1_sectors = 2048, 16 * 2048                  # 16MB, TSFS
    p2_start = p1_start + p1_sectors
    p3_sectors = 0
    if sysvol is not None:
        p3_sectors = -(-len(sysvol) // SECTOR // 2048) * 2048
    p4_sectors = 6 * 2048                                   # FAT16
    p5_sectors = 2 * 2048                                   # FAT12
    p2_sectors = (last_usable + 1 - p2_start - p3_sectors - p4_sectors - p5_sectors) \
        // 2048 * 2048                                      # FAT32
    p3_start = p2_start + p2_sectors
    p4_start = p3_start + p3_sectors
    p5_start = p4_start + p4_sectors
    assert p5_start + p5_sectors <= last_usable + 1, "image too small"

    parts = [
        (TSFS_GUID, p1_start, p1_start + p1_sectors - 1, "TessronOS TSFS"),
        (BASIC_DATA_GUID, p2_start, p2_start + p2_sectors - 1, "TessronOS boot"),
    ]
    if sysvol is not None:
        parts.append((TSFS_GUID, p3_start, p3_start + p3_sectors - 1, "TessronOS system"))
    parts.append((BASIC_DATA_GUID, p4_start, p4_start + p4_sectors - 1, "TessronOS FAT16"))
    parts.append((BASIC_DATA_GUID, p5_start, p5_start + p5_sectors - 1, "TessronOS FAT12"))

    entries = bytearray(ENTRIES * ENTRY_SIZE)
    for i, (type_guid, first, last, name) in enumerate(parts):
        e = struct.pack("<16s16sQQQ72s",
                        type_guid.bytes_le, uuid.uuid4().bytes_le,
                        first, last, 0, name.encode("utf-16-le").ljust(72, b"\0"))
        entries[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = e
    entries_crc = crc32(bytes(entries))
    disk_guid = uuid.uuid4()

    img = bytearray(nsect * SECTOR)

    # Protective MBR: one entry of type 0xEE covering the disk
    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446,
                     0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff",
                     1, min(nsect - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr

    img[SECTOR:2 * SECTOR] = gpt_header(1, nsect - 1, first_usable, last_usable,
                                        2, entries_crc, disk_guid)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries

    backup_entry_lba = nsect - 1 - entry_sectors
    img[backup_entry_lba * SECTOR:backup_entry_lba * SECTOR + len(entries)] = entries
    img[(nsect - 1) * SECTOR:nsect * SECTOR] = gpt_header(
        nsect - 1, 1, first_usable, last_usable, backup_entry_lba, entries_crc, disk_guid)

    # Mark the start of partition 1 so a wrong offset is visible
    tag = b"TESSRONOS PART 0"
    img[p1_start * SECTOR:p1_start * SECTOR + len(tag)] = tag

    # Partition 2 carries a FAT32 file system with a few files to read
    fat32_format(img, p2_start, p2_sectors, files)

    # Partition 3 is the system volume as mktsfs made it
    if sysvol is not None:
        img[p3_start * SECTOR:p3_start * SECTOR + len(sysvol)] = sysvol

    # Partitions 4 and 5 carry the smaller FAT layouts: FAT16 with two
    # sectors to a cluster and a root of 64 entries, FAT12 with one sector
    # to a cluster, a root of 32 entries and a file scattered over the
    # table's entries that straddle two sectors
    fat_format(img, p4_start, p4_sectors, 16, spc=2, root_ents=64)
    fat_format(img, p5_start, p5_sectors, 12, spc=1, root_ents=32, big=True)

    with open(out, "wb") as f:
        f.write(img)
    print("%s: %d MB, %d sectors, %d partitions" % (out, size_mb, nsect, len(parts)))


if __name__ == "__main__":
    main()
