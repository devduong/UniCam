#include "Ipu4pMmu.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace ipu4p {

Ipu4pMmu::Ipu4pMmu()
    : pciDevice_(nullptr),
      dmaMode_(kUniCamIpuDmaPhysical),
      subsysBase_(nullptr),
      subsys_(MmuSubsys::Isys),
      initialized_(false),
      hwReady_(false),
      dummyPagePfn_(0U),
      dummyL2Pfn_(0U),
      l1PtPfn_(0U),
      mappedPages_(0U),
      nrHwBlocks_(0U) {
    memset(mappings_, 0, sizeof(mappings_));
    memset(hwBlocks_, 0, sizeof(hwBlocks_));
}

Ipu4pMmu::~Ipu4pMmu() {
    destroy();
}

bool Ipu4pMmu::init(IOPCIDevice *pciDevice,
                    uint32_t dmaMode,
                    volatile uint8_t *subsysBase,
                    MmuSubsys subsys) {
    destroy();
    if (!pciDevice || !subsysBase) {
        return false;
    }

    pciDevice_ = pciDevice;
    dmaMode_ = dmaMode;
    subsysBase_ = subsysBase;
    subsys_ = subsys;

    if (subsys == MmuSubsys::Isys) {
        nrHwBlocks_ = 2U;
        hwBlocks_[0] = {
            kIsysIommu0Offset, kMmuInfoDestPrimary,
            0U, {},
            0U, {},
            0U, 0U, true
        };
        hwBlocks_[1] = {
            kIsysIommu1Offset, 0U,
            16U, { 5, 16, 6, 6, 6, 6, 6, 8, 0, 0, 0, 0, 0, 0, 0, 5 },
            16U, { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
            kMmuL1StreamIdOffset, kMmuL2StreamIdOffset, false
        };
    } else {
        nrHwBlocks_ = 3U;
        hwBlocks_[0] = {
            kPsysIommu0Offset, kMmuInfoDestPrimary,
            0U, {},
            0U, {},
            0U, 0U, true
        };
        hwBlocks_[1] = {
            kPsysIommu1Offset, 0U,
            16U, { 2, 5, 4, 2, 2, 10, 5, 16, 10, 5, 0, 0, 0, 0, 0, 3 },
            16U, { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
            kMmuL1StreamIdOffset, kMmuL2StreamIdOffset, false
        };
        hwBlocks_[2] = {
            kPsysIommu1rOffset, 0U,
            16U, { 2, 6, 5, 16, 16, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 3 },
            16U, { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
            kMmuL1StreamIdOffset, kMmuL2StreamIdOffset, false
        };
    }

    if (!dummyPage_.allocate(pciDevice_, dmaMode_, kPageSize, false)) {
        destroy();
        return false;
    }
    dummyPage_.flushCache();
    dummyPagePfn_ = static_cast<uint32_t>(dummyPage_.pageBusAddr(0U) >> kPageShift);

    if (!dummyL2Pt_.allocate(pciDevice_, dmaMode_, kPageSize, false)) {
        destroy();
        return false;
    }
    uint32_t *dummyL2 = static_cast<uint32_t *>(dummyL2Pt_.cpuAddr());
    for (uint32_t i = 0; i < kL2Ptes; ++i) {
        dummyL2[i] = dummyPagePfn_;
    }
    dummyL2Pt_.flushCache();
    dummyL2Pfn_ = static_cast<uint32_t>(dummyL2Pt_.pageBusAddr(0U) >> kPageShift);

    if (!l1Pt_.allocate(pciDevice_, dmaMode_, kPageSize, false)) {
        destroy();
        return false;
    }
    uint32_t *l1 = static_cast<uint32_t *>(l1Pt_.cpuAddr());
    for (uint32_t i = 0; i < kL1Ptes; ++i) {
        l1[i] = dummyL2Pfn_;
    }
    l1Pt_.flushCache();
    l1PtPfn_ = static_cast<uint32_t>(l1Pt_.pageBusAddr(0U) >> kPageShift);

    if (!trashPage_.allocate(pciDevice_, dmaMode_, kPageSize, false)) {
        destroy();
        return false;
    }
    trashPage_.flushCache();

    const uint32_t trashPages = kTrashBytes / kPageSize;
    const uint64_t trashBusAddr = trashPage_.pageBusAddr(0U);
    for (uint32_t i = 0; i < trashPages; ++i) {
        if (!mapSinglePage(kTrashBaseIova + i * kPageSize, trashBusAddr)) {
            destroy();
            return false;
        }
    }

    initialized_ = true;
    return true;
}

void Ipu4pMmu::destroy() {
    hwReady_ = false;
    initialized_ = false;
    mappedPages_ = 0U;
    memset(mappings_, 0, sizeof(mappings_));

    for (uint32_t i = 0; i < kL1Ptes; ++i) {
        if (l2Pts_[i].isValid()) {
            l2Pts_[i].release();
        }
    }
    trashPage_.release();
    l1Pt_.release();
    dummyL2Pt_.release();
    dummyPage_.release();
    dummyPagePfn_ = 0U;
    dummyL2Pfn_ = 0U;
    l1PtPfn_ = 0U;
}

bool Ipu4pMmu::programHardware() {
    if (!initialized_ || !subsysBase_) {
        return false;
    }

    for (uint32_t i = 0; i < nrHwBlocks_; ++i) {
        const MmuHwBlock &blk = hwBlocks_[i];
        volatile uint8_t *mmuBase = subsysBase_ + blk.offset;

        mmioWrite32(mmuBase, kMmuRegL1Phys, l1PtPfn_);
        mmioWrite32(mmuBase, kMmuRegInfo, blk.infoBits);

        uint16_t blockAddr = 0U;
        for (uint32_t j = 0; j < blk.nrL1Streams; ++j) {
            if (blockAddr > kMmuMaxL1BlockAddr) {
                IOLog("UniCamIPU: Invalid MMU L1 stream config\n");
                return false;
            }
            mmioWrite32(mmuBase, blk.l1StreamIdRegOffset + 4U * j, blockAddr);
            blockAddr += blk.l1BlockSz[j];
        }

        blockAddr = 0U;
        for (uint32_t j = 0; j < blk.nrL2Streams; ++j) {
            if (blockAddr > kMmuMaxL2BlockAddr) {
                IOLog("UniCamIPU: Invalid MMU L2 stream config\n");
                return false;
            }
            mmioWrite32(mmuBase, blk.l2StreamIdRegOffset + 4U * j, blockAddr);
            blockAddr += blk.l2BlockSz[j];
        }
    }

    hwReady_ = true;
    tlbInvalidate();
    return true;
}

void Ipu4pMmu::hwCleanup() {
    hwReady_ = false;
}

void Ipu4pMmu::tlbInvalidate() {
    if (!hwReady_ || !subsysBase_) {
        return;
    }

    for (uint32_t i = 0; i < nrHwBlocks_; ++i) {
        volatile uint8_t *mmuBase = subsysBase_ + hwBlocks_[i].offset;
        if (hwBlocks_[i].insertReadBeforeInvalidate) {
            (void)mmioRead32(mmuBase, kMmuRegL1Phys);
        }
        mmioWrite32(mmuBase, kMmuRegTlbInvalidate, 0xFFFFFFFFU);
        ipuWmb();
    }
}

bool Ipu4pMmu::mapSinglePage(uint32_t iova, uint64_t busAddr) {
    const uint32_t l1Idx = iova >> kL1PtShift;
    const uint32_t l2Idx = (iova >> kL2PtShift) & (kL2Ptes - 1U);
    if (l1Idx >= kL1Ptes) {
        return false;
    }

    uint32_t *l1 = static_cast<uint32_t *>(l1Pt_.cpuAddr());
    if (!l2Pts_[l1Idx].isValid()) {
        if (!l2Pts_[l1Idx].allocate(pciDevice_, dmaMode_, kPageSize, false)) {
            return false;
        }
        uint32_t *l2 = static_cast<uint32_t *>(l2Pts_[l1Idx].cpuAddr());
        for (uint32_t i = 0; i < kL2Ptes; ++i) {
            l2[i] = dummyPagePfn_;
        }
        l2Pts_[l1Idx].flushCache();

        l1[l1Idx] = static_cast<uint32_t>(l2Pts_[l1Idx].pageBusAddr(0U) >> kPageShift);
        ipuClflushRange(&l1[l1Idx], sizeof(uint32_t));
    }

    uint32_t *l2 = static_cast<uint32_t *>(l2Pts_[l1Idx].cpuAddr());
    l2[l2Idx] = static_cast<uint32_t>(busAddr >> kPageShift);
    ipuClflushRange(&l2[l2Idx], sizeof(uint32_t));
    ++mappedPages_;
    return true;
}

void Ipu4pMmu::unmapSinglePage(uint32_t iova) {
    const uint32_t l1Idx = iova >> kL1PtShift;
    const uint32_t l2Idx = (iova >> kL2PtShift) & (kL2Ptes - 1U);
    if (l1Idx >= kL1Ptes || !l2Pts_[l1Idx].isValid()) {
        return;
    }
    uint32_t *l2 = static_cast<uint32_t *>(l2Pts_[l1Idx].cpuAddr());
    if (l2[l2Idx] != dummyPagePfn_ && mappedPages_ > 0U) {
        --mappedPages_;
    }
    l2[l2Idx] = dummyPagePfn_;
    ipuClflushRange(&l2[l2Idx], sizeof(uint32_t));
}

uint32_t Ipu4pMmu::allocateIovaRange(uint32_t numPages) {
    if (numPages == 0U) return 0U;
    const uint64_t bytes = static_cast<uint64_t>(numPages) * kPageSize;
    uint64_t candidate = kAllocStartIova;

    /* Simple lowest-fit interval search among at most kMaxMappings active ranges */
    for (;;) {
        if (candidate + bytes > kAllocEndIova) {
            return 0U;
        }
        bool overlapped = false;
        uint64_t nextCandidate = candidate;
        for (uint32_t i = 0; i < kMaxMappings; ++i) {
            if (!mappings_[i].active) continue;
            const uint64_t rStart = mappings_[i].iovaStart;
            const uint64_t rEnd = rStart + static_cast<uint64_t>(mappings_[i].numPages) * kPageSize;
            if (candidate < rEnd && (candidate + bytes) > rStart) {
                overlapped = true;
                if (rEnd > nextCandidate) {
                    nextCandidate = rEnd;
                }
            }
        }
        if (!overlapped) {
            break;
        }
        candidate = nextCandidate;
    }

    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (!mappings_[i].active) {
            mappings_[i].iovaStart = static_cast<uint32_t>(candidate);
            mappings_[i].numPages = numPages;
            mappings_[i].active = true;
            return mappings_[i].iovaStart;
        }
    }
    return 0U;
}

void Ipu4pMmu::releaseIovaRange(uint32_t iovaStart, uint32_t numPages) {
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (mappings_[i].active &&
            mappings_[i].iovaStart == iovaStart &&
            mappings_[i].numPages == numPages) {
            mappings_[i].active = false;
            mappings_[i].iovaStart = 0U;
            mappings_[i].numPages = 0U;
            return;
        }
    }
}

uint32_t Ipu4pMmu::mapBuffer(const IpuDmaBuffer &buf) {
    if (!initialized_ || !buf.isValid()) {
        return 0U;
    }

    const uint32_t pages = buf.numPages();
    const uint32_t iovaStart = allocateIovaRange(pages);
    if (iovaStart == 0U) {
        IOLog("UniCamIPU: Out of IOVA space (%u pages)\n", pages);
        return 0U;
    }

    for (uint32_t p = 0; p < pages; ++p) {
        if (!mapSinglePage(iovaStart + p * kPageSize, buf.pageBusAddr(p))) {
            for (uint32_t r = 0; r < p; ++r) {
                unmapSinglePage(iovaStart + r * kPageSize);
            }
            releaseIovaRange(iovaStart, pages);
            return 0U;
        }
    }

    tlbInvalidate();
    return iovaStart;
}

void Ipu4pMmu::unmapRange(uint32_t iovaStart, size_t size) {
    if (!initialized_ || iovaStart == 0U || size == 0U) {
        return;
    }
    const uint32_t pages = static_cast<uint32_t>((size + kPageSize - 1U) / kPageSize);
    for (uint32_t p = 0; p < pages; ++p) {
        unmapSinglePage(iovaStart + p * kPageSize);
    }
    releaseIovaRange(iovaStart, pages);
    tlbInvalidate();
}

} // namespace ipu4p
