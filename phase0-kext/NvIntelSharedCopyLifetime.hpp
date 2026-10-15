// SPDX-License-Identifier: MIT
// Pure GA107 -> Tiger Lake shared-sink copy retirement lifetime planner.
//
// This helper turns the existing Ampere tracking-semaphore completion rule into
// a conservative ownership handoff boundary. It performs no submission, MMIO,
// GGTT, IOMMU, or memory writes and exposes no GPU/IOVA addresses or command
// stream. The shared sink and its NVIDIA peer mapping stay pinned from copy
// publication until an ordered tracking fence is proven retired. Only then may
// the Intel side begin its own GGTT/display transaction.
#ifndef NV_INTEL_SHARED_COPY_LIFETIME_HPP
#define NV_INTEL_SHARED_COPY_LIFETIME_HPP

#include "NVAmpereFence.hpp"

using NVIntelCopyU32 = unsigned int;
using NVIntelCopyU64 = unsigned long long;

#define NV_INTEL_COPY_PHASE_PRE_PEER                    1u
#define NV_INTEL_COPY_PHASE_PEER_READY                  2u
#define NV_INTEL_COPY_PHASE_COPY_UNTRACKED              3u
#define NV_INTEL_COPY_PHASE_AWAIT_COMPLETION            4u
#define NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL           5u
#define NV_INTEL_COPY_PHASE_UNCERTAIN_QUARANTINE        6u

#define NV_INTEL_COPY_OK                                0u
#define NV_INTEL_COPY_ERR_NULL_OUTPUT                   1u
#define NV_INTEL_COPY_ERR_SEQUENCE                      2u
#define NV_INTEL_COPY_ERR_CHANNEL                       3u
#define NV_INTEL_COPY_ERR_TRACKING_TARGET               4u
#define NV_INTEL_COPY_ERR_COMPLETION_EVIDENCE           5u
#define NV_INTEL_COPY_VERDICT_LIMIT                     6u

// Runtime progress is expressed only as ownership/completion facts. Raw GPU
// virtual addresses, peer IOVAs, methods, and GPFIFO entries deliberately do not
// cross this lifetime API.
struct NvIntelSharedCopyProgress {
    bool peerMappingValidated;
    bool channelSubmitReady;
    bool copySubmissionPublished;
    bool trackingFencePublishedAfterCopy;

    bool boundedCompletionWaitUsed;
    bool boundedCompletionWaitFinished;
    bool completionPayloadReadSucceeded;
    bool completionWaitTimedOut;

    NVIntelCopyU64 completedBeforeSubmission;
    NVIntelCopyU64 submittedTrackingValue;
    NVIntelCopyU32 observedCompletionPayload;
    NVIntelCopyU64 maxCredibleCompletionJump;
};

struct NvIntelSharedCopyLifetimePlan {
    NVIntelCopyU32 phase;

    bool peerMappingValidated;
    bool copySubmissionPublished;
    bool trackingFencePublishedAfterCopy;
    bool completionEvidencePresent;
    bool completionProven;
    NVIntelCopyU64 observedCompletedValue;

    bool nvidiaMayReferenceSink;
    bool sinkBackingMustRemainPinned;
    bool peerMappingMustRemainPinned;
    bool trackingFencePublicationRequired;

    bool intelHandoffAuthorized;
    bool intelGgttMappingMayBegin;
    bool peerMappingTeardownMayBeAttempted;
    bool peerMappingTeardownRequiredBeforeNvidiaIovaReuse;

