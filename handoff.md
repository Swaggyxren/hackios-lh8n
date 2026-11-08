# HakiOS Darwin / XNU Bringup on Tecno Pova 5 Pro 5G (LH8n / MT6833) — Handoff Document

**Date**: November 8, 2026  
**Target Hardware**: Tecno Pova 5 Pro 5G (`LH8n`), MediaTek Dimensity 6080 (`MT6833` / `MT6768` family)  
**Kernel Target**: Apple Darwin 22.5.0 / XNU 8796.121.2 (iOS 16.5)  
**Backups Available Locally**:
- Stock `boot_a`: `/home/ydnar/Downloads/boot.img` (40,894,464 bytes)
- Stock `vendor_boot_a`: `/home/ydnar/Downloads/vendor_boot_a_backup.img` (67,108,864 bytes)

---

## 1. What Has Been Achieved (Phase 7: Full Graphical iOS SpringBoard Homescreen Boot)

1. **Bootloader Chain & DRAM Payload Staging**:
   - `mtkloader` chained in Android boot header v4 on slot A.
   - `vendor_boot_payload.img` loaded by Little Kernel DMA into DRAM at `0x51100000`, unpacked at `0x52f0e000`.
   - Kernel memory base `0xc0000000`, entry point `0xc34424a8`.
   - Framebuffer mapped at `0x76950000`, 1080x2460, stride 4352.

2. **Native UFS 2.2 Controller Handover & Storage Root Pivot**:
   - Little Kernel UFS controller (`0x11270000`) handed over cleanly with active transfer lists at `0x80210000` (32 command slots) and shared page at `0x80200000`.
   - Kernel patched (`patch-kernel-mt6893.py`) to bind `UFSHC_PHYS` to `0x11270000`.
   - `init` mounts iOS root filesystem from `/dev/ufs_xnusys` (sdc59) at `/flash`.
   - VFS root switch executed via `kern.hakios_switch_root`: Darwin volume becomes `/`, bootstrap ramdisk mounted on `/private/preboot`.
   - Cryptexes mounted at `/System/Cryptexes/OS` and `/System/Cryptexes/App` for dyld Shared Cache.

3. **PID 1 Handover & Darwin Ignition Sequence**:
   - `init` patches applied at byte offset `0x5a0` (`1f 20 03 d5` NOP) to ensure `base = "/redwood"` even when launched as `/sbin/launchd`.
   - Process 1 hands off cleanly via `/redwood/etc/rc`: `exec /sbin/launchd.darwin`.
   - Apple's `libignition` completes primary ignition.
   - Native Apple system daemons run resident without watchdog timeout (`loader_return_after=0`):
     - `launchd.darwin[1]`
     - `containermanagerd[12]`
     - `usermanagerd[15]`
     - `lsd[26]` (LaunchServices app indexing)
     - `backboardd` (Event dispatch & WindowServer client)
     - `SpringBoard[7]` (iOS Shell / Compositor)

4. **Graphical Homescreen Verified**:
   - Full graphical iOS 16.5 homescreen, wallpaper, dock, and app icons render live on the device panel.

---

## 2. Technical Decisions & Special Fixes

1. **Process 1 `base` Detection Bypass (Offset `0x5a0`)**:
   - Stock `init` checks `strncmp(argv[0], "/redwood/", 9)`. When XNU launches process 1 as `/sbin/launchd`, `argv[0]` mismatch causes `base` to remain empty `""`, breaking all subsequent synthetic linkings and failing `/flash` mount.
   - Patch: Replaced conditional branch (`a0 00 00 35`) at `0x5a0` with `NOP` (`1f 20 03 d5`) in `prebuilt/userland/init`, `userland/root/sbin/init`, and `userland/root/sbin/launchd`.
2. **`XNU_VERBOSE 1` Console Parity**:
   - `mtkloader` maintains `XNU_VERBOSE 1` (`-v` boot-arg, `v_display = 0`), which displays the Apple boot logo and Darwin boot logs directly on screen while allowing SpringBoard to take over the display.
3. **UFS Native Physical Address**:
   - MediaTek MT6833 UFS Host Controller resides at `0x11270000`, differing from Qualcomm standard `0x1d84000`. Patched in `mach.development.mt6833` via `patch-kernel-mt6893.py`.

---

## 3. Next Phase: Input & Peripheral Enablement
1. **Touchscreen Driver**:
   - FocalTech FT8726 multi-touch controller over MT6833 I2C (GPIO 14 IRQ, GPIO 19 RST).
   - Integration into `drivers/input/hakios.input.c` / `/dev/hakios-touch`.
2. **Hardware Key Events**:
   - Volume Up / Down / Power key translation for SpringBoard lock/wake.
