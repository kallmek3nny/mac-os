// SPDX-License-Identifier: MIT
#include "NVAmpereCopyPublicationBridge.hpp"

#include <cstdio>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static constexpr NVIntelHandoffU64 kEpoch = 0x1111222233334444ull;
static constexpr NVIntelHandoffU64 kToken = 0x5555666677778888ull;

static void check(bool condition, const char *label)
{
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("FAIL: %s\n", label);
    }
}

static NVAmpereCopyPublicationEvidence validEvidence()
{
    NVAmpereCopyPublicationEvidence e = {};
    e.channel.channelClassNum = NV_SUBMIT_C56F_CLASS_ID;
    e.channel.numGpFifoEntries = 1024u;
    e.channel.gpGetMapped = true;
    e.channel.gpPutMapped = true;
    e.channel.gpFifoMapped = true;
    e.channel.workSubmissionMapped = true;
    e.channel.workSubmissionTokenReady = true;
    e.peerMappingValidated = true;

    e.copyPushPrepared = true;
    e.copyRingEntryStored = true;
    e.copyGpPutPublished = true;
    e.copySubmissionKicked = true;

    e.trackingFencePushPrepared = true;
    e.trackingFenceRingEntryStored = true;
    e.trackingFenceGpPutPublished = true;
    e.trackingFenceSubmissionKicked = true;

    e.completedBeforeSubmission = 100ull;
    e.submittedTrackingValue = 101ull;
    e.maxCredibleCompletionJump = 8ull;
    return e;
}

static void poison(NVAmpereCopyPublicationBridge *out)
{
    NVAmpereCopyPublicationBridge staged = {};
    staged.channelReadiness.allocationReady = true;
    staged.channelReadiness.ceBoundReady = false;
    staged.channelReadiness.submitReady = true;

    staged.copyProgress.peerMappingValidated = true;
    staged.copyProgress.channelSubmitReady = true;
    staged.copyProgress.copySubmissionPublished = true;
    staged.copyProgress.trackingFencePublishedAfterCopy = true;
    staged.copyProgress.completedBeforeSubmission = 900ull;
    staged.copyProgress.submittedTrackingValue = 901ull;
    staged.copyProgress.observedCompletionPayload = 0x5a5a5a5au;
    staged.copyProgress.maxCredibleCompletionJump = 16ull;

    // Mint a real non-default opaque anchor so atomic-refusal checks cover the
    // non-trivial member semantically instead of inspecting its object bytes.
    NvIntelSharedCopyProgress anchorProgress = staged.copyProgress;
    anchorProgress.observedCompletionPayload = 0u;
    NvIntelSharedCopyTransactionAnchor sentinelAnchor = {};
    if (nvIntelBuildSharedCopyTransactionAnchor(
            &anchorProgress,
            0xabcdef0123456789ull,
            0x123456789abcdef0ull,
            &sentinelAnchor) == NV_INTEL_ANCHORED_PROVENANCE_OK)
        staged.transactionAnchor = sentinelAnchor;

    staged.copyPublicationProven = false;
    staged.trackingFencePublicationProven = true;
    staged.completionObservationMayBegin = false;
    staged.runtimeWriteAuthorized = true;
    staged.valid = true;
    *out = staged;
}

