#include "Ipu4pIsysHal.hpp"
#include "Ipu4pRegs.hpp"
#include "IpuEmbeddedFw.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#include <string.h>

namespace ipu4p {

namespace {

uint32_t alignUp64(uint32_t v) {
    return (v + 63U) & ~63U;
}

uint64_t currentTimeMs() {
    uint64_t nowNs = 0ULL;
    clock_get_uptime(&nowNs);
    uint64_t absNs = 0ULL;
    absolutetime_to_nanoseconds(nowNs, &absNs);
    return absNs / 1000000ULL;
}

} // namespace

Ipu4pIsysHal::Ipu4pIsysHal()
    : pciDevice_(nullptr),
      bar0_(nullptr),
      isysBase_(nullptr),
      psysBase_(nullptr),
      bar0Phys_(0ULL),
      bar0Len_(0ULL),
      msiIrqIndex_(0U),
      vendorId_(0U),
      deviceId_(0U),
      revisionId_(0U),
      subVendorId_(0U),
      subDeviceId_(0U),
      hasIommuParent_(false),
      activeDmaMode_(kUniCamIpuDmaPhysical),
      preferredAutoDmaMode_(kUniCamIpuDmaAuto),
      initialized_(false),
      fwLoaded_(false),
      fwAuthenticated_(false),
      isysPowered_(false),
      streaming_(false),
      streamOpened_(false),
      streamStarted_(false),
      psysFwIova_(0U),
      isysFwIova_(0U),
      psysPkgDirIova_(0U),
      isysPkgDirIova_(0U),
      ringBaseIova_(0U),
      ringHdr_(nullptr),
      fwMsgBaseIova_(0U),
      fwMsgSlots_(nullptr),
      hwPortIndex_(1U),
      fwStreamSource_(kFwSrcPort6),
      activeStride_(0U),
      activePayloadBytes_(0U),
      nextFrameCounter_(0U),
      nextSequence_(0U),
      cmdOpenDone_(false),
      cmdStartDone_(false),
      cmdStopDone_(false),
      cmdFlushDone_(false),
      cmdCloseDone_(false),
      cmdLastErr_(0U),
      cmdLastErrDetails_(0U),
      readyHead_(0U),
      readyTail_(0U),
      readyCount_(0U),
      msiIrqCount_(0ULL),
      isysIrqCount_(0ULL),
      csiIrqCount_(0ULL),
      cseIrqCount_(0ULL),
      polledTokenCount_(0ULL),
      framesCompleted_(0ULL),
      framesSkipped_(0ULL),
      framesDropped_(0ULL),
      lastCsiErrMask_(0U),
      lastFwErr_(0U),
      lastFwErrDetails_(0U) {
    memset(&cpdInfo_, 0, sizeof(cpdInfo_));
    memset(slotIovas_, 0, sizeof(slotIovas_));
    memset(fwMsgTrackers_, 0, sizeof(fwMsgTrackers_));
    memset(&activeCfg_, 0, sizeof(activeCfg_));
    memset(readyQueue_, 0, sizeof(readyQueue_));
}

Ipu4pIsysHal::~Ipu4pIsysHal() {
    cleanup();
}

bool Ipu4pIsysHal::init(IOPCIDevice *pciDevice,
                        volatile uint8_t *bar0Base,
                        uint64_t bar0PhysAddr,
                        uint64_t bar0Length,
                        uint32_t msiInterruptIndex) {
    cleanup();
    if (!pciDevice || !bar0Base || bar0Length < (kPsysOffset + kBootloaderStatusOffset + 0x1000U)) {
        return false;
    }

    pciDevice_ = pciDevice;
    bar0_ = bar0Base;
    isysBase_ = bar0Base + kIsysOffset;
    psysBase_ = bar0Base + kPsysOffset;
    bar0Phys_ = bar0PhysAddr;
    bar0Len_ = bar0Length;
    msiIrqIndex_ = msiInterruptIndex;

    vendorId_ = pciDevice_->configRead16(kIOPCIConfigVendorID);
    deviceId_ = pciDevice_->configRead16(kIOPCIConfigDeviceID);
    revisionId_ = pciDevice_->configRead8(kIOPCIConfigRevisionID);
    subVendorId_ = pciDevice_->configRead16(kIOPCIConfigSubSystemVendorID);
    subDeviceId_ = pciDevice_->configRead16(kIOPCIConfigSubSystemID);

    activeDmaMode_ = IpuDmaBuffer::resolveDmaMode(pciDevice_, kUniCamIpuDmaAuto, &hasIommuParent_);
    buttress_.init(bar0_);

    initialized_ = true;
    IOLog("UniCamIPU: IPU4P HAL initialized (BAR0=0x%llx len=%lluKB secure=%u auth=%u dmaMode=%u iommu=%u)\n",
          bar0Phys_, bar0Len_ / 1024ULL,
          buttress_.isSecureMode() ? 1U : 0U,
          buttress_.isAuthDone() ? 1U : 0U,
          activeDmaMode_, hasIommuParent_ ? 1U : 0U);
    return true;
}

void Ipu4pIsysHal::onPciPowerReady() {
    if (!bar0_) {
        return;
    }
    if (initialized_) {
        buttress_.restoreAfterWake();
    } else {
        buttress_.init(bar0_);
    }
    if (!buttress_.isAuthDone()) {
        fwAuthenticated_ = false;
    }
}

void Ipu4pIsysHal::cleanup() {
    if (streaming_ || streamOpened_ || isysPowered_) {
        stopStream();
    }
    detachRingBuffer();
    freeFirmwareBuffers();
    isysBase_ = nullptr;
    psysBase_ = nullptr;
    bar0_ = nullptr;
    pciDevice_ = nullptr;
    initialized_ = false;
}

void Ipu4pIsysHal::freeFirmwareBuffers() {
    syscom_.release();
    if (fwMsgDmaBuf_.isValid()) {
        if (fwMsgBaseIova_ != 0U) {
            isysMmu_.unmapRange(fwMsgBaseIova_, fwMsgDmaBuf_.byteSize());
            fwMsgBaseIova_ = 0U;
        }
        fwMsgDmaBuf_.release();
        fwMsgSlots_ = nullptr;
    }

    if (psysPkgDirBuf_.isValid()) {
        if (psysPkgDirIova_ != 0U) {
            psysMmu_.unmapRange(psysPkgDirIova_, psysPkgDirBuf_.byteSize());
            psysPkgDirIova_ = 0U;
        }
        psysPkgDirBuf_.release();
    }
    if (isysPkgDirBuf_.isValid()) {
        if (isysPkgDirIova_ != 0U) {
            isysMmu_.unmapRange(isysPkgDirIova_, isysPkgDirBuf_.byteSize());
            isysPkgDirIova_ = 0U;
        }
        isysPkgDirBuf_.release();
    }
    if (fwCpdBuf_.isValid()) {
        if (psysFwIova_ != 0U) {
            psysMmu_.unmapRange(psysFwIova_, fwCpdBuf_.byteSize());
            psysFwIova_ = 0U;
        }
        if (isysFwIova_ != 0U) {
            isysMmu_.unmapRange(isysFwIova_, fwCpdBuf_.byteSize());
            isysFwIova_ = 0U;
        }
        fwCpdBuf_.release();
    }

    psysMmu_.destroy();
    isysMmu_.destroy();
    ringBaseIova_ = 0U;
    memset(slotIovas_, 0, sizeof(slotIovas_));
    memset(&cpdInfo_, 0, sizeof(cpdInfo_));
    fwLoaded_ = false;
}

void Ipu4pIsysHal::fillHwInfo(UniCamIpuHwInfo *outInfo) {
    if (!outInfo) return;
    const uint32_t liveMode = IpuDmaBuffer::resolveDmaMode(
        pciDevice_, preferredAutoDmaMode_, &hasIommuParent_);
    if (!fwLoaded_) {
        activeDmaMode_ = liveMode;
    }
    memset(outInfo, 0, sizeof(*outInfo));
    outInfo->schemaVersion = UNICAM_IPU_SCHEMA_VERSION;
    outInfo->vendorId = vendorId_;
    outInfo->deviceId = deviceId_;
    outInfo->revisionId = revisionId_;
    outInfo->subsystemVendorId = subVendorId_;
    outInfo->subsystemId = subDeviceId_;
    outInfo->bar0PhysAddr = bar0Phys_;
    outInfo->bar0Length = bar0Len_;
    outInfo->msiInterruptIndex = msiIrqIndex_;
    outInfo->hasIommuParent = hasIommuParent_ ? 1U : 0U;
    outInfo->activeDmaMode = static_cast<uint8_t>(activeDmaMode_);
    outInfo->secureMode = buttress_.isSecureMode() ? 1U : 0U;
    outInfo->fwAuthenticated = (fwLoaded_ && (fwAuthenticated_ || buttress_.isAuthDone())) ? 1U : 0U;
    outInfo->isysPowered = isysPowered_ ? 1U : 0U;
    outInfo->streaming = streaming_ ? 1U : 0U;
    outInfo->skuReg = buttress_.readSku();
    outInfo->securityCtlReg = buttress_.readSecurityCtl();
    outInfo->pwrStateReg = buttress_.readPwrState();
    outInfo->refClkTenthsMhz = buttress_.refClkTenthsMhz();
    outInfo->fwPkgDate = cpdInfo_.fwPkgDate;
    strlcpy(outInfo->modelName, "Intel Ice Lake IPU4P", sizeof(outInfo->modelName));
}

void Ipu4pIsysHal::fillDiag(UniCamIpuDiag *outDiag) {
    if (!outDiag) return;
    memset(outDiag, 0, sizeof(*outDiag));
    if (bar0_) {
        outDiag->securityCtl = buttress_.readSecurityCtl();
        outDiag->pwrState = buttress_.readPwrState();
        outDiag->isrStatus = mmioRead32(bar0_, kBtrsRegIsrStatus);
        outDiag->isrEnable = mmioRead32(bar0_, kBtrsRegIsrEnable);
        outDiag->isFreqCtl = mmioRead32(bar0_, kBtrsRegIsFreqCtl);
        outDiag->psFreqCtl = mmioRead32(bar0_, kBtrsRegPsFreqCtl);
        outDiag->psFreqCaps = mmioRead32(bar0_, kBtrsRegPsFreqCapabilities);
        outDiag->punitMailbox = mmioRead32(bar0_, kBtrsRegPunitMailboxInterface);
        outDiag->cse2IuCsr = mmioRead32(bar0_, kBtrsRegCse2IuCsr);
        outDiag->fwResetCtl = mmioRead32(bar0_, kBtrsRegFwResetCtl);
        outDiag->pmRespReg = mmioRead32(bar0_, kBtrsRegPunitMailboxData);
        outDiag->csiPhyPwrCtrl = mmioRead32(bar0_, kBtrsRegCsiPhyPowerCtrl);
    }
    if (isysBase_ && isysPowered_) {
        outDiag->unispartIrqStatus = mmioRead32(isysBase_, kIsysUnispartIrqStatus);
        outDiag->sip0IrqStatus = mmioRead32(isysBase_, kIsysSip0IrqCtrlStatus);
        outDiag->sip1IrqStatus = mmioRead32(isysBase_, kIsysSip1IrqCtrlStatus);
        outDiag->spcStatusCtrl = mmioRead32(isysBase_ + kIsysSpcOffset, kSpcRegStatusCtrl);
        outDiag->syscomState = syscom_.readSyscomState();
    }
    outDiag->lastCsiErrorMask = lastCsiErrMask_;
    outDiag->lastFwError = lastFwErr_;
    outDiag->lastFwErrorDetails = lastFwErrDetails_;
    outDiag->msiIrqCount = msiIrqCount_;
    outDiag->isysIrqCount = isysIrqCount_;
    outDiag->csiIrqCount = csiIrqCount_;
    outDiag->cseIrqCount = cseIrqCount_;
    outDiag->polledTokenCount = polledTokenCount_;
    outDiag->framesCompleted = framesCompleted_;
    outDiag->framesSkipped = framesSkipped_;
    outDiag->framesDropped = framesDropped_;
    outDiag->psysMmuMappedPages = psysMmu_.mappedPageCount();
    outDiag->isysMmuMappedPages = isysMmu_.mappedPageCount();
}

IOReturn Ipu4pIsysHal::sendProxyToken(uint32_t reqId, uint32_t regionIndex, uint32_t value) {
    auto *sendTok = static_cast<FwIsysProxySendToken *>(
        syscom_.sendGetToken(kBaseProxySendQueues));
    if (!sendTok) {
        return kIOReturnBusy;
    }

    sendTok->request_id = reqId;
    sendTok->region_index = regionIndex;
    sendTok->offset = 0U;
    sendTok->value = value;
    syscom_.sendPutToken(kBaseProxySendQueues);

    for (uint32_t i = 0; i < 2000U; ++i) {
        IODelay(150);
        auto *recvTok = static_cast<FwIsysProxyRecvToken *>(
            syscom_.recvGetToken(kBaseProxyRecvQueues));
        if (!recvTok) {
            continue;
        }
        const uint32_t respReq = recvTok->proxy_resp_info.request_id;
        const uint32_t respErr = recvTok->proxy_resp_info.error_info.error;
        syscom_.recvPutToken(kBaseProxyRecvQueues);
        if (respReq != reqId || respErr != 0U) {
            return kIOReturnError;
        }
        return kIOReturnSuccess;
    }

    return kIOReturnTimeout;
}

IOReturn Ipu4pIsysHal::disableIwake() {
    /* On IPU4P, IWAKE is disabled via proxy region_index=1 (GDA enable) = 0. */
    return sendProxyToken(1U, 1U, 0U);
}

IOReturn Ipu4pIsysHal::powerUpIsysAndOpenSyscom(uint32_t *outStepFailed, UniCamIpuBootReport *outReport) {
    if (outStepFailed) *outStepFailed = kUniCamIpuBootStepOk;

    IOReturn ret = kIOReturnSuccess;
    if (!isysPowered_) {
        ret = buttress_.setPower(true, true, outReport);
        if (ret != kIOReturnSuccess) {
            if (outStepFailed) *outStepFailed = kUniCamIpuBootStepIsysPowerOn;
            return ret;
        }
        isysPowered_ = true;
    }

    ret = buttress_.startTscSync();
    if (ret != kIOReturnSuccess) {
        IOLog("UniCamIPU: TSC sync warning (0x%x), continuing\n", ret);
    }

    if (!isysMmu_.programHardware()) {
        if (outStepFailed) *outStepFailed = kUniCamIpuBootStepIsysMmuInit;
        buttress_.setPower(true, false);
        isysPowered_ = false;
        return kIOReturnError;
    }

    Ipu4pCsiPhy::irqSetup(isysBase_);
    Ipu4pCsiPhy::phySetup(bar0_, isysBase_);

    Ipu4pCpd::configureSpc(isysBase_,
                           kIsysSpcOffset,
                           kIsysDmemOffset,
                           buttress_.isSecureMode(),
                           kCpdPkgDirIsysServerIdx,
                           static_cast<const uint8_t *>(fwCpdBuf_.cpuAddr()),
                           static_cast<uint32_t>(fwCpdBuf_.byteSize()),
                           isysFwIova_,
                           isysPkgDirIova_,
                           &isysPkgDirBuf_);

    ret = syscom_.prepare(pciDevice_, activeDmaMode_, &isysMmu_, isysBase_, kIsysMaxStreams);
    if (ret != kIOReturnSuccess) {
        if (outStepFailed) *outStepFailed = kUniCamIpuBootStepSyscomOpen;
        isysMmu_.hwCleanup();
        buttress_.setPower(true, false);
        isysPowered_ = false;
        return ret;
    }

    ret = syscom_.openAndWaitReady();
    if (ret != kIOReturnSuccess) {
        if (outStepFailed) *outStepFailed = kUniCamIpuBootStepSyscomOpen;
        syscom_.release();
        isysMmu_.hwCleanup();
        buttress_.setPower(true, false);
        isysPowered_ = false;
        return ret;
    }

    return kIOReturnSuccess;
}

void Ipu4pIsysHal::powerDownIsysAndCloseSyscom() {
    if (!isysPowered_) {
        syscom_.release();
        return;
    }
    syscom_.closeAndWaitRelease(true);
    isysMmu_.hwCleanup();
    buttress_.setPower(true, false);
    isysPowered_ = false;
}

IOReturn Ipu4pIsysHal::bootEmbeddedFirmware(uint32_t requestedDmaMode,
                                            UniCamIpuBootReport *outReport) {
    IOBufferMemoryDescriptor *fwDesc = nullptr;
    uint32_t fwSize = 0U;
    if (!decompressEmbeddedIpu4pFw(&fwDesc, &fwSize) || !fwDesc) {
        if (outReport) {
            memset(outReport, 0, sizeof(*outReport));
            outReport->stepFailed = kUniCamIpuBootStepInvalidCpd;
            outReport->statusRet = kIOReturnNoMemory;
        }
        return kIOReturnNoMemory;
    }

    const uint8_t *fwPtr = static_cast<const uint8_t *>(fwDesc->getBytesNoCopy());
    const IOReturn ret = bootFirmware(fwPtr, fwSize, requestedDmaMode, outReport);
    fwDesc->release();
    return ret;
}

IOReturn Ipu4pIsysHal::bootFirmware(const uint8_t *cpdData,
                                    uint32_t cpdSize,
                                    uint32_t requestedDmaMode,
                                    UniCamIpuBootReport *outReport) {
    if (requestedDmaMode != kUniCamIpuDmaAuto) {
        return bootFirmwareWithMode(cpdData, cpdSize, requestedDmaMode, outReport);
    }

    if (preferredAutoDmaMode_ != kUniCamIpuDmaAuto) {
        return bootFirmwareWithMode(cpdData, cpdSize, preferredAutoDmaMode_, outReport);
    }

    const uint32_t primary = IpuDmaBuffer::resolveDmaMode(pciDevice_, kUniCamIpuDmaAuto, &hasIommuParent_);
    IOReturn ret = bootFirmwareWithMode(cpdData, cpdSize, primary, outReport);
    if (ret == kIOReturnSuccess) {
        preferredAutoDmaMode_ = activeDmaMode_;
        return ret;
    }

    if (primary != kUniCamIpuDmaPhysical) {
        const uint32_t failedStep = outReport ? outReport->stepFailed : 0U;
        IOLog("UniCamIPU: DMA mode %u failed at step %u (0x%x), retrying with physical-39bit...\n",
              primary, failedStep, ret);
        ret = bootFirmwareWithMode(cpdData, cpdSize, kUniCamIpuDmaPhysical, outReport);
        if (ret == kIOReturnSuccess) {
            preferredAutoDmaMode_ = kUniCamIpuDmaPhysical;
        }
    }
    return ret;
}

IOReturn Ipu4pIsysHal::bootFirmwareWithMode(const uint8_t *cpdData,
                                            uint32_t cpdSize,
                                            uint32_t concreteDmaMode,
                                            UniCamIpuBootReport *outReport) {
    if (!initialized_ || !cpdData || cpdSize == 0U || !outReport) {
        return kIOReturnBadArgument;
    }
    if (streaming_) {
        return kIOReturnBusy;
    }

    const uint64_t tStartMs = currentTimeMs();
    memset(outReport, 0, sizeof(*outReport));
    outReport->fwSizeBytes = cpdSize;
    outReport->securityCtlBefore = buttress_.readSecurityCtl();
    outReport->securityCtlAfter = outReport->securityCtlBefore;
    outReport->secureMode = buttress_.isSecureMode() ? 1U : 0U;

    auto finishReport = [&](uint32_t step, IOReturn st) -> IOReturn {
        outReport->stepFailed = step;
        outReport->statusRet = st;
        outReport->securityCtlAfter = buttress_.readSecurityCtl();
        if (step != kUniCamIpuBootStepOk && bar0_ && outReport->pwrStateAtFail == 0U) {
            outReport->pwrStateAtFail = buttress_.readPwrState();
            outReport->psFreqAtFail = mmioRead32(bar0_, kBtrsRegPsFreqCtl);
            outReport->isFreqAtFail = mmioRead32(bar0_, kBtrsRegIsFreqCtl);
            outReport->pmRespAtFail = mmioRead32(bar0_, kBtrsRegPunitMailboxData);
        }
        outReport->elapsedMs = static_cast<uint32_t>(currentTimeMs() - tStartMs);
        return st;
    };

    if (outReport->securityCtlBefore == 0xFFFFFFFFU) {
        IOLog("UniCamIPU: BAR0 MMIO is wedged (SEC_CTL=0xffffffff) before bootFirmware\n");
        return finishReport(kUniCamIpuBootStepBadArg, kIOReturnNotReady);
    }

    CpdParseInfo parsed {};
    if (!Ipu4pCpd::validateCpdFile(cpdData, cpdSize, &parsed)) {
        return finishReport(kUniCamIpuBootStepInvalidCpd, kIOReturnInvalid);
    }

    freeFirmwareBuffers();
    cpdInfo_ = parsed;
    outReport->fwPkgDate = cpdInfo_.fwPkgDate;
    outReport->cpdComponents = cpdInfo_.componentCount;

    activeDmaMode_ = IpuDmaBuffer::resolveDmaMode(pciDevice_, concreteDmaMode, &hasIommuParent_);
    outReport->activeDmaMode = static_cast<uint8_t>(activeDmaMode_);

    if (!psysMmu_.init(pciDevice_, activeDmaMode_, psysBase_, MmuSubsys::Psys) ||
        !isysMmu_.init(pciDevice_, activeDmaMode_, isysBase_, MmuSubsys::Isys)) {
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepPsysMmuInit, kIOReturnNoMemory);
    }

