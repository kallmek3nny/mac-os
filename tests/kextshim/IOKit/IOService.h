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

class IORegistryEntry : public OSObject {
public:
    virtual const char *getName() const;

    virtual bool setProperty(const char *key, OSObject *value);
    virtual bool setProperty(const char *key, const char *value);
    virtual bool setProperty(const char *key, bool value);
    virtual bool setProperty(const char *key, unsigned long long value,
                             unsigned int numberOfBits);

    //
    // The bytes+length form, which Apple's IORegistryEntry really does declare
    // and this shim did not until 2026-08-26. Its absence was found by
    // IntelGtProbe.cpp -- the first file in the tree to publish a BLOB rather
    // than a scalar -- and the direction of the omission is the dangerous one
    // this shim exists to avoid: a shim FLATTER or NARROWER than Apple's accepts
    // less than Apple does, so the failure appears on the operator's one build
    // attempt rather than here. Same class as handoff19 section 2's missing
    // IODeviceMemory header, one level down from a file layout to an overload.
    //
    // `length` is unsigned int rather than IOByteCount deliberately: Apple's
    // signature is unsigned int, and a wider parameter here would accept a call
    // that does not compile on macOS.
    //
    virtual bool setProperty(const char *key, void *bytes,
                             unsigned int length);

    //
    // Apple's getProperty() returns a BORROWED reference and copyProperty()
    // returns a RETAINED one. Both are declared because confusing the two is a
    // refcount bug that no amount of testing on Windows can catch at runtime --
    // having both present at least makes the call site say which it meant.
    //
    virtual OSObject *getProperty(const char *key) const;
    virtual OSObject *copyProperty(const char *key) const;
};

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
};

#endif // NVSHIM_IOKIT_IOSERVICE_H
