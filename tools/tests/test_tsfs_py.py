#!/usr/bin/env python3
"""Tests of the host side TSFS tools (version 4 volumes).

    python3 tools/tests/test_tsfs_py.py

Everything runs on image files in a temporary directory, so nothing here
touches a device.
"""

import binascii
import filecmp
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)

from tsfs_py import (BLOCK_SIZE, BlockImage, Superblock, TsfsError, Volume,
                    crc32c, find_tsfs_partition, layout)
from tsfs_py import head as headmod
from tsfs_py import meta as metamod
from tsfs_py import obj as objmod
from tsfs_py import repair, transfer
from tsfs_py.btree import MAX_KEYS, TREE_GC, TREE_OBJ, TREE_ORPHAN, BTree, Node
from tsfs_py.check import Checker
from tsfs_py.groups import AG_FREE, head_problems
from tsfs_py.image import SECTOR_SIZE, TSFS_TYPE_GUID
from tsfs_py.journal import Journal
from tsfs_py.layout import (GroupShape, MAGIC_TBL, NIL_UUID, is_spare,
                           owner_of)
from tsfs_py.obj import (F_GARBAGE, F_ORPHAN, F_RELINK, LK_EXTERNAL, OB_LINK,
                        OB_META, OB_NLINK, OB_REFCNT, LinkEntry,
                        ObjectBlock, sort_links)

VOL_BLOCKS = 4096                       # as the test partition of the kernel
VOL_BYTES = VOL_BLOCKS * BLOCK_SIZE

U_BOX = uuid.UUID("01a0d6d1-0000-7000-8000-000000000001")
U_DOC = uuid.UUID("01a0d6d1-0000-7000-8000-000000000002")
U_BIN = uuid.UUID("01a0d6d1-0000-7000-8000-000000000003")
U_LONE = uuid.UUID("01a0d6d1-0000-7000-8000-000000000004")
U_AWAY = uuid.UUID("01a0d6d1-0000-7000-8000-0000000000ff")   # on no volume


def vobj(n):
    return uuid.UUID("01a0d8c4-0000-7000-8000-%012x" % n)


def link(target, v, extra=""):
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="8" vobjtop="8"%s/>\n'
            % (target, v, extra))


