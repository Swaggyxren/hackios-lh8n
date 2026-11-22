# HakiOS on the Tecno Pova 5 Pro 5G (LH8n)

**Branch `mtk`.** HakiOS is an independent research project that runs Apple's open-source Darwin kernel (XNU, `xnu-8796.121.2` / Darwin 22.5.0, the kernel of iOS 16.5) on Android phones. This branch is the port to the **Tecno Pova 5 Pro 5G (`LH8n`)**:

- **SoC:** MediaTek Dimensity 6080 (`MT6833`), 2× Cortex-A76 @ 2.4 GHz + 6× Cortex-A55 @ 2.0 GHz, 8 GB LPDDR4X
- **GPU:** Mali-G57 MC2 (Valhall)
- **Display:** 1080 × 2460 @ 120 Hz, row pitch 4352, OVL0 direct scanout from the Little Kernel framebuffer at `0x76950000`
- **Storage:** UFS 2.2 host controller at `0x11270000`
- **Touch:** FocalTech FT8726 on SPI4 (`0x11018000`, IRQ GPIO 14, reset GPIO 19)

The kernel, its MediaTek drivers, the `mtkloader` bootloader, and the staging tools live here. The Qualcomm device port and QEMU are on the `main` branch of the upstream project, [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel).

---

## What works

