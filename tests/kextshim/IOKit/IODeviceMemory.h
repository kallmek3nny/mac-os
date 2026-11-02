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

    // Apple (xnu iokit/IOKit/IODeviceMemory.h):
    //     static OSPtr<IODeviceMemory> withSubRange(
    //         IODeviceMemory *    of,
    //         IOPhysicalAddress   offset,
    //         IOPhysicalLength    length );
    // OSPtr<T> is a plain T* in a kext build unless IOKIT_ENABLE_SHARED_PTR is
    // defined. That mapping comes from libkern/c++/OSPtr.h, which was NOT
    // fetched for this file, so re-check it with the SIGNATURES.md recipe on a
    // macOS host. Returns the range `length` bytes long starting `offset` bytes
    // into `of`; the CALLER owns the returned reference and must release it.
    // IntelFbPresentProbe::sliceScanout() is the user: the scanout slice of
    // GMADR that getApertureRange(kIOFBSystemAperture) hands IOKit.
    // NARROWER: withRange() and arrayFromList() are left out until a
    // phase0-kext source needs them.
    static IODeviceMemory *withSubRange(IODeviceMemory *of,
                                        IOPhysicalAddress offset,
                                        IOPhysicalLength length);
};

#endif // NVSHIM_IOKIT_IODEVICEMEMORY_H
