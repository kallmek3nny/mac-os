// SPDX-License-Identifier: MIT
#include "NVGspBooterSuccessRetirementPlan.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

static std::vector<unsigned char> load(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        check(false, "fixture open");
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

static NVGspBooterFuseObservation fuseObservation(unsigned raw)
{
    NVGspBooterFuseObservation observed = {};
    observed.ucodeId = NV_GSP_GA107_BOOTER_HS_UCODE_ID;
    observed.registerOffset = NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG +
        (observed.ucodeId - NV_GSP_GA107_SEC2_UCODE_FIRST_ID) *
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    observed.rawFuse = raw;
    observed.fuseVersion = nvGspBooterFuseVersionFromMask(raw);
    observed.valid = true;
    return observed;
}

static std::vector<unsigned char> privateImage(
    const std::vector<unsigned char> &blob,
    const NVGspBooterFuseObservation &observed)
{
    NVGspBooterHsPlan hs = {};
    check(nvGspPlanBooterHs(blob.data(), blob.size(), &hs) == NV_GSP_BOOTER_HS_OK,
          "fixture hs parse");
    std::vector<unsigned char> image(hs.firmwareBytes, 0x5au);
    NVGspBooterFusePatchPlan built = {};
    check(nvGspBuildObservedBooterPrivateImage(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), &built) == NV_GSP_BOOTER_FUSE_PATCH_OK,
          "fixture private image");
    check(built.valid, "fixture private image valid");
    return image;
}

static NVGspBooterExecSequencePlan sequencePlan(const std::vector<unsigned char> &blob)
{
    const auto observed = fuseObservation(0u);
    const auto image = privateImage(blob, observed);
    NVGspBooterExecSequencePlan out = {};
    check(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), 0x12345678abcdef01ull, &out) ==
          NV_GSP_BOOTER_EXEC_OK,
          "fixture exec sequence accepted");
    check(out.valid, "fixture exec sequence valid");
    check(!out.runtimeWriteAuthorized, "fixture sequence remains no-write");
    return out;
}

static NVGspBooterSuccessObservation cleanSuccess(
    const NVGspBooterExecSequencePlan &sequence)
{
    NVGspBooterSuccessObservation observed = {};
    observed.allFirmwareDmaIdleObserved = true;
    observed.wprMetaAddressPublished = true;
    observed.bootStarted = true;
    observed.haltObserved = true;
    observed.haltWithinBudget = true;
    observed.mailbox0Observed = true;
    observed.mailbox0Value = sequence.mailbox0SuccessValue;
    observed.mailbox1Observed = true;
    observed.mailboxReadbackAfterHalt = true;
    observed.mailbox0BeforeMailbox1Readback = true;
    return observed;
}

static bool sameBytes(const NVGspBooterSuccessRetirementPlan &a,
                      const NVGspBooterSuccessRetirementPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void testGoldenSuccess(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("1. exact bounded success retires both temporary backings");
    const auto observed = cleanSuccess(sequence);
    NVGspBooterSuccessRetirementPlan plan = {};
    const auto verdict = nvGspPlanGa107BooterSuccessRetirement(
        &sequence, &observed, &plan);

    check(verdict == NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK,
          "clean success observation accepted");
    check(plan.valid, "success plan valid");
    check(plan.firmwareDmaCompletionProven,
          "all firmware dma completion proven");
    check(!plan.firmwareStagingMayStillBeReferenced &&
          plan.firmwareStagingReleaseAuthorized,
          "firmware staging may be released");
    check(plan.wprMetaWasPublished && plan.bootWasStarted,
          "wpr metadata publication and boot start proven");
    check(plan.haltProven && plan.mailbox0SuccessProven &&
          plan.mailboxPairReadbackProven,
          "bounded halt and ordered mailbox success proven");
    check(!plan.wprMetaMayStillBeReferenced && plan.wprMetaReleaseAuthorized,
          "successful booter retires wpr metadata backing");
    check(plan.recoveryGuardMayBeDismissed,
          "successful path may dismiss failure recovery guard");
    check(!plan.postSuccessResetRequiredForBackingRelease,
          "successful backing release does not require compensating reset");
    check(!plan.failureRecoveryPlannerRequired,
          "clean success does not route through failure planner");
    check(!plan.retryAuthorized && !plan.runtimeRecoveryAuthorized &&
          !plan.runtimeWriteAuthorized,
          "pure success planner authorizes no writes/recovery/retry");
}

static void expectRefusal(
    const NVGspBooterExecSequencePlan &sequence,
    const NVGspBooterSuccessObservation &observed,
    NVGspBooterHsU32 expected,
    const char *label)
{
    NVGspBooterSuccessRetirementPlan out = {};
    std::memset(&out, 0xA5, sizeof(out));
    const auto before = out;
    const auto verdict = nvGspPlanGa107BooterSuccessRetirement(
        &sequence, &observed, &out);
    if (verdict != expected) {
        std::printf("FAIL: %s verdict=%s expected=%s\n", label,
                    nvGspBooterSuccessRetirementVerdictName(verdict),
                    nvGspBooterSuccessRetirementVerdictName(expected));
    }
    check(verdict == expected, label);
    check(sameBytes(out, before), "refusal leaves output untouched");
}

static void testCompletionRefusals(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("2. missing dma/halt/mailbox proof cannot retire backing");

    auto observed = cleanSuccess(sequence);
    observed.allFirmwareDmaIdleObserved = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_DMA_NOT_RETIRED,
                  "missing dma-idle retirement proof refused");

    observed = cleanSuccess(sequence);
    observed.haltObserved = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN,
                  "missing halt proof refused");

    observed = cleanSuccess(sequence);
    observed.haltWithinBudget = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN,
                  "late/unbounded halt cannot prove clean success");

    observed = cleanSuccess(sequence);
    observed.mailbox0Observed = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_NOT_PROVEN,
                  "mailbox0 must be observed");

    observed = cleanSuccess(sequence);
    observed.mailbox1Observed = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_NOT_PROVEN,
                  "mailbox pair readback must complete");

    observed = cleanSuccess(sequence);
    observed.mailbox0Value = sequence.mailbox0SuccessValue + 1u;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_FAILURE,
                  "nonzero mailbox0 is booter failure");
}

