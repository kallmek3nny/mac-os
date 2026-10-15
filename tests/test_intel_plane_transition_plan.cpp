// SPDX-License-Identifier: MIT
#include "IntelPlaneTransitionPlan.hpp"

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

static IgPlaneLiveSample observedSample()
{
    IgPlaneLiveSample sample = {};
    sample.deviceId = 0x9A68u;
    sample.pipe = IG_TRANSCODER_A;
    sample.surfLiveBefore = 0x01058000u;
    sample.transConf = 0xC0000000u;
    sample.pipeSrc = 0x077F0437u;
    sample.ctl = 0x82009000u;
    sample.stride = 0x0000003Cu;
    sample.pos = 0x00000000u;
    sample.size = 0x0437077Fu;
    sample.surf = 0x01058000u;
    sample.offset = 0x00000000u;
    sample.surfLiveAfter = 0x01058000u;
    return sample;
}

static IgPlaneLiveObservation goldenObservation()
{
    const IgPlaneLiveSample sample = observedSample();
    IgPlaneLiveObservation out = {};
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_OK,
          "golden live observation accepted");
    return out;
}

static const IGGgttU64 kSurfaceBytes =
    (IGGgttU64)1920u * (IGGgttU64)1080u * 4ull;
static const IGGgttU64 kGgttBase = 0x01880000ull;

static IGSinkGgttPlan goldenMapping()
{
    IGSinkGgttPlan out = {};
    const IGGgttU32 verdict = igSinkGgttPlan(
        kGgttBase,
        kSurfaceBytes,
        0x0000000100000000ull,
        0x01000000ull,
        0x00100000u,
        &out);
    check(verdict == IG_SINK_GGTT_PLAN_OK, "golden GGTT mapping plan accepted");
    return out;
}

static IgPlaneProgramRequest goldenRequest()
{
    IgPlaneProgramRequest request = {};
    request.deviceId = 0x9A68u;
    request.pipe = IG_TRANSCODER_A;
    request.plane = IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE;
    request.format = IG_PLANE_PROGRAM_PLAN_FORMAT_XRGB8888;
    request.tiling = IG_PLANE_PROGRAM_PLAN_TILING_LINEAR;
    request.rotation = IG_PLANE_PROGRAM_PLAN_ROTATION_0;
    request.sourceWidth = 1920u;
    request.sourceHeight = 1080u;
    request.destWidth = 1920u;
    request.destHeight = 1080u;
    request.strideBytes = 1920u * 4u;
    request.surfaceBytes = kSurfaceBytes;
    request.enable = true;
    request.ggttMappingOwned = true;
    request.ggttMappingCommitted = true;
    request.ggttReadbackExact = true;
    return request;
}

static IgPlaneProgramPlan goldenTarget()
{
    const auto prior = goldenObservation();
    const auto mapping = goldenMapping();
    const auto request = goldenRequest();
    IgPlaneProgramPlan out = {};
    check(igPlaneProgramPlan(&prior, &mapping, &request, &out) ==
              IG_PLANE_PROGRAM_PLAN_OK,
          "golden target program plan accepted");
    return out;
}

static void poison(IgPlaneTransitionPlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const IgPlaneTransitionPlan &a,
                      const IgPlaneTransitionPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const IgPlaneLiveObservation &prior,
                          const IgPlaneProgramPlan &target,
                          IGU32 expected,
                          const char *what)
{
    IgPlaneTransitionPlan out = {};
    poison(&out);
    const IgPlaneTransitionPlan before = out;
    const IGU32 verdict = igPlaneTransitionPlan(&prior, &target, &out);
    check(verdict == expected, what);
    check(sameBytes(out, before), "refusal leaves output untouched");
}

