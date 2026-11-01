// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp"

#include <cstdio>
#include <cstring>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static constexpr NVIntelHandoffU64 kHandoffEpoch = 0x1111222233334444ull;
static constexpr NVIntelHandoffU64 kCopyToken = 0x2222333344445555ull;
static constexpr NVIntelHandoffU64 kIntelToken = 0x3333444455556666ull;

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
    id.contentGeneration = 23ull;
    return id;
}

static NvIntelSharedSurfaceHandoffProgress handoffProgress()
{
    NvIntelSharedSurfaceHandoffProgress p = {};
    p.sharedSurfaceIdentity = identity();
    p.copyCompletionIdentity = identity();
    p.intelTargetIdentity = identity();
    return p;
}

static NvIntelSharedCopyProgress copyBeforeEvidence()
{
    NvIntelSharedCopyProgress p = {};
    p.peerMappingValidated = true;
    p.channelSubmitReady = true;
    p.copySubmissionPublished = true;
    p.trackingFencePublishedAfterCopy = true;
    p.completedBeforeSubmission = 100ull;
    p.submittedTrackingValue = 101ull;
    p.maxCredibleCompletionJump = 8ull;
    return p;
}

static NvIntelSharedCopyProgress retiredCopyProgress()
{
    auto p = copyBeforeEvidence();
    p.boundedCompletionWaitUsed = true;
    p.boundedCompletionWaitFinished = true;
    p.completionPayloadReadSucceeded = true;
    p.observedCompletionPayload = 101u;
    return p;
}

static NvIntelSharedCopyLifetimePlan copyPlan(
    const NvIntelSharedCopyProgress &progress)
{
    NvIntelSharedCopyLifetimePlan out = {};
    check(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "copy helper builds valid lifetime plan");
    return out;
}

static NvIntelSharedCopyTransactionAnchor copyAnchor()
{
    const auto p = copyBeforeEvidence();
    NvIntelSharedCopyTransactionAnchor out = {};
    check(nvIntelBuildSharedCopyTransactionAnchor(
              &p, kHandoffEpoch, kCopyToken, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_OK,
          "copy helper mints pre-evidence anchor");
    return out;
}

static IgDisplayTransactionLifetimePlan armedDisplay()
{
    IgDisplayTransactionLifetimePlan p = {};
    p.deviceId = 0x9a68u;
    p.pipe = 0u;
    p.plane = 0u;
    p.phase = IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT;
    p.targetMappingPrepared = true;
    p.targetMappingCommitted = true;
    p.targetMappingReadbackExact = true;
    p.noArmProgrammingIssued = true;
    p.armControlWriteIssued = true;
    p.surfaceArmWriteIssued = true;
    p.armPointCrossed = true;
    p.oldSurfaceMustRemainPinned = true;
    p.newSurfaceMustRemainPinned = true;
    p.targetMappingMustRemainCommitted = true;
    p.ggttRestoreBlockedByDisplayState = true;
    p.quarantineOnAbortOrUncertainCompletion = true;
    p.valid = true;
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

static IgDisplayTransactionProvenanceAnchor intelAnchor(
    NVIntelHandoffU64 epoch = kHandoffEpoch)
{
    const auto display = armedDisplay();
    IgDisplayTransactionProvenanceAnchor out = {};
    check(nvIntelBuildDisplayTransactionProvenanceAnchor(
              &display, epoch, kIntelToken, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_OK,
          "intel helper mints pre-evidence anchor");
    return out;
}

static NvIntelSharedSurfaceAnchoredProvenance exactCopyProvenance()
{
    NvIntelSharedSurfaceAnchoredProvenance p = {};
    p.copyTransaction = copyAnchor();
    p.copyCompletionEvidence.handoffEpoch = kHandoffEpoch;
    p.copyCompletionEvidence.transactionToken = kCopyToken;
    return p;
}

static NvIntelSharedSurfaceAnchoredProvenance exactFullProvenance()
{
    auto p = exactCopyProvenance();
    p.intelDisplayTransaction = intelAnchor();
    p.intelDisplayEvidence.handoffEpoch = kHandoffEpoch;
    p.intelDisplayEvidence.transactionToken = kIntelToken;
    return p;
}

static void poison(NvIntelSharedSurfaceAnchoredProvenancePlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const NvIntelSharedSurfaceAnchoredProvenancePlan &a,
                      const NvIntelSharedSurfaceAnchoredProvenancePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void testAnchorsOnlyMintBeforeEvidence()
{
    std::puts("1. transaction anchors are admitted only before completion evidence");

    auto copy = retiredCopyProgress();
    NvIntelSharedCopyTransactionAnchor copyOut = {};
    check(nvIntelBuildSharedCopyTransactionAnchor(
              &copy, kHandoffEpoch, kCopyToken, &copyOut) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR,
          "copy anchor cannot be minted after completion observation starts");

    auto display = liveDisplay();
    IgDisplayTransactionProvenanceAnchor displayOut = {};
    check(nvIntelBuildDisplayTransactionProvenanceAnchor(
              &display, kHandoffEpoch, kIntelToken, &displayOut) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_DISPLAY_ANCHOR,
          "display anchor cannot be minted from retirement evidence");
}

static void testExactCopyEvidenceUsesPreEvidenceAnchor()
{
    std::puts("2. exact nvidia evidence must match the pre-evidence copy anchor");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    const auto anchored = exactCopyProvenance();
    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};

    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, nullptr, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_OK,
          "exact anchored copy evidence accepted");
    check(out.valid && out.copyTransactionAnchorRequired &&
              out.copyTransactionAnchorVerified,
          "copy transaction anchor is required and verified");
    check(!out.intelDisplayAnchorRequired &&
              !out.intelDisplayAnchorVerified,
          "intel anchor is not invented before display activity");
    check(!out.provenance.provenanceUncertain,
          "exact pre-evidence binding remains certain");
    check(out.provenance.handoff.copyRetirementProven &&
              out.provenance.handoff.intelHandoffAuthorized,
          "anchored nvidia retirement opens intel handoff");
    check(!out.runtimeWriteAuthorized &&
              !out.provenance.runtimeWriteAuthorized &&
              !out.provenance.handoff.runtimeWriteAuthorized,
          "anchored planner never authorizes runtime writes");
}

static void testStaleCopyEvidenceRefusesAtomically()
{
    std::puts("3. stale nvidia evidence cannot supply its own expected token");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    auto anchored = exactCopyProvenance();
    anchored.copyCompletionEvidence.transactionToken -= 1ull;

    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, nullptr, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_EVIDENCE,
          "stale copy token is refused against earlier anchor");
    check(sameBytes(out, before),
          "copy replay refusal leaves output untouched");
}

static void testChangedCopyTransactionCannotReuseAnchor()
{
    std::puts("4. changed tracking transaction cannot reuse an earlier anchor");

    auto progress = retiredCopyProgress();
    progress.submittedTrackingValue = 102ull;
    progress.observedCompletionPayload = 102u;
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    auto anchored = exactCopyProvenance();
    anchored.copyCompletionEvidence.transactionToken = kCopyToken;

    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, nullptr, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_ANCHOR,
          "changed tracking target is not the anchored copy transaction");
    check(sameBytes(out, before),
          "copy-anchor mismatch leaves output untouched");
}

