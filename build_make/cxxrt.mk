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
CXXRT_LD   := $(TOP)/etc/linker/user/program_cxx.ld
CXXRT_OUT  := obj/lib

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
PTH_OBJS   := $(CXXRT_OUT)/pth/pth_wait.o $(CXXRT_OUT)/pth/pth_sync.o $(CXXRT_OUT)/pth/pth_thread.o \
	      $(CXXRT_OUT)/pth/pth_stack.o $(CXXRT_OUT)/pth/pth_emutls.o

# random bytes from the object 乱数, shared with the C programs' lib/libts
$(CXXRT_OUT)/cxxrt/ts_random.o: $(TOP)/lib/libts/ts_random.c $(TOP)/include/ts/ob.h $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

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

$(CXXRT_OUT)/cxxrt/%.o: $(CXXRT_DIR)/%.S
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<

$(CXXRT_OUT)/cxxrt/svc_stub.o: $(TOP)/lib/libts/svc_stub.S $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'AS  $@'
	@$(CC) $(TSPOSIX_FLAGS) -c -o $@ $<

$(CXXRT_OUT)/pth/%.o: $(PTH_DIR)/%.c $(CXXRT_HDRS) $(APP_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(TSPOSIX_CFLAGS) -Wall -c -o $@ $<

$(CXXRT_OUT)/pth/%.o: $(PTH_DIR)/%.S
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
CRT_I     := $(shell $(CC) -print-file-name=crti.o)
CRT_BEGIN := $(shell $(CC) -print-file-name=crtbegin.o)
CRT_END   := $(shell $(CC) -print-file-name=crtend.o)
CRT_N     := $(shell $(CC) -print-file-name=crtn.o)

# $(call CXXRT_LINK,出力,オブジェクトとソース)
CXXRT_LINK = $(CXX) $(TSPOSIX_CXXFLAGS) -nostdlib -nostartfiles -static -T $(CXXRT_LD) \
	-Wl,--build-id=none -Wl,--gc-sections -o $(1) $(CRT_I) $(CRT_BEGIN) $(2) \
	-Wl,--whole-archive $(CXXRT_LIB) $(PTH_LIB) -Wl,--no-whole-archive \
	-Wl,--start-group -lstdc++ -lm -lc -lgcc -Wl,--end-group $(CRT_END) $(CRT_N)

# 試験が起こす C++ のプログラム(tests/uprog/cxxprog.cc)。/boot に CXXPROG.ELF として載り、
# ktest_cxx が走らせる
CXXPROG_ELF := obj/cxxprog.elf

$(CXXPROG_ELF): $(TOP)/tests/uprog/cxxprog.cc $(CXXRT_LIB) $(PTH_LIB) $(CXXRT_LD) $(CXXRT_HDRS)
	@mkdir -p $(dir $@)
	@echo 'CXX $@'
	@$(call CXXRT_LINK,$@,$(TSPOSIX_CXXFLAGS) $(TOP)/tests/uprog/cxxprog.cc)

EXTRA_BOOT_FILES += $(CXXPROG_ELF)

# 小物「ブラウザ」のプログラム実身のレコード1(設計書 17.19)。make BROWSER=1 のときは
# エンジンを結んだもの(browser.mk)、そうでないときは作り方を言うだけの小さなもの。
# メタデータとレコード0は etc/def にある
BROWSER_REC   := obj/rec/01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57_1.bin
BROWSER_STAMP := obj/browser.stamp
$(shell mkdir -p obj; [ -f $(BROWSER_STAMP) ] && [ "$$(cat $(BROWSER_STAMP))" = "$(BROWSER)$(BROWSER_BLINK)" ] || echo "$(BROWSER)$(BROWSER_BLINK)" > $(BROWSER_STAMP))
ifeq ($(BROWSER),1)
BROWSER_BIN := obj/browser.elf
else
BROWSER_BIN := obj/br_stub.elf
endif

obj/br_stub.elf: $(APP_HDRS) $(LIBTS_SRCS) $(TOP)/application/browser/br_stub.c $(USER_LD) $(TOP)/include/ts/svc.h
	@mkdir -p $(dir $@)
	@echo 'CC  $@'
	@$(CC) $(APP_CFLAGS) -o $@ $(LIBTS_SRCS) $(TOP)/application/browser/br_stub.c

$(BROWSER_REC): $(BROWSER_BIN) $(BROWSER_STAMP)
	@mkdir -p $(dir $@)
	@cp $< $@

EXTRA_RECS += $(BROWSER_REC)
