//
//  IOMemoryDescriptor.h -- minimal stand-in.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//

#ifndef NVSHIM_IOKIT_IOMEMORYDESCRIPTOR_H
#define NVSHIM_IOKIT_IOMEMORYDESCRIPTOR_H

#include <IOKit/IOTypes.h>
#include <libkern/c++/OSObject.h>

class IOMemoryMap : public OSObject {
public:
    virtual IOVirtualAddress getVirtualAddress();
    virtual IOByteCount      getLength();
    virtual IOPhysicalAddress getPhysicalAddress();
};

class IOMemoryDescriptor : public OSObject {
public:
    virtual IOMemoryMap *map(IOOptionBits options = 0);
    virtual IOByteCount  getLength() const;
};

//
// IODeviceMemory is NOT declared here. Apple ships it in its own
// IOKit/IODeviceMemory.h, and this shim used to collapse the two -- which made
// a by-name include of that header a fatal file-not-found, and would equally
// have accepted a .cpp that omitted an include Apple requires. See
// IODeviceMemory.h for the whole argument.
//

#endif // NVSHIM_IOKIT_IOMEMORYDESCRIPTOR_H
