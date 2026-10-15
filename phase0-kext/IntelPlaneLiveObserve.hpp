// SPDX-License-Identifier: MIT
// Read-only Tiger Lake primary-plane live-state observer.
//
// This helper performs no MMIO itself and exposes no programming ABI. Its job is
// to judge a caller-supplied sample that was read from the already-running
// display engine. The runtime shell is responsible only for bounded reads and
// publishing the result.
//
// The key source-backed register here is PLANE_SURFLIVE. Linux i915 defines
// _PLANE_SURFLIVE_1_A at 0x701ac (and the corresponding plane/pipe table), while
// the target-laptop capture observed PLANE_SURF and PLANE_SURFLIVE both equal to
// 0x01058000. Upstream does not give this project a separate SURFLIVE field
// decode to invent, so this observer deliberately treats it as an opaque raw
// word and requires exact equality with PLANE_SURF.
#ifndef INTEL_PLANE_LIVE_OBSERVE_HPP
#define INTEL_PLANE_LIVE_OBSERVE_HPP

#include "IntelGpuId.hpp"
#include "IntelDisplay.hpp"
#include "IntelFramebuffer.hpp"

#define IG_PLANE_LIVE_OK                         0u
#define IG_PLANE_LIVE_ERR_NULL_OUTPUT            1u
#define IG_PLANE_LIVE_ERR_NULL_SAMPLE            2u
#define IG_PLANE_LIVE_ERR_WRONG_DEVICE           3u
#define IG_PLANE_LIVE_ERR_PIPE_RANGE             4u
#define IG_PLANE_LIVE_ERR_SAMPLE_REFUSED         5u
#define IG_PLANE_LIVE_ERR_UNSTABLE               6u
#define IG_PLANE_LIVE_ERR_TRANSCODER_DISABLED    7u
#define IG_PLANE_LIVE_ERR_TRANSCODER_INACTIVE    8u
#define IG_PLANE_LIVE_ERR_PLANE_DISABLED         9u
#define IG_PLANE_LIVE_ERR_GEOMETRY_MISMATCH     10u
#define IG_PLANE_LIVE_ERR_SURFACE_NOT_LIVE      11u
#define IG_PLANE_LIVE_VERDICT_LIMIT             11u

// This milestone observes only the hardware primary plane. igPlaneReg() numbers
// planes from zero, so zero here names hardware plane 1.
#define IG_PLANE_LIVE_PRIMARY_PLANE               0u

struct IgPlaneLiveSample {
    IGU32 deviceId;
    IGU32 pipe;

    // Read first and last around the rest of the sample. A change means the
    // snapshot straddled a live surface transition and cannot be called atomic.
    IGU32 surfLiveBefore;
    IGU32 transConf;
    IGU32 pipeSrc;
    IGU32 ctl;
    IGU32 stride;
    IGU32 pos;
    IGU32 size;
    IGU32 surf;
    IGU32 offset;
    IGU32 surfLiveAfter;
};

struct IgPlaneLiveObservation {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 transConfWord;
    IGU32 pipeSrcWord;
    IGU32 ctlWord;
    IGU32 strideWord;
    IGU32 posWord;
    IGU32 sizeWord;
    IGU32 programmedSurfaceWord;
    IGU32 offsetWord;
    IGU32 liveSurfaceWord;

    IGU32 width;
    IGU32 height;
    IGU32 strideBytes;
    IGU32 programmedSurfaceOffset;
    IGU32 inheritVerdict;

    bool transcoderEnabled;
    bool transcoderStateEnabled;
    bool planeEnabled;
    bool liveStable;
    bool programmedSurfaceIsLive;
    bool valid;
};

static inline bool igPlaneLiveSucceeded(IGU32 verdict)
{
    return verdict == IG_PLANE_LIVE_OK;
}

static inline const char *igPlaneLiveVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_PLANE_LIVE_OK:                      return "ok";
    case IG_PLANE_LIVE_ERR_NULL_OUTPUT:         return "null-output";
    case IG_PLANE_LIVE_ERR_NULL_SAMPLE:         return "null-sample";
    case IG_PLANE_LIVE_ERR_WRONG_DEVICE:        return "wrong-device";
    case IG_PLANE_LIVE_ERR_PIPE_RANGE:          return "pipe-range";
    case IG_PLANE_LIVE_ERR_SAMPLE_REFUSED:      return "sample-refused";
    case IG_PLANE_LIVE_ERR_UNSTABLE:            return "unstable";
    case IG_PLANE_LIVE_ERR_TRANSCODER_DISABLED: return "transcoder-disabled";
    case IG_PLANE_LIVE_ERR_TRANSCODER_INACTIVE: return "transcoder-inactive";
    case IG_PLANE_LIVE_ERR_PLANE_DISABLED:      return "plane-disabled";
    case IG_PLANE_LIVE_ERR_GEOMETRY_MISMATCH:  return "geometry-mismatch";
    case IG_PLANE_LIVE_ERR_SURFACE_NOT_LIVE:    return "surface-not-live";
    default:                                    return "unknown";
    }
}

