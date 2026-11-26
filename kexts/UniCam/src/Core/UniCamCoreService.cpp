#include "UniCamCoreService.hpp"
#include "Sensors/SensorRegistry.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>
#include <string.h>

#define super IOService
OSDefineMetaClassAndStructors(UniCamCoreService, IOService)

namespace {

UniCamCoreService *gSharedCoreInstance = nullptr;

enum {
    kPowerStateOff = 0,
    kPowerStateOn  = 1,
    kPowerStateCount = 2
};

IOPMPowerState kUniCamPowerStates[kPowerStateCount] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

enum GatedOp {
    kOpGetInfo = 0,
    kOpGetDiag,
    kOpProbeSensor,
    kOpSetPower,
    kOpSetLed,
    kOpResetSensor,
    kOpReadReg,
    kOpWriteReg,
    kOpConfigureSensor,
    kOpSetStream,
    kOpSetExposureGain,
    kOpClientDisconnect,
    kOpSleepOff
};

struct GatedRequest {
    GatedOp op;
    uint32_t u0;
    uint32_t u1;
    uint32_t u2;
    bool b0;
    bool b1;
    void *ptr;
    UniCamCoreUserClient *client;
};

} // namespace

bool UniCamCoreService::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    nub_ = nullptr;
    workLoop_ = nullptr;
    commandGate_ = nullptr;
    sensorDriver_ = nullptr;
    memset(&boardInfo_, 0, sizeof(boardInfo_));
    bayerOrder_ = 0U;
    streaming_ = false;
    sleepSuspended_ = false;
    activeOwner_ = nullptr;
    return true;
}

void UniCamCoreService::free() {
    if (sensorDriver_) {
        SensorRegistry::destroy(sensorDriver_);
        sensorDriver_ = nullptr;
    }
    super::free();
}

UniCamCoreService *UniCamCoreService::sharedInstance() {
    return gSharedCoreInstance;
}

IOService *UniCamCoreService::probe(IOService *provider, SInt32 *score) {
    uint32_t offArg = 0;
    if (PE_parse_boot_argn("-unicamoff", &offArg, sizeof(offArg))) {
        IOLog("UniCamCore: Disabled via -unicamoff boot-arg\n");
        return nullptr;
    }
    if (!super::probe(provider, score)) {
        return nullptr;
    }
    VoodooI2CDeviceNub *nub = OSDynamicCast(VoodooI2CDeviceNub, provider);
    if (!nub) {
        return nullptr;
    }
    if (score) {
        *score = 500;
    }
    return this;
}

