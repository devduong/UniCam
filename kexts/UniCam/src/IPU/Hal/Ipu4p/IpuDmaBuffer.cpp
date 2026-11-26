#include "IpuDmaBuffer.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

extern "C" {
extern task_t kernel_task;
}

namespace {

constexpr size_t kPageSize  = 4096U;
constexpr size_t kPageMask  = kPageSize - 1U;
/* IPU4P 27-bit PFN << 12 = 39-bit address space, 4 KiB aligned */
constexpr mach_vm_address_t kIpu39BitPageMask = 0x0000007FFFFFF000ULL;

size_t alignPageUp(size_t sz) {
    return (sz + kPageMask) & ~kPageMask;
}

} // namespace

IpuDmaBuffer::IpuDmaBuffer()
    : desc_(nullptr),
      ownsDescPrepared_(false),
      dmaCmd_(nullptr),
      dmaCmdPrepared_(false),
      mapper_(nullptr),
      pageListDesc_(nullptr),
      pageAddrs_(nullptr),
      cpuAddr_(nullptr),
      byteSize_(0),
      numPages_(0),
      activeDmaMode_(kUniCamIpuDmaPhysical) {
}

IpuDmaBuffer::~IpuDmaBuffer() {
    release();
}

uint32_t IpuDmaBuffer::resolveDmaMode(IOPCIDevice *pciDevice,
                                      uint32_t requestedMode,
                                      bool *outHasIommuParent) {
    bool hasDevIommu = false;
    if (pciDevice) {
        OSObject *iommuProp = pciDevice->getProperty("iommu-parent");
        OSBoolean *useMapperProp = OSDynamicCast(
            OSBoolean, pciDevice->getProperty("IOPCIUseDeviceMapper"));
        hasDevIommu = (iommuProp != nullptr) || (useMapperProp == kOSBooleanTrue);
    }

    if (outHasIommuParent) {
        *outHasIommuParent = hasDevIommu;
    }

    switch (requestedMode) {
        case kUniCamIpuDmaPhysical:
        case kUniCamIpuDmaDeviceMapper:
        case kUniCamIpuDmaSystemMapper:
            return requestedMode;
        case kUniCamIpuDmaAuto:
        default:
            /*
             * On Ice Lake with DisableIoMapperMapping=True, IPU0@5 (0:5:0) sits behind a separate
             * DRHD without AppleVTDDeviceMapper ("iommu-parent"); never force system mapper unless
             * the device explicitly attaches to an IOMMU mapper.
             */
            return hasDevIommu ? kUniCamIpuDmaDeviceMapper : kUniCamIpuDmaPhysical;
    }
}

bool IpuDmaBuffer::allocate(IOPCIDevice *pciDevice,
                            uint32_t dmaMode,
                            size_t byteSize,
                            bool userShared) {
    release();
    if (!pciDevice || byteSize == 0) {
        return false;
    }

    const size_t alignedSize = alignPageUp(byteSize);
    IOOptionBits options = kIODirectionInOut;
    if (userShared) {
        options |= kIOMemoryKernelUserShared;
    }

    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, options, static_cast<mach_vm_size_t>(alignedSize), kIpu39BitPageMask);
    if (!buf) {
        IOLog("UniCamIPU: inTaskWithPhysicalMask(%zu bytes) failed\n", alignedSize);
        return false;
    }

    desc_ = buf;
    if (desc_->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog("UniCamIPU: IOBufferMemoryDescriptor::prepare failed\n");
        release();
        return false;
    }
    ownsDescPrepared_ = true;
    cpuAddr_ = desc_->getBytesNoCopy();
    byteSize_ = alignedSize;
    if (!cpuAddr_) {
        release();
        return false;
    }
    memset(cpuAddr_, 0, alignedSize);

    if (!buildPageList(pciDevice, dmaMode)) {
        release();
        return false;
    }
    return true;
}

bool IpuDmaBuffer::attachExisting(IOPCIDevice *pciDevice,
                                  uint32_t dmaMode,
                                  IOBufferMemoryDescriptor *existingDesc) {
    release();
    if (!pciDevice || !existingDesc) {
        return false;
    }

    existingDesc->retain();
    desc_ = existingDesc;
    ownsDescPrepared_ = false;
    cpuAddr_ = desc_->getBytesNoCopy();
    byteSize_ = static_cast<size_t>(desc_->getLength());
    if (!cpuAddr_ || byteSize_ == 0 || (byteSize_ & kPageMask) != 0) {
        release();
        return false;
    }

    if (!buildPageList(pciDevice, dmaMode)) {
        release();
        return false;
    }
    return true;
}

