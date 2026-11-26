#include "AcpiMethodPowerHal.hpp"
#include "UniCamKernelGate.h"

#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <string.h>

namespace {

bool readPciIdsFromEntry(const IORegistryEntry *entry, uint16_t *outVid, uint16_t *outDid) {
    if (!entry || !outVid || !outDid) {
        return false;
    }
    OSData *vidData = OSDynamicCast(OSData, entry->getProperty("vendor-id"));
    OSData *didData = OSDynamicCast(OSData, entry->getProperty("device-id"));
    if (!vidData || !didData || vidData->getLength() < 2 || didData->getLength() < 2) {
        return false;
    }
    const uint8_t *vp = static_cast<const uint8_t *>(vidData->getBytesNoCopy());
    const uint8_t *dp = static_cast<const uint8_t *>(didData->getBytesNoCopy());
    if (!vp || !dp) {
        return false;
    }
    *outVid = static_cast<uint16_t>(
        static_cast<uint16_t>(vp[0]) | (static_cast<uint16_t>(vp[1]) << 8U));
    *outDid = static_cast<uint16_t>(
        static_cast<uint16_t>(dp[0]) | (static_cast<uint16_t>(dp[1]) << 8U));
    return true;
}

bool isIceLakePlatform(IOService *provider) {
    for (IORegistryEntry *cur = provider;
         cur != nullptr;
         cur = cur->getParentEntry(gIOServicePlane)) {
        uint16_t vid = 0;
        uint16_t did = 0;
        if (readPciIdsFromEntry(cur, &vid, &did) && vid == 0x8086U &&
            ((did & 0xFF00U) == 0x3400U || (did & 0xFF00U) == 0x3800U)) {
            return true;
        }
    }

    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    if (!matching) {
        return false;
    }
    OSIterator *iter = IOService::getMatchingServices(matching);
    matching->release();
    if (!iter) {
        return false;
    }

    bool foundIpu4p = false;
    OSObject *obj = nullptr;
    while ((obj = iter->getNextObject()) != nullptr) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, obj);
        uint16_t vid = 0;
        uint16_t did = 0;
        if (readPciIdsFromEntry(entry, &vid, &did) && vid == 0x8086U && did == 0x8A19U) {
            foundIpu4p = true;
            break;
        }
    }
    iter->release();
    return foundIpu4p;
}

} // namespace

AcpiMethodPowerHal::AcpiMethodPowerHal()
    : provider_(nullptr),
      acpiDevice_(nullptr),
      deviceProps_(nullptr),
      useTps68470_(false),
      hasUcin_(false),
      hasUcpw_(false),
      hasUcld_(false),
      hasUcrs_(false),
      capMask_(0),
      powered_(false),
      ledOn_(false),
      lastUsedMmio_(false) {
}

AcpiMethodPowerHal::~AcpiMethodPowerHal() {
    cleanup();
}

OSObject *AcpiMethodPowerHal::lookupProperty(const char *key) const {
    if (!key) {
        return nullptr;
    }

    // 1. AppleACPIPlatform stores _DSM key/value pairs inside "device-properties"
    if (deviceProps_) {
        if (OSObject *obj = deviceProps_->getObject(key)) {
            return obj;
        }
    }

    // 2. Fallback: direct property on IOACPIPlatformDevice
    if (acpiDevice_) {
        if (OSObject *obj = acpiDevice_->getProperty(key)) {
            return obj;
        }
    }

    // 3. Fallback: property on VoodooI2CDeviceNub provider
    if (provider_) {
        if (OSObject *obj = provider_->getProperty(key)) {
            return obj;
        }
    }

    return nullptr;
}

bool AcpiMethodPowerHal::readUInt64Prop(const char *key, uint64_t *outVal) const {
    OSObject *obj = lookupProperty(key);
    if (!obj || !outVal) {
        return false;
    }

    if (OSNumber *num = OSDynamicCast(OSNumber, obj)) {
        *outVal = num->unsigned64BitValue();
        return true;
    }

    if (OSData *data = OSDynamicCast(OSData, obj)) {
        const unsigned int len = data->getLength();
        const uint8_t *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
        if (!bytes || len == 0) {
            return false;
        }
        uint64_t val = 0;
        const unsigned int copyLen = (len > 8) ? 8 : len;
        for (unsigned int i = 0; i < copyLen; ++i) {
            val |= (static_cast<uint64_t>(bytes[i]) << (8U * i));
        }
        *outVal = val;
        return true;
    }

    return false;
}

bool AcpiMethodPowerHal::readUInt32Prop(const char *key, uint32_t *outVal) const {
    uint64_t tmp = 0;
    if (!readUInt64Prop(key, &tmp)) {
        return false;
    }
    *outVal = static_cast<uint32_t>(tmp & 0xFFFFFFFFULL);
    return true;
}

