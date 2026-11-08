#!/bin/bash
# Reboot the LH8n into Darwin/XNU once: mtkloader starts Android unless it finds this request
# (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android again.
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
        echo "No adb device connected. Make sure phone is booted into Android with USB debugging on." >&2
        exit 1
    fi
fi
S="$HAKIOS_SERIAL"
echo "Targeting device: $S"
adb -s "$S" shell 'su -c "printf DARWIN-LH8N:BOOT-XNU > /dev/pmsg0"'
echo "Request token DARWIN-LH8N:BOOT-XNU written to /dev/pmsg0. Rebooting into XNU..."
adb -s "$S" reboot
