"""Host side reader and writer for the TSFS native volume, version 4
(design 11.6).

The format is the one peripheral_kernel/fs/tsfsblk.c, tsfsjrnl.c,
tsfsbtree.c and tsfsobj.c put on the medium: little endian throughout, a
block of 4096 bytes made of eight sectors of 512. A volume normally sits
in a GPT partition, so everything here works on a byte range of an image
file rather than on a whole file.

    layout      the superblock, the geometry, time and UUID helpers
    head        the common head of a management block
    groups      group heads, free maps, allocation
    journal     the write ahead log: scan, replay, write
    btree       the object index, the garbage list, the orphan tree
    obj         the object block and its parts
    meta        the metadata text, TADjs file names, links in xmlTAD
    volume      a whole volume
    check       the checker behind fsck.tsfs
    repair      fsck.tsfs --fix (with the orphan tree) and --relink
    transfer    TADjs files in and out

Pure Python 3, standard library only.
"""

from .error import TsfsError
from .crc import crc32c
from .image import (BlockImage, Partition, read_partitions, find_tsfs_partition,
                    TSFS_TYPE_GUID)
from .layout import (BLOCK_SIZE, SECTOR_SIZE, MAGIC, VERSION, LABEL_MAX,
                     MIN_BLOCKS, Superblock, gen_uuid7, layout, owner_of)
from .btree import BTree, Node, MAX_KEYS, TREE_OBJ, TREE_GC, TREE_ORPHAN
from .obj import ObjectBlock, Place, PlaceEntry, ResEntry, LinkEntry
from .journal import Journal
from .volume import Volume
from .check import Checker, Report

__all__ = [
    "TsfsError", "crc32c",
    "BlockImage", "Partition", "read_partitions", "find_tsfs_partition",
    "TSFS_TYPE_GUID",
    "BLOCK_SIZE", "SECTOR_SIZE", "MAGIC", "VERSION", "LABEL_MAX",
    "MIN_BLOCKS", "Superblock", "gen_uuid7", "layout", "owner_of",
    "BTree", "Node", "MAX_KEYS", "TREE_OBJ", "TREE_GC", "TREE_ORPHAN",
    "ObjectBlock", "Place", "PlaceEntry", "ResEntry", "LinkEntry",
    "Journal", "Volume", "Checker", "Report",
]
