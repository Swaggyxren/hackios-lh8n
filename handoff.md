# hakios-lh8n Session Handoff (`B40`–`B50`)

**Device:** Tecno Pova 5 Pro 5G (`LH8n`, `MT6833`, Mali-G57 MC2 `0x9093`, `1080x2460 @ 120 Hz` panel, serial `H333LH8NU0123456`)
**Base commit:** `ac5fd17` (`mtk` / `release-lh8n`)
**Rule:** Do not run `fastboot flash`, `adb reboot`, or `git commit`/`push` without explicit user request.

---

## 1. Verified Working (Phase A + Phase B)

- **SpringBoard visuals & UFS root:**
  - `userland/var/mobile/Library/Preferences/com.apple.springboard.plist`: `SBReduceTransparency=false`, `SBDisableIconShadows=false`, `BSUIVibrancyEffectViewDisabled=false`; built with `GL=1 REST=1 RC_JOBS=rc.jobs.lh8n` (`userland/build.sh`).
  - `userland/src/init.c`: `stat("/sbin/launchd.darwin", &st) != 0` guard for UFS root boot.
- **Live `MT6359P` battery (`VBAT` + `CHRDET`):**
  - `mtk/loader/mtkloader.c`: reads `0x002a` bit 2 (`CHRDET`) and `0x110e` -> `0x1088` (`(r15 * 6300u) >> 15`), verified in `b42_ramoops.log` (`PMIC VBAT=3670mV SOC=20% chr=1`) and `b46_ramoops.log` (`VBAT=3615mV SOC=14% chr=1`).
- **Backlight & physical keys:**
  - `AppleARMBacklight` (`disp_pwm0 @ 0x1100e000`): verified in `b40_ramoops.log` and on-device brightness slider.
  - `mtk/mt6833/drivers/input/hakios.mt6833_keys.c`: Power + Volume Up + Volume Down verified in `b41_ramoops.log` and `b44_ramoops.log` and confirmed by user.
- **120 Hz zero-copy `OVL0_2L` direct scanout (`fb_zerocopy=1`):**
  - **Root cause of `B43`/`B46` horizontal streaks:** `OVL0_2L` (`0x14006000`) failed when `MiniIOSurface` used `MINISURF_ROW_ALIGN = 64` (`4544 = 0x11c0` bytes/row for `1125` width) and `L0_CON = fOvlConSaved & ~0x100u` (`0x20ff`). Cross-referencing `mtk/mt6833/drivers/gpu/hakios.mt6833_mali.c` and `tools/mesa/panfrost-darwin.patch` confirmed Mali/Panfrost was already using mainline G57 quirks (`SC_VAR_ALGORITHM`, `JM_IDVS_GROUP_SIZE(0xf)`, `AS_MEMATTR_LO=0x4c4c4c8fu`, `JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE`), and `B42` (`fb_zerocopy=0`) proved Panfrost renders clean frames to DRAM.
  - **Fix applied in `B47`–`B50`:**
    - `xnu/files/iokit/Kernel/MiniIOSurface.cpp`: `MINISURF_ROW_ALIGN` set to `128` (`4608` bytes/row for `1125` width).
    - `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`: reverted `B45`'s `DISP_MUTEX` (`fDisp[0x1028 / 4]`) write (which stalled `RDMA`), kept `fb_zc_flush=0` (`gZeroCopyFlush=0`), and restored `fOvl[FLIP_OVL_L0_CON / 4] = fOvlConSaved` (`0x21ff`).
  - **Verified log metrics:**
    - `B46`: `600 frames in 5003 ms (119 a second)`, `0 us` GPU wait, `scans=9941 blits=4 vsync=9943 ok/0 tmo uf=3 (avg 6704 us)`.
    - `B47` (`1125x2436`, `srcX=0`): horizontal streaks **gone**, `scans=8240 blits=2 vsync=8241 ok/0 tmo uf=2 (avg 5275 us)`, active `68–75 FPS`.
    - `B48` (`1125x2436`, `srcX=32`): `scans=11367 blits=3 vsync=11368 ok/0 tmo uf=2 (avg 5744 us)`, active `71–78 FPS` (`381` frames `< 10 ms` in peak window).
    - `B49` (`1080x2460` test): `scans=2104 blits=3 vsync=2105 ok/0 tmo uf=0 (avg 5469 us)`, **but** `1080x2460` breaks MobileGestalt/UIKit layout (Control Center misaligned + `rdar:45025538` red status bar).

---

## 2. Current State on Disk (`B50`)

- Built and packed (not yet checked via `ramoops` after user test):
  - `xnu-flashed-slot_a.B50.img` (`19836928` bytes, `fb_zerocopy=1`)
  - `vendor_boot_payload.B50.img` (`52891648` bytes, `50.44 MB / 64 MB`)
- `B50` settings in `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (synced to `xnu-src/xnu-8796.121.2/iokit/Kernel/`):
  - `kFBDefaultWidth = 1125`, `kFBDefaultHeight = 2436`
  - `srcX = ((s->info.width - vinfo.v_width) / 2) & ~15u` (`= 16` -> `scanPA = phys + 64`)
- User observation on `B50`: clean display (no streaks), still slightly off to the right and `12 px` short of top/bottom bezels (`2436` vs `2460` panel).

---

## 3. Open Items / Next Steps

1. **Pull `B50` `console-ramoops-0`** (`adb -s H333LH8NU0123456 shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'" | tr -cd '\11\12\15\40-\176' > ...`) if needed to confirm `scans`/`uf`/`avg us` with `srcX=16`.
2. **Centering & Bezel Fill (`1125x2436` on `1080x2460` panel):**
   - `OVL0_2L` does 1:1 scanout without scaling, so `1125x2436` leaves `12 px` black bars at top/bottom (`dstY=12`) and crops `45 px` horizontally (`srcX=16` leaves `6.5 px` rightward shift; `srcX=32` leaves `9.5 px` leftward shift; `srcX=22` is exact center `+88` bytes if `OVL0_2L` accepts 8-byte `L0_ADDR` alignment when `bytesPerRow=4608`).
   - To fill top-to-bottom bezels (`1080x2460`) while keeping iOS layout at `1125x2436`, either enable MediaTek `DISP_RSZ0` hardware scaling in the display path or keep `1125x2436` 1:1 (`srcX=22` / `srcX=16`) with `12 px` top/bottom margin.
3. **Logs archived in:** `/home/ydnar/.gemini/antigravity/brain/f603f957-2283-43c8-983c-ae0ee562a71b/scratch/` (`b40_ramoops.log` through `b49_ramoops.log`).
