#
# testagent.mk — 試験用の遠隔操作の口(make TESTAGENT=1。設計書 12.4.1)
#
# 既定のビルドとリリースのビルドには何も入らない。TLS は lib/mbedtls の一部を
# カーネルの旗で作り、1つのオブジェクトにまとめて ta_tls_ の名前だけを外に見せる
# (Bluetooth と Wi-Fi の持つ Mbed TLS とは別の設定なので、名前をぶつけない)
#
ifeq ($(TESTAGENT),1)
ifneq ($(DESKTOP),1)
$(error TESTAGENT=1 は DESKTOP=1 と併せて使う)
endif

TA_MBED := aes cipher cipher_wrap constant_time gcm md platform platform_util sha256 \
	   ssl_ciphersuites ssl_msg ssl_tls ssl_tls12_server
TA_DIR  := $(OBJDIR)/ta_tls
TA_OBJ  := $(TA_DIR)/ta_tls_all.o
TA_PARTS := $(patsubst %,$(TA_DIR)/%.o,$(TA_MBED)) $(TA_DIR)/ta_tls.o
TA_FLAGS = -w '-DMBEDTLS_CONFIG_FILE="ta_tls_config.h"' -I$(TOP)/application/desktop -I$(TOP)/lib/mbedtls/include \
	   -I$(TOP)/lib/mbedtls/library
TA_HDRS := $(TOP)/application/desktop/ta_tls_config.h $(TOP)/application/desktop/ta_tls.h

$(TA_DIR)/%.o: $(TOP)/lib/mbedtls/library/%.c $(TA_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CFLAGS) $(TA_FLAGS) -c -o $@ $<

$(TA_DIR)/ta_tls.o: $(TOP)/application/desktop/ta_tls.c $(TA_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(CFLAGS) $(TA_FLAGS) -c -o $@ $<

$(TA_OBJ): $(TA_PARTS)
	@echo 'LD  $@'
	@$(CROSS)ld -r -o $@ $(TA_PARTS)
	@$(OBJCOPY) --keep-global-symbol=ta_tls_setup --keep-global-symbol=ta_tls_open \
	  --keep-global-symbol=ta_tls_read --keep-global-symbol=ta_tls_write --keep-global-symbol=ta_tls_close $@

EXTRA_OBJS += $(TA_OBJ)
endif
