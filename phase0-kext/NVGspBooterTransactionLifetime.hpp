// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter transaction lifetime state machine.
//
// Composes the already-validated execution sequence, failure recovery policy,
// and success retirement proof. No GPU I/O, reset, retry, recovery, register
// writes, raw GPU-address ABI, or command-stream ABI is authorized here.
#ifndef NV_GSP_BOOTER_TRANSACTION_LIFETIME_HPP
#define NV_GSP_BOOTER_TRANSACTION_LIFETIME_HPP

#include "NVGspBooterRecoveryPlan.hpp"
#include "NVGspBooterSuccessRetirementPlan.hpp"

#define NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION            1u
#define NV_GSP_BOOTER_TXN_PHASE_RESET_READY              2u
#define NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_IN_FLIGHT   3u
#define NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_RETIRED     4u
#define NV_GSP_BOOTER_TXN_PHASE_WPR_META_PUBLISHED       5u
#define NV_GSP_BOOTER_TXN_PHASE_BOOT_RUNNING             6u
#define NV_GSP_BOOTER_TXN_PHASE_HALT_OBSERVED            7u
#define NV_GSP_BOOTER_TXN_PHASE_SUCCESS_RETIRED          8u
#define NV_GSP_BOOTER_TXN_PHASE_FAILURE_PRE_DMA          9u
#define NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_QUARANTINE  10u
#define NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_RETIRED     11u
#define NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE 12u
#define NV_GSP_BOOTER_TXN_PHASE_LIMIT                   12u

#define NV_GSP_BOOTER_TXN_OK                             0u
#define NV_GSP_BOOTER_TXN_ERR_NULL_OUTPUT                1u
#define NV_GSP_BOOTER_TXN_ERR_NULL_SEQUENCE              2u
#define NV_GSP_BOOTER_TXN_ERR_NULL_PROGRESS              3u
#define NV_GSP_BOOTER_TXN_ERR_SEQUENCE                   4u
#define NV_GSP_BOOTER_TXN_ERR_PROGRESS                   5u
#define NV_GSP_BOOTER_TXN_ERR_TERMINAL_CONFLICT          6u
#define NV_GSP_BOOTER_TXN_ERR_RECOVERY                   7u
#define NV_GSP_BOOTER_TXN_ERR_RECOVERY_PROGRESS          8u
#define NV_GSP_BOOTER_TXN_ERR_SUCCESS                    9u
#define NV_GSP_BOOTER_TXN_ERR_SUCCESS_PROGRESS          10u
#define NV_GSP_BOOTER_TXN_VERDICT_LIMIT                 10u

struct NVGspBooterTransactionProgress {
    bool resetSequenceCompleted;
    bool firmwareDmaSubmitted;
    bool allFirmwareDmaIdleObserved;
    bool wprMetaAddressPublished;
    bool bootStarted;
    bool haltObservedWithinBudget;
    bool mailbox0Observed;
    bool mailbox1Observed;
    bool mailboxReadbackAfterHalt;
    bool mailbox0BeforeMailbox1Readback;
};

struct NVGspBooterTransactionLifetimePlan {
    NVGspBooterHsU32 phase;
    NVGspBooterHsU32 failureEvent;

    bool resetSequenceCompleted;
    bool firmwareDmaSubmitted;
    bool firmwareDmaRetired;
    bool wprMetaAddressPublished;
    bool bootStarted;
    bool haltProven;
    bool mailbox0Observed;
    bool mailboxPairObserved;

    bool terminalOutcomePresent;
    bool terminalSuccess;
    bool terminalFailure;
    bool completionUncertain;

    bool firmwareStagingMayStillBeReferenced;
    bool wprMetaMayStillBeReferenced;
    bool firmwareStagingMustRemainPinned;
    bool wprMetaMustRemainPinned;
    bool firmwareStagingReleaseAuthorized;
    bool wprMetaReleaseAuthorized;

