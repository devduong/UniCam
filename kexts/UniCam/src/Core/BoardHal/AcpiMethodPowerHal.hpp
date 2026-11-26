#ifndef UNICAM_ACPI_METHOD_POWER_HAL_HPP
#define UNICAM_ACPI_METHOD_POWER_HAL_HPP

#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSDictionary.h>
#include "IBoardPowerHal.hpp"
#include "MmioGpioClockPowerHal.hpp"
#include "Tps68470PowerHal.hpp"

class AcpiMethodPowerHal : public IBoardPowerHal {
public:
    AcpiMethodPowerHal();
    ~AcpiMethodPowerHal() override;

    bool init(IOService *provider, UniCamBoardSensorInfo *outInfo) override;
    void cleanup() override;

    IOReturn setPower(bool enable, bool forceMmio) override;
    IOReturn setPrivacyLed(bool enable, bool forceMmio) override;
    IOReturn pulseReset(bool forceMmio) override;

    void readDiagnostics(UniCamHardwareDiag *outDiag) override;
    bool isPowered() const override { return powered_; }
    bool isLedOn() const override { return ledOn_; }

private:
    OSObject *lookupProperty(const char *key) const;
    bool readUInt32Prop(const char *key, uint32_t *outVal) const;
    bool readUInt64Prop(const char *key, uint64_t *outVal) const;
    bool readStringProp(const char *key, char *outBuf, size_t bufSize) const;
    IOReturn evaluateIntMethod(const char *methodName, bool hasArg, uint32_t argVal, UInt32 *outResult);

    IOService *provider_;
    IOACPIPlatformDevice *acpiDevice_;
    OSDictionary *deviceProps_;
    MmioGpioClockPowerHal mmioHal_;
    Tps68470PowerHal tpsHal_;

    bool useTps68470_;
    bool hasUcin_;
    bool hasUcpw_;
    bool hasUcld_;
    bool hasUcrs_;
    uint32_t capMask_;
    bool powered_;
    bool ledOn_;
    bool lastUsedMmio_;
};

#endif /* UNICAM_ACPI_METHOD_POWER_HAL_HPP */
