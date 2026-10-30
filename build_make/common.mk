#
# common.mk — 共通のコンパイルオプションとパターンルール(設計書 13.3)
#

TOP     := ..
OBJDIR  := obj/$(TARGET)$(if $(filter 1,$(KTEST)),_ktest)$(BUILD_ID)
INCPATH := -I$(TOP)/include -I$(TOP)/config -I$(TOP)/kernel/knlinc \
	-I$(TOP)/kernel -I$(TOP)/peripheral_kernel \
	-I$(TOP)/kernel/sysdepend/cpu/core/$(ARCH_CORE) -I$(OBJDIR) \
	-I$(TOP)/peripheral_kernel/network/lwip/src/include \
	-I$(TOP)/peripheral_kernel/libc/include \
	-I$(TOP)/peripheral_kernel/network/port/include \
	-I$(TOP)/outer_kernel/font/port \
	-I$(TOP)/outer_kernel/font/freetype/include

# 命令セットの基準(ARCH=aarch64)。Raspberry Pi 4 系の Cortex-A72 は ARMv8.0-A で、CRC32 の命令はあるが、
# LSE のアトミック命令(ldadd、cas など)も暗号拡張(AES、SHA)も持たない。そのターゲットだけ
# ARMv8.0-A に CRC32 を足したもので作り、ほかは ARMv8.2-A のまま(docs/tessronos/03-platform.md 3.6)
# アトミック操作は、ARMv8.0-A では関数(libgcc の __aarch64_cas4_acq_rel など)を呼ぶ形が
# 既定になる。ライブラリを結ばない小物もあるので、排他ロードとストアの命令で直に作らせる
ifeq ($(ARCH),aarch64)
ifeq ($(TARGET),_RPI4_)
ARCH_MARCH := armv8-a+crc
ARCHFLAGS := -march=$(ARCH_MARCH) -mno-outline-atomics
else
ARCH_MARCH := armv8.2-a
ARCHFLAGS := -march=$(ARCH_MARCH)
endif
# カーネルは浮動小数点・SIMD のレジスタを使わず(-mgeneral-regs-only)、整列の検査に
# 頼らない(-mstrict-align)。アトミック操作は関数を呼ばず、命令で直に作らせる
ARCH_KFLAGS  := -mgeneral-regs-only -mstrict-align
ARCH_KFLAGS2 := -mno-outline-atomics
endif
# x86_64: 命令セットの基準は x86-64-v2(SSE4.2、POPCNT。AVX2 は実行時に選ぶ)。カーネルは
# 最上位の 2GB に置き(-mcmodel=kernel)、割込みが利用者のでないスタックの赤い領域を
# 壊さないよう -mno-red-zone、浮動小数点・SIMD のレジスタは使わない(-mgeneral-regs-only)
# (docs/private/x64_surface_go2_study.md 2.11)
ifeq ($(ARCH),x86_64)
ARCH_MARCH := x86-64-v2
ARCHFLAGS := -march=$(ARCH_MARCH)
ARCH_KFLAGS  := -mgeneral-regs-only -mno-red-zone -mcmodel=kernel
ARCH_KFLAGS2 :=
endif

# 利用者のプログラムの旗とアーキテクチャ別の置き場。UARCH_NOFP は浮動小数点を使わない小物の、
# UARCH_FP は使うプログラムの旗。UARCH_DIR はアセンブラのソース(crt0.S など)のアーキテクチャ別の
# ディレクトリ(AArch64 は元の場所)、USER_LD_DIR はリンカスクリプトの置き場(etc/linker/<>)
ifeq ($(ARCH),x86_64)
UARCH_NOFP  := -mgeneral-regs-only
UARCH_FP    :=
UARCH_DIR   := x86_64/
USER_LD_DIR := x64
# newlib の x86_64-elf 版が系を呼ぶ名前を、プログラムの下回りの名前(_write など)へ渡す
UARCH_LIBC  := $(TOP)/lib/libts/x86_64/sysnames.S
else
UARCH_NOFP  := -mgeneral-regs-only -mstrict-align
UARCH_FP    := -mstrict-align
UARCH_DIR   :=
USER_LD_DIR := user
UARCH_LIBC  :=
endif

# 利用者のプログラムと実身のレコード、それらが結ぶライブラリの置き場。命令セットの基準が
# 違うターゲットのものは混ぜない
ifeq ($(TARGET),_RPI4_)
UOBJ := obj/v80
else ifeq ($(ARCH),x86_64)
UOBJ := obj/x64
else
UOBJ := obj
endif

