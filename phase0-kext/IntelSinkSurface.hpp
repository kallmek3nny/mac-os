//
//  IntelSinkSurface.hpp
//
//  Pure-logic validation of a scanout surface the driver OWNS -- the sink
//  the 3050 will copy frames into over PCIe (handoff40 §7, build step (a)).
//
//  IntelScanout.hpp judges the plane the firmware lit, starting from a
//  PLANE_SURF word that may never have been read.  This header judges a
//  surface we allocated ourselves, so there is no SURF word and no refusal
//  sentinel: the inputs are a GGTT base, a stride, a geometry and the GGTT
//  size.  The verdicts reuse IG_SCAN_ERR_* so a caller can log either
//  descriptor through the same table, plus one sink-only arm: a linear
//  plane's PLANE_STRIDE is programmed in 64-byte units, so a stride that is
//  not a multiple of 64 cannot be described to the hardware truthfully.
//
//  Refusal order is fixed and matches igScanDescribe where the arms overlap:
//  alignment, zero geometry, short row, stride units, extent ceiling,
//  aperture.  The unit test pins it with doubly-bad inputs.
//
//  No register I/O, no allocation, no IOKit -- this compiles on the host.
//

#ifndef INTEL_SINK_SURFACE_HPP
#define INTEL_SINK_SURFACE_HPP

#include "IntelScanout.hpp"

// Gen12 linear PLANE_STRIDE counts 64-byte units.
#define IG_SINK_STRIDE_UNIT_BYTES       64u

// Sink-only verdict, numbered past the scanout verdicts so the two ranges
// never collide.
#define IG_SINK_ERR_STRIDE_UNITS        (IG_SCAN_VERDICT_LIMIT + 1u)
#define IG_SINK_VERDICT_LIMIT           IG_SINK_ERR_STRIDE_UNITS

static inline int igSinkBaseAligned(IGS32 base)
{
    return (base & (IG_SCAN_SURF_ALIGN_BYTES - 1u)) == 0u;
}

static inline int igSinkStrideInUnits(IGS32 stride)
{
    return (stride % IG_SINK_STRIDE_UNIT_BYTES) == 0u;
}

//  Returns IG_SCAN_OK when the surface can be scanned out as described, or
//  the first refusal in the fixed order above.  The name reads as a
//  predicate on purpose: any nonzero return is a refusal.
static inline IGS32 igSinkSurfaceRefused(IGS32 base,
                                         IGS32 stride,
                                         IGS32 width,
                                         IGS32 height,
                                         IGS32 bytesPerPixel,
                                         IGS64 ggttSize)
{
    if (!igSinkBaseAligned(base))
        return IG_SCAN_ERR_UNALIGNED;
    if (width == 0u || height == 0u || stride == 0u || bytesPerPixel == 0u)
        return IG_SCAN_ERR_ZERO_GEOMETRY;
    if ((IGS64)stride < igScanMinimumRowBytes(width, bytesPerPixel))
        return IG_SCAN_ERR_ROW_TOO_SHORT;
    if (!igSinkStrideInUnits(stride))
        return IG_SINK_ERR_STRIDE_UNITS;

    IGS64 extent = igScanExtentBytes(stride, height);
    if (!igScanExtentFits(extent))
        return IG_SCAN_ERR_EXTENT_OVERFLOW;
    if (!igScanWithinAperture(base, extent, ggttSize))
        return IG_SCAN_ERR_OUTSIDE_APERTURE;
    return IG_SCAN_OK;
}

#endif  // INTEL_SINK_SURFACE_HPP