def tool(name, *args):
    return subprocess.run([sys.executable, os.path.join(TOOLS, name)]
                          + [str(a) for a in args],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class Temp(object):
    """A directory thrown away afterwards."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="tsfs-test-")

    def path(self, *names):
        return os.path.join(self.dir, *names)

    def image(self, name="volume.img", blocks=VOL_BLOCKS):
        p = self.path(name)
        with open(p, "wb") as f:
            f.truncate(blocks * BLOCK_SIZE)
        return p

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def write_set(d):
    """A small TADjs set: a box linking a document, a byte object, the
    lone one (with no vobjid) and something on no volume; the document
    linking the byte object and the lone one and carrying an icon and
    resources; a byte object; and the lone one, which links nothing."""
    os.makedirs(d, exist_ok=True)

    def put(name, data):
        with open(os.path.join(d, name), "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))

    def js(name, extra=None):
        m = {"name": name, "relationship": [], "linktype": False,
             "makeDate": "2026-09-20T01:02:03Z",
             "updateDate": "2026-09-21T01:02:03Z",
             "accessDate": "2026-09-22T01:02:03Z",
             "periodDate": None, "refCount": 7, "recordCount": 9,
             "editable": True, "deletable": True, "readable": True,
             "maker": "test"}
        m.update(extra or {})
        return json.dumps(m, ensure_ascii=False, indent=2) + "\n"

    put("%s.json" % U_BOX, js("箱", {"deletable": False}))
    put("%s_0.xtad" % U_BOX,
        '<tad version="1.0" encoding="UTF-8" filename="箱">\n<figure>\n'
        + link(U_DOC, vobj(3)) + link(U_BIN, vobj(1))
        + link(U_AWAY, vobj(2))
        + '<link id="%s_0.xtad" vobjleft="1"/>\n' % U_LONE     # no vobjid
        + '<link id="not-a-uuid" vobjid="%s"/>\n' % vobj(9)
        + link(U_DOC, vobj(3))                                 # same vobjid again
        + '</figure>\n</tad>\n')
    put("%s.json" % U_DOC, js("文章"))
    body = "".join("<p>%d 行目の文章です。</p>\n" % i for i in range(300))
    put("%s_0.xtad" % U_DOC,
        '<tad version="1.0" encoding="UTF-8" filename="文章">\n<document>\n'
        + body + link(U_BIN, vobj(5), ' name="a > b"')
        + '</document>\n</tad>\n')
    put("%s_1.xtad" % U_DOC,
        '<tad><document><p>2</p>\n<link id="%s_0.xtad"/>\n' % U_LONE  # no vobjid
        + link(U_BIN, vobj(5))              # the vobjid of record 0 again
        + "</document></tad>")
    put("%s.ico" % U_DOC, b"\0\0\1\0\1\0" + bytes(range(256)) * 3)
    put("%s_0_1.png" % U_DOC, b"\x89PNG" + bytes(5000))
    put("%s_1_bgm.mp3" % U_DOC, b"ID3" + bytes(100))
    put("%s.json" % U_BIN, js("バイト", {"tessronos": {"records": [
        {"n": 0, "rt": 1}, {"n": 1, "rt": 11}]}}))
    put("%s_0.xtad" % U_BIN, "<tad><document><p>font</p></document></tad>")
    put("%s_1.bin" % U_BIN, bytes((i * 7) & 0xFF for i in range(20000)))
    put("%s_2.bin" % U_BIN, b"")
    put("%s.json" % U_LONE, js("ひとり"))
    put("%s_0.xtad" % U_LONE, "<tad/>")
    put("README", "not an object")


class Base(unittest.TestCase):

    def setUp(self):
        self.tmp = Temp()
        self.addCleanup(self.tmp.close)

    def make(self, blocks=VOL_BLOCKS, **kw):
        dev = BlockImage(self.tmp.image(blocks=blocks), writable=True)
        vol = Volume.format(dev, **kw)
        self.addCleanup(dev.close)
        return vol

    def reopen(self, path=None, writable=False, replay=False):
        dev = BlockImage(path or self.tmp.path("volume.img"), writable=writable)
        vol = Volume.open(dev, replay=replay)
        self.addCleanup(dev.close)
        return vol

    def check(self, vol):
        vol.maps = None
        c = Checker(vol)
        return c, c.run()

    def built(self, roots=None):
        """A volume with the small set on it, closed; answers its path."""
        d = self.tmp.path("in")
        write_set(d)
        vol = self.make(label="T")
        n, skipped = transfer.build(
            vol, [d], {"root": U_BOX} if roots is None else roots)
        self.assertEqual(skipped, [])
        self.assertEqual(n, 4)
        vol.flush(both=True, spares=True)
        vol.dev.close()
        return self.tmp.path("volume.img")

    def flip(self, path, blk, off):
        with open(path, "r+b") as f:
            f.seek(blk * BLOCK_SIZE + off)
            b = f.read(1)
            f.seek(blk * BLOCK_SIZE + off)
            f.write(bytes([b[0] ^ 0x5A]))

    def assertFound(self, items, *words):
        text = "\n".join("%s: %s" % (w, t) for (w, t) in items)
        for word in words:
            self.assertIn(word, text)


class TestBasics(unittest.TestCase):

    def test_crc(self):
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)
        self.assertEqual(crc32c(b""), 0)

    def test_layout(self):
        # the kernel's 16MB test partition
        self.assertEqual(layout(4096), (515, 1024, 4))
        self.assertEqual(layout(1 << 20), (4096, 1 << 17, 8))
        self.assertRaises(TsfsError, layout, 600)
        self.assertEqual([n for n in range(60) if is_spare(n)],
                         [1, 3, 5, 7, 9, 25, 27, 49])

    def test_group_shape(self):
        s = GroupShape(517, 1024, 4096, 3)
        self.assertEqual((s.first, s.len, s.spare, s.map_first, s.map_blocks),
                         (3589, 507, True, 3591, 1))
        self.assertEqual(s.meta, 3)

    def test_superblock_round_trip(self):
        sb = Superblock()
        sb.total_blocks, sb.ag_blocks, sb.ag_count = 4096, 1024, 4
        sb.journal_blocks = 515
        sb.generation = 7
        sb.set_label("ラベル")
        sb.root_uuid = U_BOX
        buf = sb.pack()
        self.assertEqual(buf[0:8], b"TFSVOL02")
        self.assertEqual(struct.unpack_from("<I", buf, 4092)[0],
                         crc32c(buf[:4092]))
        back = Superblock.unpack(buf)
        self.assertEqual((back.label, back.generation, back.root_uuid),
                         ("ラベル", 7, U_BOX))
        bad = bytearray(buf)
        bad[300] ^= 1
        self.assertRaises(TsfsError, Superblock.unpack, bytes(bad))

    def test_owner_is_the_low_half_read_little_endian(self):
        u = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
        self.assertEqual(owner_of(u), 0xffeeddccbbaa9988)

    def test_res_names(self):
        self.assertEqual(metamod.res_key("_2_1.png"), (2, 1, "png"))
        rec, res, ext = metamod.res_key("_0_bgm.mp3")
        self.assertEqual((rec, ext), (0, "bgm.mp3"))
        self.assertLess(res, -1)
        self.assertEqual(metamod.res_name(U_DOC, 0, res, ext),
                         "%s_0_bgm.mp3" % U_DOC)
        self.assertIsNone(metamod.res_key("_0_1." + "x" * 16))

    def test_links_are_read_as_tokens(self):
        nil = NIL_UUID
        data = (b'<a><link id="%s_0.xtad" vobjid=\'%s\' t="x>y"/>'
                b'<linkage id="%s_0.xtad" vobjid="%s"/>'
                b'<link vobjid="%s" id="%s"></link></a>'
                % (str(U_DOC).encode(), str(vobj(1)).encode(),
                   str(U_BIN).encode(), str(vobj(2)).encode(),
                   str(vobj(3)).encode(), str(U_BIN).encode()))
        # a single quoted vobjid is not taken: the link has none
        self.assertEqual(metamod.extract_links(data),
                         [(nil, U_DOC), (vobj(3), U_BIN)])
        hexonly = U_BIN.hex.encode()
        data = (b'<link\tid="%s_1.xtad">'                 # a tab, no hyphens
                b'<link id = "%s">'                          # blanks around =
                b'<link id="%s">'                            # cut short
                b'<link/>'
                b'<link id="%s" vobjid="%s"/><link id="%s" vobjid="%s"/>'
                b'<link id="%s"/><link id="%s"/>'
                % (hexonly, str(U_DOC).encode(), str(U_DOC)[:30].encode(),
                   str(U_DOC).encode(), str(vobj(4)).encode(),
                   str(U_LONE).encode(), str(vobj(4)).encode(),
                   str(U_LONE).encode(), str(U_LONE).encode()))
        self.assertEqual(metamod.extract_links(data),
                         [(nil, U_BIN), (vobj(4), U_DOC), (nil, U_LONE),
                          (nil, U_LONE)])
        # the same vobjid in a later record is left out, and a link past
        # the limit stops the scan
        out = []
        self.assertTrue(metamod.scan_links(link(U_DOC, vobj(1)).encode(), 1, out))
        two = link(U_BIN, vobj(1)) + link(U_BIN, vobj(2))
        self.assertTrue(metamod.scan_links(two.encode(), 2, out))
        self.assertEqual(out, [(vobj(1), U_DOC, 1), (vobj(2), U_BIN, 2)])
        self.assertFalse(metamod.scan_links(link(U_BIN, vobj(3)).encode(), 3, out, 2))

    def test_link_uuid_is_text_order(self):
        """The kernel's ts_str_to_uuid puts the bytes in text order, which
        is what uuid.UUID.bytes holds."""
        u = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
        self.assertEqual(metamod.link_uuid(str(u).encode()).bytes,
                         bytes.fromhex("00112233445566778899aabbccddeeff"))
        self.assertEqual(metamod.link_uuid(b"-0011-2233445566778899aabbccddeeffxx"),
                         u)
        self.assertIsNone(metamod.link_uuid(b"0-0112233-4455-6677-8899-aabbccddeef"))
        self.assertIsNone(metamod.link_uuid(str(u).encode()[:35]))

    def test_link_order_is_the_raw_bytes(self):
        """The first 40 bytes as they lie: the rid little endian, so rid 256
        comes before rid 1; a zero vobjid before any other."""
        t = U_DOC
        es = [LinkEntry(vobj(1), t, 1), LinkEntry(NIL_UUID, t, 1),
              LinkEntry(NIL_UUID, t, 256), LinkEntry(NIL_UUID, U_BIN, 2),
              LinkEntry(NIL_UUID, t, 1, LK_EXTERNAL)]
        got = [(e.vobjid, e.target, e.rid, e.flags) for e in sort_links(es)]
        self.assertEqual(got, [(NIL_UUID, t, 256, 0), (NIL_UUID, t, 1, 0),
                               (NIL_UUID, t, 1, LK_EXTERNAL),
                               (NIL_UUID, U_BIN, 2, 0), (vobj(1), t, 1, 0)])

    def test_metadata_text_edits(self):
        """Members are taken out and put in as the kernel's jx_del and
        jx_set do; "records" goes first."""
        t = (b'{\n  "name": "x",\n  "refCount": 7,\n  "records": [],\n'
             b'  "makeDate": "2026-01-01T00:00:00Z"\n}')
        recs = [{"rid": 1, "rt": 1, "sub": 0}, {"rid": 2, "rt": 9, "sub": 3}]
        st = metamod.stored_text(t, recs)
        self.assertEqual(st, b'{"records":[{"rid":1,"rt":1,"sub":0},'
                             b'{"rid":2,"rt":9,"sub":3}],\n  "name": "x"\n}')
        self.assertEqual(metamod.stored_text(b'{ }', []), b'{ }')
        self.assertEqual(metamod.stored_text(b'[1, 2]', recs), b'[1, 2]')
        ns = metamod.parse_date("2026-09-26T01:02:03Z")
        out = metamod.synth(st, 3, 2, ns, ns, 0)
        self.assertTrue(out.startswith(
            b'{"refCount":3,"recordCount":2,"makeDate":"2026-09-26T01:02:03Z",'
            b'"updateDate":"2026-09-26T01:02:03Z","records":'))
        self.assertNotIn(b"accessDate", out)
        # a text from before stage T4 keeps its members where they are
        self.assertEqual(metamod.synth(b'{"refCount":9,"n":1}', 2, 0, 0, 0, 0),
                         b'{"recordCount":0,"refCount":2,"n":1}')
        self.assertEqual(metamod.synth(b'"text"', 1, 1, ns, ns, ns), b'"text"')

    def test_order_parse(self):
        """rid, rt and sub as the kernel's js_get_num reads them."""
        self.assertEqual(metamod.order_parse(
            b'{"records":[{"rid":1.9},{"rid":2,"rt":"x","sub":-1}]}'),
            [(1, 1, 0), (2, 1, 0xFFFFFFFF)])
        self.assertIsNone(metamod.order_parse(b'{"records":[{"rid":0}]}'))
        self.assertIsNone(metamod.order_parse(b'{"records":[1]}'))
        self.assertIsNone(metamod.order_parse(b'[{"rid":1}]'))
        self.assertIsNone(metamod.order_parse(
            b'{"records":[%s]}' % b",".join([b'{"rid":1}'] * 65)))
        self.assertEqual(metamod.order_parse(
            b'{"records":[{"rid":3}],"records":[{"rid":4}]}'), [(3, 1, 0)])

    def test_dates(self):
        ns = metamod.parse_date("2026-09-26T00:00:00Z")
        self.assertEqual(metamod.format_date(ns), "2026-09-26T00:00:00Z")
        self.assertEqual(metamod.parse_date("2026-09-26T09:00:00+09:00"), ns)
        self.assertEqual(metamod.parse_date("1970-01-01T00:00:00Z"), 0)
        self.assertIsNone(metamod.parse_date("yesterday"))


class TestFormat(Base):

    def test_as_the_kernel_makes_it(self):
        vol = self.make(label="VOL", journal_seq=0x1201)
        dev = vol.dev
        a = Superblock.unpack(dev.read_block(0))
        b = Superblock.unpack(dev.read_block(1))
        self.assertEqual((a.generation, b.generation), (2, 1))
        self.assertEqual(a.journal_seq, 0x1201)
        self.assertEqual((a.journal_start, a.journal_blocks, a.ag_count,
                          a.ag_blocks), (2, 515, 4, 1024))
        self.assertEqual(a.free_blocks, 1022 + 1021 + 1022 + 504)
        self.assertEqual(dev.read_block(2), bytes(BLOCK_SIZE))   # journal head
        for s in a.groups():
            hb = dev.read_block(s.first)
            self.assertEqual(head_problems(hb, a, s), [])
            self.assertEqual(headmod.generation(hb), 0x1201)
            if s.spare and s.n:
                self.assertEqual(dev.read_block(s.first + 1), dev.read_block(0))
        c, rep = self.check(vol)
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.warnings, [])

    def test_too_small(self):
        dev = BlockImage(self.tmp.image(blocks=600), writable=True)
        self.addCleanup(dev.close)
        self.assertRaises(TsfsError, Volume.format, dev)

    def test_mktfs_in_a_partition(self):
        path = self.tmp.path("disk.img")
        first_lba, part_blocks = 2048, 4096
        nsect = first_lba + part_blocks * 8 + 64
        img = bytearray(nsect * SECTOR_SIZE)
        entries = bytearray(128 * 128)
        struct.pack_into("<16s16sQQQ", entries, 0, TSFS_TYPE_GUID.bytes_le,
                         uuid.uuid4().bytes_le, first_lba,
                         first_lba + part_blocks * 8 - 1, 0)
        hdr = bytearray(92)
        hdr[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ16sQIII", hdr, 8, 0x10000, 92, 0, 0, 1,
                         nsect - 1, 34, nsect - 34, uuid.uuid4().bytes_le, 2,
                         128, 128, binascii.crc32(bytes(entries)) & 0xFFFFFFFF)
        img[SECTOR_SIZE:SECTOR_SIZE + 92] = hdr
        img[2 * SECTOR_SIZE:2 * SECTOR_SIZE + len(entries)] = entries
        with open(path, "wb") as f:
            f.write(img)
        before = bytes(img[:first_lba * SECTOR_SIZE])

        r = tool("mktsfs", "-q", "-L", "PART", path)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(path, "rb") as f:
            self.assertEqual(f.read(first_lba * SECTOR_SIZE), before)
        part = find_tsfs_partition(path)
        dev = BlockImage(path, offset=part.offset, length=part.length)
        self.addCleanup(dev.close)
        vol = Volume.open(dev)
        self.assertEqual(vol.sb.total_blocks, part_blocks)
        r = tool("fsck.tsfs", path)
        self.assertEqual(r.returncode, 0, r.stdout)


