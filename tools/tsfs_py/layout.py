"""The superblock of a version 4 volume and where everything else sits.

Layout of a volume (peripheral_kernel/fs/tsfsblk.c):

    block 0, 1      superblock A and B, written in turn
    2 ..            the journal area: a head block and a ring
    then            allocation groups 0, 1, 2 ...

A group starts with its head, then (groups 1 and the powers of 3, 5 and
7) a spare copy of the superblock, then its free map, one bit per block
of the group, then data. The map blocks carry no head of their own; the
group head holds a CRC of each.

Superblock fields, little endian:

      0  magic "TFSVOL02"            8  version (4)
     12  block size (4096)          16  generation
     24  volume UUID                40  label, 64 bytes, NUL padded
    112  total blocks              120  free blocks (sum of the groups)
    128  blocks of a group         136  number of groups
    144  journal start             152  journal blocks
    160  next transaction number
    176  object index root         184  its nodes
    192  garbage list root         200  its nodes
    208  orphan tree root          216  its nodes
    224  root object UUID          240  system box UUID
    256  protection domain UUID
    272  compat                    280  ro_compat           288  incompat
    296  state (32)                300  error kind (32)
    304  first error               312  last error          320  error block
    360  mount count               368  mount time
    376  fsck time                 384  mkfs time
   4092  CRC32C of bytes 0..4091
"""

import os
import struct
import time
import uuid

from .crc import crc32c
from .error import TsfsError

SECTOR_SIZE = 512
BLOCK_SIZE = 4096
SECT_PER_BLK = BLOCK_SIZE // SECTOR_SIZE

MAGIC = b"TFSVOL02"
VERSION = 4
LABEL_MAX = 64
MIN_BLOCKS = 1024

# The common head of a management block
HDR_SIZE = 64
MAGIC_AGH = b"TFGH"
MAGIC_NODE = b"TFBT"
MAGIC_OBJ = b"TFOB"
MAGIC_TBL = b"TFTB"

# State bits
ST_CLEAN = 0x0001
ST_ERROR = 0x0002
ST_NOFLUSH = 0x0004

# What went wrong, as the superblock records it
ERR_CSUM = 1
ERR_IO = 2
ERR_TREE = 3
ERR_NAMES = {0: "none", ERR_CSUM: "checksum", ERR_IO: "i/o", ERR_TREE: "tree"}

KNOWN_COMPAT = 0
KNOWN_ROCOMPAT = 0
KNOWN_INCOMPAT = 0

# Geometry
AG_MAX_BLOCKS = 1 << 18
AG_MIN_BLOCKS = 1024
AG_MIN_TAIL = 64
BITS_PER_MAP = BLOCK_SIZE * 8
AG_MAX_MAP = AG_MAX_BLOCKS // BITS_PER_MAP
JRNL_MAX_BLOCKS = 1 << 18

# The journal's smallest area: a head and room for two of the largest
# transactions (tsfsjrnl.h)
JRNL_HDR_SIZE = 32
JRNL_DESC_ENT = 16
JRNL_MAX_BLOCKS_TXN = (BLOCK_SIZE - JRNL_HDR_SIZE) // JRNL_DESC_ENT
JRNL_MAX_REVOKE = (BLOCK_SIZE - JRNL_HDR_SIZE) // 8
JRNL_TXN_MAX_FOOT = JRNL_MAX_BLOCKS_TXN + 3
JRNL_AREA_BLOCKS = 1 + 2 * JRNL_TXN_MAX_FOOT

# Superblock offsets
SB_MAGIC = 0
SB_VERSION = 8
SB_BSIZE = 12
SB_GEN = 16
SB_VOLUUID = 24
SB_LABEL = 40
SB_TOTAL = 112
SB_FREE = 120
SB_AGBLOCKS = 128
SB_AGCOUNT = 136
SB_JSTART = 144
SB_JBLOCKS = 152
SB_JSEQ = 160
SB_OBJROOT = 176
SB_OBJNODES = 184
SB_GCROOT = 192
SB_GCNODES = 200
SB_ORROOT = 208
SB_ORNODES = 216
SB_ROOTUUID = 224
SB_SYSUUID = 240
SB_DOMUUID = 256
SB_COMPAT = 272
SB_ROCOMPAT = 280
SB_INCOMPAT = 288
SB_STATE = 296
SB_ERRKIND = 300
SB_ERRFIRST = 304
SB_ERRLAST = 312
SB_ERRBLK = 320
SB_MOUNTS = 360
SB_MOUNTTIME = 368
SB_FSCKTIME = 376
SB_MKFSTIME = 384
SB_CRC = 4092

