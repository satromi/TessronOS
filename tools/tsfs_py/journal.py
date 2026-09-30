"""The write ahead log of a volume (design 11.14.1).

The journal area is a head block followed by a ring of the rest:

    head block   "TFJH", version (32 at 4), the number of the oldest
                 transaction not yet put in place (64 at 8), where in the
                 ring it starts (64 at 16), CRC (32 at 24)

One transaction in the ring:

    descriptor   "TFJD", version, number (64 at 8), count (32 at 16),
                 CRC (32 at 24), then from 32 one entry of 16 bytes per
                 copy: the block it belongs in (64), the plain CRC32C of
                 the copy (32), spare (32)
    copies       the blocks as the transaction left them
    revoke       "TFJR", as the descriptor, entries of 8 bytes: blocks
                 freed by the transaction whose older copies must not
                 come back (only when there are any)
    commit       "TFJC", time (64 at 32), chain CRC (32 at 40), blocks of
                 the transaction before it (32 at 44)

The CRCs of the head, descriptor, revoke and commit blocks are taken with
their own field zero and XORed with the volume's salt:
crc32c(volume UUID) ^ the low 32 bits of the time it was made. The chain
CRC starts from the CRC of the descriptor's own CRC value, and takes in
each copy's CRC (plus its index) and the revoke block's CRC (plus
0x5A5A5A5A), each as the CRC of the four bytes of the value.
"""

import struct
import time

from .crc import crc32c, crc32c_u32
from .error import TsfsError
from .layout import (BLOCK_SIZE, JRNL_AREA_BLOCKS, JRNL_DESC_ENT,
                     JRNL_HDR_SIZE, JRNL_MAX_BLOCKS_TXN, JRNL_MAX_REVOKE,
                     TRON_EPOCH)

VERSION = 2

JH_MAGIC = b"TFJH"
JB_DESC = b"TFJD"
JB_REVOKE = b"TFJR"
JB_COMMIT = b"TFJC"

JH_SEQ = 8
JH_POS = 16
JH_CRC = 24

JB_SEQ = 8
JB_COUNT = 16
JB_CRC = 24
JB_ENT = JRNL_HDR_SIZE
JC_TIME = 32
JC_CHAIN = 40
JC_BLOCKS = 44

REPLAY_MAX_REVOKE = 4096
REVOKE_SALT = 0x5A5A5A5A


def salt_of(sb):
    return crc32c(sb.vol_uuid.bytes) ^ (sb.mkfs_time & 0xFFFFFFFF)


class Transaction(object):
    """One transaction the scan found good."""

    def __init__(self, seq, pos, homes):
        self.seq = seq
        self.pos = pos                  # of its descriptor in the ring
        self.homes = homes              # where each copy belongs
        self.revokes = []

    def __repr__(self):
        return "<txn %d at %d: %d blocks, %d revoked>" % (
            self.seq, self.pos, len(self.homes), len(self.revokes))


