// SPDX-License-Identifier: MIT
// Pure Tiger Lake primary-plane transition / retirement contract planner.
//
// This helper performs no MMIO and intentionally does not expose a generic
// register-write list. It composes the already-proven coherent live observer
// with the already-proven narrow linear XRGB8888 programming plan and answers a
// narrower question: what sequencing and lifetime obligations must a future
// transactional runtime satisfy before it may arm a plane transition?
//
// Source-backed facts pinned here:
//   - Tiger Lake PRM Volume 12, Display Engine, Flip Programming: program plane
//     configuration fields other than the arming register first; a PLANE_SURF
//     write arms a plane update; old-surface retirement must be proven before
//     software reuses/releases the old surface (flip-done interrupt or another
//     source-backed proof may be used).
//   - Linux i915 skl_universal_plane.c (icl_plane_update_noarm/update_arm): TGL
//     prepares non-arming plane state separately, then writes PLANE_CTL
//     immediately before PLANE_SURF. The control register may self-arm when the
//     plane was previously disabled, which is why enable-from-disabled is not
//     generalized by this milestone.
//
// The exact observed firmware plane is enabled but differs from the target
// linear uncompressed plan, so the only successful operation class here is a
// state change while enabled. A state that merely *looks* compatible with a
// surface-only flip is refused because the target plan deliberately does not
// carry every control/color/aux field needed to prove exact state equivalence.
//
// Runtime writes remain unauthorized. After any uncertain arm/completion, both
// old and new surface lifetimes plus the target GGTT mapping must be retained or
// quarantined until hardware retirement is proven.
#ifndef INTEL_PLANE_TRANSITION_PLAN_HPP
#define INTEL_PLANE_TRANSITION_PLAN_HPP

#include "IntelPlaneProgramPlan.hpp"

#define IG_PLANE_TRANSITION_KIND_SURFACE_ONLY            1u
#define IG_PLANE_TRANSITION_KIND_STATE_CHANGE_ENABLED    2u
#define IG_PLANE_TRANSITION_KIND_ENABLE_FROM_DISABLED    3u

#define IG_PLANE_TRANSITION_OK                           0u
#define IG_PLANE_TRANSITION_ERR_NULL_OUTPUT              1u
#define IG_PLANE_TRANSITION_ERR_NULL_PRIOR               2u
#define IG_PLANE_TRANSITION_ERR_NULL_TARGET              3u
#define IG_PLANE_TRANSITION_ERR_WRONG_DEVICE             4u
#define IG_PLANE_TRANSITION_ERR_WRONG_PIPE               5u
#define IG_PLANE_TRANSITION_ERR_WRONG_PLANE              6u
#define IG_PLANE_TRANSITION_ERR_ENABLE_FROM_DISABLED     7u
#define IG_PLANE_TRANSITION_ERR_INCOHERENT_PRIOR         8u
#define IG_PLANE_TRANSITION_ERR_CURRENT_ASYNC            9u
#define IG_PLANE_TRANSITION_ERR_TARGET_INVALID          10u
#define IG_PLANE_TRANSITION_ERR_TARGET_WRITE_AUTHORIZED 11u
#define IG_PLANE_TRANSITION_ERR_SURFACE_ONLY_UNPROVEN   12u
#define IG_PLANE_TRANSITION_VERDICT_LIMIT               12u

struct IgPlaneTransitionPlan {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;
    IGU32 operationKind;

    bool currentPlaneEnabled;
    bool targetPlaneEnabled;
    bool stateChangeWhileEnabled;
    bool surfaceOnlyFlip;
    bool enableFromDisabled;

    bool requiresNoArmProgramming;
    bool requiresArmControlWrite;
    bool requiresSurfaceArmWrite;
    bool armControlImmediatelyBeforeSurface;
    bool planeDisableRequired;

    bool oldSurfaceMustRemainPinned;
    bool newSurfaceMustRemainPinned;
    bool targetMappingMustRemainCommitted;
    bool flipCompletionRequired;
    bool retirementProofRequired;
    bool flipDoneOrEquivalentProofRequired;
    bool ggttRestoreForbiddenUntilRetirement;

