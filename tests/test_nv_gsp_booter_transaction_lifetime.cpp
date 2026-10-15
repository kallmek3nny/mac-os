// SPDX-License-Identifier: MIT
#include "NVGspBooterTransactionLifetime.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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

static NVGspBooterTransactionProgress progressThroughHalt()
{
    NVGspBooterTransactionProgress p = {};
    p.resetSequenceCompleted = true;
    p.firmwareDmaSubmitted = true;
    p.allFirmwareDmaIdleObserved = true;
    p.wprMetaAddressPublished = true;
    p.bootStarted = true;
    p.haltObservedWithinBudget = true;
    return p;
}

static NVGspBooterTransactionProgress progressForSuccess()
{
    auto p = progressThroughHalt();
    p.mailbox0Observed = true;
    p.mailbox1Observed = true;
    p.mailboxReadbackAfterHalt = true;
    p.mailbox0BeforeMailbox1Readback = true;
    return p;
}

static NVGspBooterSuccessRetirementPlan successPlan(
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

    NVGspBooterSuccessRetirementPlan out = {};
    check(nvGspPlanGa107BooterSuccessRetirement(&sequence, &observed, &out) ==
              NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK,
          "fixture success retirement accepted");
    check(out.valid, "fixture success retirement valid");
    return out;
}

static NVGspBooterRecoveryPlan recoveryPlan(
    const NVGspBooterExecSequencePlan &sequence,
    NVGspBooterHsU32 event,
    bool laterDmaIdle = false)
{
    NVGspBooterRecoveryObservation observed = {};
    observed.event = event;

    switch (event) {
    case NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT:
    case NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT:
        break;
    case NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT:
        observed.laterDmaIdleObserved = laterDmaIdle;
        break;
    case NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT:
        // Exercise the source-backed edge where OpenRM has read mailbox0 after
        // the halt wait failed. It must not become completion proof.
        observed.mailbox0Observed = true;
        observed.mailbox0Value = sequence.mailbox0SuccessValue;
        break;
    case NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE:
        observed.haltProven = true;
        observed.mailbox0Observed = true;
        observed.mailbox0Value = sequence.mailbox0SuccessValue + 1u;
        break;
    default:
        check(false, "unsupported recovery fixture event");
        break;
    }

    NVGspBooterRecoveryPlan out = {};
    check(nvGspPlanGa107BooterRecovery(&sequence, &observed, &out) ==
              NV_GSP_BOOTER_RECOVERY_OK,
          "fixture recovery plan accepted");
    check(out.valid, "fixture recovery plan valid");
    return out;
}

static bool sameBytes(const NVGspBooterTransactionLifetimePlan &a,
                      const NVGspBooterTransactionLifetimePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void checkPure(const NVGspBooterTransactionLifetimePlan &plan)
{
    check(!plan.retryAuthorized, "transaction never authorizes retry");
    check(!plan.runtimeRecoveryAuthorized,
          "transaction never authorizes runtime recovery");
    check(!plan.runtimeWriteAuthorized,
          "transaction never authorizes runtime writes");
    check(!plan.resetAloneProvesRetirement,
          "reset alone never proves retirement");
    check(!plan.mailboxReadAfterTimeoutProvesCompletion,
          "mailbox read after timeout never proves completion");
}

static NVGspBooterTransactionLifetimePlan planNonterminal(
    const NVGspBooterExecSequencePlan &sequence,
    const NVGspBooterTransactionProgress &progress,
    const char *what)
{
    NVGspBooterTransactionLifetimePlan out = {};
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &progress, nullptr, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          what);
    check(out.valid, "nonterminal transaction plan valid");
    check(!out.terminalOutcomePresent && !out.terminalSuccess && !out.terminalFailure,
          "nonterminal phase carries no invented terminal result");
    checkPure(out);
    return out;
}

