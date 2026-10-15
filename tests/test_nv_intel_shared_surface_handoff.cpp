// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoff.hpp"

#include <cstdio>
#include <cstring>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void check(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

static NvIntelSharedSurfaceIdentity identity()
{
    NvIntelSharedSurfaceIdentity id = {};
    id.allocationToken = 0x445566778899aabbull;
    id.contentGeneration = 17ull;
    return id;
}

static NvIntelSharedSurfaceHandoffProgress baseProgress()
{
    NvIntelSharedSurfaceHandoffProgress p = {};
    p.sharedSurfaceIdentity = identity();
    p.copyCompletionIdentity = identity();
    p.intelTargetIdentity = identity();
    return p;
}

static NvIntelSharedCopyProgress trackedCopyProgress()
{
    NvIntelSharedCopyProgress p = {};
    p.peerMappingValidated = true;
    p.channelSubmitReady = true;
    p.copySubmissionPublished = true;
    p.trackingFencePublishedAfterCopy = true;
    p.completedBeforeSubmission = 40ull;
    p.submittedTrackingValue = 41ull;
    p.maxCredibleCompletionJump = 8ull;
    return p;
}

static NvIntelSharedCopyLifetimePlan pendingCopy()
{
    const auto progress = trackedCopyProgress();
    NvIntelSharedCopyLifetimePlan out = {};
    const auto verdict = nvIntelSharedCopyLifetimePlan(progress, &out);
    check(verdict == NV_INTEL_COPY_OK, "pending copy helper builds valid plan");
    return out;
}

static NvIntelSharedCopyLifetimePlan uncertainCopy()
{
    auto progress = trackedCopyProgress();
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionWaitTimedOut = true;
    NvIntelSharedCopyLifetimePlan out = {};
    const auto verdict = nvIntelSharedCopyLifetimePlan(progress, &out);
    check(verdict == NV_INTEL_COPY_OK, "uncertain copy helper builds valid plan");
    return out;
}

static NvIntelSharedCopyLifetimePlan retiredCopy()
{
    auto progress = trackedCopyProgress();
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.observedCompletionPayload = 41u;
    NvIntelSharedCopyLifetimePlan out = {};
    const auto verdict = nvIntelSharedCopyLifetimePlan(progress, &out);
    check(verdict == NV_INTEL_COPY_OK, "retired copy helper builds valid plan");
    return out;
}

static IgDisplayTransactionLifetimePlan idleDisplay()
{
    IgDisplayTransactionLifetimePlan p = {};
    p.phase = IG_DISPLAY_TXN_PHASE_PRE_MAPPING;
    p.oldSurfaceMustRemainPinned = true;
    p.valid = true;
    return p;
}

static IgDisplayTransactionLifetimePlan mappingCommittedDisplay()
{
    IgDisplayTransactionLifetimePlan p = {};
    p.phase = IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED;
    p.targetMappingPrepared = true;
    p.targetMappingCommitted = true;
    p.targetMappingReadbackExact = true;
    p.oldSurfaceMustRemainPinned = true;
    p.newSurfaceMustRemainPinned = true;
    p.targetMappingMustRemainCommitted = true;
    p.ggttRestoreMayBeAttempted = true;
    p.ggttRestoreSuccessRequiredBeforeTargetBackingRelease = true;
    p.valid = true;
    return p;
}

static IgDisplayTransactionLifetimePlan armedDisplay()
{
    auto p = mappingCommittedDisplay();
    p.phase = IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT;
    p.noArmProgrammingIssued = true;
    p.armControlWriteIssued = true;
    p.surfaceArmWriteIssued = true;
    p.armPointCrossed = true;
    p.ggttRestoreMayBeAttempted = false;
    p.ggttRestoreSuccessRequiredBeforeTargetBackingRelease = false;
    p.ggttRestoreBlockedByDisplayState = true;
    p.quarantineOnAbortOrUncertainCompletion = true;
    return p;
}