# 1970-01-01 to 1985-01-01 in seconds: the volume counts time from 1985,
# the origin of the T-Kernel system time
TRON_EPOCH = 473385600

NIL_UUID = uuid.UUID(int=0)


# ---------------------------------------------------------------- time

def now_tron():
    """Seconds since 1985-01-01 UTC, as the kernel's TS_TIME counts."""
    return int(time.time()) - TRON_EPOCH


def now_ns():
    """Nanoseconds since 1985-01-01 UTC, whole seconds as the kernel's
    clock gives them."""
    return now_tron() * 1000000000


def tron_to_text(t):
    if not t:
        return "never"
    return time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(t + TRON_EPOCH))


# ---------------------------------------------------------------- UUIDs

def gen_uuid7():
    """A version 7 UUID: 48 bits of Unix time in milliseconds first, so
    byte order is creation order."""
    ms = int(time.time() * 1000) & 0xFFFFFFFFFFFF
    rnd = os.urandom(10)
    b = bytearray(16)
    for i in range(6):
        b[i] = (ms >> (40 - 8 * i)) & 0xFF
    b[6] = 0x70 | (rnd[0] & 0x0F)
    b[7] = rnd[1]
    b[8] = 0x80 | (rnd[2] & 0x3F)
    b[9:16] = rnd[3:10]
    return uuid.UUID(bytes=bytes(b))


def owner_of(obj_uuid):
    """The owner field of an object's blocks: the low 64 bits of its
    UUID, read little endian from bytes 8..15 as the kernel reads them."""
    return struct.unpack_from("<Q", obj_uuid.bytes, 8)[0]


def parse_uuid(text):
    """A UUID from its text form, or None."""
    try:
        u = uuid.UUID(text)
    except (ValueError, TypeError, AttributeError):
        return None
    if len(text) != 36:
        return None
    return u


# ---------------------------------------------------------------- geometry

def is_spare(n):
    """Whether group n keeps a spare superblock: 1 and the powers of 3, 5
    and 7."""
    if n == 1:
        return True
    for b in (3, 5, 7):
        p = b
        while p <= n:
            if p == n:
                return True
            p *= b
    return False


def layout(nblk):
    """(journal blocks, blocks of a group, groups) for a volume of nblk
    blocks. The journal is a 256th of the volume, no less than two of the
    largest transactions and no more than a gigabyte; a group is the
    largest power of two up to a gigabyte that still makes four of them,
    and a last group shorter than AG_MIN_TAIL is left out."""
    j = nblk // 256
    if j < JRNL_AREA_BLOCKS:
        j = JRNL_AREA_BLOCKS
    if j > JRNL_MAX_BLOCKS:
        j = JRNL_MAX_BLOCKS
    first = 2 + j
    if nblk < MIN_BLOCKS or first + AG_MIN_TAIL >= nblk:
        raise TsfsError("%d blocks is too small for a volume" % nblk)
    data = nblk - first
    agb = AG_MAX_BLOCKS
    while agb > AG_MIN_BLOCKS and data // agb < 4:
        agb >>= 1
    n = data // agb
    if data % agb >= AG_MIN_TAIL:
        n += 1
    return j, agb, n


class GroupShape(object):
    """Where group n lies and what it uses itself."""

    __slots__ = ("n", "first", "len", "spare", "map_first", "map_blocks")

    def __init__(self, ag_first, agb, data_end, n):
        self.n = n
        self.first = ag_first + n * agb
        end = min(self.first + agb, data_end)
        self.len = end - self.first
        self.spare = is_spare(n)
        self.map_first = self.first + 1 + (1 if self.spare else 0)
        self.map_blocks = (self.len + BITS_PER_MAP - 1) // BITS_PER_MAP

    @property
    def meta(self):
        """Blocks at the front of the group that the group uses itself."""
        return (self.map_first - self.first) + self.map_blocks

    @property
    def data_first(self):
        return self.first + self.meta

    @property
    def end(self):
        return self.first + self.len

    @property
    def spare_blk(self):
        return self.first + 1 if self.spare else None


# ---------------------------------------------------------------- superblock