class TestBTree(Base):

    def test_build_walk_lookup(self):
        vol = self.make()
        keys = [uuid.UUID(int=(i * 2654435761) % (1 << 128) | (1 << 127))
                for i in range(1, 400)]
        items = [(k, 1000 + i) for i, k in enumerate(keys)]
        nodes = vol.btree.build(items)
        self.assertEqual(nodes, vol.sb.objtbl_blocks)
        got = list(vol.btree.items())
        self.assertEqual([k for k, _ in got], sorted(keys, key=lambda k: k.bytes))
        for k, v in items[::37]:
            self.assertEqual(vol.btree.lookup(k), v)
        self.assertIsNone(vol.btree.lookup(uuid.UUID(int=5)))

    def test_three_levels(self):
        vol = self.make()
        n = MAX_KEYS * (MAX_KEYS + 1) + 10
        blocks = []
        # values must be blocks the checker can take: give each key the
        # same fake value and look only at the tree's own shape
        items = [(uuid.UUID(int=(1 << 120) + i), 7) for i in range(n)]
        vol.btree.build(items)
        root = vol.btree.read_node(vol.sb.objtbl_start)
        self.assertEqual(root.level, 2)
        self.assertEqual(sum(1 for _ in vol.btree.items()), n)
        c = Checker(vol)
        vol.load_maps()
        self.assertEqual(len(c.check_tree(TREE_OBJ)), n)
        del blocks

    def test_node_layout(self):
        vol = self.make()
        vol.btree.build([(U_BOX, 1234)])
        blk = vol.sb.objtbl_start
        buf = vol.read_block(blk)
        self.assertEqual(buf[0:4], b"TFBT")
        self.assertEqual(struct.unpack_from("<Q", buf, 8)[0], blk)
        self.assertEqual(buf[16:32], vol.sb.vol_uuid.bytes)
        self.assertEqual(struct.unpack_from("<Q", buf, 32)[0], TREE_OBJ)
        self.assertEqual((buf[64], buf[65]), (0, 0))
        self.assertEqual(struct.unpack_from("<H", buf, 66)[0], 1)
        self.assertEqual(buf[96:112], U_BOX.bytes)
        self.assertEqual(struct.unpack_from("<Q", buf, 112)[0], 1234)
        self.assertIsNone(headmod.problem(buf, vol.sb.vol_uuid, b"TFBT", blk))