    bool noWriteAbortSafe;
    bool postNoArmRollbackProofRequired;
    bool postArmRollbackSafe;
    bool timeoutMayHaveLatched;
    bool quarantineOnUncertainCompletion;
    bool retainBothSurfacesOnUncertainCompletion;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool igPlaneTransitionSucceeded(IGU32 verdict)
{
    return verdict == IG_PLANE_TRANSITION_OK;
}

static inline const char *igPlaneTransitionVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_PLANE_TRANSITION_OK:                          return "ok";
    case IG_PLANE_TRANSITION_ERR_NULL_OUTPUT:             return "null-output";
    case IG_PLANE_TRANSITION_ERR_NULL_PRIOR:              return "null-prior";
    case IG_PLANE_TRANSITION_ERR_NULL_TARGET:             return "null-target";
    case IG_PLANE_TRANSITION_ERR_WRONG_DEVICE:            return "wrong-device";
    case IG_PLANE_TRANSITION_ERR_WRONG_PIPE:              return "wrong-pipe";
    case IG_PLANE_TRANSITION_ERR_WRONG_PLANE:             return "wrong-plane";
    case IG_PLANE_TRANSITION_ERR_ENABLE_FROM_DISABLED:    return "enable-from-disabled";
    case IG_PLANE_TRANSITION_ERR_INCOHERENT_PRIOR:        return "incoherent-prior";
    case IG_PLANE_TRANSITION_ERR_CURRENT_ASYNC:           return "current-async";
    case IG_PLANE_TRANSITION_ERR_TARGET_INVALID:          return "target-invalid";
    case IG_PLANE_TRANSITION_ERR_TARGET_WRITE_AUTHORIZED: return "target-write-authorized";
    case IG_PLANE_TRANSITION_ERR_SURFACE_ONLY_UNPROVEN:   return "surface-only-unproven";
    default:                                              return "unknown";
    }
}

static inline bool igPlaneTransitionTargetValid(const IgPlaneProgramPlan *target)
{
    if (!target || !target->valid)
        return false;

    if (target->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        target->pipe != IG_TRANSCODER_A ||
        target->plane != IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE)
        return false;

    if (target->format != IG_PLANE_PROGRAM_PLAN_FORMAT_XRGB8888 ||
        target->tiling != IG_PLANE_PROGRAM_PLAN_TILING_LINEAR ||
        target->rotation != IG_PLANE_PROGRAM_PLAN_ROTATION_0)
        return false;

    if (!target->enable || !target->fullPipe || !target->noScaling ||
        !target->uncompressed || !target->synchronousOnly)
        return false;

    if (!target->mappingOwnershipRequired || !target->mappingPinRequired ||
        !target->sequencingProofRequired || !target->latchProofRequired ||
        !target->rollbackOrQuarantineRequired)
        return false;

    if (target->width == 0u || target->height == 0u ||
        target->strideBytes == 0u || target->linearStrideUnits == 0u ||
        target->surfaceBytes == 0ull || target->mappedBytes < target->surfaceBytes)
        return false;

    return true;
}

static inline bool igPlaneTransitionLooksSurfaceOnly(const IgPlaneLiveObservation *prior,
                                                     const IgPlaneProgramPlan *target)
{
    if (!prior || !target)
        return false;

    if (igFbFormat(prior->ctlWord) != IG_FB_FORMAT_XRGB_8888 ||
        igFbTiling(prior->ctlWord) != IG_FB_TILING_LINEAR ||
        igFbRenderCompressed(prior->ctlWord) ||
        igFbAsyncFlip(prior->ctlWord))
        return false;

    if (igFbPosX(prior->posWord) != 0u || igFbPosY(prior->posWord) != 0u ||
        prior->offsetWord != 0u)
        return false;

    if (prior->width != target->width || prior->height != target->height)
        return false;

    // For a current *linear* plane use the source-backed i915 64-byte linear
    // stride unit from the target planner, not the firmware Y-tiled 128-byte
    // observation-derived helper in IntelFramebuffer.hpp.
    if ((prior->strideWord & IG_FB_STRIDE_MASK) != target->linearStrideUnits)
        return false;

    return true;
}