static IgDisplayTransactionLifetimePlan liveDisplay()
{
    auto p = armedDisplay();
    p.phase = IG_DISPLAY_TXN_PHASE_RETIRED_LIVE;
    p.retirementEvidencePresent = true;
    p.retirementProven = true;
    p.newSurfaceIsLiveScanout = true;
    p.oldSurfaceMustRemainPinned = false;
    p.oldSurfaceBackingReleaseAuthorized = true;
    p.quarantineOnAbortOrUncertainCompletion = false;
    return p;
}

static IgDisplayTransactionLifetimePlan uncertainDisplay()
{
    auto p = armedDisplay();
    p.phase = IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE;
    p.retirementEvidencePresent = true;
    p.completionUncertain = true;
    p.quarantineOldSurface = true;
    p.quarantineNewSurface = true;
    p.quarantineTargetMapping = true;
    return p;
}

static void poison(NvIntelSharedSurfaceHandoffPlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const NvIntelSharedSurfaceHandoffPlan &a,
                      const NvIntelSharedSurfaceHandoffPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const NvIntelSharedCopyLifetimePlan *copy,
                          const NvIntelSharedSurfaceHandoffProgress *progress,
                          const IgDisplayTransactionLifetimePlan *display,
                          NVIntelHandoffU32 expected,
                          const char *what)
{
    NvIntelSharedSurfaceHandoffPlan out = {};
    poison(&out);
    const auto before = out;
    const auto verdict = nvIntelSharedSurfaceHandoffPlan(copy, progress, display, &out);
    check(verdict == expected, what);
    check(sameBytes(out, before), "refusal leaves handoff output untouched");
}

static void checkAlwaysSafe(const NvIntelSharedSurfaceHandoffPlan &p)
{
    check(p.valid, "accepted handoff plan marked valid");
    check(!p.runtimeWriteAuthorized, "handoff planner never authorizes writes");
    check(p.sharedBackingMustRemainPinned,
          "shared backing remains pinned throughout cross-gpu handoff");
    check(!p.sharedBackingReleaseAuthorized,
          "handoff never releases shared backing to allocator");
    if (p.intelHandoffAuthorized)
        check(p.copyRetirementProven,
              "intel handoff always rests on nvidia retirement proof");
    if (p.peerMappingSlotReuseAuthorized)
        check(p.peerMappingTeardownSucceeded,
              "peer mapping slot reuse requires successful teardown");
}

static void testPendingCopyBlocksIntelActivity()
{
    std::puts("1. pending nvidia copy blocks intel mapping and peer teardown");
    const auto copy = pendingCopy();
    const auto progress = baseProgress();
    const auto idle = idleDisplay();
    NvIntelSharedSurfaceHandoffPlan out = {};

    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, &idle, &out) ==
              NV_INTEL_HANDOFF_OK,
          "idle intel side may observe pending copy conservatively");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_WAIT_NVIDIA,
          "pending copy classified as wait-nvidia");
    check(out.nvidiaMayStillReferenceSurface &&
              out.peerMappingMustRemainPinned,
          "pending copy retains nvidia surface and peer mapping lifetimes");
    check(!out.intelHandoffAuthorized && !out.intelMappingMayBegin,
          "pending copy cannot hand surface to intel");
    check(!out.peerMappingTeardownMayBeAttempted,
          "pending copy cannot tear down peer mapping");
    checkAlwaysSafe(out);

    const auto mapped = mappingCommittedDisplay();
    expectRefusal(&copy, &progress, &mapped,
                  NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL,
                  "intel mapping before nvidia retirement refused");

    auto teardown = progress;
    teardown.peerMappingTeardownAttempted = true;
    expectRefusal(&copy, &teardown, &idle,
                  NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL,
                  "peer teardown before nvidia retirement refused");
}

