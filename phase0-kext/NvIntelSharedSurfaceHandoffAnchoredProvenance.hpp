// SPDX-License-Identifier: MIT
// Pure pre-evidence provenance anchors for the GA107 -> Tiger Lake handoff.
//
// Expected transaction identities are admitted before retirement evidence can
// exist, then carried in opaque value objects whose identity/state has no public
// mutator surface. Later evidence can be compared with an earlier anchor but
// cannot rewrite the expected epoch/token through this API.
//
// This helper performs no submission, MMIO, GGTT, IOMMU, or memory writes. It
// carries opaque epochs/tokens and lifetime facts only.
#ifndef NV_INTEL_SHARED_SURFACE_HANDOFF_ANCHORED_PROVENANCE_HPP
#define NV_INTEL_SHARED_SURFACE_HANDOFF_ANCHORED_PROVENANCE_HPP

#include "NvIntelSharedSurfaceHandoffProvenance.hpp"

#define NV_INTEL_ANCHORED_PROVENANCE_OK                         0u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_OUTPUT            1u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT             2u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR            3u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_PLAN              4u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_DISPLAY_ANCHOR         5u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_ROOT_IDENTITY          6u
#define NV_INTEL_ANCHORED_PROVENANCE_ERR_EVIDENCE               7u
#define NV_INTEL_ANCHORED_PROVENANCE_VERDICT_LIMIT              7u

class NvIntelSharedCopyTransactionAnchor;
class IgDisplayTransactionProvenanceAnchor;

static inline NVIntelHandoffU32 nvIntelBuildSharedCopyTransactionAnchor(
    const NvIntelSharedCopyProgress *progress,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    NvIntelSharedCopyTransactionAnchor *out);

static inline NVIntelHandoffU32 nvIntelBuildDisplayTransactionProvenanceAnchor(
    const IgDisplayTransactionLifetimePlan *preEvidence,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    IgDisplayTransactionProvenanceAnchor *out);

class NvIntelSharedCopyTransactionAnchor {
public:
    NvIntelSharedCopyTransactionAnchor() = default;

    NVIntelHandoffU64 handoffEpoch() const { return handoffEpoch_; }
    NVIntelHandoffU64 transactionToken() const { return transactionToken_; }
    NVIntelCopyU64 completedBeforeSubmission() const
    {
        return completedBeforeSubmission_;
    }
    NVIntelCopyU64 submittedTrackingValue() const
    {
        return submittedTrackingValue_;
    }
    NVIntelCopyU64 maxCredibleCompletionJump() const
    {
        return maxCredibleCompletionJump_;
    }
    bool peerMappingValidated() const { return peerMappingValidated_; }
    bool channelSubmitReady() const { return channelSubmitReady_; }
    bool copySubmissionPublished() const { return copySubmissionPublished_; }
    bool trackingFencePublishedAfterCopy() const
    {
        return trackingFencePublishedAfterCopy_;
    }
    bool runtimeWriteAuthorized() const { return false; }
    bool valid() const { return valid_; }

private:
    friend NVIntelHandoffU32 nvIntelBuildSharedCopyTransactionAnchor(
        const NvIntelSharedCopyProgress *progress,
        NVIntelHandoffU64 handoffEpoch,
        NVIntelHandoffU64 transactionToken,
        NvIntelSharedCopyTransactionAnchor *out);

    NVIntelHandoffU64 handoffEpoch_ = 0ull;
    NVIntelHandoffU64 transactionToken_ = 0ull;
    NVIntelCopyU64 completedBeforeSubmission_ = 0ull;
    NVIntelCopyU64 submittedTrackingValue_ = 0ull;
    NVIntelCopyU64 maxCredibleCompletionJump_ = 0ull;
    bool peerMappingValidated_ = false;
    bool channelSubmitReady_ = false;
    bool copySubmissionPublished_ = false;
    bool trackingFencePublishedAfterCopy_ = false;
    bool valid_ = false;
};

class IgDisplayTransactionProvenanceAnchor {
public:
    IgDisplayTransactionProvenanceAnchor() = default;

