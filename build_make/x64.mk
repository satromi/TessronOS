#
# x64.mk — x64 の PC(UEFI。QEMU q35 + OVMF と Microsoft Surface Go 2 を1つのイメージで)向けの設定
#            (docs/tessronos/03-platform.md 3.10、docs/private/x64_surface_go2_study.md)
#
#   make TARGET=_X64_ BUILD_ID=<名前>          tessronos<名前>.elf と $(OBJDIR)/esp/(UEFI のローダとカーネル)
#   make TARGET=_X64_ BUILD_ID=<名前> qemu-x64 ビルドして Windows の QEMU(q35 + OVMF)で起動する
#   make TARGET=_X64_ [DESKTOP=1] BUILD_ID=<名前> qemu-x64-disk
#                                            ディスクのイメージ(sdimg)を NVMe として挿して起動する
#

# この .mk の規則が既定の目標にならないように
.DEFAULT_GOAL := all

LNKFILE := $(TOP)/etc/linker/x64/tkernel_map.ld
# カーネルの物理アドレスはローダが決めるので、決まった値は無い。共通部が読む LOAD_PA_VALUE は
# イメージの物理アドレスの変数(reset_main.c の knl_kimg_pa)になる
LOAD_PA := knl_kimg_pa

CFLAGS_TARGET  :=
IMAGE_TARGETS  := $(OBJDIR)/esp/EFI/BOOT/BOOTX64.EFI $(OBJDIR)/esp/tessronos.elf

# AArch64 の命令で書いた共通部のファイルは、x86-64 では建てない
SRCS := $(filter-out device/gpu/v3d/v3d_mmu.c outer_kernel/wm/compose_simd.c peripheral_kernel/libc/setjmp.S,$(SRCS)) \
	peripheral_kernel/libc/x86_64/setjmp.S
# Arm の機器だけの試験(デバイスツリー、SD のホストコントローラ、V3D の表)は建てない。
# ktest_main.c が飛ばしたと言う
SRCS := $(filter-out tests/ktest/ktest_board.c tests/ktest/ktest_sd.c tests/ktest/ktest_gpu.c tests/ktest/ktest_gl.c,$(SRCS))

# x64 の PC だけの装置の試験(USB でないキーボード、ACPI の電源、SATA、Intel の LAN)
ifeq ($(KTEST),1)
SRCS += tests/ktest/x86_64/ktest_pcdev.c tests/ktest/x86_64/ktest_igt.c tests/ktest/x86_64/ktest_sysinst.c \
	tests/ktest/x86_64/ktest_camisp.c
endif

# ボード依存ソース(コア依存は sources.mk の SRCS_CORE)
SRCS += kernel/sysdepend/x64/hw_setting.c \
	kernel/sysdepend/x64/devinit.c \
	kernel/sysdepend/x64/rtc.c \
	kernel/sysdepend/x64/rng_hw.c \
	kernel/sysdepend/x64/fbcon.c \
	kernel/sysdepend/x64/bootdisk.c \
	kernel/sysdepend/x64/bootlog.c \
	kernel/sysdepend/x64/gfxdump.c \
	kernel/sysdepend/x64/igt_ggtt.c \
	kernel/sysdepend/x64/igt_pcode.c \
	kernel/sysdepend/x64/vtd.c \
	kernel/sysdepend/acpi/acpi.c \
	kernel/sysdepend/acpi/acpi_os.c \
	kernel/sysdepend/acpi/acpi_power.c \
	outer_kernel/wm/x86_64/compose_simd.c \
	device/disp/disp_gop.c \
	device/disp/x86_64/disp_igfx.c \
	device/gpu/intel/igt.c \
	device/gpu/intel/igt_vm.c \
	device/gpu/intel/igt_pm.c \
	device/gpu/intel/igt_test.c \
	device/gpu/intel/igt_compose.c \
	device/gpu/intel/igt_rcompose.c \
	device/gpu/intel/igt_rplan.c \
	device/nvme/nvme.c \
	device/ps2/ps2.c \
	device/ahci/ahci.c \
	device/i2c/x86_64/dw_i2c.c \
	device/i2c/x86_64/hid_i2c.c \
	device/ser/x86_64/ser.c \
	device/virtio/vio_pci.c \
	device/vnet/vnet.c \
	device/net/netsel.c \
	device/net/netadp.c \
	device/net/x86_64/e1k.c \
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
	device/usb/x86_64/cdcnet.c \
	device/usb/x86_64/xhci_pc.c \
	device/usb/vhc.c \
	device/snd/snd.c \
	device/snd/x86_64/snd_hda.c \
	device/cam/cam.c \
	device/cam/cam_isp.c \
	device/cam/x86_64/cio2.c \
	device/cam/x86_64/int3472.c \
	device/cam/x86_64/ov5693.c \
	device/gpio/x86_64/gpio_pch.c

