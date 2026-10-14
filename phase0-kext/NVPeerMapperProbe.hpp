// SPDX-License-Identifier: MIT
// Read-only capability probe for the RTX 3050 Mobile's device-specific IOMMU mapper.
#ifndef NV_PEER_MAPPER_PROBE_HPP
#define NV_PEER_MAPPER_PROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

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
};

#endif // NV_PEER_MAPPER_PROBE_HPP
