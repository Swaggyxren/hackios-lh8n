# Carving the 4 GiB `xnusys` Partition on Tecno Pova 5 Pro 5G (`LH8n`)

This guide documents the exact zero-data-loss procedure used on the **Tecno Pova 5 Pro 5G (`LH8n`, MT6833, 256 GB UFS 2.2)** to carve a dedicated **4.0 GiB `xnusys` partition** (`/dev/block/sdc59`) out of the tail of `userdata` (`/dev/block/sdc57`) without wiping Android or user data.

> **Warning:** Modifying GPT partition tables and resizing filesystems always carries risk. Back up important data and keep copies of `sdc-gpt-primary.bin` and `sdc-gpt-secondary.bin` before making any changes. Make sure your `/data` partition has plenty of free space (at least 10+ GiB free before shrinking by 4 GiB).

---

## Hardware & Partition Layout (`LH8n` 256 GB UFS LUN 2 — `/dev/block/sdc`)

- **Block device:** `/dev/block/sdc` (UFS LUN 2, `4096`-byte logical sectors)
- **Total sectors:** `62,480,384` (last usable sector: `62,480,378`)
- **Partition layout change:**

| Partition | GPT Entry | Block Node | Before | After | Size |
|---|---|---|---|---|---|
| `userdata` | 56 | `/dev/block/sdc57` | `2799616..62476282` (`59,676,667` blocks) | **`2799616..61427455`** (`58,627,840` blocks) | 227.6 GiB → **223.6 GiB** |
| `xnusys` | 58 | `/dev/block/sdc59` | *(none)* | **`61427456..62476282`** (`1,048,827` sectors) | **4.0 GiB** |

