#include "UniCamBridgeService.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

extern "C" {
#include <kern/clock.h>
}

#define super IOService
OSDefineMetaClassAndStructors(UniCamBridgeService, IOService)

namespace {
constexpr uint32_t kDiscoveryIntervalMs = 250;
constexpr uint32_t kMaxDiscoveryTicks = 40; // 10s
constexpr uint32_t kMaxYuy2BufferBytes = 1280U * 800U * 2U;
}

bool UniCamBridgeService::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }
    int dummy = 0;
    disabledByBootArg_ = PE_parse_boot_argn("-unicamoff", &dummy, sizeof(dummy));
    debugLog_ = PE_parse_boot_argn("-unicamdbg", &dummy, sizeof(dummy));
    betaBootArg_ = PE_parse_boot_argn("-unicambeta", &dummy, sizeof(dummy));
    return true;
}

bool UniCamBridgeService::start(IOService *provider) {
    if (disabledByBootArg_) {
        IOLog("UniCam: disabled via -unicamoff boot-arg\n");
        return false;
    }
    if (!super::start(provider)) {
        return false;
    }

    stateLock_ = IOLockAlloc();
    if (stateLock_ == nullptr) {
        return false;
    }

    yuy2FrameBuffer_ = static_cast<uint8_t *>(IOMalloc(kMaxYuy2BufferBytes));
    if (yuy2FrameBuffer_ == nullptr) {
        return false;
    }
    yuy2FrameCapacity_ = kMaxYuy2BufferBytes;
    bzero(yuy2FrameBuffer_, yuy2FrameCapacity_);

    workLoop_ = IOWorkLoop::workLoop();
    if (workLoop_ == nullptr) {
        return false;
    }

    discoveryTimer_ = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(IOTimerEventSource::Action,
                             this,
                             &UniCamBridgeService::onDiscoveryTimer));
    if (discoveryTimer_ == nullptr ||
        workLoop_->addEventSource(discoveryTimer_) != kIOReturnSuccess) {
        return false;
    }

    IOLog("UniCam: bridge initialized (Pure-EFI Zero-Install%s%s)\n",
          debugLog_ ? ", -unicamdbg" : "",
          betaBootArg_ ? ", -unicambeta" : "");

    tryAttachServicesAndPublishUvc();
    if (!uvcAttached_) {
        discoveryTimer_->setTimeoutMS(kDiscoveryIntervalMs);
    }

    updateRegistryStatus();
    registerService();
    return true;
}

void UniCamBridgeService::stop(IOService *provider) {
    stopping_ = true;

    if (discoveryTimer_ != nullptr) {
        discoveryTimer_->cancelTimeout();
        if (workLoop_ != nullptr) {
            workLoop_->removeEventSource(discoveryTimer_);
        }
        OSSafeReleaseNULL(discoveryTimer_);
    }

    stopWorkerThread();

    if (stateLock_ != nullptr) {
        IOLockLock(stateLock_);
        stopPipelineLocked();
        IOLockUnlock(stateLock_);
    }

    if (uvcDevice_ != nullptr) {
        uvcDevice_->terminate();
        uvcDevice_->stop(this);
        uvcDevice_->detach(this);
        OSSafeReleaseNULL(uvcDevice_);
        uvcAttached_ = false;
    }

    OSSafeReleaseNULL(workLoop_);
    super::stop(provider);
}

void UniCamBridgeService::free() {
    if (yuy2FrameBuffer_ != nullptr) {
        IOFree(yuy2FrameBuffer_, yuy2FrameCapacity_);
        yuy2FrameBuffer_ = nullptr;
        yuy2FrameCapacity_ = 0;
    }
    if (stateLock_ != nullptr) {
        IOLockFree(stateLock_);
        stateLock_ = nullptr;
    }
    coreService_ = nullptr;
    ipuService_ = nullptr;
    super::free();
}

void UniCamBridgeService::onDiscoveryTimer(IOTimerEventSource * /*sender*/) {
    if (stopping_ || uvcAttached_) {
        return;
    }
    discoveryTicks_++;
    tryAttachServicesAndPublishUvc();
    updateRegistryStatus();
    if (!uvcAttached_ && discoveryTicks_ < kMaxDiscoveryTicks && discoveryTimer_ != nullptr) {
        discoveryTimer_->setTimeoutMS(kDiscoveryIntervalMs);
    }
}

