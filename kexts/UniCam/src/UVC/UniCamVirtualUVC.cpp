#include "UniCamVirtualUVC.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSet.h>
#include <libkern/c++/OSString.h>
#include <string.h>

extern "C" {
#include <kern/clock.h>
#include <kern/sched_prim.h>
}

namespace {

constexpr uint16_t kUvcVendorId = 0x0C45;   // Sonix / SunplusIT
constexpr uint16_t kUvcProductId = 0x6723;  // Integrated Webcam HD (standard UVC match, skips Apple T2/Support checks)
constexpr uint16_t kUvcBcdDevice = 0x0100;
constexpr uint32_t kUvcLocationId = 0x14600000U;
constexpr tUSBHostDeviceAddress kUvcDeviceAddress = 7;

constexpr uint8_t kUvcEpInterruptIn = 0x83;
constexpr uint8_t kUvcEpIsochIn = 0x81;
constexpr uint16_t kUvcMaxPayloadSize = 8192;

// UVC Class Request codes
constexpr uint8_t kUvcReqSetCur = 0x01;
constexpr uint8_t kUvcReqGetCur = 0x81;
constexpr uint8_t kUvcReqGetMin = 0x82;
constexpr uint8_t kUvcReqGetMax = 0x83;
constexpr uint8_t kUvcReqGetRes = 0x84;
constexpr uint8_t kUvcReqGetLen = 0x85;
constexpr uint8_t kUvcReqGetInfo = 0x86;
constexpr uint8_t kUvcReqGetDef = 0x87;

// VideoStreaming selectors
constexpr uint8_t kVsProbeControl = 0x01;
constexpr uint8_t kVsCommitControl = 0x02;

// VideoControl selectors
constexpr uint8_t kVcRequestErrorCodeControl = 0x02;
constexpr uint8_t kCtAeModeControl = 0x02;
constexpr uint8_t kCtExposureTimeAbsControl = 0x04;
constexpr uint8_t kPuBacklightCompControl = 0x01;
constexpr uint8_t kPuBrightnessControl = 0x02;
constexpr uint8_t kPuContrastControl = 0x03;
constexpr uint8_t kPuGainControl = 0x04;
constexpr uint8_t kPuPowerLineFreqControl = 0x05;
constexpr uint8_t kPuHueControl = 0x06;
constexpr uint8_t kPuSaturationControl = 0x07;
constexpr uint8_t kPuSharpnessControl = 0x08;
constexpr uint8_t kPuGammaControl = 0x09;
constexpr uint8_t kPuWbTempControl = 0x0A;
constexpr uint8_t kPuWbAutoControl = 0x0B;
constexpr uint8_t kPuWbComponentControl = 0x0C;
constexpr uint8_t kPuWbComponentAutoControl = 0x0D;
constexpr uint8_t kPuHueAutoControl = 0x10;

// Standard USB descriptor structs matching our exact UVC 1.1 hierarchy (SuperSpeed USB 3.0)
alignas(4) const uint8_t kDeviceDescriptorBytes[18] = {
    0x12,       // bLength
    0x01,       // bDescriptorType (DEVICE)
    0x00, 0x03, // bcdUSB 3.00
    0xEF,       // bDeviceClass (Miscellaneous)
    0x02,       // bDeviceSubClass (Common Class)
    0x01,       // bDeviceProtocol (Interface Association)
    0x09,       // bMaxPacketSize0 (2^9 = 512 in SuperSpeed)
    0x45, 0x0C, // idVendor = 0x0C45
    0x23, 0x67, // idProduct = 0x6723
    0x00, 0x01, // bcdDevice = 1.00
    0x01,       // iManufacturer
    0x02,       // iProduct
    0x03,       // iSerialNumber
    0x01        // bNumConfigurations
};

// Offsets inside kConfigDescriptorBytes for direct interface/endpoint descriptor lookup
constexpr uint32_t kOffsetIface0Alt0 = 9 + 8;                              // 17
constexpr uint32_t kOffsetEp83Desc = kOffsetIface0Alt0 + 9 + 13 + 18 + 11 + 9; // 77
constexpr uint32_t kOffsetEp83SsDesc = kOffsetEp83Desc + 7;                // 84
constexpr uint32_t kOffsetIface1Alt0 = kOffsetEp83SsDesc + 6 + 5;          // 95
constexpr uint32_t kOffsetIface1Alt1 = kOffsetIface1Alt0 + 9 + 14 + 27 + 30 + 30 + 30 + 6; // 241
constexpr uint32_t kOffsetEp81Desc = kOffsetIface1Alt1 + 9;                // 250
constexpr uint32_t kOffsetEp81SsDesc = kOffsetEp81Desc + 7;                // 257
constexpr uint32_t kTotalConfigLen = kOffsetEp81SsDesc + 6;                // 263

alignas(4) const uint8_t kConfigDescriptorBytes[kTotalConfigLen] = {
    // [0..8] Configuration Descriptor (wTotalLength = 263 = 0x0107)
    0x09, 0x02,
    static_cast<uint8_t>(kTotalConfigLen & 0xFF),
    static_cast<uint8_t>((kTotalConfigLen >> 8) & 0xFF),
    0x02,       // bNumInterfaces
    0x01,       // bConfigurationValue
    0x00,       // iConfiguration
    0x80,       // bmAttributes (Bus Powered)
    0xFA,       // bMaxPower (500 mA / 2mA steps in SS=50*8=400mA compatible)

    // [9..16] Interface Association Descriptor (Video Function)
    0x08, 0x0B,
    0x00,       // bFirstInterface
    0x02,       // bInterfaceCount
    0x0E,       // bFunctionClass (CC_VIDEO)
    0x03,       // bFunctionSubClass (SC_VIDEO_INTERFACE_COLLECTION)
    0x00,       // bFunctionProtocol
    0x02,       // iFunction ("Integrated Webcam HD")

    // [17..25] Interface 0 Alt 0: Video Control
    0x09, 0x04,
    0x00,       // bInterfaceNumber
    0x00,       // bAlternateSetting
    0x01,       // bNumEndpoints (Interrupt IN 0x83)
    0x0E,       // bInterfaceClass (CC_VIDEO)
    0x01,       // bInterfaceSubClass (SC_VIDEOCONTROL)
    0x00,       // bInterfaceProtocol
    0x02,       // iInterface

    // [26..38] Class-specific VC Interface Header Descriptor (13 bytes)
    0x0D, 0x24, 0x01,
    0x10, 0x01, // bcdUVC = 1.10
    0x33, 0x00, // wTotalLength = 13 + 18 + 11 + 9 = 51 (0x0033)
    0x00, 0x6C, 0xDC, 0x02, // dwClockFrequency = 48,000,000 Hz (0x02DC6C00)
    0x01,       // bInCollection = 1
    0x01,       // baInterfaceNr(1) = 1

    // [39..56] Input Terminal Descriptor (ITT_CAMERA, 18 bytes)
    0x12, 0x24, 0x02,
    0x01,       // bTerminalID = 1
    0x01, 0x02, // wTerminalType = 0x0201 (ITT_CAMERA)
    0x00,       // bAssocTerminal
    0x00,       // iTerminal
    0x00, 0x00, // wObjectiveFocalLengthMin
    0x00, 0x00, // wObjectiveFocalLengthMax
    0x00, 0x00, // wOcularFocalLength
    0x03,       // bControlSize = 3
    0x0A, 0x00, 0x00, // bmControls: AE Mode (bit 1) + Exposure Time Abs (bit 3)

    // [57..67] Processing Unit Descriptor (11 bytes)
    0x0B, 0x24, 0x05,
    0x02,       // bUnitID = 2
    0x01,       // bSourceID = 1
    0x00, 0x00, // wMaxMultiplier
    0x02,       // bControlSize = 2
    0x7F, 0x16, // bmControls: Brightness/Contrast/Hue/Sat/Sharp/Gamma/WB/Gain/PLF/WBAuto
    0x00,       // iProcessing

    // [68..76] Output Terminal Descriptor (TT_STREAMING, 9 bytes)
    0x09, 0x24, 0x03,
    0x03,       // bTerminalID = 3
    0x01, 0x01, // wTerminalType = 0x0101 (TT_STREAMING)
    0x00,       // bAssocTerminal
    0x02,       // bSourceID = 2
    0x00,       // iTerminal

    // [77..83] Endpoint 0x83 (Interrupt IN)
    0x07, 0x05,
    kUvcEpInterruptIn,
    0x03,       // bmAttributes (Interrupt)
    0x10, 0x00, // wMaxPacketSize = 16
    0x08,       // bInterval

    // [84..89] SuperSpeed Endpoint Companion for 0x83
    0x06, 0x30,
    0x00,       // bMaxBurst = 0
    0x00,       // bmAttributes = 0
    0x10, 0x00, // wBytesPerInterval = 16

    // [90..94] Class-specific VC Interrupt Endpoint Descriptor (5 bytes)
    0x05, 0x25, 0x03,
    0x10, 0x00, // wMaxTransferSize = 16

    // [95..103] Interface 1 Alt 0: Video Streaming (Zero-Bandwidth Idle)
    0x09, 0x04,
    0x01,       // bInterfaceNumber
    0x00,       // bAlternateSetting
    0x00,       // bNumEndpoints
    0x0E,       // bInterfaceClass (CC_VIDEO)
    0x02,       // bInterfaceSubClass (SC_VIDEOSTREAMING)
    0x00,       // bInterfaceProtocol
    0x00,       // iInterface

    // [104..117] Class-specific VS Input Header Descriptor (14 bytes)
    // wTotalLength = 14 + 27 + 30 + 30 + 30 + 6 = 137 (0x0089)
    0x0E, 0x24, 0x01,
    0x01,       // bNumFormats = 1
    0x89, 0x00, // wTotalLength = 137
    kUvcEpIsochIn,
    0x00,       // bmInfo
    0x03,       // bTerminalLink = 3
    0x00,       // bStillCaptureMethod
    0x00,       // bTriggerSupport
    0x00,       // bTriggerUsage
    0x01,       // bControlSize
    0x00,       // bmaControls

    // [118..144] VS Format Uncompressed Descriptor (YUY2, 27 bytes)
    0x1B, 0x24, 0x04,
    0x01,       // bFormatIndex = 1
    0x03,       // bNumFrameDescriptors = 3
    // guidFormat = YUY2 {32595559-0000-0010-8000-00AA00389B71}
    'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    0x10,       // bBitsPerPixel = 16
    0x01,       // bDefaultFrameIndex = 1 (1280x720)
    0x00,       // bAspectRatioX
    0x00,       // bAspectRatioY
    0x00,       // bmInterlaceFlags
    0x00,       // bCopyProtect

    // [145..174] VS Frame Uncompressed 1: 1280x720 @ 30 fps (30 bytes)
    0x1E, 0x24, 0x05,
    0x01,       // bFrameIndex = 1
    0x00,       // bmCapabilities
    0x00, 0x05, // wWidth = 1280 (0x0500)
    0xD0, 0x02, // wHeight = 720 (0x02D0)
    0x00, 0x00, 0x2F, 0x0D, // dwMinBitRate = 1280*720*16*15 = 221184000 (0x0D2F0000)
    0x00, 0x00, 0x5E, 0x1A, // dwMaxBitRate = 1280*720*16*30 = 442368000 = 0x1A5E0000
    0x00, 0x20, 0x1C, 0x00, // dwMaxVideoFrameBufferSize = 1843200 = 0x001C2000
    0x15, 0x16, 0x05, 0x00, // dwDefaultFrameInterval = 333333 (0x00051615)
    0x01,       // bFrameIntervalType = 1 (discrete)
    0x15, 0x16, 0x05, 0x00, // dwFrameInterval[0] = 333333

    // [175..204] VS Frame Uncompressed 2: 1280x800 @ 30 fps (30 bytes)
    0x1E, 0x24, 0x05,
    0x02,       // bFrameIndex = 2
    0x00,       // bmCapabilities
    0x00, 0x05, // wWidth = 1280 (0x0500)
    0x20, 0x03, // wHeight = 800 (0x0320)
    0x00, 0x40, 0xA6, 0x0E, // dwMinBitRate = 245760000 (0x0EA60000)
    0x00, 0x00, 0x4C, 0x1D, // dwMaxBitRate = 491520000 (0x1D4C0000)
    0x00, 0x40, 0x1F, 0x00, // dwMaxVideoFrameBufferSize = 2048000 (0x001F4000)
    0x15, 0x16, 0x05, 0x00, // dwDefaultFrameInterval = 333333
    0x01,       // bFrameIntervalType = 1
    0x15, 0x16, 0x05, 0x00, // dwFrameInterval[0] = 333333

    // [205..234] VS Frame Uncompressed 3: 640x480 @ 30 fps (30 bytes)
    0x1E, 0x24, 0x05,
    0x03,       // bFrameIndex = 3
    0x00,       // bmCapabilities
    0x80, 0x02, // wWidth = 640 (0x0280)
    0xE0, 0x01, // wHeight = 480 (0x01E0)
    0x00, 0x80, 0x65, 0x04, // dwMinBitRate = 73728000 (0x04650000)
    0x00, 0x00, 0xCA, 0x08, // dwMaxBitRate = 147456000 (0x08CA0000)
    0x00, 0x60, 0x09, 0x00, // dwMaxVideoFrameBufferSize = 614400 (0x00096000)
    0x15, 0x16, 0x05, 0x00, // dwDefaultFrameInterval = 333333
    0x01,       // bFrameIntervalType = 1
    0x15, 0x16, 0x05, 0x00, // dwFrameInterval[0] = 333333

    // [235..240] VS Color Matching Descriptor (6 bytes)
    0x06, 0x24, 0x0D,
    0x01,       // bColorPrimaries = BT.709 / sRGB
    0x01,       // bTransferCharacteristics = BT.709
    0x04,       // bMatrixCoefficients = SMPTE 170M (BT.601)

    // [241..249] Interface 1 Alt 1: Video Streaming (Active Isochronous IN)
    0x09, 0x04,
    0x01,       // bInterfaceNumber
    0x01,       // bAlternateSetting
    0x01,       // bNumEndpoints
    0x0E,       // bInterfaceClass (CC_VIDEO)
    0x02,       // bInterfaceSubClass (SC_VIDEOSTREAMING)
    0x00,       // bInterfaceProtocol
    0x00,       // iInterface

    // [250..256] Endpoint 0x81 (Isochronous Async IN)
    0x07, 0x05,
    kUvcEpIsochIn,
    0x05,       // bmAttributes (Isochronous, Async, Data)
    0x00, 0x04, // wMaxPacketSize = 1024
    0x01,       // bInterval = 1 (every microframe)

    // [257..262] SuperSpeed Endpoint Companion for 0x81
    0x06, 0x30,
    0x07,       // bMaxBurst = 7 (8 packets * 1024 = 8192 bytes/uframe)
    0x00,       // bmAttributes = 0 (Mult = 0)
    0x00, 0x20  // wBytesPerInterval = 8192 (0x2000)
};

// Keep VC Header wTotalLength consistent with 0x33 = 51 bytes: note byte [31] above
// String Descriptors (UTF-16LE)
alignas(4) const uint8_t kString0Bytes[4] = {
    0x04, 0x03, 0x09, 0x04 // English (US)
};

// "SunplusIT Inc." = 14 chars -> 2 + 28 = 30 bytes
alignas(4) const uint8_t kString1Bytes[30] = {
    0x1E, 0x03,
    'S', 0, 'u', 0, 'n', 0, 'p', 0, 'l', 0, 'u', 0, 's', 0, 'I', 0, 'T', 0,
    ' ', 0, 'I', 0, 'n', 0, 'c', 0, '.', 0
};

// "Integrated Webcam HD" = 20 chars -> 2 + 40 = 42 bytes
alignas(4) const uint8_t kString2Bytes[42] = {
    0x2A, 0x03,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'g', 0, 'r', 0, 'a', 0, 't', 0, 'e', 0, 'd', 0,
    ' ', 0, 'W', 0, 'e', 0, 'b', 0, 'c', 0, 'a', 0, 'm', 0, ' ', 0, 'H', 0, 'D', 0
};

bool isBlockedCompositeClient(const IOService *forClient) {
    if (forClient == nullptr || forClient->getMetaClass() == nullptr) {
        return false;
    }
    const char *cls = forClient->getMetaClass()->getClassName();
    if (cls == nullptr) {
        return false;
    }
    return strncmp(cls, "AppleUSBHostCompositeDevice", 27) == 0 ||
           strncmp(cls, "AppleUSBCDCCompositeDevice", 26) == 0;
}

alignas(4) const uint8_t kString3Bytes[28] = {
    0x1C, 0x03,
    'C', 0, 'C', 0, '2', 0, 'U', 0, 'N', 0, 'I', 0, 'C', 0, 'A', 0, 'M', 0,
    '7', 0, '3', 0, '9', 0, '0', 0
};

inline uint32_t current48MhzTicks() {
    uint64_t ns = 0;
    clock_get_uptime(&ns);
    absolutetime_to_nanoseconds(ns, &ns);
    return static_cast<uint32_t>((ns * 48ULL) / 1000ULL);
}

inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U));
}

} // namespace

