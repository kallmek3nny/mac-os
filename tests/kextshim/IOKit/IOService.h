//
//  IOService.h -- minimal stand-in for IOKit/IOService.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  The five virtuals below (init/free/probe/start/stop) carry the exact
//  signatures a driver must override. Because APPLE_KEXT_OVERRIDE expands to
//  `override` in this shim, any drift in the driver's declarations -- a missing
//  const, SInt32* vs int*, bool vs IOReturn -- becomes a compile error rather
//  than a silently-never-called method at runtime. That is a real macOS bug
//  class this catches on Windows.
//

#ifndef NVSHIM_IOKIT_IOSERVICE_H
#define NVSHIM_IOKIT_IOSERVICE_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <IOKit/IORegistryEntry.h>

//
// class IORegistryEntry moved to IORegistryEntry.h for t-fccff1. Apple's
// IOService.h includes that header rather than declaring the class, and three
// phase0-kext sources include IORegistryEntry.h by name.
//

class IOUserClient;

//
// Added for t-fccff1. Transcribed from
// step7-results/apple-headers/IOKit/IOService.h:
//
//     extern const IORegistryPlane *  gIOServicePlane;
//
// IntelGtBringup, IntelGtSurfaceStageService and IntelSinkCandidateService
// pass it to IORegistryIterator::iterateOver().
//
extern const IORegistryPlane *gIOServicePlane;

class IOService : public IORegistryEntry {
public:
    // Apple's IOService really does hide OSObject::init(void) with the
    // dictionary form. Pull the base overload back in so -Woverloaded-virtual
    // stays quiet about the shim and only fires on the driver's own code.
    using OSObject::init;

    virtual bool       init(OSDictionary *dictionary = nullptr);
    virtual void       free(void);
    virtual IOService *probe(IOService *provider, SInt32 *score);
    virtual bool       start(IOService *provider);
    virtual void       stop(IOService *provider);

    virtual void       registerService(IOOptionBits options = 0);
    virtual bool       attach(IOService *provider);
    virtual void       detach(IOService *provider);

    //
    // Added for t-fccff1. Transcribed from
    // step7-results/apple-headers/IOKit/IOService.h:
    //
    //     virtual IOReturn newUserClient( task_t owningTask, void * securityID,
    //         UInt32 type, OSDictionary * properties,
    //         LIBKERN_RETURNS_RETAINED IOUserClient ** handler );
    //
    // IntelGtBringup overrides this form. LIBKERN_RETURNS_RETAINED is an
    // ownership annotation, not part of the signature, and is left out.
    // NARROWER: the 4-argument form and the two OSSharedPtr<IOUserClient>&
    // forms are not declared; phase0-kext overrides only this one.
    //
    virtual IOReturn   newUserClient(task_t owningTask, void *securityID,
                                     UInt32 type, OSDictionary *properties,
                                     IOUserClient **handler);
};

#endif // NVSHIM_IOKIT_IOSERVICE_H
