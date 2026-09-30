"""The metadata text of an object, the TADjs file names, and the links
an xmlTAD record holds.

The metadata an object keeps is the TADjs `{uuid}.json` without its
structural keys: the reference count and the three dates live in the
object block, and the record count is the length of "records", the list
that gives each record's rid, type and subtype in the order of the
records (design 11.5):

    "records": [{"rid": 1, "rt": 1, "sub": 0}, ...]

The text is edited as the kernel edits it, member by member in place:
a member taken out goes with the comma that follows it (or, for the
last one, the comma before it), and a member that is not there yet is
put first, right after the opening brace. So the kernel's stored text
has "records" first once the object has records; a text written by
other means may have it anywhere. A text that is not a JSON object is
kept and given back as it is, with no "records"; its records are then
in rid order, an xmlTAD record of type 1 and a byte record of type 15.

Read out (ts_obj_get_meta), the structural keys go in first: refCount,
recordCount, then the three dates as ISO 8601 UTC to the second, each
only when its time is not zero.
"""

import calendar
import json
import re
import time
import uuid

from .layout import TRON_EPOCH
from .obj import EXT_LEN, ICON_REC, REC_BIN, REC_XTAD, MAX_REC

STRUCT_KEYS = ("refCount", "recordCount", "makeDate", "updateDate",
               "accessDate")
DATE_KEYS = (("makeDate", "made"), ("updateDate", "updated"),
             ("accessDate", "read"))

RT_XTAD = 1                             # the record type of an xmlTAD record
RT_BIN_DEFAULT = 15

NS = 1000000000


# ---------------------------------------------------------------- dates

_ISO = re.compile(r"^\s*(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2})"
                  r"(?:\.(\d{1,9}))?)?\s*(Z|[+-]\d{2}:?\d{2})?\s*$")


def parse_date(text):
    """Nanoseconds since 1985-01-01 UTC from ISO 8601, or None."""
    if not isinstance(text, str):
        return None
    m = _ISO.match(text)
    if m is None:
        return None
    y, mo, d, h, mi = (int(m.group(i)) for i in range(1, 6))
    s = int(m.group(6) or 0)
    frac = m.group(7) or ""
    try:
        t = calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))
    except (ValueError, OverflowError):
        return None
    tz = m.group(8)
    if tz and tz != "Z":
        sign = -1 if tz[0] == "-" else 1
        hh, mm = int(tz[1:3]), int(tz[-2:])
        t -= sign * (hh * 3600 + mm * 60)
    t -= TRON_EPOCH
    if t < 0:
        return 0
    return t * NS + int((frac + "000000000")[:9])


