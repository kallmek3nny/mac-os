//
//  IntelSinkAlloc.hpp
//
//  Still-open half of step (a) of the sink plan (intel-handoff40 §7): choose
//  where in the GGTT the 3050 sink surface lives, before any page is mapped.
//  Pure logic, no MMIO: given the GGTT size, the first byte past everything
//  already in use (firmware plane, stolen-memory users), and the sink
//  geometry, produce the base, stride and byte size to allocate.
//
//  The plan is meant to be handed straight to igSinkSurfaceRefused() in
//  IntelSinkSurface.hpp; it is built to satisfy that validator's rules:
//  base aligned to IG_SINK_ALLOC_BASE_ALIGN, stride a multiple of 64, the
//  whole extent inside 32 bits and inside the GGTT.
//
//  Refusals are checked in a pinned order so a doubly-bad input always reports
//  the same reason, and `out` is untouched on every refusal.
//

#ifndef INTEL_SINK_ALLOC_HPP
#define INTEL_SINK_ALLOC_HPP

//  No angle-bracket include: section 1 of check-kext-syntax.bat parses each
//  header standalone with no system include path. The width-explicit IGS32 /
//  IGS64 typedefs come from IntelScanout.hpp, as they do for the sibling
//  IntelSinkSurface.hpp this plan is handed to.
#include "IntelScanout.hpp"

// Linear scanout surfaces on display version >= 9 want 256 KiB base alignment
// (the i915 linear alignment); a looser base only trips the validator later.
#define IG_SINK_ALLOC_BASE_ALIGN   0x40000u
#define IG_SINK_ALLOC_STRIDE_ALIGN 64u

// The sink is XRGB8888, the format the firmware plane already scans out.
#define IG_SINK_ALLOC_BPP          32u

enum IgSinkAllocResult {
    IG_SINK_ALLOC_OK = 0,
    IG_SINK_ALLOC_NULL_OUT,         // nowhere to write the plan
    IG_SINK_ALLOC_ZERO_GEOMETRY,    // width or height is zero
    IG_SINK_ALLOC_BAD_BPP,          // anything but 32 bpp
    IG_SINK_ALLOC_RESERVED_PAST,    // reserved end already beyond the GGTT
    IG_SINK_ALLOC_EXTENT_OVERFLOW,  // base + size does not fit in 32 bits
    IG_SINK_ALLOC_NO_ROOM,          // the surface would run off the GGTT
};

struct IgSinkAllocPlan {
    IGS32 base;    // GGTT offset of the first byte
    IGS32 stride;  // bytes per row, multiple of 64
    IGS32 size;    // stride * height, bytes to map
};

static inline IGS64 igSinkAllocAlignUp(IGS64 v, IGS64 align)
{
    return (v + align - 1) / align * align;
}

static inline IgSinkAllocResult igSinkAllocPlan(IGS64 ggttSize,
                                                IGS64 reservedEnd,
                                                IGS32 width,
                                                IGS32 height,
                                                IGS32 bpp,
                                                IgSinkAllocPlan *out)
{
    if (out == 0)
        return IG_SINK_ALLOC_NULL_OUT;
    if (width == 0 || height == 0)
        return IG_SINK_ALLOC_ZERO_GEOMETRY;
    if (bpp != IG_SINK_ALLOC_BPP)
        return IG_SINK_ALLOC_BAD_BPP;
    if (reservedEnd > ggttSize)
        return IG_SINK_ALLOC_RESERVED_PAST;

    // All arithmetic in 64 bits: width * 4 * height alone can pass 2^32.
    const IGS64 stride = igSinkAllocAlignUp((IGS64)width * (bpp / 8),
                                               IG_SINK_ALLOC_STRIDE_ALIGN);
    const IGS64 size = stride * height;
    const IGS64 base = igSinkAllocAlignUp(reservedEnd,
                                             IG_SINK_ALLOC_BASE_ALIGN);

    if (base + size > 0x100000000ull)
        return IG_SINK_ALLOC_EXTENT_OVERFLOW;
    if (base + size > ggttSize)
        return IG_SINK_ALLOC_NO_ROOM;

    out->base = (IGS32)base;
    out->stride = (IGS32)stride;
    out->size = (IGS32)size;
    return IG_SINK_ALLOC_OK;
}

#endif // INTEL_SINK_ALLOC_HPP