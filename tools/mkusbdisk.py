#!/usr/bin/env python3
"""Make the disk image the USB mass storage tests use.

Layout (512 byte sectors):
  0      MBR with one partition of type 0x0c (FAT32, LBA)
  2048   the partition: a FAT32 volume with the files mkdisk.py puts on its
         FAT partition (HELLO.TXT and the rest)
  end-2048 .. end  left out of the partition: a scratch area the tests
         write and read back raw

Usage: mkusbdisk.py <output> [size_mb]
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfat import fat32_format, SECTOR      # noqa: E402

SCRATCH = 2048


def main():
    out = sys.argv[1]
    size_mb = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    nsect = size_mb * 1024 * 1024 // SECTOR
    p_start = 2048
    p_sectors = nsect - p_start - SCRATCH

    img = bytearray(nsect * SECTOR)
    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446,
                     0x00, b"\x00\x21\x00", 0x0C, b"\xfe\xff\xff",
                     p_start, p_sectors)
    struct.pack_into("<I", mbr, 440, 0x55534244)             # disk signature
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr
    fat32_format(img, p_start, p_sectors)

    # the scratch area starts with a mark, so a wrong offset shows
    tag = b"TESSRONOS USB SCRATCH"
    off = (nsect - SCRATCH) * SECTOR
    img[off:off + len(tag)] = tag

    with open(out, "wb") as f:
        f.write(img)
    print("%s: %d MB, FAT32 at %d+%d, scratch %d sectors"
          % (out, size_mb, p_start, p_sectors, SCRATCH))


if __name__ == "__main__":
    main()
