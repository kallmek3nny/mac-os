// SPDX-License-Identifier: MIT
#include "NVAmpereCopyPublicationRuntime.hpp"

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

static NVAmpereChannelEvidence readyChannel()
{
    NVAmpereChannelEvidence e = {};
    e.channelClassNum = NV_SUBMIT_C56F_CLASS_ID;
    e.numGpFifoEntries = 1024u;
    e.gpGetMapped = true;
    e.gpPutMapped = true;
    e.gpFifoMapped = true;
    e.workSubmissionMapped = true;
    e.workSubmissionTokenReady = true;
    return e;
}

static bool sameRuntime(const NVAmpereCopyPublicationRuntime &a,
                        const NVAmpereCopyPublicationRuntime &b)
{
    const auto &x = a.evidence;
    const auto &y = b.evidence;
    return x.channel.channelClassNum == y.channel.channelClassNum &&
           x.channel.numGpFifoEntries == y.channel.numGpFifoEntries &&
           x.channel.gpGetMapped == y.channel.gpGetMapped &&
           x.channel.gpPutMapped == y.channel.gpPutMapped &&
           x.channel.gpFifoMapped == y.channel.gpFifoMapped &&
           x.channel.workSubmissionMapped == y.channel.workSubmissionMapped &&
           x.channel.workSubmissionTokenReady == y.channel.workSubmissionTokenReady &&
           x.peerMappingValidated == y.peerMappingValidated &&
           x.copyPushPrepared == y.copyPushPrepared &&
           x.copyRingEntryStored == y.copyRingEntryStored &&
           x.copyGpPutPublished == y.copyGpPutPublished &&
           x.copySubmissionKicked == y.copySubmissionKicked &&
           x.copyPublicationOrdinal == y.copyPublicationOrdinal &&
           x.trackingFencePushPrepared == y.trackingFencePushPrepared &&
           x.trackingFenceRingEntryStored == y.trackingFenceRingEntryStored &&
           x.trackingFenceGpPutPublished == y.trackingFenceGpPutPublished &&
           x.trackingFenceSubmissionKicked == y.trackingFenceSubmissionKicked &&
           x.trackingFencePublicationOrdinal == y.trackingFencePublicationOrdinal &&
           x.completedBeforeSubmission == y.completedBeforeSubmission &&
           x.submittedTrackingValue == y.submittedTrackingValue &&
           x.maxCredibleCompletionJump == y.maxCredibleCompletionJump &&
           a.nextPublicationOrdinal == b.nextPublicationOrdinal &&
           a.peerMappingValidated == b.peerMappingValidated &&
           a.transactionActive == b.transactionActive &&
           a.anchorMinted == b.anchorMinted &&
           a.completionObservationStarted == b.completionObservationStarted &&
           a.runtimeWriteAuthorized == b.runtimeWriteAuthorized;
}

static bool advanceAll(NVAmpereCopyPublicationRuntime *runtime)
{
    for (NVCopyRuntimeU32 event = NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED;
         event < NV_COPY_RUNTIME_EVENT_LIMIT;
         ++event) {
        if (nvAmpereCopyRuntimeAdvance(runtime, event) != NV_COPY_RUNTIME_OK)
            return false;
    }
    return true;
}

static NVAmpereCopyPublicationRuntime begunRuntime()
{
    NVAmpereCopyPublicationRuntime runtime = {};
    check(nvAmpereCopyRuntimeInit(&runtime) == NV_COPY_RUNTIME_OK,
          "runtime initializes");
    check(nvAmpereCopyRuntimeSetPeerMapping(&runtime, true) == NV_COPY_RUNTIME_OK,
          "provider records peer mapping");
    const auto channel = readyChannel();
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_OK,
          "provider begins transaction");
    return runtime;
}

