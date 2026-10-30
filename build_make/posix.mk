#
# posix.mk — POSIX のプログラム(コマンドとシェル)の土台(設計書 5.8)
#
#   lib/libposix は newlib の下回り(_read・_write など)と、newlib が系に任せる POSIX の
#   関数を、ファイルの層(fs_)・POSIX のプロセスの呼出し(include/ts/psx.h)・実身(ob_)の上に
#   作る。コマンドとシェルは include/posix(newlib の頭書きを広げたもの)で翻訳し、
#   psx_crt.o から始まって libposix.a と newlib を結ぶ。ブラウザが結ぶ lib/libcxxrt と
#   include/tsposix には触れない。
#

PSX_DIR    := $(TOP)/lib/libposix
PSX_OUT    := $(UOBJ)/psx
PSX_LIB    := $(PSX_OUT)/libposix.a
PSX_CRT    := $(PSX_OUT)/psx_crt.o

# コマンドの翻訳の旗: 浮動小数を使い(-mgeneral-regs-only を外す)、include/posix を newlib より先に読む
PSX_INC    := -I$(TOP)/include/posix
PSX_CFLAGS := $(ARCHFLAGS) $(UARCH_FP) -fno-stack-protector -O2 -g -std=gnu11 \
	      -ffunction-sections -fdata-sections -D_GNU_SOURCE -D_POSIX_TIMERS=1 \
	      -D_POSIX_MONOTONIC_CLOCK=200112L $(PSX_INC)
PSX_LDFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles -static -T $(USER_LD) -Wl,--build-id=none \
	       -Wl,--gc-sections
PSX_LIBS   := -Wl,--start-group $(PSX_LIB) -lm -lc -lgcc -Wl,--end-group

# libposix 自身は系の頭書き(include/ts)も読む。tk/typedef.h の W・UW を 32 ビットにする(config.h と同じ)
PSX_KCFLAGS := $(PSX_CFLAGS) -Wall -Wno-unused-parameter -DCHK_TKERNEL_CONST -DUSE_STDINC_STDINT=1 -include stdint.h \
	       -I$(TOP)/include -I$(TOP)/config

