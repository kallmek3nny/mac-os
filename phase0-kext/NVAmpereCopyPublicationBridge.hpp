// SPDX-License-Identifier: MIT
// Pure post-submit publication bridge for a GA107 copy into shared system memory.
//
// This boundary consumes only runtime milestone evidence after provider-owned
// writes have occurred. It proves channel readiness and publication ordering,
// then produces the pre-completion NvIntelSharedCopyProgress snapshot and its
// opaque provenance anchor. No GPU VA, IOVA, pushbuffer, GPFIFO entry, register
// write, work-submission token, or provider pointer crosses this API.
#ifndef NV_AMPERE_COPY_PUBLICATION_BRIDGE_HPP
#define NV_AMPERE_COPY_PUBLICATION_BRIDGE_HPP

#include "NVAmpereChannel.hpp"
#include "NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp"

using NVCopyPublicationU32 = unsigned int;
using NVCopyPublicationU64 = unsigned long long;

#define NV_COPY_PUBLICATION_OK                     0u
#define NV_COPY_PUBLICATION_ERR_NULL_OUTPUT        1u
#define NV_COPY_PUBLICATION_ERR_CHANNEL            2u
#define NV_COPY_PUBLICATION_ERR_PEER_MAPPING       3u
#define NV_COPY_PUBLICATION_ERR_SEQUENCE           4u
#define NV_COPY_PUBLICATION_ERR_ANCHOR             5u
#define NV_COPY_PUBLICATION_VERDICT_LIMIT          5u

// Milestones are monotonic observations supplied by the future runtime executor.
// "published" means the provider-visible publication step has completed, not
// merely that a host-side plan exists. Publication ordinals are executor-owned
// monotonic witnesses recorded only after the corresponding provider kick has
// been accepted. They never contain an address, token, or command-stream value.
struct NVAmpereCopyPublicationEvidence {
    NVAmpereChannelEvidence channel;
    bool peerMappingValidated;

    bool copyPushPrepared;
    bool copyRingEntryStored;
    bool copyGpPutPublished;
    bool copySubmissionKicked;
    NVCopyPublicationU64 copyPublicationOrdinal;

    bool trackingFencePushPrepared;
    bool trackingFenceRingEntryStored;
    bool trackingFenceGpPutPublished;
    bool trackingFenceSubmissionKicked;
    NVCopyPublicationU64 trackingFencePublicationOrdinal;

    NVIntelCopyU64 completedBeforeSubmission;
    NVIntelCopyU64 submittedTrackingValue;
    NVIntelCopyU64 maxCredibleCompletionJump;
};

struct NVAmpereCopyPublicationBridge {
    NVAmpereChannelReadiness channelReadiness;
    NvIntelSharedCopyProgress copyProgress;
    NvIntelSharedCopyTransactionAnchor transactionAnchor;

