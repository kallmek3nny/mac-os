//
//  IOTypes.h -- minimal stand-in for Apple's IOKit type header.
//
//  THIS IS NOT APPLE'S HEADER AND IS NOT A SUBSTITUTE FOR ONE.
//
//  Purpose: let clang parse phase0-kext/*.cpp with -fsyntax-only on a machine
//  with no macOS SDK. It catches the class of error that neither
//  check-registers.py nor tests/ can see: typos in member names, wrong
//  argument counts, missing declarations, unbalanced braces, bad overrides.
//
//  It CANNOT catch: wrong Apple semantics, real KPI signature drift, linkage
//  errors, or anything about whether the kext loads. STEP 7 in ../../STEPS.md
//  is still required. A green run here means "structurally plausible", not
//  "builds on macOS".
//
//  Signatures below were transcribed to match the *uses* in NV3050Probe.cpp.
//  Where Apple's real signature is wider (default args, extra overloads), the
//  stub is deliberately narrower -- a stub that is too permissive would hide
//  the very mistakes this exists to find.
//

#ifndef NVSHIM_IOKIT_IOTYPES_H
#define NVSHIM_IOKIT_IOTYPES_H

typedef signed char        SInt8;
typedef signed short       SInt16;
typedef signed int         SInt32;
typedef signed long long   SInt64;

typedef unsigned char      UInt8;
typedef unsigned short     UInt16;
typedef unsigned int       UInt32;
typedef unsigned long long UInt64;

typedef int                boolean_t;
typedef int                kern_return_t;
typedef kern_return_t      IOReturn;
typedef unsigned long long IOByteCount;
typedef unsigned long long IOPhysicalAddress;
typedef unsigned long long IOPhysicalLength;
typedef unsigned long long IOVirtualAddress;
typedef unsigned long long mach_vm_address_t;
typedef unsigned long long IOOptionBits;
typedef unsigned long long AbsoluteTime;

//
// Apple gets these from mach headers, not IOTypes.h. They live here because
// Phase 5's IOBufferMemoryDescriptor::inTaskWithPhysicalMask() call needs
// kernel_task, and there is no mach shim to put them in. `IODirection` is a
// distinct typedef rather than an alias of IOOptionBits so that passing a map
// option where a direction belongs is still a diagnosable mistake.
//
typedef struct NVShimTask *task_t;
extern task_t kernel_task;

typedef unsigned long long mach_vm_address_t;
typedef unsigned long long mach_vm_size_t;
typedef unsigned long long vm_size_t;

// Added for t-fccff1. Seven phase0-kext .cpp files cast an alignment to
// vm_offset_t at their IOBufferMemoryDescriptor::inTaskWithOptions() call:
// IntelGtBringupSurface, IntelGtSurfaceStageService, NVGspQueueBackingService,
// NVGspRmImageStageService, NVGspWprBootUcodeStageService,
// NVGspWprFactsObserver and NVGspWprMetaStageService. Apple's headers use the
// type (step7-results/apple-headers/IOKit/IOLib.h:131, IOMallocAligned, and
// IOBufferMemoryDescriptor.h:79/91/142/154/178/204) but define it in a mach
// vm_types.h that step7-results/apple-headers does not carry, so no Apple
// declaration is quoted here. It gets the same width as vm_size_t above, which
// is all those call sites rely on. Like the rest of this block it comes from a
// mach header on macOS, not IOTypes.h: FLATTER.
typedef unsigned long long vm_offset_t;

typedef unsigned int IODirection;

//
// Added 2026-08-20 for IOKit/graphics/IOFramebuffer.h. Apple declares all four
// of these outside IOFramebuffer.h -- IOItemCount and IOSelect in IOTypes.h,
// IOPixelAperture in IOGraphicsTypes.h, uintptr_t from the C library -- so a
// .cpp that names them without including IOTypes.h compiles in both trees.
// Putting them HERE rather than in the graphics header keeps the shim's include
// graph the same shape as Apple's, which is the property handoff19 SS2 says a
// flatter shim quietly destroys.
//
// IOPixelAperture is a DISTINCT typedef rather than an alias of IOIndex for the
// same reason IODirection above is distinct from IOOptionBits: passing an
// aperture selector where a depth index belongs stays a diagnosable mistake.
//
typedef unsigned int       IOItemCount;
typedef unsigned int       IOSelect;
typedef unsigned int       IOPixelAperture;
typedef unsigned long long uintptr_t;

#define kIOReturnSuccess     0
#define kIOReturnError       0x2bc
#define kIOReturnNoMemory    0x2bd
#define kIOReturnNoDevice    0x2c0
#define kIOReturnUnsupported 0x2c7

// Added 2026-08-20 for IntelFbPresentProbe.cpp, which refuses a display mode,
// depth index or aperture selector it does not describe. Apple's value is
// 0x2c2 (iokit_common_err(0x2c2)); the exact number is not load-bearing here --
// what matters is that the name RESOLVES, because a .cpp naming an undeclared
// kIOReturn constant is precisely the typo class this shim exists to catch, and
// leaving it out would have reddened the gate for the right reason at the wrong
// place.
#define kIOReturnBadArgument 0x2c2

// Added for t-fccff1, same convention as kIOReturnBadArgument above: the bare
// low code, not the full iokit_common_err() value, because only the name has to
// resolve. The t-fccff1 probe found these undeclared in IntelGtFill.cpp,
// IntelGtPersistentSurfaces.cpp and IntelGtBringup.cpp. Apple's lines, from
// step7-results/apple-headers/IOKit/IOReturn.h:
//   112: #define kIOReturnIOError         iokit_common_err(0x2ca) // General I/O error
//   126: #define kIOReturnNotReady        iokit_common_err(0x2d8) // not ready
//   138: #define kIOReturnNotPermitted    iokit_common_err(0x2e2) // not permitted
#define kIOReturnIOError      0x2ca
#define kIOReturnNotReady     0x2d8
#define kIOReturnNotPermitted 0x2e2

#define kIOMapInhibitCache         0x100
#define kIOMapAnywhere             0x1
#define kIOMapDefaultCache         0x0
#define kIOMapCopybackCache        0x200
#define kIOMapWriteThruCache       0x400

// Apple defines this to __attribute__((annotate("apple_kext_override")))
// under -fapple-kext, and to `override` otherwise. `override` is the part
// that actually diagnoses a mismatched virtual, so use it.
#ifndef APPLE_KEXT_OVERRIDE
#define APPLE_KEXT_OVERRIDE override
#endif

#endif // NVSHIM_IOKIT_IOTYPES_H