// ============================================================================
// UniCamVirtualUVCPipe
// ============================================================================

#define pipe_super IOUSBHostPipe
OSDefineMetaClassAndStructors(UniCamVirtualUVCPipe, IOUSBHostPipe)

UniCamVirtualUVCPipe *UniCamVirtualUVCPipe::createWithDescriptor(
    const StandardUSB::EndpointDescriptor *epDesc,
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssDesc,
    UniCamVirtualUVCInterface *ownerIface,
    UniCamVirtualUVCDevice *ownerDev) {
    auto *pipe = OSTypeAlloc(UniCamVirtualUVCPipe);
    if (pipe == nullptr) {
        return nullptr;
    }
    if (!pipe->initVirtual(epDesc, ssDesc, ownerIface, ownerDev)) {
        pipe->release();
        return nullptr;
    }
    return pipe;
}

bool UniCamVirtualUVCPipe::initVirtual(
    const StandardUSB::EndpointDescriptor *epDesc,
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssDesc,
    UniCamVirtualUVCInterface *ownerIface,
    UniCamVirtualUVCDevice *ownerDev) {
    if (!OSObject::init() || epDesc == nullptr || ownerIface == nullptr || ownerDev == nullptr) {
        return false;
    }
    queueLock_ = IOLockAlloc();
    if (queueLock_ == nullptr) {
        return false;
    }
    pending_ = static_cast<UniCamUvcPendingTransfer *>(
        IOMallocZero(sizeof(UniCamUvcPendingTransfer) * kMaxPendingTransfers));
    if (pending_ == nullptr) {
        IOLockFree(queueLock_);
        queueLock_ = nullptr;
        return false;
    }
    IOUSBHostIOSource::_expansionData = static_cast<IOUSBHostIOSource::tExpansionData *>(
        IOMallocZero(sizeof(IOUSBHostIOSource::tExpansionData)));
    if (IOUSBHostIOSource::_expansionData == nullptr) {
        IOFree(pending_, sizeof(UniCamUvcPendingTransfer) * kMaxPendingTransfers);
        pending_ = nullptr;
        IOLockFree(queueLock_);
        queueLock_ = nullptr;
        return false;
    }
    epDesc_ = epDesc;
    ssDesc_ = ssDesc;
    ownerIface_ = ownerIface;
    ownerDev_ = ownerDev;
    pipeActive_ = true;

    /* Populate IOUSBHostIOSource & IOUSBHostPipe protected base members for DriverKit *_Impl RPCs */
    IOUSBHostIOSource::_device = ownerDev;
    IOUSBHostIOSource::_state = kStateReady;
    _speed = kIOUSBHostConnectionSpeedSuper;
    _interface = ownerIface;
    _descriptor = epDesc;
    _companionDescriptor = ssDesc;
    _sspCompanionDescriptor = nullptr;
    _currentPolicyDescriptor = *epDesc;
    if (ssDesc != nullptr) {
        _currentPolicyCompanionDescriptor = *ssDesc;
    }
    _maxPacketSize = USBToHost16(epDesc->wMaxPacketSize);
    _burstSize = (ssDesc != nullptr) ? (static_cast<uint32_t>(ssDesc->bMaxBurst) + 1U) : 1U;
    return true;
}

void UniCamVirtualUVCPipe::free() {
    flushAborted(kIOReturnAborted);
    if (IOUSBHostIOSource::_expansionData != nullptr) {
        IOFree(IOUSBHostIOSource::_expansionData, sizeof(IOUSBHostIOSource::tExpansionData));
        IOUSBHostIOSource::_expansionData = nullptr;
    }
    if (pending_ != nullptr) {
        IOFree(pending_, sizeof(UniCamUvcPendingTransfer) * kMaxPendingTransfers);
        pending_ = nullptr;
    }
    if (queueLock_ != nullptr) {
        IOLockFree(queueLock_);
        queueLock_ = nullptr;
    }
    IOUSBHostIOSource::_device = nullptr;
    _interface = nullptr;
    _descriptor = nullptr;
    _companionDescriptor = nullptr;
    _sspCompanionDescriptor = nullptr;
    epDesc_ = nullptr;
    ssDesc_ = nullptr;
    ownerIface_ = nullptr;
    ownerDev_ = nullptr;
    OSObject::free();
}

