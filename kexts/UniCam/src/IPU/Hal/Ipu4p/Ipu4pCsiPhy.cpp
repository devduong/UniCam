#include "Ipu4pCsiPhy.hpp"
#include "Ipu4pFwAbi.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>

namespace ipu4p {

namespace {

struct PhyBbConfig {
    uint8_t bb;
    uint8_t crc;
    uint8_t drc;
    uint8_t afe;
};

constexpr PhyBbConfig kPhyBbConfigs[] = {
    { 4U,  13U, 32U, 0x0fU },
    { 6U,  13U, 32U, 0x15U },
    { 8U,  13U, 32U, 0x15U },
    { 10U, 13U, 32U, 0x15U },
    { 12U, 13U, 32U, 0x0fU },
    { 14U, 13U, 32U, 0x15U },
};

} // namespace

bool Ipu4pCsiPhy::mapFwPortToHwIndex(uint32_t fwPort, uint32_t *outHwIndex) {
    if (!outHwIndex) return false;
    uint32_t index = 0U;
    if (fwPort == kFwSrcPort3) {
        index = 0U;
    } else if (fwPort >= kFwSrcPort6 && fwPort <= kFwSrcPort12) {
        index = fwPort - kFwSrcPort5;
    } else {
        return false;
    }
    if (index >= kIsysCsi2Nports) {
        return false;
    }
    *outHwIndex = index;
    return true;
}

void Ipu4pCsiPhy::calcTiming(uint64_t linkFreqHz, Csi2Timing *outTiming) {
    if (!outTiming) return;
    /* Exact iacamera64.sys CCsiRx::ConfigMipiClk (0x14004e323..0x14004e390):
     * dataRateBps = linkFreqHz * 2, mbps = dataRateBps / 1,000,000.
     * At 400 MHz (800 Mbps): csettle = 1360 (0x550), dsettle = 1320 (0x528), termen = 0.
     */
    const uint64_t dataRateBps = linkFreqHz * 2ULL;
    const uint32_t mbps = static_cast<uint32_t>(dataRateBps / 1000000ULL);

    outTiming->ctermen = 0U;
    outTiming->dtermen = 0U;
    if (mbps <= 1U) {
        outTiming->csettle = 0U;
        outTiming->dsettle = 0U;
        return;
    }

    const int32_t cSub = (dataRateBps >= 1000000000ULL)
        ? static_cast<int32_t>(0x000E2900U / mbps)
        : static_cast<int32_t>(0x0001F400U / mbps);
    const int32_t dSub = (dataRateBps >= 1000000000ULL)
        ? static_cast<int32_t>(0x000CB200U / mbps)
        : static_cast<int32_t>(0x00007D00U / mbps);

    const int32_t cVal = 0x5F0 - cSub;
    const int32_t dVal = 0x550 - dSub;
    outTiming->csettle = (cVal > 0) ? static_cast<uint32_t>(cVal) : 0U;
    outTiming->dsettle = (dVal > 0) ? static_cast<uint32_t>(dVal) : 0U;
}

void Ipu4pCsiPhy::irqSetup(volatile uint8_t *isysBase) {
    if (!isysBase) return;

    struct IrqBlockCfg {
        uint32_t base;
        uint32_t mask;
    };
    const IrqBlockCfg irqConfigs[] = {
        { kIsysUnispartIrqEdge, kIsysUnispartIrqSw | ipuBit(3) | ipuBit(4) },
        { kIsysIsaAccIrqCtrlBase, 0U },
        { kIsysAIrqCtrlBase, 0U },
        { kIsysSip0IrqCtrlBase, kIsysSip0Csi2Irq },
        { kIsysSip1IrqCtrlBase, kIsysSip1Csi2IrqMask },
    };
    const uint32_t count = sizeof(irqConfigs) / sizeof(irqConfigs[0]);

    for (uint32_t i = 0; i < count; ++i) {
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqEnableOffset, 0U);
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqMaskOffset, 0U);
    }

    for (uint32_t i = 0; i < count; ++i) {
        mmioWrite32(isysBase, irqConfigs[i].base, irqConfigs[i].mask);
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqLnpOffset, irqConfigs[i].mask);
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqClearOffset, 0xFFFFFFFFU);
    }

    mmioWrite32(isysBase, kIsysUnispartSwIrqReg, 0U);
    mmioWrite32(isysBase, kIsysUnispartSwIrqMuxReg, 0U);

    for (uint32_t idx = count; idx > 0; --idx) {
        const uint32_t i = idx - 1U;
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqMaskOffset, irqConfigs[i].mask);
        mmioWrite32(isysBase, irqConfigs[i].base + kIsysIrqEnableOffset, irqConfigs[i].mask);
    }
}

