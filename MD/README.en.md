> 🌐 **Language / Ngôn ngữ:** **🇬🇧 English** *([Root README](../README.md))* | **[🇻🇳 Tiếng Việt](README.vi.md)**

# UniCam — MIPI CSI-2 Webcam Enabler for Intel Laptops on macOS (OpenCore EFI)

**UniCam** enables integrated MIPI CSI-2 webcams built on **Intel Ice Lake IPU4P (`8086:8a19`)** on macOS using a **100% Pure-EFI Zero-Install** architecture:
- Everything lives inside your **OpenCore EFI** partition (`EFI/OC/ACPI/` + `EFI/OC/Kexts/`).
- **Zero** user-space apps, background services, LaunchDaemons, or system extensions installed into macOS.
- Works natively in **Photo Booth**, **FaceTime**, **QuickTime Player**, and web browsers via Apple's built-in `UVCAssistant`, including automatic **white Privacy LED** control and clean S3/S0ix **Sleep/Wake** resume.
- Supports multiple OmniVision sensors (**`OV01A10`**, **`OV01A1B`**, **`OV2740`**, **`OV02C10`**, **`OV5693`**, **`OV5675`**) and both power topologies (**Discrete GPIO `INT346F/INT3472`** and **TI `TPS68470` I2C PMIC `UNIC0002`**).

> [!NOTE]
> **Verified Live Hardware:** **Dell XPS 13 7390 2-in-1** (`Intel Ice Lake IPU4P 8086:8a19` + `OV01A10`, Port 6 1-lane, macOS 26.7.2 Tahoe, OpenCore `1.0.9`) — direct RAW10 capture, in-kernel SoftISP, and smooth live video in **Photo Booth**. Other **IPU4P `8086:8a19`** combinations *(such as `OV5693 + TPS68470` on Surface Pro 7, or `OV2740`/`OV02C10`/`OV5675`)* already include mainline-derived drivers and **await real-hardware confirmation** from community testers (see Section 3 below).

---

## 1. OS Requirements, Kext Dependencies & EFI Layout

### 1.1. Supported macOS Versions (`x86_64`)

