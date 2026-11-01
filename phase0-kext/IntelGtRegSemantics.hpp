// SPDX-License-Identifier: MIT
//
//  IntelGtRegSemantics.hpp
//  Per-register write/readback semantics for every MMIO location the opt-in
//  Tiger Lake GT bring-up (IntelGtBringup.cpp) writes: forcewake, the GGTT
//  PTE window, BCS0 ring/context registers and the execlist submit queue.
//
//  WHY THIS TABLE EXISTS
//  ---------------------
//  intel-handoff45.md "Still open" item 3: a doorbell or write-only register
//  cannot be verified by demanding that its readback equal the write. One
//  generic write-then-compare applied to every register either refuses
//  correct writes (a masked register does not read back its enable half) or
//  accepts writes that prove nothing (the execlist submit queue keeps its
//  contents after submission, so a matching readback is equally consistent
//  with a load that never happened). Each row names the ONE verification
//  that is legitimate for that location and cites the vendored i915 line
//  that establishes it.
//
//  WHAT THIS HEADER DOES AND DOES NOT DO
//  -------------------------------------
//  Constants and one constant table. No functions, no MMIO, no call sites:
//  nothing includes it yet and it is not wired to any write path.
//  IntelGtProbe.cpp and its write bans are untouched. Every value and
//  citation comes from the vendored Linux v6.12 i915 sources only:
//  reference/i915/intel_gt_regs.h, reference/i915/intel_engine_regs.h,
//  reference/i915/intel_lrc_reg.h, reference/i915/i915_reg_defs.h,
//  reference/i915/i915_reg.h, reference/i915/intel_uncore.c,
//  reference/i915/intel_engine_cs.c, reference/i915/intel_ggtt.c and
//  reference/i915/intel_execlists_submission.c. Row citations are
//  "file:line" or "file:first-last", relative to that directory.
//
//  SELF-CLEARING HAS NO ROWS, deliberately
//  ---------------------------------------
//  The kind exists because a bit that clears itself makes READBACK_EQUAL
//  refuse a correct write. No vendored line establishes that any register
//  in this set self-clears. EL_CTRL_LOAD in particular is written
//  (intel_execlists_submission.c:941) and no vendored line reads it back,
//  so whether it clears is UNKNOWN here; DOORBELL + INDIRECT_ONLY is the
//  classification that does not depend on the answer.
//
//  DELIBERATELY NOT ROWS
//  ---------------------
//  - FORCEWAKE_ACK_RENDER_GEN9 / FORCEWAKE_ACK_GT_GEN9 are only polled;
//    they appear as the ackOffset of the two request rows.
//  - GFX_FLSH_CNTL_GEN6 (intel_gt_regs.h:1478): gen8_ggtt_invalidate()
//    writes it only when needs_wc_ggtt_mapping() (intel_ggtt.c:223-225),
//    which is false from graphics version 11 (intel_ggtt.c:207-210). The
//    bring-up does not write it.
//  - RING_ELSP (intel_engine_regs.h:178): the bring-up never writes it; it
//    submits through the ctrl_reg branch of write_desc()
//    (intel_execlists_submission.c:728-730) and the control doorbell.
//
#ifndef IntelGtRegSemantics_hpp
#define IntelGtRegSemantics_hpp

#include "IntelGtCmd.hpp"

// i915_reg_defs.h:194 __MASKED_FIELD(mask, value) is ((mask) << 16 | (value)).
#define IG_GTRS_MASK_SHIFT_I915_16            16u
// i915_reg.h:311 BLT_RING_BASE. Engine-relative rows add this for BCS0.
#define IG_GTRS_BCS0_BASE_I915_0x22000        0x22000u

// Where a row's offset is measured from. Zero is the uninitialised sentinel.
#define IG_GTRS_BASE_INVALID                  0u
#define IG_GTRS_BASE_ABSOLUTE                 1u  // GT MMIO offset as-is
#define IG_GTRS_BASE_ENGINE                   2u  // add the engine mmio base
#define IG_GTRS_BASE_GGTT_PTE                 3u  // one 64-bit GGTT PTE entry
#define IG_GTRS_BASE_LIMIT                    4u

// What a store to the location means.
#define IG_GTRS_KIND_INVALID                  0u
#define IG_GTRS_KIND_PLAIN                    1u  // whole value is stored state
#define IG_GTRS_KIND_MASKED                   2u  // upper 16 bits enable lower 16
#define IG_GTRS_KIND_WRITE_ONLY               3u  // staged; never read upstream
#define IG_GTRS_KIND_DOORBELL                 4u  // the store is the trigger
#define IG_GTRS_KIND_SELF_CLEARING            5u  // no rows; see top comment
#define IG_GTRS_KIND_LIMIT                    6u

