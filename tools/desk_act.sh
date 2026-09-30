#!/bin/bash
# The desktop started under QEMU and worked by a script of steps, as a
# user would with the mouse and the keyboard; the screen is dumped where
# the script says, and what the console says is checked.
#
#   tools/desk_act.sh STEPS [EXPECT...]
#
# Run from build_make after `make TARGET=_QEMU_VIRT_ DESKTOP=1 BUILD_ID=<id>`
# and a KTEST build that made the test disk. The environment names them:
#   ELF   the desktop kernel            (default tessronosdesk.elf)
#   DISK  the test disk it starts from  (default obj/_QEMU_VIRT__ktest/test_disk.img)
#   WAIT  seconds to let it come up     (default 150)
#   OUT   where the screen dumps go, as PNG when Pillow is there (default .)
#   (the machine has a network card on a multicast socket, as the tests' machine does)
#   TAG   a name for this run's files, so that runs side by side keep apart
#         (default desk$$): /tmp/$TAG.img, /tmp/$TAG.sock, /tmp/$TAG.log
#   KEEPLOG  a file the console log is copied to at the end (default: none)
#
# STEPS is a file, one step a line, positions in screen pixels:
#   at X Y        the pointer to X,Y (far to the top left first, then six
#                 pixels at a time: a USB mouse moves by what it is told only
#                 in small steps)
#   click / dclick / down / up     the left button
#   key K         a key (a QEMU sendkey name: ret, ctrl-s ...)
#   shot NAME     the screen dumped to $OUT/NAME.png (or .ppm)
#   sleep N       wait N seconds
#   # ...         a comment
# Each EXPECT is a text the console must have said by the end; the script
# ends 0 when all were said, 1 when one was not.
set -u
STEPS=${1:?steps file}
shift
ELF=${ELF:-tessronosdesk.elf}
DISK=${DISK:-obj/_QEMU_VIRT__ktest/test_disk.img}
WAIT=${WAIT:-150}
OUT=${OUT:-.}
TAG=${TAG:-desk$$}
QEMU=${QEMU:-qemu-system-aarch64}
IMG=/tmp/$TAG.img
MON=/tmp/$TAG.sock
LOG=/tmp/$TAG.log

rm -f "$MON" "$LOG"
cp "$DISK" "$IMG" || exit 2
"$QEMU" -M virt,gic-version=2 -cpu cortex-a76 -smp 4 -m 2G \
  -display none -no-reboot \
  -global virtio-mmio.force-legacy=false \
  -drive file="$IMG",if=none,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -device qemu-xhci,id=xhci -device usb-kbd -device usb-mouse \
  -device bochs-display \
  -netdev socket,id=n0,mcast=230.0.0.1:12346 -device virtio-net-device,netdev=n0 \
  -kernel "$ELF" -serial file:"$LOG" \
  -monitor unix:"$MON",server,nowait &
QPID=$!
trap 'kill -9 $QPID 2>/dev/null; rm -f "$IMG" "$MON"' EXIT

sleep "$WAIT"
python3 - "$MON" "$STEPS" "$OUT" <<'PY'
import os, socket, sys, time

mon, steps, out = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX)
s.connect(mon)
time.sleep(0.5)
s.settimeout(0.5)


def drain():
    try:
        while s.recv(65536):
            pass
    except Exception:
        pass


def send(cmd, pause=0.03):
    s.sendall((cmd + "\n").encode())
    time.sleep(pause)


drain()
shots = []
for line in open(steps, encoding="utf-8"):
    w = line.split()
    if not w or w[0].startswith("#"):
        continue
    if w[0] == "at":
        x, y = int(w[1]), int(w[2])
        for _ in range(25):
            send("mouse_move -100 -100")
        time.sleep(0.5)
        while x > 0 or y > 0:
            dx, dy = min(6, x), min(6, y)
            send("mouse_move %d %d" % (dx, dy))
            x -= dx
            y -= dy
        time.sleep(0.5)
    elif w[0] in ("click", "dclick"):
        for _ in range(2 if w[0] == "dclick" else 1):
            send("mouse_button 1")
            send("mouse_button 0")
        time.sleep(0.5)
    elif w[0] == "down":
        send("mouse_button 1", 0.3)
    elif w[0] == "up":
        send("mouse_button 0", 0.3)
    elif w[0] == "key":
        send("sendkey " + w[1], 1.0)
    elif w[0] == "shot":
        p = "/tmp/%s_%s.ppm" % (os.path.basename(mon)[:-5], w[1])
        send("screendump " + p, 2.0)
        shots.append((p, w[1]))
    elif w[0] == "sleep":
        time.sleep(float(w[1]))
    else:
        send(line.strip(), 1.0)
    drain()
s.close()
time.sleep(2)
for p, name in shots:
    if not os.path.exists(p):
        continue
    try:
        from PIL import Image
        Image.open(p).save(os.path.join(out, name + ".png"))
        os.remove(p)
    except Exception:
        os.replace(p, os.path.join(out, name + ".ppm"))
PY

kill -9 $QPID 2>/dev/null
wait 2>/dev/null
ok=0
for e in "$@"; do
	if grep -a -q -F "$e" "$LOG"; then
		echo "desk_act: said: $e"
	else
		echo "desk_act: NOT said: $e"
		ok=1
	fi
done
[ -n "${KEEPLOG:-}" ] && cp "$LOG" "$KEEPLOG"
rm -f "$LOG"
exit $ok
