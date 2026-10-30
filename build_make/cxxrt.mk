#
# cxxrt.mk — C++ の実行時(lib/libcxxrt)とスレッド(lib/libpthread)、それで作るプログラム(設計書 17.19)
#
#   C++ のプログラムと、スレッドを使う C のプログラムは、lib/libcxxrt の入口(crt_cxx.S)から
#   始まり、etc/linker/user/program_cxx.ld で結ぶ。libstdc++ と newlib はツールチェーンのものを
#   使い、include/tscxx がそのヘッダにスレッドを入れ、include/tsposix が pthread.h・semaphore.h・
#   sys/mman.h を足す。二つのライブラリは丸ごと結ぶ(ライブラリの同じ名前の部品より先に取らせる)。
#

CXX     := $(CROSS)g++
AR      := $(CROSS)ar

CXXRT_DIR  := $(TOP)/lib/libcxxrt
PTH_DIR    := $(TOP)/lib/libpthread
CXXRT_LD   := $(TOP)/etc/linker/$(USER_LD_DIR)/program_cxx.ld
CXXRT_OUT  := $(UOBJ)/lib

# 利用者のプログラムの C と C++ に共通の旗。POSIX の時計と予定の宣言を newlib から出させる
TSPOSIX_FLAGS := $(ARCHFLAGS) -O2 -g -fno-stack-protector '-DCHK_TKERNEL_CONST=(1)' \
	-D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L -D_POSIX_PRIORITY_SCHEDULING=1 \
	-I$(TOP)/include/tscxx -I$(TOP)/include/tsposix -I$(TOP)/include -I$(TOP)/config
TSPOSIX_CFLAGS   := $(TSPOSIX_FLAGS) -std=gnu11
TSPOSIX_CXXFLAGS := $(TSPOSIX_FLAGS) -std=gnu++20

