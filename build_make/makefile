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
#   make clean
#

#   make TARGET=_QEMU_VIRT_ BUILD_ID=b2 test
#       別の目的物一式で建てる。同じ木で 2 つの作業を並行させるときに使う。
#       目的物も試験用ディスクも obj/<target><id>/ と tessronos<id>.* に分かれる。

TARGET   ?= _QEMU_VIRT_
BUILD_ID ?=
EXE_FILE := tessronos$(BUILD_ID)


include toolchain.mk
include common.mk
include sources.mk

ifeq ($(TARGET),_QEMU_VIRT_)
  include qemu_virt.mk
else ifeq ($(TARGET),_RPI5_)
  include rpi5.mk
else
  $(error unknown TARGET "$(TARGET)" (use _QEMU_VIRT_ or _RPI5_))
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
$(FT_LIB_OBJS): CFLAGS += -DFT2_BUILD_LIBRARY

# TARGET を切り替えたら再リンクする(成果物名が共通のため)
TARGET_STAMP := obj/target.stamp
$(shell mkdir -p obj; [ "$$(cat $(TARGET_STAMP) 2>/dev/null)" = "$(TARGET)$(KTEST)" ] || echo "$(TARGET)$(KTEST)" > $(TARGET_STAMP))

.PHONY: all image clean qemu gdb test sd sdimg docs-check pkg

all: $(EXE_FILE).elf $(IMAGE_TARGETS) $(EXE_FILE).lst

