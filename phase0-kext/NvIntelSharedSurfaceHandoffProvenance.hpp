// SPDX-License-Identifier: MIT
// Pure anti-replay provenance wrapper for the GA107 -> Tiger Lake shared-surface
// handoff lifetime planner.
//
// Opaque epochs and transaction tokens bind NVIDIA completion evidence and Intel
// display retirement evidence to one exact handoff. They are identifiers only:
// no monotonicity is assumed and they are never interpreted as addresses.
// Missing/partial provenance is conservative lifetime uncertainty. Definite
// nonzero mismatches are replay attempts and are refused atomically.
#ifndef NV_INTEL_SHARED_SURFACE_HANDOFF_PROVENANCE_HPP
#define NV_INTEL_SHARED_SURFACE_HANDOFF_PROVENANCE_HPP

#include "NvIntelSharedSurfaceHandoff.hpp"

#define NV_INTEL_HANDOFF_PROVENANCE_OK                         0u
#define NV_INTEL_HANDOFF_PROVENANCE_ERR_NULL_OUTPUT            1u
#define NV_INTEL_HANDOFF_PROVENANCE_ERR_NULL_INPUT             2u
#define NV_INTEL_HANDOFF_PROVENANCE_ERR_HANDOFF_CONTRACT       3u
#define NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY                 4u
#define NV_INTEL_HANDOFF_PROVENANCE_VERDICT_LIMIT              4u

#define NV_INTEL_HANDOFF_PROVENANCE_MATCH                      0u
#define NV_INTEL_HANDOFF_PROVENANCE_AMBIGUOUS                  1u
#define NV_INTEL_HANDOFF_PROVENANCE_REPLAY                     2u

struct NvIntelSharedSurfaceEvidenceProvenance {
    NVIntelHandoffU64 handoffEpoch;
    NVIntelHandoffU64 transactionToken;
};

struct NvIntelSharedSurfaceHandoffProvenance {
    // One opaque value identifies the exact cross-GPU handoff transaction.
    NVIntelHandoffU64 handoffEpoch;

    // The NVIDIA token identifies the exact copy submission whose retirement
    // evidence is being consumed by the handoff planner.
    NVIntelHandoffU64 copySubmissionToken;
    NvIntelSharedSurfaceEvidenceProvenance copyCompletionEvidence;

    // The Intel token identifies the exact display transaction whose retirement
    // evidence may establish live scanout state.
    NVIntelHandoffU64 intelDisplayTransactionToken;
    NvIntelSharedSurfaceEvidenceProvenance intelDisplayEvidence;
};

struct NvIntelSharedSurfaceHandoffProvenancePlan {
    NvIntelSharedSurfaceHandoffPlan handoff;

    NVIntelHandoffU64 handoffEpoch;
    NVIntelHandoffU64 copySubmissionToken;
    NVIntelHandoffU64 intelDisplayTransactionToken;

    bool copySubmissionProvenanceRequired;
    bool copySubmissionProvenanceBound;
    bool copyCompletionProvenanceRequired;
    bool copyCompletionProvenanceBound;
    bool intelDisplayTransactionProvenanceRequired;
    bool intelDisplayTransactionProvenanceBound;
    bool intelDisplayEvidenceProvenanceRequired;
    bool intelDisplayEvidenceProvenanceBound;

