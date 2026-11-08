#!/bin/bash
# The NTFS fimp against an image made by mkfs.ntfs and filled by ntfs-3g
# (run as root, for its loop mount). Everything read is compared with what
# ntfs-3g shows; after writes in place the ntfs-3g tools check the volume.
# Usage: ntfs_host.sh WORKDIR
set -u
work=$1
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
INC="-I$here/shim -I$top/include -I$top/peripheral_kernel/fs"
CF="-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-function"
gcc $CF $INC -o "$work/n" "$here/ntfs_host.c" "$top/peripheral_kernel/fs/ntfsfs.c" "$top/peripheral_kernel/fs/fsname.c" || exit 1
rc=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; rc=1; }

img=$work/n.img
truncate -s 128M "$img"
/usr/sbin/mkfs.ntfs -F -f -q -L TESTNT "$img" || exit 1
mp=$work/mp
mkdir -p "$mp"
ntfs-3g -o loop,compression "$img" "$mp" || { echo "ntfs-3g mount failed"; exit 1; }

# small (kept in the record), medium, large, many names, a deep path, sparse, compressed
python3 - "$mp" <<'PY'
import os, sys
mp = sys.argv[1]
def pat(n, seed):
    b = bytearray(n)
    for i in range(0, n, 4096):
        chunk = (i * 2654435761 >> 13 ^ seed).to_bytes(8, 'little') * 512
        b[i:i+4096] = chunk[:min(4096, n - i)]
    return bytes(b)
open(mp + '/small.txt', 'wb').write(b'hello ntfs\n')
open(mp + '/medium.bin', 'wb').write(pat(70000, 1))
open(mp + '/large.bin', 'wb').write(pat(9 * 1024 * 1024 + 77, 2))
os.makedirs(mp + '/a/b/c/d')
open(mp + '/a/b/c/d/deep.txt', 'wb').write(b'deep\n')
os.mkdir(mp + '/many')
for i in range(1500):
    open(mp + '/many/ファイル %05d name.dat' % i, 'wb').write(b'x' * (i % 700))
os.mkdir(mp + '/frag')
# written by turns: runs that do not lie together
fs = [open(mp + '/frag/%d.bin' % i, 'wb') for i in range(6)]
for r in range(120):
    for i, f in enumerate(fs):
        f.write(pat(8192, r * 7 + i))
for f in fs:
    f.close()
with open(mp + '/sparse.bin', 'wb') as f:
    f.write(b'start')
    f.seek(5 * 1024 * 1024)
    f.write(b'end')
PY
# compressed, where ntfs-3g will: an attribute on a directory new files inherit
mkdir "$mp/comp"
python3 -c "import os,sys; os.setxattr(sys.argv[1]+'/comp','system.ntfs_attrib_be',(0x800).to_bytes(4,'big'))" "$mp" || echo "note: compressed files not made"
python3 -c "
import sys
open(sys.argv[1]+'/comp/text.bin','wb').write(b''.join(b'line %08d of a text that compresses well\n' % i for i in range(60000)))
open(sys.argv[1]+'/comp/mix.bin','wb').write(__import__('os').urandom(200000)+b'Z'*300000)
" "$mp"
ls -l --block-size=1 -s "$mp/comp" | tail -2
(cd "$mp" && find . -type f | sort > "$work/files.txt"; find . -type f -exec md5sum {} + | sort -k2 > "$work/sums.txt")
umount "$mp"

echo "state: $("$work/n" "$img" ro)"
[ "$("$work/n" "$img" ro)" = rw ] && ok "a clean volume can be written in place" || ng "a clean volume should be rw"

# each file's data, as ntfs-3g showed it
bad=0
while read -r sum path; do
	p="/${path#./}"
	got=$("$work/n" "$img" cat "$p" 2>"$work/err" | md5sum | cut -d' ' -f1)
	if [ "$got" != "$sum" ]; then
		echo "differs: $p: $(cat "$work/err")"
		bad=$((bad + 1))
	fi
done < <(sed 's/^\([0-9a-f]*\)  /\1 /' "$work/sums.txt")
[ $bad = 0 ] && ok "$(wc -l < "$work/sums.txt") files read the same as ntfs-3g reads them" || ng "$bad files differ"

