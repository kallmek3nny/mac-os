// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter success/retirement evidence planner.
//
// This helper performs no GPU I/O. It consumes the already-validated GA107
// execution sequence plus observations from a future bounded executor and
// decides whether the firmware staging source and WPR metadata backing have a
// source-backed lifetime end after a successful Booter run.
//
// Source-backed boundaries pinned here:
//   - Linux Nova firmware/booter.rs: Booter::run resets SEC2, loads the signed
//     image, publishes the WPR metadata DMA address through mailbox0/1, waits
//     for Falcon::boot to return, and accepts the run only when mailbox0 == 0.
//   - Linux Nova gsp/hal/tu102.rs: wpr_meta is deliberately declared before a
//     recovery ScopeGuard so that a failed Booter run resets SEC2 before that
//     coherent allocation is freed. On successful Booter return the guard is
//     dismissed and the boot function returns, so the WPR metadata allocation
//     may leave that guarded lifetime without a compensating SEC2 reset.
//   - Linux Nova falcon.rs: firmware DMA loading waits for the per-transfer
//     idle predicate before advancing, so reaching the Booter start implies the
//     firmware staging source has completed its FBDMA use.
//
// A mailbox value read without a proven bounded halt is not accepted. Missing
// DMA-idle, halt, ordering, or mailbox evidence refuses atomically. This pure
// planner never authorizes execution, retry, reset, recovery, or register I/O.
#ifndef NV_GSP_BOOTER_SUCCESS_RETIREMENT_PLAN_HPP
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_PLAN_HPP

#include "NVGspBooterExecSequencePlan.hpp"

#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK                    0u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_OUTPUT       1u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_SEQUENCE     2u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE          3u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION       4u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_DMA_NOT_RETIRED   5u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN   6u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_NOT_PROVEN 7u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_FAILURE   8u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER    9u
#define NV_GSP_BOOTER_SUCCESS_RETIREMENT_VERDICT_LIMIT         9u

struct NVGspBooterSuccessObservation {
    bool allFirmwareDmaIdleObserved;
    bool wprMetaAddressPublished;
    bool bootStarted;
    bool haltObserved;
    bool haltWithinBudget;
    bool mailbox0Observed;
    NVGspBooterHsU32 mailbox0Value;
    bool mailbox1Observed;
    bool mailboxReadbackAfterHalt;
    bool mailbox0BeforeMailbox1Readback;
};

struct NVGspBooterSuccessRetirementPlan {
    bool firmwareDmaCompletionProven;
    bool firmwareStagingMayStillBeReferenced;
    bool firmwareStagingReleaseAuthorized;

    bool wprMetaWasPublished;
    bool bootWasStarted;
    bool haltProven;
    bool mailbox0SuccessProven;
    bool mailboxPairReadbackProven;
    bool wprMetaMayStillBeReferenced;
    bool wprMetaReleaseAuthorized;

    bool recoveryGuardMayBeDismissed;
    bool postSuccessResetRequiredForBackingRelease;
    bool failureRecoveryPlannerRequired;
    bool retryAuthorized;
    bool runtimeRecoveryAuthorized;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvGspBooterSuccessRetirementSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK;
}

static inline const char *nvGspBooterSuccessRetirementVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK:                     return "ok";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_OUTPUT:        return "null-output";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_SEQUENCE:      return "null-sequence";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE:           return "sequence";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION:        return "observation";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_DMA_NOT_RETIRED:    return "dma-not-retired";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN:    return "halt-not-proven";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_NOT_PROVEN: return "mailbox-not-proven";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_FAILURE:    return "mailbox-failure";
    case NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER:     return "readback-order";
    default:                                                       return "unknown";
    }
}

