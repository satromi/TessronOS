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

On a volume of version 5 (design 7.3) every one of these blocks carries
version 5, and one transaction may hold any number of descriptor and
revoke blocks: descriptor 0 and its copies, descriptor 1 and its copies,
..., revoke 0, revoke 1, ..., the commit. Each descriptor and revoke
block has its number within the transaction at 28 and names at most 254
copies or 508 blocks; the commit has the number of descriptors at 48 and
of revoke blocks at 52. The chain takes in each copy with its index
counted over the whole transaction, descriptor k (from 1) plus
0xA5A5A5A5 + k and revoke block r plus 0x5A5A5A5A + r, so that a
transaction of one descriptor and one revoke block has the chain it has
on version 4. A version 4 head on a volume of version 5 with an empty
ring is what a conversion cut before its last step leaves; the kernel
writes the head again at version 5 and opens it.
"""

import struct
import time

from .crc import crc32c, crc32c_u32
from .error import TsfsError
from .layout import (BLOCK_SIZE, JRNL_AREA_BLOCKS, JRNL_DESC_ENT,
                     JRNL_HDR_SIZE, JRNL_MAX_BLOCKS_TXN, JRNL_MAX_REVOKE,
                     JRNL_VERSION5, TRON_EPOCH, txn_budget, txn_foot)

VERSION = 2
VERSION5 = JRNL_VERSION5

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
JB_INDEX = 28                   # version 5: the block's number within its transaction
JB_ENT = JRNL_HDR_SIZE
JC_TIME = 32
JC_CHAIN = 40
JC_BLOCKS = 44
JC_NDESC = 48                   # version 5
JC_NREVOKE = 52                 # version 5

REVOKE_SALT = 0x5A5A5A5A
DESC_SALT = 0xA5A5A5A5
M32 = 0xFFFFFFFF


def salt_of(sb):
    return crc32c(sb.vol_uuid.bytes) ^ (sb.mkfs_time & 0xFFFFFFFF)


class Transaction(object):
    """One transaction the scan found good."""

    def __init__(self, seq, pos, homes):
        self.seq = seq
        self.pos = pos                  # of its descriptor in the ring
        self.homes = homes              # where each copy belongs
        self.revokes = []
        self.ring = []                  # ring position of each copy
        self.ndesc = 1
        self.nrevoke = 0

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
        self.v5 = sb.v5
        self.version = VERSION5 if self.v5 else VERSION
        self.budget = txn_budget(self.blocks, self.v5)
        self.head_version = None
        # what the scan found
        self.head_state = None          # "good", "old", "empty", "damaged"
        self.old_ring = False
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

    def is_kind(self, buf, magic, version=None):
        return (bytes(buf[0:4]) == magic
                and struct.unpack_from("<I", buf, 4)[0]
                == (self.version if version is None else version))

    def make_kind(self, magic, seq, count, index=0):
        buf = bytearray(BLOCK_SIZE)
        buf[0:4] = magic
        struct.pack_into("<IQI", buf, 4, self.version, seq, count)
        if self.v5:
            struct.pack_into("<I", buf, JB_INDEX, index)
        return buf

    def _read(self, blk):
        return self.vol.raw_read(blk)

    # ------------------------------------------------------------ head

    def read_head(self):
        """Look at the head block; answers its state. On a volume of
        version 5 a head of version 4 is "old": good, as long as the ring
        it starts is empty (scan says)."""
        buf = self._read(self.start)
        ver = struct.unpack_from("<I", buf, 4)[0]
        ok_ver = ver == self.version or (self.v5 and ver == VERSION)
        if (bytes(buf[0:4]) == JH_MAGIC and ok_ver
                and struct.unpack_from("<I", buf, JH_CRC)[0]
                == self.crc_at(buf, JH_CRC)
                and struct.unpack_from("<Q", buf, JH_POS)[0] < self.len):
            self.head_seq, self.head = struct.unpack_from("<QQ", buf, JH_SEQ)
            self.head_version = ver
            self.head_state = "good" if ver == self.version else "old"
        else:
            self.head = 0
            self.head_seq = self.seq_sb
            self.head_state = "empty" if buf == bytes(BLOCK_SIZE) else "damaged"
        return self.head_state

    def pack_head(self, head_seq, pos, version=None):
        buf = bytearray(BLOCK_SIZE)
        buf[0:4] = JH_MAGIC
        struct.pack_into("<IQQ", buf, 4,
                         self.version if version is None else version,
                         head_seq, pos)
        struct.pack_into("<I", buf, JH_CRC, self.crc_at(buf, JH_CRC))
        return bytes(buf)

    def write_head(self, head_seq, pos, version=None):
        self.vol.raw_write(self.start, self.pack_head(head_seq, pos, version))
        self.head_seq = head_seq
        self.head = pos
        self.head_version = self.version if version is None else version
        self.head_state = "good" if self.head_version == self.version else "old"


    # ------------------------------------------------------------ scan

    def scan(self):
        """Find the transactions that became real, from the head on, by
        the kernel's rules (ts_jrnl_replay): each has to carry the next
        number, pass its CRCs and end in a commit whose chain agrees. The
        first that does not stops the scan; its revokes do not count.
        Answers the list of transactions. On a volume of version 5 whose
        head is still of version 4 the ring has to be empty; old_ring
        says when it is not."""
        self.txns = []
        self.stop_reason = None
        self.old_ring = False
        if not self.usable():
            self.head_state = "none"
            return self.txns
        if self.read_head() not in ("good", "old"):
            self.good_end = 0
            self.next_seq = self.seq_sb
            return self.txns
        if self.head_state == "old":
            for ver in (VERSION, VERSION5):
                self.version, self.v5 = ver, ver == VERSION5
                if self._scan():
                    self.old_ring = True
            self.version, self.v5 = VERSION5, True
            self.good_end = self.head
            self.next_seq = self.head_seq
            self.stop_reason = ("the head is of version 4 on a volume of "
                                "version 5 and the ring is not empty"
                                if self.old_ring else None)
            return self.txns
        self.txns = self._scan()
        return self.txns

    def _scan(self):
        txns = []
        self.stop_reason = None
        v5 = self.v5
        pos = self.head
        cur = self.head_seq
        good_end = pos
        scanned = 0
        maxtx = self.len // 2 + 1
        in_tx = False
        phase = None                    # "desc", then "revoke"
        chain = 0
        tx = None
        ndesc = nrev = ncopy = 0
        nrvk = 0
        while scanned < self.len and len(txns) < maxtx:
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
            index = struct.unpack_from("<I", buf, JB_INDEX)[0] if v5 else 0
            if self.is_kind(buf, JB_DESC) and (
                    (not in_tx and index == 0)
                    or (v5 and in_tx and phase == "desc" and index == ndesc)):
                n = struct.unpack_from("<I", buf, JB_COUNT)[0]
                if n > JRNL_MAX_BLOCKS_TXN:
                    self.stop_reason = "a descriptor names %d blocks" % n
                    break
                c = struct.unpack_from("<I", buf, JB_CRC)[0]
                if not in_tx:
                    chain = crc32c_u32(c)
                    tx = Transaction(cur, pos, [])
                    ndesc = nrev = ncopy = 0
                else:
                    chain ^= (crc32c_u32(c) + DESC_SALT + index) & M32
                ok = True
                for i in range(n):
                    ent = JB_ENT + i * JRNL_DESC_ENT
                    home, want = struct.unpack_from("<QI", buf, ent)
                    copy = self._read(self.ring_blk(pos + 1 + i))
                    c = crc32c(copy)
                    if c != want:
                        ok = False
                        self.stop_reason = ("copy %d of transaction %d did not "
                                            "reach the medium" % (ncopy + i, cur))
                        break
                    chain ^= (crc32c_u32(c) + ncopy + i) & M32
                    tx.homes.append(home)
                    tx.ring.append(pos + 1 + i)
                if not ok:
                    break
                ncopy += n
                ndesc += 1
                pos += 1 + n
                scanned += 1 + n
                in_tx = True
                phase = "desc"
                continue
            if (in_tx and self.is_kind(buf, JB_REVOKE)
                    and (not v5 or index == nrev)):
                n = struct.unpack_from("<I", buf, JB_COUNT)[0]
                if n > JRNL_MAX_REVOKE:
                    self.stop_reason = "a revoke block names %d blocks" % n
                    break
                tx.revokes.extend(struct.unpack_from("<Q", buf, JB_ENT + 8 * i)[0]
                                  for i in range(n))
                nrvk += n
                c = struct.unpack_from("<I", buf, JB_CRC)[0]
                chain ^= (crc32c_u32(c) + REVOKE_SALT + index) & M32
                nrev += 1
                phase = "revoke"
                pos += 1
                scanned += 1
                continue
            if in_tx and self.is_kind(buf, JB_COMMIT):
                if struct.unpack_from("<I", buf, JC_CHAIN)[0] != chain:
                    self.stop_reason = ("the commit of transaction %d does not "
                                        "match its chain" % cur)
                    break
                if v5:
                    have = struct.unpack_from("<II", buf, JC_NDESC)
                    if have != (ndesc, nrev):
                        self.stop_reason = (
                            "the commit of transaction %d counts %d descriptors "
                            "and %d revoke blocks, the ring %d and %d"
                            % (cur, have[0], have[1], ndesc, nrev))
                        break
                tx.ndesc, tx.nrevoke = ndesc, nrev
                txns.append(tx)
                tx = None
                cur += 1
                pos += 1
                scanned += 1
                good_end = pos
                in_tx = False
                phase = None
                continue
            self.stop_reason = ("block %d of the ring is not what comes next"
                                % (pos % self.len))
            break
        if tx is not None and self.stop_reason is not None:
            self.stop_reason += " (transaction %d is not complete)" % tx.seq
        self.good_end = good_end
        self.next_seq = cur
        return txns

    def pending(self):
        return len(self.txns)

    def blocks_to_replay(self):
        """(home, ring position of the copy) in the order they go back,
        leaving out a copy that the same or a later transaction revoked."""
        revoked = {}
        for t in self.txns:
            for b in t.revokes:
                if revoked.get(b, -1) < t.seq:
                    revoked[b] = t.seq
        out = []
        for t in self.txns:
            for home, rpos in zip(t.homes, t.ring):
                if revoked.get(home, -1) >= t.seq:
                    continue
                out.append((home, rpos))
        return out

    def overlay(self):
        """{home: bytes} as a replay would leave the blocks."""
        over = {}
        for home, rpos in self.blocks_to_replay():
            over[home] = self._read(self.ring_blk(rpos))
        return over

    def replay(self):
        """Put every good transaction in place, then move the head past
        them with an empty ring. Answers the number of blocks written. A
        head of version 4 on a volume of version 5 is written again at
        version 5 when the ring is empty, and refused when it is not."""
        if not self.usable():
            return 0
        self.scan()
        if self.head_state == "old":
            if self.old_ring:
                raise TsfsError(self.stop_reason)
            self.write_head(self.head_seq, self.head)
            return 0
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
        machine that lost its power just before it would leave the ring.
        On a volume of version 5 it takes up to the volume's budget of
        copies, in as many descriptors and revoke blocks as they need."""
        if not self.usable():
            raise TsfsError("the volume has no journal area")
        if self.head_state is None:
            self.read_head()
        if self.head_state != "good":
            self.write_head(self.seq_sb, 0)
        revokes = list(revokes)
        if self.v5:
            if len(blocks) > self.budget:
                raise TsfsError("%d copies, more than the budget %d"
                                % (len(blocks), self.budget))
        elif len(blocks) > JRNL_MAX_BLOCKS_TXN or len(revokes) > JRNL_MAX_REVOKE:
            raise TsfsError("too much for one transaction")
        seq = self.head_seq + len(self.txns) if seq is None else seq
        if pos is None:
            pos = self.good_end if self.txns else self.head
        per = JRNL_MAX_BLOCKS_TXN
        groups = [blocks[i:i + per] for i in range(0, len(blocks), per)] or [[]]
        rgroups = [revokes[i:i + JRNL_MAX_REVOKE]
                   for i in range(0, len(revokes), JRNL_MAX_REVOKE)]
        foot = txn_foot(len(blocks), len(revokes))
        if foot > self.len:
            raise TsfsError("the ring is too small")

        p = pos
        chain = 0
        ring = []
        k = 0
        for d, grp in enumerate(groups):
            desc = self.make_kind(JB_DESC, seq, len(grp), d)
            crcs = []
            for i, (home, data) in enumerate(grp):
                if len(data) != BLOCK_SIZE:
                    raise TsfsError("a copy is %d bytes" % len(data))
                c = crc32c(data)
                crcs.append(c)
                struct.pack_into("<QI", desc, JB_ENT + i * JRNL_DESC_ENT, home, c)
            c = self.crc_at(desc, JB_CRC)
            struct.pack_into("<I", desc, JB_CRC, c)
            if d == 0:
                chain = crc32c_u32(c)
            else:
                chain ^= (crc32c_u32(c) + DESC_SALT + d) & M32
            self.vol.raw_write(self.ring_blk(p), bytes(desc))
            p += 1
            for i, (home, data) in enumerate(grp):
                chain ^= (crc32c_u32(crcs[i]) + k) & M32
                self.vol.raw_write(self.ring_blk(p), bytes(data))
                ring.append(p)
                p += 1
                k += 1
        for r, rg in enumerate(rgroups):
            rv = self.make_kind(JB_REVOKE, seq, len(rg), r)
            for i, b in enumerate(rg):
                struct.pack_into("<Q", rv, JB_ENT + 8 * i, b)
            c = self.crc_at(rv, JB_CRC)
            struct.pack_into("<I", rv, JB_CRC, c)
            chain ^= (crc32c_u32(c) + REVOKE_SALT + r) & M32
            self.vol.raw_write(self.ring_blk(p), bytes(rv))
            p += 1
        if write_commit:
            cm = self.make_kind(JB_COMMIT, seq, 0)
            t = int(time.time()) - TRON_EPOCH if when is None else when
            struct.pack_into("<QII", cm, JC_TIME, t, chain, foot - 1)
            if self.v5:
                struct.pack_into("<II", cm, JC_NDESC, len(groups), len(rgroups))
            struct.pack_into("<I", cm, JB_CRC, self.crc_at(cm, JB_CRC))
            self.vol.raw_write(self.ring_blk(p), bytes(cm))
            t = Transaction(seq, pos, [home for (home, _d) in blocks])
            t.ring = ring
            t.revokes = revokes
            t.ndesc, t.nrevoke = len(groups), len(rgroups)
            self.txns.append(t)
            self.good_end = pos + foot
            self.next_seq = seq + 1
        return foot

    def summary(self):
        return {
            "start": self.start,
            "blocks": self.blocks,
            "version": self.version,
            "head": self.head_state,
            "head_version": self.head_version,
            "head_seq": self.head_seq,
            "head_pos": self.head,
            "budget": self.budget,
            "pending": [{"seq": t.seq, "blocks": len(t.homes),
                         "revoked": len(t.revokes), "descriptors": t.ndesc,
                         "revoke_blocks": t.nrevoke} for t in self.txns],
        }
