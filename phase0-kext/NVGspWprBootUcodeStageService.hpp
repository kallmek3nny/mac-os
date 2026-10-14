// SPDX-License-Identifier: MIT
#ifndef NV_GSP_WPR_BOOT_UCODE_STAGE_SERVICE_HPP
#define NV_GSP_WPR_BOOT_UCODE_STAGE_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/c++/OSData.h>

#include "NVGspWprBootUcode.hpp"
#include "NVGspWprBootDma.hpp"

#define NV_GSP_WPR_BOOT_STAGE_BOOT_ARG "nvgsp-wpr-boot-ucode-stage"
#define NV_GSP_WPR_BOOT_STAGE_PROPERTY "NVGspFirmware"

class NVGspWprBootUcodeStageService : public IOService
{
    OSDeclareDefaultStructors(NVGspWprBootUcodeStageService);

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCI;
    OSData *fFirmware;
    IOBufferMemoryDescriptor *fBacking;
    IODMACommand *fDMA;
    bool fDMABound;

    OSData *copyBootFirmwareBlob(void);
    void releaseResources(void);
    void publishState(const char *state);
};

#endif // NV_GSP_WPR_BOOT_UCODE_STAGE_SERVICE_HPP
