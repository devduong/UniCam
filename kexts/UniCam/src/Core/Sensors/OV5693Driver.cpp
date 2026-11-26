#include "OV5693Driver.hpp"
#include "SensorRegHelpers.hpp"

#include <IOKit/IOLib.h>

namespace {

constexpr uint16_t kRegModeSelect   = 0x0100;
constexpr uint16_t kRegSwReset      = 0x0103;
constexpr uint16_t kRegChipId       = 0x300A;

constexpr uint16_t kRegMwbRGain     = 0x3400;
constexpr uint16_t kRegMwbGGain     = 0x3402;
constexpr uint16_t kRegMwbBGain     = 0x3404;

constexpr uint16_t kRegExposure     = 0x3500; // 24-bit, bits [19:4]
constexpr uint16_t kRegAnalogGain   = 0x350A; // 16-bit, bits [10:4]

constexpr uint16_t kRegCropStartX   = 0x3800;
constexpr uint16_t kRegCropStartY   = 0x3802;
constexpr uint16_t kRegCropEndX     = 0x3804;
constexpr uint16_t kRegCropEndY     = 0x3806;
constexpr uint16_t kRegOutputW      = 0x3808;
constexpr uint16_t kRegOutputH      = 0x380A;
constexpr uint16_t kRegHts          = 0x380C;
constexpr uint16_t kRegVts          = 0x380E;
constexpr uint16_t kRegOffsetX      = 0x3810;
constexpr uint16_t kRegOffsetY      = 0x3812;
constexpr uint16_t kRegSubIncX      = 0x3814;
constexpr uint16_t kRegSubIncY      = 0x3815;
constexpr uint16_t kRegFormat1      = 0x3820;
constexpr uint16_t kRegFormat2      = 0x3821;
constexpr uint16_t kRegTestPattern  = 0x5E00;

constexpr uint32_t kActiveLeft      = 16;
constexpr uint32_t kActiveTop       = 6;
constexpr uint32_t kActiveCropW     = 2592;
constexpr uint32_t kActiveCropH     = 1944;
constexpr uint32_t kOutWidth        = 1296;
constexpr uint32_t kOutHeight       = 972;
constexpr uint32_t kFixedPpl        = 2688;
constexpr uint32_t kVts30Fps        = 2078;
constexpr uint32_t kExposureMin     = 1;
constexpr uint32_t kExposureMargin  = 8;
constexpr uint32_t kDigitalGainMin  = 1;
constexpr uint32_t kDigitalGainMax  = 4095;

// Reference: Linux drivers/media/i2c/ov5693.c (MCLK 19.2 MHz, 419.2 MHz 2-lane link)
static const UniCamSensorRegVal8 kOv5693GlobalRegs[] = {
    {0x3016, 0xf0}, {0x3017, 0xf0}, {0x3018, 0xf0}, {0x3022, 0x01}, {0x3028, 0x44},
    {0x3098, 0x02}, {0x3099, 0x19}, {0x309a, 0x02}, {0x309b, 0x01}, {0x309c, 0x00},
    {0x30a0, 0xd2}, {0x30a2, 0x01}, {0x30b2, 0x00}, {0x30b3, 0x83}, {0x30b4, 0x03},
    {0x30b5, 0x04}, {0x30b6, 0x01}, {0x3080, 0x01}, {0x3104, 0x21}, {0x3106, 0x00},
    {0x3406, 0x01}, {0x3503, 0x07}, {0x350b, 0x40}, {0x3601, 0x0a}, {0x3602, 0x38},
    {0x3612, 0x80}, {0x3620, 0x54}, {0x3621, 0xc7}, {0x3622, 0x0f}, {0x3625, 0x10},
    {0x3630, 0x55}, {0x3631, 0xf4}, {0x3632, 0x00}, {0x3633, 0x34}, {0x3634, 0x02},
    {0x364d, 0x0d}, {0x364f, 0xdd}, {0x3660, 0x04}, {0x3662, 0x10}, {0x3663, 0xf1},
    {0x3665, 0x00}, {0x3666, 0x20}, {0x3667, 0x00}, {0x366a, 0x80}, {0x3680, 0xe0},
    {0x3681, 0x00}, {0x3700, 0x42}, {0x3701, 0x14}, {0x3702, 0xa0}, {0x3703, 0xd8},
    {0x3704, 0x78}, {0x3705, 0x02}, {0x370a, 0x00}, {0x370b, 0x20}, {0x370c, 0x0c},
    {0x370d, 0x11}, {0x370e, 0x00}, {0x370f, 0x40}, {0x3710, 0x00}, {0x371a, 0x1c},
    {0x371b, 0x05}, {0x371c, 0x01}, {0x371e, 0xa1}, {0x371f, 0x0c}, {0x3721, 0x00},
    {0x3724, 0x10}, {0x3726, 0x00}, {0x372a, 0x01}, {0x3730, 0x10}, {0x3738, 0x22},
    {0x3739, 0xe5}, {0x373a, 0x50}, {0x373b, 0x02}, {0x373c, 0x41}, {0x373f, 0x02},
    {0x3740, 0x42}, {0x3741, 0x02}, {0x3742, 0x18}, {0x3743, 0x01}, {0x3744, 0x02},
    {0x3747, 0x10}, {0x374c, 0x04}, {0x3751, 0xf0}, {0x3752, 0x00}, {0x3753, 0x00},
    {0x3754, 0xc0}, {0x3755, 0x00}, {0x3756, 0x1a}, {0x3758, 0x00}, {0x3759, 0x0f},
    {0x376b, 0x44}, {0x375c, 0x04}, {0x3774, 0x10}, {0x3776, 0x00}, {0x377f, 0x08},
    {0x3780, 0x22}, {0x3781, 0x0c}, {0x3784, 0x2c}, {0x3785, 0x1e}, {0x378f, 0xf5},
    {0x3791, 0xb0}, {0x3795, 0x00}, {0x3796, 0x64}, {0x3797, 0x11}, {0x3798, 0x30},
    {0x3799, 0x41}, {0x379a, 0x07}, {0x379b, 0xb0}, {0x379c, 0x0c}, {0x3a04, 0x06},
    {0x3a05, 0x14}, {0x3e07, 0x20}, {0x4000, 0x08}, {0x4001, 0x04}, {0x4004, 0x08},
    {0x4006, 0x20}, {0x4008, 0x24}, {0x4009, 0x10}, {0x4058, 0x00}, {0x4101, 0xb2},
    {0x4307, 0x31}, {0x4511, 0x05}, {0x4512, 0x01}, {0x481f, 0x30}, {0x4826, 0x2c},
    {0x4d02, 0xfd}, {0x4d03, 0xf5}, {0x4d04, 0x0c}, {0x4d05, 0xcc}, {0x4837, 0x0a},
    {0x5003, 0x20}, {0x5013, 0x00}, {0x5842, 0x01}, {0x5843, 0x2b}, {0x5844, 0x01},
    {0x5845, 0x92}, {0x5846, 0x01}, {0x5847, 0x8f}, {0x5848, 0x01}, {0x5849, 0x0c},
    {0x5e10, 0x0c}, {0x3820, 0x00}, {0x3821, 0x1e}, {0x5041, 0x14},
};

} // namespace

