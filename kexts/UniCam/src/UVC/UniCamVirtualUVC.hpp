#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostFamily.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/StandardUSB.h>

class OSSet;
class UniCamVirtualUVCDevice;
class UniCamVirtualUVCInterface;
class UniCamVirtualUVCPipe;

class UniCamUvcDelegate {
public:
    virtual ~UniCamUvcDelegate() = default;
    virtual void onUvcStreamingChanged(bool active,
                                       uint8_t formatIndex,
                                       uint8_t frameIndex,
                                       uint32_t frameInterval100ns) = 0;
    virtual void onUvcControlUpdated(uint8_t unitOrTerminalId,
                                     uint8_t controlSelector,
                                     int32_t value) = 0;
};

struct UniCamUvcPendingTransfer {
    bool inUse;
    bool isTransaction;
    IOMemoryDescriptor *dataBuffer;
    IOUSBHostIsochronousTransaction *transactions;
    IOUSBHostIsochronousFrame *frames;
    uint32_t count;
    IOUSBHostIsochronousTransactionCompletion txCompletion;
    IOUSBHostIsochronousCompletion legacyCompletion;
};

class UniCamVirtualUVCPipe : public IOUSBHostPipe {
    OSDeclareDefaultStructors(UniCamVirtualUVCPipe)

public:
    static constexpr uint32_t kMaxPendingTransfers = 256;

    static UniCamVirtualUVCPipe *createWithDescriptor(
        const StandardUSB::EndpointDescriptor *epDesc,
        const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssDesc,
        UniCamVirtualUVCInterface *ownerIface,
        UniCamVirtualUVCDevice *ownerDev);

    bool initVirtual(const StandardUSB::EndpointDescriptor *epDesc,
                     const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssDesc,
                     UniCamVirtualUVCInterface *ownerIface,
                     UniCamVirtualUVCDevice *ownerDev);

    virtual void free() override;
    virtual IOReturn abort(IOOptionBits options = kAbortAsynchronous,
                           IOReturn withError = kIOReturnAborted,
                           IOService *forClient = nullptr) override;

    virtual const StandardUSB::EndpointDescriptor *getEndpointDescriptor(
        tGetEndpointDescriptorOptions type = kGetEndpointDescriptorCurrentPolicy) override;
    virtual const StandardUSB::SuperSpeedEndpointCompanionDescriptor *getSuperSpeedEndpointCompanionDescriptor(
        tGetEndpointDescriptorOptions type = kGetEndpointDescriptorCurrentPolicy) override;
    virtual IOReturn adjustPipe(const StandardUSB::EndpointDescriptor *endpointDescriptor,
                                const StandardUSB::SuperSpeedEndpointCompanionDescriptor *companionDescriptor) override;
    virtual IOReturn setIdlePolicy(uint32_t idleTimeoutMs) override;
    virtual uint32_t getIdlePolicy() override;
    virtual IOReturn clearStall(bool withRequest) override;
    virtual IOReturn adjustPipe(StandardUSBDescriptors *descriptors) override;
    virtual IOReturn getDescriptors(StandardUSBDescriptors *descriptors,
                                    tGetEndpointDescriptorOptions type = kGetEndpointDescriptorCurrentPolicy) override;

    virtual IOReturn io(IOMemoryDescriptor *dataBuffer,
                        uint32_t dataBufferLength,
                        IOUSBHostCompletion *completion,
                        uint32_t completionTimeoutMs = 0) override;
    virtual IOReturn io(IOMemoryDescriptor *dataBuffer,
                        uint32_t dataBufferLength,
                        uint32_t &bytesTransferred,
                        uint32_t completionTimeoutMs = 0) override;
    virtual IOReturn io(IOMemoryDescriptor *dataBuffer,
                        IOUSBHostIsochronousFrame *frameList,
                        uint32_t frameListCount,
                        uint64_t firstFrameNumber = 0,
                        IOUSBHostIsochronousCompletion *completion = nullptr) override;
    virtual IOReturn io(IOMemoryDescriptor *dataBuffer,
                        uint32_t dataBufferLength,
                        IOUSBHostBundledCompletion *completion,
                        uint32_t completionTimeoutMs = 0) override;
    virtual IOReturn io(IOMemoryDescriptor *dataBuffer,
                        IOUSBHostIsochronousTransaction *transactionList,
                        uint32_t transactionListCount,
                        uint64_t firstFrameNumber = 0,
                        IOOptionBits options = kIsochronousTransferOptionsNone,
                        IOUSBHostIsochronousTransactionCompletion *completion = nullptr) override;

