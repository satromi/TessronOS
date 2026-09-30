#
# common.mk — 共通のコンパイルオプションとパターンルール(設計書 13.3)
#

TOP     := ..
OBJDIR  := obj/$(TARGET)$(if $(filter 1,$(KTEST)),_ktest)$(BUILD_ID)
INCPATH := -I$(TOP)/include -I$(TOP)/config -I$(TOP)/kernel/knlinc \
	-I$(TOP)/kernel -I$(TOP)/peripheral_kernel \
	-I$(TOP)/kernel/sysdepend/cpu/core/armv8a -I$(OBJDIR) \
	-I$(TOP)/peripheral_kernel/network/lwip/src/include \
	-I$(TOP)/peripheral_kernel/libc/include \
	-I$(TOP)/peripheral_kernel/network/port/include \
	-I$(TOP)/outer_kernel/font/port \
	-I$(TOP)/outer_kernel/font/freetype/include

ARCHFLAGS := -march=armv8.2-a

CFLAGS_COMMON := $(ARCHFLAGS) -mgeneral-regs-only -mstrict-align \
	-ffreestanding -fno-builtin -fno-common -fno-pic -fno-stack-protector \
	-fno-omit-frame-pointer -mno-outline-atomics \
	-std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-Werror=implicit-function-declaration -Werror=int-conversion \
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

# make DESKTOP=1 で、試験ではなくデスクトップが立ち上がる版を作る
ifeq ($(DESKTOP),1)
  CFLAGS_COMMON += -DUSE_DESKTOP=1
  # make DESKTOP=1 UITEST=1: 二つ目のシリアルから画面を操作できる(tools/ui_run.py)
  ifeq ($(UITEST),1)
    CFLAGS_COMMON += -DUSE_UITEST=1
  endif
endif

# make NETSTACK=netbsd(既定)で、TCP/IP スタックは NetBSD のもの(rump カーネルとして
# tools/netbsd/build.sh が作る一つのオブジェクト)。NETSTACK=lwip で以前の lwIP に戻す
# (設計書 12.6)。オブジェクトが無ければ、リンクの前に tools/netbsd/build.sh が作る
NETSTACK ?= netbsd
NETBSD_OBJ ?= $(HOME)/tessronos-netbsd/rumpnet.o
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
MOZC_OBJ ?= $(HOME)/tessronos-mozc/tsmozc.o
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
