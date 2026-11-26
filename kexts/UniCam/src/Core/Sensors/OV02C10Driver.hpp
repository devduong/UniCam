#ifndef UNICAM_OV02C10_DRIVER_HPP
#define UNICAM_OV02C10_DRIVER_HPP

#include "ISensorDriver.hpp"

/**
 * I2C/CCI sensor driver for OmniVision OV02C10 MIPI CSI-2 sensor (1928x1092 RAW10 SGRBG10).
 */
class OV02C10Driver : public ISensorDriver {
public:
    OV02C10Driver();
    ~OV02C10Driver() override = default;

    const char *name() const override { return "ov02c10"; }
    uint32_t expectedChipId() const override { return UNICAM_OV02C10_CHIP_ID; }
    UniCamSensorModeDesc defaultMode() const override;
    void setBusConfig(uint32_t csiLanes, uint64_t linkFreqHz) override;

    IOReturn identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) override;
    IOReturn configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) override;
    IOReturn setStreaming(II2CTransport *i2c, bool enable) override;
    IOReturn setExposureAndGain(II2CTransport *i2c,
                                uint32_t exposureLines,
                                uint32_t analogGain,
                                uint32_t digitalGain) override;

private:
    uint32_t csiLanes_;
    uint32_t activeVts_;
};

#endif /* UNICAM_OV02C10_DRIVER_HPP */
