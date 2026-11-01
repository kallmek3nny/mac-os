// SPDX-License-Identifier: MIT
#include "NVGspBooterRecoveryPlan.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ctest runs each suite from tests/build, where a cwd-relative
// reference/ga107-firmware path resolves to nothing: MSVC ctest failed
// test_nv_gsp_booter_success_retirement_plan on 'fixture open' for exactly
// that (fixed in f5110c1). CMake injects the source-tree path as
// NV_FIRMWARE_DIR (tests/CMakeLists.txt); the fallback keeps a hand-built
// binary run from the repo root working. A wrong path still FAILS, never skips.
#ifndef NV_FIRMWARE_DIR
#define NV_FIRMWARE_DIR "reference/ga107-firmware"
#endif

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

static NVGspBooterFuseObservation observation(unsigned raw)
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
    const auto observed = observation(0u);
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

static NVGspBooterRecoveryPlan recover(
    const NVGspBooterExecSequencePlan &sequence,
    NVGspBooterHsU32 event,
    bool haltProven,
    bool mailbox0Observed,
    NVGspBooterHsU32 mailbox0Value,
    bool laterDmaIdleObserved,
    bool postFailureResetObserved,
    const char *label)
{
    NVGspBooterRecoveryObservation observed = {};
    observed.event = event;
    observed.haltProven = haltProven;
    observed.mailbox0Observed = mailbox0Observed;
    observed.mailbox0Value = mailbox0Value;
    observed.laterDmaIdleObserved = laterDmaIdleObserved;
    observed.postFailureResetObserved = postFailureResetObserved;

    NVGspBooterRecoveryPlan out = {};
    const auto verdict = nvGspPlanGa107BooterRecovery(&sequence, &observed, &out);
    if (verdict != NV_GSP_BOOTER_RECOVERY_OK)
        std::printf("FAIL: %s verdict=%s\n", label,
                    nvGspBooterRecoveryVerdictName(verdict));
    check(verdict == NV_GSP_BOOTER_RECOVERY_OK, "recovery plan accepted");
    check(out.valid, "recovery plan valid");
    return out;
}