CXXRT_LIB  := $(CXXRT_OUT)/libcxxrt.a
PTH_LIB    := $(CXXRT_OUT)/libpthread.a
CXXRT_HDRS := $(wildcard $(TOP)/include/tsposix/*.h) $(wildcard $(TOP)/include/tsposix/sys/*.h) \
	      $(wildcard $(TOP)/include/tscxx/bits/*.h) $(TOP)/include/ts/umem.h $(PTH_DIR)/pth_int.h

CXXRT_OBJS := $(CXXRT_OUT)/cxxrt/crt_cxx.o $(CXXRT_OUT)/cxxrt/ts_start.o $(CXXRT_OUT)/cxxrt/ts_syscalls.o \
	      $(CXXRT_OUT)/cxxrt/ts_locks.o $(CXXRT_OUT)/cxxrt/ts_mman.o $(CXXRT_OUT)/cxxrt/cxx_thread.o \
	      $(CXXRT_OUT)/cxxrt/cxx_abi.o $(CXXRT_OUT)/cxxrt/svc_stub.o $(CXXRT_OUT)/cxxrt/ts_socket.o \
	      $(CXXRT_OUT)/cxxrt/ts_dirent.o $(CXXRT_OUT)/cxxrt/ts_face.o $(CXXRT_OUT)/cxxrt/ts_posix.o $(CXXRT_OUT)/cxxrt/ts_evfd.o \
	      $(CXXRT_OUT)/cxxrt/ts_random.o $(CXXRT_OUT)/cxxrt/ts_urandom.o
ifneq ($(UARCH_LIBC),)
CXXRT_OBJS += $(CXXRT_OUT)/cxxrt/sysnames.o

$(CXXRT_OUT)/cxxrt/sysnames.o: $(UARCH_LIBC)
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<
endif
PTH_OBJS   := $(CXXRT_OUT)/pth/pth_wait.o $(CXXRT_OUT)/pth/pth_sync.o $(CXXRT_OUT)/pth/pth_thread.o \
	      $(CXXRT_OUT)/pth/pth_stack.o $(CXXRT_OUT)/pth/pth_emutls.o

# random bytes from the object 乱数, shared with the C programs' lib/libts
$(CXXRT_OUT)/cxxrt/ts_random.o: $(TOP)/lib/libts/ts_random.c $(TOP)/include/ts/ob.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

# The system's C libraries a C++ program takes what it needs from (the browser): a program's
# panels from a data box (lib/libui, include/ts/ui.h), the object of pages it prints
# (lib/libui/ui_prn.c, include/ts/uiprn.h), and real objects given out as files and
# files taken in as real objects (lib/libxfu, include/ts/xfu.h, with the text encodings of
# lib/libtxc, the TRON code of lib/libbpk and lib/libjson). An archive of its own, not in
# libcxxrt.a, which every C++ program takes whole: a C program linked as one (基本表計算) has
# its own copy of lib/libui
UI_CXX_LIB  := $(CXXRT_OUT)/libtsui.a
UI_CXX_SRCS := $(TOP)/lib/libui/ui.c $(TOP)/lib/libui/ui_prn.c \
	       $(addprefix $(TOP)/lib/libxfu/,xfu_util.c xfu_name.c xfu_obj.c xfu_imp.c xfu_exp.c xfu_prc.c) \
	       $(TOP)/lib/libtxc/txc.c $(TOP)/lib/libtxc/txc_tab.c $(TOP)/lib/libjson/json.c \
	       $(TOP)/lib/libbpk/bpk_tron.c $(TOP)/lib/libbpk/bpk_jis.c
UI_CXX_OBJS := $(patsubst $(TOP)/lib/%.c,$(CXXRT_OUT)/tsui/%.o,$(UI_CXX_SRCS))

# the system's error numbers without their lower part, and TRUE as tk/typedef.h has it (as
# ファイル変換 compiles lib/libxfu)
$(CXXRT_OUT)/tsui/%.o: $(TOP)/lib/%.c $(TOP)/include/ts/ui.h $(TOP)/include/ts/uiprn.h $(TOP)/include/ts/part.h $(TOP)/include/ts/xfu.h \
		       $(TOP)/lib/libxfu/xfu_in.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation \
		-DTK_SUPPORT_SERCD=0 -DTRUE=1 -c -o $@ $<

$(UI_CXX_LIB): $(UI_CXX_OBJS)
	@echo 'AR  $@'
	@rm -f $@
	@$(AR) rcs $@ $^

# the faces as objects (include/ts/face.h), shared with the C programs' lib/libts
$(CXXRT_OUT)/cxxrt/ts_face.o: $(TOP)/lib/libts/ts_face.c $(TOP)/include/ts/face.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

$(CXXRT_OUT)/cxxrt/%.o: $(CXXRT_DIR)/%.c $(CXXRT_HDRS) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

$(CXXRT_OUT)/cxxrt/%.o: $(CXXRT_DIR)/%.cc $(CXXRT_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CXX $@'
	@$(CXX) $(TSPOSIX_CXXFLAGS) -Wall -c -o $@ $<

$(CXXRT_OUT)/cxxrt/%.o: $(CXXRT_DIR)/$(UARCH_DIR)%.S
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<

$(CXXRT_OUT)/cxxrt/svc_stub.o: $(TOP)/lib/libts/$(UARCH_DIR)svc_stub.S $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<

$(CXXRT_OUT)/pth/%.o: $(PTH_DIR)/%.c $(CXXRT_HDRS) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

$(CXXRT_OUT)/pth/%.o: $(PTH_DIR)/$(UARCH_DIR)%.S
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<

$(CXXRT_LIB): $(CXXRT_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^

$(PTH_LIB): $(PTH_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^

# 結び方: crti・crtbegin、プログラム、二つのライブラリ(丸ごと)、libstdc++・libm・libc・libgcc、crtend・crtn
# (x86_64-elf のツールチェーンには crti と crtn が無く、lib/libcxxrt/x86_64 のものを使う)
ifeq ($(ARCH),x86_64)
CRT_I     := $(CXXRT_OUT)/cxxrt/crti.o
CRT_N     := $(CXXRT_OUT)/cxxrt/crtn.o
$(CXXRT_LIB): | $(CRT_I) $(CRT_N)
else
CRT_I     := $(shell $(CC) -print-file-name=crti.o)
CRT_N     := $(shell $(CC) -print-file-name=crtn.o)
endif
CRT_BEGIN := $(shell $(CC) -print-file-name=crtbegin.o)
CRT_END   := $(shell $(CC) -print-file-name=crtend.o)

# $(call CXXRT_LINK,出力,オブジェクトとソース)
CXXRT_LINK = $(CXX) $(TSPOSIX_CXXFLAGS) -nostdlib -nostartfiles -static -T $(CXXRT_LD) \
	-Wl,--build-id=none -Wl,--gc-sections -o $(1) $(CRT_I) $(CRT_BEGIN) $(2) \
	-Wl,--whole-archive $(CXXRT_LIB) $(PTH_LIB) -Wl,--no-whole-archive \
	-Wl,--start-group -lstdc++ -lm -lc -lgcc -Wl,--end-group $(CRT_END) $(CRT_N)

# 試験が起こす C++ のプログラム(tests/uprog/cxxprog.cc)。/boot に CXXPROG.ELF として載り、
# ktest_cxx が走らせる
CXXPROG_ELF := $(UOBJ)/cxxprog.elf

$(CXXPROG_ELF): $(TOP)/tests/uprog/cxxprog.cc $(CXXRT_LIB) $(PTH_LIB) $(CXXRT_LD) $(CXXRT_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CXX $@'
	@$(call CXXRT_LINK,$@,$(TSPOSIX_CXXFLAGS) $(TOP)/tests/uprog/cxxprog.cc)

EXTRA_BOOT_FILES += $(CXXPROG_ELF)

# 小物「ブラウザ」のプログラム実身のレコード1(設計書 17.19)。make BROWSER=1 のときは
# エンジンを結んだもの(browser.mk)、そうでないときは作り方を言うだけの小さなもの。
# メタデータとレコード0は etc/def にある
BROWSER_REC   := $(UOBJ)/rec/01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57_1.bin
BROWSER_STAMP := $(UOBJ)/browser.stamp
$(shell mkdir -p $(UOBJ); [ -f $(BROWSER_STAMP) ] && [ "$$(cat $(BROWSER_STAMP))" = "$(BROWSER)$(BROWSER_BLINK)" ] || echo "$(BROWSER)$(BROWSER_BLINK)" > $(BROWSER_STAMP))
# エンジンを結んだプログラム(browser.mk、v8app)の置き場と、tessron-chromium のスクリプトに渡す
# アーキテクチャ。x86-64 では命令セットの違うものを混ぜないよう $(UOBJ) に置き、TS_ARCH=x86_64 で
# x86-64 の目的物(~/tessronos-obj-x64)とツールチェーンを使わせる(設計書 14.3)
ifeq ($(ARCH),x86_64)
BROWSER_OUT      := $(UOBJ)
BROWSER_ARCH_ENV := TS_ARCH=x86_64
else
BROWSER_OUT      := obj
BROWSER_ARCH_ENV :=
endif
ifeq ($(BROWSER),1)
BROWSER_BIN := $(BROWSER_OUT)/browser.elf
else
BROWSER_BIN := $(UOBJ)/br_stub.elf
endif

$(UOBJ)/br_stub.elf: $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/application/browser/br_stub.c $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/application/browser/br_stub.c

$(BROWSER_REC): $(BROWSER_BIN) $(BROWSER_STAMP)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(BROWSER_REC)

# レコード2: Blink の版の音声の合成と認識の辞書・声・モデル(tools/browser/speech.sh が作り、
# build.sh がプログラムの横に置く)。プログラムに入れず、話すときにだけ読む(設計書 14.4.15)
ifeq ($(BROWSER)$(BROWSER_BLINK),11)
BROWSER_SPEECH_REC := $(UOBJ)/rec/01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57_2.bin
$(BROWSER_SPEECH_REC): $(BROWSER_BIN)
	@mkdir -p $(dir $@)
	@cp $(BROWSER_OUT)/browser_speech.rec $@

EXTRA_RECS += $(BROWSER_SPEECH_REC)
else
# 前の Blink のビルドが共有の obj/rec に残した音声のレコードは、このビルドのブラウザには無い
SYS_VOL_EXCLUDE += 01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57_2.bin
endif

# OpenGL ES(第13章 13.6)。make GL=1 のとき、tools/mesa/build.sh が Mesa(EGL、OpenGL ES、
# Gallium と softpipe・v3d)と移植層(lib/libgl)を GL_DIR(既定 ~/tsmesa)に一つの静的
# ライブラリ libtsgl.a として建て、それを結んだ試験のプログラム(tests/uprog/glprog.c、
# /boot の GLPROG.ELF)と小物「歯車」(application/glgears)を作る。Mesa のソースは
# リポジトリに入れず、build.sh が取得して写しにパッチを当てる。ライブラリは移植層・
# パッチ・スクリプトが変わったときだけ作り直す(build.sh は変わったものだけをコンパイルする)
# x86-64 では tools/x64/mesa/build.sh が x86_64-elf 用のライブラリを ~/tsmesa-x64 に建てる(softpipe と iris)
ifeq ($(ARCH),x86_64)
GL_DIR    ?= $(HOME)/tsmesa-x64
GL_BUILD  := $(TOP)/tools/x64/mesa/build.sh
else
GL_DIR    ?= $(HOME)/tsmesa
GL_BUILD  := $(TOP)/tools/mesa/build.sh
endif
GL_LIB    := $(GL_DIR)/libtsgl.a
GL_CFLAGS := $(TSPOSIX_CFLAGS) -I$(GL_DIR)/include -DEGL_NO_PLATFORM_SPECIFIC_TYPES -Wall
GL_DEPS   := $(wildcard $(GL_BUILD) $(TOP)/tools/mesa/translate.py \
		$(TOP)/tools/mesa/patches/*.patch $(TOP)/tools/mesa/shim/*.h $(TOP)/tools/mesa/shim/*/*.h \
		$(TOP)/lib/libgl/*.c) $(shell find $(TOP)/tools/mesa/src -type f 2>/dev/null) \
		$(TOP)/include/ts/gpuapp.h $(TOP)/include/ts/gpu.h $(TOP)/include/ts/glapp.h

# $(call GL_LINK,出力,オブジェクト): C++ のプログラムと同じ結び方に、libtsgl.a を加える
GL_GROUP := -Wl,--start-group $(GL_LIB) -Wl,--end-group
GL_LINK = $(call CXXRT_LINK,$(1),$(2) $(GL_GROUP))

# 小物「歯車」のプログラム実身のレコード1。make GL=1 のときは OpenGL ES を結んだもの、
# そうでないときは作り方を言うだけの小さなもの。メタデータとレコード0は etc/def にある
GLGEARS_REC   := $(UOBJ)/rec/01a0d8c4-52e0-7a41-9b52-c6d7e8f9a0b1_1.bin
GL_STAMP      := $(UOBJ)/gl.stamp
$(shell mkdir -p $(UOBJ); [ -f $(GL_STAMP) ] && [ "$$(cat $(GL_STAMP))" = "$(GL)" ] || echo "$(GL)" > $(GL_STAMP))
ifeq ($(GL),1)
GLGEARS_BIN := $(UOBJ)/glgears.elf
GLPROG_ELF  := $(UOBJ)/glprog.elf
# OpenGL ES を結んだ歯車のレコードは約 19MB あり、256MB の試験用ディスクの FAT の区画に
# 収まらないので、試験用ディスクを大きくする
ifneq ($(BROWSER_BLINK),1)
TEST_DISK_MB := 512
endif

$(GL_LIB): $(GL_DEPS) $(CXXRT_LIB) $(PTH_LIB)
	@echo 'GL  $@'
	@bash $(GL_BUILD) all

$(UOBJ)/gl/%.o: $(TOP)/application/glgears/%.c $(GL_LIB) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(GL_CFLAGS) -c -o $@ $<

$(UOBJ)/gl/glprog.o: $(TOP)/tests/uprog/glprog.c $(GL_LIB) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(GL_CFLAGS) -c -o $@ $<

$(GLGEARS_BIN): $(UOBJ)/gl/glgears.o $(GL_LIB) $(CXXRT_LIB) $(PTH_LIB) $(CXXRT_LD)
	@echo 'LD  $@'
	@$(call GL_LINK,$@,$<)

$(GLPROG_ELF): $(UOBJ)/gl/glprog.o $(GL_LIB) $(CXXRT_LIB) $(PTH_LIB) $(CXXRT_LD)
	@echo 'LD  $@'
	@$(call GL_LINK,$@,$<)

EXTRA_BOOT_FILES += $(GLPROG_ELF)
else
GLGEARS_BIN := $(UOBJ)/gl_stub.elf
endif

$(UOBJ)/gl_stub.elf: $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/application/glgears/gl_stub.c $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/application/glgears/gl_stub.c

$(GLGEARS_REC): $(GLGEARS_BIN) $(GL_STAMP)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(GLGEARS_REC)
