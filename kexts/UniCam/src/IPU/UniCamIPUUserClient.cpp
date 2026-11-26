#include "UniCamIPUUserClient.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
extern task_t kernel_task;
}

#define super IOUserClient
OSDefineMetaClassAndStructors(UniCamIPUUserClient, IOUserClient)

namespace {
constexpr uint64_t kMaxUserFwBytes = 16ULL * 1024ULL * 1024ULL;
} // namespace

const IOExternalMethodDispatch UniCamIPUUserClient::sMethods[kUniCamIpuMethodCount] = {
    // kUniCamIpuMethodGetInfo
    { &UniCamIPUUserClient::sGetInfo,          0, 0, 0, sizeof(UniCamIpuHwInfo) },
    // kUniCamIpuMethodGetDiag
    { &UniCamIPUUserClient::sGetDiag,          0, 0, 0, sizeof(UniCamIpuDiag) },
    // kUniCamIpuMethodBootFirmware (userAddr, userSize, dmaMode)
    { &UniCamIPUUserClient::sBootFirmware,     3, 0, 0, sizeof(UniCamIpuBootReport) },
    // kUniCamIpuMethodShutdownFirmware
    { &UniCamIPUUserClient::sShutdownFirmware, 0, 0, 0, 0 },
    // kUniCamIpuMethodStartStream
    { &UniCamIPUUserClient::sStartStream,      0, sizeof(UniCamIpuStreamConfig), 0, sizeof(UniCamIpuStreamStatus) },
    // kUniCamIpuMethodWaitNextFrame
    { &UniCamIPUUserClient::sWaitNextFrame,    1, 0, 0, sizeof(UniCamIpuFrameEvent) },
    // kUniCamIpuMethodRequeueSlot
    { &UniCamIPUUserClient::sRequeueSlot,      1, 0, 0, 0 },
    // kUniCamIpuMethodStopStream
    { &UniCamIPUUserClient::sStopStream,       0, 0, 0, 0 },
    // kUniCamIpuMethodProcessSlotYuy2 (slotIdx, bayerOrder, rotDeg, dstW, dstH, userDstAddr)
    { &UniCamIPUUserClient::sProcessSlotYuy2,  6, 0, 0, sizeof(UniCamIspFrameStats) },
};

bool UniCamIPUUserClient::initWithTask(task_t owningTask,
                                       void *securityToken,
                                       UInt32 type,
                                       OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityToken, type, properties)) {
        return false;
    }
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        IOLog("UniCamIPU: UserClient open denied (requires root/administrator privileges)\n");
        return false;
    }
    owningTask_ = owningTask;
    service_ = nullptr;
    return true;
}

bool UniCamIPUUserClient::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    service_ = OSDynamicCast(UniCamIPUService, provider);
    if (!service_) {
        return false;
    }
    return true;
}

IOReturn UniCamIPUUserClient::clientClose() {
    if (service_) {
        service_->handleClientDisconnect(this);
    }
    terminate();
    return kIOReturnSuccess;
}

void UniCamIPUUserClient::stop(IOService *provider) {
    service_ = nullptr;
    super::stop(provider);
}

IOReturn UniCamIPUUserClient::externalMethod(uint32_t selector,
                                             IOExternalMethodArguments *arguments,
                                             IOExternalMethodDispatch *dispatch,
                                             OSObject *target,
                                             void *reference) {
    if (selector >= kUniCamIpuMethodCount) {
        return kIOReturnUnsupported;
    }
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target = this;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn UniCamIPUUserClient::clientMemoryForType(UInt32 type,
                                                  IOOptionBits *options,
                                                  IOMemoryDescriptor **memory) {
    if (!service_ || !options || !memory) {
        return kIOReturnBadArgument;
    }
    if (type != kUniCamIpuMemRingBuffer) {
        return kIOReturnUnsupported;
    }

    IOBufferMemoryDescriptor *ringDesc = service_->retainRingBufferDescriptor();
    if (!ringDesc) {
        return kIOReturnNoMemory;
    }

    *options = kIOMapAnywhere;
    *memory = ringDesc;
    return kIOReturnSuccess;
}

IOReturn UniCamIPUUserClient::sGetInfo(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return uc->service_->getInfo(static_cast<UniCamIpuHwInfo *>(args->structureOutput));
}

IOReturn UniCamIPUUserClient::sGetDiag(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return uc->service_->getDiagnostics(static_cast<UniCamIpuDiag *>(args->structureOutput));
}

IOReturn UniCamIPUUserClient::sBootFirmware(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !uc->owningTask_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }

    const mach_vm_address_t userAddr = static_cast<mach_vm_address_t>(args->scalarInput[0]);
    const mach_vm_size_t userSize = static_cast<mach_vm_size_t>(args->scalarInput[1]);
    const uint32_t requestedDmaMode = static_cast<uint32_t>(args->scalarInput[2]);
    UniCamIpuBootReport *rep = static_cast<UniCamIpuBootReport *>(args->structureOutput);

    if (userAddr == 0ULL && userSize == 0ULL) {
        const IOReturn ret = uc->service_->bootFirmware(nullptr, 0U, requestedDmaMode, rep);
        if (ret != kIOReturnSuccess && rep->statusRet == kIOReturnSuccess) {
            rep->statusRet = ret;
        }
        return (ret == kIOReturnBadArgument) ? ret : kIOReturnSuccess;
    }

    if (userAddr == 0ULL || userSize == 0ULL || userSize > kMaxUserFwBytes) {
        return kIOReturnBadArgument;
    }

    IOMemoryDescriptor *userDesc = IOMemoryDescriptor::withAddressRange(
        userAddr, userSize, kIODirectionOut, uc->owningTask_);
    if (!userDesc) {
        return kIOReturnBadArgument;
    }
    if (userDesc->prepare(kIODirectionOut) != kIOReturnSuccess) {
        userDesc->release();
        return kIOReturnVMError;
    }

    IOBufferMemoryDescriptor *kernCopy = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, static_cast<vm_size_t>(userSize));
    if (!kernCopy) {
        userDesc->complete();
        userDesc->release();
        return kIOReturnNoMemory;
    }

    void *dst = kernCopy->getBytesNoCopy();
    const IOByteCount copied = userDesc->readBytes(0, dst, static_cast<IOByteCount>(userSize));
    userDesc->complete();
    userDesc->release();

    if (copied != static_cast<IOByteCount>(userSize)) {
        kernCopy->release();
        return kIOReturnVMError;
    }

    const IOReturn ret = uc->service_->bootFirmware(
        static_cast<const uint8_t *>(dst),
        static_cast<uint32_t>(userSize),
        requestedDmaMode,
        rep);
    kernCopy->release();
    if (ret != kIOReturnSuccess && rep->statusRet == kIOReturnSuccess) {
        rep->statusRet = ret;
    }
    return (ret == kIOReturnBadArgument) ? ret : kIOReturnSuccess;
}