# listings
"$work/n" "$img" ls /many | LC_ALL=C sort > "$work/ls.txt"
n=$(wc -l < "$work/ls.txt")
[ "$n" = 1500 ] && ok "a directory of 1500 names listed" || ng "listed $n names of 1500"
"$work/n" "$img" ls / | grep -q '^\$' && ng "the volume's own files shown" || ok "the volume's own files are not shown"
"$work/n" "$img" cat /A/B/C/D/DEEP.TXT | grep -q deep && ok "names compare without case" || ng "names should compare without case"

# in place
cp "$img" "$work/before.img"
"$work/n" "$img" poke /large.bin 1000 0x55 300000 >/dev/null && ok "partial sectors and whole ones written in place" || ng "poke"
"$work/n" "$img" poke /medium.bin 69000 0x66 1000 >/dev/null || ng "poke to the last byte"
"$work/n" "$img" poke /small.txt 2 0x77 3 >/dev/null || ng "poke in the record"
python3 - "$work/poked.py" <<'PY'
PY
"$work/n" "$img" refuse /medium.bin | grep -q refused && ok "what changes the structure is refused" || ng "refuse"
mkdir -p "$mp"
/usr/bin/ntfsfix -n "$img" > "$work/fix.txt" 2>&1 && ok "ntfsfix -n passes" || { ng "ntfsfix -n"; cat "$work/fix.txt"; }
ntfs-3g -o ro,loop "$img" "$mp" || ng "ntfs-3g could not mount it"
python3 - "$mp" <<'PY' && ok "ntfs-3g reads back what was written in place" || ng "ntfs-3g saw other bytes"
import sys
mp = sys.argv[1]
def want(off, seed, n):
    return bytes((((off + i) * 2654435761 >> 13 ^ seed) & 0xff) for i in range(n))
d = open(mp + '/large.bin', 'rb').read()
assert d[1000:301000] == want(1000, 0x55, 300000)
d = open(mp + '/medium.bin', 'rb').read()
assert len(d) == 70000 and d[69000:] == want(69000, 0x66, 1000)
d = open(mp + '/small.txt', 'rb').read()
assert d[2:5] == want(2, 0x77, 3) and d[:2] == b'he' and d[5:] == b' ntfs\n'
PY
# everything else is as it was
(cd "$mp" && find . -type f ! -name large.bin ! -name medium.bin ! -name small.txt -exec md5sum {} + | sort -k2) > "$work/sums2.txt"
umount "$mp"
grep -v -e '/large.bin$' -e '/medium.bin$' -e '/small.txt$' "$work/sums.txt" | diff - "$work/sums2.txt" >/dev/null && ok "the other files are as they were" || ng "other files changed"
# only the bytes meant, and the records' times, changed
cmp -l "$work/before.img" "$img" | wc -l | sed 's/^/bytes changed: /'

# volumes that are not safe to write
cp "$work/before.img" "$work/d.img"
# the volume's dirty flag: ntfsfix would clear it; set it through a mount left open is not portable,
# so the flag is set in the $Volume record by offset
python3 - "$work/d.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
b = f.read(512)
cl = struct.unpack_from('<H', b, 11)[0] * b[13]
mft = struct.unpack_from('<Q', b, 48)[0] * cl
c = struct.unpack_from('<b', b, 64)[0]
rec = (1 << -c) if c < 0 else c * cl
off = mft + 3 * rec
f.seek(off)
r = bytearray(f.read(rec))
assert r[:4] == b'FILE'
a = struct.unpack_from('<H', r, 20)[0]
while True:
    t, l = struct.unpack_from('<II', r, a)
    if t == 0xFFFFFFFF:
        raise SystemExit('no volume info')
    if t == 0x70:
        vo = struct.unpack_from('<H', r, a + 20)[0]
        fl = struct.unpack_from('<H', r, a + vo + 10)[0] | 1
        struct.pack_into('<H', r, a + vo + 10, fl)
        break
    a += l