class TestImport(Base):

    def test_objects_links_and_counts(self):
        path = self.built()
        vol = self.reopen(path)
        c, rep = self.check(vol)
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.warnings, [])
        self.assertEqual(rep.stats["objects"], 4)

        box = vol.object(U_BOX)
        doc = vol.object(U_DOC)
        binobj = vol.object(U_BIN)
        lone = vol.object(U_LONE)
        # the root's hold; box -> doc; box, doc -> bin; box, doc -> lone
        # (links with no vobjid, each an entry of its own)
        self.assertEqual((box.refcnt, doc.refcnt, binobj.refcnt, lone.refcnt),
                         (1, 1, 2, 2))
        self.assertEqual((box.pins, doc.pins, binobj.pins, lone.pins),
                         (1, 0, 0, 0))
        self.assertFalse(lone.flags & F_GARBAGE)
        self.assertEqual(vol.garbage(), [])
        self.assertEqual(vol.sb.root_uuid, U_BOX)
        self.assertFalse(box.flags & objmod.F_DELETABLE)

        links = box.links()
        self.assertEqual([(e.vobjid, e.target, e.rid, e.flags) for e in links],
                         [(NIL_UUID, U_LONE, 1, 0),
                          (vobj(1), U_BIN, 1, 0), (vobj(2), U_AWAY, 1, LK_EXTERNAL),
                          (vobj(3), U_DOC, 1, 0)])
        # vobj(5) again in record 2 is left out; the link with none is kept
        self.assertEqual([(e.vobjid, e.target, e.rid) for e in doc.links()],
                         [(NIL_UUID, U_LONE, 2), (vobj(5), U_BIN, 1)])

        # the metadata keeps no structural key, and says what the records
        # are, first, as the kernel puts it in
        self.assertTrue(box.meta().startswith(b'{"records":[{"rid":1,"rt":1,'
                                              b'"sub":0}],'))
        m = json.loads(box.meta().decode("utf-8"))
        for k in metamod.STRUCT_KEYS:
            self.assertNotIn(k, m)
        self.assertEqual(m["records"], [{"rid": 1, "rt": 1, "sub": 0}])
        mb = json.loads(binobj.meta().decode("utf-8"))
        self.assertEqual(mb["records"], [{"rid": 1, "rt": 1, "sub": 0},
                                         {"rid": 2, "rt": 11, "sub": 0},
                                         {"rid": 3, "rt": 15, "sub": 0}])
        self.assertEqual(binobj.nextrid, 4)
        self.assertEqual(box.made, metamod.parse_date("2026-09-20T01:02:03Z"))

        # where the parts went
        pl = binobj.placements()
        self.assertEqual([(e.rid, e.kind, e.nbytes) for e in pl],
                         [(1, 0, 43), (2, 1, 20000), (3, 1, 0)])
        self.assertEqual(pl[0].place.form, objmod.FORM_INLINE)
        self.assertEqual(pl[1].place.form, objmod.FORM_RUN)
        self.assertEqual(pl[1].place.b, 5)
        self.assertEqual(binobj.read_data(pl[1].place),
                         bytes((i * 7) & 0xFF for i in range(20000)))
        # the icon is a management part beside the metadata, not a resource
        self.assertNotEqual(doc.place(objmod.OB_ICON).form, objmod.FORM_NONE)
        self.assertEqual(doc.icon(), b"\0\0\1\0\1\0" + bytes(range(256)) * 3)
        res = doc.resources()
        self.assertEqual(sorted((e.owner, e.ext) for e in res),
                         [(1, "png"), (2, "bgm.mp3")])

    def test_extent_tree(self):
        vol = self.make()
        # a volume broken up: every other block of group 0 taken
        s = vol.sb.group(0)
        for b in range(s.data_first, s.data_first + 40, 2):
            vol.maps.put(b, True)
        vol.maps.hint = s.data_first
        blk = vol.alloc_blk()
        o = ObjectBlock.new(vol, blk, U_LONE)
        data = bytes(range(256)) * 64               # 4 blocks
        pl = o.dseg_new(data)
        self.assertEqual(pl.form, objmod.FORM_TREE)
        self.assertEqual(o.read_data(pl), data)
        leaf = vol.read_block(pl.a)
        self.assertEqual(leaf[0:4], MAGIC_TBL)
        self.assertEqual(struct.unpack_from("<I", leaf, 64)[0], pl.b)

    def test_bad_sets_are_left_out(self):
        d = self.tmp.path("in")
        write_set(d)
        bad1 = uuid.UUID("01a0d6d1-0000-7000-8000-00000000000a")
        bad2 = uuid.UUID("01a0d6d1-0000-7000-8000-00000000000b")
        with open(os.path.join(d, "%s.json" % bad1), "w") as f:
            f.write("{not json")
        with open(os.path.join(d, "%s.json" % bad2), "w") as f:
            f.write("{}")
        with open(os.path.join(d, "%s_1.xtad" % bad2), "w") as f:
            f.write("<tad/>")                  # no record 0
        vol = self.make()
        n, skipped = transfer.build(vol, [d])
        self.assertEqual(n, 4)
        self.assertEqual(sorted(u for u, _ in skipped), [bad1, bad2])

    def test_spread_over_directories(self):
        d1, d2 = self.tmp.path("def"), self.tmp.path("rec")
        write_set(d1)
        os.makedirs(d2)
        shutil.move(os.path.join(d1, "%s_1.bin" % U_BIN), d2)
        vol = self.make()
        n, skipped = transfer.build(vol, [d1, d2])
        self.assertEqual((n, skipped), (4, []))
        self.assertEqual(vol.object(U_BIN).nplace, 3)

    def test_mktfs_from(self):
        d = self.tmp.path("in")
        write_set(d)
        img = self.tmp.path("m.img")
        r = tool("mktsfs", "-q", "--size", "16M", "--from", d, "--root", U_BOX,
                 "--sysbox", U_DOC, img)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = tool("fsck.tsfs", "--json", img)
        self.assertEqual(r.returncode, 0, r.stdout)
        out = json.loads(r.stdout.decode("utf-8"))
        self.assertTrue(out["clean"])
        self.assertEqual(out["stats"]["objects"], 4)
        self.assertEqual(out["volume"]["sysbox_uuid"], str(U_DOC))
        vol = self.reopen(img)
        doc = vol.object(U_DOC)
        self.assertEqual((doc.refcnt, doc.pins), (2, 1))  # box and the superblock
        self.assertEqual(vol.object(U_BOX).pins, 1)
        r = tool("mktsfs", "-q", "--size", "16M", "--from", d, "--root", U_AWAY,
                 self.tmp.path("n.img"))
        self.assertEqual(r.returncode, 2)