bool IpuDmaBuffer::buildPageList(IOPCIDevice *pciDevice, uint32_t dmaMode) {
    if (pageListDesc_) {
        pageListDesc_->release();
        pageListDesc_ = nullptr;
        pageAddrs_ = nullptr;
    }
    numPages_ = static_cast<uint32_t>(byteSize_ / kPageSize);
    if (numPages_ == 0) {
        return false;
    }

    const size_t listBytes = static_cast<size_t>(numPages_) * sizeof(uint64_t);
    IOBufferMemoryDescriptor *listBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, listBytes);
    if (!listBuf) {
        return false;
    }
    pageListDesc_ = listBuf;
    pageAddrs_ = static_cast<uint64_t *>(pageListDesc_->getBytesNoCopy());
    if (!pageAddrs_) {
        return false;
    }
    memset(pageAddrs_, 0, listBytes);

    bool hasIommu = false;
    uint32_t resolved = resolveDmaMode(pciDevice, dmaMode, &hasIommu);
    (void)hasIommu;
    if (resolved == kUniCamIpuDmaDeviceMapper) {
        IOMapper *devMapper = IOMapper::copyMapperForDevice(pciDevice);
        mapper_ = devMapper;
        if (!mapper_) {
            IOLog("UniCamIPU: copyMapperForDevice returned null, falling back to physical DMA\n");
            resolved = kUniCamIpuDmaPhysical;
        }
    }
    activeDmaMode_ = resolved;

    if (resolved == kUniCamIpuDmaPhysical) {
        for (uint32_t p = 0; p < numPages_; ++p) {
            IOByteCount segLen = 0;
            const IOPhysicalAddress pa = desc_->getPhysicalSegment(
                static_cast<IOByteCount>(p) * kPageSize, &segLen, kIOMemoryMapperNone);
            if (pa == 0 || segLen < kPageSize || (pa & kPageMask) != 0 || (pa >> 39) != 0) {
                IOLog("UniCamIPU: Invalid physical DMA page[%u]=0x%llx len=%llu\n",
                      p, static_cast<uint64_t>(pa), static_cast<uint64_t>(segLen));
                return false;
            }
            pageAddrs_[p] = static_cast<uint64_t>(pa);
        }
        return true;
    }

    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        39,
        0x10000000ULL,
        IODMACommand::kMapped,
        0,
        static_cast<UInt32>(kPageSize),
        mapper_,
        nullptr);
    if (!cmd) {
        cmd = IODMACommand::withSpecification(
            kIODMACommandOutputHost64,
            39,
            0x10000000ULL,
            IODMACommand::kMapped,
            0,
            1U,
            mapper_,
            nullptr);
    }
    if (!cmd) {
        IOLog("UniCamIPU: IODMACommand::withSpecification failed\n");
        return false;
    }

    dmaCmd_ = cmd;
    if (dmaCmd_->setMemoryDescriptor(desc_, true) != kIOReturnSuccess) {
        IOLog("UniCamIPU: IODMACommand::setMemoryDescriptor failed\n");
        return false;
    }
    dmaCmdPrepared_ = true;

    uint32_t pageIdx = 0U;
    UInt64 offset = 0ULL;
    while (offset < byteSize_ && pageIdx < numPages_) {
        IODMACommand::Segment64 seg {};
        UInt32 numSegs = 1U;
        const IOReturn ret = dmaCmd_->gen64IOVMSegments(&offset, &seg, &numSegs);
        if (ret != kIOReturnSuccess || numSegs != 1U || (seg.fIOVMAddr & kPageMask) != 0) {
            IOLog("UniCamIPU: gen64IOVMSegments failed (ret=0x%x addr=0x%llx len=%llu)\n",
                  ret, seg.fIOVMAddr, seg.fLength);
            return false;
        }
        for (UInt64 sub = 0; sub < seg.fLength && pageIdx < numPages_; sub += kPageSize) {
            const uint64_t addr = seg.fIOVMAddr + sub;
            if ((addr >> 39) != 0) {
                IOLog("UniCamIPU: Mapped DMA address exceeds 39 bits: 0x%llx\n", addr);
                return false;
            }
            pageAddrs_[pageIdx++] = addr;
        }
    }

    return (pageIdx == numPages_);
}

void IpuDmaBuffer::release() {
    if (dmaCmd_) {
        if (dmaCmdPrepared_) {
            dmaCmd_->clearMemoryDescriptor(true);
            dmaCmdPrepared_ = false;
        }
        dmaCmd_->release();
        dmaCmd_ = nullptr;
    }
    if (mapper_) {
        mapper_->release();
        mapper_ = nullptr;
    }
    if (pageListDesc_) {
        pageListDesc_->release();
        pageListDesc_ = nullptr;
    }
    pageAddrs_ = nullptr;

    if (desc_) {
        if (ownsDescPrepared_) {
            desc_->complete();
            ownsDescPrepared_ = false;
        }
        desc_->release();
        desc_ = nullptr;
    }
    cpuAddr_ = nullptr;
    byteSize_ = 0;
    numPages_ = 0;
}

uint64_t IpuDmaBuffer::pageBusAddr(uint32_t pageIndex) const {
    if (!pageAddrs_ || pageIndex >= numPages_) {
        return 0ULL;
    }
    return pageAddrs_[pageIndex];
}

void IpuDmaBuffer::flushCache() const {
    if (cpuAddr_ && byteSize_ > 0) {
        ipu4p::ipuClflushRange(cpuAddr_, byteSize_);
    }
}