# カーネルのタスクのスタックは小さく、下にガードページがないので、4096 バイトを超える
# フレームは警告にする(設計書 4.7.7)
CFLAGS_COMMON := $(ARCHFLAGS) $(ARCH_KFLAGS) \
	-ffreestanding -fno-builtin -fno-common -fno-pic -fno-stack-protector \
	-fno-omit-frame-pointer $(ARCH_KFLAGS2) \
	-std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-Werror=implicit-function-declaration -Werror=int-conversion \
	-Wframe-larger-than=4096 \
	-MMD -MP

# FreeType の構成。何を組み込むか(port/ftmodule.h)と、どの選択肢を落とすか
# (port/ftoption.h)。ライブラリ側と使う側で同じでなければ構造体の形が食い違う
# ので、全体に効かせる(設計書 16.6)
CFLAGS_COMMON += -DFT_CONFIG_OPTIONS_H='<ftoption.h>' \
	-DFT_CONFIG_MODULES_H='<ftmodule.h>'

ASFLAGS_COMMON := $(ARCHFLAGS) -x assembler-with-cpp -g3 -MMD -MP

LDFLAGS_COMMON := $(ARCHFLAGS) -nostdlib -nostartfiles -static \
	-Wl,--gc-sections -Wl,--build-id=none

ifeq ($(KTEST),1)
  CFLAGS_COMMON += -DUSE_KTEST=1
  # KTGROUP=kernel|fs|gui|net で一族だけ走らせる(既定は全部)
  ifneq ($(KTONLY),)
    CFLAGS_COMMON += -DKT_ONE='"$(KTONLY)"'
  endif
  # KT_RUMP_S=<秒> で、rump の群の負荷を掛ける時間を変える(既定は 20 秒)
  ifneq ($(KT_RUMP_S),)
    CFLAGS_COMMON += -DKT_RUMP_S=$(KT_RUMP_S)
  endif
  # KT_CHURN_N=<回数> で、プロセスやタスクを作っては終える試験の回数を変える(既定は 1000 回)
  ifneq ($(KT_CHURN_N),)
    CFLAGS_COMMON += -DKT_CHURN_N=$(KT_CHURN_N)
  endif
  ifneq ($(KTGROUP),)
    ifneq ($(KTGROUP),all)
      CFLAGS_COMMON += -DKT_ONLY_$(shell echo $(KTGROUP) | tr a-z A-Z)=1
    endif
  endif
endif

# make BROWSER=1 BROWSER_BLINK=1 で、ブラウザは Blink を結んだ版になる(設計書 17.19)。
# 試験はそれに合わせて Blink の頁の試験を走らせる
ifeq ($(BROWSER_BLINK),1)
  CFLAGS_COMMON += -DKT_BROWSER_BLINK=1
endif

# make KLOCKSTAT=1 で、カーネルのロックの取得・競合・待ち・保持を取った所ごとに
# 数える(設計書 8.6.2、include/ts/klstat.h)。試験は組ごとに数を出す
ifeq ($(KLOCKSTAT),1)
  CFLAGS_COMMON += -DCNF_KLOCK_STAT=1
endif

# make KLOCK=<0〜3> で、カーネルの排他の方式を替える(docs/tessronos/04-kernel.md 4.3.3)。
# 0 は大きなカーネルロック(Arm の既定)、3 はスケジューラのロックと物ごとのロック(x64 の既定)、
# 1 と 2 はその間の区切り。make KLOCKCHECK=1 で、ロックの順番の検査を試験のビルドでなくても入れる
ifneq ($(KLOCK),)
  CFLAGS_COMMON += -DCNF_KLOCK_MODE=$(KLOCK)
  ASFLAGS_COMMON += -DCNF_KLOCK_MODE=$(KLOCK)
endif
ifeq ($(KLOCKCHECK),1)
  CFLAGS_COMMON += -DCNF_KLOCK_CHECK=1
endif

# make STKWATCH=1 で、走っていないタスクのスタックの戻り番地が書き換えられたら、
# ディスパッチャがそのタスクを走らせる前に見つけて止まる(設計書 4.7.6)
ifeq ($(STKWATCH),1)
  CFLAGS_COMMON += -DCNF_STK_WATCH=1
  ASFLAGS_COMMON += -DCNF_STK_WATCH=1
endif