class TestExport(Base):

    def test_round_trip(self):
        path = self.built()
        e1, e2 = self.tmp.path("e1"), self.tmp.path("e2")
        r = tool("tsfs-export", "-o", e1, path)
        self.assertEqual(r.returncode, 0, r.stderr)
        img2 = self.tmp.path("again.img")
        r = tool("tsfs-import", "--size", "16M", e1, img2)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = tool("tsfs-export", "-o", e2, img2)
        self.assertEqual(r.returncode, 0, r.stderr)
        names = sorted(n for n in os.listdir(e1) if n != transfer.VOLUME_FILE)
        self.assertEqual(names, sorted(n for n in os.listdir(e2)
                                       if n != transfer.VOLUME_FILE))
        _match, mismatch, errors = filecmp.cmpfiles(e1, e2, names, shallow=False)
        self.assertEqual((mismatch, errors), ([], []))

        # what went out: the TADjs names, the structural keys back in
        self.assertIn("%s_1.bin" % U_BIN, names)
        self.assertIn("%s.ico" % U_DOC, names)
        self.assertIn("%s_0_1.png" % U_DOC, names)
        self.assertIn("%s_1_bgm.mp3" % U_DOC, names)
        with open(os.path.join(e1, "%s.json" % U_BIN), "rb") as f:
            raw = f.read()
        self.assertTrue(raw.startswith(
            b'{"refCount":2,"recordCount":3,"makeDate":"2026-09-20T01:02:03Z",'
            b'"updateDate":"2026-09-21T01:02:03Z",'
            b'"accessDate":"2026-09-22T01:02:03Z","records":[{"rid":1,'), raw)
        m = json.loads(raw.decode("utf-8"))
        self.assertEqual(m["records"][1], {"rid": 2, "rt": 11, "sub": 0})
        with open(os.path.join(e1, "%s_1.bin" % U_BIN), "rb") as f:
            self.assertEqual(f.read(), bytes((i * 7) & 0xFF for i in range(20000)))
        with open(os.path.join(e1, transfer.VOLUME_FILE)) as f:
            self.assertEqual(json.load(f)["root"], str(U_BOX))


