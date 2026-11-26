#include "Tps68470PowerHal.hpp"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <string.h>

namespace {

UniCamPmicService *gSharedPmicInstance = nullptr;
constexpr uint32_t kMaxI2CRetries = 3U;

// TI TPS68470 register map (include/linux/mfd/tps68470.h)
constexpr uint8_t kRegPostDiv2  = 0x06U;
constexpr uint8_t kRegBoostDiv  = 0x07U;
constexpr uint8_t kRegBuckDiv   = 0x08U;
constexpr uint8_t kRegPllSwr    = 0x09U;
constexpr uint8_t kRegXtalDiv   = 0x0AU;
constexpr uint8_t kRegPllDiv    = 0x0BU;
constexpr uint8_t kRegPostDiv   = 0x0CU;
constexpr uint8_t kRegPllCtl    = 0x0DU;
constexpr uint8_t kRegClkCfg1   = 0x0FU;
constexpr uint8_t kRegClkCfg2   = 0x10U;
constexpr uint8_t kRegGpCtl0A   = 0x14U;
constexpr uint8_t kRegSgpo      = 0x22U;
constexpr uint8_t kRegGpdo      = 0x27U;
constexpr uint8_t kRegVcmVal    = 0x3CU;
constexpr uint8_t kRegVioVal    = 0x3FU;
constexpr uint8_t kRegVsioVal   = 0x40U;
constexpr uint8_t kRegVaVal     = 0x41U;
constexpr uint8_t kRegVdVal     = 0x42U;
constexpr uint8_t kRegSI2cCtl   = 0x43U;
constexpr uint8_t kRegVcmCtl    = 0x44U;
constexpr uint8_t kRegVaCtl     = 0x47U;
constexpr uint8_t kRegVdCtl     = 0x48U;
constexpr uint8_t kRegRevId     = 0xFFU;

constexpr uint8_t kNumRegularGpios = 7U;
constexpr uint8_t kNumTotalGpios   = 10U;
constexpr uint8_t kGpioModeCmosOut = 0x02U;

uint8_t encodeCoreVoltageMv(uint32_t mv) {
    // TPS68470_CORE: 900 mV + val * 25 mV, val in [0..42]
    if (mv <= 900U) return 0U;
    uint32_t steps = (mv - 900U + 12U) / 25U;
    return static_cast<uint8_t>(steps > 42U ? 42U : steps);
}

uint8_t encodeLdoVoltageMv(uint32_t mv) {
    // TPS68470 LDOs: 875 mV + val * 17.8 mV, val in [0..125]
    if (mv <= 875U) return 0U;
    uint32_t steps = ((mv - 875U) * 10U + 89U) / 178U;
    return static_cast<uint8_t>(steps > 125U ? 125U : steps);
}

} // namespace

// ============================================================================
// UniCamPmicService
// ============================================================================

#define super IOService
OSDefineMetaClassAndStructors(UniCamPmicService, IOService)

bool UniCamPmicService::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }
    nub_ = nullptr;
    return true;
}

