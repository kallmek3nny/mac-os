// SPDX-License-Identifier: MIT
// Read-only capability probe for the RTX 3050 Mobile's device-specific IOMMU mapper.
#ifndef NV_PEER_MAPPER_PROBE_HPP
#define NV_PEER_MAPPER_PROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "NVAmpereCopyPublicationRuntime.hpp"

class NVPeerMapperProbe : public IOService
{
    OSDeclareDefaultStructors(NVPeerMapperProbe)
    typedef IOService super;

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCIDevice;

    // Provider-owned publication state. The exact-device mapper result is
    // recorded here by start(); future channel execution can extend the same
    // state without exporting raw mapper/channel internals through IORegistry.
    NVAmpereCopyPublicationRuntime fCopyPublicationRuntime;
};

#endif // NV_PEER_MAPPER_PROBE_HPP
