#
# wpamode.mk — Wi-Fi の鍵の交換の置き場(設計書 09章 9.6.11)。x64.mk と wifi_arm.mk が WIFI=1 のときに読む。
#
#   WPA_MODE=kernel(既定)	wpa_supplicant の一部と Mbed TLS をカーネルの中で動かす
#				(tools/wpa/build.sh の wpasupp.o を結ぶ。読む側の mk が書く)
#   WPA_MODE=proc		プロセス wpasupp で動かす。カーネルは、knl_wpa_* をメッセージにして
#				プロセスに渡す口(device/net/wifi/wpaproc/)と、プロセスの起動と見張りを
#				持つ。プログラム実身は build_make/wpaproc.mk が作る
#
# WPA_MODE が変われば、カーネルのリンクとシステムボリュームを作り直す
WPA_MODE ?= kernel
export WPA_MODE
ifeq ($(filter kernel proc,$(WPA_MODE)),)
$(error unknown WPA_MODE "$(WPA_MODE)" (use kernel or proc))
endif
WPA_STAMP := $(OBJDIR)/wpa_mode.stamp
$(shell mkdir -p $(OBJDIR); [ "$$(cat $(WPA_STAMP) 2>/dev/null)" = "$(WPA_MODE)" ] || echo "$(WPA_MODE)" > $(WPA_STAMP))
$(EXE_FILE).elf $(OBJDIR)/sysvol.img: $(WPA_STAMP)
ifeq ($(WPA_MODE),proc)
SRCS += device/net/wifi/wpaproc/wpaproc.c device/net/wifi/wpaproc/wpaproc_call.c
WPAP_KOBJS := $(OBJDIR)/device/net/wifi/wpaproc/wpaproc.o $(OBJDIR)/device/net/wifi/wpaproc/wpaproc_call.o
$(WPAP_KOBJS): CFLAGS += -I$(TOP)/device/net/wifi/wpa -I$(TOP)/device/net/wifi/wpaproc
$(WPAP_KOBJS): $(TOP)/device/net/wifi/wpaproc/wpaproc.h $(TOP)/device/net/wifi/wpa/wpa_glue.h \
	       $(TOP)/build_make/wpamode.mk
# KTEST=1 では、プロセスが答えることと「wpasup」をほかが読めないことのテスト(群 wpaproc)
ifeq ($(KTEST),1)
SRCS += tests/ktest/ktest_wpaproc.c
$(OBJDIR)/tests/ktest/ktest_main.o: CFLAGS += -DKT_WPAPROC=1
endif
endif
$(OBJDIR)/tests/ktest/ktest_main.o: $(WPA_STAMP)
