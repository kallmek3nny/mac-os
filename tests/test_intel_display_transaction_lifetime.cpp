// SPDX-License-Identifier: MIT
#include "IntelDisplayTransactionLifetime.hpp"

#include <cstdio>
#include <cstring>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void checkTrue(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

static IgPlaneTransitionPlan goldenTransition()
{
    IgPlaneTransitionPlan p = {};
    p.deviceId = IG_DEVICE_OBSERVED_2026_08_16;
    p.pipe = IG_TRANSCODER_A;
    p.plane = IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE;
    p.operationKind = IG_PLANE_TRANSITION_KIND_STATE_CHANGE_ENABLED;
    p.currentPlaneEnabled = true;
    p.targetPlaneEnabled = true;
    p.stateChangeWhileEnabled = true;
    p.surfaceOnlyFlip = false;
    p.enableFromDisabled = false;
    p.requiresNoArmProgramming = true;
    p.requiresArmControlWrite = true;
    p.requiresSurfaceArmWrite = true;
    p.armControlImmediatelyBeforeSurface = true;
    p.planeDisableRequired = false;
    p.oldSurfaceMustRemainPinned = true;
    p.newSurfaceMustRemainPinned = true;
    p.targetMappingMustRemainCommitted = true;
    p.flipCompletionRequired = true;
    p.retirementProofRequired = true;
    p.flipDoneOrEquivalentProofRequired = true;
    p.ggttRestoreForbiddenUntilRetirement = true;
    p.noWriteAbortSafe = true;
    p.postNoArmRollbackProofRequired = true;
    p.postArmRollbackSafe = false;
    p.timeoutMayHaveLatched = true;
    p.quarantineOnUncertainCompletion = true;
    p.retainBothSurfacesOnUncertainCompletion = true;
    p.runtimeWriteAuthorized = false;
    p.valid = true;
    return p;
}

static IgPlaneRetirementObservation retirementObservation(bool proven)
{
    IgPlaneRetirementObservation o = {};
    o.deviceId = IG_DEVICE_OBSERVED_2026_08_16;
    o.pipe = IG_TRANSCODER_A;
    o.plane = IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE;
    o.surfaceArmIssued = true;
    o.flipDoneInterruptUnmaskedBeforeArm = true;
    o.flipDoneInterruptEnabledBeforeArm = true;
    o.staleFlipDoneStatusClearedBeforeArm = true;
    o.exactPrimaryPlaneFlipDoneObserved = proven;
    o.flipDoneObservedAfterArm = proven;
    o.boundedCompletionWaitUsed = true;
    o.boundedCompletionWaitFinished = true;
    o.completionWaitTimedOut = !proven;
    o.vblankAdvancedAfterArm = true;
    o.coherentTargetLiveObservedAfterArm = true;
    return o;
}

static IgPlaneRetirementEvidencePlan goldenRetirement(bool proven)
{
    const auto transition = goldenTransition();
    const auto observation = retirementObservation(proven);
    IgPlaneRetirementEvidencePlan out = {};
    checkTrue(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          proven ? "proven retirement fixture accepted"
                 : "uncertain retirement fixture accepted");
    return out;
}

static IgDisplayTransactionProgress committedProgress()
{
    IgDisplayTransactionProgress p = {};
    p.ggttTransactionPrepared = true;
    p.targetMappingCommitSucceeded = true;
    p.targetMappingExactReadbackVerified = true;
    return p;
}

static void poison(IgDisplayTransactionLifetimePlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const IgDisplayTransactionLifetimePlan &a,
                      const IgDisplayTransactionLifetimePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const IgPlaneTransitionPlan &transition,
                          const IgDisplayTransactionProgress &progress,
                          const IgPlaneRetirementEvidencePlan *retirement,
                          IGU32 expected,
                          const char *what)
{
    IgDisplayTransactionLifetimePlan out = {};
    poison(&out);
    const auto before = out;
    const IGU32 verdict = igDisplayTransactionLifetimePlan(
        &transition, &progress, retirement, &out);
    checkTrue(verdict == expected, what);
    checkTrue(sameBytes(out, before), "refusal leaves output untouched");
}

