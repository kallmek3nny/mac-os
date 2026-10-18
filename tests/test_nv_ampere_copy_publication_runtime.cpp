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

static NVAmpereChannelRuntime readyChannelRuntime()
{
    NVAmpereChannelRuntime runtime = {};
    (void)nvAmpereChannelRuntimeInit(&runtime);
    (void)nvAmpereChannelRuntimeObserveAllocation(
        &runtime, NV_SUBMIT_C56F_CLASS_ID, 1024u, true, true, true);
    (void)nvAmpereChannelRuntimeObserveSubmissionEndpoint(
        &runtime, true, true);
    (void)nvAmpereChannelRuntimeSeal(&runtime);
    return runtime;
}

static bool sameRuntime(const NVAmpereCopyPublicationRuntime &a,
                        const NVAmpereCopyPublicationRuntime &b)
{
    const auto &x = a.evidence;
    const auto &y = b.evidence;
    return a.initialized == b.initialized &&
           a.providerOwnershipUncertain == b.providerOwnershipUncertain &&
           x.channel.channelClassNum == y.channel.channelClassNum &&
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
           a.acceptedEventCount == b.acceptedEventCount &&
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
    const auto channel = readyChannelRuntime();
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_OK,
          "provider begins transaction from sealed channel runtime");
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

    const auto channel = readyChannelRuntime();
    const auto before = runtime;
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_PEER_MAPPING,
          "transaction refuses before peer mapping proof");
    check(sameRuntime(runtime, before), "peer refusal is atomic");
}

static void testChannelGateIsAtomic()
{
    std::puts("2. sealed provider channel runtime is required before publication");
    NVAmpereCopyPublicationRuntime runtime = {};
    nvAmpereCopyRuntimeInit(&runtime);
    nvAmpereCopyRuntimeSetPeerMapping(&runtime, true);

    NVAmpereChannelRuntime channel = {};
    nvAmpereChannelRuntimeInit(&channel);
    nvAmpereChannelRuntimeObserveAllocation(
        &channel, NV_SUBMIT_C56F_CLASS_ID, 1024u, true, true, true);
    const auto before = runtime;

    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_CHANNEL,
          "unsealed channel runtime refuses publication start");
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
    check(nvAmpereCopyRuntimeBegin(
              &runtime, readyChannelRuntime(), 101ull, 102ull, 8ull) ==
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
              &runtime, readyChannelRuntime(), 100ull, 100ull, 8ull) ==
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


static void testInitIsOneShot()
{
    std::puts("8. uninitialized publication and lifetime reset are refused");
    NVAmpereCopyPublicationRuntime state = {};
    const auto empty = state;
    check(nvAmpereCopyRuntimeSetPeerMapping(&state, true) ==
              NV_COPY_RUNTIME_ERR_UNINITIALIZED,
          "no mapping proof before init");
    check(nvAmpereCopyRuntimeBegin(&state, readyChannelRuntime(), 100ull, 101ull,
              8ull) == NV_COPY_RUNTIME_ERR_UNINITIALIZED,
          "no transaction before init");
    check(nvAmpereCopyRuntimeAdvance(
              &state, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED) ==
              NV_COPY_RUNTIME_ERR_UNINITIALIZED,
          "no milestones before init");
    check(nvAmpereCopyRuntimeExpectedEvent(state) == 0u &&
              sameRuntime(state, empty),
          "uninitialized state has no event and remains unchanged");
    NVAmpereCopyPublicationBridge bridge = {};
    check(nvAmpereCopyRuntimeMintAnchor(&state, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_UNINITIALIZED,
          "no anchor before init");
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&state) ==
              NV_COPY_RUNTIME_ERR_UNINITIALIZED,
          "no completion before init");

    check(nvAmpereCopyRuntimeInit(&state) == NV_COPY_RUNTIME_OK,
          "initialize once");
    const auto initialized = state;
    check(nvAmpereCopyRuntimeInit(&state) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, initialized),
          "reinit refused before mapping");
    check(nvAmpereCopyRuntimeSetPeerMapping(&state, true) == NV_COPY_RUNTIME_OK,
          "mapping accepted");
    const auto mapped = state;
    check(nvAmpereCopyRuntimeInit(&state) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, mapped),
          "mapping ownership cannot be erased");

    check(nvAmpereCopyRuntimeBegin(&state, readyChannelRuntime(), 100ull,
              101ull, 8ull) == NV_COPY_RUNTIME_OK,
          "transaction begins");
    const auto active = state;
    check(nvAmpereCopyRuntimeInit(&state) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, active),
          "active transaction cannot be reset");
    check(advanceAll(&state), "publication milestones accepted");
    check(nvAmpereCopyRuntimeMintAnchor(
              &state, kEpoch, kToken, &bridge) == NV_COPY_RUNTIME_OK,
          "anchor minted");
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&state) ==
              NV_COPY_RUNTIME_OK,
          "completion observation started");
    const auto observing = state;
    check(nvAmpereCopyRuntimeInit(&state) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, observing),
          "post-anchor ownership cannot be reset");
}


