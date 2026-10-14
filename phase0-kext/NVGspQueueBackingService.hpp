// SPDX-License-Identifier: MIT
#ifndef NV_GSP_QUEUE_BACKING_SERVICE_HPP
#define NV_GSP_QUEUE_BACKING_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

#include "NVGspQueueDma.hpp"
#include "NVGspQueueTx.hpp"

class NVGspQueueBackingService : public IOService
{
    OSDeclareDefaultStructors(NVGspQueueBackingService);

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

#endif // NV_GSP_QUEUE_BACKING_SERVICE_HPP
