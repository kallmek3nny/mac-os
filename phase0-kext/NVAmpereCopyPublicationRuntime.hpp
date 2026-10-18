// SPDX-License-Identifier: MIT
// Provider-owned GA107 copy publication state.
//
// This state machine sits between a future provider/channel executor and the
// already-validated post-submit publication bridge. It owns publication order
// witnesses so callers cannot manufacture copy-before-fence ordering by filling
// a final snapshot. Channel readiness must arrive through the separately sealed
// provider-owned channel runtime. This state contains no GPU VA, IOVA, GPFIFO
// entry, pushbuffer, register write, work-submission token value, or provider
// pointer, and it authorizes no hardware writes.
#ifndef NV_AMPERE_COPY_PUBLICATION_RUNTIME_HPP
#define NV_AMPERE_COPY_PUBLICATION_RUNTIME_HPP

#include "NVAmpereChannelRuntime.hpp"
#include "NVAmpereCopyPublicationBridge.hpp"

using NVCopyRuntimeU32 = unsigned int;
using NVCopyRuntimeU64 = unsigned long long;

#define NV_COPY_RUNTIME_OK                         0u
#define NV_COPY_RUNTIME_ERR_NULL                  1u
#define NV_COPY_RUNTIME_ERR_ACTIVE                2u
#define NV_COPY_RUNTIME_ERR_PEER_MAPPING          3u
#define NV_COPY_RUNTIME_ERR_CHANNEL               4u
#define NV_COPY_RUNTIME_ERR_SEQUENCE              5u
#define NV_COPY_RUNTIME_ERR_ORDINAL_OVERFLOW      6u
#define NV_COPY_RUNTIME_ERR_BRIDGE                7u
#define NV_COPY_RUNTIME_ERR_NOT_ANCHORED          8u
#define NV_COPY_RUNTIME_ERR_COMPLETION_STARTED    9u
#define NV_COPY_RUNTIME_ERR_UNINITIALIZED         10u
#define NV_COPY_RUNTIME_ERR_REINITIALIZATION      11u
#define NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN   12u
#define NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE   13u
#define NV_COPY_RUNTIME_VERDICT_LIMIT             14u

#define NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED       1u
#define NV_COPY_RUNTIME_EVENT_COPY_RING_STORED         2u
#define NV_COPY_RUNTIME_EVENT_COPY_GPPUT_PUBLISHED     3u
#define NV_COPY_RUNTIME_EVENT_COPY_KICK_ACCEPTED       4u
#define NV_COPY_RUNTIME_EVENT_FENCE_PUSH_PREPARED      5u
#define NV_COPY_RUNTIME_EVENT_FENCE_RING_STORED        6u
#define NV_COPY_RUNTIME_EVENT_FENCE_GPPUT_PUBLISHED    7u
#define NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED      8u
#define NV_COPY_RUNTIME_EVENT_LIMIT                    9u

struct NVAmpereCopyPublicationRuntime {
    // Once initialized, ownership cannot be discarded by reinitializing.
    bool initialized = false;
    // Absorbing stop/loss quarantine; keep publication facts and ordinals.
    bool providerOwnershipUncertain = false;
    NVAmpereCopyPublicationEvidence evidence;
    NVCopyRuntimeU64 nextPublicationOrdinal;
    // Count of accepted milestones; monotonic within the single transaction.
    NVCopyRuntimeU32 acceptedEventCount;
    bool peerMappingValidated;
    bool transactionActive;
    bool anchorMinted;
    bool completionObservationStarted;
    bool runtimeWriteAuthorized;
};

static inline bool nvAmpereCopyRuntimeSucceeded(NVCopyRuntimeU32 verdict)
{
    return verdict == NV_COPY_RUNTIME_OK;
}

static inline const char *nvAmpereCopyRuntimeVerdictName(NVCopyRuntimeU32 verdict)
{
    switch (verdict) {
    case NV_COPY_RUNTIME_OK:                     return "ok";
    case NV_COPY_RUNTIME_ERR_NULL:               return "null";
    case NV_COPY_RUNTIME_ERR_ACTIVE:             return "active";
    case NV_COPY_RUNTIME_ERR_PEER_MAPPING:       return "peer-mapping";
    case NV_COPY_RUNTIME_ERR_CHANNEL:            return "channel";
    case NV_COPY_RUNTIME_ERR_SEQUENCE:           return "sequence";
    case NV_COPY_RUNTIME_ERR_ORDINAL_OVERFLOW:   return "ordinal-overflow";
    case NV_COPY_RUNTIME_ERR_BRIDGE:             return "bridge";
    case NV_COPY_RUNTIME_ERR_NOT_ANCHORED:       return "not-anchored";
    case NV_COPY_RUNTIME_ERR_COMPLETION_STARTED: return "completion-started";
    case NV_COPY_RUNTIME_ERR_UNINITIALIZED:       return "uninitialized";
    case NV_COPY_RUNTIME_ERR_REINITIALIZATION:    return "reinitialization";
    case NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN: return "ownership-uncertain";
    case NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE:  return "inconsistent-state";
    default:                                     return "unknown";
    }
}

