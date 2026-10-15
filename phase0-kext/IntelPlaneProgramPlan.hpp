// SPDX-License-Identifier: MIT
// Pure Tiger Lake primary-plane programming contract planner.
//
// This helper deliberately does not encode registers, expose a register-write
// list, or authorize runtime display writes. It joins three already-proven
// facts: a coherent live primary-plane observation, an owned GGTT mapping plan,
// and a narrow future full-screen linear XRGB8888 request. The result is only a
// semantic contract for a later sequencing/rollback milestone.
//
// Source-backed facts used here:
//   - Linux i915 tgl_plane_min_alignment(): linear PLANE_SURF is 4 KiB aligned.
//   - Linux i915 skl_plane_stride_mult(): linear stride is expressed in 64-byte
//     units. This must NOT reuse the 128-byte unit derived from the observed
//     Y-tiled firmware plane.
//   - vendored skl_universal_plane_regs.h / IntelFramebuffer.hpp: PLANE_SURF
//     address field is bits 31:12 and PLANE_STRIDE is 12 bits.
//
// Policy restrictions are intentionally narrower than the hardware: exact
// 8086:9a68, pipe A primary plane, full-pipe 1:1 XRGB8888, linear, origin 0, no
// rotation, no compression, no async flip, and a tight stride. Anything else is
// refused until a later source-backed milestone proves it.
#ifndef INTEL_PLANE_PROGRAM_PLAN_HPP
#define INTEL_PLANE_PROGRAM_PLAN_HPP

#include "IntelPlaneLiveObserve.hpp"
#include "IntelSinkGgtt.hpp"

#define IG_PLANE_PROGRAM_PLAN_FORMAT_XRGB8888             1u
#define IG_PLANE_PROGRAM_PLAN_TILING_LINEAR               1u
#define IG_PLANE_PROGRAM_PLAN_ROTATION_0                  0u
#define IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE               0u
#define IG_PLANE_PROGRAM_PLAN_XRGB8888_BYTES_PER_PIXEL    4u
#define IG_PLANE_PROGRAM_PLAN_LINEAR_STRIDE_UNIT_I915     64u
#define IG_PLANE_PROGRAM_PLAN_SURFACE_ALIGN_I915          4096u

#define IG_PLANE_PROGRAM_PLAN_OK                          0u
#define IG_PLANE_PROGRAM_PLAN_ERR_NULL_OUTPUT             1u
#define IG_PLANE_PROGRAM_PLAN_ERR_NULL_OBSERVATION        2u
#define IG_PLANE_PROGRAM_PLAN_ERR_NULL_MAPPING            3u
#define IG_PLANE_PROGRAM_PLAN_ERR_NULL_REQUEST            4u
#define IG_PLANE_PROGRAM_PLAN_ERR_WRONG_DEVICE            5u
#define IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR        6u
#define IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PIPE              7u
#define IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PLANE             8u
#define IG_PLANE_PROGRAM_PLAN_ERR_FORMAT                  9u
#define IG_PLANE_PROGRAM_PLAN_ERR_TILING                 10u
#define IG_PLANE_PROGRAM_PLAN_ERR_ROTATION               11u
#define IG_PLANE_PROGRAM_PLAN_ERR_DISABLED               12u
#define IG_PLANE_PROGRAM_PLAN_ERR_COMPRESSION            13u
#define IG_PLANE_PROGRAM_PLAN_ERR_ASYNC_FLIP             14u
#define IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_ORIGIN          15u
#define IG_PLANE_PROGRAM_PLAN_ERR_DEST_ORIGIN            16u
#define IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_GEOMETRY        17u
#define IG_PLANE_PROGRAM_PLAN_ERR_DEST_GEOMETRY          18u
#define IG_PLANE_PROGRAM_PLAN_ERR_SCALING                19u
#define IG_PLANE_PROGRAM_PLAN_ERR_STRIDE                 20u
#define IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE          21u
#define IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_ALIGNMENT      22u
#define IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_SHAPE          23u
#define IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_REGISTER_RANGE 24u
#define IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_SIZE           25u
#define IG_PLANE_PROGRAM_PLAN_ERR_FRAMEBUFFER_BOUNDS     26u
#define IG_PLANE_PROGRAM_PLAN_VERDICT_LIMIT              26u