    if (!fwCpdBuf_.allocate(pciDevice_, activeDmaMode_, cpdSize, false)) {
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepMmuMapFw, kIOReturnNoMemory);
    }
    memcpy(fwCpdBuf_.cpuAddr(), cpdData, cpdSize);
    fwCpdBuf_.flushCache();

    psysFwIova_ = psysMmu_.mapBuffer(fwCpdBuf_);
    if (psysFwIova_ == 0U) {
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepMmuMapFw, kIOReturnNoMemory);
    }

    if (!Ipu4pCpd::createPkgDir(pciDevice_, activeDmaMode_, &psysMmu_,
                                static_cast<const uint8_t *>(fwCpdBuf_.cpuAddr()),
                                psysFwIova_, cpdInfo_, &psysPkgDirBuf_, &psysPkgDirIova_)) {
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepCreatePkgDir, kIOReturnNoMemory);
    }
    outReport->psysPkgDirIova = psysPkgDirIova_;

    if (!buttress_.isSecureMode()) {
        isysFwIova_ = isysMmu_.mapBuffer(fwCpdBuf_);
        if (isysFwIova_ == 0U ||
            !Ipu4pCpd::createPkgDir(pciDevice_, activeDmaMode_, &isysMmu_,
                                    static_cast<const uint8_t *>(fwCpdBuf_.cpuAddr()),
                                    isysFwIova_, cpdInfo_, &isysPkgDirBuf_, &isysPkgDirIova_)) {
            freeFirmwareBuffers();
            return finishReport(kUniCamIpuBootStepCreatePkgDir, kIOReturnNoMemory);
        }
    }

    IOLog("UniCamIPU: Booting IPU4P FW (dmaMode=%u pkgDirIova=0x%08x sec=0x%08x pwr=0x%08x csr=0x%08x)\n",
          activeDmaMode_, psysPkgDirIova_,
          buttress_.readSecurityCtl(),
          buttress_.readPwrState(),
          mmioRead32(bar0_, kBtrsRegCse2IuCsr));

    /* Follow iacamera64.sys Resume order: fabric QoS -> ipcReset -> ISYS ON first -> PSYS ON second -> auth -> PSYS OFF -> VC. */
    buttress_.prepareForPower();
    const uint32_t preAuthSt = outReport->securityCtlBefore & kBtrsSecCtlFwSetupMask;
    if (buttress_.isSecureMode() && preAuthSt != 0U && preAuthSt != kBtrsSecCtlAuthDone) {
        (void)buttress_.resetAuthentication();
    }
    if (buttress_.isSecureMode() && !buttress_.isAuthDone()) {
        const IOReturn ipcPreRet = buttress_.ipcReset();
        if (ipcPreRet != kIOReturnSuccess) {
            IOLog("UniCamIPU: CSE IPC reset failed (0x%x) sec=0x%08x pwr=0x%08x csr=0x%08x\n",
                  ipcPreRet,
                  buttress_.readSecurityCtl(),
                  buttress_.readPwrState(),
                  mmioRead32(bar0_, kBtrsRegCse2IuCsr));
            freeFirmwareBuffers();
            return finishReport(kUniCamIpuBootStepIpcReset, ipcPreRet);
        }
        outReport->ipcResetOk = 1U;
    }

    /* Power on ISYS first while bringing up PSYS for CSE FW authentication. */
    IOReturn ret = buttress_.setPower(true, true, outReport);
    if (ret != kIOReturnSuccess) {
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepIsysPowerOn, ret);
    }
    isysPowered_ = true;

    ret = buttress_.setPower(false, true, outReport);
    if (ret != kIOReturnSuccess) {
        buttress_.setPower(true, false);
        isysPowered_ = false;
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepPsysPowerOn, ret);
    }

    if (!psysMmu_.programHardware()) {
        buttress_.setPower(false, false);
        buttress_.setPower(true, false);
        isysPowered_ = false;
        freeFirmwareBuffers();
        return finishReport(kUniCamIpuBootStepPsysMmuInit, kIOReturnError);
    }

    ret = buttress_.authenticate(psysPkgDirIova_, cpdSize, psysBase_, outReport);
    psysMmu_.hwCleanup();
    buttress_.setPower(false, false);

    if (ret != kIOReturnSuccess) {
        buttress_.setPower(true, false);
        isysPowered_ = false;
        freeFirmwareBuffers();
        return finishReport(outReport->stepFailed ? outReport->stepFailed : kUniCamIpuBootStepAuthRun, ret);
    }

    buttress_.configureVcMechanism();
    fwLoaded_ = true;
    fwAuthenticated_ = true;

    /* Complete ISYS SysCom bring-up & iwake disable, then shut ISYS down cleanly until stream start. */
    uint32_t sysStep = kUniCamIpuBootStepOk;
    ret = powerUpIsysAndOpenSyscom(&sysStep, outReport);
    if (ret != kIOReturnSuccess) {
        return finishReport(sysStep, ret);
    }

    outReport->syscomConfigIova = syscom_.configIova();
    outReport->syscomState = syscom_.readSyscomState();
    outReport->syscomHandshakeOk = (outReport->syscomState == kSyscomStateReady) ? 1U : 0U;

    const IOReturn iwakeRet = disableIwake();
    outReport->iwakeDisableOk = (iwakeRet == kIOReturnSuccess) ? 1U : 0U;
    if (iwakeRet != kIOReturnSuccess) {
        IOLog("UniCamIPU: disableIwake warning (0x%x)\n", iwakeRet);
    }

    powerDownIsysAndCloseSyscom();
    buttress_.enableIrqs(true);

    return finishReport(kUniCamIpuBootStepOk, kIOReturnSuccess);
}

