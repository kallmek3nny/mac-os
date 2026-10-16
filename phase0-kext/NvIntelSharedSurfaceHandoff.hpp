// SPDX-License-Identifier: MIT
// Pure GA107 -> Tiger Lake shared-surface handoff lifetime planner.
//
// This helper composes the NVIDIA shared-copy retirement contract with the
// Tiger Lake display transaction lifetime contract. It carries only opaque
// allocation/content identity and ownership facts. It performs no submission,
// MMIO, GGTT, IOMMU, or memory writes and exposes no raw GPU addresses,
// command streams, or register-write arrays.
#ifndef NV_INTEL_SHARED_SURFACE_HANDOFF_HPP
#define NV_INTEL_SHARED_SURFACE_HANDOFF_HPP

#include "IntelDisplayTransactionLifetime.hpp"
#include "NvIntelSharedCopyLifetime.hpp"

using NVIntelHandoffU32 = unsigned int;
using NVIntelHandoffU64 = unsigned long long;

#define NV_INTEL_HANDOFF_PHASE_WAIT_NVIDIA                 1u
#define NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED              2u
#define NV_INTEL_HANDOFF_PHASE_PEER_TEARDOWN_ACTIVE        3u
#define NV_INTEL_HANDOFF_PHASE_INTEL_MAPPING_COMMITTED     4u
#define NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_ARMED         5u
#define NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE          6u
#define NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE        7u

#define NV_INTEL_HANDOFF_OK                                0u
#define NV_INTEL_HANDOFF_ERR_NULL_OUTPUT                   1u
#define NV_INTEL_HANDOFF_ERR_NULL_COPY_PLAN                2u
#define NV_INTEL_HANDOFF_ERR_NULL_PROGRESS                 3u
#define NV_INTEL_HANDOFF_ERR_COPY_CONTRACT                 4u
#define NV_INTEL_HANDOFF_ERR_IDENTITY                      5u
#define NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE        6u
#define NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL               7u
#define NV_INTEL_HANDOFF_ERR_DISPLAY_CONTRACT              8u
#define NV_INTEL_HANDOFF_VERDICT_LIMIT                     8u

// Opaque provenance only. These values identify an allocation generation and
// its produced content generation; they are never interpreted as addresses.
struct NvIntelSharedSurfaceIdentity {
    NVIntelHandoffU64 allocationToken;
    NVIntelHandoffU64 contentGeneration;
};

struct NvIntelSharedSurfaceHandoffProgress {
    NvIntelSharedSurfaceIdentity sharedSurfaceIdentity;
    NvIntelSharedSurfaceIdentity copyCompletionIdentity;
    NvIntelSharedSurfaceIdentity intelTargetIdentity;

    bool peerMappingTeardownAttempted;
    bool peerMappingTeardownFinished;
    bool peerMappingTeardownSucceeded;
};

struct NvIntelSharedSurfaceHandoffPlan {
    NVIntelHandoffU32 phase;
    NvIntelSharedSurfaceIdentity identity;

    bool copyRetirementProven;
    bool nvidiaMayStillReferenceSurface;
    bool peerMappingTeardownMayBeAttempted;
    bool peerMappingTeardownAttempted;
    bool peerMappingTeardownFinished;
    bool peerMappingTeardownSucceeded;
    bool peerMappingMustRemainPinned;
    bool peerMappingSlotReuseAuthorized;

    bool intelHandoffAuthorized;
    bool intelMappingMayBegin;
    bool intelMappingCommitted;
    bool intelMappingMustRemainCommitted;
    bool intelDisplayArmObserved;
    bool intelSurfaceIsLiveScanout;

    bool sharedBackingMustRemainPinned;
    bool sharedBackingReleaseAuthorized;

