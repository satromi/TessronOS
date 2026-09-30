#!/bin/bash
#
# build.sh -- the kana-kanji conversion engine (mozc) for TessronOS
#
# Copyright (C) 2026 satromi
# This software is distributed under the T-License 2.2.
#
# Builds mozc's engine, the libraries it stands on (abseil, protobuf, zlib)
# and the adapter that speaks the BTRON conversion-server protocol
# (mozc/src/btron: MozcKserver, the TRON code codec) with the bare aarch64
# toolchain -- newlib and a single-threaded libstdc++ -- and links all of it,
# with the C and C++ runtimes it needs, into one relocatable object:
#
#	$OUT/tsmozc.o
#
# The object exports only the engine's C interface (tsmozc_*, tools/mozc/
# tsmozc.h) and the start and end of its constructor table; every other
# symbol is made local, so its copy of the C library cannot collide with
# the kernel's. What it needs from outside is the handful of system calls
# newlib is built on (_sbrk, _write, _open ...), which the desktop's glue
# supplies (application/kconv).
#
# The generated sources (*.pb.cc, the embedded dictionary) come from the
# host build of mozc/src/btron, and the lists of what to compile from
# $LISTS (mozc_src2.txt, gen_src.txt and the protobuf object names):
#	cd ~/mozc/src && bazelisk build --config oss_linux //btron:mozc_kserver
#
# Usage: build.sh [gen|absl|pb|zlib|mozc|adapter|tf|link|all]
#
set -e

STAGE=${1:-all}
TFP=$(cd "$(dirname "$0")" && pwd)
XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
CC=$XB/aarch64-none-elf-gcc
CXX=$XB/aarch64-none-elf-g++
LD=$XB/aarch64-none-elf-ld
OBJCOPY=$XB/aarch64-none-elf-objcopy
NM=$XB/aarch64-none-elf-nm
READELF=$XB/aarch64-none-elf-readelf
SRC=${SRC:-$HOME/mozc/src}
BTRON=$SRC/btron
PORTB=$BTRON/port
BIN=$(readlink -f "$SRC/bazel-bin")
EXT=$(readlink -f "$SRC/bazel-src/../..")/external
PB=$EXT/protobuf+
ABSL=$EXT/abseil-cpp+
ZLIB=$EXT/zlib+
UTF8=$PB/third_party/utf8_range
LISTS=${LISTS:-$HOME/mozc-btron-thirdparty}
OUT=${OUT:-$HOME/tessronos-mozc}
GEN=$OUT/gen
JOBS=${JOBS:-$(nproc)}
mkdir -p "$OUT" "$GEN"

BASE="-march=armv8.2-a -O2 -mno-outline-atomics -fno-pic -fno-stack-protector \
 -ffunction-sections -fdata-sections -funsigned-char \
 -D_POSIX_THREADS -D_POSIX_TIMERS -D_POSIX_MONOTONIC_CLOCK \
 -D_POSIX_THREAD_PRIORITY_SCHEDULING -DABSL_FORCE_WAITER_MODE=2 \
 -DMOZC_BTRON_TARGET -DMOZC_TESSRONOS_TARGET -DMOZC_DATASET_MAGIC_NUMBER_LENGTH=7 \
 -I$TFP/shim/include -include $TFP/shim/ts_posix.h"
CXXF="$BASE -std=gnu++20 -fno-exceptions -fno-rtti"
INCS="-I$GEN/absl_patch -I$GEN/pb_patch -I$GEN/mozc_patch -I$SRC -I$BIN \
 -I$ABSL -I$PB/src -I$UTF8 -I$ZLIB"

export CC CXX BASE CXXF INCS OUT

# One compile job: "<kind> <source> <object dir> <object name>"; a failure is
# kept in <object dir>.fail with its first errors.
cat > "$OUT/cc1.sh" <<'EOS'
#!/bin/bash
kind=$1; src=$2; dir=$3; obj=$4
[ -f "$dir/$obj" ] && [ "$dir/$obj" -nt "$src" ] && exit 0
if [ "$kind" = c ]; then
	cmd="$CC $BASE $INCS -c $src -o $dir/$obj"
else
	cmd="$CXX $CXXF $INCS -c $src -o $dir/$obj"
