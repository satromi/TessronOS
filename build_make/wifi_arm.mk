#
# wifi_arm.mk — Raspberry Pi 5 と Raspberry Pi 4 の Wi-Fi(CYW43455、SDIO。設計書 09章 9.6.8)
#
# rpi5.mk(既定の WIFI=1)、rpi4.mk と qemu_virt.mk(既定の WIFI=0)が読む。WIFI=1 で次を足す。
# Pi 4 の系統は ARMv8.0-A なので、rump カーネルと鍵の交換は別の置き場のものを結ぶ
# (~/tessronos-netbsd-v80-wifi、~/tessronos-wpa-arm-v80)。
#   - rump カーネルに NetBSD の sdhc・sdmmc・bwfm・net80211 を入れたもの
#     (tools/netbsd/build.sh の WIFI=1、~/tessronos-netbsd-wifi/rumpnet.o。bwfm.c は
#     tools/netbsd/patches の当てを当てた写しで翻訳し、当てを変えれば作り直す)
#   - そのハイパーコール(wifi_user_rpi5.c)と、基板の設定の文章を読む部分(wifi_nvram.c)
#   - Wi-Fi の管理(device/net/wifi/wifi.c)、WPA2 の鍵の交換(tools/wpa/build.sh の
#     ARCH=aarch64、~/tessronos-wpa-arm/wpasupp.o)、カードの選択(device/net/netsel.c)と
#     アダプタの管理(device/net/netadp.c)。ボードの Ethernet(Pi 5 は eth_rp1.c、QEMU は
#     vnet.c)の net_* は knl_eth_* の名前で翻訳し、netsel.c が Wi-Fi と選ぶ
#   - ファームウェア、CLM、基板の設定(文章の実身)、Wi-Fi の設定の実身と、それらを結ぶ
#     システム箱(tools/rpi5/wifi/fw.sh、mkobj.py)
#   - KTEST=1 では、基板の設定の取り出しのテスト(ktest_wifinv.c、群 wifinv)と、保存した
#     ネットワークと鍵のテスト(ktest_wifikey.c、群 wifikey)
# QEMU virt の WIFI=1 は、Wi-Fi のホストが無い機械で何も壊れないことと、取り出しの
# 試験のためのもの。WIFI=0 では何も足さず、生成物は WIFI の無いときと同じ
export WIFI
# 802.11 の層とドライバ。nbsd(既定)は rump カーネルの中の NetBSD の net80211、sdhc、sdmmc、bwfm、
# obsd は rump カーネルの外の OpenBSD のもの(device/net/wifi/obsd、tools/openbsd/build.sh の
# ARCH=aarch64、~/tessronos-obsd-arm/obsdwifi.o。Pi 4 の系統は -v80。設計書 9.6.8、9.6.9)
WIFI_NET80211 ?= nbsd
ifneq ($(filter-out nbsd obsd,$(WIFI_NET80211)),)
$(error WIFI_NET80211 は nbsd か obsd。"$(WIFI_NET80211)" は無い)
endif
# 下の規則が make の既定の目標にならないように、読む前のものを取っておいて戻す
WIFI_GOAL := $(.DEFAULT_GOAL)
# WIFI が変われば、それで翻訳の変わるものを作り直す(同じ BUILD_ID で WIFI を替えたとき)
WIFI_STAMP := $(OBJDIR)/wifi.stamp
$(shell mkdir -p $(OBJDIR); [ "$$(cat $(WIFI_STAMP) 2>/dev/null)" = "$(WIFI)$(subst nbsd,,$(WIFI_NET80211))" ] || echo "$(WIFI)$(subst nbsd,,$(WIFI_NET80211))" > $(WIFI_STAMP))
ifeq ($(TARGET),_RPI5_)
WIFI_ETH_OBJ := $(OBJDIR)/device/net/eth_rp1.o
else ifeq ($(TARGET),_RPI4_)
WIFI_ETH_OBJ := $(OBJDIR)/device/net/eth_genet.o
else
WIFI_ETH_OBJ := $(OBJDIR)/device/vnet/vnet.o
endif
$(WIFI_ETH_OBJ) $(OBJDIR)/tests/ktest/ktest_main.o $(OBJDIR)/sysvol.img: $(WIFI_STAMP)
# make WIFI_SIM=1(WIFI=1 と)で、チップの代わりに Wi-Fi のシミュレータ(仮想の無線と AP、
# 設計書 9.6.12、build_make/wifisim.mk)を入れる。テストのためのもので、既定は 0(何も足さない)
WIFI_SIM ?= 0
export WIFI_SIM
ifeq ($(WIFI),1)
ifeq ($(WIFI_NET80211),obsd)
# rump カーネルは Wi-Fi の無いもの(common.mk の既定の置き場)
$(NETBSD_OBJ): WIFI := 0
OBSD_OBJ ?= $(HOME)/tessronos-obsd-arm$(if $(filter _RPI4_,$(TARGET)),-v80)$(if $(filter 1,$(WIFI_SIM)),-wifisim)/obsdwifi.o
EXTRA_OBJS += $(OBSD_OBJ)
$(OBSD_OBJ): $(TOP)/tools/openbsd/build.sh $(wildcard $(TOP)/device/net/wifi/obsd/*.[ch]) \
	     $(wildcard $(TOP)/tools/openbsd/patches/*.patch)
	@echo 'OBSD $@'
	@ARCH=aarch64 OUT=$(patsubst %/,%,$(dir $(OBSD_OBJ))) MARCH=$(ARCH_MARCH) bash $(TOP)/tools/openbsd/build.sh all
SRCS += device/net/wifi/obsd/obsd_user.c
WIFI5_OBJS := $(OBJDIR)/device/net/wifi/obsd/obsd_user.o
endif
ifeq ($(WIFI_SIM),1)
include $(TOP)/build_make/wifisim.mk
else
ifeq ($(WIFI_NET80211),obsd)
ifeq ($(KTEST),1)
SRCS += tests/ktest/ktest_obsdwifi.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_OBSDWIFI=1
endif
else
ifeq ($(TARGET),_RPI4_)
NETBSD_OBJ := $(HOME)/tessronos-netbsd-v80-wifi/rumpnet.o
else
NETBSD_OBJ := $(HOME)/tessronos-netbsd-wifi/rumpnet.o
endif
$(NETBSD_OBJ): $(wildcard $(TOP)/peripheral_kernel/network/netbsd/rumpcomp/wifi/*.c) \
	       $(wildcard $(TOP)/tools/netbsd/patches/*.patch)
endif
SRCS += peripheral_kernel/network/netbsd/wifi_user_rpi5.c
WIFI5_OBJS += $(OBJDIR)/peripheral_kernel/network/netbsd/wifi_user_rpi5.o
endif
SRCS += peripheral_kernel/network/netbsd/wifi_nvram.c \
	device/net/netsel.c \
	device/net/netadp.c \
	device/net/wifi/wifi.c
WIFI5_OBJS += $(OBJDIR)/peripheral_kernel/network/netbsd/wifi_nvram.o \
	$(OBJDIR)/device/net/netsel.o \
	$(OBJDIR)/device/net/netadp.o \
	$(OBJDIR)/device/net/wifi/wifi.o
$(WIFI5_OBJS): CFLAGS += -DUSE_WIFI=1 -DUSE_NETSEL=1 $(if $(filter obsd,$(WIFI_NET80211)),-DWIFI_OBSD=1)
$(WIFI5_OBJS): $(TOP)/build_make/wifi_arm.mk
$(WIFI_ETH_OBJ): CFLAGS += -Dnet_get_mac=knl_eth_get_mac -Dnet_send=knl_eth_send \
	-Dnet_recv=knl_eth_recv -Dnet_stat=knl_eth_stat
# WPA2 の鍵の交換(wpa_supplicant の一部と device/net/wifi/wpa/ の受け)
# 鍵の交換の置き場(WPA_MODE、build_make/wpamode.mk)。既定の kernel はカーネルの中の wpa_supplicant
include $(TOP)/build_make/wpamode.mk
ifeq ($(WPA_MODE),kernel)
ifeq ($(TARGET),_RPI4_)
WPA_OBJ ?= $(HOME)/tessronos-wpa-arm-v80/wpasupp.o
else
WPA_OBJ ?= $(HOME)/tessronos-wpa-arm/wpasupp.o
endif
EXTRA_OBJS += $(WPA_OBJ)
$(WPA_OBJ): $(TOP)/tools/wpa/build.sh $(wildcard $(TOP)/device/net/wifi/wpa/*.[ch]) \
	    $(wildcard $(TOP)/device/net/wifi/wpa/include/*.h $(TOP)/device/net/wifi/wpa/include/*/*.h)
	@echo 'WPA $@'
	@ARCH=aarch64 MARCH=$(ARCH_MARCH) OUT=$(patsubst %/,%,$(dir $(WPA_OBJ))) bash $(TOP)/tools/wpa/build.sh all