static void testGoldenStateChange()
{
    std::puts("1. observed firmware plane yields enabled state-change contract");
    const auto prior = goldenObservation();
    const auto target = goldenTarget();
    IgPlaneTransitionPlan out = {};

    const IGU32 verdict = igPlaneTransitionPlan(&prior, &target, &out);
    check(verdict == IG_PLANE_TRANSITION_OK, "golden transition accepted");
    check(out.valid, "transition plan valid");
    check(out.deviceId == 0x9A68u && out.pipe == IG_TRANSCODER_A && out.plane == 0u,
          "exact device/pipe/primary-plane relationship retained");
    check(out.operationKind == IG_PLANE_TRANSITION_KIND_STATE_CHANGE_ENABLED,
          "operation classified as state change while enabled");
    check(out.currentPlaneEnabled && out.targetPlaneEnabled &&
              out.stateChangeWhileEnabled,
          "enabled-to-enabled state transition retained");
    check(!out.surfaceOnlyFlip && !out.enableFromDisabled,
          "unsupported operation classes are not claimed");
    check(out.requiresNoArmProgramming && out.requiresArmControlWrite &&
              out.requiresSurfaceArmWrite,
          "noarm/control/surface responsibilities retained");
    check(out.armControlImmediatelyBeforeSurface,
          "control must immediately precede surface arm");
    check(!out.planeDisableRequired,
          "narrow enabled-to-enabled i915 path does not require plane disable");
    check(out.oldSurfaceMustRemainPinned && out.newSurfaceMustRemainPinned &&
              out.targetMappingMustRemainCommitted,
          "both surfaces and target mapping remain live through retirement");
    check(out.flipCompletionRequired && out.retirementProofRequired &&
              out.flipDoneOrEquivalentProofRequired,
          "completion and retirement proof are mandatory");
    check(out.ggttRestoreForbiddenUntilRetirement,
          "GGTT restore is forbidden before retirement proof");
    check(out.noWriteAbortSafe && out.postNoArmRollbackProofRequired,
          "rollback boundary is explicit before and after noarm state");
    check(!out.postArmRollbackSafe && out.timeoutMayHaveLatched,
          "post-arm timeout never claims safe rollback");
    check(out.quarantineOnUncertainCompletion &&
              out.retainBothSurfacesOnUncertainCompletion,
          "uncertain completion retains/quarantines both surface lifetimes");
    check(!out.runtimeWriteAuthorized,
          "pure transition planner never authorizes display writes");
}

static void testNullsIdentityAndPrior()
{
    std::puts("2. nulls, exact identity and coherent enabled prior are fail-closed");
    const auto prior = goldenObservation();
    const auto target = goldenTarget();
    IgPlaneTransitionPlan out = {};

    check(igPlaneTransitionPlan(&prior, &target, nullptr) ==
              IG_PLANE_TRANSITION_ERR_NULL_OUTPUT,
          "null output refused");
    check(igPlaneTransitionPlan(nullptr, &target, &out) ==
              IG_PLANE_TRANSITION_ERR_NULL_PRIOR,
          "null prior refused");
    check(igPlaneTransitionPlan(&prior, nullptr, &out) ==
              IG_PLANE_TRANSITION_ERR_NULL_TARGET,
          "null target refused");

    auto p = prior;
    p.deviceId = 0x9A40u;
    expectRefusal(p, target, IG_PLANE_TRANSITION_ERR_WRONG_DEVICE,
                  "nearby Tiger Lake id refused");

    p = prior;
    p.pipe = IG_TRANSCODER_B;
    expectRefusal(p, target, IG_PLANE_TRANSITION_ERR_WRONG_PIPE,
                  "pipe mismatch refused");

    auto t = target;
    t.plane = 1u;
    expectRefusal(prior, t, IG_PLANE_TRANSITION_ERR_WRONG_PLANE,
                  "secondary plane target refused");

    p = prior;
    p.planeEnabled = false;
    p.ctlWord &= ~IG_FB_CTL_ENABLE;
    expectRefusal(p, target, IG_PLANE_TRANSITION_ERR_ENABLE_FROM_DISABLED,
                  "enable-from-disabled is a separate refused transaction class");

    p = prior;
    p.liveStable = false;
    expectRefusal(p, target, IG_PLANE_TRANSITION_ERR_INCOHERENT_PRIOR,
                  "unstable prior refused");

    p = prior;
    p.ctlWord |= IG_FB_CTL_ASYNC_FLIP;
    expectRefusal(p, target, IG_PLANE_TRANSITION_ERR_CURRENT_ASYNC,
                  "current async-flip mode refused");
}

