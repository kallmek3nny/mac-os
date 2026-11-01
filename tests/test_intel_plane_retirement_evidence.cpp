// SPDX-License-Identifier: MIT
#include "IntelPlaneRetirementEvidence.hpp"

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

static IgPlaneRetirementObservation goldenObservation()
{
    IgPlaneRetirementObservation o = {};
    o.deviceId = IG_DEVICE_OBSERVED_2026_08_16;
    o.pipe = IG_TRANSCODER_A;
    o.plane = IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE;
    o.surfaceArmIssued = true;
    o.flipDoneInterruptUnmaskedBeforeArm = true;
    o.flipDoneInterruptEnabledBeforeArm = true;
    o.staleFlipDoneStatusClearedBeforeArm = true;
    o.exactPrimaryPlaneFlipDoneObserved = true;
    o.flipDoneObservedAfterArm = true;
    o.boundedCompletionWaitUsed = true;
    o.boundedCompletionWaitFinished = true;
    o.completionWaitTimedOut = false;
    o.vblankAdvancedAfterArm = true;
    o.coherentTargetLiveObservedAfterArm = true;
    return o;
}

static void poison(IgPlaneRetirementEvidencePlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const IgPlaneRetirementEvidencePlan &a,
                      const IgPlaneRetirementEvidencePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const IgPlaneTransitionPlan &transition,
                          const IgPlaneRetirementObservation &observation,
                          IGU32 expected,
                          const char *what)
{
    IgPlaneRetirementEvidencePlan out = {};
    poison(&out);
    const auto before = out;
    const IGU32 verdict =
        igPlaneRetirementEvidencePlan(&transition, &observation, &out);
    check(verdict == expected, what);
    check(sameBytes(out, before), "refusal leaves output untouched");
}

static void checkAlwaysNoWrite(const IgPlaneRetirementEvidencePlan &p)
{
    check(p.synchronousFlipRequired, "retirement proof stays synchronous");
    check(p.flipDoneBit == (1u << 3), "TGL pipe-A plane-1 flip-done is bit 3");
    check(p.exactPrimaryPlaneFlipDoneRequired,
          "exact primary-plane completion event required");
    check(p.flipDoneMustBeUnmaskedBeforeArm &&
              p.flipDoneMustBeEnabledBeforeArm &&
              p.staleFlipDoneMustBeClearedBeforeArm,
          "interrupt path must be prepared before surface arm");
    check(p.completionEventMustFollowSurfaceArm,
          "completion event must be fresh after the arm point");
    check(p.boundedCompletionWaitRequired,
          "completion wait is explicitly bounded");
    check(p.doubleBufferStallMayDelayCompletion,
          "double-buffer stall cannot be mistaken for completion");
    check(p.disabledPlaneImmediateEventNotApplicable,
          "disabled-plane immediate-event special case is excluded");
    check(!p.vblankOnlyRetirementProofAccepted,
          "vblank-only evidence is not retirement proof");
    check(!p.surfLiveOnlyRetirementProofAccepted,
          "SURFLIVE-only evidence is not retirement proof");
    check(!p.targetMappingReleaseAuthorized,
          "new active target mapping never becomes releaseable here");
    check(!p.rollbackSafe, "completion does not invent rollback safety");
    check(!p.runtimeWriteAuthorized,
          "retirement evidence planner never authorizes display writes");
}

static void testGoldenFlipDoneRetirement()
{
    std::puts("1. exact fresh bounded flip-done evidence retires only old backing");
    const auto transition = goldenTransition();
    const auto observation = goldenObservation();
    IgPlaneRetirementEvidencePlan out = {};

    const IGU32 verdict =
        igPlaneRetirementEvidencePlan(&transition, &observation, &out);
    check(verdict == IG_PLANE_RETIREMENT_OK, "golden evidence accepted");
    check(out.valid, "golden retirement plan valid");
    check(out.deviceId == IG_DEVICE_OBSERVED_2026_08_16 &&
              out.pipe == IG_TRANSCODER_A &&
              out.plane == IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE,
          "exact target identity retained");
    check(out.interruptSetupProven, "pre-arm interrupt setup proven");
    check(out.exactFreshFlipDoneProven, "fresh exact flip-done proven");
    check(out.boundedCompletionProven, "bounded completion proven");
    check(out.retirementProven, "old-surface retirement proven");
    check(!out.oldSurfaceMustRemainPinned &&
              out.oldSurfaceBackingReleaseAuthorized,
          "old backing may be released only after retirement proof");
    check(out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "new active surface and mapping remain pinned");
    check(!out.completionUncertain,
          "proven completion is not marked uncertain");
    check(!out.quarantineOldSurfaceOnUncertainCompletion &&
              !out.quarantineNewSurfaceOnUncertainCompletion &&
              !out.quarantineTargetMappingOnUncertainCompletion,
          "successful retirement needs no uncertainty quarantine");
    checkAlwaysNoWrite(out);
}