// Atomic refusal: *out is assigned only after every gate succeeds.
static inline IGU32 igPlaneTransitionPlan(const IgPlaneLiveObservation *prior,
                                          const IgPlaneProgramPlan *target,
                                          IgPlaneTransitionPlan *out)
{
    if (!out)
        return IG_PLANE_TRANSITION_ERR_NULL_OUTPUT;
    if (!prior)
        return IG_PLANE_TRANSITION_ERR_NULL_PRIOR;
    if (!target)
        return IG_PLANE_TRANSITION_ERR_NULL_TARGET;

    if (prior->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        target->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        prior->deviceId != target->deviceId)
        return IG_PLANE_TRANSITION_ERR_WRONG_DEVICE;

    if (prior->pipe != IG_TRANSCODER_A || target->pipe != IG_TRANSCODER_A ||
        prior->pipe != target->pipe)
        return IG_PLANE_TRANSITION_ERR_WRONG_PIPE;

    if (target->plane != IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE)
        return IG_PLANE_TRANSITION_ERR_WRONG_PLANE;

    // A disabled prior cannot come from the merged coherent live observer, and
    // i915 explicitly warns that PLANE_CTL may self-arm when enabling a disabled
    // plane. Refuse that different transaction class instead of extrapolating.
    if (!prior->planeEnabled || !igFbPlaneEnabled(prior->ctlWord))
        return IG_PLANE_TRANSITION_ERR_ENABLE_FROM_DISABLED;

    if (!igPlaneProgramPriorCoherent(prior))
        return IG_PLANE_TRANSITION_ERR_INCOHERENT_PRIOR;

    // Do not compose a new synchronous transaction on top of a plane whose
    // current control word requests async flips. A stable SURFLIVE sample does
    // not prove that no async transition is pending elsewhere in the frame.
    if (igFbAsyncFlip(prior->ctlWord))
        return IG_PLANE_TRANSITION_ERR_CURRENT_ASYNC;

    if (target->runtimeWriteAuthorized)
        return IG_PLANE_TRANSITION_ERR_TARGET_WRITE_AUTHORIZED;

    if (!igPlaneTransitionTargetValid(target))
        return IG_PLANE_TRANSITION_ERR_TARGET_INVALID;

    // Even when all fields this repository currently understands match, the
    // target plan intentionally omits several control/color/aux fields. Exact
    // state equality is therefore not proven, so a pure PLANE_SURF-only flip is
    // still a refusal rather than an optimistic classification.
    if (igPlaneTransitionLooksSurfaceOnly(prior, target))
        return IG_PLANE_TRANSITION_ERR_SURFACE_ONLY_UNPROVEN;

    IgPlaneTransitionPlan staged = {};
    staged.deviceId = target->deviceId;
    staged.pipe = target->pipe;
    staged.plane = target->plane;
    staged.operationKind = IG_PLANE_TRANSITION_KIND_STATE_CHANGE_ENABLED;

    staged.currentPlaneEnabled = true;
    staged.targetPlaneEnabled = true;
    staged.stateChangeWhileEnabled = true;
    staged.surfaceOnlyFlip = false;
    staged.enableFromDisabled = false;

    // i915's TGL/ICL path programs non-arming state first, then places the
    // control write immediately before the surface arm write. For this narrow
    // enabled->enabled update, a disable is not required by that path.
    staged.requiresNoArmProgramming = true;
    staged.requiresArmControlWrite = true;
    staged.requiresSurfaceArmWrite = true;
    staged.armControlImmediatelyBeforeSurface = true;
    staged.planeDisableRequired = false;

    // The PRM permits flip-done or another source-backed retirement proof, but
    // writing PLANE_SURF alone is never release permission for either surface.
    staged.oldSurfaceMustRemainPinned = true;
    staged.newSurfaceMustRemainPinned = true;
    staged.targetMappingMustRemainCommitted = true;
    staged.flipCompletionRequired = true;
    staged.retirementProofRequired = true;
    staged.flipDoneOrEquivalentProofRequired = true;
    staged.ggttRestoreForbiddenUntilRetirement = true;

    // Before the first display write, abort is trivially safe. Once shadow
    // state has been touched, rollback needs its own proof. Once the arm point
    // is crossed, a timeout may mean either surface is still referenced, so this
    // pure planner refuses to claim a safe rollback and requires quarantine.
    staged.noWriteAbortSafe = true;
    staged.postNoArmRollbackProofRequired = true;
    staged.postArmRollbackSafe = false;
    staged.timeoutMayHaveLatched = true;
    staged.quarantineOnUncertainCompletion = true;
    staged.retainBothSurfacesOnUncertainCompletion = true;

    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return IG_PLANE_TRANSITION_OK;
}

#endif // INTEL_PLANE_TRANSITION_PLAN_HPP
