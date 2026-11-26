/*
 * SSDT-CAM-IntelGNVS.dsl — Dynamic GNVS-backed UNIC0001 template (Intel DSC1 + LNK2)
 *
 * Intended for Dell/Intel laptops (Ice Lake / Comet Lake / Tiger Lake using DSC1 INT346F
 * Discrete GPIO + ICLK CLKC/CLKF + \_SB.GADR) without hardcoding GPIO DW0 addresses.
 *
 * Note: Verify against the target laptop's DSDT:
 *  - Target I2C bus of LNK2 (L2BS -> I2C0/1/2/3).
 *  - Helper method names for pad DW0 lookup (\_SB.GADR) and clock control (\_SB.CLKC, \_SB.CLKF).
 *  - GNVS variable prefixes matching LNKx / DSCy (defaults to DSC1 + LNK2).
 */
DefinitionBlock ("", "SSDT", 2, "UNIC", "CAMGNVS ", 0x00001000)
{
    External (_SB_.PCI0.I2C3, DeviceObj)
    External (_SB_.PCI0.IICB, MethodObj)    // 2 Arguments
    External (_SB_.GADR, MethodObj)         // 2 Arguments
    External (_SB_.CLKC, MethodObj)         // 2 Arguments
    External (_SB_.CLKF, MethodObj)         // 2 Arguments

    // GNVS fields for DSC1 & LNK2 defined in Intel DSDT
    External (SBRG, IntObj)
    External (C1TP, FieldUnitObj)
    External (C1GP, FieldUnitObj)
    External (C1G0, FieldUnitObj)
    External (C1G1, FieldUnitObj)
    External (C1G2, FieldUnitObj)
    External (C1G3, FieldUnitObj)
    External (C1P0, FieldUnitObj)
    External (C1P1, FieldUnitObj)
    External (C1P2, FieldUnitObj)
    External (C1P3, FieldUnitObj)
    External (C1F0, FieldUnitObj)
    External (C1F1, FieldUnitObj)
    External (C1F2, FieldUnitObj)
    External (C1F3, FieldUnitObj)
    External (C1A0, FieldUnitObj)
    External (C1A1, FieldUnitObj)
    External (C1A2, FieldUnitObj)
    External (C1A3, FieldUnitObj)

    External (L2BS, FieldUnitObj)
    External (L2A0, FieldUnitObj)
    External (L2A1, FieldUnitObj)
    External (L2LU, FieldUnitObj)
    External (L2NL, FieldUnitObj)
    External (L2CK, FieldUnitObj)
    External (L2CL, FieldUnitObj)
    External (L2DG, FieldUnitObj)

    Scope (\_SB.PCI0.I2C3)
    {
        Device (UCAM)
        {
            Name (_HID, "UNIC0001")
            Name (_CID, "OVTI01A0")
            Name (_UID, 0x01)

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

            Method (_CRS, 0, Serialized)
            {
                Return (\_SB.PCI0.IICB (L2A0, L2BS))
            }

            // Look up GPIO pad by INT3472 function code (0x00=RESET, 0x01=PWDN, 0x0B=PWR, 0x0C=CLKEN, 0x0D=LED)
            // Returns Package(2): { dw0_addr, active_high } or { 0, 1 } if absent.
            Method (UGFD, 1, Serialized)
            {
                Name (RES, Package (0x02) { Zero, One })
                If (((C1GP > Zero) && (C1F0 == Arg0)))
                {
                    RES [Zero] = (\_SB.GADR (C1G0, 0x02) + (C1P0 * 0x10))
                    RES [One] = C1A0
                    Return (RES)
                }
                If (((C1GP > One) && (C1F1 == Arg0)))
                {
                    RES [Zero] = (\_SB.GADR (C1G1, 0x02) + (C1P1 * 0x10))
                    RES [One] = C1A1
                    Return (RES)
                }
                If (((C1GP > 0x02) && (C1F2 == Arg0)))
                {
                    RES [Zero] = (\_SB.GADR (C1G2, 0x02) + (C1P2 * 0x10))
                    RES [One] = C1A2
                    Return (RES)
                }
                If (((C1GP > 0x03) && (C1F3 == Arg0)))
                {
                    RES [Zero] = (\_SB.GADR (C1G3, 0x02) + (C1P3 * 0x10))
                    RES [One] = C1A3
                    Return (RES)
                }
                Return (RES)
            }

            // Write GPIO state for (func_code, sensor_on: 1=ON/Release, 0=OFF/Reset)
            Method (UGWR, 2, Serialized)
            {
                Local0 = UGFD (Arg0)
                Local1 = DerefOf (Local0 [Zero]) // DW0 physical address
                If ((Local1 == Zero))
                {
                    Return (Zero)
                }

                Local2 = DerefOf (Local0 [One]) // active_high (1 or 0)
                If (Arg1)
                {
                    Local3 = Local2
                }
                Else
                {
                    Local3 = (One - Local2)
                }

                OperationRegion (GPAD, SystemMemory, Local1, 0x04)
                Field (GPAD, DWordAcc, NoLock, Preserve)
                {
                    VAL0, 32
                }

                Local4 = VAL0
                Local4 &= 0xFFFFE2FE
                If (Local3)
                {
                    Local4 |= One
                }

                VAL0 = Local4
                Return (Zero)
            }

            Method (_DSM, 4, Serialized)
            {
                // Ignore VoodooI2C TP7G UUID so parseResourcesDSM() does not log warnings
                If ((Arg0 == ToUUID ("ef87eb82-f951-46da-84ec-14871ac6f84b")))
                {
                    Return (Buffer (One)
                    {
                        0x00
                    })
                }

                If ((Arg2 == Zero))
                {
                    Return (Buffer (One)
                    {
                        0x03
                    })
                }

                Name (DPKG, Package ()
                {
                    "unicam-schema",         0x01,       // [0x01]
                    "unicam-board-id",       "intel,gnvs-generic", // [0x03]
                    "unicam-ipu-gen",        "ipu4p",    // [0x05]
                    "unicam-ipu-path",       "\\_SB.PCI0.IPU0", // [0x07]
                    "unicam-sensor-hid",     "OVTI01A0", // [0x09]
                    "unicam-sensor-compat",  "ovti,ov01a10", // [0x0B]
                    "unicam-i2c-bus",        Zero,       // [0x0D]
                    "unicam-i2c-addr",       Zero,       // [0x0F]
                    "unicam-eeprom-addr",    Zero,       // [0x11]
                    "unicam-csi-port",       Zero,       // [0x13]
                    "unicam-csi-lanes",      Zero,       // [0x15]
                    "unicam-link-freq-hz",   400000000,  // [0x17]
                    "unicam-mclk-hz",        Zero,       // [0x19]
                    "unicam-mclk-src",       Zero,       // [0x1B]
                    "unicam-mclk-reg",       Zero,       // [0x1D]
                    "unicam-rotation",       Zero,       // [0x1F]
                    "unicam-position",       "front",    // [0x21]
                    "unicam-gpio-reset-dw0", Zero,       // [0x23]
                    "unicam-gpio-reset-act", One,        // [0x25]
                    "unicam-gpio-pwdn-dw0",  Zero,       // [0x27]
                    "unicam-gpio-pwdn-act",  One,        // [0x29]
                    "unicam-gpio-power-dw0", Zero,       // [0x2B]
                    "unicam-gpio-power-act", One,        // [0x2D]
                    "unicam-gpio-clken-dw0", Zero,       // [0x2F]
                    "unicam-gpio-clken-act", One,        // [0x31]
                    "unicam-gpio-led-dw0",   Zero,       // [0x33]
                    "unicam-gpio-led-act",   One,        // [0x35]
                })

                Local0 = UGFD (0x00)
                Local1 = UGFD (0x01)
                Local2 = UGFD (0x0B)
                Local3 = UGFD (0x0C)
                Local4 = UGFD (0x0D)

                DPKG [0x0D] = L2BS
                DPKG [0x0F] = L2A0
                DPKG [0x11] = L2A1
                DPKG [0x13] = L2LU
                DPKG [0x15] = L2NL
                DPKG [0x19] = L2CK
                DPKG [0x1B] = L2CL
                DPKG [0x1D] = ((SBRG + 0x00AD8000) + (L2CL * 0x0C))
                DPKG [0x1F] = L2DG
                DPKG [0x23] = DerefOf (Local0 [Zero])
                DPKG [0x25] = DerefOf (Local0 [One])
                DPKG [0x27] = DerefOf (Local1 [Zero])
                DPKG [0x29] = DerefOf (Local1 [One])
                DPKG [0x2B] = DerefOf (Local2 [Zero])
                DPKG [0x2D] = DerefOf (Local2 [One])
                DPKG [0x2F] = DerefOf (Local3 [Zero])
                DPKG [0x31] = DerefOf (Local3 [One])
                DPKG [0x33] = DerefOf (Local4 [Zero])
                DPKG [0x35] = DerefOf (Local4 [One])

                Return (DPKG)
            }

            Method (UCIN, 0, NotSerialized)
            {
                Local0 = One // bit 0: ready
                Local1 = UGFD (0x0D)
                If ((DerefOf (Local1 [Zero]) != Zero))
                {
                    Local0 |= 0x02 // bit 1: has privacy LED
                }
                If ((C1TP == One))
                {
                    Local0 |= 0x04 // bit 2: discrete GPIO PMIC
                }
                Return (Local0)
            }

            Method (UCPW, 1, Serialized)
            {
                If (Arg0)
                {
                    UGWR (0x00, Zero) // Assert RESET
                    UGWR (0x01, Zero) // Assert PWDN
                    UGWR (0x0B, One)  // Assert POWER_ENABLE
                    Sleep (0x0A)
                    If ((L2CK == 19200000))
                    {
                        \_SB.CLKF (L2CL, One)
                    }
                    Else
                    {
                        \_SB.CLKF (L2CL, Zero)
                    }
                    \_SB.CLKC (L2CL, One)
                    UGWR (0x0C, One)  // Assert CLK_ENABLE
                    Sleep (0x05)
                    UGWR (0x01, One)  // Deassert PWDN
                    UGWR (0x00, One)  // Deassert RESET
                    Sleep (0x0A)
                }
                Else
                {
                    UGWR (0x0D, Zero) // Disable PRIVACY_LED
                    UGWR (0x00, Zero) // Assert RESET
                    UGWR (0x01, Zero) // Assert PWDN
                    Sleep (0x02)
                    UGWR (0x0C, Zero) // Disable CLK_ENABLE
                    \_SB.CLKC (L2CL, Zero)
                    UGWR (0x0B, Zero) // Disable POWER_ENABLE
                }

                Return (Zero)
            }

            Method (UCLD, 1, Serialized)
            {
                If (Arg0)
                {
                    UGWR (0x0D, One)
                }
                Else
                {
                    UGWR (0x0D, Zero)
                }
                Return (Zero)
            }

            Method (UCRS, 0, Serialized)
            {
                UGWR (0x00, Zero)
                Sleep (0x05)
                UGWR (0x00, One)
                Sleep (0x0A)
                Return (Zero)
            }
        }
    }
}