static void testPublicationAndOrderRefusals(
    const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("3. publication/start/readback order must match the exact run");

    auto observed = cleanSuccess(sequence);
    observed.wprMetaAddressPublished = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION,
                  "unpublished wpr metadata cannot be retired by this run");

    observed = cleanSuccess(sequence);
    observed.bootStarted = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION,
                  "unstarted booter cannot report success");

    observed = cleanSuccess(sequence);
    observed.mailboxReadbackAfterHalt = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER,
                  "mailbox read before halt cannot prove bounded completion");

    observed = cleanSuccess(sequence);
    observed.mailbox0BeforeMailbox1Readback = false;
    expectRefusal(sequence, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER,
                  "mailbox readback order mismatch refused");
}

static void testSequenceRefusals(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("4. weakened or write-capable input sequences are refused");
    const auto observed = cleanSuccess(sequence);

    auto bad = sequence;
    bad.runtimeWriteAuthorized = true;
    expectRefusal(bad, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE,
                  "write-authorized sequence refused");

    bad = sequence;
    bad.timeoutRecoveryProven = true;
    expectRefusal(bad, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE,
                  "invented timeout recovery proof refused");

    bad = sequence;
    bad.perTransferIdlePollRequired = false;
    expectRefusal(bad, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE,
                  "dma idle requirement cannot be weakened");

    bad = sequence;
    bad.wprMetaBackingRetainUntilHalt = false;
    expectRefusal(bad, observed,
                  NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE,
                  "wpr lifetime contract cannot be weakened");
}

static void testNullsAndVerdictNames(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("5. nulls and verdict names remain stable");
    const auto observed = cleanSuccess(sequence);
    NVGspBooterSuccessRetirementPlan out = {};

    check(nvGspPlanGa107BooterSuccessRetirement(nullptr, &observed, &out) ==
          NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_SEQUENCE,
          "null sequence refused");
    check(nvGspPlanGa107BooterSuccessRetirement(&sequence, nullptr, &out) ==
          NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION,
          "null observation refused");
    check(nvGspPlanGa107BooterSuccessRetirement(&sequence, &observed, nullptr) ==
          NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_OUTPUT,
          "null output refused");

    for (NVGspBooterHsU32 v = 0u;
         v <= NV_GSP_BOOTER_SUCCESS_RETIREMENT_VERDICT_LIMIT; ++v) {
        check(std::strcmp(nvGspBooterSuccessRetirementVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(nvGspBooterSuccessRetirementVerdictName(
                          NV_GSP_BOOTER_SUCCESS_RETIREMENT_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");
}

// ctest runs each suite from tests/build, so the cwd-relative path this used
// to open resolved to nothing there: MSVC ctest failed on 'fixture open' while
// the g++ reading, run from the repo root, passed 59/0. CMake injects the
// source-tree path as NV_FIRMWARE_DIR (tests/CMakeLists.txt), the same way
// test_riscv_ucode.cpp finds its files. The fallback keeps a hand-built binary
// run from the repo root working; a wrong path still FAILS here, never skips.
#ifndef NV_FIRMWARE_DIR
#define NV_FIRMWARE_DIR "reference/ga107-firmware"
#endif

int main()
{
    const auto blob = load(NV_FIRMWARE_DIR "/booter_load-570.144.bin");
    if (blob.empty())
        return 1;

    const auto sequence = sequencePlan(blob);
    testGoldenSuccess(sequence);
    testCompletionRefusals(sequence);
    testPublicationAndOrderRefusals(sequence);
    testSequenceRefusals(sequence);
    testNullsAndVerdictNames(sequence);

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