// The only verification a row permits.
//  READBACK_EQUAL  read the same location at the same width; compare all bits.
//  MASKED_FIELD    sel = write >> 16. Compare (read & sel & ~hwOwnedMask) with
//                  (write & sel & ~hwOwnedMask). Never compare the upper half
//                  of a readback, or any bit outside sel: it was not written.
//  ACK_REGISTER    never read the request register back; poll ackOffset
//                  (absolute) for the requested bit (IntelGtForcewake.hpp).
//  INDIRECT_ONLY   a readback is NOT evidence; only an effect elsewhere
//                  (execlist status, CSB entries) shows the write landed.
#define IG_GTRS_VERIFY_INVALID                0u
#define IG_GTRS_VERIFY_READBACK_EQUAL         1u
#define IG_GTRS_VERIFY_MASKED_FIELD           2u
#define IG_GTRS_VERIFY_ACK_REGISTER           3u
#define IG_GTRS_VERIFY_INDIRECT_ONLY          4u
#define IG_GTRS_VERIFY_LIMIT                  5u

#define IG_GTRS_ROW_COUNT                     12u

struct IGGtRegSemantics {
    const char *i915Name;    // vendored symbol named at defineCite
    IGU32 base;              // IG_GTRS_BASE_*
    IGU32 offset;            // absolute, engine-relative, or 0 for a PTE
    IGU32 widthBits;         // 32 or 64
    IGU32 kind;              // IG_GTRS_KIND_*
    IGU32 verify;            // IG_GTRS_VERIFY_*
    IGU32 hwOwnedMask;       // low-half bits the hardware updates; never compared
    IGU32 ackOffset;         // absolute ack register for ACK_REGISTER, else 0
    const char *defineCite;  // where the offset (or the PTE store) is defined
    const char *kindCite;    // the upstream store that shows the kind
    const char *verifyCite;  // the line that justifies the verification
    const char *hwOwnedCite; // where hwOwnedMask comes from, or ""
};

