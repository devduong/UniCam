#include "UniCamIPUService.hpp"
#include "Hal/Ipu4p/Ipu4pIsysHal.hpp"
#include "Hal/Ipu4p/Ipu4pRegs.hpp"
#include "SoftISP/UniCamSoftISP.hpp"
#include "UniCamIPUUserClient.hpp"
#include "UniCamKernelGate.h"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>
#include <string.h>

extern "C" {
extern task_t kernel_task;
}

#define super IOService
OSDefineMetaClassAndStructors(UniCamIPUService, IOService)

namespace {

enum {
    kPowerStateOff   = 0,
    kPowerStateOn    = 1,
    kPowerStateCount = 2
};

IOPMPowerState kUniCamIpuPowerStates[kPowerStateCount] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

constexpr size_t kMaxFirmwareBytes = 16U * 1024U * 1024U;

enum GatedIpuOp {
    kOpGetInfo = 0,
    kOpGetDiag,
    kOpBootFirmware,
    kOpBootEmbeddedFirmware,
    kOpShutdownFirmware,
    kOpStartStream,
    kOpWaitNextFrame,
    kOpRequeueSlot,
    kOpStopStream,
    kOpRetainRingDesc,
    kOpClientDisconnect,
    kOpSleep,
    kOpWake,
    kOpTeardown
};

struct GatedIpuRequest {
    GatedIpuOp op;
    uint32_t u0;
    uint32_t u1;
    const void *inPtr;
    void *outPtr;
    UniCamIPUUserClient *client;
};

uint32_t findMsiInterruptIndex(IOPCIDevice *pciDevice) {
    if (!pciDevice) return 0U;
    for (int idx = 0; idx < 8; ++idx) {
        int irqType = 0;
        if (pciDevice->getInterruptType(idx, &irqType) != kIOReturnSuccess) {
            break;
        }
        if ((irqType & kIOInterruptTypePCIMessaged) != 0) {
            return static_cast<uint32_t>(idx);
        }
    }
    return 0U;
}

uint64_t currentUptimeNs() {
    uint64_t nowTicks = 0ULL;
    clock_get_uptime(&nowTicks);
    uint64_t nowNs = 0ULL;
    absolutetime_to_nanoseconds(nowTicks, &nowNs);
    return nowNs;
}

} // namespace

bool UniCamIPUService::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    pciDevice_ = nullptr;
    bar0FallbackDesc_ = nullptr;
    bar0Map_ = nullptr;
    bar0Base_ = nullptr;
    bar0Phys_ = 0ULL;
    bar0Len_ = 0ULL;
    pmCapOff_ = 0U;
    msiCapOff_ = 0U;
    savedMsiCtrl_ = 0U;
    savedMsiData_ = 0U;
    savedMsiAddrLo_ = 0U;
    savedMsiAddrHi_ = 0U;
    savedBar0TypeBits_ = 0U;
    bootBar0Lo_ = 0U;
    bootBar0Hi_ = 0U;
    bar0RepairLogged_ = false;
    workLoop_ = nullptr;
    commandGate_ = nullptr;
    irqSource_ = nullptr;
    msiIrqIndex_ = 0U;
    hal_ = nullptr;
    softIsp_ = nullptr;
    ispLock_ = nullptr;
    ringDesc_ = nullptr;
    activeOwner_ = nullptr;
    frameWaitChannel_ = 0U;
    pmInitialized_ = false;
    gateAttached_ = false;
    stopping_ = false;
    hwStreaming_ = false;
    sleepSuspended_ = false;
    return true;
}

UniCamIPUService *UniCamIPUService::copySharedInstance() {
    OSDictionary *matching = IOService::serviceMatching("UniCamIPUService");
    if (!matching) {
        return nullptr;
    }
    IOService *svc = IOService::copyMatchingService(matching);
    matching->release();
    if (!svc) {
        return nullptr;
    }
    UniCamIPUService *ipu = OSDynamicCast(UniCamIPUService, svc);
    if (!ipu || ipu->isInactive() || ipu->stopping_) {
        svc->release();
        return nullptr;
    }
    return ipu;
}

void UniCamIPUService::free() {
    if (gateAttached_ && workLoop_ && commandGate_) {
        workLoop_->removeEventSource(commandGate_);
        gateAttached_ = false;
    }
    OSSafeReleaseNULL(commandGate_);
    OSSafeReleaseNULL(workLoop_);
    if (softIsp_) {
        delete softIsp_;
        softIsp_ = nullptr;
    }
    if (ispLock_) {
        IOLockFree(ispLock_);
        ispLock_ = nullptr;
    }
    if (hal_) {
        delete hal_;
        hal_ = nullptr;
    }
    if (ringDesc_) {
        ringDesc_->complete();
        ringDesc_->release();
        ringDesc_ = nullptr;
    }
    super::free();
}