IOReturn UniCamIPUUserClient::sShutdownFirmware(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    (void)args;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->shutdownFirmware();
}

IOReturn UniCamIPUUserClient::sStartStream(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !args->structureInput || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return uc->service_->startStream(
        static_cast<const UniCamIpuStreamConfig *>(args->structureInput),
        static_cast<UniCamIpuStreamStatus *>(args->structureOutput),
        uc);
}

IOReturn UniCamIPUUserClient::sWaitNextFrame(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    const uint32_t timeoutMs = static_cast<uint32_t>(args->scalarInput[0]);
    return uc->service_->waitNextFrame(timeoutMs, static_cast<UniCamIpuFrameEvent *>(args->structureOutput));
}

IOReturn UniCamIPUUserClient::sRequeueSlot(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    const uint32_t slotIndex = static_cast<uint32_t>(args->scalarInput[0]);
    return uc->service_->requeueSlot(slotIndex);
}

IOReturn UniCamIPUUserClient::sStopStream(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    (void)args;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->stopStream(uc);
}

IOReturn UniCamIPUUserClient::sProcessSlotYuy2(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamIPUUserClient *uc = OSDynamicCast(UniCamIPUUserClient, target);
    if (!uc || !uc->service_ || !uc->owningTask_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }

    const uint32_t slotIndex  = static_cast<uint32_t>(args->scalarInput[0]);
    const uint32_t bayerOrder = static_cast<uint32_t>(args->scalarInput[1]);
    const uint32_t rotDeg     = static_cast<uint32_t>(args->scalarInput[2]);
    const uint32_t dstW       = static_cast<uint32_t>(args->scalarInput[3]);
    const uint32_t dstH       = static_cast<uint32_t>(args->scalarInput[4]);
    const mach_vm_address_t userDstAddr = static_cast<mach_vm_address_t>(args->scalarInput[5]);

    if (dstW == 0U || dstH == 0U || dstW > 1280U || dstH > 800U || (dstW & 1U) != 0U || userDstAddr == 0ULL) {
        return kIOReturnBadArgument;
    }

    const uint32_t outBytes = dstW * dstH * 2U;
    IOBufferMemoryDescriptor *kernOut = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, outBytes);
    if (!kernOut) {
        return kIOReturnNoMemory;
    }

    UniCamIspFrameStats stats {};
    const IOReturn ispRet = uc->service_->processSlotToYuy2(
        slotIndex,
        bayerOrder,
        rotDeg,
        static_cast<uint8_t *>(kernOut->getBytesNoCopy()),
        dstW,
        dstH,
        outBytes,
        &stats);
    if (ispRet != kIOReturnSuccess) {
        kernOut->release();
        return ispRet;
    }

    IOMemoryDescriptor *userDesc = IOMemoryDescriptor::withAddressRange(
        userDstAddr, outBytes, kIODirectionIn, uc->owningTask_);
    if (!userDesc) {
        kernOut->release();
        return kIOReturnBadArgument;
    }
    IOReturn copyRet = userDesc->prepare(kIODirectionIn);
    if (copyRet == kIOReturnSuccess) {
        const IOByteCount written = userDesc->writeBytes(0, kernOut->getBytesNoCopy(), outBytes);
        userDesc->complete();
        if (written != static_cast<IOByteCount>(outBytes)) {
            copyRet = kIOReturnVMError;
        }
    }
    userDesc->release();
    kernOut->release();
    if (copyRet != kIOReturnSuccess) {
        return copyRet;
    }

    *static_cast<UniCamIspFrameStats *>(args->structureOutput) = stats;
    return kIOReturnSuccess;
}
