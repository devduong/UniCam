#ifndef UNICAM_IPU_ABI_H
#define UNICAM_IPU_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNICAM_IPU_SERVICE_NAME         "UniCamIPUService"
#define UNICAM_IPU_USER_CLIENT_TYPE     0U
#define UNICAM_IPU_SCHEMA_VERSION       1U

#define UNICAM_IPU_PCI_VENDOR_INTEL     0x8086U
#define UNICAM_IPU_PCI_DEVICE_IPU4P     0x8A19U

#define UNICAM_IPU_RING_MAGIC           0x55434950U /* 'UCIP' */
#define UNICAM_IPU_RING_SLOTS           6U
#define UNICAM_IPU_RING_HEADER_BYTES    4096U
/*
 * 4.25 MiB per slot (1088 x 4 KiB pages): fits up to 1936x1096 packed RAW10
 * (~2.7 MiB) AND unpacked RAW16 (4,243,712B) plus IPU ISYS DMA overshoot guard.
 */
#define UNICAM_IPU_RING_SLOT_BYTES      0x00440000U
#define UNICAM_IPU_RING_TOTAL_BYTES     \
    (UNICAM_IPU_RING_HEADER_BYTES + (UNICAM_IPU_RING_SLOTS * UNICAM_IPU_RING_SLOT_BYTES))

enum UniCamIpuSelector {
    kUniCamIpuMethodGetInfo          = 0,
    kUniCamIpuMethodGetDiag          = 1,
    kUniCamIpuMethodBootFirmware     = 2,
    kUniCamIpuMethodShutdownFirmware = 3,
    kUniCamIpuMethodStartStream      = 4,
    kUniCamIpuMethodWaitNextFrame    = 5,
    kUniCamIpuMethodRequeueSlot      = 6,
    kUniCamIpuMethodStopStream       = 7,
    kUniCamIpuMethodProcessSlotYuy2  = 8,
    kUniCamIpuMethodCount
};

enum UniCamIpuMemoryType {
    kUniCamIpuMemRingBuffer = 0
};

enum UniCamIpuDmaMode {
    kUniCamIpuDmaAuto         = 0,
    kUniCamIpuDmaPhysical     = 1,
    kUniCamIpuDmaDeviceMapper = 2,
    kUniCamIpuDmaSystemMapper = 3
};

enum UniCamIpuPixelFormat {
    kUniCamIpuFormatRaw10Packed   = 0,
    kUniCamIpuFormatRaw16Unpacked = 1
};

enum UniCamIpuSlotState {
    kUniCamIpuSlotFree         = 0,
    kUniCamIpuSlotQueuedToFw   = 1,
    kUniCamIpuSlotReadyForUser = 2
};

enum UniCamIpuBootStep {
    kUniCamIpuBootStepOk             = 0,
    kUniCamIpuBootStepBadArg         = 1,
    kUniCamIpuBootStepInvalidCpd     = 2,
    kUniCamIpuBootStepMmuMapFw       = 3,
    kUniCamIpuBootStepCreatePkgDir   = 4,
    kUniCamIpuBootStepPsysPowerOn    = 5,
    kUniCamIpuBootStepPsysMmuInit    = 6,
    kUniCamIpuBootStepIpcReset       = 7,
    kUniCamIpuBootStepBootLoad       = 8,
    kUniCamIpuBootStepBootMagic      = 9,
    kUniCamIpuBootStepAuthRun        = 10,
    kUniCamIpuBootStepIsysPowerOn    = 11,
    kUniCamIpuBootStepTscSync        = 12,
    kUniCamIpuBootStepIsysMmuInit    = 13,
    kUniCamIpuBootStepSyscomOpen     = 14,
    kUniCamIpuBootStepIwakeDisable   = 15
};

typedef struct UniCamIpuHwInfo {
    uint32_t schemaVersion;
    uint16_t vendorId;
    uint16_t deviceId;
    uint16_t revisionId;
    uint16_t subsystemVendorId;
    uint16_t subsystemId;
    uint16_t pciCommand;
    uint64_t bar0PhysAddr;
    uint64_t bar0Length;
    uint32_t msiInterruptIndex;
    uint8_t  hasIommuParent;
    uint8_t  activeDmaMode;
    uint8_t  secureMode;
    uint8_t  fwAuthenticated;
    uint8_t  isysPowered;
    uint8_t  streaming;
    uint16_t pciPmcsr;
    uint32_t skuReg;
    uint32_t securityCtlReg;
    uint32_t pwrStateReg;
    uint32_t refClkTenthsMhz;
    uint32_t fwPkgDate;
    char     modelName[32];
} UniCamIpuHwInfo;

typedef struct UniCamIpuDiag {
    uint32_t securityCtl;
    uint32_t pwrState;
    uint32_t isrStatus;
    uint32_t isrEnable;
    uint32_t isFreqCtl;
    uint32_t psFreqCtl;
    uint32_t unispartIrqStatus;
    uint32_t sip0IrqStatus;
    uint32_t sip1IrqStatus;
    uint32_t spcStatusCtrl;
    uint32_t syscomState;
    uint32_t lastCsiErrorMask;
    uint32_t lastFwError;
    uint32_t lastFwErrorDetails;
    uint64_t msiIrqCount;
    uint64_t isysIrqCount;
    uint64_t csiIrqCount;
    uint64_t cseIrqCount;
    uint64_t polledTokenCount;
    uint64_t framesCompleted;
    uint64_t framesSkipped;
    uint64_t framesDropped;
    uint32_t psysMmuMappedPages;
    uint32_t isysMmuMappedPages;
    uint32_t pciBar0Lo;
    uint32_t pciBar0Hi;
    uint32_t bootBar0Lo;
    uint32_t bootBar0Hi;
    uint32_t psFreqCaps;
    uint32_t punitMailbox;
    uint32_t cse2IuCsr;
    uint32_t fwResetCtl;
    uint32_t pmRespReg;
    uint32_t csiPhyPwrCtrl;
} UniCamIpuDiag;

