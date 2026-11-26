/*
 * SSDT-CAM-Static.dsl — Standard UNIC0001 template using static GPIO DW0 & ICLK addresses
 *
 * Customization guide for a new laptop board:
 *  1. Boot with SSDT-CAM-DUMP.aml and run parse_cam_dump.py to capture the board pinout.
 *  2. Replace I2C3 with the target I2Cx bus (I2C0 / I2C1 / I2C2 / I2C3...) and adjust PCI0/PC00 if needed.
 *  3. Update the 7-bit I2C slave address in I2cSerialBusV2 and "unicam-i2c-addr".
 *  4. Update the DW0 MMIO addresses (Reset/Power/ClkEn/LED) and ICLK IMGCLKOUT_x register (0xFDAD8000 + 0x0C*n).
 *  5. Test-compile with: iasl -w2 -vw 4089 SSDT-CAM-Static.dsl
 */
DefinitionBlock ("", "SSDT", 2, "UNIC", "CAMSTAT ", 0x00001000)
{
    External (_SB_.PCI0.I2C3, DeviceObj)

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

            // Place under \_SB.PCI0.I2Cx so VoodooI2CControllerDriver attaches a VoodooI2CDeviceNub
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0036, ControllerInitiated, 0x00061A80,
                    AddressingMode7Bit, "\\_SB.PCI0.I2C3",
                    0x00, ResourceConsumer, , Exclusive,
                    )
            })

            Method (_DSM, 4, Serialized)
            {
                // Ignore VoodooI2C TP7G UUID so parseResourcesDSM() does not log OSData cast warnings
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

                Return (Package ()
                {
                    "unicam-schema",         0x01,
                    "unicam-board-id",       "vendor,board-template-static",
                    "unicam-ipu-gen",        "ipu4p",
                    "unicam-ipu-path",       "\\_SB.PCI0.IPU0",
                    "unicam-sensor-hid",     "OVTI01A0",
                    "unicam-sensor-compat",  "ovti,ov01a10",
                    "unicam-i2c-bus",        0x03,
                    "unicam-i2c-addr",       0x36,
                    "unicam-eeprom-addr",    0x50,
                    "unicam-csi-port",       0x06,
                    "unicam-csi-lanes",      0x01,
                    "unicam-link-freq-hz",   400000000,
                    "unicam-mclk-hz",        19200000,
                    "unicam-mclk-src",       0x01,
                    "unicam-mclk-reg",       0xFDAD800C,
                    "unicam-rotation",       0,
                    "unicam-position",       "front",
                    "unicam-gpio-mmio",      Package ()
                    {
                        Package (0x04) { 0x00, 0xFD6A06F0, 0x01, 10000 },
                        Package (0x04) { 0x0B, 0xFD6E0970, 0x01, 10000 },
                        Package (0x04) { 0x0C, 0xFD6D0740, 0x01, 5000 },
                        Package (0x04) { 0x0D, 0xFD6E08D0, 0x01, 0 },
                    },
                    "unicam-gpio-reset-dw0", 0xFD6A06F0,
                    "unicam-gpio-reset-act", 0x01,
                    "unicam-gpio-pwdn-dw0",  0x00000000,
                    "unicam-gpio-pwdn-act",  0x01,
                    "unicam-gpio-power-dw0", 0xFD6E0970,
                    "unicam-gpio-power-act", 0x01,
                    "unicam-gpio-clken-dw0", 0xFD6D0740,
                    "unicam-gpio-clken-act", 0x01,
                    "unicam-gpio-led-dw0",   0xFD6E08D0,
                    "unicam-gpio-led-act",   0x01,
                })
            }

            // Helper to write PCH GPIO Pad DW0 register (GPIO Mode, Output Enable, Set Tx State)
            Method (USET, 2, Serialized)
            {
                OperationRegion (GPAD, SystemMemory, Arg0, 0x04)
                Field (GPAD, DWordAcc, NoLock, Preserve)
                {
                    VAL0, 32
                }

                Local0 = VAL0
                Local0 &= 0xFFFFE2FE
                If (Arg1)
                {
                    Local0 |= One
                }

                VAL0 = Local0
            }

            OperationRegion (CLKX, SystemMemory, 0xFDAD800C, 0x04)
            Field (CLKX, ByteAcc, NoLock, Preserve)
            {
                REGV, 8
            }

            Method (UCLK, 1, Serialized)
            {
                Local0 = REGV
                Local0 &= 0xFC
                If (Arg0)
                {
                    Local0 |= 0x03 // Enable + 19.2 MHz (change to 0x02 for 24 MHz)
                }
                Else
                {
                    Local0 |= 0x01
                }

                REGV = Local0
            }

            Method (UCIN, 0, NotSerialized)
            {
                Return (0x07)
            }

            Method (UCPW, 1, Serialized)
            {
                If (Arg0)
                {
                    USET (0xFD6A06F0, 0x00)
                    USET (0xFD6E0970, 0x01)
                    Sleep (0x0A)
                    UCLK (One)
                    USET (0xFD6D0740, 0x01)
                    Sleep (0x05)
                    USET (0xFD6A06F0, 0x01)
                    Sleep (0x0A)
                }
                Else
                {
                    USET (0xFD6E08D0, 0x00)
                    USET (0xFD6A06F0, 0x00)
                    Sleep (0x02)
                    USET (0xFD6D0740, 0x00)
                    UCLK (Zero)
                    USET (0xFD6E0970, 0x00)
                }

                Return (Zero)
            }

            Method (UCLD, 1, Serialized)
            {
                If (Arg0)
                {
                    USET (0xFD6E08D0, 0x01)
                }
                Else
                {
                    USET (0xFD6E08D0, 0x00)
                }

                Return (Zero)
            }

            Method (UCRS, 0, Serialized)
            {
                USET (0xFD6A06F0, 0x00)
                Sleep (0x05)
                USET (0xFD6A06F0, 0x01)
                Sleep (0x0A)
                Return (Zero)
            }
        }
    }
}