PSX_SRCS   := $(wildcard $(PSX_DIR)/*.c)
PSX_OBJS   := $(patsubst $(PSX_DIR)/%.c,$(PSX_OUT)/%.o,$(PSX_SRCS)) \
	      $(PSX_OUT)/psx_stub.o $(PSX_OUT)/svc_stub.o $(PSX_OUT)/ts_random.o
PSX_HDRS   := $(wildcard $(TOP)/include/posix/*.h) $(wildcard $(TOP)/include/posix/sys/*.h) \
	      $(wildcard $(TOP)/include/posix/machine/*.h) $(PSX_DIR)/psx_int.h \
	      $(TOP)/include/ts/psx.h $(TOP)/include/ts/psxid.h $(TOP)/include/ts/fs.h $(TOP)/include/ts/svc.h
# アーキテクチャの置き場の C(x86_64/ だけにある)
ifneq ($(UARCH_DIR),)
PSX_ASRCS  := $(wildcard $(PSX_DIR)/$(UARCH_DIR)*.c)
PSX_OBJS   += $(patsubst $(PSX_DIR)/$(UARCH_DIR)%.c,$(PSX_OUT)/%.o,$(PSX_ASRCS))
# newlib の stdio が呼ぶ状態の問合せは libposix のもの(psx_nlstat.c)を先に結ぶ
PSX_LDFLAGS += -Wl,-u,_fstat_r -Wl,-u,_stat_r

$(PSX_OUT)/%.o: $(PSX_DIR)/$(UARCH_DIR)%.c $(PSX_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(PSX_KCFLAGS) -c -o $@ $<
endif

$(PSX_OUT)/%.o: $(PSX_DIR)/%.c $(PSX_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(PSX_KCFLAGS) -c -o $@ $<

$(PSX_OUT)/%.o: $(PSX_DIR)/$(UARCH_DIR)%.S $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(ARCHFLAGS) -I$(TOP)/include -c -o $@ $<

$(PSX_OUT)/svc_stub.o: $(TOP)/lib/libts/$(UARCH_DIR)svc_stub.S $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(ARCHFLAGS) -I$(TOP)/include -c -o $@ $<

$(PSX_OUT)/ts_random.o: $(TOP)/lib/libts/ts_random.c $(TOP)/include/ts/ob.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(PSX_KCFLAGS) -c -o $@ $<

$(PSX_LIB): $(PSX_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $(filter-out $(PSX_CRT),$^)

# 土台の試験のプログラム(tests/uprog/psxtest.c): /boot に PSXTEST.ELF として載り、ktest_psx が起こす
PSXTEST_ELF := $(UOBJ)/psxtest.elf

$(PSXTEST_ELF): $(TOP)/tests/uprog/psxtest.c $(PSX_LIB) $(PSX_CRT) $(USER_LD)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(PSX_CFLAGS) -Wall $(PSX_LDFLAGS) -o $@ $(PSX_CRT) $< $(PSX_LIBS)

psx: $(PSX_LIB) $(PSXTEST_ELF)

ifeq ($(KTEST),1)
EXTRA_BOOT_FILES += $(PSXTEST_ELF)
endif

# 標準のコマンド(設計書 5.8.7)。make CMDS=1 のとき、tools/netbsd/cmds.sh が NetBSD 11.0 と
# OpenBSD 7.9(diff)のソースを取り寄せて(初めてのときだけ。sha512・sha256 を確かめる)、
# コマンドを一つずつのプログラムに作る(既定で ~/tessronos-netbsd-cmds)。それを
# tools/netbsd/mkcmdobj.py がプログラム実身にして、実身「bin」から仮身で結ぶ。mktsfs の
# --over で、その bin が etc/def のものに代わる
CMDS      ?= 0
CMD_OUT   ?= $(HOME)/tessronos-netbsd-cmds
CMD_STAGE := $(UOBJ)/cmdstage
CMD_DONE  := $(UOBJ)/cmdstage.done
CMD_BOX   := $(TOP)/etc/def/01a0d8c4-5460-7a71-8c82-9fab0c1d5460_0.xtad
CMD_ICO   := $(TOP)/etc/def/01a0d8c4-5462-7c93-8ea4-bc1d2e3f5462.ico
CMD_STAMP := $(UOBJ)/cmds.stamp
$(shell mkdir -p $(UOBJ); [ -f $(CMD_STAMP) ] && [ "$$(cat $(CMD_STAMP))" = "$(CMDS)" ] || echo "$(CMDS)" > $(CMD_STAMP))

ifeq ($(CMDS),1)
CMD_OVER  := --over $(CMD_STAGE)
$(CMD_DONE): $(PSX_LIB) $(PSX_CRT) $(USER_LD) $(TOP)/tools/netbsd/cmds.sh $(TOP)/tools/netbsd/mkcmdobj.py \
	     $(wildcard $(TOP)/tools/netbsd/port/*.c) $(PSX_HDRS) $(CMD_BOX) $(CMD_ICO)
	@echo 'CMDS $(CMD_OUT)'
	@OUT=$(CMD_OUT) PSX_LIB=$(PSX_LIB) PSX_CRT=$(PSX_CRT) USER_LD=$(USER_LD) bash $(TOP)/tools/netbsd/cmds.sh all
	@python3 -I $(TOP)/tools/netbsd/mkcmdobj.py --bin $(CMD_OUT)/bin --box $(CMD_BOX) --ico $(CMD_ICO) \
	  --out $(CMD_STAGE) --origin $(CMD_OUT)/origin.txt
	@touch $@
else
CMD_OVER  :=
CMD_DONE  :=
endif

$(OBJDIR)/sysvol.img: $(CMD_DONE) $(CMD_STAMP)

cmds: $(CMD_DONE)
