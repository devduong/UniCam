#ifndef UNICAM_CORE_ABI_H
#define UNICAM_CORE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNICAM_CORE_SERVICE_NAME    "UniCamCoreService"
#define UNICAM_USER_CLIENT_TYPE     0U
#define UNICAM_SCHEMA_VERSION       1U
#define UNICAM_OV01A10_CHIP_ID      0x00560141U
#define UNICAM_OV2740_CHIP_ID       0x00002740U
#define UNICAM_OV02C10_CHIP_ID      0x00560243U
#define UNICAM_OV5693_CHIP_ID       0x00005690U
#define UNICAM_OV5675_CHIP_ID       0x00005675U

enum UniCamSelector {
    kUniCamMethodGetInfo          = 0,
    kUniCamMethodGetDiag          = 1,
    kUniCamMethodProbeSensor      = 2,
    kUniCamMethodSetPower         = 3,
    kUniCamMethodSetLed           = 4,
    kUniCamMethodResetSensor      = 5,
    kUniCamMethodReadReg          = 6,
    kUniCamMethodWriteReg         = 7,
    kUniCamMethodConfigureSensor  = 8,
    kUniCamMethodSetStream        = 9,
    kUniCamMethodSetExposureGain  = 10,
    kUniCamMethodCount
};

enum UniCamGpioRoleIndex {
    kUniCamGpioReset = 0,
    kUniCamGpioPwdn  = 1,
    kUniCamGpioPower = 2,
    kUniCamGpioClkEn = 3,
    kUniCamGpioLed   = 4,
    kUniCamGpioRoleCount = 5
};

enum UniCamBoardCapFlags {
    kUniCamCapReady        = (1U << 0),
    kUniCamCapHasLed       = (1U << 1),
    kUniCamCapDiscretePmic = (1U << 2),
    kUniCamCapI2cPmic      = (1U << 3)
};

typedef struct UniCamGpioDiag {
    uint32_t role;        /* UniCamGpioRoleIndex */
    uint32_t activeHigh;  /* 1 = active high, 0 = active low */
    uint64_t dw0PhysAddr; /* 0 if absent on this board */
    uint32_t dw0Val;      /* Raw PCH GPIO PADCFG DW0 */
    uint32_t dw1Val;      /* Raw PCH GPIO PADCFG DW1 */
    uint8_t  mapped;      /* 1 if MMIO mapped and readable */
    uint8_t  txState;     /* DW0 bit 0 (GPIOTxState) */
    uint8_t  txDisabled;  /* DW0 bit 8 (GPIOTxDis) */
    uint8_t  padMode;     /* DW0 bits 10..12 (0 = GPIO) */
} UniCamGpioDiag;

typedef struct UniCamHardwareDiag {
    UniCamGpioDiag gpios[kUniCamGpioRoleCount];
    uint64_t mclkRegPhysAddr; /* e.g. 0xFDAD800C */
    uint32_t mclkRegVal;      /* Raw 8-bit/32-bit ICLK register */
    uint8_t  mclkMapped;
    uint8_t  mclkEnabled;     /* bit 1 of ICLK */
    uint8_t  mclkFreq19_2;    /* bit 0 of ICLK (1 = 19.2MHz, 0 = 24MHz) */
    uint8_t  powerStateOn;    /* Cached software power state */
    uint8_t  ledStateOn;      /* Cached software LED state */
    uint8_t  streamStateOn;   /* Cached software stream state */
    uint8_t  usingMmioHal;    /* 1 if MMIO fallback active */
    uint8_t  reserved0;
    uint32_t i2cTxCount;
    uint32_t i2cRxCount;
    uint32_t i2cErrCount;
} UniCamHardwareDiag;

typedef struct UniCamBoardSensorInfo {
    uint32_t schemaVersion;
    uint32_t acpiCapMask;     /* UCIN() return mask */
    uint32_t i2cBus;
    uint32_t i2cAddr;
    uint32_t eepromAddr;
    uint32_t csiPort;
    uint32_t csiLanes;
    uint64_t linkFreqHz;
    uint64_t mclkHz;
    uint32_t mclkSrc;
    uint64_t mclkRegPhysAddr;
    uint32_t rotation;
    uint32_t sensorWidth;
    uint32_t sensorHeight;
    uint32_t sensorBpp;
    uint32_t expectedChipId;
    uint8_t  powerOn;
    uint8_t  ledOn;
    uint8_t  streaming;
    uint8_t  hasAcpiMethods;
    char     boardId[64];
    char     ipuGen[32];
    char     ipuPath[64];
    char     sensorHid[32];
    char     sensorCompat[64];
    char     position[16];
} UniCamBoardSensorInfo;

typedef struct UniCamProbeReport {
    uint32_t chipId;
    uint32_t expectedChipId;
    uint8_t  rawIdBytes[4];
    uint8_t  eepromHeader[8];
    uint8_t  eepromReadOk;
    uint8_t  chipIdMatched;
    uint8_t  reserved[2];
    int32_t  powerOnRet;
    int32_t  i2cRet;
    int32_t  powerOffRet;
    UniCamHardwareDiag diagBefore;
    UniCamHardwareDiag diagActive;
    UniCamHardwareDiag diagAfter;
} UniCamProbeReport;

#ifdef __cplusplus
static_assert(sizeof(UniCamGpioDiag) == 32, "UniCamGpioDiag size mismatch");
static_assert(sizeof(UniCamHardwareDiag) == 192, "UniCamHardwareDiag size mismatch");
static_assert(sizeof(UniCamBoardSensorInfo) == 360, "UniCamBoardSensorInfo size mismatch");
static_assert(sizeof(UniCamProbeReport) == 616, "UniCamProbeReport size mismatch");
}
#endif

#endif /* UNICAM_CORE_ABI_H */