static void testProviderUncertaintyBlocksPublication()
{
    std::puts("9. provider loss permanently quarantines publication");
    NVAmpereCopyPublicationRuntime runtime = {};
    const auto empty = runtime;
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_COPY_RUNTIME_ERR_UNINITIALIZED &&
              sameRuntime(runtime, empty), "uninitialized loss refuses");
    check(nvAmpereCopyRuntimeInit(&runtime) == NV_COPY_RUNTIME_OK,
          "runtime initialized");
    check(nvAmpereCopyRuntimeSetPeerMapping(&runtime, true) ==
              NV_COPY_RUNTIME_OK, "mapper available");
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_COPY_RUNTIME_OK, "provider uncertain before transaction");
    const auto lost = runtime;
    check(nvAmpereCopyRuntimeBegin(&runtime, readyChannelRuntime(),
              100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "sealed channel cannot revive stopped provider");
    check(nvAmpereCopyRuntimeSetPeerMapping(&runtime, true) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "mapper cannot be rebound after stop");
    check(nvAmpereCopyRuntimeInit(&runtime) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION,
          "no reinit to erase quarantine");
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_COPY_RUNTIME_OK && sameRuntime(runtime, lost),
          "quarantine absorbing and idempotent");

    runtime = begunRuntime();
    check(nvAmpereCopyRuntimeAdvance(&runtime,
              NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED) == NV_COPY_RUNTIME_OK,
          "first event accepted");
    const auto active = runtime;
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_COPY_RUNTIME_OK, "active publication quarantined");
    check(runtime.transactionActive && runtime.providerOwnershipUncertain &&
              runtime.evidence.copyPushPrepared ==
                  active.evidence.copyPushPrepared,
          "active history stays pinned");
    const auto quarantined = runtime;
    check(nvAmpereCopyRuntimeExpectedEvent(runtime) == 0u,
          "uncertain provider has no legal next event");
    check(nvAmpereCopyRuntimeAdvance(&runtime,
              NV_COPY_RUNTIME_EVENT_COPY_RING_STORED) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "cannot advance lost publication");
    NVAmpereCopyPublicationBridge bridge = {};
    check(nvAmpereCopyRuntimeMintAnchor(&runtime, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "no new anchor after loss");
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&runtime) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "cannot begin completion after loss");
    check(sameRuntime(runtime, quarantined),
          "refusals leave historical events untouched");

    runtime = begunRuntime();
    check(advanceAll(&runtime), "normal publication sequence accepted");
    check(nvAmpereCopyRuntimeMintAnchor(&runtime, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_OK, "anchor minted");
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_COPY_RUNTIME_OK, "anchored provider loss");
    const auto anchoredLost = runtime;
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&runtime) ==
              NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "pre-existing anchor cannot prove completion");
    check(sameRuntime(runtime, anchoredLost) && runtime.anchorMinted &&
              !runtime.completionObservationStarted,
          "anchored resources remain quarantined");
}