typedef struct UniCamIpuBootReport {
    uint32_t fwSizeBytes;
    uint32_t fwPkgDate;
    uint32_t cpdComponents;
    uint32_t bootloaderMagic;
    uint32_t syscomState;
    uint32_t psysPkgDirIova;
    uint32_t syscomConfigIova;
    uint32_t securityCtlBefore;
    uint32_t securityCtlAfter;
    uint32_t elapsedMs;
    uint32_t stepFailed;
    int32_t  statusRet;
    uint8_t  secureMode;
    uint8_t  alreadyAuthenticated;
    uint8_t  ipcResetOk;
    uint8_t  bootLoadOk;
    uint8_t  authRunOk;
    uint8_t  syscomHandshakeOk;
    uint8_t  iwakeDisableOk;
    uint8_t  activeDmaMode;
    uint8_t  reserved[3];
    uint32_t pwrStateAtFail;
    uint32_t psFreqAtFail;
    uint32_t isFreqAtFail;
    uint32_t pmRespAtFail;
} UniCamIpuBootReport;

typedef struct UniCamIpuStreamConfig {
    uint32_t csiPort;
    uint32_t csiLanes;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat; /* UniCamIpuPixelFormat */
    uint32_t vc;
    uint64_t linkFreqHz;
    uint32_t shiftedDataTicks;
    uint32_t reserved0;
} UniCamIpuStreamConfig;

typedef struct UniCamIpuStreamStatus {
    uint32_t hwPortIndex;
    uint32_t fwStreamSource;
    uint32_t stride;
    uint32_t payloadBytes;
    uint32_t ringBaseIova;
    uint32_t ctermen;
    uint32_t csettle;
    uint32_t dtermen;
    uint32_t dsettle;
    uint32_t queuedSlots;
    uint8_t  streamOpened;
    uint8_t  streamStarted;
    uint8_t  reserved[2];
} UniCamIpuStreamStatus;

typedef struct UniCamIpuFrameEvent {
    uint32_t slotIndex;
    uint32_t sequence;
    uint32_t fwFrameCounter;
    uint32_t payloadBytes;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint64_t timestampTicks;
    uint64_t timestampNs;
    uint32_t csiErrorMask;
    uint32_t fwError;
    uint8_t  isSkipped;
    uint8_t  reserved[7];
} UniCamIpuFrameEvent;

typedef struct UniCamIpuRingSlotMeta {
    uint32_t state;         /* UniCamIpuSlotState */
    uint32_t sequence;
    uint32_t fwFrameCounter;
    uint32_t payloadBytes;
    uint32_t csiErrorMask;
    uint32_t fwError;
    uint64_t timestampTicks;
    uint64_t timestampNs;
} UniCamIpuRingSlotMeta;

typedef struct UniCamIpuRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slotCount;
    uint32_t slotStrideBytes;
    uint32_t activeWidth;
    uint32_t activeHeight;
    uint32_t activeStride;
    uint32_t activePayloadBytes;
    uint32_t pixelFormat;
    uint32_t streaming;
    uint32_t slotOffsets[UNICAM_IPU_RING_SLOTS];
    UniCamIpuRingSlotMeta slots[UNICAM_IPU_RING_SLOTS];
} UniCamIpuRingHeader;

typedef struct UniCamIspFrameStats {
    uint32_t payloadBytes;
    uint32_t avgLuma;
    uint32_t avgR;
    uint32_t avgG;
    uint32_t avgB;
    uint32_t rGainQ10;
    uint32_t gGainQ10;
    uint32_t bGainQ10;
    uint32_t exposureLines;
    uint32_t analogGain;
    uint8_t  aeUpdated;
    uint8_t  reserved[7];
} UniCamIspFrameStats;

#ifdef __cplusplus
static_assert(sizeof(UniCamIpuHwInfo) == 96, "UniCamIpuHwInfo size mismatch");
static_assert(sizeof(UniCamIpuDiag) == 168, "UniCamIpuDiag size mismatch");
static_assert(sizeof(UniCamIpuBootReport) == 76, "UniCamIpuBootReport size mismatch");
static_assert(sizeof(UniCamIpuStreamConfig) == 40, "UniCamIpuStreamConfig size mismatch");
static_assert(sizeof(UniCamIpuStreamStatus) == 44, "UniCamIpuStreamStatus size mismatch");
static_assert(sizeof(UniCamIpuFrameEvent) == 64, "UniCamIpuFrameEvent size mismatch");
static_assert(sizeof(UniCamIpuRingSlotMeta) == 40, "UniCamIpuRingSlotMeta size mismatch");
static_assert(sizeof(UniCamIspFrameStats) == 48, "UniCamIspFrameStats size mismatch");
static_assert(sizeof(UniCamIpuRingHeader) <= UNICAM_IPU_RING_HEADER_BYTES,
              "UniCamIpuRingHeader exceeds 4KB page");
}
#endif

#endif /* UNICAM_IPU_ABI_H */