static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeInit(
    NVAmpereCopyPublicationRuntime *runtime)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (runtime->initialized)
        return NV_COPY_RUNTIME_ERR_REINITIALIZATION;

    NVAmpereCopyPublicationRuntime staged = {};
    staged.initialized = true;
    staged.nextPublicationOrdinal = 1ull;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// Called by the exact-device provider after it has resolved and retained proof
// of the device-specific peer-mapping capability. Mapping loss may be recorded
// only while no copy transaction is active; changing ownership underneath an
// active transaction is refused atomically.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeSetPeerMapping(
    NVAmpereCopyPublicationRuntime *runtime,
    bool validated)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN;
    if (runtime->transactionActive &&
        validated != runtime->peerMappingValidated)
        return NV_COPY_RUNTIME_ERR_ACTIVE;
    if (runtime->transactionActive &&
        (!runtime->peerMappingValidated ||
         runtime->evidence.peerMappingValidated != runtime->peerMappingValidated))
        return NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    staged.peerMappingValidated = validated;
    if (staged.transactionActive)
        staged.evidence.peerMappingValidated = validated;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// Begin exactly one provider-owned publication transaction. The channel must
// already have been built monotonically and sealed by NVAmpereChannelRuntime;
// a caller can no longer inject a complete free-form readiness snapshot here.
// Raw mappings/tokens remain in the future executor and only bounded presence
// evidence crosses this boundary. A second transaction cannot begin until a
// later retirement owner explicitly adds a safe reuse path.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeBegin(
    NVAmpereCopyPublicationRuntime *runtime,
    const NVAmpereChannelRuntime &channelRuntime,
    NVIntelCopyU64 completedBeforeSubmission,
    NVIntelCopyU64 submittedTrackingValue,
    NVIntelCopyU64 maxCredibleCompletionJump)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN;
    if (runtime->transactionActive)
        return NV_COPY_RUNTIME_ERR_ACTIVE;
    if (!runtime->peerMappingValidated)
        return NV_COPY_RUNTIME_ERR_PEER_MAPPING;

    NVAmpereChannelEvidence channel = {};
    if (nvAmpereChannelRuntimeCopyEvidence(channelRuntime, &channel) !=
        NV_CHANNEL_RUNTIME_OK)
        return NV_COPY_RUNTIME_ERR_CHANNEL;

    // Defense in depth: a sealed runtime is still re-evaluated at this exact
    // transaction boundary before its evidence is copied into publication.
    NVAmpereChannelReadiness readiness = {};
    if (nvAmpereEvaluateChannel(channel, &readiness) != NV_CHANNEL_READY_OK ||
        !readiness.allocationReady || !readiness.ceBoundReady ||
        !readiness.submitReady)
        return NV_COPY_RUNTIME_ERR_CHANNEL;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    staged.evidence = {};
    staged.acceptedEventCount = 0u;
    staged.evidence.channel = channel;
    staged.evidence.peerMappingValidated = true;
    staged.evidence.completedBeforeSubmission = completedBeforeSubmission;
    staged.evidence.submittedTrackingValue = submittedTrackingValue;
    staged.evidence.maxCredibleCompletionJump = maxCredibleCompletionJump;
    staged.transactionActive = true;
    staged.anchorMinted = false;
    staged.completionObservationStarted = false;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// Loss of the exact-device provider is not retirement evidence. This
// absorbing quarantine keeps all historical publication and completion state,
// but refuses every later milestone, anchor, or completion transition.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeMarkOwnershipUncertain(
    NVAmpereCopyPublicationRuntime *runtime)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_OK;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    staged.providerOwnershipUncertain = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// The externally inspectable evidence must always agree with the private