static void testInconsistentSealedChannelCannotBegin()
{
    std::puts("10. copy start revalidates sealed channel export");
    NVAmpereCopyPublicationRuntime runtime = {};
    check(nvAmpereCopyRuntimeInit(&runtime) == NV_COPY_RUNTIME_OK,
          "runtime initializes");
    check(nvAmpereCopyRuntimeSetPeerMapping(&runtime, true) ==
              NV_COPY_RUNTIME_OK, "mapper proof recorded");

    auto channel = readyChannelRuntime();
    channel.evidence.gpPutMapped = false;
    const auto before = runtime;
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_CHANNEL,
          "changed sealed mapping fails copy start");
    check(sameRuntime(runtime, before),
          "inconsistent channel refusal leaves copy runtime unchanged");

    channel = readyChannelRuntime();
    channel.readiness.submitReady = false;
    check(nvAmpereCopyRuntimeBegin(&runtime, channel, 100ull, 101ull, 8ull) ==
              NV_COPY_RUNTIME_ERR_CHANNEL,
          "changed cached readiness fails copy start");
    check(sameRuntime(runtime, before),
          "cached readiness refusal is atomic");
}


static void testProviderStartGateIsFailClosed()
{
    std::puts("11. provider start refuses prior ownership and uncertain lifetime");
    NVAmpereChannelRuntime channel = {};
    NVAmpereCopyPublicationRuntime publication = {};
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "uninitialized provider cannot start");
    check(nvAmpereChannelRuntimeInit(&channel) == NV_CHANNEL_RUNTIME_OK &&
              nvAmpereCopyRuntimeInit(&publication) == NV_COPY_RUNTIME_OK,
          "provider runtimes initialized");
    check(nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "fresh provider may start");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, true, false),
          "retained service cannot restart");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, true),
          "retained PCI dependency cannot be overwritten");

    check(nvAmpereCopyRuntimeSetPeerMapping(&publication, true) ==
              NV_COPY_RUNTIME_OK, "mapper proof accepted");
    check(nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "clean provider mapping evidence alone is not an active channel");
    check(nvAmpereChannelRuntimeObserveAllocation(&channel,
              NV_SUBMIT_C56F_CLASS_ID, 1024u, true, true, true) ==
              NV_CHANNEL_RUNTIME_OK, "channel allocated");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "channel allocation ownership refuses restart");
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&channel) ==
              NV_CHANNEL_RUNTIME_OK, "channel ownership quarantined");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "quarantined channel refuses restart");

    channel = readyChannelRuntime();
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "sealed channel cannot restart");
    channel = {};
    check(nvAmpereChannelRuntimeInit(&channel) == NV_CHANNEL_RUNTIME_OK,
          "fresh channel state for publication scenario");

    check(nvAmpereCopyRuntimeBegin(&publication, readyChannelRuntime(),
              100ull, 101ull, 8ull) == NV_COPY_RUNTIME_OK,
          "copy publication begins");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "active copy transaction refuses provider restart");
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&publication) ==
              NV_COPY_RUNTIME_OK, "copy publication quarantined");
    check(!nvAmpereProviderRuntimeMayStart(channel, publication, false, false),
          "uncertain copy provider refuses restart");
    check(nvAmpereCopyRuntimeInit(&publication) ==
              NV_COPY_RUNTIME_ERR_REINITIALIZATION,
          "reinit cannot clear quarantined copy lifetime");
}


static void checkCorruptStageRefused(NVAmpereCopyPublicationRuntime runtime,
                                     NVCopyRuntimeU32 next,
                                     const char *label)
{
    const auto before = runtime;
    check(nvAmpereCopyRuntimeExpectedEvent(runtime) == 0u, label);
    check(nvAmpereCopyRuntimeAdvance(&runtime, next) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "corrupt history cannot advance publication");
    check(sameRuntime(runtime, before),
          "refused publication does not repair or erase corrupt history");
}