static void testNonterminalPhases(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("1. monotonic nonterminal phases preserve backing lifetimes");

    NVGspBooterTransactionProgress p = {};
    auto out = planNonterminal(sequence, p, "pre-execution accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION,
          "pre-execution classified");
    check(out.firmwareStagingReleaseAuthorized && out.wprMetaReleaseAuthorized,
          "nothing submitted/published is hardware-owned");
    check(!out.firmwareStagingMustRemainPinned && !out.wprMetaMustRemainPinned,
          "pre-exec temporary backing has no hardware retention");

    p.resetSequenceCompleted = true;
    out = planNonterminal(sequence, p, "reset-ready accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_RESET_READY,
          "reset-ready classified");
    check(out.firmwareStagingReleaseAuthorized && out.wprMetaReleaseAuthorized,
          "reset alone acquires no temporary backing reference");

    p.firmwareDmaSubmitted = true;
    out = planNonterminal(sequence, p, "dma in-flight accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_IN_FLIGHT,
          "dma in-flight classified");
    check(out.firmwareStagingMayStillBeReferenced &&
              out.firmwareStagingMustRemainPinned &&
              !out.firmwareStagingReleaseAuthorized,
          "in-flight dma pins firmware source");
    check(!out.wprMetaMayStillBeReferenced && out.wprMetaReleaseAuthorized,
          "wpr metadata is not hardware-owned before publication");

    p.allFirmwareDmaIdleObserved = true;
    out = planNonterminal(sequence, p, "dma retired accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_RETIRED,
          "dma-retired classified");
    check(out.firmwareDmaRetired && !out.firmwareStagingMayStillBeReferenced &&
              out.firmwareStagingReleaseAuthorized,
          "per-transfer idle retires firmware staging");

    p.wprMetaAddressPublished = true;
    out = planNonterminal(sequence, p, "wpr publication accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_WPR_META_PUBLISHED,
          "wpr-published classified");
    check(out.wprMetaMayStillBeReferenced && out.wprMetaMustRemainPinned &&
              !out.wprMetaReleaseAuthorized,
          "published wpr metadata becomes retained");

    p.bootStarted = true;
    out = planNonterminal(sequence, p, "boot-running accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_BOOT_RUNNING,
          "boot-running classified");
    check(out.wprMetaMustRemainPinned && !out.wprMetaReleaseAuthorized,
          "started booter keeps published wpr metadata pinned");

    p.haltObservedWithinBudget = true;
    out = planNonterminal(sequence, p, "halt-observed accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_HALT_OBSERVED,
          "halt-observed classified");
    check(out.wprMetaMustRemainPinned && !out.wprMetaReleaseAuthorized,
          "halt without terminal mailbox interpretation does not release wpr metadata");
}

static void testSuccess(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("2. exact success retirement releases both temporary backings");
    const auto p = progressForSuccess();
    const auto success = successPlan(sequence);
    NVGspBooterTransactionLifetimePlan out = {};
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, nullptr, &success, &out) == NV_GSP_BOOTER_TXN_OK,
          "terminal success transaction accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_SUCCESS_RETIRED,
          "success-retired phase classified");
    check(out.terminalOutcomePresent && out.terminalSuccess && !out.terminalFailure,
          "terminal success recorded exclusively");
    check(!out.completionUncertain && out.haltProven && out.mailboxPairObserved,
          "bounded halt and mailbox pair proof preserved");
    check(!out.firmwareStagingMayStillBeReferenced &&
              !out.wprMetaMayStillBeReferenced &&
              out.firmwareStagingReleaseAuthorized &&
              out.wprMetaReleaseAuthorized,
          "successful retirement releases temporary staging and wpr metadata");
    check(out.recoveryGuardMayBeDismissed,
          "only exact success can dismiss recovery guard");
    check(!out.quarantineFirmwareStaging && !out.quarantineWprMeta &&
              !out.quarantineSec2State,
          "clean success carries no failure quarantine");
    checkPure(out);
}

static void testPreDmaFailure(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("3. pre-dma reset/core failure releases unsubmitted backing but quarantines sec2");
    const auto recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT);
    NVGspBooterTransactionProgress p = {};
    NVGspBooterTransactionLifetimePlan out = {};
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, &recovery, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          "pre-dma failure accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FAILURE_PRE_DMA,
          "pre-dma failure phase classified");
    check(out.terminalFailure && !out.terminalSuccess,
          "failure recorded exclusively");
    check(out.firmwareStagingReleaseAuthorized && out.wprMetaReleaseAuthorized,
          "never-submitted backing may be released");
    check(!out.quarantineFirmwareStaging && !out.quarantineWprMeta &&
              out.quarantineSec2State,
          "only sec2 state is quarantined before dma submission");
    check(out.requiresFreshExactTargetOwnership &&
              out.requiresFreshSequenceRevalidation,
          "future attempt requires fresh ownership and sequence proof");
    checkPure(out);
}