    NVIntelHandoffU64 handoffEpoch() const { return handoffEpoch_; }
    NVIntelHandoffU64 transactionToken() const { return transactionToken_; }
    IGU32 deviceId() const { return deviceId_; }
    IGU32 pipe() const { return pipe_; }
    IGU32 plane() const { return plane_; }
    bool targetMappingPrepared() const { return targetMappingPrepared_; }
    bool targetMappingCommitted() const { return targetMappingCommitted_; }
    bool targetMappingReadbackExact() const
    {
        return targetMappingReadbackExact_;
    }
    bool noArmProgrammingIssued() const { return noArmProgrammingIssued_; }
    bool armControlWriteIssued() const { return armControlWriteIssued_; }
    bool surfaceArmWriteIssued() const { return surfaceArmWriteIssued_; }
    bool armPointCrossed() const { return armPointCrossed_; }
    bool runtimeWriteAuthorized() const { return false; }
    bool valid() const { return valid_; }

private:
    friend NVIntelHandoffU32 nvIntelBuildDisplayTransactionProvenanceAnchor(
        const IgDisplayTransactionLifetimePlan *preEvidence,
        NVIntelHandoffU64 handoffEpoch,
        NVIntelHandoffU64 transactionToken,
        IgDisplayTransactionProvenanceAnchor *out);

    NVIntelHandoffU64 handoffEpoch_ = 0ull;
    NVIntelHandoffU64 transactionToken_ = 0ull;
    IGU32 deviceId_ = 0u;
    IGU32 pipe_ = 0u;
    IGU32 plane_ = 0u;
    bool targetMappingPrepared_ = false;
    bool targetMappingCommitted_ = false;
    bool targetMappingReadbackExact_ = false;
    bool noArmProgrammingIssued_ = false;
    bool armControlWriteIssued_ = false;
    bool surfaceArmWriteIssued_ = false;
    bool armPointCrossed_ = false;
    bool valid_ = false;
};

struct NvIntelSharedSurfaceAnchoredProvenance {
    NvIntelSharedCopyTransactionAnchor copyTransaction;
    NvIntelSharedSurfaceEvidenceProvenance copyCompletionEvidence;

    IgDisplayTransactionProvenanceAnchor intelDisplayTransaction;
    NvIntelSharedSurfaceEvidenceProvenance intelDisplayEvidence;
};

struct NvIntelSharedSurfaceAnchoredProvenancePlan {
    NvIntelSharedSurfaceHandoffProvenancePlan provenance;

