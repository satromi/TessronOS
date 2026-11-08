#!/usr/bin/env python3
"""Make the USB disks the test of many disks and volumes uses (ktest_vols,
docs/tessronos 7.6).

  mkvoldisk.py OUTDIR

Four images go into OUTDIR. ktest_vols plugs them into the machine through
the QEMU monitor, each a USB disk of its own, so that there are seventeen
volumes at once besides the machine's own: TSFS volumes of objects, FAT
volumes of three kinds, an exFAT one, and a
partition with nothing on it. The sizes, none of them that of a disk the test machine has, tell the disks apart.

  vola.img  50MB  GPT:  1 TSFS "VOLA1" 6MB, 2 FAT16 8MB, 3 TSFS "VOLA3" 6MB,
                        4 exFAT 1MB, 5 NTFS 4MB
  volb.img  44MB  MBR:  1 FAT16 8MB, 2 extended holding six FAT12 of 2MB
                        and a TSFS "VOLB" of 6MB (MBR type 0xDA), as
                        logical partitions linked one after another
  volc.img  26MB  GPT:  1 TSFS "VOLC1" 6MB, 2 blank 6MB (the test copies a
                        volume into it), 3 FAT16 8MB
  vold.img  18MB  no partition table: a FAT16 volume on the whole disk
  volp.img  30MB  GPT of 40 partitions: 39 blank ones of 512KB, and a FAT12
                  "LAST" of 2MB as the fortieth, past what one table of 16
                  would have held
  volx00.img ... volx21.img  1MB each, blank: plugged in together, with the
                  machine's own, they are more than 26 disks
  volg.img  96MB  blank: ktest_grow lays its own tables down on it (design 7.7)
  volh.img  64MB  GPT:  1 TSFS "VOLH" 8MB, the rest free: the desktop's
                  拡張 and 区画作成 (tools/tests/ui_disk_grow.steps)

The TSFS volumes are made by tools/mktsfs, the FAT ones by tools/mkfat.py.
"""

import os
import struct
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
from mkfat import fat_format                    # noqa: E402
from mkdisk import crc32, gpt_header, TSFS_GUID, BASIC_DATA_GUID, \
    ENTRIES, ENTRY_SIZE, SECTOR                 # noqa: E402

MB = 2048                                       # sectors of a megabyte
EXFAT_GUID = BASIC_DATA_GUID


def mktsfs(path, start, nsect, label):
    subprocess.run([sys.executable, os.path.join(TOOLS, "mktsfs"), "-q", "-L", label,
                    "--offset", str(start * SECTOR), "--size", str(nsect * SECTOR), path],
                   check=True)


