#include "OV5675Driver.hpp"
#include "SensorRegHelpers.hpp"

#include <IOKit/IOLib.h>

namespace {

constexpr uint16_t kRegModeSelect   = 0x0100;
constexpr uint16_t kRegSwReset      = 0x0103;
constexpr uint16_t kRegChipId       = 0x300A;
constexpr uint16_t kRegGroupAccess  = 0x3208;

constexpr uint16_t kRegExposure     = 0x3500; // 24-bit: val << 3
constexpr uint16_t kRegAnalogGain   = 0x3508; // 16-bit [128..2047] (128 = 1.0x)
constexpr uint16_t kRegVts          = 0x380E;
constexpr uint16_t kRegTestPattern  = 0x4503;
constexpr uint16_t kRegMwbRGain     = 0x5019;
constexpr uint16_t kRegMwbGGain     = 0x501B;
constexpr uint16_t kRegMwbBGain     = 0x501D;

constexpr uint32_t kWidth           = 1296;
constexpr uint32_t kHeight          = 972;
constexpr uint32_t kHts             = 1500;
constexpr uint32_t kVts30Fps        = 0x07E4; // 2020
constexpr uint32_t kExposureMin     = 4;
constexpr uint32_t kExposureMargin  = 4;
constexpr uint32_t kAnalogGainMin   = 128;
constexpr uint32_t kAnalogGainMax   = 2047;
constexpr uint32_t kDigitalGainMin  = 1024;
constexpr uint32_t kDigitalGainMax  = 4095;

// Reference: Linux drivers/media/i2c/ov5675.c (450 MHz link / 900 Mbps 2-lane, MCLK 19.2 MHz)
static const UniCamSensorRegVal8 kMipiDataRate900Mbps[] = {
    {0x0103, 0x01},
    {0x0100, 0x00},
    {0x0300, 0x04},
    {0x0302, 0x8d},
    {0x0303, 0x00},
    {0x030d, 0x26},
};

static const UniCamSensorRegVal8 kMode1296x972Regs[] = {
    {0x3002, 0x21}, {0x3107, 0x23}, {0x3501, 0x20}, {0x3503, 0x0c}, {0x3508, 0x03},
    {0x3509, 0x00}, {0x3600, 0x66}, {0x3602, 0x30}, {0x3610, 0xa5}, {0x3612, 0x93},
    {0x3620, 0x80}, {0x3642, 0x0e}, {0x3661, 0x00}, {0x3662, 0x08}, {0x3664, 0xf3},
    {0x3665, 0x9e}, {0x3667, 0xa5}, {0x366e, 0x55}, {0x366f, 0x55}, {0x3670, 0x11},
    {0x3671, 0x11}, {0x3672, 0x11}, {0x3673, 0x11}, {0x3714, 0x28}, {0x371a, 0x3e},
    {0x3733, 0x10}, {0x3734, 0x00}, {0x373d, 0x24}, {0x3764, 0x20}, {0x3765, 0x20},
    {0x3766, 0x12}, {0x37a1, 0x14}, {0x37a8, 0x1c}, {0x37ab, 0x0f}, {0x37c2, 0x14},
    {0x37cb, 0x00}, {0x37cc, 0x00}, {0x37cd, 0x00}, {0x37ce, 0x00}, {0x37d8, 0x02},
    {0x37d9, 0x04}, {0x37dc, 0x04}, {0x3800, 0x00}, {0x3801, 0x00}, {0x3802, 0x00},
    {0x3803, 0x00}, {0x3804, 0x0a}, {0x3805, 0x3f}, {0x3806, 0x07}, {0x3807, 0xb7},
    {0x3808, 0x05}, {0x3809, 0x10}, {0x380a, 0x03}, {0x380b, 0xcc}, {0x380c, 0x02},
    {0x380d, 0xee}, {0x380e, 0x07}, {0x380f, 0xd0}, {0x3811, 0x08}, {0x3813, 0x0d},
    {0x3814, 0x03}, {0x3815, 0x01}, {0x3816, 0x03}, {0x3817, 0x01}, {0x381e, 0x02},
    {0x3820, 0x8b}, {0x3821, 0x01}, {0x3832, 0x04}, {0x3c80, 0x01}, {0x3c82, 0x00},
    {0x3c83, 0xc8}, {0x3c8c, 0x0f}, {0x3c8d, 0xa0}, {0x3c90, 0x07}, {0x3c91, 0x00},
    {0x3c92, 0x00}, {0x3c93, 0x00}, {0x3c94, 0xd0}, {0x3c95, 0x50}, {0x3c96, 0x35},
    {0x3c97, 0x00}, {0x4001, 0xe0}, {0x4008, 0x00}, {0x4009, 0x07}, {0x400f, 0x80},
    {0x4013, 0x02}, {0x4040, 0x00}, {0x4041, 0x03}, {0x404c, 0x50}, {0x404e, 0x20},
    {0x4500, 0x06}, {0x4503, 0x00}, {0x450a, 0x04}, {0x4809, 0x04}, {0x480c, 0x12},
    {0x4819, 0x70}, {0x4825, 0x32}, {0x4826, 0x32}, {0x482a, 0x06}, {0x4833, 0x08},
    {0x4837, 0x0d}, {0x5000, 0x77}, {0x5b00, 0x01}, {0x5b01, 0x10}, {0x5b02, 0x01},
    {0x5b03, 0xdb}, {0x5b05, 0x6c}, {0x5e10, 0xfc}, {0x3500, 0x00}, {0x3501, 0x1F},
    {0x3502, 0x20}, {0x3503, 0x08}, {0x3508, 0x04}, {0x3509, 0x00}, {0x3832, 0x48},
    {0x5780, 0x3e}, {0x5781, 0x0f}, {0x5782, 0x44}, {0x5783, 0x02}, {0x5784, 0x01},
    {0x5785, 0x01}, {0x5786, 0x00}, {0x5787, 0x04}, {0x5788, 0x02}, {0x5789, 0x0f},
    {0x578a, 0xfd}, {0x578b, 0xf5}, {0x578c, 0xf5}, {0x578d, 0x03}, {0x578e, 0x08},
    {0x578f, 0x0c}, {0x5790, 0x08}, {0x5791, 0x06}, {0x5792, 0x00}, {0x5793, 0x52},
    {0x5794, 0xa3}, {0x4003, 0x40}, {0x3107, 0x01}, {0x3c80, 0x08}, {0x3c83, 0xb1},
    {0x3c8c, 0x10}, {0x3c8d, 0x00}, {0x3c90, 0x00}, {0x3c94, 0x00}, {0x3c95, 0x00},
    {0x3c96, 0x00}, {0x37cb, 0x09}, {0x37cc, 0x15}, {0x37cd, 0x1f}, {0x37ce, 0x1f},
};

} // namespace