UniCamSensorModeDesc OV5693Driver::defaultMode() const {
    UniCamSensorModeDesc mode;
    mode.width = kOutWidth;
    mode.height = kOutHeight;
    mode.bpp = 10;
    mode.hts = kFixedPpl;
    mode.vts = kVts30Fps;
    mode.linkFreqHz = 419200000ULL;
    mode.sclkHz = 167680000ULL;
    mode.bayerOrder = 0U; /* SBGGR10 */
    return mode;
}

IOReturn OV5693Driver::identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) {
    if (!i2c || !outChipId) {
        return kIOReturnBadArgument;
    }
    uint16_t id16 = 0;
    const IOReturn ret = i2c->readReg16(kRegChipId, &id16);
    if (rawBytes) {
        rawBytes[0] = static_cast<uint8_t>((id16 >> 8) & 0xFFU);
        rawBytes[1] = static_cast<uint8_t>(id16 & 0xFFU);
        rawBytes[2] = 0;
        rawBytes[3] = 0;
    }
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    *outChipId = static_cast<uint32_t>(id16);
    return (static_cast<uint32_t>(id16) == UNICAM_OV5693_CHIP_ID) ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn OV5693Driver::configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    if (modeIndex != 0) {
        return kIOReturnUnsupported;
    }

    IOReturn ret = i2c->writeReg8(kRegSwReset, 0x01);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    IOSleep(2);

    ret = unicamWriteRegTable8(
        i2c, "OV5693", kOv5693GlobalRegs,
        sizeof(kOv5693GlobalRegs) / sizeof(kOv5693GlobalRegs[0]), 0, 0);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Program 2x2 binned 1296x972 active readout centered at (16, 6)
    if ((ret = i2c->writeReg16(kRegCropStartX, kActiveLeft)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegOffsetX, 0)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegOutputW, kOutWidth)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegCropEndX, kActiveLeft + kActiveCropW)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegHts, kFixedPpl)) != kIOReturnSuccess) return ret;

    if ((ret = i2c->writeReg16(kRegCropStartY, kActiveTop)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegOffsetY, 0)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegOutputH, kOutHeight)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegCropEndY, kActiveTop + kActiveCropH)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegVts, kVts30Fps)) != kIOReturnSuccess) return ret;

    if ((ret = i2c->writeReg8(kRegSubIncX, 0x31U)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg8(kRegSubIncY, 0x31U)) != kIOReturnSuccess) return ret;
    if ((ret = unicamUpdateRegBits8(i2c, kRegFormat1, 0x01U, 0x01U)) != kIOReturnSuccess) return ret;
    if ((ret = unicamUpdateRegBits8(i2c, kRegFormat2, 0x01U, 0x01U)) != kIOReturnSuccess) return ret;

    uint8_t patVal = 0x00U;
    if (testPattern == 1U) {
        patVal = 0x81U;
    } else if (testPattern == 2U) {
        patVal = 0x80U;
    } else if (testPattern >= 3U) {
        patVal = 0xC0U;
    }
    if ((ret = i2c->writeReg8(kRegTestPattern, patVal)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg8(kRegModeSelect, 0x00U)) != kIOReturnSuccess) return ret;

    return setExposureAndGain(i2c, kVts30Fps - kExposureMargin, 256U, 1024U);
}