# 構造体オフセット(アセンブラ用)。offset.c を -S で処理して "@@name value" 行を抜き出す
OFFSET_H := $(OBJDIR)/offset.h
$(OFFSET_H): $(TOP)/kernel/sysdepend/cpu/core/armv8a/offset.c
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
$(NETBSD_OBJ): $(TOP)/tools/netbsd/build.sh $(wildcard $(TOP)/peripheral_kernel/network/netbsd/rumpcomp/*.c)
	@echo 'NETBSD $@'
	@OUT=$(patsubst %/,%,$(dir $(NETBSD_OBJ))) bash $(TOP)/tools/netbsd/build.sh all
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
ifeq ($(NET),user)
QEMU_WIN ?= /mnt/c/Program Files/qemu/qemu-system-aarch64.exe
ifneq ($(shell "$(QEMU)" -M none -netdev help 2>/dev/null | tr -d '\r' | grep -x user),user)
QEMU     := $(QEMU_WIN)
HOST_PY  ?= $(shell command -v python.exe || command -v py.exe)
endif
TEST_NETDEV := -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$(FTP_ACT_HOST)-:$(FTP_ACT_PORT)
TEST_SERVER := -s "$(or $(HOST_PY),python3) $(TOP)/tools/ftpd.py --exit-on-eof --port $(FTP_PORT) --pasv-address 10.0.2.2 --active-via 127.0.0.1:$(FTP_ACT_HOST) --http $(HTTP_PORT) $(FTP_DIR)"
$(OBJDIR)/tests/ktest/ktest_svcio.o: CFLAGS += -DKT_FTP_PORT=$(FTP_PORT)
$(OBJDIR)/tests/ktest/ktest_ms.o: CFLAGS += -DKT_HTTP_PORT=$(HTTP_PORT)
$(OBJDIR)/tests/ktest/ktest_xfc.o: CFLAGS += -DKT_FTP_PORT=$(FTP_PORT) -DKT_FTP_ACT_PORT=$(FTP_ACT_PORT)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_NET_USER=1
# ktest_so は利用者モードの網があれば、DHCP と名前の問い合わせと時刻合わせも確かめる
$(OBJDIR)/tests/ktest/ktest_so.o: CFLAGS += -DKT_NET_USER=1
# KT_SITE=<URL>: ktest_v8 only opens that page with the browser and takes the screen
# (a site looked at on its own, BUILD_ID of its own)
ifneq ($(KT_SITE),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_SITE='"$(KT_SITE)"'
endif
else
TEST_NETDEV := -netdev socket,id=n0,mcast=230.0.0.1:12345
TEST_SERVER :=
endif
# KT_V8=<name>,<name>: ktest_v8 runs only those of its tests (make ... KT_V8=test_blink_media)
ifneq ($(KT_V8),)
$(OBJDIR)/tests/ktest/ktest_v8.o: CFLAGS += -DKT_V8='"$(KT_V8)"'
endif
# NET と FTP_PORT(と KT_SITE・KT_V8)が変われば、それを見る試験(ktest_svcio・ktest_xfc・ktest_ms・ktest_v8)を作り直す
NET_STAMP := $(OBJDIR)/net.stamp
$(shell mkdir -p $(OBJDIR); [ "$$(cat $(NET_STAMP) 2>/dev/null)" = "$(NET):$(FTP_PORT):$(FTP_ACT_PORT):$(KT_SITE):$(KT_V8)" ] || echo "$(NET):$(FTP_PORT):$(FTP_ACT_PORT):$(KT_SITE):$(KT_V8)" > $(NET_STAMP))
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
		      -drive file=$(SD_CARD),if=none,format=raw,id=sdcard -device sdhci-pci,sd-spec-version=3 -device sd-card,drive=sdcard
TEST_DEPS  := $(TEST_DISK) $(USB_DISK) $(USB_DISK2) $(USB_DISK3) $(USB_DISK4) $(USB_DISK5) $(SD_CARD)
ifeq ($(TARGET),_QEMU_VIRT_)
$(OBJDIR)/tests/ktest/ktest_sd.o: CFLAGS += -DKT_SD_SECTORS=$(SD_CARD_SECTORS)
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
USER_ELF  := obj/hello.elf
USER_SRCS := $(USER_DIR)/crt0.S $(USER_DIR)/svc_stub.S $(USER_DIR)/hello.c
USER_LD   := $(TOP)/etc/linker/user/program.ld
# 利用者のプログラムが読むヘッダ。構造体の形(依頼や事象の大きさ)が変われば、
# 利用者のプログラムも作り直す
APP_HDRS  := $(wildcard $(TOP)/include/ts/*.h) $(wildcard $(TOP)/include/tk/*.h)

$(USER_ELF): $(APP_HDRS) $(USER_SRCS) $(USER_LD)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARCHFLAGS) -mgeneral-regs-only -mstrict-align -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -T $(USER_LD) -Wl,--build-id=none \
	  -o $@ $(USER_SRCS)

$(USB_DISK): $(TOP)/tools/mkusbdisk.py $(TOP)/tools/mkfat.py
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mkusbdisk.py $@ 40

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
# にする(設計書 18.20)。書体は資産なのでリポジトリに入れない(設計書 17.1)
SYS_FILES ?= $(wildcard $(TOP)/etc/font/*.ttf) $(wildcard $(TOP)/etc/font/*.otf) \
	     $(wildcard $(TOP)/etc/wall/*.PIC)
SYS_RECS  := obj/rec/sys.stamp

$(SYS_RECS): $(SYS_FILES) $(TOP)/tools/mkobj.py
	@rm -rf obj/rec/sys
	@python3 $(TOP)/tools/mkobj.py rec obj/rec/sys $(SYS_FILES)
	@touch $@

# 試験用の記録。etc/xtad/ に .XTD を置くと試験用ディスクに載り、
# ktest_tv が読んで画面に出す(設計書 17.5)
KTEST_XTAD ?= $(wildcard $(TOP)/etc/xtad/*.xtad) $(wildcard $(TOP)/etc/xtad/*.json) $(wildcard $(TOP)/etc/xtad/*.png) $(wildcard $(TOP)/etc/xtad/*.ico) \
	      $(wildcard $(TOP)/etc/xtad/*.bin)

# 定義実身。etc/def/ のメタデータとレコード0がそのままシステムの実身の置き場に載る(設計書 18.15)
KTEST_DEF ?= $(wildcard $(TOP)/etc/def/*.json) $(wildcard $(TOP)/etc/def/*.xtad) $(wildcard $(TOP)/etc/def/*.ico)

# 利用者プログラムの共通部分(lib/libts)と、それで作る小物
LIBTS_SRCS := $(TOP)/lib/libts/crt0.S $(TOP)/lib/libts/svc_stub.S $(TOP)/lib/libts/libc.c \
	      $(TOP)/lib/libts/ts_face.c $(TOP)/lib/libts/ts_random.c
CLOCK_ELF  := obj/clock.elf
# 時計のプログラム実身のレコード1。メタデータとレコード0は etc/def にある(設計書 18.20)
CLOCK_REC  := obj/rec/01a0d8c4-5212-7a19-8d63-e04b5c9f2a71_1.bin

$(CLOCK_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/application/clock/clock.c $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ARCHFLAGS) -mgeneral-regs-only -mstrict-align -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none \
	  -o $@ $(LIBTS_SRCS) $(TOP)/application/clock/clock.c

$(CLOCK_REC): $(CLOCK_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# コンソールとシリアル通信の小物。どちらも行の窓(application/lterm)で描く。
# プログラム実身のレコード1で、メタデータとレコード0は etc/def にある(設計書 16.5.16)
LTERM_SRCS  := $(TOP)/application/lterm/lterm.c
CONSOLE_ELF := obj/console.elf
CONSOLE_REC := obj/rec/01a0d8c4-5219-7c3e-8a41-2f6b9d0e5c11_1.bin
SERIAL_ELF  := obj/serial.elf
SERIAL_REC  := obj/rec/01a0d8c4-521a-7d52-9b17-4e8c2a6f0d23_1.bin
APP_CFLAGS  := $(ARCHFLAGS) -mgeneral-regs-only -mstrict-align -fno-builtin -fno-stack-protector \
	  -ffreestanding -nostdlib -nostartfiles -static -Os -g \
	  -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none

$(CONSOLE_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(LTERM_SRCS) $(TOP)/application/console/console.c \
		$(TOP)/application/lterm/lterm.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(LTERM_SRCS) $(TOP)/application/console/console.c

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
MS_SRCS     := $(MS_DIR)/ms_co.S $(MS_DIR)/ms_libc.c $(MS_DIR)/ms_xml.c $(MS_DIR)/ms_fig.c 	       $(MS_DIR)/ms_lex.c $(MS_DIR)/ms_parse.c $(MS_DIR)/ms_run.c $(MS_DIR)/ms_rtlib.c 	       $(MS_DIR)/ms_stage.c $(MS_DIR)/ms_main.c $(MS_DIR)/ms_jis.c $(MS_DIR)/ms_vobj.c $(MS_DIR)/ms_shared.c $(MS_DIR)/ms_ime.c $(MS_DIR)/ms_net.c $(TOP)/lib/libjson/json.c
# lib/libts の libc.c は使わない: 同じ関数を newlib が持ち、最適化すると自分を呼ぶ
MS_LIBTS    := $(TOP)/lib/libts/crt0.S $(TOP)/lib/libts/svc_stub.S $(TOP)/lib/libts/ts_random.c
MS_ELF      := obj/microscript.elf
MS_REC      := obj/rec/01a0d8c4-521b-7e63-8c28-5f9d3b7a1e34_1.bin
MS_CFLAGS   := $(ARCHFLAGS) -mstrict-align -fno-stack-protector -O2 -g -DCHK_TKERNEL_CONST 	       -nostdlib -nostartfiles -static 	       -I$(TOP)/include -I$(TOP)/config -T $(USER_LD) -Wl,--build-id=none

$(MS_ELF): $(APP_HDRS) $(MS_LIBTS) $(MS_SRCS) $(MS_DIR)/ms.h $(MS_DIR)/ms_run.h $(USER_LD) $(TOP)/include/ts/svc.h $(TLS_LIB)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(MS_CFLAGS) -o $@ $(MS_LIBTS) $(MS_SRCS) $(TLS_LIB) -lm -lc -lgcc

$(MS_REC): $(MS_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# 書庫(BPK)・LH5・二進 TAD を読む部分(lib/libbpk、include/ts/bpk.h)。系の呼出しをせず、
# 書庫解凍が結ぶ。LH5 の復号(bpk_lh5.c)だけはカーネルの試験も結ぶ(設計書 17.16)
LIBBPK_DIR  := $(TOP)/lib/libbpk
LIBBPK_SRCS := $(LIBBPK_DIR)/bpk_arc.c $(LIBBPK_DIR)/bpk_lh5.c $(LIBBPK_DIR)/bpk_tad.c $(LIBBPK_DIR)/bpk_png.c \
	       $(LIBBPK_DIR)/bpk_tron.c $(LIBBPK_DIR)/bpk_jis.c $(LIBBPK_DIR)/bpk_util.c $(LIBBPK_DIR)/bpk_fax.c

# 書庫解凍: BTRON の書庫の実身から中の実身を取り出すアプリ。newlib を使う。
# プログラム実身のレコード1(設計書 17.16)
UNP_DIR     := $(TOP)/application/unpack
UNP_SRCS    := $(UNP_DIR)/unp_libc.c $(UNP_DIR)/unp_obj.c $(UNP_DIR)/unp_main.c $(LIBBPK_SRCS) \
	       $(TOP)/lib/libjson/json.c
UNP_ELF     := obj/unpack.elf
UNP_REC     := obj/rec/01a0d8c4-52a7-7c31-9b5e-3f1d8a6c2e90_1.bin
UNP_CFLAGS  := $(MS_CFLAGS) -Wall -Wno-unused-parameter

$(UNP_ELF): $(APP_HDRS) $(MS_LIBTS) $(UNP_SRCS) $(UNP_DIR)/unp.h $(TOP)/include/ts/bpk.h $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(UNP_CFLAGS) -o $@ $(MS_LIBTS) $(UNP_SRCS) -lc -lgcc

$(UNP_REC): $(UNP_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

# バックアップ: BTRON のバックアップ書庫に実身を保存し、そこから復帰する小物。書庫の形式は
# lib/libbtbk、TAD の読み書きと TRON コードは lib/libbpk、巻のファイル名は lib/libxfu。
# プログラム実身のレコード1(設計書 17.18)
LIBBTBK_SRCS := $(TOP)/lib/libbtbk/btbk_lzss.c $(TOP)/lib/libbtbk/btbk_arc.c $(TOP)/lib/libbtbk/btbk_tad.c
BKP_DIR     := $(TOP)/application/backup
BKP_SRCS    := $(BKP_DIR)/bk_libc.c $(BKP_DIR)/bk_util.c $(BKP_DIR)/bk_media.c $(BKP_DIR)/bk_vol.c \
	       $(BKP_DIR)/bk_save.c $(BKP_DIR)/bk_rest.c $(BKP_DIR)/bk_main.c $(LIBBTBK_SRCS) $(LIBBPK_SRCS) \
	       $(TOP)/lib/libjson/json.c $(TOP)/lib/libxfu/xfu_name.c $(TOP)/lib/libxfu/xfu_util.c \
	       $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c
BKP_ELF     := obj/backup.elf
BKP_REC     := obj/rec/01a0d8c4-52b0-7d15-9a3c-4e6f8b2d1c07_1.bin
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
# 変換の部品(lib/libxfu・lib/libtxc・lib/libftp)を結ぶ(設計書 17.17)
XFC_DIR     := $(TOP)/application/xfconv
XFC_SRCS    := $(XFC_DIR)/xfc_libc.c $(XFC_DIR)/xfc_net.c $(XFC_DIR)/xfc_main.c $(XFC_DIR)/xfc_draw.c 	       $(XFC_DIR)/xfc_dlg.c $(TOP)/lib/libui/ui.c \
	       $(TOP)/lib/libxfu/xfu_util.c $(TOP)/lib/libxfu/xfu_name.c $(TOP)/lib/libxfu/xfu_obj.c \
	       $(TOP)/lib/libxfu/xfu_imp.c $(TOP)/lib/libxfu/xfu_exp.c $(TOP)/lib/libxfu/xfu_fs.c \
	       $(TOP)/lib/libxfu/xfu_ftp.c $(TOP)/lib/libxfu/xfu_prc.c \
	       $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c $(TOP)/lib/libftp/ftp.c \
	       $(LIBBPK_DIR)/bpk_jis.c $(TOP)/lib/libjson/json.c
XFC_ELF     := obj/xfconv.elf
XFC_REC     := obj/rec/01a0d8c4-52b1-7c21-9a31-4f5e6d7c8b91_1.bin
# 系の答える誤りの番号は下位の番号を持たない(sys/profile.h の TK_SUPPORT_SERCD)。
# プログラムの側でも E_OBJ などを同じ値にするため、どのファイルでも tk/errno.h が
# そう読むように言っておく(TRUE は tk/typedef.h と同じ値)
XFC_CFLAGS  := $(MS_CFLAGS) -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation 	       -DTK_SUPPORT_SERCD=0 -DTRUE=1

$(XFC_ELF): $(APP_HDRS) $(MS_LIBTS) $(XFC_SRCS) $(XFC_DIR)/xfc.h $(TOP)/lib/libxfu/xfu_in.h $(TOP)/include/ts/xfu.h 	    $(TOP)/include/ts/ui.h $(TOP)/include/ts/dtreq.h \
	    $(TOP)/include/ts/ftp.h $(TOP)/include/ts/txc.h $(USER_LD) $(TOP)/include/ts/svc.h
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
NETENV_ELF   := obj/netenv.elf
NETENV_REC   := obj/rec/01a0d8c4-52b5-7a41-8c61-2d7e9f0a1b31_1.bin

$(NETENV_ELF): $(ENV_HDRS) $(TOP)/application/netenv/netenv.c
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/netenv/netenv.c -lc -lgcc

$(NETENV_REC): $(NETENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

USERENV_ELF  := obj/userenv.elf
USERENV_REC  := obj/rec/01a0d8c4-52b7-7c63-8e83-4f9a012c3d53_1.bin

$(USERENV_ELF): $(ENV_HDRS) $(TOP)/application/userenv/userenv.c $(TOP)/include/ts/look.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/userenv/userenv.c -lc -lgcc

$(USERENV_REC): $(USERENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

SYSENV_ELF   := obj/sysenv.elf
SYSENV_REC   := obj/rec/01a0d8c4-52b6-7b52-9d72-3e8fa01b2c42_1.bin

$(SYSENV_ELF): $(ENV_HDRS) $(TOP)/application/sysenv/sysenv.c $(TOP)/include/ts/look.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(ENV_CFLAGS) -o $@ $(MS_LIBTS) $(UI_SRCS) $(TOP)/application/sysenv/sysenv.c -lc -lgcc

$(SYSENV_REC): $(SYSENV_ELF)
	@mkdir -p $(dir $@)
	@cp $< $@

ENV_RECS     := $(NETENV_REC) $(USERENV_REC) $(SYSENV_REC)

# C++ の実行時とスレッド、それで作るプログラム(設計書 17.19)
include cxxrt.mk
# ブラウザ(Chromium の移植)は別のリポジトリ tessron-chromium にあり、make BROWSER=1 の
# ときだけそこの browser.mk を読んで、イメージに取り込む。置き場は CHROMIUM で変えられる
CHROMIUM ?= $(abspath $(TOP)/../tessron-chromium)
ifeq ($(BROWSER),1)
ifeq ($(wildcard $(CHROMIUM)/build_make/browser.mk),)
$(error BROWSER=1: no $(CHROMIUM)/build_make/browser.mk (CHROMIUM=<tessron-chromium>))
endif
include $(CHROMIUM)/build_make/browser.mk
endif

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
DICT_REC   := obj/rec/01a0d8c4-5216-7f4a-8c27-1d5b3e9a6c02_1.bin
endif

$(DICT_REC): $(MOZC_DATA)
	@mkdir -p $(dir $@)
	@cp $< $@

# システムのボリューム: 定義実身と試験用の記録、書体・壁紙・時計・辞書のレコードを
# tools/mktsfs で TSFS の版数4のボリュームにしたもの。試験用ディスクの区画3になり、
# 起動時に実身の置き場として付く(設計書 11.15、11.17 の T7)。同じファイルは
# 区画2の FAT にも載せたままにしてある(FAT の試験と、区画3が無いときの置き場)
SYS_VOL    := $(OBJDIR)/sysvol.img
SYS_VOL_SIZE ?= 64M
TSFS_TOOLS  := $(TOP)/tools/mktsfs $(wildcard $(TOP)/tools/tsfs_py/*.py)

# 版(R1.000 の形: R、桁数の決まらない整数、点、3桁)。プログラムの版はプログラム実身の
# メタデータ tessronos.program.version に書く(etc/def)。TS_VERSION はシステムの版で、
# システムの実身の OB_SYS_INFO に VERSION として出る
TS_VERSION   ?= R1.000
VER_STAMP    := obj/version.stamp
$(shell mkdir -p obj; [ -f $(VER_STAMP) ] && [ "$$(cat $(VER_STAMP))" = "$(TS_VERSION)" ] || echo "$(TS_VERSION)" > $(VER_STAMP))
$(OBJDIR)/peripheral_kernel/obj/obsys.o: $(VER_STAMP)
$(OBJDIR)/peripheral_kernel/obj/obsys.o: CFLAGS += -DTS_VERSION='"$(TS_VERSION)"'

# アプリのパッケージ(.tpk): make pkg PKG=<プログラムの id か UUID> で obj/pkg/<id>.tpk。
# 版はプログラム実身のメタデータに書いてあるもの
PKG ?=
pkg: $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS)
	@test -n "$(PKG)" || { echo 'usage: make pkg PKG=<program id>'; exit 1; }
	@mkdir -p obj/pkg
	@python3 $(TOP)/tools/mktpk.py --from $(TOP)/etc/def --from obj/rec --program $(PKG) -o obj/pkg/$(PKG).tpk

$(SYS_VOL): $(TSFS_TOOLS) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF)
	@mkdir -p $(dir $@)
	@rm -f $@
	@python3 $(TOP)/tools/mktsfs -q -L "TessronOS system" --size $(SYS_VOL_SIZE) \
	  --from $(TOP)/etc/def --from $(TOP)/etc/xtad --from obj/rec --from obj/rec/sys \
	  --sysbox 01a0d8c4-5211-7c52-9e0a-3b61d27f4c85 $@

# 試験が起こすプロセス(tests/uprog)。/boot に SVCPROG.ELF として載り、ktest_svcio が
# ファイル・マウント・ソケットの SVC を EL0 から通す
SVCPROG_ELF := obj/svcprog.elf

$(SVCPROG_ELF): $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/tests/uprog/svcprog.c $(USER_LD) $(TOP)/include/ts/svc.h \
		$(TOP)/include/ts/uapp.h $(TOP)/include/ts/soapp.h $(TOP)/include/ts/fs.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/tests/uprog/svcprog.c

$(SD_CARD): $(TOP)/tools/mksd.py $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF)
	@mkdir -p $(dir $@)
	@python3 $(TOP)/tools/mksd.py --sys $(SYS_VOL) --boot-mb 64 --scratch-mb 16 $@ $(USER_ELF)
	@truncate -s $$(( $(SD_CARD_SECTORS) * 512 )) $@

$(TEST_DISK): $(TOP)/tools/mkdisk.py $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(EXTRA_BOOT_FILES) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF)
	@mkdir -p $(dir $@)
	@cp $(USER_ELF) obj/init.elf
	@python3 $(TOP)/tools/mkdisk.py --sys $(SYS_VOL) $@ $(TEST_DISK_MB) $(USER_ELF) obj/init.elf $(SVCPROG_ELF) $(EXTRA_BOOT_FILES) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) \
	  $$(ls obj/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF)

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

sd: $(EXE_FILE).img
	@test -n "$(SD)" || { echo 'usage: make TARGET=_RPI5_ sd SD=<mount point of the FAT partition>'; exit 1; }
	cp $(EXE_FILE).img "$(SD)/tessronos.img"
	cp $(TOP)/etc/boot/config.txt "$(SD)/config.txt"

# SD カードの像(設計書 13.6): make TARGET=_RPI5_ [KTEST=1] BUILD_ID=<id> sdimg で
# $(OBJDIR)/sd.img。区画1(FAT32)に起動の三つ(config.txt、デバイスツリー、
# tessronos.img)と試験用ディスクの FAT と同じファイル、区画2にシステムのボリューム。
# KTEST=1 では区画3に試験が初期化してよい作業用の区画(SD_SCRATCH_MB、0 で無し)を足す。
# Raspberry Pi Imager の「カスタムイメージを使う」などでカードに書く。
# デバイスツリーはファームウェアの配布物から一度だけ取ってくる(再配布しないので
# リポジトリには置かない)。RPI_DTB=... で手元のものを指してもよい
RPI_DTB_URL ?= https://github.com/raspberrypi/firmware/raw/master/boot/bcm2712-rpi-5-b.dtb
RPI_DTB     ?= obj/boot/bcm2712-rpi-5-b.dtb
SD_IMG      := $(OBJDIR)/sd.img
SD_STAGE    := $(OBJDIR)/sdboot
ifeq ($(KTEST),1)
SD_SCRATCH_MB ?= 16
else
SD_SCRATCH_MB ?= 0
endif

obj/boot/bcm2712-rpi-5-b.dtb:
	@mkdir -p $(dir $@)
	@echo 'GET $@'
	@curl -fsSL -o $@.part $(RPI_DTB_URL) && mv $@.part $@

$(SD_IMG): $(EXE_FILE).img $(RPI_DTB) $(TOP)/etc/boot/config.txt $(TOP)/tools/mksd.py $(TOP)/tools/mkdisk.py \
	   $(TOP)/tools/mkfat.py $(SYS_VOL) $(USER_ELF) $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) \
	   $(MS_REC) $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $(SYS_RECS) $(KTEST_XTAD) $(KTEST_DEF)
	@rm -rf $(SD_STAGE) && mkdir -p $(SD_STAGE)
	@cp $(EXE_FILE).img $(SD_STAGE)/tessronos.img
	@cp $(TOP)/etc/boot/config.txt $(SD_STAGE)/config.txt
	@if [ "$(RP1_CONSOLE)" = 1 ] || [ "$(RP1_UART)" = 1 ]; then echo 'enable_rp1_uart=1' >> $(SD_STAGE)/config.txt; fi
	@cp $(RPI_DTB) $(SD_STAGE)/bcm2712-rpi-5-b.dtb
	@cp $(USER_ELF) $(SD_STAGE)/init.elf
	@python3 $(TOP)/tools/mksd.py --sys $(SYS_VOL) --scratch-mb $(SD_SCRATCH_MB) $@ \
	  $(SD_STAGE)/tessronos.img $(SD_STAGE)/config.txt $(SD_STAGE)/bcm2712-rpi-5-b.dtb \
	  $(USER_ELF) $(SD_STAGE)/init.elf $(SVCPROG_ELF) $(CLOCK_REC) $(CONSOLE_REC) $(SERIAL_REC) $(MS_REC) \
	  $(UNP_REC) $(XFC_REC) $(BKP_REC) $(ENV_RECS) $(DICT_REC) $(EXTRA_RECS) $$(ls obj/rec/sys/*.bin 2>/dev/null) $(KTEST_XTAD) $(KTEST_DEF) \
	  | tail -1
	@rm -rf $(SD_STAGE)

sdimg: $(SD_IMG)

clean:
	rm -rf obj $(EXE_FILE).elf $(EXE_FILE).img $(EXE_FILE).map $(EXE_FILE).lst

-include $(DEPS)