static void testDmaFailure(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("4. dma timeout keeps source pinned until the exact idle predicate is later seen");
    auto recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT, false);
    NVGspBooterTransactionProgress p = {};
    p.resetSequenceCompleted = true;
    p.firmwareDmaSubmitted = true;
    NVGspBooterTransactionLifetimePlan out = {};
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, &recovery, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          "uncertain dma timeout accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_QUARANTINE,
          "dma quarantine phase classified");
    check(out.completionUncertain && out.firmwareStagingMustRemainPinned &&
              !out.firmwareStagingReleaseAuthorized &&
              out.quarantineFirmwareStaging,
          "uncertain dma source remains pinned/quarantined");
    check(out.wprMetaReleaseAuthorized && !out.quarantineWprMeta,
          "unpublished wpr metadata is not captured by dma failure");
    check(out.quarantineSec2State, "dma failure quarantines sec2 state");
    checkPure(out);

    recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT, true);
    p.allFirmwareDmaIdleObserved = true;
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, &recovery, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          "later-idle dma timeout accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_RETIRED,
          "later idle moves failed dma to retired phase");
    check(!out.completionUncertain && !out.firmwareStagingMustRemainPinned &&
              out.firmwareStagingReleaseAuthorized &&
              !out.quarantineFirmwareStaging,
          "later exact idle retires firmware staging only");
    check(out.quarantineSec2State,
          "later dma idle does not magically recover sec2 state");
    checkPure(out);
}

static void testBootFailures(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("5. boot timeout/mailbox failure retain wpr metadata regardless of mailbox appearance");
    auto recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT);
    NVGspBooterTransactionProgress p = {};
    p.resetSequenceCompleted = true;
    p.firmwareDmaSubmitted = true;
    p.allFirmwareDmaIdleObserved = true;
    p.wprMetaAddressPublished = true;
    p.bootStarted = true;
    // Model OpenRM reading mailbox0 after the failed halt wait. No halt proof.
    p.mailbox0Observed = true;
    NVGspBooterTransactionLifetimePlan out = {};
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, &recovery, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          "halt-timeout transaction accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE,
          "halt timeout enters boot quarantine");
    check(out.completionUncertain && out.firmwareStagingReleaseAuthorized,
          "firmware staging already retired before boot timeout");
    check(out.wprMetaMayStillBeReferenced && out.wprMetaMustRemainPinned &&
              !out.wprMetaReleaseAuthorized && out.quarantineWprMeta,
          "published wpr metadata remains pinned after halt timeout");
    check(!out.mailboxReadAfterTimeoutProvesCompletion,
          "zero-looking mailbox after timeout remains non-authoritative");
    check(out.quarantineSec2State, "boot timeout quarantines sec2 state");
    checkPure(out);

    recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE);
    p.haltObservedWithinBudget = true;
    p.mailboxReadbackAfterHalt = true;
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, &recovery, nullptr, &out) == NV_GSP_BOOTER_TXN_OK,
          "mailbox0 failure transaction accepted");
    check(out.phase == NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE,
          "mailbox failure stays in boot quarantine");
    check(!out.completionUncertain && out.haltProven,
          "halt completion is preserved without claiming booter success");
    check(out.wprMetaMustRemainPinned && !out.wprMetaReleaseAuthorized &&
              out.quarantineWprMeta,
          "failed booter retains wpr metadata despite proven halt");
    checkPure(out);
}

static void expectRefusal(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterTransactionProgress *progress,
    const NVGspBooterRecoveryPlan *recovery,
    const NVGspBooterSuccessRetirementPlan *success,
    NVGspBooterHsU32 expected,
    const char *what)
{
    NVGspBooterTransactionLifetimePlan out = {};
    std::memset(&out, 0xA5, sizeof(out));
    const auto before = out;
    const auto verdict = nvGspPlanGa107BooterTransactionLifetime(
        sequence, progress, recovery, success, &out);
    check(verdict == expected, what);
    check(sameBytes(out, before), "refusal leaves output untouched");
}

