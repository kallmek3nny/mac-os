//
//  IOLib.h -- minimal stand-in for IOKit/IOLib.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//

#ifndef NVSHIM_IOKIT_IOLIB_H
#define NVSHIM_IOKIT_IOLIB_H

#include <IOKit/IOTypes.h>

// printf-format checking is the single most valuable thing this shim buys:
// every NVLOG call in NV3050Probe.cpp passes register values through casts,
// and a %llx paired with a UInt32 (or vice versa) is exactly the silent bug
// this project keeps hitting. The format attribute makes clang diagnose it.
extern "C" void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

extern "C" void IOSleep(unsigned int milliseconds);

//
// Apple's real IOKit/IOLib.h includes <string.h>, so a kext that includes
// IOLib.h gets memcpy/memset/bzero without naming them. Declaring them here
// rather than shipping a shim <string.h> keeps the include graph identical to
// the driver's: NV3050Probe.cpp includes only IOKit headers, exactly as it
// will when Apple's toolchain compiles it.
//
// Signatures are the C library ones. Phase 5's loadGspFirmware() zero-fills the
// DMA staging buffer and copies the bootloader region into it, and a wrong
// argument order there would hand the GPU uninitialised kernel memory -- which
// is precisely the class of mistake this shim exists to diagnose.
//
extern "C" void *memset(void *dst, int c, unsigned long long len);
extern "C" void *memcpy(void *dst, const void *src, unsigned long long len);

// Added 2026-08-20 for IntelFbPresentProbe.cpp, which zeroes IOKit's two
// description structs before filling the fields it can answer, and copies a
// fixed-width pixel-format string into IOPixelEncoding.
//
// bzero comes from <string.h> in Apple's kernel headers and strlcpy from
// <libkern/libkern.h> -- but IOLib.h pulls both in transitively there, so a .cpp
// that names them having included only IOLib.h compiles in BOTH trees. That is
// the include-graph shape handoff19 SS2 says a flatter shim quietly destroys, and
// putting them here rather than in libkern/libkern.h is what keeps it.
//
// strlcpy returns the length it TRIED to write, which is how truncation is
// detected. The subject does not check it today because IOPixelEncoding is 32
// bytes and the format literal is exactly 32 characters plus a terminator -- but
// the return type is declared honestly rather than as void, so a caller that
// wants the check can have it.
extern "C" void bzero(void *dst, unsigned long long len);
extern "C" unsigned long strlcpy(char *dst, const char *src, unsigned long len);
extern "C" void IODelay(unsigned int microseconds);

// Added for t-fccff1. NVGspWprBootUcodeStageService.cpp:183 and
// NVGspWprMetaStageService.cpp:270 stage firmware bytes with bcopy, reaching it
// only through IOLib.h. In Apple's tree that works because IOLib.h includes
// <libkern/OSAtomic.h>, which includes <string.h> (osfmk/libsa/string.h) -- so,
// as with bzero above, it is declared here to keep the include-graph shape.
//
// Apple: extern void bcopy(const void *src __sized_by(n), void *dst __sized_by(n), size_t n);
// SOURCE FIRST, DESTINATION SECOND -- the reverse of memcpy. The const on src is
// kept so a const source passed in the destination slot fails to compile here
// exactly as it would against Apple's header. The length is unsigned long long
// for the same reason memset's is (size_t on the probe target). Apple's
// __sized_by annotations and its always_inline overload that forwards to
// memmove are omitted: they change bounds checking, not the signature.
extern "C" void bcopy(const void *src, void *dst, unsigned long long len);

// Added for t-fccff1. IntelSinkCandidateService.cpp:97/99/120 fence MMIO with
// OSSynchronizeIO(). Apple defines it in <libkern/OSAtomic.h> (which IOLib.h
// includes), inside extern "C", split only by architecture -- there is no
// KERNEL / KERNEL_PRIVATE guard:
//   arm/arm64:  extern void OSSynchronizeIO(void);
//   otherwise:  static __inline__ void OSSynchronizeIO(void) { }
// An x86_64 kext gets the empty inline, so on this target it is NOT a barrier.
// The shim mirrors that rather than inventing one; any ordering the Intel path
// actually needs must come from somewhere else, and this stub will not hide it.
#if defined(__arm__) || defined(__arm64__)
extern "C" void OSSynchronizeIO(void);
#else
static inline void OSSynchronizeIO(void)
{
}
#endif

extern "C" void *IOMalloc(unsigned long size);
extern "C" void  IOFree(void *address, unsigned long size);

// Byte-order accessors. Real ones are macros over OSReadLittleInt32; the
// signature is what matters here (base pointer + byte offset).
static inline UInt32 OSReadLittleInt32(const volatile void *base, UInt64 offset)
{
    return *(const volatile UInt32 *)((const volatile UInt8 *)base + offset);
}

static inline UInt16 OSReadLittleInt16(const volatile void *base, UInt64 offset)
{
    return *(const volatile UInt16 *)((const volatile UInt8 *)base + offset);
}

static inline void OSWriteLittleInt32(volatile void *base, UInt64 offset, UInt32 value)
{
    *(volatile UInt32 *)((volatile UInt8 *)base + offset) = value;
}

#endif // NVSHIM_IOKIT_IOLIB_H