class TestJournal(Base):

    def changed_object(self, vol, refcnt):
        o = vol.object(U_LONE)
        o.refcnt = refcnt
        return o.blk, headmod.seal(bytearray(o.hdr), vol.sb.vol_uuid,
                                   vol.sb.journal_seq, b"TFOB", o.blk,
                                   owner_of(U_LONE))

    def test_replay(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        j = vol.journal
        self.assertEqual(j.head_state, "empty")
        blk, new = self.changed_object(vol, 5)
        old = vol.read_block(blk)
        j.commit([(blk, new)])
        vol.dev.close()

        # read only: laid over in memory, reported, not written
        vol = self.reopen(path)
        self.assertEqual(vol.journal.pending(), 1)
        self.assertEqual(vol.read_block(blk), new)
        self.assertEqual(vol.dev.read_block(blk), old)
        c, rep = self.check(vol)
        self.assertFound(rep.warnings, "not in place")
        self.assertFound(rep.errors, "reference count is 5")
        r = tool("fsck.tsfs", path)
        self.assertEqual(r.returncode, 1)

        # replayed onto the medium, the head moved past it
        vol = self.reopen(path, writable=True, replay=True)
        self.assertEqual(vol.replayed, (1, 1))
        self.assertEqual(vol.dev.read_block(blk), new)
        j2 = Journal(vol)
        j2.scan()
        self.assertEqual((j2.head_state, j2.pending()), ("good", 0))
        self.assertEqual(j2.head, 3)
        self.assertEqual(j2.head_seq, vol.sb.journal_seq + 1)

    def test_revoke_and_incomplete(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        j = vol.journal
        j.write_head(vol.sb.journal_seq, 510)   # the ring wraps
        blk, v6 = self.changed_object(vol, 6)
        _b, v7 = self.changed_object(vol, 7)
        spare = vol.alloc_blk()
        j.commit([(blk, v6), (spare, bytes(BLOCK_SIZE))])
        j.commit([(blk, v7)], revokes=[spare])
        _b, v8 = self.changed_object(vol, 8)
        j.commit([(blk, v8)], write_commit=False)
        before = vol.dev.read_block(spare)
        vol.dev.close()

        vol = self.reopen(path)
        self.assertEqual(vol.journal.pending(), 2)
        self.assertIn("not complete", vol.journal.stop_reason)
        self.assertEqual(vol.read_block(blk), v7)
        self.assertEqual(vol.read_block(spare), before)   # revoked

        # a ring made for another volume does not count
        other = Journal(vol)
        other.salt ^= 1
        other.scan()
        self.assertNotEqual(other.head_state, "good")

    def test_commit_layout(self):
        vol = self.make(journal_seq=0x300)
        j = vol.journal
        j.commit([(700, bytes(range(256)) * 16)], revokes=[701], when=99)
        desc = vol.raw_read(3)
        self.assertEqual(desc[0:4], b"TFJD")
        self.assertEqual(struct.unpack_from("<IQI", desc, 4), (2, 0x300, 1))
        self.assertEqual(struct.unpack_from("<QI", desc, 32),
                         (700, crc32c(bytes(range(256)) * 16)))
        rv = vol.raw_read(5)
        self.assertEqual(rv[0:4], b"TFJR")
        self.assertEqual(struct.unpack_from("<Q", rv, 32)[0], 701)
        cm = vol.raw_read(6)
        self.assertEqual(cm[0:4], b"TFJC")
        self.assertEqual(struct.unpack_from("<QI", cm, 32)[0], 99)
        self.assertEqual(struct.unpack_from("<I", cm, 44)[0], 3)
        hd = vol.raw_read(2)
        self.assertEqual(hd[0:4], b"TFJH")
        salt = crc32c(vol.sb.vol_uuid.bytes) ^ (vol.sb.mkfs_time & 0xFFFFFFFF)
        tmp = bytearray(hd)
        tmp[24:28] = bytes(4)
        self.assertEqual(struct.unpack_from("<I", hd, 24)[0],
                         crc32c(bytes(tmp)) ^ salt)


class TestDamage(Base):

    def test_group_head(self):
        path = self.built()
        vol = self.reopen(path)
        s = vol.sb.group(2)
        self.flip(path, s.first, AG_FREE)
        c, rep = self.check(self.reopen(path))
        self.assertFound(rep.errors, "group 2", "head")
        r = tool("fsck.tsfs", "--fix", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        c, rep = self.check(self.reopen(path))
        self.assertEqual((rep.errors, rep.warnings), ([], []))

    def test_object_block(self):
        path = self.built()
        blk = self.reopen(path).lookup(U_DOC)
        self.flip(path, blk, 700)
        c, rep = self.check(self.reopen(path))
        self.assertFound(rep.errors, "object %s" % U_DOC, "CRC")
        r = tool("fsck.tsfs", "--json", path)
        self.assertEqual(r.returncode, 1)
        self.assertFalse(json.loads(r.stdout.decode("utf-8"))["clean"])

    def test_node(self):
        path = self.built()
        root = self.reopen(path).sb.objtbl_start
        self.flip(path, root, 100)
        c, rep = self.check(self.reopen(path))
        self.assertFound(rep.errors, "object index node %d" % root)

    def test_map(self):
        path = self.built()
        vol = self.reopen(path)
        blk = vol.lookup(U_BIN)
        s = vol.sb.group(vol.sb.group_of(blk))
        off = blk - s.first
        self.flip(path, s.map_first + off // 32768, (off % 32768) // 8)
        c, rep = self.check(self.reopen(path))
        self.assertFound(rep.errors, "does not match the CRC")
        r = tool("fsck.tsfs", "--fix", path)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_lost_and_leaked_blocks(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        blk = vol.lookup(U_BIN)
        vol.load_maps()
        leak = vol.maps.alloc_blk()              # taken, used by nothing
        vol.maps.put(blk, False)                 # in use, marked free
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "block %d is used by the object block" % blk)
        self.assertFound(rep.warnings, "taken but nothing uses them (%d" % leak)
        vol.dev.close()
        r = tool("fsck.tsfs", "--fix", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))
        self.assertFalse(vol.maps.get(leak))

    def test_superblock_copy(self):
        path = self.built()
        self.flip(path, 0, 50)
        vol = self.reopen(path)
        self.assertEqual(vol.sb_copies[1].sb.generation, vol.sb.generation)
        c, rep = self.check(vol)
        self.assertFound(rep.warnings, "superblock A")
        r = tool("fsck.tsfs", "--fix", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue(all(c.ok for c in self.reopen(path).sb_copies))


class TestCounts(Base):

    def test_relink(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        o = vol.object(U_DOC)
        o.set_table(OB_LINK, OB_NLINK, [])      # its link to the byte object gone
        o.store()
        b = vol.object(U_LONE)
        b.refcnt = 3
        b.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "object %s" % U_BIN, "is 2",
                         "object %s" % U_LONE, "is 3")
        vol.dev.close()
        r = tool("fsck.tsfs", "--relink", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        self.assertEqual([e.vobjid for e in vol.object(U_DOC).links()],
                         [NIL_UUID, vobj(5)])
        self.assertEqual(vol.object(U_LONE).refcnt, 2)
        self.assertEqual(vol.garbage(), [])
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))

    def test_relink_to_garbage(self):
        """An object whose count falls to zero goes on the garbage list;
        one never referred to does not."""
        path = self.built()
        vol = self.reopen(path, writable=True)
        items = vol.objects()
        extra = []
        for n, count in ((0x10, 2), (0x11, 0)):
            u = uuid.UUID("01a0d6d1-0000-7000-8000-0000000000%02x" % n)
            o = ObjectBlock.new(vol, vol.alloc_blk(), u)
            o.refcnt = count
            o.mseg_write(OB_META, b'{"name":"x"}')
            o.store()
            extra.append((u, o.blk))
        vol.btree.rebuild(items + extra)
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "object %s" % extra[0][0], "is 2")
        vol.dev.close()
        r = tool("fsck.tsfs", "--relink", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        gone, never = vol.object(extra[0][0]), vol.object(extra[1][0])
        self.assertEqual((gone.refcnt, never.refcnt), (0, 0))
        self.assertTrue(gone.flags & F_GARBAGE)
        self.assertFalse(never.flags & F_GARBAGE)
        self.assertEqual([u for u, _ in vol.garbage()], [extra[0][0]])
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))

    def test_garbage_list_disagrees(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        o = vol.object(U_LONE)
        o.flags = o.flags | F_GARBAGE
        o.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "marked as garbage but is not on the garbage list")
        vol.gctree.build([(U_LONE, o.blk), (U_BIN, vol.lookup(U_BIN))])
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "%s is on it but not marked" % U_BIN)

    def test_holds(self):
        """The count is the links and the holds; a hold more than the count
        is an error; --relink keeps the holds and gives the superblock's
        roots theirs."""
        path = self.built()
        vol = self.reopen(path, writable=True)
        lone = vol.object(U_LONE)
        lone.pins = 3                   # held from elsewhere
        lone.store()
        box = vol.object(U_BOX)
        box.pins = 0                    # the root without its hold
        box.refcnt = 0
        box.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "object %s: holds 3, more than its "
                         "reference count 2" % U_LONE,
                         "the link tables and 3 hold(s) make it 5")
        self.assertFound(rep.warnings, "object %s: the superblock names it 1 "
                         "time(s) but it has 0 hold(s)" % U_BOX)
        vol.dev.close()
        r = tool("fsck.tsfs", "--relink", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        lone, box = vol.object(U_LONE), vol.object(U_BOX)
        self.assertEqual((lone.pins, lone.refcnt), (3, 5))
        self.assertEqual((box.pins, box.refcnt), (1, 1))
        self.assertFalse(box.flags & F_GARBAGE)
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))

    def test_link_table_order(self):
        """Entries in the order of their raw bytes; one out of it is an
        error, a repeated non-zero vobjid a warning."""
        path = self.built()
        vol = self.reopen(path, writable=True)
        doc = vol.object(U_DOC)
        es = [LinkEntry(NIL_UUID, U_LONE, 1), LinkEntry(NIL_UUID, U_LONE, 256)]
        doc.set_table(OB_LINK, OB_NLINK, es)       # rid order, not byte order
        doc.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "the link table is not in order at entry 1")
        doc.set_table(OB_LINK, OB_NLINK, sort_links(
            [LinkEntry(vobj(5), U_BIN, 1), LinkEntry(vobj(5), U_LONE, 1)]))
        doc.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.warnings, "vobjid %s is in the link table twice"
                         % vobj(5))

    def test_a_volume_before_link_tables(self):
        """The metadata with its structural keys, no "records", no link
        table and counts that nothing explains: noted, not an error."""
        vol = self.make()
        blk = vol.alloc_blk()
        o = ObjectBlock.new(vol, blk, U_DOC)
        o.refcnt = 2
        o.mseg_write(objmod.OB_META, b'{"name":"","refCount":2,"recordCount":0}')
        o.store()
        vol.btree.build([(U_DOC, blk)])
        vol.flush()
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))
        self.assertFound(rep.notes, "before stages T4/T5")