void UniCamBridgeService::tryAttachServicesAndPublishUvc() {
    if (coreService_ == nullptr) {
        coreService_ = UniCamCoreService::sharedInstance();
    }
    if (ipuService_ == nullptr) {
        ipuService_ = UniCamIPUService::sharedInstance();
    }

    if (coreService_ != nullptr && sensorRotation_ == 0) {
        UniCamBoardSensorInfo info{};
        if (coreService_->getInfo(&info) == kIOReturnSuccess) {
            sensorRotation_ = info.rotation;
        }
    }

    if (uvcAttached_) {
        return;
    }
    // Publish virtual UVC device once both UniCamCore & UniCamIPU are online,
    // or after initial settle window if IPU/Core is partially active.
    if ((coreService_ == nullptr || ipuService_ == nullptr) &&
        discoveryTicks_ < 12) {
        return;
    }

    uvcDevice_ = UniCamVirtualUVCDevice::createWithDelegate(this, debugLog_);
    if (uvcDevice_ == nullptr) {
        return;
    }
    if (!uvcDevice_->attach(this) || !uvcDevice_->start(this)) {
        OSSafeReleaseNULL(uvcDevice_);
        return;
    }
    uvcDevice_->registerService();
    uvcAttached_ = true;
    IOLog("UniCam: attached virtual Integrated Webcam HD (0x0C45:0x6723) core=%d ipu=%d rot=%u\n",
          coreService_ != nullptr ? 1 : 0,
          ipuService_ != nullptr ? 1 : 0,
          sensorRotation_);
}

void UniCamBridgeService::updateRegistryStatus() {
    const char *statusStr = "waiting_dependencies";
    if (streamingActive_) {
        statusStr = "streaming";
    } else if (uvcAttached_ && coreService_ != nullptr && ipuService_ != nullptr) {
        statusStr = "ready";
    } else if (uvcAttached_) {
        statusStr = "ready_degraded";
    }

    setProperty("UniCamStatus", statusStr);
    setProperty("UniCamCoreReady", (coreService_ != nullptr) ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamIPUReady", (ipuService_ != nullptr) ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamUvcAttached", uvcAttached_ ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamStreaming", streamingActive_ ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamWidth", streamWidth_, 32);
    setProperty("UniCamHeight", streamHeight_, 32);
    setProperty("UniCamFps", streamFps_, 32);
    setProperty("UniCamHwFrames", hwFramesDelivered_, 64);
    setProperty("UniCamFallbackFrames", fallbackFramesDelivered_, 64);
}

void UniCamBridgeService::onUvcStreamingChanged(bool active,
                                              uint8_t /*formatIndex*/,
                                              uint8_t frameIndex,
                                              uint32_t /*frameInterval100ns*/) {
    uint32_t targetW = 1280;
    uint32_t targetH = 720;
    if (frameIndex == 2) {
        targetW = 1280;
        targetH = 800;
    } else if (frameIndex == 3) {
        targetW = 640;
        targetH = 480;
    }

    IOLog("UniCam: UVC stream %s (%ux%u@30)\n",
          active ? "START" : "STOP", targetW, targetH);

    if (active) {
        const bool dimChanged = (targetW != streamWidth_ || targetH != streamHeight_);
        if (workerRunning_ && dimChanged) {
            stopWorkerThread();
            if (stateLock_ != nullptr) {
                IOLockLock(stateLock_);
                stopPipelineLocked();
                IOLockUnlock(stateLock_);
            }
        }
        streamWidth_ = targetW;
        streamHeight_ = targetH;
        streamingActive_ = true;
        if (!workerRunning_) {
            if (stateLock_ != nullptr) {
                IOLockLock(stateLock_);
                startPipelineLocked();
                IOLockUnlock(stateLock_);
            }
            workerStopRequested_ = false;
            kern_return_t kr = kernel_thread_start(UniCamBridgeService::workerThreadEntry,
                                                   this,
                                                   &workerThread_);
            if (kr == KERN_SUCCESS) {
                workerRunning_ = true;
            }
        }
    } else {
        streamingActive_ = false;
        stopWorkerThread();
        if (stateLock_ != nullptr) {
            IOLockLock(stateLock_);
            stopPipelineLocked();
            IOLockUnlock(stateLock_);
        }
    }
    updateRegistryStatus();
}

