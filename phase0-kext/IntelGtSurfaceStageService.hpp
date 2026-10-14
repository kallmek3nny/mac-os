// SPDX-License-Identifier: MIT
#ifndef IntelGtSurfaceStageService_hpp
#define IntelGtSurfaceStageService_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

class IntelGtSurfaceStageService : public IOService
{
    OSDeclareDefaultStructors(IntelGtSurfaceStageService);

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

    bool acceleratorPresent(void);
    void releaseResources(void);
    void publishState(const char *state);
};

#endif // IntelGtSurfaceStageService_hpp
