#include "UniCamSoftISP.hpp"

#include <string.h>

namespace {

constexpr int32_t kBlackLevel10Bit  = 64;
constexpr int32_t kWhiteLevel10Bit  = 1023;
/* (1023 << 10) / (1023 - 64) ~= 1092.4 -> 1092 in Q10 */
constexpr int32_t kBlcScaleQ10      = 1092;
constexpr int32_t kDpcThreshold10   = 160;

/* D65 Color Correction Matrix in Q10 fixed-point (from profiles/sensors/ov01a10.json) */
constexpr int32_t kCcmQ10[3][3] = {
    { 1659, -492, -143 }, /* [ 1.62, -0.48, -0.14 ] */
    { -225, 1444, -195 }, /* [-0.22,  1.41, -0.19 ] */
    {  -41, -532, 1597 }, /* [-0.04, -0.52,  1.56 ] */
};

constexpr uint32_t kExposureMin     = 16U;
constexpr uint32_t kExposureMax     = 1784U;
constexpr uint32_t kAnalogGainMin   = 256U;
constexpr uint32_t kAnalogGainMax   = 4096U;
constexpr uint32_t kTargetLuma8     = 108U; /* ~0.42 * 255 */

/*
 * Static 1024-entry sRGB gamma LUT (10-bit linear [0..1023] -> 8-bit sRGB [0..255]).
 * Built deterministically via piecewise-linear sRGB curve fit in pure integer math at
 * compile time / static init (or initialized once lazily without floats).
 */
struct SrgbLutHolder {
    uint8_t lut[1024];
    bool initialized;
};

SrgbLutHolder gSrgbLut = { {}, false };

/* Integer cube-root helper used once to populate a smooth ~gamma 1/2.2 curve. */
uint32_t isqrtU32(uint32_t x) {
    uint32_t res = 0U;
    uint32_t bit = 1U << 30;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

void ensureSrgbLutInitialized() {
    if (gSrgbLut.initialized) {
        return;
    }
    /*
     * Approximate x^(1/2.2) mapped 0..1023 -> 0..255 purely in integer arithmetic:
     * x^0.4545 is tightly approximated by blend of sqrt(x) (x^0.5) and linear x^0.25/x.
     * For sRGB [0..1023]:
     * - Toe region x <= 3: y = x * 32 / 10 (linear 12.92x scaled)
     * - Main region x > 3: s = isqrt(x * 1024) in [0..1023], y = (278 * s + 10 * 1023 - 20480) >> 10
     * Clamped cleanly to [0..255], monotonic across all 1024 entries.
     */
    for (uint32_t i = 0U; i < 1024U; ++i) {
        uint32_t y;
        if (i == 0U) {
            y = 0U;
        } else if (i <= 3U) {
            y = (i * 10U + 1U) / 3U;
        } else {
            /* Compute s ~= x^0.5 and q ~= x^0.25 scaled to 1023, blend ~gamma 2.2 */
            const uint32_t xScaled = (i << 12) / 1023U; /* 0..4096 -> sqrt 0..64 */
            const uint32_t sq = isqrtU32(xScaled * 1024U); /* 0..2048 (11-bit for x^0.5) */
            const uint32_t q4 = isqrtU32(sq * 2048U);      /* 0..2048 (x^0.25) */
            /* x^0.455 ~ 0.82 * x^0.5 + 0.18 * x^0.25 */
            const uint32_t norm11 = (840U * sq + 184U * q4) >> 10;
            y = (norm11 * 255U + 1024U) >> 11;
            if (y > 255U) y = 255U;
        }
        gSrgbLut.lut[i] = static_cast<uint8_t>(y);
    }
    gSrgbLut.lut[1023] = 255U;
    gSrgbLut.initialized = true;
}

static inline int32_t clampI32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t clampByte(int32_t v) {
    if (v < 0) return 0U;
    if (v > 255) return 255U;
    return static_cast<uint8_t>(v);
}

static inline void rgbToYuvBt601(uint8_t r, uint8_t g, uint8_t b,
                                  uint8_t *outY, int32_t *outU, int32_t *outV) {
    const int32_t ri = r;
    const int32_t gi = g;
    const int32_t bi = b;
    const int32_t y = ((66 * ri + 129 * gi + 25 * bi + 128) >> 8) + 16;
    const int32_t u = ((-38 * ri - 74 * gi + 112 * bi + 128) >> 8) + 128;
    const int32_t v = ((112 * ri - 94 * gi - 18 * bi + 128) >> 8) + 128;
    *outY = clampByte(y);
    *outU = clampI32(u, 0, 255);
    *outV = clampI32(v, 0, 255);
}

/* Returns color channel index (0=R, 1=Gr, 2=Gb, 3=B) for (x, y). */
static inline uint32_t bayerPhaseAt(uint32_t x, uint32_t y, uint32_t order) {
    const uint32_t oddX = x & 1U;
    const uint32_t oddY = y & 1U;
    const uint32_t cell = (oddY << 1) | oddX; /* 0:(0,0), 1:(1,0), 2:(0,1), 3:(1,1) */
    switch (order) {
        case UniCamSoftISP::kBayerSgrbg10: {
            /* (0,0)=G, (1,0)=R, (0,1)=B, (1,1)=G */
            static constexpr uint32_t map[4] = { 1U, 0U, 3U, 2U };
            return map[cell];
        }
        case UniCamSoftISP::kBayerSgbrg10: {
            /* (0,0)=G, (1,0)=B, (0,1)=R, (1,1)=G */
            static constexpr uint32_t map[4] = { 2U, 3U, 0U, 1U };
            return map[cell];
        }
        case UniCamSoftISP::kBayerSrggb10: {
            /* (0,0)=R, (1,0)=G, (0,1)=G, (1,1)=B */
            static constexpr uint32_t map[4] = { 0U, 1U, 2U, 3U };
            return map[cell];
        }
        case UniCamSoftISP::kBayerSbggr10:
        default: {
            /* (0,0)=B, (1,0)=G, (0,1)=G, (1,1)=R */
            static constexpr uint32_t map[4] = { 3U, 2U, 1U, 0U };
            return map[cell];
        }
    }
}

} // namespace

UniCamSoftISP::UniCamSoftISP() {
    ensureSrgbLutInitialized();
    memset(lineBuf_, 0, sizeof(lineBuf_));
    reset3A();
}

void UniCamSoftISP::reset3A() {
    frameCount_ = 0U;
    /* Default D65 gains from ov01a10.json (r=1.68, g=1.0, b=1.54 -> Q10) */
    rGainQ10_ = 1720U;
    gGainQ10_ = 1024U;
    bGainQ10_ = 1577U;
    exposureLines_ = 1200U;
    analogGain_ = kAnalogGainMin;
}

void UniCamSoftISP::current3AState(uint32_t *outExposureLines,
                                   uint32_t *outAnalogGain,
                                   uint32_t *outRGainQ10,
                                   uint32_t *outBGainQ10) const {
    if (outExposureLines) *outExposureLines = exposureLines_;
    if (outAnalogGain) *outAnalogGain = analogGain_;
    if (outRGainQ10) *outRGainQ10 = rGainQ10_;
    if (outBGainQ10) *outBGainQ10 = bGainQ10_;
}

void UniCamSoftISP::unpackLine(const uint8_t *srcLine,
                               uint32_t width,
                               uint32_t pixelFormat,
                               bool flipX,
                               uint16_t *dstLine) const {
    if (pixelFormat == kUniCamIpuFormatRaw16Unpacked) {
        const uint16_t *src16 = reinterpret_cast<const uint16_t *>(srcLine);
        for (uint32_t x = 0U; x < width; ++x) {
            const uint32_t sx = flipX ? (width - 1U - x) : x;
            int32_t v = static_cast<int32_t>(src16[sx] & 0x03FFU);
            v = ((v - kBlackLevel10Bit) * kBlcScaleQ10) >> 10;
            dstLine[x] = static_cast<uint16_t>(clampI32(v, 0, kWhiteLevel10Bit));
        }
        return;
    }

    /* MIPI CSI-2 packed RAW10: 4 pixels encoded across 5 bytes. */
    for (uint32_t x = 0U; x + 3U < width; x += 4U) {
        const uint32_t bOff = (x >> 2) * 5U;
        const uint8_t b0 = srcLine[bOff + 0U];
        const uint8_t b1 = srcLine[bOff + 1U];
        const uint8_t b2 = srcLine[bOff + 2U];
        const uint8_t b3 = srcLine[bOff + 3U];
        const uint8_t lsbs = srcLine[bOff + 4U];

        const uint16_t raw[4] = {
            static_cast<uint16_t>((static_cast<uint16_t>(b0) << 2) | (lsbs & 0x03U)),
            static_cast<uint16_t>((static_cast<uint16_t>(b1) << 2) | ((lsbs >> 2) & 0x03U)),
            static_cast<uint16_t>((static_cast<uint16_t>(b2) << 2) | ((lsbs >> 4) & 0x03U)),
            static_cast<uint16_t>((static_cast<uint16_t>(b3) << 2) | ((lsbs >> 6) & 0x03U)),
        };

        for (uint32_t k = 0U; k < 4U; ++k) {
            int32_t v = static_cast<int32_t>(raw[k]);
            v = ((v - kBlackLevel10Bit) * kBlcScaleQ10) >> 10;
            const uint16_t corr = static_cast<uint16_t>(clampI32(v, 0, kWhiteLevel10Bit));
            const uint32_t dx = flipX ? (width - 1U - (x + k)) : (x + k);
            dstLine[dx] = corr;
        }
    }
}

void UniCamSoftISP::applyDpcToMiddleLine(uint16_t *mid, uint32_t width) const {
    if (width < 5U) {
        return;
    }
    for (uint32_t x = 2U; x + 2U < width; ++x) {
        const int32_t left = mid[x - 2U];
        const int32_t right = mid[x + 2U];
        const int32_t c = mid[x];
        const int32_t avg = (left + right) >> 1;
        const int32_t hi = (left > right) ? left : right;
        const int32_t lo = (left < right) ? left : right;
        if ((c - hi) > kDpcThreshold10 || (lo - c) > kDpcThreshold10) {
            mid[x] = static_cast<uint16_t>(avg);
        }
    }
}

void UniCamSoftISP::update3A(uint64_t sumR,
                             uint64_t sumG,
                             uint64_t sumB,
                             uint64_t sumY,
                             uint32_t sampleCount,
                             UniCamIspFrameStats *stats) {
    ++frameCount_;
    if (sampleCount == 0U) {
        return;
    }

    const uint32_t avgR = static_cast<uint32_t>(sumR / sampleCount);
    const uint32_t avgG = static_cast<uint32_t>(sumG / sampleCount);
    const uint32_t avgB = static_cast<uint32_t>(sumB / sampleCount);
    const uint32_t avgY = static_cast<uint32_t>(sumY / sampleCount);

    stats->avgLuma = avgY;
    stats->avgR = avgR;
    stats->avgG = avgG;
    stats->avgB = avgB;

    /* Run closed-loop 3A adjustment every 3 frames for stability. */
    if ((frameCount_ % 3U) == 0U) {
        if (avgR > 4U && avgG > 4U && avgB > 4U) {
            const uint32_t targetRGain = clampU32((avgG * 1024U) / avgR, 768U, 2560U);
            const uint32_t targetBGain = clampU32((avgG * 1024U) / avgB, 768U, 2560U);
            rGainQ10_ = (rGainQ10_ * 3U + targetRGain) >> 2;
            bGainQ10_ = (bGainQ10_ * 3U + targetBGain) >> 2;
        }

        const int32_t err = static_cast<int32_t>(kTargetLuma8) - static_cast<int32_t>(avgY);
        if (err > 8 || err < -8) {
            const uint32_t prevExp = exposureLines_;
            const uint32_t prevGain = analogGain_;
            const uint32_t denom = (avgY > 8U) ? avgY : 8U;
            /* Total exposure-gain product scaled toward target luma with 0.25 dampening. */
            const uint64_t curTotal = static_cast<uint64_t>(exposureLines_) * analogGain_;
            uint64_t desiredTotal = (curTotal * kTargetLuma8) / denom;
            desiredTotal = (curTotal * 3ULL + desiredTotal) >> 2;

            const uint64_t maxSingleExp = static_cast<uint64_t>(kExposureMax) * kAnalogGainMin;
            if (desiredTotal <= maxSingleExp) {
                analogGain_ = kAnalogGainMin;
                exposureLines_ = clampU32(
                    static_cast<uint32_t>(desiredTotal / kAnalogGainMin),
                    kExposureMin, kExposureMax);
            } else {
                exposureLines_ = kExposureMax;
                analogGain_ = clampU32(
                    static_cast<uint32_t>(desiredTotal / kExposureMax),
                    kAnalogGainMin, kAnalogGainMax);
            }
            if (exposureLines_ != prevExp || analogGain_ != prevGain) {
                stats->aeUpdated = 1U;
            }
        }
    }

    stats->rGainQ10 = rGainQ10_;
    stats->gGainQ10 = gGainQ10_;
    stats->bGainQ10 = bGainQ10_;
    stats->exposureLines = exposureLines_;
    stats->analogGain = analogGain_;
}

IOReturn UniCamSoftISP::processRawToYuy2(const uint8_t *srcRaw,
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
                                         UniCamIspFrameStats *outStats) {
    if (!srcRaw || !dstYuy2 || srcWidth == 0U || srcHeight == 0U ||
        srcWidth > kMaxWidth || srcHeight > kMaxHeight ||
        dstWidth == 0U || dstHeight == 0U || (dstWidth & 1U) != 0U) {
        return kIOReturnBadArgument;
    }

    const uint32_t neededBytes = dstWidth * dstHeight * 2U;
    if (dstCapacityBytes < neededBytes) {
        return kIOReturnNoSpace;
    }

    ensureSrgbLutInitialized();

    UniCamIspFrameStats localStats {};
    const bool flip = (rotationDeg == 180U);
    uint32_t effectiveBayer = bayerOrder;
    if (flip) {
        /* 180-degree spatial rotation reverses row & column parity -> flips Bayer order. */
        static constexpr uint32_t kFlipBayer[4] = {
            kBayerSrggb10, kBayerSgbrg10, kBayerSgrbg10, kBayerSbggr10
        };
        effectiveBayer = kFlipBayer[bayerOrder & 3U];
    }

    uint32_t roiYStart = 0U;
    const bool directRows = (srcWidth == dstWidth && srcHeight >= dstHeight);
    if (directRows) {
        roiYStart = ((srcHeight - dstHeight) >> 1) & ~1U; /* even-aligned vertical center crop */
    }

    uint64_t sumR = 0ULL;
    uint64_t sumG = 0ULL;
    uint64_t sumB = 0ULL;
    uint64_t sumY = 0ULL;
    uint32_t sampleCount = 0U;

    auto readSrcRow = [&](uint32_t sy, uint32_t slot) {
        const uint32_t actualY = flip ? (srcHeight - 1U - sy) : sy;
        unpackLine(srcRaw + actualY * srcStride, srcWidth, srcPixelFormat, flip, lineBuf_[slot]);
    };

    const uint32_t rGain = rGainQ10_;
    const uint32_t bGain = bGainQ10_;

    auto processPixelPair = [&](uint32_t sx0, uint32_t sx1, uint32_t sy,
                                const uint16_t *prev, const uint16_t *mid, const uint16_t *next,
                                uint8_t *outQuad) {
        uint8_t rgbOut[2][3];
        const uint32_t xs[2] = { sx0, sx1 };

        for (uint32_t k = 0U; k < 2U; ++k) {
            const uint32_t x = xs[k];
            const uint32_t xm = (x > 0U) ? (x - 1U) : 1U;
            const uint32_t xp = (x + 1U < srcWidth) ? (x + 1U) : (srcWidth - 2U);
            const uint32_t phase = bayerPhaseAt(x, sy, effectiveBayer);

            int32_t r10 = 0;
            int32_t g10 = 0;
            int32_t b10 = 0;

            if (phase == 0U) {
                /* R site */
                r10 = mid[x];
                g10 = (prev[x] + next[x] + mid[xm] + mid[xp]) >> 2;
                b10 = (prev[xm] + prev[xp] + next[xm] + next[xp]) >> 2;
            } else if (phase == 3U) {
                /* B site */
                r10 = (prev[xm] + prev[xp] + next[xm] + next[xp]) >> 2;
                g10 = (prev[x] + next[x] + mid[xm] + mid[xp]) >> 2;
                b10 = mid[x];
            } else if (phase == 1U) {
                /* G on R/G row */
                r10 = (mid[xm] + mid[xp]) >> 1;
                g10 = mid[x];
                b10 = (prev[x] + next[x]) >> 1;
            } else {
                /* G on B/G row */
                r10 = (prev[x] + next[x]) >> 1;
                g10 = mid[x];
                b10 = (mid[xm] + mid[xp]) >> 1;
            }

            /* Apply Q10 AWB white balance gains */
            const int32_t rWb = clampI32((r10 * static_cast<int32_t>(rGain)) >> 10, 0, kWhiteLevel10Bit);
            const int32_t gWb = g10;
            const int32_t bWb = clampI32((b10 * static_cast<int32_t>(bGain)) >> 10, 0, kWhiteLevel10Bit);

            /* Apply 3x3 CCM in Q10 fixed-point */
            const int32_t rCcm = clampI32((kCcmQ10[0][0] * rWb + kCcmQ10[0][1] * gWb + kCcmQ10[0][2] * bWb) >> 10, 0, kWhiteLevel10Bit);
            const int32_t gCcm = clampI32((kCcmQ10[1][0] * rWb + kCcmQ10[1][1] * gWb + kCcmQ10[1][2] * bWb) >> 10, 0, kWhiteLevel10Bit);
            const int32_t bCcm = clampI32((kCcmQ10[2][0] * rWb + kCcmQ10[2][1] * gWb + kCcmQ10[2][2] * bWb) >> 10, 0, kWhiteLevel10Bit);

            const uint8_t r8 = gSrgbLut.lut[rCcm];
            const uint8_t g8 = gSrgbLut.lut[gCcm];
            const uint8_t b8 = gSrgbLut.lut[bCcm];
            rgbOut[k][0] = r8;
            rgbOut[k][1] = g8;
            rgbOut[k][2] = b8;

            if (k == 0U && ((x & 15U) == 0U) && ((sy & 15U) == 0U)) {
                sumR += static_cast<uint64_t>(rWb >> 2);
                sumG += static_cast<uint64_t>(gWb >> 2);
                sumB += static_cast<uint64_t>(bWb >> 2);
                sumY += static_cast<uint64_t>((77U * r8 + 150U * g8 + 29U * b8) >> 8);
                ++sampleCount;
            }
        }

        uint8_t y0 = 0U, y1 = 0U;
        int32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        rgbToYuvBt601(rgbOut[0][0], rgbOut[0][1], rgbOut[0][2], &y0, &u0, &v0);
        rgbToYuvBt601(rgbOut[1][0], rgbOut[1][1], rgbOut[1][2], &y1, &u1, &v1);

        outQuad[0] = y0;
        outQuad[1] = static_cast<uint8_t>((u0 + u1) >> 1);
        outQuad[2] = y1;
        outQuad[3] = static_cast<uint8_t>((v0 + v1) >> 1);
    };

    if (directRows) {
        const uint32_t outLines = (dstHeight <= srcHeight) ? dstHeight : srcHeight;
        const uint32_t startY = roiYStart;
        const uint32_t prevInit = (startY > 0U) ? (startY - 1U) : 1U;
        readSrcRow(prevInit, 0U);
        readSrcRow(startY, 1U);

        for (uint32_t dy = 0U; dy < outLines; ++dy) {
            const uint32_t sy = startY + dy;
            const uint32_t nextY = (sy + 1U < srcHeight) ? (sy + 1U) : (srcHeight - 2U);
            const uint32_t sPrev = dy % 3U;
            const uint32_t sMid  = (dy + 1U) % 3U;
            const uint32_t sNext = (dy + 2U) % 3U;
            readSrcRow(nextY, sNext);
            applyDpcToMiddleLine(lineBuf_[sMid], srcWidth);

            uint8_t *dstRow = dstYuy2 + dy * (dstWidth * 2U);
            const uint32_t maxPairs = dstWidth >> 1;
            for (uint32_t p = 0U; p < maxPairs; ++p) {
                const uint32_t sx = p << 1;
                processPixelPair(sx, sx + 1U, sy,
                                 lineBuf_[sPrev], lineBuf_[sMid], lineBuf_[sNext],
                                 dstRow + (p << 2));
            }
        }
    } else {
        /* Aspect-matched center ROI & arbitrary integer scaling (e.g. 1932x1092 / 1296x972 -> 1280x720 or 640x480). */
        uint32_t roiW = srcWidth & ~1U;
        uint32_t roiH = srcHeight & ~1U;
        if (static_cast<uint64_t>(srcWidth) * dstHeight > static_cast<uint64_t>(dstWidth) * srcHeight) {
            roiW = static_cast<uint32_t>((static_cast<uint64_t>(srcHeight) * dstWidth) / dstHeight) & ~1U;
        } else {
            roiH = static_cast<uint32_t>((static_cast<uint64_t>(srcWidth) * dstHeight) / dstWidth) & ~1U;
        }
        roiW = clampU32(roiW, 4U, srcWidth);
        roiH = clampU32(roiH, 4U, srcHeight);
        const uint32_t roiX = ((srcWidth - roiW) >> 1) & ~1U;
        const uint32_t roiY = ((srcHeight - roiH) >> 1) & ~1U;

        int32_t cachedSy = -1;
        for (uint32_t dy = 0U; dy < dstHeight; ++dy) {
            const uint32_t sy = clampU32(roiY + ((dy * roiH) / dstHeight), 0U, srcHeight - 1U);
            if (static_cast<int32_t>(sy) != cachedSy) {
                const uint32_t prevY = (sy > 0U) ? (sy - 1U) : 1U;
                const uint32_t nextY = (sy + 1U < srcHeight) ? (sy + 1U) : (srcHeight - 2U);
                readSrcRow(prevY, 0U);
                readSrcRow(sy, 1U);
                readSrcRow(nextY, 2U);
                applyDpcToMiddleLine(lineBuf_[1], srcWidth);
                cachedSy = static_cast<int32_t>(sy);
            }

            uint8_t *dstRow = dstYuy2 + dy * (dstWidth * 2U);
            const uint32_t maxPairs = dstWidth >> 1;
            for (uint32_t p = 0U; p < maxPairs; ++p) {
                const uint32_t dx0 = p << 1;
                const uint32_t dx1 = dx0 + 1U;
                const uint32_t sx0 = clampU32(roiX + ((dx0 * roiW) / dstWidth), 0U, srcWidth - 1U);
                const uint32_t sx1 = clampU32(roiX + ((dx1 * roiW) / dstWidth), 0U, srcWidth - 1U);
                processPixelPair(sx0, sx1, sy,
                                 lineBuf_[0], lineBuf_[1], lineBuf_[2],
                                 dstRow + (p << 2));
            }
        }
    }

    update3A(sumR, sumG, sumB, sumY, sampleCount, &localStats);
    localStats.payloadBytes = neededBytes;
    if (outStats) {
        *outStats = localStats;
    }
    return kIOReturnSuccess;
}

void UniCamSoftISP::generateTestPatternYuy2(uint8_t *dstYuy2,
                                            uint32_t dstWidth,
                                            uint32_t dstHeight,
                                            uint32_t frameCounter) {
    if (!dstYuy2 || dstWidth == 0U || dstHeight == 0U || (dstWidth & 1U) != 0U) {
        return;
    }
    static constexpr uint8_t kBars[8][3] = {
        { 235, 128, 128 }, /* White */
        { 210,  16, 146 }, /* Yellow */
        { 170, 166,  16 }, /* Cyan */
        { 145,  54,  34 }, /* Green */
        { 106, 202, 222 }, /* Magenta */
        {  81,  90, 240 }, /* Red */
        {  41, 240, 110 }, /* Blue */
        {  16, 128, 128 }, /* Black */
    };
    const uint32_t scroll = (frameCounter * 4U) % dstWidth;
    for (uint32_t y = 0U; y < dstHeight; ++y) {
        uint8_t *row = dstYuy2 + y * dstWidth * 2U;
        const bool inMovingBar = (y >= (dstHeight - 24U)) && (y < (dstHeight - 8U));
        for (uint32_t x = 0U; x < dstWidth; x += 2U) {
            const uint32_t barIdx = (x * 8U) / dstWidth;
            uint8_t luma = kBars[barIdx & 7U][0];
            uint8_t u = kBars[barIdx & 7U][1];
            uint8_t v = kBars[barIdx & 7U][2];
            if (inMovingBar && x >= scroll && x < (scroll + 64U)) {
                luma = 235U;
                u = 128U;
                v = 128U;
            }
            const uint32_t off = x * 2U;
            row[off + 0U] = luma;
            row[off + 1U] = u;
            row[off + 2U] = luma;
            row[off + 3U] = v;
        }
    }
}