void UniCamBridgeService::onUvcControlUpdated(uint8_t unitOrTerminalId,
                                            uint8_t controlSelector,
                                            int32_t value) {
    if (debugLog_) {
        IOLog("UniCam: UVC ctrl unit=%u sel=%u val=%d\n",
              unitOrTerminalId, controlSelector, value);
    }
    if (coreService_ == nullptr) {
        return;
    }
    // Map Exposure / Gain manual updates to sensor CCI when streaming
    if (unitOrTerminalId == 1 && controlSelector == 0x04 && value > 0) {
        // 100us -> OV01A10 lines (~35us/line -> lines = val * 3)
        uint32_t lines = static_cast<uint32_t>(value) * 3U;
        if (lines < 8) lines = 8;
        if (lines > 900) lines = 900;
        coreService_->setExposureGain(lines, 16, 256);
    } else if (unitOrTerminalId == 2 && controlSelector == 0x04 && value >= 0) {
        uint32_t gain = static_cast<uint32_t>(value);
        if (gain < 16) gain = 16;
        if (gain > 248) gain = 248;
        coreService_->setExposureGain(450, gain, 256);
    }
}

IOReturn UniCamBridgeService::startPipelineLocked(uint32_t shiftedDataTicks) {
    if (coreService_ == nullptr) {
        coreService_ = UniCamCoreService::sharedInstance();
    }
    if (ipuService_ == nullptr) {
        ipuService_ = UniCamIPUService::sharedInstance();
    }

    UniCamBoardSensorInfo sensorInfo{};
    bool haveSensorInfo = false;
    if (coreService_ != nullptr) {
        if (coreService_->getInfo(&sensorInfo) == kIOReturnSuccess) {
            haveSensorInfo = true;
            sensorRotation_ = sensorInfo.rotation;
        }
        coreService_->setPower(true, false, nullptr);
        coreService_->setPrivacyLed(true, false, nullptr);
        coreService_->configureSensor(0, 0);
    }

    if (ipuService_ != nullptr) {
        ipuService_->resetIsp3A();
        UniCamIpuStreamConfig cfg{};
        cfg.width = (haveSensorInfo && sensorInfo.sensorWidth != 0U) ? sensorInfo.sensorWidth : 1280U;
        cfg.height = (haveSensorInfo && sensorInfo.sensorHeight != 0U) ? sensorInfo.sensorHeight : 800U;
        cfg.pixelFormat = kUniCamIpuFormatRaw10Packed;
        cfg.csiPort = (haveSensorInfo && sensorInfo.csiPort != 0U) ? sensorInfo.csiPort : 6U;
        cfg.csiLanes = (haveSensorInfo && sensorInfo.csiLanes != 0U) ? sensorInfo.csiLanes : 1U;
        cfg.vc = 0;
        cfg.linkFreqHz = (haveSensorInfo && sensorInfo.linkFreqHz != 0ULL) ? sensorInfo.linkFreqHz : 400000000ULL;
        cfg.shiftedDataTicks = shiftedDataTicks;
        UniCamIpuStreamStatus st{};
        IOReturn ipuRet = ipuService_->startStream(&cfg, &st, nullptr);
        if (ipuRet != kIOReturnSuccess) {
            IOLog("UniCam: ipu startStream failed 0x%x (will stream diagnostic pattern on timeout)\n",
                  ipuRet);
        }
    }

    if (coreService_ != nullptr) {
        coreService_->setStream(true, nullptr);
    }
    return kIOReturnSuccess;
}

void UniCamBridgeService::stopPipelineLocked() {
    if (coreService_ != nullptr) {
        coreService_->setStream(false, nullptr);
    }
    if (ipuService_ != nullptr) {
        ipuService_->stopStream(nullptr);
    }
    if (coreService_ != nullptr) {
        coreService_->setPrivacyLed(false, false, nullptr);
        coreService_->setPower(false, false, nullptr);
    }
}

void UniCamBridgeService::stopWorkerThread() {
    if (!workerRunning_) {
        return;
    }
    workerStopRequested_ = true;
    for (int i = 0; i < 60 && workerRunning_; ++i) {
        IOSleep(10);
    }
    if (workerThread_ != THREAD_NULL) {
        thread_deallocate(workerThread_);
        workerThread_ = THREAD_NULL;
    }
}