void UniCamVirtualUVCPipe::setActive(bool active) {
    if (queueLock_ == nullptr) {
        return;
    }
    IOLockLock(queueLock_);
    pipeActive_ = active;
    if (!active) {
        IOLockWakeup(queueLock_, &count_, false);
    }
    IOLockUnlock(queueLock_);
}

void UniCamVirtualUVCPipe::flushAborted(IOReturn err) {
    if (queueLock_ == nullptr || pending_ == nullptr) {
        return;
    }
    for (;;) {
        IOLockLock(queueLock_);
        pipeActive_ = false;
        if (count_ == 0) {
            IOLockWakeup(queueLock_, &count_, false);
            IOLockUnlock(queueLock_);
            break;
        }
        UniCamUvcPendingTransfer tr = pending_[head_];
        pending_[head_] = {};
        head_ = (head_ + 1) % kMaxPendingTransfers;
        count_--;
        IOLockWakeup(queueLock_, &count_, false);
        IOLockUnlock(queueLock_);

        if (tr.isTransaction && tr.transactions != nullptr) {
            for (uint32_t i = 0; i < tr.count; ++i) {
                tr.transactions[i].status = err;
                tr.transactions[i].completeCount = 0;
            }
            if (tr.txCompletion.action != nullptr) {
                tr.txCompletion.action(tr.txCompletion.owner,
                                       tr.txCompletion.parameter,
                                       err,
                                       tr.transactions);
            }
        } else if (!tr.isTransaction && tr.frames != nullptr) {
            for (uint32_t i = 0; i < tr.count; ++i) {
                tr.frames[i].status = err;
                tr.frames[i].completeCount = 0;
            }
            if (tr.legacyCompletion.action != nullptr) {
                tr.legacyCompletion.action(tr.legacyCompletion.owner,
                                           tr.legacyCompletion.parameter,
                                           err,
                                           tr.frames);
            }
        }
        if (tr.dataBuffer != nullptr) {
            tr.dataBuffer->release();
        }
    }
}

IOReturn UniCamVirtualUVCPipe::abort(IOOptionBits /*options*/,
                                     IOReturn withError,
                                     IOService * /*forClient*/) {
    flushAborted(withError == kIOReturnSuccess ? kIOReturnAborted : withError);
    return kIOReturnSuccess;
}

const StandardUSB::EndpointDescriptor *UniCamVirtualUVCPipe::getEndpointDescriptor(
    tGetEndpointDescriptorOptions /*type*/) {
    return epDesc_;
}

const StandardUSB::SuperSpeedEndpointCompanionDescriptor *UniCamVirtualUVCPipe::getSuperSpeedEndpointCompanionDescriptor(
    tGetEndpointDescriptorOptions /*type*/) {
    return ssDesc_;
}