IOReturn Ipu4pIsysHal::shutdownFirmware() {
    if (streaming_) {
        stopStream();
    }
    powerDownIsysAndCloseSyscom();
    buttress_.enableIrqs(false);
    freeFirmwareBuffers();
    return kIOReturnSuccess;
}

IOReturn Ipu4pIsysHal::attachRingBuffer(IOBufferMemoryDescriptor *ringDesc) {
    if (!initialized_ || !ringDesc) {
        return kIOReturnBadArgument;
    }
    if (ringDmaBuf_.isValid() && ringDmaBuf_.descriptor() == ringDesc) {
        return kIOReturnSuccess;
    }

    detachRingBuffer();
    if (!ringDmaBuf_.attachExisting(pciDevice_, activeDmaMode_, ringDesc)) {
        return kIOReturnNoMemory;
    }

    ringHdr_ = static_cast<UniCamIpuRingHeader *>(ringDmaBuf_.cpuAddr());
    if (ringHdr_) {
        memset(ringHdr_, 0, sizeof(*ringHdr_));
        ringHdr_->magic = UNICAM_IPU_RING_MAGIC;
        ringHdr_->version = UNICAM_IPU_SCHEMA_VERSION;
        ringHdr_->slotCount = UNICAM_IPU_RING_SLOTS;
        ringHdr_->slotStrideBytes = UNICAM_IPU_RING_SLOT_BYTES;
        for (uint32_t s = 0; s < UNICAM_IPU_RING_SLOTS; ++s) {
            ringHdr_->slotOffsets[s] = UNICAM_IPU_RING_HEADER_BYTES + s * UNICAM_IPU_RING_SLOT_BYTES;
            ringHdr_->slots[s].state = kUniCamIpuSlotFree;
        }
    }
    return kIOReturnSuccess;
}