class TestOrphans(Base):
    """The orphan tree: objects deleted while open and link tables marked
    to be made again, as a cut in the power leaves them."""

    def power_cut(self):
        """A volume with no roots, then as the kernel leaves it: the box
        deleted while open (out of the index, on the orphan tree), and the
        document part way through making its link table again (the table
        emptied, the counts moved with it, marked, on the orphan tree)."""
        path = self.built(roots={})
        vol = self.reopen(path, writable=True)
        items = dict(vol.objects())
        box = vol.object(U_BOX)
        self.assertEqual(box.refcnt, 0)
        box.flags = box.flags | F_ORPHAN
        box.store()
        doc = vol.object(U_DOC)
        for e in doc.links():
            t = vol.object(e.target)
            t.refcnt = t.refcnt - 1
            t.store()
        doc = vol.object(U_DOC)
        doc.set_table(OB_LINK, OB_NLINK, [])
        doc.flags = doc.flags | F_RELINK
        doc.store()
        blk_box = items.pop(U_BOX)
        vol.btree.rebuild(sorted(items.items(), key=lambda kv: kv[0].bytes))
        vol.ortree.build([(U_BOX, blk_box), (U_DOC, items[U_DOC])])
        vol.flush()
        vol.dev.close()
        return path, blk_box

    def test_check_and_fix(self):
        path, blk_box = self.power_cut()
        vol = self.reopen(path)
        c, rep = self.check(vol)
        self.assertEqual(rep.errors, [])
        self.assertFound(rep.warnings, "orphan %s: was deleted while open" % U_BOX,
                         "object %s: its link table is marked" % U_DOC)
        self.assertEqual(rep.stats["orphans"], 1)
        self.assertEqual(c.tree_nodes[TREE_ORPHAN], 1)
        self.assertIn(blk_box, c.claims)            # still its own, not leaked
        r = tool("fsck.tsfs", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol.dev.close()

        r = tool("fsck.tsfs", "--fix", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))
        self.assertEqual(vol.sb.orphan_start, 0)
        self.assertEqual(vol.orphans(), [])
        self.assertIsNone(vol.lookup(U_BOX))
        # given back (and perhaps taken again by a node of a tree)
        self.assertNotIn(str(U_BOX), c.claims.get(blk_box, ""))
        # the box's links went: the document fell to zero; the document's
        # table was made again, so the byte object and the lone one are
        # counted by it alone
        doc, binobj, lone = (vol.object(U_DOC), vol.object(U_BIN),
                             vol.object(U_LONE))
        self.assertEqual([(e.vobjid, e.target) for e in doc.links()],
                         [(NIL_UUID, U_LONE), (vobj(5), U_BIN)])
        self.assertFalse(doc.flags & F_RELINK)
        self.assertEqual((doc.refcnt, binobj.refcnt, lone.refcnt), (0, 1, 1))
        self.assertTrue(doc.flags & F_GARBAGE)
        self.assertFalse(binobj.flags & F_GARBAGE)
        self.assertEqual([u for u, _ in vol.garbage()], [U_DOC])

    def test_relink_keeps_the_orphan(self):
        """--relink makes the marked table again but leaves the object
        deleted while open, whose links still count."""
        path, _blk = self.power_cut()
        r = tool("fsck.tsfs", "--relink", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        vol = self.reopen(path)
        self.assertEqual([u for u, _ in vol.orphans()], [U_BOX])
        self.assertFalse(vol.object(U_DOC).flags & F_RELINK)
        self.assertEqual((vol.object(U_DOC).refcnt, vol.object(U_BIN).refcnt,
                          vol.object(U_LONE).refcnt), (1, 2, 2))
        c, rep = self.check(vol)
        self.assertEqual(rep.errors, [])
        self.assertFound(rep.warnings, "orphan %s: was deleted while open" % U_BOX)

    def test_tree_disagrees(self):
        path, blk_box = self.power_cut()
        vol = self.reopen(path, writable=True)
        # the document's entry at the wrong block, the lone one marked but
        # not on the tree, the box put back in the index
        vol.ortree.rebuild([(U_BOX, blk_box), (U_DOC, vol.lookup(U_BIN))])
        lone = vol.object(U_LONE)
        lone.flags = lone.flags | F_RELINK
        lone.store()
        vol.btree.rebuild(sorted(vol.objects() + [(U_BOX, blk_box)],
                                 key=lambda kv: kv[0].bytes))
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "orphan tree: %s points at block %d, the "
                         "index at %d" % (U_DOC, vol.lookup(U_BIN),
                                          vol.lookup(U_DOC)),
                         "object %s: is marked as deleted while open but is "
                         "in the index" % U_BOX)
        self.assertFound(rep.warnings, "object %s: is marked for its link "
                         "table to be made again but is not on the orphan "
                         "tree" % U_LONE)
        # an entry for an object with nothing to finish
        vol.ortree.rebuild([(U_BIN, vol.lookup(U_BIN))])
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.warnings, "holds %s, which has nothing to be "
                         "finished" % U_BIN)