# 試験のビルドでは、タスクのスタックを決まった値で埋め、どこまで使ったかを測る。UITEST の
# stkuse で、タスクごとの最も深い使用量をコンソールに出す(設計書 4.7.7)
ifneq ($(filter 1,$(KTEST) $(UITEST)),)
  CFLAGS_COMMON += -DCNF_STK_USAGE=1
endif

# make DESKTOP=1 で、試験ではなくデスクトップが立ち上がる版を作る
ifeq ($(DESKTOP),1)
  CFLAGS_COMMON += -DUSE_DESKTOP=1
  # make DESKTOP=1 UITEST=1: 二つ目のシリアルから画面を操作できる(tools/ui_run.py)
  ifeq ($(UITEST),1)
    CFLAGS_COMMON += -DUSE_UITEST=1 -DOB_DENY_LOG=1
  endif
endif

# make OACVLOG=1 で、プロセスの呼出しが権限の不足(E_OACV)で断られるたびに、
# どのプロセスがどの実身に何を求めたかをコンソールに「!OACV!」で出す(設計書 8.7.4)。
# UITEST=1 では常に出す
ifeq ($(OACVLOG),1)
  CFLAGS_COMMON += -DOB_DENY_LOG=1
endif

# make NETSTACK=netbsd(既定)で、TCP/IP スタックは NetBSD のもの(rump カーネルとして
# tools/netbsd/build.sh が作る一つのオブジェクト)。NETSTACK=lwip で以前の lwIP に戻す
# (設計書 12.6)。オブジェクトが無ければ、リンクの前に tools/netbsd/build.sh が作る
NETSTACK ?= netbsd
# 命令セットの基準ごとに別の置き場に作る(Raspberry Pi 4 系は ARMv8.0-A の版)。
# x64 は tools/x64/netbsd/build.sh が作る x86_64 の版
ifeq ($(TARGET),_RPI4_)
NETBSD_OBJ ?= $(HOME)/tessronos-netbsd-v80/rumpnet.o
else ifeq ($(ARCH),x86_64)
NETBSD_OBJ ?= $(HOME)/tessronos-netbsd-x64/rumpnet.o
else
NETBSD_OBJ ?= $(HOME)/tessronos-netbsd/rumpnet.o
endif
ifeq ($(NETSTACK),netbsd)
  CFLAGS_COMMON += -DTS_NETSTACK_NETBSD=1
  EXTRA_OBJS += $(NETBSD_OBJ)
  # make RUTRACE=1: 待ちが数秒続いたロックを、待たせた場所と一緒に出す(rumpuser.c)
  ifeq ($(RUTRACE),1)
    CFLAGS_COMMON += -DRU_TRACE=1
  endif
else ifneq ($(NETSTACK),lwip)
  $(error unknown NETSTACK "$(NETSTACK)" (use netbsd or lwip))
endif

# make MOZC=1 で、かな漢字変換器 mozc(tools/mozc/build.sh で作る一つのオブジェクト)
# を組み込む。無ければ application/kconv のローマ字かな変換だけで入力する
# Raspberry Pi 4 系は ARMv8.0-A で作ったもの(OUT=~/tessronos-mozc-v80 MARCH=armv8-a+crc)
ifeq ($(TARGET),_RPI4_)
MOZC_OBJ ?= $(HOME)/tessronos-mozc-v80/tsmozc.o
else ifeq ($(ARCH),x86_64)
# x64 は tools/x64/mozc/build.sh が作る x86_64 の版
MOZC_OBJ ?= $(HOME)/tessronos-mozc-x64/tsmozc.o
else
MOZC_OBJ ?= $(HOME)/tessronos-mozc/tsmozc.o
endif
ifeq ($(MOZC),1)
  CFLAGS_COMMON += -DUSE_MOZC=1
  EXTRA_OBJS += $(MOZC_OBJ)
endif

ifeq ($(DEBUG),1)
  CFLAGS_COMMON += -O0 -g3 -DCHK_LOCKORDER=1
else
  CFLAGS_COMMON += -O2 -g3
endif

# パターンルール: ソースは TOP からの相対パス、オブジェクトは obj/<TARGET>/ 以下に同じ階層で置く
$(OBJDIR)/%.o: $(TOP)/%.c
	@mkdir -p $(dir $@)
	@echo 'CC  $<'
	@$(CC) $(CFLAGS) $(INCPATH) -c -o $@ $<

$(OBJDIR)/%.o: $(TOP)/%.S
	@mkdir -p $(dir $@)
	@echo 'AS  $<'
	@$(AS) $(ASFLAGS) $(INCPATH) -c -o $@ $<