    bool quarantineFirmwareStaging;
    bool quarantineWprMeta;
    bool quarantineSec2State;

    bool mailboxReadAfterTimeoutProvesCompletion;
    bool resetAloneProvesRetirement;
    bool recoveryGuardMayBeDismissed;
    bool requiresFreshExactTargetOwnership;
    bool requiresFreshSequenceRevalidation;

    bool retryAuthorized;
    bool runtimeRecoveryAuthorized;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvGspBooterTransactionLifetimeSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_TXN_OK;
}

static inline const char *nvGspBooterTransactionLifetimeVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
    case NV_GSP_BOOTER_TXN_OK:                    return "ok";
    case NV_GSP_BOOTER_TXN_ERR_NULL_OUTPUT:       return "null-output";
    case NV_GSP_BOOTER_TXN_ERR_NULL_SEQUENCE:     return "null-sequence";
    case NV_GSP_BOOTER_TXN_ERR_NULL_PROGRESS:     return "null-progress";
    case NV_GSP_BOOTER_TXN_ERR_SEQUENCE:          return "sequence";
    case NV_GSP_BOOTER_TXN_ERR_PROGRESS:          return "progress";
    case NV_GSP_BOOTER_TXN_ERR_TERMINAL_CONFLICT: return "terminal-conflict";
    case NV_GSP_BOOTER_TXN_ERR_RECOVERY:          return "recovery";
    case NV_GSP_BOOTER_TXN_ERR_RECOVERY_PROGRESS: return "recovery-progress";
    case NV_GSP_BOOTER_TXN_ERR_SUCCESS:           return "success";
    case NV_GSP_BOOTER_TXN_ERR_SUCCESS_PROGRESS:  return "success-progress";
    default:                                       return "unknown";
    }
}

static inline const char *nvGspBooterTransactionLifetimePhaseName(NVGspBooterHsU32 phase)
{
    switch (phase) {
    case NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION:           return "pre-execution";
    case NV_GSP_BOOTER_TXN_PHASE_RESET_READY:             return "reset-ready";
    case NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_IN_FLIGHT:  return "firmware-dma-in-flight";
    case NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_RETIRED:    return "firmware-dma-retired";
    case NV_GSP_BOOTER_TXN_PHASE_WPR_META_PUBLISHED:      return "wpr-meta-published";
    case NV_GSP_BOOTER_TXN_PHASE_BOOT_RUNNING:            return "boot-running";
    case NV_GSP_BOOTER_TXN_PHASE_HALT_OBSERVED:           return "halt-observed";
    case NV_GSP_BOOTER_TXN_PHASE_SUCCESS_RETIRED:         return "success-retired";
    case NV_GSP_BOOTER_TXN_PHASE_FAILURE_PRE_DMA:         return "failure-pre-dma";
    case NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_QUARANTINE:  return "failure-dma-quarantine";
    case NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_RETIRED:     return "failure-dma-retired";
    case NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE: return "failure-boot-quarantine";
    default:                                               return "unknown";
    }
}

static inline bool nvGspBooterTransactionProgressValid(
    const NVGspBooterTransactionProgress *progress)
{
    if (!progress)
        return false;
    if (progress->firmwareDmaSubmitted && !progress->resetSequenceCompleted)
        return false;
    if (progress->allFirmwareDmaIdleObserved && !progress->firmwareDmaSubmitted)
        return false;
    if (progress->wprMetaAddressPublished && !progress->allFirmwareDmaIdleObserved)
        return false;
    if (progress->bootStarted && !progress->wprMetaAddressPublished)
        return false;
    if (progress->haltObservedWithinBudget && !progress->bootStarted)
        return false;
    if ((progress->mailbox0Observed || progress->mailbox1Observed) &&
        !progress->bootStarted)
        return false;
    if (progress->mailbox1Observed && !progress->mailbox0Observed)
        return false;
    if (progress->mailboxReadbackAfterHalt &&
        (!progress->haltObservedWithinBudget || !progress->mailbox0Observed))
        return false;
    if (progress->mailbox0BeforeMailbox1Readback &&
        (!progress->mailbox0Observed || !progress->mailbox1Observed))
        return false;
    return true;
}