| Feature | Status | Implementation |
|---|---|---|
| Boot chain | working | Android boot header v4; `mtkloader` chained in `boot_a`, XNU payload staged in `vendor_boot_a` |
| Homescreen / shell | working | Graphical iOS 16.5 SpringBoard, frosted-glass dock/vibrancy/icon shadows, wallpaper, resident (`loader_return_after=0`, graphics mode) |
| Display | working | 1080 × 2460 @ 120 Hz (`OVL0_2L` triple-buffered `fb_flip=1` and zero-copy `IOSurface` direct scanout `fb_zerocopy=1` with 128-byte row alignment, up to 119 FPS) |
| Brightness / backlight | working | `AppleARMBacklight` driving MediaTek `disp_pwm0 @ 0x1100e000` (Control Center & Lock Screen brightness slider) |
| CPU | working | All 8 cores — 2× Cortex-A76 @ 1993 MHz (stock OPP #4 via MT6833 v2 PWRAP) + 6× Cortex-A55 @ 2000 MHz |
| Storage / root | working | Native UFS 2.2 handover; APFS root on `ufs_xnusys` mounted read-only |
| Touch / input | working | FT8726 over SPI4; boot-ROM firmware download, then live `DOWN`/`UP` events reach SpringBoard |
| Buttons | working | Power key (`MT6359P` PMIC `TOPSTATUS 0x002a` via PWRAP v2 `SWINF_2`), Volume Up (`KP_MEM1` bit 0) & Volume Down (`GPIO 42` / `KP_MEM1`) |
| Battery / charger | working | Live `MT6359P` AUXADC `VBAT` + `CHRDET` read in `mtkloader` (`0x80240000` shared block) -> `AppleARMPMUCharger` (`IOPMPowerSource`) |
| GPU | working | Hardware Mali-G57 MC2 @ 1000 MHz (`MFGPLL`, `HakiosPanfrostGPU` + Mesa 25.0.7 Panfrost, WC BOs, fast job poll, 0 faults) |
| Sensors | staged | Accelerometer via the SCP sensor hub |
| Wi-Fi / BT | staged | CONNSYS initialization |
| Apple services | no | Not a goal |

---

## How it boots

MediaTek's Little Kernel (LK) does not offer a clean `fastboot boot` into RAM, so HakiOS uses a chainboot trampoline:

```
boot_a (Android Boot Header v4)
  ├── mtkloader.bin        bare-metal ARM64 program, chained in front of the kernel
  └── stock Linux Image    the phone's own Android kernel, untouched
vendor_boot_a
  └── vendor ramdisk + XNUP payload (HakiOS kernel + root.apfs)
```

1. LK loads `boot_a`'s kernel — which is `mtkloader` — and jumps to it.
2. `mtkloader` does early hardware setup (clocks, framebuffer, UFS handover, device tree), and on a one-shot request written to `/dev/pmsg0` starts XNU instead of chainbooting Android. With no request it boots Android normally.
3. The XNU kernel and its RAM disk are read from the vendor ramdisk that LK DMA-loads into DRAM (`0x51100000`); `mtkloader` finds the `XNUP` header in that initrd and hands the payload to the kernel.
4. XNU boots with a slide, mounts the APFS root from `ufs_xnusys`, and launches `launchd`, which brings up SpringBoard.

By default HakiOS stays resident on the homescreen (`loader_return_after=0`). Builds made for log capture set `loader_return_after=120`: XNU writes its log to the ramoops console zone, `mtkloader` copies it to pstore, and the phone returns to Android after two minutes.

---

## Storage and partitions

HakiOS does **not** overwrite Android's system, vendor, or boot partitions, and it does not wipe user data. The iOS userland lives in its own partition on the phone's internal UFS storage:

- **`xnusys`** — a dedicated ~4 GiB partition (`sdc59`, GPT entry 58) holding an **APFS container** with the iOS 16.5 root filesystem (`system.apfs`). The APFS superblock magic `NXSB` appears at byte offset 32 of its first block.
- The partition is carved out of the tail of `userdata`. `mtk/tools/gpt-carve.py` shrinks the `userdata` partition while preserving the F2FS filesystem (the owner resizes F2FS in recovery first), and `mtk/tools/system-flash.sh` writes `system.apfs` into `xnusys` from Android.
- At boot, `mtkloader` performs a native handover of the UFS host controller at `0x11270000` (32 command descriptors at `0x80210000`, shared block at `0x80200000`). XNU's own UFS driver reads `ufs_xnusys` at full hardware speed; the loader's small shared page is only a fallback.
- `init` mounts `/dev/ufs_xnusys` read-only as `/flash` and pivots the VFS root there, after which `launchd.darwin` takes over as PID 1.

For early bringup the whole system can instead run from a RAM disk (`root.apfs` staged inside the `XNUP` payload), so no partition needs to be carved at all. That is the mode used by the flash commands below.

---

## Quick start

### Prerequisites

- Tecno Pova 5 Pro 5G (`LH8n`) with an unlocked bootloader and Magisk/KernelSU root
- Stock backups of `boot_a` and `vendor_boot_a`
- For the full userland: `system.apfs` written to `xnusys`

### Build and flash

```bash
make lh8n_defconfig
make firmware                     # pull firmware from your own phone
mtk/tools/mk-ft8726-fw.py         # generates the touch driver's firmware header (not tracked)
make kernel                       # hakios-kernel
make payload                      # build-payload.bin (kernel + root.apfs)
make boot-image                   # xnu-flashed-slot_a.img
```

The FT8726 touch firmware is vendor property and is never committed. Before `make kernel`,
generate `mtk/mt6833/drivers/input/hakios.ft8726_fw.h` from the `focaltech_ts_fw_00.bin` you
pulled from your own phone with `mtk/tools/mk-ft8726-fw.py`.

Then flash the loader in `boot_a` and the payload in `vendor_boot_a`:

```bash
fastboot flash boot_a xnu-flashed-slot_a.img
fastboot flash vendor_boot_a vendor_boot_payload.img
fastboot reboot
```

To restore stock Android, flash back the `boot_a` and `vendor_boot_a` backups.

---

## Repository layout

- `mtk/loader/` — `mtkloader`, the bare-metal chainboot program, and its build scripts
- `mtk/mt6833/` — MT6833 SoC drivers (UFS, touch, Mali, audio, CCCI, CONNSYS)
- `mtk/tools/` — boot-header packers, partition tools, probes and run scripts
- `drivers/<subsystem>/` — the port's XNU drivers, organized by subsystem (see `drivers/map`)
- `xnu/` — patches and new files applied to Apple's XNU tag by `xnu/apply.sh`
- `userland/` — programs and libraries of ours, launchd jobs, APFS image builder
- `configs/`, `devices/lh8n/` — kernel configuration and the device profile
- `tools/`, `docs/` — configuration tools, checks, and documentation

See [docs/index.md](docs/index.md) for the full documentation.

---

## Independent research notice

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**, MediaTek, or Tecno. "Darwin", "iOS", "iPhone", "XNU", and "Apple" are used solely to describe system compatibility. No Apple software is distributed here; the Darwin userland is laid out by the user from their own legally obtained copy.

## Credits

- **[`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel)** — upstream HakiOS XNU kernel, `mtkloader`, `MiniIOMobileFramebuffer`, and MediaTek bring-up framework.
- **[`Luminous418/hakios-a32`](https://github.com/Luminous418/hakios-a32)** — Mesa 25.0.7 Panfrost Darwin patchset (`panfrost-darwin.patch`), `hakios.drm.c` Panfrost DRM shim, and Panfrost userland GL integration.
- **[`open-mapple`](https://github.com/open-mapple)** (`@DrtSinX98` / Pritish Joshi) & **[`Nik300/viva-gpu`](https://github.com/Nik300/viva-gpu)** — Mali-G57 hardware bring-up reference (stack power-on sequence, Valhall AArch64 MMU/PTE validation, and GPU architecture references).
- **ryanistar — [`Xia-s-Projekt/hakios-x6739`](https://github.com/Xia-s-Projekt/hakios-x6739)** (`X6739` / MT6893 HakiOS port) — CoreAnimation 120 Hz environment variable set and frame-pacing diagnostics.
- Special thanks to **sicilium** for the sources and all upstream HakiOS contributors.
