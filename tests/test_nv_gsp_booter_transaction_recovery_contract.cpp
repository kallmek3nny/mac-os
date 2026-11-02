// SPDX-License-Identifier: MIT
#include "NVGspBooterTransactionLifetime.hpp"

#include <cstdio>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void checkThat(bool condition, const char *what)
{
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

static void checkValid(const NVGspBooterRecoveryPlan &plan, const char *what)
{
    checkThat(nvGspBooterTransactionRecoveryEventSemanticsValid(&plan), what);
}

static void checkInvalid(const NVGspBooterRecoveryPlan &plan, const char *what)
{
    checkThat(!nvGspBooterTransactionRecoveryEventSemanticsValid(&plan), what);
}

static NVGspBooterRecoveryPlan preDmaPlan(NVGspBooterHsU32 event)
{
    NVGspBooterRecoveryPlan plan = {};
    plan.event = event;
    plan.failureBeforeFirmwareDmaSubmission = true;
    return plan;
}

static NVGspBooterRecoveryPlan dmaTimeoutPlan(bool laterIdle)
{
    NVGspBooterRecoveryPlan plan = {};
    plan.event = NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT;
    plan.firmwareDmaWasSubmitted = true;
    plan.firmwareDmaCompletionUncertain = !laterIdle;
    plan.firmwareStagingMayStillBeReferenced = !laterIdle;
    plan.laterDmaIdleCanRetireFirmwareStaging = true;
    plan.laterDmaIdleRetirementObserved = laterIdle;
    return plan;
}

static NVGspBooterRecoveryPlan haltTimeoutPlan()
{
    NVGspBooterRecoveryPlan plan = {};
    plan.event = NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT;
    plan.firmwareDmaWasSubmitted = true;
    plan.bootWasStarted = true;
    plan.bootCompletionUncertain = true;
    plan.wprMetaMayStillBeReferenced = true;
    return plan;
}

static NVGspBooterRecoveryPlan mailboxFailurePlan()
{
    NVGspBooterRecoveryPlan plan = {};
    plan.event = NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE;
    plan.firmwareDmaWasSubmitted = true;
    plan.bootWasStarted = true;
    plan.haltProven = true;
    plan.mailboxFailureObserved = true;
    plan.wprMetaMayStillBeReferenced = true;
    return plan;
}

static void testPreDmaSemantics()
{
    auto plan = preDmaPlan(NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT);
    checkValid(plan, "core-select timeout canonical semantics accepted");

    plan.postFailureResetObserved = true;
    checkValid(plan, "post-failure reset observation does not rewrite provenance");

    auto forged = plan;
    forged.failureBeforeFirmwareDmaSubmission = false;
    checkInvalid(forged, "pre-dma event cannot deny pre-dma provenance");

    forged = plan;
    forged.firmwareDmaWasSubmitted = true;
    checkInvalid(forged, "pre-dma event cannot claim dma submission");

    forged = plan;
    forged.bootWasStarted = true;
    checkInvalid(forged, "pre-dma event cannot claim booter start");

    forged = plan;
    forged.firmwareDmaCompletionUncertain = true;
    checkInvalid(forged, "pre-dma event cannot invent dma uncertainty");

    forged = plan;
    forged.laterDmaIdleCanRetireFirmwareStaging = true;
    checkInvalid(forged, "pre-dma event cannot invent later-idle retirement path");

    plan = preDmaPlan(NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT);
    checkValid(plan, "memory-scrub timeout canonical semantics accepted");
}

static void testDmaTimeoutSemantics()
{
    auto plan = dmaTimeoutPlan(false);
    checkValid(plan, "dma timeout without later idle accepted");

    auto forged = plan;
    forged.failureBeforeFirmwareDmaSubmission = true;
    checkInvalid(forged, "dma timeout cannot claim failure before submission");

    forged = plan;
    forged.firmwareDmaWasSubmitted = false;
    checkInvalid(forged, "dma timeout requires submitted dma");

    forged = plan;
    forged.firmwareDmaCompletionUncertain = false;
    checkInvalid(forged, "dma timeout without later idle stays uncertain");

    forged = plan;
    forged.firmwareStagingMayStillBeReferenced = false;
    checkInvalid(forged, "dma timeout without later idle keeps staging referenced");

    forged = plan;
    forged.laterDmaIdleCanRetireFirmwareStaging = false;
    checkInvalid(forged, "dma timeout must preserve exact later-idle retirement path");

    plan = dmaTimeoutPlan(true);
    checkValid(plan, "dma timeout with later exact idle accepted");

    forged = plan;
    forged.firmwareDmaCompletionUncertain = true;
    checkInvalid(forged, "later exact idle clears dma uncertainty");

    forged = plan;
    forged.firmwareStagingMayStillBeReferenced = true;
    checkInvalid(forged, "later exact idle retires staging reference");

    forged = plan;
    forged.bootWasStarted = true;
    checkInvalid(forged, "dma timeout cannot claim booter start");
}

static void testBootTimeoutSemantics()
{
    auto plan = haltTimeoutPlan();
    checkValid(plan, "halt timeout canonical semantics accepted");

    plan.postFailureResetObserved = true;
    checkValid(plan, "halt-timeout reset observation does not become retirement proof");

    auto forged = plan;
    forged.bootCompletionUncertain = false;
    checkInvalid(forged, "halt timeout must preserve boot uncertainty");

    forged = plan;
    forged.haltProven = true;
    checkInvalid(forged, "halt timeout cannot claim halt proof");

    forged = plan;
    forged.mailboxFailureObserved = true;
    checkInvalid(forged, "halt timeout cannot masquerade as mailbox failure");

    forged = plan;
    forged.wprMetaMayStillBeReferenced = false;
    checkInvalid(forged, "halt timeout keeps published wpr metadata referenced");

    forged = plan;
    forged.laterDmaIdleRetirementObserved = true;
    checkInvalid(forged, "halt timeout cannot import dma-timeout retirement evidence");
}

static void testMailboxFailureSemantics()
{
    const auto plan = mailboxFailurePlan();
    checkValid(plan, "mailbox failure canonical semantics accepted");

    auto forged = plan;
    forged.haltProven = false;
    checkInvalid(forged, "mailbox failure requires prior halt proof");

    forged = plan;
    forged.mailboxFailureObserved = false;
    checkInvalid(forged, "mailbox failure requires failure observation");

    forged = plan;
    forged.bootCompletionUncertain = true;
    checkInvalid(forged, "mailbox failure cannot retain halt-timeout uncertainty");

    forged = plan;
    forged.wprMetaMayStillBeReferenced = false;
    checkInvalid(forged, "failed booter keeps wpr metadata referenced");

    forged = plan;
    forged.laterDmaIdleCanRetireFirmwareStaging = true;
    checkInvalid(forged, "mailbox failure cannot import dma-timeout policy");
}

static void testUnsupportedEvents()
{
    NVGspBooterRecoveryPlan plan = {};
    checkInvalid(plan, "zero event refused");

    plan.event = NV_GSP_BOOTER_RECOVERY_EVENT_RESET_READY_TIMEOUT;
    checkInvalid(plan, "nonfatal reset-ready timeout cannot terminate transaction");

    plan.event = NV_GSP_BOOTER_RECOVERY_EVENT_LIMIT + 1u;
    checkInvalid(plan, "out-of-range recovery event refused");

    checkThat(!nvGspBooterTransactionRecoveryEventSemanticsValid(nullptr),
          "null recovery plan refused");
}

int main()
{
    testPreDmaSemantics();
    testDmaTimeoutSemantics();
    testBootTimeoutSemantics();
    testMailboxFailureSemantics();
    testUnsupportedEvents();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