static bool sameBytes(const NVGspBooterRecoveryPlan &a,
                      const NVGspBooterRecoveryPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void commonNoWriteChecks(const NVGspBooterRecoveryPlan &p)
{
    check(p.requiresFreshExactTargetOwnership, "fresh exact target ownership required");
    check(p.requiresFreshSequenceRevalidation, "fresh sequence revalidation required");
    check(!p.retryAuthorized, "retry never authorized by recovery planner");
    check(!p.runtimeRecoveryAuthorized, "runtime recovery never authorized");
    check(!p.runtimeWriteAuthorized, "runtime writes never authorized");
    check(!p.resetAloneProvesRetirement, "reset alone never proves backing retirement");
    check(!p.postFailureResetAllowsBackingRelease,
          "post-failure reset alone never grants backing release");
    check(!p.mailboxReadAfterTimeoutProvesCompletion,
          "mailbox read after timeout never proves completion");
}

static void testEarlyResetFailures(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("1. pre-dma reset/core failures create no staging references");
    const NVGspBooterHsU32 events[] = {
        NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT,
        NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT,
    };

    for (const auto event : events) {
        const auto p = recover(sequence, event, false, false, 0u,
                               false, true, "early reset failure");
        check(p.failureBeforeFirmwareDmaSubmission, "failure is before firmware dma");
        check(!p.firmwareDmaWasSubmitted && !p.bootWasStarted,
              "no dma or boot submission happened");
        check(!p.firmwareDmaCompletionUncertain && !p.bootCompletionUncertain,
              "no unsubmitted transfer can be completion-uncertain");
        check(!p.firmwareStagingMayStillBeReferenced &&
              !p.wprMetaMayStillBeReferenced,
              "no backing reference was handed to sec2");
        check(p.firmwareStagingReleaseAuthorized && p.wprMetaReleaseAuthorized,
              "unsubmitted backings may be released");
        check(!p.quarantineFirmwareStaging && !p.quarantineWprMeta,
              "unsubmitted backings do not require quarantine");
        check(p.quarantineSec2State, "failed reset/core state remains quarantined");
        commonNoWriteChecks(p);
    }
}

static void testDmaTimeout(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("2. fbdma timeout retains staging until the exact idle predicate is later proven");
    const auto uncertain = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT,
        false, false, 0u, false, true, "dma timeout uncertain");
    check(uncertain.firmwareDmaWasSubmitted && !uncertain.bootWasStarted,
          "dma submitted before boot start");
    check(uncertain.firmwareDmaCompletionUncertain,
          "dma completion remains uncertain without idle proof");
    check(uncertain.firmwareStagingMayStillBeReferenced &&
          !uncertain.firmwareStagingReleaseAuthorized &&
          uncertain.quarantineFirmwareStaging,
          "uncertain dma source retained and quarantined");
    check(!uncertain.wprMetaMayStillBeReferenced &&
          uncertain.wprMetaReleaseAuthorized && !uncertain.quarantineWprMeta,
          "wpr metadata not yet published at dma failure");
    check(uncertain.laterDmaIdleCanRetireFirmwareStaging &&
          !uncertain.laterDmaIdleRetirementObserved,
          "later exact idle observation is the staging retirement path");
    commonNoWriteChecks(uncertain);

    const auto retired = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT,
        false, false, 0u, true, true, "dma timeout later idle");
    check(!retired.firmwareDmaCompletionUncertain,
          "later idle resolves dma completion uncertainty");
    check(!retired.firmwareStagingMayStillBeReferenced &&
          retired.firmwareStagingReleaseAuthorized &&
          !retired.quarantineFirmwareStaging,
          "later idle retires staging source");
    check(retired.laterDmaIdleRetirementObserved,
          "later idle retirement observation recorded");
    check(retired.quarantineSec2State,
          "dma retirement does not prove sec2 retry safety");
    commonNoWriteChecks(retired);
}

static void testBootHaltTimeout(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("3. halt timeout quarantines wpr metadata even if mailbox0 is later readable");
    const auto noMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, false, 0u, false, true, "halt timeout no mailbox");
    check(noMailbox.firmwareDmaWasSubmitted && noMailbox.bootWasStarted,
          "boot timeout happens after dma and cpu start");
    check(!noMailbox.firmwareDmaCompletionUncertain &&
          noMailbox.firmwareStagingReleaseAuthorized,
          "firmware dma source retired before boot stage");
    check(noMailbox.bootCompletionUncertain &&
          noMailbox.wprMetaMayStillBeReferenced &&
          !noMailbox.wprMetaReleaseAuthorized && noMailbox.quarantineWprMeta,
          "uncertain boot retains wpr metadata");
    commonNoWriteChecks(noMailbox);

    const auto zeroMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, true, 0u, false, true, "halt timeout zero mailbox");
    check(zeroMailbox.bootCompletionUncertain,
          "zero mailbox after timeout does not repair failed halt proof");
    check(zeroMailbox.wprMetaMayStillBeReferenced && zeroMailbox.quarantineWprMeta,
          "zero mailbox after timeout still retains wpr metadata");
    check(!zeroMailbox.mailboxReadAfterTimeoutProvesCompletion,
          "openrm mailbox read after timeout is not completion proof");
    commonNoWriteChecks(zeroMailbox);

    const auto nonzeroMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, true, 7u, false, false, "halt timeout nonzero mailbox");
    check(nonzeroMailbox.bootCompletionUncertain && nonzeroMailbox.quarantineWprMeta,
          "nonzero mailbox after halt timeout also remains uncertain");
    commonNoWriteChecks(nonzeroMailbox);
}