static inline bool igPlaneLiveSampleReadable(const IgPlaneLiveSample *sample)
{
    if (!sample)
        return false;

    const IGU32 words[] = {
        sample->surfLiveBefore,
        sample->transConf,
        sample->pipeSrc,
        sample->ctl,
        sample->stride,
        sample->pos,
        sample->size,
        sample->surf,
        sample->offset,
        sample->surfLiveAfter,
    };

    for (IGU32 i = 0u; i < (IGU32)(sizeof(words) / sizeof(words[0])); ++i) {
        if (words[i] == IG_MMIO_INVALID)
            return false;
    }
    return true;
}

// Atomic refusal: *out is assigned only after every gate succeeds. In
// particular, a render-compressed firmware plane is NOT refused here merely
// because this project cannot inherit its pixel layout. That fact is recorded
// in inheritVerdict. This observer answers a narrower question: did we capture a
// coherent, enabled, currently-live primary-plane state on the exact observed
// Tiger Lake part?
static inline IGU32 igPlaneLiveObserve(const IgPlaneLiveSample *sample,
                                       IgPlaneLiveObservation *out)
{
    if (!out)
        return IG_PLANE_LIVE_ERR_NULL_OUTPUT;
    if (!sample)
        return IG_PLANE_LIVE_ERR_NULL_SAMPLE;

    // Exact target-device gate for the only Tiger Lake part physically observed
    // by this project. Nearby supported TGL IDs do not inherit this evidence.
    if (sample->deviceId != IG_DEVICE_OBSERVED_2026_08_16)
        return IG_PLANE_LIVE_ERR_WRONG_DEVICE;

    // Plane register addressing is valid on the ordinary A..D pipe ladder only.
    // DSI transcoders do not share that ladder and are intentionally refused.
    if (sample->pipe > IG_TRANSCODER_D)
        return IG_PLANE_LIVE_ERR_PIPE_RANGE;

    if (!igPlaneLiveSampleReadable(sample))
        return IG_PLANE_LIVE_ERR_SAMPLE_REFUSED;

    // This is the concurrency gate. The live surface word brackets every other
    // sampled register. A change means the snapshot can combine two frames or
    // two flips even if each individual MMIO read was valid.
    if (sample->surfLiveBefore != sample->surfLiveAfter)
        return IG_PLANE_LIVE_ERR_UNSTABLE;

    if (!igTransconfEnabled(sample->transConf))
        return IG_PLANE_LIVE_ERR_TRANSCODER_DISABLED;
    if (!igTransconfStateEnabled(sample->transConf))
        return IG_PLANE_LIVE_ERR_TRANSCODER_INACTIVE;
    if (!igFbPlaneEnabled(sample->ctl))
        return IG_PLANE_LIVE_ERR_PLANE_DISABLED;

    const IGU32 pipeWidth = igPipesrcWidth(sample->pipeSrc);
    const IGU32 pipeHeight = igPipesrcHeight(sample->pipeSrc);
    const IGU32 planeWidth = igFbSizeWidth(sample->size);
    const IGU32 planeHeight = igFbSizeHeight(sample->size);
    if (pipeWidth != planeWidth || pipeHeight != planeHeight)
        return IG_PLANE_LIVE_ERR_GEOMETRY_MISMATCH;

    // Exact raw-word equality is intentionally stricter than masking an assumed
    // address field. i915 gives us the SURFLIVE register location, not a separate
    // field encoding we can responsibly invent here.
    if (sample->surf != sample->surfLiveAfter)
        return IG_PLANE_LIVE_ERR_SURFACE_NOT_LIVE;

    IgPlaneLiveObservation staged = {};
    staged.deviceId = sample->deviceId;
    staged.pipe = sample->pipe;
    staged.transConfWord = sample->transConf;
    staged.pipeSrcWord = sample->pipeSrc;
    staged.ctlWord = sample->ctl;
    staged.strideWord = sample->stride;
    staged.posWord = sample->pos;
    staged.sizeWord = sample->size;
    staged.programmedSurfaceWord = sample->surf;
    staged.offsetWord = sample->offset;
    staged.liveSurfaceWord = sample->surfLiveAfter;
    staged.width = planeWidth;
    staged.height = planeHeight;
    staged.strideBytes = igFbStrideBytes(sample->stride);
    staged.programmedSurfaceOffset = igFbSurfaceOffset(sample->surf);
    staged.inheritVerdict = igFbInheritable(sample->ctl,
                                            sample->stride,
                                            sample->size);
    staged.transcoderEnabled = true;
    staged.transcoderStateEnabled = true;
    staged.planeEnabled = true;
    staged.liveStable = true;
    staged.programmedSurfaceIsLive = true;
    staged.valid = true;
    *out = staged;
    return IG_PLANE_LIVE_OK;
}

#endif // INTEL_PLANE_LIVE_OBSERVE_HPP