static void testTimeoutQuarantine()
{
    std::puts("2. bounded timeout retains and quarantines every possibly-live object");
    const auto transition = goldenTransition();
    auto observation = goldenObservation();
    observation.exactPrimaryPlaneFlipDoneObserved = false;
    observation.flipDoneObservedAfterArm = false;
    observation.completionWaitTimedOut = true;
    observation.vblankAdvancedAfterArm = true;
    observation.coherentTargetLiveObservedAfterArm = true;

    IgPlaneRetirementEvidencePlan out = {};
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "timeout yields safety policy instead of optimistic refusal");
    check(out.interruptSetupProven, "timeout does not erase valid interrupt setup");
    check(!out.exactFreshFlipDoneProven && !out.boundedCompletionProven,
          "timeout has no fresh bounded completion proof");
    check(!out.retirementProven && out.completionUncertain,
          "timeout leaves retirement uncertain");
    check(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "timeout retains both surfaces and target mapping");
    check(!out.oldSurfaceBackingReleaseAuthorized,
          "timeout never authorizes old backing release");
    check(out.quarantineOldSurfaceOnUncertainCompletion &&
              out.quarantineNewSurfaceOnUncertainCompletion &&
              out.quarantineTargetMappingOnUncertainCompletion,
          "timeout quarantines all possibly referenced display objects");
    checkAlwaysNoWrite(out);
}

static void testCorroborationIsNotRetirement()
{
    std::puts("3. vblank or coherent SURFLIVE alone cannot retire old backing");
    const auto transition = goldenTransition();

    auto observation = goldenObservation();
    observation.exactPrimaryPlaneFlipDoneObserved = false;
    observation.flipDoneObservedAfterArm = false;
    observation.vblankAdvancedAfterArm = true;
    observation.coherentTargetLiveObservedAfterArm = false;
    IgPlaneRetirementEvidencePlan out = {};
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "vblank-only observation accepted as uncertain policy input");
    check(!out.retirementProven && out.oldSurfaceMustRemainPinned,
          "vblank-only observation does not release old backing");

    observation.vblankAdvancedAfterArm = false;
    observation.coherentTargetLiveObservedAfterArm = true;
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "SURFLIVE-only observation accepted as uncertain policy input");
    check(!out.retirementProven && out.oldSurfaceMustRemainPinned,
          "SURFLIVE-only observation does not release old backing");
    checkAlwaysNoWrite(out);
}

static void testStaleOrUnpreparedInterruptIsNotProof()
{
    std::puts("4. stale or unprepared flip-done evidence remains ambiguous");
    const auto transition = goldenTransition();

    auto observation = goldenObservation();
    observation.staleFlipDoneStatusClearedBeforeArm = false;
    IgPlaneRetirementEvidencePlan out = {};
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "uncleared status yields conservative plan");
    check(!out.interruptSetupProven && !out.retirementProven,
          "uncleared stale status cannot prove fresh completion");
    check(out.oldSurfaceMustRemainPinned && out.completionUncertain,
          "uncleared stale status retains old backing");

    observation = goldenObservation();
    observation.flipDoneInterruptUnmaskedBeforeArm = false;
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "masked pre-arm event path yields conservative plan");
    check(!out.retirementProven && out.quarantineOldSurfaceOnUncertainCompletion,
          "masked event path cannot prove retirement");

    observation = goldenObservation();
    observation.flipDoneInterruptEnabledBeforeArm = false;
    check(igPlaneRetirementEvidencePlan(&transition, &observation, &out) ==
              IG_PLANE_RETIREMENT_OK,
          "disabled pre-arm event path yields conservative plan");
    check(!out.retirementProven && out.quarantineTargetMappingOnUncertainCompletion,
          "disabled event path preserves mapping quarantine");
    checkAlwaysNoWrite(out);
}

