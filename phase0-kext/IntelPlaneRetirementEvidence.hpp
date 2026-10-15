// SPDX-License-Identifier: MIT
// Pure Tiger Lake primary-plane retirement-evidence planner.
//
// This helper performs no MMIO. It consumes the already-proven no-write
// transition contract and classifies post-arm completion evidence. The only
// retirement proof accepted by this milestone is a fresh, exact pipe-A plane-1
// flip-done event whose interrupt path was prepared before PLANE_SURF was armed
// and whose completion wait was bounded.
//
// Source-backed facts pinned here:
//   - Tiger Lake PRM Volume 12, Display Engine, Flip Programming: synchronous
//     flips update at the next frame boundary. If software uses the flip-done
//     interrupt to wait for completion, it must unmask and enable the plane's
//     flip-done interrupt before programming/arming the flip; PLANE_SURF arms
//     the update; completion is the point at which the old surface is no longer
//     being used.
//   - The same PRM states synchronous flips do not complete or signal flip-done
//     while double buffering is stalled. They complete and signal at the next
//     double-buffer update point after the stall is released.
//   - Linux i915 maps TGL/SKL+ pipe-A primary plane (PLANE_1) to
//     GEN9_PIPE_PLANE1_FLIP_DONE, bit 3. Current i915 only enables its per-plane
//     callback for do_async_flip, so this project does not copy that software
//     policy into the synchronous path; the hardware completion semantics come
//     from the TGL PRM.
//
// A vblank advance or coherent PLANE_SURFLIVE observation may corroborate a
// transition, but the sources used by this milestone do not establish either
// one alone as the exact old-backing retirement proof. They are therefore never
// sufficient here. Timeout or ambiguous/stale interrupt evidence retains and
// quarantines both surface lifetimes and the target mapping.
#ifndef INTEL_PLANE_RETIREMENT_EVIDENCE_HPP
#define INTEL_PLANE_RETIREMENT_EVIDENCE_HPP

#include "IntelPlaneTransitionPlan.hpp"

#define IG_PLANE_RETIREMENT_PRIMARY_FLIP_DONE_BIT       (1u << 3)

#define IG_PLANE_RETIREMENT_OK                           0u
#define IG_PLANE_RETIREMENT_ERR_NULL_OUTPUT              1u
#define IG_PLANE_RETIREMENT_ERR_NULL_TRANSITION          2u
#define IG_PLANE_RETIREMENT_ERR_NULL_OBSERVATION         3u
#define IG_PLANE_RETIREMENT_ERR_TRANSITION               4u
#define IG_PLANE_RETIREMENT_ERR_TRANSITION_WRITE_AUTH    5u
#define IG_PLANE_RETIREMENT_ERR_WRONG_DEVICE             6u
#define IG_PLANE_RETIREMENT_ERR_WRONG_PIPE               7u
#define IG_PLANE_RETIREMENT_ERR_WRONG_PLANE              8u
#define IG_PLANE_RETIREMENT_ERR_ARM_NOT_ISSUED           9u
#define IG_PLANE_RETIREMENT_ERR_OBSERVATION              10u
#define IG_PLANE_RETIREMENT_VERDICT_LIMIT                10u

struct IgPlaneRetirementObservation {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;

    bool surfaceArmIssued;

    bool flipDoneInterruptUnmaskedBeforeArm;
    bool flipDoneInterruptEnabledBeforeArm;
    bool staleFlipDoneStatusClearedBeforeArm;

    bool exactPrimaryPlaneFlipDoneObserved;
    bool flipDoneObservedAfterArm;

    bool boundedCompletionWaitUsed;
    bool boundedCompletionWaitFinished;
    bool completionWaitTimedOut;

    // Corroborating observations only. Neither is accepted as retirement proof
    // by itself in this milestone.
    bool vblankAdvancedAfterArm;
    bool coherentTargetLiveObservedAfterArm;
};

struct IgPlaneRetirementEvidencePlan {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;
    IGU32 flipDoneBit;

    bool synchronousFlipRequired;
    bool exactPrimaryPlaneFlipDoneRequired;
    bool flipDoneMustBeUnmaskedBeforeArm;
    bool flipDoneMustBeEnabledBeforeArm;
    bool staleFlipDoneMustBeClearedBeforeArm;
    bool completionEventMustFollowSurfaceArm;
    bool boundedCompletionWaitRequired;
    bool doubleBufferStallMayDelayCompletion;
    bool disabledPlaneImmediateEventNotApplicable;

    bool vblankOnlyRetirementProofAccepted;
    bool surfLiveOnlyRetirementProofAccepted;

