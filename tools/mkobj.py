#!/usr/bin/env python3
"""The system's files as real objects, made when the system is built.

A file the system brings with it (a face, a picture) is not taken in
when the system starts: it is on the disk as an object already, in the
form the common module xf_ would give it (design 18.20) -- metadata
with "tessronos.file", record 0 in xmlTAD naming the file, and the file's
bytes, unchanged, in record 1 -- owned by the system and linked from
its box.

  mkobj.py def <box uuid> <file ...>
      Writes etc/def/{uuid}.json and {uuid}_0.xtad for each file, and
      the box's record 0 with a link to each. What is written depends
      only on the files, so it is kept with the sources.

  mkobj.py rec <out dir> <file ...>
      Copies each file to <out dir>/{uuid}_1.bin, its record 1 on the
      disk. The object's metadata must say the file's size: when the
      file has changed, def is to be run again.

An object's UUID comes from the file's name, so the same file is the
same object in every build.
"""

import hashlib
import json
import os
import re
import shutil
import sys

TOP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEF = os.path.join(TOP, "etc", "def")
STAMP = "2026-09-26T00:00:00Z"

# The system's user and the administrators' group (include/ts/ob.h)
ACCESS = {
    "owner": "01a0d680-3aa0-75b3-bd7d-075e14217cd2",
    "group": "01a0d680-3aa1-7922-ae79-ba5cbbaa2037",
    "mode": "rwxr-xr-x",
    "attr": ["perm"],
    "acl": [],
    "records": [],
}

# extension: (media type, record type of record 1), as xf.c has them
KINDS = {
    ".ttf": ("font/ttf", 11),
    ".otf": ("font/otf", 11),
    ".ttc": ("font/collection", 11),
    ".pic": ("image/x-tessronos-picture", 15),
    ".png": ("image/png", 15),
    ".jpg": ("image/jpeg", 15),
    ".jpeg": ("image/jpeg", 15),
}


def uuid_of(seed):
    """A version 7 UUID whose time is fixed and whose rest is the seed's hash."""
    h = bytearray(hashlib.sha1(seed.encode("utf-8")).digest()[:16])
    h[0:6] = bytes.fromhex("01a0d8c45300")
    h[6] = 0x70 | (h[6] & 0x0F)
    h[8] = 0x80 | (h[8] & 0x3F)
    s = h.hex()
    return "%s-%s-%s-%s-%s" % (s[:8], s[8:12], s[12:16], s[16:20], s[20:])


def file_uuid(path):
    return uuid_of("tessronos-sys-file:" + os.path.basename(path))


def xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def kind_of(path):
    ext = os.path.splitext(path)[1].lower()
    if ext not in KINDS:
        sys.exit("mkobj: %s: not a kind of file the system brings" % path)
    return KINDS[ext]


def meta_of(path):
    name = os.path.basename(path)
    mediatype, rt = kind_of(path)
    return {
        "name": os.path.splitext(name)[0],
        "relationship": [],
        "linktype": False,
        "makeDate": STAMP,
        "updateDate": STAMP,
        "accessDate": STAMP,
        "periodDate": None,
        "refCount": 1,
        "recordCount": 2,
        "editable": False,
        "deletable": False,
        "readable": True,
        "maker": "TessronOS",
        "applist": {},
        "tessronos": {
            "file": {
                "name": name,
                "mediatype": mediatype,
                "size": os.path.getsize(path),
                "source": "system",
            },
            "records": [{"n": 0, "rt": 1}, {"n": 1, "rt": rt}],
            "access": ACCESS,
        },
    }


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def link(uuid, y):
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="8" vobjtop="%d" vobjright="248" '
            'vobjbottom="%d" height="25" chsz="14" frcol="#000000" chcol="#000000" '
            'tbcol="#ffffff" bgcol="#ffffff" pictdisp="true" namedisp="true" framedisp="true"/>\n'
            % (uuid, uuid_of("tessronos-sys-link:" + uuid), y, y + 25))


def cmd_def(box, files):
    boxrec = os.path.join(DEF, box + "_0.xtad")
    if not os.path.exists(boxrec):
        sys.exit("mkobj: no box %s in etc/def" % box)
    head = open(boxrec, encoding="utf-8").read()
    m = re.search(r'filename="([^"]*)"', head)
    boxname = m.group(1) if m else ""
    links = []
    for i, path in enumerate(sorted(files, key=lambda p: os.path.basename(p).lower())):
        u = file_uuid(path)
        name = os.path.basename(path)
        write(os.path.join(DEF, u + ".json"),
              json.dumps(meta_of(path), ensure_ascii=False, indent=2) + "\n")
        write(os.path.join(DEF, u + "_0.xtad"),
              '<tad version="1.0" encoding="UTF-8" filename="%s"><document><p>%s</p>'
              '</document></tad>' % (xml(os.path.splitext(name)[0]), xml(name)))
        links.append(link(u, 8 + i * 32))
    write(boxrec, '<tad version="1.0" encoding="UTF-8" filename="%s">\n<figure>\n%s</figure>\n</tad>\n'
          % (xml(boxname), "".join(links)))


def cmd_rec(out, files):
    os.makedirs(out, exist_ok=True)
    for path in files:
        u = file_uuid(path)
        meta = os.path.join(DEF, u + ".json")
        if not os.path.exists(meta):
            sys.exit("mkobj: %s has no object in etc/def (run mkobj.py def)" % path)
        size = json.load(open(meta, encoding="utf-8"))["tessronos"]["file"]["size"]
        if size != os.path.getsize(path):
            sys.exit("mkobj: %s has changed since its object was made (run mkobj.py def)" % path)
        shutil.copyfile(path, os.path.join(out, u + "_1.bin"))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("def", "rec"):
        sys.exit(__doc__)
    if sys.argv[1] == "def":
        cmd_def(sys.argv[2], sys.argv[3:])
    else:
        cmd_rec(sys.argv[2], sys.argv[3:])


if __name__ == "__main__":
    main()
