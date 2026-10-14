// SPDX-License-Identifier: MIT
// Allocation-only rehearsal of GA107 GSP RMARGS + LibOS init-list memory.
#ifndef NV_GSP_BOOTARGS_REHEARSAL_SERVICE_HPP
#define NV_GSP_BOOTARGS_REHEARSAL_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

#include "NVGspQueueMemory.hpp"
#include "NVGspCachedArgs.hpp"
#include "NVGspLibosInit.hpp"

class NVGspBootArgsRehearsalService : public IOService
{
    OSDeclareDefaultStructors(NVGspBootArgsRehearsalService);

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCI;
    IOBufferMemoryDescriptor *fRmArgs;
    IOBufferMemoryDescriptor *fLibosList;
    IODMACommand *fRmArgsDMA;
    IODMACommand *fLibosDMA;
    bool fRmArgsBound;
    bool fLibosBound;

    void releaseResources(void);
    void publishState(const char *state);
};

#endif // NV_GSP_BOOTARGS_REHEARSAL_SERVICE_HPP
