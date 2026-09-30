#!/usr/bin/env python3
"""Make FAT volumes for the TessronOS tests, and check FAT volumes.

  mkfat.py make {12|16|32} <output> [size_kb]
      write a volume with no partition table (the boot sector at sector 0,
      as on a "superfloppy" medium) holding the test files below
  mkfat.py make floppy <output>
      the same on a 1.44MB floppy: FAT12, 2880 sectors, 224 root entries,
      media byte 0xF0, 18 sectors to a track and two heads
  mkfat.py check [-v] <image> ...
      check every FAT volume of a disk image: the basic data partitions of
      a GPT, the FAT partitions of an MBR, or the whole image when it has
      no partition table. Exits with 1 when anything is wrong.

The files every volume made here holds (the tests read them):
  HELLO.TXT            "TessronOS FAT12 test file.\\n" (or FAT16)
  DATA.BIN             9 sectors, each filled with "SECTOR nnnn "
  SUB/INNER.TXT        "inner file\\n"
  日本語のファイル.txt   a long name: "日本語\\n" in UTF-8
  BIG.BIN              (FAT12) 700 clusters taken every other cluster, each
                       sector filled with "BIG nnnnn "; it crosses the
                       twelve bit entries that straddle two sectors

The volume is written with the stdlib alone: nothing here needs mkfs.fat.
"""

import os
import struct
import sys

SECTOR = 512

FAT16_MIN_CLUS = 4085
FAT32_MIN_CLUS = 65525

JP_NAME = "日本語のファイル.txt"
JP_TEXT = "日本語\n".encode("utf-8")


# ---------------------------------------------------------------- names

def short_sum(name83):
    """The checksum the long-name entries carry of the short name after
    them."""
    s = 0
    for ch in name83:
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


def lfn_entries(long_name, name83):
    """The entries that hold a long name: thirteen UTF-16 units each, the
    last piece first."""
    raw = long_name.encode("utf-16-le")
    units = [raw[i] | (raw[i + 1] << 8) for i in range(0, len(raw), 2)]
    units.append(0)
    while len(units) % 13:
        units.append(0xFFFF)
    n = len(units) // 13
    out = bytearray()
    sumv = short_sum(name83)
    for i in range(n - 1, -1, -1):
        piece = units[i * 13:(i + 1) * 13]
        e = bytearray(32)
        e[0] = (i + 1) | (0x40 if i == n - 1 else 0)
        e[11] = 0x0F
        e[13] = sumv
        for k in range(5):
            struct.pack_into("<H", e, 1 + k * 2, piece[k])
        for k in range(6):
            struct.pack_into("<H", e, 14 + k * 2, piece[5 + k])
        for k in range(2):
            struct.pack_into("<H", e, 28 + k * 2, piece[11 + k])
        out += e
    return bytes(out)