static void testContradictions(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("6. impossible progress and forged terminal proof refuse atomically");
    NVGspBooterTransactionProgress p = {};

    p.firmwareDmaSubmitted = true;
    expectRefusal(&sequence, &p, nullptr, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_PROGRESS,
                  "dma before reset completion refused");

    p = {};
    p.allFirmwareDmaIdleObserved = true;
    expectRefusal(&sequence, &p, nullptr, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_PROGRESS,
                  "dma idle without submission refused");

    p = {};
    p.resetSequenceCompleted = true;
    p.firmwareDmaSubmitted = true;
    p.wprMetaAddressPublished = true;
    expectRefusal(&sequence, &p, nullptr, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_PROGRESS,
                  "wpr publication before all dma idle refused");

    p = progressForSuccess();
    const auto success = successPlan(sequence);
    const auto recovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE);
    expectRefusal(&sequence, &p, &recovery, &success,
                  NV_GSP_BOOTER_TXN_ERR_TERMINAL_CONFLICT,
                  "success and failure cannot both terminate one transaction");

    auto badSuccess = success;
    badSuccess.runtimeWriteAuthorized = true;
    expectRefusal(&sequence, &p, nullptr, &badSuccess,
                  NV_GSP_BOOTER_TXN_ERR_SUCCESS,
                  "write-authorized forged success refused");

    badSuccess = success;
    badSuccess.wprMetaReleaseAuthorized = false;
    expectRefusal(&sequence, &p, nullptr, &badSuccess,
                  NV_GSP_BOOTER_TXN_ERR_SUCCESS,
                  "weakened success retirement refused");

    p.mailbox1Observed = false;
    p.mailbox0BeforeMailbox1Readback = false;
    expectRefusal(&sequence, &p, nullptr, &success,
                  NV_GSP_BOOTER_TXN_ERR_SUCCESS_PROGRESS,
                  "success proof cannot attach to incomplete mailbox progress");

    auto dmaRecovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT, false);
    p = {};
    p.resetSequenceCompleted = true;
    p.firmwareDmaSubmitted = true;
    p.allFirmwareDmaIdleObserved = true;
    expectRefusal(&sequence, &p, &dmaRecovery, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_RECOVERY_PROGRESS,
                  "unretired dma recovery cannot attach to retired progress");

    dmaRecovery = recoveryPlan(
        sequence, NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT, true);
    dmaRecovery.firmwareStagingReleaseAuthorized = false;
    expectRefusal(&sequence, &p, &dmaRecovery, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_RECOVERY,
                  "contradictory recovery lifetime refused");

    auto badSequence = sequence;
    badSequence.runtimeWriteAuthorized = true;
    expectRefusal(&badSequence, &p, nullptr, nullptr,
                  NV_GSP_BOOTER_TXN_ERR_SEQUENCE,
                  "write-authorized execution sequence refused");
}

static void testNullsAndNames(const NVGspBooterExecSequencePlan &sequence)
{
    std::puts("7. nulls, verdict names and phase names remain stable");
    NVGspBooterTransactionProgress p = {};
    NVGspBooterTransactionLifetimePlan out = {};

    check(nvGspPlanGa107BooterTransactionLifetime(
              nullptr, &p, nullptr, nullptr, &out) ==
              NV_GSP_BOOTER_TXN_ERR_NULL_SEQUENCE,
          "null sequence refused");
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, nullptr, nullptr, nullptr, &out) ==
              NV_GSP_BOOTER_TXN_ERR_NULL_PROGRESS,
          "null progress refused");
    check(nvGspPlanGa107BooterTransactionLifetime(
              &sequence, &p, nullptr, nullptr, nullptr) ==
              NV_GSP_BOOTER_TXN_ERR_NULL_OUTPUT,
          "null output refused");

    for (NVGspBooterHsU32 v = 0u; v <= NV_GSP_BOOTER_TXN_VERDICT_LIMIT; ++v) {
        check(std::strcmp(nvGspBooterTransactionLifetimeVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(nvGspBooterTransactionLifetimeVerdictName(
                          NV_GSP_BOOTER_TXN_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");

    for (NVGspBooterHsU32 phase = NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION;
         phase <= NV_GSP_BOOTER_TXN_PHASE_LIMIT; ++phase) {
        check(std::strcmp(nvGspBooterTransactionLifetimePhaseName(phase), "unknown") != 0,
              "in-range phase named");
    }
    check(std::strcmp(nvGspBooterTransactionLifetimePhaseName(0u), "unknown") == 0,
          "out-of-range phase unknown");
}

int main()
{
    const auto blob = load("reference/ga107-firmware/booter_load-570.144.bin");
    if (blob.empty())
        return 1;

    const auto sequence = sequencePlan(blob);
    testNonterminalPhases(sequence);
    testSuccess(sequence);
    testPreDmaFailure(sequence);
    testDmaFailure(sequence);
    testBootFailures(sequence);
    testContradictions(sequence);
    testNullsAndNames(sequence);

    if (gFailures != 0u) {
        std::printf("%u/%u GA107 SEC2 transaction lifetime checks failed\n",
                    gFailures, gChecks);
        return 1;
    }

    std::printf("all %u GA107 SEC2 transaction lifetime checks passed\n", gChecks);
    return 0;
}