fi
if ! $cmd 2> "$dir/$obj.err"; then
	{ echo "=== $src"; grep -m3 -E "error:|fatal error:" "$dir/$obj.err"; } > "$dir/$obj.fail"
	rm -f "$dir/$obj"
else
	rm -f "$dir/$obj.err" "$dir/$obj.fail"
fi
EOS
chmod +x "$OUT/cc1.sh"

# Compile the jobs listed in $1 (one per line) into $2
run_jobs() {
	local list=$1 dir=$2 name=$3
	mkdir -p "$dir"
	rm -f "$dir"/*.fail
	xargs -P "$JOBS" -L 1 "$OUT/cc1.sh" < "$list"
	local n ok fail
	n=$(wc -l < "$list")
	fail=$(ls "$dir"/*.fail 2>/dev/null | wc -l)
	echo "$name: total=$n fail=$fail"
	if [ "$fail" -ne 0 ]; then
		cat "$dir"/*.fail | grep -E "error" | sed -E 's/^[^ ]+ //' | sort | uniq -c | sort -rn | head -25
		return 1
	fi
}

stage_gen() {
	# Shadow headers for abseil, protobuf and mozc: platform probes, and
	# thread_local made plain (the engine runs in one task)
	python3 "$PORTB/patch_thirdparty.py" "$ABSL" "$PB" "$SRC" "$GEN" > /dev/null
	# mozc::Thread runs its work inline, so its std::thread is never made;
	# this library has no std::thread at all
	python3 - "$GEN/mozc_patch/base/thread.h" <<'EOP'
import sys
p = sys.argv[1]
s = open(p).read()
old = "  std::thread thread_;"
assert s.count(old) == 1
s = s.replace(old, """#ifdef MOZC_TESSRONOS_TARGET
  // TessronOS: no threads in this library; the work has already run
  struct NoThread {
    bool joinable() const noexcept { return false; }
    void join() {}
    void detach() {}
  } thread_;
#else
  std::thread thread_;
#endif""")
open(p, "w").write(s)
EOP
	# pthread_t is a number here, not a pointer
	sed -i 's#reinterpret_cast<pid_t>(pthread_self())#static_cast<pid_t>(pthread_self())#' \
		"$GEN/absl_src_patch/sysinfo.cc"
	mkdir -p "$GEN/absl_src"
	sed 's#^  thread_local uint16_t seed =#  static uint16_t seed =#' \
		"$ABSL/absl/container/internal/raw_hash_set.cc" > "$GEN/absl_src/raw_hash_set.cc"
	# newlib's struct tm has no tm_gmtoff: the offset comes from _timezone,
	# as it does for the other libraries without it
	sed 's/^#elif defined(__native_client__) || defined(__myriad2__)/#elif defined(MOZC_TESSRONOS_TARGET) || defined(__native_client__) || defined(__myriad2__)/' \
		"$ABSL/absl/time/internal/cctz/src/time_zone_libc.cc" > "$GEN/absl_src/time_zone_libc.cc"
	grep -q MOZC_TESSRONOS_TARGET "$GEN/absl_src/time_zone_libc.cc"
	echo "gen: done"
}

stage_absl() {
	local EXCLUDE='_test|_testing|benchmark|/test_|mock|matchers|/debugging/internal/(examine_stack|stack_consumption|vdso_support|elf_mem_image|addresses|bounded_utf8_length_sequence)|/debugging/failure_signal_handler|/debugging/internal/stacktrace|time_zone_name_win|/base/internal/poison|stdcpp_waiter|win32_waiter'
	: > "$OUT/absl.list"
	for f in $(find -L "$ABSL/absl" -name "*.cc" | grep -vE "$EXCLUDE"); do
		local o src=$f
		o=$(echo "$f" | sed 's#.*/absl/#absl_#; s#/#_#g; s#\.cc$#.o#')
		case "$f" in
		*/base/internal/sysinfo.cc)		src="$GEN/absl_src_patch/sysinfo.cc" ;;
		*/container/internal/raw_hash_set.cc)	src="$GEN/absl_src/raw_hash_set.cc" ;;
		*/random/internal/seed_material.cc)	src="$GEN/absl_src_patch/seed_material.cc" ;;
		*/cctz/src/time_zone_libc.cc)		src="$GEN/absl_src/time_zone_libc.cc" ;;
		esac
		echo "cxx $src $OUT/absl_obj $o" >> "$OUT/absl.list"
	done
	run_jobs "$OUT/absl.list" "$OUT/absl_obj" absl
}

