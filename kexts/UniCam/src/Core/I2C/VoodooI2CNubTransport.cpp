#include "VoodooI2CNubTransport.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace {
constexpr uint32_t kMaxI2CRetries = 3;
} // namespace

VoodooI2CNubTransport::VoodooI2CNubTransport(VoodooI2CDeviceNub *nub)
    : nub_(nub), txCount_(0), rxCount_(0), errCount_(0) {
}

void VoodooI2CNubTransport::setNub(VoodooI2CDeviceNub *nub) {
    nub_ = nub;
}

IOReturn VoodooI2CNubTransport::writeBytes(const uint8_t *buf, uint16_t len) {
    if (!nub_ || !buf || len == 0 || len > 64) {
        return kIOReturnBadArgument;
    }

    uint8_t localBuf[64];
    memcpy(localBuf, buf, len);

    IOReturn ret = kIOReturnError;
    for (uint32_t attempt = 0; attempt < kMaxI2CRetries; ++attempt) {
        ret = nub_->writeI2C(localBuf, len);
        if (ret == kIOReturnSuccess) {
            ++txCount_;
            return kIOReturnSuccess;
        }
        ++errCount_;
        if (attempt + 1 < kMaxI2CRetries) {
            IOSleep(1);
        }
    }
    return ret;
}

IOReturn VoodooI2CNubTransport::writeReadBytes(const uint8_t *wbuf, uint16_t wlen,
                                               uint8_t *rbuf, uint16_t rlen) {
    if (!nub_ || !wbuf || wlen == 0 || wlen > 32 || !rbuf || rlen == 0 || rlen > 64) {
        return kIOReturnBadArgument;
    }

    uint8_t localWrite[32];
    memcpy(localWrite, wbuf, wlen);

    IOReturn ret = kIOReturnError;
    for (uint32_t attempt = 0; attempt < kMaxI2CRetries; ++attempt) {
        memset(rbuf, 0, rlen);
        ret = nub_->writeReadI2C(localWrite, wlen, rbuf, rlen);
        if (ret == kIOReturnSuccess) {
            ++txCount_;
            ++rxCount_;
            return kIOReturnSuccess;
        }
        ++errCount_;
        if (attempt + 1 < kMaxI2CRetries) {
            IOSleep(1);
        }
    }
    return ret;
}

IOReturn VoodooI2CNubTransport::readReg8(uint16_t reg, uint8_t *outVal) {
    if (!outVal) {
        return kIOReturnBadArgument;
    }
    const uint8_t addr[2] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU)
    };
    uint8_t val = 0;
    const IOReturn ret = writeReadBytes(addr, 2, &val, 1);
    if (ret == kIOReturnSuccess) {
        *outVal = val;
    }
    return ret;
}

IOReturn VoodooI2CNubTransport::readReg16(uint16_t reg, uint16_t *outVal) {
    if (!outVal) {
        return kIOReturnBadArgument;
    }
    const uint8_t addr[2] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU)
    };
    uint8_t buf[2] = {0, 0};
    const IOReturn ret = writeReadBytes(addr, 2, buf, 2);
    if (ret == kIOReturnSuccess) {
        *outVal = static_cast<uint16_t>((static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
    }
    return ret;
}

IOReturn VoodooI2CNubTransport::readReg24(uint16_t reg, uint32_t *outVal, uint8_t rawBytes[3]) {
    if (!outVal) {
        return kIOReturnBadArgument;
    }
    const uint8_t addr[2] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU)
    };
    uint8_t buf[3] = {0, 0, 0};
    const IOReturn ret = writeReadBytes(addr, 2, buf, 3);
    if (ret == kIOReturnSuccess) {
        if (rawBytes) {
            rawBytes[0] = buf[0];
            rawBytes[1] = buf[1];
            rawBytes[2] = buf[2];
        }
        *outVal = (static_cast<uint32_t>(buf[0]) << 16) |
                  (static_cast<uint32_t>(buf[1]) << 8) |
                  static_cast<uint32_t>(buf[2]);
    }
    return ret;
}

IOReturn VoodooI2CNubTransport::writeReg8(uint16_t reg, uint8_t val) {
    const uint8_t buf[3] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU),
        val
    };
    return writeBytes(buf, 3);
}

IOReturn VoodooI2CNubTransport::writeReg16(uint16_t reg, uint16_t val) {
    const uint8_t buf[4] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU),
        static_cast<uint8_t>((val >> 8) & 0xFFU),
        static_cast<uint8_t>(val & 0xFFU)
    };
    return writeBytes(buf, 4);
}

IOReturn VoodooI2CNubTransport::writeReg24(uint16_t reg, uint32_t val) {
    const uint8_t buf[5] = {
        static_cast<uint8_t>((reg >> 8) & 0xFFU),
        static_cast<uint8_t>(reg & 0xFFU),
        static_cast<uint8_t>((val >> 16) & 0xFFU),
        static_cast<uint8_t>((val >> 8) & 0xFFU),
        static_cast<uint8_t>(val & 0xFFU)
    };
    return writeBytes(buf, 5);
}