def gpt_disk(path, size_mb, parts):
    """parts: (kind, sectors, label); kind is tsfs, fat16, exfat or blank"""
    nsect = size_mb * MB
    entry_sectors = ENTRIES * ENTRY_SIZE // SECTOR
    first_usable = 2 + entry_sectors
    last_usable = nsect - 2 - entry_sectors
    img = bytearray(nsect * SECTOR)
    entries = bytearray(ENTRIES * ENTRY_SIZE)
    at = MB
    placed = []
    for i, (kind, n, label) in enumerate(parts):
        assert at + n - 1 <= last_usable, "image too small"
        guid = TSFS_GUID if kind == "tsfs" else BASIC_DATA_GUID
        e = struct.pack("<16s16sQQQ72s", guid.bytes_le, uuid.uuid4().bytes_le,
                        at, at + n - 1, 0, label.encode("utf-16-le").ljust(72, b"\0"))
        entries[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = e
        placed.append((kind, at, n, label))
        at += n
    ecrc = crc32(bytes(entries))
    dguid = uuid.uuid4()
    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff",
                     1, min(nsect - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr
    img[SECTOR:2 * SECTOR] = gpt_header(1, nsect - 1, first_usable, last_usable, 2, ecrc, dguid)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    back = nsect - 1 - entry_sectors
    img[back * SECTOR:back * SECTOR + len(entries)] = entries
    img[(nsect - 1) * SECTOR:] = gpt_header(nsect - 1, 1, first_usable, last_usable, back, ecrc, dguid)
    fill(img, placed)
    with open(path, "wb") as f:
        f.write(img)
    for kind, start, n, label in placed:
        if kind == "tsfs":
            mktsfs(path, start, n, label)


def fill(img, placed):
    for kind, start, n, label in placed:
        if kind == "fat16":
            fat_format(img, start, n, 16, spc=2, root_ents=64)
        elif kind == "fat12":
            fat_format(img, start, n, 12, spc=1, root_ents=32)
        elif kind == "exfat":
            exfat_format(img, start, n, label)
        elif kind == "ntfs":
            ntfs_format(img, start, n, label)


def ntfs_format(img, start, n, label):
    """An NTFS volume by mkfs.ntfs, with one file the tests know: VOLS.BIN, 3000 bytes"""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        vol = os.path.join(d, "n.img")
        data = os.path.join(d, "f")
        with open(vol, "wb") as f:
            f.truncate(n * SECTOR)
        with open(data, "wb") as f:
            f.write(bytes((i * 3) & 0xFF for i in range(3000)))
        subprocess.run(["/usr/sbin/mkfs.ntfs", "-F", "-f", "-q", "-L", label, vol], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["/usr/sbin/ntfscp", "-f", vol, data, "VOLS.BIN"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(vol, "rb") as f:
            img[start * SECTOR:(start + n) * SECTOR] = f.read()


def exfat_format(img, start, n, label):
    """An exFAT volume, clusters of 4KB: bitmap, up-case table and root in the
    first three. The up-case table says only that every code is itself."""
    fat_off, fat_len, heap_off, spc_sh = 24, 8, 64, 3
    clusters = (n - heap_off) >> spc_sh
    base = start * SECTOR
    clus = lambda c: base + (heap_off + ((c - 2) << spc_sh)) * SECTOR

    boot = bytearray(12 * SECTOR)
    boot[0:3] = b"\xeb\x76\x90"
    boot[3:11] = b"EXFAT   "
    struct.pack_into("<QQIIIIIIHHBBBBB", boot, 64, start, n, fat_off, fat_len, heap_off, clusters,
                     4, 0x12345678, 0x100, 0, 9, spc_sh, 1, 0x80, 0)
    struct.pack_into("<H", boot, 510, 0xAA55)
    for s in range(1, 9):
        struct.pack_into("<H", boot, s * SECTOR + 510, 0xAA55)
    total = 0
    for i, b in enumerate(boot[:11 * SECTOR]):
        if i in (106, 107, 112):
            continue
        total = (((total >> 1) | (total << 31)) + b) & 0xFFFFFFFF
    for off in range(11 * SECTOR, 12 * SECTOR, 4):
        struct.pack_into("<I", boot, off, total)
    img[base:base + 12 * SECTOR] = boot
    img[base + 12 * SECTOR:base + 24 * SECTOR] = boot

    fat = bytearray(16 * 4)
    struct.pack_into("<IIIII", fat, 0, 0xFFFFFFF8, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
    img[base + fat_off * SECTOR:base + fat_off * SECTOR + len(fat)] = fat

    img[clus(2)] = 0x07				# the first three clusters taken
    upcase = struct.pack("<HH", 0xFFFF, 0xFFFF) + struct.pack("<HH", 0xFFFF, 0x0001)
    upsum = 0
    for b in upcase:
        upsum = (((upsum >> 1) | (upsum << 31)) + b) & 0xFFFFFFFF
    img[clus(3):clus(3) + len(upcase)] = upcase

    root = bytearray(3 * 32)
    name = (label or "").encode("utf-16-le")[:22]
    root[0] = 0x83 if name else 0x03
    root[1] = len(name) // 2
    root[2:2 + len(name)] = name
    root[32] = 0x81
    struct.pack_into("<IQ", root, 32 + 20, 2, (clusters + 7) // 8)
    root[64] = 0x82
    struct.pack_into("<I", root, 64 + 4, upsum)
    struct.pack_into("<IQ", root, 64 + 20, 3, len(upcase))
    img[clus(4):clus(4) + len(root)] = root


def mbr_entry(boot, kind, start, n):
    return struct.pack("<B3sB3sII", boot, b"\xfe\xff\xff", kind, b"\xfe\xff\xff", start, n)


def mbr_disk(path, size_mb):
    """A primary FAT16 and an extended partition of logical ones."""
    nsect = size_mb * MB
    img = bytearray(nsect * SECTOR)
    p1 = (MB, 8 * MB)
    ext_start = p1[0] + p1[1]
    ext_len = nsect - ext_start
    logical = [("fat12", 2 * MB, "")] * 6 + [("tsfs", 6 * MB, "VOLB")]
    mbr = bytearray(SECTOR)
    mbr[446:462] = mbr_entry(0, 0x06, p1[0], p1[1])
    mbr[462:478] = mbr_entry(0, 0x0F, ext_start, ext_len)
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr
    placed = [("fat16", p1[0], p1[1], "")]
    ebr = ext_start
    for i, (kind, n, label) in enumerate(logical):
        start = ebr + 63                        # the logical partition, after its link
        sect = bytearray(SECTOR)
        sect[446:462] = mbr_entry(0, 0x01 if kind == "fat12" else 0xDA, 63, n)
        if i + 1 < len(logical):
            nxt = start + n
            sect[462:478] = mbr_entry(0, 0x05, nxt - ext_start, logical[i + 1][1] + 63)
        struct.pack_into("<H", sect, 510, 0xAA55)
        img[ebr * SECTOR:(ebr + 1) * SECTOR] = sect
        placed.append((kind, start, n, label))
        ebr = start + n
    assert ebr <= nsect, "image too small"
    fill(img, placed)
    with open(path, "wb") as f:
        f.write(img)
    for kind, start, n, label in placed:
        if kind == "tsfs":
            mktsfs(path, start, n, label)


def floppy_disk(path, size_mb):
    nsect = size_mb * MB
    img = bytearray(nsect * SECTOR)
    fat_format(img, 0, nsect, 16, spc=2, root_ents=64)
    with open(path, "wb") as f:
        f.write(img)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    gpt_disk(os.path.join(out, "vola.img"), 50,
             [("tsfs", 6 * MB, "VOLA1"), ("fat16", 8 * MB, "FAT A"), ("tsfs", 6 * MB, "VOLA3"),
              ("exfat", 1 * MB, "EXFAT"), ("ntfs", 4 * MB, "NTFS")])
    mbr_disk(os.path.join(out, "volb.img"), 44)
    gpt_disk(os.path.join(out, "volc.img"), 26,
             [("tsfs", 6 * MB, "VOLC1"), ("blank", 6 * MB, "COPY"), ("fat16", 8 * MB, "FAT C")])
    floppy_disk(os.path.join(out, "vold.img"), 18)
    gpt_disk(os.path.join(out, "volp.img"), 30,
             [("blank", MB // 2, "P%d" % i) for i in range(39)] + [("fat12", 2 * MB, "LAST")])
    for i in range(22):
        with open(os.path.join(out, "volx%02d.img" % i), "wb") as f:
            f.truncate(MB * 512)
    with open(os.path.join(out, "volg.img"), "wb") as f:
        f.truncate(96 * MB * 512)
    gpt_disk(os.path.join(out, "volh.img"), 64, [("tsfs", 8 * MB, "VOLH")])
    print("%s: vola.img volb.img volc.img vold.img volp.img volx00..21.img volg.img volh.img" % out)


if __name__ == "__main__":
    main()
