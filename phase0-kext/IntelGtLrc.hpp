// SPDX-License-Identifier: MIT
// Pure Tiger Lake Gen12 XCS logical-ring-context composition.
//
// IntelGtBringup is the macOS IOKit shell. This header owns the first Gen12
// context block and the BCS0 descriptor word so those hardware-facing bits can
// execute under the host test suite on Linux/macOS before any MMIO write exists.
#ifndef IntelGtLrc_hpp
#define IntelGtLrc_hpp

#include "IntelGtPpgtt.hpp"

// gen12_xcs_offsets in reference/i915/intel_lrc.c begins with one NOP, then a
// posted 13-register LRI. The next block begins at slot 0x1c, which is exactly
// where IntelGtPpgtt.hpp starts its five NOPs.
#define IG_GTLRC_FIRST_NOP_SLOT_I915_0          0u
#define IG_GTLRC_FIRST_LRI_SLOT_I915_1          1u
#define IG_GTLRC_FIRST_PAIR_SLOT_I915_2         2u
#define IG_GTLRC_FIRST_PAIR_COUNT_I915_13       13u
#define IG_GTLRC_FIRST_BLOCK_DWORDS_I915_28     28u

// Value slots for the five ring/context registers Linux actively initializes.
#define IG_GTLRC_CONTEXT_CONTROL_VALUE_SLOT_I915_0x03 0x03u
#define IG_GTLRC_RING_HEAD_VALUE_SLOT_I915_0x05       0x05u
#define IG_GTLRC_RING_TAIL_VALUE_SLOT_I915_0x07       0x07u
#define IG_GTLRC_RING_START_VALUE_SLOT_I915_0x09      0x09u
#define IG_GTLRC_RING_CTL_VALUE_SLOT_I915_0x0B        0x0Bu

// intel_engine_regs.h: RING_CONTEXT_CONTROL bits 0 and 3. Linux composes the
// masked write through _MASKED_BIT_ENABLE/_DISABLE, whose mask half is +16.
#define IG_GTLRC_CTX_RESTORE_INHIBIT_I915_0x1   0x1u
#define IG_GTLRC_CTX_INHIBIT_SYNC_I915_0x8      0x8u
#define IG_GTLRC_MASK_SHIFT_I915_16             16u

// BCS0 is COPY engine class 1, instance 0. The class/instance bit positions are
// GEN11_ENGINE_CLASS_SHIFT / GEN11_ENGINE_INSTANCE_SHIFT in intel_lrc.h and are
// the descriptor format used on Tiger Lake (< 12.55).
#define IG_GTLRC_ENGINE_CLASS_SHIFT_I915_61     61u
#define IG_GTLRC_ENGINE_INSTANCE_SHIFT_I915_48  48u
#define IG_GTLRC_BCS_CLASS_I915_1               1u
#define IG_GTLRC_BCS0_INSTANCE_I915_0           0u

#define IG_GTLRC_RING_START_CEILING_POLICY_0xFFFFF000 0xFFFFF000ull

#define IG_GTLRC_WRITE_NOT_ATTEMPTED            0u
#define IG_GTLRC_WRITE_OK                       1u
#define IG_GTLRC_WRITE_NULL_SPAN                2u
#define IG_GTLRC_WRITE_SPAN_TOO_SHORT           3u
#define IG_GTLRC_WRITE_BAD_ENGINE               4u
#define IG_GTLRC_WRITE_BAD_RING                 5u
#define IG_GTLRC_WRITE_BAD_START                6u
#define IG_GTLRC_WRITE_HEAD_OUT_OF_RING         7u
#define IG_GTLRC_WRITE_TAIL_OUT_OF_RING         8u
#define IG_GTLRC_WRITE_TAIL_UNALIGNED           9u
#define IG_GTLRC_WRITE_LIMIT                    10u

static inline bool igGtLrcWriteSucceeded(IGU32 verdict)
{
    return verdict == IG_GTLRC_WRITE_OK;
}

