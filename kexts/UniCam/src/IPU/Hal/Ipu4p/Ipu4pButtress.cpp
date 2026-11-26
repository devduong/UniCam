#include "Ipu4pButtress.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>

namespace ipu4p {

namespace {

constexpr uint32_t kButtressPowerTimeoutUs    = 500000U;   /* 500 ms (matches iacamera64.sys) */
constexpr uint32_t kButtressIpcValidityUs     = 1000000U;  /* 1 s */
constexpr uint32_t kButtressIpcTxTimeoutUs    = 1000000U;  /* 1 s */
constexpr uint32_t kButtressBootloadTimeoutUs = 5000000U;  /* 5 s */
constexpr uint32_t kButtressAuthTimeoutUs     = 10000000U; /* 10 s */
constexpr uint32_t kButtressTscTimeoutUs      = 5000U;     /* 5 ms */
constexpr uint32_t kButtressIpcResetRetry     = 2000U;
constexpr uint32_t kButtressCseResetOuter     = 4U;

struct QosLutTriplet {
    uint8_t hi;
    uint8_t mid;
    uint8_t lo;
};

/* Exact 14-entry Buttress fabric fill-time LUT from iacamera64.sys (0x14001dca0) */
constexpr QosLutTriplet kIpu4pQosLut[14] = {
    { 0x0bU, 0x02U, 0x04U },
    { 0x0cU, 0x1aU, 0x05U },
    { 0x0dU, 0x1dU, 0x06U },
    { 0x0eU, 0x1eU, 0x07U },
    { 0x0fU, 0x1fU, 0x08U },
    { 0x10U, 0x00U, 0x09U },
    { 0x11U, 0x00U, 0x00U },
    { 0x1bU, 0x00U, 0x00U },
    { 0x1cU, 0x00U, 0x00U },
    { 0x00U, 0x00U, 0x00U },
    { 0x00U, 0x00U, 0x00U },
    { 0x00U, 0x00U, 0x00U },
    { 0x00U, 0x00U, 0x00U },
    { 0x00U, 0x00U, 0x00U },
};

} // namespace

Ipu4pButtress::Ipu4pButtress()
    : bar0_(nullptr),
      refClkTenthsMhz_(192U),
      wdtCached_(0U) {
}

void Ipu4pButtress::init(volatile uint8_t *bar0Base) {
    bar0_ = bar0Base;
    if (!bar0_ || mmioRead32(bar0_, kBtrsRegSecurityCtl) == 0xFFFFFFFFU) {
        return;
    }

    /* Decode ref_clk from BTRS_CTRL[9:8] per ipu6-buttress.c: 0->24.0, 1->19.2, 2->38.4 MHz. */
    const uint32_t ctrl = mmioRead32(bar0_, kBtrsRegCtrl);
    const uint32_t clkInd = (ctrl & kBtrsCtrlRefClkIndMask) >> 8;
    switch (clkInd) {
        case 0U:
            refClkTenthsMhz_ = 240U;
            break;
        case 1U:
            refClkTenthsMhz_ = 192U;
            break;
        case 2U:
            refClkTenthsMhz_ = 384U;
            break;
        default:
            refClkTenthsMhz_ = 192U;
            break;
    }

    wdtCached_ = mmioRead32(bar0_, kBtrsRegWdt);
    const uint32_t fabricCfg10 = mmioRead32(bar0_, kBtrsRegFabricCfg10);
    if (fabricCfg10 != 0xFFFFFFFFU) {
        mmioWrite32(bar0_, kBtrsRegFabricCfg10, fabricCfg10 | kBtrsFabricCfg10EnableBit);
    }
    configureFabricQos();
    mmioWrite32(bar0_, kBtrsRegIsrClear, 0xFFFFFFFFU);
    mmioWrite32(bar0_, kBtrsRegIsrEnable, kBtrsIrqsMask);
}

bool Ipu4pButtress::isSecureMode() const {
    if (!bar0_) return false;
    return (mmioRead32(bar0_, kBtrsRegSecurityCtl) & kBtrsSecCtlFwSecureMode) != 0;
}

bool Ipu4pButtress::isAuthDone() const {
    if (!bar0_) return false;
    if (!isSecureMode()) return true;
    return (mmioRead32(bar0_, kBtrsRegSecurityCtl) & kBtrsSecCtlFwSetupMask) == kBtrsSecCtlAuthDone;
}

uint32_t Ipu4pButtress::readSecurityCtl() const {
    return bar0_ ? mmioRead32(bar0_, kBtrsRegSecurityCtl) : 0U;
}

uint32_t Ipu4pButtress::readPwrState() const {
    return bar0_ ? mmioRead32(bar0_, kBtrsRegPwrState) : 0U;
}

uint32_t Ipu4pButtress::readSku() const {
    return bar0_ ? mmioRead32(bar0_, kBtrsRegSku) : 0U;
}

uint64_t Ipu4pButtress::readTscTicks() const {
    if (!bar0_) return 0ULL;
    uint32_t hi1 = mmioRead32(bar0_, kBtrsRegTscHi);
    uint32_t lo = mmioRead32(bar0_, kBtrsRegTscLo);
    uint32_t hi2 = mmioRead32(bar0_, kBtrsRegTscHi);
    if (hi1 == hi2) {
        return (static_cast<uint64_t>(hi1) << 32) | lo;
    }
    if (lo & ipuBit(31)) {
        return (static_cast<uint64_t>(hi1) << 32) | lo;
    }
    return (static_cast<uint64_t>(hi2) << 32) | lo;
}

uint64_t Ipu4pButtress::tscTicksToNs(uint64_t ticks) const {
    const uint32_t denom = refClkTenthsMhz_ ? refClkTenthsMhz_ : 192U;
    return (ticks * 10000ULL) / denom;
}

void Ipu4pButtress::enableIrqs(bool enable) {
    if (!bar0_) return;
    if (enable) {
        mmioWrite32(bar0_, kBtrsRegIsrClear, kBtrsIrqsMask);
        mmioWrite32(bar0_, kBtrsRegIsrEnable, kBtrsIrqsMask);
    } else {
        mmioWrite32(bar0_, kBtrsRegIsrEnable, 0U);
        mmioWrite32(bar0_, kBtrsRegIsrClear, 0xFFFFFFFFU);
    }
}

void Ipu4pButtress::configureVcMechanism() {
    if (!bar0_) return;
    uint32_t val = mmioRead32(bar0_, kBtrsRegCtrl);
    val &= ~(kBtrsCtrlStallModeVc0 | kBtrsCtrlStallModeVc1);
    mmioWrite32(bar0_, kBtrsRegCtrl, val);
}

void Ipu4pButtress::configureFabricQos() {
    if (!bar0_) return;
    uint32_t c0 = mmioRead32(bar0_, kBtrsRegFabricQosCfg0);
    if (c0 != 0xFFFFFFFFU) {
        c0 = (c0 & 0xE0000000U) | ipuBit(29);
        mmioWrite32(bar0_, kBtrsRegFabricQosCfg0, c0);
    }
    uint32_t c1 = mmioRead32(bar0_, kBtrsRegFabricQosCfg1);
    if (c1 != 0xFFFFFFFFU) {
        c1 = (c1 & 0xC000B581U) | 0x0000B581U;
        mmioWrite32(bar0_, kBtrsRegFabricQosCfg1, c1);
    }
    for (uint32_t i = 0U; i < 14U; ++i) {
        const uint32_t regOff = kBtrsRegFabricQosLutBase + i * 4U;
        uint32_t cur = mmioRead32(bar0_, regOff);
        if (cur == 0xFFFFFFFFU) {
            break;
        }
        const uint32_t packed =
            (cur & 0xFFFC0000U) |
            ((static_cast<uint32_t>(kIpu4pQosLut[i].hi) & 0x3FU) << 12) |
            ((static_cast<uint32_t>(kIpu4pQosLut[i].mid) & 0x3FU) << 6) |
            (static_cast<uint32_t>(kIpu4pQosLut[i].lo) & 0x3FU);
        mmioWrite32(bar0_, regOff, packed);
    }
}

void Ipu4pButtress::prepareForPower() {
    if (!bar0_ || mmioRead32(bar0_, kBtrsRegSecurityCtl) == 0xFFFFFFFFU) {
        return;
    }
    const uint32_t fabricCfg10 = mmioRead32(bar0_, kBtrsRegFabricCfg10);
    if (fabricCfg10 != 0xFFFFFFFFU) {
        mmioWrite32(bar0_, kBtrsRegFabricCfg10, fabricCfg10 | kBtrsFabricCfg10EnableBit);
    }
    configureFabricQos();
    mmioWrite32(bar0_, kBtrsRegIsrClear, 0xFFFFFFFFU);
    mmioWrite32(bar0_, kBtrsRegIsrEnable, kBtrsIrqsMask);
}

void Ipu4pButtress::restoreAfterWake() {
    if (!bar0_ || mmioRead32(bar0_, kBtrsRegSecurityCtl) == 0xFFFFFFFFU) {
        return;
    }
    mmioWrite32(bar0_, kBtrsRegWdt, wdtCached_);
    prepareForPower();
}

bool Ipu4pButtress::pollPwrState(uint32_t mask, uint32_t target, uint32_t timeoutUs) const {
    const uint32_t maxIters = timeoutUs / 100U;
    for (uint32_t i = 0U; i <= maxIters; ++i) {
        const uint32_t pwr = mmioRead32(bar0_, kBtrsRegPwrState);
        if (pwr == 0xFFFFFFFFU) {
            return false;
        }
        if ((pwr & mask) == target) {
            return true;
        }
        IODelay(100);
    }
    return false;
}

IOReturn Ipu4pButtress::setPower(bool isIsys, bool on, UniCamIpuBootReport *outReport) {
    if (!bar0_) return kIOReturnNotReady;

    const uint32_t freqCtlReg = isIsys ? kBtrsRegIsFreqCtl : kBtrsRegPsFreqCtl;
    const uint32_t mask = isIsys ? kBtrsPwrStateIsFsmMask : kBtrsPwrStatePsFsmMask;
    const uint32_t target = on
        ? (isIsys
            ? (kBtrsPwrStateIsFsmRdy << kBtrsPwrStateIsFsmShift)
            : (kBtrsPwrStatePsFsmPwrUp << kBtrsPwrStatePsFsmShift))
        : 0U;

    const uint32_t initialPwr = mmioRead32(bar0_, kBtrsRegPwrState);
    if (initialPwr == 0xFFFFFFFFU) {
        IOLog("UniCamIPU: Buttress %s %s aborted (PWR_STATE=0xffffffff)\n",
              isIsys ? "ISYS" : "PSYS", on ? "ON" : "OFF");
        return kIOReturnNotReady;
    }

    if ((initialPwr & mask) == target) {
        if (!on) {
            mmioWrite32(bar0_, freqCtlReg, 0U);
        }
        return kIOReturnSuccess;
    }

    if (!on) {
        mmioWrite32(bar0_, freqCtlReg, 0U);
        if (pollPwrState(mask, 0U, kButtressPowerTimeoutUs)) {
            return kIOReturnSuccess;
        }
        if (!isIsys && mmioRead32(bar0_, kBtrsRegPwrState) != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegPsFreqCtl, kBtrsPsFreqCtlOvrd);
            if (pollPwrState(mask, 0U, 100000U)) {
                mmioWrite32(bar0_, kBtrsRegPsFreqCtl, 0U);
                return kIOReturnSuccess;
            }
            const uint32_t mbx = mmioRead32(bar0_, kBtrsRegPunitMailboxInterface);
            if (mbx != 0xFFFFFFFFU && (mbx & kBtrsPunitMailboxBusy) == 0U) {
                mmioWrite32(bar0_, kBtrsRegPunitMailboxInterface, mbx | kBtrsPunitResetPsysCmd);
                if (pollPwrState(mask, 0U, 200000U)) {
                    mmioWrite32(bar0_, kBtrsRegPsFreqCtl, 0U);
                    return kIOReturnSuccess;
                }
            }
            if (mmioRead32(bar0_, kBtrsRegPwrState) != 0xFFFFFFFFU) {
                mmioWrite32(bar0_, kBtrsRegPsFreqCtl, 0U);
            }
        }
        IOLog("UniCamIPU: Buttress %s OFF timeout (pwr=0x%08x freq=0x%08x mbx=0x%08x)\n",
              isIsys ? "ISYS" : "PSYS",
              mmioRead32(bar0_, kBtrsRegPwrState),
              mmioRead32(bar0_, freqCtlReg),
              mmioRead32(bar0_, kBtrsRegPunitMailboxInterface));
        return kIOReturnTimeout;
    }