// Ordered as the bring-up stages them: wake, map, engine init, submit.
// Consistent with IntelGtBringup.cpp today: it compares a readback only for
// RING_HWS_PGA and the PTEs, waits on masked fields of RING_MODE_GEN7 and
// RING_MI_MODE, polls the ack registers for forcewake, and never reads back
// the submit queue or the execlist control doorbell.
static const IGGtRegSemantics igGtrsTable[IG_GTRS_ROW_COUNT] = {
    // Wake. fw_set()/fw_clear() store _MASKED_BIT_ENABLE/_DISABLE into
    // reg_set; acquisition is judged on the ack register, never the request.
    { "FORCEWAKE_RENDER_GEN9", IG_GTRS_BASE_ABSOLUTE, 0xA278u, 32u,
      IG_GTRS_KIND_MASKED, IG_GTRS_VERIFY_ACK_REGISTER, 0x0u, 0xD84u,
      "intel_gt_regs.h:909", "intel_uncore.c:125-126",
      "intel_gt_regs.h:59", "" },
    { "FORCEWAKE_GT_GEN9", IG_GTRS_BASE_ABSOLUTE, 0xA188u, 32u,
      IG_GTRS_KIND_MASKED, IG_GTRS_VERIFY_ACK_REGISTER, 0x0u, 0x130044u,
      "intel_gt_regs.h:862", "intel_uncore.c:125-126",
      "intel_gt_regs.h:1500", "" },
    // Map. gen8_set_pte() is a whole-entry writeq; the mapping is uncached
    // from version 11 (intel_ggtt.c:207-210), which is what makes the
    // readback check named at :204-205 meaningful.
    { "gen8_set_pte", IG_GTRS_BASE_GGTT_PTE, 0x0u, 64u,
      IG_GTRS_KIND_PLAIN, IG_GTRS_VERIFY_READBACK_EQUAL, 0x0u, 0x0u,
      "intel_ggtt.c:430", "intel_ggtt.c:430-433",
      "intel_ggtt.c:204-205", "" },
    // Engine init, BCS0-relative. RING_HWS_PGA is an unmasked store followed
    // by a posting read; upstream never compares, so equality is the reading
    // of a plain register rather than an upstream assertion, and a mismatch
    // is a refusal rather than a diagnosis.
    { "RING_HWS_PGA", IG_GTRS_BASE_ENGINE, 0x80u, 32u,
      IG_GTRS_KIND_PLAIN, IG_GTRS_VERIFY_READBACK_EQUAL, 0x0u, 0x0u,
      "intel_engine_regs.h:66", "intel_execlists_submission.c:2947-2949",
      "intel_execlists_submission.c:2950", "" },
    // The CSB write pointer (GEN11_CSB_WRITE_PTR_MASK) is advanced by the
    // hardware: after a reset it starts writing CSB entry [0]
    // (intel_execlists_submission.c:2823-2830). Only the read pointer is ours.
    { "RING_CONTEXT_STATUS_PTR", IG_GTRS_BASE_ENGINE, 0x3A0u, 32u,
      IG_GTRS_KIND_MASKED, IG_GTRS_VERIFY_MASKED_FIELD, 0xFu, 0x0u,
      "intel_engine_regs.h:209", "intel_execlists_submission.c:2818-2819",
      "i915_reg_defs.h:194", "intel_lrc_reg.h:72" },
    // Upstream sets GEN11_GFX_DISABLE_LEGACY_MODE as a masked bit.
    { "RING_MODE_GEN7", IG_GTRS_BASE_ENGINE, 0x29Cu, 32u,
      IG_GTRS_KIND_MASKED, IG_GTRS_VERIFY_MASKED_FIELD, 0x0u, 0x0u,
      "intel_engine_regs.h:192", "intel_execlists_submission.c:2940-2943",
      "i915_reg_defs.h:194", "" },
    // MODE_IDLE (bit 9) is engine status, set by the hardware after
    // STOP_RING and read at intel_engine_cs.c:1859.
    { "RING_MI_MODE", IG_GTRS_BASE_ENGINE, 0x9Cu, 32u,
      IG_GTRS_KIND_MASKED, IG_GTRS_VERIFY_MASKED_FIELD, 0x200u, 0x0u,
      "intel_engine_regs.h:73", "intel_execlists_submission.c:2945",
      "i915_reg_defs.h:194", "intel_engine_regs.h:77" },
    // Submit. write_desc() stores port n at submit_reg + n * 2 dwords, low
    // dword then high (:729-730), and the queue is not cleared after it is
    // submitted (:926-927), so its readback cannot show that a load happened.
    { "RING_EXECLIST_SQ_CONTENTS", IG_GTRS_BASE_ENGINE, 0x510u, 32u,
      IG_GTRS_KIND_WRITE_ONLY, IG_GTRS_VERIFY_INDIRECT_ONLY, 0x0u, 0x0u,
      "intel_engine_regs.h:232", "intel_execlists_submission.c:729",
      "intel_execlists_submission.c:926-927", "" },
    { "RING_EXECLIST_SQ_CONTENTS", IG_GTRS_BASE_ENGINE, 0x514u, 32u,
      IG_GTRS_KIND_WRITE_ONLY, IG_GTRS_VERIFY_INDIRECT_ONLY, 0x0u, 0x0u,
      "intel_engine_regs.h:232", "intel_execlists_submission.c:730",
      "intel_execlists_submission.c:926-927", "" },
    { "RING_EXECLIST_SQ_CONTENTS", IG_GTRS_BASE_ENGINE, 0x518u, 32u,
      IG_GTRS_KIND_WRITE_ONLY, IG_GTRS_VERIFY_INDIRECT_ONLY, 0x0u, 0x0u,
      "intel_engine_regs.h:232", "intel_execlists_submission.c:729",
      "intel_execlists_submission.c:926-927", "" },
    { "RING_EXECLIST_SQ_CONTENTS", IG_GTRS_BASE_ENGINE, 0x51Cu, 32u,
      IG_GTRS_KIND_WRITE_ONLY, IG_GTRS_VERIFY_INDIRECT_ONLY, 0x0u, 0x0u,
      "intel_engine_regs.h:232", "intel_execlists_submission.c:730",
      "intel_execlists_submission.c:926-927", "" },
    // EL_CTRL_LOAD is the trigger; the evidence is the CSB update the reset
    // path checks for (:2835).
    { "RING_EXECLIST_CONTROL", IG_GTRS_BASE_ENGINE, 0x550u, 32u,
      IG_GTRS_KIND_DOORBELL, IG_GTRS_VERIFY_INDIRECT_ONLY, 0x0u, 0x0u,
      "intel_engine_regs.h:234", "intel_execlists_submission.c:941",
      "intel_execlists_submission.c:2835", "" },
};

#endif // IntelGtRegSemantics_hpp