IOService *UniCamIPUService::probe(IOService *provider, SInt32 *score) {
    if (!unicam::isKernelSupportedForLoad("UniCamIPU")) {
        return nullptr;
    }
    if (!super::probe(provider, score)) {
        return nullptr;
    }
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return nullptr;
    }
    const uint16_t vid = pci->configRead16(kIOPCIConfigVendorID);
    const uint16_t did = pci->configRead16(kIOPCIConfigDeviceID);
    if (vid != UNICAM_IPU_PCI_VENDOR_INTEL || did != UNICAM_IPU_PCI_DEVICE_IPU4P) {
        return nullptr;
    }
    if (score) {
        *score = 1000;
    }
    return this;
}

void UniCamIPUService::captureMsiStateIfEnabled() {
    if (!pciDevice_ || msiCapOff_ == 0U) {
        return;
    }
    const uint16_t ctrl = pciDevice_->configRead16(static_cast<UInt8>(msiCapOff_ + 2U));
    const uint32_t addrLo = pciDevice_->configRead32(static_cast<UInt8>(msiCapOff_ + 4U));
    if ((ctrl & 0x0001U) == 0U || addrLo == 0U || addrLo == 0xFFFFFFFFU) {
        return;
    }
    savedMsiCtrl_ = ctrl;
    savedMsiAddrLo_ = addrLo;
    if ((ctrl & 0x0080U) != 0U) {
        savedMsiAddrHi_ = pciDevice_->configRead32(static_cast<UInt8>(msiCapOff_ + 8U));
        savedMsiData_ = pciDevice_->configRead16(static_cast<UInt8>(msiCapOff_ + 12U));
    } else {
        savedMsiAddrHi_ = 0U;
        savedMsiData_ = pciDevice_->configRead16(static_cast<UInt8>(msiCapOff_ + 8U));
    }
}

void UniCamIPUService::restoreMsiStateIfNeeded() {
    if (!pciDevice_ || msiCapOff_ == 0U || (savedMsiCtrl_ & 0x0001U) == 0U || savedMsiAddrLo_ == 0U) {
        return;
    }
    const uint16_t ctrl = pciDevice_->configRead16(static_cast<UInt8>(msiCapOff_ + 2U));
    const uint32_t addrLo = pciDevice_->configRead32(static_cast<UInt8>(msiCapOff_ + 4U));
    if ((ctrl & 0x0001U) != 0U && addrLo != 0U) {
        return;
    }
    pciDevice_->configWrite32(static_cast<UInt8>(msiCapOff_ + 4U), savedMsiAddrLo_);
    if ((savedMsiCtrl_ & 0x0080U) != 0U) {
        pciDevice_->configWrite32(static_cast<UInt8>(msiCapOff_ + 8U), savedMsiAddrHi_);
        pciDevice_->configWrite16(static_cast<UInt8>(msiCapOff_ + 12U), savedMsiData_);
    } else {
        pciDevice_->configWrite16(static_cast<UInt8>(msiCapOff_ + 8U), savedMsiData_);
    }
    pciDevice_->configWrite16(static_cast<UInt8>(msiCapOff_ + 2U), savedMsiCtrl_);
    (void)pciDevice_->configRead16(static_cast<UInt8>(msiCapOff_ + 2U));
    IOLog("UniCamIPU: Restored MSI capability @0x%02x (ctrl=0x%04x)\n", msiCapOff_, savedMsiCtrl_);
}