static inline bool nvGspBooterTransactionRecoveryContractValid(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterRecoveryPlan *recovery)
{
    if (!sequence || !recovery || !recovery->valid ||
        recovery->runtimeWriteAuthorized || recovery->runtimeRecoveryAuthorized ||
        recovery->retryAuthorized || !nvGspBooterRecoverySequenceValid(sequence))
        return false;

    if (recovery->event < NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT ||
        recovery->event > NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE)
        return false;

    if (recovery->resetAloneProvesRetirement ||
        recovery->postFailureResetAllowsBackingRelease ||
        recovery->mailboxReadAfterTimeoutProvesCompletion ||
        !recovery->requiresFreshExactTargetOwnership ||
        !recovery->requiresFreshSequenceRevalidation ||
        !recovery->quarantineSec2State)
        return false;

    // A hand-forged valid bit must not let contradictory lifetime semantics
    // cross this composition boundary.
    if (recovery->firmwareStagingMayStillBeReferenced ==
            recovery->firmwareStagingReleaseAuthorized ||
        recovery->wprMetaMayStillBeReferenced == recovery->wprMetaReleaseAuthorized)
        return false;
    if (recovery->quarantineFirmwareStaging !=
            recovery->firmwareStagingMayStillBeReferenced ||
        recovery->quarantineWprMeta != recovery->wprMetaMayStillBeReferenced)
        return false;

    return true;
}

static inline bool nvGspBooterTransactionSuccessContractValid(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterSuccessRetirementPlan *success)
{
    if (!sequence || !success || !success->valid ||
        success->runtimeWriteAuthorized || success->runtimeRecoveryAuthorized ||
        success->retryAuthorized || !nvGspBooterSuccessSequenceValid(sequence))
        return false;

    return success->firmwareDmaCompletionProven &&
           !success->firmwareStagingMayStillBeReferenced &&
           success->firmwareStagingReleaseAuthorized &&
           success->wprMetaWasPublished && success->bootWasStarted &&
           success->haltProven && success->mailbox0SuccessProven &&
           success->mailboxPairReadbackProven &&
           !success->wprMetaMayStillBeReferenced &&
           success->wprMetaReleaseAuthorized &&
           success->recoveryGuardMayBeDismissed &&
           !success->postSuccessResetRequiredForBackingRelease &&
           !success->failureRecoveryPlannerRequired;
}

static inline bool nvGspBooterTransactionRecoveryMatchesProgress(
    const NVGspBooterTransactionProgress *progress,
    const NVGspBooterRecoveryPlan *recovery)
{
    if (!progress || !recovery)
        return false;

    switch (recovery->event) {
    case NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT:
    case NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT:
        return !progress->resetSequenceCompleted &&
               !progress->firmwareDmaSubmitted &&
               !progress->allFirmwareDmaIdleObserved &&
               !progress->wprMetaAddressPublished &&
               !progress->bootStarted &&
               !progress->haltObservedWithinBudget &&
               !progress->mailbox0Observed && !progress->mailbox1Observed;

    case NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT:
        return progress->resetSequenceCompleted && progress->firmwareDmaSubmitted &&
               (progress->allFirmwareDmaIdleObserved ==
                recovery->laterDmaIdleRetirementObserved) &&
               !progress->wprMetaAddressPublished && !progress->bootStarted &&
               !progress->haltObservedWithinBudget &&
               !progress->mailbox0Observed && !progress->mailbox1Observed;

    case NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT:
        // OpenRM may read mailboxes after the failed halt wait. Such reads are
        // permitted as observations but remain non-authoritative for retirement.
        return progress->resetSequenceCompleted && progress->firmwareDmaSubmitted &&
               progress->allFirmwareDmaIdleObserved &&
               progress->wprMetaAddressPublished && progress->bootStarted &&
               !progress->haltObservedWithinBudget;

    case NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE:
        return progress->resetSequenceCompleted && progress->firmwareDmaSubmitted &&
               progress->allFirmwareDmaIdleObserved &&
               progress->wprMetaAddressPublished && progress->bootStarted &&
               progress->haltObservedWithinBudget && progress->mailbox0Observed;

    default:
        return false;
    }
}

