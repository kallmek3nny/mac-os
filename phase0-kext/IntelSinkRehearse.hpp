//
//  IntelSinkRehearse.hpp
//  Compose-then-publish for step (b) of the 3050 sink plan: from ONE sample of
//  the firmware plane, plan where the sink would live and build the plane
//  registers that would flip scanout onto it -- and write none of them.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  intel-handoff41 section 5 item 1: nothing called igPlaneBuildLinear(). Both
//  IntelSinkAlloc.hpp and IntelPlaneProgram.hpp passed their suites while
//  contributing nothing to the driver -- intel-handoff26 SS1's class, a
//  correct pure function nobody CONSULTS. This header is the single consumer
//  of both, kept dependency-free so a host suite can execute every line the
//  kext will run. IntelDisplayProbe::publishSinkRehearsal() reads the five
//  words, hands them here, and publishes what comes back.
//
//  WHY THE COMPOSITION IS NOT IN THE SHELL
//  ---------------------------------------
//  IntelFbPresentProbe.hpp's rule, restated: a shell includes IOKit, no host
//  suite can compile it, so anything computed THERE is computed where nothing
//  can execute it. The reserved-end arithmetic below is exactly the kind of
//  sum that is right in prose and wrong by one aux surface in code.
//
//  WHY THE FIRMWARE PLANE IS NOT REQUIRED TO BE PRESENTABLE
//  -------------------------------------------------------
//  The plane this laptop's firmware lights is RENDER-COMPRESSED: the observed
//  CTL 0x82009000 carries bit 15, so igFbInheritable() refuses it as
//  COMPRESSED and IntelFbPresentProbe declines to attach. That refusal is
//  correct for PRESENTING the firmware's pixels and irrelevant here: the sink
//  does not read those pixels. It needs three facts the compressed plane
//  still states plainly -- the panel geometry, the pixel format, and where
//  the firmware's surface (and its aux surface) END, so the sink lands past
//  them. Gating on presentability would make this rehearsal refuse on the one
//  machine it is for.
//
//  THE AUX SURFACE IS WHY AUX_DIST IS A FIFTH WORD
//  -----------------------------------------------
//  A render-compressed plane has a CCS aux surface at PLANE_SURF + AUX_DIST
//  (PLANE_AUX_DISTANCE_MASK, REG_GENMASK(31, 12), in
//  reference/i915/skl_universal_plane_regs.h:208). The 2026-08-18 capture
//  reads AUX_DIST 0x007F8000 beside SURF 0x01A7C000 -- the aux begins 0x7F8000
//  past a main surface of 0x7E9000 bytes, i.e. AFTER it. A reserved end taken
//  from the main surface alone would place the sink on top of the firmware's
//  compression metadata.
//
//  The aux SIZE is not stated by any register this tree reads. So the
//  reservation treats the aux as no larger than the main surface -- a POLICY
//  over-reservation (a Gen12 CCS is far smaller), chosen because the direction
//  it errs in costs GGTT space, while the other direction scribbles.
//
//  WHAT THIS DOES NOT ESTABLISH
//  ----------------------------
//  The bound handed in is the caller's APERTURE length (BAR2), not the GGTT
//  size, which nothing in this tree has measured (intel-handoff41 section 5
//  item 2). A sink inside the aperture is inside the GGTT and is reachable
//  through the aperture -- which is what a peer write from the 3050 would
//  target -- but other stolen-memory users past the firmware plane are
//  unknown, so the plan is a REHEARSAL, published as such, never allocated.
//

#ifndef INTEL_SINK_REHEARSE_HPP
#define INTEL_SINK_REHEARSE_HPP

#include "IntelFramebuffer.hpp"
#include "IntelScanout.hpp"
#include "IntelSinkAlloc.hpp"
#include "IntelPlaneProgram.hpp"

