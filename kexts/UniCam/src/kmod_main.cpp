#include <mach/mach_types.h>

extern "C" {
kern_return_t unicam_start(kmod_info_t *ki, void *d);
kern_return_t unicam_stop(kmod_info_t *ki, void *d);

kern_return_t unicam_start(kmod_info_t *ki, void *d) {
    (void)ki;
    (void)d;
    return KERN_SUCCESS;
}

kern_return_t unicam_stop(kmod_info_t *ki, void *d) {
    (void)ki;
    (void)d;
    return KERN_SUCCESS;
}
}

KMOD_EXPLICIT_DECL(com.unicam.UniCam, "1.0.0", unicam_start, unicam_stop)
