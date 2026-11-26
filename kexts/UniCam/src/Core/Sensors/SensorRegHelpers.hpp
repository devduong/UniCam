#ifndef UNICAM_SENSOR_REG_HELPERS_HPP
#define UNICAM_SENSOR_REG_HELPERS_HPP

#include <IOKit/IOLib.h>
#include "../I2C/II2CTransport.hpp"

struct UniCamSensorRegVal8 {
    uint16_t reg;
    uint8_t val;
};

static inline uint32_t unicamClampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline IOReturn unicamWriteRegTable8(II2CTransport *i2c,
                                             const char *tag,
                                             const UniCamSensorRegVal8 *table,
                                             size_t count,
                                             uint16_t swResetReg,
                                             uint32_t resetSleepMs) {
    if (!i2c || !table) {
        return kIOReturnBadArgument;
    }
    for (size_t i = 0; i < count; ++i) {
        const IOReturn ret = i2c->writeReg8(table[i].reg, table[i].val);
        if (ret != kIOReturnSuccess) {
            IOLog("UniCamCore: %s write 0x%04x=0x%02x failed (0x%x)\n",
                  tag ? tag : "Sensor", table[i].reg, table[i].val, ret);
            return ret;
        }
        if (swResetReg != 0 && table[i].reg == swResetReg && table[i].val == 0x01 && resetSleepMs > 0) {
            IOSleep(resetSleepMs);
        }
    }
    return kIOReturnSuccess;
}

static inline IOReturn unicamUpdateRegBits8(II2CTransport *i2c,
                                             uint16_t reg,
                                             uint8_t mask,
                                             uint8_t val) {
    if (!i2c) {
        return kIOReturnBadArgument;
    }
    uint8_t cur = 0;
    const IOReturn ret = i2c->readReg8(reg, &cur);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    const uint8_t next = static_cast<uint8_t>((cur & ~mask) | (val & mask));
    return (next == cur) ? kIOReturnSuccess : i2c->writeReg8(reg, next);
}

#endif /* UNICAM_SENSOR_REG_HELPERS_HPP */
