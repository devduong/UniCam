#ifndef UNICAM_IPU_USER_CLIENT_HPP
#define UNICAM_IPU_USER_CLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "UniCamIPUService.hpp"

class UniCamIPUUserClient : public IOUserClient {
    OSDeclareDefaultStructors(UniCamIPUUserClient)

public:
    bool initWithTask(task_t owningTask,
                      void *securityToken,
                      UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void stop(IOService *provider) override;

    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target,
                            void *reference) override;

    IOReturn clientMemoryForType(UInt32 type,
                                 IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;

private:
    static IOReturn sGetInfo(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sGetDiag(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sBootFirmware(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sShutdownFirmware(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sStartStream(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sWaitNextFrame(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sRequeueSlot(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sStopStream(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sProcessSlotYuy2(OSObject *target, void *reference, IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kUniCamIpuMethodCount];

    UniCamIPUService *service_;
    task_t owningTask_;
};

#endif /* UNICAM_IPU_USER_CLIENT_HPP */