void UniCamIPUService::ensurePciD0AndBars() {
    if (!pciDevice_) {
        return;
    }
    if (pmCapOff_ != 0U) {
        const uint16_t pmcsr = pciDevice_->configRead16(static_cast<UInt8>(pmCapOff_ + 4U));
        if ((pmcsr & 0x0003U) != 0U) {
            pciDevice_->configWrite16(static_cast<UInt8>(pmCapOff_ + 4U),
                                      static_cast<uint16_t>((pmcsr & ~0x0003U) | 0x8000U));
            IOSleep(15);
        }
    }
    if (bar0Phys_ != 0ULL) {
        /*
         * Intel IPU4P (8086:8a19) ISCABAR at 0x10/0x14 is a 39-bit register even
         * when bit 2 of 0x10 reads 0; always inspect & program both 0x10 and 0x14
         * so stale upper-DWORD probe bits (e.g. 0x0000007F) never misroute BAR0.
         */
        const uint32_t bar0Lo = pciDevice_->configRead32(kIOPCIConfigBaseAddress0);
        const uint32_t bar0Hi = pciDevice_->configRead32(kIOPCIConfigBaseAddress1);
        const uint64_t curBarPhys =
            (static_cast<uint64_t>(bar0Hi) << 32) |
            static_cast<uint64_t>(bar0Lo & ~0xFU);
        const uint32_t wantLo =
            static_cast<uint32_t>(bar0Phys_ & 0xFFFFFFF0ULL) | (savedBar0TypeBits_ & 0xEU);
        const uint32_t wantHi = static_cast<uint32_t>(bar0Phys_ >> 32);

        if (curBarPhys != bar0Phys_ ||
            bar0Hi != wantHi ||
            (bar0Lo & ~0xFU) != (wantLo & ~0xFU)) {
            const uint16_t cmdOrig = pciDevice_->configRead16(kIOPCIConfigCommand);
            pciDevice_->configWrite16(kIOPCIConfigCommand,
                                      static_cast<uint16_t>(cmdOrig & ~0x0003U));
            pciDevice_->configWrite32(kIOPCIConfigBaseAddress0, wantLo);
            pciDevice_->configWrite32(kIOPCIConfigBaseAddress1, wantHi);
            const uint32_t rbLo = pciDevice_->configRead32(kIOPCIConfigBaseAddress0);
            const uint32_t rbHi = pciDevice_->configRead32(kIOPCIConfigBaseAddress1);
            IODelay(50);
            if (!bar0RepairLogged_) {
                bar0RepairLogged_ = true;
                IOLog("UniCamIPU: Repaired ISCABAR 0x14/0x10: 0x%08x:%08x -> 0x%08x:%08x\n",
                      bar0Hi, bar0Lo, rbHi, rbLo);
            }
        }
    }
    pciDevice_->setIOEnable(false);
    pciDevice_->setMemoryEnable(true);
    pciDevice_->setBusMasterEnable(true);
    const uint16_t cmd = pciDevice_->configRead16(kIOPCIConfigCommand);
    const uint16_t want = static_cast<uint16_t>((cmd | 0x0006U) & ~0x0001U); // Mem+BM, no I/O
    if (want != cmd) {
        pciDevice_->configWrite16(kIOPCIConfigCommand, want);
        (void)pciDevice_->configRead16(kIOPCIConfigCommand);
        IODelay(50);
    }
    restoreMsiStateIfNeeded();
    if (hal_ && bar0Base_) {
        uint32_t sec = ipu4p::mmioRead32(bar0Base_, ipu4p::kBtrsRegSecurityCtl);
        if (sec == 0xFFFFFFFFU && pmCapOff_ != 0) {
            /* Attempt PCIe FLR (if supported) + PMCSR D3hot -> D0 cycle + BAR0 restore */
            UInt8 pcieCapOff = 0U;
            if (pciDevice_->findPCICapability(0x10U /* PCI Express */, &pcieCapOff) != 0) {
                const uint32_t devCap = pciDevice_->configRead32(static_cast<UInt8>(pcieCapOff + 4U));
                if ((devCap & (1U << 28)) != 0U) {
                    const uint16_t devCtl = pciDevice_->configRead16(static_cast<UInt8>(pcieCapOff + 8U));
                    pciDevice_->configWrite16(static_cast<UInt8>(pcieCapOff + 8U),
                                              static_cast<uint16_t>(devCtl | 0x8000U));
                    IOSleep(100);
                }
            }
            const uint8_t pmcsrOff = static_cast<uint8_t>(pmCapOff_ + 4U);
            const uint16_t pmcsr = pciDevice_->configRead16(pmcsrOff);
            pciDevice_->configWrite16(pmcsrOff, static_cast<uint16_t>((pmcsr & ~0x3U) | 0x3U));
            (void)pciDevice_->configRead16(pmcsrOff);
            IOSleep(10);
            pciDevice_->configWrite16(pmcsrOff, static_cast<uint16_t>(pmcsr & ~0x3U));
            (void)pciDevice_->configRead16(pmcsrOff);
            IOSleep(10);
            if (bar0Phys_ != 0ULL) {
                const uint32_t lo = static_cast<uint32_t>(bar0Phys_ & 0xFFFFFFF0ULL) | (savedBar0TypeBits_ & 0xEU);
                const uint32_t hi = static_cast<uint32_t>(bar0Phys_ >> 32);
                pciDevice_->configWrite32(kIOPCIConfigBaseAddress0, lo);
                pciDevice_->configWrite32(kIOPCIConfigBaseAddress1, hi);
            }
            pciDevice_->configWrite16(kIOPCIConfigCommand, want);
            (void)pciDevice_->configRead16(kIOPCIConfigCommand);
            IODelay(100);
            restoreMsiStateIfNeeded();
            sec = ipu4p::mmioRead32(bar0Base_, ipu4p::kBtrsRegSecurityCtl);
            IOLog("UniCamIPU: BAR0 recovery after 0xFFFFFFFF -> SEC_CTL=0x%08x\n", sec);
        }
        if (sec != 0xFFFFFFFFU) {
            hal_->onPciPowerReady();
        }
    }
}

