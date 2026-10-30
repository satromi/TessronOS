#
# TessronOS build (non-recursive)
#
#   make TARGET=_QEMU_VIRT_            tessronos.elf
#   make TARGET=_QEMU_VIRT_ qemu       ビルドして QEMU で起動(Ctrl-A X で終了)
#   make TARGET=_QEMU_VIRT_ test       tools/qemu_run.sh で起動し、出力を検査
#   make TARGET=_QEMU_VIRT_ KTEST=1 test   カーネル内テスト(tests/ktest)を実行
#   make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=svcio NET=user test
#                                      網を slirp にし、ホストの FTP サーバへ繋ぐ試験も走らせる
#   make TARGET=_RPI5_                 tessronos.elf / tessronos.img / tessronos.map
#   make TARGET=_RPI5_ sd SD=/mnt/e    SD の FAT 区画へ配置
#   make TARGET=_RPI4_                 Raspberry Pi 4、400、Compute Module 4 の tessronos.img(ARMv8.0-A)
#   make TARGET=_RPI4_ qemu-rpi4       QEMU の raspi4b で起動(Ctrl-A X で終了)
#   make TARGET=_X64_                  x64 の tessronos.elf と UEFI のローダ BOOTX64.EFI(build_make/x64.mk)
#   make clean
#

#   make TARGET=_QEMU_VIRT_ BUILD_ID=b2 test
#       別の目的物一式で建てる。同じ木で 2 つの作業を並行させるときに使う。
#       目的物も試験用ディスクも obj/<target><id>/ と tessronos<id>.* に分かれる。

TARGET   ?= _QEMU_VIRT_
BUILD_ID ?=
EXE_FILE := tessronos$(BUILD_ID)

# アーキテクチャは TARGET から決まる。ARCH はツールチェーンと命令セットの基準を、
# ARCH_CORE はコア依存のソースとヘッダの置き場(kernel/sysdepend/cpu/core/<ARCH_CORE>/ など)を選ぶ
# (docs/tessronos/04-kernel.md 4.8)
ifneq ($(filter _QEMU_VIRT_ _RPI5_ _RPI4_,$(TARGET)),)
  ARCH := aarch64
else ifeq ($(TARGET),_X64_)
  ARCH := x86_64
else
  $(error unknown TARGET "$(TARGET)" (use _QEMU_VIRT_, _RPI5_, _RPI4_ or _X64_))
endif
ARCH_CORE_aarch64 := armv8a
ARCH_CORE_x86_64  := x86_64
ARCH_CORE := $(ARCH_CORE_$(ARCH))

include toolchain.mk
include common.mk
include sources.mk

ifeq ($(TARGET),_QEMU_VIRT_)
  include qemu_virt.mk
else ifeq ($(TARGET),_RPI5_)
  include rpi5.mk
else ifeq ($(TARGET),_RPI4_)
  include rpi4.mk
else ifeq ($(TARGET),_X64_)
  include x64.mk
else
  $(error unknown TARGET "$(TARGET)" (use _QEMU_VIRT_, _RPI5_, _RPI4_ or _X64_))
endif

CFLAGS  := $(CFLAGS_COMMON) $(CFLAGS_TARGET) -D$(TARGET) -DLOAD_PA_VALUE=$(LOAD_PA)
ASFLAGS := $(ASFLAGS_COMMON) -D$(TARGET)
LDFLAGS := $(LDFLAGS_COMMON) -T $(LNKFILE) -Wl,--defsym,LOAD_PA=$(LOAD_PA) \
	-Wl,-Map,$(EXE_FILE).map

OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(filter %.c,$(SRCS))) \
	$(patsubst %.S,$(OBJDIR)/%.o,$(filter %.S,$(SRCS)))
DEPS := $(OBJS:.o=.d)

# FreeType 本体と、その受けの ftsystem.c だけが「ライブラリを作る側」である。
# 使う側(outer_kernel/font/fn.c)に同じ印を付けると内部の頭を掴んでしまうので
# 付けない(設計書 16.6)
FT_LIB_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS_FT)) \
	$(OBJDIR)/outer_kernel/font/port/ftsystem.o
$(FT_LIB_OBJS): CFLAGS += -DFT2_BUILD_LIBRARY -Wno-frame-larger-than

# 画面を組み立てるタスク(TA_FPU)だけが呼ぶ、ベクトル演算の内側のループ。カーネルの
# ほかは浮動小数点・SIMD のレジスタを使わない(-mgeneral-regs-only)ので、このファイル
# だけその印を外す。扱うのは通常のメモリ(裏の画面と窓の画素)だけで、整列の検査
# (SCTLR_EL1.A)も切ってあるので、16 バイトのロード・ストアを 4 バイトずつに分けさせる
# -mstrict-align も外す(docs/tessronos/10-display.md)
$(OBJDIR)/outer_kernel/wm/compose_simd.o: CFLAGS := $(filter-out -mgeneral-regs-only -mstrict-align,$(CFLAGS))

# TARGET を切り替えたら再リンクする(成果物名が共通のため)
TARGET_STAMP := obj/target.stamp
$(shell mkdir -p obj; [ "$$(cat $(TARGET_STAMP) 2>/dev/null)" = "$(TARGET)$(KTEST)" ] || echo "$(TARGET)$(KTEST)" > $(TARGET_STAMP))

.PHONY: all image clean qemu gdb test sd sdimg docs-check pkg

all: $(EXE_FILE).elf $(IMAGE_TARGETS) $(EXE_FILE).lst

# 構造体オフセット(アセンブラ用)。offset.c を -S で処理して "@@name value" 行を抜き出す
OFFSET_H := $(OBJDIR)/offset.h
# 構造体の並びを決めるヘッダが変われば作り直す(TCB、T_PCPU、T_PRCSET、T_CTSK)
$(OFFSET_H): $(TOP)/kernel/sysdepend/cpu/core/$(ARCH_CORE)/offset.c \
	     $(TOP)/kernel/knlinc/kernel.h $(TOP)/kernel/knlinc/prcset.h \
	     $(TOP)/kernel/sysdepend/cpu/core/$(ARCH_CORE)/sysdepend.h $(TOP)/include/tk/syscall.h
	@mkdir -p $(dir $@)
	@echo 'GEN $@'
	@$(CC) $(filter-out -MMD -MP,$(CFLAGS)) $(INCPATH) -S -o - $< | \
	  awk '/^[ 	]*@@/ { s=$$1; sub(/^.*@@/,"",s); v=$$2; sub(/#/,"",v); print "#define " s "\t" v }' > $@

$(OBJS): $(OFFSET_H)

# NETSTACK が変われば全体を作り直す(<ts/so.h> の中身が変わるため)
NETSTACK_STAMP := $(OBJDIR)/netstack.stamp
$(shell mkdir -p $(OBJDIR); [ "$$(cat $(NETSTACK_STAMP) 2>/dev/null)" = "$(NETSTACK)" ] || echo "$(NETSTACK)" > $(NETSTACK_STAMP))
$(OBJS): $(NETSTACK_STAMP)

