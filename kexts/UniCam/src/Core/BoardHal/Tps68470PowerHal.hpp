#ifndef UNICAM_TPS68470_POWER_HAL_HPP
#define UNICAM_TPS68470_POWER_HAL_HPP

#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include "UniCamCoreABI.h"
#include "../VoodooI2CDeviceNub.hpp"

/**
 * Intermediate IOService matching ACPI/I2C device UNIC0002 (INT3472 - TI TPS68470 PMIC)
 * to provide camera PMIC I2C register access to UniCamCoreService.
 */
class UniCamPmicService : public IOService {
    OSDeclareDefaultStructors(UniCamPmicService)

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    static VoodooI2CDeviceNub *sharedNub();

private:
    VoodooI2CDeviceNub *nub_;
};

struct UniCamTps68470Config {
    uint64_t mclkHz;
    uint32_t dvddMv;
    uint32_t avddMv;
    uint32_t dovddMv;
    uint32_t resetPin;      /* 0..6 regular, 7..9 SGPO (default 9 = s_resetn) */
    uint32_t pwdnPin;       /* 0..6 regular, 7..9 SGPO (default 7 = s_enable) */
    uint32_t ledPin;        /* 0xFF if absent */
    uint8_t  resetActiveHigh;
    uint8_t  pwdnActiveHigh;
    uint8_t  ledActiveHigh;
    uint8_t  daisyChain;    /* 1 = enable S_I2C_CTL + GPCTL1/2 input passthrough */
};

/**
 * I2C power, clock (PLL), and GPIO backend for the TI TPS68470 camera PMIC (INT3472 / UNIC0002).
 */
class Tps68470PowerHal {
public:
    Tps68470PowerHal();
    ~Tps68470PowerHal();

    void configure(const UniCamTps68470Config &cfg);
    void cleanup();

    IOReturn setPower(bool enable);
    IOReturn setPrivacyLed(bool enable);
    IOReturn pulseReset();

    void readDiagnostics(UniCamHardwareDiag *outDiag) const;
    bool isAvailable() const { return resolveNub() != nullptr; }
    bool isPowered() const { return powered_; }
    bool isLedOn() const { return ledOn_; }

private:
    VoodooI2CDeviceNub *resolveNub() const;
    IOReturn readReg8(uint8_t reg, uint8_t *outVal) const;
    IOReturn writeReg8(uint8_t reg, uint8_t val) const;
    IOReturn updateBits8(uint8_t reg, uint8_t mask, uint8_t val) const;
    IOReturn writeGpioPin(uint32_t pin, uint8_t activeHigh, bool sensorOn) const;
    IOReturn programPllAndClock(bool enable) const;

    UniCamTps68470Config cfg_;
    mutable uint8_t revId_;
    bool configured_;
    bool powered_;
    bool ledOn_;
};

#endif /* UNICAM_TPS68470_POWER_HAL_HPP */