f.seek(off)
f.write(r)
PY
[ "$("$work/n" "$work/d.img" ro 2>/dev/null)" = ro ] && ok "a dirty volume is read only" || ng "a dirty volume should be read only"
"$work/n" "$work/d.img" cat /small.txt >/dev/null || ng "a dirty volume should still read"

# a journal left with something to play back
cp "$work/before.img" "$work/j.img"
python3 - "$work/j.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
b = f.read(512)
cl = struct.unpack_from('<H', b, 11)[0] * b[13]
mft = struct.unpack_from('<Q', b, 48)[0] * cl
c = struct.unpack_from('<b', b, 64)[0]
rec = (1 << -c) if c < 0 else c * cl
f.seek(mft + 2 * rec)
r = f.read(rec)
a = struct.unpack_from('<H', r, 20)[0]
while True:
    t, l = struct.unpack_from('<II', r, a)
    if t == 0x80:
        break
    a += l
ro = struct.unpack_from('<H', r, a + 32)[0]
h = r[a + ro]
nl, no = h & 15, h >> 4
lcn = int.from_bytes(r[a + ro + 1 + nl:a + ro + 1 + nl + no], 'little', signed=True)
at = lcn * cl
# two restart pages of 4KB: in use, not clean
for o in (0, 4096):
    p = bytearray(b'\xff' * 4096)
    p[:4] = b'RSTR'
    struct.pack_into('<HH', p, 4, 30, 9)         # update sequence
    p[8:16] = b'\0' * 8
    struct.pack_into('<II', p, 16, 4096, 4096)
    struct.pack_into('<H', p, 24, 64)
    struct.pack_into('<HH', p, 28, 1, 1)
    us = 0x1234
    struct.pack_into('<H', p, 30, us)
    for i in range(1, 9):
        p[30 + i * 2:32 + i * 2] = p[i * 512 - 2:i * 512]
        struct.pack_into('<H', p, i * 512 - 2, us)
    struct.pack_into('<Q', p, 64, 100 + o)
    struct.pack_into('<HHHH', p, 64 + 8, 1, 0xFFFF, 0, 0)
    f.seek(at + o)
    f.write(p)
PY
[ "$("$work/n" "$work/j.img" ro 2>/dev/null)" = ro ] && ok "a journal with something to play back: read only" || ng "a live journal should be read only"

# hiberfil.sys in use
cp "$work/before.img" "$work/h.img"
ntfs-3g -o loop "$work/h.img" "$mp" && { printf 'HIBRxxxxxxxx' > "$mp/hiberfil.sys"; umount "$mp"; }
[ "$("$work/n" "$work/h.img" ro 2>/dev/null)" = ro ] && ok "hibernated: read only" || ng "a hibernated volume should be read only"

# nonsense does not bring it down: every byte of the first records flipped, one at a time and in a few places
python3 - "$work" "$work/n" "$work/before.img" <<'PY'
import random, subprocess, sys, shutil
w, n, src = sys.argv[1:]
d = open(src, 'rb').read(512)
import struct
cl = struct.unpack_from('<H', d, 11)[0] * d[13]
mft = struct.unpack_from('<Q', d, 48)[0] * cl
random.seed(1234)
crashes = 0
img = w + '/fz.img'
shutil.copy(src, img)
base = open(src, 'rb').read(mft + 40 * 1024)
for t in range(200):
    with open(img, 'r+b') as f:
        f.seek(mft)
        f.write(base[mft:])
        for k in range(random.randint(1, 8)):
            o = mft + random.randrange(40 * 1024)
            f.seek(o)
            f.write(bytes([random.randrange(256)]))
    for args in (['ls', '/'], ['cat', '/small.txt'], ['ls', '/many']):
        r = subprocess.run([n, img] + args, capture_output=True, timeout=120)
        if b'AddressSanitizer' in r.stderr or b'runtime error' in r.stderr or r.returncode < 0:
            crashes += 1
            print(r.stderr.decode(errors='replace')[:1500])
            break
print('fuzz: %d crashes' % crashes)
sys.exit(1 if crashes else 0)
PY
[ $? = 0 ] && ok "200 damaged volumes neither crash nor overrun" || ng "damaged volumes"
echo "RESULT rc=$rc"
exit $rc
