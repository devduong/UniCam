#ifndef UNICAM_IPU4P_MMU_HPP
#define UNICAM_IPU4P_MMU_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "IpuDmaBuffer.hpp"

namespace ipu4p {

enum class MmuSubsys {
    Isys = 0,
    Psys = 1
};

class Ipu4pMmu {
public:
    static constexpr uint32_t kPageShift      = 12U;
    static constexpr uint32_t kPageSize       = (1U << kPageShift);
    static constexpr uint32_t kL1PtShift      = 22U;
    static constexpr uint32_t kL2PtShift      = 12U;
    static constexpr uint32_t kL1Ptes         = 1024U;
    static constexpr uint32_t kL2Ptes         = 1024U;
    static constexpr uint32_t kTrashBytes     = (8U * 1024U * 1024U);
    static constexpr uint32_t kTrashBaseIova  = 0x00100000U;
    static constexpr uint32_t kAllocStartIova = 0x00900000U;
    /* Keep all IOVAs strictly inside 31-bit (< 2 GiB) for both secure & non-secure FW. */
    static constexpr uint32_t kAllocEndIova   = 0x7FF00000U;
    static constexpr uint32_t kMaxMappings    = 32U;

    Ipu4pMmu();
    ~Ipu4pMmu();

    bool init(IOPCIDevice *pciDevice,
              uint32_t dmaMode,
              volatile uint8_t *subsysBase,
              MmuSubsys subsys);

    void destroy();

    /* Program L1_PHYS, INFO_BITS, and L1/L2 stream IDs into powered-on MMU HW. */
    bool programHardware();
    void hwCleanup();

    /* Map DMA buffer pages into a contiguous 31-bit IOVA range; returns 0 on error. */
    uint32_t mapBuffer(const IpuDmaBuffer &buf);
    void unmapRange(uint32_t iovaStart, size_t size);
    void tlbInvalidate();

    uint32_t mappedPageCount() const { return mappedPages_; }
    bool isInitialized() const { return initialized_; }

private:
    struct MmuHwBlock {
        uint32_t offset;
        uint32_t infoBits;
        uint32_t nrL1Streams;
        uint8_t  l1BlockSz[16];
        uint32_t nrL2Streams;
        uint8_t  l2BlockSz[16];
        uint32_t l1StreamIdRegOffset;
        uint32_t l2StreamIdRegOffset;
        bool     insertReadBeforeInvalidate;
    };

    struct MappingRecord {
        uint32_t iovaStart;
        uint32_t numPages;
        bool     active;
    };

    bool mapSinglePage(uint32_t iova, uint64_t busAddr);
    void unmapSinglePage(uint32_t iova);
    uint32_t allocateIovaRange(uint32_t numPages);
    void releaseIovaRange(uint32_t iovaStart, uint32_t numPages);

    IOPCIDevice *pciDevice_;
    uint32_t dmaMode_;
    volatile uint8_t *subsysBase_;
    MmuSubsys subsys_;
    bool initialized_;
    bool hwReady_;

    IpuDmaBuffer dummyPage_;
    IpuDmaBuffer dummyL2Pt_;
    IpuDmaBuffer l1Pt_;
    IpuDmaBuffer trashPage_;
    IpuDmaBuffer l2Pts_[kL1Ptes];

    uint32_t dummyPagePfn_;
    uint32_t dummyL2Pfn_;
    uint32_t l1PtPfn_;

    MappingRecord mappings_[kMaxMappings];
    uint32_t mappedPages_;

    MmuHwBlock hwBlocks_[3];
    uint32_t nrHwBlocks_;
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_MMU_HPP */
