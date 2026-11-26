#ifndef UNICAM_ISENSOR_DRIVER_HPP
#define UNICAM_ISENSOR_DRIVER_HPP

#include <IOKit/IOReturn.h>
#include "../I2C/II2CTransport.hpp"
#include "UniCamCoreABI.h"

struct UniCamSensorModeDesc {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t hts;
    uint32_t vts;
    uint64_t linkFreqHz;
    uint64_t sclkHz;
    uint32_t bayerOrder; /* 0=SBGGR10, 1=SGRBG10, 2=SGBRG10, 3=SRGGB10 */
};

class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;

    virtual const char *name() const = 0;
    virtual uint32_t expectedChipId() const = 0;
    virtual UniCamSensorModeDesc defaultMode() const = 0;

    virtual void setBusConfig(uint32_t csiLanes, uint64_t linkFreqHz) {
        (void)csiLanes;
        (void)linkFreqHz;
    }

    virtual IOReturn identify(II2CTransport *i2c, uint32_t *outChipId, uint8_t rawBytes[4]) = 0;
    virtual IOReturn configureMode(II2CTransport *i2c, uint32_t modeIndex, uint32_t testPattern) = 0;
    virtual IOReturn setStreaming(II2CTransport *i2c, bool enable) = 0;
    virtual IOReturn setExposureAndGain(II2CTransport *i2c,
                                        uint32_t exposureLines,
                                        uint32_t analogGain,
                                        uint32_t digitalGain) = 0;
};

#endif /* UNICAM_ISENSOR_DRIVER_HPP */