def format_date(ns):
    """ISO 8601 UTC to the second: "YYYY-MM-DDTHH:MM:SSZ"."""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ",
                         time.gmtime(ns // NS + TRON_EPOCH))


# ---------------------------------------------------------------- JSON

def _first_pairs(pairs):
    """An object's members, the first of a name winning, as the kernel's
    js_get finds them."""
    out = {}
    for k, v in pairs:
        if k not in out:
            out[k] = v
    return out


def _text(raw):
    if isinstance(raw, (bytes, bytearray)):
        return bytes(raw).decode("utf-8", "replace")
    return raw


def loads(raw):
    """The metadata as an ordered dict, or raises ValueError."""
    if isinstance(raw, (bytes, bytearray)):
        raw = bytes(raw).decode("utf-8")
    d = json.loads(raw, object_pairs_hook=_first_pairs)
    if not isinstance(d, dict):
        raise ValueError("the metadata is not a JSON object")
    return d


def is_json(raw):
    """Whether the text is JSON at all."""
    try:
        json.loads(_text(raw))
    except ValueError:
        return False
    return True


_BLANKS = b" \t\r\n"


def _skip_blanks(t, i):
    while i < len(t) and t[i] in _BLANKS:
        i += 1
    return i


def first_value(t):
    """The first JSON value of a text, as the kernel's js_span takes it
    (what follows it is not looked at), or raises ValueError."""
    s = _text(t)
    return json.JSONDecoder(object_pairs_hook=_first_pairs).raw_decode(
        s, _lead(s))[0]


def _lead(s):
    """Where a text starts after its leading blanks."""
    i = 0
    while i < len(s) and s[i] in " \t\r\n":
        i += 1
    return i


def is_object(raw):
    """Whether the kernel takes the text for a JSON object."""
    t = bytes(raw)
    i = _skip_blanks(t, 0)
    if i >= len(t) or t[i:i + 1] != b"{":
        return False
    try:
        return isinstance(first_value(t), dict)
    except ValueError:
        return False


def _string_end(t, i):
    """Past the closing quote of the string that opens at t[i]."""
    i += 1
    while i < len(t):
        c = t[i]
        if c == 0x5C:
            i += 2
            continue
        if c == 0x22:
            return i + 1
        i += 1
    return len(t)


def _value_end(t, i):
    """Past the end of the value that starts at t[i]."""
    c = t[i:i + 1]
    if c == b'"':
        return _string_end(t, i)
    if c in (b"{", b"["):
        depth = 0
        while i < len(t):
            c = t[i]
            if c == 0x22:
                i = _string_end(t, i)
                continue
            if c in (0x7B, 0x5B):
                depth += 1
            elif c in (0x7D, 0x5D):
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        return len(t)
    while i < len(t) and t[i] not in b",}] \t\r\n":
        i += 1
    return i


def members(raw):
    """The members of the outer object of a text the kernel takes for an
    object: [(name as bytes, where its opening quote is, where its value
    starts, where it ends)] in the order they stand."""
    t = bytes(raw)
    i = _skip_blanks(t, 0) + 1
    out = []
    while True:
        i = _skip_blanks(t, i)
        if i >= len(t) or t[i:i + 1] != b'"':
            break
        ks = i
        ke = _string_end(t, i)
        i = _skip_blanks(t, ke)
        if t[i:i + 1] != b":":
            break
        i = _skip_blanks(t, i + 1)
        if i >= len(t):
            break
        ve = _value_end(t, i)
        out.append((t[ks + 1:ke - 1], ks, i, ve))
        i = _skip_blanks(t, ve)
        if t[i:i + 1] != b",":
            break
        i += 1
    return out


def _member(t, key):
    k = key.encode("utf-8")
    for name, ks, vs, ve in members(t):
        if name == k:
            return ks, vs, ve
    return None


def jx_del(raw, key):
    """The text with a member taken out, and the comma that went with it."""
    t = bytes(raw)
    if not is_object(t):
        return t
    m = _member(t, key)
    if m is None:
        return t
    frm, _vs, to = m
    k = _skip_blanks(t, to)
    if k < len(t) and t[k:k + 1] == b",":
        to = k + 1
    else:
        k = frm - 1
        while k > 0 and t[k] in _BLANKS:
            k -= 1
        if t[k:k + 1] == b",":
            frm = k
    return t[:frm] + t[to:]


def jx_set(raw, key, val):
    """The text with a member given the value `val` (JSON text): replaced
    where it stands, or put first. None when the text is not an object."""
    t = bytes(raw)
    if not is_object(t):
        return None
    m = _member(t, key)
    if m is not None:
        _ks, vs, ve = m
        return t[:vs] + val + t[ve:]
    op = t.index(b"{")
    n = _skip_blanks(t, op + 1)
    empty = n < len(t) and t[n:n + 1] == b"}"
    put = b'"' + key.encode("utf-8") + b'":' + val + (b"" if empty else b",")
    return t[:op + 1] + put + t[op + 1:]


def strip_text(raw):
    """The text without its structural keys, as the kernel keeps it."""
    t = bytes(raw)
    if not is_object(t):
        return t
    for k in STRUCT_KEYS:
        t = jx_del(t, k)
    return t


def order_text(records):
    """"records" as the kernel writes it: [{"rid":1,"rt":1,"sub":0},...]."""
    return ("[" + ",".join('{"rid":%d,"rt":%d,"sub":%d}'
                           % (r["rid"], r["rt"], r["sub"]) for r in records)
            + "]").encode("ascii")


def stored_text(raw, records):
    """The text an object made from this one keeps once its records are
    there: the structural keys and any "records" taken out, then, when
    there are records, "records" put in (first). A text that is not an
    object is kept as it is."""
    t = bytes(raw)
    if not is_object(t):
        return t
    t = jx_del(strip_text(t), "records")
    if records:
        t = jx_set(t, "records", order_text(records))
    return t


def date_text(ns):
    """A time of the object block as quoted ISO 8601, or b"" for none."""
    if ns == 0:
        return b""
    try:
        return ('"%s"' % format_date(ns)).encode("ascii")
    except (ValueError, OverflowError, OSError):
        return b""


def synth(raw, refcnt, nrec, made, updated, read):
    """The text as ts_obj_get_meta gives it: the structural keys put in
    from the object block, or the text as it is when it is not an
    object."""
    t = bytes(raw)
    if not is_object(t):
        return t
    for key, ns in (("accessDate", read), ("updateDate", updated),
                    ("makeDate", made)):
        v = date_text(ns)
        if v:
            t = jx_set(t, key, v)
    t = jx_set(t, "recordCount", b"%d" % nrec)
    t = jx_set(t, "refCount", b"%d" % refcnt)
    return t


def strip(d):
    """The metadata without its structural keys."""
    return dict((k, v) for k, v in d.items() if k not in STRUCT_KEYS)


def structural(d):
    """The structural keys a text carries."""
    return [k for k in STRUCT_KEYS if k in d]


# ---------------------------------------------------------------- record order

class _Num(str):
    """A JSON number kept as its text."""


def js_num(v, dflt):
    """A member's number as the kernel's js_get_num reads it: the whole
    part of the text, the fraction and any exponent not looked at; the
    default when it is not a number."""
    if not isinstance(v, _Num):
        return dflt
    neg = v.startswith("-")
    digits = re.match(r"\d*", v[1:] if neg else v).group(0)
    x = int(digits) if digits else 0
    return -x if neg else x


def order_parse(raw):
    """The "records" of a text as the kernel's order_parse reads it:
    [(rid, rt, sub)], or None when the text is not an object, has no
    "records" array, or an entry is not an object, has rid 0 or is past
    the most records an object has."""
    s = _text(raw)
    try:
        d = json.JSONDecoder(object_pairs_hook=_first_pairs,
                             parse_int=_Num, parse_float=_Num
                             ).raw_decode(s, _lead(s))[0]
    except (ValueError, IndexError):
        return None
    if not isinstance(d, dict) or not isinstance(d.get("records"), list):
        return None
    out = []
    for r in d["records"]:
        if len(out) >= MAX_REC or not isinstance(r, dict):
            return None
        rid = js_num(r.get("rid"), 0) & 0xFFFFFFFF
        rt = js_num(r.get("rt"), 1) & 0xFFFFFFFF
        sub = js_num(r.get("sub"), 0) & 0xFFFFFFFF
        if rid == 0:
            return None
        out.append((rid, rt, sub))
    return out


def default_rt(kind):
    return RT_XTAD if kind == REC_XTAD else RT_BIN_DEFAULT


def order_of(raw, places):
    """The records in order as [(PlaceEntry, rt, sub)], as the kernel's
    order_of makes it: the "records" of the text when it names as many
    records as the placement table has and each of them is there, else
    the table's own (rid) order with the default types. The second
    value says whether "records" was taken."""
    recs = order_parse(raw) if raw is not None else None
    byrid = {}
    for e in places:
        byrid.setdefault(e.rid, e)
    if recs is not None and len(recs) == len(places) \
            and all(rid in byrid for rid, _rt, _sub in recs):
        return [(byrid[rid], rt, sub) for rid, rt, sub in recs], True
    return [(e, default_rt(e.kind), 0) for e in places], False


def stored_records(d):
    """The "records" list of a parsed text, as [(rid, rt, sub)], or None
    when it has none or it is not in the right shape."""
    recs = d.get("records") if isinstance(d, dict) else None
    if not isinstance(recs, list):
        return None
    out = []
    for r in recs:
        if not isinstance(r, dict):
            return None
        rid, rt, sub = r.get("rid"), r.get("rt", RT_XTAD), r.get("sub", 0)
        if (not isinstance(rid, int) or isinstance(rid, bool)
                or not isinstance(rt, int) or not isinstance(sub, int)):
            return None
        out.append((rid, rt, sub))
    return out


def tessronos_rt(d, pos):
    """The record type "tessronos.records" gives position pos, or None."""
    tf = d.get("tessronos") if isinstance(d, dict) else None
    recs = tf.get("records") if isinstance(tf, dict) else None
    if not isinstance(recs, list):
        return None
    for r in recs:
        if isinstance(r, dict) and r.get("n") == pos and isinstance(r.get("rt"), int):
            return r["rt"]
    return None


def records_for(d, kinds):
    """The "records" list for records at positions 0.. of the given kinds
    (REC_XTAD, REC_BIN): rids 1, 2, 3 ... in position order; the type and
    subtype from a "records" list the text already has, else an xmlTAD
    record is type 1 and a byte record takes its type from
    "tessronos.records", 15 when that says nothing. A text that is not an
    object gives the types the kernel gives records in rid order."""
    if not isinstance(d, dict):
        return [{"rid": pos + 1, "rt": default_rt(kind), "sub": 0}
                for pos, kind in enumerate(kinds)]
    old = stored_records(d) or []
    out = []
    for pos, kind in enumerate(kinds):
        if pos < len(old):
            rt, sub = old[pos][1], old[pos][2]
        else:
            rt = RT_XTAD if kind == REC_XTAD else (tessronos_rt(d, pos)
                                                  or RT_BIN_DEFAULT)
            sub = 0
        out.append({"rid": pos + 1, "rt": rt, "sub": sub})
    return out


# ---------------------------------------------------------------- links

def _hexval(c):
    if 0x30 <= c <= 0x39:
        return c - 0x30
    if 0x61 <= c <= 0x66:
        return c - 0x61 + 10
    if 0x41 <= c <= 0x46:
        return c - 0x41 + 10
    return -1


def link_uuid(s):
    """A UUID from the first 36 bytes of s as the kernel's lk_uuid reads
    it (ts_str_to_uuid: 32 hex digits in text order, hyphens between
    bytes skipped), or None. The bytes are those of uuid.UUID.bytes."""
    s = bytes(s)
    if len(s) < 36:
        return None
    t = s[:36] + b"\0\0"
    out = bytearray(16)
    n = 0
    for i in range(16):
        while t[n] == 0x2D:
            n += 1
        hi = _hexval(t[n])
        lo = _hexval(t[n + 1]) if hi >= 0 else -1
        if lo < 0:
            return None
        out[i] = (hi << 4) | lo
        n += 2
    return uuid.UUID(bytes=bytes(out))


def _is_blank(c):
    return c in (0x20, 0x09, 0x0D, 0x0A)


def _attr(t, a, b, name):
    """Where the value of attribute `name` starts in the tag t[a:b]: a
    blank, the name, '=' and a double quote, or -1."""
    m = len(name)
    i = a
    while i + m + 3 < b:
        if (_is_blank(t[i]) and t[i + 1:i + 1 + m] == name
                and t[i + 1 + m] == 0x3D and t[i + 2 + m] == 0x22):
            return i + 3 + m
        i += 1
    return -1


NIL = uuid.UUID(int=0)


def scan_links(data, rid, out, limit=None):
    """The <link> elements of an xmlTAD text added to `out` as (vobjid,
    target, rid), as the kernel's lk_scan takes them: "<link" and a
    blank, the tag ending at the first '>'; the target from the 36 bytes
    the id attribute starts with, the vobjid (all zero when it is not
    there or not a UUID) from the vobjid attribute. An element whose
    vobjid is not zero and came before in `out` is left out. Answers
    False when `out` would hold more than `limit`."""
    t = bytes(data)
    n = len(t)
    i = 0
    while i + 5 < n:
        if t[i:i + 5] != b"<link" or not _is_blank(t[i + 5]):
            i += 1
            continue
        j = t.find(b">", i + 5)
        if j < 0:
            break
        v = _attr(t, i + 5, j, b"id")
        target = link_uuid(t[v:j]) if v >= 0 else None
        if target is not None:
            if limit is not None and len(out) >= limit:
                return False
            w = _attr(t, i + 5, j, b"vobjid")
            vo = link_uuid(t[w:j]) if w >= 0 else None
            if vo is None:
                vo = NIL
            if vo == NIL or all(e[0] != vo for e in out):
                out.append((vo, target, rid))
        i = j + 1
    return True


def extract_links(data):
    """The links of one xmlTAD text as [(vobjid, target)] in the order
    they stand, with scan_links' rules."""
    out = []
    scan_links(data, 0, out)
    return [(v, t) for (v, t, _r) in out]


# ---------------------------------------------------------------- file names

_UUID = r"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"
RE_JSON = re.compile(r"^" + _UUID + r"\.json$")
RE_REC = re.compile(r"^" + _UUID + r"_(\d+)\.(xtad|bin)$")
RE_ICON = re.compile(r"^" + _UUID + r"\.ico$")
RE_RES = re.compile(r"^" + _UUID + r"_(\d+)_(.+)$")


def classify(name):
    """What a file of a TADjs set is: (kind, UUID, details) or None.
    kind is "json", "rec" (position, REC_XTAD/REC_BIN), "res" (position,
    number, extension) or "icon"."""
    m = RE_JSON.match(name)
    if m:
        return "json", uuid.UUID(m.group(1)), None
    m = RE_REC.match(name)
    if m:
        kind = REC_XTAD if m.group(3) == "xtad" else REC_BIN
        return "rec", uuid.UUID(m.group(1)), (int(m.group(2)), kind)
    m = RE_ICON.match(name)
    if m:
        return "icon", uuid.UUID(m.group(1)), (ICON_REC, 0, "ico")
    m = RE_RES.match(name)
    if m:
        key = res_key("_%s_%s" % (m.group(2), m.group(3)))
        if key is None:
            return "bad", uuid.UUID(m.group(1)), "the extension is too long"
        return "res", uuid.UUID(m.group(1)), key
    return None


def res_key(tail):
    """(position, number, extension) of a resource from what follows the
    UUID in its name, as the kernel's res_key reads it: "_2_1.png" is
    (2, 1, "png"); a second part that is not a number keeps the whole of
    it as the extension under a number below 0 made from it. None when
    the extension does not fit its field."""
    b = tail.encode("utf-8")
    if not b.startswith(b"_"):
        return None
    i = 1
    rec = 0
    while i < len(b) and 0x30 <= b[i] <= 0x39:
        rec = rec * 10 + (b[i] - 0x30)
        i += 1
    if i == 1 or i >= len(b) or b[i:i + 1] != b"_":
        return None
    i += 1
    k = i
    res = 0
    while k < len(b) and 0x30 <= b[k] <= 0x39:
        res = res * 10 + (b[k] - 0x30)
        k += 1
    if k > i and b[k:k + 1] == b".":
        rest = b[k + 1:]
        res &= 0xFFFFFFFF
        if res >= 0x80000000:
            res -= 1 << 32
    else:
        h = 2166136261
        for c in b[i:]:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        res = -2 - (h & 0x3FFFFFFF)
        rest = b[i:]
    if len(rest) > EXT_LEN - 1:
        return None
    return rec, res, rest.decode("utf-8")


def res_name(obj_uuid, pos, resno, ext):
    """The file name of a resource, as the kernel's res_name spells it."""
    if pos == ICON_REC:
        return "%s.%s" % (obj_uuid, ext)
    if resno >= 0:
        return "%s_%d_%d.%s" % (obj_uuid, pos, resno, ext)
    return "%s_%d_%s" % (obj_uuid, pos, ext)


def rec_name(obj_uuid, pos, kind):
    return "%s_%d.%s" % (obj_uuid, pos, "bin" if kind == REC_BIN else "xtad")