IOReturn UniCamVirtualUVCPipe::adjustPipe(
    const StandardUSB::EndpointDescriptor * /*endpointDescriptor*/,
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor * /*companionDescriptor*/) {
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::setIdlePolicy(uint32_t idleTimeoutMs) {
    idleTimeoutMs_ = idleTimeoutMs;
    return kIOReturnSuccess;
}

uint32_t UniCamVirtualUVCPipe::getIdlePolicy() {
    return idleTimeoutMs_;
}

IOReturn UniCamVirtualUVCPipe::clearStall(bool /*withRequest*/) {
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::adjustPipe(StandardUSBDescriptors * /*descriptors*/) {
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::getDescriptors(StandardUSBDescriptors *descriptors,
                                              tGetEndpointDescriptorOptions /*type*/) {
    if (descriptors == nullptr) {
        return kIOReturnBadArgument;
    }
    descriptors->bcdUSB = 0x0300;
    descriptors->descriptor = epDesc_;
    descriptors->ssCompanionDescriptor = ssDesc_;
    descriptors->sspCompanionDescriptor = nullptr;
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::io(IOMemoryDescriptor * /*dataBuffer*/,
                                  uint32_t /*dataBufferLength*/,
                                  IOUSBHostCompletion * /*completion*/,
                                  uint32_t /*completionTimeoutMs*/) {
    // Interrupt IN status pipe (0x83): stays idle until aborted on stream/close
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::io(IOMemoryDescriptor * /*dataBuffer*/,
                                  uint32_t /*dataBufferLength*/,
                                  uint32_t &bytesTransferred,
                                  uint32_t /*completionTimeoutMs*/) {
    bytesTransferred = 0;
    return kIOReturnTimeout;
}

IOReturn UniCamVirtualUVCPipe::io(IOMemoryDescriptor * /*dataBuffer*/,
                                  uint32_t /*dataBufferLength*/,
                                  IOUSBHostBundledCompletion * /*completion*/,
                                  uint32_t /*completionTimeoutMs*/) {
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCPipe::enqueueIsoch(
    IOMemoryDescriptor *dataBuffer,
    IOUSBHostIsochronousTransaction *transactions,
    IOUSBHostIsochronousFrame *frames,
    uint32_t count,
    bool isTransaction,
    const IOUSBHostIsochronousTransactionCompletion *txComp,
    const IOUSBHostIsochronousCompletion *legacyComp) {
    if (dataBuffer == nullptr || count == 0 ||
        (isTransaction ? (transactions == nullptr) : (frames == nullptr))) {
        return kIOReturnBadArgument;
    }
    if (epDesc_ == nullptr || epDesc_->bEndpointAddress != kUvcEpIsochIn) {
        return kIOReturnUnsupported;
    }

    if (queueLock_ == nullptr || pending_ == nullptr) {
        return kIOReturnNotReady;
    }
    IOLockLock(queueLock_);
    if (!pipeActive_) {
        IOLockUnlock(queueLock_);
        return kIOReturnAborted;
    }
    if (count_ >= kMaxPendingTransfers) {
        IOLockUnlock(queueLock_);
        return kIOReturnNoResources;
    }
    UniCamUvcPendingTransfer &slot = pending_[tail_];
    slot.inUse = true;
    slot.isTransaction = isTransaction;
    dataBuffer->retain();
    slot.dataBuffer = dataBuffer;
    slot.transactions = transactions;
    slot.frames = frames;
    slot.count = count;
    if (txComp != nullptr) {
        slot.txCompletion = *txComp;
    } else {
        slot.txCompletion = {};
    }
    if (legacyComp != nullptr) {
        slot.legacyCompletion = *legacyComp;
    } else {
        slot.legacyCompletion = {};
    }
    tail_ = (tail_ + 1) % kMaxPendingTransfers;
    count_++;
    IOLockWakeup(queueLock_, &count_, true);
    IOLockUnlock(queueLock_);
    return kIOReturnSuccess;
}

bool UniCamVirtualUVCPipe::popReadyTransfer(UniCamUvcPendingTransfer *outTransfer) {
    if (queueLock_ == nullptr || pending_ == nullptr || outTransfer == nullptr) {
        return false;
    }
    IOLockLock(queueLock_);
    if (count_ == 0) {
        IOLockUnlock(queueLock_);
        return false;
    }
    *outTransfer = pending_[head_];
    pending_[head_] = {};
    head_ = (head_ + 1) % kMaxPendingTransfers;
    count_--;
    IOLockUnlock(queueLock_);
    return true;
}

bool UniCamVirtualUVCPipe::waitPopReadyTransfer(UniCamUvcPendingTransfer *outTransfer,
                                                AbsoluteTime deadline) {
    if (queueLock_ == nullptr || pending_ == nullptr || outTransfer == nullptr) {
        return false;
    }
    IOLockLock(queueLock_);
    while (count_ == 0 && pipeActive_) {
        const int wr = IOLockSleepDeadline(queueLock_, &count_, deadline, THREAD_UNINT);
        if (wr == THREAD_TIMED_OUT && count_ == 0) {
            IOLockUnlock(queueLock_);
            return false;
        }
    }
    if (count_ == 0) {
        IOLockUnlock(queueLock_);
        return false;
    }
    *outTransfer = pending_[head_];
    pending_[head_] = {};
    head_ = (head_ + 1) % kMaxPendingTransfers;
    count_--;
    IOLockUnlock(queueLock_);
    return true;
}

IOReturn UniCamVirtualUVCPipe::io(IOMemoryDescriptor *dataBuffer,
                                  IOUSBHostIsochronousFrame *frameList,
                                  uint32_t frameListCount,
                                  uint64_t /*firstFrameNumber*/,
                                  IOUSBHostIsochronousCompletion *completion) {
    return enqueueIsoch(dataBuffer, nullptr, frameList, frameListCount, false, nullptr, completion);
}

IOReturn UniCamVirtualUVCPipe::io(IOMemoryDescriptor *dataBuffer,
                                  IOUSBHostIsochronousTransaction *transactionList,
                                  uint32_t transactionListCount,
                                  uint64_t /*firstFrameNumber*/,
                                  IOOptionBits /*options*/,
                                  IOUSBHostIsochronousTransactionCompletion *completion) {
    return enqueueIsoch(dataBuffer, transactionList, nullptr, transactionListCount, true, completion, nullptr);
}

uint8_t UniCamVirtualUVCPipe::getSpeed() const {
    return kIOUSBHostConnectionSpeedSuper;
}

tUSBHostDeviceAddress UniCamVirtualUVCPipe::getDeviceAddress() const {
    return kUvcDeviceAddress;
}

bool UniCamVirtualUVCPipe::deliverYuy2Frame(const uint8_t *yuy2Data,
                                            uint32_t byteCount,
                                            uint8_t &fidInOut,
                                            uint32_t ptsTicks,
                                            uint16_t &scrSofInOut) {
    if (yuy2Data == nullptr || byteCount == 0) {
        return false;
    }

    uint32_t srcOffset = 0;
    const uint8_t fid = fidInOut & 1U;
    bool completedAtLeastOne = false;
    AbsoluteTime frameDeadline = 0;
    clock_interval_to_deadline(60U, kMillisecondScale, &frameDeadline);

    while (srcOffset < byteCount) {
        UniCamUvcPendingTransfer tr{};
        if (!waitPopReadyTransfer(&tr, frameDeadline)) {
            break;
        }

        uint64_t nowTs = 0;
        clock_get_uptime(&nowTs);

        IOMemoryMap *map = tr.dataBuffer->map();
        if (map == nullptr) {
            if (tr.isTransaction && tr.txCompletion.action != nullptr) {
                tr.txCompletion.action(tr.txCompletion.owner, tr.txCompletion.parameter,
                                       kIOReturnNoMemory, tr.transactions);
            } else if (!tr.isTransaction && tr.legacyCompletion.action != nullptr) {
                tr.legacyCompletion.action(tr.legacyCompletion.owner, tr.legacyCompletion.parameter,
                                           kIOReturnNoMemory, tr.frames);
            }
            tr.dataBuffer->release();
            continue;
        }

        auto *dstBase = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
        const uint64_t dstCap = static_cast<uint64_t>(map->getLength());
        uint64_t legacyOffset = 0;

        for (uint32_t i = 0; i < tr.count; ++i) {
            const uint32_t reqLen = tr.isTransaction
                                        ? tr.transactions[i].requestCount
                                        : tr.frames[i].requestCount;
            const uint64_t txOff = tr.isTransaction
                                       ? static_cast<uint64_t>(tr.transactions[i].offset)
                                       : legacyOffset;
            if (!tr.isTransaction) {
                legacyOffset += reqLen;
            }

            uint32_t written = 0;
            if (srcOffset < byteCount && reqLen > 12 && txOff + reqLen <= dstCap) {
                const uint32_t maxPayload = reqLen - 12;
                const uint32_t remaining = byteCount - srcOffset;
                const uint32_t payload = (remaining < maxPayload) ? remaining : maxPayload;
                const bool isLast = (srcOffset + payload >= byteCount);

                uint8_t *pkt = dstBase + txOff;
                // UVC 12-byte payload header: HLE=12, BFH (FID | EOF | PTS | SCR), PTS(4), SCR(6)
                pkt[0] = 12;
                pkt[1] = static_cast<uint8_t>(0x0C | fid | (isLast ? 0x02U : 0x00U));
                pkt[2] = static_cast<uint8_t>(ptsTicks & 0xFF);
                pkt[3] = static_cast<uint8_t>((ptsTicks >> 8) & 0xFF);
                pkt[4] = static_cast<uint8_t>((ptsTicks >> 16) & 0xFF);
                pkt[5] = static_cast<uint8_t>((ptsTicks >> 24) & 0xFF);
                pkt[6] = pkt[2];
                pkt[7] = pkt[3];
                pkt[8] = pkt[4];
                pkt[9] = pkt[5];
                pkt[10] = static_cast<uint8_t>(scrSofInOut & 0xFF);
                pkt[11] = static_cast<uint8_t>((scrSofInOut >> 8) & 0x07);
                scrSofInOut = static_cast<uint16_t>((scrSofInOut + 1U) & 0x07FFU);

                bcopy(yuy2Data + srcOffset, pkt + 12, payload);
                srcOffset += payload;
                written = 12 + payload;
            }

            if (tr.isTransaction) {
                tr.transactions[i].status = kIOReturnSuccess;
                tr.transactions[i].completeCount = written;
                tr.transactions[i].timeStamp = nowTs;
            } else {
                tr.frames[i].status = kIOReturnSuccess;
                tr.frames[i].completeCount = written;
                tr.frames[i].timeStamp = nowTs;
            }
        }

        map->release();
        if (tr.isTransaction && tr.txCompletion.action != nullptr) {
            tr.txCompletion.action(tr.txCompletion.owner, tr.txCompletion.parameter,
                                   kIOReturnSuccess, tr.transactions);
        } else if (!tr.isTransaction && tr.legacyCompletion.action != nullptr) {
            tr.legacyCompletion.action(tr.legacyCompletion.owner, tr.legacyCompletion.parameter,
                                       kIOReturnSuccess, tr.frames);
        }
        tr.dataBuffer->release();
        completedAtLeastOne = true;
    }

    if (completedAtLeastOne) {
        fidInOut ^= 1U;
    }
    return (srcOffset >= byteCount);
}

// ============================================================================
// UniCamVirtualUVCInterface
// ============================================================================

#define iface_super IOUSBHostInterface
OSDefineMetaClassAndStructors(UniCamVirtualUVCInterface, IOUSBHostInterface)

UniCamVirtualUVCInterface *UniCamVirtualUVCInterface::createWithUvc(
    UniCamVirtualUVCDevice *ownerDev,
    uint8_t interfaceNumber) {
    auto *iface = OSTypeAlloc(UniCamVirtualUVCInterface);
    if (iface == nullptr) {
        return nullptr;
    }
    if (!iface->initVirtual(ownerDev, interfaceNumber)) {
        iface->release();
        return nullptr;
    }
    return iface;
}

bool UniCamVirtualUVCInterface::initVirtual(UniCamVirtualUVCDevice *ownerDev,
                                            uint8_t interfaceNumber) {
    if (!IOUSBInterface::init() || ownerDev == nullptr) {
        return false;
    }
    ifaceLock_ = IOLockAlloc();
    if (ifaceLock_ == nullptr) {
        return false;
    }
    _expansionData = static_cast<IOUSBHostInterface::tExpansionData *>(
        IOMallocZero(sizeof(IOUSBHostInterface::tExpansionData)));
    if (_expansionData == nullptr) {
        IOLockFree(ifaceLock_);
        ifaceLock_ = nullptr;
        return false;
    }
    ownerDev_ = ownerDev;
    interfaceNumber_ = interfaceNumber;
    activeAltSetting_ = 0;

    _device = ownerDev;
    _configurationDescriptor = ownerDev->getConfigurationDescriptor();
    _interfaceDescriptor = ownerDev->getInterfaceDescriptorFor(interfaceNumber, 0);
    _locationID = kUvcLocationId;

    setProperty("IOClass", "IOUSBHostInterface");
    setProperty(kUSBHostMatchingPropertyVendorID, kUvcVendorId, 16);
    setProperty(kUSBHostMatchingPropertyProductID, kUvcProductId, 16);
    setProperty(kUSBHostMatchingPropertyDeviceReleaseNumber, kUvcBcdDevice, 16);
    setProperty(kUSBHostMatchingPropertyConfigurationValue, 1, 8);
    setProperty(kUSBHostMatchingPropertyInterfaceNumber, interfaceNumber, 8);
    setProperty(kUSBHostInterfacePropertyAlternateSetting, 0ULL, 8);
    setProperty(kUSBHostMatchingPropertyInterfaceClass, 14, 8);
    setProperty(kUSBHostMatchingPropertyInterfaceSubClass, interfaceNumber == 0 ? 1 : 2, 8);
    setProperty(kUSBHostMatchingPropertyInterfaceProtocol, 0ULL, 8);
    setProperty(kUSBHostInterfacePropertyNumEndpoints, interfaceNumber == 0 ? 1ULL : 0ULL, 8);
    setProperty(kUSBHostMatchingPropertySpeed, kIOUSBHostConnectionSpeedSuper, 8);
    setProperty("USBSpeed", kIOUSBHostConnectionSpeedSuper, 8);
    setProperty("Device Speed", kIOUSBHostConnectionSpeedSuper, 8);
    setProperty(kUSBHostMatchingPropertyPortType, kIOUSBHostPortTypeInternal, 8);
    setProperty(kUSBHostPropertyLocationID, kUvcLocationId, 32);
    setProperty("locationID", kUvcLocationId, 32);
    setProperty("USB Address", kUvcDeviceAddress, 16);
    setProperty("kUSBAddress", kUvcDeviceAddress, 16);
    setProperty("idVendor", kUvcVendorId, 16);
    setProperty("idProduct", kUvcProductId, 16);
    setProperty("bcdDevice", kUvcBcdDevice, 16);
    setProperty("bConfigurationValue", 1, 8);
    setProperty("bInterfaceNumber", interfaceNumber, 8);
    setProperty("bAlternateSetting", 0ULL, 8);
    setProperty("bInterfaceClass", 14, 8);
    setProperty("bInterfaceSubClass", interfaceNumber == 0 ? 1 : 2, 8);
    setProperty("bInterfaceProtocol", 0ULL, 8);
    setProperty("USB Product Name", "Integrated Webcam HD");
    setProperty("USB Vendor Name", "SunplusIT Inc.");
    setProperty("Built-In", kOSBooleanTrue);
    return true;
}

bool UniCamVirtualUVCInterface::attach(IOService *provider) {
    if (!IOUSBInterface::attach(provider)) {
        return false;
    }
    return true;
}

bool UniCamVirtualUVCInterface::start(IOService *provider) {
    if (!IOUSBInterface::start(provider)) {
        return false;
    }
    _workLoop = IOWorkLoop::workLoop();
    if (_workLoop != nullptr) {
        _commandGate = IOCommandGate::commandGate(this);
        if (_commandGate != nullptr) {
            _workLoop->addEventSource(_commandGate);
        }
    }

    if (interfaceNumber_ == 0) {
        vcInterruptPipe_ = UniCamVirtualUVCPipe::createWithDescriptor(
            ownerDev_->getEndpointDescriptorFor(kUvcEpInterruptIn),
            ownerDev_->getSsCompanionDescriptorFor(kUvcEpInterruptIn),
            this, ownerDev_);
    } else {
        vsIsochPipe_ = UniCamVirtualUVCPipe::createWithDescriptor(
            ownerDev_->getEndpointDescriptorFor(kUvcEpIsochIn),
            ownerDev_->getSsCompanionDescriptorFor(kUvcEpIsochIn),
            this, ownerDev_);
    }
    return true;
}

void UniCamVirtualUVCInterface::stop(IOService *provider) {
    if (vcInterruptPipe_ != nullptr) {
        vcInterruptPipe_->abort();
        vcInterruptPipe_->release();
        vcInterruptPipe_ = nullptr;
    }
    if (vsIsochPipe_ != nullptr) {
        vsIsochPipe_->abort();
        vsIsochPipe_->release();
        vsIsochPipe_ = nullptr;
    }
    if (_workLoop != nullptr && _commandGate != nullptr) {
        _workLoop->removeEventSource(_commandGate);
    }
    OSSafeReleaseNULL(_commandGate);
    OSSafeReleaseNULL(_workLoop);
    IOUSBInterface::stop(provider);
}

bool UniCamVirtualUVCInterface::terminate(IOOptionBits options) {
    if (vsIsochPipe_ != nullptr) {
        vsIsochPipe_->abort();
    }
    if (vcInterruptPipe_ != nullptr) {
        vcInterruptPipe_->abort();
    }
    return IOUSBInterface::terminate(options);
}

void UniCamVirtualUVCInterface::free() {
    if (_expansionData != nullptr) {
        IOFree(_expansionData, sizeof(IOUSBHostInterface::tExpansionData));
        _expansionData = nullptr;
    }
    if (ifaceLock_ != nullptr) {
        IOLockFree(ifaceLock_);
        ifaceLock_ = nullptr;
    }
    ownerDev_ = nullptr;
    _device = nullptr;
    IOUSBInterface::free();
}

bool UniCamVirtualUVCInterface::open(IOService *forClient, IOOptionBits options, void *arg) {
    if (forClient == nullptr || ifaceLock_ == nullptr) {
        return false;
    }
    IOLockLock(ifaceLock_);
    if (openClient_ != nullptr && openClient_ != forClient) {
        IOLockUnlock(ifaceLock_);
        return false;
    }
    openClient_ = forClient;
    IOLockUnlock(ifaceLock_);

    if (!IOUSBInterface::open(forClient, options, arg)) {
        IOLockLock(ifaceLock_);
        if (openClient_ == forClient) {
            openClient_ = nullptr;
        }
        IOLockUnlock(ifaceLock_);
        return false;
    }
    if ((options & kUSBHostOpenOptionSelectAlternateSetting) != 0) {
        uint8_t alt = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg) & 0xFFU);
        selectAlternateSetting(alt);
    }
    return true;
}

void UniCamVirtualUVCInterface::close(IOService *forClient, IOOptionBits options) {
    if (interfaceNumber_ == 1 && activeAltSetting_ != 0) {
        selectAlternateSetting(0);
    }
    if (ifaceLock_ != nullptr) {
        IOLockLock(ifaceLock_);
        if (openClient_ == forClient) {
            openClient_ = nullptr;
        }
        IOLockUnlock(ifaceLock_);
    }
    IOUSBInterface::close(forClient, options);
}

IOReturn UniCamVirtualUVCInterface::message(UInt32 type, IOService *provider, void *argument) {
    return IOUSBInterface::message(type, provider, argument);
}

IOReturn UniCamVirtualUVCInterface::setIdlePolicy(uint32_t deviceIdleTimeout) {
    _deviceIdleTimeout = deviceIdleTimeout;
    return kIOReturnSuccess;
}

uint32_t UniCamVirtualUVCInterface::getIdlePolicy() {
    return _deviceIdleTimeout;
}

const StandardUSB::ConfigurationDescriptor *UniCamVirtualUVCInterface::getConfigurationDescriptor() {
    return ownerDev_ ? ownerDev_->getConfigurationDescriptor() : _configurationDescriptor;
}

const StandardUSB::InterfaceDescriptor *UniCamVirtualUVCInterface::getInterfaceDescriptor() {
    return ownerDev_ ? ownerDev_->getInterfaceDescriptorFor(interfaceNumber_, activeAltSetting_) : _interfaceDescriptor;
}

const StandardUSB::StringDescriptor *UniCamVirtualUVCInterface::getStringDescriptor(
    uint8_t index,
    uint16_t languageID) {
    return ownerDev_ ? ownerDev_->getStringDescriptor(index, languageID) : nullptr;
}

IOReturn UniCamVirtualUVCInterface::selectAlternateSetting(uint8_t bAlternateSetting) {
    if (interfaceNumber_ == 0 && bAlternateSetting != 0) {
        return kIOReturnBadArgument;
    }
    if (interfaceNumber_ == 1 && bAlternateSetting > 1) {
        return kIOReturnBadArgument;
    }
    activeAltSetting_ = bAlternateSetting;
    _interfaceDescriptor = ownerDev_->getInterfaceDescriptorFor(interfaceNumber_, activeAltSetting_);
    setProperty(kUSBHostInterfacePropertyAlternateSetting, activeAltSetting_, 8);
    setProperty(kUSBHostInterfacePropertyNumEndpoints,
                _interfaceDescriptor ? _interfaceDescriptor->bNumEndpoints : 0, 8);
    if (interfaceNumber_ == 1 && vsIsochPipe_ != nullptr) {
        if (bAlternateSetting == 1) {
            vsIsochPipe_->setActive(true);
        } else {
            vsIsochPipe_->setActive(false);
            vsIsochPipe_->abort();
        }
    }
    if (ownerDev_ != nullptr) {
        ownerDev_->notifyAltSettingChanged(interfaceNumber_, activeAltSetting_);
    }
    return kIOReturnSuccess;
}

IOUSBHostPipe *UniCamVirtualUVCInterface::copyPipe(uint8_t address) {
    if (interfaceNumber_ == 0 && address == kUvcEpInterruptIn && vcInterruptPipe_ != nullptr) {
        vcInterruptPipe_->retain();
        return vcInterruptPipe_;
    }
    if (interfaceNumber_ == 1 && address == kUvcEpIsochIn && vsIsochPipe_ != nullptr) {
        vsIsochPipe_->retain();
        return vsIsochPipe_;
    }
    return nullptr;
}

IOReturn UniCamVirtualUVCInterface::deviceRequest(StandardUSB::DeviceRequest &request,
                                                  void *dataBuffer,
                                                  IOUSBHostCompletion *completion,
                                                  uint32_t /*completionTimeoutMs*/) {
    if (ownerDev_ == nullptr) {
        return kIOReturnNotReady;
    }
    uint32_t transferred = 0;
    IOReturn ret = ownerDev_->handleDeviceRequestSync(request, dataBuffer, request.wLength, transferred);
    if (completion != nullptr && completion->action != nullptr) {
        completion->action(completion->owner, completion->parameter, ret, transferred);
    }
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCInterface::deviceRequest(StandardUSB::DeviceRequest &request,
                                                  IOMemoryDescriptor *dataBuffer,
                                                  IOUSBHostCompletion *completion,
                                                  uint32_t completionTimeoutMs) {
    if (ownerDev_ == nullptr) {
        return kIOReturnNotReady;
    }
    uint32_t transferred = 0;
    IOReturn ret = deviceRequest(request, dataBuffer, transferred, completionTimeoutMs);
    if (completion != nullptr && completion->action != nullptr) {
        completion->action(completion->owner, completion->parameter, ret, transferred);
    }
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCInterface::deviceRequest(StandardUSB::DeviceRequest &request,
                                                  void *dataBuffer,
                                                  uint32_t &bytesTransferred,
                                                  uint32_t /*completionTimeoutMs*/) {
    if (ownerDev_ == nullptr) {
        return kIOReturnNotReady;
    }
    return ownerDev_->handleDeviceRequestSync(request, dataBuffer, request.wLength, bytesTransferred);
}

IOReturn UniCamVirtualUVCInterface::deviceRequest(StandardUSB::DeviceRequest &request,
                                                  IOMemoryDescriptor *dataBuffer,
                                                  uint32_t &bytesTransferred,
                                                  uint32_t /*completionTimeoutMs*/) {
    if (ownerDev_ == nullptr) {
        return kIOReturnNotReady;
    }
    if (dataBuffer == nullptr || request.wLength == 0) {
        return ownerDev_->handleDeviceRequestSync(request, nullptr, 0, bytesTransferred);
    }
    IOMemoryMap *map = dataBuffer->map();
    if (map == nullptr) {
        return kIOReturnNoMemory;
    }
    auto *ptr = reinterpret_cast<void *>(map->getVirtualAddress());
    uint32_t cap = static_cast<uint32_t>(map->getLength());
    IOReturn ret = ownerDev_->handleDeviceRequestSync(request, ptr, cap, bytesTransferred);
    map->release();
    return ret;
}

IOUSBHostDevice *UniCamVirtualUVCInterface::getDevice() const {
    return ownerDev_;
}

uint64_t UniCamVirtualUVCInterface::getFrameNumber(AbsoluteTime *theTime) const {
    return ownerDev_ ? ownerDev_->getUvcFrameNumber(theTime) : 0ULL;
}

uint32_t UniCamVirtualUVCInterface::getPortStatus() const {
    return kIOUSBHostPortStatusPortTypeInternal |
           kIOUSBHostPortStatusConnectedSpeedSuper |
           kIOUSBHostPortStatusEnabled;
}

IOBufferMemoryDescriptor *UniCamVirtualUVCInterface::createIOBuffer(IOOptionBits options,
                                                                    mach_vm_size_t capacity) {
    return IOBufferMemoryDescriptor::withOptions(options, capacity, PAGE_SIZE);
}

IOReturn UniCamVirtualUVCInterface::currentMicroframe(uint64_t &microframe,
                                                      uint64_t &microframeTime) {
    return ownerDev_ ? ownerDev_->currentMicroframe(microframe, microframeTime) : kIOReturnNotReady;
}

IOReturn UniCamVirtualUVCInterface::referenceMicroframe(uint64_t &microframe,
                                                        uint64_t &microframeTime) {
    return ownerDev_ ? ownerDev_->referenceMicroframe(microframe, microframeTime) : kIOReturnNotReady;
}

// ============================================================================
// UniCamVirtualUVCDevice
// ============================================================================

#define dev_super IOUSBHostDevice
OSDefineMetaClassAndStructors(UniCamVirtualUVCDevice, IOUSBHostDevice)

UniCamVirtualUVCDevice *UniCamVirtualUVCDevice::createWithDelegate(
    UniCamUvcDelegate *delegate,
    bool debugLog) {
    auto *dev = OSTypeAlloc(UniCamVirtualUVCDevice);
    if (dev == nullptr) {
        return nullptr;
    }
    if (!dev->initWithDelegate(delegate, debugLog)) {
        dev->release();
        return nullptr;
    }
    return dev;
}

bool UniCamVirtualUVCDevice::initWithDelegate(UniCamUvcDelegate *delegate, bool debugLog) {
    if (!IOUSBDevice::init()) {
        return false;
    }
    devLock_ = IOLockAlloc();
    if (devLock_ == nullptr) {
        return false;
    }
    openClients_ = OSSet::withCapacity(4);
    if (openClients_ == nullptr) {
        IOLockFree(devLock_);
        devLock_ = nullptr;
        return false;
    }
    _expansionData = static_cast<IOUSBHostDevice::tExpansionData *>(
        IOMallocZero(sizeof(IOUSBHostDevice::tExpansionData)));
    if (_expansionData == nullptr) {
        OSSafeReleaseNULL(openClients_);
        IOLockFree(devLock_);
        devLock_ = nullptr;
        return false;
    }
    delegate_ = delegate;
    debugLog_ = debugLog;
    _address = kUvcDeviceAddress;
    _speed = kIOUSBHostConnectionSpeedSuper;
    _currentConfigValue = 1;
    _locationID = kUvcLocationId;
    initDefaultUvcControls();

    setProperty("IOClass", "IOUSBHostDevice");
    setProperty(kUSBHostMatchingPropertyVendorID, kUvcVendorId, 16);
    setProperty(kUSBHostMatchingPropertyProductID, kUvcProductId, 16);
    setProperty(kUSBHostMatchingPropertyDeviceReleaseNumber, kUvcBcdDevice, 16);
    setProperty(kUSBHostMatchingPropertyDeviceClass, 0xEF, 8);
    setProperty(kUSBHostMatchingPropertyDeviceSubClass, 0x02, 8);
    setProperty(kUSBHostMatchingPropertyDeviceProtocol, 0x01, 8);
    setProperty(kUSBHostDevicePropertyStandardVersion, 0x0300, 16);
    setProperty(kUSBHostDevicePropertyNumConfigs, 1, 8);
    setProperty(kUSBHostDevicePropertyCurrentConfiguration, 1, 8);
    setProperty(kUSBHostMatchingPropertySpeed, kIOUSBHostConnectionSpeedSuper, 8);
    setProperty("USBSpeed", kIOUSBHostConnectionSpeedSuper, 8);
    setProperty("Device Speed", kIOUSBHostConnectionSpeedSuper, 8);
    setProperty(kUSBHostMatchingPropertyPortType, kIOUSBHostPortTypeInternal, 8);
    setProperty(kUSBHostPropertyLocationID, kUvcLocationId, 32);
    setProperty("locationID", kUvcLocationId, 32);
    setProperty(kUSBHostDevicePropertyAddress, kUvcDeviceAddress, 16);
    setProperty("USB Address", kUvcDeviceAddress, 16);
    setProperty("kUSBAddress", kUvcDeviceAddress, 16);
    setProperty(kUSBHostDevicePropertyVendorString, "SunplusIT Inc.");
    setProperty(kUSBHostDevicePropertyProductString, "Integrated Webcam HD");
    setProperty("USB Product Name", "Integrated Webcam HD");
    setProperty("USB Vendor Name", "SunplusIT Inc.");
    setProperty(kUSBHostDevicePropertySerialNumberString, "CC2UNICAM7390");
    setProperty("Built-In", kOSBooleanTrue);
    setProperty("non-removable", "yes");
    return true;
}

void UniCamVirtualUVCDevice::initDefaultUvcControls() {
    bzero(&probeCtrl_, sizeof(probeCtrl_));
    probeCtrl_.bmHint = 0x0001;
    probeCtrl_.bFormatIndex = 1;
    probeCtrl_.bFrameIndex = 1; // 1280x720 default
    probeCtrl_.dwFrameInterval = 333333;
    probeCtrl_.wDelay = 33;
    probeCtrl_.dwMaxVideoFrameSize = frameSizeForIndex(1);
    probeCtrl_.dwMaxPayloadTransferSize = kUvcMaxPayloadSize;
    commitCtrl_ = probeCtrl_;
}

uint32_t UniCamVirtualUVCDevice::frameSizeForIndex(uint8_t frameIndex) const {
    switch (frameIndex) {
    case 2:
        return 1280U * 800U * 2U;
    case 3:
        return 640U * 480U * 2U;
    case 1:
    default:
        return 1280U * 720U * 2U;
    }
}

bool UniCamVirtualUVCDevice::attach(IOService *provider) {
    return IOUSBDevice::attach(provider);
}

bool UniCamVirtualUVCDevice::start(IOService *provider) {
    if (!IOUSBDevice::start(provider)) {
        return false;
    }
    _workLoop = IOWorkLoop::workLoop();
    if (_workLoop != nullptr) {
        _commandGate = IOCommandGate::commandGate(this);
        if (_commandGate != nullptr) {
            _workLoop->addEventSource(_commandGate);
        }
    }

    vcIface_ = UniCamVirtualUVCInterface::createWithUvc(this, 0);
    vsIface_ = UniCamVirtualUVCInterface::createWithUvc(this, 1);
    if (vcIface_ == nullptr || vsIface_ == nullptr) {
        return false;
    }

    if (!vcIface_->attach(this) || !vcIface_->start(this)) {
        return false;
    }
    if (!vsIface_->attach(this) || !vsIface_->start(this)) {
        return false;
    }

    vcIface_->registerService();
    vsIface_->registerService();
    return true;
}

void UniCamVirtualUVCDevice::stop(IOService *provider) {
    if (vsIface_ != nullptr) {
        vsIface_->terminate();
        vsIface_->stop(this);
        vsIface_->detach(this);
        OSSafeReleaseNULL(vsIface_);
    }
    if (vcIface_ != nullptr) {
        vcIface_->terminate();
        vcIface_->stop(this);
        vcIface_->detach(this);
        OSSafeReleaseNULL(vcIface_);
    }
    if (_workLoop != nullptr && _commandGate != nullptr) {
        _workLoop->removeEventSource(_commandGate);
    }
    OSSafeReleaseNULL(_commandGate);
    OSSafeReleaseNULL(_workLoop);
    IOUSBDevice::stop(provider);
}

bool UniCamVirtualUVCDevice::terminate(IOOptionBits options) {
    return IOUSBDevice::terminate(options);
}

void UniCamVirtualUVCDevice::free() {
    if (_expansionData != nullptr) {
        OSSafeReleaseNULL(_expansionData->_dkInterfaceIteratorDict);
        IOFree(_expansionData, sizeof(IOUSBHostDevice::tExpansionData));
        _expansionData = nullptr;
    }
    if (devLock_ != nullptr) {
        IOLockLock(devLock_);
        OSSafeReleaseNULL(openClients_);
        IOLockUnlock(devLock_);
        IOLockFree(devLock_);
        devLock_ = nullptr;
    } else {
        OSSafeReleaseNULL(openClients_);
    }
    delegate_ = nullptr;
    IOUSBDevice::free();
}

bool UniCamVirtualUVCDevice::open(IOService *forClient, IOOptionBits options, void *arg) {
    if (isBlockedCompositeClient(forClient)) {
        if (debugLog_) {
            IOLog("UniCam: rejecting composite client %s open on virtual UVC device\n",
                  forClient->getMetaClass()->getClassName());
        }
        return false;
    }
    return IOUSBDevice::open(forClient, options, arg);
}

bool UniCamVirtualUVCDevice::handleOpen(IOService *forClient, IOOptionBits options, void *arg) {
    if (forClient == nullptr || isBlockedCompositeClient(forClient)) {
        return false;
    }
    if (devLock_ != nullptr && openClients_ != nullptr) {
        IOLockLock(devLock_);
        const bool ok = openClients_->setObject(forClient);
        IOLockUnlock(devLock_);
        return ok;
    }
    return IOUSBDevice::handleOpen(forClient, options, arg);
}

bool UniCamVirtualUVCDevice::handleIsOpen(const IOService *forClient) const {
    if (devLock_ != nullptr && openClients_ != nullptr) {
        IOLockLock(devLock_);
        const bool isOpen = (forClient != nullptr)
            ? openClients_->containsObject(const_cast<IOService *>(forClient))
            : (openClients_->getCount() > 0U);
        IOLockUnlock(devLock_);
        return isOpen;
    }
    return IOUSBDevice::handleIsOpen(forClient);
}

void UniCamVirtualUVCDevice::close(IOService *forClient, IOOptionBits options) {
    IOUSBDevice::close(forClient, options);
}

void UniCamVirtualUVCDevice::handleClose(IOService *forClient, IOOptionBits options) {
    if (devLock_ != nullptr && openClients_ != nullptr) {
        IOLockLock(devLock_);
        if (forClient != nullptr) {
            openClients_->removeObject(forClient);
        } else {
            openClients_->flushCollection();
        }
        IOLockUnlock(devLock_);
        return;
    }
    IOUSBDevice::handleClose(forClient, options);
}

const StandardUSB::Descriptor *UniCamVirtualUVCDevice::getDescriptor(
    uint8_t type,
    uint16_t &length,
    uint8_t index,
    uint16_t languageID,
    tDeviceRequestType requestType,
    tDeviceRequestRecipient requestRecipient) {
    if (requestType != kRequestTypeStandard || requestRecipient != kRequestRecipientDevice) {
        length = 0;
        return nullptr;
    }
    if (type == StandardUSB::kDescriptorTypeDevice && index == 0) {
        length = sizeof(kDeviceDescriptorBytes);
        return reinterpret_cast<const StandardUSB::Descriptor *>(kDeviceDescriptorBytes);
    }
    if (type == StandardUSB::kDescriptorTypeConfiguration && index == 0) {
        length = sizeof(kConfigDescriptorBytes);
        return reinterpret_cast<const StandardUSB::Descriptor *>(kConfigDescriptorBytes);
    }
    if (type == StandardUSB::kDescriptorTypeString) {
        const auto *str = getStringDescriptor(index, languageID);
        length = (str != nullptr) ? str->bLength : 0;
        return reinterpret_cast<const StandardUSB::Descriptor *>(str);
    }
    length = 0;
    return nullptr;
}

const StandardUSB::DeviceDescriptor *UniCamVirtualUVCDevice::getDeviceDescriptor() {
    return reinterpret_cast<const StandardUSB::DeviceDescriptor *>(kDeviceDescriptorBytes);
}

const StandardUSB::ConfigurationDescriptor *UniCamVirtualUVCDevice::getConfigurationDescriptor(uint8_t index) {
    if (index != 0) {
        return nullptr;
    }
    return reinterpret_cast<const StandardUSB::ConfigurationDescriptor *>(kConfigDescriptorBytes);
}

const StandardUSB::ConfigurationDescriptor *UniCamVirtualUVCDevice::getConfigurationDescriptorWithValue(uint8_t bConfigurationValue) {
    if (bConfigurationValue != 1) {
        return nullptr;
    }
    return getConfigurationDescriptor(0);
}

const StandardUSB::ConfigurationDescriptor *UniCamVirtualUVCDevice::getConfigurationDescriptor() {
    return getConfigurationDescriptor(0);
}

const StandardUSB::StringDescriptor *UniCamVirtualUVCDevice::getStringDescriptor(
    uint8_t index,
    uint16_t /*languageID*/) {
    switch (index) {
    case 0:
        return reinterpret_cast<const StandardUSB::StringDescriptor *>(kString0Bytes);
    case 1:
        return reinterpret_cast<const StandardUSB::StringDescriptor *>(kString1Bytes);
    case 2:
        return reinterpret_cast<const StandardUSB::StringDescriptor *>(kString2Bytes);
    case 3:
        return reinterpret_cast<const StandardUSB::StringDescriptor *>(kString3Bytes);
    default:
        return nullptr;
    }
}

IOReturn UniCamVirtualUVCDevice::setConfiguration(uint8_t bConfigurationValue, bool /*matchInterfaces*/) {
    if (bConfigurationValue != 0 && bConfigurationValue != 1) {
        return kIOReturnBadArgument;
    }
    _currentConfigValue = bConfigurationValue;
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCDevice::deviceRequest(IOService * /*forClient*/,
                                               StandardUSB::DeviceRequest &request,
                                               void *dataBuffer,
                                               IOUSBHostCompletion *completion,
                                               uint32_t /*completionTimeoutMs*/) {
    uint32_t transferred = 0;
    IOReturn ret = handleDeviceRequestSync(request, dataBuffer, request.wLength, transferred);
    if (completion != nullptr && completion->action != nullptr) {
        completion->action(completion->owner, completion->parameter, ret, transferred);
    }
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCDevice::deviceRequest(IOService *forClient,
                                               StandardUSB::DeviceRequest &request,
                                               IOMemoryDescriptor *dataBuffer,
                                               IOUSBHostCompletion *completion,
                                               uint32_t completionTimeoutMs) {
    uint32_t transferred = 0;
    IOReturn ret = deviceRequest(forClient, request, dataBuffer, transferred, completionTimeoutMs);
    if (completion != nullptr && completion->action != nullptr) {
        completion->action(completion->owner, completion->parameter, ret, transferred);
    }
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCDevice::deviceRequest(IOService * /*forClient*/,
                                               StandardUSB::DeviceRequest &request,
                                               void *dataBuffer,
                                               uint32_t &bytesTransferred,
                                               uint32_t /*completionTimeoutMs*/) {
    return handleDeviceRequestSync(request, dataBuffer, request.wLength, bytesTransferred);
}

IOReturn UniCamVirtualUVCDevice::deviceRequest(IOService * /*forClient*/,
                                               StandardUSB::DeviceRequest &request,
                                               IOMemoryDescriptor *dataBuffer,
                                               uint32_t &bytesTransferred,
                                               uint32_t /*completionTimeoutMs*/) {
    if (dataBuffer == nullptr || request.wLength == 0) {
        return handleDeviceRequestSync(request, nullptr, 0, bytesTransferred);
    }
    IOMemoryMap *map = dataBuffer->map();
    if (map == nullptr) {
        return kIOReturnNoMemory;
    }
    auto *ptr = reinterpret_cast<void *>(map->getVirtualAddress());
    uint32_t cap = static_cast<uint32_t>(map->getLength());
    IOReturn ret = handleDeviceRequestSync(request, ptr, cap, bytesTransferred);
    map->release();
    return ret;
}

tUSBHostDeviceAddress UniCamVirtualUVCDevice::getAddress() const {
    return kUvcDeviceAddress;
}

uint8_t UniCamVirtualUVCDevice::getSpeed() const {
    return kIOUSBHostConnectionSpeedSuper;
}

uint64_t UniCamVirtualUVCDevice::getUvcFrameNumber(AbsoluteTime *theTime) const {
    uint64_t uptime = 0;
    clock_get_uptime(&uptime);
    if (theTime != nullptr) {
        *theTime = uptime;
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(uptime, &ns);
    return ns / 1000000ULL;
}

uint64_t UniCamVirtualUVCDevice::getFrameNumber(AbsoluteTime *theTime) const {
    return getUvcFrameNumber(theTime);
}

uint64_t UniCamVirtualUVCDevice::getFrameNumber(AbsoluteTime *theTime) {
    return getUvcFrameNumber(theTime);
}

IOReturn UniCamVirtualUVCDevice::reset() {
    return kIOReturnSuccess;
}

uint32_t UniCamVirtualUVCDevice::getPortStatus() {
    return kIOUSBHostPortStatusPortTypeInternal |
           kIOUSBHostPortStatusConnectedSpeedSuper |
           kIOUSBHostPortStatusEnabled;
}

IOBufferMemoryDescriptor *UniCamVirtualUVCDevice::createIOBuffer(IOOptionBits options,
                                                                 mach_vm_size_t capacity) {
    return IOBufferMemoryDescriptor::withOptions(options, capacity, PAGE_SIZE);
}

IOReturn UniCamVirtualUVCDevice::currentMicroframe(uint64_t &microframe,
                                                   uint64_t &microframeTime) {
    uint64_t uptime = 0;
    clock_get_uptime(&uptime);
    microframeTime = uptime;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(uptime, &ns);
    microframe = ns / 125000ULL;
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCDevice::referenceMicroframe(uint64_t &microframe,
                                                     uint64_t &microframeTime) {
    return currentMicroframe(microframe, microframeTime);
}

const StandardUSB::InterfaceDescriptor *UniCamVirtualUVCDevice::getInterfaceDescriptorFor(
    uint8_t ifaceNum,
    uint8_t altSetting) const {
    if (ifaceNum == 0 && altSetting == 0) {
        return reinterpret_cast<const StandardUSB::InterfaceDescriptor *>(
            kConfigDescriptorBytes + kOffsetIface0Alt0);
    }
    if (ifaceNum == 1 && altSetting == 0) {
        return reinterpret_cast<const StandardUSB::InterfaceDescriptor *>(
            kConfigDescriptorBytes + kOffsetIface1Alt0);
    }
    if (ifaceNum == 1 && altSetting == 1) {
        return reinterpret_cast<const StandardUSB::InterfaceDescriptor *>(
            kConfigDescriptorBytes + kOffsetIface1Alt1);
    }
    return nullptr;
}

const StandardUSB::EndpointDescriptor *UniCamVirtualUVCDevice::getEndpointDescriptorFor(
    uint8_t address) const {
    if (address == kUvcEpInterruptIn) {
        return reinterpret_cast<const StandardUSB::EndpointDescriptor *>(
            kConfigDescriptorBytes + kOffsetEp83Desc);
    }
    if (address == kUvcEpIsochIn) {
        return reinterpret_cast<const StandardUSB::EndpointDescriptor *>(
            kConfigDescriptorBytes + kOffsetEp81Desc);
    }
    return nullptr;
}

const StandardUSB::SuperSpeedEndpointCompanionDescriptor *UniCamVirtualUVCDevice::getSsCompanionDescriptorFor(
    uint8_t address) const {
    if (address == kUvcEpInterruptIn) {
        return reinterpret_cast<const StandardUSB::SuperSpeedEndpointCompanionDescriptor *>(
            kConfigDescriptorBytes + kOffsetEp83SsDesc);
    }
    if (address == kUvcEpIsochIn) {
        return reinterpret_cast<const StandardUSB::SuperSpeedEndpointCompanionDescriptor *>(
            kConfigDescriptorBytes + kOffsetEp81SsDesc);
    }
    return nullptr;
}

IOReturn UniCamVirtualUVCDevice::handleDeviceRequestSync(StandardUSB::DeviceRequest &req,
                                                         void *dataBuffer,
                                                         uint32_t bufferCapacity,
                                                         uint32_t &bytesTransferred) {
    bytesTransferred = 0;
    const uint8_t reqType = req.bmRequestType & 0x60U;
    const uint8_t recipient = req.bmRequestType & 0x1FU;
    const uint8_t ifaceNum = static_cast<uint8_t>(req.wIndex & 0xFFU);

    // Class-specific UVC request
    if (reqType == 0x20U) {
        auto *buf = static_cast<uint8_t *>(dataBuffer);
        const uint32_t cap = (req.wLength < bufferCapacity) ? req.wLength : bufferCapacity;
        if (ifaceNum == 1) {
            return handleVsControl(req, buf, cap, bytesTransferred);
        }
        return handleVcControl(req, buf, cap, bytesTransferred);
    }

    // Standard USB requests
    if (reqType == 0x00U) {
        if (req.bRequest == StandardUSB::kDeviceRequestGetDescriptor && dataBuffer != nullptr) {
            const uint8_t descType = static_cast<uint8_t>(req.wValue >> 8);
            const uint8_t descIdx = static_cast<uint8_t>(req.wValue & 0xFFU);
            const uint8_t *src = nullptr;
            uint32_t srcLen = 0;
            if (descType == StandardUSB::kDescriptorTypeDevice) {
                src = kDeviceDescriptorBytes;
                srcLen = sizeof(kDeviceDescriptorBytes);
            } else if (descType == StandardUSB::kDescriptorTypeConfiguration) {
                src = kConfigDescriptorBytes;
                srcLen = sizeof(kConfigDescriptorBytes);
            } else if (descType == StandardUSB::kDescriptorTypeString) {
                const auto *str = getStringDescriptor(descIdx, req.wIndex);
                if (str != nullptr) {
                    src = reinterpret_cast<const uint8_t *>(str);
                    srcLen = src[0];
                }
            }
            if (src != nullptr && srcLen > 0) {
                uint32_t copyLen = req.wLength;
                if (copyLen > srcLen) copyLen = srcLen;
                if (copyLen > bufferCapacity) copyLen = bufferCapacity;
                bcopy(src, dataBuffer, copyLen);
                bytesTransferred = copyLen;
                return kIOReturnSuccess;
            }
            return kIOReturnUnsupported;
        }
        if (req.bRequest == StandardUSB::kDeviceRequestSetInterface && recipient == 0x01U) {
            if (ifaceNum == 1 && vsIface_ != nullptr) {
                return vsIface_->selectAlternateSetting(static_cast<uint8_t>(req.wValue & 0xFFU));
            }
            return kIOReturnSuccess;
        }
        if (req.bRequest == StandardUSB::kDeviceRequestGetStatus && dataBuffer != nullptr && bufferCapacity >= 2) {
            bzero(dataBuffer, 2);
            bytesTransferred = 2;
            return kIOReturnSuccess;
        }
        if (req.bRequest == StandardUSB::kDeviceRequestSetConfiguration) {
            _currentConfigValue = static_cast<uint8_t>(req.wValue & 0xFFU);
            return kIOReturnSuccess;
        }
        if (req.bRequest == StandardUSB::kDeviceRequestClearFeature ||
            req.bRequest == StandardUSB::kDeviceRequestSetFeature) {
            return kIOReturnSuccess;
        }
    }

    if (debugLog_) {
        IOLog("UniCam: unhandled USB req type=0x%02x bReq=0x%02x wVal=0x%04x wIdx=0x%04x\n",
              req.bmRequestType, req.bRequest, req.wValue, req.wIndex);
    }
    return kIOReturnUnsupported;
}

IOReturn UniCamVirtualUVCDevice::handleVsControl(const StandardUSB::DeviceRequest &req,
                                                 uint8_t *buf,
                                                 uint32_t cap,
                                                 uint32_t &outLen) {
    const uint8_t cs = static_cast<uint8_t>(req.wValue >> 8);
    if (cs != kVsProbeControl && cs != kVsCommitControl) {
        return kIOReturnUnsupported;
    }

    UvcStreamingControl &target = (cs == kVsProbeControl) ? probeCtrl_ : commitCtrl_;

    if (req.bRequest == kUvcReqGetLen) {
        if (buf == nullptr || cap < 2) return kIOReturnBadArgument;
        buf[0] = sizeof(UvcStreamingControl);
        buf[1] = 0;
        outLen = 2;
        return kIOReturnSuccess;
    }
    if (req.bRequest == kUvcReqGetInfo) {
        if (buf == nullptr || cap < 1) return kIOReturnBadArgument;
        buf[0] = 0x03; // GET and SET supported
        outLen = 1;
        return kIOReturnSuccess;
    }
    if (req.bRequest == kUvcReqSetCur) {
        if (buf == nullptr || cap < 26) return kIOReturnBadArgument;
        UvcStreamingControl reqCtrl = target;
        bcopy(buf, &reqCtrl, sizeof(UvcStreamingControl));
        if (reqCtrl.bFrameIndex < 1 || reqCtrl.bFrameIndex > 3) {
            reqCtrl.bFrameIndex = 1;
        }
        reqCtrl.bFormatIndex = 1;
        reqCtrl.dwFrameInterval = 333333;
        reqCtrl.wDelay = 33;
        reqCtrl.dwMaxVideoFrameSize = frameSizeForIndex(reqCtrl.bFrameIndex);
        reqCtrl.dwMaxPayloadTransferSize = kUvcMaxPayloadSize;

        target = reqCtrl;
        if (cs == kVsCommitControl && delegate_ != nullptr &&
            vsIface_ != nullptr && vsIface_->activeAltSetting() == 1) {
            delegate_->onUvcStreamingChanged(true,
                                             commitCtrl_.bFormatIndex,
                                             commitCtrl_.bFrameIndex,
                                             commitCtrl_.dwFrameInterval);
        }
        outLen = 26;
        return kIOReturnSuccess;
    }

    // GET_CUR / GET_MIN / GET_MAX / GET_RES / GET_DEF
    if (buf == nullptr || cap < 26) {
        return kIOReturnBadArgument;
    }
    UvcStreamingControl rsp = (req.bRequest == kUvcReqGetDef) ? UvcStreamingControl{
        0x0001, 1, 1, 333333, 0, 0, 0, 0, 33, frameSizeForIndex(1), kUvcMaxPayloadSize
    } : target;

    if (req.bRequest == kUvcReqGetMin) {
        rsp.bFrameIndex = 1;
        rsp.dwFrameInterval = 333333;
        rsp.dwMaxVideoFrameSize = frameSizeForIndex(3);
    } else if (req.bRequest == kUvcReqGetMax) {
        rsp.bFrameIndex = 3;
        rsp.dwFrameInterval = 333333;
        rsp.dwMaxVideoFrameSize = frameSizeForIndex(2);
    }
    bcopy(&rsp, buf, sizeof(UvcStreamingControl));
    outLen = sizeof(UvcStreamingControl);
    return kIOReturnSuccess;
}

IOReturn UniCamVirtualUVCDevice::handleVcControl(const StandardUSB::DeviceRequest &req,
                                                 uint8_t *buf,
                                                 uint32_t cap,
                                                 uint32_t &outLen) {
    const uint8_t unitId = static_cast<uint8_t>(req.wIndex >> 8);
    const uint8_t cs = static_cast<uint8_t>(req.wValue >> 8);

    auto writeVal = [&](uint32_t len, int32_t v) -> IOReturn {
        if (buf == nullptr || cap < len) return kIOReturnBadArgument;
        for (uint32_t i = 0; i < len; ++i) {
            buf[i] = static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8U * i)) & 0xFFU);
        }
        outLen = len;
        return kIOReturnSuccess;
    };

    if (unitId == 0 && cs == kVcRequestErrorCodeControl) {
        return writeVal(1, vcRequestError_);
    }

    if (req.bRequest == kUvcReqGetInfo) {
        return writeVal(1, 0x03); // GET + SET
    }

    if (unitId == 1) { // Input Terminal (Camera)
        if (cs == kCtAeModeControl) {
            if (req.bRequest == kUvcReqGetLen) return writeVal(2, 1);
            if (req.bRequest == kUvcReqSetCur && buf != nullptr && cap >= 1) {
                aeMode_ = buf[0];
                if (delegate_) delegate_->onUvcControlUpdated(unitId, cs, aeMode_);
                outLen = 1;
                return kIOReturnSuccess;
            }
            if (req.bRequest == kUvcReqGetRes) return writeVal(1, 0x0A); // Manual (1) + Auto (2) + Shutter (4) + Aperture (8)
            return writeVal(1, (req.bRequest == kUvcReqGetCur) ? aeMode_ : 0x08);
        }
        if (cs == kCtExposureTimeAbsControl) {
            if (req.bRequest == kUvcReqGetLen) return writeVal(2, 4);
            if (req.bRequest == kUvcReqSetCur && buf != nullptr && cap >= 4) {
                exposureAbs_ = static_cast<uint32_t>(buf[0]) |
                               (static_cast<uint32_t>(buf[1]) << 8) |
                               (static_cast<uint32_t>(buf[2]) << 16) |
                               (static_cast<uint32_t>(buf[3]) << 24);
                if (delegate_) delegate_->onUvcControlUpdated(unitId, cs, static_cast<int32_t>(exposureAbs_));
                outLen = 4;
                return kIOReturnSuccess;
            }
            if (req.bRequest == kUvcReqGetMin) return writeVal(4, 1);
            if (req.bRequest == kUvcReqGetMax) return writeVal(4, 1000);
            if (req.bRequest == kUvcReqGetRes) return writeVal(4, 1);
            if (req.bRequest == kUvcReqGetDef) return writeVal(4, 166);
            return writeVal(4, static_cast<int32_t>(exposureAbs_));
        }
    } else if (unitId == 2) { // Processing Unit
        auto handlePu16 = [&](int32_t &cur, int32_t minV, int32_t maxV, int32_t resV, int32_t defV, bool is1Byte) -> IOReturn {
            const uint32_t sz = is1Byte ? 1U : 2U;
            if (req.bRequest == kUvcReqGetLen) return writeVal(2, static_cast<int32_t>(sz));
            if (req.bRequest == kUvcReqSetCur && buf != nullptr && cap >= sz) {
                if (is1Byte) {
                    cur = buf[0];
                } else {
                    cur = static_cast<int16_t>(readLe16(buf));
                }
                if (delegate_) delegate_->onUvcControlUpdated(unitId, cs, cur);
                outLen = sz;
                return kIOReturnSuccess;
            }
            switch (req.bRequest) {
            case kUvcReqGetMin: return writeVal(sz, minV);
            case kUvcReqGetMax: return writeVal(sz, maxV);
            case kUvcReqGetRes: return writeVal(sz, resV);
            case kUvcReqGetDef: return writeVal(sz, defV);
            default:            return writeVal(sz, cur);
            }
        };

        int32_t tmp = 0;
        switch (cs) {
        case kPuBacklightCompControl:
            tmp = puBacklightComp_;
            { IOReturn r = handlePu16(tmp, 0, 2, 1, 1, false); puBacklightComp_ = static_cast<uint16_t>(tmp); return r; }
        case kPuBrightnessControl:
            tmp = puBrightness_;
            { IOReturn r = handlePu16(tmp, -64, 64, 1, 0, false); puBrightness_ = static_cast<int16_t>(tmp); return r; }
        case kPuContrastControl:
            tmp = puContrast_;
            { IOReturn r = handlePu16(tmp, 0, 64, 1, 32, false); puContrast_ = static_cast<uint16_t>(tmp); return r; }
        case kPuHueControl:
            tmp = puHue_;
            { IOReturn r = handlePu16(tmp, -180, 180, 1, 0, false); puHue_ = static_cast<int16_t>(tmp); return r; }
        case kPuSaturationControl:
            tmp = puSaturation_;
            { IOReturn r = handlePu16(tmp, 0, 128, 1, 32, false); puSaturation_ = static_cast<uint16_t>(tmp); return r; }
        case kPuSharpnessControl:
            tmp = puSharpness_;
            { IOReturn r = handlePu16(tmp, 0, 64, 1, 16, false); puSharpness_ = static_cast<uint16_t>(tmp); return r; }
        case kPuGammaControl:
            tmp = puGamma_;
            { IOReturn r = handlePu16(tmp, 100, 300, 10, 220, false); puGamma_ = static_cast<uint16_t>(tmp); return r; }
        case kPuWbTempControl:
            tmp = puWbTemp_;
            { IOReturn r = handlePu16(tmp, 2800, 6500, 100, 5500, false); puWbTemp_ = static_cast<uint16_t>(tmp); return r; }
        case kPuWbAutoControl:
            tmp = puWbAuto_;
            { IOReturn r = handlePu16(tmp, 0, 1, 1, 1, true); puWbAuto_ = static_cast<uint8_t>(tmp); return r; }
        case kPuWbComponentControl:
            if (req.bRequest == kUvcReqGetLen) return writeVal(2, 4);
            if (req.bRequest == kUvcReqSetCur && buf != nullptr && cap >= 4) {
                puWbComponent_ = static_cast<uint32_t>(buf[0]) |
                                 (static_cast<uint32_t>(buf[1]) << 8) |
                                 (static_cast<uint32_t>(buf[2]) << 16) |
                                 (static_cast<uint32_t>(buf[3]) << 24);
                outLen = 4;
                return kIOReturnSuccess;
            }
            if (req.bRequest == kUvcReqGetMin) return writeVal(4, 0x00010001);
            if (req.bRequest == kUvcReqGetMax) return writeVal(4, 0x00FF00FF);
            if (req.bRequest == kUvcReqGetRes) return writeVal(4, 0x00010001);
            if (req.bRequest == kUvcReqGetDef) return writeVal(4, 0x00400040);
            return writeVal(4, static_cast<int32_t>(puWbComponent_));
        case kPuWbComponentAutoControl:
            tmp = puWbComponentAuto_;
            { IOReturn r = handlePu16(tmp, 0, 1, 1, 1, true); puWbComponentAuto_ = static_cast<uint8_t>(tmp); return r; }
        case kPuHueAutoControl:
            tmp = puHueAuto_;
            { IOReturn r = handlePu16(tmp, 0, 1, 1, 1, true); puHueAuto_ = static_cast<uint8_t>(tmp); return r; }
        case kPuGainControl:
            tmp = puGain_;
            { IOReturn r = handlePu16(tmp, 0, 255, 1, 16, false); puGain_ = static_cast<uint16_t>(tmp); return r; }
        case kPuPowerLineFreqControl:
            tmp = puPowerLineFreq_;
            { IOReturn r = handlePu16(tmp, 0, 2, 1, 1, true); puPowerLineFreq_ = static_cast<uint8_t>(tmp); return r; }
        default:
            break;
        }
    }

    vcRequestError_ = 0x06; // Invalid control
    return kIOReturnUnsupported;
}

void UniCamVirtualUVCDevice::notifyAltSettingChanged(uint8_t ifaceNum, uint8_t altSetting) {
    if (ifaceNum != 1 || delegate_ == nullptr) {
        return;
    }
    const bool active = (altSetting == 1);
    delegate_->onUvcStreamingChanged(active,
                                     commitCtrl_.bFormatIndex,
                                     commitCtrl_.bFrameIndex,
                                     commitCtrl_.dwFrameInterval);
}

bool UniCamVirtualUVCDevice::submitYuy2Frame(const uint8_t *yuy2Data, uint32_t byteCount) {
    if (vsIface_ == nullptr || vsIface_->activeAltSetting() != 1) {
        return false;
    }
    UniCamVirtualUVCPipe *pipe = vsIface_->videoStreamPipe();
    if (pipe == nullptr) {
        return false;
    }
    const uint32_t pts = current48MhzTicks();
    return pipe->deliverYuy2Frame(yuy2Data, byteCount, streamFid_, pts, scrSof_);
}