stage_pb() {
	# The runtime files named by the objects in $LISTS/pb_obj
	: > "$OUT/pb.list"
	local names
	names=$(ls "$LISTS/pb_obj" | grep '\.o$')
	( cd "$PB/src" && find google -name "*.cc" ) | while read -r f; do
		local o
		o=$(echo "$f" | sed 's#/#_#g; s#\.cc$#.o#')
		if echo "$names" | grep -qx "$o"; then
			echo "cxx $PB/src/$f $OUT/pb_obj $o" >> "$OUT/pb.list"
		fi
	done
	echo "c $UTF8/utf8_range.c $OUT/pb_obj utf8_range.o" >> "$OUT/pb.list"
	run_jobs "$OUT/pb.list" "$OUT/pb_obj" protobuf
}

stage_zlib() {
	: > "$OUT/zlib.list"
	for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite \
		 infback inffast inflate inftrees trees uncompr zutil; do
		echo "c $ZLIB/$f.c $OUT/zlib_obj $f.o" >> "$OUT/zlib.list"
	done
	run_jobs "$OUT/zlib.list" "$OUT/zlib_obj" zlib
}

# A mozc source as it is compiled here: some files are patched copies
mozc_src() {
	local p=$1 out="$GEN/mozc_src/$(echo "$1" | sed 's#/#_#g')"
	mkdir -p "$GEN/mozc_src"
	case "$p" in
	base/file_util.cc)
		# newlib names the modification time st_mtim
		python3 "$PORTB/patch_file_util.py" "$SRC/$p" "$out" > /dev/null
		sed -i 's/stat_info\.st_mtimespec\.tv_sec/stat_info.st_mtim.tv_sec/' "$out" ;;
	base/password_manager.cc)
		sed 's/#if defined(__linux__) || defined(__wasm__)/#if defined(__linux__) || defined(__wasm__) || defined(MOZC_BTRON_TARGET)/' \
			"$SRC/$p" > "$out" ;;
	converter/immutable_converter.cc)
		sed 's#^  thread_local Lattice lattice;#  static Lattice lattice;#' "$SRC/$p" > "$out" ;;
	base/mmap.cc)
		python3 "$PORTB/patch_mmap.py" "$SRC/$p" "$out" > /dev/null ;;
	dictionary/user_dictionary.cc)
		python3 "$PORTB/patch_user_dictionary.py" "$SRC/$p" "$out" > /dev/null ;;
	base/process_mutex.cc)
		python3 "$PORTB/patch_process_mutex.py" "$SRC/$p" "$out" > /dev/null ;;
	storage/lru_storage.cc)
		python3 "$PORTB/patch_lru_storage.py" "$SRC/$p" "$out" > /dev/null ;;
	base/system_util.cc)
		sed 's#  struct passwd pw, \*ppw;#  return "mozc";\n  struct passwd pw, *ppw;#' \
			"$SRC/$p" > "$out" ;;
	*)	echo "$SRC/$p"; return ;;
	esac
	echo "$out"
}

