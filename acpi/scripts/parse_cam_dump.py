#!/usr/bin/env python3
"""
parse_cam_dump.py
Reads and decodes all GPIO, Clock (ICLK), I2C, and MIPI CSI-2 pinout parameters
of the camera subsystem (DSC1 / INT346F & LNK2 / OVTI01A0) from IORegistry after loading SSDT-CAM-DUMP.aml.
"""
import argparse
import json
import plistlib
import subprocess
import sys

# Intel Ice Lake-LP GPIO Group mapping table (GPCL from DSDT gpio_tables_GPCL)
ICELAKE_LP_GROUPS = {
    0x00: ("GPP_A", 0x006E0000, 0x0820, 0x40),
    0x01: ("GPP_B", 0x006E0000, 0x0680, 0x20),
    0x02: ("GPP_C", 0x006A0000, 0x0600, 0xE0),
    0x03: ("GPP_D", 0x006D0000, 0x0780, 0x80),
    0x04: ("GPP_E", 0x006A0000, 0x07E0, 0x100),
    0x05: ("GPP_F", 0x006D0000, 0x08D0, 0xA0),
    0x06: ("GPP_G", 0x006E0000, 0x0600, 0x00),
    0x07: ("GPP_H", 0x006D0000, 0x0600, 0x60),
    0x08: ("GPP_R", 0x00690000, 0x0600, 0x120),
    0x09: ("GPP_S", 0x00690000, 0x0680, 0x140),
    0x0A: ("CPU",   0x006C0000, 0x0600, 0xFFFF),
    0x0B: ("VGPIO", 0x006D0000, 0x0A10, 0xC0),
    0x0C: ("SPI",   0x00690000, 0x0930, 0xFFFF),
    0x0D: ("GPD",   0x006B0000, 0x0600, 0xFFFF),
    0x0E: ("JTAG",  0x006A0000, 0x0960, 0xFFFF),
    0x0F: ("HVCMOS", 0x006A0000, 0x0780, 0xFFFF),
}

GPIO_FUNC_NAMES = {
    0x00: "RESET (Camera Sensor Reset - Active Low)",
    0x01: "POWERDOWN / STANDBY (Sensor Shutdown)",
    0x02: "FLASH / STROBE",
    0x0B: "POWER_ENABLE (Camera LDO / Regulator Enable)",
    0x0C: "CLK_ENABLE (External Camera Clock Gate Enable)",
    0x0D: "PRIVACY_LED (White Camera Indicator LED)",
    0xFF: "UNUSED",
}

def get_ioreg_node(name):
    out = subprocess.check_output(["ioreg", "-p", "IOACPIPlane", "-n", name, "-r", "-a"])
    if not out:
        return None
    data = plistlib.loads(out)
    return data[0] if data else None

def to_int(val):
    if isinstance(val, int):
        return val
    if isinstance(val, bytes):
        return int.from_bytes(val, "little")
    return 0