    bool copyPublicationProven;
    bool trackingFencePublicationProven;
    bool completionObservationMayBegin;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvAmpereCopyPublicationSucceeded(NVCopyPublicationU32 verdict)
{
    return verdict == NV_COPY_PUBLICATION_OK;
}

static inline const char *nvAmpereCopyPublicationVerdictName(
    NVCopyPublicationU32 verdict)
{
    switch (verdict) {
    case NV_COPY_PUBLICATION_OK:               return "ok";
    case NV_COPY_PUBLICATION_ERR_NULL_OUTPUT:  return "null-output";
    case NV_COPY_PUBLICATION_ERR_CHANNEL:      return "channel";
    case NV_COPY_PUBLICATION_ERR_PEER_MAPPING: return "peer-mapping";
    case NV_COPY_PUBLICATION_ERR_SEQUENCE:     return "sequence";
    case NV_COPY_PUBLICATION_ERR_ANCHOR:       return "anchor";
    default:                                   return "unknown";
    }
}

static inline bool nvAmpereCopyPublicationSequenceValid(
    const NVAmpereCopyPublicationEvidence &e)
{
    if (e.copyRingEntryStored && !e.copyPushPrepared)
        return false;
    if (e.copyGpPutPublished && !e.copyRingEntryStored)
        return false;
    if (e.copySubmissionKicked && !e.copyGpPutPublished)
        return false;
    if (e.copySubmissionKicked != (e.copyPublicationOrdinal != 0ull))
        return false;

    if (e.trackingFenceRingEntryStored &&
        (!e.trackingFencePushPrepared || !e.copySubmissionKicked))
        return false;
    if (e.trackingFenceGpPutPublished && !e.trackingFenceRingEntryStored)
        return false;
    if (e.trackingFenceSubmissionKicked &&
        (!e.trackingFenceGpPutPublished || !e.copySubmissionKicked))
        return false;
    if (e.trackingFenceSubmissionKicked !=
        (e.trackingFencePublicationOrdinal != 0ull))
        return false;

    // A final all-true snapshot alone cannot prove temporal order. The executor
    // must supply distinct monotonic publication witnesses showing that the copy
    // kick became provider-visible before the tracking-fence kick.
    if (e.trackingFenceSubmissionKicked &&
        e.copyPublicationOrdinal >= e.trackingFencePublicationOrdinal)
        return false;

    return true;
}

// Build the handoff anchor only after both ordered submissions are provider-
// visible and before any bounded completion wait starts. Refusals are atomic:
// *out is assigned only after the inherited lifetime planner accepts the exact
// publication snapshot and mints the opaque epoch/token binding.
static inline NVCopyPublicationU32 nvAmpereBuildCopyPublicationBridge(
    const NVAmpereCopyPublicationEvidence &evidence,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    NVAmpereCopyPublicationBridge *out)
{
    if (!out)
        return NV_COPY_PUBLICATION_ERR_NULL_OUTPUT;

    NVAmpereChannelReadiness readiness = {};
    if (nvAmpereEvaluateChannel(evidence.channel, &readiness) !=
            NV_CHANNEL_READY_OK ||
        !readiness.allocationReady || !readiness.ceBoundReady ||
        !readiness.submitReady)
        return NV_COPY_PUBLICATION_ERR_CHANNEL;

    if (!evidence.peerMappingValidated)
        return NV_COPY_PUBLICATION_ERR_PEER_MAPPING;
    if (!nvAmpereCopyPublicationSequenceValid(evidence))
        return NV_COPY_PUBLICATION_ERR_SEQUENCE;

    // A publication anchor is meaningful only at the fully-published boundary.
    if (!evidence.copyPushPrepared || !evidence.copyRingEntryStored ||
        !evidence.copyGpPutPublished || !evidence.copySubmissionKicked ||
        !evidence.trackingFencePushPrepared ||
        !evidence.trackingFenceRingEntryStored ||
        !evidence.trackingFenceGpPutPublished ||
        !evidence.trackingFenceSubmissionKicked)
        return NV_COPY_PUBLICATION_ERR_SEQUENCE;

    NvIntelSharedCopyProgress progress = {};
    progress.peerMappingValidated = evidence.peerMappingValidated;
    progress.channelSubmitReady = readiness.submitReady;
    progress.copySubmissionPublished = evidence.copySubmissionKicked;
    progress.trackingFencePublishedAfterCopy =
        evidence.copyPublicationOrdinal <
        evidence.trackingFencePublicationOrdinal;
    progress.completedBeforeSubmission = evidence.completedBeforeSubmission;
    progress.submittedTrackingValue = evidence.submittedTrackingValue;
    progress.maxCredibleCompletionJump = evidence.maxCredibleCompletionJump;

    NvIntelSharedCopyTransactionAnchor anchor = {};
    if (nvIntelBuildSharedCopyTransactionAnchor(
            &progress, handoffEpoch, transactionToken, &anchor) !=
        NV_INTEL_ANCHORED_PROVENANCE_OK)
        return NV_COPY_PUBLICATION_ERR_ANCHOR;

    NVAmpereCopyPublicationBridge staged = {};
    staged.channelReadiness = readiness;
    staged.copyProgress = progress;
    staged.transactionAnchor = anchor;
    staged.copyPublicationProven = true;
    staged.trackingFencePublicationProven = true;
    staged.completionObservationMayBegin = true;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_COPY_PUBLICATION_OK;
}

#endif // NV_AMPERE_COPY_PUBLICATION_BRIDGE_HPP