stage_mozc() {
	: > "$OUT/mozc.list"
	while read -r p; do
		[ -z "$p" ] && continue
		local o
		o=$(echo "$p" | sed 's#/#_#g; s#\.cc$#.o#; s#\.c$#.o#')
		echo "cxx $(mozc_src "$p") $OUT/mozc_obj $o" >> "$OUT/mozc.list"
	done < "$LISTS/mozc_src2.txt"
	while read -r g; do
		[ -z "$g" ] && continue
		local s o
		case "$g" in
		external/*)	s="$BIN/$g"; [ -f "$s" ] || s="$EXT/${g#external/}" ;;
		*)		s="$BIN/$g" ;;
		esac
		o="gen_$(echo "$g" | sed 's#/#_#g; s#\.cc$#.o#')"
		echo "cxx $s $OUT/mozc_obj $o" >> "$OUT/mozc.list"
	done < "$LISTS/gen_src.txt"
	run_jobs "$OUT/mozc.list" "$OUT/mozc_obj" mozc
}

stage_adapter() {
	: > "$OUT/adapter.list"
	for f in tc_codec roman_table key_table user_dict mozc_kserver; do
		echo "cxx $BTRON/$f.cc $OUT/adapter_obj $f.o" >> "$OUT/adapter.list"
	done
	run_jobs "$OUT/adapter.list" "$OUT/adapter_obj" adapter
}

stage_tf() {
	: > "$OUT/tf.list"
	echo "cxx $TFP/shim/ts_posix.cc $OUT/ts_obj ts_posix.o" >> "$OUT/tf.list"
	echo "cxx $TFP/tsmozc.cc $OUT/ts_obj tsmozc.o" >> "$OUT/tf.list"
	INCS="$INCS -iquote $TFP/../../include" run_jobs "$OUT/tf.list" "$OUT/ts_obj" tf
}

stage_link() {
	local LIBDIR GCCDIR
	LIBDIR=$(dirname "$($CXX -print-file-name=libc.a)")
	GCCDIR=$(dirname "$($CXX -print-libgcc-file-name)")
	# Everything the interface reaches, with the C and C++ runtimes, as one
	# relocatable object. The constructor table is given ends the
	# desktop's glue can walk.
	cat > "$OUT/blob.ld" <<'EOL'
SECTIONS
{
	.text.tsmozc_hooks : {
		*(malloc_hook)
	}
	.rodata.tsmozc_cold : {
		*(protodesc_cold) *(flags_help_cold)
		*(.gcc_except_table .gcc_except_table.*)
		*(.eh_frame)
	}
	.data.tsmozc_init : {
		__tsmozc_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__tsmozc_init_end = .;
	}
}
EOL
	# Archives, so that only what the interface reaches is taken: the
	# libraries hold programs of their own (with main) and the adapter's
	# files appear in the engine's list too
	local a
	for a in mozc absl pb zlib; do
		rm -f "$OUT/lib${a}_tf.a"
		ar rcs "$OUT/lib${a}_tf.a" "$OUT/${a}_obj"/*.o
	done
	$LD -r -o "$OUT/tsmozc_full.o" -T "$OUT/blob.ld" \
		-u tsmozc_start \
		"$OUT"/ts_obj/*.o "$OUT"/adapter_obj/*.o \
		--start-group \
		"$OUT/libmozc_tf.a" "$OUT/libabsl_tf.a" "$OUT/libpb_tf.a" "$OUT/libzlib_tf.a" \
		"$LIBDIR/libstdc++.a" "$LIBDIR/libsupc++.a" "$LIBDIR/libm.a" "$LIBDIR/libc.a" \
		"$GCCDIR/libgcc.a" \
		--end-group
	# Only the interface stays global
	$NM -g --defined-only "$OUT/tsmozc_full.o" | awk '{print $3}' \
		| grep -E '^(tsmozc_|__tsmozc_init_)' > "$OUT/keep.txt"
	$OBJCOPY --keep-global-symbols="$OUT/keep.txt" "$OUT/tsmozc_full.o" "$OUT/tsmozc.o"
	echo "link: $(ls -la "$OUT/tsmozc.o" | awk '{print $5}') bytes"
	echo "undefined (to be supplied by the desktop):"
	$NM -u "$OUT/tsmozc.o" | awk '{print "  " $2}'
	if $READELF -S "$OUT/tsmozc.o" | grep -qE '\.tdata|\.tbss'; then
		echo "WARNING: thread-local storage left in the object"
	fi
}

case "$STAGE" in
gen)		stage_gen ;;
absl)		stage_absl ;;
pb)		stage_pb ;;
zlib)		stage_zlib ;;
mozc)		stage_mozc ;;
adapter)	stage_adapter ;;
tf)		stage_tf ;;
link)		stage_link ;;
all)		stage_gen; stage_absl; stage_pb; stage_zlib; stage_mozc
		stage_adapter; stage_tf; stage_link ;;
*)		echo "usage: $0 [gen|absl|pb|zlib|mozc|adapter|tf|link|all]"; exit 2 ;;
esac
