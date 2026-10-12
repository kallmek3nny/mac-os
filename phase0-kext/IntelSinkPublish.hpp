//
//  IntelSinkPublish.hpp
//
//  Pure-logic assembly of the descriptor the driver PUBLISHES for the sink
//  surface (handoff40 §7, build step (c)): the bus address, stride and
//  geometry the 3050 side needs to aim a DMA copy at the surface.
//
//  IntelSinkSurface.hpp judges the surface as the iGPU sees it, through
//  its GGTT base.  The 3050 never sees the GGTT; it writes to the backing
//  pages by bus address.  So a descriptor is publishable only when BOTH
//  views are sound: the surface verdict is IG_SCAN_OK, and the backing
//  range is a single contiguous bus range that is nonzero, page aligned
//  and does not wrap.  Contiguity is the caller's claim about its
//  allocation; this header cannot check it and says so in the name of the
//  input.
//
//  Refusal order is fixed: surface verdict first (passed through
//  unchanged, so the caller logs it through the IG_SCAN_ERR_* table), then
//  contiguity, zero bus address, bus alignment, bus wrap.
//
//  No register I/O, no allocation, no IOKit -- this compiles on the host.
//

#ifndef INTEL_SINK_PUBLISH_HPP
#define INTEL_SINK_PUBLISH_HPP

#include "IntelSinkSurface.hpp"

// DMA targets are page granular on both sides of the copy.
#define IG_SINK_BUS_ALIGN_BYTES         4096u

// Publish-only verdicts, numbered past the sink verdicts so the three
// ranges never collide.
#define IG_PUB_ERR_NOT_CONTIGUOUS       (IG_SINK_VERDICT_LIMIT + 1u)
#define IG_PUB_ERR_ZERO_BUS             (IG_SINK_VERDICT_LIMIT + 2u)
#define IG_PUB_ERR_BUS_UNALIGNED        (IG_SINK_VERDICT_LIMIT + 3u)
#define IG_PUB_ERR_BUS_WRAPS            (IG_SINK_VERDICT_LIMIT + 4u)
#define IG_PUB_VERDICT_LIMIT            IG_PUB_ERR_BUS_WRAPS

//  What the IORegistry property carries.  Filled only on IG_SCAN_OK; on
//  any refusal the descriptor is zeroed so a stale value cannot leak into
//  a publish.
struct IGSinkDescriptor {
    IGS64 busAddress;
    IGS64 extentBytes;
    IGS32 stride;
    IGS32 width;
    IGS32 height;
    IGS32 bytesPerPixel;
};

static inline void igSinkDescriptorClear(IGSinkDescriptor *d)
{
    d->busAddress    = 0u;
    d->extentBytes   = 0u;
    d->stride        = 0u;
    d->width         = 0u;
    d->height        = 0u;
    d->bytesPerPixel = 0u;
}

//  Returns IG_SCAN_OK and fills *out when the surface may be published,
//  or the first refusal in the fixed order above with *out zeroed.
static inline IGS32 igSinkPublishRefused(IGS32 ggttBase,
                                         IGS64 busAddress,
                                         int   callerClaimsContiguous,
                                         IGS32 stride,
                                         IGS32 width,
                                         IGS32 height,
                                         IGS32 bytesPerPixel,
                                         IGS64 ggttSize,
                                         IGSinkDescriptor *out)
{
    igSinkDescriptorClear(out);

    IGS32 verdict = igSinkSurfaceRefused(ggttBase, stride, width, height,
                                         bytesPerPixel, ggttSize);
    if (verdict != IG_SCAN_OK)
        return verdict;
    if (!callerClaimsContiguous)
        return IG_PUB_ERR_NOT_CONTIGUOUS;
    if (busAddress == 0u)
        return IG_PUB_ERR_ZERO_BUS;
    if ((busAddress & (IG_SINK_BUS_ALIGN_BYTES - 1u)) != 0u)
        return IG_PUB_ERR_BUS_UNALIGNED;

    // busAddress is nonzero here, so 0 - busAddress is the exact room left
    // up to the top of the space; a range ending on the last byte is legal.
    IGS64 extent = igScanExtentBytes(stride, height);
    if (extent > (IGS64)0u - busAddress)
        return IG_PUB_ERR_BUS_WRAPS;

    out->busAddress    = busAddress;
    out->extentBytes   = extent;
    out->stride        = stride;
    out->width         = width;
    out->height        = height;
    out->bytesPerPixel = bytesPerPixel;
    return IG_SCAN_OK;
}

#endif  // INTEL_SINK_PUBLISH_HPP