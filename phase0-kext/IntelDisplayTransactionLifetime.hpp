// SPDX-License-Identifier: MIT
// Pure Tiger Lake display transaction lifetime state machine.
//
// This helper composes the already-proven primary-plane transition contract,
// the exact flip-done retirement-evidence contract, and the reversible sink
// GGTT lifetime boundary. It performs no MMIO/GGTT writes and exposes no raw
// GPU addresses or register-write lists.
//
// Safety boundary pinned by the merged contracts:
//   - Before any display programming, an exactly-owned GGTT mapping may be
//     restored through the existing reversible transaction path, but backing
//     release still requires successful restore proof.
//   - After non-arming display state has been programmed, the transition
//     contract requires a separate rollback proof. This milestone does not
//     invent one, so direct GGTT restore is blocked from that point onward.
//   - Once PLANE_SURF has armed the update, old/new surfaces and the target
//     mapping remain pinned until exact retirement evidence resolves the flip.
//   - On proven retirement only the old backing may be released. The target
//     surface/mapping is now live scanout state and is not rollback material.
//   - Ambiguous or timed-out completion quarantines both surfaces and the target
//     mapping. No restore/free/reuse authority is inferred from uncertainty.
#ifndef INTEL_DISPLAY_TRANSACTION_LIFETIME_HPP
#define INTEL_DISPLAY_TRANSACTION_LIFETIME_HPP

#include "IntelPlaneRetirementEvidence.hpp"

#define IG_DISPLAY_TXN_PHASE_PRE_MAPPING                 1u
#define IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED           2u
#define IG_DISPLAY_TXN_PHASE_NOARM_STAGED                3u
#define IG_DISPLAY_TXN_PHASE_ARM_CONTROL_STAGED          4u
#define IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT      5u
#define IG_DISPLAY_TXN_PHASE_RETIRED_LIVE                6u
#define IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE        7u

#define IG_DISPLAY_TXN_OK                                0u
#define IG_DISPLAY_TXN_ERR_NULL_OUTPUT                   1u
#define IG_DISPLAY_TXN_ERR_NULL_TRANSITION               2u
#define IG_DISPLAY_TXN_ERR_NULL_PROGRESS                 3u
#define IG_DISPLAY_TXN_ERR_TRANSITION                    4u
#define IG_DISPLAY_TXN_ERR_TRANSITION_WRITE_AUTH         5u
#define IG_DISPLAY_TXN_ERR_MAPPING_PROOF                 6u
#define IG_DISPLAY_TXN_ERR_SEQUENCE                      7u
#define IG_DISPLAY_TXN_ERR_RETIREMENT_BEFORE_ARM         8u
#define IG_DISPLAY_TXN_ERR_RETIREMENT                    9u
#define IG_DISPLAY_TXN_ERR_RETIREMENT_IDENTITY          10u
#define IG_DISPLAY_TXN_VERDICT_LIMIT                    10u

struct IgDisplayTransactionProgress {
    bool ggttTransactionPrepared;
    bool targetMappingCommitSucceeded;
    bool targetMappingExactReadbackVerified;

    bool noArmProgrammingIssued;
    bool armControlWriteIssued;
    bool surfaceArmWriteIssued;
};

struct IgDisplayTransactionLifetimePlan {
    IGU32 deviceId;
    IGU32 pipe;
    IGU32 plane;
    IGU32 phase;

    bool targetMappingPrepared;
    bool targetMappingCommitted;
    bool targetMappingReadbackExact;
    bool noArmProgrammingIssued;
    bool armControlWriteIssued;
    bool surfaceArmWriteIssued;
    bool armPointCrossed;

    bool retirementEvidencePresent;
    bool retirementProven;
    bool completionUncertain;
    bool newSurfaceIsLiveScanout;

    bool oldSurfaceMustRemainPinned;
    bool newSurfaceMustRemainPinned;
    bool targetMappingMustRemainCommitted;
    bool oldSurfaceBackingReleaseAuthorized;
    bool newSurfaceBackingReleaseAuthorized;
    bool targetMappingReleaseAuthorized;

