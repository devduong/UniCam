#ifndef UNICAM_IPU4P_ISYS_HAL_HPP
#define UNICAM_IPU4P_ISYS_HAL_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "../IIpuIsysHal.hpp"
#include "Ipu4pButtress.hpp"
#include "Ipu4pCpd.hpp"
#include "Ipu4pCsiPhy.hpp"
#include "Ipu4pFwAbi.hpp"
#include "Ipu4pMmu.hpp"
#include "Ipu4pSyscom.hpp"
#include "IpuDmaBuffer.hpp"

namespace ipu4p {

class Ipu4pIsysHal : public IIpuIsysHal {
public:
    Ipu4pIsysHal();
    virtual ~Ipu4pIsysHal() APPLE_KEXT_OVERRIDE;

    virtual bool init(IOPCIDevice *pciDevice,
                      volatile uint8_t *bar0Base,
                      uint64_t bar0PhysAddr,
                      uint64_t bar0Length,
                      uint32_t msiInterruptIndex) APPLE_KEXT_OVERRIDE;

    virtual void cleanup() APPLE_KEXT_OVERRIDE;
    virtual void onPciPowerReady() APPLE_KEXT_OVERRIDE;

    virtual void fillHwInfo(UniCamIpuHwInfo *outInfo) APPLE_KEXT_OVERRIDE;
    virtual void fillDiag(UniCamIpuDiag *outDiag) APPLE_KEXT_OVERRIDE;

    virtual IOReturn bootFirmware(const uint8_t *cpdData,
                                  uint32_t cpdSize,
                                  uint32_t requestedDmaMode,
                                  UniCamIpuBootReport *outReport) APPLE_KEXT_OVERRIDE;
    virtual IOReturn bootEmbeddedFirmware(uint32_t requestedDmaMode,
                                          UniCamIpuBootReport *outReport) APPLE_KEXT_OVERRIDE;

    virtual IOReturn shutdownFirmware() APPLE_KEXT_OVERRIDE;

    virtual IOReturn attachRingBuffer(IOBufferMemoryDescriptor *ringDesc) APPLE_KEXT_OVERRIDE;
    virtual void detachRingBuffer() APPLE_KEXT_OVERRIDE;

    virtual IOReturn startStream(const UniCamIpuStreamConfig &cfg,
                                 UniCamIpuStreamStatus *outStatus) APPLE_KEXT_OVERRIDE;

    virtual IOReturn stopStream() APPLE_KEXT_OVERRIDE;

    virtual IOReturn requeueSlot(uint32_t slotIndex) APPLE_KEXT_OVERRIDE;
    virtual bool popCompletedFrame(UniCamIpuFrameEvent *outEvent) APPLE_KEXT_OVERRIDE;

    virtual bool pollOrHandleIrq(bool fromIsr, bool *outFrameReady) APPLE_KEXT_OVERRIDE;

    virtual bool isStreaming() const APPLE_KEXT_OVERRIDE { return streaming_; }
    virtual bool isFwAuthenticated() const APPLE_KEXT_OVERRIDE { return fwAuthenticated_; }

    virtual IOReturn handleSleep() APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleWake() APPLE_KEXT_OVERRIDE;

private:
    static constexpr uint32_t kStreamHandle     = 0U;
    static constexpr uint32_t kNumFwMsgs        = 16U;
    static constexpr uint32_t kReadyQueueCap    = 16U;

    struct FwMsgSlot {
        union {
            FwIsysStreamCfg stream;
            FwIsysFrameBuffSet frame;
        } u;
        uint8_t  pad[16];
    };
    static_assert(sizeof(FwMsgSlot) >= sizeof(FwIsysStreamCfg), "FwMsgSlot too small");

    struct FwMsgTracker {
        uint8_t  inUse;
        uint8_t  isFrame;
        uint8_t  frameCounter;
        uint8_t  streamHandle;
        int32_t  slotIndex;
    };

