//
//  IOMapper.h -- minimal stand-in for IOKit/IOMapper.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for t-fccff1. Nine phase0-kext sources include this header by name,
//  and until it existed each of them stopped at a fatal file-not-found before
//  clang read any driver code.
//
//  Transcribed from xnu iokit/IOKit/IOMapper.h (apple-oss-distributions, main):
//
//      class IOMapper : public IOService
//      static OSPtr<IOMapper>  copyMapperForDevice(IOService * device);
//
//  The shim spells the return type as IOMapper*. In a kext build OSPtr<T> is a
//  plain T* unless IOKIT_ENABLE_SHARED_PTR is defined. That mapping comes from
//  libkern/c++/OSPtr.h, which was NOT fetched for this file, so re-check it
//  with the SIGNATURES.md recipe on a macOS host. The caller owns the returned
//  reference and must release it.
//
//  The base class is IOService, as in Apple's header. Passing an IOPCIDevice*
//  where an IOMapper* belongs (IODMACommand::withSpecification arg 7,
//  NVNEG_DMA_VOID_MAPPER) stays a hard error now that the class is complete.
//
//  NARROWER: copyMapperForDeviceWithIndex and the rest of the class are not
//  declared. copyMapperForDevice is the only IOMapper:: member phase0-kext
//  names.
//
//  Apple's header includes IOService.h, IOMemoryDescriptor.h and
//  IODMACommand.h, so this one does too. That keeps the include graph the same
//  shape as Apple's.
//

#ifndef NVSHIM_IOKIT_IOMAPPER_H
#define NVSHIM_IOKIT_IOMAPPER_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

class IOMapper : public IOService {
public:
    static IOMapper *copyMapperForDevice(IOService *device);
};

#endif // NVSHIM_IOKIT_IOMAPPER_H