    bool ggttRestoreMayBeAttempted;
    bool ggttRestoreSuccessRequiredBeforeTargetBackingRelease;
    bool noArmRollbackProofRequiredForRestore;
    bool ggttRestoreBlockedByDisplayState;
    bool displayRollbackAuthorized;

    bool quarantineOnAbortOrUncertainCompletion;
    bool quarantineOldSurface;
    bool quarantineNewSurface;
    bool quarantineTargetMapping;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool igDisplayTransactionLifetimeSucceeded(IGU32 verdict)
{
    return verdict == IG_DISPLAY_TXN_OK;
}

static inline const char *igDisplayTransactionLifetimeVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_DISPLAY_TXN_OK:                        return "ok";
    case IG_DISPLAY_TXN_ERR_NULL_OUTPUT:           return "null-output";
    case IG_DISPLAY_TXN_ERR_NULL_TRANSITION:       return "null-transition";
    case IG_DISPLAY_TXN_ERR_NULL_PROGRESS:         return "null-progress";
    case IG_DISPLAY_TXN_ERR_TRANSITION:            return "transition";
    case IG_DISPLAY_TXN_ERR_TRANSITION_WRITE_AUTH: return "transition-write-authorized";
    case IG_DISPLAY_TXN_ERR_MAPPING_PROOF:         return "mapping-proof";
    case IG_DISPLAY_TXN_ERR_SEQUENCE:              return "sequence";
    case IG_DISPLAY_TXN_ERR_RETIREMENT_BEFORE_ARM: return "retirement-before-arm";
    case IG_DISPLAY_TXN_ERR_RETIREMENT:            return "retirement";
    case IG_DISPLAY_TXN_ERR_RETIREMENT_IDENTITY:   return "retirement-identity";
    default:                                        return "unknown";
    }
}

static inline const char *igDisplayTransactionLifetimePhaseName(IGU32 phase)
{
    switch (phase) {
    case IG_DISPLAY_TXN_PHASE_PRE_MAPPING:            return "pre-mapping";
    case IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED:      return "mapping-committed";
    case IG_DISPLAY_TXN_PHASE_NOARM_STAGED:           return "noarm-staged";
    case IG_DISPLAY_TXN_PHASE_ARM_CONTROL_STAGED:     return "arm-control-staged";
    case IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT: return "armed-await-retirement";
    case IG_DISPLAY_TXN_PHASE_RETIRED_LIVE:           return "retired-live";
    case IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE:   return "uncertain-quarantine";
    default:                                           return "unknown";
    }
}

static inline bool igDisplayTransactionRetirementContractValid(
    const IgPlaneTransitionPlan *transition,
    const IgPlaneRetirementEvidencePlan *retirement)
{
    if (!transition || !retirement || !retirement->valid ||
        retirement->runtimeWriteAuthorized)
        return false;

    if (retirement->deviceId != transition->deviceId ||
        retirement->pipe != transition->pipe ||
        retirement->plane != transition->plane)
        return false;

    if (!retirement->synchronousFlipRequired ||
        !retirement->exactPrimaryPlaneFlipDoneRequired ||
        !retirement->flipDoneMustBeUnmaskedBeforeArm ||
        !retirement->flipDoneMustBeEnabledBeforeArm ||
        !retirement->staleFlipDoneMustBeClearedBeforeArm ||
        !retirement->completionEventMustFollowSurfaceArm ||
        !retirement->boundedCompletionWaitRequired ||
        !retirement->doubleBufferStallMayDelayCompletion ||
        !retirement->disabledPlaneImmediateEventNotApplicable ||
        retirement->vblankOnlyRetirementProofAccepted ||
        retirement->surfLiveOnlyRetirementProofAccepted)
        return false;

    if (!retirement->newSurfaceMustRemainPinned ||
        !retirement->targetMappingMustRemainCommitted ||
        retirement->targetMappingReleaseAuthorized ||
        retirement->rollbackSafe)
        return false;

    if (retirement->retirementProven) {
        if (retirement->oldSurfaceMustRemainPinned ||
            !retirement->oldSurfaceBackingReleaseAuthorized ||
            retirement->completionUncertain ||
            retirement->quarantineOldSurfaceOnUncertainCompletion ||
            retirement->quarantineNewSurfaceOnUncertainCompletion ||
            retirement->quarantineTargetMappingOnUncertainCompletion)
            return false;
    } else {
        if (!retirement->oldSurfaceMustRemainPinned ||
            retirement->oldSurfaceBackingReleaseAuthorized ||
            !retirement->completionUncertain ||
            !retirement->quarantineOldSurfaceOnUncertainCompletion ||
            !retirement->quarantineNewSurfaceOnUncertainCompletion ||
            !retirement->quarantineTargetMappingOnUncertainCompletion)
            return false;
    }

    return true;
}