    IOReturn bootFirmwareWithMode(const uint8_t *cpdData,
                                  uint32_t cpdSize,
                                  uint32_t concreteDmaMode,
                                  UniCamIpuBootReport *outReport);
    IOReturn powerUpIsysAndOpenSyscom(uint32_t *outStepFailed, UniCamIpuBootReport *outReport = nullptr);
    void powerDownIsysAndCloseSyscom();
    IOReturn sendProxyToken(uint32_t reqId, uint32_t regionIndex, uint32_t value);
    IOReturn disableIwake();
    IOReturn queueSlotToFw(uint32_t slotIndex, bool startAndCapture);
    IOReturn waitCommandFlag(volatile bool *flagPtr, uint32_t timeoutMs);
    void pushReadyEvent(const UniCamIpuFrameEvent &ev);
    void handleRespToken(const FwIsysRecvToken *tok);
    int32_t allocFwMsg();
    void freeFwMsg(uint32_t idx);
    void freeFirmwareBuffers();

    IOPCIDevice *pciDevice_;
    volatile uint8_t *bar0_;
    volatile uint8_t *isysBase_;
    volatile uint8_t *psysBase_;
    uint64_t bar0Phys_;
    uint64_t bar0Len_;
    uint32_t msiIrqIndex_;

    uint16_t vendorId_;
    uint16_t deviceId_;
    uint16_t revisionId_;
    uint16_t subVendorId_;
    uint16_t subDeviceId_;

    bool hasIommuParent_;
    uint32_t activeDmaMode_;
    uint32_t preferredAutoDmaMode_;
    bool initialized_;
    bool fwLoaded_;
    bool fwAuthenticated_;
    bool isysPowered_;
    bool streaming_;
    bool streamOpened_;
    bool streamStarted_;

    Ipu4pButtress buttress_;
    Ipu4pMmu psysMmu_;
    Ipu4pMmu isysMmu_;
    Ipu4pSyscom syscom_;

    CpdParseInfo cpdInfo_;
    IpuDmaBuffer fwCpdBuf_;
    uint32_t psysFwIova_;
    uint32_t isysFwIova_;
    IpuDmaBuffer psysPkgDirBuf_;
    uint32_t psysPkgDirIova_;
    IpuDmaBuffer isysPkgDirBuf_;
    uint32_t isysPkgDirIova_;

    IpuDmaBuffer ringDmaBuf_;
    uint32_t ringBaseIova_;
    UniCamIpuRingHeader *ringHdr_;
    uint32_t slotIovas_[UNICAM_IPU_RING_SLOTS];

    IpuDmaBuffer fwMsgDmaBuf_;
    uint32_t fwMsgBaseIova_;
    FwMsgSlot *fwMsgSlots_;
    FwMsgTracker fwMsgTrackers_[kNumFwMsgs];

    UniCamIpuStreamConfig activeCfg_;
    uint32_t hwPortIndex_;
    uint32_t fwStreamSource_;
    uint32_t activeStride_;
    uint32_t activePayloadBytes_;
    uint8_t nextFrameCounter_;
    uint32_t nextSequence_;

    volatile bool cmdOpenDone_;
    volatile bool cmdStartDone_;
    volatile bool cmdStopDone_;
    volatile bool cmdFlushDone_;
    volatile bool cmdCloseDone_;
    volatile uint32_t cmdLastErr_;
    volatile uint32_t cmdLastErrDetails_;

    UniCamIpuFrameEvent readyQueue_[kReadyQueueCap];
    uint32_t readyHead_;
    uint32_t readyTail_;
    uint32_t readyCount_;

    uint64_t msiIrqCount_;
    uint64_t isysIrqCount_;
    uint64_t csiIrqCount_;
    uint64_t cseIrqCount_;
    uint64_t polledTokenCount_;
    uint64_t framesCompleted_;
    uint64_t framesSkipped_;
    uint64_t framesDropped_;
    uint32_t lastCsiErrMask_;
    uint32_t lastFwErr_;
    uint32_t lastFwErrDetails_;
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_ISYS_HAL_HPP */
