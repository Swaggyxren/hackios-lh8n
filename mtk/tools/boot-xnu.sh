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

read_ps() {
    adb -s "$S" shell "cat /sys/class/power_supply/$1/uevent 2>/dev/null" 2>/dev/null |
        sed -n "s/^POWER_SUPPLY_$2=//p" | head -1
}

BATT=$(read_ps battery CAPACITY)
MV=$(read_ps battery VOLTAGE_NOW)
UA=$(read_ps battery CURRENT_NOW)
AVG=$(read_ps battery CURRENT_AVG)
TEMP=$(read_ps battery TEMP)
STATUS=$(read_ps battery STATUS)
ONLINE=$(read_ps charger ONLINE)

case "$STATUS" in
Charging)       ST=1 ;;
Discharging)    ST=2 ;;
"Not charging") ST=3 ;;
Full)           ST=4 ;;
*)              ST=0 ;;
esac

CUR=${UA:-$AVG}
CUR=${CUR#-}
[ -n "$CUR" ] || CUR=0
CUR=$(( CUR / 1000 ))
[ "$ST" = 2 ] && CUR="-$CUR"

TC=$(( ${TEMP:-0} / 10 ))
MV_MV=$(( ${MV:-0} / 1000 ))
SERIAL=$(adb -s "$S" shell 'getprop ro.serialno' 2>/dev/null | tr -d '\r\n')
case "$SERIAL" in
[0-9A-Za-z]*) ;;
*) SERIAL="" ;;
esac

REQ="DARWIN-LH8N:BOOT-XNU"
if [ -n "$BATT" ]; then
    REQ="$REQ:batt=$BATT,$MV_MV,$CUR,$TC,$ST,${ONLINE:-0}"
fi
[ -n "$SERIAL" ] && REQ="$REQ:serial=$SERIAL"

adb -s "$S" shell "su -c \"printf '$REQ\n' > /dev/pmsg0\""
echo "Request token $REQ written to /dev/pmsg0. Rebooting into XNU..."
adb -s "$S" reboot