static bool sameState(const NVAmpereCopyPublicationBridge &a,
                      const NVAmpereCopyPublicationBridge &b)
{
    return a.channelReadiness.allocationReady ==
               b.channelReadiness.allocationReady &&
           a.channelReadiness.ceBoundReady == b.channelReadiness.ceBoundReady &&
           a.channelReadiness.submitReady == b.channelReadiness.submitReady &&
           a.copyProgress.peerMappingValidated ==
               b.copyProgress.peerMappingValidated &&
           a.copyProgress.channelSubmitReady ==
               b.copyProgress.channelSubmitReady &&
           a.copyProgress.copySubmissionPublished ==
               b.copyProgress.copySubmissionPublished &&
           a.copyProgress.trackingFencePublishedAfterCopy ==
               b.copyProgress.trackingFencePublishedAfterCopy &&
           a.copyProgress.boundedCompletionWaitUsed ==
               b.copyProgress.boundedCompletionWaitUsed &&
           a.copyProgress.boundedCompletionWaitFinished ==
               b.copyProgress.boundedCompletionWaitFinished &&
           a.copyProgress.completionPayloadReadSucceeded ==
               b.copyProgress.completionPayloadReadSucceeded &&
           a.copyProgress.completionWaitTimedOut ==
               b.copyProgress.completionWaitTimedOut &&
           a.copyProgress.completedBeforeSubmission ==
               b.copyProgress.completedBeforeSubmission &&
           a.copyProgress.submittedTrackingValue ==
               b.copyProgress.submittedTrackingValue &&
           a.copyProgress.observedCompletionPayload ==
               b.copyProgress.observedCompletionPayload &&
           a.copyProgress.maxCredibleCompletionJump ==
               b.copyProgress.maxCredibleCompletionJump &&
           a.transactionAnchor.handoffEpoch() ==
               b.transactionAnchor.handoffEpoch() &&
           a.transactionAnchor.transactionToken() ==
               b.transactionAnchor.transactionToken() &&
           a.transactionAnchor.completedBeforeSubmission() ==
               b.transactionAnchor.completedBeforeSubmission() &&
           a.transactionAnchor.submittedTrackingValue() ==
               b.transactionAnchor.submittedTrackingValue() &&
           a.transactionAnchor.maxCredibleCompletionJump() ==
               b.transactionAnchor.maxCredibleCompletionJump() &&
           a.transactionAnchor.peerMappingValidated() ==
               b.transactionAnchor.peerMappingValidated() &&
           a.transactionAnchor.channelSubmitReady() ==
               b.transactionAnchor.channelSubmitReady() &&
           a.transactionAnchor.copySubmissionPublished() ==
               b.transactionAnchor.copySubmissionPublished() &&
           a.transactionAnchor.trackingFencePublishedAfterCopy() ==
               b.transactionAnchor.trackingFencePublishedAfterCopy() &&
           a.transactionAnchor.valid() == b.transactionAnchor.valid() &&
           a.copyPublicationProven == b.copyPublicationProven &&
           a.trackingFencePublicationProven ==
               b.trackingFencePublicationProven &&
           a.completionObservationMayBegin ==
               b.completionObservationMayBegin &&
           a.runtimeWriteAuthorized == b.runtimeWriteAuthorized &&
           a.valid == b.valid;
}

static void testValidPublicationBuildsPreEvidenceAnchor()
{
    std::puts("1. ordered publication builds a pre-evidence handoff anchor");
    const auto e = validEvidence();
    NVAmpereCopyPublicationBridge out = {};

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_OK,
          "valid ordered publication accepted");
    check(out.valid && out.copyPublicationProven &&
              out.trackingFencePublicationProven &&
              out.completionObservationMayBegin,
          "bridge opens only the completion-observation boundary");
    check(out.channelReadiness.allocationReady &&
              out.channelReadiness.ceBoundReady &&
              out.channelReadiness.submitReady,
          "channel readiness is inherited from channel contract");
    check(out.copyProgress.peerMappingValidated &&
              out.copyProgress.channelSubmitReady &&
              out.copyProgress.copySubmissionPublished &&
              out.copyProgress.trackingFencePublishedAfterCopy,
          "published milestones become copy lifetime progress");
    check(!out.copyProgress.boundedCompletionWaitUsed &&
              !out.copyProgress.boundedCompletionWaitFinished &&
              !out.copyProgress.completionPayloadReadSucceeded &&
              !out.copyProgress.completionWaitTimedOut,
          "bridge remains strictly before completion observation");
    check(out.copyProgress.completedBeforeSubmission == 100ull &&
              out.copyProgress.submittedTrackingValue == 101ull &&
              out.copyProgress.maxCredibleCompletionJump == 8ull,
          "tracking-counter identity is preserved");
    check(out.transactionAnchor.valid() &&
              out.transactionAnchor.handoffEpoch() == kEpoch &&
              out.transactionAnchor.transactionToken() == kToken &&
              out.transactionAnchor.submittedTrackingValue() == 101ull,
          "opaque anchor binds the published transaction");
    check(!out.runtimeWriteAuthorized &&
              !out.transactionAnchor.runtimeWriteAuthorized(),
          "bridge and anchor never authorize runtime writes");
}

