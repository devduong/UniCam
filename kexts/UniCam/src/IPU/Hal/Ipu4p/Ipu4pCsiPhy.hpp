#ifndef UNICAM_IPU4P_CSI_PHY_HPP
#define UNICAM_IPU4P_CSI_PHY_HPP

#include <IOKit/IOTypes.h>

namespace ipu4p {

struct Csi2Timing {
    uint32_t ctermen;
    uint32_t csettle;
    uint32_t dtermen;
    uint32_t dsettle;
};

class Ipu4pCsiPhy {
public:
    static bool mapFwPortToHwIndex(uint32_t fwPort, uint32_t *outHwIndex);
    static void calcTiming(uint64_t linkFreqHz, Csi2Timing *outTiming);

    static void irqSetup(volatile uint8_t *isysBase);
    static void phySetup(volatile uint8_t *bar0Base, volatile uint8_t *isysBase);

    static IOReturn setPortPower(volatile uint8_t *isysBase,
                                 uint32_t hwPortIndex,
                                 uint32_t nlanes,
                                 const Csi2Timing *timing,
                                 uint32_t shiftedDataTicks,
                                 bool on);

    static uint32_t collectAndClearPortErrors(volatile uint8_t *isysBase, uint32_t hwPortIndex);
};

} // namespace ipu4p

#endif /* UNICAM_IPU4P_CSI_PHY_HPP */
