"""The common head of a management block (design 11.6.4).

     0  kind, four bytes ("TFGH", "TFBT", "TFOB", "TFTB"), then four zero
     8  the block's own number
    16  the volume UUID
    32  whose it is (group number, tree number, low 64 bits of an object)
    40  the transaction that wrote it
    48  CRC32C of the whole block, taken with this field zero
    52  zero up to 64
"""

import struct

from .crc import crc32c
from .layout import BLOCK_SIZE, HDR_SIZE

HD_MAGIC = 0
HD_BLK = 8
HD_VOLUUID = 16
HD_OWNER = 32
HD_GEN = 40
HD_CRC = 48


def seal(buf, vol_uuid, gen, magic, blk, owner):
    """Fill the head of a block in place and answer it as bytes."""
    if len(buf) != BLOCK_SIZE:
        raise ValueError("a block is %d bytes" % BLOCK_SIZE)
    buf[0:4] = magic
    buf[4:8] = b"\0\0\0\0"
    struct.pack_into("<Q", buf, HD_BLK, blk)
    buf[HD_VOLUUID:HD_VOLUUID + 16] = vol_uuid.bytes
    struct.pack_into("<QQ", buf, HD_OWNER, owner, gen)
    buf[HD_CRC:HDR_SIZE] = b"\0" * (HDR_SIZE - HD_CRC)
    struct.pack_into("<I", buf, HD_CRC, crc32c(bytes(buf)))
    return bytes(buf)


def block_crc(buf):
    """The CRC a sealed block should carry."""
    tmp = bytearray(buf)
    tmp[HD_CRC:HD_CRC + 4] = b"\0\0\0\0"
    return crc32c(bytes(tmp))


def problem(buf, vol_uuid, magic, blk, owner=None):
    """What is wrong with the head of a block, or None when it passes the
    kernel's check (hd_good). The owner is looked at only when given;
    the kernel does not check it."""
    if bytes(buf[0:4]) != magic:
        return "kind %r, not %r" % (bytes(buf[0:4]), magic)
    here = struct.unpack_from("<Q", buf, HD_BLK)[0]
    if here != blk:
        return "it says it is block %d" % here
    if bytes(buf[HD_VOLUUID:HD_VOLUUID + 16]) != vol_uuid.bytes:
        return "it belongs to another volume"
    got = struct.unpack_from("<I", buf, HD_CRC)[0]
    want = block_crc(buf)
    if got != want:
        return "CRC 0x%08x, not 0x%08x" % (got, want)
    if owner is not None:
        have = struct.unpack_from("<Q", buf, HD_OWNER)[0]
        if have != owner:
            return "owner 0x%x, not 0x%x" % (have, owner)
    return None


def owner(buf):
    return struct.unpack_from("<Q", buf, HD_OWNER)[0]


def generation(buf):
    return struct.unpack_from("<Q", buf, HD_GEN)[0]
