//
//  IODeviceMemory.h -- minimal stand-in for IOKit/IODeviceMemory.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  WHY THIS IS ITS OWN FILE
//  -----------------------
//  `IODeviceMemory` used to be declared inside this shim's
//  IOMemoryDescriptor.h. That parsed NV3050Probe.cpp clean for months, because
//  that shell reaches the class transitively through IOPCIDevice.h and never
//  names the header.
//
//  Apple puts the class HERE, in its own header, and IntelDisplayProbe.hpp
//  includes it by name -- which is the correct thing for a translation unit to
//  do rather than leaning on another header's include graph. Against the old
//  shim that include was a fatal `file not found`: a red gate caused entirely
//  by the shim's layout, on a shell whose own include list was right.
//
//  The same divergence runs the other way and is the dangerous direction. A
//  shim that collapses two Apple headers into one accepts a .cpp that omits an
//  include Apple requires, and the omission first appears under Apple clang on
//  the operator's single build attempt. Splitting the file keeps this shim's
//  include GRAPH the shape Apple's is, not merely its symbol set.
//

#ifndef NVSHIM_IOKIT_IODEVICEMEMORY_H
#define NVSHIM_IOKIT_IODEVICEMEMORY_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOMemoryDescriptor.h>

class IODeviceMemory : public IOMemoryDescriptor {
public:
    virtual IOPhysicalAddress getPhysicalAddress();
};

#endif // NVSHIM_IOKIT_IODEVICEMEMORY_H
