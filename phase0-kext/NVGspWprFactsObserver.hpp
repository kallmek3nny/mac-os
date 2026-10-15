// SPDX-License-Identifier: MIT
#ifndef NV_GSP_WPR_FACTS_OBSERVER_HPP
#define NV_GSP_WPR_FACTS_OBSERVER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

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

    // Optional SEC2 Booter signature-patch rehearsal. Both objects are private
    // kernel memory references owned by this existing observer; neither is DMA
    // mapped or published as an address to another component.
    OSData *fBooterFirmware;
    IOBufferMemoryDescriptor *fBooterPrivate;

    void clearBooterStageResources(void);
    void clearResources(void);
    OSData *copyBooterLoadFirmwareBlob(void);
    void stageBooterPrivateImage(const NVGspBooterFuseObservation &observation);
    void publishState(const char *state);
};

#endif // NV_GSP_WPR_FACTS_OBSERVER_HPP