struct IgPlaneProgramRequest {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;
    IGU32 format;
    IGU32 tiling;
    IGU32 rotation;

    IGU32 sourceX;
    IGU32 sourceY;
    IGU32 sourceWidth;
    IGU32 sourceHeight;
    IGU32 destX;
    IGU32 destY;
    IGU32 destWidth;
    IGU32 destHeight;
    IGU32 strideBytes;
    IGGgttU64 surfaceBytes;

    bool enable;
    bool renderCompression;
    bool asyncFlip;
    bool ggttMappingOwned;
    bool ggttMappingCommitted;
    bool ggttReadbackExact;
    bool ggttQuarantined;
};

// Semantic only. There is intentionally no CTL/SURF word, register offset,
// register sequence, or arbitrary GPU-address field in this output.
struct IgPlaneProgramPlan {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;
    IGU32 format;
    IGU32 tiling;
    IGU32 rotation;
    IGU32 width;
    IGU32 height;
    IGU32 strideBytes;
    IGU32 linearStrideUnits;
    IGGgttU32 mappingPageCount;
    IGGgttU64 visibleBytes;
    IGGgttU64 surfaceBytes;
    IGGgttU64 mappedBytes;

    bool enable;
    bool fullPipe;
    bool noScaling;
    bool uncompressed;
    bool synchronousOnly;
    bool mappingOwnershipRequired;
    bool mappingPinRequired;
    bool sequencingProofRequired;
    bool latchProofRequired;
    bool rollbackOrQuarantineRequired;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool igPlaneProgramPlanSucceeded(IGU32 verdict)
{
    return verdict == IG_PLANE_PROGRAM_PLAN_OK;
}

static inline const char *igPlaneProgramPlanVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_PLANE_PROGRAM_PLAN_OK:                          return "ok";
    case IG_PLANE_PROGRAM_PLAN_ERR_NULL_OUTPUT:             return "null-output";
    case IG_PLANE_PROGRAM_PLAN_ERR_NULL_OBSERVATION:        return "null-observation";
    case IG_PLANE_PROGRAM_PLAN_ERR_NULL_MAPPING:            return "null-mapping";
    case IG_PLANE_PROGRAM_PLAN_ERR_NULL_REQUEST:            return "null-request";
    case IG_PLANE_PROGRAM_PLAN_ERR_WRONG_DEVICE:            return "wrong-device";
    case IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR:        return "incoherent-prior";
    case IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PIPE:              return "wrong-pipe";
    case IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PLANE:             return "wrong-plane";
    case IG_PLANE_PROGRAM_PLAN_ERR_FORMAT:                  return "format";
    case IG_PLANE_PROGRAM_PLAN_ERR_TILING:                  return "tiling";
    case IG_PLANE_PROGRAM_PLAN_ERR_ROTATION:                return "rotation";
    case IG_PLANE_PROGRAM_PLAN_ERR_DISABLED:                return "disabled";
    case IG_PLANE_PROGRAM_PLAN_ERR_COMPRESSION:             return "compression";
    case IG_PLANE_PROGRAM_PLAN_ERR_ASYNC_FLIP:              return "async-flip";
    case IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_ORIGIN:           return "source-origin";
    case IG_PLANE_PROGRAM_PLAN_ERR_DEST_ORIGIN:             return "dest-origin";
    case IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_GEOMETRY:         return "source-geometry";
    case IG_PLANE_PROGRAM_PLAN_ERR_DEST_GEOMETRY:           return "dest-geometry";
    case IG_PLANE_PROGRAM_PLAN_ERR_SCALING:                 return "scaling";
    case IG_PLANE_PROGRAM_PLAN_ERR_STRIDE:                  return "stride";
    case IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE:           return "mapping-state";
    case IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_ALIGNMENT:       return "surface-alignment";
    case IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_SHAPE:           return "mapping-shape";
    case IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_REGISTER_RANGE:  return "surface-register-range";
    case IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_SIZE:            return "surface-size";
    case IG_PLANE_PROGRAM_PLAN_ERR_FRAMEBUFFER_BOUNDS:      return "framebuffer-bounds";
    default:                                                return "unknown";
    }
}