# UEFI のローダ(tools/x64/uefi)。mingw-w64 の GCC で PE32+ の EFI アプリにする
EFI_CROSS ?= $(if $(TS_X64_EFI_TOOLCHAIN),$(TS_X64_EFI_TOOLCHAIN)/,$(XPACK_PREFIX)/x86_64-w64-mingw32-gcc/bin/)x86_64-w64-mingw32-
EFI_CC    := $(EFI_CROSS)gcc
EFI_SRCS  := $(TOP)/tools/x64/uefi/loader.c
EFI_HDRS  := $(TOP)/tools/x64/uefi/efi.h $(TOP)/kernel/sysdepend/cpu/core/x86_64/bootinfo.h \
	     $(TOP)/kernel/sysdepend/x64/confont.h
EFI_CFLAGS := -O2 -g0 -ffreestanding -fno-builtin -fshort-wchar -mno-red-zone -fno-stack-protector \
	-fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-parameter \
	-nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--image-base,0x10000000

$(OBJDIR)/BOOTX64.EFI: $(EFI_SRCS) $(EFI_HDRS)
	@mkdir -p $(dir $@)
	@echo 'EFI $@'
	@$(EFI_CC) $(EFI_CFLAGS) -o $@ $(EFI_SRCS)

# ESP の中身: \EFI\BOOT\BOOTX64.EFI、\tessronos.elf、\cmdline.txt(起動の設定)。
# QEMU はこのディレクトリをそのまま FAT のディスクとして見せる(fat:rw:)
X64_CMDLINE ?= video=1280x800
$(OBJDIR)/esp/EFI/BOOT/BOOTX64.EFI: $(OBJDIR)/BOOTX64.EFI
	@mkdir -p $(dir $@)
	@cp $< $@

$(OBJDIR)/esp/tessronos.elf: $(EXE_FILE).elf
	@mkdir -p $(dir $@)
	@cp $< $@
	@printf '%s\n' "$(X64_CMDLINE)" > $(OBJDIR)/esp/cmdline.txt

# 試験機: Windows の QEMU の q35 と、それに付いてくる OVMF(edk2-x86_64-code.fd)。
# WSL の apt の qemu-system-x86 と ovmf は sudo が要るので使わない
QEMU_WIN_X64 ?= /mnt/c/Program Files/qemu/qemu-system-x86_64.exe
QEMU         := $(QEMU_WIN_X64)
OVMF_CODE    ?= C:/Program Files/qemu/share/edk2-x86_64-code.fd
TEST_TMO     ?= 300
QEMU_MACHINE ?= q35
QEMU_CPU     ?= max
QEMU_SMP     ?= 4
QEMU_MEM     ?= 2G
QEMU_EXTRA   ?=
# 内蔵時計(CMOS の RTC)はローカル時刻で、Windows の入った PC と同じ形(設計書 9.5.1)
QEMU_FLAGS   := -M $(QEMU_MACHINE) -cpu $(QEMU_CPU) -smp $(QEMU_SMP) -m $(QEMU_MEM) -rtc base=localtime \
	-drive "if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)" \
	-no-reboot -net none $(QEMU_EXTRA)
X64_ESP_WIN   = $(shell wslpath -m $(abspath $(OBJDIR)/esp) 2>/dev/null)
X64_ESP_DRIVE = -drive format=raw,file=fat:rw:$(X64_ESP_WIN)

.PHONY: qemu-x64
qemu-x64: $(EXE_FILE).elf $(IMAGE_TARGETS)
	"$(QEMU)" $(QEMU_FLAGS) $(X64_ESP_DRIVE) -serial stdio

# 窓の合成の内側のループ(SSE と AVX2)だけは SIMD のレジスタを使ってよい(TA_FPU の合成のタスクが呼ぶ)
$(OBJDIR)/outer_kernel/wm/x86_64/compose_simd.o: CFLAGS = $(filter-out -mgeneral-regs-only,$(CFLAGS_COMMON) \
	$(CFLAGS_TARGET) -D$(TARGET) -DLOAD_PA_VALUE=$(LOAD_PA))

