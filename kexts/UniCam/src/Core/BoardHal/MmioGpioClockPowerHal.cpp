#include "MmioGpioClockPowerHal.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace {

constexpr uint64_t kPchSidebandBase = 0xFD000000ULL;
constexpr uint64_t kPchSidebandEnd  = 0xFE000000ULL;

// Intel Ice Lake PCH GPIO PADCFG DW0 bit definitions:
//  - Bit 0     : GPIOTxState
//  - Bit 8     : GPIOTxDis (0 = TX enabled / output active)
//  - Bits 10..12: PMode (0 = GPIO mode)
constexpr uint32_t kGpioDw0OutputMask = 0xFFFFE2FEU;
constexpr uint32_t kGpioDw0TxStateBit = (1U << 0);

// Intel ICLK IMGCLKOUT_x byte layout:
//  - Bit 0: Frequency select (1 = 19.2 MHz, 0 = 24 MHz)
//  - Bit 1: Clock request enable (1 = ON, 0 = OFF)
constexpr uint32_t kIclkControlMask  = 0xFFFFFFFCU;
constexpr uint32_t kIclkEnableBit    = (1U << 1);
constexpr uint32_t kIclkFreq19_2Bit  = (1U << 0);

bool isValidPchMmioAddr(uint64_t addr) {
    return (addr >= kPchSidebandBase) && (addr < kPchSidebandEnd) && ((addr & 0x3ULL) == 0);
}

} // namespace

MmioGpioClockPowerHal::MmioGpioClockPowerHal()
    : mclkHz_(19200000ULL) {
    memset(pins_, 0, sizeof(pins_));
    memset(&mclkReg_, 0, sizeof(mclkReg_));
}

MmioGpioClockPowerHal::~MmioGpioClockPowerHal() {
    cleanup();
}

bool MmioGpioClockPowerHal::mapPhysicalDword(uint64_t physAddr, MappedReg *outReg) {
    if (!isValidPchMmioAddr(physAddr)) {
        return false;
    }

    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
        static_cast<IOPhysicalAddress>(physAddr),
        8, // Map DW0 + DW1 (8 bytes) for diagnostics and GPIO control
        kIODirectionInOut);
    if (!desc) {
        IOLog("UniCamCore: MmioHal failed to create descriptor for 0x%llx\n", physAddr);
        return false;
    }

    IOMemoryMap *map = desc->map(kIOMapInhibitCache);
    if (!map) {
        IOLog("UniCamCore: MmioHal failed to map 0x%llx\n", physAddr);
        desc->release();
        return false;
    }

    outReg->physAddr = physAddr;
    outReg->desc = desc;
    outReg->map = map;
    outReg->ptr32 = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    outReg->valid = (outReg->ptr32 != nullptr);
    return outReg->valid;
}

void MmioGpioClockPowerHal::unmapReg(MappedReg *reg) {
    if (reg->map) {
        reg->map->release();
        reg->map = nullptr;
    }
    if (reg->desc) {
        reg->desc->release();
        reg->desc = nullptr;
    }
    reg->ptr32 = nullptr;
    reg->valid = false;
}

bool MmioGpioClockPowerHal::init(const UniCamGpioPinSpec pins[kUniCamGpioRoleCount],
                                 uint64_t mclkRegPhysAddr,
                                 uint64_t mclkHz) {
    cleanup();
    mclkHz_ = (mclkHz != 0) ? mclkHz : 19200000ULL;

    for (uint32_t i = 0; i < kUniCamGpioRoleCount; ++i) {
        pins_[i].activeHigh = pins[i].activeHigh ? 1U : 0U;
        pins_[i].delayUs = pins[i].delayUs;
        if (pins[i].dw0PhysAddr != 0) {
            mapPhysicalDword(pins[i].dw0PhysAddr, &pins_[i]);
        }
    }

    if (mclkRegPhysAddr != 0) {
        mapPhysicalDword(mclkRegPhysAddr, &mclkReg_);
    }

    return true;
}

void MmioGpioClockPowerHal::cleanup() {
    for (uint32_t i = 0; i < kUniCamGpioRoleCount; ++i) {
        unmapReg(&pins_[i]);
    }
    unmapReg(&mclkReg_);
}

