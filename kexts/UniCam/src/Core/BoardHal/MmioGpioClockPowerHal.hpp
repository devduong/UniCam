#ifndef UNICAM_MMIO_GPIO_CLOCK_POWER_HAL_HPP
#define UNICAM_MMIO_GPIO_CLOCK_POWER_HAL_HPP

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOReturn.h>
#include "UniCamCoreABI.h"

struct UniCamGpioPinSpec {
    uint32_t role;
    uint64_t dw0PhysAddr;
    uint32_t activeHigh;
    uint32_t delayUs;
};

class MmioGpioClockPowerHal {
public:
    MmioGpioClockPowerHal();
    ~MmioGpioClockPowerHal();

    bool init(const UniCamGpioPinSpec pins[kUniCamGpioRoleCount],
              uint64_t mclkRegPhysAddr,
              uint64_t mclkHz);
    void cleanup();

    IOReturn setPower(bool enable);
    IOReturn setPrivacyLed(bool enable);
    IOReturn pulseReset();

    void readDiagnostics(UniCamHardwareDiag *outDiag) const;

private:
    struct MappedReg {
        uint64_t physAddr;
        IOMemoryDescriptor *desc;
        IOMemoryMap *map;
        volatile uint32_t *ptr32;
        uint32_t activeHigh;
        uint32_t delayUs;
        bool valid;
    };

    bool mapPhysicalDword(uint64_t physAddr, bool isIclk, MappedReg *outReg);
    void unmapReg(MappedReg *reg);
    IOReturn writeGpioPin(uint32_t role, bool sensorOn);
    IOReturn writeMclk(bool enable);

    MappedReg pins_[kUniCamGpioRoleCount];
    MappedReg mclkReg_;
    uint64_t mclkHz_;
};

#endif /* UNICAM_MMIO_GPIO_CLOCK_POWER_HAL_HPP */