    virtual uint8_t getSpeed() const override;
    virtual tUSBHostDeviceAddress getDeviceAddress() const override;

    void setActive(bool active);
    bool deliverYuy2Frame(const uint8_t *yuy2Data,
                          uint32_t byteCount,
                          uint8_t &fidInOut,
                          uint32_t ptsTicks,
                          uint16_t &scrSofInOut);
    void flushAborted(IOReturn err);

private:
    bool popReadyTransfer(UniCamUvcPendingTransfer *outTransfer);
    bool waitPopReadyTransfer(UniCamUvcPendingTransfer *outTransfer, AbsoluteTime deadline);
    IOReturn enqueueIsoch(IOMemoryDescriptor *dataBuffer,
                          IOUSBHostIsochronousTransaction *transactions,
                          IOUSBHostIsochronousFrame *frames,
                          uint32_t count,
                          bool isTransaction,
                          const IOUSBHostIsochronousTransactionCompletion *txComp,
                          const IOUSBHostIsochronousCompletion *legacyComp);

    const StandardUSB::EndpointDescriptor *epDesc_ = nullptr;
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssDesc_ = nullptr;
    UniCamVirtualUVCInterface *ownerIface_ = nullptr;
    UniCamVirtualUVCDevice *ownerDev_ = nullptr;
    uint32_t idleTimeoutMs_ = 0;

    IOLock *queueLock_ = nullptr;
    UniCamUvcPendingTransfer *pending_ = nullptr;
    bool pipeActive_ = false;
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint32_t count_ = 0;
};

class UniCamVirtualUVCInterface : public IOUSBHostInterface {
    OSDeclareDefaultStructors(UniCamVirtualUVCInterface)

public:
    static UniCamVirtualUVCInterface *createWithUvc(
        UniCamVirtualUVCDevice *ownerDev,
        uint8_t interfaceNumber);

    bool initVirtual(UniCamVirtualUVCDevice *ownerDev, uint8_t interfaceNumber);

    virtual bool attach(IOService *provider) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual bool terminate(IOOptionBits options = 0) override;
    virtual void free() override;