    if (isIsys) {
        /* Exact iacamera64.sys SetISysPower(1) CSI PHY control sequence before IS_FREQ_CTL. */
        const uint32_t csiPhyCtrl0 = mmioRead32(bar0_, kBtrsRegCsiPhyCtrl0);
        if (csiPhyCtrl0 != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegCsiPhyCtrl0, csiPhyCtrl0 & ~ipuBit(0));
        }

        const uint32_t csiPhyPwr = mmioRead32(bar0_, kBtrsRegCsiPhyPowerCtrl);
        if (csiPhyPwr != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegCsiPhyPowerCtrl, (csiPhyPwr & 0xF5FFFFFFU) | ipuBit(26));
        }

        const uint32_t oldIs = mmioRead32(bar0_, kBtrsRegIsFreqCtl);
        const uint32_t preserveIs = (oldIs != 0xFFFFFFFFU) ? (oldIs & 0x7FFFFF00U) : 0U;
        const uint32_t isCandidates[] = {
            preserveIs | kBtrsFreqCtlStart | 0x06U,                    // 0x80000006 (iacamera64.sys)
            preserveIs | kBtrsFreqCtlStart | kBtrsIsFreqCtlDefaultDiv, // 0x80000008 (ruslanbay/ipu4-drivers)
        };
        for (uint32_t idx = 0U; idx < (sizeof(isCandidates) / sizeof(isCandidates[0])); ++idx) {
            if (idx > 0U && mmioRead32(bar0_, kBtrsRegPwrState) == 0xFFFFFFFFU) {
                break;
            }
            mmioWrite32(bar0_, kBtrsRegIsFreqCtl, isCandidates[idx]);
            if (pollPwrState(mask, target, 250000U)) {
                IOLog("UniCamIPU: ISYS powered on (attempt=%u cmd=0x%08x pwr=0x%08x)\n",
                      idx, isCandidates[idx], mmioRead32(bar0_, kBtrsRegPwrState));
                return kIOReturnSuccess;
            }
            if (mmioRead32(bar0_, kBtrsRegPwrState) == 0xFFFFFFFFU) {
                break;
            }
        }
        const uint32_t pwrAfter = mmioRead32(bar0_, kBtrsRegPwrState);
        const uint32_t isFreqAfter = mmioRead32(bar0_, kBtrsRegIsFreqCtl);
        const uint32_t psFreqAfter = mmioRead32(bar0_, kBtrsRegPsFreqCtl);
        const uint32_t pmRespAfter = mmioRead32(bar0_, kBtrsRegPunitMailboxData);
        if (outReport) {
            outReport->pwrStateAtFail = pwrAfter;
            outReport->isFreqAtFail = isFreqAfter;
            outReport->psFreqAtFail = psFreqAfter;
            outReport->pmRespAtFail = pmRespAfter;
        }
        if (pwrAfter != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegIsFreqCtl, 0U);
        }
        IOLog("UniCamIPU: ISYS ON timeout (pwr=0x%08x is_freq=0x%08x sec=0x%08x isr=0x%08x)\n",
              pwrAfter, isFreqAfter,
              mmioRead32(bar0_, kBtrsRegSecurityCtl),
              mmioRead32(bar0_, kBtrsRegIsrStatus));
        return kIOReturnTimeout;
    }

    const uint32_t caps = mmioRead32(bar0_, kBtrsRegPsFreqCapabilities);
    const uint32_t oldPs = mmioRead32(bar0_, kBtrsRegPsFreqCtl);
    const uint32_t preservePs = (oldPs != 0xFFFFFFFFU) ? (oldPs & 0x7F0000FFU) : 0U;
    const uint32_t winCmd = preservePs | kBtrsFreqCtlStart |
        (kBtrsPsFreqCtlWinQos << kBtrsPsFreqCtlQosShift) |
        (kBtrsPsFreqCtlWinRatio << kBtrsPsFreqCtlRatioShift) | kBtrsPsFreqCtlOvrd;
    const uint32_t psCandidates[] = {
        winCmd, // Initial iacamera64.sys write
        winCmd, // Retry write without zeroing (iacamera64.sys 0x1400201b9)
        preservePs | kBtrsFreqCtlStart | (kBtrsPsFreqCtlWinQos << kBtrsPsFreqCtlQosShift) |
            (kBtrsPsFreqCtlMinRatio << kBtrsPsFreqCtlRatioShift) | kBtrsPsFreqCtlOvrd,
        preservePs | kBtrsFreqCtlStart | (kBtrsPsFreqCtlLinuxRatio << kBtrsPsFreqCtlRatioShift) |
            kBtrsPsFreqCtlOvrd,
    };

    for (uint32_t idx = 0U; idx < (sizeof(psCandidates) / sizeof(psCandidates[0])); ++idx) {
        if (idx > 0U && mmioRead32(bar0_, kBtrsRegPwrState) == 0xFFFFFFFFU) {
            break;
        }
        const uint32_t cmd = psCandidates[idx];
        /* Re-write 0x0038 directly without zeroing between attempts, matching iacamera64.sys. */
        mmioWrite32(bar0_, kBtrsRegPsFreqCtl, cmd);
        if (pollPwrState(mask, target, (idx < 2U) ? 100000U : 200000U)) {
            IOLog("UniCamIPU: PSYS powered on (attempt=%u cmd=0x%08x pwr=0x%08x)\n",
                  idx, cmd, mmioRead32(bar0_, kBtrsRegPwrState));
            return kIOReturnSuccess;
        }
        if (mmioRead32(bar0_, kBtrsRegPwrState) == 0xFFFFFFFFU) {
            break;
        }
    }

    const uint32_t pwrAfter = mmioRead32(bar0_, kBtrsRegPwrState);
    const uint32_t lastPsFreq = mmioRead32(bar0_, kBtrsRegPsFreqCtl);
    const uint32_t lastIsFreq = mmioRead32(bar0_, kBtrsRegIsFreqCtl);
    const uint32_t pmRespAfter = mmioRead32(bar0_, kBtrsRegPunitMailboxData);
    if (outReport) {
        outReport->pwrStateAtFail = pwrAfter;
        outReport->psFreqAtFail = lastPsFreq;
        outReport->isFreqAtFail = lastIsFreq;
        outReport->pmRespAtFail = pmRespAfter;
    }
    if (pwrAfter != 0xFFFFFFFFU) {
        mmioWrite32(bar0_, kBtrsRegPsFreqCtl, 0U);
    }
    IOLog("UniCamIPU: PSYS ON timeout (pwr=0x%08x ps_freq=0x%08x is_freq=0x%08x caps=0x%08x mbx=0x%08x pm_resp=0x%08x sec=0x%08x rst=0x%08x isr=0x%08x csr=0x%08x)\n",
          pwrAfter,
          lastPsFreq,
          lastIsFreq,
          caps,
          mmioRead32(bar0_, kBtrsRegPunitMailboxInterface),
          pmRespAfter,
          mmioRead32(bar0_, kBtrsRegSecurityCtl),
          mmioRead32(bar0_, kBtrsRegFwResetCtl),
          mmioRead32(bar0_, kBtrsRegIsrStatus),
          mmioRead32(bar0_, kBtrsRegCse2IuCsr));
    return kIOReturnTimeout;
}

