//
//  OSObject.h -- minimal stand-in for libkern's OSObject / OSMetaClass.
//
//  NOT APPLE'S HEADER. See ../../IOKit/IOTypes.h for the full caveat.
//
//  The real OSDeclareDefaultStructors expands to a metaclass declaration plus
//  a gMetaClass reference. The important structural properties for a syntax
//  parse are: it declares a constructor/destructor pair, it is legal inside a
//  class body, and it requires a matching OSDefineMetaClassAndStructors in
//  exactly one translation unit. The stubs preserve all three -- so forgetting
//  the OSDefine, or naming a different class in it, is still an error here.
//

#ifndef NVSHIM_LIBKERN_OSOBJECT_H
#define NVSHIM_LIBKERN_OSOBJECT_H

#include <IOKit/IOTypes.h>

class OSMetaClassBase;
class OSMetaClass;

class OSObject {
public:
    virtual ~OSObject() {}
    virtual void retain() const {}
    virtual void release() const {}
    virtual bool init() { return true; }
    virtual void free() {}
    virtual const char *getMetaClassName() const { return "OSObject"; }
};

class OSString : public OSObject {
public:
    static OSString *withCString(const char *) { return nullptr; }
    const char *getCStringNoCopy() const { return nullptr; }
};

class OSNumber : public OSObject {
public:
    static OSNumber *withNumber(unsigned long long, unsigned int) { return nullptr; }
    unsigned long long unsigned64BitValue() const { return 0; }
};

class OSBoolean : public OSObject {
public:
    bool isTrue() const { return false; }
};

class OSDictionary : public OSObject {
public:
    static OSDictionary *withCapacity(unsigned int) { return nullptr; }
    bool setObject(const char *, OSObject *) { return true; }
    OSObject *getObject(const char *) const { return nullptr; }
};

//
// Added for Phase 5. NV3050Probe::copyGspFirmwareBlob() pulls the GSP blob out
// of the IORegistry as an OSData rather than reading a file from the kernel,
// so only the three members that call site uses are declared: the length, the
// read-only bytes, and the retaining accessor's return type.
//
// getBytesNoCopy() is const-returning on purpose. The blob is NVIDIA-signed
// firmware that this driver copies into its own staging buffer and must never
// edit in place; a stub handing back a writable pointer would let that mistake
// compile.
//
class OSData : public OSObject {
public:
    static OSData *withBytes(const void *bytes, unsigned int numBytes);
    unsigned int getLength() const;
    const void  *getBytesNoCopy() const;
};

extern OSBoolean *const kOSBooleanTrue;
extern OSBoolean *const kOSBooleanFalse;

// The real macro also emits gMetaClass plussed-out structors. Declaring the
// ctor/dtor is what makes a missing OSDefineMetaClassAndStructors a hard error,
// which is the property worth keeping.
#define OSDeclareDefaultStructors(className)                     \
    public:                                                      \
        className();                                             \
        virtual ~className();                                    \
    private:                                                     \
        static const OSMetaClass * const metaClass;

#define OSDeclareAbstractStructors(className) OSDeclareDefaultStructors(className)

#define OSDefineMetaClassAndStructors(className, superClass)     \
    const OSMetaClass * const className::metaClass = nullptr;    \
    className::className() {}                                    \
    className::~className() {}

// Real OSDynamicCast is a metaclass-checked downcast -- it does NOT use C++
// RTTI, because kexts are built with -fno-rtti. So the shim must not use
// dynamic_cast either: doing so produced "use of dynamic_cast requires -frtti"
// against code that is perfectly valid on macOS.
//
// static_cast is the right stand-in. It compiles under -fno-rtti, and it is
// still a hard error when the target type is unrelated to the source -- which
// is the mistake worth catching. What it cannot do is fail at runtime for a
// wrong-but-related type; that check only exists on real hardware.
template <class T, class S>
static inline T *NVShimMetaCast(S *inst)
{
    return static_cast<T *>(inst);
}

#define OSDynamicCast(type, inst)  NVShimMetaCast<type>(inst)
#define OSRequiredCast(type, inst) NVShimMetaCast<type>(inst)

#define OSSafeReleaseNULL(ptr)                                   \
    do { if (ptr) { (ptr)->release(); (ptr) = nullptr; } } while (0)

#endif // NVSHIM_LIBKERN_OSOBJECT_H
