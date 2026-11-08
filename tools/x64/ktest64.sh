#!/bin/bash
#
# tools/x64/ktest64.sh — x64 のカーネル内の試験(KTEST=1 のディスクのイメージ)を、Windows の
# QEMU の q35 と OVMF で走らせる(docs/tessronos/12-development.md 12.3)。Git Bash から使う
#
#   tools/x64/ktest64.sh <sd.img> [秒] [コンソールの出力先]
#
#   イメージは make TARGET=_X64_ KTEST=1 [KTONLY=...] BUILD_ID=<名前> sdimg で作る
#   (build_make/obj/_X64__ktest<名前>/sd.img)。KTEST SUMMARY が出るか、マシンが電源を
#   切るか、時間(既定 900 秒)が来るまで待ち、集計と失敗の行を出す。集計が全部の成功なら 0。
#
#   環境変数:
#     ACCEL=whpx   Windows のハイパーバイザで速く走らせる(-cpu host。OVMF が読む MSR の
#                  機能は切る)。既定は TCG(-cpu max)
#     NET=user     試験用の FTP と HTTP のサーバ(tools/ftpd.py)をホストに起こし、利用者
#                  モードの網(IPv4 と IPv6)で外へ繋ぐ。イメージの隣の ftp/ を FTP の根にする。
#                  FTP_PORT(既定 2121)はホストのポート。ほかのプログラムが使っていたら別の番号に
#                  し、make にも同じ FTP_PORT を渡す
#     QCPU         TCG のときの -cpu の機種(既定 max。AMD の確認は EPYC-Rome など)
#     SMP、MEM     プロセッサの数(既定 4)とメモリ(既定 2G)
#     EXTRA        QEMU に足す引数(例: -audiodev none,id=a0 -device intel-hda -device hda-duplex,audiodev=a0)
#     USBQ=1       USB の機器を QEMU virt の試験機と同じにする(xhci にキーボード、タブレット、
#                  USB 3 の口のディスク、ハブとその先のオーディオ、ディスク、マウス、USB 2 だけの
#                  xhci2 にディスク3台とキーボード)。ディスクはイメージの隣の usb_disk.img などで、
#                  make TARGET=_X64_ KTEST=1 BUILD_ID=<名前> で作る(12.3)。QMP を開き、試験が
#                  コンソールに出す抜き差しの頼みを tools/qemu_qmp.py が QEMU に渡す
#     NVME2、NVME3、NVME4 ほかの NVMe のディスクのイメージ(nvmeb、nvmec、nvmed。システム登録の試験の
#                  tools/tests/mkwindisk.py が作る Windows の形のディスク、12.3)
#     VARS         OVMF の変数の書ける写し(edk2-i386-vars.fd を写したもの)。起動の項目
#                  (Boot####、BootOrder、BootNext)が次の起動に残る。無ければ変数はその起動の間だけ
#     NVME4K=<img> 2台目の NVMe(nvmeb)として、論理ブロックが4096バイトの装置にそのイメージを
#                  挿す(pcdev の群の test_nvme_4k が書き込む。中身は消える)
#     OFFWAIT=1    KTEST SUMMARY の後も、マシンが電源を切るまで(秒の残りの間)待つ。電源を切る
#                  前の行(NVMe のシャットダウンなど)をコンソールの出力に残す
#
set -u
IMG=${1:?usage: ktest64.sh <sd.img> [secs] [log]}
TMO=${2:-900}
LOG=${3:-$(dirname "$IMG")/ktest64.log}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
Q=${QEMU_WIN_X64:-"/c/Program Files/qemu/qemu-system-x86_64.exe"}
OVMF=${OVMF_CODE:-"C:/Program Files/qemu/share/edk2-x86_64-code.fd"}

if [ "${ACCEL:-}" = whpx ]; then
	CPU="-accel whpx -cpu host,vmx=off,lmce=off,sgx=off,sgxlc=off"
else
	CPU="-cpu ${QCPU:-max}"
fi

SRV=""
NETDEV="-netdev user,id=n0"
if [ "${NET:-}" = user ]; then
	python "$TOP/tools/ftpd.py" --port "${FTP_PORT:-2121}" --pasv-address 10.0.2.2 --active-via 127.0.0.1:2122 \
		--http 2180 --echo6 2190 "$(cygpath -m "$(dirname "$IMG")/ftp")" > "$LOG.ftpd" 2>&1 &
	SRV=$!
	NETDEV="-netdev user,id=n0,ipv4=on,ipv6=on,hostfwd=tcp:127.0.0.1:2122-:2020"
	sleep 2
	# another program on the port answers the tests in its place: every test of the network would fail
	kill -0 $SRV 2>/dev/null || echo "ktest64: the test server did not start ($LOG.ftpd); FTP_PORT names another port, with the same at make"
fi