IOReturn OV5693Driver::setStreaming(II2CTransport *i2c, bool enable) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    return i2c->writeReg8(kRegModeSelect, enable ? 0x01 : 0x00);
}

IOReturn OV5693Driver::setExposureAndGain(II2CTransport *i2c,
                                          uint32_t exposureLines,
                                          uint32_t analogGain,
                                          uint32_t digitalGain) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }

    const uint32_t exp = unicamClampU32(exposureLines, kExposureMin, kVts30Fps - kExposureMargin);
    // SoftISP Q8 gain (256 = 1.0x) -> OV5693 Q4 gain (16 = 1.0x, shifted to [10:4])
    const uint32_t aGainQ4 = unicamClampU32(analogGain >> 4, 1U, 127U);
    const uint16_t aGainReg = static_cast<uint16_t>((aGainQ4 << 4) & 0x07F0U);
    const uint16_t dGain = static_cast<uint16_t>(
        unicamClampU32(digitalGain, kDigitalGainMin, kDigitalGainMax) & 0x0FFFU);

    IOReturn ret = i2c->writeReg24(kRegExposure, (exp << 4) & 0x0FFFF0U);
    if (ret != kIOReturnSuccess) return ret;

    ret = i2c->writeReg16(kRegAnalogGain, aGainReg);
    if (ret != kIOReturnSuccess) return ret;

    if ((ret = i2c->writeReg16(kRegMwbRGain, dGain)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegMwbGGain, dGain)) != kIOReturnSuccess) return ret;
    return i2c->writeReg16(kRegMwbBGain, dGain);
}