static inline bool igPlaneProgramPriorCoherent(const IgPlaneLiveObservation *prior)
{
    if (!prior || !prior->valid || !prior->liveStable ||
        !prior->programmedSurfaceIsLive || !prior->transcoderEnabled ||
        !prior->transcoderStateEnabled || !prior->planeEnabled)
        return false;

    if (!igTransconfEnabled(prior->transConfWord) ||
        !igTransconfStateEnabled(prior->transConfWord) ||
        !igFbPlaneEnabled(prior->ctlWord))
        return false;

    if (prior->programmedSurfaceWord != prior->liveSurfaceWord)
        return false;

    if (igPipesrcWidth(prior->pipeSrcWord) != prior->width ||
        igPipesrcHeight(prior->pipeSrcWord) != prior->height ||
        igFbSizeWidth(prior->sizeWord) != prior->width ||
        igFbSizeHeight(prior->sizeWord) != prior->height)
        return false;

    return true;
}

// Atomic refusal: *out is assigned only after every gate succeeds.
static inline IGU32 igPlaneProgramPlan(const IgPlaneLiveObservation *prior,
                                       const IGSinkGgttPlan *mapping,
                                       const IgPlaneProgramRequest *request,
                                       IgPlaneProgramPlan *out)
{
    if (!out)
        return IG_PLANE_PROGRAM_PLAN_ERR_NULL_OUTPUT;
    if (!prior)
        return IG_PLANE_PROGRAM_PLAN_ERR_NULL_OBSERVATION;
    if (!mapping)
        return IG_PLANE_PROGRAM_PLAN_ERR_NULL_MAPPING;
    if (!request)
        return IG_PLANE_PROGRAM_PLAN_ERR_NULL_REQUEST;

    if (request->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        prior->deviceId != request->deviceId)
        return IG_PLANE_PROGRAM_PLAN_ERR_WRONG_DEVICE;

    if (!igPlaneProgramPriorCoherent(prior))
        return IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR;

    // The only internal-panel relationship physically observed by this project
    // is pipe A primary plane. Do not generalize the read-only A..D observer into
    // a programming claim for unobserved pipes.
    if (request->pipe != IG_TRANSCODER_A || prior->pipe != request->pipe)
        return IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PIPE;
    if (request->plane != IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE)
        return IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PLANE;

    if (request->format != IG_PLANE_PROGRAM_PLAN_FORMAT_XRGB8888)
        return IG_PLANE_PROGRAM_PLAN_ERR_FORMAT;
    if (request->tiling != IG_PLANE_PROGRAM_PLAN_TILING_LINEAR)
        return IG_PLANE_PROGRAM_PLAN_ERR_TILING;
    if (request->rotation != IG_PLANE_PROGRAM_PLAN_ROTATION_0)
        return IG_PLANE_PROGRAM_PLAN_ERR_ROTATION;
    if (!request->enable)
        return IG_PLANE_PROGRAM_PLAN_ERR_DISABLED;
    if (request->renderCompression)
        return IG_PLANE_PROGRAM_PLAN_ERR_COMPRESSION;
    if (request->asyncFlip)
        return IG_PLANE_PROGRAM_PLAN_ERR_ASYNC_FLIP;

    if (request->sourceX != 0u || request->sourceY != 0u)
        return IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_ORIGIN;
    if (request->destX != 0u || request->destY != 0u)
        return IG_PLANE_PROGRAM_PLAN_ERR_DEST_ORIGIN;

    if (request->sourceWidth == 0u || request->sourceHeight == 0u ||
        request->sourceWidth != prior->width ||
        request->sourceHeight != prior->height)
        return IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_GEOMETRY;
    if (request->destWidth == 0u || request->destHeight == 0u)
        return IG_PLANE_PROGRAM_PLAN_ERR_DEST_GEOMETRY;
    if (request->destWidth != request->sourceWidth ||
        request->destHeight != request->sourceHeight)
        return IG_PLANE_PROGRAM_PLAN_ERR_SCALING;

    const IGGgttU64 rowBytes =
        (IGGgttU64)request->sourceWidth *
        (IGGgttU64)IG_PLANE_PROGRAM_PLAN_XRGB8888_BYTES_PER_PIXEL;
    if (rowBytes > 0xFFFFFFFFull ||
        request->strideBytes != (IGU32)rowBytes ||
        request->strideBytes == 0u ||
        (request->strideBytes % IG_PLANE_PROGRAM_PLAN_LINEAR_STRIDE_UNIT_I915) != 0u)
        return IG_PLANE_PROGRAM_PLAN_ERR_STRIDE;

    const IGU32 strideUnits =
        request->strideBytes / IG_PLANE_PROGRAM_PLAN_LINEAR_STRIDE_UNIT_I915;
    if (strideUnits == 0u || strideUnits > IG_FB_STRIDE_MASK)
        return IG_PLANE_PROGRAM_PLAN_ERR_STRIDE;

    if (!request->ggttMappingOwned || !request->ggttMappingCommitted ||
        !request->ggttReadbackExact || request->ggttQuarantined)
        return IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE;

    if ((mapping->ggttBase &
         (IGGgttU64)(IG_PLANE_PROGRAM_PLAN_SURFACE_ALIGN_I915 - 1u)) != 0ull)
        return IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_ALIGNMENT;

    if (mapping->pteCount == 0u || mapping->mappedBytes == 0ull)
        return IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_SHAPE;

    const IGGgttU64 expectedBase =
        (IGGgttU64)mapping->firstPte *
        (IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096;
    const IGGgttU64 expectedMapped =
        (IGGgttU64)mapping->pteCount *
        (IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096;
    if (mapping->ggttBase != expectedBase || mapping->mappedBytes != expectedMapped)
        return IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_SHAPE;

    // PLANE_SURF is a 32-bit register whose address field is bits 31:12. This
    // planner therefore refuses an otherwise-valid GGTT mapping above that
    // source-backed register range instead of truncating it.
    if ((mapping->ggttBase & ~((IGGgttU64)IG_FB_SURF_ADDR_MASK)) != 0ull)
        return IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_REGISTER_RANGE;

    if (request->surfaceBytes == 0ull ||
        request->surfaceBytes > mapping->mappedBytes)
        return IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_SIZE;

    const IGGgttU64 visibleBytes =
        ((IGGgttU64)request->sourceHeight - 1ull) *
        (IGGgttU64)request->strideBytes + rowBytes;
    if (visibleBytes == 0ull || visibleBytes > request->surfaceBytes)
        return IG_PLANE_PROGRAM_PLAN_ERR_FRAMEBUFFER_BOUNDS;

    if ((visibleBytes - 1ull) > 0xFFFFFFFFull - mapping->ggttBase)
        return IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_REGISTER_RANGE;

    IgPlaneProgramPlan staged = {};
    staged.deviceId = request->deviceId;
    staged.pipe = request->pipe;
    staged.plane = request->plane;
    staged.format = request->format;
    staged.tiling = request->tiling;
    staged.rotation = request->rotation;
    staged.width = request->sourceWidth;
    staged.height = request->sourceHeight;
    staged.strideBytes = request->strideBytes;
    staged.linearStrideUnits = strideUnits;
    staged.mappingPageCount = mapping->pteCount;
    staged.visibleBytes = visibleBytes;
    staged.surfaceBytes = request->surfaceBytes;
    staged.mappedBytes = mapping->mappedBytes;
    staged.enable = true;
    staged.fullPipe = true;
    staged.noScaling = true;
    staged.uncompressed = true;
    staged.synchronousOnly = true;
    staged.mappingOwnershipRequired = true;
    staged.mappingPinRequired = true;
    staged.sequencingProofRequired = true;
    staged.latchProofRequired = true;
    staged.rollbackOrQuarantineRequired = true;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return IG_PLANE_PROGRAM_PLAN_OK;
}

#endif // INTEL_PLANE_PROGRAM_PLAN_HPP