USBDEV="-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0"
QMP=""
if [ "${USBQ:-}" = 1 ]; then
	# the disks the tests plug in through QMP (vols, grow, btcd) are not on QEMU's command line:
	# one not made shows only later, as tests that fail or are skipped
	MISS=""
	for f in usb_disk.img usb_disk2.img usb_disk3.img usb_sf32.img usb_fd.img btcd.iso vols/vola.img vols/volg.img; do
		[ -f "$(dirname "$IMG")/$f" ] || MISS="$MISS $f"
	done
	if [ -n "$MISS" ]; then
		echo "ktest64: not made next to the image:$MISS"
		echo "ktest64:   (make TARGET=_X64_ KTEST=1 BUILD_ID=<name> obj/_X64__ktest<name>/vols/vola.img and the others, 12.3)"
	fi
	D=$(cygpath -m "$(dirname "$IMG")")
	QPORT=${QMP_PORT:-4457}
	USBDEV="-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1,id=kbd1 -device usb-tablet,bus=xhci.0,port=2,id=tab1
		-drive file=$D/usb_disk.img,if=none,format=raw,id=usbdisk -device usb-storage,bus=xhci.0,port=3,drive=usbdisk,id=usbss
		-device usb-hub,bus=xhci.0,port=4,id=hub1
		-audiodev ${USBQ_AUDIO:-none},id=snd0 -device usb-audio,bus=xhci.0,port=4.1,audiodev=snd0,id=aud1
		-blockdev driver=file,filename=$D/usb_disk3.img,node-name=usbdisk3f -blockdev driver=raw,file=usbdisk3f,node-name=usbdisk3
		-device usb-storage,bus=xhci.0,port=4.2,drive=usbdisk3,removable=on,id=usbfs
		-device usb-mouse,bus=xhci.0,port=4.3,id=hpm
		-device qemu-xhci,id=xhci2,p3=0 -drive file=$D/usb_disk2.img,if=none,format=raw,id=usbdisk2 -device usb-storage,bus=xhci2.0,port=1,drive=usbdisk2
		-device usb-kbd,bus=xhci2.0,port=2,id=hpk
		-drive file=$D/usb_sf32.img,if=none,format=raw,id=usbdisk4 -device usb-storage,bus=xhci2.0,port=3,drive=usbdisk4
		-drive file=$D/usb_fd.img,if=none,format=raw,id=usbdisk5 -device usb-storage,bus=xhci2.0,port=4,drive=usbdisk5"
	QMP="-qmp tcp:127.0.0.1:$QPORT,server=on,wait=off"
fi

NV4K=""
if [ -n "${NVME4K:-}" ]; then
	NV4K="-drive file=$(cygpath -m "$NVME4K"),if=none,format=raw,id=nv4k
		-device nvme,serial=tessron4k,drive=nv4k,logical_block_size=4096,physical_block_size=4096"
fi

NVX=""
[ -n "${NVME2:-}" ] && NVX="$NVX -drive file=$(cygpath -m "$NVME2"),if=none,format=raw,id=nvd2 -device nvme,serial=tessronos2,drive=nvd2"
[ -n "${NVME3:-}" ] && NVX="$NVX -drive file=$(cygpath -m "$NVME3"),if=none,format=raw,id=nvd3 -device nvme,serial=tessronos3,drive=nvd3"
[ -n "${NVME4:-}" ] && NVX="$NVX -drive file=$(cygpath -m "$NVME4"),if=none,format=raw,id=nvd4 -device nvme,serial=tessronos4,drive=nvd4"
VARSDRV=""
[ -n "${VARS:-}" ] && VARSDRV="-drive if=pflash,format=raw,file=$(cygpath -m "$VARS")"

rm -f "$LOG"
# 内蔵時計(CMOS の RTC)はローカル時刻で、Windows の入った PC と同じ形(9.5.1)
"$Q" -M q35 $CPU -smp "${SMP:-4}" -m "${MEM:-2G}" -rtc base=localtime \
	-drive "if=pflash,format=raw,readonly=on,file=$OVMF" $VARSDRV \
	-drive "file=$(cygpath -m "$IMG"),if=none,format=raw,id=nvd" -device nvme,serial=tessronos,drive=nvd $NVX \
	$NV4K $USBDEV $QMP \
	$NETDEV -device virtio-net-pci,netdev=n0 \
	-display none -no-reboot -net none ${EXTRA:-} -serial "file:$(cygpath -m "$LOG")" &
QP=$!
QH=""
if [ -n "$QMP" ]; then
	python "$TOP/tools/qemu_qmp.py" "tcp:127.0.0.1:$QPORT" "$(cygpath -m "$LOG")" 2> "$LOG.qmp" &
	QH=$!
fi

t=0
while [ $t -lt "$TMO" ] && kill -0 $QP 2>/dev/null; do
	sleep 5
	t=$((t + 5))
	if [ "${OFFWAIT:-}" != 1 ] && grep -a -q "KTEST SUMMARY" "$LOG" 2>/dev/null; then
		sleep 3
		break
	fi
done
kill $QP 2>/dev/null
wait $QP 2>/dev/null
[ -n "$QH" ] && kill $QH 2>/dev/null
[ -n "$SRV" ] && kill $SRV 2>/dev/null

tr -d '\r' < "$LOG" | grep -a -E "KTEST .* FAIL|EXCEPTION|LOCKUP|KTEST SUMMARY"
S=$(tr -d '\r' < "$LOG" | grep -a -o "KTEST SUMMARY [0-9]*/[0-9]*" | tail -1)
[ -z "$S" ] && { echo "ktest64: no summary after $t s ($LOG)"; exit 1; }
set -- ${S#KTEST SUMMARY }
P=${1%/*}
N=${1#*/}
[ "$P" = "$N" ]