    bool interruptSetupProven;
    bool exactFreshFlipDoneProven;
    bool boundedCompletionProven;
    bool retirementProven;

    bool oldSurfaceMustRemainPinned;
    bool newSurfaceMustRemainPinned;
    bool targetMappingMustRemainCommitted;
    bool oldSurfaceBackingReleaseAuthorized;
    bool targetMappingReleaseAuthorized;

    bool completionUncertain;
    bool quarantineOldSurfaceOnUncertainCompletion;
    bool quarantineNewSurfaceOnUncertainCompletion;
    bool quarantineTargetMappingOnUncertainCompletion;
    bool rollbackSafe;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool igPlaneRetirementSucceeded(IGU32 verdict)
{
    return verdict == IG_PLANE_RETIREMENT_OK;
}

static inline const char *igPlaneRetirementVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_PLANE_RETIREMENT_OK:                        return "ok";
    case IG_PLANE_RETIREMENT_ERR_NULL_OUTPUT:           return "null-output";
    case IG_PLANE_RETIREMENT_ERR_NULL_TRANSITION:       return "null-transition";
    case IG_PLANE_RETIREMENT_ERR_NULL_OBSERVATION:      return "null-observation";
    case IG_PLANE_RETIREMENT_ERR_TRANSITION:            return "transition";
    case IG_PLANE_RETIREMENT_ERR_TRANSITION_WRITE_AUTH: return "transition-write-authorized";
    case IG_PLANE_RETIREMENT_ERR_WRONG_DEVICE:          return "wrong-device";
    case IG_PLANE_RETIREMENT_ERR_WRONG_PIPE:            return "wrong-pipe";
    case IG_PLANE_RETIREMENT_ERR_WRONG_PLANE:           return "wrong-plane";
    case IG_PLANE_RETIREMENT_ERR_ARM_NOT_ISSUED:        return "arm-not-issued";
    case IG_PLANE_RETIREMENT_ERR_OBSERVATION:           return "observation";
    default:                                             return "unknown";
    }
}

static inline bool igPlaneRetirementTransitionValid(
    const IgPlaneTransitionPlan *transition)
{
    if (!transition || !transition->valid)
        return false;

    if (transition->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        transition->pipe != IG_TRANSCODER_A ||
        transition->plane != IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE ||
        transition->operationKind != IG_PLANE_TRANSITION_KIND_STATE_CHANGE_ENABLED)
        return false;

    if (!transition->currentPlaneEnabled || !transition->targetPlaneEnabled ||
        !transition->stateChangeWhileEnabled || transition->surfaceOnlyFlip ||
        transition->enableFromDisabled || transition->planeDisableRequired)
        return false;

    if (!transition->requiresNoArmProgramming ||
        !transition->requiresArmControlWrite ||
        !transition->requiresSurfaceArmWrite ||
        !transition->armControlImmediatelyBeforeSurface)
        return false;

    if (!transition->oldSurfaceMustRemainPinned ||
        !transition->newSurfaceMustRemainPinned ||
        !transition->targetMappingMustRemainCommitted ||
        !transition->flipCompletionRequired ||
        !transition->retirementProofRequired ||
        !transition->flipDoneOrEquivalentProofRequired ||
        !transition->ggttRestoreForbiddenUntilRetirement)
        return false;

    if (!transition->noWriteAbortSafe ||
        !transition->postNoArmRollbackProofRequired ||
        transition->postArmRollbackSafe ||
        !transition->timeoutMayHaveLatched ||
        !transition->quarantineOnUncertainCompletion ||
        !transition->retainBothSurfacesOnUncertainCompletion)
        return false;

    return true;
}

static inline bool igPlaneRetirementObservationConsistent(
    const IgPlaneRetirementObservation *observation)
{
    if (!observation)
        return false;

    if (!observation->surfaceArmIssued)
        return false;

    if (observation->flipDoneObservedAfterArm &&
        !observation->exactPrimaryPlaneFlipDoneObserved)
        return false;

    if (observation->boundedCompletionWaitFinished &&
        !observation->boundedCompletionWaitUsed)
        return false;

    if (observation->completionWaitTimedOut &&
        (!observation->boundedCompletionWaitUsed ||
         !observation->boundedCompletionWaitFinished))
        return false;

    if (observation->completionWaitTimedOut &&
        observation->flipDoneObservedAfterArm)
        return false;

    return true;
}

