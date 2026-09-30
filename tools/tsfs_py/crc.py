"""CRC32C, the checksum of every block the volume checks.

The kernel computes it with the AArch64 crc32c instructions; the table
here gives the same value for the same bytes.
"""

import struct

_POLY = 0x82F63B78                      # Castagnoli, reflected


def _make_table():
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ _POLY if c & 1 else c >> 1
        table.append(c)
    return tuple(table)


_TABLE = _make_table()


def crc32c(data):
    """CRC32C with init and final xor of all ones. "123456789" gives
    0xE3069283."""
    c = 0xFFFFFFFF
    t = _TABLE
    for b in bytes(data):
        c = t[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def crc32c_u32(value):
    """The CRC of a 32 bit value as it lies in the kernel's memory, little
    endian: what the journal chains its block checksums with."""
    return crc32c(struct.pack("<I", value & 0xFFFFFFFF))