// progression counter. This detects accidental snapshot drift before any new
// publication witness or completion transition. It is NOT a substitute for
// true provider-owned milestone observations: no writes are authorized here.
static inline bool nvAmpereCopyRuntimeEvidenceConsistent(
    const NVAmpereCopyPublicationRuntime &runtime)
{
    if (!runtime.initialized || !runtime.transactionActive ||
        runtime.providerOwnershipUncertain || runtime.runtimeWriteAuthorized ||
        !runtime.peerMappingValidated || !runtime.evidence.peerMappingValidated ||
        runtime.acceptedEventCount > 8u)
        return false;

    const NVCopyRuntimeU32 stage = runtime.acceptedEventCount;
    const NVAmpereCopyPublicationEvidence &e = runtime.evidence;
    if (e.copyPushPrepared != (stage >= 1u) ||
        e.copyRingEntryStored != (stage >= 2u) ||
        e.copyGpPutPublished != (stage >= 3u) ||
        e.copySubmissionKicked != (stage >= 4u) ||
        e.trackingFencePushPrepared != (stage >= 5u) ||
        e.trackingFenceRingEntryStored != (stage >= 6u) ||
        e.trackingFenceGpPutPublished != (stage >= 7u) ||
        e.trackingFenceSubmissionKicked != (stage >= 8u))
        return false;

    // The runtime permits one transaction and never resets/reuses its ring.
    // Its only accepted kick witnesses are therefore ordinals one and two.
    if (e.copyPublicationOrdinal != (stage >= 4u ? 1ull : 0ull) ||
        e.trackingFencePublicationOrdinal != (stage >= 8u ? 2ull : 0ull))
        return false;
    if (stage >= 4u &&
        runtime.nextPublicationOrdinal != (stage >= 8u ? 3ull : 2ull))
        return false;
    if ((runtime.anchorMinted && stage != 8u) ||
        (runtime.completionObservationStarted && !runtime.anchorMinted))
        return false;

    // Snapshot drift must not reintroduce previously invalid channel facts.
    NVAmpereChannelReadiness readiness = {};
    return nvAmpereEvaluateChannel(e.channel, &readiness) ==
               NV_CHANNEL_READY_OK &&
           readiness.allocationReady && readiness.ceBoundReady &&
           readiness.submitReady;
}

static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeExpectedEvent(
    const NVAmpereCopyPublicationRuntime &runtime)
{
    if (!nvAmpereCopyRuntimeEvidenceConsistent(runtime) ||
        runtime.anchorMinted || runtime.completionObservationStarted)
        return 0u;
    const NVAmpereCopyPublicationEvidence &e = runtime.evidence;
    if (!e.copyPushPrepared)
        return NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED;
    if (!e.copyRingEntryStored)
        return NV_COPY_RUNTIME_EVENT_COPY_RING_STORED;
    if (!e.copyGpPutPublished)
        return NV_COPY_RUNTIME_EVENT_COPY_GPPUT_PUBLISHED;
    if (!e.copySubmissionKicked)
        return NV_COPY_RUNTIME_EVENT_COPY_KICK_ACCEPTED;
    if (!e.trackingFencePushPrepared)
        return NV_COPY_RUNTIME_EVENT_FENCE_PUSH_PREPARED;
    if (!e.trackingFenceRingEntryStored)
        return NV_COPY_RUNTIME_EVENT_FENCE_RING_STORED;
    if (!e.trackingFenceGpPutPublished)
        return NV_COPY_RUNTIME_EVENT_FENCE_GPPUT_PUBLISHED;
    if (!e.trackingFenceSubmissionKicked)
        return NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED;
    return 0u;
}

// Record one provider-visible milestone after that step has actually completed.
// The runtime, not the caller, assigns publication ordinals on accepted kicks.
// Every refusal leaves the runtime unchanged.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeAdvance(
    NVAmpereCopyPublicationRuntime *runtime,
    NVCopyRuntimeU32 event)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN;
    if (!runtime->transactionActive || runtime->anchorMinted ||
        runtime->completionObservationStarted)
        return NV_COPY_RUNTIME_ERR_SEQUENCE;
    if (!nvAmpereCopyRuntimeEvidenceConsistent(*runtime))
        return NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE;
    if (event != nvAmpereCopyRuntimeExpectedEvent(*runtime))
        return NV_COPY_RUNTIME_ERR_SEQUENCE;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    switch (event) {
    case NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED:
        staged.evidence.copyPushPrepared = true;
        break;
    case NV_COPY_RUNTIME_EVENT_COPY_RING_STORED:
        staged.evidence.copyRingEntryStored = true;
        break;
    case NV_COPY_RUNTIME_EVENT_COPY_GPPUT_PUBLISHED:
        staged.evidence.copyGpPutPublished = true;
        break;
    case NV_COPY_RUNTIME_EVENT_COPY_KICK_ACCEPTED:
        if (!staged.nextPublicationOrdinal ||
            staged.nextPublicationOrdinal == ~0ull)
            return NV_COPY_RUNTIME_ERR_ORDINAL_OVERFLOW;
        staged.evidence.copySubmissionKicked = true;
        staged.evidence.copyPublicationOrdinal =
            staged.nextPublicationOrdinal++;
        break;
    case NV_COPY_RUNTIME_EVENT_FENCE_PUSH_PREPARED:
        staged.evidence.trackingFencePushPrepared = true;
        break;
    case NV_COPY_RUNTIME_EVENT_FENCE_RING_STORED:
        staged.evidence.trackingFenceRingEntryStored = true;
        break;
    case NV_COPY_RUNTIME_EVENT_FENCE_GPPUT_PUBLISHED:
        staged.evidence.trackingFenceGpPutPublished = true;
        break;
    case NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED:
        if (!staged.nextPublicationOrdinal ||
            staged.nextPublicationOrdinal == ~0ull)
            return NV_COPY_RUNTIME_ERR_ORDINAL_OVERFLOW;
        staged.evidence.trackingFenceSubmissionKicked = true;
        staged.evidence.trackingFencePublicationOrdinal =
            staged.nextPublicationOrdinal++;
        break;
    default:
        return NV_COPY_RUNTIME_ERR_SEQUENCE;
    }

    ++staged.acceptedEventCount;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// Mint the opaque pre-completion anchor from the provider-owned snapshot.