# PC のネットワークのカードは virtio-net か USB の LAN のアダプタ(device/net/netsel.c が選ぶ)。
# vnet.c の net_* は knl_vnet_* の名前で翻訳する
$(OBJDIR)/device/vnet/vnet.o: CFLAGS += -Dnet_get_mac=knl_vnet_get_mac -Dnet_send=knl_vnet_send \
	-Dnet_recv=knl_vnet_recv -Dnet_stat=knl_vnet_stat
$(OBJDIR)/device/vnet/vnet.o: $(TOP)/build_make/x64.mk

# ACPI の解釈系(ACPICA)。tools/x64/acpica/build.sh がリリースを取得し、OS の層とカーネルが使う所
# (kernel/sysdepend/acpi/acpica/)と合わせて一つのオブジェクトにする(docs/tessronos/03-platform.md 3.10.6)
ACPICA_OBJ ?= $(HOME)/tessronos-acpica-x64/acpica.o
EXTRA_OBJS += $(ACPICA_OBJ)
$(ACPICA_OBJ): $(TOP)/tools/x64/acpica/build.sh $(wildcard $(TOP)/kernel/sysdepend/acpi/acpica/*)
	@echo 'ACPICA $@'
	@OUT=$(patsubst %/,%,$(dir $(ACPICA_OBJ))) bash $(TOP)/tools/x64/acpica/build.sh all

# NetBSD のスタックの x86_64 用のもの(~/tessronos-netbsd-x64/rumpnet.o)を作る手順
NETBSD_BUILD := $(TOP)/tools/x64/netbsd/build.sh

# Wi-Fi(docs/tessronos/09-network.md 9.6)。既定の WIFI=1 で、Intel の 8265 などを OpenBSD の
# net80211 と iwm で(rump カーネルの外の層、device/net/wifi/obsd、tools/openbsd/build.sh、
# ~/tessronos-obsd-x64/obsdwifi.o。設計書 9.6.9)、WPA2 の鍵の交換を wpa_supplicant の一部で
# (tools/wpa/build.sh)動かし、ファームウェア(API の版36)の実身と Wi-Fi の設定の実身をシステム
# ボリュームに載せる(tools/x64/wifi/fw.sh、mkobj.py)。rump カーネルは Wi-Fi の無いもの。アダプタの
# 管理(netadp.c)は WIFI=0 でも入る。NetBSD の net80211 と iwm を rump カーネルの中で動かす形
# (WIFI_NET80211=nbsd)は除いた(NetBSD の 802.11 の層は Raspberry Pi 5 の bwfm で使う)
WIFI ?= 1
export WIFI
NETSEL_OBJS := $(OBJDIR)/device/net/netsel.o $(OBJDIR)/device/net/netadp.o
$(NETSEL_OBJS): $(TOP)/build_make/x64.mk
WIFI_NET80211 ?= obsd
ifneq ($(WIFI_NET80211),obsd)
$(error x64 の Wi-Fi は OpenBSD の層(WIFI_NET80211=obsd)だけ。"$(WIFI_NET80211)" は除いた)
endif
# make WIFI_SIM=1 で、カードの代わりに Wi-Fi のシミュレータ(仮想の無線と AP、設計書 9.6.12、
# build_make/wifisim.mk)を入れる。テストのためのもので、既定は 0(何も足さない)
WIFI_SIM ?= 0
export WIFI_SIM
ifeq ($(WIFI),1)
# the rump kernel without the Wi-Fi (tools/x64/netbsd/build.sh has none)
$(NETBSD_OBJ): WIFI := 0
# the Wi-Fi simulator's (WIFI_SIM=1, build_make/wifisim.mk) in another place: no iwm in it
OBSD_OBJ ?= $(HOME)/tessronos-obsd-x64$(if $(filter 1,$(WIFI_SIM)),-wifisim)/obsdwifi.o
EXTRA_OBJS += $(OBSD_OBJ)
$(OBSD_OBJ): $(TOP)/tools/openbsd/build.sh $(wildcard $(TOP)/device/net/wifi/obsd/*.[ch]) \
	     $(wildcard $(TOP)/tools/openbsd/patches/*.patch)
	@echo 'OBSD $@'
	@OUT=$(patsubst %/,%,$(dir $(OBSD_OBJ))) MARCH=$(ARCH_MARCH) bash $(TOP)/tools/openbsd/build.sh all
SRCS += device/net/wifi/obsd/obsd_user.c
$(OBJDIR)/device/net/wifi/wifi.o $(OBJDIR)/peripheral_kernel/network/netbsd/wifi_user.o \
$(OBJDIR)/device/net/wifi/obsd/obsd_user.o: CFLAGS += -DWIFI_OBSD=1
# KTEST=1 では、層が立つもの(ロック、眠り、タイムアウト、タスク、mbuf と DMA、暗号)のテスト(群 obsdwifi)。
# Wi-Fi のシミュレータ(WIFI_SIM=1)の層には DMA が無いので入れない
ifeq ($(KTEST)$(WIFI_SIM),10)
SRCS += tests/ktest/ktest_obsdwifi.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_OBSDWIFI=1
endif
$(NETSEL_OBJS): CFLAGS += -DUSE_WIFI=1
SRCS += device/net/wifi/wifi.c
ifeq ($(WIFI_SIM),1)
include $(TOP)/build_make/wifisim.mk
else
SRCS += peripheral_kernel/network/netbsd/wifi_user.c
endif
$(OBJDIR)/device/net/wifi/wifi.o: CFLAGS += -DUSE_WIFI=1
# 鍵の交換の置き場(WPA_MODE、build_make/wpamode.mk)。既定の kernel はカーネルの中の wpa_supplicant
include $(TOP)/build_make/wpamode.mk
ifeq ($(WPA_MODE),kernel)
WPA_OBJ ?= $(HOME)/tessronos-wpa-x64/wpasupp.o
EXTRA_OBJS += $(WPA_OBJ)
$(WPA_OBJ): $(TOP)/tools/wpa/build.sh $(wildcard $(TOP)/device/net/wifi/wpa/*.[ch]) \
	    $(wildcard $(TOP)/device/net/wifi/wpa/include/*.h $(TOP)/device/net/wifi/wpa/include/*/*.h)
	@echo 'WPA $@'
	@OUT=$(patsubst %/,%,$(dir $(WPA_OBJ))) bash $(TOP)/tools/wpa/build.sh all
