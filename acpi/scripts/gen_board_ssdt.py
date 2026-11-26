#!/usr/bin/env python3
r"""
gen_board_ssdt.py
Automatically generates SSDT-CAM-<Board>.dsl (and compiles .aml via iasl) following the
UniCam UNIC0001 ACPI contract from pinout_decoded.json (exported by parse_cam_dump.py --json).

All board-specific parameters (I2C bus, slave address, GPIO DW0 + polarity, MCLK register,
MIPI CSI-2 port/lanes, and sensor rotation) are encapsulated in Device (UCAM) under
Scope (\_SB.<PCI_ROOT>.I2C<bus>), keeping the kext completely board-agnostic.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys


def find_iasl() -> str:
    """Locate ACPICA iasl compiler on PATH, Homebrew, or inside MaciASL.app."""
    candidates = []
    which_iasl = shutil.which("iasl")
    if which_iasl:
        candidates.append(which_iasl)
    candidates.extend([
        "/opt/homebrew/bin/iasl",
        "/usr/local/bin/iasl",
    ])
    for pattern in (
        "/Applications/MaciASL.app/Contents/MacOS/iasl*",
        os.path.expanduser("~/Applications/MaciASL.app/Contents/MacOS/iasl*"),
        os.path.expanduser("~/Downloads/MaciASL.app/Contents/MacOS/iasl*"),
    ):
        candidates.extend(sorted(glob.glob(pattern), reverse=True))

    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return ""


def gpio_on_val(active_high: int, sensor_on: bool) -> int:
    """
    Map logical sensor state (sensor_on=True for active/running, False for off/reset)
    to the physical voltage level (0/1) using C1Ax (INT3472_GPIO_DSM_SENSOR_ON_VAL).
    """
    on_level = 1 if active_high else 0
    return on_level if sensor_on else (1 - on_level)


def find_gpio(gpios: list, func_code: int):
    for g in gpios:
        if g.get("func") == func_code:
            return g
    return None


def generate_asl(data: dict, args: argparse.Namespace) -> str:
    board_id = args.board_id
    pci_root = args.pci_root
    ipu_name = args.ipu_name
    ipu_gen = args.ipu_gen
    sensor_hid = args.sensor_hid
    sensor_compat = args.sensor_compat
    link_freq_hz = args.link_freq_hz
    position = args.position

    i2c_bus = data["i2c_bus"]
    i2c_addrs = data.get("i2c_addrs", [])
    sensor_addr = i2c_addrs[0]["addr"] if i2c_addrs else 0x36
    eeprom_addr = next((a["addr"] for a in i2c_addrs[1:] if a.get("addr", 0) != 0), 0)

    mipi_port = data["mipi_port"]
    mipi_lanes = data["mipi_lanes"]
    mclk_hz = data["mclk_hz"]
    mclk_src = data["mclk_src"]
    mclk_reg = data["mclk_reg"]
    rotation = args.rotation if args.rotation is not None else data.get("degree", 0)
    gpios = data.get("gpios", [])

    gpio_reset = find_gpio(gpios, 0x00)
    gpio_pwdn = find_gpio(gpios, 0x01)
    gpio_power = find_gpio(gpios, 0x0B)
    gpio_clken = find_gpio(gpios, 0x0C)
    gpio_led = find_gpio(gpios, 0x0D)

    freq_bit = 1 if mclk_hz == 19200000 else 0
    i2c_scope = f"\\_SB.{pci_root}.I2C{i2c_bus}"
    i2c_scope_ext = f"\\_SB_.{pci_root}.I2C{i2c_bus}"
    i2c_scope_str = i2c_scope.replace("\\", "\\\\")
    ipu_path = f"\\_SB.{pci_root}.{ipu_name}"
    ipu_path_str = ipu_path.replace("\\", "\\\\")

    cap_mask = 0x01  # bit 0: ready
    if gpio_led is not None:
        cap_mask |= 0x02  # bit 1: has privacy LED

    pmic_mode = getattr(args, "pmic_type", "auto")
    pmic_i2c_bus = getattr(args, "pmic_i2c_bus", None)
    if pmic_i2c_bus is None:
        pmic_i2c_bus = data.get("pmic_i2c_bus", i2c_bus)
    pmic_i2c_addr = getattr(args, "pmic_i2c_addr", None)
    if pmic_i2c_addr is None:
        pmic_i2c_addr = data.get("pmic_i2c_addr", 0)

    if pmic_mode == "auto":
        use_tps68470 = (data.get("c1tp", 1) != 1 and pmic_i2c_addr != 0)
    else:
        use_tps68470 = (pmic_mode == "tps68470")

    if use_tps68470:
        if not pmic_i2c_addr:
            pmic_i2c_addr = 0x4D
        cap_mask |= 0x08  # bit 3: I2C PMIC (TPS68470)
    elif data.get("c1tp", 1) == 1:
        cap_mask |= 0x04  # bit 2: discrete GPIO PMIC

    def gpio_line(g, sensor_on: bool, comment: str) -> str:
        if not g:
            return f"                // (No GPIO assigned: {comment})"
        val = gpio_on_val(g["active_high"], sensor_on)
        return f"                USET (0x{g['dw0_addr']:08X}, 0x{val:02X}) // {comment}: {g['group']}_{g['pad']}"

    # Power-ON sequence (INT3472: sensor_on=False -> hold reset/pwdn; sensor_on=True -> power/clk/release)
    pw_on_lines = [
        gpio_line(gpio_reset, False, "Hold RESET asserted before power-on"),
        gpio_line(gpio_pwdn, False, "Hold PWDN asserted in shutdown before power-on"),
        gpio_line(gpio_power, True, "Enable POWER_ENABLE rail"),
        "                Sleep (0x0A) // Wait 10 ms for rail settling",
        "                UCLK (One)   // Enable MCLK output",
        gpio_line(gpio_clken, True, "Enable CLK_ENABLE gate"),
        "                Sleep (0x05) // Wait 5 ms for clock stabilization",
        gpio_line(gpio_pwdn, True, "Deassert PWDN to wake sensor"),
        gpio_line(gpio_reset, True, "Deassert RESET to start sensor"),
        "                Sleep (0x0A) // Wait 10 ms before starting I2C transactions",
    ]

    # Power-OFF sequence
    pw_off_lines = [
        gpio_line(gpio_led, False, "Ensure PRIVACY_LED is off"),
        gpio_line(gpio_reset, False, "Assert RESET on sensor"),
        gpio_line(gpio_pwdn, False, "Assert PWDN on sensor"),
        "                Sleep (0x02) // Wait 2 ms",
        gpio_line(gpio_clken, False, "Disable CLK_ENABLE gate"),
        "                UCLK (Zero)  // Disable MCLK output",
        gpio_line(gpio_power, False, "Disable POWER_ENABLE rail"),
    ]

    # Privacy LED lines
    if gpio_led is not None:
        led_on = gpio_line(gpio_led, True, "Enable PRIVACY_LED")
        led_off = gpio_line(gpio_led, False, "Disable PRIVACY_LED")
    else:
        led_on = "                // Board has no dedicated PRIVACY_LED GPIO"
        led_off = "                // Board has no dedicated PRIVACY_LED GPIO"

    # Reset pulse lines
    if gpio_reset is not None:
        rst_lines = "\n".join([
            gpio_line(gpio_reset, False, "Assert RESET (Shutdown)"),
            "            Sleep (0x05)",
            gpio_line(gpio_reset, True, "Deassert RESET (Active)"),
            "            Sleep (0x0A)",
        ])
    else:
        rst_lines = "            // Board has no dedicated RESET GPIO"

    # Optional GPIO DW0 metadata inside _DSM for diagnostics/MMIO fallback
    gpio_dsm_entries = []
    role_map = [
        ("reset", gpio_reset),
        ("pwdn", gpio_pwdn),
        ("power", gpio_power),
        ("clken", gpio_clken),
        ("led", gpio_led),
    ]
    for role, g in role_map:
        addr = g["dw0_addr"] if g else 0
        act = g["active_high"] if g else 1
        gpio_dsm_entries.append(f'                "unicam-gpio-{role}-dw0",  0x{addr:08X},')
        gpio_dsm_entries.append(f'                "unicam-gpio-{role}-act",  0x{act:02X},')
    gpio_dsm_block = "\n".join(gpio_dsm_entries)

    gpio_mmio_entries = []
    mmio_roles = [
        (0x00, gpio_reset, 10000),
        (0x01, gpio_pwdn, 5000),
        (0x0B, gpio_power, 10000),
        (0x0C, gpio_clken, 5000),
        (0x0D, gpio_led, 0),
    ]
    for role_code, g, delay_us in mmio_roles:
        if g:
            gpio_mmio_entries.append(
                f'                        Package (0x04) {{ 0x{role_code:02X}, 0x{g["dw0_addr"]:08X}, 0x{g["active_high"]:02X}, {delay_us} }},'
            )
    if gpio_mmio_entries:
        gpio_mmio_block = "\n".join(gpio_mmio_entries)
    else:
        gpio_mmio_block = "                        // No discrete GPIO MMIO pins"

    oem_table_id = args.oem_table_id[:8].ljust(8)

    if use_tps68470:
        pmic_reset_pin = getattr(args, "pmic_reset_pin", 9)
        pmic_pwdn_pin = getattr(args, "pmic_pwdn_pin", 7)
        pmic_led_pin = getattr(args, "pmic_led_pin", 0xFF)
        if pmic_led_pin < 10:
            cap_mask |= 0x02

        pmic_scope = f"\\_SB.{pci_root}.I2C{pmic_i2c_bus}"
        pmic_scope_ext = f"\\_SB_.{pci_root}.I2C{pmic_i2c_bus}"
        pmic_scope_str = pmic_scope.replace("\\", "\\\\")
        extra_ext = f"\n    External ({pmic_scope_ext}, DeviceObj)" if pmic_i2c_bus != i2c_bus else ""

        return f'''/*
 * SSDT-CAM-{args.board_slug}.dsl — Generated by acpi/scripts/gen_board_ssdt.py
 * Reusable UniCam ACPI contract (UNIC0001 + UNIC0002 TPS68470 PMIC)
 *
 * Board ID  : {board_id}
 * IPU       : {ipu_gen} ({ipu_path})
 * Sensor    : {sensor_hid} ({sensor_compat}) @ {i2c_scope} addr 0x{sensor_addr:02X}
 * PMIC      : TI TPS68470 (INT3472) @ {pmic_scope} addr 0x{pmic_i2c_addr:02X}
 * MIPI CSI2 : Port {mipi_port}, {mipi_lanes} lane(s), MCLK {mclk_hz} Hz
 */
DefinitionBlock ("", "SSDT", 2, "UNIC", "{oem_table_id}", 0x00001000)
{{
    External ({i2c_scope_ext}, DeviceObj){extra_ext}

    Scope ({pmic_scope})
    {{
        Device (UPMC)
        {{
            Name (_HID, "UNIC0002")
            Name (_CID, "INT3472")
            Name (_UID, 0x01)

            Method (_STA, 0, NotSerialized)
            {{
                If (_OSI ("Darwin"))
                {{
                    Return (0x0F)
                }}
                Else
                {{
                    Return (Zero)
                }}
            }}

            Name (_CRS, ResourceTemplate ()
            {{
                I2cSerialBusV2 (0x{pmic_i2c_addr:04X}, ControllerInitiated, 0x00061A80,
                    AddressingMode7Bit, "{pmic_scope_str}",
                    0x00, ResourceConsumer, , Exclusive,
                    )
            }})

            Method (_DSM, 4, Serialized)
            {{
                If ((Arg0 == ToUUID ("ef87eb82-f951-46da-84ec-14871ac6f84b")))
                {{
                    Return (Buffer (One)
                    {{
                        0x00
                    }})
                }}

                If ((Arg2 == Zero))
                {{
                    Return (Buffer (One)
                    {{
                        0x03
                    }})
                }}

                Return (Package ()
                {{
                    "unicam-pmic-type", "tps68470",
                }})
            }}
        }}
    }}

    Scope ({i2c_scope})
    {{
        Device (UCAM)
        {{
            Name (_HID, "UNIC0001")
            Name (_CID, "{sensor_hid}")
            Name (_UID, 0x01)

            Method (_STA, 0, NotSerialized)
            {{
                If (_OSI ("Darwin"))
                {{
                    Return (0x0F)
                }}
                Else
                {{
                    Return (Zero)
                }}
            }}

            Name (_CRS, ResourceTemplate ()
            {{
                I2cSerialBusV2 (0x{sensor_addr:04X}, ControllerInitiated, 0x00061A80,
                    AddressingMode7Bit, "{i2c_scope_str}",
                    0x00, ResourceConsumer, , Exclusive,
                    )
            }})

            Method (_DSM, 4, Serialized)
            {{
                If ((Arg0 == ToUUID ("ef87eb82-f951-46da-84ec-14871ac6f84b")))
                {{
                    Return (Buffer (One)
                    {{
                        0x00
                    }})
                }}

                If ((Arg2 == Zero))
                {{
                    Return (Buffer (One)
                    {{
                        0x03
                    }})
                }}

                Return (Package ()
                {{
                    "unicam-schema",         0x01,
                    "unicam-board-id",       "{board_id}",
                    "unicam-ipu-gen",        "{ipu_gen}",
                    "unicam-ipu-path",       "{ipu_path_str}",
                    "unicam-sensor-hid",     "{sensor_hid}",
                    "unicam-sensor-compat",  "{sensor_compat}",
                    "unicam-pmic-type",      "tps68470",
                    "unicam-pmic-i2c-bus",   0x{pmic_i2c_bus:02X},
                    "unicam-pmic-i2c-addr",  0x{pmic_i2c_addr:02X},
                    "unicam-pmic-reset-pin", 0x{pmic_reset_pin:02X},
                    "unicam-pmic-pwdn-pin",  0x{pmic_pwdn_pin:02X},
                    "unicam-pmic-led-pin",   0x{pmic_led_pin:02X},
                    "unicam-i2c-bus",        0x{i2c_bus:02X},
                    "unicam-i2c-addr",       0x{sensor_addr:02X},
                    "unicam-eeprom-addr",    0x{eeprom_addr:02X},
                    "unicam-csi-port",       0x{mipi_port:02X},
                    "unicam-csi-lanes",      0x{mipi_lanes:02X},
                    "unicam-link-freq-hz",   {link_freq_hz},
                    "unicam-mclk-hz",        {mclk_hz},
                    "unicam-mclk-src",       0x{mclk_src:02X},
                    "unicam-mclk-reg",       0x00000000,
                    "unicam-rotation",       {rotation},
                    "unicam-position",       "{position}",
                }})
            }}

            Method (UCIN, 0, NotSerialized)
            {{
                Return (0x{cap_mask:02X})
            }}
        }}
    }}
}}
'''

    return f'''/*
 * SSDT-CAM-{args.board_slug}.dsl — Generated by acpi/scripts/gen_board_ssdt.py
 * Reusable UniCam ACPI contract (UNIC0001)
 *
 * Board ID  : {board_id}
 * IPU       : {ipu_gen} ({ipu_path})
 * Sensor    : {sensor_hid} ({sensor_compat}) @ {i2c_scope} addr 0x{sensor_addr:02X}
 * MIPI CSI2 : Port {mipi_port}, {mipi_lanes} lane(s), MCLK {mclk_hz} Hz (0x{mclk_reg:08X})
 */
DefinitionBlock ("", "SSDT", 2, "UNIC", "{oem_table_id}", 0x00001000)
{{
    External ({i2c_scope_ext}, DeviceObj)

    Scope ({i2c_scope})
    {{
        Device (UCAM)
        {{
            Name (_HID, "UNIC0001")
            Name (_CID, "{sensor_hid}")
            Name (_UID, 0x01)

            Method (_STA, 0, NotSerialized)
            {{
                If (_OSI ("Darwin"))
                {{
                    Return (0x0F)
                }}
                Else
                {{
                    Return (Zero)
                }}
            }}

            // Declare I2cSerialBusV2 under {i2c_scope} so VoodooI2C attaches a VoodooI2CDeviceNub
            Name (_CRS, ResourceTemplate ()
            {{
                I2cSerialBusV2 (0x{sensor_addr:04X}, ControllerInitiated, 0x00061A80,
                    AddressingMode7Bit, "{i2c_scope_str}",
                    0x00, ResourceConsumer, , Exclusive,
                    )
            }})

            // Export configuration properties for AppleACPIPlatform -> IORegistry "device-properties"
            Method (_DSM, 4, Serialized)
            {{
                // Ignore VoodooI2C TP7G UUID to prevent OSData cast errors in VoodooI2CDeviceNub::parseResourcesDSM()
                If ((Arg0 == ToUUID ("ef87eb82-f951-46da-84ec-14871ac6f84b")))
                {{
                    Return (Buffer (One)
                    {{
                        0x00
                    }})
                }}

                If ((Arg2 == Zero))
                {{
                    Return (Buffer (One)
                    {{
                        0x03
                    }})
                }}

                Return (Package ()
                {{
                    "unicam-schema",         0x01,
                    "unicam-board-id",       "{board_id}",
                    "unicam-ipu-gen",        "{ipu_gen}",
                    "unicam-ipu-path",       "{ipu_path_str}",
                    "unicam-sensor-hid",     "{sensor_hid}",
                    "unicam-sensor-compat",  "{sensor_compat}",
                    "unicam-i2c-bus",        0x{i2c_bus:02X},
                    "unicam-i2c-addr",       0x{sensor_addr:02X},
                    "unicam-eeprom-addr",    0x{eeprom_addr:02X},
                    "unicam-csi-port",       0x{mipi_port:02X},
                    "unicam-csi-lanes",      0x{mipi_lanes:02X},
                    "unicam-link-freq-hz",   {link_freq_hz},
                    "unicam-mclk-hz",        {mclk_hz},
                    "unicam-mclk-src",       0x{mclk_src:02X},
                    "unicam-mclk-reg",       0x{mclk_reg:08X},
                    "unicam-rotation",       {rotation},
                    "unicam-position",       "{position}",
                    "unicam-gpio-mmio",      Package ()
                    {{
{gpio_mmio_block}
                    }},
{gpio_dsm_block}
                }})
            }}

            // Helper to write PCH GPIO Pad DW0 register (GPIO Mode, Output Enable, Set Tx State)
            Method (USET, 2, Serialized)
            {{
                OperationRegion (GPAD, SystemMemory, Arg0, 0x04)
                Field (GPAD, DWordAcc, NoLock, Preserve)
                {{
                    VAL0, 32
                }}

                Local0 = VAL0
                Local0 &= 0xFFFFE2FE // Clear PMode (10..12), GPIOTxDis (8), GPIOTxState (0)
                If (Arg1)
                {{
                    Local0 |= One
                }}

                VAL0 = Local0
            }}

            // PCH ICLK IMGCLKOUT_x register (0x{mclk_reg:08X})
            OperationRegion (CLKX, SystemMemory, 0x{mclk_reg:08X}, 0x04)
            Field (CLKX, ByteAcc, NoLock, Preserve)
            {{
                REGV, 8
            }}

            // Helper to control PCH ICLK IMGCLKOUT_x register
            Method (UCLK, 1, Serialized)
            {{
                Local0 = REGV
                Local0 &= 0xFC
                If (Arg0)
                {{
                    Local0 |= 0x{(0x02 | freq_bit):02X}
                }}
                Else
                {{
                    Local0 |= 0x{freq_bit:02X}
                }}

                REGV = Local0
            }}

            // UCIN (): Return board capability bitmask
            Method (UCIN, 0, NotSerialized)
            {{
                Return (0x{cap_mask:02X})
            }}

            // UCPW (Arg0): 1 = Enable power + MCLK + release Reset; 0 = Assert Reset + stop MCLK + power off
            Method (UCPW, 1, Serialized)
            {{
                If (Arg0)
                {{
{chr(10).join(pw_on_lines)}
                }}
                Else
                {{
{chr(10).join(pw_off_lines)}
                }}

                Return (Zero)
            }}

            // UCLD (Arg0): 1 = Turn on Privacy LED; 0 = Turn off Privacy LED
            Method (UCLD, 1, Serialized)
            {{
                If (Arg0)
                {{
{led_on}
                }}
                Else
                {{
{led_off}
                }}

                Return (Zero)
            }}

            // UCRS (): Pulse hardware Reset on the image sensor
            Method (UCRS, 0, Serialized)
            {{
{rst_lines}
                Return (Zero)
            }}
        }}
    }}
}}
'''


def main():
    parser = argparse.ArgumentParser(description="Generate UNIC0001 SSDT-CAM-<Board>.dsl from pinout_decoded.json")
    parser.add_argument("pinout_json", help="Path to pinout_decoded.json exported by parse_cam_dump.py --json")
    parser.add_argument("-o", "--output", required=True, help="Output .dsl file path")
    parser.add_argument("--board-id", required=True, help="e.g. dell,xps-7390-2in1")
    parser.add_argument("--board-slug", default="Board", help="Short CamelCase name for header comment")
    parser.add_argument("--oem-table-id", default="UNICCAM1", help="8-char ACPI OEM Table ID")
    parser.add_argument("--pci-root", default="PCI0", help="ACPI PCI root name (PCI0 or PC00)")
    parser.add_argument("--ipu-name", default="IPU0", help="ACPI IPU device name (IPU0)")
    parser.add_argument("--ipu-gen", default="ipu4p", help="IPU generation (ipu3, ipu4p, ipu6, ipu6ep)")
    parser.add_argument("--sensor-hid", default="OVTI01A0", help="ACPI HID of camera sensor")
    parser.add_argument("--sensor-compat", default="ovti,ov01a10", help="Compatible string for SensorRegistry")
    parser.add_argument("--link-freq-hz", type=int, default=400000000, help="MIPI CSI-2 link frequency in Hz")
    parser.add_argument("--rotation", type=int, default=None, help="Override sensor rotation in degrees")
    parser.add_argument("--position", default="front", help="Camera position (front or back)")
    parser.add_argument("--pmic-type", choices=["auto", "discrete", "tps68470"], default="auto",
                        help="PMIC power backend (auto detects from C1TP/I2C, discrete, or tps68470)")
    parser.add_argument("--pmic-i2c-bus", type=int, default=None, help="TPS68470 I2C bus index")
    parser.add_argument("--pmic-i2c-addr", type=lambda x: int(x, 0), default=None, help="TPS68470 I2C address (e.g. 0x4D)")
    parser.add_argument("--pmic-reset-pin", type=int, default=9, help="TPS68470 reset GPIO index (default 9)")
    parser.add_argument("--pmic-pwdn-pin", type=int, default=7, help="TPS68470 powerdown GPIO index (default 7)")
    parser.add_argument("--pmic-led-pin", type=int, default=0xFF, help="TPS68470 privacy LED GPIO index (255 = none)")
    parser.add_argument("--compile", action="store_true", help="Compile .dsl to .aml with iasl")
    args = parser.parse_args()

    with open(args.pinout_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    asl_src = generate_asl(data, args)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(asl_src)
    print(f"[+] Generated {args.output}")

    if args.compile:
        iasl_bin = find_iasl()
        if not iasl_bin:
            print("[!] `iasl` compiler not found. Install via `brew install acpica` or place MaciASL.app in /Applications.", file=sys.stderr)
            sys.exit(1)
        aml_prefix = os.path.splitext(args.output)[0]
        subprocess.run([iasl_bin, "-w2", "-vw", "4089", "-p", aml_prefix, args.output], check=True)
        print(f"[+] Compiled {aml_prefix}.aml")


if __name__ == "__main__":
    main()
