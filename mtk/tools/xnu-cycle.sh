#!/bin/bash
# One XNU run on the Tecno LH8n: build mtkloader (extra args go to the compiler, e.g.
# -DXNU_CHECKPOINT=17), pack it with Android and the XNU payload, flash boot_a (and optionally
# vendor_boot_a), let XNU run and Android come back, then print what XNU left in pstore.
set -e
if [ -z "$HAKIOS_SERIAL" ]; then
    DEVS=$(adb devices | grep -w "device" | awk '{print $1}')
    DEV_COUNT=$(echo "$DEVS" | grep -v '^$' | wc -l)
    if [ "$DEV_COUNT" -eq 1 ]; then
        HAKIOS_SERIAL="$DEVS"
    elif [ "$DEV_COUNT" -gt 1 ]; then
        echo "Multiple adb devices found. Set HAKIOS_SERIAL to specify target." >&2
        exit 1
    else
        FDEVS=$(fastboot devices | awk '{print $1}')
        FDEV_COUNT=$(echo "$FDEVS" | grep -v '^$' | wc -l)
        if [ "$FDEV_COUNT" -eq 1 ]; then
            HAKIOS_SERIAL="$FDEVS"
        else
            echo "No adb or fastboot device connected. Set HAKIOS_SERIAL." >&2
            exit 1
        fi
    fi
fi
S="$HAKIOS_SERIAL"
echo "Targeting device: $S"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
mkdir -p "$ROOT/logs"

# test settings: watchdog and watcher on, XNU comes back by itself
SECS=${XNU_SECS:-45}
XNU_ARGS=${XNU_ARGS:-cpus=1}
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}

TEST_FLAGS=(-DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"")
echo "Building mtkloader..."
"$ROOT/mtk/loader/build.sh" "${TEST_FLAGS[@]}" "$@" >/dev/null
python3 "$ROOT/mtk/loader/build_flashed.py" >/dev/null

if [ "$(adb -s $S get-state 2>/dev/null)" = device ]; then
    echo "Rebooting into bootloader..."
    adb -s $S reboot bootloader
fi

for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }

echo "Flashing boot_a..."
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1

echo "Rebooting phone into Android..."
fastboot -s $S reboot >/dev/null 2>&1

echo "Waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4

"$ROOT/mtk/tools/boot-xnu.sh"
echo "XNU is running; Android will return automatically after completion / timeout ($SECS s)..."
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3

for i in $(seq 1 10); do adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done

adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" 2>/dev/null | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
echo "Log saved to: $OUT ($(wc -c < "$OUT" 2>/dev/null || echo 0) bytes)"