void Ipu4pCsiPhy::phySetup(volatile uint8_t *bar0Base, volatile uint8_t *isysBase) {
    if (!bar0Base || !isysBase) return;

    mmioWrite32(isysBase, kIsysGpOffset + kCsi2GpregCrPortConfig, kCsi2GpregCrPortConfigVal);
    mmioWrite32(isysBase, kIsysComboGpOffset + kCsi2GpregCrPortConfig, kCsi2GpregCrPortConfigVal);

    const uint32_t bscan = mmioRead32(bar0Base, kBtrsRegCsiBscanExclude);
    if (bscan != 0xFFFFFFFFU) {
        mmioWrite32(bar0Base, kBtrsRegCsiBscanExclude, (bscan & 0xF8040201U) | kCsiBscanExcludeVal);
    } else {
        mmioWrite32(bar0Base, kBtrsRegCsiBscanExclude, kCsiBscanExcludeVal);
    }

    /* Enable DMA flush-for-idrain on all 3 IPU4P IBufCtrl blocks (lts_ipu4-isys.c / iacamera64.sys). */
    mmioWrite32(isysBase, kIsysCl0IbufCtlFlushIdrain, 1U);
    mmioWrite32(isysBase, kIsysCl1IbufCtlFlushIdrain, 1U);
    mmioWrite32(isysBase, kIsysIbufCtlFlushIdrain, 1U);

    const uint32_t nBb = sizeof(kPhyBbConfigs) / sizeof(kPhyBbConfigs[0]);
    for (uint32_t i = 0; i < nBb; ++i) {
        const PhyBbConfig &cfg = kPhyBbConfigs[i];
        const uint32_t stride = (static_cast<uint32_t>(cfg.bb) >> 1) * 0x100U;

        uint32_t val = mmioRead32(bar0Base, kBtrsRegCphy0DllOvrd + stride);
        val &= ~ipuMask(6, 1);
        val |= ((static_cast<uint32_t>(cfg.crc) << 1) & ipuMask(6, 1)) | ipuBit(0);
        mmioWrite32(bar0Base, kBtrsRegCphy0DllOvrd + stride, val);

        val = mmioRead32(bar0Base, kBtrsRegDphy0DllOvrd + stride);
        val &= ~ipuMask(6, 1);
        val |= ((static_cast<uint32_t>(cfg.drc) << 1) & ipuMask(6, 1)) | ipuBit(0);
        mmioWrite32(bar0Base, kBtrsRegDphy0DllOvrd + stride, val);

        /* CCsiRx::ConfigBBAFE (iacamera64.sys 0x14004e1b7..0x14004e1ea):
         * bit 30 | (0x08 << 23) | (0x08 << 17) | (0x08 << 11) | (afe & 0x7ff).
         */
        constexpr uint32_t kAfeBiasSlice = 0x08U;
        val = ipuBit(30) |
              (kAfeBiasSlice << 23) |
              (kAfeBiasSlice << 17) |
              (kAfeBiasSlice << 11) |
              (static_cast<uint32_t>(cfg.afe) & 0x7FFU);
        mmioWrite32(bar0Base, kBtrsRegBb0AfeConfig + stride, val);
    }
}

