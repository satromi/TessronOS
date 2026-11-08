#
# wpaproc.mk — wpa_supplicant のプロセス wpasupp(設計書 09章 9.6.11)
#
# WIFI=1 WPA_MODE=proc のとき、鍵の交換とその暗号(OpenSSL の libcrypto)をプロセス wpasupp で
# 動かす。カーネルの側(device/net/wifi/wpaproc/)は x64.mk と wifi_arm.mk が結び、ここでは
# プログラム実身 wpasupp(SYSDEF_PROG_WPASUPP)を作ってシステムボリュームに載せる。
#   - tools/wpa/proc.sh が、OpenSSL 3.5 の libcrypto をプログラムの旗で構成して作り、
#     wpa_supplicant のソースと crypto_openssl.c、wpa_glue.c、wps_serve.c と合わせて
#     ~/tessronos-wpaproc-x64/wpasupp_lib.o(ARM は ~/tessronos-wpaproc-arm/)にする
#   - application/wpasupp/ の wps_main.c と wps_libc.c をそれと newlib で結ぶ
#   - プログラム実身のメタデータとレコード0(application/wpasupp/def)と、strip した
#     プログラムのレコード1を $(UOBJ)/wpapstage に置き、システムボリュームの --over と
#     --hold にする(どの仮身からも結ばれず、カーネルが UUID で起動する)
# 既定の WPA_MODE=kernel では何も足さず、生成物は今までと同じ
ifeq ($(WIFI)$(WPA_MODE),1proc)
WPAP_DIR    := $(TOP)/application/wpasupp
WPAP_UUID   := 01a0d8c4-5a61-7b02-8c13-5e3f70819a61
WPAP_OUT    ?= $(HOME)/tessronos-wpaproc-$(if $(filter x86_64,$(ARCH)),x64,arm)
WPAP_LIB    := $(WPAP_OUT)/wpasupp_lib.o
WPAP_ELF    := $(UOBJ)/wpasupp.elf
WPAP_STAGE  := $(UOBJ)/wpapstage
WPAP_DONE   := $(WPAP_STAGE)/.done
WPAP_HOLD   := --hold $(WPAP_STAGE)
WPAP_HDRS   := $(TOP)/device/net/wifi/wpaproc/wpaproc.h $(TOP)/device/net/wifi/wpa/wpa_glue.h

$(WPAP_LIB): $(TOP)/tools/wpa/proc.sh $(WPAP_HDRS) $(TOP)/device/net/wifi/wpa/wpa_glue.c $(WPAP_DIR)/wps_eap.c \
	     $(WPAP_DIR)/wps_serve.c $(WPAP_DIR)/include/dirent.h
	@echo 'WPAP $@'
	@ARCH=$(ARCH) UFLAGS="$(ARCHFLAGS) $(UARCH_FP)" OUT=$(WPAP_OUT) bash $(TOP)/tools/wpa/proc.sh all

$(WPAP_ELF): $(APP_HDRS) $(MS_LIBTS) $(WPAP_DIR)/wps_main.c $(WPAP_DIR)/wps_libc.c $(WPAP_HDRS) \
	     $(WPAP_LIB) $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(MS_CFLAGS) -I$(TOP)/device/net/wifi/wpaproc -I$(WPAP_DIR)/include -o $@ $(MS_LIBTS) \
	  $(WPAP_DIR)/wps_main.c $(WPAP_DIR)/wps_libc.c $(WPAP_LIB) -lc -lgcc

$(WPAP_DONE): $(WPAP_ELF) $(WPAP_DIR)/def/$(WPAP_UUID).json $(WPAP_DIR)/def/$(WPAP_UUID)_0.xtad
	@rm -rf $(WPAP_STAGE)
	@mkdir -p $(WPAP_STAGE)
	@cp $(WPAP_DIR)/def/$(WPAP_UUID).json $(WPAP_DIR)/def/$(WPAP_UUID)_0.xtad $(WPAP_STAGE)/
	@$(CROSS)strip -o $(WPAP_STAGE)/$(WPAP_UUID)_1.bin $(WPAP_ELF)
	@touch $@

WIFI_OVER += --over $(WPAP_STAGE)
$(SYS_VOL): $(WPAP_DONE)
else
WPAP_HOLD   :=
endif
