#include "OV01A10Driver.hpp"

#include <IOKit/IOLib.h>

namespace {

struct RegVal8 {
    uint16_t reg;
    uint8_t val;
};

constexpr uint16_t kRegModeSelect   = 0x0100;
constexpr uint16_t kRegSwReset      = 0x0103;
constexpr uint16_t kRegChipId       = 0x300A;

constexpr uint16_t kRegExposure     = 0x3501;
constexpr uint16_t kRegAnalogGain   = 0x3508;
constexpr uint16_t kRegDGainB       = 0x350A;
constexpr uint16_t kRegDGainGb      = 0x3510;
constexpr uint16_t kRegDGainGr      = 0x3513;
constexpr uint16_t kRegDGainR       = 0x3516;

constexpr uint16_t kRegXAddrStart   = 0x3800;
constexpr uint16_t kRegYAddrStart   = 0x3802;
constexpr uint16_t kRegXAddrEnd     = 0x3804;
constexpr uint16_t kRegYAddrEnd     = 0x3806;
constexpr uint16_t kRegXOutputSize  = 0x3808;
constexpr uint16_t kRegYOutputSize  = 0x380A;
constexpr uint16_t kRegHts          = 0x380C;
constexpr uint16_t kRegVts          = 0x380E;
constexpr uint16_t kRegXWin         = 0x3810;
constexpr uint16_t kRegYWin         = 0x3812;
constexpr uint16_t kRegFormat1      = 0x3820;
constexpr uint16_t kRegTestPattern  = 0x4503;

constexpr uint32_t kNativeWidth     = 1296;
constexpr uint32_t kNativeHeight    = 816;
constexpr uint32_t kActiveWidth     = 1280;
constexpr uint32_t kActiveHeight    = 800;
constexpr uint32_t kHtsDef          = 1488;
constexpr uint32_t kVtsDef          = 0x0700; // 1792
constexpr uint32_t kExposureMin     = 4;
constexpr uint32_t kExposureMargin  = 8;
constexpr uint32_t kAnalogGainMin   = 0x0100;
constexpr uint32_t kAnalogGainMax   = 0x3FFF;
constexpr uint32_t kDigitalGainDef  = 1024;
constexpr uint32_t kDigitalGainMax  = 0x3FFF;

// Reference: Linux drivers/media/i2c/ov01a10.c (400 MHz link / 720 Mbps MIPI CSI-2, MCLK 19.2 MHz)
static const RegVal8 kMipiDataRate720Mbps[] = {
    {0x0103, 0x01},
    {0x0302, 0x00},
    {0x0303, 0x06},
    {0x0304, 0x01},
    {0x0305, 0xf4},
    {0x0306, 0x00},
    {0x0308, 0x01},
    {0x0309, 0x00},
    {0x030c, 0x01},
    {0x0322, 0x01},
    {0x0323, 0x06},
    {0x0324, 0x01},
    {0x0325, 0x68},
};

static const RegVal8 kOv01a10GlobalSetting[] = {
    {0x3002, 0xa1},
    {0x301e, 0xf0},
    {0x3022, 0x01},
    {0x3504, 0x0c},
    {0x3601, 0xc0},
    {0x3603, 0x71},
    {0x3610, 0x68},
    {0x3611, 0x86},
    {0x3640, 0x10},
    {0x3641, 0x80},
    {0x3642, 0xdc},
    {0x3646, 0x55},
    {0x3647, 0x57},
    {0x364b, 0x00},
    {0x3653, 0x10},
    {0x3655, 0x00},
    {0x3656, 0x00},
    {0x365f, 0x0f},
    {0x3661, 0x45},
    {0x3662, 0x24},
    {0x3663, 0x11},
    {0x3664, 0x07},
    {0x3709, 0x34},
    {0x370b, 0x6f},
    {0x3714, 0x22},
    {0x371b, 0x27},
    {0x371c, 0x67},
    {0x371d, 0xa7},
    {0x371e, 0xe7},
    {0x3730, 0x81},
    {0x3733, 0x10},
    {0x3734, 0x40},
    {0x3737, 0x04},
    {0x3739, 0x1c},
    {0x3767, 0x00},
    {0x376c, 0x81},
    {0x3772, 0x14},
    {0x37c2, 0x04},
    {0x37d8, 0x03},
    {0x37d9, 0x0c},
    {0x37e0, 0x00},
    {0x37e1, 0x08},
    {0x37e2, 0x10},
    {0x37e3, 0x04},
    {0x37e4, 0x04},
    {0x37e5, 0x03},
    {0x37e6, 0x04},
    {0x3814, 0x01},
    {0x3815, 0x01},
    {0x3816, 0x01},
    {0x3817, 0x01},
    {0x3822, 0x13},
    {0x3832, 0x28},
    {0x3833, 0x10},
    {0x3b00, 0x00},
    {0x3c80, 0x00},
    {0x3c88, 0x02},
    {0x3c8c, 0x07},
    {0x3c8d, 0x40},
    {0x3cc7, 0x80},
    {0x4000, 0xc3},
    {0x4001, 0xe0},
    {0x4003, 0x40},
    {0x4008, 0x02},
    {0x4009, 0x19},
    {0x400a, 0x01},
    {0x400b, 0x6c},
    {0x4011, 0x00},
    {0x4041, 0x00},
    {0x4300, 0xff},
    {0x4301, 0x00},
    {0x4302, 0x0f},
    {0x4601, 0x50},
    {0x4800, 0x64},
    {0x481f, 0x34},
    {0x4825, 0x33},
    {0x4837, 0x11},
    {0x4881, 0x40},
    {0x4883, 0x01},
    {0x4890, 0x00},
    {0x4901, 0x00},
    {0x4902, 0x00},
    {0x4b00, 0x2a},
    {0x4b0d, 0x00},
    {0x450a, 0x04},
    {0x450b, 0x00},
    {0x5000, 0x65},
    {0x5200, 0x18},
    {0x5004, 0x00},
    {0x5080, 0x40},
    {0x0325, 0xc2},
};

IOReturn writeRegTable(II2CTransport *i2c, const RegVal8 *table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const IOReturn ret = i2c->writeReg8(table[i].reg, table[i].val);
        if (ret != kIOReturnSuccess) {
            IOLog("UniCamCore: OV01A10 write 0x%04x=0x%02x failed (0x%x)\n",
                  table[i].reg, table[i].val, ret);
            return ret;
        }
        // After SW reset (0x0103=0x01), allow PLL/CCI internal logic to settle
        if (table[i].reg == kRegSwReset && table[i].val == 0x01) {
            IOSleep(2);
        }
    }
    return kIOReturnSuccess;
}

uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

OV01A10Driver::OV01A10Driver(bool isMonoVariant)
    : isMonoVariant_(isMonoVariant) {
}

UniCamSensorModeDesc OV01A10Driver::defaultMode() const {
    UniCamSensorModeDesc mode;
    mode.width = kActiveWidth;
    mode.height = kActiveHeight;
    mode.bpp = 10;
    mode.hts = kHtsDef;
    mode.vts = kVtsDef;
    mode.linkFreqHz = 400000000ULL;
    mode.sclkHz = 80000000ULL;
    mode.bayerOrder = 0U; /* SBGGR10 */
    return mode;
}

IOReturn OV01A10Driver::identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) {
    if (!i2c || !outChipId) {
        return kIOReturnBadArgument;
    }

    uint8_t buf[3] = {0, 0, 0};
    uint32_t chipId = 0;
    const IOReturn ret = i2c->readReg24(kRegChipId, &chipId, buf);
    if (rawBytes) {
        rawBytes[0] = buf[0];
        rawBytes[1] = buf[1];
        rawBytes[2] = buf[2];
        rawBytes[3] = 0;
    }
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    *outChipId = chipId;
    return (chipId == UNICAM_OV01A10_CHIP_ID) ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn OV01A10Driver::configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    if (modeIndex != 0) {
        return kIOReturnUnsupported;
    }

    IOReturn ret = writeRegTable(i2c, kMipiDataRate720Mbps,
                                 sizeof(kMipiDataRate720Mbps) / sizeof(kMipiDataRate720Mbps[0]));
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = writeRegTable(i2c, kOv01a10GlobalSetting,
                        sizeof(kOv01a10GlobalSetting) / sizeof(kOv01a10GlobalSetting[0]));
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Configure 1280x800 active window centered inside 1296x816 pixel array
    if ((ret = i2c->writeReg16(kRegXAddrStart, 0)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegYAddrStart, 0)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegXAddrEnd, kNativeWidth - 1)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegYAddrEnd, kNativeHeight - 1)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegXOutputSize, kActiveWidth)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegYOutputSize, kActiveHeight)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegHts, kHtsDef / 2)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegVts, kVtsDef)) != kIOReturnSuccess) return ret;

    // Preserve SBGGR10 Bayer phase when hflip=0, vflip=0 (matching upstream ov01a10.c)
    const uint16_t cropLeft = (kNativeWidth - kActiveWidth) / 2;   // 8
    const uint16_t cropTop  = (kNativeHeight - kActiveHeight) / 2; // 8
    const uint16_t xWin = isMonoVariant_ ? cropLeft : (cropLeft + 1);
    const uint16_t yWin = cropTop;
    const uint8_t format1 = 0xA0U | 0x08U; // format1_base_val (0xA0) | hflip_inverted (BIT(3))

    if ((ret = i2c->writeReg16(kRegXWin, xWin)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegYWin, yWin)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg8(kRegFormat1, format1)) != kIOReturnSuccess) return ret;

    const uint8_t patternVal = testPattern ? static_cast<uint8_t>((testPattern & 0x7FU) | 0x80U) : 0x00U;
    if ((ret = i2c->writeReg8(kRegTestPattern, patternVal)) != kIOReturnSuccess) return ret;

    return setExposureAndGain(i2c, kVtsDef - kExposureMargin, kAnalogGainMin, kDigitalGainDef);
}

IOReturn OV01A10Driver::setStreaming(II2CTransport *i2c, bool enable) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    return i2c->writeReg8(kRegModeSelect, enable ? 0x01 : 0x00);
}

IOReturn OV01A10Driver::setExposureAndGain(II2CTransport *i2c,
                                           uint32_t exposureLines,
                                           uint32_t analogGain,
                                           uint32_t digitalGain) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }

    const uint16_t exp = static_cast<uint16_t>(
        clampU32(exposureLines, kExposureMin, kVtsDef - kExposureMargin));
    const uint16_t aGain = static_cast<uint16_t>(
        clampU32(analogGain, kAnalogGainMin, kAnalogGainMax));
    const uint32_t dGainClamped = clampU32(digitalGain, 0U, kDigitalGainMax);
    const uint32_t dGainHw = dGainClamped << 6;

    IOReturn ret = i2c->writeReg16(kRegExposure, exp);
    if (ret != kIOReturnSuccess) return ret;

    ret = i2c->writeReg16(kRegAnalogGain, aGain);
    if (ret != kIOReturnSuccess) return ret;

    if ((ret = i2c->writeReg24(kRegDGainB, dGainHw)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg24(kRegDGainGb, dGainHw)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg24(kRegDGainGr, dGainHw)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg24(kRegDGainR, dGainHw)) != kIOReturnSuccess) return ret;

    return kIOReturnSuccess;
}