    virtual bool open(IOService *forClient, IOOptionBits options = 0, void *arg = nullptr) override;
    virtual void close(IOService *forClient, IOOptionBits options = 0) override;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument = nullptr) override;

    virtual IOReturn setIdlePolicy(uint32_t deviceIdleTimeout) override;
    virtual uint32_t getIdlePolicy() override;

    virtual const StandardUSB::ConfigurationDescriptor *getConfigurationDescriptor() override;
    virtual const StandardUSB::InterfaceDescriptor *getInterfaceDescriptor() override;
    virtual const StandardUSB::StringDescriptor *getStringDescriptor(
        uint8_t index,
        uint16_t languageID = StandardUSB::kLanguageIDEnglishUS) override;

    virtual IOReturn selectAlternateSetting(uint8_t bAlternateSetting) override;
    virtual IOUSBHostPipe *copyPipe(uint8_t address) override;

    virtual IOReturn deviceRequest(StandardUSB::DeviceRequest &request,
                                   void *dataBuffer,
                                   IOUSBHostCompletion *completion,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(StandardUSB::DeviceRequest &request,
                                   IOMemoryDescriptor *dataBuffer,
                                   IOUSBHostCompletion *completion,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(StandardUSB::DeviceRequest &request,
                                   void *dataBuffer,
                                   uint32_t &bytesTransferred,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(StandardUSB::DeviceRequest &request,
                                   IOMemoryDescriptor *dataBuffer,
                                   uint32_t &bytesTransferred,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;

    virtual IOUSBHostDevice *getDevice() const override;
    virtual uint64_t getFrameNumber(AbsoluteTime *theTime = nullptr) const override;
    virtual uint32_t getPortStatus() const override;
    virtual IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options,
                                                     mach_vm_size_t capacity) override;
    virtual IOReturn currentMicroframe(uint64_t &microframe, uint64_t &microframeTime) override;
    virtual IOReturn referenceMicroframe(uint64_t &microframe, uint64_t &microframeTime) override;

    UniCamVirtualUVCPipe *videoStreamPipe() const { return vsIsochPipe_; }
    uint8_t activeAltSetting() const { return activeAltSetting_; }

private:
    UniCamVirtualUVCDevice *ownerDev_ = nullptr;
    uint8_t interfaceNumber_ = 0;
    uint8_t activeAltSetting_ = 0;
    IOService *openClient_ = nullptr;
    IOLock *ifaceLock_ = nullptr;

    UniCamVirtualUVCPipe *vcInterruptPipe_ = nullptr;
    UniCamVirtualUVCPipe *vsIsochPipe_ = nullptr;
};

class UniCamVirtualUVCDevice : public IOUSBHostDevice {
    OSDeclareDefaultStructors(UniCamVirtualUVCDevice)

public:
    static UniCamVirtualUVCDevice *createWithDelegate(UniCamUvcDelegate *delegate,
                                                      bool debugLog);

    bool initWithDelegate(UniCamUvcDelegate *delegate, bool debugLog);

    virtual bool attach(IOService *provider) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual bool terminate(IOOptionBits options = 0) override;
    virtual void free() override;

    virtual bool open(IOService *forClient, IOOptionBits options = 0, void *arg = nullptr) override;
    virtual bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) override;
    virtual bool handleIsOpen(const IOService *forClient) const override;
    virtual void close(IOService *forClient, IOOptionBits options = 0) override;
    virtual void handleClose(IOService *forClient, IOOptionBits options) override;

    virtual const StandardUSB::Descriptor *getDescriptor(
        uint8_t type,
        uint16_t &length,
        uint8_t index = 0,
        uint16_t languageID = 0,
        tDeviceRequestType requestType = kRequestTypeStandard,
        tDeviceRequestRecipient requestRecipient = kRequestRecipientDevice) override;
    virtual const StandardUSB::DeviceDescriptor *getDeviceDescriptor() override;
    virtual const StandardUSB::ConfigurationDescriptor *getConfigurationDescriptor(uint8_t index) override;
    virtual const StandardUSB::ConfigurationDescriptor *getConfigurationDescriptorWithValue(uint8_t bConfigurationValue) override;
    virtual const StandardUSB::ConfigurationDescriptor *getConfigurationDescriptor() override;
    virtual const StandardUSB::StringDescriptor *getStringDescriptor(
        uint8_t index,
        uint16_t languageID = StandardUSB::kLanguageIDEnglishUS) override;

    virtual IOReturn setConfiguration(uint8_t bConfigurationValue, bool matchInterfaces = true) override;

    virtual IOReturn deviceRequest(IOService *forClient,
                                   StandardUSB::DeviceRequest &request,
                                   void *dataBuffer,
                                   IOUSBHostCompletion *completion,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(IOService *forClient,
                                   StandardUSB::DeviceRequest &request,
                                   IOMemoryDescriptor *dataBuffer,
                                   IOUSBHostCompletion *completion,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(IOService *forClient,
                                   StandardUSB::DeviceRequest &request,
                                   void *dataBuffer,
                                   uint32_t &bytesTransferred,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;
    virtual IOReturn deviceRequest(IOService *forClient,
                                   StandardUSB::DeviceRequest &request,
                                   IOMemoryDescriptor *dataBuffer,
                                   uint32_t &bytesTransferred,
                                   uint32_t completionTimeoutMs = kUSBHostDefaultControlCompletionTimeoutMS) override;

    virtual tUSBHostDeviceAddress getAddress() const override;
    virtual uint8_t getSpeed() const override;
    virtual uint64_t getFrameNumber(AbsoluteTime *theTime = nullptr) const;
    virtual uint64_t getFrameNumber(AbsoluteTime *theTime = nullptr) override;
    virtual IOReturn reset() override;
    virtual uint32_t getPortStatus() override;
    virtual IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options,
                                                     mach_vm_size_t capacity) override;
    virtual IOReturn currentMicroframe(uint64_t &microframe, uint64_t &microframeTime) override;
    virtual IOReturn referenceMicroframe(uint64_t &microframe, uint64_t &microframeTime) override;

    // Helpers for child interfaces and stream worker
    const StandardUSB::InterfaceDescriptor *getInterfaceDescriptorFor(uint8_t ifaceNum,
                                                                      uint8_t altSetting) const;
    const StandardUSB::EndpointDescriptor *getEndpointDescriptorFor(uint8_t address) const;
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor *getSsCompanionDescriptorFor(uint8_t address) const;

    IOReturn handleDeviceRequestSync(StandardUSB::DeviceRequest &request,
                                     void *dataBuffer,
                                     uint32_t bufferCapacity,
                                     uint32_t &bytesTransferred);
    void notifyAltSettingChanged(uint8_t ifaceNum, uint8_t altSetting);
    bool submitYuy2Frame(const uint8_t *yuy2Data, uint32_t byteCount);
    uint64_t getUvcFrameNumber(AbsoluteTime *theTime = nullptr) const;

private:
    struct __attribute__((packed)) UvcStreamingControl {
        uint16_t bmHint;
        uint8_t bFormatIndex;
        uint8_t bFrameIndex;
        uint32_t dwFrameInterval;
        uint16_t wKeyFrameRate;
        uint16_t wPFrameRate;
        uint16_t wCompQuality;
        uint16_t wCompWindowSize;
        uint16_t wDelay;
        uint32_t dwMaxVideoFrameSize;
        uint32_t dwMaxPayloadTransferSize;
    };

    void initDefaultUvcControls();
    uint32_t frameSizeForIndex(uint8_t frameIndex) const;
    IOReturn handleVsControl(const StandardUSB::DeviceRequest &req,
                             uint8_t *buf,
                             uint32_t cap,
                             uint32_t &outLen);
    IOReturn handleVcControl(const StandardUSB::DeviceRequest &req,
                             uint8_t *buf,
                             uint32_t cap,
                             uint32_t &outLen);

    UniCamUvcDelegate *delegate_ = nullptr;
    bool debugLog_ = false;
    IOLock *devLock_ = nullptr;
    OSSet *openClients_ = nullptr;

    UniCamVirtualUVCInterface *vcIface_ = nullptr;
    UniCamVirtualUVCInterface *vsIface_ = nullptr;

    UvcStreamingControl probeCtrl_{};
    UvcStreamingControl commitCtrl_{};

    uint8_t vcRequestError_ = 0;
    uint8_t aeMode_ = 0x08;           // Aperture priority
    uint32_t exposureAbs_ = 166;      // 16.6 ms (in 100us units)
    uint16_t puBacklightComp_ = 1;
    int16_t puBrightness_ = 0;
    uint16_t puContrast_ = 32;
    int16_t puHue_ = 0;
    uint16_t puSaturation_ = 32;
    uint16_t puSharpness_ = 16;
    uint16_t puGamma_ = 220;
    uint16_t puWbTemp_ = 5500;
    uint8_t puWbAuto_ = 1;
    uint32_t puWbComponent_ = 0x00400040U;
    uint8_t puWbComponentAuto_ = 1;
    uint8_t puHueAuto_ = 1;
    uint16_t puGain_ = 16;
    uint8_t puPowerLineFreq_ = 1;

    uint8_t streamFid_ = 0;
    uint16_t scrSof_ = 0;
};
