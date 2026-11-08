# HakiOS for MediaTek: Infinix Hot 20 & Tecno Pova 5 Pro 5G

**This is the branch `mtk`**: HakiOS -- Apple's open-source Darwin kernel (XNU, `xnu-8796.121.2`) on Android phones -- for devices with **MediaTek** SoCs:
1. **Infinix Hot 20 (X6826B)**: Helio G85 (`MT6768`), Mali-G52 MC2, eMMC 5.1, 6 GB RAM.
2. **Tecno Pova 5 Pro 5G (LH8n)**: Dimensity 6080 (`MT6833`), Mali-G57 MC2, UFS 2.2, 8 GB RAM.

The kernel, its MediaTek drivers, the `mtkloader` bootloader, and staging scripts are located here. The Qualcomm phone (POCO X5 Pro 5G) and QEMU are on branch [`main`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main).

---

## What Works

| Feature | Infinix Hot 20 (`x6826b`) | Tecno Pova 5 Pro 5G (`LH8n`) | Technical Implementation |
|---|---|---|---|
| **Boot Chain** | yes | yes | **LH8n**: Android boot header v4; `mtkloader` in `boot_a`, XNU payload unpacked from `vendor_boot_a` |
| **Homescreen / Shell** | yes | yes | Full graphical iOS 16.5 SpringBoard, wallpaper, dock, and icons resident (`loader_return_after=0`) |
| **Display** | yes | yes | **LH8n**: 1080 × 2460 @ 120 Hz, row pitch 4352, OVL0 direct hardware scanout at `0x76950000` |
| **Touch / Input** | yes | in progress | **LH8n**: FocalTech FT8726 on SPI4 (`0x11018000`, IRQ GPIO 14); automated `rwtouch` gestures verified |
| **Buttons** | yes | staged | Power & volume keys via MT6833 GPIO/PMIC |
| **CPU** | yes (4 cores) | yes (8 cores) | **LH8n**: 2x Cortex-A76 @ 2.4 GHz + 6x Cortex-A55 @ 2.0 GHz, 8 GB LPDDR4X |
| **GPU** | yes | staged | Mali-G57 MC2 bringup via `HakiosMali` (`mali=1`) |
| **Storage** | read only | read only | **LH8n**: Native UFS 2.2 handover at `0x11270000` (sdc59 / `ufs_xnusys` as root APFS) |
| **Sensors** | partly | staged | Accelerometer via SCP sensor hub (`MTK_SCP`) |
| **Wi-Fi / BT** | partly | staged | CONNSYS initialization on MT6833 |
| **Apple Services** | no | no | Not a goal |

---

## Tecno Pova 5 Pro 5G (LH8n / MT6833): Port Architecture & Deviations from Stock

The Tecno Pova 5 Pro 5G (`LH8n`) runs on the MediaTek Dimensity 6080 (`MT6833`). While it shares architectural lineage with the MT6768, booting modern Darwin 22.5.0 into a resident SpringBoard homescreen required several critical departures from upstream HakiOS:

### 1. Android Boot Header v4 & `vendor_boot` Payload Staging
* **Upstream**: Single Android boot header v2 image combines `mtkloader`, Android kernel, and the XNU payload in `boot_a`.
* **LH8n Fix**: The MT6833 uses Android Boot Image Header v4, where `boot_a` contains only the kernel image, and all ramdisks, DTB, and boot arguments reside in `vendor_boot_a`.
  - `mtkloader` is flashed as the kernel in `xnu-flashed-slot_a.img`.
  - The XNU payload (`mach.development.mt6833` + `userland/root.apfs`) is repacked into the vendor ramdisk via `mtk/tools/repack_vendor_boot.py` into `vendor_boot_payload.img`. Little Kernel DMA loads it into DRAM at `0x51100000`, and `mtkloader` detects the `XNUP` header at `0x52f0e000`.

### 2. Native UFS 2.2 Controller Handover (`0x11270000`)
* **Upstream**: Qualcomm platforms use UFSHCI at `0x1d84000`; MT6768 uses eMMC (`MSDC0`).
* **LH8n Fix**: The MT6833 utilizes UFS 2.2 storage mapped at `0x11270000`.
  - The kernel binary was patched (`tools/patch-kernel-mt6893.py`) to remap `UFSHC_PHYS` to `0x11270000`.
  - `mtkloader` configures the UFS controller handover: 32 command descriptor slots at `0x80210000` and a shared block at `0x80200000`. This allows XNU's native UFS driver to read partition 59 (`sdc59` / `ufs_xnusys`) at full hardware speed rather than falling back to loader polling.

