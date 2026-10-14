// SPDX-License-Identifier: MIT
// Pure Tiger Lake / Gen12 context-status-buffer decode and retirement policy.
#ifndef IntelGtCsb_hpp
#define IntelGtCsb_hpp

#include "IntelGtCmd.hpp"

// Source-backed by reference/i915/intel_lrc_reg.h and
// reference/i915/intel_execlists_submission.c (Linux v6.12 vendored in-tree).
#define IG_GTCSB_ENTRIES_I915_12                 12u
#define IG_GTCSB_PTR_MASK_I915_0xF               0xFu
#define IG_GTCSB_READ_PTR_SHIFT_I915_8            8u
#define IG_GTCSB_STATUS0_OFFSET_I915_0x370        0x370u
#define IG_GTCSB_STATUS6_OFFSET_I915_0x3C0        0x3C0u
#define IG_GTCSB_ENTRY_BYTES_I915_8               8u
#define IG_GTCSB_SPLIT_ENTRY_I915_6               6u
#define IG_GTCSB_CONTEXT_STATUS_PTR_I915_0x3A0    0x3A0u

#define IG_GTCSB_NEW_QUEUE_BIT_I915_0x1           0x1u
#define IG_GTCSB_CTX_ID_SHIFT_I915_15             15u
#define IG_GTCSB_CTX_ID_MASK_I915_0x7FF           0x7FFu
#define IG_GTCSB_IDLE_CTX_ID_I915_0x7FF           0x7FFu
#define IG_GTCSB_SWITCH_DETAIL_MASK_I915_0xF      0xFu
#define IG_GTCSB_SWITCH_DETAIL_COMPLETE_I915_0    0u
#define IG_GTCSB_SWITCH_DETAIL_PREEMPTED_I915_5   5u
#define IG_GTCSB_EXPECTED_ID_MAX_POLICY_0x7FE     0x7FEu

// Parse verdicts. Zero remains the caller's uninitialised sentinel.
#define IG_GTCSB_PARSE_NOT_ATTEMPTED              0u
#define IG_GTCSB_PARSE_OK                         1u
#define IG_GTCSB_PARSE_NULL_OUT                   2u
#define IG_GTCSB_PARSE_BAD_EXPECTED_ID            3u
#define IG_GTCSB_PARSE_UNAVAILABLE                4u
#define IG_GTCSB_PARSE_BAD_DETAIL                 5u
#define IG_GTCSB_PARSE_UNEXPECTED_TO              6u
#define IG_GTCSB_PARSE_UNEXPECTED_AWAY            7u
#define IG_GTCSB_PARSE_LIMIT                      8u

struct IGGtCsbEvent {
    IGU32 toContextId;
    IGU32 awayContextId;
    IGU32 switchDetail;
    bool newQueue;
};

static inline bool igGtCsbParseSucceeded(IGU32 verdict)
{
    return verdict == IG_GTCSB_PARSE_OK;
}

static inline const char *igGtCsbParseVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTCSB_PARSE_NOT_ATTEMPTED:   return "not attempted";
        case IG_GTCSB_PARSE_OK:              return "ok";
        case IG_GTCSB_PARSE_NULL_OUT:        return "null event output";
        case IG_GTCSB_PARSE_BAD_EXPECTED_ID: return "invalid expected context id";
        case IG_GTCSB_PARSE_UNAVAILABLE:     return "unavailable CSB entry";
        case IG_GTCSB_PARSE_BAD_DETAIL:      return "unsupported switch detail";
        case IG_GTCSB_PARSE_UNEXPECTED_TO:   return "unexpected switched-to context";
        case IG_GTCSB_PARSE_UNEXPECTED_AWAY: return "unexpected switched-away context";
        default:                             return "unknown CSB parse verdict";
    }
}

static inline bool igGtCsbExpectedContextIdOk(IGU32 contextId)
{
    // This bringup allocator uses 1..2046. Zero is deliberately not accepted so
    // an uninitialised fContextId cannot accidentally validate an event.
    return contextId >= 1u && contextId <= IG_GTCSB_EXPECTED_ID_MAX_POLICY_0x7FE;
}

static inline IGU32 igGtCsbContextId(IGU32 dword)
{
    return (dword >> IG_GTCSB_CTX_ID_SHIFT_I915_15) & IG_GTCSB_CTX_ID_MASK_I915_0x7FF;
}

static inline bool igGtCsbContextIdIsIdle(IGU32 contextId)
{
    return contextId == IG_GTCSB_IDLE_CTX_ID_I915_0x7FF;
}

static inline IGU32 igGtCsbWritePointer(IGU32 pointerWord)
{
    return pointerWord & IG_GTCSB_PTR_MASK_I915_0xF;
}

static inline bool igGtCsbWritePointerOk(IGU32 pointerWord)
{
    return igGtCsbWritePointer(pointerWord) < IG_GTCSB_ENTRIES_I915_12;
}

static inline IGU32 igGtCsbNextSlot(IGU32 slot)
{
    if (slot >= IG_GTCSB_ENTRIES_I915_12) return IG_GTCSB_ENTRIES_I915_12;
    return (slot + 1u) % IG_GTCSB_ENTRIES_I915_12;
}

