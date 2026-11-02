#!/bin/bash
# Build fbdiag.img: fbdiag in front of Android's kernel (rooted boot_a backup by default).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

LLD="ld.lld"
if [ -x "$ROOT/xnu-deps/host/zig/zig" ]; then
    LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
fi

if [ ! -f "$HERE/font8x16.h" ]; then
    echo "Generating font8x16.h..."
    python3 "$ROOT/mtk/tools/mkfont8x16.py" > "$HERE/font8x16.h"
fi

PANEL_W=${PANEL_W:-1080}
PANEL_H=${PANEL_H:-2460}

CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra -DPANEL_W=${PANEL_W}u -DPANEL_H=${PANEL_H}u"

clang $CFLAGS -c "$HERE/fbdiag_start.S" -o "$HERE/fbdiag_start.o"
clang $CFLAGS -c "$HERE/fbdiag.c" -o "$HERE/fbdiag.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/fbdiag_start.o" "$HERE/fbdiag.o" -o "$HERE/fbdiag.bin"
echo "Built fbdiag.bin ($(stat -c%s "$HERE/fbdiag.bin") bytes for ${PANEL_W}x${PANEL_H})"

BOOT=${ANDROID_BOOT:-$ROOT/backup/boot_a_magisk.img}
if [ -f "$BOOT" ]; then
    CHAINBOOT="$ROOT/mtk/tools/chainboot.py"
    if [ -f "$ROOT/mtk/tools/chainboot_v4.py" ]; then
        CHAINBOOT="$ROOT/mtk/tools/chainboot_v4.py"
    fi
    python3 "$CHAINBOOT" "$BOOT" "$HERE/fbdiag.bin" "$HERE/fbdiag.img"
else
    echo "Note: ANDROID_BOOT not found at $BOOT. To package fbdiag.img, provide ANDROID_BOOT=<path/to/boot.img>"
fi
