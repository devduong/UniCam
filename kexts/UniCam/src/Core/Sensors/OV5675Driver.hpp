#ifndef UNICAM_OV5675_DRIVER_HPP
#define UNICAM_OV5675_DRIVER_HPP

#include "ISensorDriver.hpp"

/**
 * I2C/CCI sensor driver for OmniVision OV5675 MIPI CSI-2 sensor (2x2 binned 1296x972 RAW10 SGRBG10).
 */
class OV5675Driver : public ISensorDriver {
public:
    OV5675Driver() = default;
    ~OV5675Driver() override = default;

    const char *name() const override { return "ov5675"; }
    uint32_t expectedChipId() const override { return UNICAM_OV5675_CHIP_ID; }
    UniCamSensorModeDesc defaultMode() const override;

    IOReturn identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) override;
    IOReturn configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) override;
    IOReturn setStreaming(II2CTransport *i2c, bool enable) override;
    IOReturn setExposureAndGain(II2CTransport *i2c,
                                uint32_t exposureLines,
                                uint32_t analogGain,
                                uint32_t digitalGain) override;
};

#endif /* UNICAM_OV5675_DRIVER_HPP */