endif
ifeq ($(KTEST),1)
SRCS += tests/ktest/ktest_wifinv.c tests/ktest/ktest_wifikey.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_WIFINV=1 -DKT_WIFIKEY=1
$(OBJDIR)/tests/ktest/ktest_main.o: $(TOP)/build_make/wifi_arm.mk
endif
# ファームウェア(~/tessronos-rpi5-wifi-fw、make WIFI_FW_DIR=... で別の場所)と、その実身、Wi-Fi の
# 設定の実身、それらを結ぶシステム箱
WIFI_FW_DIR ?= $(HOME)/tessronos-rpi5-wifi-fw
ifeq ($(TARGET),_RPI4_)
WIFI_OBJ_ARGS := --board "Raspberry Pi 4"
else
WIFI_OBJ_ARGS :=
endif
WIFI_STAGE  := $(OBJDIR)/wifistage
WIFI_DONE   := $(WIFI_STAGE)/.done
WIFI_OVER   := --over $(WIFI_STAGE)
$(WIFI_DONE): $(TOP)/tools/rpi5/wifi/fw.sh $(TOP)/tools/rpi5/wifi/mkobj.py $(TOP)/tools/rpi5/wifi/nvram.py \
	      $(TOP)/etc/def/01a0d8c4-5211-7c52-9e0a-3b61d27f4c85_0.xtad
	@OUT=$(WIFI_FW_DIR) bash $(TOP)/tools/rpi5/wifi/fw.sh fetch
	@python3 -I $(TOP)/tools/rpi5/wifi/mkobj.py $(WIFI_OBJ_ARGS) --fw $(WIFI_FW_DIR) \
	  --box $(TOP)/etc/def/01a0d8c4-5211-7c52-9e0a-3b61d27f4c85_0.xtad --out $(WIFI_STAGE)
	@touch $@
$(OBJDIR)/sysvol.img: $(WIFI_DONE)
else
WIFI_OVER   :=
endif
.DEFAULT_GOAL := $(WIFI_GOAL)