IOReturn Ipu4pButtress::sendTscRequest() {
    mmioWrite32(bar0_, kBtrsRegFabricCmd, kBtrsFabricCmdStartTscSync);
    uint32_t val =
        (mmioRead32(bar0_, kBtrsRegPwrState) & kBtrsPwrStateHhStatusMask) >> kBtrsPwrStateHhStatusShift;
    if (val == kBtrsPwrStateHhStatusErr) {
        return kIOReturnError;
    }

    const uint32_t maxIters = kButtressTscTimeoutUs / 250U;
    for (uint32_t i = 0; i < maxIters; ++i) {
        val = (mmioRead32(bar0_, kBtrsRegPwrState) & kBtrsPwrStateHhStatusMask) >> kBtrsPwrStateHhStatusShift;
        if (val == kBtrsPwrStateHhStatusDone) {
            return kIOReturnSuccess;
        }
        IODelay(250);
    }
    return kIOReturnTimeout;
}

IOReturn Ipu4pButtress::startTscSync() {
    if (!bar0_) return kIOReturnNotReady;
    for (uint32_t trial = 0; trial < 10U; ++trial) {
        const IOReturn ret = sendTscRequest();
        if (ret != kIOReturnTimeout) {
            return ret;
        }
        uint32_t tsw = mmioRead32(bar0_, kBtrsRegTswCtl);
        mmioWrite32(bar0_, kBtrsRegTswCtl, tsw | kBtrsTswCtlSoftReset);
        mmioWrite32(bar0_, kBtrsRegTswCtl, tsw & ~kBtrsTswCtlSoftReset);
    }
    return kIOReturnTimeout;
}

