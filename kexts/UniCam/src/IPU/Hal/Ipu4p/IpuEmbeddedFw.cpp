#include "IpuEmbeddedFw.hpp"

#include <IOKit/IOLib.h>
#include <libkern/zlib.h>
#include <string.h>

extern "C" {
extern task_t kernel_task;
}

namespace ipu4p {

namespace {

constexpr size_t kZlibScratchBytes = 128U * 1024U;

struct ZScratchArena {
    uint8_t *base;
    size_t capacity;
    size_t used;
};

void *zArenaAlloc(void *opaque, uInt items, uInt size) {
    ZScratchArena *arena = static_cast<ZScratchArena *>(opaque);
    if (!arena || !arena->base || items == 0U || size == 0U) {
        return nullptr;
    }
    const size_t total = static_cast<size_t>(items) * static_cast<size_t>(size);
    const size_t alignedUsed = (arena->used + 15U) & ~static_cast<size_t>(15U);
    if (alignedUsed + total > arena->capacity) {
        return nullptr;
    }
    void *ptr = arena->base + alignedUsed;
    arena->used = alignedUsed + total;
    return ptr;
}

void zArenaFree(void *opaque, void *address) {
    (void)opaque;
    (void)address;
}

} // namespace

bool decompressEmbeddedIpu4pFw(IOBufferMemoryDescriptor **outDesc, uint32_t *outSize) {
    if (!outDesc || !outSize || gEmbeddedIpu4pFwRawSize == 0U || gEmbeddedIpu4pFwZlibSize == 0U) {
        return false;
    }
    *outDesc = nullptr;
    *outSize = 0U;

    IOBufferMemoryDescriptor *rawDesc = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, static_cast<vm_size_t>(gEmbeddedIpu4pFwRawSize));
    if (!rawDesc) {
        IOLog("UniCamIPU: Failed to allocate %u bytes for embedded FW inflation\n",
              gEmbeddedIpu4pFwRawSize);
        return false;
    }

    IOBufferMemoryDescriptor *scratchDesc = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, kZlibScratchBytes);
    if (!scratchDesc) {
        rawDesc->release();
        return false;
    }

    uint8_t *rawPtr = static_cast<uint8_t *>(rawDesc->getBytesNoCopy());
    uint8_t *scratchPtr = static_cast<uint8_t *>(scratchDesc->getBytesNoCopy());
    if (!rawPtr || !scratchPtr) {
        scratchDesc->release();
        rawDesc->release();
        return false;
    }

    ZScratchArena arena { scratchPtr, kZlibScratchBytes, 0U };
    z_stream strm {};
    strm.zalloc = &zArenaAlloc;
    strm.zfree = &zArenaFree;
    strm.opaque = &arena;
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(gEmbeddedIpu4pFwZlib));
    strm.avail_in = static_cast<uInt>(gEmbeddedIpu4pFwZlibSize);
    strm.next_out = reinterpret_cast<Bytef *>(rawPtr);
    strm.avail_out = static_cast<uInt>(gEmbeddedIpu4pFwRawSize);

    if (inflateInit2(&strm, 15) != Z_OK) {
        IOLog("UniCamIPU: zlib inflateInit2 failed\n");
        scratchDesc->release();
        rawDesc->release();
        return false;
    }

    const int zret = inflate(&strm, Z_FINISH);
    const uLong totalOut = strm.total_out;
    inflateEnd(&strm);
    scratchDesc->release();

    if (zret != Z_STREAM_END || totalOut != gEmbeddedIpu4pFwRawSize) {
        IOLog("UniCamIPU: zlib inflate failed (ret=%d out=%lu expected=%u)\n",
              zret, totalOut, gEmbeddedIpu4pFwRawSize);
        rawDesc->release();
        return false;
    }

    *outDesc = rawDesc;
    *outSize = gEmbeddedIpu4pFwRawSize;
    return true;
}

} // namespace ipu4p
