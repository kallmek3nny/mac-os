// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoff.hpp"

#include <cstdio>

static NvIntelSharedCopyLifetimePlan retiredCopy()
{
    NvIntelSharedCopyProgress progress = {};
    progress.peerMappingValidated = true;
    progress.channelSubmitReady = true;
    progress.copySubmissionPublished = true;
    progress.trackingFencePublishedAfterCopy = true;
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.completedBeforeSubmission = 40ull;
    progress.submittedTrackingValue = 41ull;
    progress.observedCompletionPayload = 41u;
    progress.maxCredibleCompletionJump = 8ull;

    NvIntelSharedCopyLifetimePlan out = {};
    if (nvIntelSharedCopyLifetimePlan(progress, &out) != NV_INTEL_COPY_OK)
        return {};
    return out;
}

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static bool checkTrue(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::puts(what);
    }
    return cond;
}

static void testFailedPeerTeardownQuarantinesLifetime()
{
    const NvIntelSharedCopyLifetimePlan copy = retiredCopy();
    // The handoff plan below consumes this copy plan, so stop here if it is not valid.
    if (!checkTrue(copy.valid && copy.completionProven,
                   "failed to construct retired copy plan"))
        return;

    NvIntelSharedSurfaceIdentity identity = {};
    identity.allocationToken = 0x8899aabbccddeeffull;
    identity.contentGeneration = 22ull;

    NvIntelSharedSurfaceHandoffProgress progress = {};
    progress.sharedSurfaceIdentity = identity;
    progress.copyCompletionIdentity = identity;
    progress.intelTargetIdentity = identity;
    progress.peerMappingTeardownAttempted = true;
    progress.peerMappingTeardownFinished = true;
    progress.peerMappingTeardownSucceeded = false;

    NvIntelSharedSurfaceHandoffPlan out = {};
    // The quarantine fields below are the plan's output only when the plan returned OK.
    if (!checkTrue(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) ==
                       NV_INTEL_HANDOFF_OK,
                   "failed peer teardown plan was refused instead of quarantined"))
        return;

    // One assertion per lifetime fact, so a regression names the exact field.
    checkTrue(out.valid, "failed peer teardown plan is not marked valid");
    checkTrue(out.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
              "failed peer teardown did not enter uncertain quarantine");
    checkTrue(nvIntelSharedSurfaceIdentityEqual(out.identity, identity),
              "failed peer teardown plan lost the shared-surface identity");
    checkTrue(out.copyRetirementProven, "failed peer teardown erased proven copy retirement");
    checkTrue(!out.nvidiaMayStillReferenceSurface, "retired copy is still reported as referenced by NVIDIA");
    checkTrue(out.peerMappingTeardownAttempted, "peer teardown attempt was erased");
    checkTrue(out.peerMappingTeardownFinished, "peer teardown completion was erased");
    checkTrue(!out.peerMappingTeardownSucceeded, "failed peer teardown was reported as successful");
    checkTrue(out.lifetimeUncertain, "failed peer teardown did not mark the lifetime uncertain");
    checkTrue(!out.completionUncertain, "failed peer teardown was misreported as uncertain copy completion");
    checkTrue(out.quarantineSharedBacking, "failed peer teardown did not quarantine the shared backing");
    checkTrue(out.quarantinePeerMapping, "failed peer teardown did not quarantine the peer mapping");
    checkTrue(!out.quarantineIntelMapping, "Intel mapping was quarantined with no display plan");
    checkTrue(out.peerMappingMustRemainPinned, "peer mapping was unpinned after failed teardown");
    checkTrue(!out.peerMappingSlotReuseAuthorized, "peer mapping slot reuse was authorized after failed teardown");
    checkTrue(out.sharedBackingMustRemainPinned, "shared backing was unpinned after failed teardown");
    checkTrue(!out.sharedBackingReleaseAuthorized, "shared backing release was authorized after failed teardown");
    checkTrue(!out.intelMappingCommitted, "Intel mapping was reported committed with no display plan");
    checkTrue(!out.intelSurfaceIsLiveScanout, "surface was reported as live scanout with no display plan");
    checkTrue(!out.runtimeWriteAuthorized, "failed peer teardown plan authorized runtime writes");

    if (gFailures == 0u)
        std::puts("failed peer teardown lifetime quarantine ok");
}

// Teardown progress that skips a step is refused before *out is written, so an
// impossible teardown record cannot be laundered into a quarantine plan.
static void testImpossibleTeardownSequenceIsRefusedAtomically()
{
    const NvIntelSharedCopyLifetimePlan copy = retiredCopy();
    // The refusals below are only meaningful against an otherwise valid copy plan.
    if (!checkTrue(copy.valid && copy.completionProven,
                   "failed to construct retired copy plan for sequence refusal"))
        return;

    NvIntelSharedSurfaceIdentity identity = {};
    identity.allocationToken = 0x8899aabbccddeeffull;
    identity.contentGeneration = 22ull;

    NvIntelSharedSurfaceHandoffProgress finishedUnattempted = {};
    finishedUnattempted.sharedSurfaceIdentity = identity;
    finishedUnattempted.copyCompletionIdentity = identity;
    finishedUnattempted.intelTargetIdentity = identity;
    finishedUnattempted.peerMappingTeardownFinished = true;

    NvIntelSharedSurfaceHandoffProgress succeededUnfinished = finishedUnattempted;
    succeededUnfinished.peerMappingTeardownAttempted = true;
    succeededUnfinished.peerMappingTeardownFinished = false;
    succeededUnfinished.peerMappingTeardownSucceeded = true;

    NvIntelSharedSurfaceHandoffPlan untouched = {};
    untouched.phase = 0xdeadu;

    checkTrue(nvIntelSharedSurfaceHandoffPlan(&copy, &finishedUnattempted, nullptr, &untouched) ==
                  NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE,
              "teardown finished without an attempt was not refused");
    checkTrue(untouched.phase == 0xdeadu, "finished-without-attempt refusal overwrote the output plan");
    checkTrue(!untouched.valid, "finished-without-attempt refusal marked the output plan valid");

    checkTrue(nvIntelSharedSurfaceHandoffPlan(&copy, &succeededUnfinished, nullptr, &untouched) ==
                  NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE,
              "teardown succeeded without finishing was not refused");
    checkTrue(untouched.phase == 0xdeadu, "succeeded-without-finish refusal overwrote the output plan");
    checkTrue(!untouched.valid, "succeeded-without-finish refusal marked the output plan valid");
}

int main()
{
    testFailedPeerTeardownQuarantinesLifetime();
    testImpossibleTeardownSequenceIsRefusedAtomically();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
