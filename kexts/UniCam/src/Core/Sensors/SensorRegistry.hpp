#ifndef UNICAM_SENSOR_REGISTRY_HPP
#define UNICAM_SENSOR_REGISTRY_HPP

#include "ISensorDriver.hpp"

class SensorRegistry {
public:
    static ISensorDriver *createForCompat(const char *compat, const char *hid);
    static void destroy(ISensorDriver *driver);
};

#endif /* UNICAM_SENSOR_REGISTRY_HPP */