IOService *UniCamPmicService::probe(IOService *provider, SInt32 *score) {
    uint32_t offArg = 0;
    if (PE_parse_boot_argn("-unicamoff", &offArg, sizeof(offArg))) {
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

bool UniCamPmicService::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    nub_ = OSDynamicCast(VoodooI2CDeviceNub, provider);
    if (!nub_) {
        return false;
    }
    gSharedPmicInstance = this;
    setName("UniCamPmicService");
    setProperty("unicam-pmic", "tps68470");
    registerService();
    IOLog("UniCamCore: Attached TPS68470 PMIC I2C nub (UNIC0002)\n");
    return true;
}

void UniCamPmicService::stop(IOService *provider) {
    if (gSharedPmicInstance == this) {
        gSharedPmicInstance = nullptr;
    }
    nub_ = nullptr;
    super::stop(provider);
}

VoodooI2CDeviceNub *UniCamPmicService::sharedNub() {
    return gSharedPmicInstance ? gSharedPmicInstance->nub_ : nullptr;
}

// ============================================================================
// Tps68470PowerHal
// ============================================================================

Tps68470PowerHal::Tps68470PowerHal()
    : revId_(0U),
      configured_(false),
      powered_(false),
      ledOn_(false) {
    memset(&cfg_, 0, sizeof(cfg_));
    cfg_.mclkHz = 19200000ULL;
    cfg_.dvddMv = 1200U;
    cfg_.avddMv = 2815U;
    cfg_.dovddMv = 1800U;
    cfg_.resetPin = 9U;
    cfg_.pwdnPin = 7U;
    cfg_.ledPin = 0xFFU;
    cfg_.resetActiveHigh = 0U;
    cfg_.pwdnActiveHigh = 0U;
    cfg_.ledActiveHigh = 1U;
    cfg_.daisyChain = 1U;
}

Tps68470PowerHal::~Tps68470PowerHal() {
    cleanup();
}

void Tps68470PowerHal::configure(const UniCamTps68470Config &cfg) {
    cfg_ = cfg;
    if (cfg_.mclkHz == 0ULL) cfg_.mclkHz = 19200000ULL;
    if (cfg_.dvddMv == 0U) cfg_.dvddMv = 1200U;
    if (cfg_.avddMv == 0U) cfg_.avddMv = 2815U;
    if (cfg_.dovddMv == 0U) cfg_.dovddMv = 1800U;
    configured_ = true;
}

void Tps68470PowerHal::cleanup() {
    configured_ = false;
    powered_ = false;
    ledOn_ = false;
}

VoodooI2CDeviceNub *Tps68470PowerHal::resolveNub() const {
    return UniCamPmicService::sharedNub();
}

IOReturn Tps68470PowerHal::readReg8(uint8_t reg, uint8_t *outVal) const {
    VoodooI2CDeviceNub *nub = resolveNub();
    if (!nub || !outVal) {
        return kIOReturnNotReady;
    }
    IOReturn ret = kIOReturnError;
    for (uint32_t attempt = 0; attempt < kMaxI2CRetries; ++attempt) {
        uint8_t r = reg;
        uint8_t v = 0;
        ret = nub->writeReadI2C(&r, 1, &v, 1);
        if (ret == kIOReturnSuccess) {
            *outVal = v;
            return kIOReturnSuccess;
        }
        if (attempt + 1U < kMaxI2CRetries) {
            IOSleep(1);
        }
    }
    return ret;
}

IOReturn Tps68470PowerHal::writeReg8(uint8_t reg, uint8_t val) const {
    VoodooI2CDeviceNub *nub = resolveNub();
    if (!nub) {
        return kIOReturnNotReady;
    }
    IOReturn ret = kIOReturnError;
    for (uint32_t attempt = 0; attempt < kMaxI2CRetries; ++attempt) {
        uint8_t buf[2] = { reg, val };
        ret = nub->writeI2C(buf, 2);
        if (ret == kIOReturnSuccess) {
            return kIOReturnSuccess;
        }
        if (attempt + 1U < kMaxI2CRetries) {
            IOSleep(1);
        }
    }
    return ret;
}

IOReturn Tps68470PowerHal::updateBits8(uint8_t reg, uint8_t mask, uint8_t val) const {
    uint8_t cur = 0;
    const IOReturn ret = readReg8(reg, &cur);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    const uint8_t next = static_cast<uint8_t>((cur & ~mask) | (val & mask));
    return (next == cur) ? kIOReturnSuccess : writeReg8(reg, next);
}

IOReturn Tps68470PowerHal::writeGpioPin(uint32_t pin, uint8_t activeHigh, bool sensorOn) const {
    if (pin >= kNumTotalGpios) {
        return kIOReturnSuccess; // Optional pin absent
    }
    const uint8_t onLevel = activeHigh ? 1U : 0U;
    const uint8_t level = sensorOn ? onLevel : static_cast<uint8_t>(1U - onLevel);

    if (pin >= kNumRegularGpios) {
        const uint8_t bit = static_cast<uint8_t>(1U << (pin - kNumRegularGpios));
        return updateBits8(kRegSgpo, bit, level ? bit : 0U);
    }

    const uint8_t bit = static_cast<uint8_t>(1U << pin);
    IOReturn ret = updateBits8(kRegGpdo, bit, level ? bit : 0U);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    const uint8_t ctlA = static_cast<uint8_t>(kRegGpCtl0A + pin * 2U);
    return updateBits8(ctlA, 0x03U, kGpioModeCmosOut);
}

IOReturn Tps68470PowerHal::programPllAndClock(bool enable) const {
    if (!enable) {
        IOReturn ret = updateBits8(kRegPllCtl, 0x01U, 0x00U);
        (void)writeReg8(kRegClkCfg1, 0x00U);
        return ret;
    }

    // Frequency table from Linux drivers/clk/clk-tps68470.c (20 MHz XTAL)
    uint8_t pllDiv = 32U;
    uint8_t buckDiv = 2U;
    uint8_t boostDiv = 3U;
    if (cfg_.mclkHz == 20000000ULL) {
        pllDiv = 40U;
        buckDiv = 3U;
        boostDiv = 4U;
    } else if (cfg_.mclkHz == 24000000ULL) {
        pllDiv = 80U;
        buckDiv = 4U;
        boostDiv = 8U;
    }

    IOReturn ret = kIOReturnSuccess;
    if ((ret = writeReg8(kRegBoostDiv, boostDiv)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegBuckDiv, buckDiv)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegPllSwr, 0x03U)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegXtalDiv, 170U)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegPllDiv, pllDiv)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegPostDiv, 1U)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegPostDiv2, 1U)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegClkCfg2, 0x05U)) != kIOReturnSuccess) return ret;

    // Enable HCLK_A & HCLK_B outputs (mode=2 CMOS) and assert PLL_EN (bit 0)
    if ((ret = writeReg8(kRegClkCfg1, 0x0AU)) != kIOReturnSuccess) return ret;
    if ((ret = writeReg8(kRegPllCtl, 0x51U)) != kIOReturnSuccess) return ret;
    IOSleep(5);
    return kIOReturnSuccess;
}