    bool provenanceUncertain;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvIntelSharedSurfaceEvidenceProvenancePresent(
    const NvIntelSharedSurfaceEvidenceProvenance &evidence)
{
    return evidence.handoffEpoch != 0ull && evidence.transactionToken != 0ull;
}

// A nonzero conflicting component is definite stale/replayed evidence. Missing
// components cannot establish a match, but also cannot safely identify which
// transaction produced the observation, so they are treated as ambiguous.
static inline NVIntelHandoffU32 nvIntelSharedSurfaceEvidenceProvenanceAssess(
    NVIntelHandoffU64 expectedHandoffEpoch,
    NVIntelHandoffU64 expectedTransactionToken,
    const NvIntelSharedSurfaceEvidenceProvenance &evidence,
    bool required)
{
    if (!required)
        return NV_INTEL_HANDOFF_PROVENANCE_MATCH;

    if (expectedHandoffEpoch == 0ull || expectedTransactionToken == 0ull)
        return NV_INTEL_HANDOFF_PROVENANCE_AMBIGUOUS;

    if ((evidence.handoffEpoch != 0ull &&
         evidence.handoffEpoch != expectedHandoffEpoch) ||
        (evidence.transactionToken != 0ull &&
         evidence.transactionToken != expectedTransactionToken))
        return NV_INTEL_HANDOFF_PROVENANCE_REPLAY;

    if (!nvIntelSharedSurfaceEvidenceProvenancePresent(evidence))
        return NV_INTEL_HANDOFF_PROVENANCE_AMBIGUOUS;

    return NV_INTEL_HANDOFF_PROVENANCE_MATCH;
}

static inline void nvIntelSharedSurfaceApplyProvenanceQuarantine(
    const NvIntelSharedCopyLifetimePlan &copy,
    const IgDisplayTransactionLifetimePlan *display,
    bool copyProvenanceAmbiguous,
    bool displayProvenanceAmbiguous,
    NvIntelSharedSurfaceHandoffPlan *handoff)
{
    const bool displayActive = nvIntelSharedSurfaceDisplayActivityObserved(display);

    handoff->phase = NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE;
    handoff->lifetimeUncertain = true;
    handoff->quarantineSharedBacking = true;
    handoff->sharedBackingMustRemainPinned = true;
    handoff->sharedBackingReleaseAuthorized = false;

    if (copyProvenanceAmbiguous) {
        // Without exact submission/evidence binding, NVIDIA retirement is not a
        // trustworthy ownership boundary. Keep every still-live affected
        // lifetime closed. A peer mapping already proven torn down stays gone;
        // ambiguity instead quarantines its slot against reuse so stale NVIDIA
        // references cannot be redirected into a different allocation.
        const bool peerMappingStillPresent =
            copy.peerMappingValidated &&
            !handoff->peerMappingTeardownSucceeded;

        handoff->copyRetirementProven = false;
        handoff->nvidiaMayStillReferenceSurface = copy.copySubmissionPublished;
        handoff->peerMappingTeardownMayBeAttempted = false;
        handoff->peerMappingMustRemainPinned = peerMappingStillPresent;
        handoff->peerMappingSlotReuseAuthorized = false;
        handoff->intelHandoffAuthorized = false;
        handoff->intelMappingMayBegin = false;
        handoff->quarantinePeerMapping = peerMappingStillPresent;

        if (displayActive) {
            handoff->quarantineIntelMapping = true;
            handoff->intelSurfaceIsLiveScanout = false;
        }
    }

    if (displayProvenanceAmbiguous) {
        // Display activity is retained, never rolled back or released based on
        // evidence that cannot be tied to this exact display transaction.
        handoff->intelSurfaceIsLiveScanout = false;
        handoff->quarantineIntelMapping =
            displayActive || handoff->intelMappingCommitted;
    }

    handoff->runtimeWriteAuthorized = false;
}

// Atomic replay refusal: *out is assigned only after the base lifetime planner
// accepts and all nonzero provenance components are shown not to conflict.
// Ambiguous provenance is not upgraded into ownership. Instead an accepted,
// explicitly uncertain quarantine plan retains the affected resources.
static inline NVIntelHandoffU32 nvIntelSharedSurfaceHandoffProvenancePlan(
    const NvIntelSharedCopyLifetimePlan *copy,
    const NvIntelSharedSurfaceHandoffProgress *progress,
    const IgDisplayTransactionLifetimePlan *display,
    const NvIntelSharedSurfaceHandoffProvenance *provenance,
    NvIntelSharedSurfaceHandoffProvenancePlan *out)
{
    if (!out)
        return NV_INTEL_HANDOFF_PROVENANCE_ERR_NULL_OUTPUT;
    if (!copy || !progress || !provenance)
        return NV_INTEL_HANDOFF_PROVENANCE_ERR_NULL_INPUT;

    NvIntelSharedSurfaceHandoffPlan base = {};
    if (nvIntelSharedSurfaceHandoffPlan(copy, progress, display, &base) !=
        NV_INTEL_HANDOFF_OK)
        return NV_INTEL_HANDOFF_PROVENANCE_ERR_HANDOFF_CONTRACT;

    const bool displayActive = nvIntelSharedSurfaceDisplayActivityObserved(display);
    const bool copySubmissionRequired = copy->copySubmissionPublished;
    const bool copyCompletionRequired =
        copy->completionEvidencePresent || copy->completionProven;
    const bool intelTransactionRequired = displayActive;
    const bool intelEvidenceRequired =
        display && (display->retirementEvidencePresent ||
                    display->retirementProven ||
                    display->completionUncertain ||
                    display->newSurfaceIsLiveScanout);

    const bool rootAmbiguous = provenance->handoffEpoch == 0ull;
    bool copyAmbiguous =
        rootAmbiguous && (copySubmissionRequired || copyCompletionRequired);
    bool displayAmbiguous =
        rootAmbiguous && (intelTransactionRequired || intelEvidenceRequired);

    const bool copySubmissionBound =
        copySubmissionRequired && provenance->handoffEpoch != 0ull &&
        provenance->copySubmissionToken != 0ull;
    if (copySubmissionRequired && !copySubmissionBound)
        copyAmbiguous = true;

    const NVIntelHandoffU32 copyEvidenceAssessment =
        nvIntelSharedSurfaceEvidenceProvenanceAssess(
            provenance->handoffEpoch,
            provenance->copySubmissionToken,
            provenance->copyCompletionEvidence,
            copyCompletionRequired);
    if (copyEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_REPLAY)
        return NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY;
    if (copyEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_AMBIGUOUS)
        copyAmbiguous = true;

    const bool intelTransactionBound =
        intelTransactionRequired && provenance->handoffEpoch != 0ull &&
        provenance->intelDisplayTransactionToken != 0ull;
    if (intelTransactionRequired && !intelTransactionBound)
        displayAmbiguous = true;

    const NVIntelHandoffU32 intelEvidenceAssessment =
        nvIntelSharedSurfaceEvidenceProvenanceAssess(
            provenance->handoffEpoch,
            provenance->intelDisplayTransactionToken,
            provenance->intelDisplayEvidence,
            intelEvidenceRequired);
    if (intelEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_REPLAY)
        return NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY;
    if (intelEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_AMBIGUOUS)
        displayAmbiguous = true;

    NvIntelSharedSurfaceHandoffProvenancePlan staged = {};
    staged.handoff = base;
    staged.handoffEpoch = provenance->handoffEpoch;
    staged.copySubmissionToken = provenance->copySubmissionToken;
    staged.intelDisplayTransactionToken =
        provenance->intelDisplayTransactionToken;

    staged.copySubmissionProvenanceRequired = copySubmissionRequired;
    staged.copySubmissionProvenanceBound = copySubmissionBound;
    staged.copyCompletionProvenanceRequired = copyCompletionRequired;
    staged.copyCompletionProvenanceBound =
        copyCompletionRequired &&
        copyEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_MATCH;
    staged.intelDisplayTransactionProvenanceRequired = intelTransactionRequired;
    staged.intelDisplayTransactionProvenanceBound = intelTransactionBound;
    staged.intelDisplayEvidenceProvenanceRequired = intelEvidenceRequired;
    staged.intelDisplayEvidenceProvenanceBound =
        intelEvidenceRequired &&
        intelEvidenceAssessment == NV_INTEL_HANDOFF_PROVENANCE_MATCH;

    staged.provenanceUncertain =
        rootAmbiguous || copyAmbiguous || displayAmbiguous;
    if (staged.provenanceUncertain) {
        nvIntelSharedSurfaceApplyProvenanceQuarantine(
            *copy, display, copyAmbiguous || rootAmbiguous,
            displayAmbiguous || rootAmbiguous, &staged.handoff);
    }

    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_INTEL_HANDOFF_PROVENANCE_OK;
}

#endif // NV_INTEL_SHARED_SURFACE_HANDOFF_PROVENANCE_HPP
