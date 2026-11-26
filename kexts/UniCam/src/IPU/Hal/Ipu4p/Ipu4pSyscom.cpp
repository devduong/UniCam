#include "Ipu4pSyscom.hpp"
#include "Ipu4pRegs.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace ipu4p {

namespace {

size_t roundUp8(size_t v) {
    return (v + 7U) & ~static_cast<size_t>(7U);
}

} // namespace

Ipu4pSyscom::Ipu4pSyscom()
    : isysBase_(nullptr),
      dmemBase_(nullptr),
      spcBase_(nullptr),
      isysMmu_(nullptr),
      configIova_(0U),
      prepared_(false),
      inputQueues_(nullptr),
      outputQueues_(nullptr),
      numInputQueues_(0U),
      numOutputQueues_(0U) {
}

Ipu4pSyscom::~Ipu4pSyscom() {
    release();
}

IOReturn Ipu4pSyscom::prepare(IOPCIDevice *pciDevice,
                              uint32_t dmaMode,
                              Ipu4pMmu *isysMmu,
                              volatile uint8_t *isysBase,
                              uint32_t numStreams) {
    release();

    if (!pciDevice || !isysMmu || !isysBase || numStreams == 0U || numStreams > kIsysMaxStreams) {
        return kIOReturnBadArgument;
    }

    isysBase_ = isysBase;
    dmemBase_ = isysBase + kIsysDmemOffset;
    spcBase_ = isysBase + kIsysSpcOffset;
    isysMmu_ = isysMmu;

    const uint32_t numMsgQueues = numStreams;
    numInputQueues_ = kBaseMsgSendQueues + numMsgQueues;
    numOutputQueues_ = kNMaxRecvQueues;

    uint32_t inTokenSz[kNMaxSendQueues] {};
    uint32_t inQueueSz[kNMaxSendQueues] {};
    inTokenSz[kBaseProxySendQueues] = sizeof(FwIsysProxySendToken);
    inQueueSz[kBaseProxySendQueues] = kIsysSizeProxySendQueue;
    inTokenSz[kBaseDevSendQueues] = sizeof(FwIsysSendToken);
    inQueueSz[kBaseDevSendQueues] = kDevSendQueueSize;
    for (uint32_t i = 0; i < numMsgQueues; ++i) {
        inTokenSz[kBaseMsgSendQueues + i] = sizeof(FwIsysSendToken);
        inQueueSz[kBaseMsgSendQueues + i] = kIsysSizeSendQueue;
    }

    uint32_t outTokenSz[kNMaxRecvQueues] {};
    uint32_t outQueueSz[kNMaxRecvQueues] {};
    outTokenSz[kBaseProxyRecvQueues] = sizeof(FwIsysProxyRecvToken);
    outQueueSz[kBaseProxyRecvQueues] = kIsysSizeProxyRecvQueue;
    outTokenSz[kBaseMsgRecvQueues] = sizeof(FwIsysRecvToken);
    outQueueSz[kBaseMsgRecvQueues] = kIsysSizeRecvQueue;

    const size_t confSize = roundUp8(sizeof(FwSyscomConfig));
    const size_t inqSize = static_cast<size_t>(numInputQueues_) * sizeof(FwSysQueue);
    const size_t outqSize = static_cast<size_t>(numOutputQueues_) * sizeof(FwSysQueue);
    const size_t specSize = roundUp8(sizeof(FwIsysFwConfig));

    size_t sizeInput = 0;
    for (uint32_t i = 0; i < numInputQueues_; ++i) {
        sizeInput += static_cast<size_t>(inQueueSz[i] + 1U) * inTokenSz[i];
    }
    size_t sizeOutput = 0;
    for (uint32_t i = 0; i < numOutputQueues_; ++i) {
        sizeOutput += static_cast<size_t>(outQueueSz[i] + 1U) * outTokenSz[i];
    }

    const size_t totalSize = confSize + inqSize + outqSize + specSize + sizeInput + sizeOutput;
    if (!dmaBuf_.allocate(pciDevice, dmaMode, totalSize, false)) {
        return kIOReturnNoMemory;
    }

    configIova_ = isysMmu_->mapBuffer(dmaBuf_);
    if (configIova_ == 0U) {
        dmaBuf_.release();
        return kIOReturnNoMemory;
    }

    uint8_t *baseCpu = static_cast<uint8_t *>(dmaBuf_.cpuAddr());
    memset(baseCpu, 0, totalSize);

    auto *cfg = reinterpret_cast<FwSyscomConfig *>(baseCpu);
    size_t off = confSize;
    inputQueues_ = reinterpret_cast<FwSysQueue *>(baseCpu + off);
    cfg->input_queue = configIova_ + static_cast<uint32_t>(off);
    cfg->num_input_queues = numInputQueues_;

    off += inqSize;
    outputQueues_ = reinterpret_cast<FwSysQueue *>(baseCpu + off);
    cfg->output_queue = configIova_ + static_cast<uint32_t>(off);
    cfg->num_output_queues = numOutputQueues_;

    off += outqSize;
    auto *isysCfg = reinterpret_cast<FwIsysFwConfig *>(baseCpu + off);
    cfg->specific_addr = configIova_ + static_cast<uint32_t>(off);
    cfg->specific_size = sizeof(FwIsysFwConfig);

    isysCfg->num_send_queues[kFwQueueProxy] = kNMaxProxySendQueues;
    isysCfg->num_send_queues[kFwQueueDev] = kNMaxDevSendQueues;
    isysCfg->num_send_queues[kFwQueueMsg] = numMsgQueues;
    isysCfg->num_recv_queues[kFwQueueProxy] = kNMaxProxyRecvQueues;
    isysCfg->num_recv_queues[kFwQueueDev] = 0U;
    isysCfg->num_recv_queues[kFwQueueMsg] = kNMaxRecvQueues - kNMaxProxyRecvQueues;

    const uint32_t gdaPagesPerStream = (kDeviceGdaNrPages * kDeviceGdaVirtFactor) / numMsgQueues;
    for (uint32_t i = 0; i < kNofSramBlocksMax; ++i) {
        isysCfg->buffer_partition.num_gda_pages[i] = (i < numMsgQueues) ? gdaPagesPerStream : 0U;
    }

    off += specSize;
    uint32_t dmemRegIdx = kSyscomQprBaseReg;
    uint64_t curHost = reinterpret_cast<uintptr_t>(baseCpu + off);
    uint32_t curIova = configIova_ + static_cast<uint32_t>(off);

    for (uint32_t i = 0; i < numInputQueues_; ++i) {
        const uint32_t qBytes = (inQueueSz[i] + 1U) * inTokenSz[i];
        inputQueues_[i].size = inQueueSz[i] + 1U;
        inputQueues_[i].token_size = inTokenSz[i];
        inputQueues_[i].host_address = curHost;
        inputQueues_[i].vied_address = curIova;
        inputQueues_[i].wr_reg = dmemRegIdx++;
        inputQueues_[i].rd_reg = dmemRegIdx++;
        curHost += qBytes;
        curIova += qBytes;
    }

    off += sizeInput;
    curHost = reinterpret_cast<uintptr_t>(baseCpu + off);
    curIova = configIova_ + static_cast<uint32_t>(off);
    for (uint32_t i = 0; i < numOutputQueues_; ++i) {
        const uint32_t qBytes = (outQueueSz[i] + 1U) * outTokenSz[i];
        outputQueues_[i].size = outQueueSz[i] + 1U;
        outputQueues_[i].token_size = outTokenSz[i];
        outputQueues_[i].host_address = curHost;
        outputQueues_[i].vied_address = curIova;
        outputQueues_[i].wr_reg = dmemRegIdx++;
        outputQueues_[i].rd_reg = dmemRegIdx++;
        curHost += qBytes;
        curIova += qBytes;
    }

    dmaBuf_.flushCache();
    prepared_ = true;
    return kIOReturnSuccess;
}