void UniCamBridgeService::workerThreadEntry(void *parameter, wait_result_t /*waitResult*/) {
    auto *self = static_cast<UniCamBridgeService *>(parameter);
    if (self != nullptr) {
        self->runStreamWorker();
    }
    thread_terminate(current_thread());
}

void UniCamBridgeService::runStreamWorker() {
    uint32_t frameCounter = 0;
    uint32_t consecutiveTimeouts = 0;
    bool triedShiftedTiming = false;

    while (!workerStopRequested_ && !stopping_) {
        uint64_t loopStartAbs = 0;
        uint64_t loopStartNs = 0;
        clock_get_uptime(&loopStartAbs);
        absolutetime_to_nanoseconds(loopStartAbs, &loopStartNs);

        const uint32_t w = streamWidth_;
        const uint32_t h = streamHeight_;
        const uint32_t yuy2Bytes = w * h * 2U;
        if (yuy2FrameBuffer_ == nullptr || yuy2Bytes > yuy2FrameCapacity_) {
            IOSleep(16);
            continue;
        }

        bool haveFrame = false;
        bool inFallback = false;
        if (ipuService_ != nullptr) {
            UniCamIpuFrameEvent ev{};
            IOReturn waitRet = ipuService_->waitNextFrame(50, &ev);
            if (waitRet == kIOReturnSuccess) {
                const uint32_t bayerOrder = (coreService_ != nullptr) ? coreService_->activeBayerOrder() : 0U;
                UniCamIspFrameStats stats{};
                IOReturn ispRet = ipuService_->processSlotToYuy2(
                    ev.slotIndex,
                    bayerOrder,
                    sensorRotation_,
                    yuy2FrameBuffer_,
                    w,
                    h,
                    yuy2FrameCapacity_,
                    &stats);
                ipuService_->requeueSlot(ev.slotIndex);
                if (ispRet == kIOReturnSuccess) {
                    haveFrame = true;
                    consecutiveTimeouts = 0;
                    hwFramesDelivered_++;
                    if (stats.aeUpdated != 0 && coreService_ != nullptr) {
                        coreService_->setExposureGain(stats.exposureLines,
                                                      stats.analogGain,
                                                      1024U);
                    }
                }
            } else {
                consecutiveTimeouts++;
                if (!triedShiftedTiming && hwFramesDelivered_ == 0 && consecutiveTimeouts == 10 &&
                    stateLock_ != nullptr) {
                    triedShiftedTiming = true;
                    IOLog("UniCam: no CSI-2 frame after 500ms, retrying with shiftedTicks=1280\n");
                    IOLockLock(stateLock_);
                    stopPipelineLocked();
                    startPipelineLocked(1280U);
                    IOLockUnlock(stateLock_);
                }
            }
        } else {
            consecutiveTimeouts++;
        }

        if (!haveFrame && consecutiveTimeouts >= 12) {
            if (ipuService_ != nullptr) {
                ipuService_->generateTestPatternYuy2(yuy2FrameBuffer_, w, h, frameCounter);
            } else {
                // Minimal neutral YUY2 frame if IPU service unavailable
                for (uint32_t i = 0; i + 1 < yuy2Bytes; i += 2) {
                    yuy2FrameBuffer_[i] = 0x50;
                    yuy2FrameBuffer_[i + 1] = 0x80;
                }
            }
            haveFrame = true;
            inFallback = true;
            fallbackFramesDelivered_++;
        }

        if (haveFrame && uvcDevice_ != nullptr) {
            uvcDevice_->submitYuy2Frame(yuy2FrameBuffer_, yuy2Bytes);
            frameCounter++;
        }

        if (inFallback && !workerStopRequested_ && !stopping_) {
            uint64_t loopEndAbs = 0;
            uint64_t loopEndNs = 0;
            clock_get_uptime(&loopEndAbs);
            absolutetime_to_nanoseconds(loopEndAbs, &loopEndNs);
            const uint64_t elapsedMs = (loopEndNs > loopStartNs)
                ? ((loopEndNs - loopStartNs) / 1000000ULL)
                : 0ULL;
            if (elapsedMs < 33ULL) {
                IOSleep(static_cast<UInt32>(33ULL - elapsedMs));
            }
        }
    }

    workerRunning_ = false;
}