static void checkAlwaysPure(const IgDisplayTransactionLifetimePlan &p)
{
    checkTrue(!p.displayRollbackAuthorized,
          "transaction planner never invents display rollback authority");
    checkTrue(!p.newSurfaceBackingReleaseAuthorized,
          "new display backing is never released by this transaction");
    checkTrue(!p.targetMappingReleaseAuthorized,
          "active/possibly-active target mapping is never directly released");
    checkTrue(!p.runtimeWriteAuthorized,
          "transaction lifetime planner never authorizes writes");
}

static void testPreMappingAndCommitted()
{
    std::puts("1. pre-write and mapping-committed phases preserve exact restore boundary");
    const auto transition = goldenTransition();

    IgDisplayTransactionProgress progress = {};
    IgDisplayTransactionLifetimePlan out = {};
    checkTrue(igDisplayTransactionLifetimePlan(&transition, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_OK,
          "pre-mapping phase accepted");
    checkTrue(out.valid && out.phase == IG_DISPLAY_TXN_PHASE_PRE_MAPPING,
          "pre-mapping phase classified");
    checkTrue(out.oldSurfaceMustRemainPinned,
          "currently scanned old surface remains pinned");
    checkTrue(!out.newSurfaceMustRemainPinned &&
              !out.targetMappingMustRemainCommitted,
          "uncommitted target has no display lifetime yet");
    checkTrue(!out.ggttRestoreMayBeAttempted,
          "no mapping means no restore transaction to attempt");
    checkTrue(!out.quarantineOnAbortOrUncertainCompletion,
          "no-write pre-mapping abort is trivially safe");
    checkAlwaysPure(out);

    progress = committedProgress();
    checkTrue(igDisplayTransactionLifetimePlan(&transition, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_OK,
          "mapping-committed phase accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED,
          "mapping-committed phase classified");
    checkTrue(out.targetMappingPrepared && out.targetMappingCommitted &&
              out.targetMappingReadbackExact,
          "exact prepared/committed/readback mapping proof retained");
    checkTrue(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "mapping commit pins both transaction surfaces and mapping");
    checkTrue(out.ggttRestoreMayBeAttempted &&
              out.ggttRestoreSuccessRequiredBeforeTargetBackingRelease,
          "pre-display-write exact restore path remains available");
    checkTrue(!out.noArmRollbackProofRequiredForRestore &&
              !out.ggttRestoreBlockedByDisplayState,
          "no display shadow state blocks restore yet");
    checkAlwaysPure(out);
}

static void testNoArmAndArmControlBoundaries()
{
    std::puts("2. noarm/control progress closes direct GGTT rollback path");
    const auto transition = goldenTransition();
    auto progress = committedProgress();
    progress.noArmProgrammingIssued = true;
    IgDisplayTransactionLifetimePlan out = {};

    checkTrue(igDisplayTransactionLifetimePlan(&transition, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_OK,
          "noarm-staged phase accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_NOARM_STAGED,
          "noarm phase classified");
    checkTrue(!out.ggttRestoreMayBeAttempted &&
              out.noArmRollbackProofRequiredForRestore &&
              out.ggttRestoreBlockedByDisplayState,
          "noarm state blocks direct mapping restore without rollback proof");
    checkTrue(out.quarantineOnAbortOrUncertainCompletion,
          "aborting after noarm requires conservative quarantine");
    checkTrue(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "all display lifetimes remain held after noarm");
    checkAlwaysPure(out);

    progress.armControlWriteIssued = true;
    checkTrue(igDisplayTransactionLifetimePlan(&transition, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_OK,
          "arm-control-staged phase accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_ARM_CONTROL_STAGED,
          "arm-control phase classified");
    checkTrue(!out.armPointCrossed && !out.surfaceArmWriteIssued,
          "control staging alone is not surface arm completion");
    checkTrue(out.noArmRollbackProofRequiredForRestore &&
              out.ggttRestoreBlockedByDisplayState,
          "failure between control and surface still needs rollback proof");
    checkAlwaysPure(out);
}

static void testArmedAwaitingRetirement()
{
    std::puts("3. surface arm forbids restore and retains both surfaces");
    const auto transition = goldenTransition();
    auto progress = committedProgress();
    progress.noArmProgrammingIssued = true;
    progress.armControlWriteIssued = true;
    progress.surfaceArmWriteIssued = true;
    IgDisplayTransactionLifetimePlan out = {};

    checkTrue(igDisplayTransactionLifetimePlan(&transition, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_OK,
          "armed waiting phase accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT,
          "armed-await-retirement phase classified");
    checkTrue(out.armPointCrossed && !out.retirementEvidencePresent &&
              !out.retirementProven,
          "arm point recorded without invented completion");
    checkTrue(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "armed transition retains both surfaces and target mapping");
    checkTrue(!out.oldSurfaceBackingReleaseAuthorized &&
              !out.ggttRestoreMayBeAttempted &&
              out.ggttRestoreBlockedByDisplayState,
          "post-arm state authorizes neither release nor restore");
    checkTrue(out.quarantineOnAbortOrUncertainCompletion,
          "abort/timeout after arm is a quarantine boundary");
    checkAlwaysPure(out);
}

static void testUncertainCompletionQuarantines()
{
    std::puts("4. uncertain completion quarantines all possibly-live display objects");
    const auto transition = goldenTransition();
    const auto retirement = goldenRetirement(false);
    auto progress = committedProgress();
    progress.noArmProgrammingIssued = true;
    progress.armControlWriteIssued = true;
    progress.surfaceArmWriteIssued = true;
    IgDisplayTransactionLifetimePlan out = {};

    checkTrue(igDisplayTransactionLifetimePlan(
              &transition, &progress, &retirement, &out) == IG_DISPLAY_TXN_OK,
          "uncertain retirement policy accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE,
          "uncertain completion enters quarantine phase");
    checkTrue(out.retirementEvidencePresent && !out.retirementProven &&
              out.completionUncertain,
          "uncertainty propagated from retirement evidence");
    checkTrue(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "uncertain completion retains every possibly referenced object");
    checkTrue(out.quarantineOldSurface && out.quarantineNewSurface &&
              out.quarantineTargetMapping,
          "old/new/mapping all quarantined");
    checkTrue(!out.oldSurfaceBackingReleaseAuthorized &&
              !out.ggttRestoreMayBeAttempted,
          "uncertain completion cannot release or restore");
    checkAlwaysPure(out);
}

static void testProvenRetirementMakesTargetLive()
{
    std::puts("5. proven retirement releases only old backing and keeps target live");
    const auto transition = goldenTransition();
    const auto retirement = goldenRetirement(true);
    auto progress = committedProgress();
    progress.noArmProgrammingIssued = true;
    progress.armControlWriteIssued = true;
    progress.surfaceArmWriteIssued = true;
    IgDisplayTransactionLifetimePlan out = {};

    checkTrue(igDisplayTransactionLifetimePlan(
              &transition, &progress, &retirement, &out) == IG_DISPLAY_TXN_OK,
          "proven retirement accepted");
    checkTrue(out.phase == IG_DISPLAY_TXN_PHASE_RETIRED_LIVE,
          "retired-live phase classified");
    checkTrue(out.retirementEvidencePresent && out.retirementProven &&
              !out.completionUncertain,
          "exact retirement proof propagated");
    checkTrue(out.newSurfaceIsLiveScanout,
          "new surface becomes live scanout state only after retirement proof");
    checkTrue(!out.oldSurfaceMustRemainPinned &&
              out.oldSurfaceBackingReleaseAuthorized,
          "only old backing becomes releaseable");
    checkTrue(out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "new surface and mapping remain live/pinned");
    checkTrue(!out.ggttRestoreMayBeAttempted &&
              out.ggttRestoreBlockedByDisplayState,
          "live target mapping is not rollback material");
    checkTrue(!out.quarantineOnAbortOrUncertainCompletion &&
              !out.quarantineOldSurface && !out.quarantineNewSurface &&
              !out.quarantineTargetMapping,
          "proven retirement clears uncertainty quarantine policy");
    checkAlwaysPure(out);
}

static void testContradictionsAndAtomicity()
{
    std::puts("6. impossible phase order and weakened inherited proof refuse atomically");
    const auto transition = goldenTransition();
    auto progress = committedProgress();

    auto p = progress;
    p.ggttTransactionPrepared = false;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_MAPPING_PROOF,
                  "commit without prepare refused");

    p = {};
    p.targetMappingExactReadbackVerified = true;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_MAPPING_PROOF,
                  "readback proof without commit refused");

    p = committedProgress();
    p.targetMappingExactReadbackVerified = false;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_MAPPING_PROOF,
                  "commit without exact readback refused");

    p = {};
    p.noArmProgrammingIssued = true;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_MAPPING_PROOF,
                  "noarm without committed exact mapping refused");

    p = committedProgress();
    p.armControlWriteIssued = true;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_SEQUENCE,
                  "arm control before noarm refused");

    p = committedProgress();
    p.noArmProgrammingIssued = true;
    p.surfaceArmWriteIssued = true;
    expectRefusal(transition, p, nullptr, IG_DISPLAY_TXN_ERR_SEQUENCE,
                  "surface arm before control refused");

    const auto retirement = goldenRetirement(true);
    p = committedProgress();
    expectRefusal(transition, p, &retirement,
                  IG_DISPLAY_TXN_ERR_RETIREMENT_BEFORE_ARM,
                  "retirement evidence before surface arm refused");

    p.noArmProgrammingIssued = true;
    p.armControlWriteIssued = true;
    p.surfaceArmWriteIssued = true;
    auto badRetirement = retirement;
    badRetirement.runtimeWriteAuthorized = true;
    expectRefusal(transition, p, &badRetirement,
                  IG_DISPLAY_TXN_ERR_RETIREMENT,
                  "write-authorized retirement evidence refused");

    badRetirement = retirement;
    badRetirement.deviceId = 0x9A40u;
    expectRefusal(transition, p, &badRetirement,
                  IG_DISPLAY_TXN_ERR_RETIREMENT_IDENTITY,
                  "retirement evidence for nearby device refused");

    badRetirement = retirement;
    badRetirement.targetMappingReleaseAuthorized = true;
    expectRefusal(transition, p, &badRetirement,
                  IG_DISPLAY_TXN_ERR_RETIREMENT,
                  "retirement proof cannot release live target mapping");

    auto badTransition = transition;
    badTransition.runtimeWriteAuthorized = true;
    expectRefusal(badTransition, p, nullptr,
                  IG_DISPLAY_TXN_ERR_TRANSITION_WRITE_AUTH,
                  "write-authorized transition refused");

    badTransition = transition;
    badTransition.valid = false;
    expectRefusal(badTransition, p, nullptr,
                  IG_DISPLAY_TXN_ERR_TRANSITION,
                  "invalid inherited transition refused");
}

