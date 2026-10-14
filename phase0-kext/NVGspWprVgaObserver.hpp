// SPDX-License-Identifier: MIT
#ifndef NV_GSP_WPR_VGA_OBSERVER_HPP
#define NV_GSP_WPR_VGA_OBSERVER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>

#include "NVDecode.hpp"
#include "NVGspWprVga.hpp"

#define NV_GSP_WPR_VGA_OBSERVER_BOOT_ARG "nvgsp-wpr-vga-observe"

class NVGspWprVgaObserver : public IOService
{
    OSDeclareDefaultStructors(NVGspWprVgaObserver);

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

    void clearResources(void);
    void publishState(const char *state);
    UInt32 read32(UInt32 offset) const;
};

#endif // NV_GSP_WPR_VGA_OBSERVER_HPP