// Atomic refusal: *out is written only after identity, transition and
// observation consistency gates succeed. Ambiguous but internally consistent
// completion evidence is not a refusal: it yields a valid quarantine plan.
static inline IGU32 igPlaneRetirementEvidencePlan(
    const IgPlaneTransitionPlan *transition,
    const IgPlaneRetirementObservation *observation,
    IgPlaneRetirementEvidencePlan *out)
{
    if (!out)
        return IG_PLANE_RETIREMENT_ERR_NULL_OUTPUT;
    if (!transition)
        return IG_PLANE_RETIREMENT_ERR_NULL_TRANSITION;
    if (!observation)
        return IG_PLANE_RETIREMENT_ERR_NULL_OBSERVATION;

    if (transition->runtimeWriteAuthorized)
        return IG_PLANE_RETIREMENT_ERR_TRANSITION_WRITE_AUTH;
    if (!igPlaneRetirementTransitionValid(transition))
        return IG_PLANE_RETIREMENT_ERR_TRANSITION;

    if (observation->deviceId != IG_DEVICE_OBSERVED_2026_08_16 ||
        observation->deviceId != transition->deviceId)
        return IG_PLANE_RETIREMENT_ERR_WRONG_DEVICE;
    if (observation->pipe != IG_TRANSCODER_A ||
        observation->pipe != transition->pipe)
        return IG_PLANE_RETIREMENT_ERR_WRONG_PIPE;
    if (observation->plane != IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE ||
        observation->plane != transition->plane)
        return IG_PLANE_RETIREMENT_ERR_WRONG_PLANE;

    if (!observation->surfaceArmIssued)
        return IG_PLANE_RETIREMENT_ERR_ARM_NOT_ISSUED;
    if (!igPlaneRetirementObservationConsistent(observation))
        return IG_PLANE_RETIREMENT_ERR_OBSERVATION;

    IgPlaneRetirementEvidencePlan staged = {};
    staged.deviceId = transition->deviceId;
    staged.pipe = transition->pipe;
    staged.plane = transition->plane;
    staged.flipDoneBit = IG_PLANE_RETIREMENT_PRIMARY_FLIP_DONE_BIT;

    staged.synchronousFlipRequired = true;
    staged.exactPrimaryPlaneFlipDoneRequired = true;
    staged.flipDoneMustBeUnmaskedBeforeArm = true;
    staged.flipDoneMustBeEnabledBeforeArm = true;
    staged.staleFlipDoneMustBeClearedBeforeArm = true;
    staged.completionEventMustFollowSurfaceArm = true;
    staged.boundedCompletionWaitRequired = true;
    staged.doubleBufferStallMayDelayCompletion = true;
    staged.disabledPlaneImmediateEventNotApplicable = true;

    // The PRM permits "other methods", but this milestone has not proven an
    // alternate retirement predicate. In particular, a frame advance or live
    // address observation alone is not promoted into backing-release authority.
    staged.vblankOnlyRetirementProofAccepted = false;
    staged.surfLiveOnlyRetirementProofAccepted = false;

    staged.interruptSetupProven =
        observation->flipDoneInterruptUnmaskedBeforeArm &&
        observation->flipDoneInterruptEnabledBeforeArm &&
        observation->staleFlipDoneStatusClearedBeforeArm;

    staged.exactFreshFlipDoneProven =
        observation->exactPrimaryPlaneFlipDoneObserved &&
        observation->flipDoneObservedAfterArm;

    staged.boundedCompletionProven =
        observation->boundedCompletionWaitUsed &&
        observation->boundedCompletionWaitFinished &&
        !observation->completionWaitTimedOut;

    staged.retirementProven =
        staged.interruptSetupProven &&
        staged.exactFreshFlipDoneProven &&
        staged.boundedCompletionProven;

    // A successful flip retires only the old scanout backing. The new surface
    // and its target mapping are now the live display state and must remain
    // pinned/committed. Restoring or releasing that mapping is a future
    // transition, not rollback bookkeeping.
    staged.oldSurfaceMustRemainPinned = !staged.retirementProven;
    staged.newSurfaceMustRemainPinned = true;
    staged.targetMappingMustRemainCommitted = true;
    staged.oldSurfaceBackingReleaseAuthorized = staged.retirementProven;
    staged.targetMappingReleaseAuthorized = false;

    staged.completionUncertain = !staged.retirementProven;
    staged.quarantineOldSurfaceOnUncertainCompletion = !staged.retirementProven;
    staged.quarantineNewSurfaceOnUncertainCompletion = !staged.retirementProven;
    staged.quarantineTargetMappingOnUncertainCompletion = !staged.retirementProven;

    // Even after completion, "rollback" would itself require another properly
    // planned and retired display transition. This planner never authorizes it.
    staged.rollbackSafe = false;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;

    *out = staged;
    return IG_PLANE_RETIREMENT_OK;
}

#endif // INTEL_PLANE_RETIREMENT_EVIDENCE_HPP