IOReturn Tps68470PowerHal::setPower(bool enable) {
    if (!resolveNub()) {
        // Give VoodooI2C briefly in case UNIC0002 publishes slightly after UNIC0001
        for (int i = 0; i < 20 && !resolveNub(); ++i) {
            IOSleep(25);
        }
    }
    if (!resolveNub()) {
        IOLog("UniCamCore: TPS68470 PMIC nub (UNIC0002) not attached\n");
        return kIOReturnNotReady;
    }

    IOReturn ret = kIOReturnSuccess;
    if (enable) {
        uint8_t rev = 0;
        if (readReg8(kRegRevId, &rev) == kIOReturnSuccess) {
            revId_ = rev;
            IOLog("UniCamCore: TPS68470 rev=0x%02x (mclk=%lluHz)\n", revId_, cfg_.mclkHz);
        }

        // Hold sensor reset/pwdn asserted before bringing up rails
        (void)writeGpioPin(cfg_.resetPin, cfg_.resetActiveHigh, false);
        (void)writeGpioPin(cfg_.pwdnPin, cfg_.pwdnActiveHigh, false);

        // Configure daisy-chain I2C passthrough (GPIO1 & GPIO2 as inputs + VIO/VSIO)
        const uint8_t dovddCode = encodeLdoVoltageMv(cfg_.dovddMv);
        if ((ret = writeReg8(kRegVioVal, dovddCode)) != kIOReturnSuccess) return ret;
        if ((ret = writeReg8(kRegVsioVal, dovddCode)) != kIOReturnSuccess) return ret;
        if (cfg_.daisyChain) {
            (void)updateBits8(kRegGpCtl0A + 2U, 0x03U, 0x00U); // GPCTL1A input
            (void)updateBits8(kRegGpCtl0A + 4U, 0x03U, 0x00U); // GPCTL2A input
            if ((ret = writeReg8(kRegSI2cCtl, 0x03U)) != kIOReturnSuccess) return ret;
        }

        // Program PLL & clock outputs (required before Core buck regulator)
        if ((ret = programPllAndClock(true)) != kIOReturnSuccess) return ret;

        // Program & enable sensor voltage rails (DVDD, AVDD, VCM)
        if ((ret = writeReg8(kRegVdVal, encodeCoreVoltageMv(cfg_.dvddMv))) != kIOReturnSuccess) return ret;
        if ((ret = writeReg8(kRegVaVal, encodeLdoVoltageMv(cfg_.avddMv))) != kIOReturnSuccess) return ret;
        if ((ret = writeReg8(kRegVcmVal, encodeLdoVoltageMv(cfg_.avddMv))) != kIOReturnSuccess) return ret;

        if ((ret = updateBits8(kRegVdCtl, 0x01U, 0x01U)) != kIOReturnSuccess) return ret;
        if ((ret = updateBits8(kRegVaCtl, 0x01U, 0x01U)) != kIOReturnSuccess) return ret;
        (void)updateBits8(kRegVcmCtl, 0x01U, 0x01U);
        IOSleep(5);

        // Release powerdown & reset lines
        (void)writeGpioPin(cfg_.pwdnPin, cfg_.pwdnActiveHigh, true);
        (void)writeGpioPin(cfg_.resetPin, cfg_.resetActiveHigh, true);
        IOSleep(10);
        powered_ = true;
    } else {
        (void)writeGpioPin(cfg_.ledPin, cfg_.ledActiveHigh, false);
        ledOn_ = false;
        (void)writeGpioPin(cfg_.resetPin, cfg_.resetActiveHigh, false);
        (void)writeGpioPin(cfg_.pwdnPin, cfg_.pwdnActiveHigh, false);
        IOSleep(2);

        (void)updateBits8(kRegVcmCtl, 0x01U, 0x00U);
        (void)updateBits8(kRegVaCtl, 0x01U, 0x00U);
        (void)updateBits8(kRegVdCtl, 0x01U, 0x00U);
        if (cfg_.daisyChain) {
            (void)writeReg8(kRegSI2cCtl, 0x00U);
        }
        (void)programPllAndClock(false);
        powered_ = false;
    }
    return kIOReturnSuccess;
}

