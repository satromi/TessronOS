#
# rpi4.mk — Raspberry Pi 4、Raspberry Pi 400、Compute Module 4(BCM2711)向けの設定
#
#   命令セットの基準は ARMv8.0-A に CRC32 を足したもの(common.mk)。NetBSD のスタックと
#   mozc もその基準で作った別の置き場のもの(~/tessronos-netbsd-v80、~/tessronos-mozc-v80)を
#   結ぶ。ブラウザ(BROWSER=1)は組み込まない(docs/tessronos/03-platform.md 3.6)
#

LNKFILE := $(TOP)/etc/linker/rpi4/tkernel_map.ld
LOAD_PA := 0x200000

CFLAGS_TARGET  :=
IMAGE_TARGETS  := $(EXE_FILE).img

ifeq ($(BROWSER),1)
$(error TARGET=_RPI4_ does not take BROWSER=1 (the browser is built for ARMv8.2-A only))
endif

# イメージに組み込むアプリのパッケージ(.tpk)。etc/pkg/ のものは ARMv8.2-A 向けなので、
# ARMv8.0-A(Cortex-A72)向けに作ったものを etc/pkg/rpi4/ に置く。MPEG Player は
# ffmpeg_btron の TS_CPU=cortex-a72 の build-ffmpeg-ts.sh と build-tessron.sh で作る。
# make PKGS="a.tpk b.tpk" で直に指定もできる
PKGS ?= $(wildcard $(TOP)/etc/pkg/rpi4/*.tpk)

# make DISP_SWAP_RB=1: 画面の赤と青が入れ替わって見えるとき、ファームウェアの
# 画素の並びを逆に読む(設計書 16.5.1)
ifeq ($(DISP_SWAP_RB),1)
CFLAGS_TARGET += -DCNF_DISP_SWAP_RB=1
endif

# 電池の無い時計の代わりに、起動時に DHCP でアドレスを取り、SNTP で時計を合わせる
# (設計書 12.6)。make NET_AUTO=0 で止める
ifneq ($(NET_AUTO),0)
CFLAGS_TARGET += -DCNF_NET_DHCP=1 -DCNF_NET_SNTP=1
endif

# make OBJTEST=0: 起動の後のデバイスの実身の確認(objtest.c、objtest: の行)を行わない
ifeq ($(OBJTEST),0)
CFLAGS_TARGET += -DCNF_OBJTEST=0
endif

# make TPANEL=0: 40 ピンヘッダのピンのタッチパネル(GT911)を探さない(設計書 3.6.7)
ifeq ($(TPANEL),0)
CFLAGS_TARGET += -DCNF_RPI_TPANEL=0
endif

# make LCD=dpi35: 3.5inch DPI LCD の画面の設定(etc/boot/config-rpi4-lcd-<名前>.txt)を
# config.txt の後ろに足す(設計書 3.6.7)。ヘッダの UART のコンソールは使えなくなる。
# PocketTerm35(HDMI の液晶)は普通の Pi 4 のカードで動き、映らないときだけ config.txt の
# コメントにした4行を使う。タッチパネルはどちらも起動時に見分ける
LCD ?=
ifeq ($(LCD),)
RPI4_CONFIG := $(TOP)/etc/boot/config-rpi4.txt
else
RPI4_LCD_TXT := $(TOP)/etc/boot/config-rpi4-lcd-$(LCD).txt
ifeq ($(wildcard $(RPI4_LCD_TXT)),)
$(error LCD=$(LCD): there is no $(RPI4_LCD_TXT) (use LCD=dpi35))
endif
RPI4_CONFIG := $(OBJDIR)/config-rpi4.txt
endif

# make DWC2=0: USB-C のポートの DWC2 を調べず、使わない(設計書 3.6.7)
ifeq ($(DWC2),0)
CFLAGS_TARGET += -DCNF_DWC2=0
endif

# make PCIE_POLL=1: PCIe の先の xHCI を割込み(INTA)ではなく一定の間隔で見る
ifeq ($(PCIE_POLL),1)
CFLAGS_TARGET += -DCNF_PCIE_POLL=1
endif

# Wi-Fi(CYW43455、SDIO。設計書 09章 9.6.8)。ファームウェアを含むイメージになるので、
# 既定は入れない。make WIFI=1 で入れる
WIFI ?= 0
include $(TOP)/build_make/wifi_arm.mk

# ボード依存ソース(コア依存は sources.mk の SRCS_CORE)。メールボックス、時計、乱数は
# Raspberry Pi 5 と共通(kernel/sysdepend/rpi)
SRCS += kernel/sysdepend/rpi4/hw_setting.c \
	kernel/sysdepend/rpi4/devinit.c \
	kernel/sysdepend/rpi4/pcie_rc.c \
	kernel/sysdepend/rpi4/objtest.c \
	kernel/sysdepend/rpi4/tpanel.c \
	kernel/sysdepend/rpi/mbox.c \
	kernel/sysdepend/rpi/rtc.c \
	kernel/sysdepend/rpi/rng_hw.c \
	kernel/sysdepend/rpi/wdog.c \
	device/gpio/gpio_bcm2711.c \
	device/ser/ser.c \
	device/net/eth_genet.c \
	device/net/netdev.c \
	device/sd/sdhci.c \
	device/usb/usbdma.c \
	device/usb/xhci.c \
	device/usb/dwc2.c \
	device/usb/usbcore.c \
	device/usb/usbman.c \
	device/usb/hub.c \
	device/usb/hid.c \
	device/usb/msc.c \
	device/usb/uac.c \
	device/usb/uvc.c \
	$(SRCS_BT) \
	device/usb/uusb.c \
	device/snd/snd.c \
	device/cam/cam.c \
	device/disp/disp_rpi5.c

# SD カードの FAT 区画(make sd SD=/mnt/e)
SD ?=

# QEMU の raspi4b(設計書 03-platform.md 3.6.4)。xPack の 9.2 にもある。
# make TARGET=_RPI4_ qemu-rpi4 で起動し、make TARGET=_RPI4_ KTEST=1 test-rpi4 で試験を走らせる。
# カーネルはファームウェアと同じくヘッダの無いフラットな像を 0x80000 から始める
# (boot.S がリンク番地 0x200000 へ写す)。デバイスツリーはファームウェアの配布物のもの
RPI4_QEMU_DTB ?= obj/boot/bcm2711-rpi-4-b.dtb
RPI4_QEMU_FLAGS := -M raspi4b -smp 4 -nographic -no-reboot
# USB-C のポートの DWC2 に挿すキーボード(3.6.7)。usb_version=1 を足すとフルスピードになる
RPI4_QEMU_USB ?= -device usb-kbd,id=dwkbd
RPI4_QEMU_DWC2 ?=
TEST_TMO ?= 900