    bool completionUncertain;
    bool lifetimeUncertain;
    bool quarantineSharedBacking;
    bool quarantinePeerMapping;
    bool quarantineIntelMapping;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvIntelSharedSurfaceIdentityPresent(
    const NvIntelSharedSurfaceIdentity &identity)
{
    return identity.allocationToken != 0ull && identity.contentGeneration != 0ull;
}

static inline bool nvIntelSharedSurfaceIdentityEqual(
    const NvIntelSharedSurfaceIdentity &a,
    const NvIntelSharedSurfaceIdentity &b)
{
    return a.allocationToken == b.allocationToken &&
           a.contentGeneration == b.contentGeneration;
}

static inline bool nvIntelSharedSurfaceCopyContractValid(
    const NvIntelSharedCopyLifetimePlan *copy)
{
    if (!copy || !copy->valid || copy->runtimeWriteAuthorized)
        return false;

    if (copy->completionProven) {
        if (copy->phase != NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL ||
            !copy->copySubmissionPublished ||
            !copy->trackingFencePublishedAfterCopy ||
            copy->nvidiaMayReferenceSink ||
            !copy->sinkBackingMustRemainPinned ||
            copy->peerMappingMustRemainPinned ||
            !copy->intelHandoffAuthorized ||
            !copy->intelGgttMappingMayBegin ||
            !copy->peerMappingTeardownMayBeAttempted ||
            copy->completionUncertain ||
            copy->quarantineSinkBacking ||
            copy->quarantinePeerMapping)
            return false;
    } else {
        if (copy->intelHandoffAuthorized || copy->intelGgttMappingMayBegin)
            return false;
        if (copy->copySubmissionPublished &&
            (!copy->nvidiaMayReferenceSink ||
             !copy->sinkBackingMustRemainPinned ||
             !copy->peerMappingMustRemainPinned ||
             copy->peerMappingTeardownMayBeAttempted))
            return false;
        if (copy->completionUncertain &&
            (!copy->quarantineOnAbortOrUncertainCompletion ||
             !copy->quarantineSinkBacking ||
             !copy->quarantinePeerMapping))
            return false;
    }

    return true;
}

static inline bool nvIntelSharedSurfaceDisplayContractValid(
    const IgDisplayTransactionLifetimePlan *display)
{
    if (!display)
        return true;
    if (!display->valid || display->runtimeWriteAuthorized)
        return false;
    if (display->newSurfaceBackingReleaseAuthorized ||
        display->targetMappingReleaseAuthorized ||
        display->displayRollbackAuthorized)
        return false;

    if (display->targetMappingCommitted) {
        if (!display->targetMappingPrepared ||
            !display->targetMappingReadbackExact ||
            !display->newSurfaceMustRemainPinned ||
            !display->targetMappingMustRemainCommitted)
            return false;
    }

    if (display->surfaceArmWriteIssued) {
        if (!display->armPointCrossed ||
            !display->targetMappingCommitted ||
            display->ggttRestoreMayBeAttempted ||
            !display->ggttRestoreBlockedByDisplayState)
            return false;
    }

    if (display->phase == IG_DISPLAY_TXN_PHASE_RETIRED_LIVE) {
        if (!display->retirementProven ||
            display->completionUncertain ||
            !display->newSurfaceIsLiveScanout ||
            display->oldSurfaceMustRemainPinned ||
            !display->oldSurfaceBackingReleaseAuthorized ||
            !display->newSurfaceMustRemainPinned ||
            !display->targetMappingMustRemainCommitted ||
            display->quarantineNewSurface ||
            display->quarantineTargetMapping)
            return false;
    } else if (display->retirementProven || display->newSurfaceIsLiveScanout) {
        return false;
    }

    if (display->phase == IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE) {
        if (!display->completionUncertain ||
            !display->quarantineOnAbortOrUncertainCompletion ||
            !display->quarantineNewSurface ||
            !display->quarantineTargetMapping)
            return false;
    } else if (display->completionUncertain) {
        return false;
    }

    return true;
}

static inline bool nvIntelSharedSurfaceDisplayActivityObserved(
    const IgDisplayTransactionLifetimePlan *display)
{
    return display &&
           (display->targetMappingPrepared ||
            display->targetMappingCommitted ||
            display->noArmProgrammingIssued ||
            display->armControlWriteIssued ||
            display->surfaceArmWriteIssued ||
            display->retirementEvidencePresent ||
            display->newSurfaceIsLiveScanout);
}

static inline bool nvIntelSharedSurfacePeerTeardownSequenceValid(
    const NvIntelSharedSurfaceHandoffProgress &progress)
{
    if (progress.peerMappingTeardownFinished &&
        !progress.peerMappingTeardownAttempted)
        return false;
    if (progress.peerMappingTeardownSucceeded &&
        !progress.peerMappingTeardownFinished)
        return false;
    return true;
}

// Atomic refusal: *out is assigned only after upstream contracts, opaque
// identity, teardown ordering, and the NVIDIA -> Intel ownership boundary are
// validated. The returned booleans are lifetime policy, never write authority.
static inline NVIntelHandoffU32 nvIntelSharedSurfaceHandoffPlan(
    const NvIntelSharedCopyLifetimePlan *copy,
    const NvIntelSharedSurfaceHandoffProgress *progress,
    const IgDisplayTransactionLifetimePlan *display,
    NvIntelSharedSurfaceHandoffPlan *out)
{
    if (!out)
        return NV_INTEL_HANDOFF_ERR_NULL_OUTPUT;
    if (!copy)
        return NV_INTEL_HANDOFF_ERR_NULL_COPY_PLAN;
    if (!progress)
        return NV_INTEL_HANDOFF_ERR_NULL_PROGRESS;
    if (!nvIntelSharedSurfaceCopyContractValid(copy))
        return NV_INTEL_HANDOFF_ERR_COPY_CONTRACT;

    if (!nvIntelSharedSurfaceIdentityPresent(progress->sharedSurfaceIdentity) ||
        !nvIntelSharedSurfaceIdentityEqual(progress->sharedSurfaceIdentity,
                                           progress->copyCompletionIdentity) ||
        !nvIntelSharedSurfaceIdentityEqual(progress->sharedSurfaceIdentity,
                                           progress->intelTargetIdentity))
        return NV_INTEL_HANDOFF_ERR_IDENTITY;

    if (!nvIntelSharedSurfacePeerTeardownSequenceValid(*progress))
        return NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE;

    if (!nvIntelSharedSurfaceDisplayContractValid(display))
        return NV_INTEL_HANDOFF_ERR_DISPLAY_CONTRACT;

    const bool copyRetired = copy->completionProven;
    const bool displayActive = nvIntelSharedSurfaceDisplayActivityObserved(display);

    if (!copyRetired &&
        (progress->peerMappingTeardownAttempted || displayActive))
        return NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL;

    if (progress->peerMappingTeardownAttempted &&
        (!copyRetired || !copy->peerMappingTeardownMayBeAttempted))
        return NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL;

    NvIntelSharedSurfaceHandoffPlan staged = {};
    staged.identity = progress->sharedSurfaceIdentity;
    staged.copyRetirementProven = copyRetired;
    staged.nvidiaMayStillReferenceSurface = !copyRetired && copy->nvidiaMayReferenceSink;

    staged.peerMappingTeardownMayBeAttempted =
        copyRetired && copy->peerMappingTeardownMayBeAttempted;
    staged.peerMappingTeardownAttempted = progress->peerMappingTeardownAttempted;
    staged.peerMappingTeardownFinished = progress->peerMappingTeardownFinished;
    staged.peerMappingTeardownSucceeded = progress->peerMappingTeardownSucceeded;
    staged.peerMappingMustRemainPinned =
        copy->peerMappingValidated && !progress->peerMappingTeardownSucceeded;
    staged.peerMappingSlotReuseAuthorized = progress->peerMappingTeardownSucceeded;

    staged.intelHandoffAuthorized = copyRetired;
    staged.intelMappingMayBegin = copyRetired;
    staged.intelMappingCommitted = display && display->targetMappingCommitted;
    staged.intelMappingMustRemainCommitted =
        display && display->targetMappingMustRemainCommitted;
    staged.intelDisplayArmObserved = display && display->surfaceArmWriteIssued;
    staged.intelSurfaceIsLiveScanout = display && display->newSurfaceIsLiveScanout;

    // The shared backing crosses ownership domains but never becomes allocator
    // release material in this transaction. Before NVIDIA retirement it is a CE
    // sink; afterwards it is an Intel target and potentially live scanout.
    staged.sharedBackingMustRemainPinned = true;
    staged.sharedBackingReleaseAuthorized = false;

    const bool peerTeardownUncertain =
        progress->peerMappingTeardownFinished &&
        !progress->peerMappingTeardownSucceeded;
    const bool displayUncertain = display && display->completionUncertain;
    staged.completionUncertain = copy->completionUncertain || displayUncertain;
    staged.lifetimeUncertain = staged.completionUncertain || peerTeardownUncertain;
    staged.quarantineSharedBacking = staged.lifetimeUncertain;
    staged.quarantinePeerMapping =
        copy->completionUncertain || peerTeardownUncertain;
    staged.quarantineIntelMapping = displayUncertain;

    if (!copyRetired) {
        staged.phase = staged.lifetimeUncertain
                           ? NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE
                           : NV_INTEL_HANDOFF_PHASE_WAIT_NVIDIA;
    } else if (staged.lifetimeUncertain) {
        staged.phase = NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE;
    } else if (display && display->newSurfaceIsLiveScanout) {
        staged.phase = NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE;
    } else if (display && display->surfaceArmWriteIssued) {
        staged.phase = NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_ARMED;
    } else if (display && display->targetMappingCommitted) {
        staged.phase = NV_INTEL_HANDOFF_PHASE_INTEL_MAPPING_COMMITTED;
    } else if (progress->peerMappingTeardownAttempted &&
               !progress->peerMappingTeardownFinished) {
        staged.phase = NV_INTEL_HANDOFF_PHASE_PEER_TEARDOWN_ACTIVE;
    } else {
        staged.phase = NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED;
    }

    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_INTEL_HANDOFF_OK;
}

#endif // NV_INTEL_SHARED_SURFACE_HANDOFF_HPP