//
//  PLANE_AUX_DIST's distance field. DOCUMENTED: PLANE_AUX_DISTANCE_MASK is
//  REG_GENMASK(31, 12) in reference/i915/skl_universal_plane_regs.h:208. The
//  low twelve bits are PLANE_AUX_STRIDE and are not a distance.
//
#define IG_SINK_AUX_DISTANCE_MASK_DOCUMENTED  0xFFFFF000u

//
//  Verdicts, checked in this order so a doubly-bad sample always reports the
//  same reason. Contiguous from zero; the limit is INCLUSIVE and equals the
//  largest member, the convention every Intel-track family follows.
//
#define IG_SINK_REHEARSE_OK               0u  // a plan and five plane words were composed
#define IG_SINK_REHEARSE_NULL_OUT         1u  // nowhere to write the rehearsal
#define IG_SINK_REHEARSE_SAMPLE_REFUSED   2u  // a word read back as the refusal sentinel
#define IG_SINK_REHEARSE_PLANE_DISABLED   3u  // firmware left the plane off: no geometry to copy
#define IG_SINK_REHEARSE_NO_BOUND         4u  // the caller measured no aperture
#define IG_SINK_REHEARSE_ALLOC_REFUSED    5u  // igSinkAllocPlan() refused; see allocResult
#define IG_SINK_REHEARSE_PLANE_REFUSED    6u  // igPlaneBuildLinear() refused; see planeVerdict
#define IG_SINK_REHEARSE_VERDICT_LIMIT    6u

//
//  What a stage that was never reached reports, and what every composed field
//  holds until its stage succeeds. All-ones rather than zero: zero is a legal
//  GGTT base, a legal POS word and IG_SINK_ALLOC_OK, so a zero would read as
//  an answer.
//
#define IG_SINK_REHEARSE_NOT_REACHED      0xFFFFFFFFu
#define IG_SINK_REHEARSE_INVALID64        0xFFFFFFFFFFFFFFFFull

struct IgSinkRehearsal {
    IGS32           allocResult;   // IgSinkAllocResult, or NOT_REACHED
    IGS32           planeVerdict;  // IG_PLANE_*, or NOT_REACHED
    IGS64           reservedEnd;   // first GGTT byte past firmware main + aux, or INVALID64
    IgSinkAllocPlan plan;          // NOT_REACHED in every field unless allocResult is OK
    IgPlaneRegs     regs;          // NOT_REACHED in every field unless the verdict is OK
};

//
//  An EQUALITY against OK, never a negation of one error arm -- a verdict
//  added later must make this false without anybody remembering to edit it.
//
static inline int igSinkRehearseUsable(IGS32 verdict)
{
    return verdict == IG_SINK_REHEARSE_OK;
}

//
//  A switch rather than a table, for IntelDisplayNames.hpp's reason: a table
//  reads past its end when the family grows. tests/test_intel_sink_rehearse.cpp
//  walks 0..IG_SINK_REHEARSE_VERDICT_LIMIT and refuses "unknown" inside it.
//
static inline const char *igSinkRehearseVerdictName(IGS32 verdict)
{
    switch (verdict) {
    case IG_SINK_REHEARSE_OK:             return "ok";
    case IG_SINK_REHEARSE_NULL_OUT:       return "null-out";
    case IG_SINK_REHEARSE_SAMPLE_REFUSED: return "sample";
    case IG_SINK_REHEARSE_PLANE_DISABLED: return "plane-disabled";
    case IG_SINK_REHEARSE_NO_BOUND:       return "no-aperture";
    case IG_SINK_REHEARSE_ALLOC_REFUSED:  return "alloc";
    case IG_SINK_REHEARSE_PLANE_REFUSED:  return "plane";
    default:                              return "unknown";
    }
}