static inline bool igDisplayTransactionProgressConsistent(
    const IgDisplayTransactionProgress *progress)
{
    if (!progress)
        return false;

    if (progress->targetMappingExactReadbackVerified &&
        !progress->targetMappingCommitSucceeded)
        return false;

    // A committed mapping is only admitted when the reversible GGTT path was
    // prepared first and exact post-commit readback has already been proven.
    if (progress->targetMappingCommitSucceeded &&
        (!progress->ggttTransactionPrepared ||
         !progress->targetMappingExactReadbackVerified))
        return false;

    if (progress->noArmProgrammingIssued &&
        (!progress->targetMappingCommitSucceeded ||
         !progress->targetMappingExactReadbackVerified))
        return false;

    if (progress->armControlWriteIssued &&
        !progress->noArmProgrammingIssued)
        return false;

    if (progress->surfaceArmWriteIssued &&
        !progress->armControlWriteIssued)
        return false;

    return true;
}

// Atomic refusal: *out is assigned only after all inherited contracts and the
// monotonic phase ordering have been validated. The result is lifetime policy,
// never permission to perform the writes represented by the progress flags.
static inline IGU32 igDisplayTransactionLifetimePlan(
    const IgPlaneTransitionPlan *transition,
    const IgDisplayTransactionProgress *progress,
    const IgPlaneRetirementEvidencePlan *retirement,
    IgDisplayTransactionLifetimePlan *out)
{
    if (!out)
        return IG_DISPLAY_TXN_ERR_NULL_OUTPUT;
    if (!transition)
        return IG_DISPLAY_TXN_ERR_NULL_TRANSITION;
    if (!progress)
        return IG_DISPLAY_TXN_ERR_NULL_PROGRESS;

    if (transition->runtimeWriteAuthorized)
        return IG_DISPLAY_TXN_ERR_TRANSITION_WRITE_AUTH;
    if (!igPlaneRetirementTransitionValid(transition))
        return IG_DISPLAY_TXN_ERR_TRANSITION;

    if (!igDisplayTransactionProgressConsistent(progress)) {
        const bool mappingContradiction =
            (progress->targetMappingExactReadbackVerified &&
             !progress->targetMappingCommitSucceeded) ||
            (progress->targetMappingCommitSucceeded &&
             (!progress->ggttTransactionPrepared ||
              !progress->targetMappingExactReadbackVerified)) ||
            (progress->noArmProgrammingIssued &&
             (!progress->targetMappingCommitSucceeded ||
              !progress->targetMappingExactReadbackVerified));
        return mappingContradiction ? IG_DISPLAY_TXN_ERR_MAPPING_PROOF
                                    : IG_DISPLAY_TXN_ERR_SEQUENCE;
    }

    if (retirement && !progress->surfaceArmWriteIssued)
        return IG_DISPLAY_TXN_ERR_RETIREMENT_BEFORE_ARM;

    if (retirement) {
        if (retirement->deviceId != transition->deviceId ||
            retirement->pipe != transition->pipe ||
            retirement->plane != transition->plane)
            return IG_DISPLAY_TXN_ERR_RETIREMENT_IDENTITY;
        if (!igDisplayTransactionRetirementContractValid(transition, retirement))
            return IG_DISPLAY_TXN_ERR_RETIREMENT;
    }

    IgDisplayTransactionLifetimePlan staged = {};
    staged.deviceId = transition->deviceId;
    staged.pipe = transition->pipe;
    staged.plane = transition->plane;
    staged.targetMappingPrepared = progress->ggttTransactionPrepared;
    staged.targetMappingCommitted = progress->targetMappingCommitSucceeded;
    staged.targetMappingReadbackExact = progress->targetMappingExactReadbackVerified;
    staged.noArmProgrammingIssued = progress->noArmProgrammingIssued;
    staged.armControlWriteIssued = progress->armControlWriteIssued;
    staged.surfaceArmWriteIssued = progress->surfaceArmWriteIssued;
    staged.armPointCrossed = progress->surfaceArmWriteIssued;
    staged.retirementEvidencePresent = retirement != nullptr;
    staged.retirementProven = retirement && retirement->retirementProven;
    staged.completionUncertain = retirement && retirement->completionUncertain;
    staged.newSurfaceIsLiveScanout = staged.retirementProven;

    // The currently scanned-out old surface remains live until exact retirement.
    staged.oldSurfaceMustRemainPinned = !staged.retirementProven;
    staged.oldSurfaceBackingReleaseAuthorized = staged.retirementProven;

    // The target becomes a display lifetime once its GGTT image is committed.
    staged.newSurfaceMustRemainPinned = progress->targetMappingCommitSucceeded;
    staged.targetMappingMustRemainCommitted = progress->targetMappingCommitSucceeded;
    staged.newSurfaceBackingReleaseAuthorized = false;
    staged.targetMappingReleaseAuthorized = false;

    // Only the pre-display-write mapping-committed phase may attempt the exact
    // reversible GGTT restore. Restore success is still a separate proof before
    // target backing may be released.
    staged.ggttRestoreMayBeAttempted =
        progress->targetMappingCommitSucceeded &&
        progress->targetMappingExactReadbackVerified &&
        !progress->noArmProgrammingIssued;
    staged.ggttRestoreSuccessRequiredBeforeTargetBackingRelease =
        staged.ggttRestoreMayBeAttempted;

    staged.noArmRollbackProofRequiredForRestore =
        progress->noArmProgrammingIssued && !progress->surfaceArmWriteIssued;
    staged.ggttRestoreBlockedByDisplayState =
        progress->noArmProgrammingIssued || progress->surfaceArmWriteIssued;
    staged.displayRollbackAuthorized = false;

    staged.quarantineOnAbortOrUncertainCompletion =
        progress->noArmProgrammingIssued ||
        progress->surfaceArmWriteIssued ||
        staged.completionUncertain;

    if (!progress->targetMappingCommitSucceeded) {
        staged.phase = IG_DISPLAY_TXN_PHASE_PRE_MAPPING;
        staged.newSurfaceMustRemainPinned = false;
        staged.targetMappingMustRemainCommitted = false;
        staged.quarantineOnAbortOrUncertainCompletion = false;
    } else if (!progress->noArmProgrammingIssued) {
        staged.phase = IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED;
    } else if (!progress->armControlWriteIssued) {
        staged.phase = IG_DISPLAY_TXN_PHASE_NOARM_STAGED;
    } else if (!progress->surfaceArmWriteIssued) {
        staged.phase = IG_DISPLAY_TXN_PHASE_ARM_CONTROL_STAGED;
    } else if (!retirement) {
        staged.phase = IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT;
    } else if (retirement->retirementProven) {
        staged.phase = IG_DISPLAY_TXN_PHASE_RETIRED_LIVE;
    } else {
        staged.phase = IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE;
    }

    if (staged.phase == IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE) {
        staged.quarantineOldSurface = true;
        staged.quarantineNewSurface = true;
        staged.quarantineTargetMapping = true;
    }

    // Once the successful flip is retired, the new surface/mapping is active
    // display state. It remains pinned/committed; "restore" would be another
    // display transition and is not authorized by this transaction.
    if (staged.phase == IG_DISPLAY_TXN_PHASE_RETIRED_LIVE) {
        staged.newSurfaceMustRemainPinned = true;
        staged.targetMappingMustRemainCommitted = true;
        staged.ggttRestoreMayBeAttempted = false;
        staged.ggttRestoreSuccessRequiredBeforeTargetBackingRelease = false;
        staged.noArmRollbackProofRequiredForRestore = false;
        staged.ggttRestoreBlockedByDisplayState = true;
        staged.quarantineOnAbortOrUncertainCompletion = false;
    }

    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return IG_DISPLAY_TXN_OK;
}

#endif // INTEL_DISPLAY_TRANSACTION_LIFETIME_HPP