static void testNullsNamesAndPhases()
{
    std::puts("7. nulls, verdict names and phase names remain stable");
    const auto transition = goldenTransition();
    const auto progress = committedProgress();
    IgDisplayTransactionLifetimePlan out = {};

    checkTrue(igDisplayTransactionLifetimePlan(nullptr, &progress, nullptr, &out) ==
              IG_DISPLAY_TXN_ERR_NULL_TRANSITION,
          "null transition refused");
    checkTrue(igDisplayTransactionLifetimePlan(&transition, nullptr, nullptr, &out) ==
              IG_DISPLAY_TXN_ERR_NULL_PROGRESS,
          "null progress refused");
    checkTrue(igDisplayTransactionLifetimePlan(
              &transition, &progress, nullptr, nullptr) ==
              IG_DISPLAY_TXN_ERR_NULL_OUTPUT,
          "null output refused");

    for (IGU32 v = 0u; v <= IG_DISPLAY_TXN_VERDICT_LIMIT; ++v) {
        checkTrue(std::strcmp(igDisplayTransactionLifetimeVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    checkTrue(std::strcmp(igDisplayTransactionLifetimeVerdictName(
                          IG_DISPLAY_TXN_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");

    for (IGU32 p = IG_DISPLAY_TXN_PHASE_PRE_MAPPING;
         p <= IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE; ++p) {
        checkTrue(std::strcmp(igDisplayTransactionLifetimePhaseName(p), "unknown") != 0,
              "in-range phase named");
    }
    checkTrue(std::strcmp(igDisplayTransactionLifetimePhaseName(0u), "unknown") == 0,
          "out-of-range phase unknown");
}

int main()
{
    testPreMappingAndCommitted();
    testNoArmAndArmControlBoundaries();
    testArmedAwaitingRetirement();
    testUncertainCompletionQuarantines();
    testProvenRetirementMakesTargetLive();
    testContradictionsAndAtomicity();
    testNullsNamesAndPhases();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
