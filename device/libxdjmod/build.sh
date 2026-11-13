#!/bin/sh
# Cross-compile libxdjmod.so for the XDJ-RX (ARMv7 Cortex-A9, glibc 2.13, softfp like Pioneer's program) and
# compare it with the library inside the builder (builder/release/libxdjmod.so, MD5 in release.json).
#     device/libxdjmod/build.sh [<out dir>]          (default: device/libxdjmod/build)
# Needs zig 0.16.0 (https://ziglang.org/download/): on PATH, or ZIG=/path/to/zig.
# -z lazy is required: zig links with bind-now by default, and a bind-now library crashes the XDJ-RX's glibc 2.13
# loader on its first PLT call.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
ZIG=${ZIG:-zig}
mkdir -p "$OUT"
v=$("$ZIG" version)
[ "$v" = "0.16.0" ] || echo "note: zig $v, the release was built with 0.16.0 (another version may give other bytes)"

"$ZIG" cc \
	-target arm-linux-gnueabi.2.13 -mcpu=cortex_a9 -mfpu=neon -mfloat-abi=softfp \
	-O2 -fPIC -shared -fvisibility=hidden -fno-sanitize=all \
	-fno-builtin-memcmp -fno-builtin-bcmp \
	-Wall -Wextra -Werror -Wno-unused-parameter \
	-Wl,-z,lazy -Wl,-soname,libxdjmod.so -s \
	-o "$OUT/libxdjmod.so" "$HERE/xdjmod.c" -ldl -lpthread -lm

md5sum_() { if command -v md5 >/dev/null; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi; }
got=$(md5sum_ "$OUT/libxdjmod.so")
want=$(md5sum_ "$HERE/../../builder/release/libxdjmod.so")
echo "built $OUT/libxdjmod.so, md5 $got"
if [ "$got" = "$want" ]; then
	echo "same as builder/release/libxdjmod.so"
else
	echo "DIFFERS from builder/release/libxdjmod.so ($want)"
	exit 1
fi
