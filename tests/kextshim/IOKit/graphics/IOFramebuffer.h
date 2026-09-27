//
//  IOFramebuffer.h -- minimal stand-in for IOKit/graphics/IOFramebuffer.h.
//
//  NOT APPLE'S HEADER. See ../IOTypes.h for the full caveat, which applies here
//  in full and then some: IOFramebuffer's real surface is enormous (mode lists,
//  cursor handling, connection changes, power management, DDC/EDID) and this file
//  declares only the members IntelFbPresentProbe.cpp actually names.
//
//  WHY THIS FILE IS THE RISKIEST ONE IN THE SHIM
//  --------------------------------------------
//  handoff19 SS2 is the finding this file is most exposed to: matching Apple's
//  SYMBOL set is not matching Apple's INCLUDE GRAPH, and a shim FLATTER than
//  Apple's accepts a .cpp that OMITS an include Apple requires. That direction is
//  invisible here and first appears under Apple clang on the operator's build.
//
//  There is a second, sharper exposure that the NVIDIA and probe shells never
//  had. IOFramebuffer is an ABSTRACT class in Apple's SDK: a subclass that fails
//  to override a pure virtual does not compile there, and does not fail to compile
//  HERE unless this file marks the same members pure. So every member below that
//  Apple declares pure virtual is declared `= 0` here, deliberately, even though
//  a non-pure declaration would parse the subclass just as happily. A shim that
//  made them concrete would accept a subclass missing an override Apple demands --
//  the flat-include-graph defect one level up, at the vtable rather than the file
//  layout.
//
//  WHAT THIS CANNOT ESTABLISH
//  --------------------------
//  Nothing about whether the numbers handed to IOKit are RIGHT, nothing about
//  whether IOFramebufferOpen ever calls any of it, and nothing about the real
//  signatures. IODisplayModeID being SInt32 and IOIndex being SInt32 means a
//  swapped pair of arguments parses clean in both trees. STEP 7 on the guest is
//  what checks the signatures; this only checks that the file is well-formed.
//

#ifndef NVSHIM_IOKIT_GRAPHICS_IOFRAMEBUFFER_H
#define NVSHIM_IOKIT_GRAPHICS_IOFRAMEBUFFER_H

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>

//
//  IOGraphicsTypes.h in Apple's SDK. Folded in here rather than given its own
//  file because the subclass includes only IOFramebuffer.h and Apple's
//  IOFramebuffer.h includes IOGraphicsTypes.h itself -- so a .cpp that names
//  IOPixelInformation without including IOGraphicsTypes.h compiles in BOTH trees.
//  Splitting it would make this shim STRICTER than Apple's, which reddens correct
//  code, and that is the direction that wastes an operator build slot.
//
typedef SInt32 IODisplayModeID;
typedef UInt32 IOIndex;
typedef UInt32 IOPixelEncoding[32];

enum {
    kIOFBNoError            = 0
};

//
//  Apple's aperture selector. Only the base one is named because the subclass
//  refuses every other value rather than describing it.
//
enum {
    kIOFBSystemAperture     = 0
};

//
//  IOPixelInformation's componentMasks is 8 * 16 UInt32s in Apple's header. The
//  subclass writes only the first three, and the array bound is what makes a
//  fourth write a diagnosable overrun rather than a silent one.
//
struct IOPixelInformation {
    UInt32              bytesPerRow;
    UInt32              bytesPerPlane;
    UInt32              bitsPerPixel;
    UInt32              pixelType;
    UInt32              componentCount;
    UInt32              bitsPerComponent;
    UInt32              componentMasks[8 * 16];
    IOPixelEncoding     pixelFormat;
    UInt32              flags;
    UInt32              activeWidth;
    UInt32              activeHeight;
    UInt32              reserved[2];
};

struct IODisplayModeInformation {
    UInt32              nominalWidth;
    UInt32              nominalHeight;
    UInt32              refreshRate;
    UInt32              maxDepthIndex;
    UInt32              reserved[4];
};

enum {
    kIORGBDirectPixels      = 2
};

#define IO32BitDirectPixels  "--------RRRRRRRRGGGGGGGGBBBBBBBB"
#define IO30BitDirectPixels  "--RRRRRRRRRRGGGGGGGGGGBBBBBBBBBB"

class IOFramebuffer : public IOService {
    OSDeclareDefaultStructors(IOFramebuffer)

public:
    //
    //  PURE in Apple's header, and pure here for that reason. See the file
    //  comment: a concrete declaration would accept a subclass that forgot one.
    //
    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) = 0;
    virtual const char     *getPixelFormats(void) = 0;
    virtual IOItemCount     getDisplayModeCount(void) = 0;
    virtual IOReturn        getDisplayModes(IODisplayModeID *allDisplayModes) = 0;
    virtual IOReturn        getInformationForDisplayMode(IODisplayModeID displayMode,
                                                         IODisplayModeInformation *info) = 0;
    virtual UInt64          getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                          IOIndex depth) = 0;
    virtual IOReturn        getPixelInformation(IODisplayModeID displayMode,
                                                IOIndex depth,
                                                IOPixelAperture aperture,
                                                IOPixelInformation *pixelInfo) = 0;
    virtual IOReturn        getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                  IOIndex *depth) = 0;

    //
    //  Concrete in Apple's header. Declared here so an override of one is checked
    //  against a real base declaration rather than silently introducing a new
    //  method -- APPLE_KEXT_OVERRIDE expands to `override` in this shim, so drift
    //  in any of these is a compile error.
    //
    virtual bool            start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void            stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual bool            init(OSDictionary *dictionary = 0) APPLE_KEXT_OVERRIDE;
    virtual void            free(void) APPLE_KEXT_OVERRIDE;

    virtual IOReturn        setDisplayMode(IODisplayModeID displayMode, IOIndex depth);
    virtual IOReturn        enableController(void);
    virtual bool            isConsoleDevice(void);
    virtual IOReturn        setAttribute(IOSelect attribute, uintptr_t value);
    virtual IOReturn        getAttribute(IOSelect attribute, uintptr_t *value);
};

#endif // NVSHIM_IOKIT_GRAPHICS_IOFRAMEBUFFER_H
