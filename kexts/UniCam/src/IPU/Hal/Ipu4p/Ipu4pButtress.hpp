#ifndef UNICAM_IPU4P_BUTTRESS_HPP
#define UNICAM_IPU4P_BUTTRESS_HPP

#include <IOKit/IOTypes.h>

#include "UniCamIPUABI.h"

namespace ipu4p {

class Ipu4pButtress {
public:
    Ipu4pButtress();

    void init(volatile uint8_t *bar0Base);

    bool isSecureMode() const;
    bool isAuthDone() const;
    uint32_t refClkTenthsMhz() const { return refClkTenthsMhz_; }
    uint32_t readSecurityCtl() const;
    uint32_t readPwrState() const;
    uint32_t readSku() const;
    uint64_t readTscTicks() const;
    uint64_t tscTicksToNs(uint64_t ticks) const;

    void enableIrqs(bool enable);
    void configureVcMechanism();
    void configureFabricQos();
    void prepareForPower();
    void restoreAfterWake();

    IOReturn setPower(bool isIsys, bool on, UniCamIpuBootReport *outReport = nullptr);
    IOReturn ipcReset();
    IOReturn resetAuthentication();
    bool setSecureMode(bool enable);
    IOReturn startTscSync();

    IOReturn authenticate(uint32_t psysPkgDirIova,
                          uint32_t fwSizeBytes,
                          volatile uint8_t *psysBase,
                          UniCamIpuBootReport *outReport);

private:
    bool pollPwrState(uint32_t mask, uint32_t target, uint32_t timeoutUs) const;
    IOReturn sendTscRequest();
    IOReturn ipcValidityOpen();
    void ipcValidityClose();
    IOReturn ipcSend(uint32_t cmd,
                     uint32_t expectedResp,
                     uint32_t cmdSize,
                     UniCamIpuBootReport *rep);

    volatile uint8_t *bar0_;
    uint32_t refClkTenthsMhz_;
    uint32_t wdtCached_;
    bool wdtCaptured_;
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_BUTTRESS_HPP */
