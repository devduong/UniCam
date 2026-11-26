#ifndef UNICAM_II2C_TRANSPORT_HPP
#define UNICAM_II2C_TRANSPORT_HPP

#include <IOKit/IOReturn.h>
#include <stdint.h>

class II2CTransport {
public:
    virtual ~II2CTransport() = default;

    virtual IOReturn writeBytes(const uint8_t *buf, uint16_t len) = 0;
    virtual IOReturn writeReadBytes(const uint8_t *wbuf, uint16_t wlen,
                                    uint8_t *rbuf, uint16_t rlen) = 0;

    virtual IOReturn readReg8(uint16_t reg, uint8_t *outVal) = 0;
    virtual IOReturn readReg16(uint16_t reg, uint16_t *outVal) = 0;
    virtual IOReturn readReg24(uint16_t reg, uint32_t *outVal, uint8_t rawBytes[3]) = 0;

    virtual IOReturn writeReg8(uint16_t reg, uint8_t val) = 0;
    virtual IOReturn writeReg16(uint16_t reg, uint16_t val) = 0;
    virtual IOReturn writeReg24(uint16_t reg, uint32_t val) = 0;

    virtual uint32_t txCount() const = 0;
    virtual uint32_t rxCount() const = 0;
    virtual uint32_t errCount() const = 0;
};

#endif /* UNICAM_II2C_TRANSPORT_HPP */