# NetBSD のスタック(rump カーネル)。ソースを取得し、TessronOS のツールチェーンで
# 一つのオブジェクトにする(tools/netbsd/build.sh、設計書 12.6)
ifeq ($(NETSTACK),netbsd)
NETBSD_BUILD ?= $(TOP)/tools/netbsd/build.sh
$(NETBSD_OBJ): $(NETBSD_BUILD) $(wildcard $(TOP)/peripheral_kernel/network/netbsd/rumpcomp/*.c)
	@echo 'NETBSD $@'
	@OUT=$(patsubst %/,%,$(dir $(NETBSD_OBJ))) MARCH=$(ARCH_MARCH) bash $(NETBSD_BUILD) all
endif

$(EXE_FILE).elf: $(OBJS) $(EXTRA_OBJS) $(LNKFILE) $(TARGET_STAMP)
	@echo 'LD  $@'
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(EXTRA_OBJS) -lgcc
	@$(SIZE) $@

$(EXE_FILE).img: $(EXE_FILE).elf
	@echo 'IMG $@'
	@$(OBJCOPY) -O binary $< $@

$(EXE_FILE).lst: $(EXE_FILE).elf
	@$(OBJDUMP) -d -S $< > $@

image: $(EXE_FILE).img

qemu: $(EXE_FILE).elf
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio -kernel $<

gdb: $(EXE_FILE).elf
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio -kernel $< -s -S

ifeq ($(KTEST),1)
# 上限であって普段の所要ではない。1 コアの TCG では索引の深い試験だけで
# 200 秒近くかかるため、そこに合わせてある
# QMP 係: 試験が USB 機器の抜き差しや鍵の押下を頼む(tools/qemu_qmp.py)
TEST_EXPECT := -t $(TEST_TMO) -e "KTEST SUMMARY" -f "FAIL" -q $(TOP)/tools/qemu_qmp.py
# 区画表(GPT)を持つ試験用ディスク。ブロックデバイスの試験が使う
TEST_DISK  := $(OBJDIR)/test_disk.img
TEST_DISK_MB ?= 256
# TEST_DISK_ROOM=N: the test disk is written as a card written with a smaller image has it, N MB
# past its tables, the system volume's partition last, so that the start grows it (design 7.7)
TEST_DISK_ROOM ?= 0
TEST_DISK_STAMP := $(OBJDIR)/testdisk.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(TEST_DISK_STAMP) ] && [ "$$(cat $(TEST_DISK_STAMP))" = "$(TEST_DISK_ROOM)" ] || echo "$(TEST_DISK_ROOM)" > $(TEST_DISK_STAMP))
# USB の大容量記憶の試験が使う。usb_disk.img は MBR と FAT32 の区画を持ち
# SuperSpeed の口に(usbss。ktest_svcio が最後に抜く)、usb_disk2.img は空で High-Speed だけの二台目の制御器に、
# usb_disk3.img は空でハブの先(Full-Speed)に挿す(usbfs。ktest_usb と ktest_svcio が抜いて差し直すので、
# 中身は -blockdev で機器と別に持ち、機器を消しても残す)
USB_DISK   := $(OBJDIR)/usb_disk.img
USB_DISK2  := $(OBJDIR)/usb_disk2.img
USB_DISK3  := $(OBJDIR)/usb_disk3.img
# 区画表の無い媒体(スーパーフロッピー形式)。usb_sf32.img は34MBのFAT32、
# usb_fd.img は1.44MBのフロッピーの形のFAT12で、どちらもセクタ0がブート
# セクタ。二台目の制御器の3番と4番の口(High-Speed)に挿す
USB_DISK4  := $(OBJDIR)/usb_sf32.img
USB_DISK5  := $(OBJDIR)/usb_fd.img
# SDカード(sdhci-pciに挿す)。RPi5のカードと同じ作り(mksd.py、試験用の区画付き)で、
# 4GBのSDHCに見えるよう後ろを疎に伸ばす(控えのGPTは、実機のカードに焼いたときと同じく途中に残る)。
# ktest_sdが大きさと区画と読み書きを確かめる
SD_CARD    := $(OBJDIR)/sd_card.img
SD_CARD_SECTORS := 8388608
# SD_EMMC=1: 同じ像を SD カードではなく eMMC(QEMU の emmc)として挿す。
# Compute Module 5 の eMMC と同じ MMC の初期化(sdhci.c)を ktest_sd で通す。
# xPack の QEMU 9.2 の emmc は -device で挿せないので、そのときは Windows の
# QEMU(10.0 以降。NET=user と同じく QEMU_WIN)を使い、QMP 係も Windows の Python で走らせる
ifeq ($(SD_EMMC),1)
SD_DEVICE  := emmc
# 制御器の能力に8本のデータ線(ビット18)を足し、Compute Module 5 と同じく8ビットの幅に切り替えさせる
SD_HOST_OPT := ,capareg=0x057c34b4
QEMU_WIN ?= /mnt/c/Program Files/qemu/qemu-system-aarch64.exe
ifeq ($(shell "$(QEMU)" -device help 2>/dev/null | tr -d '\r' | grep -c 'name "emmc"'),0)
QEMU     := $(QEMU_WIN)
HOST_PY  ?= $(shell command -v python.exe || command -v py.exe)
endif
else
SD_DEVICE  := sd-card
endif
# NET=user で網をQEMUの利用者モード(slirp)にする。ホストは 10.0.2.2 に見え、
# DHCP が 10.0.2.15 を配る。試験用の FTP サーバ(tools/ftpd.py)をホストの
# 127.0.0.1:$(FTP_PORT) に QEMU の前に起こし、後で止める。$(FTP_DIR) を配る。
# ktest_svcio がそこへ繋ぐ(make ... KTONLY=svcio NET=user test)。
# QEMU に利用者モードの網(libslirp)が組み込まれている必要がある。xPack の
# QEMU 9.2 には無いので、そのときは Windows の QEMU(QEMU_WIN)を WSL から
# 使い、FTP サーバと QMP 係も Windows の Python(HOST_PY)で走らせる。
# 網の両端が Windows のループバックに揃い、PASV の口もそのまま届く
# (tools/qemu_run.sh)。QEMU=... で組み込まれたものを指してもよい。
# 既定は他の試験機と混ざらないマルチキャストの網で、FTP の試験は飛ばす
NET      ?=
FTP_PORT ?= 2121
# FTP の能動の口: 試験機は FTP_ACT_PORT で待ち受け、QEMU がホストの
# 127.0.0.1:FTP_ACT_HOST からそこへ繋ぐ(hostfwd)。ホストから 10.0.2.15 へは
# 届かないので、サーバは PORT・EPRT が言う所の代わりにそこへ繋ぐ(--active-via)
FTP_ACT_PORT ?= 2020
FTP_ACT_HOST ?= 2122
FTP_DIR  := $(OBJDIR)/ftp
# マイクロスクリプトの網の試験のための HTTP サーバ(tools/httpd_test.py): ホストの
# 127.0.0.1:$(HTTP_PORT) が JSON で、その次の口がイベントの流れで答える。ftpd.py が一緒に起こす
HTTP_PORT ?= 2180
# IPv6 の試験の折り返しサーバ: ftpd.py がホストの [::1]:$(ECHO6_PORT) で受けたものを送り返す。
# 利用者モードの網は IPv4 とともに IPv6 も持ち(ipv4=on,ipv6=on。ipv6=on だけでは IPv4 が切られる)、ルータ広告で fec0::/64 を配り、試験機からの
# [fec0::2] をホストの ::1 に繋ぐ。ktest_so がそこへ繋ぐ
ECHO6_PORT ?= 2190
ifeq ($(NET),user)
QEMU_WIN ?= /mnt/c/Program Files/qemu/qemu-system-aarch64.exe
ifneq ($(shell "$(QEMU)" -M none -netdev help 2>/dev/null | tr -d '\r' | grep -x user),user)
QEMU     := $(QEMU_WIN)
HOST_PY  ?= $(shell command -v python.exe || command -v py.exe)
endif
TEST_NETDEV := -netdev user,id=n0,ipv4=on,ipv6=on,hostfwd=tcp:127.0.0.1:$(FTP_ACT_HOST)-:$(FTP_ACT_PORT)
TEST_SERVER := -s "$(or $(HOST_PY),python3) $(TOP)/tools/ftpd.py --exit-on-eof --port $(FTP_PORT) --pasv-address 10.0.2.2 --active-via 127.0.0.1:$(FTP_ACT_HOST) --http $(HTTP_PORT) --echo6 $(ECHO6_PORT) $(FTP_DIR)"
$(OBJDIR)/tests/ktest/ktest_svcio.o: CFLAGS += -DKT_FTP_PORT=$(FTP_PORT)
$(OBJDIR)/tests/ktest/ktest_ms.o: CFLAGS += -DKT_HTTP_PORT=$(HTTP_PORT)
$(OBJDIR)/tests/ktest/ktest_xfc.o: CFLAGS += -DKT_FTP_PORT=$(FTP_PORT) -DKT_FTP_ACT_PORT=$(FTP_ACT_PORT)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_NET_USER=1
# ktest_so は利用者モードの網があれば、DHCP と名前の問い合わせと時刻合わせ、IPv6 のアドレスの自動設定と
# AAAA とホストへの IPv6 の接続も確かめる
$(OBJDIR)/tests/ktest/ktest_so.o: CFLAGS += -DKT_NET_USER=1 -DKT_ECHO6_PORT=$(ECHO6_PORT)
# KT_SITE=<URL>: ktest_v8 only opens that page with the browser and takes the screen
# (a site looked at on its own, BUILD_ID of its own)
ifneq ($(KT_SITE),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_SITE='"$(KT_SITE)"'
# KT_SITE_WAIT=<ms>: the second screen that long after the first (3000 when not given)
ifneq ($(KT_SITE_WAIT),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_SITE_WAIT=$(KT_SITE_WAIT)
endif
# KT_SITE_CLICK=<x>,<y>[,<x>,<y>...]: after the first screen, the left button pressed and let go at
# those points of the screen (one each 4 s), and the screen taken after each (SITEC1.PPM...)
ifneq ($(KT_SITE_CLICK),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_SITE_CLICK=$(KT_SITE_CLICK)
endif
endif
else
ifneq ($(filter %.exe,$(QEMU)),)
# Windows の QEMU(SD_EMMC=1 のとき)はマルチキャストの口に入れないので、外へ出ない利用者モードの網にする
TEST_NETDEV := -netdev user,id=n0,restrict=on
else
TEST_NETDEV := -netdev socket,id=n0,mcast=230.0.0.1:12345
endif
TEST_SERVER :=
endif
# KT_V8=<name>,<name>: ktest_v8 runs only those of its tests (make ... KT_V8=test_blink_media)
ifneq ($(KT_V8),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_V8='"$(KT_V8)"'
endif
# KT_BR_GPU=1: ktest_v8 starts the browser rastering the page on the GPU, softpipe too (its line gpu=on)
ifeq ($(KT_BR_GPU),1)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_BR_GPU=1
endif
# NET と FTP_PORT(と KT_SITE・KT_SITE_WAIT・KT_SITE_CLICK・KT_V8)が変われば、それを見る試験(ktest_svcio・ktest_xfc・ktest_ms・ktest_v8)を作り直す
NET_STAMP := $(OBJDIR)/net.stamp
$(shell mkdir -p $(OBJDIR); [ "$$(cat $(NET_STAMP) 2>/dev/null)" = "$(NET):$(FTP_PORT):$(FTP_ACT_PORT):$(KT_SITE):$(KT_SITE_WAIT):$(KT_SITE_CLICK):$(KT_V8):$(KT_BR_GPU)" ] || echo "$(NET):$(FTP_PORT):$(FTP_ACT_PORT):$(KT_SITE):$(KT_SITE_WAIT):$(KT_SITE_CLICK):$(KT_V8):$(KT_BR_GPU)" > $(NET_STAMP))
$(OBJDIR)/tests/ktest/ktest_svcio.o $(OBJDIR)/tests/ktest/ktest_xfc.o $(OBJDIR)/tests/ktest/ktest_ms.o $(OBJDIR)/tests/ktest/ktest_v8.o $(OBJDIR)/tests/ktest/ktest_so.o: $(NET_STAMP)
# force-legacy=false: virtio-mmio を現行仕様(version 2)にする
# 二つ目のシリアルポート(serb)は何にも繋がず、試験が UART の折り返しで使う
TEST_DRIVE := -serial null \
		      -global virtio-mmio.force-legacy=false -drive file=$(TEST_DISK),if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
		      $(TEST_NETDEV) -device virtio-net-device,netdev=n0 \
		      -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1,id=kbd1 -device usb-tablet,bus=xhci.0,port=2,id=tab1 \
		      -drive file=$(USB_DISK),if=none,format=raw,id=usbdisk -device usb-storage,bus=xhci.0,port=3,drive=usbdisk,id=usbss \
		      -device usb-hub,bus=xhci.0,port=4,id=hub1 \
		      -audiodev none,id=snd0 -device usb-audio,bus=xhci.0,port=4.1,audiodev=snd0,id=aud1 \
		      -blockdev driver=file,filename=$(USB_DISK3),node-name=usbdisk3f -blockdev driver=raw,file=usbdisk3f,node-name=usbdisk3 -device usb-storage,bus=xhci.0,port=4.2,drive=usbdisk3,removable=on,id=usbfs \
		      -device usb-mouse,bus=xhci.0,port=4.3,id=hpm \
		      -device qemu-xhci,id=xhci2,p3=0 -drive file=$(USB_DISK2),if=none,format=raw,id=usbdisk2 -device usb-storage,bus=xhci2.0,port=1,drive=usbdisk2 \
		      -device usb-kbd,bus=xhci2.0,port=2,id=hpk 		      -drive file=$(USB_DISK4),if=none,format=raw,id=usbdisk4 -device usb-storage,bus=xhci2.0,port=3,drive=usbdisk4 		      -drive file=$(USB_DISK5),if=none,format=raw,id=usbdisk5 -device usb-storage,bus=xhci2.0,port=4,drive=usbdisk5 \
		      -device bochs-display \
		      -drive file=$(SD_CARD),if=none,format=raw,id=sdcard -device sdhci-pci,sd-spec-version=3$(SD_HOST_OPT) -device $(SD_DEVICE),drive=sdcard
TEST_DEPS  := $(TEST_DISK) $(USB_DISK) $(USB_DISK2) $(USB_DISK3) $(USB_DISK4) $(USB_DISK5) $(SD_CARD)
# BTRON の CD-ROM の像(tools/tests/mkbtcd.py、設計書 11.9)。起動時には挿さず、ktest_btcd が
# QMP 係に頼んで USB の CD-ROM 装置として挿し(drive_add の media=cdrom と usb-storage、ハブの 4.4)、
# 試験の後で抜く。ほかの試験が数える USB の機器はそのまま
BTCD_IMG   := $(OBJDIR)/btcd.iso
TEST_DEPS  += $(BTCD_IMG)
$(OBJDIR)/tests/ktest/ktest_btcd.o: CFLAGS += -DKT_BTCD_IMG='"$(abspath $(BTCD_IMG))"'
# iob の群(ktest_iob.c)が使うディスクの名前と、抜き差しの試験の FAT のイメージ(-DKT_IOB_FAT=... -DKT_IOB_PULL=... など)。既定は無し
KT_IOB_CFLAGS ?=
$(OBJDIR)/tests/ktest/ktest_iob.o: CFLAGS += $(KT_IOB_CFLAGS)
# 多くのディスクと区画の試験(ktest_vols、docs/tessronos 7.6)の USB ディスク 4 台(tools/tests/mkvoldisk.py)。
# 起動時には挿さず、試験が QMP 係に頼んでハブの 4.5〜4.8 に挿し、抜き差しを繰り返して、最後に抜く
VOL_DIR    := $(OBJDIR)/vols
VOL_STAMP  := $(VOL_DIR)/vola.img
TEST_DEPS  += $(VOL_STAMP)
$(OBJDIR)/tests/ktest/ktest_vols.o: CFLAGS += -DKT_VOL_DIR='"$(abspath $(VOL_DIR))"'
$(OBJDIR)/tests/ktest/ktest_grow.o: CFLAGS += -DKT_VOL_DIR='"$(abspath $(VOL_DIR))"'
ifeq ($(TARGET),_QEMU_VIRT_)
$(OBJDIR)/tests/ktest/ktest_sd.o: CFLAGS += -DKT_SD_SECTORS=$(SD_CARD_SECTORS) -DKT_SD_EMMC=$(if $(filter 1,$(SD_EMMC)),1,0)
# SD_EMMC が変われば、それを見る ktest_sd を作り直す
SD_EMMC_STAMP := $(OBJDIR)/sdemmc.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(SD_EMMC_STAMP) ] && [ "$$(cat $(SD_EMMC_STAMP))" = "$(SD_EMMC)" ] || echo "$(SD_EMMC)" > $(SD_EMMC_STAMP))
$(OBJDIR)/tests/ktest/ktest_sd.o: $(SD_EMMC_STAMP)
endif
# KTONLY(走らせる群)が変われば、群を選ぶ ktest_main を作り直す
KTONLY_STAMP := $(OBJDIR)/ktonly.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(KTONLY_STAMP) ] && [ "$$(cat $(KTONLY_STAMP))" = "$(KTONLY)" ] || echo "$(KTONLY)" > $(KTONLY_STAMP))
$(OBJDIR)/tests/ktest/ktest_main.o: $(KTONLY_STAMP)
# 時計が合っていない機械(電池の無いRPi5)では、試験がビルドした時刻に合わせてから実身の群を走らせる
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_BUILD_UNIX=$(shell date +%s)
ifeq ($(NET),user)
TEST_DEPS  += $(FTP_DIR)/KTEST.TXT $(FTP_DIR)/RATE.BIN

$(FTP_DIR)/KTEST.TXT:
	@mkdir -p $(dir $@)
	@printf 'TessronOS FTP test file\n' > $@

# ktest_svcio がホストからの TCP の速さを測るのに受け取る 16MB
$(FTP_DIR)/RATE.BIN:
	@mkdir -p $(dir $@)
	@head -c 16777216 /dev/zero > $@
endif
else
TEST_EXPECT := -t 10 -e "TessronOS usermain: done"
TEST_DRIVE :=
TEST_DEPS  :=
TEST_SERVER :=
endif

# 利用者プログラム(プロセスとして起動する ELF)
USER_DIR  := $(TOP)/application/hello
USER_ELF  := $(UOBJ)/hello.elf
USER_SRCS := $(USER_DIR)/$(UARCH_DIR)crt0.S $(USER_DIR)/$(UARCH_DIR)svc_stub.S $(USER_DIR)/hello.c
USER_LD   := $(TOP)/etc/linker/$(USER_LD_DIR)/program.ld
# 利用者のプログラムが読むヘッダ。構造体の形(依頼や事象の大きさ)が変われば、
# 利用者のプログラムも作り直す
APP_HDRS  := $(wildcard $(TOP)/include/ts/*.h) $(wildcard $(TOP)/include/tk/*.h)

$(USER_ELF): $(APP_HDRS) $(USER_SRCS) $(USER_LD)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARCHFLAGS) $(UARCH_NOFP) -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -T $(USER_LD) -Wl,--build-id=none \
	  -o $@ $(USER_SRCS)

$(USB_DISK): $(TOP)/tools/mkusbdisk.py $(TOP)/tools/mkfat.py
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mkusbdisk.py $@ 40

$(VOL_STAMP): $(TOP)/tools/tests/mkvoldisk.py $(TOP)/tools/mkfat.py $(TOP)/tools/mkdisk.py
	@mkdir -p $(VOL_DIR)
	@python3 $(TOP)/tools/tests/mkvoldisk.py $(VOL_DIR)

$(BTCD_IMG): $(TOP)/tools/tests/mkbtcd.py
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/tests/mkbtcd.py $@

$(USB_DISK4): $(TOP)/tools/mkfat.py
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mkfat.py make 32 $@ 34816

$(USB_DISK5): $(TOP)/tools/mkfat.py
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mkfat.py make floppy $@

$(USB_DISK2) $(USB_DISK3):
	@mkdir -p $(dir $@)
	@dd if=/dev/zero of=$@ bs=1048576 count=4 2>/dev/null

# システムの書体と壁紙。etc/font/ と etc/wall/ のファイルは、起動時に取り込まず、
# ビルドのときに実身として載せる: メタデータとレコード0は tools/mkobj.py def で
# etc/def に作ってあり、ここではファイルそのものをレコード1(obj/rec/sys/{uuid}_1.bin)
# にする(設計書 18.20)。書体は資産なのでリポジトリに入れない(設計書 17.1)。
# TEST_FONTS は書体箱に入れず、試験用ディスクの /boot にファイルのまま載せる
# (koganei.ttf は出どころとライセンスが分からないので、TrueType の試験 ktest_fn だけが読む)
TEST_FONTS ?= $(wildcard $(TOP)/etc/font/koganei.ttf)
SYS_FILES ?= $(filter-out $(TEST_FONTS),$(wildcard $(TOP)/etc/font/*.ttf) $(wildcard $(TOP)/etc/font/*.otf) \
	     $(wildcard $(TOP)/etc/wall/*.PIC))
SYS_RECS  := $(UOBJ)/rec/sys.stamp

$(SYS_RECS): $(SYS_FILES) $(TOP)/tools/mkobj.py
	@rm -rf $(UOBJ)/rec/sys
	@python3 $(TOP)/tools/mkobj.py rec $(UOBJ)/rec/sys $(SYS_FILES)
	@touch $@

# 試験用の記録。etc/xtad/ に .XTD を置くと試験用ディスクに載り、
# ktest_tv が読んで画面に出す(設計書 17.5)
KTEST_XTAD ?= $(wildcard $(TOP)/etc/xtad/*.xtad) $(wildcard $(TOP)/etc/xtad/*.json) $(wildcard $(TOP)/etc/xtad/*.png) $(wildcard $(TOP)/etc/xtad/*.ico) \
	      $(wildcard $(TOP)/etc/xtad/*.bin)

# 定義実身。etc/def/ のメタデータとレコード0がそのままシステムの実身の置き場に載る(設計書 18.15)
KTEST_DEF ?= $(wildcard $(TOP)/etc/def/*.json) $(wildcard $(TOP)/etc/def/*.xtad) $(wildcard $(TOP)/etc/def/*.ico)

# 利用者プログラムの共通部分(lib/libts)と、それで作る小物
LIBTS_SRCS := $(TOP)/lib/libts/$(UARCH_DIR)crt0.S $(TOP)/lib/libts/$(UARCH_DIR)svc_stub.S $(TOP)/lib/libts/libc.c \
	      $(TOP)/lib/libts/ts_face.c $(TOP)/lib/libts/ts_random.c $(TOP)/lib/libts/ts_admin.c
CLOCK_ELF  := $(UOBJ)/clock.elf
# 時計のプログラム実身のレコード1。メタデータとレコード0は etc/def にある(設計書 18.20)
CLOCK_REC  := $(UOBJ)/rec/01a0d8c4-5212-7a19-8d63-e04b5c9f2a71_1.bin

$(CLOCK_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/application/clock/clock.c $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARCHFLAGS) $(UARCH_NOFP) -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none \
	  -o $@ $(LIBTS_SRCS) $(TOP)/application/clock/clock.c

$(CLOCK_REC): $(CLOCK_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# コンソールとシリアル通信の小物。どちらも行の窓(application/lterm)で描く。
# プログラム実身のレコード1で、メタデータとレコード0は etc/def にある(設計書 16.5.16)
LTERM_SRCS  := $(TOP)/application/lterm/lterm.c $(TOP)/application/lterm/lt_vt.c
CONSOLE_ELF := $(UOBJ)/console.elf
CONSOLE_REC := $(UOBJ)/rec/01a0d8c4-5219-7c3e-8a41-2f6b9d0e5c11_1.bin
SERIAL_ELF  := $(UOBJ)/serial.elf
SERIAL_REC  := $(UOBJ)/rec/01a0d8c4-521a-7d52-9b17-4e8c2a6f0d23_1.bin
APP_CFLAGS  := $(ARCHFLAGS) $(UARCH_NOFP) -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none

# コンソールは、いまいる箱と名前でのプログラムの実行(con_box.c・con_job.c)、
# かな漢字変換の入力(con_ime.c)を持つ。実身のメタデータは lib/libjson で読む(設計書 16.5.19)
CONSOLE_SRCS := $(addprefix $(TOP)/application/console/,console.c con_box.c con_job.c con_ime.c) \
		$(TOP)/lib/libjson/json.c $(TOP)/lib/libposix/$(UARCH_DIR)psx_stub.S

$(CONSOLE_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(LTERM_SRCS) $(CONSOLE_SRCS) $(TOP)/application/console/console.h \
		$(TOP)/application/lterm/lterm.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(LTERM_SRCS) $(CONSOLE_SRCS)

# コマンド(窓を持たずコンソールで動くプログラム。実身 bin に仮身がある、設計書 16.5.19)。hello は application/hello を HELLO_CMD で建てた版で、
# 引数を文字で受ける(/boot の HELLO.ELF と init は元のまま)
HELLO_CMD_ELF := $(UOBJ)/hello_cmd.elf
HELLO_CMD_REC := $(UOBJ)/rec/01a0d8c4-5462-7c93-8ea4-bc1d2e3f5462_1.bin

$(HELLO_CMD_ELF): $(APP_HDRS) $(USER_SRCS) $(USER_LD)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARCHFLAGS) $(UARCH_NOFP) -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g -DHELLO_CMD \
	  -I$(TOP)/include -T $(USER_LD) -Wl,--build-id=none \
	  -o $@ $(USER_SRCS)

$(HELLO_CMD_REC): $(HELLO_CMD_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(HELLO_CMD_REC)

$(SERIAL_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(LTERM_SRCS) $(TOP)/application/serial/serial.c \
		$(TOP)/application/lterm/lterm.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(LTERM_SRCS) $(TOP)/application/serial/serial.c

# プログラムの TLS(マイクロスクリプトの HTTPS が使う)
include tls.mk

# マイクロスクリプト: 図形を舞台に台本を動かすアプリ。浮動小数と C ライブラリ(newlib)を使うので
# -mgeneral-regs-only を外し、libm・libc を結ぶ。プログラム実身のレコード1(設計書 17.15)
MS_DIR      := $(TOP)/application/microscript
MS_SRCS     := $(MS_DIR)/$(UARCH_DIR)ms_co.S $(MS_DIR)/ms_libc.c $(MS_DIR)/ms_xml.c $(MS_DIR)/ms_fig.c 	       $(MS_DIR)/ms_lex.c $(MS_DIR)/ms_parse.c $(MS_DIR)/ms_run.c $(MS_DIR)/ms_rtlib.c 	       $(MS_DIR)/ms_stage.c $(MS_DIR)/ms_main.c $(MS_DIR)/ms_jis.c $(MS_DIR)/ms_vobj.c $(MS_DIR)/ms_shared.c $(MS_DIR)/ms_ime.c $(MS_DIR)/ms_net.c $(TOP)/lib/libjson/json.c $(TOP)/lib/libui/ui.c
# lib/libts の libc.c は使わない: 同じ関数を newlib が持ち、最適化すると自分を呼ぶ
MS_LIBTS    := $(TOP)/lib/libts/$(UARCH_DIR)crt0.S $(TOP)/lib/libts/$(UARCH_DIR)svc_stub.S $(TOP)/lib/libts/ts_random.c \
	       $(TOP)/lib/libts/ts_admin.c
ifneq ($(UARCH_LIBC),)
MS_LIBTS    += $(UARCH_LIBC)
endif
MS_ELF      := $(UOBJ)/microscript.elf
MS_REC      := $(UOBJ)/rec/01a0d8c4-521b-7e63-8c28-5f9d3b7a1e34_1.bin
MS_CFLAGS   := $(ARCHFLAGS) $(UARCH_FP) -fno-stack-protector -O2 -g -DCHK_TKERNEL_CONST 	       -nostdlib -nostartfiles -static 	       -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none

$(MS_ELF): $(APP_HDRS) $(MS_LIBTS) $(MS_SRCS) $(MS_DIR)/ms.h $(MS_DIR)/ms_run.h $(USER_LD) $(TOP)/include/ts/svc.h $(TLS_LIB)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(MS_CFLAGS) -o $@ $(MS_LIBTS) $(MS_SRCS) $(TLS_LIB) -lm -lc -lgcc

$(MS_REC): $(MS_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# 書庫(BPK)・LH5・二進 TAD を読む部分(lib/libbpk、include/ts/bpk.h)。系の呼出しをせず、
# 書庫管理が結ぶ。LH5 の復号(bpk_lh5.c)と二進 TAD の読み書きはカーネルの試験も結ぶ(設計書 17.16)。
# 実時間制御TADの要素とセグメントの表は bpk_rt.c(設計書 15.7)
LIBBPK_DIR  := $(TOP)/lib/libbpk
LIBBPK_SRCS := $(LIBBPK_DIR)/bpk_arc.c $(LIBBPK_DIR)/bpk_lh5.c $(LIBBPK_DIR)/bpk_tad.c $(LIBBPK_DIR)/bpk_png.c \
	       $(LIBBPK_DIR)/bpk_tron.c $(LIBBPK_DIR)/bpk_jis.c $(LIBBPK_DIR)/bpk_util.c $(LIBBPK_DIR)/bpk_fax.c \
	       $(LIBBPK_DIR)/bpk_rt.c $(LIBBPK_DIR)/bpk_lh5e.c $(LIBBPK_DIR)/bpk_pack.c

# 書庫管理: 仮身で結ばれた実身のまとまりを BTRON の書庫の書籍にまとめ、取り出すアプリ。取り込んだ
# .bpk の実身も開いて取り出す。書庫の書き出しと LH5 の符号化は lib/libbpk(bpk_pack.c、bpk_lh5e.c)、
# xmlTAD から二進 TAD へは lib/libbtbk/btbk_tad.c、取り出しは unp_obj.c(ファイル変換も結ぶ)。
# newlib を使う。プログラム実身のレコード1(設計書 11.20)
ARC_CFLAGS  := $(MS_CFLAGS) -Wall -Wno-unused-parameter
ARC_DIR     := $(TOP)/application/archive
ARC_SRCS    := $(ARC_DIR)/arc_main.c $(ARC_DIR)/arc_pack.c $(ARC_DIR)/unp_libc.c $(ARC_DIR)/unp_obj.c \
	       $(LIBBPK_SRCS) $(TOP)/lib/libbtbk/btbk_tad.c $(TOP)/lib/libjson/json.c $(TOP)/lib/libui/ui.c
ARC_ELF     := $(UOBJ)/archive.elf
ARC_REC     := $(UOBJ)/rec/01a0d8c4-5370-7a71-8e82-9fab0c1d7070_1.bin

$(ARC_ELF): $(APP_HDRS) $(MS_LIBTS) $(ARC_SRCS) $(ARC_DIR)/arc.h $(ARC_DIR)/unp.h $(TOP)/include/ts/bpk.h \
	    $(TOP)/include/ts/btbk.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARC_CFLAGS) -o $@ $(MS_LIBTS) $(ARC_SRCS) -lc -lgcc

$(ARC_REC): $(ARC_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(ARC_REC)

# バックアップ: BTRON のバックアップ書庫に実身を保存し、そこから復帰する小物。書庫の形式は
# lib/libbtbk、TAD の読み書きと TRON コードは lib/libbpk、巻のファイル名は lib/libxfu。
# プログラム実身のレコード1(設計書 17.18)
LIBBTBK_SRCS := $(TOP)/lib/libbtbk/btbk_lzss.c $(TOP)/lib/libbtbk/btbk_arc.c $(TOP)/lib/libbtbk/btbk_tad.c
BKP_DIR     := $(TOP)/application/backup
BKP_SRCS    := $(BKP_DIR)/bk_libc.c $(BKP_DIR)/bk_util.c $(BKP_DIR)/bk_media.c $(BKP_DIR)/bk_vol.c \
	       $(BKP_DIR)/bk_save.c $(BKP_DIR)/bk_rest.c $(BKP_DIR)/bk_main.c $(LIBBTBK_SRCS) $(LIBBPK_SRCS) \
	       $(TOP)/lib/libjson/json.c $(TOP)/lib/libxfu/xfu_name.c $(TOP)/lib/libxfu/xfu_util.c \
	       $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c $(TOP)/lib/libui/ui.c
BKP_ELF     := $(UOBJ)/backup.elf
BKP_REC     := $(UOBJ)/rec/01a0d8c4-52b0-7d15-9a3c-4e6f8b2d1c07_1.bin
BKP_CFLAGS  := $(MS_CFLAGS) -Wall -Wno-unused-parameter -Wno-stringop-truncation -Wno-format-truncation

$(BKP_ELF): $(APP_HDRS) $(MS_LIBTS) $(BKP_SRCS) $(BKP_DIR)/bk.h $(BKP_DIR)/bk_arg.h $(BKP_DIR)/bk_vol.h \
	    $(TOP)/include/ts/btbk.h $(TOP)/include/ts/bpk.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(BKP_CFLAGS) -o $@ $(MS_LIBTS) $(BKP_SRCS) -lc -lgcc

$(BKP_REC): $(BKP_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# ファイル変換: 媒体・FTP のファイルと実身の相互変換の小物。newlib を使う。
# 変換の部品(lib/libxfu・lib/libtxc・lib/libftp)を結ぶ(設計書 17.17)。BTRON のファイルシステムの
# ディスク(BTRON の CD-ROM)は lib/libbtfs で読み、書庫管理の unp_obj.c で実身にする(設計書 11.9)
LIBBTFS_SRCS := $(TOP)/lib/libbtfs/btfs.c
XFC_DIR     := $(TOP)/application/xfconv
XFC_SRCS    := $(XFC_DIR)/xfc_libc.c $(XFC_DIR)/xfc_net.c $(XFC_DIR)/xfc_main.c $(XFC_DIR)/xfc_draw.c 	       $(XFC_DIR)/xfc_dlg.c $(XFC_DIR)/xfc_btron.c $(TOP)/lib/libui/ui.c \
	       $(TOP)/lib/libxfu/xfu_util.c $(TOP)/lib/libxfu/xfu_name.c $(TOP)/lib/libxfu/xfu_obj.c \
	       $(TOP)/lib/libxfu/xfu_imp.c $(TOP)/lib/libxfu/xfu_exp.c $(TOP)/lib/libxfu/xfu_fs.c \
	       $(TOP)/lib/libxfu/xfu_ftp.c $(TOP)/lib/libxfu/xfu_prc.c \
	       $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c $(TOP)/lib/libftp/ftp.c \
	       $(LIBBPK_SRCS) $(LIBBTFS_SRCS) $(ARC_DIR)/unp_obj.c $(TOP)/lib/libjson/json.c
XFC_ELF     := $(UOBJ)/xfconv.elf
XFC_REC     := $(UOBJ)/rec/01a0d8c4-52b1-7c21-9a31-4f5e6d7c8b91_1.bin
# 系の答える誤りの番号は下位の番号を持たない(sys/profile.h の TK_SUPPORT_SERCD)。
# プログラムの側でも E_OBJ などを同じ値にするため、どのファイルでも tk/errno.h が
# そう読むように言っておく(TRUE は tk/typedef.h と同じ値)
XFC_CFLAGS  := $(MS_CFLAGS) -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation 	       -DTK_SUPPORT_SERCD=0 -DTRUE=1

$(XFC_ELF): $(APP_HDRS) $(MS_LIBTS) $(XFC_SRCS) $(XFC_DIR)/xfc.h $(TOP)/lib/libxfu/xfu_in.h $(TOP)/include/ts/xfu.h 	    $(TOP)/include/ts/ui.h $(TOP)/include/ts/dtreq.h \
	    $(TOP)/include/ts/ftp.h $(TOP)/include/ts/txc.h $(TOP)/include/ts/btfs.h $(TOP)/include/ts/bpk.h \
	    $(ARC_DIR)/unp.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(XFC_CFLAGS) -o $@ $(MS_LIBTS) $(XFC_SRCS) -lc -lgcc

$(XFC_REC): $(XFC_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# 設定の小物: ネットワーク設定・システム環境設定・ユーザ環境設定。窓の部品は lib/libui、
# 設定の行は lib/libconf(カーネルと同じファイル)。newlib を使う。プログラム実身の
# レコード1(設計書 16.5.23)
UI_SRCS      := $(TOP)/lib/libui/ui_libc.c $(TOP)/lib/libui/ui.c $(TOP)/lib/libconf/conf.c $(TOP)/lib/libjson/json.c
ENV_CFLAGS   := $(MS_CFLAGS) -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation
ENV_HDRS     := $(APP_HDRS) $(MS_LIBTS) $(UI_SRCS) $(TOP)/include/ts/ui.h $(TOP)/include/ts/conf.h \
		$(USER_LD) $(TOP)/include/ts/svc.h
NETENV_ELF   := $(UOBJ)/netenv.elf
NETENV_REC   := $(UOBJ)/rec/01a0d8c4-52b5-7a41-8c61-2d7e9f0a1b31_1.bin

$(NETENV_ELF): $(ENV_HDRS) $(TOP)/application/netenv/netenv.c
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/netenv/netenv.c -lc -lgcc

$(NETENV_REC): $(NETENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

USERENV_ELF  := $(UOBJ)/userenv.elf
USERENV_REC  := $(UOBJ)/rec/01a0d8c4-52b7-7c63-8e83-4f9a012c3d53_1.bin

$(USERENV_ELF): $(ENV_HDRS) $(TOP)/application/userenv/userenv.c $(TOP)/include/ts/look.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/userenv/userenv.c -lc -lgcc

$(USERENV_REC): $(USERENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

SYSENV_ELF   := $(UOBJ)/sysenv.elf
SYSENV_REC   := $(UOBJ)/rec/01a0d8c4-52b6-7b52-9d72-3e8fa01b2c42_1.bin

$(SYSENV_ELF): $(ENV_HDRS) $(TOP)/application/sysenv/sysenv.c $(TOP)/include/ts/look.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/sysenv/sysenv.c -lc -lgcc

$(SYSENV_REC): $(SYSENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# 音量ミキサー: サウンドデバイスの全体とプログラムごとの音量(設計書 11.6)
SNDMIX_ELF   := $(UOBJ)/sndmix.elf
SNDMIX_REC   := $(UOBJ)/rec/01a0d8c4-5440-7a51-8c62-7e8f90a15440_1.bin

$(SNDMIX_ELF): $(ENV_HDRS) $(TOP)/application/sndmix/sndmix.c $(TOP)/include/ts/snd.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/sndmix/sndmix.c -lc -lgcc

$(SNDMIX_REC): $(SNDMIX_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

ENV_RECS     := $(NETENV_REC) $(USERENV_REC) $(SYSENV_REC) $(SNDMIX_REC)

# 予定表: 月の表に日の予定の実身を並べるアプリ。上の帯の部品は lib/libui、年月の実身の
# 読み書きは application/calendar/cal_tad.c(カーネルの試験も結ぶ)。newlib を使う。
# プログラム実身のレコード1(設計書 11.10)
CAL_DIR      := $(TOP)/application/calendar
CAL_SRCS     := $(CAL_DIR)/cal_tad.c $(CAL_DIR)/cal_main.c $(TOP)/lib/libui/ui_libc.c \
		$(TOP)/lib/libui/ui.c $(TOP)/lib/libui/ui_prn.c $(TOP)/lib/libjson/json.c
CAL_ELF      := $(UOBJ)/calendar.elf
CAL_REC      := $(UOBJ)/rec/01a0d8c4-52d4-7b31-9c42-5d6e7f809a12_1.bin

$(CAL_ELF): $(APP_HDRS) $(MS_LIBTS) $(CAL_SRCS) $(CAL_DIR)/cal_tad.h $(TOP)/include/ts/ui.h \
	    $(TOP)/include/ts/sysdef.h $(TOP)/include/ts/dtreq.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(CAL_SRCS) -lc -lgcc

$(CAL_REC): $(CAL_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(CAL_REC)

# かな入力練習: 問題文を打ち、次に打つキーをキーボードの図に示すアプリ。キーボードとかなの
# 配列の表は lib/libkbmap、判定と成績は application/kanatype/kp_eng.c(どちらもカーネルの試験も
# 結ぶ)。結果のパネルは lib/libui。newlib を使う。プログラム実身のレコード1(設計書 11.14)
KANA_DIR     := $(TOP)/application/kanatype
KBMAP_SRCS   := $(TOP)/lib/libkbmap/kbmap.c $(TOP)/lib/libkbmap/kbmap_tab.c
KANA_SRCS    := $(KANA_DIR)/kp_eng.c $(KANA_DIR)/kp_main.c $(KBMAP_SRCS) $(TOP)/lib/libui/ui_libc.c \
		$(TOP)/lib/libui/ui.c $(TOP)/lib/libjson/json.c
KANA_ELF     := $(UOBJ)/kanatype.elf
KANA_REC     := $(UOBJ)/rec/01a0d8c4-52f0-7a01-8b12-3c4d5e6f7a90_1.bin

$(KANA_ELF): $(APP_HDRS) $(MS_LIBTS) $(KANA_SRCS) $(KANA_DIR)/kp_eng.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(KANA_SRCS) -lc -lgcc

$(KANA_REC): $(KANA_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(KANA_REC)

# 単語登録: かな漢字変換のユーザ辞書に単語を登録する小物。辞書の行の読み書きは lib/libudic
# (カーネルの変換器と試験も結ぶ)、窓の部品は lib/libui。newlib を使う。プログラム実身の
# レコード1(設計書 11.16)
WD_DIR       := $(TOP)/application/worddic
WD_SRCS      := $(WD_DIR)/wd_main.c $(TOP)/lib/libudic/udic.c $(TOP)/lib/libui/ui_libc.c \
		$(TOP)/lib/libui/ui.c $(TOP)/lib/libjson/json.c
WD_ELF       := $(UOBJ)/worddic.elf
WD_REC       := $(UOBJ)/rec/01a0d8c4-5312-7c23-8d34-5e6f7b8c9d12_1.bin

$(WD_ELF): $(APP_HDRS) $(MS_LIBTS) $(WD_SRCS) $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(WD_SRCS) -lc -lgcc

$(WD_REC): $(WD_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(WD_REC)

# キー配列: キーボードの配列を絵で示し、絵のキーで入力する画面キーボードの小物。窓は入力の対象を
# 取らない(WM_ATTR_NOFOCUS)。配列と付け替えは km_get・km_set、キーの入力は wm_key_put。newlib を
# 使う。プログラム実身のレコード1(設計書 11.15)
KM_DIR       := $(TOP)/application/keymap
KM_SRCS      := $(KM_DIR)/km_main.c $(TOP)/lib/libui/ui_libc.c $(TOP)/lib/libui/ui.c $(TOP)/lib/libjson/json.c
KM_ELF       := $(UOBJ)/keymap.elf
KM_REC       := $(UOBJ)/rec/01a0d8c4-52e8-7b19-9c2a-3d4e5f6a7b84_1.bin

$(KM_ELF): $(APP_HDRS) $(MS_LIBTS) $(KM_SRCS) $(TOP)/include/ts/keymap.h $(TOP)/include/ts/wm.h \
	   $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(KM_SRCS) -lc -lgcc

$(KM_REC): $(KM_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(KM_REC)

# ソフトウェアキーボード: ポインタやタッチパネルで押す画面のキーボードの小物。窓は入力の対象を取らず
# (WM_ATTR_NOFOCUS)、ほかの窓より前に浮かび(WM_ATTR_FLOAT)、大きさを変えられ、キーは窓の大きさに
# 合わせて伸び縮みする。配列は英字(QWERTY)、TRON 配列(sk_tron.c)、μTRON 配列(sk_utron.c、左右の窓)。キーの形は縁をなめらかにして描く(sk_draw.c)。キーの入力は wm_key_put、選んだ
# 配列はユーザ情報の実身の行(lib/libconf)。newlib を使う。プログラム実身のレコード1(設計書 11.31)
SK_DIR       := $(TOP)/application/softkb
SK_SRCS      := $(SK_DIR)/sk_main.c $(SK_DIR)/sk_tron.c $(SK_DIR)/sk_utron.c $(SK_DIR)/sk_draw.c $(TOP)/lib/libui/ui_libc.c $(TOP)/lib/libui/ui.c \
		$(TOP)/lib/libconf/conf.c $(TOP)/lib/libjson/json.c
SK_ELF       := $(UOBJ)/softkb.elf
SK_REC       := $(UOBJ)/rec/01a0d8c4-54b0-7ac1-8cd2-9fab0c1d54b0_1.bin

$(SK_ELF): $(APP_HDRS) $(MS_LIBTS) $(SK_SRCS) $(SK_DIR)/sk.h $(TOP)/include/ts/keymap.h $(TOP)/include/ts/wm.h \
	   $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(SK_SRCS) -lc -lgcc

$(SK_REC): $(SK_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(SK_REC)

# 電卓: 四則、メモリ、関数電卓の小物。数は十進(cu_dec.c。カーネルの試験も結ぶ)で持ち、関数だけを
# 倍精度(cu_fn.c、libm)で求める。窓の部品は lib/libui。newlib を使う。プログラム実身のレコード1(設計書 11.17)
CU_DIR       := $(TOP)/application/calculator
CU_SRCS      := $(CU_DIR)/cu_main.c $(CU_DIR)/cu_eng.c $(CU_DIR)/cu_dec.c $(CU_DIR)/cu_fn.c \
		$(TOP)/lib/libui/ui_libc.c $(TOP)/lib/libui/ui.c
CU_ELF       := $(UOBJ)/calculator.elf
CU_REC       := $(UOBJ)/rec/01a0d8c4-5320-7a41-8b52-6c7d8e9fa020_1.bin

$(CU_ELF): $(APP_HDRS) $(MS_LIBTS) $(CU_SRCS) $(CU_DIR)/cu_dec.h $(CU_DIR)/cu_eng.h $(CU_DIR)/cu_cases.h \
	   $(TOP)/include/ts/ui.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(CU_SRCS) -lm -lc -lgcc

$(CU_REC): $(CU_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(CU_REC)

# マイクロカード: カード型データベース。カードと記録(マイクロカード実身のレコードタイプ31)、検索、
# 並べ替え、書き出しと読み込み、差し込みは application/mcard の mc_db.c ほか(カーネルの試験も結ぶ)、
# 表計算の記録は application/calc/calc_tad.c、TRON コードは lib/libbpk、ファイルの文字コードは
# lib/libtxc、窓の部品は lib/libui。newlib を使う。プログラム実身のレコード1(設計書 11.18)
MC_DIR       := $(TOP)/application/mcard
MC_ENG       := $(addprefix $(MC_DIR)/,mc_db.c mc_bin.c mc_rx.c mc_var.c mc_io.c mc_merge.c mc_prn.c)
MC_SRCS      := $(addprefix $(MC_DIR)/,mc_main.c mc_card.c mc_list.c mc_edit.c mc_dlg.c mc_obj.c mc_libc.c) \
		$(MC_ENG) $(TOP)/application/calc/calc_tad.c $(TOP)/application/calendar/cal_tad.c \
		$(LIBBPK_DIR)/bpk_tron.c $(LIBBPK_DIR)/bpk_jis.c $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c \
		$(TOP)/lib/libui/ui.c $(TOP)/lib/libui/ui_prn.c $(TOP)/lib/libjson/json.c
MC_ELF       := $(UOBJ)/mcard.elf
MC_REC       := $(UOBJ)/rec/01a0d8c4-5330-7a51-8c62-7d8e9fab3030_1.bin

$(MC_ELF): $(APP_HDRS) $(MS_LIBTS) $(MC_SRCS) $(MC_DIR)/mc.h $(MC_DIR)/mc_db.h $(TOP)/application/calc/calc_tad.h \
	   $(TOP)/application/calendar/cal_tad.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(MC_SRCS) -lc -lgcc

$(MC_REC): $(MC_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(MC_REC)

# 電子手帳: 予定表、カレンダー、住所録を1つの文章の実身に持つアプリ。実身の読み書き、休日、検索、
# 印刷の頁は application/techo/tc_db.c(カーネルの試験も結ぶ)、文章を書く部品は
# application/calendar/cal_tad.c、窓の部品は lib/libui。newlib を使う。プログラム実身のレコード1(設計書 11.23)
TC_DIR       := $(TOP)/application/techo
TC_SRCS      := $(addprefix $(TC_DIR)/,tc_main.c tc_draw.c tc_edit.c tc_db.c) \
		$(TOP)/application/calendar/cal_tad.c $(TOP)/lib/libui/ui_libc.c $(TOP)/lib/libui/ui.c \
		$(TOP)/lib/libui/ui_prn.c $(TOP)/lib/libjson/json.c
TC_ELF       := $(UOBJ)/techo.elf
TC_REC       := $(UOBJ)/rec/01a0d8c4-53c0-7a11-8b22-3c4d5e6f53c0_1.bin

$(TC_ELF): $(APP_HDRS) $(MS_LIBTS) $(TC_SRCS) $(TC_DIR)/tc.h $(TC_DIR)/tc_db.h \
	   $(TOP)/application/calendar/cal_tad.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -DUI_ARENA_SIZE='(16*1024*1024)' -o $@ $(MS_LIBTS) $(TC_SRCS) -lc -lgcc

$(TC_REC): $(TC_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(TC_REC)

# 文字検索: 文字をコード一覧、漢字のよみ・画数・部品、記号の名称・部品から探して入力する小物。辞書と検索と
# 文字集合は application/charsearch の cs_dic.c と cs_code.c(カーネルの試験も結ぶ)、TRON コードは lib/libbpk、
# 窓の部品は lib/libui。newlib を使う。プログラム実身のレコード1(設計書 11.19)
CS_DIR       := $(TOP)/application/charsearch
CS_SRCS      := $(CS_DIR)/cs_main.c $(CS_DIR)/cs_dic.c $(CS_DIR)/cs_code.c $(CS_DIR)/cs_libc.c \
		$(LIBBPK_DIR)/bpk_tron.c $(LIBBPK_DIR)/bpk_jis.c $(TOP)/lib/libui/ui.c
CS_ELF       := $(UOBJ)/charsearch.elf
CS_REC       := $(UOBJ)/rec/01a0d8c4-5350-7a61-8d72-9fab0c1d5050_1.bin

$(CS_ELF): $(APP_HDRS) $(MS_LIBTS) $(CS_SRCS) $(CS_DIR)/cs_dic.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -I$(CS_DIR) -o $@ $(MS_LIBTS) $(CS_SRCS) -lc -lgcc

$(CS_REC): $(CS_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(CS_REC)

# 文字検索の辞書実身のレコード1。tools/gen_chardic.py が CHARDIC_SRC に取得した公開データ(Unihan、
# KANJIDIC2、KRADFILE、KanjiVG。取得は tools/gen_chardic.py fetch ~/chardic)と Python の文字表から作る。
# データがなければ、よみと部品のない辞書になる。メタデータとレコード0は etc/def にある(設計書 11.19)
CHARDIC_SRC  ?= $(HOME)/chardic
CHARDIC_REC  := $(UOBJ)/rec/01a0d8c4-5354-7d94-8a05-cd2e3f405454_1.bin

$(CHARDIC_REC): $(TOP)/tools/gen_chardic.py $(wildcard $(CHARDIC_SRC)/*.zip $(CHARDIC_SRC)/*.gz) \
		$(wildcard $(TOP)/etc/font/NotoSansJP-Regular.otf)
	@mkdir -p $(dir $@)
	@echo 'GEN $@'
	@python3 $(TOP)/tools/gen_chardic.py make $(CHARDIC_SRC) $@.tmp --font $(TOP)/etc/font/NotoSansJP-Regular.otf
	@mv $@.tmp $@

EXTRA_RECS += $(CHARDIC_REC)

# 実時間メディア: 実時間制御TADの音(音声ファイル、音符、MIDI、PCM)を鳴らすプロセス。デスクトップの
# 再生部(application/desktop/dtrt.c)が起動する。newlib と libm、MP3 は minimp3。プログラム実身の
# レコード1(設計書 15.6)
RTM_DIR      := $(TOP)/application/rtmedia
RTM_SRCS     := $(RTM_DIR)/rtm_main.c $(RTM_DIR)/rtm_mix.c $(RTM_DIR)/rtm_synth.c $(RTM_DIR)/rtm_dec.c \
		$(RTM_DIR)/rtm_libc.c
RTM_ELF      := $(UOBJ)/rtmedia.elf
RTM_REC      := $(UOBJ)/rec/01a0d8c4-52da-7c34-9d45-be6f708192a3_1.bin
RTM_CFLAGS   := $(ARCHFLAGS) $(UARCH_FP) -fno-stack-protector -O2 -g -nostdlib -nostartfiles -static \
		-I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none -Wall -Wno-unused-parameter

$(RTM_ELF): $(APP_HDRS) $(MS_LIBTS) $(RTM_SRCS) $(RTM_DIR)/rtm.h $(RTM_DIR)/minimp3.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(RTM_CFLAGS) -o $@ $(MS_LIBTS) $(RTM_SRCS) -lm -lc -lgcc

$(RTM_REC): $(RTM_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(RTM_REC)

# C++ の実行時とスレッド、それで作るプログラム(設計書 17.19)
include cxxrt.mk
# POSIX のプログラム(コマンドとシェル)の土台 lib/libposix(設計書 5.8)
include posix.mk
# ブラウザ(Chromium の移植)は別のリポジトリ tessron-chromium にあり、make BROWSER=1 の
# ときだけそこの browser.mk を読んで、イメージに取り込む。置き場は CHROMIUM で変えられる
CHROMIUM ?= $(abspath $(TOP)/../tessron-chromium)
# Raspberry Pi のカードの像のシステムのボリュームは 2GB にする。使う人の実身の置き場でもあり、
# 中身ぎりぎりの大きさでは原紙からの実身も作れなくなる。browser.mk の大きさより先に決める
ifneq ($(filter _RPI5_ _RPI4_,$(TARGET)),)
SYS_VOL_SIZE ?= 2G
endif
ifeq ($(BROWSER),1)
ifeq ($(wildcard $(CHROMIUM)/build_make/browser.mk),)
$(error BROWSER=1: no $(CHROMIUM)/build_make/browser.mk (CHROMIUM=<tessron-chromium>))
endif
include $(CHROMIUM)/build_make/browser.mk
endif

# コマンドの v8prog(実身 bin に仮身がある、設計書 16.5.19): browser.mk が作る obj/v8prog.elf を strip してレコード1にする。
# (x86-64 では $(BROWSER_OUT) すなわち obj/x64 の v8prog.elf)
# システムのボリュームだけに載せる(strip 後で約47MB)。CMD_V8=0 で外す。外すと実身は実行形式なしで残る
CMD_V8 ?= $(BROWSER)
ifeq ($(CMD_V8)$(filter v8prog,$(BROWSER_PROGS)),1v8prog)
CMD_V8_REC := $(UOBJ)/rec/01a0d8c4-5464-7eb5-80c6-de3f405f5464_1.bin

$(CMD_V8_REC): $(BROWSER_OUT)/v8prog.elf
	@mkdir -p $(dir $@)
	@$(CROSS)strip -o $@ $<
else
CMD_V8_REC :=
$(shell rm -f $(UOBJ)/rec/01a0d8c4-5464-7eb5-80c6-de3f405f5464_1.bin)
endif

# 基本表計算: 表計算の実身を編集するアプリ。画面と xmlTAD の読み書きは C(application/calc、
# 読み書きの calc_tad.c はカーネルの試験も結ぶ)、数式は V8 で formula.min.js と移植した
# calc_engine.js を動かす(calc_v8.cc)。V8 は tessron-chromium の build.sh v8app が結ぶ。
# CALC_V8=0 なら V8 を結ばず、四則と基本の関数だけの小さな評価器(calc_ceval.c)で動く。
# 既定は BROWSER と同じ。プログラム実身のレコード1(設計書 11.13)
CALC_V8      ?= $(BROWSER)
CALC_DIR     := $(TOP)/application/calc
CALC_COBJ    := $(patsubst %,$(UOBJ)/calc/%.o,calc_main calc_draw calc_edit calc_panel calc_obj calc_chart calc_test \
		calc_print calc_tad cal_tad ui ui_prn json)
CALC_HDRS    := $(CALC_DIR)/calc.h $(CALC_DIR)/calc_tad.h $(CALC_DIR)/calc_eng.h $(CALC_DIR)/calc_cases.h \
		$(TOP)/application/calendar/cal_tad.h $(APP_HDRS)
CALC_CFLAGS  := $(TSPOSIX_CFLAGS) -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation
CALC_REC     := $(UOBJ)/rec/01a0d8c4-52e5-7e86-8f97-0a1b2c3d4e51_1.bin
CALC_STAMP   := $(UOBJ)/calc.stamp
$(shell mkdir -p $(UOBJ); [ -f $(CALC_STAMP) ] && [ "$$(cat $(CALC_STAMP))" = "$(CALC_V8)" ] || echo "$(CALC_V8)" > $(CALC_STAMP))

$(UOBJ)/calc/%.o: $(CALC_DIR)/%.c $(CALC_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CALC_CFLAGS) -c -o $@ $<

$(UOBJ)/calc/cal_tad.o: $(TOP)/application/calendar/cal_tad.c $(CALC_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CALC_CFLAGS) -c -o $@ $<

$(UOBJ)/calc/ui.o: $(TOP)/lib/libui/ui.c $(TOP)/include/ts/ui.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CALC_CFLAGS) -c -o $@ $<

$(UOBJ)/calc/ui_prn.o: $(TOP)/lib/libui/ui_prn.c $(TOP)/include/ts/uiprn.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CALC_CFLAGS) -c -o $@ $<

$(UOBJ)/calc/json.o: $(TOP)/lib/libjson/json.c $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CALC_CFLAGS) -c -o $@ $<

$(UOBJ)/calc/calc_js.o: $(CALC_DIR)/calc_js.S $(wildcard $(CALC_DIR)/js/*.js)
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(ARCHFLAGS) -I$(CALC_DIR)/js -c -o $@ $<

ifeq ($(CALC_V8),1)
CALC_BIN     := $(UOBJ)/calc_v8.elf
# V8 を結んだプログラムは大きいので、試験用ディスクとシステムのボリュームを大きくする
ifneq ($(BROWSER_BLINK),1)
TEST_DISK_MB := 512
endif

$(CALC_BIN): $(CALC_COBJ) $(UOBJ)/calc/calc_js.o $(CALC_DIR)/calc_v8.cc $(CALC_DIR)/calc_eng.h $(CXXRT_LIB) $(PTH_LIB)
	@mkdir -p $(dir $@)
	@echo 'ENG $@'
	@TESSRONOS=$(abspath $(TOP)) LIBDIR=$(abspath $(CXXRT_OUT)) $(BROWSER_ARCH_ENV) bash $(CHROMIUM)/tools/browser/build.sh v8app \
		$(abspath $@) $(abspath $(CALC_DIR)/calc_v8.cc) $(abspath $(CALC_COBJ) $(UOBJ)/calc/calc_js.o)
else
CALC_BIN     := $(UOBJ)/calc_c.elf

$(CALC_BIN): $(CALC_COBJ) $(UOBJ)/calc/calc_ceval.o $(CXXRT_LIB) $(PTH_LIB) $(CXXRT_LD)
	@mkdir -p $(dir $@)
	@echo 'LD  $@'
	@$(call CXXRT_LINK,$@,$(CALC_COBJ) $(UOBJ)/calc/calc_ceval.o)
endif

$(CALC_REC): $(CALC_BIN) $(CALC_STAMP)
	@mkdir -p $(dir $@)
	@$(CROSS)strip -o $@ $<

EXTRA_RECS += $(CALC_REC)

$(CONSOLE_REC): $(CONSOLE_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

$(SERIAL_REC): $(SERIAL_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# mozc の辞書実身のレコード1。mozc のデータセットをそのまま載せる。
# メタデータとレコード0は etc/def にある。MOZC=1 のときだけ(設計書 18.20)
MOZC_DATA ?= $(HOME)/mozc/src/bazel-bin/data_manager/oss/mozc.data
ifeq ($(MOZC),1)
DICT_REC   := $(UOBJ)/rec/01a0d8c4-5216-7f4a-8c27-1d5b3e9a6c02_1.bin
endif

$(DICT_REC): $(MOZC_DATA)
	@mkdir -p $(dir $@)
	@cp $< $@

# システムのボリューム: 定義実身と試験用の記録、書体・壁紙・時計・辞書のレコードを
# tools/mktsfs で TSFS の版数4のボリュームにしたもの。試験用ディスクの区画3になり、
# 起動時に実身の置き場として付く(設計書 11.15、11.17 の T7)。同じファイルは
# 区画2の FAT にも載せたままにしてある(FAT の試験と、区画3が無いときの置き場)
SYS_VOL    := $(OBJDIR)/sysvol.img
# 大きさは、明示されなければ載せる中身の合計に余裕を足して決める(mktsfs --size auto)。
# 明示されて足りなければ mktsfs がそう言って止まる。SYS_VOL_EXCLUDE は、共有の obj/rec に
# あっても、このビルドで有効でないプログラムのレコードとして載せないファイルの名前。
# Raspberry Pi のカードの像の大きさは、browser.mk より前で決めてある
SYS_VOL_SIZE ?= auto
SYS_VOL_EXCLUDE ?=
SYS_VOL_STAMP := $(OBJDIR)/sysvol_size.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(SYS_VOL_STAMP) ] && [ "$$(cat $(SYS_VOL_STAMP))" = "$(SYS_VOL_SIZE)" ] || echo "$(SYS_VOL_SIZE)" > $(SYS_VOL_STAMP))
TSFS_TOOLS  := $(TOP)/tools/mktsfs $(wildcard $(TOP)/tools/tsfs_py/*.py)

# 版(R1.000 の形: R、桁数の決まらない整数、点、3桁)。プログラムの版はプログラム実身の
# メタデータ tessronos.program.version に書く(etc/def)。TS_VERSION はシステムの版で、
# システムの実身の OB_SYS_INFO に VERSION として出る
TS_VERSION   ?= R1.200
VER_STAMP    := obj/version.stamp
$(shell mkdir -p obj; [ -f $(VER_STAMP) ] && [ "$$(cat $(VER_STAMP))" = "$(TS_VERSION)" ] || echo "$(TS_VERSION)" > $(VER_STAMP))
$(OBJDIR)/peripheral_kernel/obj/obsys.o: $(VER_STAMP)
$(OBJDIR)/peripheral_kernel/obj/obsys.o: CFLAGS += -DTS_VERSION='"$(TS_VERSION)"'

# アプリのパッケージ(.tpk): make pkg PKG=<プログラムの id か UUID> で $(UOBJ)/pkg/<id>.tpk。
# 版はプログラム実身のメタデータに書いてあるもの
PKG ?=
pkg: $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(CAL_REC) $(RTM_REC) $(CALC_REC)
	@test -n "$(PKG)" || { echo 'usage: make pkg PKG=<program id>'; exit 1; }
	@mkdir -p $(UOBJ)/pkg
	@python3 $(TOP)/tools/mktpk.py --from $(TOP)/etc/def --from $(TOP)/etc/xtad --from $(UOBJ)/rec --program $(PKG) -o $(UOBJ)/pkg/$(PKG).tpk

# イメージに組み込むアプリのパッケージ(.tpk)。既定は etc/pkg/ に置いたもので、
# make PKGS="a.tpk b.tpk" で直に指定もできる。tools/pkgstage.py が実身を $(PKG_STAGE) に
# 展開し、そのプログラムへの仮身を足したプログラム箱を作る。mktsfs の --over で、
# そのプログラム箱が etc/def のものに代わる(システム環境設定から取り込んだときと
# 同じ形になる)。Raspberry Pi 4 の像のものは rpi4.mk が決める(etc/pkg/rpi4/)
ifeq ($(TARGET),_RPI4_)
PKGS ?=
else ifeq ($(ARCH),x86_64)
# etc/pkg/ のパッケージのプログラムは AArch64 のもの。x86-64 のものは etc/pkg/x64/ に置く
PKGS ?= $(wildcard $(TOP)/etc/pkg/x64/*.tpk)
else
PKGS ?= $(wildcard $(TOP)/etc/pkg/*.tpk)
endif
PKG_STAGE := $(UOBJ)/pkgstage
PKG_BOX   := $(TOP)/etc/def/01a0d6d1-08cf-750a-8479-73bdcfc4823c_0.xtad
PKG_STAMP := $(UOBJ)/pkgs.stamp
$(shell mkdir -p $(UOBJ); [ -f $(PKG_STAMP) ] && [ "$$(cat $(PKG_STAMP))" = "$(strip $(PKGS))" ] || echo "$(strip $(PKGS))" > $(PKG_STAMP))
ifneq ($(strip $(PKGS)),)
PKG_OVER  := --over $(PKG_STAGE)
PKG_HOLD  := --hold $(PKG_STAGE)
PKG_DONE  := $(PKG_STAGE)/.done
$(PKG_DONE): $(PKGS) $(PKG_BOX) $(PKG_STAMP) $(TOP)/tools/pkgstage.py
	@python3 $(TOP)/tools/pkgstage.py --box $(PKG_BOX) --out $(PKG_STAGE) $(PKGS)
	@touch $@
else
PKG_OVER  :=
PKG_HOLD  :=
PKG_DONE  :=
endif

# wpa_supplicant のプロセス(WIFI=1 WPA_MODE=proc、設計書 9.6.11)
include wpaproc.mk

$(SYS_VOL): $(TSFS_TOOLS) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF) $(PKG_DONE) $(PKG_STAMP) $(SYS_VOL_STAMP)
	@mkdir -p $(dir $@)
	@rm -f $@
	@python3 $(TOP)/tools/mktsfs -q -L "TessronOS system" --size $(SYS_VOL_SIZE) \
	  $(addprefix --exclude ,$(SYS_VOL_EXCLUDE)) \
	  $(PKG_OVER) $(CMD_OVER) $(WIFI_OVER) --from $(TOP)/etc/def --from $(TOP)/etc/xtad --from $(UOBJ)/rec --from $(UOBJ)/rec/sys \
	  --hold $(TOP)/etc/def --hold $(TOP)/etc/xtad $(PKG_HOLD) $(WPAP_HOLD) \
	  --sysbox 01a0d8c4-5211-7c52-9e0a-3b61d27f4c85 $@

$(SYS_VOL): $(CMD_V8_REC)

# 試験が起こすプロセス(tests/uprog)。/boot に SVCPROG.ELF として載り、ktest_svcio が
# ファイル・マウント・ソケットの SVC を EL0 から通す
SVCPROG_ELF := $(UOBJ)/svcprog.elf

$(SVCPROG_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/tests/uprog/svcprog.c $(USER_LD) $(TOP)/include/ts/svc.h \
		$(TOP)/include/ts/uapp.h $(TOP)/include/ts/soapp.h $(TOP)/include/ts/fs.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/tests/uprog/svcprog.c

# 試験が起こすプロセス(tests/uprog/gpprog.c)。/boot に GPPROG.ELF として載り、ktest_gl が
# GPU の SVC(クラス10、include/ts/gpuapp.h)を EL0 から通す
GPPROG_ELF := $(UOBJ)/gpprog.elf

$(GPPROG_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/tests/uprog/gpprog.c $(USER_LD)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/tests/uprog/gpprog.c

EXTRA_BOOT_FILES += $(GPPROG_ELF)
# 縦書きの試験が読む見本の文書(20字×20行のマス目の用紙オーバーレイを持つ)
EXTRA_BOOT_FILES += $(TOP)/tests/ktest/data/genko.xtad
# 書体の試験が TrueType の字形を読むための書体(書体箱には入れない: TEST_FONTS)
EXTRA_BOOT_FILES += $(TEST_FONTS)

$(SD_CARD): $(TOP)/tools/mksd.py $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF)
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mksd.py --sys $(SYS_VOL) --boot-mb 64 --scratch-mb 16 $@ $(USER_ELF)
	@truncate -s $$(( $(SD_CARD_SECTORS) * 512 )) $@

$(TEST_DISK): $(TOP)/tools/mkdisk.py $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(EXTRA_BOOT_FILES) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF) $(TEST_DISK_STAMP)
	@mkdir -p $(dir $@)
	@cp $(USER_ELF) $(UOBJ)/init.elf
	@python3 $(TOP)/tools/mkdisk.py --sys $(SYS_VOL) --sys-room $(TEST_DISK_ROOM) $@ $(TEST_DISK_MB) $(USER_ELF) $(UOBJ)/init.elf $(SVCPROG_ELF) $(EXTRA_BOOT_FILES) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) \
	  $$(ls $(UOBJ)/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF)

test: $(EXE_FILE).elf $(TEST_DEPS)
	QEMU="$(QEMU)" HOST_PY="$(HOST_PY)" $(TOP)/tools/qemu_run.sh $(TEST_EXPECT) $(TEST_SERVER) -- $(QEMU_FLAGS) $(TEST_DRIVE) -kernel $<

# 電源断の試験(設計書 11.14.6): make KTEST=1 KTONLY=tsfscut BUILD_ID=cut powercut。
# 区画1のネイティブのボリュームを変え続ける機械を、途中の好きなところで殺し、
# fsck.tsfs に誤りが無いこと、--fix --relink の後に何も残らないことを見る
PCUT_RUNS ?= 5
powercut: $(EXE_FILE).elf $(TEST_DEPS)
	QEMU="$(QEMU)" python3 $(TOP)/tools/tsfs_powercut.py --runs $(PCUT_RUNS) --disk $(TEST_DISK) 	  -- $(QEMU_FLAGS) $(TEST_DRIVE) -kernel $<

# 設計書の検査: 実身化の確認の表(設計書 18.19)
docs-check:
	python3 $(TOP)/tools/check_bt2.py

# SD カードの像(設計書 13.6、docs/tessronos/03-platform.md 3.3・3.5): make TARGET=_RPI5_ [KTEST=1] BUILD_ID=<id> sdimg で
# $(OBJDIR)/sd.img。区画1(FAT32)に起動のもの(config.txt、デバイスツリー、
# tessronos.img)と試験用ディスクの FAT と同じファイル、区画2にシステムのボリューム。
# KTEST=1 では区画3に試験が初期化してよい作業用の区画(SD_SCRATCH_MB、0 で無し)を足す。
# Raspberry Pi Imager の「カスタムイメージを使う」などでカードに書く。
# 同じ像が Raspberry Pi 5 と Compute Module 5(eMMC 付きと Lite、CM5IO)の両方で起動する。
# config.txt の [cm5] と [board-type=0x1a] の条件でファームウェアがデバイスツリーを
# 選び、CM5 ではコンソールを 40 ピンヘッダへ移す(cmdline-cm5.txt)。CM5 の eMMC へは
# rpiboot で大容量記憶として見せてから同じ像を書く。
# デバイスツリーはファームウェアの配布物から一度だけ取ってくる(再配布しないので
# リポジトリには置かない)。RPI_DTB=...、CM5_DTB=...、CM5L_DTB=... で手元のものを指してもよい
RPI_DTB_BASE ?= https://github.com/raspberrypi/firmware/raw/master/boot
RPI_DTB     ?= obj/boot/bcm2712-rpi-5-b.dtb
CM5_DTB     ?= obj/boot/bcm2712-rpi-cm5-cm5io.dtb
CM5L_DTB    ?= obj/boot/bcm2712-rpi-cm5l-cm5io.dtb
SD_IMG      := $(OBJDIR)/sd.img
SD_STAGE    := $(OBJDIR)/sdboot
ifeq ($(KTEST),1)
SD_SCRATCH_MB ?= 16
else
SD_SCRATCH_MB ?= 0
endif
# 区画1(FAT32)。Blink 入りのブラウザと V8 入りの表計算のレコードで 256MB を超える
SD_BOOT_MB  ?= 512

obj/boot/bcm2712-%.dtb:
	@mkdir -p $(dir $@)
	@echo 'GET $@'
	@curl -fsSL -o $@.part $(RPI_DTB_BASE)/$(notdir $@) && mv $@.part $@

ifeq ($(TARGET),_RPI4_)
# Raspberry Pi 4、400、Compute Module 4 の像(docs/tessronos/03-platform.md 3.6.3):
# make TARGET=_RPI4_ [DESKTOP=1] [MOZC=1] [KTEST=1] BUILD_ID=<id> sdimg で $(OBJDIR)/sd.img。
# Pi 4 系の EEPROM のブートローダは区画1の start4.elf と fixup4.dat を読み、それが
# config.txt(etc/boot/config-rpi4.txt)の [pi4]・[pi400]・[cm4] の節でデバイスツリーを
# 選んでカーネルを読み込む。ファームウェアのファイルと overlays/disable-bt.dtbo も
# 配布物から一度だけ取ってくる(Broadcom のライセンスのもとでバイナリとしてだけ
# 再配布できるものなので、リポジトリには置かない)。RPI4_FW=<ディレクトリ> で手元の
# ファームウェアの boot ディレクトリを指してもよい
RPI4_FW     ?= obj/boot
RPI4_FW_FILES := $(RPI4_FW)/start4.elf $(RPI4_FW)/fixup4.dat $(RPI4_FW)/bcm2711-rpi-4-b.dtb \
		 $(RPI4_FW)/bcm2711-rpi-400.dtb $(RPI4_FW)/bcm2711-rpi-cm4.dtb $(RPI4_FW)/disable-bt.dtbo

obj/boot/bcm2711-%.dtb:
	@mkdir -p $(dir $@)
	@echo 'GET $@'
	@curl -fsSL -o $@.part $(RPI_DTB_BASE)/$(notdir $@) && mv $@.part $@

obj/boot/start4.elf obj/boot/fixup4.dat:
	@mkdir -p $(dir $@)
	@echo 'GET $@'
	@curl -fsSL -o $@.part $(RPI_DTB_BASE)/$(notdir $@) && mv $@.part $@

obj/boot/disable-bt.dtbo:
	@mkdir -p $(dir $@)
	@echo 'GET $@'
	@curl -fsSL -o $@.part $(RPI_DTB_BASE)/overlays/$(notdir $@) && mv $@.part $@

# LCD=<名前>(rpi4.mk): 起動の設定に液晶の画面の設定を足したもの。LCD が変われば作り直す
ifneq ($(LCD),)
RPI4_LCD_STAMP := $(OBJDIR)/lcd.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(RPI4_LCD_STAMP) ] && [ "$$(cat $(RPI4_LCD_STAMP))" = "$(LCD)" ] || echo "$(LCD)" > $(RPI4_LCD_STAMP))
$(RPI4_CONFIG): $(TOP)/etc/boot/config-rpi4.txt $(RPI4_LCD_TXT) $(RPI4_LCD_STAMP)
	@mkdir -p $(dir $@)
	@cat $(TOP)/etc/boot/config-rpi4.txt $(RPI4_LCD_TXT) > $@
endif

$(SD_IMG): $(EXE_FILE).img $(RPI4_FW_FILES) $(RPI4_CONFIG) \
	   $(TOP)/tools/mksd.py $(TOP)/tools/mkdisk.py \
	   $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) \
	   $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF)
	@rm -rf $(SD_STAGE) && mkdir -p $(SD_STAGE)
	@cp $(USER_ELF) $(SD_STAGE)/init.elf
	@python3 $(TOP)/tools/mksd.py --sys $(SYS_VOL) --boot-mb $(SD_BOOT_MB) --scratch-mb $(SD_SCRATCH_MB) $@ \
	  tessronos.img=$(EXE_FILE).img config.txt=$(RPI4_CONFIG) \
	  $(RPI4_FW)/start4.elf $(RPI4_FW)/fixup4.dat \
	  $(RPI4_FW)/bcm2711-rpi-4-b.dtb $(RPI4_FW)/bcm2711-rpi-400.dtb $(RPI4_FW)/bcm2711-rpi-cm4.dtb \
	  overlays/disable-bt.dtbo=$(RPI4_FW)/disable-bt.dtbo \
	  $(USER_ELF) $(SD_STAGE)/init.elf $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) \
	  $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $$(ls $(UOBJ)/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF) \
	  > $(SD_STAGE)/mksd.log || { cat $(SD_STAGE)/mksd.log; rm -f $@; exit 1; }
	@tail -1 $(SD_STAGE)/mksd.log
	@rm -rf $(SD_STAGE)

sdimg: $(SD_IMG)

# 書き込んだカードの区画1のカーネルと起動の設定だけを差し替える(make TARGET=_RPI4_ sd SD=/mnt/e)
sd: $(EXE_FILE).img $(RPI4_FW_FILES) $(RPI4_CONFIG)
	@test -n "$(SD)" || { echo 'usage: make TARGET=_RPI4_ sd SD=<mount point of the FAT partition>'; exit 1; }
	cp $(EXE_FILE).img "$(SD)/tessronos.img"
	cp $(RPI4_CONFIG) "$(SD)/config.txt"
	cp $(RPI4_FW)/start4.elf $(RPI4_FW)/fixup4.dat $(RPI4_FW)/bcm2711-rpi-4-b.dtb \
	   $(RPI4_FW)/bcm2711-rpi-400.dtb $(RPI4_FW)/bcm2711-rpi-cm4.dtb "$(SD)/"
	mkdir -p "$(SD)/overlays" && cp $(RPI4_FW)/disable-bt.dtbo "$(SD)/overlays/"

# QEMU の raspi4b(設計書 03-platform.md 3.6.4)。カードの像は QEMU が求める2のべき乗の
# 大きさに後ろを疎に伸ばした写しにする(控えの GPT は、実機に焼いたときと同じく途中に残る)。
# make TARGET=_RPI4_ [KTEST=1] BUILD_ID=<id> qemu-rpi4 で起動し(Ctrl-A X で終了)、
# make TARGET=_RPI4_ KTEST=1 BUILD_ID=<id> test-rpi4 で試験を走らせる
RPI4_QEMU_SD := $(OBJDIR)/sd_qemu.img
RPI4_QEMU_SD_MB ?= 512

$(RPI4_QEMU_SD): $(SD_IMG)
	@cp --sparse=always $< $@
	@truncate -s $(RPI4_QEMU_SD_MB)M $@

# QEMU の raspi4b はカードを EMMC2 ではなく古い EMMC に繋ぐので、起動引数でそちらを使わせる
RPI4_QEMU_ARGS = $(RPI4_QEMU_FLAGS) -kernel $(EXE_FILE).img -dtb $(RPI4_QEMU_DTB) \
	-drive file=$(RPI4_QEMU_SD),if=sd,format=raw -append "tessronos.sd=emmc"

qemu-rpi4: $(EXE_FILE).img $(RPI4_QEMU_SD) $(RPI4_QEMU_DTB)
	$(QEMU) $(RPI4_QEMU_ARGS) -serial mon:stdio

test-rpi4: $(EXE_FILE).img $(RPI4_QEMU_SD) $(RPI4_QEMU_DTB)
	QEMU="$(QEMU)" $(TOP)/tools/qemu_run.sh -t $(TEST_TMO) -e "$(if $(filter 1,$(KTEST)),KTEST SUMMARY,TessronOS: > init)" \
	  -f "FAIL" -- $(RPI4_QEMU_ARGS)

.PHONY: qemu-rpi4 test-rpi4
else ifeq ($(ARCH),x86_64)
# KTEST=1 では、QEMU virt の試験用ディスクと同じく、試験が起こすプログラムと見本も区画1に載せる
X64_KT_BOOT = $(if $(filter 1,$(KTEST)),$(EXTRA_BOOT_FILES))
# x64 の PC のディスクのイメージ(docs/tessronos/03-platform.md 3.10): make TARGET=_X64_ [DESKTOP=1] BUILD_ID=<id> sdimg で
# $(OBJDIR)/sd.img。区画1は EFI システム区画(FAT32)で、UEFI のローダ(EFI/BOOT/BOOTX64.EFI)、カーネル(tessronos.elf)、
# 起動の設定(cmdline.txt)と、試験用ディスクの FAT と同じファイル。区画2はシステムのボリューム(nvmea1)。
# USB メモリか NVMe の SSD にそのまま書く。QEMU では NVMe として挿す(x64.mk の qemu-x64-disk、test)
# 記録の区画(SD_LOG_MB、FAT32、ラベル TSLOG)を区画1の直後に置き、表の最後の項目にする。ローダとカーネルが
# EFI/TESSRON の記録をその TESSRON にも書き、Windows のエクスプローラで取り出せる(取り外せる装置では ESP が見えないため)
SD_LOG_MB ?= 64
$(SD_IMG): $(EXE_FILE).elf $(OBJDIR)/BOOTX64.EFI $(TOP)/tools/mksd.py $(TOP)/tools/mkdisk.py \
	   $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) \
	   $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF) \
	   $(X64_KT_BOOT)
	@rm -rf $(SD_STAGE) && mkdir -p $(SD_STAGE)
	@cp $(USER_ELF) $(SD_STAGE)/init.elf
	@printf '%s\n' "$(X64_CMDLINE)" > $(SD_STAGE)/cmdline.txt
	@python3 $(TOP)/tools/mksd.py --esp --sys $(SYS_VOL) --boot-mb $(SD_BOOT_MB) --scratch-mb $(SD_SCRATCH_MB) --log-mb $(SD_LOG_MB) $@ \
	  EFI/BOOT/BOOTX64.EFI=$(OBJDIR)/BOOTX64.EFI tessronos.elf=$(EXE_FILE).elf $(SD_STAGE)/cmdline.txt \
	  $(USER_ELF) $(SD_STAGE)/init.elf $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) \
	  $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $$(ls $(UOBJ)/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF) \
	  $(X64_KT_BOOT) > $(SD_STAGE)/mksd.log || { cat $(SD_STAGE)/mksd.log; rm -f $@; exit 1; }
	@tail -1 $(SD_STAGE)/mksd.log
	@rm -rf $(SD_STAGE)

sdimg: $(SD_IMG)
else
$(SD_IMG): $(EXE_FILE).img $(RPI_DTB) $(CM5_DTB) $(CM5L_DTB) $(TOP)/etc/boot/config.txt $(TOP)/etc/boot/cmdline-cm5.txt \
	   $(TOP)/tools/mksd.py $(TOP)/tools/mkdisk.py \
	   $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) \
	   $(MS_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF)
	@rm -rf $(SD_STAGE) && mkdir -p $(SD_STAGE)
	@cp $(EXE_FILE).img $(SD_STAGE)/tessronos.img
	@cp $(TOP)/etc/boot/config.txt $(SD_STAGE)/config.txt
	@if [ "$(RP1_CONSOLE)" = 1 ] || [ "$(RP1_UART)" = 1 ]; then echo 'enable_rp1_uart=1' >> $(SD_STAGE)/config.txt; fi
	@cp $(TOP)/etc/boot/cmdline-cm5.txt $(SD_STAGE)/cmdline-cm5.txt
	@cp $(RPI_DTB) $(SD_STAGE)/bcm2712-rpi-5-b.dtb
	@cp $(CM5_DTB) $(SD_STAGE)/bcm2712-rpi-cm5-cm5io.dtb
	@cp $(CM5L_DTB) $(SD_STAGE)/bcm2712-rpi-cm5l-cm5io.dtb
	@cp $(USER_ELF) $(SD_STAGE)/init.elf
	@python3 $(TOP)/tools/mksd.py --sys $(SYS_VOL) --boot-mb $(SD_BOOT_MB) --scratch-mb $(SD_SCRATCH_MB) $@ \
	  $(SD_STAGE)/tessronos.img $(SD_STAGE)/config.txt $(SD_STAGE)/cmdline-cm5.txt \
	  $(SD_STAGE)/bcm2712-rpi-5-b.dtb $(SD_STAGE)/bcm2712-rpi-cm5-cm5io.dtb $(SD_STAGE)/bcm2712-rpi-cm5l-cm5io.dtb \
	  $(USER_ELF) $(SD_STAGE)/init.elf $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) \
	  $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $$(ls $(UOBJ)/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF) \
	  > $(SD_STAGE)/mksd.log || { cat $(SD_STAGE)/mksd.log; rm -f $@; exit 1; }
	@tail -1 $(SD_STAGE)/mksd.log
	@rm -rf $(SD_STAGE)

sdimg: $(SD_IMG)

# 書き込んだカードの区画1のカーネルと起動の設定だけを差し替える(make TARGET=_RPI5_ sd SD=/mnt/e)
sd: $(EXE_FILE).img $(RPI_DTB) $(CM5_DTB) $(CM5L_DTB)
	@test -n "$(SD)" || { echo 'usage: make TARGET=_RPI5_ sd SD=<mount point of the FAT partition>'; exit 1; }
	cp $(EXE_FILE).img "$(SD)/tessronos.img"
	cp $(TOP)/etc/boot/config.txt "$(SD)/config.txt"
	cp $(TOP)/etc/boot/cmdline-cm5.txt "$(SD)/cmdline-cm5.txt"
	cp $(RPI_DTB) "$(SD)/bcm2712-rpi-5-b.dtb"
	cp $(CM5_DTB) "$(SD)/bcm2712-rpi-cm5-cm5io.dtb"
	cp $(CM5L_DTB) "$(SD)/bcm2712-rpi-cm5l-cm5io.dtb"
endif

# システムのバージョンアップ用のパッケージ(.tpk、設計書 11.8): make TARGET=<機種> [イメージと同じ指定] BUILD_ID=<id> sysup で
# $(OBJDIR)/tessronos-<機種>-$(TS_VERSION).tpk。システムのボリュームに載る実身(mktsfs と同じ --over・--from・--exclude。
# 組み込みのアプリのパッケージ PKGS は除く)と、etc/xtad の実身のうちプログラムが原紙として名指すもの、
# その機種の起動のファイル(区画1・ESP に置くもの)をまとめる。〈バージョン〉の一覧に落とすと入る
SYSUP_STAGE := $(OBJDIR)/sysupboot
ifeq ($(TARGET),_RPI5_)
SYSUP_MACHINE := rpi5
SYSUP_BOOT_DEPS := $(EXE_FILE).img $(RPI_DTB) $(CM5_DTB) $(CM5L_DTB) $(TOP)/etc/boot/config.txt $(TOP)/etc/boot/cmdline-cm5.txt
SYSUP_BOOT = --boot tessronos.img=$(EXE_FILE).img --boot config.txt=$(SYSUP_STAGE)/config.txt \
	--boot cmdline-cm5.txt=$(TOP)/etc/boot/cmdline-cm5.txt \
	--boot bcm2712-rpi-5-b.dtb=$(RPI_DTB) --boot bcm2712-rpi-cm5-cm5io.dtb=$(CM5_DTB) \
	--boot bcm2712-rpi-cm5l-cm5io.dtb=$(CM5L_DTB) --boot init.elf=$(USER_ELF)
else ifeq ($(TARGET),_RPI4_)
SYSUP_MACHINE := rpi4
SYSUP_BOOT_DEPS := $(EXE_FILE).img $(RPI4_FW_FILES) $(RPI4_CONFIG)
SYSUP_BOOT = --boot tessronos.img=$(EXE_FILE).img --boot config.txt=$(RPI4_CONFIG) \
	--boot start4.elf=$(RPI4_FW)/start4.elf --boot fixup4.dat=$(RPI4_FW)/fixup4.dat \
	--boot bcm2711-rpi-4-b.dtb=$(RPI4_FW)/bcm2711-rpi-4-b.dtb --boot bcm2711-rpi-400.dtb=$(RPI4_FW)/bcm2711-rpi-400.dtb \
	--boot bcm2711-rpi-cm4.dtb=$(RPI4_FW)/bcm2711-rpi-cm4.dtb \
	--boot overlays/disable-bt.dtbo=$(RPI4_FW)/disable-bt.dtbo --boot init.elf=$(USER_ELF)
else ifeq ($(TARGET),_X64_)
SYSUP_MACHINE := x64
SYSUP_BOOT_DEPS := $(EXE_FILE).elf $(OBJDIR)/BOOTX64.EFI
SYSUP_BOOT = --boot EFI/BOOT/BOOTX64.EFI=$(OBJDIR)/BOOTX64.EFI --boot tessronos.elf=$(EXE_FILE).elf \
	--boot init.elf=$(USER_ELF) --boot-keep cmdline.txt=$(SYSUP_STAGE)/cmdline.txt
else
SYSUP_MACHINE := qemu_virt
SYSUP_BOOT_DEPS :=
SYSUP_BOOT = --boot init.elf=$(USER_ELF)
endif
SYSUP_TPK := $(OBJDIR)/tessronos-$(SYSUP_MACHINE)-$(TS_VERSION).tpk

$(SYSUP_TPK): $(TOP)/tools/mktpk.py $(TSFS_TOOLS) $(SYSUP_BOOT_DEPS) $(USER_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) \
	      $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF) \
	      $(CMD_V8_REC) $(CMD_DONE) $(WIFI_DONE) $(WPAP_DONE) $(VER_STAMP)
	@rm -rf $(SYSUP_STAGE) && mkdir -p $(SYSUP_STAGE)
	@cp $(TOP)/etc/boot/config.txt $(SYSUP_STAGE)/config.txt
	@if [ "$(RP1_CONSOLE)" = 1 ] || [ "$(RP1_UART)" = 1 ]; then echo 'enable_rp1_uart=1' >> $(SYSUP_STAGE)/config.txt; fi
	@printf '%s\n' "$(X64_CMDLINE)" > $(SYSUP_STAGE)/cmdline.txt
	@echo 'TPK $@'
	@python3 $(TOP)/tools/mktpk.py --system --machine $(SYSUP_MACHINE) --arch $(ARCH) --version $(TS_VERSION) \
	  $(CMD_OVER) $(WIFI_OVER) --from $(TOP)/etc/def --from $(UOBJ)/rec --from $(UOBJ)/rec/sys --user $(TOP)/etc/xtad \
	  $(addprefix --exclude ,$(SYS_VOL_EXCLUDE)) --keep 01a0d8c4-5481-7b92-9db3-0abc1d2e5481 \
	  $(SYSUP_BOOT) -o $@
	@rm -rf $(SYSUP_STAGE)

sysup: $(SYSUP_TPK)

.PHONY: sysup

clean:
	rm -rf obj $(EXE_FILE).elf $(EXE_FILE).img $(EXE_FILE).map $(EXE_FILE).lst

-include $(DEPS)
