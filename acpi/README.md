# `acpi/` — Pinout Probe & `SSDT-CAM` Generator

This directory contains:
- **[`UniCam-SSDT.command`](UniCam-SSDT.command)**: Interactive 1-click wizard to probe BIOS pinouts and compile `SSDT-CAM-<Board>.aml`.
- **[`SSDT-CAM-DUMP.aml`](SSDT-CAM-DUMP.aml)** & **[`SSDT-CAM-DUMP.dsl`](SSDT-CAM-DUMP.dsl)**: Diagnostic SSDT that exposes BIOS camera pinout data to macOS `IORegistry` (no Windows installation required).
- **[`boards/dell-xps-7390-2in1/`](boards/dell-xps-7390-2in1)**: Prebuilt [`SSDT-CAM-Dell7390.aml`](boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml) for **Dell XPS 13 7390 2-in-1**.
- **[`templates/`](templates)**: Two standard `UNIC0001` DSL templates ([`SSDT-CAM-Static.dsl`](templates/SSDT-CAM-Static.dsl) and [`SSDT-CAM-IntelGNVS.dsl`](templates/SSDT-CAM-IntelGNVS.dsl)) for manual editing in `MaciASL`.

---

> [!IMPORTANT]
> **Choose ONLY ONE of the 3 options below** *(do not perform all 3)* to obtain `SSDT-CAM-<Board>.aml`, then copy it to `EFI/OC/ACPI/` alongside `UniCam.kext` in `EFI/OC/Kexts/`.

---

## Option 1 — Use Prebuilt SSDT *(Dell XPS 13 7390 2-in-1)*

- Copy [`boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml`](boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml) directly to `EFI/OC/ACPI/` and enable it in `config.plist -> ACPI -> Add`. **No pinout probing or `iasl` required.**
- *(Alternatively, double-click [`UniCam-SSDT.command`](UniCam-SSDT.command) and choose **`[3]`** to copy it to `acpi/output/SSDT-CAM-Dell7390.aml`.)*

---

## Option 2 — Use the Interactive [`UniCam-SSDT.command`](UniCam-SSDT.command) Wizard *(Recommended for new boards, no coding required)*

- **Prerequisite:** Install [`MaciASL.app`](https://github.com/acidanthera/MaciASL/releases) in `/Applications` *(or run `brew install acpica`)* so the wizard can compile `.aml` with `iasl`.
- Double-click **[`UniCam-SSDT.command`](UniCam-SSDT.command)** in Finder *(if blocked by Gatekeeper: right-click -> **Open** -> **Open**)*.

### If you do NOT know your BIOS pinout yet *(auto-probe flow)*:
1. **Run 1:** Choose **`[1]`** -> generates `acpi/output/SSDT-CAM-DUMP.aml`. Copy it to `EFI/OC/ACPI/`, enable it in `config.plist -> ACPI -> Add`, and **Reboot** macOS.
2. **Run 2:** Re-open `UniCam-SSDT.command` and choose **`[2]`** -> reads your live GPIO (or `TPS68470` PMIC), `MCLK`, `I2C` bus, and MIPI CSI-2 port from `IORegistry`, lets you pick your sensor (`OV01A10`, `OV01A1B`, `OV2740`, `OV02C10`, `OV5693`, `OV5675`), and compiles **`acpi/output/SSDT-CAM-<Board>.aml`**.
3. **Activate:** Copy **`acpi/output/SSDT-CAM-<Board>.aml`** to `EFI/OC/ACPI/`, **DISABLE (`Enabled = False`) `SSDT-CAM-DUMP.aml`** in `config.plist -> ACPI -> Add`, enable **`SSDT-CAM-<Board>.aml`**, and **Reboot**.

### If you ALREADY know your pinout parameters *(skip probing)*:
- Select **`[4]`** in `UniCam-SSDT.command` to enter your I2C bus, GPIO/PMIC, and MIPI port directly and generate **`acpi/output/SSDT-CAM-<Board>.aml`** immediately.

---

## Option 3 — Create Manually Without the Wizard *(For MaciASL or CLI users)*

### 3A. Edit manually in `MaciASL.app` from `templates/`
1. Open either template in `MaciASL.app`:
   - [`templates/SSDT-CAM-Static.dsl`](templates/SSDT-CAM-Static.dsl): hardcoded PCH GPIO DW0 (`0xFD...`) and `ICLK` addresses.
   - [`templates/SSDT-CAM-IntelGNVS.dsl`](templates/SSDT-CAM-IntelGNVS.dsl): dynamically reads BIOS GNVS variables at boot (`C1G0..C1G3`, `L2A0`, `L2LU`...).
2. Update `Scope (\_SB.PCI0.I2Cx)` *(or `PC00`)*, `I2cSerialBusV2`, `_DSM` (`unicam-sensor-hid`, `unicam-sensor-compat`, `unicam-csi-port`, `unicam-csi-lanes`, `unicam-mclk-hz`), and your board's GPIO DW0 / `TPS68470` PMIC settings.
3. Choose **File -> Save As -> File Format: ACPI Machine Language Binary (`.aml`)** and save as `SSDT-CAM-<Board>.aml`.

### 3B. Generate via Terminal CLI
```bash
# 1. Decode live pinout after booting with SSDT-CAM-DUMP.aml
python3 acpi/scripts/parse_cam_dump.py --json acpi/boards/<board-dir>/pinout_decoded.json

# 2. Generate and compile SSDT-CAM-<Board>.aml (add --pmic-type tps68470 for TI PMIC boards)
python3 acpi/scripts/gen_board_ssdt.py acpi/boards/<board-dir>/pinout_decoded.json \
  -o acpi/boards/<board-dir>/SSDT-CAM-<Board>.dsl \
  --board-id "<vendor>,<board-name>" \
  --board-slug "<Board>" \
  --sensor-hid "OVTI01A0" \
  --sensor-compat "ovti,ov01a10" \
  --compile
```