class Journal(object):
    """The journal of a volume. It reads and writes the device through
    raw_read / raw_write, past any overlay the volume keeps."""

    def __init__(self, vol):
        self.vol = vol
        sb = vol.sb
        self.start = sb.journal_start
        self.blocks = sb.journal_blocks
        self.len = self.blocks - 1
        self.salt = salt_of(sb)
        self.seq_sb = sb.journal_seq
        # what the scan found
        self.head_state = None          # "good", "empty", "damaged"
        self.head = 0
        self.head_seq = sb.journal_seq
        self.txns = []
        self.stop_reason = None
        self.good_end = 0
        self.next_seq = sb.journal_seq

    # ------------------------------------------------------------ helpers

    def usable(self):
        return self.start != 0 and self.blocks >= JRNL_AREA_BLOCKS

    def ring_blk(self, pos):
        return self.start + 1 + (pos % self.len)

    def crc_at(self, buf, off):
        tmp = bytearray(buf)
        tmp[off:off + 4] = b"\0\0\0\0"
        return crc32c(bytes(tmp)) ^ self.salt

    @staticmethod
    def is_kind(buf, magic):
        return (bytes(buf[0:4]) == magic
                and struct.unpack_from("<I", buf, 4)[0] == VERSION)

    @staticmethod
    def make_kind(magic, seq, count):
        buf = bytearray(BLOCK_SIZE)
        buf[0:4] = magic
        struct.pack_into("<IQI", buf, 4, VERSION, seq, count)
        return buf

    def _read(self, blk):
        return self.vol.raw_read(blk)

    # ------------------------------------------------------------ head

    def read_head(self):
        """Look at the head block; answers its state."""
        buf = self._read(self.start)
        if (bytes(buf[0:4]) == JH_MAGIC
                and struct.unpack_from("<I", buf, 4)[0] == VERSION
                and struct.unpack_from("<I", buf, JH_CRC)[0]
                == self.crc_at(buf, JH_CRC)
                and struct.unpack_from("<Q", buf, JH_POS)[0] < self.len):
            self.head_seq, self.head = struct.unpack_from("<QQ", buf, JH_SEQ)
            self.head_state = "good"
        else:
            self.head = 0
            self.head_seq = self.seq_sb
            self.head_state = "empty" if buf == bytes(BLOCK_SIZE) else "damaged"
        return self.head_state

    def pack_head(self, head_seq, pos):
        buf = bytearray(BLOCK_SIZE)
        buf[0:4] = JH_MAGIC
        struct.pack_into("<IQQ", buf, 4, VERSION, head_seq, pos)
        struct.pack_into("<I", buf, JH_CRC, self.crc_at(buf, JH_CRC))
        return bytes(buf)

    def write_head(self, head_seq, pos):
        self.vol.raw_write(self.start, self.pack_head(head_seq, pos))
        self.head_seq = head_seq
        self.head = pos
        self.head_state = "good"

    # ------------------------------------------------------------ scan

    def scan(self):
        """Find the transactions that became real, from the head on, by
        the kernel's rules (ts_jrnl_replay): each has to carry the next
        number, pass its CRCs and end in a commit whose chain agrees. The
        first that does not stops the scan; its revokes do not count.
        Answers the list of transactions."""
        self.txns = []
        self.stop_reason = None
        if not self.usable():
            self.head_state = "none"
            return self.txns
        if self.read_head() != "good":
            self.good_end = 0
            self.next_seq = self.seq_sb
            return self.txns

        pos = self.head
        cur = self.head_seq
        good_end = pos
        scanned = 0
        maxtx = self.len // 2 + 1
        in_tx = False
        chain = 0
        tx = None
        nrvk = 0
        while scanned < self.len and len(self.txns) < maxtx:
            buf = self._read(self.ring_blk(pos))
            seq = struct.unpack_from("<Q", buf, JB_SEQ)[0]
            if seq != cur:
                self.stop_reason = ("block %d of the ring carries number %d, "
                                    "not %d" % (pos % self.len, seq, cur))
                break
            if struct.unpack_from("<I", buf, JB_CRC)[0] != self.crc_at(buf, JB_CRC):
                self.stop_reason = ("block %d of the ring fails its CRC"
                                    % (pos % self.len))
                break
            if not in_tx and self.is_kind(buf, JB_DESC):
                n = struct.unpack_from("<I", buf, JB_COUNT)[0]
                if n > JRNL_MAX_BLOCKS_TXN:
                    self.stop_reason = "a descriptor names %d blocks" % n
                    break
                c = struct.unpack_from("<I", buf, JB_CRC)[0]
                chain = crc32c_u32(c)
                homes = []
                ok = True
                for i in range(n):
                    ent = JB_ENT + i * JRNL_DESC_ENT
                    home, want = struct.unpack_from("<QI", buf, ent)
                    copy = self._read(self.ring_blk(pos + 1 + i))
                    c = crc32c(copy)
                    if c != want:
                        ok = False
                        self.stop_reason = ("copy %d of transaction %d did not "
                                            "reach the medium" % (i, cur))
                        break
                    chain ^= (crc32c_u32(c) + i) & 0xFFFFFFFF
                    homes.append(home)
                if not ok:
                    break
                tx = Transaction(cur, pos, homes)
                pos += 1 + n
                scanned += 1 + n
                in_tx = True
                continue
            if in_tx and self.is_kind(buf, JB_REVOKE):
                n = struct.unpack_from("<I", buf, JB_COUNT)[0]
                if n > JRNL_MAX_REVOKE or nrvk + n > REPLAY_MAX_REVOKE:
                    self.stop_reason = "a revoke block names %d blocks" % n
                    break
                tx.revokes.extend(struct.unpack_from("<Q", buf, JB_ENT + 8 * i)[0]
                                  for i in range(n))
                nrvk += n
                c = struct.unpack_from("<I", buf, JB_CRC)[0]
                chain ^= (crc32c_u32(c) + REVOKE_SALT) & 0xFFFFFFFF
                pos += 1
                scanned += 1
                continue
            if in_tx and self.is_kind(buf, JB_COMMIT):
                if struct.unpack_from("<I", buf, JC_CHAIN)[0] != chain:
                    self.stop_reason = ("the commit of transaction %d does not "
                                        "match its chain" % cur)
                    break
                self.txns.append(tx)
                tx = None
                cur += 1
                pos += 1
                scanned += 1
                good_end = pos
                in_tx = False
                continue
            self.stop_reason = ("block %d of the ring is not what comes next"
                                % (pos % self.len))
            break
        if tx is not None and self.stop_reason is not None:
            self.stop_reason += " (transaction %d is not complete)" % tx.seq
        self.good_end = good_end
        self.next_seq = cur
        return self.txns

    def pending(self):
        return len(self.txns)

    def blocks_to_replay(self):
        """(home, ring position of the copy) in the order they go back,
        leaving out a copy that the same or a later transaction revoked."""
        revoked = []
        for t in self.txns:
            for b in t.revokes:
                revoked.append((b, t.seq))
        out = []
        for t in self.txns:
            for i, home in enumerate(t.homes):
                if any(b == home and s >= t.seq for (b, s) in revoked):
                    continue
                out.append((home, t.pos + 1 + i))
        return out

    def overlay(self):
        """{home: bytes} as a replay would leave the blocks."""
        over = {}
        for home, rpos in self.blocks_to_replay():
            over[home] = self._read(self.ring_blk(rpos))
        return over

    def replay(self):
        """Put every good transaction in place, then move the head past
        them with an empty ring. Answers the number of blocks written."""
        if not self.usable():
            return 0
        self.scan()
        if self.head_state != "good":
            self.write_head(self.seq_sb, 0)
            self.next_seq = self.seq_sb
            return 0
        n = 0
        for home, rpos in self.blocks_to_replay():
            self.vol.raw_write(home, self._read(self.ring_blk(rpos)))
            n += 1
        self.write_head(self.next_seq, self.good_end % self.len)
        self.txns = []
        return n

    # ------------------------------------------------------------ writing

    def commit(self, blocks, revokes=(), seq=None, pos=None, when=None,
               write_commit=True):
        """Put one transaction into the ring as the kernel's ring_write
        does: `blocks` is a list of (home, 4096 bytes). By default it goes
        where the head says the next one starts, with the number the head
        says. Nothing is put in place. Answers the number of ring blocks it
        took. With write_commit False the commit block is left out, as a
        machine that lost its power just before it would leave the ring."""
        if not self.usable():
            raise TsfsError("the volume has no journal area")
        if self.head_state is None:
            self.read_head()
        if self.head_state != "good":
            self.write_head(self.seq_sb, 0)
        if len(blocks) > JRNL_MAX_BLOCKS_TXN or len(revokes) > JRNL_MAX_REVOKE:
            raise TsfsError("too much for one transaction")
        seq = self.head_seq + len(self.txns) if seq is None else seq
        if pos is None:
            pos = self.good_end if self.txns else self.head
        foot = 1 + len(blocks) + (1 if revokes else 0) + 1
        if foot > self.len:
            raise TsfsError("the ring is too small")

        desc = self.make_kind(JB_DESC, seq, len(blocks))
        crcs = []
        for i, (home, data) in enumerate(blocks):
            if len(data) != BLOCK_SIZE:
                raise TsfsError("a copy is %d bytes" % len(data))
            c = crc32c(data)
            crcs.append(c)
            struct.pack_into("<QI", desc, JB_ENT + i * JRNL_DESC_ENT, home, c)
        c = self.crc_at(desc, JB_CRC)
        struct.pack_into("<I", desc, JB_CRC, c)
        chain = crc32c_u32(c)
        p = pos
        self.vol.raw_write(self.ring_blk(p), bytes(desc))
        p += 1
        for i, (home, data) in enumerate(blocks):
            chain ^= (crc32c_u32(crcs[i]) + i) & 0xFFFFFFFF
            self.vol.raw_write(self.ring_blk(p), bytes(data))
            p += 1
        if revokes:
            rv = self.make_kind(JB_REVOKE, seq, len(revokes))
            for i, b in enumerate(revokes):
                struct.pack_into("<Q", rv, JB_ENT + 8 * i, b)
            c = self.crc_at(rv, JB_CRC)
            struct.pack_into("<I", rv, JB_CRC, c)
            chain ^= (crc32c_u32(c) + REVOKE_SALT) & 0xFFFFFFFF
            self.vol.raw_write(self.ring_blk(p), bytes(rv))
            p += 1
        if write_commit:
            cm = self.make_kind(JB_COMMIT, seq, 0)
            t = int(time.time()) - TRON_EPOCH if when is None else when
            struct.pack_into("<QII", cm, JC_TIME, t, chain, foot - 1)
            struct.pack_into("<I", cm, JB_CRC, self.crc_at(cm, JB_CRC))
            self.vol.raw_write(self.ring_blk(p), bytes(cm))
            t = Transaction(seq, pos, [home for (home, _d) in blocks])
            t.revokes = list(revokes)
            self.txns.append(t)
            self.good_end = pos + foot
            self.next_seq = seq + 1
        return foot

    def summary(self):
        return {
            "start": self.start,
            "blocks": self.blocks,
            "head": self.head_state,
            "head_seq": self.head_seq,
            "head_pos": self.head,
            "pending": [{"seq": t.seq, "blocks": len(t.homes),
                         "revoked": len(t.revokes)} for t in self.txns],
        }
