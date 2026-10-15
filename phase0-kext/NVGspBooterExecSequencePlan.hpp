// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter executor-sequencing planner.
//
// This helper performs no GPU I/O and intentionally cannot authorize it. It
// composes the already-validated GA107 Booter run plan with the source-backed
// GA102+ Falcon execution semantics used by Linux Nova and NVIDIA OpenRM.
//
// Source-backed sequencing facts pinned here:
//   - Linux Nova drivers/gpu/nova-core/firmware/booter.rs:
//       SEC2 reset -> load -> mailbox low/high -> boot -> mailbox0 status.
//   - Linux Nova drivers/gpu/nova-core/falcon.rs:
//       GA102 uses DMA loading; FBIF target is coherent system memory with
//       physical addresses; Falcon DMA transfers are 256 bytes and each waits
//       for DMA idle with a bounded two-second timeout; mailbox0 is written
//       before mailbox1; start is followed by a bounded halt wait and mailbox
//       reads in 0,1 order.
//   - Linux Nova drivers/gpu/nova-core/falcon/hal/ga102.rs:
//       reset-ready is best-effort for 150 us, engine reset is followed by
//       memory-scrub completion, Falcon-core selection waits for VALID, and a
//       second scrub wait occurs before reset returns.
//   - NVIDIA OpenRM kernel_falcon_ga102.c:
//       GA102/GA107 pre-reset RESET_READY timeout is explicitly non-fatal.
//
// No register offsets, arbitrary write lists, raw command streams, or generic
// GPU-address ABI are emitted. Recovery after an uncertain DMA/boot timeout is
// not yet proven, so the plan requires retention/quarantine and keeps runtime
// writes disabled.
#ifndef NV_GSP_BOOTER_EXEC_SEQUENCE_PLAN_HPP
#define NV_GSP_BOOTER_EXEC_SEQUENCE_PLAN_HPP

#include "NVGspBooterRunPlan.hpp"

#define NV_GSP_BOOTER_EXEC_OK                         0u
#define NV_GSP_BOOTER_EXEC_ERR_NULL_OUTPUT            1u
#define NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN               2u
#define NV_GSP_BOOTER_EXEC_ERR_CONTAINER              3u
#define NV_GSP_BOOTER_EXEC_ERR_TARGET                 4u
#define NV_GSP_BOOTER_EXEC_ERR_BROM                   5u
#define NV_GSP_BOOTER_EXEC_ERR_DMA_SHAPE              6u
#define NV_GSP_BOOTER_EXEC_ERR_INTERNAL               7u
#define NV_GSP_BOOTER_EXEC_VERDICT_LIMIT              7u

#define NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES            256u
#define NV_GSP_BOOTER_EXEC_RESET_READY_BEST_EFFORT_US 150u
#define NV_GSP_BOOTER_EXEC_RESET_ASSERT_HOLD_US       10u
#define NV_GSP_BOOTER_EXEC_CORE_SELECT_TIMEOUT_US     10000u
#define NV_GSP_BOOTER_EXEC_MEM_SCRUB_TIMEOUT_US       20000u
#define NV_GSP_BOOTER_EXEC_DMA_IDLE_TIMEOUT_US        2000000u
#define NV_GSP_BOOTER_EXEC_BOOT_HALT_TIMEOUT_US       2000000u

struct NVGspBooterExecSequencePlan {
    NVGspBooterHsU32 engineIdMask;
    NVGspBooterHsU32 ucodeId;
    NVGspBooterHsU32 pkcDataOffset;
    NVGspBooterHsU32 bootVector;

    NVGspBooterHsU32 secureImemBytes;
    NVGspBooterHsU32 secureImemTransferCount;
    NVGspBooterHsU32 dmemBytes;
    NVGspBooterHsU32 dmemTransferCount;
    NVGspBooterHsU32 dmaBlockBytes;

    NVGspBooterHsU32 mailbox0Input;
    NVGspBooterHsU32 mailbox1Input;
    NVGspBooterHsU32 mailbox0SuccessValue;

    NVGspBooterHsU32 resetReadyBestEffortTimeoutUs;
    NVGspBooterHsU32 resetAssertHoldUs;
    NVGspBooterHsU32 coreSelectTimeoutUs;
    NVGspBooterHsU32 memoryScrubTimeoutUs;
    NVGspBooterHsU32 dmaIdleTimeoutUs;
    NVGspBooterHsU32 bootHaltTimeoutUs;

    bool resetReadyTimeoutIsNonFatal;
    bool engineResetRequired;
    bool memoryScrubAfterResetRequired;
    bool falconCoreSelectRequired;
    bool coreSelectValidPollRequired;
    bool secondMemoryScrubWaitRequired;
    bool falconRmSeedRequired;

