#ifndef UNICAM_SOFT_ISP_HPP
#define UNICAM_SOFT_ISP_HPP

#include <IOKit/IOTypes.h>
#include <stdint.h>

#include "UniCamIPUABI.h"

class UniCamSoftISP {
public:
    static constexpr uint32_t kMaxWidth  = 1936U;
    static constexpr uint32_t kMaxHeight = 1096U;

    enum BayerOrder : uint32_t {
        kBayerSbggr10 = 0,
        kBayerSgrbg10 = 1,
        kBayerSgbrg10 = 2,
        kBayerSrggb10 = 3,
    };

    UniCamSoftISP();

    void reset3A();

    /*
     * Converts a captured RAW10 (packed) or RAW16 (unpacked) frame to packed YUY2 (YUV422 16bpp).
     * Pure integer fixed-point Q10 + LUT pipeline (no FPU/SIMD).
     * Also runs real-time 3A (AE/AWB) and populates outStats.
     */
    IOReturn processRawToYuy2(const uint8_t *srcRaw,
                              uint32_t srcWidth,
                              uint32_t srcHeight,
                              uint32_t srcStride,
                              uint32_t srcPixelFormat,
                              uint32_t bayerOrder,
                              uint32_t rotationDeg,
                              uint8_t *dstYuy2,
                              uint32_t dstWidth,
                              uint32_t dstHeight,
                              uint32_t dstCapacityBytes,
                              UniCamIspFrameStats *outStats);

    /*
     * Generates a synthetic YUY2 color-bar test frame (useful for UVC pipeline verification
     * if a sensor frame is zero-filled or during fallback testing).
     */
    void generateTestPatternYuy2(uint8_t *dstYuy2,
                                 uint32_t dstWidth,
                                 uint32_t dstHeight,
                                 uint32_t frameCounter);

    void current3AState(uint32_t *outExposureLines,
                        uint32_t *outAnalogGain,
                        uint32_t *outRGainQ10,
                        uint32_t *outBGainQ10) const;

private:
    void unpackLine(const uint8_t *srcLine,
                    uint32_t width,
                    uint32_t pixelFormat,
                    bool flipX,
                    uint16_t *dstLine) const;

    void applyDpcToMiddleLine(uint16_t *mid, uint32_t width) const;

    void update3A(uint64_t sumR,
                  uint64_t sumG,
                  uint64_t sumB,
                  uint64_t sumY,
                  uint32_t sampleCount,
                  UniCamIspFrameStats *stats);

    /* 3 rolling unpacked & BLC-corrected 10-bit lines (7,680 bytes total). */
    uint16_t lineBuf_[3][kMaxWidth];

    uint32_t frameCount_;
    uint32_t rGainQ10_;
    uint32_t gGainQ10_;
    uint32_t bGainQ10_;
    uint32_t exposureLines_;
    uint32_t analogGain_;
};

#endif /* UNICAM_SOFT_ISP_HPP */
