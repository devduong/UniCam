#ifndef UNICAM_VOODOO_I2C_NUB_TRANSPORT_HPP
#define UNICAM_VOODOO_I2C_NUB_TRANSPORT_HPP

#include "II2CTransport.hpp"
#include "../VoodooI2CDeviceNub.hpp"

class VoodooI2CNubTransport : public II2CTransport {
public:
    explicit VoodooI2CNubTransport(VoodooI2CDeviceNub *nub = nullptr);
    ~VoodooI2CNubTransport() override = default;

    void setNub(VoodooI2CDeviceNub *nub);
    VoodooI2CDeviceNub *nub() const { return nub_; }

    IOReturn writeBytes(const uint8_t *buf, uint16_t len) override;
    IOReturn writeReadBytes(const uint8_t *wbuf, uint16_t wlen,
                            uint8_t *rbuf, uint16_t rlen) override;

    IOReturn readReg8(uint16_t reg, uint8_t *outVal) override;
    IOReturn readReg16(uint16_t reg, uint16_t *outVal) override;
    IOReturn readReg24(uint16_t reg, uint32_t *outVal, uint8_t rawBytes[3]) override;

    IOReturn writeReg8(uint16_t reg, uint8_t val) override;
    IOReturn writeReg16(uint16_t reg, uint16_t val) override;
    IOReturn writeReg24(uint16_t reg, uint32_t val) override;

    uint32_t txCount() const override { return txCount_; }
    uint32_t rxCount() const override { return rxCount_; }
    uint32_t errCount() const override { return errCount_; }

private:
    VoodooI2CDeviceNub *nub_;
    uint32_t txCount_;
    uint32_t rxCount_;
    uint32_t errCount_;
};

#endif /* UNICAM_VOODOO_I2C_NUB_TRANSPORT_HPP */