bool AcpiMethodPowerHal::readStringProp(const char *key, char *outBuf, size_t bufSize) const {
    if (!outBuf || bufSize == 0) {
        return false;
    }
    outBuf[0] = '\0';

    OSObject *obj = lookupProperty(key);
    if (!obj) {
        return false;
    }

    if (OSString *str = OSDynamicCast(OSString, obj)) {
        strlcpy(outBuf, str->getCStringNoCopy(), bufSize);
        return true;
    }

    if (OSData *data = OSDynamicCast(OSData, obj)) {
        const unsigned int len = data->getLength();
        const char *bytes = static_cast<const char *>(data->getBytesNoCopy());
        if (!bytes || len == 0) {
            return false;
        }
        size_t copyLen = (len < (bufSize - 1)) ? len : (bufSize - 1);
        memcpy(outBuf, bytes, copyLen);
        outBuf[copyLen] = '\0';
        return true;
    }

    return false;
}

IOReturn AcpiMethodPowerHal::evaluateIntMethod(const char *methodName,
                                               bool hasArg,
                                               uint32_t argVal,
                                               UInt32 *outResult) {
    if (!acpiDevice_ || !methodName) {
        return kIOReturnNotReady;
    }

    UInt32 dummy = 0;
    UInt32 *resPtr = outResult ? outResult : &dummy;
    *resPtr = 0;

    if (!hasArg) {
        return acpiDevice_->evaluateInteger(methodName, resPtr, nullptr, 0);
    }

    OSNumber *argNum = OSNumber::withNumber(static_cast<unsigned long long>(argVal), 32);
    if (!argNum) {
        return kIOReturnNoMemory;
    }

    OSObject *params[1] = { argNum };
    const IOReturn ret = acpiDevice_->evaluateInteger(methodName, resPtr, params, 1);
    argNum->release();
    return ret;
}