bool Ipu4pSyscom::isSpReady() const {
    if (!spcBase_) return false;
    const uint32_t val = mmioRead32(spcBase_, kSpcRegStatusCtrl) & (kSpcStatusReady | kSpcStatusStart);
    return val == kSpcStatusReady;
}

void Ipu4pSyscom::startSp() {
    uint32_t val = kSpcStatusStart | kSpcStatusRun | kSpcStatusIcacheInvalidate;
    mmioWrite32(spcBase_, kSpcRegStatusCtrl, val);
}

uint32_t Ipu4pSyscom::readSpcStatusCtrl() const {
    return spcBase_ ? mmioRead32(spcBase_, kSpcRegStatusCtrl) : 0U;
}

uint32_t Ipu4pSyscom::readSyscomState() const {
    return dmemBase_ ? mmioRead32(dmemBase_, kSyscomStateReg * 4U) : 0U;
}

bool Ipu4pSyscom::isSyscomReady() const {
    return readSyscomState() == kSyscomStateReady;
}

IOReturn Ipu4pSyscom::openAndWaitReady() {
    if (!prepared_ || !dmemBase_ || !spcBase_) {
        return kIOReturnNotReady;
    }

    if (!isSpReady()) {
        IOLog("UniCamIPU: ISYS SP not ready before SysCom open (spc=0x%08x)\n", readSpcStatusCtrl());
        return kIOReturnError;
    }

    mmioWrite32(dmemBase_, kSyscomStateReg * 4U, kSyscomStateUninit);
    mmioWrite32(dmemBase_, kSyscomCommandReg * 4U, kSyscomCmdUninit);
    mmioWrite32(dmemBase_, kSyscomConfigReg * 4U, configIova_);
    ipuWmb();

    startSp();

    for (uint32_t retry = 0; retry < 2000U; ++retry) {
        IODelay(450);
        if (isSyscomReady()) {
            return kIOReturnSuccess;
        }
    }

    IOLog("UniCamIPU: SysCom handshake timeout (state=0x%08x spc=0x%08x)\n",
          readSyscomState(), readSpcStatusCtrl());
    return kIOReturnTimeout;
}