static void testInitAndOwnershipGate()
{
    std::puts("1. provider ownership gates transaction start");
    NVAmpereCopyPublicationRuntime runtime = {};
    check(nvAmpereCopyRuntimeInit(&runtime) == NV_COPY_RUNTIME_OK,
          "init accepted");
    check(runtime.nextPublicationOrdinal == 1ull,
          "provider ordinal starts nonzero");
    check(!runtime.peerMappingValidated && !runtime.transactionActive &&
              !runtime.runtimeWriteAuthorized,
          "init grants no ownership or write authority");

    const auto channel = readyChannel();
    const auto before = runtime;
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_PEER_MAPPING,
          "transaction refuses before peer mapping proof");
    check(sameRuntime(runtime, before), "peer refusal is atomic");
}

static void testChannelGateIsAtomic()
{
    std::puts("2. real channel readiness is required before publication");
    NVAmpereCopyPublicationRuntime runtime = {};
    nvAmpereCopyRuntimeInit(&runtime);
    nvAmpereCopyRuntimeSetPeerMapping(&runtime, true);
    auto channel = readyChannel();
    channel.workSubmissionTokenReady = false;
    const auto before = runtime;

    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_CHANNEL,
          "missing provider submission token refuses");
    check(sameRuntime(runtime, before), "channel refusal is atomic");
}

static void testRuntimeOwnsPublicationOrder()
{
    std::puts("3. runtime mints copy-before-fence publication witnesses");
    auto runtime = begunRuntime();

    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_RING_STORED) ==
              NV_COPY_RUNTIME_ERR_SEQUENCE,
          "caller cannot skip copy push preparation");
    check(runtime.nextPublicationOrdinal == 1ull &&
              !runtime.evidence.copyRingEntryStored,
          "out-of-order refusal leaves witness state unchanged");

    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED) ==
              NV_COPY_RUNTIME_OK,
          "copy push prepared");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_RING_STORED) ==
              NV_COPY_RUNTIME_OK,
          "copy ring entry stored");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_GPPUT_PUBLISHED) ==
              NV_COPY_RUNTIME_OK,
          "copy gpPut published");
    check(runtime.evidence.copyPublicationOrdinal == 0ull,
          "ordinal is not minted before accepted kick");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_KICK_ACCEPTED) ==
              NV_COPY_RUNTIME_OK,
          "copy kick accepted");
    check(runtime.evidence.copyPublicationOrdinal == 1ull &&
              runtime.nextPublicationOrdinal == 2ull,
          "runtime owns copy publication ordinal");

    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_FENCE_PUSH_PREPARED) ==
              NV_COPY_RUNTIME_OK,
          "fence push prepared only after copy kick");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_FENCE_RING_STORED) ==
              NV_COPY_RUNTIME_OK,
          "fence ring entry stored");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_FENCE_GPPUT_PUBLISHED) ==
              NV_COPY_RUNTIME_OK,
          "fence gpPut published");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED) ==
              NV_COPY_RUNTIME_OK,
          "fence kick accepted");
    check(runtime.evidence.trackingFencePublicationOrdinal == 2ull &&
              runtime.evidence.copyPublicationOrdinal <
                  runtime.evidence.trackingFencePublicationOrdinal,
          "runtime proves copy-before-fence order");
    check(!runtime.runtimeWriteAuthorized,
          "publication tracking never grants write authority");
}

