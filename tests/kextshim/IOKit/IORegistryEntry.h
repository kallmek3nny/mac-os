//
//  IORegistryEntry.h -- minimal stand-in for IOKit/IORegistryEntry.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for t-fccff1. IntelGtBringup.cpp, IntelSinkCandidateService.cpp and
//  NVGspBootArgsRehearsalService.cpp include this header by name, so each of
//  them stopped at a fatal file-not-found before clang read any driver code.
//
//  class IORegistryEntry used to be declared in IOService.h. Apple declares it
//  here and IOService.h includes this file, so the shim now does the same. A
//  .cpp that includes only this header sees IORegistryEntry but not IOService,
//  as it would on macOS. The existing members and their comments moved
//  unchanged; removeProperty() is new.
//
//  Transcribed from step7-results/apple-headers/IOKit/IORegistryEntry.h:
//
//      class IORegistryEntry : public OSObject
//      enum { kIORegistryIterateRecursively   = 0x00000001,
//             kIORegistryIterateParents       = 0x00000002, };
//      virtual void removeProperty( const char * aKey);
//      class IORegistryIterator : public OSIterator
//      static OSPtr<IORegistryIterator> iterateOver( IORegistryEntry * start,
//          const IORegistryPlane * plane, IOOptionBits options = 0 );
//      virtual IORegistryEntry * getNextObject( void ) APPLE_KEXT_OVERRIDE;
//      virtual void reset( void ) APPLE_KEXT_OVERRIDE;
//      virtual bool isValid( void ) APPLE_KEXT_OVERRIDE;
//
//  OSPtr<IORegistryIterator> is spelled IORegistryIterator* for the same
//  reason, and on the same unverified OSPtr.h assumption, as IOMapper.h's
//  copyMapperForDevice. The caller owns the returned iterator and must release
//  it, which all three phase0-kext call sites do.
//
//  NARROWER: the OSSymbol* and OSString* removeProperty overloads, the
//  plane-only iterateOver overload, getNextObjectFlat, iterateAll and the rest
//  of both classes are not declared. phase0-kext calls iterateOver only with a
//  start entry.
//

#ifndef NVSHIM_IOKIT_IOREGISTRYENTRY_H
#define NVSHIM_IOKIT_IOREGISTRYENTRY_H

#include <IOKit/IOTypes.h>
#include <libkern/c++/OSObject.h>

class IORegistryPlane;
class IORegistryIterator;

enum {
    kIORegistryIterateRecursively = 0x00000001,
    kIORegistryIterateParents     = 0x00000002,
};

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

    //
    // Added for t-fccff1. NVGspQueueObserver.cpp is the first source the gate
    // reaches that calls it; phase0-kext has 56 call sites, every one passing
    // a string literal, so only Apple's const char * overload is declared.
    //
    virtual void removeProperty(const char *aKey);
};

//
// OSIterator lives in libkern/c++/OSIterator.h on macOS, reached through
// OSContainers.h. There is no shim for that header yet, so the three pure
// virtuals IORegistryIterator overrides are declared here, behind the guard a
// future libkern/c++/OSIterator.h shim would use. These three signatures were
// NOT fetched for this file; re-check them with the SIGNATURES.md recipe:
//
//     virtual void       reset() = 0;
//     virtual bool       isValid() = 0;
//     virtual OSObject * getNextObject() = 0;
//
// Declaring the base is what gives APPLE_KEXT_OVERRIDE on the overrides below
// its meaning: a drifted signature becomes a compile error, not a new virtual.
//
#ifndef NVSHIM_LIBKERN_OSITERATOR_H
#define NVSHIM_LIBKERN_OSITERATOR_H
class OSIterator : public OSObject {
public:
    virtual void      reset() = 0;
    virtual bool      isValid() = 0;
    virtual OSObject *getNextObject() = 0;
};
#endif // NVSHIM_LIBKERN_OSITERATOR_H

class IORegistryIterator : public OSIterator {
public:
    static IORegistryIterator *iterateOver(IORegistryEntry *start,
                                           const IORegistryPlane *plane,
                                           IOOptionBits options = 0);

    // Covariant with OSIterator::getNextObject(), as in Apple's header.
    virtual IORegistryEntry *getNextObject(void) APPLE_KEXT_OVERRIDE;
    virtual void             reset(void) APPLE_KEXT_OVERRIDE;
    virtual bool             isValid(void) APPLE_KEXT_OVERRIDE;
};

#endif // NVSHIM_IOKIT_IOREGISTRYENTRY_H
