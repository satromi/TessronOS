#
# toolchain.mk — ツールチェーンの探索
#
#   優先順位: TS_TOOLCHAIN(bin ディレクトリ) > PATH > ~/.local/xPacks(tools/setup_wsl.sh の既定)
#

XPACK_PREFIX ?= $(HOME)/.local/xPacks

ifneq ($(TS_TOOLCHAIN),)
  CROSS := $(TS_TOOLCHAIN)/aarch64-none-elf-
else ifneq ($(shell command -v aarch64-none-elf-gcc 2>/dev/null),)
  CROSS := aarch64-none-elf-
else ifneq ($(wildcard $(XPACK_PREFIX)/aarch64-none-elf-gcc/bin/aarch64-none-elf-gcc),)
  CROSS := $(XPACK_PREFIX)/aarch64-none-elf-gcc/bin/aarch64-none-elf-
else
  $(error aarch64-none-elf-gcc not found: run tools/setup_wsl.sh and "source tools/env.sh")
endif

CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
GDB     := $(CROSS)gdb

ifneq ($(shell command -v qemu-system-aarch64 2>/dev/null),)
  QEMU ?= qemu-system-aarch64
else
  QEMU ?= $(XPACK_PREFIX)/qemu-arm/bin/qemu-system-aarch64
endif
