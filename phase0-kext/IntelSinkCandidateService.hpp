// SPDX-License-Identifier: MIT
// Opt-in runtime owner for the still-unmapped Intel/3050 sink backing store.
#ifndef INTEL_SINK_CANDIDATE_SERVICE_HPP
#define INTEL_SINK_CANDIDATE_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "IntelGpuId.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelSinkCandidate.hpp"

// Allocation constraint only. Clearing the low twelve bits requests page
// alignment; no peer-DMA magnitude claim is made from this mask.
#define IG_SINK_CANDIDATE_PHYS_MASK 0xFFFFFFFFFFFFF000ull
#define IG_SINK_CANDIDATE_DISPLAY_WAIT_NS 1000000000ull

class IntelSinkCandidateService : public IOService
{
    OSDeclareDefaultStructors(IntelSinkCandidateService)
    typedef IOService super;

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCIDevice;
    IOBufferMemoryDescriptor *fBacking;
    bool fPrepared;

    void releaseBacking(void);
    void publishNotReady(void);
};

#endif // INTEL_SINK_CANDIDATE_SERVICE_HPP
