"""Blocks of an image file, and the GPT partition a volume sits in.

A volume is a byte range of a file: an image of a whole disk with a
partition table, or a file that is nothing but the volume. BlockImage
hides the difference, so every other module counts blocks from the start
of the volume.
"""

import os
import struct
import uuid

from .error import TsfsError
from .layout import BLOCK_SIZE, SECTOR_SIZE

# The type GUID TessronOS gives its own partitions
TSFS_TYPE_GUID = uuid.UUID("54465331-0000-4000-8000-5446532d5631")

GPT_SIGNATURE = b"EFI PART"


class Partition(object):
    """One entry of a GPT partition table."""

    def __init__(self, index, type_guid, part_guid, first_lba, last_lba, name):
        self.index = index                      # 1 based, as tools name them
        self.type_guid = type_guid
        self.part_guid = part_guid
        self.first_lba = first_lba
        self.last_lba = last_lba
        self.name = name

    @property
    def offset(self):
        return self.first_lba * SECTOR_SIZE

    @property
    def length(self):
        return (self.last_lba - self.first_lba + 1) * SECTOR_SIZE

    def is_tsfs(self):
        return self.type_guid == TSFS_TYPE_GUID

    def __str__(self):
        return ("%d: %s %s lba %d..%d (%d MB)%s"
                % (self.index, self.type_guid, self.name,
                   self.first_lba, self.last_lba,
                   self.length // (1024 * 1024),
                   " [TSFS]" if self.is_tsfs() else ""))


def read_partitions(path):
    """The partitions of a GPT image. An empty list when the file carries
    no partition table."""
    with open(path, "rb") as f:
        f.seek(SECTOR_SIZE)
        head = f.read(SECTOR_SIZE)
        if len(head) < 92 or head[0:8] != GPT_SIGNATURE:
            return []
        entry_lba, count, size = struct.unpack_from("<QII", head, 72)
        if size < 128 or count == 0 or count > 4096:
            return []
        f.seek(entry_lba * SECTOR_SIZE)
        raw = f.read(count * size)

    parts = []
    for i in range(count):
        e = raw[i * size:(i + 1) * size]
        if len(e) < 128 or e[0:16] == b"\0" * 16:
            continue
        type_guid = uuid.UUID(bytes_le=bytes(e[0:16]))
        part_guid = uuid.UUID(bytes_le=bytes(e[16:32]))
        first, last = struct.unpack_from("<QQ", e, 32)
        name = bytes(e[56:128]).decode("utf-16-le").split("\0", 1)[0]
        parts.append(Partition(i + 1, type_guid, part_guid, first, last, name))
    return parts


def find_tsfs_partition(path):
    """The first partition whose type GUID is the TessronOS one, or None."""
    for p in read_partitions(path):
        if p.is_tsfs():
            return p
    return None


class BlockImage(object):
    """Blocks of 4096 bytes inside a byte range of a file."""

    def __init__(self, path, offset=0, length=None, writable=False,
                 create=False):
        self.path = path
        self.offset = offset
        self.writable = writable
        if create and not os.path.exists(path):
            if length is None:
                raise TsfsError("a new image needs a size")
            with open(path, "wb") as f:
                f.truncate(offset + length)
        size = os.path.getsize(path)
        if length is None:
            length = size - offset
        if offset + length > size:
            if not writable:
                raise TsfsError("the range ends past the file: %d > %d"
                               % (offset + length, size))
            with open(path, "r+b") as f:
                f.truncate(offset + length)
        if length < BLOCK_SIZE:
            raise TsfsError("the range holds no whole block")
        self.length = length
        self.nblk = length // BLOCK_SIZE
        self._f = open(path, "r+b" if writable else "rb")

    # ------------------------------------------------------------ blocks

    def read_block(self, blk):
        if blk < 0 or blk >= self.nblk:
            raise TsfsError("block %d is outside the volume (%d blocks)"
                           % (blk, self.nblk))
        self._f.seek(self.offset + blk * BLOCK_SIZE)
        buf = self._f.read(BLOCK_SIZE)
        if len(buf) != BLOCK_SIZE:
            raise TsfsError("block %d is short by %d bytes"
                           % (blk, BLOCK_SIZE - len(buf)))
        return buf

    def write_block(self, blk, buf):
        if not self.writable:
            raise TsfsError("the image was opened read only")
        if blk < 0 or blk >= self.nblk:
            raise TsfsError("block %d is outside the volume (%d blocks)"
                           % (blk, self.nblk))
        if len(buf) != BLOCK_SIZE:
            raise TsfsError("a block is %d bytes, not %d"
                           % (BLOCK_SIZE, len(buf)))
        self._f.seek(self.offset + blk * BLOCK_SIZE)
        self._f.write(bytes(buf))

    def flush(self):
        if self.writable:
            self._f.flush()
            os.fsync(self._f.fileno())

    def close(self):
        if self._f is not None:
            self.flush()
            self._f.close()
            self._f = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False
