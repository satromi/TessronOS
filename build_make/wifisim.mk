#
# wifisim.mk — Wi-Fi のシミュレータ(設計書 09章 9.6.12)。make WIFI_SIM=1 のときだけ、x64.mk と
# wifi_arm.mk の WIFI=1 の中から読む。既定(WIFI_SIM=0)では読まれず、何も足さない。
#
# カードの代わりに仮想の無線を2つ(STA と HOSTAP の AP、その間の仮想の電波)を置き、AP の側
# (device/net/wifi/sim/wifisim_ap.c)が DHCP・DNS・ARP・ICMP と UDP のエコーの応答を持つ。STA は
# そのマシンのカードと同じ名前(x64 は iwm0、ARM は bwfm0)で、Wi-Fi の管理(wifi.c)、カードの
# 部品、カードの選択(netsel.c)、アダプタの管理(netadp.c)、DHCP はそのまま通る。
#   - x64: OpenBSD の 802.11 の層の中(device/net/wifi/sim/obsd_sim.c、tools/openbsd/build.sh の
#     WIFI_SIM=1、~/tessronos-obsd-x64-wifisim/obsdwifi.o。iwm と PCI は入れない)。AP の WPA2 の
#     認証は層の HOSTAP が持つものを使う。rump カーネルは Wi-Fi の無いもの
#   - ARM: rump カーネルの中の NetBSD の 802.11 の層の上(rumpcomp/wifisim/tswifi_sim.c、
#     ~/tessronos-netbsd-wifisim。sdhc・sdmmc・bwfm は入れない)。AP の WPA2 の認証は hostapd の
#     認証側(tools/wpa/build.sh ap、~/tessronos-wpaap-arm/wpaap.o)
#   - ハイパーコールは wifi_user_sim.c(wifi_user.c、wifi_user_rpi5.c の代わり)
#   - KTEST=1 では群 wifisim(tests/ktest/ktest_wifisim.c)
# 生成物は BUILD_ID を分けて作る(例 make TARGET=_X64_ KTEST=1 KTONLY=wifisim WIFI_SIM=1 BUILD_ID=simob
# sdimg、make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=wifisim WIFI=1 WIFI_SIM=1 BUILD_ID=simka test)。
#
ifeq ($(ARCH),x86_64)
WIFISIM_LAYER_AUTH := 1
$(OBSD_OBJ): $(TOP)/device/net/wifi/sim/obsd_sim.c
else
WIFISIM_LAYER_AUTH := 0
NETBSD_OBJ := $(HOME)/tessronos-netbsd-wifisim/rumpnet.o
WSAP_OBJ   ?= $(HOME)/tessronos-wpaap-arm/wpaap.o
WSAP_ENV   := ARCH=aarch64 MARCH=$(ARCH_MARCH)
$(NETBSD_OBJ): $(wildcard $(TOP)/peripheral_kernel/network/netbsd/rumpcomp/wifi/*.c) \
	       $(wildcard $(TOP)/peripheral_kernel/network/netbsd/rumpcomp/wifisim/*.c)
endif

SRCS += peripheral_kernel/network/netbsd/wifi_user_sim.c device/net/wifi/sim/wifisim_ap.c
WIFISIM_OBJS := $(OBJDIR)/peripheral_kernel/network/netbsd/wifi_user_sim.o \
	$(OBJDIR)/device/net/wifi/sim/wifisim_ap.o
# ARM では wifi_arm.mk の WIFI5_OBJS(USE_WIFI、USE_NETSEL)にも入れる
WIFI5_OBJS := $(WIFISIM_OBJS)
WIFISIM_STAMP := $(OBJDIR)/wifisim.stamp
$(shell mkdir -p $(OBJDIR); [ -f $(WIFISIM_STAMP) ] || echo 1 > $(WIFISIM_STAMP))
$(WIFISIM_OBJS) $(OBJDIR)/device/net/wifi/wifi.o $(OBJDIR)/device/net/netadp.o: CFLAGS += -DWIFI_SIM=1
$(OBJDIR)/device/net/wifi/sim/wifisim_ap.o: CFLAGS += -DWIFISIM_LAYER_AUTH=$(WIFISIM_LAYER_AUTH)
$(OBJDIR)/peripheral_kernel/network/netbsd/wifi_user_sim.o: CFLAGS += $(if $(filter x86_64,$(ARCH)),-DWIFI_OBSD=1)
$(WIFISIM_OBJS) $(OBJDIR)/device/net/wifi/wifi.o $(OBJDIR)/device/net/netadp.o: \
	$(WIFISIM_STAMP) $(TOP)/build_make/wifisim.mk
$(OBJDIR)/device/net/wifi/sim/wifisim_ap.o: $(wildcard $(TOP)/device/net/wifi/sim/*.h)

# AP の認証側(hostapd の src/ap と wsap_glue.c、それだけの写しの supplicant と Mbed TLS)。
# ARM の NetBSD の層のときだけ(OpenBSD の層は HOSTAP の中に認証側を持つ)
ifeq ($(WIFISIM_LAYER_AUTH),0)
EXTRA_OBJS += $(WSAP_OBJ)
$(WSAP_OBJ): $(TOP)/tools/wpa/build.sh $(wildcard $(TOP)/device/net/wifi/sim/wsap_*) \
	     $(wildcard $(TOP)/device/net/wifi/wpa/*.[ch]) \
	     $(wildcard $(TOP)/device/net/wifi/wpa/include/*.h $(TOP)/device/net/wifi/wpa/include/*/*.h)
	@echo 'WPAAP $@'
	@$(WSAP_ENV) OUT=$(patsubst %/,%,$(dir $(WSAP_OBJ))) bash $(TOP)/tools/wpa/build.sh ap
endif

ifeq ($(KTEST),1)
SRCS += tests/ktest/ktest_wifisim.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_WIFISIM=1
$(OBJDIR)/tests/ktest/ktest_main.o $(OBJDIR)/tests/ktest/ktest_wifisim.o: $(WIFISIM_STAMP)
endif
