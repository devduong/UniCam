#include "UniCamCoreUserClient.hpp"

#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(UniCamCoreUserClient, IOUserClient)

const IOExternalMethodDispatch UniCamCoreUserClient::sMethods[kUniCamMethodCount] = {
    // kUniCamMethodGetInfo
    { &UniCamCoreUserClient::sGetInfo,         0, 0, 0, sizeof(UniCamBoardSensorInfo) },
    // kUniCamMethodGetDiag
    { &UniCamCoreUserClient::sGetDiag,         0, 0, 0, sizeof(UniCamHardwareDiag) },
    // kUniCamMethodProbeSensor
    { &UniCamCoreUserClient::sProbeSensor,     2, 0, 0, sizeof(UniCamProbeReport) },
    // kUniCamMethodSetPower
    { &UniCamCoreUserClient::sSetPower,        2, 0, 0, 0 },
    // kUniCamMethodSetLed
    { &UniCamCoreUserClient::sSetLed,          1, 0, 0, 0 },
    // kUniCamMethodResetSensor
    { &UniCamCoreUserClient::sResetSensor,     1, 0, 0, 0 },
    // kUniCamMethodReadReg
    { &UniCamCoreUserClient::sReadReg,         2, 0, 1, 0 },
    // kUniCamMethodWriteReg
    { &UniCamCoreUserClient::sWriteReg,        3, 0, 0, 0 },
    // kUniCamMethodConfigureSensor
    { &UniCamCoreUserClient::sConfigureSensor, 2, 0, 0, 0 },
    // kUniCamMethodSetStream
    { &UniCamCoreUserClient::sSetStream,       1, 0, 0, 0 },
    // kUniCamMethodSetExposureGain
    { &UniCamCoreUserClient::sSetExposureGain, 3, 0, 0, 0 },
};

bool UniCamCoreUserClient::initWithTask(task_t owningTask,
                                        void *securityToken,
                                        UInt32 type,
                                        OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityToken, type, properties)) {
        return false;
    }
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        IOLog("UniCamCore: UserClient open denied (requires root/administrator privileges)\n");
        return false;
    }
    owningTask_ = owningTask;
    service_ = nullptr;
    return true;
}

bool UniCamCoreUserClient::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    service_ = OSDynamicCast(UniCamCoreService, provider);
    if (!service_) {
        return false;
    }
    return true;
}

IOReturn UniCamCoreUserClient::clientClose() {
    if (service_) {
        service_->handleClientDisconnect(this);
    }
    terminate();
    return kIOReturnSuccess;
}

void UniCamCoreUserClient::stop(IOService *provider) {
    service_ = nullptr;
    super::stop(provider);
}

IOReturn UniCamCoreUserClient::externalMethod(uint32_t selector,
                                              IOExternalMethodArguments *arguments,
                                              IOExternalMethodDispatch *dispatch,
                                              OSObject *target,
                                              void *reference) {
    if (selector >= kUniCamMethodCount) {
        return kIOReturnUnsupported;
    }
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target = this;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn UniCamCoreUserClient::sGetInfo(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return uc->service_->getInfo(static_cast<UniCamBoardSensorInfo *>(args->structureOutput));
}

IOReturn UniCamCoreUserClient::sGetDiag(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return uc->service_->getDiagnostics(static_cast<UniCamHardwareDiag *>(args->structureOutput));
}

IOReturn UniCamCoreUserClient::sProbeSensor(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_ || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    const uint32_t holdMs = static_cast<uint32_t>(args->scalarInput[0]);
    const bool forceMmio = (args->scalarInput[1] != 0);
    UniCamProbeReport *rep = static_cast<UniCamProbeReport *>(args->structureOutput);
    const IOReturn ret = uc->service_->probeSensor(holdMs, forceMmio, rep);
    if (ret == kIOReturnBadArgument) {
        return ret;
    }
    if (ret != kIOReturnSuccess && rep->i2cRet == 0) {
        rep->i2cRet = ret;
    }
    return kIOReturnSuccess;
}

IOReturn UniCamCoreUserClient::sSetPower(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->setPower(args->scalarInput[0] != 0, args->scalarInput[1] != 0, uc);
}

IOReturn UniCamCoreUserClient::sSetLed(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->setPrivacyLed(args->scalarInput[0] != 0, false, uc);
}

IOReturn UniCamCoreUserClient::sResetSensor(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->resetSensor(args->scalarInput[0] != 0);
}

IOReturn UniCamCoreUserClient::sReadReg(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    uint32_t val = 0;
    const IOReturn ret = uc->service_->readRegister(
        static_cast<uint16_t>(args->scalarInput[0] & 0xFFFFU),
        static_cast<uint8_t>(args->scalarInput[1] & 0xFFU),
        &val);
    if (ret == kIOReturnSuccess) {
        args->scalarOutput[0] = val;
    }
    return ret;
}

IOReturn UniCamCoreUserClient::sWriteReg(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->writeRegister(
        static_cast<uint16_t>(args->scalarInput[0] & 0xFFFFU),
        static_cast<uint8_t>(args->scalarInput[1] & 0xFFU),
        static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFU));
}

IOReturn UniCamCoreUserClient::sConfigureSensor(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->configureSensor(
        static_cast<uint32_t>(args->scalarInput[0]),
        static_cast<uint32_t>(args->scalarInput[1]));
}

IOReturn UniCamCoreUserClient::sSetStream(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->setStream(args->scalarInput[0] != 0, uc);
}

IOReturn UniCamCoreUserClient::sSetExposureGain(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    (void)reference;
    UniCamCoreUserClient *uc = OSDynamicCast(UniCamCoreUserClient, target);
    if (!uc || !uc->service_) {
        return kIOReturnBadArgument;
    }
    return uc->service_->setExposureGain(
        static_cast<uint32_t>(args->scalarInput[0]),
        static_cast<uint32_t>(args->scalarInput[1]),
        static_cast<uint32_t>(args->scalarInput[2]));
}