    bool completionUncertain;
    bool quarantineOnAbortOrUncertainCompletion;
    bool quarantineSinkBacking;
    bool quarantinePeerMapping;

    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvIntelSharedCopyLifetimeSucceeded(NVIntelCopyU32 verdict)
{
    return verdict == NV_INTEL_COPY_OK;
}

static inline const char *nvIntelSharedCopyLifetimeVerdictName(NVIntelCopyU32 verdict)
{
    switch (verdict) {
    case NV_INTEL_COPY_OK:                      return "ok";
    case NV_INTEL_COPY_ERR_NULL_OUTPUT:         return "null-output";
    case NV_INTEL_COPY_ERR_SEQUENCE:            return "sequence";
    case NV_INTEL_COPY_ERR_CHANNEL:             return "channel";
    case NV_INTEL_COPY_ERR_TRACKING_TARGET:     return "tracking-target";
    case NV_INTEL_COPY_ERR_COMPLETION_EVIDENCE: return "completion-evidence";
    default:                                    return "unknown";
    }
}

static inline const char *nvIntelSharedCopyLifetimePhaseName(NVIntelCopyU32 phase)
{
    switch (phase) {
    case NV_INTEL_COPY_PHASE_PRE_PEER:             return "pre-peer";
    case NV_INTEL_COPY_PHASE_PEER_READY:           return "peer-ready";
    case NV_INTEL_COPY_PHASE_COPY_UNTRACKED:       return "copy-untracked";
    case NV_INTEL_COPY_PHASE_AWAIT_COMPLETION:     return "await-completion";
    case NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL:    return "retired-for-intel";
    case NV_INTEL_COPY_PHASE_UNCERTAIN_QUARANTINE: return "uncertain-quarantine";
    default:                                       return "unknown";
    }
}

static inline bool nvIntelSharedCopyProgressSequenceValid(
    const NvIntelSharedCopyProgress &p)
{
    if (p.copySubmissionPublished &&
        (!p.peerMappingValidated || !p.channelSubmitReady))
        return false;
    if (p.trackingFencePublishedAfterCopy && !p.copySubmissionPublished)
        return false;
    if (p.boundedCompletionWaitUsed && !p.trackingFencePublishedAfterCopy)
        return false;
    if (p.boundedCompletionWaitFinished && !p.boundedCompletionWaitUsed)
        return false;
    if (p.completionPayloadReadSucceeded && !p.boundedCompletionWaitUsed)
        return false;
    if (p.completionWaitTimedOut &&
        (!p.boundedCompletionWaitUsed || !p.boundedCompletionWaitFinished))
        return false;
    return true;
}

// Atomic refusal: *out is assigned only after all ordering and fence-counter
// evidence validates. A target tracking value must be strictly newer than the
// completion snapshot taken before submission and must fit inside the caller's
// bounded credible queue window. Reaching or passing that target through the
// existing wrap-aware fence extender proves that every earlier CE operation,
// including the sink copy, has retired.
static inline NVIntelCopyU32 nvIntelSharedCopyLifetimePlan(
    const NvIntelSharedCopyProgress &progress,
    NvIntelSharedCopyLifetimePlan *out)
{
    if (!out)
        return NV_INTEL_COPY_ERR_NULL_OUTPUT;
    if (!nvIntelSharedCopyProgressSequenceValid(progress))
        return progress.copySubmissionPublished && !progress.channelSubmitReady
                   ? NV_INTEL_COPY_ERR_CHANNEL
                   : NV_INTEL_COPY_ERR_SEQUENCE;

    if (progress.trackingFencePublishedAfterCopy) {
        if (!progress.maxCredibleCompletionJump ||
            progress.submittedTrackingValue <= progress.completedBeforeSubmission ||
            progress.submittedTrackingValue - progress.completedBeforeSubmission >
                progress.maxCredibleCompletionJump)
            return NV_INTEL_COPY_ERR_TRACKING_TARGET;
    }

    bool completionProven = false;
    NVIntelCopyU64 observedCompleted = progress.completedBeforeSubmission;
    if (progress.completionPayloadReadSucceeded) {
        const NVFenceU32 fenceVerdict = nvAmpereFenceExtendCompleted(
            progress.completedBeforeSubmission,
            progress.observedCompletionPayload,
            progress.maxCredibleCompletionJump,
            &observedCompleted);
        if (fenceVerdict != NV_FENCE_PLAN_OK)
            return NV_INTEL_COPY_ERR_COMPLETION_EVIDENCE;
        completionProven =
            progress.trackingFencePublishedAfterCopy &&
            observedCompleted >= progress.submittedTrackingValue;
    }

    NvIntelSharedCopyLifetimePlan staged = {};
    staged.peerMappingValidated = progress.peerMappingValidated;
    staged.copySubmissionPublished = progress.copySubmissionPublished;
    staged.trackingFencePublishedAfterCopy =
        progress.trackingFencePublishedAfterCopy;
    staged.completionEvidencePresent = progress.completionPayloadReadSucceeded;
    staged.completionProven = completionProven;
    staged.observedCompletedValue = observedCompleted;

    // A validated peer mapping owns a lifetime on the sink backing even before
    // submission. Once copy publication occurs, neither object may be torn down
    // until the ordered tracking fence retires.
    staged.sinkBackingMustRemainPinned = progress.peerMappingValidated;
    staged.nvidiaMayReferenceSink =
        progress.copySubmissionPublished && !completionProven;
    staged.peerMappingMustRemainPinned = staged.nvidiaMayReferenceSink;
    staged.trackingFencePublicationRequired =
        progress.copySubmissionPublished &&
        !progress.trackingFencePublishedAfterCopy;

    staged.intelHandoffAuthorized = completionProven;
    staged.intelGgttMappingMayBegin = completionProven;
    staged.peerMappingTeardownMayBeAttempted =
        progress.peerMappingValidated &&
        (!progress.copySubmissionPublished || completionProven);
    staged.peerMappingTeardownRequiredBeforeNvidiaIovaReuse =
        staged.peerMappingTeardownMayBeAttempted;

    staged.completionUncertain =
        progress.copySubmissionPublished && !completionProven &&
        progress.completionWaitTimedOut;
    staged.quarantineOnAbortOrUncertainCompletion =
        progress.copySubmissionPublished && !completionProven;
    staged.quarantineSinkBacking = staged.completionUncertain;
    staged.quarantinePeerMapping = staged.completionUncertain;

    if (!progress.peerMappingValidated) {
        staged.phase = NV_INTEL_COPY_PHASE_PRE_PEER;
        staged.sinkBackingMustRemainPinned = false;
        staged.peerMappingTeardownMayBeAttempted = false;
        staged.peerMappingTeardownRequiredBeforeNvidiaIovaReuse = false;
        staged.quarantineOnAbortOrUncertainCompletion = false;
    } else if (!progress.copySubmissionPublished) {
        staged.phase = NV_INTEL_COPY_PHASE_PEER_READY;
    } else if (!progress.trackingFencePublishedAfterCopy) {
        staged.phase = NV_INTEL_COPY_PHASE_COPY_UNTRACKED;
    } else if (completionProven) {
        staged.phase = NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL;
        // The NVIDIA reference is retired, but the sink backing remains pinned:
        // ownership is being handed to Intel, not released to the allocator.
        staged.sinkBackingMustRemainPinned = true;
        staged.quarantineOnAbortOrUncertainCompletion = false;
        staged.completionUncertain = false;
        staged.quarantineSinkBacking = false;
        staged.quarantinePeerMapping = false;
    } else if (progress.completionWaitTimedOut) {
        staged.phase = NV_INTEL_COPY_PHASE_UNCERTAIN_QUARANTINE;
    } else {
        staged.phase = NV_INTEL_COPY_PHASE_AWAIT_COMPLETION;
    }

    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_INTEL_COPY_OK;
}

#endif // NV_INTEL_SHARED_COPY_LIFETIME_HPP
