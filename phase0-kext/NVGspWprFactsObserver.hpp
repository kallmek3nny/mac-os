// SPDX-License-Identifier: MIT
#ifndef NV_GSP_WPR_FACTS_OBSERVER_HPP
#define NV_GSP_WPR_FACTS_OBSERVER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>

#include "NVDecode.hpp"
#include "NVGspBooterFuse.hpp"
#include "NVGspWprGa107Facts.hpp"

#define NV_GSP_BOOTER_PATCH_STAGE_BOOT_ARG "nvgsp-booter-patch-stage"
#define NV_GSP_BOOTER_LOAD_FIRMWARE_PROPERTY "NVGspBooterLoadFirmware"

class NVGspWprFactsObserver : public IOService
{
    OSDeclareDefaultStructors(NVGspWprFactsObserver);

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCI;
    IOMemoryMap *fBar0Map;
    volatile UInt8 *fBar0Base;

    // Optional SEC2 Booter signature-patch rehearsal. The helper allocates a
    // transient kernel-private copy, patches it, validates the decision, and
    // releases it without DMA mapping or publishing an address.
    void stageBooterPrivateImage(const NVGspBooterFuseObservation &observation);
    void clearResources(void);
    void publishState(const char *state);
};

#endif // NV_GSP_WPR_FACTS_OBSERVER_HPP