static inline const char *igGtLrcWriteVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_GTLRC_WRITE_NOT_ATTEMPTED:    return "not attempted";
    case IG_GTLRC_WRITE_OK:               return "ok";
    case IG_GTLRC_WRITE_NULL_SPAN:        return "null context span";
    case IG_GTLRC_WRITE_SPAN_TOO_SHORT:   return "context span too short";
    case IG_GTLRC_WRITE_BAD_ENGINE:       return "invalid engine";
    case IG_GTLRC_WRITE_BAD_RING:         return "invalid ring geometry";
    case IG_GTLRC_WRITE_BAD_START:        return "invalid ring start";
    case IG_GTLRC_WRITE_HEAD_OUT_OF_RING: return "ring head outside ring";
    case IG_GTLRC_WRITE_TAIL_OUT_OF_RING: return "ring tail outside ring";
    case IG_GTLRC_WRITE_TAIL_UNALIGNED:   return "ring tail not qword aligned";
    default:                              return "unknown lrc write verdict";
    }
}

static inline IGU32 igGtLrcFirstBlockRegOffset(IGU32 pair)
{
    switch (pair) {
    case 0u:  return 0x244u;
    case 1u:  return 0x034u;
    case 2u:  return 0x030u;
    case 3u:  return 0x038u;
    case 4u:  return 0x03Cu;
    case 5u:  return 0x168u;
    case 6u:  return 0x140u;
    case 7u:  return 0x110u;
    case 8u:  return 0x1C0u;
    case 9u:  return 0x1C4u;
    case 10u: return 0x1C8u;
    case 11u: return 0x180u;
    case 12u: return 0x2B4u;
    default:  return IG_GTC_OFFSET_INVALID;
    }
}

static inline IGU32 igGtLrcFirstBlockLriHeader(void)
{
    return igGtcLriHeader(IG_GTLRC_FIRST_PAIR_COUNT_I915_13) |
           IG_GTPG_LRI_LRM_CS_MMIO_I915_0x80000 |
           IG_GTPG_LRI_FORCE_POSTED_I915_0x1000;
}

static inline IGU32 igGtLrcMaskedEnable(IGU32 bit)
{
    return bit | (bit << IG_GTLRC_MASK_SHIFT_I915_16);
}

static inline IGU32 igGtLrcMaskedDisable(IGU32 bit)
{
    return bit << IG_GTLRC_MASK_SHIFT_I915_16;
}

static inline IGU32 igGtLrcContextControl(bool inhibitRestore)
{
    IGU32 word = igGtLrcMaskedEnable(IG_GTLRC_CTX_INHIBIT_SYNC_I915_0x8) |
                 igGtLrcMaskedDisable(IG_GTLRC_CTX_RESTORE_INHIBIT_I915_0x1);
    if (inhibitRestore) {
        word |= IG_GTLRC_CTX_RESTORE_INHIBIT_I915_0x1;
    }
    return word;
}

static inline bool igGtLrcRingStartOk(IGU64 ringStart)
{
    return igGtIsPageAligned(ringStart) &&
           ringStart <= IG_GTLRC_RING_START_CEILING_POLICY_0xFFFFF000;
}

// Linux set_offsets(..., close=true) writes MI_BATCH_BUFFER_END and adds bit 0
// on Gen11+. Keep that Gen12 close word out of the macOS-only shell as well.
static inline IGU32 igGtLrcCloseWord(void)
{
    return igGtMiBatchBufferEnd() | 1u;
}