void Ipu4pIsysHal::detachRingBuffer() {
    if (ringBaseIova_ != 0U && ringDmaBuf_.isValid() && isysMmu_.isInitialized()) {
        isysMmu_.unmapRange(ringBaseIova_, ringDmaBuf_.byteSize());
    }
    ringBaseIova_ = 0U;
    memset(slotIovas_, 0, sizeof(slotIovas_));
    ringHdr_ = nullptr;
    ringDmaBuf_.release();
}

int32_t Ipu4pIsysHal::allocFwMsg() {
    for (uint32_t i = 0; i < kNumFwMsgs; ++i) {
        if (!fwMsgTrackers_[i].inUse) {
            memset(&fwMsgSlots_[i], 0, sizeof(fwMsgSlots_[i]));
            memset(&fwMsgTrackers_[i], 0, sizeof(fwMsgTrackers_[i]));
            fwMsgTrackers_[i].inUse = 1U;
            fwMsgTrackers_[i].slotIndex = -1;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void Ipu4pIsysHal::freeFwMsg(uint32_t idx) {
    if (idx < kNumFwMsgs) {
        fwMsgTrackers_[idx].inUse = 0U;
        fwMsgTrackers_[idx].isFrame = 0U;
        fwMsgTrackers_[idx].slotIndex = -1;
    }
}

IOReturn Ipu4pIsysHal::waitCommandFlag(volatile bool *flagPtr, uint32_t timeoutMs) {
    for (uint32_t elapsed = 0; elapsed < timeoutMs; ++elapsed) {
        if (*flagPtr) {
            return (cmdLastErr_ == 0U) ? kIOReturnSuccess : kIOReturnError;
        }
        bool dummyReady = false;
        pollOrHandleIrq(false, &dummyReady);
        if (*flagPtr) {
            return (cmdLastErr_ == 0U) ? kIOReturnSuccess : kIOReturnError;
        }
        IOSleep(1);
    }
    return kIOReturnTimeout;
}

IOReturn Ipu4pIsysHal::queueSlotToFw(uint32_t slotIndex, bool startAndCapture) {
    if (slotIndex >= UNICAM_IPU_RING_SLOTS || !ringHdr_ || !fwMsgSlots_) {
        return kIOReturnBadArgument;
    }

    const int32_t msgIdx = allocFwMsg();
    if (msgIdx < 0) {
        return kIOReturnNoResources;
    }

    const uint8_t fc = nextFrameCounter_++;
    FwIsysFrameBuffSet &set = fwMsgSlots_[msgIdx].u.frame;
    set.output_pins[0].addr = slotIovas_[slotIndex];
    set.output_pins[0].out_buf_id = static_cast<uint64_t>(msgIdx) + 1ULL;
    set.output_pins[0].compress = 0U;
    set.frame_counter = fc;
    set.send_irq_sof = 1U;
    set.send_irq_eof = 1U;
    set.send_irq_capture_ack = 1U;
    set.send_irq_capture_done = 1U;
    set.send_resp_sof = 1U;
    set.send_resp_eof = 1U;

    fwMsgTrackers_[msgIdx].isFrame = 1U;
    fwMsgTrackers_[msgIdx].frameCounter = fc;
    fwMsgTrackers_[msgIdx].streamHandle = kStreamHandle;
    fwMsgTrackers_[msgIdx].slotIndex = static_cast<int32_t>(slotIndex);

    ringHdr_->slots[slotIndex].state = kUniCamIpuSlotQueuedToFw;

    const uint32_t msgIova = fwMsgBaseIova_ + static_cast<uint32_t>(msgIdx) * sizeof(FwMsgSlot);
    ipuClflushRange(&set, sizeof(set));

    auto *tok = static_cast<FwIsysSendToken *>(
        syscom_.sendGetToken(kBaseMsgSendQueues + kStreamHandle));
    if (!tok) {
        ringHdr_->slots[slotIndex].state = kUniCamIpuSlotFree;
        freeFwMsg(static_cast<uint32_t>(msgIdx));
        return kIOReturnBusy;
    }

    tok->payload = msgIova;
    tok->buf_handle = static_cast<uint64_t>(msgIdx) + 1ULL;
    tok->send_type = startAndCapture ? kFwSendStreamStartAndCapture : kFwSendStreamCapture;
    tok->stream_id = kStreamHandle;
    syscom_.sendPutToken(kBaseMsgSendQueues + kStreamHandle);
    return kIOReturnSuccess;
}

IOReturn Ipu4pIsysHal::startStream(const UniCamIpuStreamConfig &cfg,
                                   UniCamIpuStreamStatus *outStatus) {
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    if (streaming_) {
        stopStream();
    }
    if (!ringDmaBuf_.isValid() || !ringHdr_) {
        return kIOReturnNotReady;
    }

    uint32_t hwPort = 0U;
    if (!Ipu4pCsiPhy::mapFwPortToHwIndex(cfg.csiPort, &hwPort)) {
        IOLog("UniCamIPU: Unsupported CSI-2 FW port %u\n", cfg.csiPort);
        return kIOReturnBadArgument;
    }
    if (cfg.csiLanes == 0U || cfg.csiLanes > 4U || cfg.width == 0U || cfg.height == 0U) {
        return kIOReturnBadArgument;
    }

    const bool packedRaw10 = (cfg.pixelFormat == kUniCamIpuFormatRaw10Packed);
    const uint32_t rowBytes = packedRaw10 ? ((cfg.width * 10U + 7U) / 8U) : (cfg.width * 2U);
    const uint32_t stride = alignUp64(rowBytes);
    const uint32_t payloadBytes = stride * cfg.height;
    if (payloadBytes + stride > UNICAM_IPU_RING_SLOT_BYTES) {
        return kIOReturnNoSpace;
    }

    bool bootedFreshFw = false;
    if (!fwLoaded_ || !fwAuthenticated_ || !buttress_.isAuthDone() || !isysMmu_.isInitialized()) {
        if (buttress_.isSecureMode() && !buttress_.isAuthDone()) {
            fwAuthenticated_ = false;
        }
        UniCamIpuBootReport autoRep {};
        const IOReturn fwRet = bootEmbeddedFirmware(kUniCamIpuDmaAuto, &autoRep);
        if (fwRet != kIOReturnSuccess) {
            IOLog("UniCamIPU: On-demand FW boot failed in startStream (ret=0x%x step=%u sec=0x%08x pwr=0x%08x)\n",
                  fwRet, autoRep.stepFailed, autoRep.securityCtlAfter, autoRep.pwrStateAtFail);
            return fwRet;
        }
        bootedFreshFw = true;
    }

    IOReturn ret = startStreamOnce(cfg, outStatus);
    if (ret != kIOReturnSuccess && !bootedFreshFw) {
        IOLog("UniCamIPU: Cached FW stream start failed (0x%x), resetting CSE auth & retrying cold FW boot\n",
              ret);
        stopStream();
        (void)buttress_.resetAuthentication();
        fwAuthenticated_ = false;
        freeFirmwareBuffers();

        UniCamIpuBootReport recovRep {};
        const IOReturn fwRet = bootEmbeddedFirmware(kUniCamIpuDmaAuto, &recovRep);
        if (fwRet != kIOReturnSuccess) {
            IOLog("UniCamIPU: Recovery FW boot failed (ret=0x%x step=%u sec=0x%08x pwr=0x%08x)\n",
                  fwRet, recovRep.stepFailed, recovRep.securityCtlAfter, recovRep.pwrStateAtFail);
            return fwRet;
        }
        ret = startStreamOnce(cfg, outStatus);
    }
    return ret;
}

IOReturn Ipu4pIsysHal::startStreamOnce(const UniCamIpuStreamConfig &cfg,
                                       UniCamIpuStreamStatus *outStatus) {
    if (!initialized_ || (!fwAuthenticated_ && !buttress_.isAuthDone()) || !isysMmu_.isInitialized()) {
        return kIOReturnNotReady;
    }
    if (streaming_) {
        stopStream();
    }
    if (!ringDmaBuf_.isValid() || !ringHdr_) {
        return kIOReturnNotReady;
    }

    uint32_t hwPort = 0U;
    if (!Ipu4pCsiPhy::mapFwPortToHwIndex(cfg.csiPort, &hwPort)) {
        IOLog("UniCamIPU: Unsupported CSI-2 FW port %u\n", cfg.csiPort);
        return kIOReturnBadArgument;
    }
    if (cfg.csiLanes == 0U || cfg.csiLanes > 4U || cfg.width == 0U || cfg.height == 0U) {
        return kIOReturnBadArgument;
    }

    activeCfg_ = cfg;
    hwPortIndex_ = hwPort;
    fwStreamSource_ = cfg.csiPort;

    const bool packedRaw10 = (cfg.pixelFormat == kUniCamIpuFormatRaw10Packed);
    const uint32_t rowBytes = packedRaw10 ? ((cfg.width * 10U + 7U) / 8U) : (cfg.width * 2U);
    activeStride_ = alignUp64(rowBytes);
    activePayloadBytes_ = activeStride_ * cfg.height;
    if (activePayloadBytes_ + activeStride_ > UNICAM_IPU_RING_SLOT_BYTES) {
        return kIOReturnNoSpace;
    }

    /* Reset queues and counters cleanly before mapping & powering up ISYS. */
    readyHead_ = 0U;
    readyTail_ = 0U;
    readyCount_ = 0U;
    nextFrameCounter_ = 0U;
    nextSequence_ = 0U;
    cmdOpenDone_ = false;
    cmdStartDone_ = false;
    cmdStopDone_ = false;
    cmdFlushDone_ = false;
    cmdCloseDone_ = false;
    cmdLastErr_ = 0U;
    cmdLastErrDetails_ = 0U;
    memset(fwMsgTrackers_, 0, sizeof(fwMsgTrackers_));

    ringHdr_->activeWidth = cfg.width;
    ringHdr_->activeHeight = cfg.height;
    ringHdr_->activeStride = activeStride_;
    ringHdr_->activePayloadBytes = activePayloadBytes_;
    ringHdr_->pixelFormat = cfg.pixelFormat;
    ringHdr_->streaming = 1U;
    for (uint32_t s = 0; s < UNICAM_IPU_RING_SLOTS; ++s) {
        memset(&ringHdr_->slots[s], 0, sizeof(ringHdr_->slots[s]));
        ringHdr_->slots[s].state = kUniCamIpuSlotFree;
    }

    ringBaseIova_ = isysMmu_.mapBuffer(ringDmaBuf_);
    if (ringBaseIova_ == 0U) {
        return kIOReturnNoMemory;
    }
    for (uint32_t s = 0; s < UNICAM_IPU_RING_SLOTS; ++s) {
        slotIovas_[s] = ringBaseIova_ + UNICAM_IPU_RING_HEADER_BYTES + s * UNICAM_IPU_RING_SLOT_BYTES;
    }

    if (!fwMsgDmaBuf_.allocate(pciDevice_, activeDmaMode_, kNumFwMsgs * sizeof(FwMsgSlot), false)) {
        stopStream();
        return kIOReturnNoMemory;
    }
    fwMsgBaseIova_ = isysMmu_.mapBuffer(fwMsgDmaBuf_);
    if (fwMsgBaseIova_ == 0U) {
        stopStream();
        return kIOReturnNoMemory;
    }
    fwMsgSlots_ = static_cast<FwMsgSlot *>(fwMsgDmaBuf_.cpuAddr());

    uint32_t sysStep = kUniCamIpuBootStepOk;
    IOReturn ret = powerUpIsysAndOpenSyscom(&sysStep);
    if (ret != kIOReturnSuccess) {
        stopStream();
        return ret;
    }
    (void)disableIwake();
    buttress_.enableIrqs(true);

    Csi2Timing timing {};
    Ipu4pCsiPhy::calcTiming(cfg.linkFreqHz ? cfg.linkFreqHz : 400000000ULL, &timing);

    /* Send STREAM_OPEN first (before enabling CSI-2 RX port, matching Linux ipu6/ipu4p V4L2 order). */
    const int32_t openMsgIdx = allocFwMsg();
    if (openMsgIdx < 0) {
        stopStream();
        return kIOReturnNoMemory;
    }

    FwIsysStreamCfg &sc = fwMsgSlots_[openMsgIdx].u.stream;
    sc.src = static_cast<uint8_t>(fwStreamSource_);
    sc.vc = static_cast<uint8_t>(cfg.vc & 0x3U);
    sc.isl_use = 0U; /* IPU4P_FW_ISYS_USE_NO_ISL_NO_ISA */
    sc.nof_input_pins = 1U;
    sc.nof_output_pins = 1U;

    sc.input_pins[0].input_res.width = cfg.width;
    sc.input_pins[0].input_res.height = cfg.height;
    sc.input_pins[0].dt = kFwMipiDtRaw10;
    sc.input_pins[0].bits_per_pix = 10U;
    sc.input_pins[0].mapped_dt = kFwMipiDtMax;
    sc.input_pins[0].mipi_store_mode =
        packedRaw10 ? kFwMipiStoreDiscardLongHeader : kFwMipiStoreNormal;

    sc.output_pins[0].input_pin_id = 0U;
    sc.output_pins[0].output_res.width = cfg.width;
    sc.output_pins[0].output_res.height = cfg.height;
    sc.output_pins[0].stride = activeStride_;
    sc.output_pins[0].payload_buf_size = 0U;
    sc.output_pins[0].pt = packedRaw10 ? kFwPinMipi : kFwPinRawSoc;
    sc.output_pins[0].ft = packedRaw10 ? kFwFmtRaw10 : kFwFmtRaw16;
    sc.output_pins[0].send_irq = 1U;
    sc.output_pins[0].link_id = 0U; /* IPU4P_FW_ISYS_LINK_OFFLINE */

    ipuClflushRange(&sc, sizeof(sc));
    auto *openTok = static_cast<FwIsysSendToken *>(
        syscom_.sendGetToken(kBaseMsgSendQueues + kStreamHandle));
    if (!openTok) {
        freeFwMsg(static_cast<uint32_t>(openMsgIdx));
        stopStream();
        return kIOReturnBusy;
    }
    openTok->payload = fwMsgBaseIova_ + static_cast<uint32_t>(openMsgIdx) * sizeof(FwMsgSlot);
    openTok->buf_handle = static_cast<uint64_t>(openMsgIdx) + 1ULL;
    openTok->send_type = kFwSendStreamOpen;
    openTok->stream_id = kStreamHandle;
    syscom_.sendPutToken(kBaseMsgSendQueues + kStreamHandle);

    ret = waitCommandFlag(&cmdOpenDone_, 2000U);
    freeFwMsg(static_cast<uint32_t>(openMsgIdx));
    if (ret != kIOReturnSuccess) {
        IOLog("UniCamIPU: STREAM_OPEN failed (ret=0x%x fwErr=%u)\n", ret, cmdLastErr_);
        stopStream();
        return ret;
    }
    streamOpened_ = true;

    /* Start capture with slot 0, then enqueue all remaining slots in the ring. */
    ret = queueSlotToFw(0U, true);
    if (ret != kIOReturnSuccess) {
        stopStream();
        return ret;
    }

    ret = waitCommandFlag(&cmdStartDone_, 2000U);
    if (ret != kIOReturnSuccess) {
        IOLog("UniCamIPU: STREAM_START_AND_CAPTURE failed (ret=0x%x fwErr=%u)\n", ret, cmdLastErr_);
        stopStream();
        return ret;
    }
    streamStarted_ = true;

    uint32_t queuedCount = 1U;
    for (uint32_t s = 1U; s < UNICAM_IPU_RING_SLOTS; ++s) {
        if (queueSlotToFw(s, false) == kIOReturnSuccess) {
            ++queuedCount;
        }
    }

    /* Enable CSI-2 D-PHY receiver & release LP-11 AFTER firmware STREAM_START_AND_CAPTURE_ACK. */
    ret = Ipu4pCsiPhy::setPortPower(isysBase_, hwPortIndex_, cfg.csiLanes, &timing, cfg.shiftedDataTicks, true);
    if (ret != kIOReturnSuccess) {
        stopStream();
        return ret;
    }

    streaming_ = true;
    if (outStatus) {
        memset(outStatus, 0, sizeof(*outStatus));
        outStatus->hwPortIndex = hwPortIndex_;
        outStatus->fwStreamSource = fwStreamSource_;
        outStatus->stride = activeStride_;
        outStatus->payloadBytes = activePayloadBytes_;
        outStatus->ringBaseIova = ringBaseIova_;
        outStatus->ctermen = cfg.shiftedDataTicks ? 0U : timing.ctermen;
        outStatus->csettle = cfg.shiftedDataTicks ? 0U : timing.csettle;
        outStatus->dtermen = cfg.shiftedDataTicks ? 0U : timing.dtermen;
        outStatus->dsettle = cfg.shiftedDataTicks ? cfg.shiftedDataTicks : timing.dsettle;
        outStatus->queuedSlots = queuedCount;
        outStatus->streamOpened = 1U;
        outStatus->streamStarted = 1U;
    }

    IOLog("UniCamIPU: ISYS stream started (port=%u->hw%u lanes=%u %ux%u stride=%u iova=0x%x slots=%u)\n",
          fwStreamSource_, hwPortIndex_, cfg.csiLanes, cfg.width, cfg.height,
          activeStride_, ringBaseIova_, queuedCount);
    return kIOReturnSuccess;
}

IOReturn Ipu4pIsysHal::stopStream() {
    streaming_ = false;
    if (ringHdr_) {
        ringHdr_->streaming = 0U;
    }

    if (isysPowered_ && syscom_.isSyscomReady()) {
        if (streamStarted_) {
            cmdFlushDone_ = false;
            auto *tok = static_cast<FwIsysSendToken *>(
                syscom_.sendGetToken(kBaseMsgSendQueues + kStreamHandle));
            if (tok) {
                tok->payload = 0U;
                tok->buf_handle = 0ULL;
                tok->send_type = kFwSendStreamFlush;
                tok->stream_id = kStreamHandle;
                syscom_.sendPutToken(kBaseMsgSendQueues + kStreamHandle);
                waitCommandFlag(&cmdFlushDone_, 500U);
            }
            streamStarted_ = false;
        }

        if (streamOpened_) {
            cmdCloseDone_ = false;
            auto *tok = static_cast<FwIsysSendToken *>(
                syscom_.sendGetToken(kBaseMsgSendQueues + kStreamHandle));
            if (tok) {
                tok->payload = 0U;
                tok->buf_handle = 0ULL;
                tok->send_type = kFwSendStreamClose;
                tok->stream_id = kStreamHandle;
                syscom_.sendPutToken(kBaseMsgSendQueues + kStreamHandle);
                waitCommandFlag(&cmdCloseDone_, 500U);
            }
            streamOpened_ = false;
        }
    } else {
        streamStarted_ = false;
        streamOpened_ = false;
    }

    if (isysPowered_ && isysBase_) {
        Ipu4pCsiPhy::setPortPower(isysBase_, hwPortIndex_, activeCfg_.csiLanes, nullptr, 0U, false);
    }

    powerDownIsysAndCloseSyscom();

    if (fwMsgDmaBuf_.isValid()) {
        if (fwMsgBaseIova_ != 0U) {
            isysMmu_.unmapRange(fwMsgBaseIova_, fwMsgDmaBuf_.byteSize());
            fwMsgBaseIova_ = 0U;
        }
        fwMsgDmaBuf_.release();
        fwMsgSlots_ = nullptr;
    }

    if (ringBaseIova_ != 0U && ringDmaBuf_.isValid()) {
        isysMmu_.unmapRange(ringBaseIova_, ringDmaBuf_.byteSize());
        ringBaseIova_ = 0U;
    }
    memset(slotIovas_, 0, sizeof(slotIovas_));
    memset(fwMsgTrackers_, 0, sizeof(fwMsgTrackers_));
    return kIOReturnSuccess;
}

IOReturn Ipu4pIsysHal::requeueSlot(uint32_t slotIndex) {
    if (!streaming_ || !isysPowered_ || !syscom_.isSyscomReady()) {
        return kIOReturnNotReady;
    }
    if (slotIndex >= UNICAM_IPU_RING_SLOTS || !ringHdr_) {
        return kIOReturnBadArgument;
    }
    return queueSlotToFw(slotIndex, false);
}

void Ipu4pIsysHal::pushReadyEvent(const UniCamIpuFrameEvent &ev) {
    if (readyCount_ >= kReadyQueueCap) {
        /* Drop oldest event cleanly so newer frames keep moving. */
        readyHead_ = (readyHead_ + 1U) % kReadyQueueCap;
        --readyCount_;
        ++framesDropped_;
    }
    readyQueue_[readyTail_] = ev;
    readyTail_ = (readyTail_ + 1U) % kReadyQueueCap;
    ++readyCount_;
}

bool Ipu4pIsysHal::popCompletedFrame(UniCamIpuFrameEvent *outEvent) {
    if (!outEvent || readyCount_ == 0U) {
        return false;
    }
    *outEvent = readyQueue_[readyHead_];
    readyHead_ = (readyHead_ + 1U) % kReadyQueueCap;
    --readyCount_;
    return true;
}

void Ipu4pIsysHal::handleRespToken(const FwIsysRecvToken *tok) {
    const FwIsysRespInfo &r = tok->resp_info;

    if (r.error_info.error != 0U) {
        lastFwErr_ = r.error_info.error;
        lastFwErrDetails_ = r.error_info.error_details;
    }

    switch (r.type) {
        case kFwRespStreamOpenDone:
            cmdLastErr_ = r.error_info.error;
            cmdLastErrDetails_ = r.error_info.error_details;
            cmdOpenDone_ = true;
            break;

        case kFwRespStreamStartAck:
        case kFwRespStreamStartAndCaptureAck:
            cmdLastErr_ = r.error_info.error;
            cmdLastErrDetails_ = r.error_info.error_details;
            cmdStartDone_ = true;
            break;

        case kFwRespStreamStopAck:
            cmdLastErr_ = r.error_info.error;
            cmdLastErrDetails_ = r.error_info.error_details;
            cmdStopDone_ = true;
            break;

        case kFwRespStreamFlushAck:
            cmdLastErr_ = r.error_info.error;
            cmdLastErrDetails_ = r.error_info.error_details;
            cmdFlushDone_ = true;
            break;

        case kFwRespStreamCloseAck:
            cmdLastErr_ = r.error_info.error;
            cmdLastErrDetails_ = r.error_info.error_details;
            cmdCloseDone_ = true;
            break;

        case kFwRespPinDataReady:
        case kFwRespPinDataSkipped:
        case kFwRespStreamCaptureSkipped: {
            int32_t matchedMsg = -1;
            int32_t slotIdx = -1;

            for (uint32_t i = 0; i < kNumFwMsgs; ++i) {
                const FwMsgTracker &tr = fwMsgTrackers_[i];
                if (!tr.inUse || !tr.isFrame) continue;
                if (tr.streamHandle != r.stream_handle || tr.frameCounter != r.frame_counter) continue;
                if (r.type != kFwRespStreamCaptureSkipped &&
                    r.pin.out_buf_id != 0ULL &&
                    r.pin.out_buf_id != (static_cast<uint64_t>(i) + 1ULL)) {
                    continue;
                }
                matchedMsg = static_cast<int32_t>(i);
                slotIdx = tr.slotIndex;
                break;
            }

            if (matchedMsg < 0 || slotIdx < 0 || slotIdx >= static_cast<int32_t>(UNICAM_IPU_RING_SLOTS)) {
                break;
            }

            freeFwMsg(static_cast<uint32_t>(matchedMsg));

            const uint64_t ticks = (static_cast<uint64_t>(r.timestamp[1]) << 32) | r.timestamp[0];
            const uint64_t tsNs = buttress_.tscTicksToNs(ticks);
            const bool skipped = (r.type == kFwRespPinDataSkipped || r.type == kFwRespStreamCaptureSkipped);

            if (skipped) {
                ++framesSkipped_;
            } else {
                ++framesCompleted_;
            }

            const uint32_t seq = nextSequence_++;
            const uint32_t s = static_cast<uint32_t>(slotIdx);

            if (ringHdr_) {
                UniCamIpuRingSlotMeta &meta = ringHdr_->slots[s];
                meta.state = kUniCamIpuSlotReadyForUser;
                meta.sequence = seq;
                meta.fwFrameCounter = r.frame_counter;
                meta.payloadBytes = activePayloadBytes_;
                meta.csiErrorMask = lastCsiErrMask_;
                meta.fwError = r.error_info.error;
                meta.timestampTicks = ticks;
                meta.timestampNs = tsNs;
            }

            UniCamIpuFrameEvent ev {};
            ev.slotIndex = s;
            ev.sequence = seq;
            ev.fwFrameCounter = r.frame_counter;
            ev.payloadBytes = activePayloadBytes_;
            ev.stride = activeStride_;
            ev.width = activeCfg_.width;
            ev.height = activeCfg_.height;
            ev.pixelFormat = activeCfg_.pixelFormat;
            ev.timestampTicks = ticks;
            ev.timestampNs = tsNs;
            ev.csiErrorMask = lastCsiErrMask_;
            ev.fwError = r.error_info.error;
            ev.isSkipped = skipped ? 1U : 0U;
            pushReadyEvent(ev);
            break;
        }

        default:
            break;
    }
}

bool Ipu4pIsysHal::pollOrHandleIrq(bool fromIsr, bool *outFrameReady) {
    if (outFrameReady) *outFrameReady = false;
    if (!initialized_ || !bar0_) {
        return false;
    }

    bool anyWork = false;
    const uint32_t btrsIsr = mmioRead32(bar0_, kBtrsRegIsrStatus);
    if (btrsIsr != 0U && btrsIsr != 0xFFFFFFFFU) {
        if (fromIsr) {
            ++msiIrqCount_;
        }
        mmioWrite32(bar0_, kBtrsRegIsrClear, btrsIsr);
        if (btrsIsr & (kBtrsIsrIpcExecDoneByCse | kBtrsIsrIpcFromCseWaiting)) {
            ++cseIrqCount_;
        }
        if (btrsIsr & kBtrsIsrIsIrq) {
            ++isysIrqCount_;
        }
        anyWork = true;
    }

    if (!isysPowered_ || !isysBase_) {
        return anyWork;
    }

    const uint32_t unispart = mmioRead32(isysBase_, kIsysUnispartIrqStatus);
    const uint32_t sip0 = mmioRead32(isysBase_, kIsysSip0IrqCtrlStatus);
    const uint32_t sip1 = mmioRead32(isysBase_, kIsysSip1IrqCtrlStatus);

    if (unispart) {
        mmioWrite32(isysBase_, kIsysUnispartIrqClear, unispart);
    }
    if (sip0) {
        mmioWrite32(isysBase_, kIsysSip0IrqCtrlClear, sip0);
    }
    if (sip1) {
        mmioWrite32(isysBase_, kIsysSip1IrqCtrlClear, sip1);
    }

    if (sip0 || sip1 || (unispart & (ipuBit(3) | ipuBit(4)))) {
        const uint32_t csiErr = Ipu4pCsiPhy::collectAndClearPortErrors(isysBase_, hwPortIndex_);
        if (csiErr != 0U) {
            lastCsiErrMask_ |= csiErr;
            ++csiIrqCount_;
        }
        anyWork = true;
    }

    if (unispart & kIsysUnispartIrqSw) {
        mmioWrite32(isysBase_, kIsysUnispartSwIrqReg, 0U);
        anyWork = true;
    }

    const uint32_t prevReadyCount = readyCount_;
    for (uint32_t iter = 0; iter < 64U; ++iter) {
        auto *tok = static_cast<FwIsysRecvToken *>(syscom_.recvGetToken(kBaseMsgRecvQueues));
        if (!tok) {
            break;
        }
        FwIsysRecvToken copy {};
        memcpy(&copy, tok, sizeof(copy));
        syscom_.recvPutToken(kBaseMsgRecvQueues);

        if (!fromIsr) {
            ++polledTokenCount_;
        }
        handleRespToken(&copy);
        anyWork = true;
    }

    if (outFrameReady && readyCount_ > prevReadyCount) {
        *outFrameReady = true;
    }
    return anyWork;
}

IOReturn Ipu4pIsysHal::handleSleep() {
    if (streaming_ || streamOpened_ || isysPowered_) {
        stopStream();
    }
    buttress_.enableIrqs(false);
    return kIOReturnSuccess;
}

IOReturn Ipu4pIsysHal::handleWake() {
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    buttress_.restoreAfterWake();
    if (!buttress_.isAuthDone()) {
        fwAuthenticated_ = false;
        freeFirmwareBuffers();
        IOLog("UniCamIPU: S3/D3 wake cleared CSE auth (sec=0x%08x) -> armed on-demand FW re-boot\n",
              buttress_.readSecurityCtl());
    } else {
        buttress_.configureVcMechanism();
        buttress_.enableIrqs(true);
    }
    return kIOReturnSuccess;
}

} // namespace ipu4p