UniCamSensorModeDesc OV5675Driver::defaultMode() const {
    UniCamSensorModeDesc mode;
    mode.width = kWidth;
    mode.height = kHeight;
    mode.bpp = 10;
    mode.hts = kHts;
    mode.vts = kVts30Fps;
    mode.linkFreqHz = 450000000ULL;
    mode.sclkHz = 90000000ULL;
    mode.bayerOrder = 1U; /* SGRBG10 */
    return mode;
}

IOReturn OV5675Driver::identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) {
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
    return (chipId == UNICAM_OV5675_CHIP_ID) ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn OV5675Driver::configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    if (modeIndex != 0) {
        return kIOReturnUnsupported;
    }

    IOReturn ret = unicamWriteRegTable8(
        i2c, "OV5675", kMipiDataRate900Mbps,
        sizeof(kMipiDataRate900Mbps) / sizeof(kMipiDataRate900Mbps[0]),
        kRegSwReset, 2U);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = unicamWriteRegTable8(
        i2c, "OV5675", kMode1296x972Regs,
        sizeof(kMode1296x972Regs) / sizeof(kMode1296x972Regs[0]), 0, 0);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if ((ret = i2c->writeReg16(kRegVts, static_cast<uint16_t>(kVts30Fps))) != kIOReturnSuccess) {
        return ret;
    }

    const uint8_t patVal = testPattern
        ? static_cast<uint8_t>((((testPattern - 1U) & 0x03U) << 2) | 0x80U)
        : 0x00U;
    if ((ret = i2c->writeReg8(kRegTestPattern, patVal)) != kIOReturnSuccess) {
        return ret;
    }

    return setExposureAndGain(i2c, kVts30Fps - kExposureMargin, 256U, kDigitalGainMin);
}

IOReturn OV5675Driver::setStreaming(II2CTransport *i2c, bool enable) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    return i2c->writeReg8(kRegModeSelect, enable ? 0x01 : 0x00);
}

IOReturn OV5675Driver::setExposureAndGain(II2CTransport *i2c,
                                          uint32_t exposureLines,
                                          uint32_t analogGain,
                                          uint32_t digitalGain) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }

    const uint32_t exp = unicamClampU32(exposureLines, kExposureMin, kVts30Fps - kExposureMargin);
    const uint32_t scaledAnalog = (analogGain >= 256U) ? (analogGain >> 1) : analogGain;
    const uint16_t aGain = static_cast<uint16_t>(
        unicamClampU32(scaledAnalog, kAnalogGainMin, kAnalogGainMax));
    const uint16_t dGain = static_cast<uint16_t>(
        unicamClampU32(digitalGain < kDigitalGainMin ? kDigitalGainMin : digitalGain,
                       kDigitalGainMin, kDigitalGainMax));

    IOReturn ret = i2c->writeReg24(kRegExposure, (exp << 3) & 0x0FFFF8U);
    if (ret != kIOReturnSuccess) return ret;

    ret = i2c->writeReg16(kRegAnalogGain, aGain);
    if (ret != kIOReturnSuccess) return ret;

    if ((ret = i2c->writeReg8(kRegGroupAccess, 0x00)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegMwbRGain, dGain)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegMwbGGain, dGain)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg16(kRegMwbBGain, dGain)) != kIOReturnSuccess) return ret;
    if ((ret = i2c->writeReg8(kRegGroupAccess, 0x10)) != kIOReturnSuccess) return ret;
    return i2c->writeReg8(kRegGroupAccess, 0xA0);
}
