// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoffProvenance.hpp"

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

static NvIntelSharedCopyLifetimePlan retiredCopy()
{
    NvIntelSharedCopyProgress p = {};
    p.peerMappingValidated = true;
    p.channelSubmitReady = true;
    p.copySubmissionPublished = true;
    p.trackingFencePublishedAfterCopy = true;
    p.boundedCompletionWaitUsed = true;
    p.boundedCompletionWaitFinished = true;
    p.completionPayloadReadSucceeded = true;
    p.completedBeforeSubmission = 100ull;
    p.submittedTrackingValue = 101ull;
    p.observedCompletionPayload = 101u;
    p.maxCredibleCompletionJump = 8ull;

    NvIntelSharedCopyLifetimePlan out = {};
    check(nvIntelSharedCopyLifetimePlan(p, &out) == NV_INTEL_COPY_OK,
          "retired copy helper builds valid plan");
    return out;
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

static IgDisplayTransactionLifetimePlan liveDisplay()
{
    auto p = mappingCommittedDisplay();
    p.phase = IG_DISPLAY_TXN_PHASE_RETIRED_LIVE;
    p.noArmProgrammingIssued = true;
    p.armControlWriteIssued = true;
    p.surfaceArmWriteIssued = true;
    p.armPointCrossed = true;
    p.retirementEvidencePresent = true;
    p.retirementProven = true;
    p.newSurfaceIsLiveScanout = true;
    p.oldSurfaceMustRemainPinned = false;
    p.oldSurfaceBackingReleaseAuthorized = true;
    p.ggttRestoreMayBeAttempted = false;
    p.ggttRestoreSuccessRequiredBeforeTargetBackingRelease = false;
    p.ggttRestoreBlockedByDisplayState = true;
    p.quarantineOnAbortOrUncertainCompletion = false;
    return p;
}

static NvIntelSharedSurfaceHandoffProvenance exactProvenance()
{
    NvIntelSharedSurfaceHandoffProvenance p = {};
    p.handoffEpoch = 0x1111222233334444ull;
    p.copySubmissionToken = 0x2222333344445555ull;
    p.copyCompletionEvidence.handoffEpoch = p.handoffEpoch;
    p.copyCompletionEvidence.transactionToken = p.copySubmissionToken;
    p.intelDisplayTransactionToken = 0x3333444455556666ull;
    p.intelDisplayEvidence.handoffEpoch = p.handoffEpoch;
    p.intelDisplayEvidence.transactionToken = p.intelDisplayTransactionToken;
    return p;
}

static void poison(NvIntelSharedSurfaceHandoffProvenancePlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const NvIntelSharedSurfaceHandoffProvenancePlan &a,
                      const NvIntelSharedSurfaceHandoffProvenancePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectReplayRefusal(
    const NvIntelSharedCopyLifetimePlan &copy,
    const NvIntelSharedSurfaceHandoffProgress &progress,
    const IgDisplayTransactionLifetimePlan *display,
    const NvIntelSharedSurfaceHandoffProvenance &provenance,
    const char *what)
{
    NvIntelSharedSurfaceHandoffProvenancePlan out = {};
    poison(&out);
    const auto before = out;
    const auto verdict = nvIntelSharedSurfaceHandoffProvenancePlan(
        &copy, &progress, display, &provenance, &out);
    check(verdict == NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY, what);
    check(sameBytes(out, before), "replay refusal leaves output untouched");
}

static void checkAlwaysSafe(const NvIntelSharedSurfaceHandoffProvenancePlan &p)
{
    check(p.valid, "accepted provenance plan marked valid");
    check(!p.runtimeWriteAuthorized,
          "provenance wrapper never authorizes runtime writes");
    check(!p.handoff.runtimeWriteAuthorized,
          "wrapped handoff planner never authorizes runtime writes");
    check(p.handoff.sharedBackingMustRemainPinned,
          "shared backing remains pinned across provenance boundary");
    check(!p.handoff.sharedBackingReleaseAuthorized,
          "provenance wrapper never releases shared backing");
}

static void testExactNvidiaEvidenceBinds()
{
    std::puts("1. exact nvidia completion provenance binds the ownership handoff");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    const auto provenance = exactProvenance();
    NvIntelSharedSurfaceHandoffProvenancePlan out = {};

    check(nvIntelSharedSurfaceHandoffProvenancePlan(
              &copy, &progress, nullptr, &provenance, &out) ==
              NV_INTEL_HANDOFF_PROVENANCE_OK,
          "exact nvidia provenance accepted");
    check(!out.provenanceUncertain,
          "exact nvidia provenance is not uncertain");
    check(out.copySubmissionProvenanceRequired &&
              out.copySubmissionProvenanceBound,
          "copy submission token is bound");
    check(out.copyCompletionProvenanceRequired &&
              out.copyCompletionProvenanceBound,
          "copy retirement evidence is bound to exact submission");
    check(out.handoff.copyRetirementProven &&
              out.handoff.intelHandoffAuthorized &&
              out.handoff.intelMappingMayBegin,
          "trusted retirement opens the intel ownership boundary");
    check(out.handoff.phase == NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED,
          "trusted retirement retains normal handoff phase");
    checkAlwaysSafe(out);
}

static void testStaleNvidiaEvidenceRefusesAtomically()
{
    std::puts("2. stale nvidia completion evidence cannot be replayed");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();

    auto staleSubmission = exactProvenance();
    staleSubmission.copyCompletionEvidence.transactionToken -= 1ull;
    expectReplayRefusal(copy, progress, nullptr, staleSubmission,
                        "prior copy-submission evidence refused");

    auto staleHandoff = exactProvenance();
    staleHandoff.copyCompletionEvidence.handoffEpoch -= 1ull;
    expectReplayRefusal(copy, progress, nullptr, staleHandoff,
                        "prior handoff epoch completion evidence refused");
}

static void testAmbiguousNvidiaEvidenceQuarantines()
{
    std::puts("3. missing nvidia provenance quarantines instead of inventing retirement");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    auto provenance = exactProvenance();
    provenance.copyCompletionEvidence = {};

    NvIntelSharedSurfaceHandoffProvenancePlan out = {};
    check(nvIntelSharedSurfaceHandoffProvenancePlan(
              &copy, &progress, nullptr, &provenance, &out) ==
              NV_INTEL_HANDOFF_PROVENANCE_OK,
          "ambiguous nvidia provenance accepted only as conservative state");
    check(out.provenanceUncertain,
          "missing nvidia evidence provenance is marked uncertain");
    check(out.handoff.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "ambiguous nvidia provenance enters uncertain quarantine");
    check(!out.handoff.copyRetirementProven &&
              !out.handoff.intelHandoffAuthorized &&
              !out.handoff.intelMappingMayBegin,
          "ambiguous evidence cannot establish nvidia retirement or intel ownership");
    check(out.handoff.nvidiaMayStillReferenceSurface &&
              out.handoff.peerMappingMustRemainPinned &&
              !out.handoff.peerMappingSlotReuseAuthorized,
          "ambiguous retirement retains nvidia and peer-map lifetimes");
    check(out.handoff.quarantineSharedBacking &&
              out.handoff.quarantinePeerMapping,
          "ambiguous nvidia provenance quarantines shared backing and peer map");
    checkAlwaysSafe(out);
}

static void testExactIntelEvidenceBinds()
{
    std::puts("4. exact intel retirement provenance binds live display evidence");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    const auto display = liveDisplay();
    const auto provenance = exactProvenance();
    NvIntelSharedSurfaceHandoffProvenancePlan out = {};

    check(nvIntelSharedSurfaceHandoffProvenancePlan(
              &copy, &progress, &display, &provenance, &out) ==
              NV_INTEL_HANDOFF_PROVENANCE_OK,
          "exact intel provenance accepted");
    check(!out.provenanceUncertain,
          "exact intel provenance is not uncertain");
    check(out.intelDisplayTransactionProvenanceRequired &&
              out.intelDisplayTransactionProvenanceBound,
          "intel display transaction token is bound");
    check(out.intelDisplayEvidenceProvenanceRequired &&
              out.intelDisplayEvidenceProvenanceBound,
          "intel retirement evidence is bound to exact display transaction");
    check(out.handoff.phase == NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE &&
              out.handoff.intelSurfaceIsLiveScanout,
          "only exact display evidence retains live-scanout classification");
    checkAlwaysSafe(out);
}

static void testStaleIntelEvidenceRefusesAtomically()
{
    std::puts("5. stale intel retirement evidence cannot be replayed");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    const auto display = liveDisplay();

    auto staleTransaction = exactProvenance();
    staleTransaction.intelDisplayEvidence.transactionToken -= 1ull;
    expectReplayRefusal(copy, progress, &display, staleTransaction,
                        "prior intel display transaction evidence refused");

    auto staleHandoff = exactProvenance();
    staleHandoff.intelDisplayEvidence.handoffEpoch -= 1ull;
    expectReplayRefusal(copy, progress, &display, staleHandoff,
                        "prior handoff epoch display evidence refused");
}

static void testAmbiguousIntelEvidenceRetainsMapping()
{
    std::puts("6. missing intel retirement provenance retains and quarantines mapping");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    const auto display = liveDisplay();
    auto provenance = exactProvenance();
    provenance.intelDisplayEvidence = {};

    NvIntelSharedSurfaceHandoffProvenancePlan out = {};
    check(nvIntelSharedSurfaceHandoffProvenancePlan(
              &copy, &progress, &display, &provenance, &out) ==
              NV_INTEL_HANDOFF_PROVENANCE_OK,
          "ambiguous intel evidence accepted only as conservative state");
    check(out.provenanceUncertain,
          "missing intel evidence provenance is marked uncertain");
    check(out.handoff.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "ambiguous intel evidence enters uncertain quarantine");
    check(out.handoff.copyRetirementProven && out.handoff.intelHandoffAuthorized,
          "exact nvidia ownership proof remains valid");
    check(out.handoff.intelMappingCommitted &&
              out.handoff.intelMappingMustRemainCommitted,
          "already committed intel mapping remains retained");
    check(!out.handoff.intelSurfaceIsLiveScanout &&
              out.handoff.quarantineIntelMapping &&
              out.handoff.quarantineSharedBacking,
          "ambiguous display retirement cannot claim live scanout and quarantines mapping");
    checkAlwaysSafe(out);
}

static void testMissingRootEpochQuarantinesAllActiveDomains()
{
    std::puts("7. missing handoff epoch cannot bind either gpu transaction");
    const auto copy = retiredCopy();
    const auto progress = handoffProgress();
    const auto display = liveDisplay();
    auto provenance = exactProvenance();
    provenance.handoffEpoch = 0ull;

    NvIntelSharedSurfaceHandoffProvenancePlan out = {};
    check(nvIntelSharedSurfaceHandoffProvenancePlan(
              &copy, &progress, &display, &provenance, &out) ==
              NV_INTEL_HANDOFF_PROVENANCE_OK,
          "missing root epoch becomes conservative quarantine");
    check(out.provenanceUncertain &&
              out.handoff.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
          "missing root epoch marks lifetime uncertain");
    check(!out.handoff.copyRetirementProven &&
              !out.handoff.intelHandoffAuthorized,
          "unbound handoff epoch cannot establish cross-gpu ownership");
    check(out.handoff.quarantinePeerMapping &&
              out.handoff.quarantineIntelMapping &&
              out.handoff.quarantineSharedBacking,
          "unbound handoff epoch quarantines every active lifetime domain");
    checkAlwaysSafe(out);
}

int main()
{
    testExactNvidiaEvidenceBinds();
    testStaleNvidiaEvidenceRefusesAtomically();
    testAmbiguousNvidiaEvidenceQuarantines();
    testExactIntelEvidenceBinds();
    testStaleIntelEvidenceRefusesAtomically();
    testAmbiguousIntelEvidenceRetainsMapping();
    testMissingRootEpochQuarantinesAllActiveDomains();

    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0u ? 0 : 1;
}
