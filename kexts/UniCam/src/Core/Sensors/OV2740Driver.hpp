#ifndef UNICAM_OV2740_DRIVER_HPP
#define UNICAM_OV2740_DRIVER_HPP

#include "ISensorDriver.hpp"

/**
 * I2C/CCI sensor driver for OmniVision OV2740 MIPI CSI-2 sensor (1932x1092 RAW10 SGRBG10).
 */
class OV2740Driver : public ISensorDriver {
public:
    OV2740Driver();
    ~OV2740Driver() override = default;

    const char *name() const override { return "ov2740"; }
    uint32_t expectedChipId() const override { return UNICAM_OV2740_CHIP_ID; }
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
    bool use180Mhz_() const;

    uint32_t csiLanes_;
    uint64_t linkFreqHz_;
    uint32_t activeVts_;
};

#endif /* UNICAM_OV2740_DRIVER_HPP */
