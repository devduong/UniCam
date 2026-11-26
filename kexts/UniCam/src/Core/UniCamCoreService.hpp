#ifndef UNICAM_CORE_SERVICE_HPP
#define UNICAM_CORE_SERVICE_HPP

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/IOPM.h>

#include "UniCamCoreABI.h"
#include "BoardHal/AcpiMethodPowerHal.hpp"
#include "I2C/VoodooI2CNubTransport.hpp"
#include "Sensors/ISensorDriver.hpp"
#include "VoodooI2CDeviceNub.hpp"

class UniCamCoreUserClient;

class UniCamCoreService : public IOService {
    OSDeclareDefaultStructors(UniCamCoreService)

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;

    static UniCamCoreService *sharedInstance();
    uint32_t activeBayerOrder() const { return bayerOrder_; }

    // Gated operations invoked by UniCamCoreUserClient and in-kernel ISP/UVC consumers
    IOReturn getInfo(UniCamBoardSensorInfo *outInfo);
    IOReturn getDiagnostics(UniCamHardwareDiag *outDiag);
    IOReturn probeSensor(uint32_t holdLedMs, bool forceMmio, UniCamProbeReport *outReport);
    IOReturn setPower(bool enable, bool forceMmio, UniCamCoreUserClient *owner);
    IOReturn setPrivacyLed(bool enable, bool forceMmio, UniCamCoreUserClient *owner);
    IOReturn resetSensor(bool forceMmio);
    IOReturn readRegister(uint16_t reg, uint8_t width, uint32_t *outVal);
    IOReturn writeRegister(uint16_t reg, uint8_t width, uint32_t val);
    IOReturn configureSensor(uint32_t modeIndex, uint32_t testPattern);
    IOReturn setStream(bool enable, UniCamCoreUserClient *owner);
    IOReturn setExposureGain(uint32_t exposureLines, uint32_t analogGain, uint32_t digitalGain);

    void handleClientDisconnect(UniCamCoreUserClient *client);

private:
    static IOReturn gatedActionTrampoline(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    IOReturn probeSensorLocked(uint32_t holdLedMs, bool forceMmio, UniCamProbeReport *outReport);
    IOReturn setPowerLocked(bool enable, bool forceMmio, UniCamCoreUserClient *owner);
    IOReturn setPrivacyLedLocked(bool enable, bool forceMmio, UniCamCoreUserClient *owner);
    IOReturn setStreamLocked(bool enable, UniCamCoreUserClient *owner);
    void forceIdleHardwareLocked();
    void fillInfoLocked(UniCamBoardSensorInfo *outInfo);
    void fillDiagLocked(UniCamHardwareDiag *outDiag);

    VoodooI2CDeviceNub *nub_;
    IOWorkLoop *workLoop_;
    IOCommandGate *commandGate_;

    AcpiMethodPowerHal powerHal_;
    VoodooI2CNubTransport i2cTransport_;
    ISensorDriver *sensorDriver_;

    UniCamBoardSensorInfo boardInfo_;
    uint32_t bayerOrder_;
    bool streaming_;
    bool sleepSuspended_;
    UniCamCoreUserClient *activeOwner_;
};

#endif /* UNICAM_CORE_SERVICE_HPP */
