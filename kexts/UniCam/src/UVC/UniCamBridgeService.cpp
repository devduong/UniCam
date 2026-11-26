#include "UniCamBridgeService.hpp"
#include "UniCamKernelGate.h"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

extern "C" {
#include <kern/clock.h>
}

#define super IOService
OSDefineMetaClassAndStructors(UniCamBridgeService, IOService)

namespace {
constexpr uint32_t kDiscoveryIntervalMs = 250;
constexpr uint32_t kMaxDiscoveryTicks = 40; // 10s
constexpr uint32_t kDegradedDiscoveryTicks = 12; // 3s
constexpr uint32_t kMaxYuy2BufferBytes = 1280U * 800U * 2U;
}

bool UniCamBridgeService::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }
    disabledByBootArg_ = unicam::hasBootArg("-unicamoff");
    debugLog_ = unicam::hasBootArg("-unicamdbg");
    testMode_ = unicam::hasBootArg("-unicamtest");
    betaBootArg_ = unicam::hasBootArg("-unicambeta");
    return true;
}

bool UniCamBridgeService::start(IOService *provider) {
    if (!unicam::isKernelSupportedForLoad("UniCam")) {
        return false;
    }
    if (!super::start(provider)) {
        return false;
    }

    stateLock_ = IOLockAlloc();
    workerLock_ = IOLockAlloc();
    streamCtrlLock_ = IOLockAlloc();
    if (stateLock_ == nullptr || workerLock_ == nullptr || streamCtrlLock_ == nullptr) {
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

    IOLog("UniCam: bridge initialized (Pure-EFI Zero-Install%s%s%s)\n",
          debugLog_ ? ", -unicamdbg" : "",
          testMode_ ? ", -unicamtest" : "",
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

    if (streamCtrlLock_ != nullptr) {
        IOLockLock(streamCtrlLock_);
    }
    streamingActive_ = false;
    stopWorkerThread();

    if (stateLock_ != nullptr) {
        IOLockLock(stateLock_);
        stopPipelineLocked();
        OSSafeReleaseNULL(coreService_);
        OSSafeReleaseNULL(ipuService_);
        IOLockUnlock(stateLock_);
    } else {
        OSSafeReleaseNULL(coreService_);
        OSSafeReleaseNULL(ipuService_);
    }
    if (streamCtrlLock_ != nullptr) {
        IOLockUnlock(streamCtrlLock_);
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
    if (workerLock_ != nullptr) {
        IOLockFree(workerLock_);
        workerLock_ = nullptr;
    }
    if (streamCtrlLock_ != nullptr) {
        IOLockFree(streamCtrlLock_);
        streamCtrlLock_ = nullptr;
    }
    OSSafeReleaseNULL(coreService_);
    OSSafeReleaseNULL(ipuService_);
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

void UniCamBridgeService::refreshServicesLocked() {
    if (coreService_ != nullptr && coreService_->isInactive()) {
        OSSafeReleaseNULL(coreService_);
    }
    if (coreService_ == nullptr) {
        coreService_ = UniCamCoreService::copySharedInstance();
    }
    if (ipuService_ != nullptr && ipuService_->isInactive()) {
        OSSafeReleaseNULL(ipuService_);
    }
    if (ipuService_ == nullptr) {
        ipuService_ = UniCamIPUService::copySharedInstance();
    }
}

void UniCamBridgeService::copyActiveServices(UniCamCoreService **outCore, UniCamIPUService **outIpu) {
    UniCamCoreService *core = nullptr;
    UniCamIPUService *ipu = nullptr;
    if (stateLock_ != nullptr) {
        IOLockLock(stateLock_);
        refreshServicesLocked();
        if (coreService_ != nullptr && !coreService_->isInactive()) {
            core = coreService_;
            core->retain();
        }
        if (ipuService_ != nullptr && !ipuService_->isInactive()) {
            ipu = ipuService_;
            ipu->retain();
        }
        IOLockUnlock(stateLock_);
    }
    if (outCore != nullptr) {
        *outCore = core;
    } else if (core != nullptr) {
        core->release();
    }
    if (outIpu != nullptr) {
        *outIpu = ipu;
    } else if (ipu != nullptr) {
        ipu->release();
    }
}

void UniCamBridgeService::tryAttachServicesAndPublishUvc() {
    if (stopping_ || uvcAttached_ || stateLock_ == nullptr) {
        return;
    }

    bool ipuValid = false;
    bool coreValid = false;
    bool sensorSupported = false;

    IOLockLock(stateLock_);
    refreshServicesLocked();
    ipuValid = (ipuService_ != nullptr && !ipuService_->isInactive());
    coreValid = (coreService_ != nullptr && !coreService_->isInactive());
    if (coreValid) {
        sensorSupported = coreService_->hasSupportedSensorDriver();
        if (sensorRotation_ == 0U) {
            UniCamBoardSensorInfo info{};
            if (coreService_->getInfo(&info) == kIOReturnSuccess) {
                sensorRotation_ = info.rotation;
            }
        }
    }
    IOLockUnlock(stateLock_);

    const bool fullHwReady = (ipuValid && sensorSupported);
    const bool allowDiagUvc = testMode_ || (debugLog_ && ipuValid);

    if (!fullHwReady) {
        if (!allowDiagUvc || discoveryTicks_ < kDegradedDiscoveryTicks) {
            if (discoveryTicks_ >= kMaxDiscoveryTicks && !loggedDormant_) {
                loggedDormant_ = true;
                IOLog("UniCam: remaining dormant without Virtual UVC (ipu=%d core=%d sensorSupported=%d; add -unicamdbg on IPU4P or -unicamtest to force diagnostic UVC)\n",
                      ipuValid ? 1 : 0, coreValid ? 1 : 0, sensorSupported ? 1 : 0);
            }
            return;
        }
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
    IOLog("UniCam: attached virtual Integrated Webcam HD (0x0C45:0x6723) mode=%s core=%d sensor=%d ipu=%d rot=%u\n",
          fullHwReady ? "hardware" : (testMode_ ? "test" : "degraded_dbg"),
          coreValid ? 1 : 0,
          sensorSupported ? 1 : 0,
          ipuValid ? 1 : 0,
          sensorRotation_);
}

void UniCamBridgeService::updateRegistryStatus() {
    bool coreValid = false;
    bool sensorSupported = false;
    bool ipuValid = false;
    if (stateLock_ != nullptr) {
        IOLockLock(stateLock_);
        coreValid = (coreService_ != nullptr && !coreService_->isInactive());
        sensorSupported = (coreValid && coreService_->hasSupportedSensorDriver());
        ipuValid = (ipuService_ != nullptr && !ipuService_->isInactive());
        IOLockUnlock(stateLock_);
    }

    const char *statusStr = "waiting_dependencies";
    if (streamingActive_) {
        statusStr = "streaming";
    } else if (uvcAttached_ && ipuValid && sensorSupported) {
        statusStr = "ready";
    } else if (uvcAttached_) {
        statusStr = "ready_degraded";
    } else if (discoveryTicks_ >= kMaxDiscoveryTicks) {
        if (!ipuValid) {
            statusStr = "dormant_no_ipu4p";
        } else if (!coreValid) {
            statusStr = "dormant_missing_ssdt";
        } else {
            statusStr = "dormant_unsupported_sensor";
        }
    }

    setProperty("UniCamStatus", statusStr);
    setProperty("UniCamCoreReady", coreValid ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamSensorSupported", sensorSupported ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("UniCamIPUReady", ipuValid ? kOSBooleanTrue : kOSBooleanFalse);
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
    if (stopping_ || streamCtrlLock_ == nullptr) {
        return;
    }

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

    IOLockLock(streamCtrlLock_);
    if (stopping_) {
        IOLockUnlock(streamCtrlLock_);
        return;
    }

    if (active) {
        bool running = false;
        if (workerLock_ != nullptr) {
            IOLockLock(workerLock_);
            running = workerRunning_;
            IOLockUnlock(workerLock_);
        }
        const bool dimChanged = (targetW != streamWidth_ || targetH != streamHeight_);
        if (running && dimChanged) {
            stopWorkerThread();
            if (stateLock_ != nullptr) {
                IOLockLock(stateLock_);
                stopPipelineLocked();
                IOLockUnlock(stateLock_);
            }
            running = false;
        }
        streamWidth_ = targetW;
        streamHeight_ = targetH;
        streamingActive_ = true;
        (void)waitForHardwareAwake(3000U);
        if (!stopping_ && !running) {
            if (stateLock_ != nullptr) {
                IOLockLock(stateLock_);
                startPipelineLocked();
                IOLockUnlock(stateLock_);
            }
            startWorkerThreadLocked();
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
    IOLockUnlock(streamCtrlLock_);
    updateRegistryStatus();
}

void UniCamBridgeService::onUvcControlUpdated(uint8_t unitOrTerminalId,
                                            uint8_t controlSelector,
                                            int32_t value) {
    if (debugLog_) {
        IOLog("UniCam: UVC ctrl unit=%u sel=%u val=%d\n",
              unitOrTerminalId, controlSelector, value);
    }
    if (stopping_) {
        return;
    }
    UniCamCoreService *core = nullptr;
    copyActiveServices(&core, nullptr);
    if (core == nullptr) {
        return;
    }
    if (core->hasSupportedSensorDriver()) {
        // Map Exposure / Gain manual updates to sensor CCI when streaming
        if (unitOrTerminalId == 1 && controlSelector == 0x04 && value > 0) {
            // 100us -> OV01A10 lines (~35us/line -> lines = val * 3)
            uint32_t lines = static_cast<uint32_t>(value) * 3U;
            if (lines < 8) lines = 8;
            if (lines > 900) lines = 900;
            core->setExposureGain(lines, 16, 256);
        } else if (unitOrTerminalId == 2 && controlSelector == 0x04 && value >= 0) {
            uint32_t gain = static_cast<uint32_t>(value);
            if (gain < 16) gain = 16;
            if (gain > 248) gain = 248;
            core->setExposureGain(450, gain, 256);
        }
    }
    core->release();
}

bool UniCamBridgeService::isHardwareSleepSuspendedLocked() const {
    return (coreService_ != nullptr && coreService_->isSleepSuspended()) ||
           (ipuService_ != nullptr && ipuService_->isSleepSuspended());
}

bool UniCamBridgeService::isHardwareSleepSuspended() {
    if (stateLock_ == nullptr) {
        return true;
    }
    IOLockLock(stateLock_);
    const bool suspended = isHardwareSleepSuspendedLocked();
    IOLockUnlock(stateLock_);
    return suspended;
}

bool UniCamBridgeService::waitForHardwareAwake(uint32_t maxWaitMs) {
    uint32_t waitedMs = 0U;
    while (!stopping_ && !workerStopRequested_ && isHardwareSleepSuspended() && waitedMs < maxWaitMs) {
        IOSleep(20);
        waitedMs += 20U;
    }
    return !isHardwareSleepSuspended();
}

IOReturn UniCamBridgeService::startPipelineLocked(uint32_t shiftedDataTicks) {
    if (stopping_) {
        pipelineRunning_ = false;
        return kIOReturnAborted;
    }
    refreshServicesLocked();
    if (coreService_ == nullptr || ipuService_ == nullptr ||
        !coreService_->hasSupportedSensorDriver() || isHardwareSleepSuspendedLocked()) {
        pipelineRunning_ = false;
        return kIOReturnNotReady;
    }

    UniCamBoardSensorInfo sensorInfo{};
    bool haveSensorInfo = false;
    if (coreService_->getInfo(&sensorInfo) == kIOReturnSuccess) {
        haveSensorInfo = true;
        sensorRotation_ = sensorInfo.rotation;
    }

    const IOReturn pwrRet = coreService_->setPower(true, false, nullptr);
    if (pwrRet != kIOReturnSuccess) {
        IOLog("UniCam: core setPower(ON) failed 0x%x\n", pwrRet);
        pipelineRunning_ = false;
        return pwrRet;
    }

    (void)coreService_->setPrivacyLed(true, false, nullptr);

    IOReturn cfgRet = coreService_->configureSensor(0, 0);
    if (cfgRet != kIOReturnSuccess) {
        IOLog("UniCam: sensor configureSensor failed 0x%x, resetting sensor and retrying\n", cfgRet);
        IOSleep(25);
        (void)coreService_->resetSensor(false);
        cfgRet = coreService_->configureSensor(0, 0);
        if (cfgRet != kIOReturnSuccess) {
            IOLog("UniCam: sensor configureSensor retry failed 0x%x\n", cfgRet);
            coreService_->setPower(false, false, nullptr);
            if (streamingActive_) {
                coreService_->setPrivacyLed(true, false, nullptr);
            }
            pipelineRunning_ = false;
            return cfgRet;
        }
    }

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
    const IOReturn ipuRet = ipuService_->startStream(&cfg, &st, nullptr);
    if (ipuRet != kIOReturnSuccess) {
        IOLog("UniCam: ipu startStream failed 0x%x (will stream diagnostic pattern on timeout)\n",
              ipuRet);
        coreService_->setPower(false, false, nullptr);
        if (streamingActive_) {
            coreService_->setPrivacyLed(true, false, nullptr);
        }
        pipelineRunning_ = false;
        return ipuRet;
    }

    const IOReturn streamRet = coreService_->setStream(true, nullptr);
    if (streamRet != kIOReturnSuccess) {
        IOLog("UniCam: sensor setStream(ON) failed 0x%x\n", streamRet);
        ipuService_->stopStream(nullptr);
        coreService_->setPower(false, false, nullptr);
        if (streamingActive_) {
            coreService_->setPrivacyLed(true, false, nullptr);
        }
        pipelineRunning_ = false;
        return streamRet;
    }

    pipelineRunning_ = true;
    return kIOReturnSuccess;
}

void UniCamBridgeService::stopPipelineLocked() {
    pipelineRunning_ = false;
    if (coreService_ != nullptr && !coreService_->isInactive()) {
        coreService_->setStream(false, nullptr);
    }
    if (ipuService_ != nullptr && !ipuService_->isInactive()) {
        ipuService_->stopStream(nullptr);
    }
    if (coreService_ != nullptr && !coreService_->isInactive()) {
        coreService_->setPrivacyLed(false, false, nullptr);
        coreService_->setPower(false, false, nullptr);
    }
}

void UniCamBridgeService::startWorkerThreadLocked() {
    if (workerLock_ == nullptr || stopping_) {
        return;
    }
    IOLockLock(workerLock_);
    if (workerRunning_) {
        IOLockUnlock(workerLock_);
        return;
    }
    workerStopRequested_ = false;
    workerRunning_ = true;
    retain();
    IOLockUnlock(workerLock_);

    thread_t newThread = THREAD_NULL;
    const kern_return_t kr = kernel_thread_start(UniCamBridgeService::workerThreadEntry,
                                                 this,
                                                 &newThread);
    if (kr != KERN_SUCCESS) {
        IOLog("UniCam: kernel_thread_start failed (0x%x)\n", kr);
        IOLockLock(workerLock_);
        workerRunning_ = false;
        IOLockWakeup(workerLock_, &workerRunning_, false);
        IOLockUnlock(workerLock_);
        release();
        return;
    }

    IOLockLock(workerLock_);
    if (workerRunning_) {
        workerThread_ = newThread;
        IOLockUnlock(workerLock_);
    } else {
        IOLockUnlock(workerLock_);
        thread_deallocate(newThread);
    }
}

void UniCamBridgeService::stopWorkerThread() {
    if (workerLock_ == nullptr) {
        return;
    }
    IOLockLock(workerLock_);
    workerStopRequested_ = true;
    while (workerRunning_) {
        IOLockSleep(workerLock_, &workerRunning_, THREAD_UNINT);
    }
    thread_t oldThread = workerThread_;
    workerThread_ = THREAD_NULL;
    IOLockUnlock(workerLock_);

    if (oldThread != THREAD_NULL) {
        thread_deallocate(oldThread);
    }
}

void UniCamBridgeService::workerThreadEntry(void *parameter, wait_result_t /*waitResult*/) {
    auto *self = static_cast<UniCamBridgeService *>(parameter);
    if (self != nullptr) {
        self->runStreamWorker();
        self->release();
    }
    thread_terminate(current_thread());
}

void UniCamBridgeService::runStreamWorker() {
    uint32_t frameCounter = 0;
    uint32_t consecutiveTimeouts = 0;
    uint32_t streamHwFrames = 0;
    uint32_t recoveryAttempts = 0;
    bool triedShiftedTiming = false;

    while (!workerStopRequested_ && !stopping_) {
        if (isHardwareSleepSuspended()) {
            pipelineRunning_ = false;
            consecutiveTimeouts = 0;
            recoveryAttempts = 0;
            triedShiftedTiming = false;
            IOSleep(25);
            continue;
        }

        UniCamCoreService *core = nullptr;
        UniCamIPUService *ipu = nullptr;
        copyActiveServices(&core, &ipu);

        const bool hwStreamingNow = (ipu != nullptr && ipu->isHardwareStreaming());
        const bool canStartHw = (ipu != nullptr && core != nullptr && core->hasSupportedSensorDriver());

        if ((!pipelineRunning_ || !hwStreamingNow) &&
            canStartHw &&
            recoveryAttempts < 4U &&
            stateLock_ != nullptr) {
            if (recoveryAttempts > 0U) {
                IOSleep(150);
                if (workerStopRequested_ || stopping_ || isHardwareSleepSuspended()) {
                    if (core) core->release();
                    if (ipu) ipu->release();
                    continue;
                }
            }
            IOLockLock(stateLock_);
            stopPipelineLocked();
            const IOReturn recRet = startPipelineLocked(triedShiftedTiming ? 1280U : 0U);
            IOLockUnlock(stateLock_);
            recoveryAttempts++;
            if (recRet == kIOReturnSuccess) {
                consecutiveTimeouts = 0;
            }
        }

        if (workerStopRequested_ || stopping_) {
            if (core) core->release();
            if (ipu) ipu->release();
            break;
        }

        uint64_t loopStartAbs = 0;
        uint64_t loopStartNs = 0;
        clock_get_uptime(&loopStartAbs);
        absolutetime_to_nanoseconds(loopStartAbs, &loopStartNs);

        const uint32_t w = streamWidth_;
        const uint32_t h = streamHeight_;
        const uint32_t yuy2Bytes = w * h * 2U;
        if (yuy2FrameBuffer_ == nullptr || yuy2Bytes > yuy2FrameCapacity_) {
            if (core) core->release();
            if (ipu) ipu->release();
            IOSleep(16);
            continue;
        }

        bool haveFrame = false;
        bool inFallback = false;
        if (ipu != nullptr && pipelineRunning_) {
            UniCamIpuFrameEvent ev{};
            IOReturn waitRet = ipu->waitNextFrame(50, &ev);
            if (workerStopRequested_ || stopping_) {
                if (core) core->release();
                if (ipu) ipu->release();
                break;
            }
            if (waitRet == kIOReturnSuccess) {
                const uint32_t bayerOrder = (core != nullptr) ? core->activeBayerOrder() : 0U;
                UniCamIspFrameStats stats{};
                IOReturn ispRet = ipu->processSlotToYuy2(
                    ev.slotIndex,
                    bayerOrder,
                    sensorRotation_,
                    yuy2FrameBuffer_,
                    w,
                    h,
                    yuy2FrameCapacity_,
                    &stats);
                ipu->requeueSlot(ev.slotIndex);
                if (ispRet == kIOReturnSuccess) {
                    haveFrame = true;
                    consecutiveTimeouts = 0;
                    recoveryAttempts = 0;
                    streamHwFrames++;
                    hwFramesDelivered_++;
                    if (stats.aeUpdated != 0 && core != nullptr && core->hasSupportedSensorDriver()) {
                        core->setExposureGain(stats.exposureLines,
                                              stats.analogGain,
                                              1024U);
                    }
                }
            } else {
                consecutiveTimeouts++;
                if (!triedShiftedTiming && streamHwFrames == 0U && consecutiveTimeouts == 10U &&
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

        if (workerStopRequested_ || stopping_) {
            if (core) core->release();
            if (ipu) ipu->release();
            break;
        }

        if (!haveFrame && consecutiveTimeouts >= 12) {
            if (ipu != nullptr) {
                ipu->generateTestPatternYuy2(yuy2FrameBuffer_, w, h, frameCounter);
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

        if (core) core->release();
        if (ipu) ipu->release();

        if (workerStopRequested_ || stopping_) {
            break;
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

    if (workerLock_ != nullptr) {
        IOLockLock(workerLock_);
        workerRunning_ = false;
        IOLockWakeup(workerLock_, &workerRunning_, false);
        IOLockUnlock(workerLock_);
    } else {
        workerRunning_ = false;
    }
}
