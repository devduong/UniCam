#ifndef UNICAM_IPU_DMA_BUFFER_HPP
#define UNICAM_IPU_DMA_BUFFER_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSPtr.h>

#include "UniCamIPUABI.h"

class IpuDmaBuffer {
public:
    IpuDmaBuffer();
    ~IpuDmaBuffer();

    static uint32_t resolveDmaMode(IOPCIDevice *pciDevice, uint32_t requestedMode, bool *outHasIommuParent);

    bool allocate(IOPCIDevice *pciDevice,
                  uint32_t dmaMode,
                  size_t byteSize,
                  bool userShared = false);

    bool attachExisting(IOPCIDevice *pciDevice,
                        uint32_t dmaMode,
                        IOBufferMemoryDescriptor *existingDesc);

    void release();

    bool isValid() const { return desc_ != nullptr && cpuAddr_ != nullptr && numPages_ > 0; }
    void *cpuAddr() const { return cpuAddr_; }
    size_t byteSize() const { return byteSize_; }
    uint32_t numPages() const { return numPages_; }
    uint32_t activeDmaMode() const { return activeDmaMode_; }
    IOBufferMemoryDescriptor *descriptor() const { return desc_; }

    /* Bus/physical address of pageIndex (always 4KB aligned, <= 39-bit). */
    uint64_t pageBusAddr(uint32_t pageIndex) const;

    void flushCache() const;

private:
    bool buildPageList(IOPCIDevice *pciDevice, uint32_t dmaMode);

    IOBufferMemoryDescriptor *desc_;
    bool ownsDescPrepared_;
    IODMACommand *dmaCmd_;
    bool dmaCmdPrepared_;
    IOMapper *mapper_;
    IOBufferMemoryDescriptor *pageListDesc_;
    uint64_t *pageAddrs_;
    void *cpuAddr_;
    size_t byteSize_;
    uint32_t numPages_;
    uint32_t activeDmaMode_;
};

#endif /* UNICAM_IPU_DMA_BUFFER_HPP */