bool AcpiMethodPowerHal::init(IOService *provider, UniCamBoardSensorInfo *outInfo) {
    cleanup();
    if (!provider || !outInfo) {
        return false;
    }

    const bool iceLakeOk = isIceLakePlatform(provider);
    if (!iceLakeOk && !unicam::hasBootArg("-unicamtest")) {
        IOLog("UniCamCore: Host platform is not Intel Ice Lake (IPU4P); refusing UNIC0001 power HAL init for PCH MMIO safety\n");
        return false;
    }

    provider_ = provider;

    // Extract IOACPIPlatformDevice attached by VoodooI2CDeviceNub::attach()
    OSObject *acpiProp = provider->getProperty("acpi-device");
    acpiDevice_ = OSDynamicCast(IOACPIPlatformDevice, acpiProp);
    if (!acpiDevice_) {
        acpiDevice_ = OSDynamicCast(IOACPIPlatformDevice, provider);
    }

    if (acpiDevice_) {
        acpiDevice_->retain();
        if (OSObject *dp = acpiDevice_->getProperty("device-properties")) {
            deviceProps_ = OSDynamicCast(OSDictionary, dp);
            if (deviceProps_) {
                deviceProps_->retain();
            }
        }
    }

    if (!deviceProps_) {
        if (OSObject *dp = provider->getProperty("device-properties")) {
            deviceProps_ = OSDynamicCast(OSDictionary, dp);
            if (deviceProps_) {
                deviceProps_->retain();
            }
        }
    }

    memset(outInfo, 0, sizeof(*outInfo));
    outInfo->schemaVersion = UNICAM_SCHEMA_VERSION;
    readUInt32Prop("unicam-schema", &outInfo->schemaVersion);
    readStringProp("unicam-board-id", outInfo->boardId, sizeof(outInfo->boardId));
    readStringProp("unicam-ipu-gen", outInfo->ipuGen, sizeof(outInfo->ipuGen));
    readStringProp("unicam-ipu-path", outInfo->ipuPath, sizeof(outInfo->ipuPath));
    readStringProp("unicam-sensor-hid", outInfo->sensorHid, sizeof(outInfo->sensorHid));
    readStringProp("unicam-sensor-compat", outInfo->sensorCompat, sizeof(outInfo->sensorCompat));
    readStringProp("unicam-position", outInfo->position, sizeof(outInfo->position));

    readUInt32Prop("unicam-i2c-bus", &outInfo->i2cBus);
    readUInt32Prop("unicam-i2c-addr", &outInfo->i2cAddr);
    readUInt32Prop("unicam-eeprom-addr", &outInfo->eepromAddr);
    readUInt32Prop("unicam-csi-port", &outInfo->csiPort);
    readUInt32Prop("unicam-csi-lanes", &outInfo->csiLanes);
    readUInt64Prop("unicam-link-freq-hz", &outInfo->linkFreqHz);
    readUInt64Prop("unicam-mclk-hz", &outInfo->mclkHz);
    readUInt32Prop("unicam-mclk-src", &outInfo->mclkSrc);
    readUInt64Prop("unicam-mclk-reg", &outInfo->mclkRegPhysAddr);
    readUInt32Prop("unicam-rotation", &outInfo->rotation);

    // Fallback: if unicam-mclk-reg wasn't specified, compute Ice Lake PCH ICLK address
    if (iceLakeOk && outInfo->mclkRegPhysAddr == 0 && outInfo->mclkSrc < 6) {
        outInfo->mclkRegPhysAddr = 0xFDAD8000ULL + (static_cast<uint64_t>(outInfo->mclkSrc) * 0x0CULL);
    }

    UniCamGpioPinSpec pins[kUniCamGpioRoleCount];
    memset(pins, 0, sizeof(pins));
    const char *roleKeys[kUniCamGpioRoleCount] = {
        "reset", "pwdn", "power", "clken", "led"
    };
    const uint32_t roleDelays[kUniCamGpioRoleCount] = {
        10000, 5000, 10000, 5000, 0
    };

    for (uint32_t i = 0; i < kUniCamGpioRoleCount; ++i) {
        char dw0Key[40];
        char actKey[40];
        snprintf(dw0Key, sizeof(dw0Key), "unicam-gpio-%s-dw0", roleKeys[i]);
        snprintf(actKey, sizeof(actKey), "unicam-gpio-%s-act", roleKeys[i]);

        uint64_t dw0Addr = 0;
        uint32_t actHigh = 1;
        readUInt64Prop(dw0Key, &dw0Addr);
        readUInt32Prop(actKey, &actHigh);

        pins[i].role = i;
        pins[i].dw0PhysAddr = dw0Addr;
        pins[i].activeHigh = actHigh ? 1U : 0U;
        pins[i].delayUs = roleDelays[i];
    }

    if (iceLakeOk) {
        mmioHal_.init(pins, outInfo->mclkRegPhysAddr, outInfo->mclkHz);
    }

    char pmicType[32] = {};
    readStringProp("unicam-pmic-type", pmicType, sizeof(pmicType));
    useTps68470_ = (strcmp(pmicType, "tps68470") == 0 || strcmp(pmicType, "TPS68470") == 0);
    if (useTps68470_) {
        UniCamTps68470Config tpsCfg{};
        tpsCfg.mclkHz = outInfo->mclkHz ? outInfo->mclkHz : 19200000ULL;
        tpsCfg.dvddMv = 1200U;
        tpsCfg.avddMv = 2815U;
        tpsCfg.dovddMv = 1800U;
        tpsCfg.resetPin = 9U;
        tpsCfg.pwdnPin = 7U;
        tpsCfg.ledPin = 0xFFU;
        tpsCfg.resetActiveHigh = 0U;
        tpsCfg.pwdnActiveHigh = 0U;
        tpsCfg.ledActiveHigh = 1U;
        tpsCfg.daisyChain = 1U;
        (void)readUInt32Prop("unicam-pmic-dvdd-mv", &tpsCfg.dvddMv);
        (void)readUInt32Prop("unicam-pmic-avdd-mv", &tpsCfg.avddMv);
        (void)readUInt32Prop("unicam-pmic-dovdd-mv", &tpsCfg.dovddMv);
        (void)readUInt32Prop("unicam-pmic-reset-pin", &tpsCfg.resetPin);
        (void)readUInt32Prop("unicam-pmic-pwdn-pin", &tpsCfg.pwdnPin);
        (void)readUInt32Prop("unicam-pmic-led-pin", &tpsCfg.ledPin);
        uint32_t tmp = 0U;
        if (readUInt32Prop("unicam-pmic-reset-act", &tmp)) tpsCfg.resetActiveHigh = tmp ? 1U : 0U;
        if (readUInt32Prop("unicam-pmic-pwdn-act", &tmp)) tpsCfg.pwdnActiveHigh = tmp ? 1U : 0U;
        if (readUInt32Prop("unicam-pmic-led-act", &tmp)) tpsCfg.ledActiveHigh = tmp ? 1U : 0U;
        if (readUInt32Prop("unicam-pmic-daisy-chain", &tmp)) tpsCfg.daisyChain = tmp ? 1U : 0U;
        tpsHal_.configure(tpsCfg);
    }

    if (acpiDevice_) {
        hasUcin_ = (acpiDevice_->validateObject("UCIN") == kIOReturnSuccess);
        hasUcpw_ = (acpiDevice_->validateObject("UCPW") == kIOReturnSuccess);
        hasUcld_ = (acpiDevice_->validateObject("UCLD") == kIOReturnSuccess);
        hasUcrs_ = (acpiDevice_->validateObject("UCRS") == kIOReturnSuccess);
    }

    if (hasUcin_) {
        UInt32 mask = 0;
        if (evaluateIntMethod("UCIN", false, 0, &mask) == kIOReturnSuccess) {
            capMask_ = mask;
        }
    } else if (useTps68470_) {
        capMask_ = kUniCamCapReady | kUniCamCapI2cPmic;
    } else {
        capMask_ = kUniCamCapReady |
                   ((pins[kUniCamGpioLed].dw0PhysAddr != 0) ? kUniCamCapHasLed : 0U) |
                   kUniCamCapDiscretePmic;
    }

    outInfo->acpiCapMask = capMask_;
    outInfo->hasAcpiMethods = (hasUcpw_ && hasUcld_) ? 1 : 0;

    IOLog("UniCamCore: Board=%s sensor=%s(%s) I2C%u@0x%02x MIPI=P%u/%ul MCLK=%lluHz(0x%llx) ACPI=%u\n",
          outInfo->boardId[0] ? outInfo->boardId : "<unknown>",
          outInfo->sensorCompat[0] ? outInfo->sensorCompat : "<unknown>",
          outInfo->sensorHid[0] ? outInfo->sensorHid : "?",
          outInfo->i2cBus,
          outInfo->i2cAddr,
          outInfo->csiPort,
          outInfo->csiLanes,
          outInfo->mclkHz,
          outInfo->mclkRegPhysAddr,
          outInfo->hasAcpiMethods);

    return (outInfo->i2cAddr != 0) || hasUcpw_;
}