static void testMailboxFailure(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("4. proven halt plus nonzero mailbox is failure, not release permission");
    const auto p = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE,
        true, true, 5u, false, true, "mailbox failure");
    check(p.haltProven && p.mailboxFailureObserved,
          "halt and mailbox failure are both recorded");
    check(!p.bootCompletionUncertain,
          "halt wait completion itself is proven");
    check(p.firmwareStagingReleaseAuthorized &&
          !p.firmwareStagingMayStillBeReferenced,
          "firmware staging already retired by dma idle");
    check(p.wprMetaMayStillBeReferenced && !p.wprMetaReleaseAuthorized &&
          p.quarantineWprMeta,
          "failed booter wpr metadata remains conservatively retained");
    check(p.quarantineSec2State, "failed booter state remains quarantined");
    commonNoWriteChecks(p);
}

static void testRefusalsAndAtomicity(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("5. non-failure and contradictory observations are refused atomically");
    NVGspBooterRecoveryPlan out = {};
    std::memset(&out, 0xA5, sizeof(out));
    const auto before = out;

    NVGspBooterRecoveryObservation resetReady = {};
    resetReady.event = NV_GSP_BOOTER_RECOVERY_EVENT_RESET_READY_TIMEOUT;
    check(nvGspPlanGa107BooterRecovery(&sequence, &resetReady, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NONFATAL_EVENT,
          "best-effort reset-ready timeout is not a recovery failure");
    check(sameBytes(out, before), "nonfatal refusal leaves output untouched");

    NVGspBooterRecoveryObservation badMailbox = {};
    badMailbox.event = NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE;
    badMailbox.haltProven = true;
    badMailbox.mailbox0Observed = true;
    badMailbox.mailbox0Value = 0u;
    check(nvGspPlanGa107BooterRecovery(&sequence, &badMailbox, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION,
          "zero mailbox cannot be classified as mailbox failure");
    check(sameBytes(out, before), "mailbox contradiction leaves output untouched");

    NVGspBooterRecoveryObservation badEarly = {};
    badEarly.event = NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT;
    badEarly.laterDmaIdleObserved = true;
    check(nvGspPlanGa107BooterRecovery(&sequence, &badEarly, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION,
          "pre-dma failure cannot claim later dma idle retirement");
    check(sameBytes(out, before), "early contradiction leaves output untouched");

    auto writeCapable = sequence;
    writeCapable.runtimeWriteAuthorized = true;
    NVGspBooterRecoveryObservation dma = {};
    dma.event = NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT;
    check(nvGspPlanGa107BooterRecovery(&writeCapable, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE,
          "write-authorized input sequence refused");
    check(sameBytes(out, before), "bad sequence leaves output untouched");

    auto recovered = sequence;
    recovered.timeoutRecoveryProven = true;
    check(nvGspPlanGa107BooterRecovery(&recovered, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE,
          "planner refuses invented pre-proven timeout recovery");
    check(sameBytes(out, before), "invented recovery proof leaves output untouched");

    check(nvGspPlanGa107BooterRecovery(nullptr, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NULL_SEQUENCE,
          "null sequence refused");
    check(nvGspPlanGa107BooterRecovery(&sequence, &dma, nullptr) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NULL_OUTPUT,
          "null output refused");
}

static void testVerdictNames(void)
{
    std::puts("6. verdict names remain stable");
    for (NVGspBooterHsU32 v = 0u; v <= NV_GSP_BOOTER_RECOVERY_VERDICT_LIMIT; ++v) {
        check(std::strcmp(nvGspBooterRecoveryVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(nvGspBooterRecoveryVerdictName(
                          NV_GSP_BOOTER_RECOVERY_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");
}

int main()
{
    const auto blob = load(NV_FIRMWARE_DIR "/booter_load-570.144.bin");
    if (blob.empty())
        return 1;

    const auto sequence = sequencePlan(blob);
    testEarlyResetFailures(sequence);
    testDmaTimeout(sequence);
    testBootHaltTimeout(sequence);
    testMailboxFailure(sequence);
    testRefusalsAndAtomicity(sequence);
    testVerdictNames();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
