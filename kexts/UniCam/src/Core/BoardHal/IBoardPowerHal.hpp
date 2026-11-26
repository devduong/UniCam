#ifndef UNICAM_IBOARD_POWER_HAL_HPP
#define UNICAM_IBOARD_POWER_HAL_HPP

#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include "UniCamCoreABI.h"

class IBoardPowerHal {
public:
    virtual ~IBoardPowerHal() = default;

    virtual bool init(IOService *provider, UniCamBoardSensorInfo *outInfo) = 0;
    virtual void cleanup() = 0;

    virtual IOReturn setPower(bool enable, bool forceMmio) = 0;
    virtual IOReturn setPrivacyLed(bool enable, bool forceMmio) = 0;
    virtual IOReturn pulseReset(bool forceMmio) = 0;

    virtual void readDiagnostics(UniCamHardwareDiag *outDiag) = 0;
    virtual bool isPowered() const = 0;
    virtual bool isLedOn() const = 0;
};

#endif /* UNICAM_IBOARD_POWER_HAL_HPP */