static void testAnchorAndCompletionBoundary()
{
    std::puts("4. full provider publication mints anchor before completion");
    auto runtime = begunRuntime();
    check(advanceAll(&runtime), "all ordered provider milestones accepted");

    NVAmpereCopyPublicationBridge bridge = {};
    check(nvAmpereCopyRuntimeMintAnchor(&runtime, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_OK,
          "runtime mints inherited opaque anchor");
    check(runtime.anchorMinted && bridge.valid &&
              bridge.transactionAnchor.valid(),
          "runtime and bridge record anchor validity");
    check(bridge.transactionAnchor.handoffEpoch() == kEpoch &&
              bridge.transactionAnchor.transactionToken() == kToken,
          "opaque transaction identity is preserved");
    check(bridge.completionObservationMayBegin &&
              !runtime.completionObservationStarted,
          "anchor opens but does not start completion observation");
    check(!bridge.runtimeWriteAuthorized && !runtime.runtimeWriteAuthorized,
          "anchor path grants no runtime writes");

    check(nvAmpereCopyRuntimeBeginCompletionObservation(&runtime) ==
              NV_COPY_RUNTIME_OK,
          "completion observation starts only after anchor");
    check(runtime.completionObservationStarted,
          "completion boundary is latched");
    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED) ==
              NV_COPY_RUNTIME_ERR_SEQUENCE,
          "publication cannot mutate after completion starts");
    check(nvAmpereCopyRuntimeBegin(&runtime, readyChannel(), 101ull, 102ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_ACTIVE,
          "resource reuse is refused while transaction remains active");
}

static void testInvalidTrackingTargetRefusesAtomically()
{
    std::puts("5. inherited lifetime planner still validates tracking target");
    NVAmpereCopyPublicationRuntime runtime = {};
    nvAmpereCopyRuntimeInit(&runtime);
    nvAmpereCopyRuntimeSetPeerMapping(&runtime, true);
    check(nvAmpereCopyRuntimeBegin(
              &runtime, readyChannel(), 100ull, 100ull, 8ull) ==
              NV_COPY_RUNTIME_OK,
          "runtime may stage provider facts before anchor validation");
    check(advanceAll(&runtime), "ordered publication completes");
    const auto before = runtime;
    NVAmpereCopyPublicationBridge bridge = {};

    check(nvAmpereCopyRuntimeMintAnchor(&runtime, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_BRIDGE,
          "non-new tracking target cannot mint anchor");
    check(sameRuntime(runtime, before), "anchor refusal leaves runtime untouched");
    check(!runtime.anchorMinted && !runtime.completionObservationStarted,
          "completion cannot begin after refused anchor");
}

static void testOrdinalOverflowRefusesAtomically()
{
    std::puts("6. publication witness cannot wrap");
    auto runtime = begunRuntime();
    runtime.nextPublicationOrdinal = ~0ull;
    nvAmpereCopyRuntimeAdvance(&runtime, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED);
    nvAmpereCopyRuntimeAdvance(&runtime, NV_COPY_RUNTIME_EVENT_COPY_RING_STORED);
    nvAmpereCopyRuntimeAdvance(&runtime, NV_COPY_RUNTIME_EVENT_COPY_GPPUT_PUBLISHED);
    const auto before = runtime;

    check(nvAmpereCopyRuntimeAdvance(
              &runtime, NV_COPY_RUNTIME_EVENT_COPY_KICK_ACCEPTED) ==
              NV_COPY_RUNTIME_ERR_ORDINAL_OVERFLOW,
          "max publication witness refuses instead of wrapping");
    check(sameRuntime(runtime, before), "overflow refusal is atomic");
}

static void testPeerMappingCannotDisappearMidTransaction()
{
    std::puts("7. peer mapping ownership cannot change under active copy");
    auto runtime = begunRuntime();
    const auto before = runtime;
    check(nvAmpereCopyRuntimeSetPeerMapping(&runtime, false) ==
              NV_COPY_RUNTIME_ERR_ACTIVE,
          "active transaction refuses mapping loss");
    check(sameRuntime(runtime, before), "mapping-loss refusal is atomic");
}

int main()
{
    testInitAndOwnershipGate();
    testChannelGateIsAtomic();
    testRuntimeOwnsPublicationOrder();
    testAnchorAndCompletionBoundary();
    testInvalidTrackingTargetRefusesAtomically();
    testOrdinalOverflowRefusesAtomically();
    testPeerMappingCannotDisappearMidTransaction();

    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0u ? 0 : 1;
}