bool UniCamCoreService::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    nub_ = OSDynamicCast(VoodooI2CDeviceNub, provider);
    if (!nub_) {
        IOLog("UniCamCore: Provider is not VoodooI2CDeviceNub\n");
        return false;
    }

    workLoop_ = IOWorkLoop::workLoop();
    if (!workLoop_) {
        IOLog("UniCamCore: Failed to allocate IOWorkLoop\n");
        return false;
    }

    commandGate_ = IOCommandGate::commandGate(this);
    if (!commandGate_ || workLoop_->addEventSource(commandGate_) != kIOReturnSuccess) {
        IOLog("UniCamCore: Failed to create/attach IOCommandGate\n");
        if (commandGate_) {
            commandGate_->release();
            commandGate_ = nullptr;
        }
        workLoop_->release();
        workLoop_ = nullptr;
        return false;
    }

    if (!powerHal_.init(provider, &boardInfo_)) {
        IOLog("UniCamCore: AcpiMethodPowerHal init failed\n");
        stop(provider);
        return false;
    }

    i2cTransport_.setNub(nub_);
    sensorDriver_ = SensorRegistry::createForCompat(boardInfo_.sensorCompat, boardInfo_.sensorHid);
    if (sensorDriver_) {
        sensorDriver_->setBusConfig(boardInfo_.csiLanes ? boardInfo_.csiLanes : 1U,
                                    boardInfo_.linkFreqHz);
        const UniCamSensorModeDesc mode = sensorDriver_->defaultMode();
        boardInfo_.sensorWidth = mode.width;
        boardInfo_.sensorHeight = mode.height;
        boardInfo_.sensorBpp = mode.bpp;
        boardInfo_.expectedChipId = sensorDriver_->expectedChipId();
        bayerOrder_ = mode.bayerOrder;
        if (boardInfo_.linkFreqHz == 0ULL) {
            boardInfo_.linkFreqHz = mode.linkFreqHz;
        }
    } else {
        boardInfo_.sensorWidth = 1280;
        boardInfo_.sensorHeight = 800;
        boardInfo_.sensorBpp = 10;
        boardInfo_.expectedChipId = UNICAM_OV01A10_CHIP_ID;
        bayerOrder_ = 0U;
        IOLog("UniCamCore: Warning - no sensor driver registered for compat='%s' hid='%s'\n",
              boardInfo_.sensorCompat, boardInfo_.sensorHid);
    }

    // Publish clean summary properties in IORegistry for user-space & UniCamIPU.kext
    setProperty("unicam-schema", boardInfo_.schemaVersion, 32);
    setProperty("unicam-board-id", boardInfo_.boardId);
    setProperty("unicam-ipu-gen", boardInfo_.ipuGen);
    setProperty("unicam-ipu-path", boardInfo_.ipuPath);
    setProperty("unicam-sensor-hid", boardInfo_.sensorHid);
    setProperty("unicam-sensor-compat", boardInfo_.sensorCompat);
    setProperty("unicam-bayer-order", bayerOrder_, 32);
    setProperty("unicam-i2c-bus", boardInfo_.i2cBus, 32);
    setProperty("unicam-i2c-addr", boardInfo_.i2cAddr, 32);
    setProperty("unicam-csi-port", boardInfo_.csiPort, 32);
    setProperty("unicam-csi-lanes", boardInfo_.csiLanes, 32);
    setProperty("unicam-link-freq-hz", boardInfo_.linkFreqHz, 64);
    setProperty("unicam-mclk-hz", boardInfo_.mclkHz, 64);
    setProperty("unicam-rotation", boardInfo_.rotation, 32);
    setProperty("unicam-ready", true);

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, kUniCamPowerStates, kPowerStateCount);
    makeUsable();

    gSharedCoreInstance = this;
    setName("UniCamCoreService");
    registerService();
    IOLog("UniCamCore: Started (%s / %s)\n", boardInfo_.boardId, boardInfo_.sensorCompat);
    return true;
}

void UniCamCoreService::stop(IOService *provider) {
    if (gSharedCoreInstance == this) {
        gSharedCoreInstance = nullptr;
    }
    if (commandGate_) {
        GatedRequest req;
        memset(&req, 0, sizeof(req));
        req.op = kOpSleepOff;
        commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
    }

    PMstop();

    if (commandGate_ && workLoop_) {
        workLoop_->removeEventSource(commandGate_);
    }
    if (commandGate_) {
        commandGate_->release();
        commandGate_ = nullptr;
    }
    if (workLoop_) {
        workLoop_->release();
        workLoop_ = nullptr;
    }

    powerHal_.cleanup();
    i2cTransport_.setNub(nullptr);
    if (sensorDriver_) {
        SensorRegistry::destroy(sensorDriver_);
        sensorDriver_ = nullptr;
    }
    nub_ = nullptr;
    super::stop(provider);
}

IOReturn UniCamCoreService::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) {
    (void)whatDevice;
    if (powerStateOrdinal == kPowerStateOff) {
        sleepSuspended_ = true;
        if (commandGate_) {
            GatedRequest req;
            memset(&req, 0, sizeof(req));
            req.op = kOpSleepOff;
            commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
        }
    } else {
        // On wake, keep sensor & LED OFF until a client explicitly requests camera access
        sleepSuspended_ = false;
    }
    return kIOPMAckImplied;
}