*(Note: Verify your own device's exact `sdc` total sector count and `userdata` bounds before running — never blindly reuse sector numbers from a different storage capacity variant.)*

---

## Prerequisites

- Rooted Android (Magisk/KernelSU) with `adb` access
- **OrangeFox Recovery** (or another recovery with `resize.f2fs`, `fsck.f2fs`, and working `dm-default-key` `/data` decryption)
- `python3` on the host PC
- `gpt-carve.py` (from [`Xia-s-Projekt/hakios-x6739`](https://github.com/Xia-s-Projekt/hakios-x6739))
- Built `system.apfs` (~2.47 GiB) ready on an SD card or pushed over `adb`

---

## Step 1 — Back Up Primary and Secondary GPT (in Android)

Dump sectors `0..5` (primary GPT) and `62480378..62480383` (secondary/backup GPT) from `/dev/block/sdc` and pull them to your PC:

```bash
mkdir -p backup

adb shell 'su -c "toybox dd if=/dev/block/sdc bs=4096 count=6 of=/data/local/tmp/sdc-gpt-primary.bin status=none && \
  toybox dd if=/dev/block/sdc bs=4096 count=6 skip=62480378 of=/data/local/tmp/sdc-gpt-secondary.bin status=none"'

adb pull /data/local/tmp/sdc-gpt-primary.bin backup/
adb pull /data/local/tmp/sdc-gpt-secondary.bin backup/
```

---

## Step 2 — Generate the Carved GPT Tables on PC

Run `gpt-carve.py` against the backed-up GPT headers with the total sector count (`62480384`):

```bash
python3 tools/gpt-carve.py \
  backup/sdc-gpt-primary.bin \
  backup/sdc-gpt-secondary.bin \
  62480384
```

Expected output:

```text
58 partitions, last usable sector 62480378
userdata: 2799616..62476282 (227.6 GiB), entry 56
userdata: 2799616..61427455 (223.6 GiB)
xnusys:   61427456..62476282 (4.0 GiB), entry 58
written: backup/sdc-gpt-primary.bin.new (sectors 0..5), backup/sdc-gpt-secondary.bin.new (sectors 62480378..62480383)
```

Compute the new `userdata` 4K block count from the output above:

```text
61427455 - 2799616 + 1 = 58,627,840 blocks
```

---

## Step 3 — Safely Shrink Encrypted F2FS in OrangeFox Recovery

> **Critical:** Android 14's `/data` uses `dm-default-key` metadata encryption on top of raw `userdata`. You **must** shrink F2FS **before** writing the new GPT, and you **must** run `fsck.f2fs` / `resize.f2fs` against the **decrypted device-mapper node** (`/dev/block/mapper/userdata` → `/dev/block/dm-9`), never the raw encrypted `/dev/block/sdc57`.

1. Boot into **OrangeFox Recovery** and unlock/decrypt `/data` (so `/dev/block/mapper/userdata` is active).
2. Unmount `/data` and `/sdcard` while keeping the decrypted mapper open:

```bash
adb shell "umount /sdcard 2>/dev/null; umount /data/user/0 2>/dev/null; umount /data 2>/dev/null; mount | grep -i data"
```

3. Verify filesystem integrity first:

```bash
adb shell "/first_stage_ramdisk/system/bin/fsck.f2fs /dev/block/mapper/userdata"
```

4. Run `resize.f2fs` with **`-s` (safe resize)** to migrate all data blocks out of the top 4 GiB without touching F2FS metadata structures, targeting the exact new block count (`58627840`):

```bash
adb shell "/first_stage_ramdisk/system/bin/resize.f2fs -d 1 -s -t 58627840 /dev/block/mapper/userdata"
```

5. Test-mount `/data` to confirm the shrunk size (`224G`) and verify all user data is intact, then unmount again:

```bash
adb shell "mount -t f2fs /dev/block/mapper/userdata /data && df -h /data && umount /data"
```

---

## Step 4 — Write the Carved GPT to `/dev/block/sdc`

Only proceed once Step 3 exits cleanly with code `0` and `/data` test-mounts without errors.

```bash
adb push backup/sdc-gpt-primary.bin.new /tmp/
adb push backup/sdc-gpt-secondary.bin.new /tmp/

adb shell 'dd if=/tmp/sdc-gpt-primary.bin.new of=/dev/block/sdc bs=4096 count=6 seek=0 conv=fsync'
adb shell 'dd if=/tmp/sdc-gpt-secondary.bin.new of=/dev/block/sdc bs=4096 count=6 seek=62480378 conv=fsync'
adb shell 'sync'

# Reboot recovery once so the kernel rereads the updated GPT
adb reboot recovery
```

Once back in recovery, confirm the new partition node exists and is 4.0 GiB:

```bash
adb wait-for-recovery
adb shell 'ls -la /dev/block/by-name/xnusys'
# Expected: /dev/block/by-name/xnusys -> /dev/block/sdc59

adb shell 'blockdev --getsize64 /dev/block/by-name/xnusys; blockdev --getsize64 /dev/block/by-name/userdata'
```

---

## Step 5 — Write `system.apfs` to `xnusys` and Verify

Copy `system.apfs` into `/dev/block/by-name/xnusys` (example below streams 2.47 GiB from SD card `/dev/block/mmcblk0p1`; note recovery `toybox dd` does not support `status=progress`):

```bash
adb shell 'dd if=/dev/block/mmcblk0p1 of=/dev/block/by-name/xnusys bs=4M count=591 conv=fsync'
adb shell 'sync'
```

Verify the APFS container magic (`NXSB`) at byte offset `32`:

```bash
adb shell 'dd if=/dev/block/by-name/xnusys bs=4096 count=1 status=none | dd bs=1 skip=32 count=4 status=none'
# Expected output: NXSB
```

---

## Rollback (Restoring Original Partition Table)

If you ever want to reclaim the 4 GiB and remove `xnusys`:

1. Boot into OrangeFox Recovery, unmount `/data`, and restore the backed-up GPT headers:
   ```bash
   adb push backup/sdc-gpt-primary.bin /tmp/
   adb push backup/sdc-gpt-secondary.bin /tmp/
   adb shell 'dd if=/tmp/sdc-gpt-primary.bin of=/dev/block/sdc bs=4096 count=6 seek=0 conv=fsync'
   adb shell 'dd if=/tmp/sdc-gpt-secondary.bin of=/dev/block/sdc bs=4096 count=6 seek=62480378 conv=fsync'
   adb shell 'sync' && adb reboot recovery
   ```
2. After rebooting recovery, unmount `/data` and grow F2FS back to fill the full `userdata` partition:
   ```bash
   adb shell "/first_stage_ramdisk/system/bin/resize.f2fs /dev/block/mapper/userdata"
   ```
