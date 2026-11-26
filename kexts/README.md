# `kexts/` — Standalone `UniCam.kext` Source & 1-Click Builder

This directory contains **everything required for the kernel extension (`UniCam.kext`)**: the 3-layer C++ source (`Core` + `IPU4P/SoftISP` + `Virtual UVC`), the IPU4P firmware downloader/checksum script, sensor reference profiles, and a 1-click builder for non-developers.

---

## Getting `UniCam.kext`: Prebuilt Download or Build from Source

- **Fastest method (no compiler required):** Download the prebuilt **`UniCam.kext`** (with Intel IPU4P firmware already embedded, compatible with **macOS 13 Ventura → macOS 26 Tahoe**) from **[GitHub Releases](https://github.com/devduong/UniCam/releases)**.
- **If you want to build from source:**
  1. Double-click **[`Build-UniCam.command`](Build-UniCam.command)** in Finder *(if blocked by Gatekeeper: right-click -> **Open** -> **Open**)*.
  2. The script automatically:
     - Checks for Apple build tools (and triggers the *Xcode Command Line Tools* installer if missing).
     - Downloads and SHA-256 verifies the **Intel IPU4P** firmware from Microsoft Update Catalog via [`scripts/extract_ipu_fw.sh`](UniCam/scripts/extract_ipu_fw.sh).
     - Compresses (`zlib -9`) and embeds the firmware inside `UniCam.kext` via [`scripts/gen_embedded_fw.py`](UniCam/scripts/gen_embedded_fw.py).
     - Compiles, ad-hoc signs, validates (`KEXTBUNDLE`, `_kmod_info`, zero-SIMD), and reveals **`UniCam/build/UniCam.kext`** in Finder.

*(Or via Terminal: `make -C UniCam verify`)*

---

## macOS Support & Required OpenCore EFI Dependencies

- **Supported macOS:** **macOS 13.0 Ventura → macOS 26 Tahoe** (`x86_64`; a single binary build supports all versions thanks to `-mmacosx-version-min=13.0` and stable `IOUSBHostFamily` ABI; verified live on **macOS 26.7.2 Tahoe**, OpenCore **`1.0.9`**). Newer kernels (`Darwin >= 26`) require `-unicambeta`; macOS 12 Monterey and older are not supported.
- **Boot Arguments (`boot-args`):** *(none)* = safe auto-gating on supported IPU4P hardware; `-unicamdbg` = verbose logs + diagnostic fallback UVC when testing new boards; `-unicamtest` = force test-pattern UVC without IPU4P hardware; `-unicambeta` = allow `Darwin >= 26`; `-unicamoff` = disable kext completely.
- **Required components in `EFI/OC/`** (zero files installed into macOS):
  1. **`EFI/OC/Kexts/Lilu.kext`** (`>= 1.6.7`, recommended `>= 1.7.0`, verified **`1.7.2`**) — first in `Kernel -> Add`.
  2. **`EFI/OC/Kexts/VoodooI2C.kext`** (`>= 2.0`, recommended `>= 2.8+`, verified **`2.9.1`**) with both bundled plugins enabled before it: **`VoodooI2CServices.kext`** (`1.0`) and **`VoodooGPIO.kext`** (`1.1`).
  3. **`EFI/OC/Kexts/UniCam.kext`** (`1.0.0`) — **must be ordered AFTER `VoodooI2C.kext`** in `Kernel -> Add`.
  4. **`EFI/OC/ACPI/SSDT-CAM-<Board>.aml`** — generated using the tools in [`../acpi/`](../acpi/README.md).

---

## Directory Layout inside [`UniCam/`](UniCam)

- [`UniCam/Info.plist`](UniCam/Info.plist) — IOKit personalities matching `UNIC0001` (I2C sensor), `UNIC0002` (`TPS68470` PMIC), `IPU0` (`0x8a198086`), `UniCamBridgeService`, and `VoodooI2C` dependencies.
- [`UniCam/Makefile`](UniCam/Makefile) — Standalone kext Makefile that automatically downloads firmware if absent.
- [`UniCam/firmware/`](UniCam/firmware/README.md) — Expected `SHA256SUMS.txt` and Intel IPU4P firmware staging directory (`cpd_component_signed.bin`).
- [`UniCam/profiles/sensors/`](UniCam/profiles/sensors/ov01a10.json) — Sensor calibration/register profiles (`ov01a10.json`, `ov2740.json`, `ov02c10.json`, `ov5693.json`, `ov5675.json`).
- [`UniCam/scripts/`](UniCam/scripts/extract_ipu_fw.sh) — Firmware download and embedding scripts.
- [`UniCam/include/`](UniCam/include/UniCamCoreABI.h) & `UniCam/src/`:
  - `src/Core/` — ACPI/GPIO/Clock HAL (`Discrete GPIO` + `TI TPS68470` I2C PMIC), `VoodooI2C` transport, and sensor drivers (`OV01A10`, `OV01A1B`, `OV2740`, `OV02C10`, `OV5693`, `OV5675`).
  - `src/IPU/` — IPU4P Buttress, MMU, SysCom, CSI-2 PHY, and integer `Q10` SoftISP & 3A (`AE/AWB`) supporting up to `1936x1096`.
  - `src/UVC/` — Virtual Built-in UVC 1.1 device (`0x0C45:0x6723` `"Integrated Webcam HD"`).
