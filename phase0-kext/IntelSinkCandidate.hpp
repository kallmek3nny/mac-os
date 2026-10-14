// SPDX-License-Identifier: MIT
// Pure staged readiness contract for a runtime-owned Intel scanout sink.
#ifndef INTEL_SINK_CANDIDATE_HPP
#define INTEL_SINK_CANDIDATE_HPP

#include "IntelSinkAlloc.hpp"
#include "IntelSinkPublish.hpp"

// A publishable descriptor is not automatically usable by the 3050. The
// backing pages still need an Intel GGTT mapping and a peer-visible DMA mapping;
// scanout additionally needs the plane programmed. Keeping those stages in one
// tested object prevents an allocated physical range from being mislabeled as
// a finished cross-GPU surface.
#define IG_SINK_CANDIDATE_NOT_ATTEMPTED     0u
#define IG_SINK_CANDIDATE_OK                1u
#define IG_SINK_CANDIDATE_NULL_OUT          2u
#define IG_SINK_CANDIDATE_NULL_PLAN         3u
#define IG_SINK_CANDIDATE_BACKING_TOO_SHORT 4u
#define IG_SINK_CANDIDATE_PUBLISH_REFUSED   5u
#define IG_SINK_CANDIDATE_LIMIT             6u

struct IGSinkCandidate {
    IGS32 publishVerdict;
    IGSinkDescriptor descriptor;
    int backingContiguous;
    int ggttMapped;
    int peerDmaMapped;
    int planeProgrammed;
    int producerReady;
    int scanoutReady;
};

static inline void igSinkCandidateClear(IGSinkCandidate *out)
{
    out->publishVerdict = IG_SINK_CANDIDATE_NOT_ATTEMPTED;
    igSinkDescriptorClear(&out->descriptor);
    out->backingContiguous = 0;
    out->ggttMapped = 0;
    out->peerDmaMapped = 0;
    out->planeProgrammed = 0;
    out->producerReady = 0;
    out->scanoutReady = 0;
}

static inline int igSinkCandidateSucceeded(IGS32 verdict)
{
    return verdict == IG_SINK_CANDIDATE_OK;
}

static inline const char *igSinkCandidateVerdictName(IGS32 verdict)
{
    switch (verdict) {
    case IG_SINK_CANDIDATE_NOT_ATTEMPTED:     return "not-attempted";
    case IG_SINK_CANDIDATE_OK:                return "ok";
    case IG_SINK_CANDIDATE_NULL_OUT:          return "null-out";
    case IG_SINK_CANDIDATE_NULL_PLAN:         return "null-plan";
    case IG_SINK_CANDIDATE_BACKING_TOO_SHORT: return "backing-too-short";
    case IG_SINK_CANDIDATE_PUBLISH_REFUSED:   return "publish-refused";
    default:                                  return "unknown";
    }
}

// Compose the staged contract over an allocation plan plus facts the runtime
// shell measured about the backing allocation. `physicalCandidateAddress` is
// intentionally only a candidate address: the caller may not set peerDmaMapped
// until the 3050-side DMA domain has explicitly mapped it.
//
// `out` is cleared on every non-null call. A short allocation therefore cannot
// leave a descriptor from an earlier successful attempt behind.
static inline IGS32 igSinkCandidateBuild(const IgSinkAllocPlan *plan,
                                         IGS32 width,
                                         IGS32 height,
                                         IGS32 bytesPerPixel,
                                         IGS64 physicalCandidateAddress,
                                         IGS64 backingBytes,
                                         int backingContiguous,
                                         IGS64 ggttBound,
                                         int ggttMapped,
                                         int peerDmaMapped,
                                         int planeProgrammed,
                                         IGSinkCandidate *out)
{
    if (out == 0)
        return IG_SINK_CANDIDATE_NULL_OUT;
    igSinkCandidateClear(out);

    if (plan == 0)
        return IG_SINK_CANDIDATE_NULL_PLAN;
    if (backingBytes < (IGS64) plan->size)
        return IG_SINK_CANDIDATE_BACKING_TOO_SHORT;

    IGSinkDescriptor descriptor;
    const IGS32 pub = igSinkPublishRefused(plan->base,
                                           physicalCandidateAddress,
                                           backingContiguous,
                                           plan->stride,
                                           width,
                                           height,
                                           bytesPerPixel,
                                           ggttBound,
                                           &descriptor);
    out->publishVerdict = pub;
    out->backingContiguous = backingContiguous != 0;
    out->ggttMapped = ggttMapped != 0;
    out->peerDmaMapped = peerDmaMapped != 0;
    out->planeProgrammed = planeProgrammed != 0;

    if (pub != IG_SCAN_OK)
        return IG_SINK_CANDIDATE_PUBLISH_REFUSED;

    out->descriptor = descriptor;
    out->producerReady = out->ggttMapped && out->peerDmaMapped;
    out->scanoutReady = out->producerReady && out->planeProgrammed;
    return IG_SINK_CANDIDATE_OK;
}

#endif // INTEL_SINK_CANDIDATE_HPP