static void testTargetContract()
{
    std::puts("3. target must remain the narrow pure merged programming plan");
    const auto prior = goldenObservation();
    const auto target = goldenTarget();

    auto t = target;
    t.valid = false;
    expectRefusal(prior, t, IG_PLANE_TRANSITION_ERR_TARGET_INVALID,
                  "invalid target program plan refused");

    t = target;
    t.fullPipe = false;
    expectRefusal(prior, t, IG_PLANE_TRANSITION_ERR_TARGET_INVALID,
                  "non-full-pipe target refused");

    t = target;
    t.mappingPinRequired = false;
    expectRefusal(prior, t, IG_PLANE_TRANSITION_ERR_TARGET_INVALID,
                  "target without mapping pin requirement refused");

    t = target;
    t.runtimeWriteAuthorized = true;
    expectRefusal(prior, t, IG_PLANE_TRANSITION_ERR_TARGET_WRITE_AUTHORIZED,
                  "input planner may not pre-authorize runtime writes");
}

static void testSurfaceOnlyRefusal()
{
    std::puts("4. apparent surface-only state is refused without exact state equivalence");
    auto prior = goldenObservation();
    const auto target = goldenTarget();

    const IGU32 formatMask = IG_FB_CTL_FORMAT_MASK << IG_FB_CTL_FORMAT_SHIFT;
    const IGU32 tilingMask = IG_FB_CTL_TILING_MASK << IG_FB_CTL_TILING_SHIFT;
    prior.ctlWord &= ~(formatMask | tilingMask |
                       IG_FB_CTL_RENDER_DECOMP | IG_FB_CTL_ASYNC_FLIP);
    prior.ctlWord |= IG_FB_CTL_ENABLE |
        (IG_FB_FORMAT_XRGB_8888 << IG_FB_CTL_FORMAT_SHIFT) |
        (IG_FB_TILING_LINEAR << IG_FB_CTL_TILING_SHIFT);
    prior.strideWord = target.linearStrideUnits;
    prior.strideBytes = target.strideBytes;
    prior.posWord = 0u;
    prior.offsetWord = 0u;

    expectRefusal(prior, target, IG_PLANE_TRANSITION_ERR_SURFACE_ONLY_UNPROVEN,
                  "surface-only flip requires stronger exact-state proof");
}

static void testVerdictsAndAtomicity()
{
    std::puts("5. verdict names are stable and all refusals are atomic");
    for (IGU32 v = 0u; v <= IG_PLANE_TRANSITION_VERDICT_LIMIT; ++v) {
        const char *name = igPlaneTransitionVerdictName(v);
        check(name != nullptr && std::strcmp(name, "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(igPlaneTransitionVerdictName(
                          IG_PLANE_TRANSITION_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");

    const auto prior = goldenObservation();
    auto target = goldenTarget();
    target.valid = false;
    IgPlaneTransitionPlan out = {};
    poison(&out);
    const IgPlaneTransitionPlan before = out;
    check(igPlaneTransitionPlan(&prior, &target, &out) ==
              IG_PLANE_TRANSITION_ERR_TARGET_INVALID,
          "atomic refusal trigger");
    check(sameBytes(out, before), "atomic refusal preserves entire output");
}

int main()
{
    testGoldenStateChange();
    testNullsIdentityAndPrior();
    testTargetContract();
    testSurfaceOnlyRefusal();
    testVerdictsAndAtomicity();

    if (gFailures != 0u) {
        std::printf("%u/%u Intel plane transition checks failed\n",
                    gFailures, gChecks);
        return 1;
    }

    std::printf("all %u Intel plane transition checks passed\n", gChecks);
    return 0;
}
