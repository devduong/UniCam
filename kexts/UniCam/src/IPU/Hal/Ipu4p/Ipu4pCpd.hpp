#ifndef UNICAM_IPU4P_CPD_HPP
#define UNICAM_IPU4P_CPD_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "Ipu4pFwAbi.hpp"
#include "Ipu4pMmu.hpp"
#include "IpuDmaBuffer.hpp"

namespace ipu4p {

struct CpdParseInfo {
    uint32_t fwPkgDate;
    uint32_t componentCount;
    uint32_t manifestOffset;
    uint32_t manifestSize;
    uint32_t metadataOffset;
    uint32_t metadataSize;
    uint32_t moduleDataOffset;
    uint32_t moduleDataSize;
};

class Ipu4pCpd {
public:
    static bool validateCpdFile(const uint8_t *cpd, uint32_t size, CpdParseInfo *outInfo);

    static bool createPkgDir(IOPCIDevice *pciDevice,
                             uint32_t dmaMode,
                             Ipu4pMmu *mmu,
                             const uint8_t *cpd,
                             uint32_t fwBaseIova,
                             const CpdParseInfo &info,
                             IpuDmaBuffer *outPkgDirBuf,
                             uint32_t *outPkgDirIova);

    static void configureSpc(volatile uint8_t *subsysBase,
                             uint32_t spcOffset,
                             uint32_t dmemOffset,
                             bool secureMode,
                             uint32_t pkgDirIdx,
                             const uint8_t *cpd,
                             uint32_t cpdSize,
                             uint32_t fwBaseIova,
                             uint32_t pkgDirIova,
                             const IpuDmaBuffer *pkgDirBuf);
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_CPD_HPP */