    bool copyTransactionAnchorRequired;
    bool copyTransactionAnchorVerified;
    bool intelDisplayAnchorRequired;
    bool intelDisplayAnchorVerified;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvIntelAnchoredProvenanceSucceeded(NVIntelHandoffU32 verdict)
{
    return verdict == NV_INTEL_ANCHORED_PROVENANCE_OK;
}

static inline const char *nvIntelAnchoredProvenanceVerdictName(
    NVIntelHandoffU32 verdict)
{
    switch (verdict) {
    case NV_INTEL_ANCHORED_PROVENANCE_OK:                 return "ok";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_OUTPUT:    return "null-output";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT:     return "null-input";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR:    return "copy-anchor";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_PLAN:      return "copy-plan";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_DISPLAY_ANCHOR: return "display-anchor";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_ROOT_IDENTITY:  return "root-identity";
    case NV_INTEL_ANCHORED_PROVENANCE_ERR_EVIDENCE:       return "evidence";
    default:                                               return "unknown";
    }
}

// Mint the NVIDIA anchor at the publication boundary, before any bounded wait
// or completion payload observation begins. The base lifetime planner must
// independently accept the exact progress snapshot first.
static inline NVIntelHandoffU32 nvIntelBuildSharedCopyTransactionAnchor(
    const NvIntelSharedCopyProgress *progress,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    NvIntelSharedCopyTransactionAnchor *out)
{
    if (!out)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_OUTPUT;
    if (!progress)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT;
    if (handoffEpoch == 0ull || transactionToken == 0ull)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR;

    if (!progress->peerMappingValidated ||
        !progress->channelSubmitReady ||
        !progress->copySubmissionPublished ||
        !progress->trackingFencePublishedAfterCopy ||
        progress->boundedCompletionWaitUsed ||
        progress->boundedCompletionWaitFinished ||
        progress->completionPayloadReadSucceeded ||
        progress->completionWaitTimedOut)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR;

    NvIntelSharedCopyLifetimePlan lifetime = {};
    if (nvIntelSharedCopyLifetimePlan(*progress, &lifetime) != NV_INTEL_COPY_OK ||
        !lifetime.valid || lifetime.runtimeWriteAuthorized ||
        lifetime.phase != NV_INTEL_COPY_PHASE_AWAIT_COMPLETION ||
        lifetime.completionEvidencePresent || lifetime.completionProven ||
        lifetime.intelHandoffAuthorized)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR;

    NvIntelSharedCopyTransactionAnchor staged;
    staged.handoffEpoch_ = handoffEpoch;
    staged.transactionToken_ = transactionToken;
    staged.completedBeforeSubmission_ = progress->completedBeforeSubmission;
    staged.submittedTrackingValue_ = progress->submittedTrackingValue;
    staged.maxCredibleCompletionJump_ = progress->maxCredibleCompletionJump;
    staged.peerMappingValidated_ = progress->peerMappingValidated;
    staged.channelSubmitReady_ = progress->channelSubmitReady;
    staged.copySubmissionPublished_ = progress->copySubmissionPublished;
    staged.trackingFencePublishedAfterCopy_ =
        progress->trackingFencePublishedAfterCopy;
    staged.valid_ = true;
    *out = staged;
    return NV_INTEL_ANCHORED_PROVENANCE_OK;
}

static inline bool nvIntelSharedCopyTransactionAnchorMatchesProgress(
    const NvIntelSharedCopyTransactionAnchor &anchor,
    const NvIntelSharedCopyProgress &progress)
{
    if (!anchor.valid() || anchor.runtimeWriteAuthorized() ||
        anchor.handoffEpoch() == 0ull || anchor.transactionToken() == 0ull)
        return false;

    return anchor.peerMappingValidated() == progress.peerMappingValidated &&
           anchor.channelSubmitReady() == progress.channelSubmitReady &&
           anchor.copySubmissionPublished() == progress.copySubmissionPublished &&
           anchor.trackingFencePublishedAfterCopy() ==
               progress.trackingFencePublishedAfterCopy &&
           anchor.completedBeforeSubmission() == progress.completedBeforeSubmission &&
           anchor.submittedTrackingValue() == progress.submittedTrackingValue &&
           anchor.maxCredibleCompletionJump() ==
               progress.maxCredibleCompletionJump;
}

static inline bool nvIntelSharedCopyLifetimePlansEquivalent(
    const NvIntelSharedCopyLifetimePlan &a,
    const NvIntelSharedCopyLifetimePlan &b)
{
    return a.phase == b.phase &&
           a.peerMappingValidated == b.peerMappingValidated &&
           a.copySubmissionPublished == b.copySubmissionPublished &&
           a.trackingFencePublishedAfterCopy == b.trackingFencePublishedAfterCopy &&
           a.completionEvidencePresent == b.completionEvidencePresent &&
           a.completionProven == b.completionProven &&
           a.observedCompletedValue == b.observedCompletedValue &&
           a.nvidiaMayReferenceSink == b.nvidiaMayReferenceSink &&
           a.sinkBackingMustRemainPinned == b.sinkBackingMustRemainPinned &&
           a.peerMappingMustRemainPinned == b.peerMappingMustRemainPinned &&
           a.trackingFencePublicationRequired == b.trackingFencePublicationRequired &&
           a.intelHandoffAuthorized == b.intelHandoffAuthorized &&
           a.intelGgttMappingMayBegin == b.intelGgttMappingMayBegin &&
           a.peerMappingTeardownMayBeAttempted ==
               b.peerMappingTeardownMayBeAttempted &&
           a.peerMappingTeardownRequiredBeforeNvidiaIovaReuse ==
               b.peerMappingTeardownRequiredBeforeNvidiaIovaReuse &&
           a.completionUncertain == b.completionUncertain &&
           a.quarantineOnAbortOrUncertainCompletion ==
               b.quarantineOnAbortOrUncertainCompletion &&
           a.quarantineSinkBacking == b.quarantineSinkBacking &&
           a.quarantinePeerMapping == b.quarantinePeerMapping &&
           a.runtimeWriteAuthorized == b.runtimeWriteAuthorized &&
           a.valid == b.valid;
}

// Rebuild the current copy lifetime from its progress and require all immutable
// publication facts to equal the pre-evidence anchor. This prevents later
// completion observations from silently changing the transaction being retired.
static inline NVIntelHandoffU32 nvIntelVerifySharedCopyTransactionAnchor(
    const NvIntelSharedCopyTransactionAnchor *anchor,
    const NvIntelSharedCopyProgress *currentProgress,
    const NvIntelSharedCopyLifetimePlan *currentPlan)
{
    if (!anchor || !currentProgress || !currentPlan)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT;
    if (!nvIntelSharedCopyTransactionAnchorMatchesProgress(*anchor,
                                                            *currentProgress))
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR;

    NvIntelSharedCopyLifetimePlan recomputed = {};
    if (nvIntelSharedCopyLifetimePlan(*currentProgress, &recomputed) !=
        NV_INTEL_COPY_OK)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_PLAN;
    if (!nvIntelSharedCopyLifetimePlansEquivalent(recomputed, *currentPlan))
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_PLAN;
    return NV_INTEL_ANCHORED_PROVENANCE_OK;
}

// Mint the Intel anchor only at the armed, pre-retirement-evidence boundary.
// No retirement observation is allowed to participate in choosing this token.
static inline NVIntelHandoffU32 nvIntelBuildDisplayTransactionProvenanceAnchor(
    const IgDisplayTransactionLifetimePlan *preEvidence,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    IgDisplayTransactionProvenanceAnchor *out)
{
    if (!out)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_OUTPUT;
    if (!preEvidence)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT;
    if (handoffEpoch == 0ull || transactionToken == 0ull ||
        !preEvidence->valid || preEvidence->runtimeWriteAuthorized ||
        preEvidence->phase != IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT ||
        !preEvidence->targetMappingPrepared ||
        !preEvidence->targetMappingCommitted ||
        !preEvidence->targetMappingReadbackExact ||
        !preEvidence->noArmProgrammingIssued ||
        !preEvidence->armControlWriteIssued ||
        !preEvidence->surfaceArmWriteIssued ||
        !preEvidence->armPointCrossed ||
        preEvidence->retirementEvidencePresent ||
        preEvidence->retirementProven ||
        preEvidence->completionUncertain ||
        preEvidence->newSurfaceIsLiveScanout ||
        preEvidence->ggttRestoreMayBeAttempted ||
        !preEvidence->ggttRestoreBlockedByDisplayState ||
        preEvidence->newSurfaceBackingReleaseAuthorized ||
        preEvidence->targetMappingReleaseAuthorized ||
        preEvidence->displayRollbackAuthorized)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_DISPLAY_ANCHOR;

    IgDisplayTransactionProvenanceAnchor staged;
    staged.handoffEpoch_ = handoffEpoch;
    staged.transactionToken_ = transactionToken;
    staged.deviceId_ = preEvidence->deviceId;
    staged.pipe_ = preEvidence->pipe;
    staged.plane_ = preEvidence->plane;
    staged.targetMappingPrepared_ = preEvidence->targetMappingPrepared;
    staged.targetMappingCommitted_ = preEvidence->targetMappingCommitted;
    staged.targetMappingReadbackExact_ = preEvidence->targetMappingReadbackExact;
    staged.noArmProgrammingIssued_ = preEvidence->noArmProgrammingIssued;
    staged.armControlWriteIssued_ = preEvidence->armControlWriteIssued;
    staged.surfaceArmWriteIssued_ = preEvidence->surfaceArmWriteIssued;
    staged.armPointCrossed_ = preEvidence->armPointCrossed;
    staged.valid_ = true;
    *out = staged;
    return NV_INTEL_ANCHORED_PROVENANCE_OK;
}

static inline bool nvIntelDisplayTransactionAnchorMatchesPlan(
    const IgDisplayTransactionProvenanceAnchor &anchor,
    const IgDisplayTransactionLifetimePlan &current)
{
    if (!anchor.valid() || anchor.runtimeWriteAuthorized() ||
        anchor.handoffEpoch() == 0ull || anchor.transactionToken() == 0ull ||
        !current.valid || current.runtimeWriteAuthorized)
        return false;

    if (anchor.deviceId() != current.deviceId ||
        anchor.pipe() != current.pipe || anchor.plane() != current.plane)
        return false;

    if (anchor.targetMappingPrepared() != current.targetMappingPrepared ||
        anchor.targetMappingCommitted() != current.targetMappingCommitted ||
        anchor.targetMappingReadbackExact() != current.targetMappingReadbackExact ||
        anchor.noArmProgrammingIssued() != current.noArmProgrammingIssued ||
        anchor.armControlWriteIssued() != current.armControlWriteIssued ||
        anchor.surfaceArmWriteIssued() != current.surfaceArmWriteIssued ||
        anchor.armPointCrossed() != current.armPointCrossed)
        return false;

    if (current.newSurfaceBackingReleaseAuthorized ||
        current.targetMappingReleaseAuthorized ||
        current.displayRollbackAuthorized ||
        current.ggttRestoreMayBeAttempted ||
        !current.ggttRestoreBlockedByDisplayState)
        return false;

    return current.phase == IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT ||
           current.phase == IG_DISPLAY_TXN_PHASE_RETIRED_LIVE ||
           current.phase == IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE;
}

static inline NVIntelHandoffU32 nvIntelVerifyDisplayTransactionProvenanceAnchor(
    const IgDisplayTransactionProvenanceAnchor *anchor,
    const IgDisplayTransactionLifetimePlan *current)
{
    if (!anchor || !current)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT;
    return nvIntelDisplayTransactionAnchorMatchesPlan(*anchor, *current)
               ? NV_INTEL_ANCHORED_PROVENANCE_OK
               : NV_INTEL_ANCHORED_PROVENANCE_ERR_DISPLAY_ANCHOR;
}

// Atomic anchored handoff: expected tokens come only from pre-evidence anchors.
// The evidence object contains observations, never the expected transaction
// identity. Missing evidence remains conservative quarantine through the base
// provenance planner; conflicting evidence refuses atomically.
static inline NVIntelHandoffU32 nvIntelSharedSurfaceAnchoredProvenancePlan(
    const NvIntelSharedCopyProgress *copyProgress,
    const NvIntelSharedCopyLifetimePlan *copyPlan,
    const NvIntelSharedSurfaceHandoffProgress *handoffProgress,
    const IgDisplayTransactionLifetimePlan *displayPlan,
    const NvIntelSharedSurfaceAnchoredProvenance *anchored,
    NvIntelSharedSurfaceAnchoredProvenancePlan *out)
{
    if (!out)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_OUTPUT;
    if (!copyProgress || !copyPlan || !handoffProgress || !anchored)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_NULL_INPUT;

    const NVIntelHandoffU32 copyAnchorVerdict =
        nvIntelVerifySharedCopyTransactionAnchor(
            &anchored->copyTransaction, copyProgress, copyPlan);
    if (copyAnchorVerdict != NV_INTEL_ANCHORED_PROVENANCE_OK)
        return copyAnchorVerdict;

    const bool displayActive =
        nvIntelSharedSurfaceDisplayActivityObserved(displayPlan);
    if (displayActive) {
        const NVIntelHandoffU32 displayAnchorVerdict =
            nvIntelVerifyDisplayTransactionProvenanceAnchor(
                &anchored->intelDisplayTransaction, displayPlan);
        if (displayAnchorVerdict != NV_INTEL_ANCHORED_PROVENANCE_OK)
            return displayAnchorVerdict;
        if (anchored->intelDisplayTransaction.handoffEpoch() !=
            anchored->copyTransaction.handoffEpoch())
            return NV_INTEL_ANCHORED_PROVENANCE_ERR_ROOT_IDENTITY;
    }

    NvIntelSharedSurfaceHandoffProvenance expected = {};
    expected.handoffEpoch = anchored->copyTransaction.handoffEpoch();
    expected.copySubmissionToken = anchored->copyTransaction.transactionToken();
    expected.copyCompletionEvidence = anchored->copyCompletionEvidence;
    if (displayActive) {
        expected.intelDisplayTransactionToken =
            anchored->intelDisplayTransaction.transactionToken();
        expected.intelDisplayEvidence = anchored->intelDisplayEvidence;
    }

    NvIntelSharedSurfaceHandoffProvenancePlan provenancePlan = {};
    const NVIntelHandoffU32 provenanceVerdict =
        nvIntelSharedSurfaceHandoffProvenancePlan(
            copyPlan, handoffProgress, displayPlan, &expected,
            &provenancePlan);
    if (provenanceVerdict != NV_INTEL_HANDOFF_PROVENANCE_OK)
        return NV_INTEL_ANCHORED_PROVENANCE_ERR_EVIDENCE;

    NvIntelSharedSurfaceAnchoredProvenancePlan staged = {};
    staged.provenance = provenancePlan;
    staged.copyTransactionAnchorRequired = copyPlan->copySubmissionPublished;
    staged.copyTransactionAnchorVerified = true;
    staged.intelDisplayAnchorRequired = displayActive;
    staged.intelDisplayAnchorVerified = displayActive;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_INTEL_ANCHORED_PROVENANCE_OK;
}

#endif // NV_INTEL_SHARED_SURFACE_HANDOFF_ANCHORED_PROVENANCE_HPP