    bool dmaPhysicalNoContextRequired;
    bool dmaControllerResetRequired;
    bool dmaTargetCoherentSystemMemory;
    bool dmaAddressTypePhysical;
    bool secureImemBeforeDmem;
    bool perTransferIdlePollRequired;

    bool bromProgramRequired;
    bool bromOrderPkcEngineUcodeRsa3k;
    bool bromRsa3kRequired;
    bool bootVectorProgramRequired;

    bool mailbox0BeforeMailbox1;
    bool startAfterMailboxes;
    bool boundedHaltPollRequired;
    bool mailbox0BeforeMailbox1Readback;
    bool nonzeroMailbox0IsFailure;

    bool firmwareDmaBackingRetainUntilTransferIdle;
    bool quarantineFirmwareDmaBackingOnTransferTimeout;
    bool wprMetaBackingRetainUntilHalt;
    bool quarantineWprMetaBackingOnBootTimeout;
    bool quarantineWprMetaBackingOnBootFailure;
    bool timeoutRecoveryProven;
    bool runtimeWriteAuthorized;
    bool valid;
};

static inline bool nvGspBooterExecSequenceSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_EXEC_OK;
}

static inline const char *nvGspBooterExecSequenceVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
    case NV_GSP_BOOTER_EXEC_OK:              return "ok";
    case NV_GSP_BOOTER_EXEC_ERR_NULL_OUTPUT: return "null-output";
    case NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN:    return "run-plan";
    case NV_GSP_BOOTER_EXEC_ERR_CONTAINER:   return "container";
    case NV_GSP_BOOTER_EXEC_ERR_TARGET:      return "target";
    case NV_GSP_BOOTER_EXEC_ERR_BROM:        return "brom";
    case NV_GSP_BOOTER_EXEC_ERR_DMA_SHAPE:   return "dma-shape";
    case NV_GSP_BOOTER_EXEC_ERR_INTERNAL:    return "internal";
    default:                                 return "unknown";
    }
}

static inline bool nvGspBooterExecBlockAligned(NVGspBooterHsU32 value)
{
    return (value & (NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES - 1u)) == 0u;
}

