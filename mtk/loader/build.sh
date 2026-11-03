#!/bin/bash
# Build mtkloader.bin. MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"     # the top of the tree (mtk/loader/ is two below it)

# the linker: ld.lld on the PATH, else zig's (xnu-deps/host/zig), or LLD="..." given
if [ -z "${LLD:-}" ]; then
	if command -v ld.lld >/dev/null; then LLD=ld.lld
	elif [ -x "$ROOT/xnu-deps/host/zig/zig" ]; then LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
	else echo "mtkloader: no ld.lld (on the PATH, or zig in xnu-deps/host/zig); give LLD=..." >&2; exit 1; fi
fi
[ -f "$ROOT/xnuloader/identity.h" ] || { echo "mtkloader: no xnuloader/identity.h -- xnuloader/mkidentity.sh makes it from your IPSW" >&2; exit 1; }
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

# Two headers are made here and never kept in git (.gitignore):
#  touchfw.h   the touch controller's firmware, out of YOUR phone's hdl_firmware.img (pull-firmware.sh puts it
#              in fw-touch/): it is the vendor's, and the controller loses it at every reset, so the loader
#              carries it (tcm.h)
#  font8x16.h  the loader's 8x16 text font
TOUCH_IMG="${TOUCH_IMG:-$ROOT/fw-touch/hdl_firmware.img}"
if [ ! -f "$HERE/touchfw.h" ]; then
	if [ -f "$TOUCH_IMG" ]; then
		python3 "$HERE/../tools/mktouchfw.py" "$TOUCH_IMG" ROMBOOT_APP_CODE "$HERE/touchfw.h"
	else
		cat << 'EOF' > "$HERE/touchfw.h"
#ifndef TOUCHFW_H
#define TOUCHFW_H
#include <stdint.h>
#define TOUCH_FW_FLAG 0
#define TOUCH_FW_SIZE 16
static const uint8_t touch_fw[16] = { 0 };
#endif
EOF
	fi
fi
[ -f "$HERE/font8x16.h" ] || python3 "$HERE/../tools/mkfont8x16.py" > "$HERE/font8x16.h"

clang $CFLAGS "$@" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS "$@" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