def decode_live_cdmp(quiet: bool = False):
    """Read camera pinout properties from IORegistry (CDMP node from SSDT-CAM-DUMP.aml)."""
    cdmp = get_ioreg_node("CDMP")
    if cdmp and "cam-c1gp-gpio-count" in cdmp.get("device-properties", {}):
        dsc1_props = cdmp.get("device-properties", {})
        lnk2_props = dsc1_props
    else:
        dsc1 = get_ioreg_node("DSC1")
        lnk2 = get_ioreg_node("LNK2")

        if not dsc1 or not lnk2:
            if not quiet:
                print("[!] CDMP, DSC1, or LNK2 node not found in IOACPIPlane.")
            return None

        dsc1_props = dsc1.get("device-properties", {})
        lnk2_props = lnk2.get("device-properties", {})

        if "cam-c1gp-gpio-count" not in dsc1_props:
            if not quiet:
                print("[!] SSDT-CAM-DUMP.aml data (CDMP node) not detected in IORegistry.")
                print("    -> Copy SSDT-CAM-DUMP.aml to EFI/OC/ACPI, enable it in config.plist, then reboot.")
            return None

    def p(text: str) -> None:
        if not quiet:
            print(text)

    p("=" * 72)
    p(" CAMERA HARDWARE PINOUT TABLE (LIVE BIOS DUMP)")
    p("=" * 72)

    sbrg = to_int(dsc1_props.get("cam-sbrg-base", 0xFD000000))
    iclk = to_int(dsc1_props.get("cam-iclk-base", 0xFDAD8000))
    c1tp = to_int(dsc1_props.get("cam-c1tp-type", 1))
    gpio_cnt = to_int(dsc1_props.get("cam-c1gp-gpio-count", 0))
    pmic_i2c_bus = to_int(dsc1_props.get("cam-c1ib-i2c-bus", 0))
    pmic_i2c_addr = to_int(dsc1_props.get("cam-c1ia-i2c-addr", 0))

    pmic_desc = "Discrete GPIO (INT346F/INT3472)" if c1tp == 1 else f"I2C PMIC TPS68470 (C1TP={c1tp}, I2C{pmic_i2c_bus}@0x{pmic_i2c_addr:02X})"

    p(f"\n[1] POWER & CLOCK CIRCUITRY:")
    p(f"    - Power backend (C1TP)     : {pmic_desc}")
    p(f"    - PCH Sideband Base (SBRG) : 0x{sbrg:08X}")
    p(f"    - Camera Clock MMIO (ICLK) : 0x{iclk:08X}")
    p(f"    - GPIO pin count           : {gpio_cnt}")

    gpios_out = []
    for i in range(gpio_cnt):
        grp = to_int(dsc1_props.get(f"cam-gpio{i}-group", 0))
        pad = to_int(dsc1_props.get(f"cam-gpio{i}-pad", 0))
        abspin = to_int(dsc1_props.get(f"cam-gpio{i}-abs-pin", 0))
        dw0_addr = to_int(dsc1_props.get(f"cam-gpio{i}-dw0-addr", 0))
        func = to_int(dsc1_props.get(f"cam-gpio{i}-func", 0xFF))
        act = to_int(dsc1_props.get(f"cam-gpio{i}-active", 0))
        init = to_int(dsc1_props.get(f"cam-gpio{i}-init", 0))

        grp_name, com_off, pad_bar, _ = ICELAKE_LP_GROUPS.get(grp, (f"GRP_{grp}", 0, 0, 0))
        pad_mmio = dw0_addr if dw0_addr else (sbrg + com_off + pad_bar + (pad * 0x10))
        func_desc = GPIO_FUNC_NAMES.get(func, f"Unknown (0x{func:02X})")

        p(f"    * GPIO[{i}]: {grp_name}_{pad} (AbsPin={abspin} / 0x{abspin:02X})")
        p(f"        + Function     : 0x{func:02X} -> {func_desc}")
        p(f"        + Polarity     : Active {'High (1)' if act else 'Low (0)'} (Init={init})")
        p(f"        + Pad Registers: 0x{pad_mmio:08X} (DW0), 0x{pad_mmio+4:08X} (DW1)")
        gpios_out.append({
            "index": i,
            "group": grp_name,
            "group_id": grp,
            "pad": pad,
            "abs_pin": abspin,
            "dw0_addr": pad_mmio,
            "dw0_hex": f"0x{pad_mmio:08X}",
            "func": func,
            "func_name": func_desc,
            "active_high": act,
            "init": init,
        })

    p(f"\n[2] MIPI CSI-2 IMAGE SENSOR:")
    i2c_bus = to_int(lnk2_props.get("cam-l2bs-i2c-bus", 0))
    i2c_devs = to_int(lnk2_props.get("cam-l2di-i2c-devs", 0))
    mipi_port = to_int(lnk2_props.get("cam-l2lu-mipi-port", 0))
    mipi_lanes = to_int(lnk2_props.get("cam-l2nl-mipi-lanes", 0))
    mclk_hz = to_int(lnk2_props.get("cam-l2ck-mclk-hz", 0))
    clk_src = to_int(lnk2_props.get("cam-l2cl-clock-src", 0))
    degree = to_int(lnk2_props.get("cam-l2dg-degree", 0))
    mclk_reg = iclk + clk_src * 0x0C

    i2c_addrs_out = []
    p(f"    - I2C Control Bus          : \\_SB.PCI0.I2C{i2c_bus}")
    p(f"    - Declared I2C Devices     : {i2c_devs}")
    for i in range(min(i2c_devs, 3)):
        addr = to_int(lnk2_props.get(f"cam-l2a{i}-i2c-addr{i}", 0))
        dtype = to_int(lnk2_props.get(f"cam-l2d{i}-i2c-type{i}", 0))
        p(f"        + I2C[{i}]: Slave Address = 0x{addr:02X} (Type={dtype})")
        i2c_addrs_out.append({"index": i, "addr": addr, "addr_hex": f"0x{addr:02X}", "type": dtype})
    p(f"    - MIPI CSI-2 Port          : Port {mipi_port}")
    p(f"    - MIPI CSI-2 Lanes         : {mipi_lanes} lane(s)")
    p(f"    - MCLK Frequency           : {mclk_hz} Hz ({mclk_hz/1e6:.2f} MHz)")
    p(f"    - Clock Source (CLKx)      : IMGCLKOUT_{clk_src} (Register 0x{mclk_reg:08X})")

    cldb_raw = dsc1_props.get("cam-cldb-buffer")
    ssdb_raw = lnk2_props.get("cam-ssdb-buffer")
    if isinstance(cldb_raw, (bytes, bytearray)) or isinstance(ssdb_raw, (bytes, bytearray)):
        p("\n[3] RAW ACPI BUFFERS (CLDB / SSDB):")
        if isinstance(cldb_raw, (bytes, bytearray)):
            p(f"    - CLDB ({len(cldb_raw)} bytes): {cldb_raw.hex(' ')}")
        if isinstance(ssdb_raw, (bytes, bytearray)):
            p(f"    - SSDB ({len(ssdb_raw)} bytes): {ssdb_raw.hex(' ')}")

    return {
        "sbrg": sbrg,
        "iclk": iclk,
        "c1tp": c1tp,
        "pmic_i2c_bus": pmic_i2c_bus,
        "pmic_i2c_addr": pmic_i2c_addr,
        "gpios": gpios_out,
        "i2c_bus": i2c_bus,
        "i2c_devs": i2c_devs,
        "i2c_addrs": i2c_addrs_out,
        "mipi_port": mipi_port,
        "mipi_lanes": mipi_lanes,
        "mclk_hz": mclk_hz,
        "mclk_src": clk_src,
        "mclk_reg": mclk_reg,
        "degree": degree,
        "cldb_hex": cldb_raw.hex() if isinstance(cldb_raw, (bytes, bytearray)) else "",
        "ssdb_hex": ssdb_raw.hex() if isinstance(ssdb_raw, (bytes, bytearray)) else "",
    }


def main():
    parser = argparse.ArgumentParser(description="Decode Camera Pinout from IORegistry (CDMP)")
    parser.add_argument("--json", dest="json_path", help="Export decoded pinout JSON to the specified path")
    args = parser.parse_args()

    export_data = decode_live_cdmp(quiet=False)
    if export_data is None:
        sys.exit(2)

    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as jf:
            json.dump(export_data, jf, indent=2, ensure_ascii=False)
        print(f"\n[+] Exported pinout JSON to: {args.json_path}")


if __name__ == "__main__":
    main()
