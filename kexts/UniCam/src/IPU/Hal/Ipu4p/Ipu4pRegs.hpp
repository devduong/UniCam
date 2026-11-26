#ifndef UNICAM_IPU4P_REGS_HPP
#define UNICAM_IPU4P_REGS_HPP

#include <stddef.h>
#include <stdint.h>

namespace ipu4p {

constexpr uint32_t ipuBit(uint32_t n) {
    return (1U << n);
}

constexpr uint32_t ipuMask(uint32_t hi, uint32_t lo) {
    return (hi == 31U && lo == 0U)
        ? 0xFFFFFFFFU
        : (((1U << (hi - lo + 1U)) - 1U) << lo);
}

constexpr uint64_t ipuMask64(uint32_t hi, uint32_t lo) {
    return ((1ULL << (hi - lo + 1U)) - 1ULL) << lo;
}

static inline uint32_t mmioRead32(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static inline void mmioWrite32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    *reinterpret_cast<volatile uint32_t *>(base + offset) = val;
}

static inline void ipuWmb() {
    asm volatile("mfence" ::: "memory");
}

static inline void ipuClflushRange(const void *addr, size_t size) {
    if (!addr || size == 0) {
        return;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(63ULL);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + size + 63ULL) & ~static_cast<uintptr_t>(63ULL);
    asm volatile("mfence" ::: "memory");
    for (uintptr_t p = start; p < end; p += 64ULL) {
        asm volatile("clflush (%0)" :: "r"(reinterpret_cast<const void *>(p)) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

/* Subsystem offsets relative to PCI BAR0 */
constexpr uint32_t kIsysOffset                     = 0x00100000U;
constexpr uint32_t kIsysSpcOffset                  = 0x00000000U;
constexpr uint32_t kIsysDmemOffset                 = 0x00008000U;

constexpr uint32_t kPsysOffset                     = 0x00400000U;
constexpr uint32_t kPsysSpcOffset                  = 0x00000000U;
constexpr uint32_t kPsysDmemOffset                 = 0x00008000U;

/* MMU offsets relative to respective subsystem base (ISYS or PSYS) */
constexpr uint32_t kIsysIommu0Offset               = 0x000e0000U;
constexpr uint32_t kIsysIommu1Offset               = 0x000e0100U;
constexpr uint32_t kPsysIommu0Offset               = 0x000b0000U;
constexpr uint32_t kPsysIommu1Offset               = 0x000b0100U;
constexpr uint32_t kPsysIommu1rOffset              = 0x000b0600U;

constexpr uint32_t kMmuRegTlbInvalidate            = 0x0000U;
constexpr uint32_t kMmuRegL1Phys                   = 0x0004U;
constexpr uint32_t kMmuRegInfo                     = 0x0008U;
constexpr uint32_t kMmuL1StreamIdOffset            = 0x000cU;
constexpr uint32_t kMmuL2StreamIdOffset            = 0x004cU;
constexpr uint32_t kMmuInfoDestPrimary             = ipuBit(4);
constexpr uint32_t kMmuMaxL1Streams                = 16U;
constexpr uint32_t kMmuMaxL2Streams                = 16U;
constexpr uint16_t kMmuMaxL1BlockAddr              = 128U;
constexpr uint16_t kMmuMaxL2BlockAddr              = 32U;

/* Buttress registers relative to PCI BAR0 */
constexpr uint32_t kBtrsRegWdt                     = 0x0008U;
constexpr uint32_t kBtrsRegCtrl                    = 0x000cU;
constexpr uint32_t kBtrsCtrlStallModeVc0           = ipuBit(0);
constexpr uint32_t kBtrsCtrlStallModeVc1           = ipuBit(1);
constexpr uint32_t kBtrsCtrlRefClkIndMask          = ipuMask(9, 8);
constexpr uint32_t kBtrsRegFabricCfg10             = 0x0010U;
constexpr uint32_t kBtrsFabricCfg10EnableBit       = ipuBit(5);
constexpr uint32_t kBtrsRegFabricCfg20             = 0x0020U;
constexpr uint32_t kBtrsFabricCfg20EnableBit       = ipuBit(8);

constexpr uint32_t kBtrsRegFwResetCtl              = 0x0030U;
constexpr uint32_t kBtrsFwResetCtlStart            = ipuBit(0);
constexpr uint32_t kBtrsFwResetCtlDone             = ipuBit(1);
constexpr uint32_t kBtrsRegIsFreqCtl               = 0x0034U;
constexpr uint32_t kBtrsRegPsFreqCtl               = 0x0038U;
constexpr uint32_t kBtrsFreqCtlStart               = ipuBit(31);
constexpr uint32_t kBtrsIsFreqCtlDefaultDiv        = 0x08U; /* 1600 / 400 MHz = 4, shifted left by 1 */
constexpr uint32_t kBtrsIsFreqCtlFallbackDiv       = 0x0aU;
constexpr uint32_t kBtrsPsFreqCtlLinuxRatio        = 0x12U; /* 450 MHz = 18 * 25 MHz (lts_ipu4.c) */
constexpr uint32_t kBtrsPsFreqCtlWinRatio          = 0x08U; /* 200 MHz = 8 * 25 MHz (iacamera64.sys) */
constexpr uint32_t kBtrsPsFreqCtlWinQos            = 0x07U; /* 175 MHz = 7 * 25 MHz */
constexpr uint32_t kBtrsPsFreqCtlMinRatio          = 0x0aU; /* 250 MHz = 10 * 25 MHz (fuse 0xf7498) */
constexpr uint32_t kBtrsPsFreqCtlDefaultRatio      = kBtrsPsFreqCtlLinuxRatio;
constexpr uint32_t kBtrsPsFreqCtlDefaultQos        = kBtrsPsFreqCtlWinQos;
constexpr uint32_t kBtrsPsFreqCtlRatioShift        = 8U;
constexpr uint32_t kBtrsPsFreqCtlQosShift          = 16U;
constexpr uint32_t kBtrsPsFreqCtlOvrd              = ipuBit(7);

constexpr uint32_t kBtrsRegPwrState                = 0x005cU;
constexpr uint32_t kBtrsPwrStateHhStatusShift      = 12U;
constexpr uint32_t kBtrsPwrStateHhStatusMask       = ipuMask(13, 12);
constexpr uint32_t kBtrsPwrStateHhStatusDone       = 2U;
constexpr uint32_t kBtrsPwrStateHhStatusErr        = 3U;

constexpr uint32_t kBtrsRegLtrCtrl                 = 0x0068U;
constexpr uint32_t kBtrsLtrCtrlPreserveMask        = 0xe800e000U;
constexpr uint32_t kBtrsLtrCtrlActiveVal           = 0x08140814U;
constexpr uint32_t kBtrsLtrCtrlIdleVal             = 0x08640864U;
constexpr uint32_t kBtrsRegLtrState                = 0x006cU;

constexpr uint32_t kBtrsPwrStateIsFsmShift         = 20U;
constexpr uint32_t kBtrsPwrStateIsFsmMask          = ipuMask(24, 20);
constexpr uint32_t kBtrsPwrStateIsFsmIdle          = 0x0U;
constexpr uint32_t kBtrsPwrStateIsFsmRdy           = 0xcU;

constexpr uint32_t kBtrsPwrStatePsFsmShift         = 25U;
constexpr uint32_t kBtrsPwrStatePsFsmMask          = ipuMask(29, 25);
constexpr uint32_t kBtrsPwrStatePsFsmIdle          = 0x0U;
constexpr uint32_t kBtrsPwrStatePsFsmPwrUp         = 0x10U;

constexpr uint32_t kBtrsRegFwSourceBaseLo          = 0x0078U;
constexpr uint32_t kBtrsRegFwSourceBaseHi          = 0x007cU;
constexpr uint32_t kBtrsRegFwSourceSize            = 0x0080U;
constexpr uint32_t kBtrsRegFabricCmd               = 0x0088U;
constexpr uint32_t kBtrsFabricCmdStartTscSync      = ipuBit(0);

constexpr uint32_t kBtrsRegIsrStatus               = 0x0090U;
constexpr uint32_t kBtrsRegIsrEnabledStatus        = 0x0094U;
constexpr uint32_t kBtrsRegIsrEnable               = 0x0098U;
constexpr uint32_t kBtrsRegIsrClear                = 0x009cU;

constexpr uint32_t kBtrsIsrIsIrq                   = ipuBit(0);
constexpr uint32_t kBtrsIsrPsIrq                   = ipuBit(1);
constexpr uint32_t kBtrsIsrIpcExecDoneByCse        = ipuBit(2);
constexpr uint32_t kBtrsIsrIpcFromCseWaiting       = ipuBit(4);
constexpr uint32_t kBtrsIsrSaiViolation            = ipuBit(11);
constexpr uint32_t kBtrsIsrIsFatalMemErr           = ipuBit(14);
constexpr uint32_t kBtrsIsrPsFatalMemErr           = ipuBit(17);
constexpr uint32_t kBtrsIsrUfiError                = ipuBit(20);
constexpr uint32_t kBtrsIrqsMask =
    kBtrsIsrIsIrq | kBtrsIsrPsIrq | kBtrsIsrIpcExecDoneByCse | kBtrsIsrIpcFromCseWaiting;

constexpr uint32_t kBtrsRegIu2CseDb0               = 0x0100U;
constexpr uint32_t kBtrsIu2CseDb0Busy              = ipuBit(31);
constexpr uint32_t kBtrsRegIu2CseData0             = 0x0104U;
constexpr uint32_t kBtrsIu2CseBootLoad             = 1U;
constexpr uint32_t kBtrsIu2CseAuthRun              = 2U;
constexpr uint32_t kBtrsCse2IuBootLoadDone         = ipuBit(0);
constexpr uint32_t kBtrsCse2IuAuthRunDone          = ipuBit(1);

constexpr uint32_t kBtrsRegIu2CseCsr               = 0x0108U;
constexpr uint32_t kBtrsCsrRstPhase1               = ipuBit(0);
constexpr uint32_t kBtrsCsrRstPhase2               = ipuBit(1);
constexpr uint32_t kBtrsCsrQueriedRstPhase         = ipuBit(2);
constexpr uint32_t kBtrsCsrAssertedRegValidReq     = ipuBit(3);
constexpr uint32_t kBtrsCsrAckedRegValid           = ipuBit(4);
constexpr uint32_t kBtrsCsrDeassertedRegValidReq   = ipuBit(5);

constexpr uint32_t kBtrsRegTswCtl                  = 0x0120U;
constexpr uint32_t kBtrsTswCtlSoftReset            = ipuBit(8);
constexpr uint32_t kBtrsRegTscLo                   = 0x0164U;
constexpr uint32_t kBtrsRegTscHi                   = 0x0168U;

constexpr uint32_t kBtrsRegSecurityCtl             = 0x0300U;
constexpr uint32_t kBtrsSecCtlFwSecureMode         = ipuBit(16);
constexpr uint32_t kBtrsSecCtlFwSetupMask          = 0x1fU;
constexpr uint32_t kBtrsSecCtlFwSetupDone          = ipuBit(0);
constexpr uint32_t kBtrsSecCtlAuthDone             = ipuBit(1);
constexpr uint32_t kBtrsSecCtlAuthFailed           = ipuBit(3);

constexpr uint32_t kBtrsRegCse2IuDb0               = 0x0304U;
constexpr uint32_t kBtrsCse2IuDb0Busy              = ipuBit(31);
constexpr uint32_t kBtrsRegCse2IuData0             = 0x0308U;
constexpr uint32_t kBtrsRegCse2IuCsr               = 0x030cU;
constexpr uint32_t kBtrsCse2IuNack                 = 0x0f20U;
constexpr uint32_t kBtrsCse2IuNackMask             = 0xffffU;

constexpr uint32_t kBtrsRegSku                     = 0x0314U;
constexpr uint32_t kBtrsRegSecurityTouch           = 0x0318U;
constexpr uint32_t kBtrsRegFabricQosCfg0           = 0x05a0U;
constexpr uint32_t kBtrsRegFabricQosCfg1           = 0x05a4U;
constexpr uint32_t kBtrsRegFabricQosLutBase        = 0x05a8U;
constexpr uint32_t kBtrsRegPsFreqCapabilities      = 0xf7498U;
constexpr uint32_t kBtrsRegPunitMailboxInterface   = 0xf74a0U;
constexpr uint32_t kBtrsRegPunitMailboxData        = 0xf74a4U;
constexpr uint32_t kBtrsPunitMailboxBusy           = ipuBit(31);
constexpr uint32_t kBtrsPunitResetPsysCmd          = 0x80000002U;
constexpr uint32_t kBootloaderStatusOffset         = 0x8000U; /* relative to PSYS base */
constexpr uint32_t kBootloaderMagicKey             = 0xb00710adU;

/* SPC & Info registers */
constexpr uint32_t kSpcRegStatusCtrl               = 0x0000U;
constexpr uint32_t kSpcRegStartPc                  = 0x0004U;
constexpr uint32_t kSpcRegIcacheBase               = 0x0010U;
constexpr uint32_t kSpcRegInfoSeg0IcacheMaster     = 0x0014U;

constexpr uint32_t kSpcStatusStart                 = ipuBit(1);
constexpr uint32_t kSpcStatusRun                   = ipuBit(3);
constexpr uint32_t kSpcStatusReady                 = ipuBit(5);
constexpr uint32_t kSpcStatusIcacheInvalidate      = ipuBit(12);
constexpr uint32_t kSpcStatusIcachePrefetch        = ipuBit(13);
constexpr uint32_t kPkgDirImrOffset                = 0x40U;
constexpr uint32_t kInfoRequestDestIosf            = ipuBit(9);

/* IPU4P CSI PHY building-block registers relative to PCI BAR0 */
constexpr uint32_t kBtrsRegCsiPhyCtrl0             = 0x100c0U;
constexpr uint32_t kBtrsRegCsiPhyPowerCtrl         = 0x100c8U;
constexpr uint32_t kBtrsRegCsiBscanExclude         = 0x100d8U;
constexpr uint32_t kCsiBscanExcludeVal             = ipuBit(9) | ipuBit(18) | ipuBit(27);
constexpr uint32_t kBtrsRegCphy0DllOvrd            = 0x10100U;
constexpr uint32_t kBtrsRegDphy0DllOvrd            = 0x1014cU;
constexpr uint32_t kBtrsRegBb0AfeConfig            = 0x10174U;
constexpr uint32_t kCsi2GpregCrPortConfigVal       = 0x3895U;

/* IPU4P ISYS IBufCtrl flush-for-idrain registers relative to ISYS base */
constexpr uint32_t kIsysCl0IbufCtlFlushIdrain      = 0x6002cU;
constexpr uint32_t kIsysCl1IbufCtlFlushIdrain      = 0x6802cU;
constexpr uint32_t kIsysIbufCtlFlushIdrain         = 0xb602cU;

/* IPU4P ISYS IRQ & CSI registers relative to ISYS base */
constexpr uint32_t kIsysUnispartIrqEdge            = 0x7c000U;
constexpr uint32_t kIsysUnispartIrqStatus          = 0x7c008U;
constexpr uint32_t kIsysUnispartIrqClear           = 0x7c00cU;
constexpr uint32_t kIsysUnispartSwIrqReg           = 0x7c414U;
constexpr uint32_t kIsysUnispartSwIrqMuxReg        = 0x7c418U;
constexpr uint32_t kIsysUnispartIrqSw              = ipuBit(22);

constexpr uint32_t kIsysIsaAccIrqCtrlBase          = 0xb0c00U;
constexpr uint32_t kIsysAIrqCtrlBase               = 0xbe200U;
constexpr uint32_t kIsysSip0IrqCtrlBase            = 0x66d00U;
constexpr uint32_t kIsysSip0IrqCtrlStatus          = 0x66d08U;
constexpr uint32_t kIsysSip0IrqCtrlClear           = 0x66d0cU;
constexpr uint32_t kIsysSip1IrqCtrlBase            = 0x6ed00U;
constexpr uint32_t kIsysSip1IrqCtrlStatus          = 0x6ed08U;
constexpr uint32_t kIsysSip1IrqCtrlClear           = 0x6ed0cU;
constexpr uint32_t kIsysIrqMaskOffset              = 0x04U;
constexpr uint32_t kIsysIrqStatusOffset            = 0x08U;
constexpr uint32_t kIsysIrqClearOffset             = 0x0cU;
constexpr uint32_t kIsysIrqEnableOffset            = 0x10U;
constexpr uint32_t kIsysIrqLnpOffset               = 0x14U;

constexpr uint32_t kIsysSip0Csi2Irq                = ipuMask(3, 0);
constexpr uint32_t kIsysSip1Csi2IrqMask            = ipuMask(6, 0);

constexpr uint32_t kIsysGpOffset                   = 0x66800U;
constexpr uint32_t kIsysComboGpOffset              = 0x6e800U;
constexpr uint32_t kCsi2GpregCrPortConfig          = 0x0014U;

constexpr uint32_t kCsi2RxEnable                   = 0x0000U;
constexpr uint32_t kCsi2RxEnableEnable             = ipuBit(0);
constexpr uint32_t kCsi2RxNofEnabledLanes          = 0x0004U;
constexpr uint32_t kCsi2RxConfig                   = 0x0008U;
constexpr uint32_t kCsi2RxConfigReleaseLp11        = ipuBit(0);
constexpr uint32_t kCsi2RxConfigDisableByteClkGate = ipuBit(1);
static inline bool isComboDphyPort(uint32_t hwPortIndex) {
    /* fwPort 6 (hwPortIndex=1 @ 0x6c000) and fwPort 7 (hwPortIndex=2 @ 0x6c100)
     * are Combo C-PHY/D-PHY receivers whose D-PHY timing offsets shift by +4.
     */
    return hwPortIndex == 1U || hwPortIndex == 2U;
}
static inline uint32_t csi2RxDlyCntTermenClane(uint32_t hwPortIndex) {
    return isComboDphyPort(hwPortIndex) ? 0x0030U : 0x002cU;
}
static inline uint32_t csi2RxDlyCntSettleClane(uint32_t hwPortIndex) {
    return isComboDphyPort(hwPortIndex) ? 0x0034U : 0x0030U;
}
static inline uint32_t csi2RxDlyCntTermenDlane(uint32_t hwPortIndex, uint32_t lane) {
    return (isComboDphyPort(hwPortIndex) ? 0x0038U : 0x0034U) + lane * 8U;
}
static inline uint32_t csi2RxDlyCntSettleDlane(uint32_t hwPortIndex, uint32_t lane) {
    return (isComboDphyPort(hwPortIndex) ? 0x003cU : 0x0038U) + lane * 8U;
}

static inline uint32_t isysCsiIrqCtrlBase(uint32_t hwPort) {
    return hwPort ? (0x6cb00U + 0x800U * (hwPort - 1U)) : 0x66300U;
}
static inline uint32_t isysCsiIrqCtrl0Base(uint32_t hwPort) {
    return hwPort ? (0x6cc00U + 0x800U * (hwPort - 1U)) : 0x66400U;
}

constexpr uint32_t kIsysCsiIrqActive               = ipuBit(0);
constexpr uint32_t kIsysCsiErrorMask               = 0x0000FFFFU;
constexpr uint32_t kIsysCsi2Nports                 = 8U;

constexpr uint32_t kIsysPortOffsets[kIsysCsi2Nports] = {
    0x64300U, 0x6c000U, 0x6c100U, 0x6c200U,
    0x6c300U, 0x6c400U, 0x6c500U, 0x6c600U,
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_REGS_HPP */