//
//  The first GGTT byte past everything the firmware plane occupies.
//
//  64-bit throughout: SURF + AUX_DIST + a main extent can pass 2^32 for words
//  that are each individually legal. An uncompressed plane has no aux, and
//  its AUX_DIST is IGNORED rather than trusted -- firmware is free to leave a
//  stale distance in a register the hardware is not consulting.
//
static inline IGS64 igSinkRehearseReservedEnd(IGFB32 ctl, IGFB32 stride,
                                              IGFB32 size, IGFB32 surf,
                                              IGFB32 auxDist)
{
    const IGS64 base = (IGS64) igFbSurfaceOffset(surf);
    const IGS64 mainBytes = igScanExtentBytes(igFbStrideBytes(stride),
                                         igFbSizeHeight(size));
    const IGS64 mainEnd = base + mainBytes;

    if (!igFbRenderCompressed(ctl))
        return mainEnd;

    const IGS64 auxEnd = base
                       + (IGS64) (auxDist & IG_SINK_AUX_DISTANCE_MASK_DOCUMENTED)
                       + mainBytes;
    return auxEnd > mainEnd ? auxEnd : mainEnd;
}

//
//  The rehearsal. `out` is written on EVERY non-null call -- sentinels first,
//  then each stage's result as it is reached -- so a caller publishing
//  allocResult and planeVerdict after a refusal publishes NOT_REACHED, never a
//  stack residue. Composed fields stay NOT_REACHED unless the verdict is OK.
//
//  The sink copies the firmware plane's geometry and format. igSinkAllocPlan()
//  is what refuses a format that is not 32 bpp, so a firmware plane in some
//  other format reports ALLOC_REFUSED with allocResult BAD_BPP rather than
//  being silently converted.
//
static inline IGS32 igSinkRehearse(IGFB32 ctl, IGFB32 stride, IGFB32 size,
                                   IGFB32 surf, IGFB32 auxDist,
                                   IGS64 apertureBytes,
                                   IgSinkRehearsal *out)
{
    if (out == 0)
        return IG_SINK_REHEARSE_NULL_OUT;

    out->allocResult  = IG_SINK_REHEARSE_NOT_REACHED;
    out->planeVerdict = IG_SINK_REHEARSE_NOT_REACHED;
    out->reservedEnd  = IG_SINK_REHEARSE_INVALID64;
    out->plan.base = out->plan.stride = out->plan.size = IG_SINK_REHEARSE_NOT_REACHED;
    out->regs.ctl = out->regs.stride = out->regs.pos = out->regs.size =
        out->regs.surf = IG_SINK_REHEARSE_NOT_REACHED;

    if (ctl == IG_FB_INVALID || stride == IG_FB_INVALID || size == IG_FB_INVALID
        || surf == IG_FB_INVALID || auxDist == IG_FB_INVALID)
        return IG_SINK_REHEARSE_SAMPLE_REFUSED;

    if (!igFbPlaneEnabled(ctl))
        return IG_SINK_REHEARSE_PLANE_DISABLED;

    if (apertureBytes == 0)
        return IG_SINK_REHEARSE_NO_BOUND;

    const IGFB32 width  = igFbSizeWidth(size);
    const IGFB32 height = igFbSizeHeight(size);
    const IGFB32 format = igFbFormat(ctl);
    const IGS64  reservedEnd =
        igSinkRehearseReservedEnd(ctl, stride, size, surf, auxDist);
    out->reservedEnd = reservedEnd;

    IgSinkAllocPlan plan;
    const IgSinkAllocResult ar = igSinkAllocPlan(apertureBytes, reservedEnd,
                                                 width, height,
                                                 igFbBytesPerPixel(format) * 8u,
                                                 &plan);
    out->allocResult = (IGS32) ar;
    if (ar != IG_SINK_ALLOC_OK)
        return IG_SINK_REHEARSE_ALLOC_REFUSED;
    out->plan = plan;

    IgPlaneRegs regs;
    const IGFB32 pv = igPlaneBuildLinear(ctl, format, plan.base, width, height,
                                         plan.stride, &regs);
    out->planeVerdict = pv;
    if (pv != IG_PLANE_OK)
        return IG_SINK_REHEARSE_PLANE_REFUSED;
    out->regs = regs;

    return IG_SINK_REHEARSE_OK;
}

#endif  // INTEL_SINK_REHEARSE_HPP
