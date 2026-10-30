#
# rpi5.mk — Raspberry Pi 5(BCM2712)向けの設定
#

LNKFILE := $(TOP)/etc/linker/rpi5/tkernel_map.ld
LOAD_PA := 0x200000

CFLAGS_TARGET  :=
IMAGE_TARGETS  := $(EXE_FILE).img

# make DISP_SWAP_RB=1: 画面の赤と青が入れ替わって見えるとき、ファームウェアの
# 画素の並びを逆に読む(設計書 16.5.1)。別の BUILD_ID で建てるか make clean の後に
ifeq ($(DISP_SWAP_RB),1)
CFLAGS_TARGET += -DCNF_DISP_SWAP_RB=1
endif

# 画面の2面の切り替え(docs/tessronos/10-display.md 10.2.1)。表示の装置(HVS)の
# 表示リストの画素の場所を書き換えて切り替える。make DISP_FLIP=0 でファームウェアが
# 用意した1面のまま使う
ifeq ($(DISP_FLIP),0)
CFLAGS_TARGET += -DCNF_DISP_FLIP=0
endif

# make RP1_CONSOLE=1: コンソールを3ピンのデバッグUARTから40ピンのヘッダの
# 8番(TXD)・10番(RXD)に移す(一般のUSBシリアル変換器でつなげる、設計書 14.3.1)
ifeq ($(RP1_CONSOLE),1)
CFLAGS_TARGET += -DCNF_RP1_CONSOLE=1
endif

# make RP1_UART=1: ヘッダのUART0を2つ目のシリアルポート(serb)として使う。
# ファームウェアがこのポートにクロックを入れるのは config.txt に enable_rp1_uart=1 が
# あるときだけで、無いまま触るとSErrorになる。sdimg がその行を書き足す
# 実機は電池の無い RTC で起動の度に時計が 1985 年に戻るので、起動時に DHCP でアドレスを取り、
# SNTP で時計を合わせる(設計書 12.6)。make NET_AUTO=0 で止める
ifneq ($(NET_AUTO),0)
CFLAGS_TARGET += -DCNF_NET_DHCP=1 -DCNF_NET_SNTP=1
endif

# make V3D=1: GPU(V3D)のクロックを入れ、識別のレジスタを読んで表示するだけ。
# V3D=2: さらに MMU と割込みを設定し、GPU として登録して自己試験をし、起動の後で
# 実機の試験(v3d_test.c)をまとめて走らせ、最後に GPU でのウインドウの合成を
# CPU と突き合わせて確かめる(第13章 13.8、13.10)。
# V3D=3: 実機の試験をせずに GPU を使える状態にし、合成の確認だけをして、通れば
# ウインドウの合成に GPU を使う(第13章 13.10)
ifneq ($(V3D),)
CFLAGS_TARGET += -DCNF_V3D=$(V3D)
endif

# make OBJTEST=0: 起動の後のデバイスの実身の確認(objtest.c、objtest: の行)を行わない
ifeq ($(OBJTEST),0)
CFLAGS_TARGET += -DCNF_OBJTEST=0
endif

# make SNDTEST=1: 起動の25秒後に、音の出力先を実機で確かめる(sndtest.c、sndtest: の行)。
# I2S のクロックと進み、HDMI0・HDMI1・USB の音、再生中の切替えを順に鳴らして確かめる
# (第6章 6.5)
ifeq ($(SNDTEST),1)
CFLAGS_TARGET += -DCNF_SNDTEST=1
endif

# make HEVCTEST=1: 起動の20秒後に、HEVC の復号器で小さな動画の絵(Main と Main10)を復号し、
# NV12(10ビットは P010)のハッシュを ffmpeg の復号と比べる(hevctest.c、hevctest: の行。データは tools/hevc/mkjobs.py が作る)
ifeq ($(HEVCTEST),1)
CFLAGS_TARGET += -DCNF_HEVCTEST=1
SRCS += kernel/sysdepend/rpi5/hevctest_data.S
endif

ifeq ($(RP1_UART),1)
CFLAGS_TARGET += -DCNF_RP1_UART0=1
endif

# Wi-Fi(CYW43455、SDIO。設計書 09章 9.6.8)。make WIFI=0 で入れない
WIFI ?= 1
include $(TOP)/build_make/wifi_arm.mk

# ボード依存ソース(コア依存は sources.mk の SRCS_CORE)
SRCS += kernel/sysdepend/rpi5/hw_setting.c \
	kernel/sysdepend/rpi5/devinit.c \
	kernel/sysdepend/rpi5/pcie_rc.c \
	kernel/sysdepend/rpi/mbox.c \
	kernel/sysdepend/rpi5/objtest.c \
	kernel/sysdepend/rpi5/sndtest.c \
	kernel/sysdepend/rpi5/hevctest.c \
	kernel/sysdepend/rpi5/fan.c \
	kernel/sysdepend/rpi/rtc.c \
	kernel/sysdepend/rpi/rng_hw.c \
	kernel/sysdepend/rpi/wdog.c \
	device/gpio/gpio_bcm2712.c \
	device/gpio/gpio_rp1.c \
	device/ser/uart_rp1.c \
	device/ser/ser.c \
	device/net/eth_rp1.c \
	device/net/netdev.c \
	device/sd/sdhci.c \
	device/usb/usbdma.c \
	device/usb/xhci.c \
	device/usb/usbcore.c \
	device/usb/usbman.c \
	device/usb/hub.c \
	device/usb/hid.c \
	device/usb/msc.c \
	device/usb/uac.c \
	device/usb/uvc.c \
	device/usb/uusb.c \
	device/snd/snd.c \
	device/cam/cam.c \
	device/hevc/hevc.c \
	device/snd/snd_rp1.c \
	device/snd/snd_hdmi.c \
	device/disp/disp_rpi5.c \
	device/gpu/v3d/v3d.c \
	device/gpu/v3d/v3d_test.c \
	device/gpu/v3d/v3d_compose.c

# SD カードの FAT 区画(make sd SD=/mnt/e)
SD ?=
