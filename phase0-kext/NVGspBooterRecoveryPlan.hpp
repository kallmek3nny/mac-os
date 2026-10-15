// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter failure/recovery and backing-lifetime planner.
//
// This helper performs no GPU I/O. It classifies failures from the already-
// validated GA107 SEC2 execution sequence and tells a future runtime which
// backing objects must remain retained/quarantined. It never authorizes a
// recovery write or a retry.
//
// Source-backed boundaries pinned here:
//   - Linux Nova falcon.rs: reset/core-select/memory-scrub complete before the
//     DMA firmware load begins. Each 256-byte FBDMA transfer is followed by a
//     bounded DMATRFCMD.IDLE poll before the load path can advance/release its
//     coherent source object.
//   - Linux Nova falcon/hal/ga102.rs: RESET_READY is best-effort/non-fatal;
//     core-select VALID and memory-scrub completion are bounded requirements.
//   - Linux Nova firmware/booter.rs: SEC2 reset -> load -> publish WPR metadata
//     through mailbox0/1 -> start/wait -> mailbox0 status.
//   - NVIDIA OpenRM kernel_gsp_falcon_ga102.c: DMA timeout is returned as an
//     error; HS execution still reads mailboxes after a failed halt wait and
//     returns the halt-wait status. Therefore a mailbox value observed after a
//     halt timeout is not completion/retirement proof.
//
// No source used by this milestone proves that a post-failure Falcon reset
// drains every outstanding FBDMA source reference or retires a WPR metadata
// reference published to a started Booter. Reset alone is therefore never used
// as backing-release proof. Runtime writes and retries remain unauthorized.
#ifndef NV_GSP_BOOTER_RECOVERY_PLAN_HPP
#define NV_GSP_BOOTER_RECOVERY_PLAN_HPP

#include "NVGspBooterExecSequencePlan.hpp"

#define NV_GSP_BOOTER_RECOVERY_EVENT_RESET_READY_TIMEOUT       1u
#define NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT       2u
#define NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT      3u
#define NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT      4u
#define NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT         5u
#define NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE          6u
#define NV_GSP_BOOTER_RECOVERY_EVENT_LIMIT                     6u

#define NV_GSP_BOOTER_RECOVERY_OK                              0u
#define NV_GSP_BOOTER_RECOVERY_ERR_NULL_OUTPUT                 1u
#define NV_GSP_BOOTER_RECOVERY_ERR_NULL_SEQUENCE               2u
#define NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE                    3u
#define NV_GSP_BOOTER_RECOVERY_ERR_EVENT                       4u
#define NV_GSP_BOOTER_RECOVERY_ERR_NONFATAL_EVENT              5u
#define NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION                 6u
#define NV_GSP_BOOTER_RECOVERY_VERDICT_LIMIT                   6u

struct NVGspBooterRecoveryObservation {
    NVGspBooterHsU32 event;
    bool haltProven;
    bool mailbox0Observed;
    NVGspBooterHsU32 mailbox0Value;
    bool laterDmaIdleObserved;
    bool postFailureResetObserved;
};

struct NVGspBooterRecoveryPlan {
    NVGspBooterHsU32 event;

    bool failureBeforeFirmwareDmaSubmission;
    bool firmwareDmaWasSubmitted;
    bool bootWasStarted;
    bool firmwareDmaCompletionUncertain;
    bool bootCompletionUncertain;
    bool haltProven;
    bool mailboxFailureObserved;

    bool firmwareStagingMayStillBeReferenced;
    bool wprMetaMayStillBeReferenced;
    bool firmwareStagingReleaseAuthorized;
    bool wprMetaReleaseAuthorized;
    bool quarantineFirmwareStaging;
    bool quarantineWprMeta;
    bool quarantineSec2State;

    bool laterDmaIdleCanRetireFirmwareStaging;
    bool laterDmaIdleRetirementObserved;
    bool mailboxReadAfterTimeoutProvesCompletion;
    bool resetAloneProvesRetirement;
    bool postFailureResetObserved;
    bool postFailureResetAllowsBackingRelease;

    bool requiresFreshExactTargetOwnership;
    bool requiresFreshSequenceRevalidation;
    bool retryAuthorized;
    bool runtimeRecoveryAuthorized;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvGspBooterRecoverySucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_RECOVERY_OK;
}

static inline const char *nvGspBooterRecoveryVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
    case NV_GSP_BOOTER_RECOVERY_OK:                 return "ok";
    case NV_GSP_BOOTER_RECOVERY_ERR_NULL_OUTPUT:    return "null-output";
    case NV_GSP_BOOTER_RECOVERY_ERR_NULL_SEQUENCE:  return "null-sequence";
    case NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE:       return "sequence";
    case NV_GSP_BOOTER_RECOVERY_ERR_EVENT:          return "event";
    case NV_GSP_BOOTER_RECOVERY_ERR_NONFATAL_EVENT: return "nonfatal-event";
    case NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION:    return "observation";
    default:                                         return "unknown";
    }
}

static inline bool nvGspBooterRecoverySequenceValid(
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

static inline bool nvGspBooterRecoveryObservationValid(
    const NVGspBooterRecoveryObservation *observation)
{
    if (!observation || observation->event == 0u ||
        observation->event > NV_GSP_BOOTER_RECOVERY_EVENT_LIMIT)
        return false;

    switch (observation->event) {
    case NV_GSP_BOOTER_RECOVERY_EVENT_RESET_READY_TIMEOUT:
    case NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT:
    case NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT:
        return !observation->haltProven &&
               !observation->mailbox0Observed &&
               observation->mailbox0Value == 0u &&
               !observation->laterDmaIdleObserved;

    case NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT:
        return !observation->haltProven &&
               !observation->mailbox0Observed &&
               observation->mailbox0Value == 0u;

    case NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT:
        // OpenRM may read mailbox values after the wait has already failed.
        // Accept that observation, but never turn it into completion proof.
        return !observation->haltProven &&
               !observation->laterDmaIdleObserved &&
               (observation->mailbox0Observed || observation->mailbox0Value == 0u);

    case NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE:
        return observation->haltProven &&
               observation->mailbox0Observed &&
               observation->mailbox0Value != 0u &&
               !observation->laterDmaIdleObserved;

    default:
        return false;
    }
}

// Atomic refusal: *out is assigned only after the sequence and semantic failure
// observation are internally consistent. This function emits policy only.
static inline NVGspBooterHsU32 nvGspPlanGa107BooterRecovery(
    const NVGspBooterExecSequencePlan *sequence,
    const NVGspBooterRecoveryObservation *observation,
    NVGspBooterRecoveryPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_RECOVERY_ERR_NULL_OUTPUT;
    if (!sequence)
        return NV_GSP_BOOTER_RECOVERY_ERR_NULL_SEQUENCE;
    if (!nvGspBooterRecoverySequenceValid(sequence))
        return NV_GSP_BOOTER_RECOVERY_ERR_SEQUENCE;
    if (!observation || observation->event == 0u ||
        observation->event > NV_GSP_BOOTER_RECOVERY_EVENT_LIMIT)
        return NV_GSP_BOOTER_RECOVERY_ERR_EVENT;

    // GA102 RESET_READY is explicitly a best-effort pre-wait. Its timeout does
    // not make the reset operation fail and must not be converted into a
    // recovery/quarantine event by a future executor.
    if (observation->event == NV_GSP_BOOTER_RECOVERY_EVENT_RESET_READY_TIMEOUT)
        return NV_GSP_BOOTER_RECOVERY_ERR_NONFATAL_EVENT;

    if (!nvGspBooterRecoveryObservationValid(observation))
        return NV_GSP_BOOTER_RECOVERY_ERR_OBSERVATION;

    NVGspBooterRecoveryPlan staged = {};
    staged.event = observation->event;
    staged.haltProven = observation->haltProven;
    staged.postFailureResetObserved = observation->postFailureResetObserved;

    // No referenced source establishes a reset-only retirement guarantee.
    staged.resetAloneProvesRetirement = false;
    staged.postFailureResetAllowsBackingRelease = false;
    staged.mailboxReadAfterTimeoutProvesCompletion = false;

    // Every failed attempt needs a completely fresh ownership/sequence proof
    // before a future executor may even consider another attempt. This pure
    // planner cannot authorize that executor or any recovery write.
    staged.requiresFreshExactTargetOwnership = true;
    staged.requiresFreshSequenceRevalidation = true;
    staged.retryAuthorized = false;
    staged.runtimeRecoveryAuthorized = false;
    staged.runtimeWriteAuthorized = false;

    switch (observation->event) {
    case NV_GSP_BOOTER_RECOVERY_EVENT_CORE_SELECT_TIMEOUT:
    case NV_GSP_BOOTER_RECOVERY_EVENT_MEMORY_SCRUB_TIMEOUT:
        // reset()/core selection/scrubbing precede dma_load(). No firmware DMA
        // base or WPR mailbox address has been submitted by this attempt yet.
        staged.failureBeforeFirmwareDmaSubmission = true;
        staged.firmwareDmaWasSubmitted = false;
        staged.bootWasStarted = false;
        staged.firmwareDmaCompletionUncertain = false;
        staged.bootCompletionUncertain = false;
        staged.firmwareStagingMayStillBeReferenced = false;
        staged.wprMetaMayStillBeReferenced = false;
        staged.firmwareStagingReleaseAuthorized = true;
        staged.wprMetaReleaseAuthorized = true;
        staged.quarantineFirmwareStaging = false;
        staged.quarantineWprMeta = false;
        staged.quarantineSec2State = true;
        staged.laterDmaIdleCanRetireFirmwareStaging = false;
        staged.laterDmaIdleRetirementObserved = false;
        break;

    case NV_GSP_BOOTER_RECOVERY_EVENT_FIRMWARE_DMA_TIMEOUT:
        staged.failureBeforeFirmwareDmaSubmission = false;
        staged.firmwareDmaWasSubmitted = true;
        staged.bootWasStarted = false;
        staged.bootCompletionUncertain = false;
        staged.wprMetaMayStillBeReferenced = false;
        staged.wprMetaReleaseAuthorized = true;
        staged.quarantineWprMeta = false;
        staged.quarantineSec2State = true;
        staged.laterDmaIdleCanRetireFirmwareStaging = true;
        staged.laterDmaIdleRetirementObserved = observation->laterDmaIdleObserved;

        if (observation->laterDmaIdleObserved) {
            // The exact source-backed completion predicate for this DMA engine
            // has subsequently been observed. This retires the staging source,
            // but does not authorize a retry or prove SEC2 state recoverable.
            staged.firmwareDmaCompletionUncertain = false;
            staged.firmwareStagingMayStillBeReferenced = false;
            staged.firmwareStagingReleaseAuthorized = true;
            staged.quarantineFirmwareStaging = false;
        } else {
            staged.firmwareDmaCompletionUncertain = true;
            staged.firmwareStagingMayStillBeReferenced = true;
            staged.firmwareStagingReleaseAuthorized = false;
            staged.quarantineFirmwareStaging = true;
        }
        break;

    case NV_GSP_BOOTER_RECOVERY_EVENT_BOOT_HALT_TIMEOUT:
        // Reaching boot means both firmware DMA loads already passed their
        // source-backed IDLE completion polls. The firmware staging source is
        // retired, but the WPR metadata address has been published and SEC2 was
        // started. A later mailbox read (even zero) is not halt proof.
        staged.failureBeforeFirmwareDmaSubmission = false;
        staged.firmwareDmaWasSubmitted = true;
        staged.bootWasStarted = true;
        staged.firmwareDmaCompletionUncertain = false;
        staged.bootCompletionUncertain = true;
        staged.firmwareStagingMayStillBeReferenced = false;
        staged.wprMetaMayStillBeReferenced = true;
        staged.firmwareStagingReleaseAuthorized = true;
        staged.wprMetaReleaseAuthorized = false;
        staged.quarantineFirmwareStaging = false;
        staged.quarantineWprMeta = true;
        staged.quarantineSec2State = true;
        staged.laterDmaIdleCanRetireFirmwareStaging = false;
        staged.laterDmaIdleRetirementObserved = false;
        break;

    case NV_GSP_BOOTER_RECOVERY_EVENT_MAILBOX0_FAILURE:
        // Halt completed, then mailbox0 reported Booter failure. That proves the
        // halt wait itself completed, but no current source proves that a failed
        // Booter has retired every possible WPR metadata reference. Retain it.
        staged.failureBeforeFirmwareDmaSubmission = false;
        staged.firmwareDmaWasSubmitted = true;
        staged.bootWasStarted = true;
        staged.firmwareDmaCompletionUncertain = false;
        staged.bootCompletionUncertain = false;
        staged.mailboxFailureObserved = true;
        staged.firmwareStagingMayStillBeReferenced = false;
        staged.wprMetaMayStillBeReferenced = true;
        staged.firmwareStagingReleaseAuthorized = true;
        staged.wprMetaReleaseAuthorized = false;
        staged.quarantineFirmwareStaging = false;
        staged.quarantineWprMeta = true;
        staged.quarantineSec2State = true;
        staged.laterDmaIdleCanRetireFirmwareStaging = false;
        staged.laterDmaIdleRetirementObserved = false;
        break;

    default:
        return NV_GSP_BOOTER_RECOVERY_ERR_EVENT;
    }

    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_RECOVERY_OK;
}

#endif // NV_GSP_BOOTER_RECOVERY_PLAN_HPP
