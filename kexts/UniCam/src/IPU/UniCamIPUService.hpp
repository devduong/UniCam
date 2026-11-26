#ifndef UNICAM_IPU_SERVICE_HPP
#define UNICAM_IPU_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/IOPM.h>

#include "UniCamIPUABI.h"
#include "Hal/IIpuIsysHal.hpp"

class UniCamIPUUserClient;
class UniCamSoftISP;

class UniCamIPUService : public IOService {
    OSDeclareDefaultStructors(UniCamIPUService)

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;

    static UniCamIPUService *copySharedInstance();
    bool isSleepSuspended() const { return sleepSuspended_; }
    bool isHardwareStreaming() const { return !stopping_ && hwStreaming_; }

    IOReturn getInfo(UniCamIpuHwInfo *outInfo);
    IOReturn getDiagnostics(UniCamIpuDiag *outDiag);
    IOReturn bootFirmware(const uint8_t *cpdData,
                          uint32_t cpdSize,
                          uint32_t requestedDmaMode,
                          UniCamIpuBootReport *outReport);
    IOReturn shutdownFirmware();
    IOReturn startStream(const UniCamIpuStreamConfig *cfg,
                         UniCamIpuStreamStatus *outStatus,
                         UniCamIPUUserClient *owner);
    IOReturn waitNextFrame(uint32_t timeoutMs, UniCamIpuFrameEvent *outEvent);
    IOReturn requeueSlot(uint32_t slotIndex);
    IOReturn stopStream(UniCamIPUUserClient *owner);
    IOReturn processSlotToYuy2(uint32_t slotIndex,
                               uint32_t bayerOrder,
                               uint32_t rotationDeg,
                               uint8_t *dstYuy2,
                               uint32_t dstWidth,
                               uint32_t dstHeight,
                               uint32_t dstCapacityBytes,
                               UniCamIspFrameStats *outStats);
    void generateTestPatternYuy2(uint8_t *dstYuy2,
                                 uint32_t dstWidth,
                                 uint32_t dstHeight,
                                 uint32_t frameCounter);
    void resetIsp3A();

    IOBufferMemoryDescriptor *retainRingBufferDescriptor();
    void handleClientDisconnect(UniCamIPUUserClient *client);

private:
    static IOReturn gatedActionTrampoline(OSObject *owner,
                                          void *arg0,
                                          void *arg1,
                                          void *arg2,
                                          void *arg3);
    static void interruptActionTrampoline(OSObject *owner,
                                        IOInterruptEventSource *sender,
                                        int count);

    void onInterrupt(int count);
    void ensurePciD0AndBars();
    void captureMsiStateIfEnabled();
    void restoreMsiStateIfNeeded();
    IOReturn ensureRingBufferLocked();
    IOReturn waitNextFrameLocked(uint32_t timeoutMs, UniCamIpuFrameEvent *outEvent);

    IOPCIDevice *pciDevice_;
    IOMemoryDescriptor *bar0FallbackDesc_;
    IOMemoryMap *bar0Map_;
    volatile uint8_t *bar0Base_;
    uint64_t bar0Phys_;
    uint64_t bar0Len_;
    uint8_t pmCapOff_;
    uint8_t msiCapOff_;
    uint16_t savedMsiCtrl_;
    uint16_t savedMsiData_;
    uint32_t savedMsiAddrLo_;
    uint32_t savedMsiAddrHi_;
    uint32_t savedBar0TypeBits_;
    uint32_t bootBar0Lo_;
    uint32_t bootBar0Hi_;
    bool bar0RepairLogged_;
    IOWorkLoop *workLoop_;
    IOCommandGate *commandGate_;
    IOInterruptEventSource *irqSource_;
    uint32_t msiIrqIndex_;

    IIpuIsysHal *hal_;
    UniCamSoftISP *softIsp_;
    IOLock *ispLock_;
    IOBufferMemoryDescriptor *ringDesc_;
    UniCamIPUUserClient *activeOwner_;
    uint8_t frameWaitChannel_;
    bool pmInitialized_;
    bool gateAttached_;
    volatile bool stopping_;
    volatile bool hwStreaming_;
    volatile bool sleepSuspended_;
};

#endif /* UNICAM_IPU_SERVICE_HPP */
