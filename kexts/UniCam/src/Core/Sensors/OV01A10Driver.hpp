#ifndef UNICAM_OV01A10_DRIVER_HPP
#define UNICAM_OV01A10_DRIVER_HPP

#include "ISensorDriver.hpp"

class OV01A10Driver : public ISensorDriver {
public:
    explicit OV01A10Driver(bool isMonoVariant = false);
    ~OV01A10Driver() override = default;

    const char *name() const override { return isMonoVariant_ ? "ov01a1b" : "ov01a10"; }
    uint32_t expectedChipId() const override { return UNICAM_OV01A10_CHIP_ID; }
    UniCamSensorModeDesc defaultMode() const override;

    IOReturn identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) override;
    IOReturn configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) override;
    IOReturn setStreaming(II2CTransport *i2c, bool enable) override;
    IOReturn setExposureAndGain(II2CTransport *i2c,
                                uint32_t exposureLines,
                                uint32_t analogGain,
                                uint32_t digitalGain) override;

private:
    bool isMonoVariant_;
};

#endif /* UNICAM_OV01A10_DRIVER_HPP */