endif
# KTEST=1 では、保存したネットワークと鍵のテスト(ktest_wifikey.c、群 wifikey)
ifeq ($(KTEST),1)
SRCS += tests/ktest/ktest_wifikey.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_WIFIKEY=1
$(OBJDIR)/tests/ktest/ktest_main.o: $(TOP)/build_make/x64.mk
endif
# ファームウェア(~/tessronos-wifi-fw)と、その実身、Wi-Fi の設定の実身、それらを結ぶシステム箱
WIFI_FW_DIR ?= $(HOME)/tessronos-wifi-fw
# ファームウェア(OpenBSD の iwm が求める API の版36)の実身などの置き場
WIFI_STAGE  := $(UOBJ)/wifistage36
WIFI_DONE   := $(WIFI_STAGE)/.done
WIFI_OVER   := --over $(WIFI_STAGE)
$(WIFI_DONE): $(TOP)/tools/x64/wifi/fw.sh $(TOP)/tools/x64/wifi/mkobj.py \
	      $(TOP)/etc/def/01a0d8c4-5211-7c52-9e0a-3b61d27f4c85_0.xtad
	@OUT=$(WIFI_FW_DIR) bash $(TOP)/tools/x64/wifi/fw.sh fetch
	@python3 -I $(TOP)/tools/x64/wifi/mkobj.py --fw $(WIFI_FW_DIR) \
	  --box $(TOP)/etc/def/01a0d8c4-5211-7c52-9e0a-3b61d27f4c85_0.xtad --out $(WIFI_STAGE)
	@touch $@
$(OBJDIR)/sysvol.img: $(WIFI_DONE)
else
WIFI_OVER   :=
endif

# カーネルの libgcc は -mno-red-zone の組(tools/x64/build_toolchain.sh のマルチライブ)
LDFLAGS_COMMON += $(ARCH_KFLAGS)

# ディスクのイメージ(makefile の sdimg、$(OBJDIR)/sd.img)を NVMe の SSD として挿して起動する
X64_DISK_WIN  = $(shell wslpath -m $(abspath $(OBJDIR)/sd.img) 2>/dev/null)
X64_DISK_DEV  = -drive file=$(X64_DISK_WIN),if=none,format=raw,id=nvd -device nvme,serial=tessronos,drive=nvd
X64_DEVICES  ?= -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
	-netdev user,id=n0,restrict=on -device virtio-net-pci,netdev=n0

.PHONY: qemu-x64-disk
qemu-x64-disk: sdimg
	"$(QEMU)" $(QEMU_FLAGS) $(X64_DISK_DEV) $(X64_DEVICES) -serial stdio
