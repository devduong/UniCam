#include "SensorRegistry.hpp"
#include "OV01A10Driver.hpp"
#include "OV02C10Driver.hpp"
#include "OV2740Driver.hpp"
#include "OV5675Driver.hpp"
#include "OV5693Driver.hpp"

#include <string.h>

namespace {

bool equalsIgnoreCase(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) {
            return false;
        }
    }
    return (*a == '\0') && (*b == '\0');
}

} // namespace

ISensorDriver *SensorRegistry::createForCompat(const char *compat, const char *hid) {
    if (equalsIgnoreCase(compat, "ovti,ov01a1b") || equalsIgnoreCase(hid, "OVTI01AB")) {
        return new OV01A10Driver(true);
    }
    if (equalsIgnoreCase(compat, "ovti,ov01a10") || equalsIgnoreCase(hid, "OVTI01A0")) {
        return new OV01A10Driver(false);
    }
    if (equalsIgnoreCase(compat, "ovti,ov2740") ||
        equalsIgnoreCase(hid, "INT3474") ||
        equalsIgnoreCase(hid, "OVTI2740")) {
        return new OV2740Driver();
    }
    if (equalsIgnoreCase(compat, "ovti,ov02c10") || equalsIgnoreCase(hid, "OVTI02C1")) {
        return new OV02C10Driver();
    }
    if (equalsIgnoreCase(compat, "ovti,ov5693") ||
        equalsIgnoreCase(hid, "INT33BE") ||
        equalsIgnoreCase(hid, "OVTI5693")) {
        return new OV5693Driver();
    }
    if (equalsIgnoreCase(compat, "ovti,ov5675") ||
        equalsIgnoreCase(hid, "OVTI5675") ||
        equalsIgnoreCase(hid, "INT3476")) {
        return new OV5675Driver();
    }
    return nullptr;
}

void SensorRegistry::destroy(ISensorDriver *driver) {
    delete driver;
}