// Atomic refusal: *out is assigned only after the immutable source, observed
// fuse, private image, WPR metadata DMA, BROM derivation, exact target and DMA
// transfer shape have all been revalidated through the existing pure planners.
static inline NVGspBooterHsU32 nvGspPlanGa107BooterExecSequence(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    const NVGspBooterFuseObservation *observation,
    const unsigned char *privateImage,
    NVGspBooterHsU64 privateImageBytes,
    NVGspBooterHsU64 wprMetaDmaAddress,
    NVGspBooterExecSequencePlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_EXEC_ERR_NULL_OUTPUT;

    NVGspBooterRunPlan run = {};
    const NVGspBooterHsU32 runVerdict = nvGspPlanGa107BooterRun(
        container,
        containerBytes,
        observation,
        privateImage,
        privateImageBytes,
        wprMetaDmaAddress,
        &run);
    if (!nvGspBooterRunPlanSucceeded(runVerdict) || !run.valid)
        return NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN;

    NVGspBooterHsPlan hs = {};
    if (!nvGspBooterHsSucceeded(nvGspPlanBooterHs(container, containerBytes, &hs)) || !hs.valid)
        return NV_GSP_BOOTER_EXEC_ERR_CONTAINER;

    if (run.engineId != NV_GSP_GA107_BOOTER_HS_ENGINE_ID ||
        run.ucodeId != NV_GSP_GA107_BOOTER_HS_UCODE_ID ||
        hs.engineId != run.engineId || hs.ucodeId != run.ucodeId ||
        run.nonSecureImemLoadRequired)
        return NV_GSP_BOOTER_EXEC_ERR_TARGET;

    // Nova derives Booter's BROM PKC offset from the signature patch location
    // relative to os_data, and on GA102+ starts at the secure app0_code source
    // offset because there is no separate non-secure IMEM load.
    if (hs.patchLocation < hs.osDataOffset)
        return NV_GSP_BOOTER_EXEC_ERR_BROM;
    const NVGspBooterHsU32 pkcDataOffset = hs.patchLocation - hs.osDataOffset;
    if (pkcDataOffset == 0u || run.secureImemSourceOffset != hs.appCodeOffset ||
        run.dmemSourceOffset != hs.osDataOffset)
        return NV_GSP_BOOTER_EXEC_ERR_BROM;

    // This milestone deliberately narrows GA107 execution to the exact checked
    // artifacts' 256-byte FBDMA shape. Nova can round a final transfer up, but
    // accepting that broader case here would make retention/bounds proof more
    // complicated for no benefit to the current fixtures.
    if (!nvGspBooterExecBlockAligned(run.secureImemSourceOffset) ||
        !nvGspBooterExecBlockAligned(run.secureImemDestinationOffset) ||
        !nvGspBooterExecBlockAligned(run.secureImemBytes) ||
        !nvGspBooterExecBlockAligned(run.dmemSourceOffset) ||
        !nvGspBooterExecBlockAligned(run.dmemDestinationOffset) ||
        !nvGspBooterExecBlockAligned(run.dmemBytes) ||
        run.secureImemBytes == 0u || run.dmemBytes == 0u)
        return NV_GSP_BOOTER_EXEC_ERR_DMA_SHAPE;

    const NVGspBooterHsU32 imemTransfers =
        run.secureImemBytes / NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES;
    const NVGspBooterHsU32 dmemTransfers =
        run.dmemBytes / NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES;
    if (imemTransfers == 0u || dmemTransfers == 0u ||
        !run.resetRequired || !run.secureImemLoadRequired ||
        !run.dmemLoadRequired || !run.mailboxPairRequired ||
        !run.boundedCompletionPollRequired || !run.nonzeroMailbox0IsFailure)
        return NV_GSP_BOOTER_EXEC_ERR_INTERNAL;

    NVGspBooterExecSequencePlan staged = {};
    staged.engineIdMask = run.engineId;
    staged.ucodeId = run.ucodeId;
    staged.pkcDataOffset = pkcDataOffset;
    staged.bootVector = run.secureImemSourceOffset;
    staged.secureImemBytes = run.secureImemBytes;
    staged.secureImemTransferCount = imemTransfers;
    staged.dmemBytes = run.dmemBytes;
    staged.dmemTransferCount = dmemTransfers;
    staged.dmaBlockBytes = NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES;
    staged.mailbox0Input = run.mailbox0Input;
    staged.mailbox1Input = run.mailbox1Input;
    staged.mailbox0SuccessValue = run.mailbox0SuccessValue;

    staged.resetReadyBestEffortTimeoutUs = NV_GSP_BOOTER_EXEC_RESET_READY_BEST_EFFORT_US;
    staged.resetAssertHoldUs = NV_GSP_BOOTER_EXEC_RESET_ASSERT_HOLD_US;
    staged.coreSelectTimeoutUs = NV_GSP_BOOTER_EXEC_CORE_SELECT_TIMEOUT_US;
    staged.memoryScrubTimeoutUs = NV_GSP_BOOTER_EXEC_MEM_SCRUB_TIMEOUT_US;
    staged.dmaIdleTimeoutUs = NV_GSP_BOOTER_EXEC_DMA_IDLE_TIMEOUT_US;
    staged.bootHaltTimeoutUs = NV_GSP_BOOTER_EXEC_BOOT_HALT_TIMEOUT_US;

    staged.resetReadyTimeoutIsNonFatal = true;
    staged.engineResetRequired = true;
    staged.memoryScrubAfterResetRequired = true;
    staged.falconCoreSelectRequired = true;
    staged.coreSelectValidPollRequired = true;
    staged.secondMemoryScrubWaitRequired = true;
    staged.falconRmSeedRequired = true;

    staged.dmaPhysicalNoContextRequired = true;
    staged.dmaControllerResetRequired = true;
    staged.dmaTargetCoherentSystemMemory = true;
    staged.dmaAddressTypePhysical = true;
    staged.secureImemBeforeDmem = true;
    staged.perTransferIdlePollRequired = true;

    staged.bromProgramRequired = true;
    staged.bromOrderPkcEngineUcodeRsa3k = true;
    staged.bromRsa3kRequired = true;
    staged.bootVectorProgramRequired = true;

    staged.mailbox0BeforeMailbox1 = true;
    staged.startAfterMailboxes = true;
    staged.boundedHaltPollRequired = true;
    staged.mailbox0BeforeMailbox1Readback = true;
    staged.nonzeroMailbox0IsFailure = true;

    // Lifetime policy is intentionally stricter than the reference code. If a
    // FBDMA transfer times out, the DMA engine may still own the source; if the
    // Booter halt wait times out or reports failure, SEC2/WPR ownership is not
    // considered recoverable yet. A future executor must retain/quarantine the
    // affected backing instead of freeing or reusing it.
    staged.firmwareDmaBackingRetainUntilTransferIdle = true;
    staged.quarantineFirmwareDmaBackingOnTransferTimeout = true;
    staged.wprMetaBackingRetainUntilHalt = true;
    staged.quarantineWprMetaBackingOnBootTimeout = true;
    staged.quarantineWprMetaBackingOnBootFailure = true;
    staged.timeoutRecoveryProven = false;
    staged.runtimeWriteAuthorized = false;
    staged.valid = true;

    *out = staged;
    return NV_GSP_BOOTER_EXEC_OK;
}

#endif // NV_GSP_BOOTER_EXEC_SEQUENCE_PLAN_HPP
