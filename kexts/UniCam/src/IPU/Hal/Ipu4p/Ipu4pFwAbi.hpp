#ifndef UNICAM_IPU4P_FW_ABI_HPP
#define UNICAM_IPU4P_FW_ABI_HPP

#include <stddef.h>
#include <stdint.h>

namespace ipu4p {

/* CPD / Package Directory binary structures */
constexpr uint32_t kCpdHdrMark                     = 0x44504324U; /* '$CPD' */
constexpr uint64_t kPkgDirHdrMark                  = 0x5f4955504b44525fULL; /* '_IUPKDR_' */
constexpr uint32_t kMaxPkgDirEntCnt                = 16U;
constexpr uint32_t kPkgDirEntLen                   = 2U;
constexpr uint32_t kPkgDirSize                     = kMaxPkgDirEntCnt * kPkgDirEntLen * sizeof(uint64_t);

constexpr uint32_t kCpdMetadataExtnTypeIunit       = 0x10U;
constexpr uint32_t kCpdMetadataImgTypeMainFw       = 2U;
constexpr uint32_t kCpdPkgDirPsysServerIdx         = 0U;
constexpr uint32_t kCpdPkgDirIsysServerIdx         = 1U;
constexpr uint32_t kCpdMaxComponentId              = 127U;
constexpr uint32_t kCpdMaxComponentVer             = 0xffffU;

struct __attribute__((packed)) CpdHdr {
    uint32_t hdr_mark;
    uint32_t ent_cnt;
    uint8_t  hdr_ver;
    uint8_t  ent_ver;
    uint8_t  hdr_len;
};

struct __attribute__((packed)) CpdEnt {
    uint8_t  name[12];
    uint32_t offset;
    uint32_t len;
    uint8_t  rsvd[4];
};

struct __attribute__((packed)) CpdModuleDataHdr {
    uint32_t hdr_len;
    uint32_t endian;
    uint32_t fw_pkg_date;
    uint32_t hive_sdk_date;
    uint32_t compiler_date;
    uint32_t target_platform_type;
    uint8_t  sys_ver[11];
    uint8_t  fw_arch_ver[7];
    uint8_t  rsvd[2];
};

struct __attribute__((packed)) CpdMetadataExtn {
    uint32_t extn_type;
    uint32_t len;
    uint32_t img_type;
    uint8_t  rsvd[16];
};

struct __attribute__((packed)) CpdMetadataCmpntHdr {
    uint32_t id;
    uint32_t size;
    uint32_t ver;
};

struct __attribute__((packed)) CpdMetadataCmpntIpu4p {
    CpdMetadataCmpntHdr hdr;
    uint8_t  sha2_hash[32];
    uint32_t entry_point;
    uint32_t icache_base_offs;
    uint8_t  attrs[16];
};

struct CellProgram {
    uint32_t magic_number;
    uint32_t blob_offset;
    uint32_t blob_size;
    uint32_t start[3];
    uint32_t icache_source;
    uint32_t icache_target;
    uint32_t icache_size;
    uint32_t pmem_source;
    uint32_t pmem_target;
    uint32_t pmem_size;
    uint32_t data_source;
    uint32_t data_target;
    uint32_t data_size;
    uint32_t bss_target;
    uint32_t bss_size;
    uint32_t cell_id;
    uint32_t regs_addr;
    uint32_t cell_pmem_data_bus_address;
    uint32_t cell_dmem_data_bus_address;
    uint32_t cell_pmem_control_bus_address;
    uint32_t cell_dmem_control_bus_address;
    uint32_t next;
    uint32_t dummy[2];
};

static_assert(sizeof(CpdMetadataExtn) == 28, "CpdMetadataExtn size mismatch");
static_assert(sizeof(CpdMetadataCmpntIpu4p) == 68, "CpdMetadataCmpntIpu4p size mismatch");

/* SysCom & ISYS firmware wire protocol structures */
constexpr uint32_t kIsysMaxStreams                 = 8U;
constexpr uint32_t kMaxIpins                       = 4U;
constexpr uint32_t kMaxOpins                       = 6U;
constexpr uint32_t kNofSramBlocksMax               = 8U;
constexpr uint32_t kDeviceGdaNrPages               = 128U;
constexpr uint32_t kDeviceGdaVirtFactor            = 32U;
constexpr uint32_t kNMaxProxySendQueues            = 1U;
constexpr uint32_t kNMaxDevSendQueues              = 1U;
constexpr uint32_t kNMaxProxyRecvQueues            = 1U;
constexpr uint32_t kDevSendQueueSize               = 8U;
constexpr uint32_t kIsysSizeRecvQueue              = 40U;
constexpr uint32_t kIsysSizeSendQueue              = 40U;
constexpr uint32_t kIsysSizeProxyRecvQueue         = 5U;
constexpr uint32_t kIsysSizeProxySendQueue         = 5U;

constexpr uint32_t kBaseProxySendQueues            = 0U;
constexpr uint32_t kBaseDevSendQueues              = 1U;
constexpr uint32_t kBaseMsgSendQueues              = 2U;
constexpr uint32_t kNMaxSendQueues                 = kBaseMsgSendQueues + kIsysMaxStreams;
constexpr uint32_t kBaseProxyRecvQueues            = 0U;
constexpr uint32_t kBaseMsgRecvQueues              = 1U;
constexpr uint32_t kNMaxRecvQueues                 = 2U;

constexpr uint32_t kIsysCroppingLocationMax        = 4U;
constexpr uint32_t kIsysResolutionInfoMax          = 2U;

constexpr uint32_t kSyscomStateUninit              = 0x57a7e000U;
constexpr uint32_t kSyscomStateReady               = 0x57a7e001U;
constexpr uint32_t kSyscomStateInactive            = 0x57a7e002U;
constexpr uint32_t kSyscomCmdUninit                = 0x57a7f000U;
constexpr uint32_t kSyscomCmdInactive              = 0x57a7f001U;

constexpr uint32_t kSyscomConfigReg                = 1U;
constexpr uint32_t kSyscomStateReg                 = 2U;
constexpr uint32_t kSyscomCommandReg               = 3U;
constexpr uint32_t kSyscomQprBaseReg               = 8U;

enum FwIsysRespType : uint8_t {
    kFwRespStreamOpenDone            = 0,
    kFwRespStreamStartAck            = 1,
    kFwRespStreamStartAndCaptureAck  = 2,
    kFwRespStreamCaptureAck          = 3,
    kFwRespStreamStopAck             = 4,
    kFwRespStreamFlushAck            = 5,
    kFwRespStreamCloseAck            = 6,
    kFwRespPinDataReady              = 7,
    kFwRespPinDataWatermark          = 8,
    kFwRespFrameSof                  = 9,
    kFwRespFrameEof                  = 10,
    kFwRespStreamStartAndCaptureDone = 11,
    kFwRespStreamCaptureDone         = 12,
    kFwRespPinDataSkipped            = 13,
    kFwRespStreamCaptureSkipped      = 14,
    kFwRespFrameSofDiscarded         = 15,
    kFwRespFrameEofDiscarded         = 16,
    kFwRespStatsDataReady            = 17,
    kFwRespCount
};

enum FwIsysSendType : uint16_t {
    kFwSendStreamOpen            = 0,
    kFwSendStreamStart           = 1,
    kFwSendStreamStartAndCapture = 2,
    kFwSendStreamCapture         = 3,
    kFwSendStreamStop            = 4,
    kFwSendStreamFlush           = 5,
    kFwSendStreamClose           = 6,
    kFwSendCount
};

enum FwIsysQueueType {
    kFwQueueProxy = 0,
    kFwQueueDev   = 1,
    kFwQueueMsg   = 2,
    kFwQueueCount
};

enum FwIsysStreamSource : uint8_t {
    kFwSrcPort3  = 3,
    kFwSrcPort5  = 5,
    kFwSrcPort6  = 6,
    kFwSrcPort12 = 12
};

enum FwIsysFrameFormat : uint8_t {
    kFwFmtRaw8  = 20,
    kFwFmtRaw10 = 21,
    kFwFmtRaw12 = 22,
    kFwFmtRaw14 = 23,
    kFwFmtRaw16 = 24
};

enum FwIsysMipiDataType : uint8_t {
    kFwMipiDtRaw10 = 0x2b,
    kFwMipiDtMax   = 0x40
};

enum FwIsysPinType : uint8_t {
    kFwPinMipi   = 0,
    kFwPinRawNs  = 1,
    kFwPinRawS   = 2,
    kFwPinRawSoc = 3
};

enum FwIsysMipiStoreMode : uint8_t {
    kFwMipiStoreNormal            = 0,
    kFwMipiStoreDiscardLongHeader = 1
};

struct __attribute__((packed)) FwSysQueue {
    uint64_t host_address;
    uint32_t vied_address;
    uint32_t size;
    uint32_t token_size;
    uint32_t wr_reg;
    uint32_t rd_reg;
    uint32_t _align;
};

struct FwSyscomConfig {
    uint32_t firmware_address;
    uint32_t num_input_queues;
    uint32_t num_output_queues;
    uint32_t input_queue;
    uint32_t output_queue;
    uint32_t specific_addr;
    uint32_t specific_size;
};

struct FwIsysResolution {
    uint32_t width;
    uint32_t height;
};

struct FwIsysIsaCfg {
    FwIsysResolution isa_res[kIsysResolutionInfoMax];
    uint32_t cfg_fields;
};

struct FwIsysCropping {
    int32_t top_offset;
    int32_t left_offset;
    int32_t bottom_offset;
    int32_t right_offset;
};

struct FwIsysInputPinInfo {
    FwIsysResolution input_res;
    uint8_t dt;
    uint8_t mipi_store_mode;
    uint8_t bits_per_pix;
    uint8_t mapped_dt;
};

struct FwIsysOutputPinInfo {
    FwIsysResolution output_res;
    uint32_t stride;
    uint32_t watermark_in_lines;
    uint32_t payload_buf_size;
    uint8_t send_irq;
    uint8_t input_pin_id;
    uint8_t pt;
    uint8_t ft;
    uint8_t link_id;
    uint8_t reserve_compression;
};

struct FwIsysStreamCfg {
    FwIsysIsaCfg isa_cfg;
    FwIsysCropping crop[kIsysCroppingLocationMax];
    FwIsysInputPinInfo input_pins[kMaxIpins];
    FwIsysOutputPinInfo output_pins[kMaxOpins];
    uint32_t compfmt;
    uint8_t nof_input_pins;
    uint8_t nof_output_pins;
    uint8_t send_irq_sof_discarded;
    uint8_t send_irq_eof_discarded;
    uint8_t send_resp_sof_discarded;
    uint8_t send_resp_eof_discarded;
    uint8_t src;
    uint8_t vc;
    uint8_t isl_use;
};

struct FwIsysOutputPinPayload {
    uint64_t out_buf_id;
    uint32_t addr;
    uint32_t compress;
};

struct FwIsysParamPin {
    uint64_t param_buf_id;
    uint32_t addr;
};

struct FwIsysFrameBuffSet {
    FwIsysOutputPinPayload output_pins[kMaxOpins];
    FwIsysParamPin process_group_light;
    uint8_t send_irq_sof;
    uint8_t send_irq_eof;
    uint8_t send_irq_capture_ack;
    uint8_t send_irq_capture_done;
    uint8_t send_resp_sof;
    uint8_t send_resp_eof;
    uint8_t frame_counter;
};

struct FwIsysErrorInfo {
    uint32_t error;
    uint32_t error_details;
};

struct FwIsysRespInfo {
    uint64_t buf_id;
    FwIsysOutputPinPayload pin;
    FwIsysParamPin process_group_light;
    FwIsysErrorInfo error_info;
    uint32_t timestamp[2];
    uint8_t stream_handle;
    uint8_t type;
    uint8_t pin_id;
    uint8_t acc_id;
    uint8_t frame_counter;
    uint8_t written_direct;
};

struct FwIsysProxyRespInfo {
    uint32_t request_id;
    FwIsysErrorInfo error_info;
};

struct FwIsysSendToken {
    uint64_t buf_handle;
    uint32_t payload;
    uint16_t send_type;
    uint16_t stream_id;
};

struct FwIsysRecvToken {
    FwIsysRespInfo resp_info;
};

struct FwIsysProxySendToken {
    uint32_t request_id;
    uint32_t region_index;
    uint32_t offset;
    uint32_t value;
};

struct FwIsysProxyRecvToken {
    FwIsysProxyRespInfo proxy_resp_info;
};

struct FwIsysBufferPartition {
    uint32_t num_gda_pages[kIsysMaxStreams];
};

struct FwIsysFwConfig {
    FwIsysBufferPartition buffer_partition;
    uint32_t num_send_queues[kFwQueueCount];
    uint32_t num_recv_queues[kFwQueueCount];
};

/* Binary ABI assertions matching ruslanbay/ipu4-drivers ipu4p-fw-isys.h */
static_assert(sizeof(FwIsysOutputPinPayload) == 16, "OutputPinPayload size mismatch");
static_assert(offsetof(FwIsysOutputPinPayload, addr) == 8, "OutputPinPayload addr offset mismatch");
static_assert(sizeof(FwIsysSendToken) == 16, "SendToken size mismatch");
static_assert(offsetof(FwIsysSendToken, payload) == 8, "SendToken payload offset mismatch");
static_assert(sizeof(FwIsysOutputPinInfo) == 28, "OutputPinInfo size mismatch");
static_assert(offsetof(FwIsysOutputPinInfo, link_id) == 24, "OutputPinInfo link_id offset mismatch");
static_assert(offsetof(FwIsysOutputPinInfo, reserve_compression) == 25, "OutputPinInfo reserve_compression offset mismatch");
static_assert(sizeof(FwIsysStreamCfg) == 316, "StreamCfg size mismatch");
static_assert(offsetof(FwIsysStreamCfg, output_pins) == 132, "StreamCfg output_pins offset mismatch");
static_assert(offsetof(FwIsysStreamCfg, compfmt) == 300, "StreamCfg compfmt offset mismatch");
static_assert(offsetof(FwIsysStreamCfg, src) == 310, "StreamCfg src offset mismatch");
static_assert(offsetof(FwIsysFrameBuffSet, process_group_light) == 96, "FrameBuffSet ppg offset mismatch");
static_assert(offsetof(FwIsysRespInfo, process_group_light) == 24, "RespInfo ppg offset mismatch");
static_assert(sizeof(FwIsysRecvToken) == 64, "RecvToken size mismatch");
static_assert(sizeof(FwIsysProxySendToken) == 16, "ProxySendToken size mismatch");
static_assert(sizeof(FwIsysProxyRecvToken) == 12, "ProxyRecvToken size mismatch");

} // namespace ipu4p

#endif /* UNICAM_IPU4P_FW_ABI_HPP */