static void testRetirementOpensIndependentHandoffAndTeardown()
{
    std::puts("2. exact copy retirement opens intel mapping and peer teardown independently");
    const auto copy = retiredCopy();
    auto progress = baseProgress();
    NvIntelSharedSurfaceHandoffPlan out = {};

    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) ==
              NV_INTEL_HANDOFF_OK,
          "retired copy accepted for handoff");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED,
          "retired copy enters nvidia-retired handoff phase");
    check(out.copyRetirementProven && out.intelHandoffAuthorized &&
              out.intelMappingMayBegin,
          "retirement authorizes intel mapping boundary");
    check(out.peerMappingTeardownMayBeAttempted,
          "retirement independently authorizes peer teardown attempt");
    check(out.peerMappingMustRemainPinned &&
              !out.peerMappingSlotReuseAuthorized,
          "peer mapping remains pinned until teardown actually succeeds");
    checkAlwaysSafe(out);

    const auto mapped = mappingCommittedDisplay();
    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, &mapped, &out) ==
              NV_INTEL_HANDOFF_OK,
          "intel mapping may commit while retired peer mapping is still pinned");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_INTEL_MAPPING_COMMITTED,
          "intel mapping-committed phase classified");
    check(out.intelMappingCommitted && out.intelMappingMustRemainCommitted,
          "committed intel mapping inherits display lifetime pin");
    check(out.peerMappingMustRemainPinned,
          "coexisting retired peer mapping remains pinned until teardown proof");
    checkAlwaysSafe(out);

    progress.peerMappingTeardownAttempted = true;
    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) ==
              NV_INTEL_HANDOFF_OK,
          "peer teardown in progress accepted after retirement");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_PEER_TEARDOWN_ACTIVE,
          "teardown-in-progress phase classified");
    check(out.peerMappingMustRemainPinned &&
              !out.peerMappingSlotReuseAuthorized,
          "unfinished teardown retains peer mapping and forbids slot reuse");

    progress.peerMappingTeardownFinished = true;
    progress.peerMappingTeardownSucceeded = true;
    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) ==
              NV_INTEL_HANDOFF_OK,
          "successful peer teardown accepted");
    check(!out.peerMappingMustRemainPinned &&
              out.peerMappingSlotReuseAuthorized,
          "only successful teardown releases peer mapping slot lifetime");
    checkAlwaysSafe(out);
}

static void testDisplayArmAndLiveScanoutRetainSharedBacking()
{
    std::puts("3. intel display arm and retirement keep target backing pinned");
    const auto copy = retiredCopy();
    const auto progress = baseProgress();
    NvIntelSharedSurfaceHandoffPlan out = {};

    const auto armed = armedDisplay();
    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, &armed, &out) ==
              NV_INTEL_HANDOFF_OK,
          "armed intel display lifetime accepted after nvidia retirement");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_ARMED,
          "armed display phase classified");
    check(out.intelDisplayArmObserved && !out.intelSurfaceIsLiveScanout,
          "surface arm does not pretend retirement already happened");
    checkAlwaysSafe(out);

    const auto live = liveDisplay();
    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, &live, &out) ==
              NV_INTEL_HANDOFF_OK,
          "retired live intel display accepted");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE,
          "live scanout phase classified");
    check(out.intelSurfaceIsLiveScanout && out.intelMappingMustRemainCommitted,
          "live target retains intel mapping lifetime");
    check(out.sharedBackingMustRemainPinned &&
              !out.sharedBackingReleaseAuthorized,
          "live scanout backing cannot be freed by cross-gpu transaction");
    checkAlwaysSafe(out);
}