def dirent(name83, attr, clus, size, date=0x5A21, time=0x6000):
    e = bytearray(32)
    e[0:11] = name83
    e[11] = attr
    struct.pack_into("<HH", e, 14, time, date)
    struct.pack_into("<H", e, 18, date)
    struct.pack_into("<HH", e, 22, time, date)
    struct.pack_into("<H", e, 26, clus & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


# ---------------------------------------------------------------- making

def fat_layout(nsect, bits, spc, rsvd, nfats, root_ents):
    """Sectors of one table, and the clusters there are, for a volume of
    nsect sectors."""
    root_sects = (root_ents * 32 + SECTOR - 1) // SECTOR
    fatsz = 1
    while True:
        clusters = (nsect - rsvd - nfats * fatsz - root_sects) // spc
        need = -(-((clusters + 2) * bits // 8 + 1) // SECTOR)
        if need <= fatsz:
            return fatsz, clusters, root_sects
        fatsz = need


def boot_code():
    """What sits in the boot code area: a message, and at its end bytes
    that look like a partition entry but for the first byte, so that the
    sector is not taken for a partition table on a medium that has none."""
    code = bytearray(448)                       # offsets 62 .. 509
    msg = b"\xfa\xf4\xeb\xfd TessronOS test volume: not a system disk.\r\n"
    code[0:len(msg)] = msg
    fake = struct.pack("<B3sB3sII", 0x54, b"\x00\x01\x00", 0x06, b"\x00\x02\x00", 16, 32)
    code[446 - 62:446 - 62 + 16] = fake
    return bytes(code)


def fat_format(img, start, nsect, bits, spc=1, root_ents=64, nfats=2, rsvd=1,
               label=b"TESSRONOS  ", big=False, media=0xF8, spt=63, heads=255):
    """Write a FAT12 or FAT16 volume with the test files into img[start:]."""
    assert bits in (12, 16)
    fatsz, clusters, root_sects = fat_layout(nsect, bits, spc, rsvd, nfats, root_ents)
    kind = 12 if clusters < FAT16_MIN_CLUS else 16 if clusters < FAT32_MIN_CLUS else 32
    assert kind == bits, "%d clusters make FAT%d, not FAT%d" % (clusters, kind, bits)
    root_start = start + rsvd + nfats * fatsz
    data_start = root_start + root_sects
    csize = spc * SECTOR
    eoc = 0xFFF if bits == 12 else 0xFFFF

    bpb = bytearray(SECTOR)
    bpb[0:3] = b"\xeb\x3c\x90"
    bpb[3:11] = b"TESSRON "
    struct.pack_into("<HBHBHHBHHHII", bpb, 11,
                     SECTOR, spc, rsvd, nfats, root_ents,
                     nsect if nsect < 0x10000 else 0, media, fatsz, spt, heads, start,
                     nsect if nsect >= 0x10000 else 0)
    bpb[36] = 0x80
    bpb[38] = 0x29
    struct.pack_into("<I", bpb, 39, 0x54460000 | bits)
    bpb[43:54] = label
    bpb[54:62] = ("FAT%d   " % bits).encode()
    bpb[62:510] = boot_code()
    struct.pack_into("<H", bpb, 510, 0xAA55)
    img[start * SECTOR:(start + 1) * SECTOR] = bpb

    fat = [0] * (clusters + 2)
    fat[0] = (0xF00 if bits == 12 else 0xFF00) | media
    fat[1] = eoc
    nxt = [2]

    def chain(clist):
        for i, c in enumerate(clist):
            fat[c] = eoc if i == len(clist) - 1 else clist[i + 1]
        return clist[0]

    def alloc(n, step=1):
        clist = [nxt[0] + i * step for i in range(n)]
        nxt[0] = clist[-1] + 1
        chain(clist)
        return clist

    def write(clist, data):
        for i, c in enumerate(clist):
            chunk = data[i * csize:(i + 1) * csize]
            off = (data_start + (c - 2) * spc) * SECTOR
            img[off:off + len(chunk)] = chunk

    root = bytearray()
    root += dirent(label, 0x08, 0, 0)

    hello = ("TessronOS FAT%d test file.\n" % bits).encode()
    cl = alloc(1)
    write(cl, hello)
    root += dirent(b"HELLO   TXT", 0x20, cl[0], len(hello))

    data = bytearray()
    for s in range(9):
        stamp = ("SECTOR %04d " % s).encode()
        data += (stamp * (SECTOR // len(stamp) + 1))[:SECTOR]
    cl = alloc(-(-len(data) // csize))
    write(cl, data)
    root += dirent(b"DATA    BIN", 0x20, cl[0], len(data))

    sub = alloc(1)
    inner = b"inner file\n"
    ci = alloc(1)
    write(ci, inner)
    subdir = dirent(b".          ", 0x10, sub[0], 0) + dirent(b"..         ", 0x10, 0, 0) \
        + dirent(b"INNER   TXT", 0x20, ci[0], len(inner))
    write(sub, subdir)
    root += dirent(b"SUB        ", 0x10, sub[0], 0)

    jp83 = b"______~1TXT"
    cl = alloc(1)
    write(cl, JP_TEXT)
    root += lfn_entries(JP_NAME, jp83) + dirent(jp83, 0x20, cl[0], len(JP_TEXT))

    if big:
        n = 700
        bigdata = bytearray()
        for s in range(n * spc):
            stamp = ("BIG %05d " % s).encode()
            bigdata += (stamp * (SECTOR // len(stamp) + 1))[:SECTOR]
        cl = alloc(n, step=2)
        write(cl, bigdata)
        root += dirent(b"BIG     BIN", 0x20, cl[0], len(bigdata))

    assert len(root) <= root_ents * 32
    off = root_start * SECTOR
    img[off:off + len(root)] = root

    raw = fat_bytes(fat, bits, fatsz)
    for i in range(nfats):
        off = (start + rsvd + i * fatsz) * SECTOR
        img[off:off + len(raw)] = raw
    return clusters


def fat_bytes(fat, bits, fatsz):
    raw = bytearray(fatsz * SECTOR)
    for c, val in enumerate(fat):
        if bits == 16:
            struct.pack_into("<H", raw, c * 2, val)
        else:
            off = c + c // 2
            if c & 1:
                raw[off] = (raw[off] & 0x0F) | ((val << 4) & 0xF0)
                raw[off + 1] = (val >> 4) & 0xFF
            else:
                raw[off] = val & 0xFF
                raw[off + 1] = (raw[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
    return raw


def fat32_format(img, start, nsect, extra=()):
    """Write a FAT32 volume with a known set of files into img[start:]."""
    spc = 1                                     # sectors per cluster
    rsvd = 32
    nfats = 2

    # FAT size: every cluster needs one 32 bit entry
    fatsz = 1
    while True:
        clusters = (nsect - rsvd - nfats * fatsz) // spc
        need = -(-(clusters + 2) * 4 // SECTOR)
        if need <= fatsz:
            break
        fatsz = need
    clusters = (nsect - rsvd - nfats * fatsz) // spc
    assert clusters >= 65525, "not enough clusters for FAT32: %d" % clusters

    data_start = start + rsvd + nfats * fatsz

    bpb = bytearray(SECTOR)
    bpb[0:3] = b"\xeb\x58\x90"
    bpb[3:11] = b"TESSRON "
    struct.pack_into("<HBHBHHBHHHII", bpb, 11,
                     SECTOR, spc, rsvd, nfats, 0, 0, 0xF8, 0, 63, 255, start, nsect)
    struct.pack_into("<IHHIHH", bpb, 36, fatsz, 0, 0, 2, 1, 6)
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into("<I", bpb, 67, 0x54464F52)
    bpb[71:82] = b"TESSRONOS  "
    bpb[82:90] = b"FAT32   "
    struct.pack_into("<H", bpb, 510, 0xAA55)
    img[start * SECTOR:(start + 1) * SECTOR] = bpb
    img[(start + 6) * SECTOR:(start + 7) * SECTOR] = bpb        # backup

    fat = bytearray(fatsz * SECTOR)
    struct.pack_into("<II", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF)
    next_free = [3]                             # cluster 2 is the root

    def set_fat(clus, val):
        struct.pack_into("<I", fat, clus * 4, val & 0x0FFFFFFF)

    def alloc_chain(nclus):
        first = next_free[0]
        for i in range(nclus):
            c = first + i
            set_fat(c, 0x0FFFFFFF if i == nclus - 1 else c + 1)
        next_free[0] += nclus
        return first

    set_fat(2, 0x0FFFFFFF)                      # root directory

    def write_clusters(first, data):
        for i in range(-(-len(data) // (spc * SECTOR))):
            c = first + i
            off = (data_start + (c - 2) * spc) * SECTOR
            chunk = data[i * spc * SECTOR:(i + 1) * spc * SECTOR]
            img[off:off + len(chunk)] = chunk

    def dirent(name83, attr, clus, size, date=0x5A21, time=0x6000):
        e = bytearray(32)
        e[0:11] = name83.encode()
        e[11] = attr
        struct.pack_into("<HH", e, 14, time, date)
        struct.pack_into("<H", e, 18, date)
        struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)
        struct.pack_into("<HH", e, 22, time, date)
        struct.pack_into("<H", e, 26, clus & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    root = bytearray()
    root += dirent("TESSRONOS  ", 0x08, 0, 0)           # volume label

    hello = b"TessronOS FAT test file." + b"\n"
    c_hello = alloc_chain(1)
    write_clusters(c_hello, hello)
    root += dirent("HELLO   TXT", 0x20, c_hello, len(hello))

    # a file that spans several clusters, each sector stamped with its number
    nsec_data = 9
    data = bytearray()
    for s in range(nsec_data):
        sec = bytearray(SECTOR)
        stamp = ("SECTOR %04d " % s).encode()
        for off in range(0, SECTOR, len(stamp)):
            sec[off:off + min(len(stamp), SECTOR - off)] = stamp[:SECTOR - off]
        data += sec
    c_data = alloc_chain(nsec_data)
    write_clusters(c_data, bytes(data))
    root += dirent("DATA    BIN", 0x20, c_data, len(data))

    # a subdirectory with one file
    c_sub = alloc_chain(1)
    inner = b"inner file" + b"\n"
    c_inner = alloc_chain(1)
    write_clusters(c_inner, inner)
    sub = bytearray()
    sub += dirent(".          ", 0x10, c_sub, 0)
    sub += dirent("..         ", 0x10, 0, 0)
    sub += dirent("INNER   TXT", 0x20, c_inner, len(inner))
    write_clusters(c_sub, bytes(sub))
    root += dirent("SUB        ", 0x10, c_sub, 0)

    def short_sum(name83):
        """The checksum the long-name entries carry, so that a reader can
        tell that they belong to the short entry after them."""
        s = 0
        for ch in name83.encode():
            s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
        return s

    def lfn_entries(long_name, name83):
        """The entries that carry a name too long for the eleven bytes of a
        directory entry: thirteen characters each, last piece first, every
        one holding the checksum of the short name they belong to."""
        chars = [ord(c) for c in long_name] + [0]
        while len(chars) % 13:
            chars.append(0xFFFF)
        n = len(chars) // 13
        out = bytearray()
        sumv = short_sum(name83)
        for i in range(n - 1, -1, -1):
            piece = chars[i * 13:(i + 1) * 13]
            e = bytearray(32)
            e[0] = (i + 1) | (0x40 if i == n - 1 else 0)
            e[11] = 0x0F                    # the mark of a long-name entry
            e[12] = 0
            e[13] = sumv
            e[26:28] = b"\x00\x00"
            for k in range(5):
                struct.pack_into("<H", e, 1 + k * 2, piece[k])
            for k in range(6):
                struct.pack_into("<H", e, 14 + k * 2, piece[5 + k])
            for k in range(2):
                struct.pack_into("<H", e, 28 + k * 2, piece[11 + k])
            out += e
        return bytes(out)

    # files named on the command line, placed in the root
    taken = set()
    for path in extra:
        with open(path, "rb") as f:
            content = f.read()
        base = os.path.basename(path)
        upper = base.upper()
        stem, _, ext = upper.partition(".")
        keep = "".join(c if (c.isalnum() or c in "-_") else "_" for c in stem)
        name83 = (keep[:8].ljust(8) + ext[:3].ljust(3))
        n = 1
        while name83 in taken:
            tail = "~%d" % n
            name83 = ((keep[:8 - len(tail)] + tail).ljust(8) + ext[:3].ljust(3))
            n += 1
        taken.add(name83)
        nclus = max(1, -(-len(content) // (spc * SECTOR)))
        c = alloc_chain(nclus)
        write_clusters(c, content)
        if base.upper() != (name83[:8].strip() + "." + name83[8:].strip()):
            root += lfn_entries(base, name83)
        root += dirent(name83, 0x20, c, len(content))
        print("  %s -> %s (%d bytes)" % (base, name83, len(content)))

    """
    The root directory is a chain like any other file's on FAT32, and it
    starts at cluster 2 because the volume's own record says so. It is
    written last: the clusters past the first come after everything
    already placed, so the directory grows without treading on the files
    it names.
    """
    csize = spc * SECTOR
    nclus = max(1, -(-len(root) // csize))
    if nclus > 1:
        extra = alloc_chain(nclus - 1)
        set_fat(2, extra)
        for i in range(nclus):
            c = 2 if i == 0 else extra + i - 1
            off = (data_start + (c - 2) * spc) * SECTOR
            chunk = bytes(root)[i * csize:(i + 1) * csize]
            img[off:off + len(chunk)] = chunk
    else:
        write_clusters(2, bytes(root))

    for i in range(nfats):
        off = (start + rsvd + i * fatsz) * SECTOR
        img[off:off + len(fat)] = fat

    # FSInfo: every cluster from next_free on is free, as fsck.fat counts
    # them (the clusters of the data area, not the spare entries at the
    # end of the table's last sector)
    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<II", fsinfo, 488, clusters + 2 - next_free[0], next_free[0])
    struct.pack_into("<H", fsinfo, 510, 0xAA55)
    img[(start + 1) * SECTOR:(start + 2) * SECTOR] = fsinfo
    return clusters


# ---------------------------------------------------------------- checking

class Volume:
    def __init__(self, img, start, nsect):
        self.img = img
        self.start = start
        b = img[start * SECTOR:(start + 1) * SECTOR]
        self.bps, self.spc, self.rsvd, self.nfats, self.root_ents, tot16, self.media, fat16 = \
            struct.unpack_from("<HBHBHHBH", b, 11)
        tot32, = struct.unpack_from("<I", b, 32)
        fat32sz, = struct.unpack_from("<I", b, 36)
        self.tot = tot16 or tot32
        self.fatsz = fat16 or fat32sz
        if self.bps != SECTOR or self.spc == 0 or self.nfats == 0 or self.fatsz == 0:
            raise ValueError("not a FAT boot sector")
        self.root_sects = (self.root_ents * 32 + SECTOR - 1) // SECTOR
        self.root_start = self.rsvd + self.nfats * self.fatsz
        self.data_start = self.root_start + self.root_sects
        self.nclus = (self.tot - self.data_start) // self.spc
        self.bits = 12 if self.nclus < FAT16_MIN_CLUS else 16 if self.nclus < FAT32_MIN_CLUS else 32
        self.root_clus = struct.unpack_from("<I", b, 44)[0] if self.bits == 32 else 0
        self.eoc = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]
        self.bad = self.eoc - 1
        self.csize = self.spc * SECTOR
        self.fats = [self.read_fat(i) for i in range(self.nfats)]

    def sect(self, n):
        o = (self.start + n) * SECTOR
        return self.img[o:o + SECTOR]

    def read_fat(self, i):
        o = (self.start + self.rsvd + i * self.fatsz) * SECTOR
        raw = self.img[o:o + self.fatsz * SECTOR]
        out = []
        for c in range(self.nclus + 2):
            if self.bits == 12:
                off = c + c // 2
                w = raw[off] | (raw[off + 1] << 8)
                out.append(w >> 4 if c & 1 else w & 0xFFF)
            elif self.bits == 16:
                out.append(struct.unpack_from("<H", raw, c * 2)[0])
            else:
                out.append(struct.unpack_from("<I", raw, c * 4)[0] & 0x0FFFFFFF)
        return out

    def clus_data(self, c):
        o = (self.start + self.data_start + (c - 2) * self.spc) * SECTOR
        return self.img[o:o + self.csize]


def check_volume(img, start, nsect, name, verbose):
    errs = []
    try:
        v = Volume(img, start, nsect)
    except (ValueError, struct.error, IndexError) as e:
        return ["%s: %s" % (name, e)], None
    fat = v.fats[0]
    for i in range(1, v.nfats):
        for c in range(v.nclus + 2):
            if v.fats[i][c] != fat[c]:
                errs.append("%s: FAT copy %d differs at entry %d (%#x, %#x)"
                            % (name, i + 1, c, fat[c], v.fats[i][c]))
                break
    if (fat[0] & 0xFF) != v.media:
        errs.append("%s: FAT entry 0 is %#x, the media byte is %#x" % (name, fat[0], v.media))

    owner = {}
    stats = {"files": 0, "dirs": 0}

    def follow(first, path):
        clist = []
        c = first
        while True:
            if c < 2 or c >= v.nclus + 2:
                errs.append("%s: %s: chain goes to %#x" % (name, path, c))
                break
            if c in owner:
                errs.append("%s: %s: cluster %d is also %s" % (name, path, c, owner[c]))
                break
            owner[c] = path
            clist.append(c)
            n = fat[c]
            if n >= v.eoc:
                break
            c = n
        return clist

    def read_dir(entries_bytes, path, self_clus, parent_clus):
        seen = set()
        lfn = []
        want = None
        k = 0
        for k in range(0, len(entries_bytes), 32):
            e = entries_bytes[k:k + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn, want = [], None
                continue
            if e[11] & 0x3F == 0x0F:
                seq = e[0] & 0x3F
                if e[0] & 0x40:
                    if lfn:
                        errs.append("%s: %s: long name cut short" % (name, path))
                    lfn, want = [(seq, e)], seq - 1
                elif want is not None and seq == want and seq >= 1:
                    lfn.append((seq, e))
                    want = seq - 1
                else:
                    errs.append("%s: %s: long name entry out of order" % (name, path))
                    lfn, want = [], None
                continue
            if e[11] & 0x08:
                lfn, want = [], None
                continue
            short = bytes(e[0:11])
            if e[12] & 0xC0:
                errs.append("%s: %s: %r still carries a rename mark (%#x)"
                            % (name, path, short, e[12]))
            longname = None
            if lfn:
                if want != 0 or any(le[13] != short_sum(short) for _, le in lfn):
                    errs.append("%s: %s: long name does not belong to %r" % (name, path, short))
                else:
                    units = []
                    for _, le in sorted(lfn, key=lambda t: t[0]):
                        for o in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                            units.append(le[o] | (le[o + 1] << 8))
                    if 0 in units:
                        units = units[:units.index(0)]
                    try:
                        longname = b"".join(struct.pack("<H", u) for u in units).decode("utf-16-le")
                    except UnicodeDecodeError:
                        errs.append("%s: %s: long name is not UTF-16" % (name, path))
                lfn, want = [], None
            base = short[0:8].decode("latin-1").rstrip()
            ext = short[8:11].decode("latin-1").rstrip()
            sname = base + ("." + ext if ext else "")
            attr = e[11]
            clus = struct.unpack_from("<H", e, 26)[0]
            if v.bits == 32:
                clus |= struct.unpack_from("<H", e, 20)[0] << 16
            size = struct.unpack_from("<I", e, 28)[0]
            if short in (b".          ", b"..         "):
                if self_clus is None:
                    errs.append("%s: %s: %s in the root" % (name, path, sname))
                want_c = self_clus if short[1] == 0x20 else parent_clus
                if clus != want_c:
                    errs.append("%s: %s/%s names cluster %d, not %d"
                                % (name, path, sname, clus, want_c))
                continue
            full = longname or sname
            key = full.upper()
            if key in seen or sname.upper() in seen:
                errs.append("%s: %s: %s is there twice" % (name, path, full))
            seen.add(key)
            seen.add(sname.upper())
            child = path + "/" + full
            if verbose:
                print("  %-40s %s %8d  cluster %d" % (child, "d" if attr & 0x10 else "-", size, clus))
            if attr & 0x10:
                stats["dirs"] += 1
                if clus == 0:
                    errs.append("%s: %s: a directory without a cluster" % (name, child))
                    continue
                clist = follow(clus, child)
                data = b"".join(v.clus_data(c) for c in clist)
                read_dir(data, child, clus, self_clus or 0)
            else:
                stats["files"] += 1
                if size == 0:
                    if clus != 0:
                        errs.append("%s: %s: empty, with cluster %d" % (name, child, clus))
                    continue
                clist = follow(clus, child)
                need = -(-size // v.csize)
                if len(clist) != need:
                    errs.append("%s: %s: %d bytes in %d clusters, not %d"
                                % (name, child, size, len(clist), need))

    if v.bits == 32:
        rl = follow(v.root_clus, "/")
        root = b"".join(v.clus_data(c) for c in rl)
    else:
        o = (start + v.root_start) * SECTOR
        root = v.img[o:o + v.root_sects * SECTOR]
    read_dir(root, "", None, 0)

    free = 0
    for c in range(2, v.nclus + 2):
        val = fat[c]
        if val == 0:
            free += 1
        elif val != v.bad and c not in owner:
            errs.append("%s: cluster %d is taken but no file has it" % (name, c))
            if sum(1 for x in errs if "no file has it" in x) > 20:
                break
    notes = []
    b = v.sect(0)
    if v.bits == 16 and not fat[1] & 0x8000 or v.bits == 32 and not fat[1] & 0x08000000             or v.bits == 12 and b[38] in (0x28, 0x29) and b[37] & 1:
        notes.append("marked in use (not unmounted)")
    if v.bits == 16 and not fat[1] & 0x4000 or v.bits == 32 and not fat[1] & 0x04000000:
        notes.append("marked with a disk error")
    if v.bits == 32:
        fsi = struct.unpack_from("<H", b, 48)[0]
        s = v.sect(fsi)
        if 0 < fsi < v.rsvd and struct.unpack_from("<I", s, 0)[0] == 0x41615252                 and struct.unpack_from("<I", s, 484)[0] == 0x61417272:
            cnt, nxt = struct.unpack_from("<II", s, 488)
            if cnt == 0xFFFFFFFF:
                notes.append("FSInfo free count unknown")
            elif cnt != free:
                errs.append("%s: FSInfo says %d free, not %d" % (name, cnt, free))
            if nxt != 0xFFFFFFFF and not 2 <= nxt < v.nclus + 2:
                errs.append("%s: FSInfo next free %#x is out of range" % (name, nxt))
    print("%s: FAT%d, %d clusters of %d bytes, %d free, %d files, %d directories, %s%s"
          % (name, v.bits, v.nclus, v.csize, free, stats["files"], stats["dirs"],
             "%d errors" % len(errs) if errs else "clean",
             "".join("; " + n for n in notes)))
    return errs, v


def volumes_of(img):
    """(name, start, sectors) of each FAT volume of a disk image."""
    nsect = len(img) // SECTOR
    if img[SECTOR:SECTOR + 8] == b"EFI PART":
        lba, n, size = struct.unpack_from("<QII", img, SECTOR + 72)
        out = []
        for i in range(n):
            o = lba * SECTOR + i * size
            t = img[o:o + 16]
            first, last = struct.unpack_from("<QQ", img, o + 32)
            if t == bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7"):
                out.append(("partition %d" % (i + 1), first, last - first + 1))
        return out
    if img[510:512] == b"\x55\xaa" and all(img[446 + i * 16] in (0, 0x80) for i in range(4)):
        out = []
        for i in range(4):
            e = img[446 + i * 16:462 + i * 16]
            t = e[4]
            s, n = struct.unpack_from("<II", e, 8)
            if t in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E) and n:
                out.append(("partition %d" % (i + 1), s, n))
        if out:
            return out
    return [("volume", 0, nsect)]


def check(paths, verbose):
    bad = 0
    for p in paths:
        with open(p, "rb") as f:
            img = f.read()
        print(p)
        for name, s, n in volumes_of(img):
            errs, _ = check_volume(img, s, n, name, verbose)
            for e in errs:
                print("  " + e)
            bad += len(errs)
    return 1 if bad else 0


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "make":
        floppy = args[1] == "floppy"
        bits = 12 if floppy else int(args[1])
        size_kb = 1440 if floppy else int(args[3]) if len(args) > 3 else             {12: 2048, 16: 6144, 32: 34816}[bits]
        nsect = size_kb * 1024 // SECTOR
        img = bytearray(nsect * SECTOR)
        if floppy:
            n = fat_format(img, 0, nsect, 12, spc=1, root_ents=224, big=True,
                           media=0xF0, spt=18, heads=2)
        elif bits == 12:
            n = fat_format(img, 0, nsect, 12, spc=1, root_ents=32, big=True)
        elif bits == 16:
            n = fat_format(img, 0, nsect, 16, spc=2, root_ents=64)
        else:
            n = fat32_format(img, 0, nsect)
        with open(args[2], "wb") as f:
            f.write(img)
        print("%s: FAT%d, %d clusters" % (args[2], bits, n))
        return 0
    if len(args) >= 2 and args[0] == "check":
        verbose = "-v" in args
        return check([a for a in args[1:] if a != "-v"], verbose)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
