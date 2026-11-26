#include "Ipu4pCpd.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace ipu4p {

namespace {

constexpr uint32_t kMaxManifestSize = 2048U * 4U;
constexpr uint32_t kMaxMetadataSize = 65536U;

const CpdEnt *cpdEntryAt(const uint8_t *cpd, uint8_t idx) {
    const auto *hdr = reinterpret_cast<const CpdHdr *>(cpd);
    const auto *ents = reinterpret_cast<const CpdEnt *>(cpd + hdr->hdr_len);
    return &ents[idx];
}

bool validateCpdTable(const uint8_t *cpd, uint32_t tableAreaSize, uint32_t parentDataSize) {
    if (tableAreaSize < sizeof(CpdHdr)) return false;
    const auto *hdr = reinterpret_cast<const CpdHdr *>(cpd);
    if (tableAreaSize < hdr->hdr_len) return false;
    const uint32_t avail = tableAreaSize - hdr->hdr_len;
    if ((avail / sizeof(CpdEnt)) < hdr->ent_cnt) return false;

    const auto *ents = reinterpret_cast<const CpdEnt *>(cpd + hdr->hdr_len);
    for (uint32_t i = 0; i < hdr->ent_cnt; ++i) {
        if (parentDataSize < ents[i].offset ||
            (parentDataSize - ents[i].offset) < ents[i].len) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Ipu4pCpd::validateCpdFile(const uint8_t *cpd, uint32_t size, CpdParseInfo *outInfo) {
    if (!cpd || size < sizeof(CpdHdr) || !outInfo) {
        return false;
    }

    const auto *hdr = reinterpret_cast<const CpdHdr *>(cpd);
    if (hdr->hdr_mark != kCpdHdrMark || hdr->ent_cnt < 3U) {
        IOLog("UniCamIPU: Invalid CPD header mark=0x%08x ent_cnt=%u\n", hdr->hdr_mark, hdr->ent_cnt);
        return false;
    }
    if (!validateCpdTable(cpd, size, size)) {
        IOLog("UniCamIPU: CPD entry bounds invalid\n");
        return false;
    }

    const CpdEnt *manEnt = cpdEntryAt(cpd, 0);
    const CpdEnt *metEnt = cpdEntryAt(cpd, 1);
    const CpdEnt *modEnt = cpdEntryAt(cpd, 2);

    if (manEnt->len == 0 || manEnt->len > kMaxManifestSize) {
        IOLog("UniCamIPU: Invalid CPD manifest len=%u\n", manEnt->len);
        return false;
    }

    if (metEnt->len < sizeof(CpdMetadataExtn) || metEnt->len > kMaxMetadataSize) {
        IOLog("UniCamIPU: Invalid CPD metadata len=%u\n", metEnt->len);
        return false;
    }

    const auto *extn = reinterpret_cast<const CpdMetadataExtn *>(cpd + metEnt->offset);
    if (extn->extn_type != kCpdMetadataExtnTypeIunit ||
        extn->img_type != kCpdMetadataImgTypeMainFw) {
        IOLog("UniCamIPU: Invalid CPD metadata extn_type=%u img_type=%u\n",
              extn->extn_type, extn->img_type);
        return false;
    }

    const uint32_t cmpntBytes = metEnt->len - static_cast<uint32_t>(sizeof(CpdMetadataExtn));
    if ((cmpntBytes % sizeof(CpdMetadataCmpntIpu4p)) != 0) {
        IOLog("UniCamIPU: Invalid CPD metadata component multiple (rem=%u)\n",
              static_cast<uint32_t>(cmpntBytes % sizeof(CpdMetadataCmpntIpu4p)));
        return false;
    }

    if (modEnt->len < sizeof(CpdModuleDataHdr)) {
        return false;
    }
    const auto *modHdr = reinterpret_cast<const CpdModuleDataHdr *>(cpd + modEnt->offset);
    if (modEnt->len < modHdr->hdr_len) {
        return false;
    }

    const uint8_t *modDir = cpd + modEnt->offset + modHdr->hdr_len;
    const uint32_t modDirSize = modEnt->len - modHdr->hdr_len;
    if (!validateCpdTable(modDir, modDirSize, modEnt->len)) {
        IOLog("UniCamIPU: Invalid CPD moduledata table\n");
        return false;
    }

    const auto *modCpdHdr = reinterpret_cast<const CpdHdr *>(modDir);
    if (modCpdHdr->ent_cnt >= kMaxPkgDirEntCnt) {
        IOLog("UniCamIPU: Too many CPD modules (%u)\n", modCpdHdr->ent_cnt);
        return false;
    }

    outInfo->fwPkgDate = modHdr->fw_pkg_date;
    outInfo->componentCount = modCpdHdr->ent_cnt;
    outInfo->manifestOffset = manEnt->offset;
    outInfo->manifestSize = manEnt->len;
    outInfo->metadataOffset = metEnt->offset;
    outInfo->metadataSize = metEnt->len;
    outInfo->moduleDataOffset = modEnt->offset;
    outInfo->moduleDataSize = modEnt->len;
    return true;
}

bool Ipu4pCpd::createPkgDir(IOPCIDevice *pciDevice,
                            uint32_t dmaMode,
                            Ipu4pMmu *mmu,
                            const uint8_t *cpd,
                            uint32_t fwBaseIova,
                            const CpdParseInfo &info,
                            IpuDmaBuffer *outPkgDirBuf,
                            uint32_t *outPkgDirIova) {
    if (!pciDevice || !mmu || !cpd || !outPkgDirBuf || !outPkgDirIova) {
        return false;
    }

    const size_t totalSize = kPkgDirSize + info.manifestSize + info.metadataSize;
    if (!outPkgDirBuf->allocate(pciDevice, dmaMode, totalSize, false)) {
        return false;
    }

    uint64_t *pkgDir = static_cast<uint64_t *>(outPkgDirBuf->cpuAddr());
    memset(pkgDir, 0, totalSize);

    const uint8_t *modBase = cpd + info.moduleDataOffset;
    const auto *modHdr = reinterpret_cast<const CpdModuleDataHdr *>(modBase);
    const auto *dirHdr = reinterpret_cast<const CpdHdr *>(modBase + modHdr->hdr_len);
    const auto *dirEnt = reinterpret_cast<const CpdEnt *>(
        reinterpret_cast<const uint8_t *>(dirHdr) + dirHdr->hdr_len);

    const uint8_t *metaPtr = cpd + info.metadataOffset;
    const uint32_t cmpntCount =
        (info.metadataSize - static_cast<uint32_t>(sizeof(CpdMetadataExtn))) /
        static_cast<uint32_t>(sizeof(CpdMetadataCmpntIpu4p));
    const auto *cmpnts = reinterpret_cast<const CpdMetadataCmpntIpu4p *>(
        metaPtr + sizeof(CpdMetadataExtn));

    pkgDir[0] = kPkgDirHdrMark;
    pkgDir[1] = static_cast<uint64_t>(dirHdr->ent_cnt) + 1ULL;

    for (uint32_t i = 0; i < dirHdr->ent_cnt; ++i) {
        if (i >= cmpntCount) {
            outPkgDirBuf->release();
            return false;
        }
        const uint32_t id = cmpnts[i].hdr.id;
        const uint32_t ver = cmpnts[i].hdr.ver;
        if (id > kCpdMaxComponentId || ver > kCpdMaxComponentVer) {
            outPkgDirBuf->release();
            return false;
        }

        uint64_t *p = &pkgDir[kPkgDirEntLen * (1U + i)];
        p[0] = static_cast<uint64_t>(fwBaseIova) + info.moduleDataOffset + dirEnt[i].offset;
        p[1] = (static_cast<uint64_t>(dirEnt[i].len) & 0x00FFFFFFULL) |
               ((static_cast<uint64_t>(ver) & 0xFFFFULL) << 32) |
               ((static_cast<uint64_t>(id) & 0x7FULL) << 48);
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(pkgDir) + kPkgDirSize;
    memcpy(dst, cpd + info.manifestOffset, info.manifestSize);
    memcpy(dst + info.manifestSize, cpd + info.metadataOffset, info.metadataSize);

    outPkgDirBuf->flushCache();

    const uint32_t iova = mmu->mapBuffer(*outPkgDirBuf);
    if (iova == 0U) {
        outPkgDirBuf->release();
        return false;
    }

    *outPkgDirIova = iova;
    return true;
}

void Ipu4pCpd::configureSpc(volatile uint8_t *subsysBase,
                            uint32_t spcOffset,
                            uint32_t dmemOffset,
                            bool secureMode,
                            uint32_t pkgDirIdx,
                            const uint8_t *cpd,
                            uint32_t cpdSize,
                            uint32_t fwBaseIova,
                            uint32_t pkgDirIova,
                            const IpuDmaBuffer *pkgDirBuf) {
    if (!subsysBase) return;

    volatile uint8_t *spcBase = subsysBase + spcOffset;
    uint32_t val = mmioRead32(spcBase, kSpcRegStatusCtrl);
    val |= kSpcStatusIcacheInvalidate;
    mmioWrite32(spcBase, kSpcRegStatusCtrl, val);

    if (secureMode) {
        mmioWrite32(subsysBase, dmemOffset, kPkgDirImrOffset);
        return;
    }

    if (!cpd || !pkgDirBuf || !pkgDirBuf->isValid()) {
        return;
    }

    const uint64_t *pkgDir = static_cast<const uint64_t *>(pkgDirBuf->cpuAddr());
    const uint32_t serverFwAddr = static_cast<uint32_t>(pkgDir[(pkgDirIdx + 1U) * 2U] & 0xFFFFFFFFULL);
    if (serverFwAddr < fwBaseIova) {
        return;
    }
    const uint32_t pgOffset = serverFwAddr - fwBaseIova;
    if (pgOffset + sizeof(CellProgram) > cpdSize) {
        return;
    }

    const auto *prog = reinterpret_cast<const CellProgram *>(cpd + pgOffset);
    volatile uint8_t *cellSpc = subsysBase + prog->regs_addr;
    mmioWrite32(cellSpc, kSpcRegIcacheBase, serverFwAddr + prog->blob_offset + prog->icache_source);
    mmioWrite32(cellSpc, kSpcRegInfoSeg0IcacheMaster, kInfoRequestDestIosf);
    mmioWrite32(cellSpc, kSpcRegStartPc, prog->start[1]);
    mmioWrite32(subsysBase, dmemOffset, pkgDirIova);
}

} // namespace ipu4p