bool UniCamIPUService::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    pciDevice_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice_) {
        IOLog("UniCamIPU: Provider is not IOPCIDevice\n");
        return false;
    }

    if (!pciDevice_->open(this)) {
        IOLog("UniCamIPU: Failed to open IOPCIDevice\n");
        return false;
    }

    PMinit();
    pmInitialized_ = true;
    provider->joinPMtree(this);
    registerPowerDriver(this, kUniCamIpuPowerStates, kPowerStateCount);
    makeUsable();

    UInt8 pmCap = 0;
    if (pciDevice_->findPCICapability(kIOPCICapabilityIDPowerManagement, &pmCap) != 0) {
        pmCapOff_ = pmCap;
    }
    UInt8 msiCap = 0;
    if (pciDevice_->findPCICapability(kIOPCICapabilityIDMSI, &msiCap) != 0) {
        msiCapOff_ = msiCap;
    }
    bootBar0Lo_ = pciDevice_->configRead32(kIOPCIConfigBaseAddress0);
    bootBar0Hi_ = pciDevice_->configRead32(kIOPCIConfigBaseAddress1);
    savedBar0TypeBits_ = bootBar0Lo_ & 0xEU;

    IODeviceMemory *bar0Mem = pciDevice_->getDeviceMemoryWithIndex(0);
    if (bar0Mem) {
        bar0Phys_ = static_cast<uint64_t>(bar0Mem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone));
        bar0Len_ = static_cast<uint64_t>(bar0Mem->getLength());
    }

    ensurePciD0AndBars();

    bar0Map_ = pciDevice_->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (bar0Map_) {
        const IOPhysicalAddress mappedPhys =
            bar0Map_->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        if (bar0Phys_ != 0ULL && static_cast<uint64_t>(mappedPhys) != bar0Phys_) {
            IOLog("UniCamIPU: mapDeviceMemoryWithIndex(0) mapped 0x%llx != 0x%llx, using physical fallback\n",
                  static_cast<uint64_t>(mappedPhys), bar0Phys_);
            bar0Map_->release();
            bar0Map_ = nullptr;
        }
    }
    if (!bar0Map_ && bar0Phys_ != 0ULL && bar0Len_ != 0ULL) {
        bar0FallbackDesc_ = IOMemoryDescriptor::withPhysicalAddress(
            static_cast<IOPhysicalAddress>(bar0Phys_),
            static_cast<IOByteCount>(bar0Len_),
            kIODirectionInOut);
        if (bar0FallbackDesc_) {
            bar0Map_ = bar0FallbackDesc_->map(kIOMapInhibitCache);
        }
    }
    if (!bar0Map_) {
        IOLog("UniCamIPU: Failed to map BAR0\n");
        if (bar0FallbackDesc_) {
            bar0FallbackDesc_->release();
            bar0FallbackDesc_ = nullptr;
        }
        if (pmInitialized_) {
            PMstop();
            pmInitialized_ = false;
        }
        pciDevice_->close(this);
        pciDevice_ = nullptr;
        return false;
    }
    bar0Base_ = reinterpret_cast<volatile uint8_t *>(bar0Map_->getVirtualAddress());
    if (bar0Phys_ == 0ULL) {
        bar0Phys_ = static_cast<uint64_t>(bar0Map_->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone));
    }
    if (bar0Len_ == 0ULL) {
        bar0Len_ = static_cast<uint64_t>(bar0Map_->getLength());
    }

    workLoop_ = IOWorkLoop::workLoop();
    if (!workLoop_) {
        IOLog("UniCamIPU: Failed to allocate IOWorkLoop\n");
        bar0Map_->release();
        bar0Map_ = nullptr;
        if (bar0FallbackDesc_) {
            bar0FallbackDesc_->release();
            bar0FallbackDesc_ = nullptr;
        }
        bar0Base_ = nullptr;
        if (pmInitialized_) {
            PMstop();
            pmInitialized_ = false;
        }
        pciDevice_->close(this);
        pciDevice_ = nullptr;
        return false;
    }

    commandGate_ = IOCommandGate::commandGate(this);
    if (!commandGate_ || workLoop_->addEventSource(commandGate_) != kIOReturnSuccess) {
        IOLog("UniCamIPU: Failed to create IOCommandGate\n");
        OSSafeReleaseNULL(commandGate_);
        OSSafeReleaseNULL(workLoop_);
        bar0Map_->release();
        bar0Map_ = nullptr;
        if (bar0FallbackDesc_) {
            bar0FallbackDesc_->release();
            bar0FallbackDesc_ = nullptr;
        }
        bar0Base_ = nullptr;
        if (pmInitialized_) {
            PMstop();
            pmInitialized_ = false;
        }
        pciDevice_->close(this);
        pciDevice_ = nullptr;
        return false;
    }
    gateAttached_ = true;

    msiIrqIndex_ = findMsiInterruptIndex(pciDevice_);
    IOInterruptEventSource *irq = IOInterruptEventSource::interruptEventSource(
        this,
        &UniCamIPUService::interruptActionTrampoline,
        pciDevice_,
        static_cast<int>(msiIrqIndex_));
    if (irq) {
        irqSource_ = irq;
        if (workLoop_->addEventSource(irqSource_) != kIOReturnSuccess) {
            IOLog("UniCamIPU: Failed to attach IOInterruptEventSource (idx=%u)\n", msiIrqIndex_);
            irqSource_->release();
            irqSource_ = nullptr;
        } else {
            irqSource_->enable();
            captureMsiStateIfEnabled();
        }
    } else {
        IOLog("UniCamIPU: Interrupt source unavailable (idx=%u), polled mode active\n", msiIrqIndex_);
    }

    hal_ = new ipu4p::Ipu4pIsysHal();
    if (!hal_ || !hal_->init(pciDevice_, bar0Base_, bar0Phys_, bar0Len_, msiIrqIndex_)) {
        IOLog("UniCamIPU: Failed to initialize Ipu4pIsysHal\n");
        stop(provider);
        return false;
    }

    ispLock_ = IOLockAlloc();
    softIsp_ = new UniCamSoftISP();
    if (!ispLock_ || !softIsp_) {
        IOLog("UniCamIPU: Failed to allocate UniCamSoftISP\n");
        stop(provider);
        return false;
    }

    setName("UniCamIPUService");
    registerService();

    IOLog("UniCamIPU: Started for IPU4P 0x%04x:0x%04x BAR0=0x%llx (%llu MB) CMD=0x%04x PMCSR=0x%04x MSI idx=%u\n",
          UNICAM_IPU_PCI_VENDOR_INTEL, UNICAM_IPU_PCI_DEVICE_IPU4P,
          bar0Phys_, bar0Len_ / (1024ULL * 1024ULL),
          pciDevice_->configRead16(kIOPCIConfigCommand),
          pmCapOff_ ? pciDevice_->configRead16(static_cast<UInt8>(pmCapOff_ + 4U)) : 0U,
          msiIrqIndex_);
    return true;
}

