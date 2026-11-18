# HakiOS on Tecno Pova 5 Pro 5G (`LH8n` / `MT6833`) — Handoff & Status

**Last Updated:** 2026-11-09 19:10 (+08:00)  
**Device:** Tecno Pova 5 Pro 5G (`LH8n`, MediaTek Dimensity 6080 / `MT6833`, ADB serial `H333LH8NU0123456`)  
**Repository:** `/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n` (branch `mtk`, HEAD `fb90864`)

---

## 1. Where We Left Off
- **Device State:** Running stock Android (`boot_a` and `vendor_boot_a` restored from `/home/ydnar/Downloads/boot.img` and `/home/ydnar/Downloads/vendor_boot_a_backup.img`), connected over `adb` (`H333LH8NU0123456`).
- **Latest Pulled Log:** `/sys/fs/pstore/console-ramoops-0` saved to [logs/pstore-latest.log](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/logs/pstore-latest.log).
- **Latest Built Images (2026-11-09 18:37, not yet flashed):**
  - [hakios-kernel](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/hakios-kernel) (`Darwin Kernel Version 22.5.0: Lun 09 Nob 2026 06:35:39 N.H. PST`)
  - [build-payload.bin](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/build-payload.bin) (`19,148,744` bytes)
  - [xnu-flashed-slot_a.img](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/xnu-flashed-slot_a.img) (`19,820,544` bytes)
  - [vendor_boot_payload.img](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/vendor_boot_payload.img) (`50,864,128` bytes)

---

## 2. Current Problems We Are Having

### Problem A: Touchscreen (`FT8726` on `SPI4`) Unresponsive in iOS
- When XNU boots into the iOS 16.5 `SpringBoard` homescreen, touch input does not register.
- **Root Causes Identified (from stock `focaltech_noflash_common.ko`, `spi-mt65xx.ko`, `clk-mt6833.ko`, `clk-mt6833-bus.ko`):**
  1. [`spi4_transfer_bytes`](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/mtk/mt6833/drivers/input/hakios.mt6833_touch.c#L239-L327) checked `SPI_STATUS1_REG` (`+0x20`), where bit 0 is `1` when `IDLE`, causing transfers to abort on spin 0 before the 26 MHz SPI clock domain latched `SPI_CMD_ACT`.
  2. [`mt6833_touch_hw_init`](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/mtk/mt6833/drivers/input/hakios.mt6833_touch.c#L823-L875) had `INFRACFG_AO` `ifrao_spi4` `clr_ofs` (`+0x00a8`) and `sta_ofs` (`+0x00ac`) swapped and did not un-gate `TOPCKGEN` `spi_sel` (`CLK_CFG_6_CLR = +0x0078` bit 23).
  3. `FT8726` uses `IRQF_TRIGGER_FALLING` (`0x2002`) pulses on `GPIO 14` rather than holding a level-low interrupt across `8 ms` polling intervals.
- **Status:** Fixes are implemented and compiled into [xnu-flashed-slot_a.img](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/xnu-flashed-slot_a.img) and [vendor_boot_payload.img](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/vendor_boot_payload.img), plus live `ramoops` logging via [`ramoops_live_putc`](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/mtk/mt6833/drivers/input/hakios.mt6833_touch.c#L183-L214) to `0x480ef000ULL`, awaiting hardware verification.

### Problem B: Getting Stuck on White Console Log Lines (`[FAIL] mount the iOS volume read only` / `CPU halted`)
- **What [logs/pstore-latest.log](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/logs/pstore-latest.log) Shows:**
  - Lines 108–111: `LK initrd: 0x51100000 .. 0x544122ea` loaded an older `19,066,272`-byte payload at `0x52ed6000` (`Darwin Kernel Version 22.5.0: Lun 09 Nob 2026 03:55:34 N.H. PST`).
  - Line 402: `ufs: native: not used: the loader offers no area for a list (or not one as expected: controller 11270000, area 80210000 of 69632, command descriptors at 4096 of 2048)` — it fell back to the loader's shared page, which timed out after 20s (`ufs: no answer from the loader to request 1 in 20 s`).
  - Lines 448–450 & 1155–1179: `mount /dev/ufs_xnusys: Input/output error` → `[FAIL] mount the iOS volume read only` → `/sbin/launchd.darwin: No such file or directory` → `CPU halted` (leaving the device stuck on the white console text lines).
- **Why an Older Payload or Mismatched `ufs_shared_pa` Area Occurred:**
  1. [mtk/tools/repack_vendor_boot.py](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/mtk/tools/repack_vendor_boot.py) updates `vrsize` in the Boot Header v4 header (offset `24`), but does **not** update the v4 vendor ramdisk table entry's `ramdisk_size` (`entry 0` at `table_off + 0`, which stays `31,512,105` / `0x1e0d629`). If LK's v4 loader reads `entry 0` from the vendor ramdisk table instead of the top-level `vrsize`, or if `xnu-flashed-slot_a.img` (`mtkloader`) is out of sync with `vendor_boot_payload.img`, either the payload is truncated/missed or `mtkloader`'s `ufs_shared` layout (`69,632` bytes vs `73,728` bytes) mismatches the kernel's `ufs_native_attach` check in [drivers/storage/hakios.ufshc.c](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/drivers/storage/hakios.ufshc.c).

---

## 3. Completed & Verified Earlier
1. **Native UFS 2.2 Storage & APFS Root Mount (`ufs_xnusys`):**
   - Fixed `UFSHC_PHYS` (`0x11270000ULL`) in [drivers/storage/hakios.ufshc.c](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/drivers/storage/hakios.ufshc.c) and [mtk/mt6833/drivers/storage/hakios.mt6833_ufs.c](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/mtk/mt6833/drivers/storage/hakios.mt6833_ufs.c).
   - Fixed `ufs_find_xnusys()` in [drivers/storage/hakios.ufs.c](file:///home/ydnar/Documents/LH8n%20Tree%20sar/hakios-lh8n/drivers/storage/hakios.ufs.c) to use `ufs_shared_pa=0x80200000` (`start_lba = 975616`, `count_lba = 3145728`).
2. **iOS 16.5 Homescreen (`SpringBoard`) Boot:**
   - Confirmed on hardware when `mtkloader` and `vendor_boot_payload.img` were in sync.

---

## 4. Next Steps When Resuming
1. **Check `ufs_shared` area size & `repack_vendor_boot.py` v4 table:**
   - Verify `mtkloader.c` `ufs_setup_shared()` area size matches `hakios.ufshc.c` (`area 80210000` size check) and update `repack_vendor_boot.py` to also patch `vr_table` entry 0's `ramdisk_size`.
2. **Flash & Test Touch on iOS Homescreen:**
   ```bash
   adb reboot bootloader
   fastboot flash boot_a "/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n/xnu-flashed-slot_a.img"
   fastboot flash vendor_boot_a "/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n/vendor_boot_payload.img"
   fastboot reboot
   ```
3. **Restore Stock Android:**
   ```bash
   fastboot flash boot_a /home/ydnar/Downloads/boot.img
   fastboot flash vendor_boot_a /home/ydnar/Downloads/vendor_boot_a_backup.img
   fastboot reboot
   ```