IOReturn Ipu4pButtress::ipcReset() {
    if (!bar0_) return kIOReturnNotReady;
    const uint32_t initCsr = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
    if (initCsr == 0xFFFFFFFFU) {
        IOLog("UniCamIPU: CSE2IU_CSR is 0xFFFFFFFF (BAR0 wedged)\n");
        return kIOReturnNotReady;
    }
    if (!isSecureMode()) {
        return kIOReturnSuccess;
    }

    const uint32_t entry = kBtrsCsrRstPhase1;
    const uint32_t exitBit = kBtrsCsrRstPhase2;
    const uint32_t query = kBtrsCsrQueriedRstPhase;
    const uint32_t csrInClr =
        kBtrsCsrDeassertedRegValidReq | kBtrsCsrAckedRegValid | kBtrsCsrAssertedRegValidReq | query;

    for (uint32_t outer = 0; outer <= kButtressCseResetOuter; ++outer) {
        uint32_t val = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
        if (val == 0xFFFFFFFFU) {
            return kIOReturnNotReady;
        }
        mmioWrite32(bar0_, kBtrsRegCse2IuCsr, val);
        mmioWrite32(bar0_, kBtrsRegIu2CseCsr, entry);

        for (uint32_t retries = 0; retries <= kButtressIpcResetRetry; ++retries) {
            IODelay(400);
            val = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
            if (val == 0xFFFFFFFFU) {
                return kIOReturnNotReady;
            }
            if (val & entry) {
                if (val & exitBit) {
                    mmioWrite32(bar0_, kBtrsRegCse2IuCsr, entry | exitBit);
                    mmioWrite32(bar0_, kBtrsRegIu2CseCsr, query);
                } else {
                    mmioWrite32(bar0_, kBtrsRegCse2IuCsr, entry | query);
                    mmioWrite32(bar0_, kBtrsRegIu2CseCsr, entry);
                }
            } else if (val & exitBit) {
                mmioWrite32(bar0_, kBtrsRegCse2IuCsr, exitBit);
                mmioWrite32(bar0_, kBtrsRegCse2IuDb0, 0U);
                mmioWrite32(bar0_, kBtrsRegCse2IuCsr, csrInClr);
                mmioWrite32(bar0_, kBtrsRegIu2CseCsr, exitBit);
                IODelay(250);
                val = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
                if (val == 0xFFFFFFFFU) {
                    return kIOReturnNotReady;
                }
                if (val & query) {
                    continue;
                }
                return kIOReturnSuccess;
            } else if (val & query) {
                mmioWrite32(bar0_, kBtrsRegCse2IuCsr, query);
                mmioWrite32(bar0_, kBtrsRegIu2CseCsr, entry);
            }
        }
        IOLog("UniCamIPU: CSE IPC reset attempt %u timed out (csr_in=0x%08x)\n",
              outer, mmioRead32(bar0_, kBtrsRegCse2IuCsr));
    }

    return kIOReturnTimeout;
}