void UniCamIPUService::stop(IOService *provider) {
    if (commandGate_ && gateAttached_) {
        GatedIpuRequest req = { kOpTeardown, 0U, 0U, nullptr, nullptr, nullptr };
        commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
    } else {
        stopping_ = true;
        sleepSuspended_ = true;
        hwStreaming_ = false;
    }

    if (irqSource_) {
        irqSource_->disable();
        if (workLoop_) {
            workLoop_->removeEventSource(irqSource_);
        }
        irqSource_->release();
        irqSource_ = nullptr;
    }

    if (pmInitialized_) {
        PMstop();
        pmInitialized_ = false;
    }

    if (ispLock_) {
        IOLockLock(ispLock_);
    }
    if (ringDesc_) {
        ringDesc_->complete();
        ringDesc_->release();
        ringDesc_ = nullptr;
    }
    if (ispLock_) {
        IOLockUnlock(ispLock_);
    }

    if (bar0Map_) {
        bar0Map_->release();
        bar0Map_ = nullptr;
    }
    if (bar0FallbackDesc_) {
        bar0FallbackDesc_->release();
        bar0FallbackDesc_ = nullptr;
    }
    bar0Base_ = nullptr;

    if (pciDevice_) {
        pciDevice_->close(this);
        pciDevice_ = nullptr;
    }

    super::stop(provider);
}

IOReturn UniCamIPUService::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) {
    (void)whatDevice;
    if (powerStateOrdinal == kPowerStateOff) {
        sleepSuspended_ = true;
    }
    if (!commandGate_) {
        if (powerStateOrdinal != kPowerStateOff) {
            sleepSuspended_ = false;
        }
        return kIOPMAckImplied;
    }
    GatedIpuRequest req = {
        (powerStateOrdinal == kPowerStateOff) ? kOpSleep : kOpWake,
        0U, 0U, nullptr, nullptr, nullptr
    };
    commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
    return kIOPMAckImplied;
}

void UniCamIPUService::interruptActionTrampoline(OSObject *owner,
                                                 IOInterruptEventSource *sender,
                                                 int count) {
    (void)sender;
    UniCamIPUService *self = OSDynamicCast(UniCamIPUService, owner);
    if (self) {
        self->onInterrupt(count);
    }
}

void UniCamIPUService::onInterrupt(int count) {
    (void)count;
    if (!hal_ || stopping_ || sleepSuspended_) {
        return;
    }
    bool frameReady = false;
    hal_->pollOrHandleIrq(true, &frameReady);
    if (frameReady && commandGate_) {
        commandGate_->commandWakeup(&frameWaitChannel_, false);
    }
}

