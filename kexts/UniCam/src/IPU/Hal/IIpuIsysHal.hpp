#ifndef UNICAM_IPU_IIPU_ISYS_HAL_HPP
#define UNICAM_IPU_IIPU_ISYS_HAL_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "UniCamIPUABI.h"

class IIpuIsysHal {
public:
    virtual ~IIpuIsysHal() = default;

    virtual bool init(IOPCIDevice *pciDevice,
                      volatile uint8_t *bar0Base,
                      uint64_t bar0PhysAddr,
                      uint64_t bar0Length,
                      uint32_t msiInterruptIndex) = 0;

    virtual void cleanup() = 0;
    virtual void onPciPowerReady() = 0;

    virtual void fillHwInfo(UniCamIpuHwInfo *outInfo) = 0;
    virtual void fillDiag(UniCamIpuDiag *outDiag) = 0;

    virtual IOReturn bootFirmware(const uint8_t *cpdData,
                                  uint32_t cpdSize,
                                  uint32_t requestedDmaMode,
                                  UniCamIpuBootReport *outReport) = 0;
    virtual IOReturn bootEmbeddedFirmware(uint32_t requestedDmaMode,
                                          UniCamIpuBootReport *outReport) = 0;

    virtual IOReturn shutdownFirmware() = 0;

    virtual IOReturn attachRingBuffer(IOBufferMemoryDescriptor *ringDesc) = 0;
    virtual void detachRingBuffer() = 0;

    virtual IOReturn startStream(const UniCamIpuStreamConfig &cfg,
                                 UniCamIpuStreamStatus *outStatus) = 0;

    virtual IOReturn stopStream() = 0;

    virtual IOReturn requeueSlot(uint32_t slotIndex) = 0;
    virtual bool popCompletedFrame(UniCamIpuFrameEvent *outEvent) = 0;

    /* Returns true if IRQ/queue processing completed a frame or command. */
    virtual bool pollOrHandleIrq(bool fromIsr, bool *outFrameReady) = 0;

    virtual bool isStreaming() const = 0;
    virtual bool isFwAuthenticated() const = 0;

    virtual IOReturn handleSleep() = 0;
    virtual IOReturn handleWake() = 0;
};

#endif /* UNICAM_IPU_IIPU_ISYS_HAL_HPP */