IOReturn UniCamCoreService::gatedActionTrampoline(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;

    UniCamCoreService *self = OSDynamicCast(UniCamCoreService, owner);
    GatedRequest *req = static_cast<GatedRequest *>(arg0);
    if (!self || !req) {
        return kIOReturnBadArgument;
    }

    switch (req->op) {
        case kOpGetInfo:
            self->fillInfoLocked(static_cast<UniCamBoardSensorInfo *>(req->ptr));
            return kIOReturnSuccess;

        case kOpGetDiag:
            self->fillDiagLocked(static_cast<UniCamHardwareDiag *>(req->ptr));
            return kIOReturnSuccess;

        case kOpProbeSensor:
            return self->probeSensorLocked(req->u0, req->b0, static_cast<UniCamProbeReport *>(req->ptr));

        case kOpSetPower:
            return self->setPowerLocked(req->b0, req->b1, req->client);

        case kOpSetLed:
            return self->setPrivacyLedLocked(req->b0, req->b1, req->client);

        case kOpResetSensor:
            return self->powerHal_.pulseReset(req->b0);

        case kOpReadReg: {
            if (!self->powerHal_.isPowered()) {
                return kIOReturnNotReady;
            }
            const uint16_t reg = static_cast<uint16_t>(req->u0 & 0xFFFFU);
            uint32_t *outVal = static_cast<uint32_t *>(req->ptr);
            if (req->u1 == 1) {
                uint8_t v = 0;
                const IOReturn ret = self->i2cTransport_.readReg8(reg, &v);
                if (ret == kIOReturnSuccess && outVal) *outVal = v;
                return ret;
            }
            if (req->u1 == 2) {
                uint16_t v = 0;
                const IOReturn ret = self->i2cTransport_.readReg16(reg, &v);
                if (ret == kIOReturnSuccess && outVal) *outVal = v;
                return ret;
            }
            if (req->u1 == 3) {
                uint8_t raw[3] = {0, 0, 0};
                return self->i2cTransport_.readReg24(reg, outVal, raw);
            }
            return kIOReturnBadArgument;
        }

        case kOpWriteReg: {
            if (!self->powerHal_.isPowered()) {
                return kIOReturnNotReady;
            }
            const uint16_t reg = static_cast<uint16_t>(req->u0 & 0xFFFFU);
            if (req->u1 == 1) {
                return self->i2cTransport_.writeReg8(reg, static_cast<uint8_t>(req->u2 & 0xFFU));
            }
            if (req->u1 == 2) {
                return self->i2cTransport_.writeReg16(reg, static_cast<uint16_t>(req->u2 & 0xFFFFU));
            }
            if (req->u1 == 3) {
                return self->i2cTransport_.writeReg24(reg, req->u2 & 0xFFFFFFU);
            }
            return kIOReturnBadArgument;
        }

        case kOpConfigureSensor:
            if (!self->powerHal_.isPowered() || !self->sensorDriver_) {
                return kIOReturnNotReady;
            }
            return self->sensorDriver_->configureMode(&self->i2cTransport_, req->u0, req->u1);

        case kOpSetStream:
            return self->setStreamLocked(req->b0, req->client);

        case kOpSetExposureGain:
            if (!self->powerHal_.isPowered() || !self->sensorDriver_) {
                return kIOReturnNotReady;
            }
            return self->sensorDriver_->setExposureAndGain(&self->i2cTransport_, req->u0, req->u1, req->u2);

        case kOpClientDisconnect:
            if (self->activeOwner_ == req->client) {
                self->forceIdleHardwareLocked();
                self->activeOwner_ = nullptr;
            }
            return kIOReturnSuccess;

        case kOpSleepOff:
            self->forceIdleHardwareLocked();
            self->activeOwner_ = nullptr;
            return kIOReturnSuccess;
    }

    return kIOReturnBadArgument;
}