IOReturn Ipu4pButtress::ipcValidityOpen() {
    mmioWrite32(bar0_, kBtrsRegIu2CseCsr, kBtrsCsrAssertedRegValidReq);
    const uint32_t maxIters = kButtressIpcValidityUs / 200U;
    for (uint32_t i = 0; i < maxIters; ++i) {
        const uint32_t val = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
        if (val == 0xFFFFFFFFU) {
            return kIOReturnNotReady;
        }
        if (val & kBtrsCsrAckedRegValid) {
            return kIOReturnSuccess;
        }
        IODelay(200);
    }
    ipcValidityClose();
    return kIOReturnTimeout;
}

void Ipu4pButtress::ipcValidityClose() {
    if (mmioRead32(bar0_, kBtrsRegSecurityCtl) != 0xFFFFFFFFU) {
        mmioWrite32(bar0_, kBtrsRegIu2CseCsr, kBtrsCsrDeassertedRegValidReq);
    }
}

IOReturn Ipu4pButtress::ipcSend(uint32_t cmd,
                                uint32_t expectedResp,
                                uint32_t cmdSize,
                                UniCamIpuBootReport *rep) {
    (void)rep;
    IOReturn ret = ipcValidityOpen();
    if (ret != kIOReturnSuccess) {
        IOLog("UniCamIPU: CSE IPC validity open failed (0x%x)\n", ret);
        return ret;
    }

    const uint32_t dbVal = kBtrsIu2CseDb0Busy | (cmdSize & 0x1FU);
    for (uint32_t attempt = 0; attempt < 2U; ++attempt) {
        mmioWrite32(bar0_, kBtrsRegIsrClear,
                    kBtrsIsrIpcExecDoneByCse | kBtrsIsrIpcFromCseWaiting);
        mmioWrite32(bar0_, kBtrsRegIu2CseData0, cmd);
        mmioWrite32(bar0_, kBtrsRegIu2CseDb0, dbVal);

        bool txDone = false;
        bool rxDone = false;
        uint32_t rxData = 0U;

        const uint32_t maxIters = kButtressIpcTxTimeoutUs / 250U;
        for (uint32_t i = 0; i < maxIters; ++i) {
            const uint32_t isr = mmioRead32(bar0_, kBtrsRegIsrStatus);
            if (isr == 0xFFFFFFFFU) {
                return kIOReturnNotReady;
            }
            if (!txDone) {
                const uint32_t outDb = mmioRead32(bar0_, kBtrsRegIu2CseDb0);
                if ((isr & kBtrsIsrIpcExecDoneByCse) || !(outDb & kBtrsIu2CseDb0Busy)) {
                    if (isr & kBtrsIsrIpcExecDoneByCse) {
                        mmioWrite32(bar0_, kBtrsRegIsrClear, kBtrsIsrIpcExecDoneByCse);
                    }
                    txDone = true;
                }
            }
            if (!rxDone) {
                const uint32_t inDb = mmioRead32(bar0_, kBtrsRegCse2IuDb0);
                if ((isr & kBtrsIsrIpcFromCseWaiting) || (inDb & kBtrsCse2IuDb0Busy)) {
                    rxData = mmioRead32(bar0_, kBtrsRegCse2IuData0);
                    mmioWrite32(bar0_, kBtrsRegCse2IuDb0, 0U);
                    if (isr & kBtrsIsrIpcFromCseWaiting) {
                        mmioWrite32(bar0_, kBtrsRegIsrClear, kBtrsIsrIpcFromCseWaiting);
                    }
                    rxDone = true;
                    txDone = true;
                }
            }
            if (txDone && rxDone) {
                ipcValidityClose();
                if ((rxData & kBtrsCse2IuNackMask) == kBtrsCse2IuNack) {
                    IOLog("UniCamIPU: CSE NACK for cmd 0x%x size=%u (resp=0x%x)\n",
                          cmd, cmdSize, rxData);
                    return kIOReturnError;
                }
                if (rxData != expectedResp) {
                    IOLog("UniCamIPU: CSE cmd 0x%x size=%u unexpected resp 0x%x (expected 0x%x)\n",
                          cmd, cmdSize, rxData, expectedResp);
                    return kIOReturnError;
                }
                return kIOReturnSuccess;
            }
            IODelay(250);
        }

        if (!txDone && attempt == 0U) {
            mmioWrite32(bar0_, kBtrsRegIu2CseDb0, 0U);
            continue;
        }
        break;
    }

    ipcValidityClose();
    IOLog("UniCamIPU: CSE IPC cmd 0x%x size=%u timed out (isr=0x%08x dbOut=0x%08x dbIn=0x%08x)\n",
          cmd, cmdSize,
          mmioRead32(bar0_, kBtrsRegIsrStatus),
          mmioRead32(bar0_, kBtrsRegIu2CseDb0),
          mmioRead32(bar0_, kBtrsRegCse2IuDb0));
    return kIOReturnTimeout;
}