static inline bool nvGspBooterSuccessSequenceValid(
    const NVGspBooterExecSequencePlan *sequence)
{
    if (!sequence || !sequence->valid || sequence->runtimeWriteAuthorized ||
        sequence->timeoutRecoveryProven)
        return false;

    if (sequence->engineIdMask != NV_GSP_GA107_BOOTER_HS_ENGINE_ID ||
        sequence->ucodeId != NV_GSP_GA107_BOOTER_HS_UCODE_ID ||
        sequence->dmaBlockBytes != NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES ||
        sequence->secureImemTransferCount == 0u ||
        sequence->dmemTransferCount == 0u)
        return false;

    if (!sequence->engineResetRequired ||
        !sequence->memoryScrubAfterResetRequired ||
        !sequence->falconCoreSelectRequired ||
        !sequence->coreSelectValidPollRequired ||
        !sequence->secondMemoryScrubWaitRequired ||
        !sequence->dmaControllerResetRequired ||
        !sequence->dmaTargetCoherentSystemMemory ||
        !sequence->dmaAddressTypePhysical ||
        !sequence->secureImemBeforeDmem ||
        !sequence->perTransferIdlePollRequired ||
        !sequence->mailbox0BeforeMailbox1 ||
        !sequence->startAfterMailboxes ||
        !sequence->boundedHaltPollRequired ||
        !sequence->mailbox0BeforeMailbox1Readback ||
        !sequence->nonzeroMailbox0IsFailure)
        return false;

    if (!sequence->firmwareDmaBackingRetainUntilTransferIdle ||
        !sequence->quarantineFirmwareDmaBackingOnTransferTimeout ||
        !sequence->wprMetaBackingRetainUntilHalt ||
        !sequence->quarantineWprMetaBackingOnBootTimeout ||
        !sequence->quarantineWprMetaBackingOnBootFailure)
        return false;

    return true;
}

// Atomic refusal: *out is assigned only after every success predicate is
// mutually consistent. This planner emits backing-lifetime policy only.
static inline NVGspBooterHsU32 nvGspPlanGa107BooterSuccessRetirement(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterSuccessObservation *observation,
    NVGspBooterSuccessRetirementPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_OUTPUT;
    if (!sequence)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_NULL_SEQUENCE;
    if (!nvGspBooterSuccessSequenceValid(sequence))
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_SEQUENCE;
    if (!observation)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION;

    if (!observation->allFirmwareDmaIdleObserved)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_DMA_NOT_RETIRED;

    if (!observation->wprMetaAddressPublished || !observation->bootStarted)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_OBSERVATION;

    if (!observation->haltObserved || !observation->haltWithinBudget)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN;

    if (!observation->mailboxReadbackAfterHalt ||
        !observation->mailbox0BeforeMailbox1Readback)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER;

    if (!observation->mailbox0Observed || !observation->mailbox1Observed)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_NOT_PROVEN;

    if (observation->mailbox0Value != sequence->mailbox0SuccessValue)
        return NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_FAILURE;

    NVGspBooterSuccessRetirementPlan staged = {};
    staged.firmwareDmaCompletionProven = true;
    staged.firmwareStagingMayStillBeReferenced = false;
    staged.firmwareStagingReleaseAuthorized = true;

    staged.wprMetaWasPublished = true;
    staged.bootWasStarted = true;
    staged.haltProven = true;
    staged.mailbox0SuccessProven = true;
    staged.mailboxPairReadbackProven = true;
    staged.wprMetaMayStillBeReferenced = false;
    staged.wprMetaReleaseAuthorized = true;

    // Nova's successful TU102/GA10x path dismisses the recovery guard after
    // Booter::run returns. The failure path keeps the guard active specifically
    // so SEC2 is reset before wpr_meta can be freed. We model only that lifetime
    // distinction; this does not authorize the runtime writes that produced it.
    staged.recoveryGuardMayBeDismissed = true;
    staged.postSuccessResetRequiredForBackingRelease = false;
    staged.failureRecoveryPlannerRequired = false;
    staged.retryAuthorized = false;
    staged.runtimeRecoveryAuthorized = false;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;

    *out = staged;
    return NV_GSP_BOOTER_SUCCESS_RETIREMENT_OK;
}

#endif // NV_GSP_BOOTER_SUCCESS_RETIREMENT_PLAN_HPP