IOReturn UniCamIPUService::ensureRingBufferLocked() {
    if (!hal_) {
        return kIOReturnNotReady;
    }
    if (!ringDesc_) {
        IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryKernelUserShared,
            UNICAM_IPU_RING_TOTAL_BYTES,
            0x0000007FFFFFF000ULL);
        if (!buf) {
            IOLog("UniCamIPU: Failed to allocate %u byte Shared Ring Buffer\n",
                  UNICAM_IPU_RING_TOTAL_BYTES);
            return kIOReturnNoMemory;
        }
        ringDesc_ = buf;
        if (ringDesc_->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            ringDesc_->release();
            ringDesc_ = nullptr;
            return kIOReturnNoMemory;
        }
    }
    return hal_->attachRingBuffer(ringDesc_);
}

IOReturn UniCamIPUService::waitNextFrameLocked(uint32_t timeoutMs, UniCamIpuFrameEvent *outEvent) {
    if (!hal_ || !outEvent || !commandGate_) {
        return kIOReturnBadArgument;
    }
    if (sleepSuspended_) {
        return kIOReturnNotReady;
    }
    if (!hal_->isStreaming()) {
        return kIOReturnNotReady;
    }

    if (hal_->popCompletedFrame(outEvent)) {
        return kIOReturnSuccess;
    }

    bool frameReady = false;
    hal_->pollOrHandleIrq(false, &frameReady);
    if (hal_->popCompletedFrame(outEvent)) {
        return kIOReturnSuccess;
    }

    const uint64_t deadlineNs = currentUptimeNs() + static_cast<uint64_t>(timeoutMs) * 1000000ULL;
    while (!stopping_ && !sleepSuspended_ && hal_->isStreaming()) {
        const uint64_t nowNs = currentUptimeNs();
        if (nowNs >= deadlineNs) {
            return kIOReturnTimeout;
        }

        const uint64_t remNs = deadlineNs - nowNs;
        const uint64_t sliceNs = (remNs > 5000000ULL) ? 5000000ULL : remNs;
        AbsoluteTime wakeAbs = 0ULL;
        uint64_t wakeTicks = 0ULL;
        nanoseconds_to_absolutetime(nowNs + sliceNs, &wakeTicks);
        wakeAbs = static_cast<AbsoluteTime>(wakeTicks);

        const IOReturn sleepRes = commandGate_->commandSleep(&frameWaitChannel_, wakeAbs, THREAD_ABORTSAFE);
        if (stopping_) {
            return kIOReturnAborted;
        }
        if (sleepRes == THREAD_INTERRUPTED || sleepRes == THREAD_RESTART) {
            return kIOReturnAborted;
        }

        hal_->pollOrHandleIrq(false, &frameReady);
        if (hal_->popCompletedFrame(outEvent)) {
            return kIOReturnSuccess;
        }
    }

    if (!hal_->isStreaming()) {
        hwStreaming_ = false;
    }
    return kIOReturnNotReady;
}