static void testMilestoneIntegrityAndWitnesses()
{
    std::puts("12. runtime-owned milestone counter rejects altered history");
    auto runtime = begunRuntime();
    check(runtime.acceptedEventCount == 0u &&
              nvAmpereCopyRuntimeExpectedEvent(runtime) ==
                  NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED,
          "begun runtime expects first milestone only");

    auto drift = runtime;
    drift.evidence.copyPushPrepared = true;
    checkCorruptStageRefused(drift, NV_COPY_RUNTIME_EVENT_COPY_RING_STORED,
                            "early copied flag cannot skip provider event");

    drift = runtime;
    drift.acceptedEventCount = 8u;
    checkCorruptStageRefused(drift, NV_COPY_RUNTIME_EVENT_FENCE_KICK_ACCEPTED,
                            "forged milestone count cannot skip all events");

    drift = runtime;
    drift.evidence.channel.gpFifoMapped = false;
    checkCorruptStageRefused(drift, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED,
                            "channel mapping drift cannot continue publication");

    drift = runtime;
    drift.evidence.peerMappingValidated = false;
    checkCorruptStageRefused(drift, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED,
                            "mapping proof drift cannot continue publication");
    const auto badPeer = drift;
    check(nvAmpereCopyRuntimeSetPeerMapping(&drift, true) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "peer proof cannot silently repair active history");
    check(sameRuntime(drift, badPeer),
          "active peer refusal is atomic");

    drift = runtime;
    drift.runtimeWriteAuthorized = true;
    checkCorruptStageRefused(drift, NV_COPY_RUNTIME_EVENT_COPY_PUSH_PREPARED,
                            "unexpected hardware-write authority rejected");

    for (NVCopyRuntimeU32 i = 0u; i < 8u; ++i) {
        check(runtime.acceptedEventCount == i,
              "accepted event count agrees with actual provider milestones");
        check(nvAmpereCopyRuntimeExpectedEvent(runtime) == i + 1u,
              "only exact next monotonic provider event accepted");
        check(nvAmpereCopyRuntimeAdvance(&runtime, i + 1u) ==
                  NV_COPY_RUNTIME_OK,
              "runtime advanced by one provider milestone");
        check(runtime.acceptedEventCount == i + 1u,
              "successful milestone increments owned counter once");
    }
    check(nvAmpereCopyRuntimeExpectedEvent(runtime) == 0u &&
              runtime.evidence.copyPublicationOrdinal == 1ull &&
              runtime.evidence.trackingFencePublicationOrdinal == 2ull,
          "complete sequence has precisely two ordered witnesses");

    drift = runtime;
    drift.evidence.copyRingEntryStored = false;
    const auto brokenFinal = drift;
    NVAmpereCopyPublicationBridge bridge = {};
    bridge.valid = true;
    check(nvAmpereCopyRuntimeMintAnchor(&drift, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "cannot mint anchor after milestone rollback");
    check(sameRuntime(drift, brokenFinal) && bridge.valid,
          "rejected anchor preserves provider history and caller output");

    drift = runtime;
    drift.evidence.trackingFencePublicationOrdinal = 1ull;
    check(nvAmpereCopyRuntimeMintAnchor(&drift, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "forged fence ordinal cannot anchor transaction");

    drift = runtime;
    drift.nextPublicationOrdinal = 9ull;
    check(nvAmpereCopyRuntimeMintAnchor(&drift, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "forged next witness cannot anchor transaction");

    check(nvAmpereCopyRuntimeMintAnchor(&runtime, kEpoch, kToken, &bridge) ==
              NV_COPY_RUNTIME_OK,
          "intact provider milestones still mint an anchor");
    drift = runtime;
    drift.evidence.channel.gpPutMapped = false;
    const auto brokenAnchor = drift;
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&drift) ==
              NV_COPY_RUNTIME_ERR_INCONSISTENT_STATE,
          "post-anchor channel drift cannot start completion");
    check(sameRuntime(drift, brokenAnchor),
          "post-anchor refusal preserves pinned transaction");
    check(nvAmpereCopyRuntimeBeginCompletionObservation(&runtime) ==
              NV_COPY_RUNTIME_OK,
          "intact anchored evidence still allows completion observation");

    drift = runtime;
    drift.acceptedEventCount = 0u;
    check(nvAmpereCopyRuntimeMarkOwnershipUncertain(&drift) ==
              NV_COPY_RUNTIME_OK && drift.providerOwnershipUncertain,
          "uncertain ownership can always quarantine inconsistent history");
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
    testInitIsOneShot();
    testProviderUncertaintyBlocksPublication();
    testInconsistentSealedChannelCannotBegin();
    testProviderStartGateIsFailClosed();
    testMilestoneIntegrityAndWitnesses();

    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0u ? 0 : 1;
}