IOReturn Ipu4pButtress::resetAuthentication() {
    if (!bar0_) return kIOReturnNotReady;
    if (!isSecureMode()) {
        return kIOReturnSuccess;
    }

    mmioWrite32(bar0_, kBtrsRegFwResetCtl, kBtrsFwResetCtlStart);
    for (uint32_t i = 0U; i < 1000U; ++i) {
        const uint32_t val = mmioRead32(bar0_, kBtrsRegFwResetCtl);
        if (val & kBtrsFwResetCtlDone) {
            mmioWrite32(bar0_, kBtrsRegFwResetCtl, 0U);
            IODelay(500);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }
    return kIOReturnTimeout;
}

bool Ipu4pButtress::setSecureMode(bool enable) {
    if (!bar0_) return false;
    uint32_t cur = mmioRead32(bar0_, kBtrsRegSecurityCtl);
    const uint32_t want = enable
        ? (cur | kBtrsSecCtlFwSecureMode)
        : (cur & ~kBtrsSecCtlFwSecureMode);
    if (want == cur) {
        return true;
    }
    mmioWrite32(bar0_, kBtrsRegSecurityCtl, want);
    mmioWrite32(bar0_, kBtrsRegSecurityCtl, want);
    for (uint32_t i = 0U; i < 100U; ++i) {
        if (mmioRead32(bar0_, kBtrsRegSecurityCtl) == want) {
            return true;
        }
        mmioWrite32(bar0_, kBtrsRegSecurityCtl, want);
    }
    return false;
}

IOReturn Ipu4pButtress::authenticate(uint32_t psysPkgDirIova,
                                     uint32_t fwSizeBytes,
                                     volatile uint8_t *psysBase,
                                     UniCamIpuBootReport *outReport) {
    if (!bar0_) return kIOReturnNotReady;

    if (outReport) {
        outReport->securityCtlBefore = readSecurityCtl();
        outReport->secureMode = isSecureMode() ? 1 : 0;
    }
    if (mmioRead32(bar0_, kBtrsRegSecurityCtl) == 0xFFFFFFFFU) {
        if (outReport) outReport->stepFailed = kUniCamIpuBootStepBadArg;
        return kIOReturnNotReady;
    }

    if (!isSecureMode()) {
        if (outReport) {
            outReport->alreadyAuthenticated = 1;
            outReport->bootLoadOk = 1;
            outReport->authRunOk = 1;
            outReport->securityCtlAfter = readSecurityCtl();
        }
        return kIOReturnSuccess;
    }

    if (isAuthDone()) {
        if (outReport) {
            outReport->alreadyAuthenticated = 1;
            outReport->bootLoadOk = 1;
            outReport->authRunOk = 1;
            if (psysBase) {
                outReport->bootloaderMagic = mmioRead32(psysBase, kBootloaderStatusOffset);
            }
            outReport->securityCtlAfter = readSecurityCtl();
        }
        return kIOReturnSuccess;
    }

    /* Mask Buttress IRQs during synchronous IPC polling so MSI cannot race on ISR_CLEAR. */
    const uint32_t savedIsrEnable = mmioRead32(bar0_, kBtrsRegIsrEnable);
    mmioWrite32(bar0_, kBtrsRegIsrEnable, 0U);

    IOReturn ret = kIOReturnSuccess;
    if (outReport == nullptr || outReport->ipcResetOk == 0U) {
        ret = ipcReset();
        if (ret != kIOReturnSuccess) {
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepIpcReset;
            if (mmioRead32(bar0_, kBtrsRegSecurityCtl) != 0xFFFFFFFFU) {
                mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
            }
            return ret;
        }
        if (outReport) outReport->ipcResetOk = 1;
    }

    mmioWrite32(bar0_, kBtrsRegFwSourceBaseLo, psysPkgDirIova);
    mmioWrite32(bar0_, kBtrsRegFwSourceBaseHi, 0U);

    ret = ipcSend(kBtrsIu2CseBootLoad, kBtrsCse2IuBootLoadDone, 1U, outReport);
    if (ret != kIOReturnSuccess &&
        fwSizeBytes > 0U &&
        mmioRead32(bar0_, kBtrsRegSecurityCtl) != 0xFFFFFFFFU) {
        mmioWrite32(bar0_, kBtrsRegFwSourceSize, fwSizeBytes);
        ret = ipcSend(kBtrsIu2CseBootLoad, kBtrsCse2IuBootLoadDone, 4U, outReport);
    }
    if (ret != kIOReturnSuccess) {
        if (outReport) outReport->stepFailed = kUniCamIpuBootStepBootLoad;
        if (mmioRead32(bar0_, kBtrsRegSecurityCtl) != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
        }
        return ret;
    }

    uint32_t secCtl = 0U;
    bool setupOk = false;
    for (uint32_t i = 0; i < (kButtressBootloadTimeoutUs / 500U); ++i) {
        secCtl = mmioRead32(bar0_, kBtrsRegSecurityCtl);
        if (secCtl == 0xFFFFFFFFU) {
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepBootLoad;
            return kIOReturnNotReady;
        }
        const uint32_t status = secCtl & kBtrsSecCtlFwSetupMask;
        if (status == kBtrsSecCtlFwSetupDone) {
            setupOk = true;
            break;
        }
        if (status == kBtrsSecCtlAuthFailed) {
            IOLog("UniCamIPU: CSE FW_SETUP failed (sec=0x%08x)\n", secCtl);
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepBootLoad;
            mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
            return kIOReturnError;
        }
        IOSleep(1);
    }
    if (!setupOk) {
        IOLog("UniCamIPU: CSE FW_SETUP timeout (sec=0x%08x)\n", secCtl);
        if (outReport) outReport->stepFailed = kUniCamIpuBootStepBootLoad;
        mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
        return kIOReturnTimeout;
    }
    if (outReport) outReport->bootLoadOk = 1;

    if (psysBase) {
        uint32_t magic = 0U;
        bool magicOk = false;
        for (uint32_t i = 0; i < (kButtressBootloadTimeoutUs / 500U); ++i) {
            magic = mmioRead32(psysBase, kBootloaderStatusOffset);
            if (magic == kBootloaderMagicKey) {
                magicOk = true;
                break;
            }
            IOSleep(1);
        }
        if (outReport) outReport->bootloaderMagic = magic;
        if (!magicOk) {
            IOLog("UniCamIPU: Bootloader magic mismatch 0x%08x (expected 0x%08x)\n",
                  magic, kBootloaderMagicKey);
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepBootMagic;
            mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
            return kIOReturnTimeout;
        }
    }

    ret = ipcSend(kBtrsIu2CseAuthRun, kBtrsCse2IuAuthRunDone, 1U, outReport);
    if (ret != kIOReturnSuccess) {
        if (outReport) outReport->stepFailed = kUniCamIpuBootStepAuthRun;
        if (mmioRead32(bar0_, kBtrsRegSecurityCtl) != 0xFFFFFFFFU) {
            mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
        }
        return ret;
    }

    bool authOk = false;
    for (uint32_t i = 0; i < (kButtressAuthTimeoutUs / 1000U); ++i) {
        secCtl = mmioRead32(bar0_, kBtrsRegSecurityCtl);
        if (secCtl == 0xFFFFFFFFU) {
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepAuthRun;
            return kIOReturnNotReady;
        }
        const uint32_t status = secCtl & kBtrsSecCtlFwSetupMask;
        if (status == kBtrsSecCtlAuthDone) {
            authOk = true;
            break;
        }
        if (status == kBtrsSecCtlAuthFailed) {
            IOLog("UniCamIPU: CSE AUTH_RUN failed (sec=0x%08x)\n", secCtl);
            if (outReport) outReport->stepFailed = kUniCamIpuBootStepAuthRun;
            mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
            return kIOReturnError;
        }
        IOSleep(1);
    }

    if (outReport) {
        outReport->authRunOk = authOk ? 1 : 0;
        outReport->securityCtlAfter = readSecurityCtl();
        if (!authOk) {
            IOLog("UniCamIPU: CSE AUTH_RUN timeout (sec=0x%08x)\n", secCtl);
            outReport->stepFailed = kUniCamIpuBootStepAuthRun;
        }
    }

    mmioWrite32(bar0_, kBtrsRegIsrEnable, savedIsrEnable);
    return authOk ? kIOReturnSuccess : kIOReturnTimeout;
}

} // namespace ipu4p
