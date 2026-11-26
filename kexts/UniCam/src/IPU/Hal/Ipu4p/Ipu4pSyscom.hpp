#ifndef UNICAM_IPU4P_SYSCOM_HPP
#define UNICAM_IPU4P_SYSCOM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "Ipu4pFwAbi.hpp"
#include "Ipu4pMmu.hpp"
#include "IpuDmaBuffer.hpp"

namespace ipu4p {

class Ipu4pSyscom {
public:
    Ipu4pSyscom();
    ~Ipu4pSyscom();

    IOReturn prepare(IOPCIDevice *pciDevice,
                     uint32_t dmaMode,
                     Ipu4pMmu *isysMmu,
                     volatile uint8_t *isysBase,
                     uint32_t numStreams);

    IOReturn openAndWaitReady();
    IOReturn closeAndWaitRelease(bool force);
    void release();

    bool isSpReady() const;
    bool isSyscomReady() const;
    uint32_t readSyscomState() const;
    uint32_t readSpcStatusCtrl() const;
    uint32_t configIova() const { return configIova_; }

    void *sendGetToken(uint32_t queueIndex);
    void sendPutToken(uint32_t queueIndex);

    void *recvGetToken(uint32_t queueIndex);
    void recvPutToken(uint32_t queueIndex);

private:
    void startSp();

    volatile uint8_t *isysBase_;
    volatile uint8_t *dmemBase_;
    volatile uint8_t *spcBase_;
    Ipu4pMmu *isysMmu_;

    IpuDmaBuffer dmaBuf_;
    uint32_t configIova_;
    bool prepared_;

    FwSysQueue *inputQueues_;
    FwSysQueue *outputQueues_;
    uint32_t numInputQueues_;
    uint32_t numOutputQueues_;
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_SYSCOM_HPP */
