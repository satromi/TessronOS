"""What the four command line tools share: where the volume is, and how
its size is spelled.

A volume is either a whole file or a range of one. With neither --offset
nor --partition given, an image that carries a GPT with a TessronOS
partition is taken to mean that partition, and anything else the whole
file.
"""

import argparse
import os
import sys

from .error import TsfsError
from .image import BlockImage, find_tsfs_partition, read_partitions
from .layout import parse_uuid


def parse_size(text):
    """A size in bytes, with an optional K, M or G suffix."""
    text = text.strip()
    mult = 1
    if text and text[-1] in "kKmMgG":
        mult = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}[text[-1].lower()]
        text = text[:-1]
    try:
        value = int(text, 0)
    except ValueError:
        raise TsfsError("%r is not a size" % text)
    return value * mult


def add_volume_args(parser):
    """The options that say where the volume sits in the image."""
    parser.add_argument("--offset", metavar="BYTES",
                        help="byte offset of the volume in the image")
    parser.add_argument("--partition", metavar="N", type=int,
                        help="GPT partition to use, counting from 1")
    parser.add_argument("--whole", action="store_true",
                        help="use the whole file, partition table or not")


def locate(path, args):
    """(offset, length) of the volume the options name. A length of None
    means as far as the file goes."""
    if args.whole or not os.path.exists(path):
        if args.offset is not None:
            return parse_size(args.offset), None
        return 0, None
    if args.partition is not None:
        parts = read_partitions(path)
        for p in parts:
            if p.index == args.partition:
                return p.offset, p.length
        raise TsfsError("the image has no partition %d" % args.partition)
    if args.offset is not None:
        return parse_size(args.offset), None
    part = find_tsfs_partition(path)
    if part is not None:
        return part.offset, part.length
    return 0, None


def open_device(path, args, writable=False, length=None, create=False):
    """The block image the options name."""
    offset, plen = locate(path, args)
    if length is not None:
        plen = length
    return BlockImage(path, offset=offset, length=plen, writable=writable,
                      create=create)


def uuid_arg(text):
    """A UUID on the command line, in its text form."""
    u = parse_uuid(text.strip().lower())
    if u is None:
        raise argparse.ArgumentTypeError("%r is not a UUID" % text)
    return u


def fail(msg):
    """Say what went wrong and stop."""
    sys.stderr.write("%s: %s\n" % (os.path.basename(sys.argv[0]), msg))
    raise SystemExit(1)
