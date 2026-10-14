// SPDX-License-Identifier: MIT
// Pure Tiger Lake PPGTT batch dispatch into an already-retired/empty ring.
#ifndef IntelGtDispatch_hpp
#define IntelGtDispatch_hpp

#include "IntelGtRing.hpp"

// intel_gpu_commands.h: MI_BATCH_PPGTT_HSW. The live Tiger Lake BCS0 path
// executes batches through its private PPGTT, so dropping this bit silently
// changes the address space while leaving a perfectly well-formed command.
#define IG_GTD_BB_START_PPGTT_I915_0x100 0x100u

static inline IGU32 igGtdPpgttBatchStartWord(void)
{
    return igGtMiBatchBufferStart() | IG_GTD_BB_START_PPGTT_I915_0x100;
}

// Compose the existing three-dword BB_START packet, add the PPGTT selector,
// then hand the packet to IntelGtRing.hpp for wrap/padding/tail arithmetic.
//
// EMPTY RING is part of the function name and contract. The caller must have
// observed retirement before calling it, so HEAD == TAIL is the logical ring
// state even though no live HEAD MMIO read is needed. This is exactly the state
// IntelGtBringup has before each new synchronous copy request.
static inline IGU32 igGtdWritePpgttBatchStartToEmptyRing(
    IGU32 *ring,
    IGU32 ringDwords,
    IGU32 engine,
    IGU32 tailBytes,
    IGU64 batchAddress,
    IGU32 *outBatchVerdict,
    IGU32 *outTailBytes)
{
    IGU32 packet[IG_GT_MI_BB_START_DWORDS_POLICY_3];
    IGU32 batchVerdict;
    IGU32 written = 0u;

    if (outBatchVerdict != 0) { *outBatchVerdict = IG_GTQ_BATCH_NOT_ATTEMPTED; }
    if (outTailBytes != 0) { *outTailBytes = tailBytes; }

    batchVerdict = igGtqWriteBatchStart(packet,
                                        IG_GT_MI_BB_START_DWORDS_POLICY_3,
                                        batchAddress,
                                        &written);
    if (outBatchVerdict != 0) { *outBatchVerdict = batchVerdict; }
    if (!igGtqBatchSucceeded(batchVerdict)) {
        return IG_GTR_RING_EMPTY_PAYLOAD;
    }

    // The generic composer deliberately has no address-space opinion. Add the
    // Tiger Lake private-PPGTT selector only after it has validated the address
    // and produced a complete packet.
    packet[0] = igGtdPpgttBatchStartWord();

    return igGtrWriteWrapped(ring, ringDwords,
                             packet, written,
                             engine,
                             tailBytes, tailBytes,
                             outTailBytes);
}

#endif // IntelGtDispatch_hpp
