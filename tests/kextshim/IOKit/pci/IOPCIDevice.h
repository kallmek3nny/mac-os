//
//  IOPCIDevice.h -- minimal stand-in for IOKit/pci/IOPCIDevice.h.
//
//  NOT APPLE'S HEADER. See ../IOTypes.h for the full caveat.
//
//  Config-space offset constants match IOPCIFamily's published values.
//

#ifndef NVSHIM_IOKIT_PCI_IOPCIDEVICE_H
#define NVSHIM_IOKIT_PCI_IOPCIDEVICE_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
// Apple's own IOPCIDevice.h pulls in IODeviceMemory.h, and it has to:
// getDeviceMemoryWithIndex() below returns IODeviceMemory*, so a shim that
// merely forward-declared it would let a .cpp CALL the method and never touch
// the returned object's members. That is how NV3050Probe.cpp reaches the class
// without naming the header.
#include <IOKit/IODeviceMemory.h>

enum {
    kIOPCIConfigVendorID            = 0x00,
    kIOPCIConfigDeviceID            = 0x02,
    kIOPCIConfigCommand             = 0x04,
    kIOPCIConfigStatus              = 0x06,
    kIOPCIConfigRevisionID          = 0x08,
    kIOPCIConfigClassCode           = 0x09,
    kIOPCIConfigCacheLineSize       = 0x0C,
    kIOPCIConfigBaseAddress0        = 0x10,
    kIOPCIConfigBaseAddress1        = 0x14,
    kIOPCIConfigBaseAddress2        = 0x18,
    kIOPCIConfigSubSystemVendorID   = 0x2C,
    kIOPCIConfigSubSystemID         = 0x2E,
    kIOPCIConfigExpansionROMBase    = 0x30,
    kIOPCIConfigCapabilitiesPtr     = 0x34,
    kIOPCIConfigInterruptLine       = 0x3C,
    kIOPCIConfigInterruptPin        = 0x3D,
};

enum {
    kPCIPMCSPowerStateD0 = 0,
    kPCIPMCSPowerStateD1 = 1,
    kPCIPMCSPowerStateD2 = 2,
    kPCIPMCSPowerStateD3 = 3,
};

class IOPCIDevice : public IOService {
public:
    virtual UInt8  configRead8(UInt8 offset);
    virtual UInt16 configRead16(UInt8 offset);
    virtual UInt32 configRead32(UInt8 offset);

    virtual void   configWrite8(UInt8 offset, UInt8 data);
    virtual void   configWrite16(UInt8 offset, UInt16 data);
    virtual void   configWrite32(UInt8 offset, UInt32 data);

    virtual bool   setMemoryEnable(bool enable);
    virtual bool   setBusMasterEnable(bool enable);
    virtual bool   setIOEnable(bool enable, bool exclusive = false);

    virtual IOReturn enablePCIPowerManagement(IOOptionBits state);

    virtual IODeviceMemory *getDeviceMemoryWithIndex(unsigned int index);

    // Apple: "virtual IODeviceMemory * getDeviceMemoryWithRegister( UInt8 reg );"
    // (step7-results/apple-headers/IOKit/pci/IOPCIDevice.h:612). Looks a BAR up
    // by its config-space register (kIOPCIConfigBaseAddress0..5), not by its
    // position in the device-memory list. The provider retains the result; the
    // caller must not release it.
    virtual IODeviceMemory *getDeviceMemoryWithRegister(UInt8 reg);
};

#endif // NVSHIM_IOKIT_PCI_IOPCIDEVICE_H