// MMIO byte offset of the low dword for a Gen11+ 12-entry CSB slot, relative
// to the engine base. Entries 0..5 live at 0x370; 6..11 continue at 0x3c0.
static inline IGU32 igGtCsbSlotOffset(IGU32 slot)
{
    if (slot >= IG_GTCSB_ENTRIES_I915_12) return 0xFFFFFFFFu;
    if (slot < IG_GTCSB_SPLIT_ENTRY_I915_6) {
        return IG_GTCSB_STATUS0_OFFSET_I915_0x370 + slot * IG_GTCSB_ENTRY_BYTES_I915_8;
    }
    return IG_GTCSB_STATUS6_OFFSET_I915_0x3C0 +
           (slot - IG_GTCSB_SPLIT_ENTRY_I915_6) * IG_GTCSB_ENTRY_BYTES_I915_8;
}

// RING_CONTEXT_STATUS_PTR is a masked register. GEN11_CSB_READ_PTR_MASK is
// 0xF << 8; Intel's _MASKED_FIELD spelling places that mask in bits 16..31 and
// the chosen read pointer in bits 8..11. This exactly reproduces 0x0F000000 |
// (slot << 8) without leaving a magic word in the IOKit shell.
static inline IGU32 igGtCsbReadPointerAckWord(IGU32 slot)
{
    if (slot >= IG_GTCSB_ENTRIES_I915_12) return 0u;
    const IGU32 mask = IG_GTCSB_PTR_MASK_I915_0xF << IG_GTCSB_READ_PTR_SHIFT_I915_8;
    const IGU32 value = slot << IG_GTCSB_READ_PTR_SHIFT_I915_8;
    return (mask << 16) | value;
}

// Linux reset_csb_pointers() seeds both read and write pointers to the last
// Gen11+ entry and masks both fields with 0xFFFF in the upper half. Keeping the
// reset word here removes another raw scheduler constant from the IOKit shell.
static inline IGU32 igGtCsbResetPointerWord(void)
{
    const IGU32 reset = IG_GTCSB_ENTRIES_I915_12 - 1u;
    return 0xFFFF0000u |
           (reset << IG_GTCSB_READ_PTR_SHIFT_I915_8) |
           reset;
}

// Decode one stable Tiger Lake CSB qword. Refusals are atomic: `out` is not
// touched unless the entire event belongs to the currently submitted context
// (or to the hardware's idle context) and the switch-detail value is one of the
// TGL values documented by the vendored i915 source (0..5).
static inline IGU32 igGtCsbParseTgl(IGU32 lower,
                                    IGU32 upper,
                                    IGU32 expectedContextId,
                                    IGGtCsbEvent *out)
{
    if (out == 0) return IG_GTCSB_PARSE_NULL_OUT;
    if (!igGtCsbExpectedContextIdOk(expectedContextId)) {
        return IG_GTCSB_PARSE_BAD_EXPECTED_ID;
    }
    // An all-ones MMIO read is the IOKit shell's unavailable sentinel. Refuse
    // either half, not just the pair, so a failed high-dword read cannot look
    // like an idle context paired with a valid low dword.
    if (lower == 0xFFFFFFFFu || upper == 0xFFFFFFFFu) {
        return IG_GTCSB_PARSE_UNAVAILABLE;
    }

    const IGU32 detail = upper & IG_GTCSB_SWITCH_DETAIL_MASK_I915_0xF;
    if (detail > IG_GTCSB_SWITCH_DETAIL_PREEMPTED_I915_5) {
        return IG_GTCSB_PARSE_BAD_DETAIL;
    }

    const IGU32 toId = igGtCsbContextId(lower);
    const IGU32 awayId = igGtCsbContextId(upper);
    if (toId != expectedContextId && !igGtCsbContextIdIsIdle(toId)) {
        return IG_GTCSB_PARSE_UNEXPECTED_TO;
    }
    if (awayId != expectedContextId && !igGtCsbContextIdIsIdle(awayId)) {
        return IG_GTCSB_PARSE_UNEXPECTED_AWAY;
    }

    IGGtCsbEvent event;
    event.toContextId = toId;
    event.awayContextId = awayId;
    event.switchDetail = detail;
    event.newQueue = (lower & IG_GTCSB_NEW_QUEUE_BIT_I915_0x1) != 0u;
    *out = event;
    return IG_GTCSB_PARSE_OK;
}

// The synchronous BCS0 queue is retired only after the submitted context was
// switched AWAY, the engine switched TO idle, this was not a new-queue event,
// and the switch detail says context complete. A fence by itself is not enough
// because the context image may still be live in hardware.
static inline bool igGtCsbEventRetiresContext(const IGGtCsbEvent &event,
                                              IGU32 expectedContextId)
{
    if (!igGtCsbExpectedContextIdOk(expectedContextId)) return false;
    return event.awayContextId == expectedContextId &&
           event.toContextId == IG_GTCSB_IDLE_CTX_ID_I915_0x7FF &&
           !event.newQueue &&
           event.switchDetail == IG_GTCSB_SWITCH_DETAIL_COMPLETE_I915_0;
}

#endif // IntelGtCsb_hpp