static void testMissingCopyEvidenceQuarantines()
{
    std::puts("5. missing copy evidence stays conservative under a valid anchor");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    auto anchored = exactCopyProvenance();
    anchored.copyCompletionEvidence = {};
    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};

    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, nullptr, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_OK,
          "missing completion provenance is accepted only as quarantine");
    check(out.provenance.provenanceUncertain &&
              out.provenance.handoff.lifetimeUncertain &&
              out.provenance.handoff.phase ==
                  NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "missing evidence cannot inherit certainty from the anchor");
    check(!out.provenance.handoff.copyRetirementProven &&
              !out.provenance.handoff.intelHandoffAuthorized,
          "missing evidence cannot retire nvidia ownership");
    check(out.provenance.handoff.sharedBackingMustRemainPinned &&
              !out.provenance.handoff.sharedBackingReleaseAuthorized,
          "uncertainty retains shared backing");
}

static void testExactIntelEvidenceUsesPreRetirementAnchor()
{
    std::puts("6. exact intel evidence must match the pre-retirement display anchor");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    const auto display = liveDisplay();
    const auto anchored = exactFullProvenance();
    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};

    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, &display, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_OK,
          "exact anchored display evidence accepted");
    check(out.intelDisplayAnchorRequired && out.intelDisplayAnchorVerified,
          "active display transaction requires its pre-evidence anchor");
    check(!out.provenance.provenanceUncertain &&
              out.provenance.handoff.phase ==
                  NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE &&
              out.provenance.handoff.intelSurfaceIsLiveScanout,
          "only anchored retirement evidence establishes live scanout state");
}

static void testStaleIntelEvidenceRefusesAtomically()
{
    std::puts("7. stale intel evidence cannot replace the anchored display token");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    const auto display = liveDisplay();
    auto anchored = exactFullProvenance();
    anchored.intelDisplayEvidence.transactionToken -= 1ull;

    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, &display, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_EVIDENCE,
          "stale display token is refused against earlier anchor");
    check(sameBytes(out, before),
          "display replay refusal leaves output untouched");
}

static void testCrossGpuRootEpochMustAgree()
{
    std::puts("8. copy and display anchors must share one handoff epoch");

    const auto progress = retiredCopyProgress();
    const auto plan = copyPlan(progress);
    const auto handoff = handoffProgress();
    const auto display = liveDisplay();
    auto anchored = exactCopyProvenance();
    anchored.intelDisplayTransaction = intelAnchor(kHandoffEpoch + 1ull);
    anchored.intelDisplayEvidence.handoffEpoch = kHandoffEpoch + 1ull;
    anchored.intelDisplayEvidence.transactionToken = kIntelToken;

    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, &display, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_ROOT_IDENTITY,
          "different gpu anchor epochs cannot form one handoff");
    check(sameBytes(out, before),
          "root-identity refusal leaves output untouched");
}

static void testCopyPlanMustMatchRecomputedProgress()
{
    std::puts("9. supplied copy lifetime cannot drift from anchored progress");

    const auto progress = retiredCopyProgress();
    auto plan = copyPlan(progress);
    plan.intelHandoffAuthorized = false;
    const auto handoff = handoffProgress();
    const auto anchored = exactCopyProvenance();

    NvIntelSharedSurfaceAnchoredProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    check(nvIntelSharedSurfaceAnchoredProvenancePlan(
              &progress, &plan, &handoff, nullptr, &anchored, &out) ==
              NV_INTEL_ANCHORED_PROVENANCE_ERR_COPY_PLAN,
          "forged copy lifetime output is refused");
    check(sameBytes(out, before),
          "copy-plan mismatch leaves output untouched");
}

int main()
{
    testAnchorsOnlyMintBeforeEvidence();
    testExactCopyEvidenceUsesPreEvidenceAnchor();
    testStaleCopyEvidenceRefusesAtomically();
    testChangedCopyTransactionCannotReuseAnchor();
    testMissingCopyEvidenceQuarantines();
    testExactIntelEvidenceUsesPreRetirementAnchor();
    testStaleIntelEvidenceRefusesAtomically();
    testCrossGpuRootEpochMustAgree();
    testCopyPlanMustMatchRecomputedProgress();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