IOReturn UniCamIPUService::gatedActionTrampoline(OSObject *owner,
                                                 void *arg0,
                                                 void *arg1,
                                                 void *arg2,
                                                 void *arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;

    UniCamIPUService *self = OSDynamicCast(UniCamIPUService, owner);
    const auto *req = static_cast<const GatedIpuRequest *>(arg0);
    if (!self || !req || !self->hal_) {
        return kIOReturnBadArgument;
    }
    if (req->op != kOpTeardown && self->stopping_) {
        return kIOReturnNotReady;
    }

    switch (req->op) {
        case kOpGetInfo: {
            self->ensurePciD0AndBars();
            auto *hwInfo = static_cast<UniCamIpuHwInfo *>(req->outPtr);
            self->hal_->fillHwInfo(hwInfo);
            if (self->pciDevice_) {
                hwInfo->pciCommand = self->pciDevice_->configRead16(kIOPCIConfigCommand);
                hwInfo->pciPmcsr = self->pmCapOff_
                    ? self->pciDevice_->configRead16(static_cast<UInt8>(self->pmCapOff_ + 4U))
                    : 0U;
            }
            return kIOReturnSuccess;
        }

        case kOpGetDiag: {
            self->ensurePciD0AndBars();
            auto *diag = static_cast<UniCamIpuDiag *>(req->outPtr);
            self->hal_->fillDiag(diag);
            if (self->pciDevice_ && diag) {
                diag->pciBar0Lo = self->pciDevice_->configRead32(kIOPCIConfigBaseAddress0);
                diag->pciBar0Hi = self->pciDevice_->configRead32(kIOPCIConfigBaseAddress1);
                diag->bootBar0Lo = self->bootBar0Lo_;
                diag->bootBar0Hi = self->bootBar0Hi_;
            }
            return kIOReturnSuccess;
        }

        case kOpBootFirmware:
            if (self->sleepSuspended_) return kIOReturnNotReady;
            self->ensurePciD0AndBars();
            return self->hal_->bootFirmware(
                static_cast<const uint8_t *>(req->inPtr),
                req->u0,
                req->u1,
                static_cast<UniCamIpuBootReport *>(req->outPtr));

        case kOpBootEmbeddedFirmware:
            if (self->sleepSuspended_) return kIOReturnNotReady;
            self->ensurePciD0AndBars();
            return self->hal_->bootEmbeddedFirmware(
                req->u1,
                static_cast<UniCamIpuBootReport *>(req->outPtr));

        case kOpShutdownFirmware:
            return self->hal_->shutdownFirmware();

        case kOpStartStream: {
            if (self->sleepSuspended_) return kIOReturnNotReady;
            self->ensurePciD0AndBars();
            self->captureMsiStateIfEnabled();
            IOReturn ret = self->ensureRingBufferLocked();
            if (ret != kIOReturnSuccess) return ret;
            ret = self->hal_->startStream(
                *static_cast<const UniCamIpuStreamConfig *>(req->inPtr),
                static_cast<UniCamIpuStreamStatus *>(req->outPtr));
            self->hwStreaming_ = (ret == kIOReturnSuccess);
            if (ret == kIOReturnSuccess) {
                self->activeOwner_ = req->client;
            }
            return ret;
        }

        case kOpWaitNextFrame:
            return self->waitNextFrameLocked(req->u0, static_cast<UniCamIpuFrameEvent *>(req->outPtr));

        case kOpRequeueSlot:
            return self->hal_->requeueSlot(req->u0);

        case kOpStopStream: {
            self->hwStreaming_ = false;
            const IOReturn ret = self->hal_->stopStream();
            if (self->activeOwner_ == req->client || req->client == nullptr) {
                self->activeOwner_ = nullptr;
            }
            self->commandGate_->commandWakeup(&self->frameWaitChannel_, false);
            return ret;
        }

        case kOpRetainRingDesc: {
            const IOReturn ret = self->ensureRingBufferLocked();
            if (ret != kIOReturnSuccess || !self->ringDesc_) {
                return (ret != kIOReturnSuccess) ? ret : kIOReturnNoMemory;
            }
            self->ringDesc_->retain();
            *static_cast<IOBufferMemoryDescriptor **>(req->outPtr) = self->ringDesc_;
            return kIOReturnSuccess;
        }

        case kOpClientDisconnect:
            if (req->client && self->activeOwner_ == req->client) {
                IOLog("UniCamIPU: Owner client closed while active -> stopping ISYS stream\n");
                self->hwStreaming_ = false;
                self->hal_->stopStream();
                self->activeOwner_ = nullptr;
                self->commandGate_->commandWakeup(&self->frameWaitChannel_, false);
            }
            return kIOReturnSuccess;

        case kOpSleep:
            self->sleepSuspended_ = true;
            self->hwStreaming_ = false;
            self->captureMsiStateIfEnabled();
            self->commandGate_->commandWakeup(&self->frameWaitChannel_, false);
            return self->hal_->handleSleep();

        case kOpWake:
            self->sleepSuspended_ = false;
            self->ensurePciD0AndBars();
            return self->hal_->handleWake();

        case kOpTeardown:
            self->stopping_ = true;
            self->sleepSuspended_ = true;
            self->hwStreaming_ = false;
            if (self->irqSource_) {
                self->irqSource_->disable();
            }
            self->commandGate_->commandWakeup(&self->frameWaitChannel_, false);
            self->hal_->cleanup();
            return kIOReturnSuccess;

        default:
            return kIOReturnUnsupported;
    }
}

