#ifndef UNICAM_VOODOO_I2C_DEVICE_NUB_HPP
#define UNICAM_VOODOO_I2C_DEVICE_NUB_HPP

#include <IOKit/IOService.h>

/*
 * ABI declaration matching VoodooI2C.kext (com.alexandred.VoodooI2C 2.x).
 * Do NOT subclass VoodooI2CDeviceNub; UniCamCoreService attaches as a client
 * via OSDynamicCast(VoodooI2CDeviceNub, provider) and invokes these exported
 * non-virtual methods safely.
 */
class VoodooI2CDeviceNub : public IOService {
    OSDeclareDefaultStructors(VoodooI2CDeviceNub)

public:
    IOReturn readI2C(UInt8 *values, UInt16 length);
    IOReturn writeI2C(UInt8 *values, UInt16 length);
    IOReturn writeReadI2C(UInt8 *write_buffer, UInt16 write_length, UInt8 *read_buffer, UInt16 read_length);
};

#endif /* UNICAM_VOODOO_I2C_DEVICE_NUB_HPP */
