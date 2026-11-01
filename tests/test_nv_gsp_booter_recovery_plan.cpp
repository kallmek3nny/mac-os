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

static void checkThat(bool cond, const char *what)
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
        checkThat(false, "fixture open");
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
    checkThat(nvGspPlanBooterHs(blob.data(), blob.size(), &hs) == NV_GSP_BOOTER_HS_OK,
          "fixture hs parse");
    std::vector<unsigned char> image(hs.firmwareBytes, 0x5au);
    NVGspBooterFusePatchPlan built = {};
    checkThat(nvGspBuildObservedBooterPrivateImage(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), &built) == NV_GSP_BOOTER_FUSE_PATCH_OK,
          "fixture private image");
    checkThat(built.valid, "fixture private image valid");
    return image;
}

static NVGspBooterExecSequencePlan sequencePlan(const std::vector<unsigned char> &blob)
{
    const auto observed = observation(0u);
    const auto image = privateImage(blob, observed);
    NVGspBooterExecSequencePlan out = {};
    checkThat(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), 0x12345678abcdef01ull, &out) ==
          NV_GSP_BOOTER_EXEC_OK,
          "fixture exec sequence accepted");
    checkThat(out.valid, "fixture exec sequence valid");
    checkThat(!out.runtimeWriteAuthorized, "fixture sequence remains no-write");
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
    checkThat(verdict == NV_GSP_BOOTER_RECOVERY_OK, "recovery plan accepted");
    checkThat(out.valid, "recovery plan valid");
    return out;
}

static bool sameBytes(const NVGspBooterRecoveryPlan &a,
                      const NVGspBooterRecoveryPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void commonNoWriteChecks(const NVGspBooterRecoveryPlan &p)
{
    checkThat(p.requiresFreshExactTargetOwnership, "fresh exact target ownership required");
    checkThat(p.requiresFreshSequenceRevalidation, "fresh sequence revalidation required");
    checkThat(!p.retryAuthorized, "retry never authorized by recovery planner");
    checkThat(!p.runtimeRecoveryAuthorized, "runtime recovery never authorized");
    checkThat(!p.runtimeWriteAuthorized, "runtime writes never authorized");
    checkThat(!p.resetAloneProvesRetirement, "reset alone never proves backing retirement");
    checkThat(!p.postFailureResetAllowsBackingRelease,
          "post-failure reset alone never grants backing release");
    checkThat(!p.mailboxReadAfterTimeoutProvesCompletion,
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
        checkThat(p.failureBeforeFirmwareDmaSubmission, "failure is before firmware dma");
        checkThat(!p.firmwareDmaWasSubmitted && !p.bootWasStarted,
              "no dma or boot submission happened");
        checkThat(!p.firmwareDmaCompletionUncertain && !p.bootCompletionUncertain,
              "no unsubmitted transfer can be completion-uncertain");
        checkThat(!p.firmwareStagingMayStillBeReferenced &&
              !p.wprMetaMayStillBeReferenced,
              "no backing reference was handed to sec2");
        checkThat(p.firmwareStagingReleaseAuthorized && p.wprMetaReleaseAuthorized,
              "unsubmitted backings may be released");
        checkThat(!p.quarantineFirmwareStaging && !p.quarantineWprMeta,
              "unsubmitted backings do not require quarantine");
        checkThat(p.quarantineSec2State, "failed reset/core state remains quarantined");
        commonNoWriteChecks(p);
    }
}

static void testDmaTimeout(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("2. fbdma timeout retains staging until the exact idle predicate is later proven");
    const auto uncertain = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT,
        false, false, 0u, false, true, "dma timeout uncertain");
    checkThat(uncertain.firmwareDmaWasSubmitted && !uncertain.bootWasStarted,
          "dma submitted before boot start");
    checkThat(uncertain.firmwareDmaCompletionUncertain,
          "dma completion remains uncertain without idle proof");
    checkThat(uncertain.firmwareStagingMayStillBeReferenced &&
          !uncertain.firmwareStagingReleaseAuthorized &&
          uncertain.quarantineFirmwareStaging,
          "uncertain dma source retained and quarantined");
    checkThat(!uncertain.wprMetaMayStillBeReferenced &&
          uncertain.wprMetaReleaseAuthorized && !uncertain.quarantineWprMeta,
          "wpr metadata not yet published at dma failure");
    checkThat(uncertain.laterDmaIdleCanRetireFirmwareStaging &&
          !uncertain.laterDmaIdleRetirementObserved,
          "later exact idle observation is the staging retirement path");
    commonNoWriteChecks(uncertain);

    const auto retired = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT,
        false, false, 0u, true, true, "dma timeout later idle");
    checkThat(!retired.firmwareDmaCompletionUncertain,
          "later idle resolves dma completion uncertainty");
    checkThat(!retired.firmwareStagingMayStillBeReferenced &&
          retired.firmwareStagingReleaseAuthorized &&
          !retired.quarantineFirmwareStaging,
          "later idle retires staging source");
    checkThat(retired.laterDmaIdleRetirementObserved,
          "later idle retirement observation recorded");
    checkThat(retired.quarantineSec2State,
          "dma retirement does not prove sec2 retry safety");
    commonNoWriteChecks(retired);
}