IOReturn UniCamIPUService::getInfo(UniCamIpuHwInfo *outInfo) {
    if (!commandGate_ || !outInfo) return kIOReturnBadArgument;
    GatedIpuRequest req = { kOpGetInfo, 0U, 0U, nullptr, outInfo, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::getDiagnostics(UniCamIpuDiag *outDiag) {
    if (!commandGate_ || !outDiag) return kIOReturnBadArgument;
    GatedIpuRequest req = { kOpGetDiag, 0U, 0U, nullptr, outDiag, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::bootFirmware(const uint8_t *cpdData,
                                        uint32_t cpdSize,
                                        uint32_t requestedDmaMode,
                                        UniCamIpuBootReport *outReport) {
    if (!commandGate_ || !outReport) {
        return kIOReturnBadArgument;
    }
    if (cpdData == nullptr && cpdSize == 0U) {
        GatedIpuRequest req = { kOpBootEmbeddedFirmware, 0U, requestedDmaMode, nullptr, outReport, nullptr };
        return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
    }
    if (!cpdData || cpdSize == 0U || cpdSize > kMaxFirmwareBytes) {
        return kIOReturnBadArgument;
    }
    GatedIpuRequest req = { kOpBootFirmware, cpdSize, requestedDmaMode, cpdData, outReport, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::shutdownFirmware() {
    if (!commandGate_) return kIOReturnNotReady;
    GatedIpuRequest req = { kOpShutdownFirmware, 0U, 0U, nullptr, nullptr, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::startStream(const UniCamIpuStreamConfig *cfg,
                                       UniCamIpuStreamStatus *outStatus,
                                       UniCamIPUUserClient *owner) {
    if (!commandGate_ || !cfg || !outStatus) return kIOReturnBadArgument;
    GatedIpuRequest req = { kOpStartStream, 0U, 0U, cfg, outStatus, owner };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::waitNextFrame(uint32_t timeoutMs, UniCamIpuFrameEvent *outEvent) {
    if (!commandGate_ || !outEvent) return kIOReturnBadArgument;
    GatedIpuRequest req = { kOpWaitNextFrame, timeoutMs, 0U, nullptr, outEvent, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::requeueSlot(uint32_t slotIndex) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedIpuRequest req = { kOpRequeueSlot, slotIndex, 0U, nullptr, nullptr, nullptr };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOReturn UniCamIPUService::stopStream(UniCamIPUUserClient *owner) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedIpuRequest req = { kOpStopStream, 0U, 0U, nullptr, nullptr, owner };
    return commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

IOBufferMemoryDescriptor *UniCamIPUService::retainRingBufferDescriptor() {
    if (!commandGate_) return nullptr;
    IOBufferMemoryDescriptor *desc = nullptr;
    GatedIpuRequest req = { kOpRetainRingDesc, 0U, 0U, nullptr, &desc, nullptr };
    if (commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req) != kIOReturnSuccess) {
        return nullptr;
    }
    return desc;
}

void UniCamIPUService::handleClientDisconnect(UniCamIPUUserClient *client) {
    if (!commandGate_ || !client) return;
    GatedIpuRequest req = { kOpClientDisconnect, 0U, 0U, nullptr, nullptr, client };
    commandGate_->runAction(&UniCamIPUService::gatedActionTrampoline, &req);
}

void UniCamIPUService::resetIsp3A() {
    if (!ispLock_ || !softIsp_ || stopping_) return;
    IOLockLock(ispLock_);
    if (!stopping_) {
        softIsp_->reset3A();
    }
    IOLockUnlock(ispLock_);
}

void UniCamIPUService::generateTestPatternYuy2(uint8_t *dstYuy2,
                                               uint32_t dstWidth,
                                               uint32_t dstHeight,
                                               uint32_t frameCounter) {
    if (!ispLock_ || !softIsp_ || stopping_) return;
    IOLockLock(ispLock_);
    if (!stopping_) {
        softIsp_->generateTestPatternYuy2(dstYuy2, dstWidth, dstHeight, frameCounter);
    }
    IOLockUnlock(ispLock_);
}

IOReturn UniCamIPUService::processSlotToYuy2(uint32_t slotIndex,
                                             uint32_t bayerOrder,
                                             uint32_t rotationDeg,
                                             uint8_t *dstYuy2,
                                             uint32_t dstWidth,
                                             uint32_t dstHeight,
                                             uint32_t dstCapacityBytes,
                                             UniCamIspFrameStats *outStats) {
    if (!ispLock_ || !softIsp_ || !dstYuy2 || slotIndex >= UNICAM_IPU_RING_SLOTS) {
        return kIOReturnBadArgument;
    }

    IOLockLock(ispLock_);
    if (stopping_ || !ringDesc_) {
        IOLockUnlock(ispLock_);
        return kIOReturnNotReady;
    }

    const auto *ringBase = static_cast<const uint8_t *>(ringDesc_->getBytesNoCopy());
    const auto *hdr = reinterpret_cast<const UniCamIpuRingHeader *>(ringBase);
    if (!hdr || hdr->magic != UNICAM_IPU_RING_MAGIC || hdr->slotOffsets[slotIndex] == 0U) {
        IOLockUnlock(ispLock_);
        return kIOReturnNotReady;
    }

    const uint32_t srcW = hdr->activeWidth ? hdr->activeWidth : 1280U;
    const uint32_t srcH = hdr->activeHeight ? hdr->activeHeight : 800U;
    const uint32_t srcStride = hdr->activeStride ? hdr->activeStride : 1600U;
    const uint32_t pixFmt = hdr->pixelFormat;
    const uint8_t *srcSlot = ringBase + hdr->slotOffsets[slotIndex];

    const IOReturn ret = softIsp_->processRawToYuy2(
        srcSlot,
        srcW,
        srcH,
        srcStride,
        pixFmt,
        bayerOrder,
        rotationDeg,
        dstYuy2,
        dstWidth,
        dstHeight,
        dstCapacityBytes,
        outStats);
    IOLockUnlock(ispLock_);
    return ret;
}