// Compose slots 0x00..0x1b of a Gen12 XCS context image. Every refusal occurs
// before caller memory is touched.
static inline IGU32 igGtLrcWriteFirstBlock(IGU32 *dwords,
                                           IGU32 dwordCount,
                                           IGU32 engine,
                                           IGU32 ringBytes,
                                           IGU64 ringStart,
                                           IGU32 ringHead,
                                           IGU32 ringTail,
                                           bool inhibitRestore,
                                           IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }
    if (dwords == 0) { return IG_GTLRC_WRITE_NULL_SPAN; }
    if (dwordCount < IG_GTLRC_FIRST_BLOCK_DWORDS_I915_28) {
        return IG_GTLRC_WRITE_SPAN_TOO_SHORT;
    }

    const IGU32 base = igGtEngineBase(engine);
    if (base == IG_GT_BASE_INVALID) { return IG_GTLRC_WRITE_BAD_ENGINE; }
    if (igGtRingSizeVerdict(ringBytes) != IG_GT_SUBMIT_OK) {
        return IG_GTLRC_WRITE_BAD_RING;
    }
    if (!igGtLrcRingStartOk(ringStart)) { return IG_GTLRC_WRITE_BAD_START; }
    if (ringHead >= ringBytes) { return IG_GTLRC_WRITE_HEAD_OUT_OF_RING; }
    if (ringTail >= ringBytes) { return IG_GTLRC_WRITE_TAIL_OUT_OF_RING; }
    if ((ringTail % IG_GT_TAIL_ALIGN_POLICY_8) != 0u) {
        return IG_GTLRC_WRITE_TAIL_UNALIGNED;
    }

    const IGU32 ringCtl = igGtcRingBufferControl(ringBytes);
    if (ringCtl == 0u) { return IG_GTLRC_WRITE_BAD_RING; }

    // Validation is complete. The whole first block is now owned by this writer,
    // so clear every value slot before installing the five non-zero/variable ones.
    for (IGU32 i = 0u; i < IG_GTLRC_FIRST_BLOCK_DWORDS_I915_28; ++i) {
        dwords[i] = 0u;
    }
    dwords[IG_GTLRC_FIRST_NOP_SLOT_I915_0] = igGtMiNoop();
    dwords[IG_GTLRC_FIRST_LRI_SLOT_I915_1] = igGtLrcFirstBlockLriHeader();

    for (IGU32 pair = 0u; pair < IG_GTLRC_FIRST_PAIR_COUNT_I915_13; ++pair) {
        const IGU32 slot = IG_GTLRC_FIRST_PAIR_SLOT_I915_2 + pair * 2u;
        dwords[slot] = base + igGtLrcFirstBlockRegOffset(pair);
        dwords[slot + 1u] = 0u;
    }

    dwords[IG_GTLRC_CONTEXT_CONTROL_VALUE_SLOT_I915_0x03] =
        igGtLrcContextControl(inhibitRestore);
    dwords[IG_GTLRC_RING_HEAD_VALUE_SLOT_I915_0x05] = ringHead;
    dwords[IG_GTLRC_RING_TAIL_VALUE_SLOT_I915_0x07] = ringTail;
    dwords[IG_GTLRC_RING_START_VALUE_SLOT_I915_0x09] = (IGU32)ringStart;
    dwords[IG_GTLRC_RING_CTL_VALUE_SLOT_I915_0x0B] = ringCtl;

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GTLRC_FIRST_BLOCK_DWORDS_I915_28;
    }
    return IG_GTLRC_WRITE_OK;
}

// Submission-ready BCS0 descriptor for Tiger Lake. The lower dword and SW
// context id are composed by IntelGtExec.hpp; BCS0 contributes class=1,
// instance=0 in the upper identity fields.
static inline IGU64 igGtLrcBcs0Descriptor(IGU64 lrca,
                                          IGU32 contextId,
                                          IGU32 addressMode,
                                          bool forceRestore)
{
    IGU64 word = igGtxRenderContextDescriptor(lrca, contextId,
                                              addressMode, forceRestore);
    if (word == IG_GTX_CTXDESC_INVALID_POLICY_0) {
        return IG_GTX_CTXDESC_INVALID_POLICY_0;
    }
    word |= (IGU64)IG_GTLRC_BCS_CLASS_I915_1 <<
            IG_GTLRC_ENGINE_CLASS_SHIFT_I915_61;
    word |= (IGU64)IG_GTLRC_BCS0_INSTANCE_I915_0 <<
            IG_GTLRC_ENGINE_INSTANCE_SHIFT_I915_48;
    return word;
}

#endif // IntelGtLrc_hpp
