// SPDX-License-Identifier: MIT
// Allocation-only staging of the exact GA107 cold-boot WPR metadata image.
#ifndef NV_GSP_WPR_META_STAGE_SERVICE_HPP
#define NV_GSP_WPR_META_STAGE_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

#include "NVGspWprCandidate.hpp"

#define NV_GSP_WPR_META_STAGE_BOOT_ARG "nvgsp-wpr-meta-stage"
#define NV_GSP_WPR_META_STAGE_PAGE_BYTES 4096u
#define NV_GSP_WPR_META_STAGE_WAIT_NS 1000000000ull

class NVGspWprMetaStageService : public IOService
{
    OSDeclareDefaultStructors(NVGspWprMetaStageService);
    typedef IOService super;

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCI;
    IOBufferMemoryDescriptor *fBacking;
    IODMACommand *fDMA;
    bool fDMABound;

    void releaseResources(void);
    void publishState(const char *state);
};

#endif // NV_GSP_WPR_META_STAGE_SERVICE_HPP