class Superblock(object):
    """The fields of a superblock, as Python values."""

    def __init__(self):
        self.magic = MAGIC
        self.version = VERSION
        self.block_size = BLOCK_SIZE
        self.generation = 0
        self.vol_uuid = NIL_UUID
        self.label = ""
        self.label_raw = b"\0" * LABEL_MAX
        self.total_blocks = 0
        self.free_blocks = 0
        self.ag_blocks = 0
        self.ag_count = 0
        self.journal_start = 2
        self.journal_blocks = 0
        self.journal_seq = 1
        self.objtbl_start = 0
        self.objtbl_blocks = 0
        self.gclist_start = 0
        self.gclist_blocks = 0
        self.orphan_start = 0
        self.orphan_blocks = 0
        self.root_uuid = NIL_UUID
        self.sysbox_uuid = NIL_UUID
        self.domain_uuid = NIL_UUID
        self.compat = 0
        self.ro_compat = 0
        self.incompat = 0
        self.state = 0
        self.err_kind = 0
        self.err_first = 0
        self.err_last = 0
        self.err_blk = 0
        self.mount_count = 0
        self.mount_time = 0
        self.fsck_time = 0
        self.mkfs_time = 0

    def copy(self):
        sb = Superblock()
        sb.__dict__.update(self.__dict__)
        return sb

    def set_label(self, label):
        raw = (label or "").encode("utf-8")[:LABEL_MAX - 1]
        self.label_raw = raw + b"\0" * (LABEL_MAX - len(raw))
        self.label = raw.decode("utf-8", "replace")

    # ------------------------------------------------------------ bytes

    def pack(self):
        buf = bytearray(BLOCK_SIZE)
        buf[SB_MAGIC:SB_MAGIC + 8] = MAGIC
        struct.pack_into("<IIQ", buf, SB_VERSION, self.version,
                         self.block_size, self.generation)
        buf[SB_VOLUUID:SB_VOLUUID + 16] = self.vol_uuid.bytes
        buf[SB_LABEL:SB_LABEL + LABEL_MAX] = self.label_raw
        struct.pack_into("<QQQQQQQ", buf, SB_TOTAL,
                         self.total_blocks, self.free_blocks, self.ag_blocks,
                         self.ag_count, self.journal_start,
                         self.journal_blocks, self.journal_seq)
        struct.pack_into("<QQQQQQ", buf, SB_OBJROOT,
                         self.objtbl_start, self.objtbl_blocks,
                         self.gclist_start, self.gclist_blocks,
                         self.orphan_start, self.orphan_blocks)
        buf[SB_ROOTUUID:SB_ROOTUUID + 16] = self.root_uuid.bytes
        buf[SB_SYSUUID:SB_SYSUUID + 16] = self.sysbox_uuid.bytes
        buf[SB_DOMUUID:SB_DOMUUID + 16] = self.domain_uuid.bytes
        struct.pack_into("<QQQ", buf, SB_COMPAT,
                         self.compat, self.ro_compat, self.incompat)
        struct.pack_into("<IIQQQ", buf, SB_STATE, self.state, self.err_kind,
                         self.err_first, self.err_last, self.err_blk)
        struct.pack_into("<QQQQ", buf, SB_MOUNTS, self.mount_count,
                         self.mount_time, self.fsck_time, self.mkfs_time)
        struct.pack_into("<I", buf, SB_CRC, crc32c(bytes(buf[:SB_CRC])))
        return bytes(buf)

    @classmethod
    def unpack(cls, buf):
        """The fields of a superblock block. Raises TsfsError for anything
        the kernel's sb_unpack refuses."""
        buf = bytes(buf)
        if buf[SB_MAGIC:SB_MAGIC + 8] != MAGIC:
            if buf[0:7] == b"TFSVOL0":
                raise TsfsError("a superblock of another format (%r)"
                               % buf[0:8])
            raise TsfsError("no superblock (magic %r)" % buf[0:8])
        got = struct.unpack_from("<I", buf, SB_CRC)[0]
        want = crc32c(buf[:SB_CRC])
        if got != want:
            raise TsfsError("the CRC is 0x%08x, not 0x%08x" % (got, want))
        sb = cls()
        sb.version, sb.block_size, sb.generation = struct.unpack_from(
            "<IIQ", buf, SB_VERSION)
        sb.vol_uuid = uuid.UUID(bytes=buf[SB_VOLUUID:SB_VOLUUID + 16])
        sb.label_raw = buf[SB_LABEL:SB_LABEL + LABEL_MAX]
        sb.label = sb.label_raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        (sb.total_blocks, sb.free_blocks, sb.ag_blocks, sb.ag_count,
         sb.journal_start, sb.journal_blocks,
         sb.journal_seq) = struct.unpack_from("<QQQQQQQ", buf, SB_TOTAL)
        (sb.objtbl_start, sb.objtbl_blocks, sb.gclist_start, gcn,
         sb.orphan_start, orn) = struct.unpack_from("<QQQQQQ", buf, SB_OBJROOT)
        sb.gclist_blocks = gcn & 0xFFFFFFFF  # the kernel holds these in 32 bits
        sb.orphan_blocks = orn & 0xFFFFFFFF
        sb.root_uuid = uuid.UUID(bytes=buf[SB_ROOTUUID:SB_ROOTUUID + 16])
        sb.sysbox_uuid = uuid.UUID(bytes=buf[SB_SYSUUID:SB_SYSUUID + 16])
        sb.domain_uuid = uuid.UUID(bytes=buf[SB_DOMUUID:SB_DOMUUID + 16])
        sb.compat, sb.ro_compat, sb.incompat = struct.unpack_from(
            "<QQQ", buf, SB_COMPAT)
        (sb.state, sb.err_kind, sb.err_first, sb.err_last,
         sb.err_blk) = struct.unpack_from("<IIQQQ", buf, SB_STATE)
        (sb.mount_count, sb.mount_time, sb.fsck_time,
         sb.mkfs_time) = struct.unpack_from("<QQQQ", buf, SB_MOUNTS)
        if sb.version != VERSION or sb.block_size != BLOCK_SIZE:
            raise TsfsError("version %d with blocks of %d: this reads version "
                           "%d with blocks of %d"
                           % (sb.version, sb.block_size, VERSION, BLOCK_SIZE))
        if sb.ag_blocks == 0 or sb.ag_count == 0 or sb.total_blocks < MIN_BLOCKS:
            raise TsfsError("the geometry is impossible (%d groups of %d, %d "
                           "blocks)" % (sb.ag_count, sb.ag_blocks,
                                        sb.total_blocks))
        return sb

    # ------------------------------------------------------------ geometry

    @property
    def ag_first(self):
        return self.journal_start + self.journal_blocks

    @property
    def data_end(self):
        return min(self.ag_first + self.ag_count * self.ag_blocks,
                   self.total_blocks)

    def group(self, n):
        return GroupShape(self.ag_first, self.ag_blocks, self.data_end, n)

    def groups(self):
        return [self.group(n) for n in range(self.ag_count)]

    def group_of(self, blk):
        """The group a block is in, or None."""
        if blk < self.ag_first or blk >= self.data_end:
            return None
        n = (blk - self.ag_first) // self.ag_blocks
        return n if n < self.ag_count else None

    def roots(self):
        """The objects the superblock names, as (field, UUID)."""
        out = []
        for name, u in (("root", self.root_uuid), ("sysbox", self.sysbox_uuid),
                        ("domain", self.domain_uuid)):
            if u != NIL_UUID:
                out.append((name, u))
        return out

    def summary(self):
        return {
            "label": self.label,
            "uuid": str(self.vol_uuid),
            "generation": self.generation,
            "total_blocks": self.total_blocks,
            "free_blocks": self.free_blocks,
            "ag_blocks": self.ag_blocks,
            "ag_count": self.ag_count,
            "journal": [self.journal_start, self.journal_blocks,
                        self.journal_seq],
            "index": [self.objtbl_start, self.objtbl_blocks],
            "garbage": [self.gclist_start, self.gclist_blocks],
            "orphans": [self.orphan_start, self.orphan_blocks],
            "root_uuid": str(self.root_uuid),
            "sysbox_uuid": str(self.sysbox_uuid),
            "domain_uuid": str(self.domain_uuid),
            "state": self.state,
            "err_kind": self.err_kind,
            "err_blk": self.err_blk,
            "mount_count": self.mount_count,
            "mkfs_time": self.mkfs_time,
        }

    def __str__(self):
        flags = []
        if self.state & ST_CLEAN:
            flags.append("clean")
        if self.state & ST_ERROR:
            flags.append("error(%s at %d)" % (ERR_NAMES.get(self.err_kind, "?"),
                                              self.err_blk))
        if self.state & ST_NOFLUSH:
            flags.append("noflush")
        return ("label %r uuid %s generation %d\n"
                "  blocks %d, free %d, %d groups of %d\n"
                "  journal %d+%d next transaction %d\n"
                "  index root %d (%d nodes), garbage list root %d (%d nodes), "
                "orphan tree root %d (%d nodes)\n"
                "  root %s, system box %s, domain %s\n"
                "  state %s, mounts %d, made %s"
                % (self.label, self.vol_uuid, self.generation,
                   self.total_blocks, self.free_blocks, self.ag_count,
                   self.ag_blocks, self.journal_start, self.journal_blocks,
                   self.journal_seq, self.objtbl_start, self.objtbl_blocks,
                   self.gclist_start, self.gclist_blocks, self.orphan_start,
                   self.orphan_blocks, self.root_uuid,
                   self.sysbox_uuid, self.domain_uuid,
                   ",".join(flags) or "in use", self.mount_count,
                   tron_to_text(self.mkfs_time)))
