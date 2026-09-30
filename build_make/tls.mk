#
# tls.mk — TLS for programs (lib/libtls on lib/mbedtls)
#
#   mbed TLS is taken in as it is (lib/mbedtls, UPSTREAM.md); what TessronOS
#   has of it is said by lib/libtls/ts_tls_config.h, and the pieces it asks
#   of the system are in lib/libtls/tls.c. The two make one library that a
#   program links with newlib: マイクロスクリプト for HTTPS.
#

TLS_DIR     := $(TOP)/lib/libtls
MBEDTLS_DIR := $(TOP)/lib/mbedtls
TLS_OUT     := obj/lib/tls
TLS_LIB     := obj/lib/libtls.a

TLS_CFLAGS  := $(ARCHFLAGS) -mstrict-align -fno-stack-protector -O2 -g -DCHK_TKERNEL_CONST \
	'-DMBEDTLS_CONFIG_FILE="ts_tls_config.h"' -I$(TLS_DIR) -I$(MBEDTLS_DIR)/include \
	-I$(TOP)/include -I$(TOP)/config

TLS_HDRS    := $(TLS_DIR)/ts_tls_config.h $(TOP)/include/ts/tls.h
TLS_SRCS    := $(wildcard $(MBEDTLS_DIR)/library/*.c)
TLS_OBJS    := $(patsubst $(MBEDTLS_DIR)/library/%.c,$(TLS_OUT)/%.o,$(TLS_SRCS)) $(TLS_OUT)/ts_tls.o

$(TLS_OUT)/%.o: $(MBEDTLS_DIR)/library/%.c $(TLS_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TLS_CFLAGS) -c -o $@ $<

$(TLS_OUT)/ts_tls.o: $(TLS_DIR)/tls.c $(TLS_HDRS) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TLS_CFLAGS) -Wall -c -o $@ $<

$(TLS_LIB): $(TLS_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^