static void testIdentityTransitionAndAtomicity()
{
    std::puts("5. exact identity and inherited transition safety are fail-closed");
    const auto transition = goldenTransition();
    const auto observation = goldenObservation();
    IgPlaneRetirementEvidencePlan out = {};

    check(igPlaneRetirementEvidencePlan(&transition, &observation, nullptr) ==
              IG_PLANE_RETIREMENT_ERR_NULL_OUTPUT,
          "null output refused");
    check(igPlaneRetirementEvidencePlan(nullptr, &observation, &out) ==
              IG_PLANE_RETIREMENT_ERR_NULL_TRANSITION,
          "null transition refused");
    check(igPlaneRetirementEvidencePlan(&transition, nullptr, &out) ==
              IG_PLANE_RETIREMENT_ERR_NULL_OBSERVATION,
          "null observation refused");

    auto o = observation;
    o.deviceId = 0x9A40u;
    expectRefusal(transition, o, IG_PLANE_RETIREMENT_ERR_WRONG_DEVICE,
                  "nearby Tiger Lake id refused");
    o = observation;
    o.pipe = IG_TRANSCODER_B;
    expectRefusal(transition, o, IG_PLANE_RETIREMENT_ERR_WRONG_PIPE,
                  "wrong pipe refused");
    o = observation;
    o.plane = 1u;
    expectRefusal(transition, o, IG_PLANE_RETIREMENT_ERR_WRONG_PLANE,
                  "wrong plane refused");
    o = observation;
    o.surfaceArmIssued = false;
    o.exactPrimaryPlaneFlipDoneObserved = false;
    o.flipDoneObservedAfterArm = false;
    o.boundedCompletionWaitUsed = false;
    o.boundedCompletionWaitFinished = false;
    expectRefusal(transition, o, IG_PLANE_RETIREMENT_ERR_ARM_NOT_ISSUED,
                  "no surface arm means no transition retirement evaluation");

    auto t = transition;
    t.valid = false;
    expectRefusal(t, observation, IG_PLANE_RETIREMENT_ERR_TRANSITION,
                  "invalid inherited transition refused");
    t = transition;
    t.runtimeWriteAuthorized = true;
    expectRefusal(t, observation, IG_PLANE_RETIREMENT_ERR_TRANSITION_WRITE_AUTH,
                  "write-authorized transition refused");
    t = transition;
    t.retainBothSurfacesOnUncertainCompletion = false;
    expectRefusal(t, observation, IG_PLANE_RETIREMENT_ERR_TRANSITION,
                  "weakened uncertainty lifetime contract refused");
}

static void testContradictions()
{
    std::puts("6. contradictory completion observations are refused atomically");
    const auto transition = goldenTransition();
    auto observation = goldenObservation();

    observation.exactPrimaryPlaneFlipDoneObserved = false;
    expectRefusal(transition, observation, IG_PLANE_RETIREMENT_ERR_OBSERVATION,
                  "after-arm event cannot exist without exact event observation");

    observation = goldenObservation();
    observation.boundedCompletionWaitUsed = false;
    expectRefusal(transition, observation, IG_PLANE_RETIREMENT_ERR_OBSERVATION,
                  "finished wait requires a bounded wait");

    observation = goldenObservation();
    observation.completionWaitTimedOut = true;
    expectRefusal(transition, observation, IG_PLANE_RETIREMENT_ERR_OBSERVATION,
                  "timeout cannot coexist with fresh after-arm flip-done");
}

static void testVerdicts()
{
    std::puts("7. verdict names remain stable");
    for (IGU32 v = 0u; v <= IG_PLANE_RETIREMENT_VERDICT_LIMIT; ++v) {
        check(std::strcmp(igPlaneRetirementVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(igPlaneRetirementVerdictName(
                          IG_PLANE_RETIREMENT_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");
}

int main()
{
    testGoldenFlipDoneRetirement();
    testTimeoutQuarantine();
    testCorroborationIsNotRetirement();
    testStaleOrUnpreparedInterruptIsNotProof();
    testIdentityTransitionAndAtomicity();
    testContradictions();
    testVerdicts();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