static void testUncertaintyQuarantinesWithoutInventingOwnership()
{
    std::puts("4. completion uncertainty quarantines affected shared resources");
    const auto copy = uncertainCopy();
    const auto progress = baseProgress();
    NvIntelSharedSurfaceHandoffPlan out = {};

    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) ==
              NV_INTEL_HANDOFF_OK,
          "uncertain nvidia completion accepted as quarantine state");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "nvidia timeout classified as uncertain quarantine");
    check(out.completionUncertain && out.quarantineSharedBacking &&
              out.quarantinePeerMapping,
          "nvidia completion uncertainty quarantines shared backing and peer map");
    check(!out.intelHandoffAuthorized && !out.intelMappingMayBegin,
          "uncertain nvidia completion cannot invent intel ownership");
    checkAlwaysSafe(out);

    const auto retired = retiredCopy();
    const auto display = uncertainDisplay();
    check(nvIntelSharedSurfaceHandoffPlan(&retired, &progress, &display, &out) ==
              NV_INTEL_HANDOFF_OK,
          "uncertain intel retirement accepted conservatively");
    check(out.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "intel retirement uncertainty classified as quarantine");
    check(out.completionUncertain && out.quarantineSharedBacking &&
              out.quarantineIntelMapping,
          "intel uncertainty quarantines target backing and mapping");
    checkAlwaysSafe(out);

    auto teardown = progress;
    teardown.peerMappingTeardownAttempted = true;
    teardown.peerMappingTeardownFinished = true;
    teardown.peerMappingTeardownSucceeded = false;
    check(nvIntelSharedSurfaceHandoffPlan(&retired, &teardown, nullptr, &out) ==
              NV_INTEL_HANDOFF_OK,
          "failed peer teardown retains mapping conservatively");
    check(out.quarantinePeerMapping && out.peerMappingMustRemainPinned &&
              !out.peerMappingSlotReuseAuthorized,
          "failed teardown quarantines peer mapping and forbids slot reuse");
    checkAlwaysSafe(out);
}

static void testIdentityAndContractForgeryRefuseAtomically()
{
    std::puts("5. identity mismatch and forged upstream contracts refuse atomically");
    auto copy = retiredCopy();
    auto progress = baseProgress();

    progress.copyCompletionIdentity.contentGeneration += 1ull;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_IDENTITY,
                  "stale copy generation identity refused");

    progress = baseProgress();
    progress.intelTargetIdentity.allocationToken ^= 1ull;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_IDENTITY,
                  "wrong intel target allocation identity refused");

    progress = baseProgress();
    progress.sharedSurfaceIdentity.allocationToken = 0ull;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_IDENTITY,
                  "missing opaque allocation identity refused");

    progress = baseProgress();
    progress.peerMappingTeardownFinished = true;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE,
                  "finished teardown without attempt refused");

    progress = baseProgress();
    progress.peerMappingTeardownSucceeded = true;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_PEER_TEARDOWN_SEQUENCE,
                  "successful teardown without finished attempt refused");

    progress = baseProgress();
    copy.runtimeWriteAuthorized = true;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_COPY_CONTRACT,
                  "forged write-authorized copy plan refused");

    copy = retiredCopy();
    copy.peerMappingMustRemainPinned = true;
    expectRefusal(&copy, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_COPY_CONTRACT,
                  "forged retired copy retaining nvidia peer lifetime refused");

    copy = retiredCopy();
    auto display = mappingCommittedDisplay();
    display.targetMappingReadbackExact = false;
    expectRefusal(&copy, &progress, &display,
                  NV_INTEL_HANDOFF_ERR_DISPLAY_CONTRACT,
                  "forged committed display mapping without exact readback refused");

    display = liveDisplay();
    display.targetMappingReleaseAuthorized = true;
    expectRefusal(&copy, &progress, &display,
                  NV_INTEL_HANDOFF_ERR_DISPLAY_CONTRACT,
                  "forged live display plan releasing target mapping refused");

    check(nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, nullptr) ==
              NV_INTEL_HANDOFF_ERR_NULL_OUTPUT,
          "null output refused");
    expectRefusal(nullptr, &progress, nullptr,
                  NV_INTEL_HANDOFF_ERR_NULL_COPY_PLAN,
                  "null copy plan refused");
    expectRefusal(&copy, nullptr, nullptr,
                  NV_INTEL_HANDOFF_ERR_NULL_PROGRESS,
                  "null progress refused");
}

int main()
{
    testPendingCopyBlocksIntelActivity();
    testRetirementOpensIndependentHandoffAndTeardown();
    testDisplayArmAndLiveScanoutRetainSharedBacking();
    testUncertaintyQuarantinesWithoutInventingOwnership();
    testIdentityAndContractForgeryRefuseAtomically();

    if (gFailures) {
        std::printf("%u/%u checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %u cross-gpu shared-surface handoff checks passed\n", gChecks);
    return 0;
}