| macOS Version | Status | Technical Notes |
| :--- | :--- | :--- |
| **macOS 26 Tahoe** *(e.g. `26.7.2`, build `25G332`)* | **Verified Working** | Compiled and tested live with `UVCAssistant` (Photo Booth + Privacy LED + Sleep/Wake). |
| **macOS 15 Sequoia / 14 Sonoma / 13 Ventura** | **Supported via Single Build** *(needs confirmation)* | Built with `-mmacosx-version-min=13.0` (`LSMinimumSystemVersion = 13.0`). **One single `UniCam.kext`** from [**Releases**](https://github.com/devduong/UniCam/releases) works across all supported versions *(just ensure `Lilu.kext` & `VoodooI2C.kext` match your macOS version)*. |
| **Newer / Beta macOS (`Darwin >= 26`)** | **Gated (Requires `-unicambeta`)** | Refuses to attach on untested future kernels by default to prevent `IOUSBHostFamily` ABI panics; add `-unicambeta` to `boot-args` to test. |
| **macOS 12 Monterey and older** *(12 / 11 / 10.15)* | **Not Supported** | Older releases use legacy `VDCAssistant` and lack the `IOUSBHostFamily` / `UVCAssistant` stack of macOS 13+. |

### 1.2. Required Kexts, Tested Versions & Load Order in `EFI/OC/`

Only the following files are needed inside your OpenCore EFI partition (`EFI/OC/`), ordered as listed in `config.plist`:

| Component & Bundle ID | Minimum / Recommended | Live-Verified Version | Order & Role |
| :--- | :--- | :--- | :--- |
| **OpenCore Bootloader** | `>= 0.9.5` *(`>= 1.0.2` for Sequoia/Tahoe)* | **`1.0.9`** | Injects ACPI SSDT & kexts cleanly from EFI (`Pure-EFI`). |
| **`Lilu.kext`** *(in EFI)*<br>`as.vit9696.Lilu` | `>= 1.6.7` *(`>= 1.7.0` for macOS 15/26)* | **`1.7.2`** | **`Kernel -> Add` #1 (first):** Foundational Hackintosh kext. |
| **`VoodooI2CServices.kext`** *(plugin inside `VoodooI2C.kext`)*<br>`com.alexandred.VoodooI2CServices` | Bundled with `VoodooI2C` | **`1.0`** *(build `1`)* | **`Kernel -> Add` before `VoodooI2C`:** Provides I2C service layer for `VoodooI2C`. |
| **`VoodooGPIO.kext`** *(plugin inside `VoodooI2C.kext`)*<br>`org.coolstar.VoodooGPIO` | Bundled with `VoodooI2C` | **`1.1`** | **`Kernel -> Add` before `VoodooI2C`:** Intel PCH GPIO controller driver (`INT3455`). |
| **`VoodooI2C.kext`**<br>`com.alexandred.VoodooI2C` | ABI `>= 2.0`, recommended `>= 2.8` *(`>= 2.9.x` for macOS 15/26)* | **`2.9.1`** | **`Kernel -> Add` right before `UniCam.kext`:** Required to create `VoodooI2CDeviceNub` for the sensor (`UNIC0001`) and PMIC (`UNIC0002`). |
| **`UniCam.kext`** *(this project)*<br>`com.unicam.UniCam` | **`1.0.0`** | **`1.0.0`** | **`Kernel -> Add` AFTER `VoodooI2C.kext`:** Unified kext handling power/I2C, **embedded IPU4P firmware**, `Q10` SoftISP, and Virtual Built-in UVC. |
| **`SSDT-CAM-<Board>.aml`** | Per laptop model | `SSDT-CAM-Dell7390.aml` | **`EFI/OC/ACPI/` (`ACPI -> Add`):** Board-specific GPIO pins, `MCLK`, I2C bus, and MIPI CSI-2 port (created with [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)). |

- **BIOS Setting:** Ensure *Camera / Integrated Webcam* is **Enabled**.
- **Inside macOS:** **Install zero extra files, apps, or LaunchDaemons** (`zlib`-compressed IPU4P firmware is already embedded inside `UniCam.kext`).
- *(Built-in Apple `OSBundleLibraries` — no external download needed: `IOACPIFamily >= 1.4`, `IOPCIFamily >= 2.9`, `IOUSBHostFamily >= 1.2`, `AppleUSBCommon >= 1.0`, KPI `14+`.)*

---

## 2. Quick Start Guide (No Coding Required)

The repository is organized into **2 self-contained folders**, each with a **double-clickable** Finder launcher:
- [`kexts/`](../kexts/README.md): Contains all kext source code, firmware downloader, sensor profiles, and the 1-click **[`Build-UniCam.command`](../kexts/Build-UniCam.command)** builder.
- [`acpi/`](../acpi/README.md): Contains [`SSDT-CAM-DUMP.aml`](../acpi/SSDT-CAM-DUMP.aml) and the interactive **[`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)** wizard to probe BIOS pinouts and compile `SSDT-CAM-<Board>.aml`.

### Step 1 — Download Prebuilt `UniCam.kext` *(or Build from Source)*
- **Option A — Recommended (no Xcode required):**
  1. Go to **[GitHub Releases](https://github.com/devduong/UniCam/releases)** and download the latest release `.zip`.
  2. Extract **`UniCam.kext`** *(one universal binary for **macOS 13 Ventura → macOS 26 Tahoe**, with Intel IPU4P firmware already embedded inside)*.
- **Option B — Build from source:**
  1. Open [`kexts/`](../kexts/README.md) and double-click **[`Build-UniCam.command`](../kexts/Build-UniCam.command)** *(if Gatekeeper prompts: right-click -> **Open** -> **Open**)*.
  2. If Apple's compiler tools are missing, the *Xcode Command Line Tools* installer will open -> click **Install**, then re-run `Build-UniCam.command`.
  3. The script automatically downloads and verifies the official Intel/Microsoft firmware, embeds it into the kext, and outputs **`kexts/UniCam/build/UniCam.kext`**.

### Step 2 — Obtain `SSDT-CAM-<Board>.aml` for Your Laptop

> [!IMPORTANT]
> **Choose ONLY ONE of the 3 options below** matching your hardware *(do not perform all 3)*:

- **Option 1 — Prebuilt SSDT available (Dell XPS 13 7390 2-in-1):**
  - Download **[`SSDT-CAM-Dell7390.aml`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml)** directly *(no probing or compilation required)* and proceed to **Step 3**. *(Or run [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command) -> option **`[3]`** to copy it to `acpi/output/`.)*

- **Option 2 — Automated Wizard [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command) *(Recommended for new IPU4P laptops, no coding needed)*:**
  - *Prerequisite:* Install [`MaciASL.app`](https://github.com/acidanthera/MaciASL/releases) in `/Applications` *(or `brew install acpica`)* so the wizard can call `iasl`.
  - Double-click **[`acpi/UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)** *(if Gatekeeper prompts: right-click -> **Open** -> **Open**)*.
  - **If you do NOT know your BIOS pinout yet** *(2-step auto-probe)*:
    1. *Run 1:* Choose **`[1]`** to generate **`acpi/output/SSDT-CAM-DUMP.aml`** -> copy to `EFI/OC/ACPI/`, enable it in `config.plist`, and **Reboot** macOS.
    2. *Run 2:* Re-open `UniCam-SSDT.command` and choose **`[2]`** -> reads the live BIOS dump, lets you choose your sensor model, and compiles **`acpi/output/SSDT-CAM-<Board>.aml`**.
    3. Open `config.plist` and **DISABLE (`Enabled = False`) `SSDT-CAM-DUMP.aml`** (keep only `SSDT-CAM-<Board>.aml`).
  - **If you ALREADY know your pinout** *(from DSDT/Windows/Linux)*: Choose **`[4]`** directly in `UniCam-SSDT.command` to enter your parameters and compile `.aml` immediately *(skips `SSDT-CAM-DUMP`)*.

- **Option 3 — Manual Creation *(Skip `UniCam-SSDT.command`, for experienced MaciASL or CLI users)*:**
  - **Method A — Edit manually in `MaciASL.app`:**
    1. Open [`acpi/templates/SSDT-CAM-Static.dsl`](../acpi/templates/SSDT-CAM-Static.dsl) *(static DW0 addresses)* or [`acpi/templates/SSDT-CAM-IntelGNVS.dsl`](../acpi/templates/SSDT-CAM-IntelGNVS.dsl) *(dynamic BIOS GNVS lookup)* in `MaciASL.app`.
    2. Update `Scope (\_SB.PCI0.I2Cx)` *(or `PC00`)*, `I2cSerialBusV2` address, sensor metadata (`unicam-sensor-hid` / `unicam-sensor-compat`), MIPI CSI-2 port & lanes, `MCLK`, and your board's GPIO DW0 / `TPS68470` PMIC registers.
    3. Select **File -> Save As -> File Format: ACPI Machine Language Binary (`.aml`)** and save as `SSDT-CAM-<Board>.aml`.
  - **Method B — Generate via Terminal CLI:** See the `parse_cam_dump.py` and `gen_board_ssdt.py` commands at the bottom of [`acpi/README.md`](../acpi/README.md).

### Step 3 — Install to OpenCore EFI & Open Photo Booth
1. Copy **`SSDT-CAM-<Board>.aml`** into `EFI/OC/ACPI/` and **`UniCam.kext`** into `EFI/OC/Kexts/`.
2. Open `EFI/OC/config.plist` (via ProperTree or OpenCore Configurator):
   - **`ACPI -> Add`**: Add & enable `SSDT-CAM-<Board>.aml` (ensure `SSDT-CAM-DUMP.aml` is disabled).
   - **`Kernel -> Add`**: Ensure load order is **`Lilu.kext`** → **`VoodooI2C.kext`** *(+ plugins)* → **`UniCam.kext`**.
3. **Reboot** macOS, open **Photo Booth** (or QuickTime / FaceTime), and test **Integrated Webcam HD**.

#### OpenCore Boot Arguments (`boot-args`) for Testing & Safety

Add any of the flags below to `config.plist` → `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> boot-args` as needed:

| Boot Argument | Behavior | When to Use |
| :--- | :--- | :--- |
| *(none — default)* | Attaches Virtual UVC and powers camera rails **only** when Ice Lake `IPU4P (8086:8a19)` + `SSDT-CAM` + a supported sensor are present on macOS 13–26; stays safely dormant otherwise. | Daily driver / normal use. |
| **`-unicamdbg`** | Enables verbose `UniCam` kernel logs, allows diagnostic Virtual UVC fallback on `IPU4P` boards during SSDT bring-up, and unlocks manual CLI power/probe on unmapped sensors. | **Recommended when testing new IPU4P laptops (Section 3.2)** or diagnosing black screen / no capture. |
| **`-unicamtest`** | Forces the Virtual UVC device (`Integrated Webcam HD`) to attach in color-bar test pattern mode even without `IPU4P` or `SSDT-CAM` hardware. | Isolated testing of `UVCAssistant` / app integration without camera hardware. |
| **`-unicambeta`** | Overrides the Darwin kernel version cap so `UniCam.kext` loads on future/beta macOS releases (`Darwin >= 26`). | Testing on unreleased or newer macOS versions beyond macOS 26 Tahoe. |
| **`-unicamoff`** | Completely disables all `UniCam` kext services in `probe()`/`start()`. | Emergency safe-boot bypass in OpenCore if troubleshooting boot issues. |

---

## 3. Intel IPU4P Laptop Compatibility Matrix

### 3.1. Verified Working on Real Hardware

| Laptop Model | CPU / IPU PCI ID | Power / Clock Topology | Sensor (ACPI HID) | MIPI CSI-2 Link | Status & EFI Profile |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Dell XPS 13 7390 2-in-1** *(2-in-1 convertible model with IPU4P)* | Core 10th Gen Ice Lake<br>**IPU4P `8086:8a19`** | Discrete GPIO (`INT346F`, `C1TP=1`) + PCH `IMGCLKOUT_1` (`19.2 MHz`) + Privacy LED | **OmniVision `OV01A10`**<br>(`OVTI01A0`, `1280x800`) | **Combo PHY Port 6**, `1-lane`, `400 MHz` | **Live Verified** (Photo Booth + Privacy LED + Sleep/Wake). Use [`SSDT-CAM-Dell7390.aml`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml). |

---

### 3.2. Supported in Kext & ACPI — Pending Community Hardware Confirmation

The kext and ACPI toolchain **fully support** **Intel Ice Lake `IPU4P` (`8086:8a19`)** across both PHY blocks (**D-PHY** Ports `3`, `8..12` and **Combo PHY** Ports `6`, `7`), both power topologies (**Discrete GPIO** and **TI `TPS68470` I2C PMIC `UNIC0002`**), and **SoftISP up to `1936x1096`** for the 6 OmniVision sensors below. Since we only own the Dell XPS 7390 2-in-1, community confirmation is welcome for:

| IPU4P Hardware Combination (`8086:8a19`) | Common Laptop Examples | Pre-Implemented Kext Components | Items Needing Community Verification |
| :--- | :--- | :--- | :--- |
| **IPU4P + `TPS68470 PMIC` (`INT3472`) + `OV5693`** | **Microsoft Surface Pro 7** *(front camera)*, **Microsoft Surface Book 3** *(13.5" / 15" Ice Lake)* | [`Tps68470PowerHal`](../kexts/UniCam/src/Core/BoardHal/Tps68470PowerHal.cpp) (`UNIC0002`) + [`OV5693Driver`](../kexts/UniCam/src/Core/Sensors/OV5693Driver.cpp) (`INT33BE`, `1296x972` 2x2 bin).<br>*(Rear `OV8865` & IR `OV7251` not yet implemented)* | Confirm `TPS68470` I2C bus/address, PMIC reset/pwdn GPIO mapping, MIPI port/lane count, and D-PHY/Combo PHY frame capture. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV01A10` / `OV01A1B`** | **Dell XPS / Latitude Ice Lake 2-in-1** variants (`8086:8a19`), or **HP / Lenovo Ice Lake-U** HD MIPI camera trims | [`OV01A10Driver`](../kexts/UniCam/src/Core/Sensors/OV01A10Driver.cpp) (`OVTI01A0` / `OVTI01AB`, `1280x800`) | Run `UniCam-SSDT.command` to extract BIOS pinouts into `SSDT-CAM-<Board>.aml`, verify `0`/`180` rotation and port mapping. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV2740` / `OV02C10`** | Non-UVC MIPI SKUs of **Lenovo Yoga C940-14IIL**, **HP Spectre x360 13-aw0xxx**, **Dell Latitude 9410 / 9510 2-in-1** *(Ice Lake)* | [`OV2740Driver`](../kexts/UniCam/src/Core/Sensors/OV2740Driver.cpp) (`INT3474`/`OVTI2740`, `1932x1092`) + [`OV02C10Driver`](../kexts/UniCam/src/Core/Sensors/OV02C10Driver.cpp) (`OVTI02C1`, `1928x1092`, 1 or 2 lanes) | Confirm link frequency (`360`/`180` vs `400 MHz`), lane count (`1` vs `2`), `SGRBG10` Bayer balance, and `1280x720` SoftISP scaling. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV5675`** | Ultraportable Intel Ice Lake laptops with OmniVision 5MP (`OVTI5675`/`INT3476`) in 2x2 binned mode | [`OV5675Driver`](../kexts/UniCam/src/Core/Sensors/OV5675Driver.cpp) (`OVTI5675`/`INT3476`, `1296x972`, 900 Mbps 2-lane) | Confirm `0x5675` I2C chip ID probe, `450 MHz` PHY sync on IPU4P, and `SGRBG10` colors. |

> [!TIP]
> **Want to test & confirm one of the 3.2 configurations?**
> 1. Confirm your laptop uses **Intel IPU4P (`8086:8a19`)** (Windows Device Manager -> `PCI\VEN_8086&DEV_8A19`, or `ioreg -l | grep 8a19` in macOS).
> 2. Follow Step 1 & Step 2 above using [`Build-UniCam.command`](../kexts/Build-UniCam.command) and [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command), and add **`-unicamdbg`** to OpenCore `boot-args` *(also add **`-unicambeta`** if testing on `Darwin >= 26`)* so diagnostic logging and fallback UVC remain enabled while bringing up your board.
> 3. If video does not appear immediately, run the commands below in Terminal (with `-unicamdbg` boot-arg enabled) and open a GitHub Issue attaching `acpi/output/pinout_decoded.json`:
>    `ioreg -r -n UniCamBridgeService -d 4 && log show --last 5m --predicate 'eventMessage CONTAINS[c] "UniCam"'`

---

### 3.3. Laptops That Do Not Need or Are Not Supported by UniCam

- **Standard USB UVC Webcams *(UniCam not needed)*:** Many 10th-Gen Intel laptops (including the **non-2-in-1 clamshell Dell XPS 13 7390**, **Comet Lake 10th-Gen**, or models with an internal USB camera on `XHC`) already work natively on macOS or via `USBMap` — **no IPU4P hardware is present**.
- **Intel IPU3 *(Skylake / Kaby Lake / Amber Lake — 7th/8th/9th Gen, e.g. Surface Go 1/2/3, Dell Latitude 5285, Latitude 7212)*:** Different IPU architecture (`8086:1919` / `8086:5919`); `IPU3` HAL is not yet implemented.
- **Intel IPU6 / IPU6EP / IPU6SE *(Tiger Lake / Alder Lake / Raptor Lake — 11th–14th Gen, e.g. Dell XPS 13 9310/9315/9320, Surface Pro 7+/8/9)*:** Different `IIpuIsysHal` & firmware architecture (`8086:9a19` / `8086:465d` / `8086:a75d`); not currently supported.

### 3.4. Developer Extension Guide

| Extension Area | Code Entry Points |
| :--- | :--- |
| **Add a MIPI Sensor** (`OV8865`, `OV7251`, `HI556`, `HM2170`, etc.) | Subclass [`ISensorDriver.hpp`](../kexts/UniCam/src/Core/Sensors/ISensorDriver.hpp), register `_HID`/`compat` in [`SensorRegistry.cpp`](../kexts/UniCam/src/Core/Sensors/SensorRegistry.cpp), and add the source file to [`kexts/UniCam/Makefile`](../kexts/UniCam/Makefile). |
| **Add an I2C Power PMIC** | Implement [`IBoardPowerHal.hpp`](../kexts/UniCam/src/Core/BoardHal/IBoardPowerHal.hpp) (see [`Tps68470PowerHal.cpp`](../kexts/UniCam/src/Core/BoardHal/Tps68470PowerHal.cpp) as reference). |
| **Add a New IPU Generation (`IPU3`/`IPU6`)** | Implement [`IIpuIsysHal.hpp`](../kexts/UniCam/src/IPU/Hal/IIpuIsysHal.hpp) and add PCI matching IDs in [`Info.plist`](../kexts/UniCam/Info.plist). |

```bash
# Full build & integrity verification (Dell 7390 ACPI + UniCam.kext)
make clean && make verify

# Inspect loaded IORegistry services & kernel debug logs (-unicamdbg)
ioreg -r -n UniCamBridgeService -d 4
log show --last 5m --predicate 'eventMessage CONTAINS[c] "UniCam"'
```

---

## 4. Repository Layout

- **[`kexts/`](../kexts/README.md)** — 100% standalone kext workspace: `Build-UniCam.command` + [`UniCam/`](../kexts/UniCam/Makefile) (`src/`, `include/`, `firmware/`, `profiles/sensors/`, `scripts/`).
- **[`acpi/`](../acpi/README.md)** — 100% standalone ACPI workspace: `UniCam-SSDT.command` + `SSDT-CAM-DUMP.aml` + [`scripts/`](../acpi/scripts/unicam_ssdt_wizard.py) + [`templates/`](../acpi/templates/SSDT-CAM-Static.dsl) + [`boards/`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.dsl).
- **[`MD/`](README.vi.md)** — Multi-language documentation ([`README.en.md`](README.en.md), [`README.vi.md`](README.vi.md)).
- **[`Makefile`](../Makefile)** — Top-level build and verification targets (`make clean && make verify`).
