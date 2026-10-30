#
# qemu_virt.mk — QEMU virt(-M virt,gic-version=2 -cpu cortex-a76)向けの設定
#

LNKFILE := $(TOP)/etc/linker/qemu_virt/tkernel_map.ld
LOAD_PA := 0x40200000

CFLAGS_TARGET  :=
IMAGE_TARGETS  :=

# ボード依存ソース(コア依存は sources.mk の SRCS_CORE)
SRCS += kernel/sysdepend/qemu_virt/hw_setting.c \
	kernel/sysdepend/qemu_virt/devinit.c \
	kernel/sysdepend/qemu_virt/rtc.c \
	device/vblk/vblk.c 	device/sd/sdhci.c \
	device/ser/ser.c \
	kernel/sysdepend/qemu_virt/rng_hw.c \
	device/vnet/vnet.c \
	device/net/netdev.c \
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
	device/usb/vhc.c \
	device/snd/snd.c \
	device/cam/cam.c \
	device/disp/disp_bochs.c

ifeq ($(KTEST),1)
SRCS += $(SRCS_KTEST_QEMU)
endif

# make WIFI=1: Raspberry Pi 5 の Wi-Fi の部分を入れ、ホストの無い機械で何も壊れないことと、
# 基板の設定の取り出し(KTEST の群 wifinv)を確かめる(設計書 09章 9.6.8)。既定は入れない
WIFI ?= 0
include $(TOP)/build_make/wifi_arm.mk

# QEMU 起動オプション(設計書 13.4.1)
TEST_TMO     ?= 480
QEMU_MACHINE ?= virt,gic-version=2
QEMU_CPU     ?= cortex-a76
QEMU_SMP     ?= 4
QEMU_MEM     ?= 2G
QEMU_EXTRA   ?=
QEMU_FLAGS   := -M $(QEMU_MACHINE) -cpu $(QEMU_CPU) -smp $(QEMU_SMP) -m $(QEMU_MEM) \
	-nographic -no-reboot $(QEMU_EXTRA)