### 3. PID 1 Base Path Patch (Offset `0x5a0` ARM64 NOP)
* **Upstream**: `userland/src/init.c` expects `argv[0]` to start with `"/redwood/"` to identify the base path.
* **LH8n Fix**: In standard XNU, process 1 is launched directly as `/sbin/launchd`. Because `argv[0]` is `/sbin/launchd`, `init`'s `base` path check failed and left `base` as `""`, corrupting synthetic directory generation and failing the `/flash` root pivot.
  - Byte offset `0x5a0` in `prebuilt/userland/init`, `userland/root/sbin/init`, and `userland/root/sbin/launchd` was patched with an ARM64 `NOP` (`1f 20 03 d5`), forcing `base = "/redwood"` regardless of `argv[0]`.

### 4. Direct Display Scanout & Framebuffer (`0x76950000`)
* **Upstream**: 720 × 1640 panel on MT6768 with software flipping.
* **LH8n Fix**: 1080 × 2460 display at 120 Hz with a row pitch of 4352 bytes. `mtkloader` configures MediaTek OVL0 registers (`L0_CON = 0xff`, `SRC_CON = 0x1`) to point the hardware scanout layer directly to XNU's framebuffer at `0x76950000`, eliminating intermediate copy overhead.

### 5. Watchdog Suppression & Resident Execution
* **Upstream**: Testing cycles use `loader_return_after=N` to reboot into Android after running tests to inspect `pstore` logs.
* **LH8n Fix**: Set `loader_return_after=0` to keep SpringBoard and the window server running resident indefinitely without auto-reboot.

### 6. Userland Command Runner Token Limit (`init.c`)
* **Upstream Bug**: `userland/src/init.c`'s `run_command()` caps line arguments at 63 tokens (`char *argv[64]; argc < 63`).
* **LH8n Fix**: Chained automation scripts (such as multi-step `rwtouch` sequences) were silently truncated, dropping trailing arguments and `&` background operators. Scripts in `rc.jobs` must be structured as discrete, single-command lines with `sleep` intervals (`userland/rc.jobs.lh8n`).

---

## The Drivers

The MediaTek SoC layers are organized under `mtk/`:
- `mtk/mt6768/`: MT6768 (Helio G85) drivers (eMMC, TD4160 touch, Mali-G52).
- `mtk/mt6833/`: MT6833 (Dimensity 6080) drivers (UFS 2.2, FT8726 touch, Mali-G57, CCCI 4G LTE, AFE audio).

| Driver | Symbol | Boot-argument | Description |
|---|---|---|---|
| `mtk/mt6833/drivers/storage/hakios.mt6833_ufs.c` | `MTK_UFS` | `ufs_root=1` | MT6833 UFS 2.2 Host Controller at `0x11270000` |
| `mtk/mt6833/drivers/input/hakios.mt6833_touch.c` | `MTK_TOUCH` | `touchscreen=1` | FocalTech FT8726 multi-touch over SPI4 (`0x11018000`) |
| `mtk/mt6833/drivers/input/hakios.mt6833_keys.c` | `MTK_KEYS` | `buttons=2` | MT6833 Power & Volume keys |
| `mtk/mt6833/drivers/gpu/hakios.mt6833_mali.c` | `MALI` | `mali=1` | Mali-G57 MC2 GPU driver |
| `mtk/mt6833/drivers/audio/hakios.mt6833_audio.c` | `MTK_AUDIO` | `sound=1` | AFE DMA audio controller + AW87389 amplifier |
| `mtk/mt6833/drivers/cellular/hakios.mt6833_ccci.c` | `MTK_CCCI` | `ccci=1` | MediaTek Cross Core Cellular Interface (4G LTE) |
| `mtk/mt6833/drivers/wifi/hakios.mt6833_connsys.c` | `MTK_CONNSYS` | `connsys_step=N` | CONNSYS Wi-Fi & Bluetooth controller |

---

## Quick Start (LH8n)

### Prerequisites:
- Tecno Pova 5 Pro 5G (`LH8n`) with unlocked bootloader and Magisk/KernelSU root.
- Stock backups of `boot_a` and `vendor_boot_a`.
- Target system image installed in partition 59 (`ufs_xnusys`).

### Flash & Run:
```bash
# 1. Flash mtkloader into boot_a
fastboot flash boot_a "xnu-flashed-slot_a.img"

# 2. Flash XNU payload into vendor_boot_a
fastboot flash vendor_boot_a "vendor_boot_payload.img"

# 3. Boot into iOS 16.5 SpringBoard
fastboot reboot
```

---

## Independent Research Notice

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**, MediaTek, or Tecno; "Darwin", "iOS", "iPhone", "XNU", and "Apple" are named solely to describe system compatibility.

## Owners & Credits

- @oodze - founder
- @netusd - advertising / ceo-dev
- @wannqn - dev 
- @hxfuxyyXD - dev
- @wryzzew - dev
- Rhaenja Hun（杭润嘉） — Developer / Sponsor
- Special thanks to sicilium for the sources!
