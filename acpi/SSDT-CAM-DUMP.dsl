/*
 * SSDT-CAM-DUMP.dsl (v2 — _SB.CDMP + _DSM)
 * Extracts the Camera subsystem GPIO, Clock, I2C, and MIPI CSI-2 pinout
 * (DSC1 / INT346F + LNK2 / OVTI01A0 + ICLK) from BIOS GNVS into macOS IORegistry
 * via the standard AppleACPIPlatform Method (_DSM, 4) interface (same as SSDT-USBX).
 * Default target: Dell XPS 13 7390 2-in-1 (Intel Ice Lake IPU4P 8086:8a19)
 */
DefinitionBlock ("", "SSDT", 2, "DELL", "CAMDUMP", 0x00002000)
{
    External (_SB_.PCI0.DSC1.CLDB, MethodObj)
    External (_SB_.PCI0.LNK2.SSDB, MethodObj)
    External (_SB_.GINF, MethodObj)
    External (_SB_.GADR, MethodObj)
    External (SBRG, IntObj)
    External (_SB_.CLK1, FieldUnitObj)
    External (_SB_.CLK2, FieldUnitObj)
    External (_SB_.CLK3, FieldUnitObj)

    // GNVS variables for DSC1 (INT346F - PMIC / Discrete GPIO & Clock Control)
    External (C1TP, FieldUnitObj)
    External (C1GP, FieldUnitObj)
    External (C1IB, FieldUnitObj)
    External (C1IA, FieldUnitObj)
    External (C1P0, FieldUnitObj)
    External (C1P1, FieldUnitObj)
    External (C1P2, FieldUnitObj)
    External (C1P3, FieldUnitObj)
    External (C1G0, FieldUnitObj)
    External (C1G1, FieldUnitObj)
    External (C1G2, FieldUnitObj)
    External (C1G3, FieldUnitObj)
    External (C1F0, FieldUnitObj)
    External (C1F1, FieldUnitObj)
    External (C1F2, FieldUnitObj)
    External (C1F3, FieldUnitObj)
    External (C1A0, FieldUnitObj)
    External (C1A1, FieldUnitObj)
    External (C1A2, FieldUnitObj)
    External (C1A3, FieldUnitObj)
    External (C1I0, FieldUnitObj)
    External (C1I1, FieldUnitObj)
    External (C1I2, FieldUnitObj)
    External (C1I3, FieldUnitObj)

    // GNVS variables for LNK2 (OVTI01A0 - OmniVision OV01A10 Sensor)
    External (L2EN, FieldUnitObj)
    External (L2DI, FieldUnitObj)
    External (L2BS, FieldUnitObj)
    External (L2A0, FieldUnitObj)
    External (L2A1, FieldUnitObj)
    External (L2A2, FieldUnitObj)
    External (L2D0, FieldUnitObj)
    External (L2D1, FieldUnitObj)
    External (L2D2, FieldUnitObj)
    External (L2LU, FieldUnitObj)
    External (L2NL, FieldUnitObj)
    External (L2DG, FieldUnitObj)
    External (L2CK, FieldUnitObj)
    External (L2CL, FieldUnitObj)
    External (L2PP, FieldUnitObj)
    External (L2VR, FieldUnitObj)

    Scope (\_SB)
    {
        Device (CDMP)
        {
            Name (_ADR, Zero)

            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }
                Else
                {
                    Return (Zero)
                }
            }

            Method (_DSM, 4, Serialized)
            {
                If ((Arg2 == Zero))
                {
                    Return (Buffer (One)
                    {
                        0x03
                    })
                }

                Name (DPKG, Package ()
                {
                    "cam-cldb-buffer",    Zero, // [0x01]
                    "cam-ssdb-buffer",    Zero, // [0x03]
                    "cam-sbrg-base",      Zero, // [0x05]
                    "cam-iclk-base",      Zero, // [0x07]
                    "cam-clk1-reg",       Zero, // [0x09]
                    "cam-clk2-reg",       Zero, // [0x0B]
                    "cam-clk3-reg",       Zero, // [0x0D]
                    "cam-c1tp-type",      Zero, // [0x0F]
                    "cam-c1gp-gpio-count", Zero, // [0x11]
                    "cam-c1ib-i2c-bus",   Zero, // [0x13]
                    "cam-c1ia-i2c-addr",  Zero, // [0x15]

                    "cam-gpio0-group",    Zero, // [0x17]
                    "cam-gpio0-pad",      Zero, // [0x19]
                    "cam-gpio0-abs-pin",  Zero, // [0x1B]
                    "cam-gpio0-dw0-addr", Zero, // [0x1D]
                    "cam-gpio0-func",     Zero, // [0x1F]
                    "cam-gpio0-active",   Zero, // [0x21]
                    "cam-gpio0-init",     Zero, // [0x23]

                    "cam-gpio1-group",    Zero, // [0x25]
                    "cam-gpio1-pad",      Zero, // [0x27]
                    "cam-gpio1-abs-pin",  Zero, // [0x29]
                    "cam-gpio1-dw0-addr", Zero, // [0x2B]
                    "cam-gpio1-func",     Zero, // [0x2D]
                    "cam-gpio1-active",   Zero, // [0x2F]
                    "cam-gpio1-init",     Zero, // [0x31]

                    "cam-gpio2-group",    Zero, // [0x33]
                    "cam-gpio2-pad",      Zero, // [0x35]
                    "cam-gpio2-abs-pin",  Zero, // [0x37]
                    "cam-gpio2-dw0-addr", Zero, // [0x39]
                    "cam-gpio2-func",     Zero, // [0x3B]
                    "cam-gpio2-active",   Zero, // [0x3D]
                    "cam-gpio2-init",     Zero, // [0x3F]

                    "cam-gpio3-group",    Zero, // [0x41]
                    "cam-gpio3-pad",      Zero, // [0x43]
                    "cam-gpio3-abs-pin",  Zero, // [0x45]
                    "cam-gpio3-dw0-addr", Zero, // [0x47]
                    "cam-gpio3-func",     Zero, // [0x49]
                    "cam-gpio3-active",   Zero, // [0x4B]
                    "cam-gpio3-init",     Zero, // [0x4D]

                    "cam-l2en-enabled",   Zero, // [0x4F]
                    "cam-l2di-i2c-devs",  Zero, // [0x51]
                    "cam-l2bs-i2c-bus",   Zero, // [0x53]
                    "cam-l2a0-i2c-addr0", Zero, // [0x55]
                    "cam-l2d0-i2c-type0", Zero, // [0x57]
                    "cam-l2a1-i2c-addr1", Zero, // [0x59]
                    "cam-l2d1-i2c-type1", Zero, // [0x5B]
                    "cam-l2a2-i2c-addr2", Zero, // [0x5D]
                    "cam-l2d2-i2c-type2", Zero, // [0x5F]
                    "cam-l2lu-mipi-port", Zero, // [0x61]
                    "cam-l2nl-mipi-lanes", Zero, // [0x63]
                    "cam-l2ck-mclk-hz",   Zero, // [0x65]
                    "cam-l2cl-clock-src", Zero, // [0x67]
                    "cam-l2dg-degree",    Zero, // [0x69]
                    "cam-l2pp-pmic-link", Zero, // [0x6B]
                    "cam-l2vr-vcm-type",  Zero  // [0x6D]
                })

                DPKG [0x01] = \_SB.PCI0.DSC1.CLDB ()
                DPKG [0x03] = \_SB.PCI0.LNK2.SSDB ()
                DPKG [0x05] = SBRG
                DPKG [0x07] = (SBRG + 0x00AD8000)
                DPKG [0x09] = \_SB.CLK1
                DPKG [0x0B] = \_SB.CLK2
                DPKG [0x0D] = \_SB.CLK3
                DPKG [0x0F] = C1TP
                DPKG [0x11] = C1GP
                DPKG [0x13] = C1IB
                DPKG [0x15] = C1IA

                DPKG [0x17] = C1G0
                DPKG [0x19] = C1P0
                If (((C1GP > Zero) && (C1G0 < 0x10)))
                {
                    DPKG [0x1B] = (\_SB.GINF (C1G0, 0x06) + C1P0)
                    DPKG [0x1D] = (\_SB.GADR (C1G0, 0x02) + (C1P0 * 0x10))
                }
                DPKG [0x1F] = C1F0
                DPKG [0x21] = C1A0
                DPKG [0x23] = C1I0

                DPKG [0x25] = C1G1
                DPKG [0x27] = C1P1
                If (((C1GP > One) && (C1G1 < 0x10)))
                {
                    DPKG [0x29] = (\_SB.GINF (C1G1, 0x06) + C1P1)
                    DPKG [0x2B] = (\_SB.GADR (C1G1, 0x02) + (C1P1 * 0x10))
                }
                DPKG [0x2D] = C1F1
                DPKG [0x2F] = C1A1
                DPKG [0x31] = C1I1

                DPKG [0x33] = C1G2
                DPKG [0x35] = C1P2
                If (((C1GP > 0x02) && (C1G2 < 0x10)))
                {
                    DPKG [0x37] = (\_SB.GINF (C1G2, 0x06) + C1P2)
                    DPKG [0x39] = (\_SB.GADR (C1G2, 0x02) + (C1P2 * 0x10))
                }
                DPKG [0x3B] = C1F2
                DPKG [0x3D] = C1A2
                DPKG [0x3F] = C1I2

                DPKG [0x41] = C1G3
                DPKG [0x43] = C1P3
                If (((C1GP > 0x03) && (C1G3 < 0x10)))
                {
                    DPKG [0x45] = (\_SB.GINF (C1G3, 0x06) + C1P3)
                    DPKG [0x47] = (\_SB.GADR (C1G3, 0x02) + (C1P3 * 0x10))
                }
                DPKG [0x49] = C1F3
                DPKG [0x4B] = C1A3
                DPKG [0x4D] = C1I3

                DPKG [0x4F] = L2EN
                DPKG [0x51] = L2DI
                DPKG [0x53] = L2BS
                DPKG [0x55] = L2A0
                DPKG [0x57] = L2D0
                DPKG [0x59] = L2A1
                DPKG [0x5B] = L2D1
                DPKG [0x5D] = L2A2
                DPKG [0x5F] = L2D2
                DPKG [0x61] = L2LU
                DPKG [0x63] = L2NL
                DPKG [0x65] = L2CK
                DPKG [0x67] = L2CL
                DPKG [0x69] = L2DG
                DPKG [0x6B] = L2PP
                DPKG [0x6D] = L2VR

                Return (DPKG)
            }
        }
    }
}