static inline bool nvGspBooterTransactionSuccessMatchesProgress(
    const NVGspBooterTransactionProgress *progress)
{
    return progress && progress->resetSequenceCompleted &&
           progress->firmwareDmaSubmitted && progress->allFirmwareDmaIdleObserved &&
           progress->wprMetaAddressPublished && progress->bootStarted &&
           progress->haltObservedWithinBudget && progress->mailbox0Observed &&
           progress->mailbox1Observed && progress->mailboxReadbackAfterHalt &&
           progress->mailbox0BeforeMailbox1Readback;
}

// Atomic refusal: *out is assigned only after sequence, monotonic progress,
// and at most one terminal proof are mutually consistent.
static inline NVGspBooterHsU32 nvGspPlanGa107BooterTransactionLifetime(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterTransactionProgress *progress,
    const NVGspBooterRecoveryPlan *recovery,
    const NVGspBooterSuccessRetirementPlan *success,
    NVGspBooterTransactionLifetimePlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_TXN_ERR_NULL_OUTPUT;
    if (!sequence)
        return NV_GSP_BOOTER_TXN_ERR_NULL_SEQUENCE;
    if (!progress)
        return NV_GSP_BOOTER_TXN_ERR_NULL_PROGRESS;
    if (!nvGspBooterRecoverySequenceValid(sequence) ||
        !nvGspBooterSuccessSequenceValid(sequence))
        return NV_GSP_BOOTER_TXN_ERR_SEQUENCE;
    if (!nvGspBooterTransactionProgressValid(progress))
        return NV_GSP_BOOTER_TXN_ERR_PROGRESS;
    if (recovery && success)
        return NV_GSP_BOOTER_TXN_ERR_TERMINAL_CONFLICT;

    if (recovery) {
        if (!nvGspBooterTransactionRecoveryContractValid(sequence, recovery))
            return NV_GSP_BOOTER_TXN_ERR_RECOVERY;
        if (!nvGspBooterTransactionRecoveryMatchesProgress(progress, recovery))
            return NV_GSP_BOOTER_TXN_ERR_RECOVERY_PROGRESS;
    }
    if (success) {
        if (!nvGspBooterTransactionSuccessContractValid(sequence, success))
            return NV_GSP_BOOTER_TXN_ERR_SUCCESS;
        if (!nvGspBooterTransactionSuccessMatchesProgress(progress))
            return NV_GSP_BOOTER_TXN_ERR_SUCCESS_PROGRESS;
    }

    NVGspBooterTransactionLifetimePlan staged = {};
    staged.resetSequenceCompleted = progress->resetSequenceCompleted;
    staged.firmwareDmaSubmitted = progress->firmwareDmaSubmitted;
    staged.firmwareDmaRetired = progress->allFirmwareDmaIdleObserved;
    staged.wprMetaAddressPublished = progress->wprMetaAddressPublished;
    staged.bootStarted = progress->bootStarted;
    staged.haltProven = progress->haltObservedWithinBudget;
    staged.mailbox0Observed = progress->mailbox0Observed;
    staged.mailboxPairObserved = progress->mailbox0Observed && progress->mailbox1Observed;
    staged.terminalOutcomePresent = recovery || success;
    staged.terminalSuccess = success != nullptr;
    staged.terminalFailure = recovery != nullptr;

    staged.firmwareStagingMayStillBeReferenced =
        progress->firmwareDmaSubmitted && !progress->allFirmwareDmaIdleObserved;
    staged.firmwareStagingMustRemainPinned = staged.firmwareStagingMayStillBeReferenced;
    staged.firmwareStagingReleaseAuthorized = !staged.firmwareStagingMayStillBeReferenced;
    staged.wprMetaMayStillBeReferenced = progress->wprMetaAddressPublished;
    staged.wprMetaMustRemainPinned = staged.wprMetaMayStillBeReferenced;
    staged.wprMetaReleaseAuthorized = !staged.wprMetaMayStillBeReferenced;

    staged.mailboxReadAfterTimeoutProvesCompletion = false;
    staged.resetAloneProvesRetirement = false;
    staged.recoveryGuardMayBeDismissed = false;
    staged.requiresFreshExactTargetOwnership = false;
    staged.requiresFreshSequenceRevalidation = false;

    if (recovery) {
        staged.failureEvent = recovery->event;
        staged.completionUncertain = recovery->firmwareDmaCompletionUncertain ||
                                     recovery->bootCompletionUncertain;
        staged.firmwareStagingMayStillBeReferenced = recovery->firmwareStagingMayStillBeReferenced;
        staged.wprMetaMayStillBeReferenced = recovery->wprMetaMayStillBeReferenced;
        staged.firmwareStagingMustRemainPinned = recovery->firmwareStagingMayStillBeReferenced;
        staged.wprMetaMustRemainPinned = recovery->wprMetaMayStillBeReferenced;
        staged.firmwareStagingReleaseAuthorized = recovery->firmwareStagingReleaseAuthorized;
        staged.wprMetaReleaseAuthorized = recovery->wprMetaReleaseAuthorized;
        staged.quarantineFirmwareStaging = recovery->quarantineFirmwareStaging;
        staged.quarantineWprMeta = recovery->quarantineWprMeta;
        staged.quarantineSec2State = recovery->quarantineSec2State;
        staged.requiresFreshExactTargetOwnership = recovery->requiresFreshExactTargetOwnership;
        staged.requiresFreshSequenceRevalidation = recovery->requiresFreshSequenceRevalidation;

        switch (recovery->event) {
        case NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT:
        case NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT:
            staged.phase = NV_GSP_BOOTER_TXN_PHASE_FAILURE_PRE_DMA;
            break;
        case NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT:
            staged.phase = recovery->laterDmaIdleRetirementObserved
                ? NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_RETIRED
                : NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_QUARANTINE;
            break;
        case NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT:
        case NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE:
            staged.phase = NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE;
            break;
        default:
            return NV_GSP_BOOTER_TXN_ERR_RECOVERY;
        }
    } else if (success) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_SUCCESS_RETIRED;
        staged.completionUncertain = false;
        staged.firmwareStagingMayStillBeReferenced = false;
        staged.wprMetaMayStillBeReferenced = false;
        staged.firmwareStagingMustRemainPinned = false;
        staged.wprMetaMustRemainPinned = false;
        staged.firmwareStagingReleaseAuthorized = true;
        staged.wprMetaReleaseAuthorized = true;
        staged.recoveryGuardMayBeDismissed = true;
    } else if (!progress->resetSequenceCompleted) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION;
    } else if (!progress->firmwareDmaSubmitted) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_RESET_READY;
    } else if (!progress->allFirmwareDmaIdleObserved) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_IN_FLIGHT;
    } else if (!progress->wprMetaAddressPublished) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_RETIRED;
    } else if (!progress->bootStarted) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_WPR_META_PUBLISHED;
    } else if (!progress->haltObservedWithinBudget) {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_BOOT_RUNNING;
    } else {
        staged.phase = NV_GSP_BOOTER_TXN_PHASE_HALT_OBSERVED;
    }

    staged.retryAuthorized = false;
    staged.runtimeRecoveryAuthorized = false;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_TXN_OK;
}

#endif // NV_GSP_BOOTER_TRANSACTION_LIFETIME_HPP
