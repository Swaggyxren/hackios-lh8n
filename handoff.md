# HakiOS Darwin / XNU Bringup on Tecno Pova 5 Pro 5G (LH8n / MT6833) — Handoff Document

**Date**: November 8, 2026  
**Target Hardware**: Tecno Pova 5 Pro 5G (`LH8n`), MediaTek Dimensity 6080 (`MT6833` / `MT6768` family)  
**Kernel Target**: Apple Darwin 22.5.0 / XNU 8796.121.2 (iOS 16.5)  
**Status**: Paused after Phase 7 (Full Graphical iOS SpringBoard Homescreen Boot verified; awaiting native physical touch driver implementation).

---

## 1. What Has Been Achieved

1. **Bootloader Chain & DRAM Payload Staging**:
   - `mtkloader` chained in Android boot header v4 on slot A (`xnu-flashed-slot_a.img`).
   - `vendor_boot_payload.img` loaded by Little Kernel DMA into DRAM at `0x51100000`, unpacked at `0x52f0e000`.
   - Kernel memory base `0xc0000000`, entry point `0xc34424a8`.
   - Framebuffer mapped at `0x76950000`, 1080x2460, stride 4352.

2. **Native UFS 2.2 Controller Handover & Storage Root Pivot**:
   - Little Kernel UFS controller (`0x11270000`) handed over cleanly with active transfer lists at `0x80210000` (32 command slots) and shared page at `0x80200000`.
   - Kernel binary patched (`tools/patch-kernel-mt6893.py`) to bind `UFSHC_PHYS` to `0x11270000`.
   - `init` mounts iOS root filesystem from `/dev/ufs_xnusys` (sdc59) at `/flash`.
   - VFS root switch executed via `kern.hakios_switch_root`: Darwin volume becomes `/`, bootstrap ramdisk mounted on `/private/preboot`.
   - Cryptexes mounted at `/System/Cryptexes/OS` and `/System/Cryptexes/App` for dyld Shared Cache.

3. **PID 1 Handover & Darwin Ignition**:
   - `init` patches applied at byte offset `0x5a0` (`1f 20 03 d5` NOP) so `base = "/redwood"` remains intact when launched as `/sbin/launchd`.
   - Process 1 hands off cleanly via `/redwood/etc/rc`: `exec /sbin/launchd.darwin`.
   - Native Apple system daemons run resident without watchdog timeout (`loader_return_after=0`):
     - `launchd.darwin[1]`
     - `containermanagerd[12]`
     - `usermanagerd[15]`
     - `lsd[26]`
     - `backboardd` (Event dispatch & WindowServer client)
     - `SpringBoard[7]` (iOS Shell / Compositor)

4. **Graphical Homescreen Verified**:
   - Full graphical iOS 16.5 homescreen, wallpaper, dock, and app icons render live on the device panel.

---

## 2. Touch Subsystem Findings & Current State

### Hardware Device Tree Details (from `dtbo.img` `/fragment@17`):
- **Touch IC**: FocalTech FT8726 (`focaltech,fts` / `tchip,ilitek`)
- **Bus**: MediaTek `spi4` at `0x11018000` (max frequency 8 MHz = `0x007a1200`)
- **IRQ GPIO**: GPIO 14 (`touch,irq-gpio` = 14, active-low)
- **RST GPIO**: GPIO 19 (`touch,reset-gpio` = 19)
- **Resolution**: 1080 x 2460
- **Android Driver**: `focaltech_noflash_common.ko`

### Current Obstacles:
1. **Prebuilt Kernel Stub**:
   - In `mach.development.mt6833`, `hakios_tcm_init()` is a no-op stub (`drivers/core/hakios.stubs.c`). The MT6833 touch code (`bsd/dev/hakios.mt6833_touch.c`) was never compiled into the prebuilt kernel binary.
   - Rebuilding XNU from source (`xnu-src/`) previously produced unbootable kernels and broke userland stability, so a non-destructive integration path is needed.
2. **Scripted Touch (`rwtouch`) Token Limit Fix**:
   - Discovered that `userland/src/init.c`'s `run_command()` has a hard limit of 63 tokens (`char *argv[64]; argc < 63`).
   - Long chained commands in `/redwood/etc/rc.jobs` get truncated, dropping trailing commands and arguments.
   - Fixed by formatting gestures into discrete lines with `sleep` intervals (`userland/rc.jobs.lh8n`).

### Community Prior Art & References:
- **Reddit Reference**: [r/hackios: the github repo with a bunch of repos combined](https://www.reddit.com/r/hackios/comments/1xpcawt/the_github_repo_with_a_bunch_of_repos_combined/) points to `Chromitsune/Portsmith-iOS` and `oosexgang/hakios-xnu-kernel`.
- **Telegram Chat Exports**: `ChatExport_2026-11-07` (`oosexgang comment`). Traced HakiOS 2.3.4 touch latency fix (~5ms finger lift) and SakuraKyuo's Corvette touch implementation (`SakuraKyuo/hackios-xnu-kernel_corvette` commit `120f063a`), clamping coordinates and capping `MOVE` events at 125 Hz.

---

## 3. Backups & Staged Images

- **Stock `boot_a`**: `/home/ydnar/Downloads/boot.img` (40,894,464 bytes)
- **Stock `vendor_boot_a`**: `/home/ydnar/Downloads/vendor_boot_a_backup.img` (67,108,864 bytes)
- **Working Boot Image**: `/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n/xnu-flashed-slot_a.img` (19,820,544 bytes)
- **Working Payload Image**: `/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n/vendor_boot_payload.img` (52,719,616 bytes)
- **Gesture Template**: `/home/ydnar/Documents/LH8n Tree sar/hakios-lh8n/userland/rc.jobs.lh8n`

---

## 4. Next Steps When Resuming

1. **Proper Physical Touch Implementation**:
   - Wire MT6833 SPI4 controller registers (`0x11018000`) and GPIO 14 into XNU without full source recompile (e.g. surgical binary patch hooking `hakios_input_init()` or a targeted external driver shim).
   - Verify SPI4 clocks are kept enabled by Little Kernel across the handover.
2. **Hardware Buttons**:
   - Map volume up/down and power keys via MT6833 PMIC/GPIO registers into `/dev/hakios-touch`.
3. **Audio / Wi-Fi**:
   - Audio DMA (AFE + AW87389 amplifier) and MT6631/MT6833 connsys Wi-Fi initialization.
