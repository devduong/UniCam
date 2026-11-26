#ifndef UNICAM_IPU4P_EMBEDDED_FW_HPP
#define UNICAM_IPU4P_EMBEDDED_FW_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <stdint.h>

namespace ipu4p {

extern const uint32_t gEmbeddedIpu4pFwRawSize;
extern const uint32_t gEmbeddedIpu4pFwZlibSize;
extern const uint8_t gEmbeddedIpu4pFwZlib[];

bool decompressEmbeddedIpu4pFw(IOBufferMemoryDescriptor **outDesc, uint32_t *outSize);

} // namespace ipu4p

#endif /* UNICAM_IPU4P_EMBEDDED_FW_HPP */