static void testBootHaltTimeout(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("3. halt timeout quarantines wpr metadata even if mailbox0 is later readable");
    const auto noMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, false, 0u, false, true, "halt timeout no mailbox");
    checkThat(noMailbox.firmwareDmaWasSubmitted && noMailbox.bootWasStarted,
          "boot timeout happens after dma and cpu start");
    checkThat(!noMailbox.firmwareDmaCompletionUncertain &&
          noMailbox.firmwareStagingReleaseAuthorized,
          "firmware dma source retired before boot stage");
    checkThat(noMailbox.bootCompletionUncertain &&
          noMailbox.wprMetaMayStillBeReferenced &&
          !noMailbox.wprMetaReleaseAuthorized && noMailbox.quarantineWprMeta,
          "uncertain boot retains wpr metadata");
    commonNoWriteChecks(noMailbox);

    const auto zeroMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, true, 0u, false, true, "halt timeout zero mailbox");
    checkThat(zeroMailbox.bootCompletionUncertain,
          "zero mailbox after timeout does not repair failed halt proof");
    checkThat(zeroMailbox.wprMetaMayStillBeReferenced && zeroMailbox.quarantineWprMeta,
          "zero mailbox after timeout still retains wpr metadata");
    checkThat(!zeroMailbox.mailboxReadAfterTimeoutProvesCompletion,
          "openrm mailbox read after timeout is not completion proof");
    commonNoWriteChecks(zeroMailbox);

    const auto nonzeroMailbox = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT,
        false, true, 7u, false, false, "halt timeout nonzero mailbox");
    checkThat(nonzeroMailbox.bootCompletionUncertain && nonzeroMailbox.quarantineWprMeta,
          "nonzero mailbox after halt timeout also remains uncertain");
    commonNoWriteChecks(nonzeroMailbox);
}

static void testMailboxFailure(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("4. proven halt plus nonzero mailbox is failure, not release permission");
    const auto p = recover(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE,
        true, true, 5u, false, true, "mailbox failure");
    checkThat(p.haltProven && p.mailboxFailureObserved,
          "halt and mailbox failure are both recorded");
    checkThat(!p.bootCompletionUncertain,
          "halt wait completion itself is proven");
    checkThat(p.firmwareStagingReleaseAuthorized &&
          !p.firmwareStagingMayStillBeReferenced,
          "firmware staging already retired by dma idle");
    checkThat(p.wprMetaMayStillBeReferenced && !p.wprMetaReleaseAuthorized &&
          p.quarantineWprMeta,
          "failed booter wpr metadata remains conservatively retained");
    checkThat(p.quarantineSec2State, "failed booter state remains quarantined");
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
    checkThat(nvGspPlanGa107BooterRecovery(&sequence, &resetReady, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NONFATAL_EVENT,
          "best-effort reset-ready timeout is not a recovery failure");
    checkThat(sameBytes(out, before), "nonfatal refusal leaves output untouched");

    NVGspBooterRecoveryObservation badMailbox = {};
    badMailbox.event = NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE;
    badMailbox.haltProven = true;
    badMailbox.mailbox0Observed = true;
    badMailbox.mailbox0Value = 0u;
    checkThat(nvGspPlanGa107BooterRecovery(&sequence, &badMailbox, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION,
          "zero mailbox cannot be classified as mailbox failure");
    checkThat(sameBytes(out, before), "mailbox contradiction leaves output untouched");

    NVGspBooterRecoveryObservation badEarly = {};
    badEarly.event = NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT;
    badEarly.laterDmaIdleObserved = true;
    checkThat(nvGspPlanGa107BooterRecovery(&sequence, &badEarly, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION,
          "pre-dma failure cannot claim later dma idle retirement");
    checkThat(sameBytes(out, before), "early contradiction leaves output untouched");

    auto writeCapable = sequence;
    writeCapable.runtimeWriteAuthorized = true;
    NVGspBooterRecoveryObservation dma = {};
    dma.event = NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT;
    checkThat(nvGspPlanGa107BooterRecovery(&writeCapable, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE,
          "write-authorized input sequence refused");
    checkThat(sameBytes(out, before), "bad sequence leaves output untouched");

    auto recovered = sequence;
    recovered.timeoutRecoveryProven = true;
    checkThat(nvGspPlanGa107BooterRecovery(&recovered, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE,
          "planner refuses invented pre-proven timeout recovery");
    checkThat(sameBytes(out, before), "invented recovery proof leaves output untouched");

    checkThat(nvGspPlanGa107BooterRecovery(nullptr, &dma, &out) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NULL_SEQUENCE,
          "null sequence refused");
    checkThat(nvGspPlanGa107BooterRecovery(&sequence, &dma, nullptr) ==
          NV_GSP_BOOTER_RECOVERY_ERR_NULL_OUTPUT,
          "null output refused");
}

static void testVerdictNames(void)
{
    std::puts("6. verdict names remain stable");
    for (NVGspBooterHsU32 v = 0u; v <= NV_GSP_BOOTER_RECOVERY_VERDICT_LIMIT; ++v) {
        checkThat(std::strcmp(nvGspBooterRecoveryVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    checkThat(std::strcmp(nvGspBooterRecoveryVerdictName(
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