IOReturn Ipu4pCsiPhy::setPortPower(volatile uint8_t *isysBase,
                                   uint32_t hwPortIndex,
                                   uint32_t nlanes,
                                   const Csi2Timing *timing,
                                   uint32_t shiftedDataTicks,
                                   bool on) {
    if (!isysBase || hwPortIndex >= kIsysCsi2Nports) {
        return kIOReturnBadArgument;
    }

    const uint32_t portBase = kIsysPortOffsets[hwPortIndex];
    const uint32_t irqCtrl = isysCsiIrqCtrlBase(hwPortIndex);
    const uint32_t irqCtrl0 = isysCsiIrqCtrl0Base(hwPortIndex);

    if (!on) {
        uint32_t cfg = mmioRead32(isysBase, portBase + kCsi2RxConfig);
        cfg &= ~(kCsi2RxConfigDisableByteClkGate | kCsi2RxConfigReleaseLp11);
        mmioWrite32(isysBase, portBase + kCsi2RxConfig, cfg);
        mmioWrite32(isysBase, portBase + kCsi2RxEnable, 0U);

        mmioWrite32(isysBase, irqCtrl + kIsysIrqMaskOffset, 0U);
        mmioWrite32(isysBase, irqCtrl + kIsysIrqEnableOffset, 0U);
        mmioWrite32(isysBase, irqCtrl + kIsysIrqClearOffset, 0xFFFFFFFFU);

        mmioWrite32(isysBase, irqCtrl0 + kIsysIrqMaskOffset, 0U);
        mmioWrite32(isysBase, irqCtrl0 + kIsysIrqEnableOffset, 0U);
        mmioWrite32(isysBase, irqCtrl0 + kIsysIrqClearOffset, 0xFFFFFFFFU);
        return kIOReturnSuccess;
    }

    if (nlanes != 1U && nlanes != 2U && nlanes != 4U) {
        return kIOReturnBadArgument;
    }
    if (!timing && shiftedDataTicks == 0U) {
        return kIOReturnBadArgument;
    }

    const uint32_t cTermVal = (shiftedDataTicks != 0U || !timing) ? 0U : timing->ctermen;
    const uint32_t cSettleVal = (shiftedDataTicks != 0U) ? shiftedDataTicks : timing->csettle;
    const uint32_t dTermVal = (shiftedDataTicks != 0U || !timing) ? 0U : timing->dtermen;
    const uint32_t dSettleVal = (shiftedDataTicks != 0U) ? shiftedDataTicks : timing->dsettle;

    mmioWrite32(isysBase, portBase + kCsi2RxEnable, 0U);
    mmioWrite32(isysBase, portBase + kCsi2RxNofEnabledLanes, nlanes);

    uint32_t cfg = mmioRead32(isysBase, portBase + kCsi2RxConfig);
    cfg |= (kCsi2RxConfigDisableByteClkGate | kCsi2RxConfigReleaseLp11);
    mmioWrite32(isysBase, portBase + kCsi2RxConfig, cfg);

    /* Port-dependent D-PHY delay register offsets (+0x30..+0x3c+8*lane for combo ports hw1/hw2). */
    mmioWrite32(isysBase, portBase + csi2RxDlyCntSettleClane(hwPortIndex), cSettleVal);
    mmioWrite32(isysBase, portBase + csi2RxDlyCntTermenClane(hwPortIndex), cTermVal);
    for (uint32_t lane = 0; lane < nlanes; ++lane) {
        mmioWrite32(isysBase, portBase + csi2RxDlyCntSettleDlane(hwPortIndex, lane), dSettleVal);
        mmioWrite32(isysBase, portBase + csi2RxDlyCntTermenDlane(hwPortIndex, lane), dTermVal);
    }

    mmioWrite32(isysBase, irqCtrl, kIsysCsiIrqActive);
    mmioWrite32(isysBase, irqCtrl + kIsysIrqLnpOffset, 0U);
    mmioWrite32(isysBase, irqCtrl + kIsysIrqClearOffset, 0xFFFFFFFFU);
    mmioWrite32(isysBase, irqCtrl + kIsysIrqMaskOffset, kIsysCsiIrqActive);
    mmioWrite32(isysBase, irqCtrl + kIsysIrqEnableOffset, kIsysCsiIrqActive);

    constexpr uint32_t kCsi2PartIrqMask = kIsysCsiErrorMask | ipuBit(16) | ipuBit(17);
    mmioWrite32(isysBase, irqCtrl0, kCsi2PartIrqMask);
    mmioWrite32(isysBase, irqCtrl0 + kIsysIrqLnpOffset, 0U);
    mmioWrite32(isysBase, irqCtrl0 + kIsysIrqClearOffset, 0xFFFFFFFFU);
    mmioWrite32(isysBase, irqCtrl0 + kIsysIrqMaskOffset, kCsi2PartIrqMask);
    mmioWrite32(isysBase, irqCtrl0 + kIsysIrqEnableOffset, kCsi2PartIrqMask);

    mmioWrite32(isysBase, portBase + kCsi2RxEnable, kCsi2RxEnableEnable);
    (void)mmioRead32(isysBase, portBase + kCsi2RxEnable);
    return kIOReturnSuccess;
}

uint32_t Ipu4pCsiPhy::collectAndClearPortErrors(volatile uint8_t *isysBase, uint32_t hwPortIndex) {
    if (!isysBase || hwPortIndex >= kIsysCsi2Nports) return 0U;

    const uint32_t irqCtrl = isysCsiIrqCtrlBase(hwPortIndex);
    const uint32_t ctrlStatus = mmioRead32(isysBase, irqCtrl + kIsysIrqStatusOffset);
    if (ctrlStatus) {
        mmioWrite32(isysBase, irqCtrl + kIsysIrqClearOffset, ctrlStatus);
    }

    const uint32_t irqCtrl0 = isysCsiIrqCtrl0Base(hwPortIndex);
    const uint32_t errStatus = mmioRead32(isysBase, irqCtrl0 + kIsysIrqStatusOffset);
    if (errStatus) {
        mmioWrite32(isysBase, irqCtrl0 + kIsysIrqClearOffset, errStatus);
    }

    return errStatus & kIsysCsiErrorMask;
}

} // namespace ipu4p
