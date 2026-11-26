#ifndef UNICAM_KERNEL_GATE_H
#define UNICAM_KERNEL_GATE_H

#include <IOKit/IOLib.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>

namespace unicam {

constexpr int kMinSupportedDarwinMajor = 22; // macOS 13 Ventura
constexpr int kMaxVerifiedDarwinMajor  = 25; // macOS 26 Tahoe

inline bool hasBootArg(const char *name) {
    if (name == nullptr) {
        return false;
    }
    int dummy = 0;
    return PE_parse_boot_argn(name, &dummy, sizeof(dummy));
}

inline bool isKernelSupportedForLoad(const char *tag) {
    if (hasBootArg("-unicamoff")) {
        if (tag != nullptr) {
            IOLog("%s: Disabled via -unicamoff boot-arg\n", tag);
        }
        return false;
    }
    if (version_major < kMinSupportedDarwinMajor) {
        if (tag != nullptr) {
            IOLog("%s: Unsupported Darwin kernel %d (< %d / macOS 13)\n",
                  tag, version_major, kMinSupportedDarwinMajor);
        }
        return false;
    }
    if (version_major > kMaxVerifiedDarwinMajor && !hasBootArg("-unicambeta")) {
        if (tag != nullptr) {
            IOLog("%s: Untested Darwin kernel %d (> %d); add -unicambeta boot-arg to enable testing\n",
                  tag, version_major, kMaxVerifiedDarwinMajor);
        }
        return false;
    }
    return true;
}

} // namespace unicam

#endif /* UNICAM_KERNEL_GATE_H */