void UniCamCoreService::fillInfoLocked(UniCamBoardSensorInfo *outInfo) {
    if (!outInfo) {
        return;
    }
    *outInfo = boardInfo_;
    outInfo->powerOn = powerHal_.isPowered() ? 1 : 0;
    outInfo->ledOn = powerHal_.isLedOn() ? 1 : 0;
    outInfo->streaming = streaming_ ? 1 : 0;
}

void UniCamCoreService::fillDiagLocked(UniCamHardwareDiag *outDiag) {
    if (!outDiag) {
        return;
    }
    memset(outDiag, 0, sizeof(*outDiag));
    powerHal_.readDiagnostics(outDiag);
    outDiag->streamStateOn = streaming_ ? 1 : 0;
    outDiag->i2cTxCount = i2cTransport_.txCount();
    outDiag->i2cRxCount = i2cTransport_.rxCount();
    outDiag->i2cErrCount = i2cTransport_.errCount();
}

IOReturn UniCamCoreService::setPowerLocked(bool enable, bool forceMmio, UniCamCoreUserClient *owner) {
    if (sleepSuspended_ && enable) {
        return kIOReturnNotReady;
    }
    if (!enable && streaming_ && sensorDriver_) {
        sensorDriver_->setStreaming(&i2cTransport_, false);
        streaming_ = false;
    }
    const IOReturn ret = powerHal_.setPower(enable, forceMmio);
    if (ret == kIOReturnSuccess) {
        if (enable) {
            activeOwner_ = owner;
        } else if (activeOwner_ == owner) {
            activeOwner_ = nullptr;
        }
    }
    return ret;
}

IOReturn UniCamCoreService::setPrivacyLedLocked(bool enable, bool forceMmio, UniCamCoreUserClient *owner) {
    if (sleepSuspended_ && enable) {
        return kIOReturnNotReady;
    }
    const IOReturn ret = powerHal_.setPrivacyLed(enable, forceMmio);
    if (ret == kIOReturnSuccess && enable && owner) {
        activeOwner_ = owner;
    }
    return ret;
}

IOReturn UniCamCoreService::setStreamLocked(bool enable, UniCamCoreUserClient *owner) {
    if (!sensorDriver_ || !powerHal_.isPowered()) {
        return kIOReturnNotReady;
    }
    const IOReturn ret = sensorDriver_->setStreaming(&i2cTransport_, enable);
    if (ret == kIOReturnSuccess) {
        streaming_ = enable;
        if (enable && owner) {
            activeOwner_ = owner;
        }
    }
    return ret;
}

void UniCamCoreService::forceIdleHardwareLocked() {
    if (streaming_ && sensorDriver_ && powerHal_.isPowered()) {
        sensorDriver_->setStreaming(&i2cTransport_, false);
    }
    streaming_ = false;
    if (powerHal_.isLedOn()) {
        powerHal_.setPrivacyLed(false, false);
    }
    if (powerHal_.isPowered()) {
        powerHal_.setPower(false, false);
    }
}

