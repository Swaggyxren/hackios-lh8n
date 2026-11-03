# HakiOS Darwin / XNU Bringup on Tecno Pova 5 Pro 5G (LH8n / MT6833) — Handoff Document

**Date**: November 3, 2026  
**Target Hardware**: Tecno Pova 5 Pro 5G (`LH8n`), MediaTek Dimensity 6080 (`MT6833` / `MT6768` family)  
**Kernel Target**: Apple Darwin 22.5.0 / XNU 8796.121.2  
**Backups Available Locally**:
- Stock `boot_a`: `/home/ydnar/Downloads/boot.img` (40,894,464 bytes)
- Stock `vendor_boot_a`: `/home/ydnar/Downloads/vendor_boot_a_backup.img` (67,108,864 bytes)

---

## 1. What Has Been Achieved (Solid Progress)
1. **Full Kernel Initialization**:
   - `hakios-kernel` boots cleanly via `mtkloader` chained in Android boot header v4 on slot A.
   - LK device tree (DTB) and framebuffer (`0x76950000`, 1080x2460, stride 4352) correctly mapped.
   - Pmap, VM zones, CPU start on core 0, and BSD VFS subsystem mount cleanly.
2. **UFS Driver Bypass via DRAM Payload Staging**:
   - Instead of needing an incomplete bare-metal UFS/eMMC driver, `vendor_boot_payload.img` is loaded directly by Little Kernel DMA into DRAM at `0x51100000`.
   - `mtkloader` reliably finds the `XNUP` payload at `0x52f0e000` (`PAGE_SIZE` aligned), unpacking the kernel and mockfs ramdisk.
3. **Fail-Safe Gate / Zero Bootloops**:
   - DRAM marker at `0x4812f000` tracks state (`MARK_RUNNING = 0x4e52584e`, `MARK_DONE = 0x4e58444e`).
   - If XNU finishes or panics, `xnu_returned()` sets `MARK_DONE` and warm-resets the SoC.
   - On the subsequent boot, `mtkloader` detects `MARK_DONE`, resets the mark to 0, and immediately chains into stock Android kernel via `chain_android()`.
4. **Verified EL0 User Space Execution (Step 1820 Proof)**:
   - With pristine `hello.orig`, `/sbin/launchd` mounted via mockfs memory disk `md0` and executed at EL0:
     ```
     load_init_program: attempting to load /sbin/launchd
     Hello from user space on Darwin!
     I am process 1, uid 0, console fd 0.
     pid 1 exited -- no exit reason available -- (signal 0, exit 0)
     ```

---

## 2. Where We Are Currently Stuck & The Root Problems
1. **The PID 1 Lifecycle Dilemma**:
   - In Darwin/XNU, `/sbin/launchd` is process 1. If PID 1 ever exits, XNU considers it an unrecoverable failure and panics (`initproc failed to start -- (signal 0, exit status 0)`).
   - In `hello.orig`, `_start` prints the greeting and immediately issues `SYS_exit(0)`.
   - This causes XNU to panic after 0.006 seconds. Because `xnu_returned()` immediately triggers a warm reset into Android, the console output disappears in milliseconds, showing only "starting Android" to the naked eye.
2. **The `SIGILL` (Signal 4 / Subcode 4) Trap**:
   - To make PID 1 stay alive for 60 seconds (so the user can observe the greeting on screen), we patched `hello`.
   - However, modern XNU (Darwin 22.5) enforces strict W^X and Mach-O section mappings for EL0 processes:
     - Branching to offset `0x600` violated the declared `__text` section boundary (`0x338..0x4bc`), causing an EL0 Instruction Permission Fault -> `SIGILL`.
     - In-place replacement within `0x428..0x4bc` conflicted with `_start` calling `bl _put_number` at `0x440` to format UID/PID numbers.
3. **Screen Persistence / Framebuffer Handoff**:
   - The Little Kernel framebuffer is static. When `mtkloader` chains back to Android, LK reinitializes or Android kernel clears the display, hiding previous text.

---

## 3. Recommended Next Steps for Future Sessions
1. **Build a Native, Standalone C `hello` with Proper Clang/Mach-O Toolchain**:
   - Install `theos` / `cctools-port` so `xnu-clang` and `ld64` can link `userland/src/hello.c` directly with a clean `hello_wait=60` loop, instead of raw binary hex editing.
2. **Or Use Stock `hello.orig` with Delay in Loader**:
   - Alternatively, keep `hello.orig` completely untouched (which is proven to work), but insert a 30-second delay in `mtkloader`'s `xnu_returned()` BEFORE triggering `reset_board()`. This allows the XNU greeting to remain visible on the LCD screen for 30 seconds before returning to Android!
