#!/bin/bash
#
#  TessronOS
#  Copyright (C) 2026 satromi
#  This software is distributed under the T-License 2.2.
#
#  igt_mfx_test.sh: the batch of an H.264 picture for Intel's video
#  engine (device/gpu/intel/igt_mfx.c) built on the host and set against
#  Mesa's packing of the same commands (test_igt_mfx.c; docs/tessronos/
#  13-gpu.md 13.12). It needs the Mesa sources and a build of them that
#  made genxml's headers (tools/x64/mesa/build.sh). Then the pictures of
#  the check at start (device/gpu/intel/igt_vtest.c) decoded by FFmpeg's
#  H.264 decoder built for the host (test_igt_vtest.c; FFSRC and FFHOST
#  name FFmpeg's sources and that build: ffmpeg_btron's
#  tools/build-hosttest.sh makes it).
#
#    tools/x64/mesa/igt_mfx_test.sh [pictures]
#
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
XHERE=$TOP/tools/x64/mesa
MESA=${MESA:-$HOME/mesa/mesa-26.2.3}
MBUILD=${MBUILD:-$HOME/tsmesa-x64/build}
OUTD=$(mktemp -d)
trap 'rm -rf "$OUTD"' EXIT
gcc -O1 -g0 -std=gnu11 -Wall -Wno-unused-function -DIGT_MFX_HOST -DHAVE_ENDIAN_H -D_GNU_SOURCE \
	-I"$XHERE/mfxshim" -I"$XHERE/hostshim" -I"$TOP/tools/mesa/hostshim" -I"$TOP/include" \
	-I"$TOP/device/gpu/intel" -I"$MESA/src" -I"$MESA/include" -I"$MESA/src/intel" -I"$MESA/src/intel/genxml" \
	-I"$MBUILD/src" -I"$MBUILD/src/intel" \
	-o "$OUTD/test_igt_mfx" "$XHERE/test_igt_mfx.c" "$TOP/device/gpu/intel/igt_mfx.c"
"$OUTD/test_igt_mfx" "${1:-2000}"

FFSRC=${FFSRC:-$HOME/ffbt/ffmpeg-4.4.6}
FFHOST=${FFHOST:-$HOME/ffbt/hostbuild}
gcc -O1 -g0 -std=gnu11 -Wall -DIGT_MFX_HOST -I"$XHERE/mfxshim" -I"$XHERE/hostshim" -I"$TOP/tools/mesa/hostshim" \
	-I"$TOP/include" -I"$TOP/device/gpu/intel" -I"$FFHOST" -I"$FFSRC" \
	-o "$OUTD/test_igt_vtest" "$XHERE/test_igt_vtest.c" "$TOP/device/gpu/intel/igt_vtest.c" \
	"$FFHOST/libavcodec/libavcodec.a" "$FFHOST/libswresample/libswresample.a" "$FFHOST/libavutil/libavutil.a" -lm -lz -lpthread
"$OUTD/test_igt_vtest"