// Runtime state and output are committed together only after the inherited
// bridge recomputes and accepts the exact lifetime/provenance facts.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeMintAnchor(
    NVAmpereCopyPublicationRuntime *runtime,
    NVIntelHandoffU64 handoffEpoch,
    NVIntelHandoffU64 transactionToken,
    NVAmpereCopyPublicationBridge *out)
{
    if (!runtime || !out)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN;
    if (!runtime->transactionActive || runtime->anchorMinted)
        return NV_COPY_RUNTIME_ERR_SEQUENCE;
    if (runtime->completionObservationStarted)
        return NV_COPY_RUNTIME_ERR_COMPLETION_STARTED;
    if (!nvAmpereCopyRuntimeEvidenceConsistent(*runtime))
        return NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE;
    if (nvAmpereCopyRuntimeExpectedEvent(*runtime) != 0u)
        return NV_COPY_RUNTIME_ERR_SEQUENCE;

    NVAmpereCopyPublicationBridge bridge = {};
    if (nvAmpereBuildCopyPublicationBridge(
            runtime->evidence, handoffEpoch, transactionToken, &bridge) !=
        NV_COPY_PUBLICATION_OK)
        return NV_COPY_RUNTIME_ERR_BRIDGE;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    staged.anchorMinted = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    *out = bridge;
    return NV_COPY_RUNTIME_OK;
}

// Completion reads are deliberately a separate post-anchor transition. This
// does not perform a wait/read and grants no write authority; it only prevents
// any later publication mutation once completion observation has begun.
static inline NVCopyRuntimeU32 nvAmpereCopyRuntimeBeginCompletionObservation(
    NVAmpereCopyPublicationRuntime *runtime)
{
    if (!runtime)
        return NV_COPY_RUNTIME_ERR_NULL;
    if (!runtime->initialized)
        return NV_COPY_RUNTIME_ERR_UNINITIALIZED;
    if (runtime->providerOwnershipUncertain)
        return NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN;
    if (!runtime->transactionActive || !runtime->anchorMinted)
        return NV_COPY_RUNTIME_ERR_NOT_ANCHORED;
    if (runtime->completionObservationStarted)
        return NV_COPY_RUNTIME_ERR_COMPLETION_STARTED;
    if (!nvAmpereCopyRuntimeEvidenceConsistent(*runtime))
        return NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE;

    NVAmpereCopyPublicationRuntime staged = *runtime;
    staged.completionObservationStarted = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_COPY_RUNTIME_OK;
}

// A stop/restart cannot substitute for channel or transaction retirement.
// The provider may enter start() only while it owns no prior channel/copy
// lifetime and is not retaining a prior PCI dependency. The historical proof
// fields are intentionally inspected rather than zeroed.
static inline bool nvAmpereProviderRuntimeMayStart(
    const NVAmpereChannelRuntime &channel,
    const NVAmpereCopyPublicationRuntime &publication,
    bool lifetimePinned,
    bool pciDependencyRetained)
{
    return channel.initialized && publication.initialized &&
           !lifetimePinned && !pciDependencyRetained &&
           !channel.ownershipUncertain &&
           !publication.providerOwnershipUncertain &&
           !channel.allocationObserved &&
           !channel.submissionEndpointObserved && !channel.sealed &&
           !publication.transactionActive && !publication.anchorMinted &&
           !publication.completionObservationStarted;
}

#endif // NV_AMPERE_COPY_PUBLICATION_RUNTIME_HPP