IOReturn Ipu4pSyscom::closeAndWaitRelease(bool force) {
    if (!prepared_ || !dmemBase_) {
        return kIOReturnSuccess;
    }

    if (readSyscomState() == kSyscomStateReady) {
        mmioWrite32(dmemBase_, kSyscomCommandReg * 4U, kSyscomCmdInactive);
        ipuWmb();
    }

    bool spStopped = false;
    for (uint32_t retry = 0; retry < 2000U; ++retry) {
        IODelay(450);
        if (isSpReady()) {
            spStopped = true;
            break;
        }
    }

    if (!spStopped && !force) {
        IOLog("UniCamIPU: ISYS SP release timed out (spc=0x%08x syscom=0x%08x)\n",
              readSpcStatusCtrl(), readSyscomState());
        release();
        return kIOReturnTimeout;
    }

    release();
    return kIOReturnSuccess;
}

void Ipu4pSyscom::release() {
    if (isysMmu_ && configIova_ != 0U && dmaBuf_.isValid()) {
        isysMmu_->unmapRange(configIova_, dmaBuf_.byteSize());
    }
    dmaBuf_.release();
    configIova_ = 0U;
    inputQueues_ = nullptr;
    outputQueues_ = nullptr;
    numInputQueues_ = 0U;
    numOutputQueues_ = 0U;
    prepared_ = false;
}

void *Ipu4pSyscom::sendGetToken(uint32_t queueIndex) {
    if (!prepared_ || queueIndex >= numInputQueues_ || !inputQueues_) {
        return nullptr;
    }
    const FwSysQueue &q = inputQueues_[queueIndex];
    const uint32_t regOff = q.wr_reg * 4U;
    const uint32_t wr = mmioRead32(dmemBase_, regOff + 0U);
    const uint32_t rd = mmioRead32(dmemBase_, regOff + 4U);
    if (wr >= q.size || rd >= q.size) {
        return nullptr;
    }
    const uint32_t avail = (wr < rd) ? (rd - wr - 1U) : (q.size - (wr - rd + 1U));
    if (avail == 0U) {
        return nullptr;
    }
    return reinterpret_cast<void *>(static_cast<uintptr_t>(q.host_address) +
                                   static_cast<size_t>(wr) * q.token_size);
}

void Ipu4pSyscom::sendPutToken(uint32_t queueIndex) {
    if (!prepared_ || queueIndex >= numInputQueues_ || !inputQueues_) {
        return;
    }
    const FwSysQueue &q = inputQueues_[queueIndex];
    const uint32_t regOff = q.wr_reg * 4U;
    uint32_t wr = mmioRead32(dmemBase_, regOff + 0U);
    if (wr < q.size) {
        void *tokPtr = reinterpret_cast<void *>(static_cast<uintptr_t>(q.host_address) +
                                                static_cast<size_t>(wr) * q.token_size);
        ipuClflushRange(tokPtr, q.token_size);
    }
    wr += 1U;
    if (wr >= q.size) {
        wr = 0U;
    }
    mmioWrite32(dmemBase_, regOff + 0U, wr);
    ipuWmb();
}

void *Ipu4pSyscom::recvGetToken(uint32_t queueIndex) {
    if (!prepared_ || queueIndex >= numOutputQueues_ || !outputQueues_) {
        return nullptr;
    }
    const FwSysQueue &q = outputQueues_[queueIndex];
    const uint32_t regOff = q.wr_reg * 4U;
    uint32_t wr = mmioRead32(dmemBase_, regOff + 0U);
    const uint32_t rd = mmioRead32(dmemBase_, regOff + 4U);
    if (wr >= q.size || rd >= q.size) {
        return nullptr;
    }
    if (wr < rd) {
        wr += q.size;
    }
    if ((wr - rd) == 0U) {
        return nullptr;
    }
    void *tokPtr = reinterpret_cast<void *>(static_cast<uintptr_t>(q.host_address) +
                                            static_cast<size_t>(rd) * q.token_size);
    ipuClflushRange(tokPtr, q.token_size);
    return tokPtr;
}

void Ipu4pSyscom::recvPutToken(uint32_t queueIndex) {
    if (!prepared_ || queueIndex >= numOutputQueues_ || !outputQueues_) {
        return;
    }
    const FwSysQueue &q = outputQueues_[queueIndex];
    const uint32_t regOff = q.wr_reg * 4U;
    uint32_t rd = mmioRead32(dmemBase_, regOff + 4U) + 1U;
    if (rd >= q.size) {
        rd = 0U;
    }
    mmioWrite32(dmemBase_, regOff + 4U, rd);
    ipuWmb();
}

} // namespace ipu4p