IOReturn Tps68470PowerHal::setPrivacyLed(bool enable) {
    if (cfg_.ledPin >= kNumTotalGpios) {
        ledOn_ = enable;
        return kIOReturnSuccess;
    }
    const IOReturn ret = writeGpioPin(cfg_.ledPin, cfg_.ledActiveHigh, enable);
    if (ret == kIOReturnSuccess) {
        ledOn_ = enable;
    }
    return ret;
}

IOReturn Tps68470PowerHal::pulseReset() {
    IOReturn ret = writeGpioPin(cfg_.resetPin, cfg_.resetActiveHigh, false);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    IOSleep(5);
    ret = writeGpioPin(cfg_.resetPin, cfg_.resetActiveHigh, true);
    IOSleep(10);
    return ret;
}

void Tps68470PowerHal::readDiagnostics(UniCamHardwareDiag *outDiag) const {
    if (!outDiag) {
        return;
    }
    outDiag->powerStateOn = powered_ ? 1U : 0U;
    outDiag->ledStateOn = ledOn_ ? 1U : 0U;
    outDiag->usingMmioHal = 0U;
    outDiag->mclkRegPhysAddr = 0ULL;
    outDiag->mclkMapped = isAvailable() ? 1U : 0U;
    outDiag->mclkEnabled = powered_ ? 1U : 0U;
    outDiag->mclkFreq19_2 = (cfg_.mclkHz == 19200000ULL) ? 1U : 0U;
    outDiag->reserved0 = revId_;
}