class TestMetadata(Base):

    def test_records_anywhere(self):
        """"records" first as the kernel puts it, or anywhere else."""
        path = self.built()
        vol = self.reopen(path, writable=True)
        o = vol.object(U_DOC)
        m = json.loads(o.meta().decode("utf-8"))
        recs = m.pop("records")
        m["records"] = recs
        o.mseg_write(OB_META, json.dumps(m).encode("utf-8"))
        o.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))
        # the structural keys kept in the text
        m["refCount"] = 1
        o.mseg_write(OB_META, json.dumps(m).encode("utf-8"))
        o.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.warnings, "object %s: the metadata keeps refCount"
                         % U_DOC)
        # "records" that do not name the records: taken in rid order
        m.pop("refCount")
        m["records"] = recs[:1]
        o.mseg_write(OB_META, json.dumps(m).encode("utf-8"))
        o.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertEqual(rep.errors, [])
        self.assertFound(rep.warnings, "the kernel takes the records in rid order")
        # a rid twice: a record that cannot be reached
        m["records"] = [recs[0], recs[0]]
        o.mseg_write(OB_META, json.dumps(m).encode("utf-8"))
        o.store()
        vol.flush()
        c, rep = self.check(vol)
        self.assertFound(rep.errors, "\"records\" names a rid twice")

    def test_not_an_object(self):
        """A text that is JSON but not an object is kept and given back as
        it is; the records are in rid order with the default types."""
        d = self.tmp.path("in")
        os.makedirs(d)
        u = uuid.UUID("01a0d6d1-0000-7000-8000-000000000020")
        with open(os.path.join(d, "%s.json" % u), "wb") as f:
            f.write(b'["just", "a", "list"]\n')
        with open(os.path.join(d, "%s_0.xtad" % u), "wb") as f:
            f.write(link(U_AWAY, vobj(1)).encode())
        with open(os.path.join(d, "%s_1.bin" % u), "wb") as f:
            f.write(b"\1\2\3")
        vol = self.make()
        self.assertEqual(transfer.build(vol, [d]), (1, []))
        vol.flush()
        o = vol.object(u)
        self.assertEqual(o.meta(), b'["just", "a", "list"]\n')
        self.assertEqual(len(o.links()), 1)
        order, named = metamod.order_of(o.meta(), o.placements())
        self.assertEqual([(e.rid, rt) for e, rt, _s in order], [(1, 1), (2, 15)])
        self.assertFalse(named)
        c, rep = self.check(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))
        self.assertFound(rep.notes, "JSON but not an object")
        out = self.tmp.path("out")
        problems = []
        transfer.export(vol, out, problems)
        with open(os.path.join(out, "%s.json" % u), "rb") as f:
            self.assertEqual(f.read(), b'["just", "a", "list"]\n')
        self.assertTrue(os.path.exists(os.path.join(out, "%s_1.bin" % u)))

    def test_dates_only_when_set(self):
        path = self.built()
        vol = self.reopen(path, writable=True)
        o = vol.object(U_LONE)
        o.read = 0
        o.store()
        vol.flush()
        out = self.tmp.path("out")
        transfer.export(vol, out)
        with open(os.path.join(out, "%s.json" % U_LONE), "rb") as f:
            raw = f.read()
        self.assertIn(b'"makeDate":', raw)
        self.assertNotIn(b'"accessDate"', raw)


class TestLevels(Base):

    def three_levels(self):
        vol = self.make()
        n = MAX_KEYS * (MAX_KEYS + 1) + 10
        vol.btree.build([(uuid.UUID(int=(1 << 120) + i), 7) for i in range(n)])
        return vol

    def levels(self, vol):
        vol.maps = None
        vol.load_maps()
        c = Checker(vol)
        c.check_tree(TREE_OBJ)
        return c.rep

    def test_levels(self):
        vol = self.three_levels()
        rep = self.levels(vol)
        self.assertEqual((rep.errors, rep.warnings), ([], []))

        # a root at level 1 over internal nodes, as kernels before stage T5
        # made it: a warning
        root = vol.sb.objtbl_start
        node = vol.btree.read_node(root)
        node.level = 1
        vol.btree.write_node(root, node)
        rep = self.levels(vol)
        self.assertEqual(rep.errors, [])
        self.assertFound(rep.warnings, "internal node(s) %d are at a level "
                         "below their height" % root)

        # any other level is wrong
        node.level = 3
        vol.btree.write_node(root, node)
        rep = self.levels(vol)
        self.assertFound(rep.errors, "an internal node at level 3, 2 above its "
                         "leaves")
        node.level = 2
        vol.btree.write_node(root, node)
        child = node.child0
        inner = vol.btree.read_node(child)
        inner.level = 0
        vol.btree.write_node(child, inner)
        rep = self.levels(vol)
        self.assertFound(rep.errors, "object index node %d: an internal node "
                         "at level 0" % child)


if __name__ == "__main__":
    unittest.main()
