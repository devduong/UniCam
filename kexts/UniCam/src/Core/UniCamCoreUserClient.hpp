#ifndef UNICAM_CORE_USER_CLIENT_HPP
#define UNICAM_CORE_USER_CLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "UniCamCoreService.hpp"

class UniCamCoreUserClient : public IOUserClient {
    OSDeclareDefaultStructors(UniCamCoreUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void stop(IOService *provider) override;

    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target,
                            void *reference) override;

private:
    static IOReturn sGetInfo(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sGetDiag(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sProbeSensor(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetPower(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetLed(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sResetSensor(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sReadReg(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sWriteReg(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sConfigureSensor(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetStream(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetExposureGain(OSObject *target, void *reference, IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kUniCamMethodCount];

    UniCamCoreService *service_;
    task_t owningTask_;
};

#endif /* UNICAM_CORE_USER_CLIENT_HPP */