void AcpiMethodPowerHal::cleanup() {
    mmioHal_.cleanup();
    tpsHal_.cleanup();
    if (deviceProps_) {
        deviceProps_->release();
        deviceProps_ = nullptr;
    }
    if (acpiDevice_) {
        acpiDevice_->release();
        acpiDevice_ = nullptr;
    }
    provider_ = nullptr;
    useTps68470_ = false;
    hasUcin_ = false;
    hasUcpw_ = false;
    hasUcld_ = false;
    hasUcrs_ = false;
    powered_ = false;
    ledOn_ = false;
}

IOReturn AcpiMethodPowerHal::setPower(bool enable, bool forceMmio) {
    IOReturn ret = kIOReturnUnsupported;
    if (!forceMmio && useTps68470_) {
        ret = tpsHal_.setPower(enable);
        lastUsedMmio_ = false;
    } else if (!forceMmio && hasUcpw_) {
        UInt32 status = 0;
        ret = evaluateIntMethod("UCPW", true, enable ? 1U : 0U, &status);
        lastUsedMmio_ = false;
        if (ret == kIOReturnSuccess && status != 0) {
            ret = kIOReturnError;
        }
    } else {
        ret = mmioHal_.setPower(enable);
        lastUsedMmio_ = true;
    }

    if (ret == kIOReturnSuccess) {
        powered_ = enable;
        if (!enable) {
            ledOn_ = false;
        }
    }
    return ret;
}

IOReturn AcpiMethodPowerHal::setPrivacyLed(bool enable, bool forceMmio) {
    IOReturn ret = kIOReturnUnsupported;
    if (!forceMmio && useTps68470_) {
        ret = tpsHal_.setPrivacyLed(enable);
        lastUsedMmio_ = false;
    } else if (!forceMmio && hasUcld_) {
        UInt32 status = 0;
        ret = evaluateIntMethod("UCLD", true, enable ? 1U : 0U, &status);
        lastUsedMmio_ = false;
        if (ret == kIOReturnSuccess && status != 0) {
            ret = kIOReturnError;
        }
    } else {
        ret = mmioHal_.setPrivacyLed(enable);
        lastUsedMmio_ = true;
    }

    if (ret == kIOReturnSuccess) {
        ledOn_ = enable;
    }
    return ret;
}

IOReturn AcpiMethodPowerHal::pulseReset(bool forceMmio) {
    if (!forceMmio && useTps68470_) {
        lastUsedMmio_ = false;
        return tpsHal_.pulseReset();
    }
    if (!forceMmio && hasUcrs_) {
        UInt32 status = 0;
        lastUsedMmio_ = false;
        IOReturn ret = evaluateIntMethod("UCRS", false, 0, &status);
        return (ret == kIOReturnSuccess && status != 0) ? kIOReturnError : ret;
    }
    lastUsedMmio_ = true;
    return mmioHal_.pulseReset();
}

void AcpiMethodPowerHal::readDiagnostics(UniCamHardwareDiag *outDiag) {
    if (!outDiag) {
        return;
    }
    if (useTps68470_) {
        tpsHal_.readDiagnostics(outDiag);
    } else {
        mmioHal_.readDiagnostics(outDiag);
    }
    outDiag->powerStateOn = powered_ ? 1 : 0;
    outDiag->ledStateOn = ledOn_ ? 1 : 0;
    outDiag->usingMmioHal = lastUsedMmio_ ? 1 : 0;
}