static void testChannelRefusalIsAtomic()
{
    std::puts("2. incomplete channel readiness refuses atomically");
    auto e = validEvidence();
    e.channel.workSubmissionTokenReady = false;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_CHANNEL,
          "missing provider submission token is refused");
    check(sameState(out, before), "channel refusal leaves output untouched");
}

static void testPeerMappingRequiredAtomically()
{
    std::puts("3. peer ownership must exist before publication handoff");
    auto e = validEvidence();
    e.peerMappingValidated = false;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_PEER_MAPPING,
          "missing peer mapping proof is refused");
    check(sameState(out, before), "peer refusal leaves output untouched");
}

static void testCopyPublicationOrderingRefuses()
{
    std::puts("4. copy publication ordering is monotonic");
    auto e = validEvidence();
    e.copyRingEntryStored = false;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_SEQUENCE,
          "gpPut cannot be published without ring entry storage");
    check(sameState(out, before), "copy ordering refusal is atomic");
}

static void testFenceCannotPublishBeforeCopyKick()
{
    std::puts("5. tracking fence cannot become visible before copy submission");
    auto e = validEvidence();
    e.copySubmissionKicked = false;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_SEQUENCE,
          "fence ring publication before copy kick is refused");
    check(sameState(out, before), "cross-submission ordering refusal is atomic");
}

static void testPartialPublicationCannotMintAnchor()
{
    std::puts("6. partial provider publication cannot mint transaction identity");
    auto e = validEvidence();
    e.trackingFenceSubmissionKicked = false;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_SEQUENCE,
          "anchor waits for ordered tracking-fence publication");
    check(sameState(out, before), "partial publication leaves output untouched");
}

static void testInvalidTrackingTargetRefusesThroughAnchor()
{
    std::puts("7. fence target validity is inherited from lifetime planner");
    auto e = validEvidence();
    e.submittedTrackingValue = e.completedBeforeSubmission;
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_ANCHOR,
          "non-new tracking target cannot mint an anchor");
    check(sameState(out, before), "tracking-target refusal is atomic");
}

static void testZeroIdentityRefusesThroughAnchor()
{
    std::puts("8. zero epoch/token cannot be minted after publication");
    const auto e = validEvidence();
    NVAmpereCopyPublicationBridge out = {};
    poison(&out);
    const auto before = out;

    check(nvAmpereBuildCopyPublicationBridge(e, 0ull, kToken, &out) ==
              NV_COPY_PUBLICATION_ERR_ANCHOR,
          "zero handoff epoch refused");
    check(sameState(out, before), "zero-epoch refusal is atomic");

    check(nvAmpereBuildCopyPublicationBridge(e, kEpoch, 0ull, &out) ==
              NV_COPY_PUBLICATION_ERR_ANCHOR,
          "zero transaction token refused");
    check(sameState(out, before), "zero-token refusal is atomic");
}

int main()
{
    testValidPublicationBuildsPreEvidenceAnchor();
    testChannelRefusalIsAtomic();
    testPeerMappingRequiredAtomically();
    testCopyPublicationOrderingRefuses();
    testFenceCannotPublishBeforeCopyKick();
    testPartialPublicationCannotMintAnchor();
    testInvalidTrackingTargetRefusesThroughAnchor();
    testZeroIdentityRefusesThroughAnchor();

    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0u ? 0 : 1;
}