IOReturn UniCamCoreService::probeSensorLocked(uint32_t holdLedMs, bool forceMmio, UniCamProbeReport *outReport) {
    if (!outReport || !sensorDriver_) {
        return kIOReturnBadArgument;
    }
    if (sleepSuspended_) {
        return kIOReturnNotReady;
    }

    memset(outReport, 0, sizeof(*outReport));
    outReport->expectedChipId = sensorDriver_->expectedChipId();
    fillDiagLocked(&outReport->diagBefore);

    const bool wasPowered = powerHal_.isPowered();
    const bool wasLedOn = powerHal_.isLedOn();

    IOReturn pwrOnRet = kIOReturnSuccess;
    if (!wasPowered) {
        pwrOnRet = powerHal_.setPower(true, forceMmio);
    }
    outReport->powerOnRet = pwrOnRet;

    if (pwrOnRet == kIOReturnSuccess) {
        powerHal_.setPrivacyLed(true, forceMmio);
        IOSleep(5);

        outReport->i2cRet = sensorDriver_->identify(&i2cTransport_, &outReport->chipId, outReport->rawIdBytes);
        outReport->chipIdMatched = (outReport->i2cRet == kIOReturnSuccess &&
                                    outReport->chipId == outReport->expectedChipId) ? 1 : 0;

        // Capture live hardware GPIO DW0 & ICLK register states while power + LED are ON
        fillDiagLocked(&outReport->diagActive);

        if (holdLedMs > 0) {
            const uint32_t clampedMs = (holdLedMs > 10000U) ? 10000U : holdLedMs;
            IOSleep(clampedMs);
        }

        if (!wasLedOn) {
            powerHal_.setPrivacyLed(false, forceMmio);
        }
    } else {
        outReport->i2cRet = pwrOnRet;
        fillDiagLocked(&outReport->diagActive);
    }

    if (!wasPowered) {
        outReport->powerOffRet = powerHal_.setPower(false, forceMmio);
    } else {
        outReport->powerOffRet = kIOReturnSuccess;
    }

    fillDiagLocked(&outReport->diagAfter);

    IOLog("UniCamCore: Probe chipId=0x%06x (expected 0x%06x) i2cRet=0x%x matched=%u\n",
          outReport->chipId, outReport->expectedChipId, outReport->i2cRet, outReport->chipIdMatched);
    return (outReport->i2cRet == kIOReturnSuccess) ? kIOReturnSuccess : outReport->i2cRet;
}

IOReturn UniCamCoreService::getInfo(UniCamBoardSensorInfo *outInfo) {
    if (!commandGate_ || !outInfo) return kIOReturnBadArgument;
    GatedRequest req = { kOpGetInfo, 0, 0, 0, false, false, outInfo, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::getDiagnostics(UniCamHardwareDiag *outDiag) {
    if (!commandGate_ || !outDiag) return kIOReturnBadArgument;
    GatedRequest req = { kOpGetDiag, 0, 0, 0, false, false, outDiag, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::probeSensor(uint32_t holdLedMs, bool forceMmio, UniCamProbeReport *outReport) {
    if (!commandGate_ || !outReport) return kIOReturnBadArgument;
    GatedRequest req = { kOpProbeSensor, holdLedMs, 0, 0, forceMmio, false, outReport, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::setPower(bool enable, bool forceMmio, UniCamCoreUserClient *owner) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpSetPower, 0, 0, 0, enable, forceMmio, nullptr, owner };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::setPrivacyLed(bool enable, bool forceMmio, UniCamCoreUserClient *owner) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpSetLed, 0, 0, 0, enable, forceMmio, nullptr, owner };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::resetSensor(bool forceMmio) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpResetSensor, 0, 0, 0, forceMmio, false, nullptr, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::readRegister(uint16_t reg, uint8_t width, uint32_t *outVal) {
    if (!commandGate_ || !outVal) return kIOReturnBadArgument;
    GatedRequest req = { kOpReadReg, reg, width, 0, false, false, outVal, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::writeRegister(uint16_t reg, uint8_t width, uint32_t val) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpWriteReg, reg, width, val, false, false, nullptr, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::configureSensor(uint32_t modeIndex, uint32_t testPattern) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpConfigureSensor, modeIndex, testPattern, 0, false, false, nullptr, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::setStream(bool enable, UniCamCoreUserClient *owner) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpSetStream, 0, 0, 0, enable, false, nullptr, owner };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

IOReturn UniCamCoreService::setExposureGain(uint32_t exposureLines, uint32_t analogGain, uint32_t digitalGain) {
    if (!commandGate_) return kIOReturnNotReady;
    GatedRequest req = { kOpSetExposureGain, exposureLines, analogGain, digitalGain, false, false, nullptr, nullptr };
    return commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}

void UniCamCoreService::handleClientDisconnect(UniCamCoreUserClient *client) {
    if (!commandGate_ || !client) return;
    GatedRequest req = { kOpClientDisconnect, 0, 0, 0, false, false, nullptr, client };
    commandGate_->runAction(&UniCamCoreService::gatedActionTrampoline, &req);
}