IOReturn MmioGpioClockPowerHal::writeGpioPin(uint32_t role, bool sensorOn) {
    if (role >= kUniCamGpioRoleCount) {
        return kIOReturnBadArgument;
    }
    MappedReg &reg = pins_[role];
    if (!reg.valid || !reg.ptr32) {
        return kIOReturnSuccess; // Optional pin not present on board
    }

    const uint32_t onLevel = reg.activeHigh ? 1U : 0U;
    const uint32_t level = sensorOn ? onLevel : (1U - onLevel);

    uint32_t dw0 = reg.ptr32[0];
    dw0 &= kGpioDw0OutputMask;
    if (level != 0) {
        dw0 |= kGpioDw0TxStateBit;
    }
    reg.ptr32[0] = dw0;
    (void)reg.ptr32[0]; // Readback flush posted write
    return kIOReturnSuccess;
}

IOReturn MmioGpioClockPowerHal::writeMclk(bool enable) {
    if (!mclkReg_.valid || !mclkReg_.ptr32) {
        return kIOReturnNotReady;
    }

    const uint32_t freqBit = (mclkHz_ == 19200000ULL) ? kIclkFreq19_2Bit : 0U;
    uint32_t val = mclkReg_.ptr32[0];
    val &= kIclkControlMask;
    val |= freqBit;
    if (enable) {
        val |= kIclkEnableBit;
    }
    mclkReg_.ptr32[0] = val;
    (void)mclkReg_.ptr32[0];
    return kIOReturnSuccess;
}

IOReturn MmioGpioClockPowerHal::setPower(bool enable) {
    if (enable) {
        writeGpioPin(kUniCamGpioReset, false);
        writeGpioPin(kUniCamGpioPwdn, false);
        writeGpioPin(kUniCamGpioPower, true);
        IOSleep(10);
        writeMclk(true);
        writeGpioPin(kUniCamGpioClkEn, true);
        IOSleep(5);
        writeGpioPin(kUniCamGpioPwdn, true);
        writeGpioPin(kUniCamGpioReset, true);
        IOSleep(10);
    } else {
        writeGpioPin(kUniCamGpioLed, false);
        writeGpioPin(kUniCamGpioReset, false);
        writeGpioPin(kUniCamGpioPwdn, false);
        IOSleep(2);
        writeGpioPin(kUniCamGpioClkEn, false);
        writeMclk(false);
        writeGpioPin(kUniCamGpioPower, false);
    }
    return kIOReturnSuccess;
}

IOReturn MmioGpioClockPowerHal::setPrivacyLed(bool enable) {
    return writeGpioPin(kUniCamGpioLed, enable);
}

IOReturn MmioGpioClockPowerHal::pulseReset() {
    writeGpioPin(kUniCamGpioReset, false);
    IOSleep(5);
    writeGpioPin(kUniCamGpioReset, true);
    IOSleep(10);
    return kIOReturnSuccess;
}

void MmioGpioClockPowerHal::readDiagnostics(UniCamHardwareDiag *outDiag) const {
    if (!outDiag) {
        return;
    }

    for (uint32_t i = 0; i < kUniCamGpioRoleCount; ++i) {
        UniCamGpioDiag &dst = outDiag->gpios[i];
        const MappedReg &src = pins_[i];
        dst.role = i;
        dst.activeHigh = src.activeHigh;
        dst.dw0PhysAddr = src.physAddr;
        if (src.valid && src.ptr32) {
            const uint32_t dw0 = src.ptr32[0];
            const uint32_t dw1 = src.ptr32[1];
            dst.dw0Val = dw0;
            dst.dw1Val = dw1;
            dst.mapped = 1;
            dst.txState = static_cast<uint8_t>(dw0 & 0x1U);
            dst.txDisabled = static_cast<uint8_t>((dw0 >> 8) & 0x1U);
            dst.padMode = static_cast<uint8_t>((dw0 >> 10) & 0x7U);
        } else {
            dst.dw0Val = 0;
            dst.dw1Val = 0;
            dst.mapped = 0;
            dst.txState = 0;
            dst.txDisabled = 0;
            dst.padMode = 0;
        }
    }

    outDiag->mclkRegPhysAddr = mclkReg_.physAddr;
    if (mclkReg_.valid && mclkReg_.ptr32) {
        const uint32_t regVal = mclkReg_.ptr32[0];
        outDiag->mclkRegVal = regVal;
        outDiag->mclkMapped = 1;
        outDiag->mclkEnabled = static_cast<uint8_t>((regVal & kIclkEnableBit) ? 1 : 0);
        outDiag->mclkFreq19_2 = static_cast<uint8_t>((regVal & kIclkFreq19_2Bit) ? 1 : 0);
    } else {
        outDiag->mclkRegVal = 0;
        outDiag->mclkMapped = 0;
        outDiag->mclkEnabled = 0;
        outDiag->mclkFreq19_2 = 0;
    }
}
