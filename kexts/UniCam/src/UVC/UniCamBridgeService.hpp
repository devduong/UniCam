#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>

#include "UniCamCoreService.hpp"
#include "UniCamIPUService.hpp"
#include "UniCamVirtualUVC.hpp"

class UniCamBridgeService : public IOService, public UniCamUvcDelegate {
    OSDeclareDefaultStructors(UniCamBridgeService)

public:
    virtual bool init(OSDictionary *dictionary = nullptr) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

    // UniCamUvcDelegate callbacks from virtual UVC interface/device
    virtual void onUvcStreamingChanged(bool active,
                                       uint8_t formatIndex,
                                       uint8_t frameIndex,
                                       uint32_t frameInterval100ns) override;
    virtual void onUvcControlUpdated(uint8_t unitOrTerminalId,
                                     uint8_t controlSelector,
                                     int32_t value) override;

private:
    void onDiscoveryTimer(IOTimerEventSource *sender);
    void tryAttachServicesAndPublishUvc();
    void updateRegistryStatus();

    bool isHardwareSleepSuspended() const;
    bool waitForHardwareAwake(uint32_t maxWaitMs) const;
    IOReturn startPipelineLocked(uint32_t shiftedDataTicks = 0U);
    void stopPipelineLocked();
    void stopWorkerThread();
    static void workerThreadEntry(void *parameter, wait_result_t waitResult);
    void runStreamWorker();

    IOWorkLoop *workLoop_ = nullptr;
    IOTimerEventSource *discoveryTimer_ = nullptr;
    IOLock *stateLock_ = nullptr;

    UniCamCoreService *coreService_ = nullptr;
    UniCamIPUService *ipuService_ = nullptr;
    UniCamVirtualUVCDevice *uvcDevice_ = nullptr;

    bool disabledByBootArg_ = false;
    bool debugLog_ = false;
    bool betaBootArg_ = false;
    bool stopping_ = false;
    bool uvcAttached_ = false;

    volatile bool streamingActive_ = false;
    volatile bool pipelineRunning_ = false;
    volatile bool workerStopRequested_ = false;
    bool workerRunning_ = false;
    thread_t workerThread_ = THREAD_NULL;

    uint32_t streamWidth_ = 1280;
    uint32_t streamHeight_ = 720;
    uint32_t streamFps_ = 30;
    uint32_t sensorRotation_ = 0;

    uint8_t *yuy2FrameBuffer_ = nullptr;
    uint32_t yuy2FrameCapacity_ = 0;

    uint64_t hwFramesDelivered_ = 0;
    uint64_t fallbackFramesDelivered_ = 0;
    uint32_t discoveryTicks_ = 0;
};
