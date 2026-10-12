//
//  IntelGtForcewake.hpp
//  Gen12 (Tiger Lake) FORCEWAKE composition -- the first layer in this tree
//  whose output is meant to be WRITTEN to the GT, rather than composed in host
//  memory and published.
//
//  WHY THIS LAYER COMES FIRST
//  --------------------------
//  Every GT header beneath this one (Cmd, Batch, Blt, Image, Context, Ring,
//  Exec, Fence, Reset, Probe) composes bytes an engine would consume. None of
//  them can be delivered, and the reason is not the execlist port or the GGTT:
//  it is that the render and GT power wells are allowed to be ASLEEP when the
//  CPU touches them. A read of a register inside a sleeping well returns zero
//  or garbage and a write is dropped, silently, in both directions. i915 never
//  touches a GT register outside forcewake (intel_uncore.c, the whole
//  __gen12_fw_ranges table at line 1337 exists to answer "which well must be
//  held for this offset"), and before 2026-10-12 this tree had zero references
//  to forcewake at all -- measured by grep, recorded in intel-handoff43.md.
//
//  So the order is forced: wake the well, THEN map memory, THEN submit. A
//  submission written to a sleeping ring is not a failed submission; it is a
//  write that never happened, followed by a fence wait that times out and
//  blames the engine.
//
//  WHAT THIS HEADER DOES AND DOES NOT DO
//  -------------------------------------
//  It composes and JUDGES, like every sibling. It performs no MMIO:
//    - which well an absolute GT offset needs (igGtwDomainForOffset),
//    - which request / ack register belongs to a well,
//    - the masked-write values that take and drop the kernel's wake reference,
//    - a step-wise acquire sequence that is FED ack readings by the caller and
//      says what to read or write next, and a verdict when it is done.
//  The shell (IntelGtProbe.cpp) owns the reads, the writes and the delays.
//
//  WHAT IS DELIBERATELY NOT IMPLEMENTED
//  ------------------------------------
//  i915 acquires gen11+ wells through uncore_get_fallback (intel_uncore.c:2168)
//  -- fw_domains_get_with_fallback(), :313 -- which on a missed ack toggles
//  FORCEWAKE_KERNEL_FALLBACK up to 10 times (:226-238, WaRsForcewakeAddDelay-
//  ForAck) to kick the power-management state machine. This header implements
//  the NORMAL path only (fw_domains_get_normal, :294): wait ack clear, set,
//  wait ack set. A missed ack is reported as a refusal, and the caller does not
//  proceed. That is the conservative direction -- the cost of a missed ack here
//  is one boot that publishes "no-ack" instead of a blit, never a write into a
//  sleeping well -- and the fallback toggle can be added as its own verdict once
//  hardware has shown that the normal path misses.
//
//  THE 0xFFFFFFFF TRAP, which is why the ack judgement has an order
//  -----------------------------------------------------------------
//  intel_uncore.c:184 treats an ack that reads ~0 as "MMIO unreliable" and
//  wedges the GT. That case matters MORE here than in i915, for a reason the
//  i915 code does not have to state: all-ones has FORCEWAKE_KERNEL set. So in
//  the wait-for-SET phase, a dead BAR -- or a refused read from this tree's own
//  read guard, which answers all-ones by convention (handoff24) -- satisfies
//  the "ack is set" test on the first poll, and a naive judgement reports the
//  well as HELD. The driver would then write a ring into a well that is not
//  awake, from a BAR that is not answering. igGtwAckVerdict() therefore tests
//  all-ones BEFORE the bit, in both phases, and the suite drives that ordering
//  with exactly the doubly-satisfying input that separates the two orders.
//
//  Same rule as the siblings: DEPENDENCY-FREE. The one #include is the quoted
//  sibling IntelGtCmd.hpp, for IGU32 and the IG_GT_BASE_INVALID sentinel, so a
//  refused offset here is spelled the same way as a refused offset everywhere
//  else in the GT stack.
//
#ifndef IntelGtForcewake_hpp
#define IntelGtForcewake_hpp

#include "IntelGtCmd.hpp"

// ---------------------------------------------------------------------------
// Cited registers and bits -- reference/i915/intel_gt_regs.h (v6.12)
// ---------------------------------------------------------------------------
//
// tools/check-registers.py re-derives every one of these from the vendored
// text (intel_gt_forcewake_citation_checks). A value going wrong here fails
// that gate rather than compiling cleanly.
//
#define IG_GTW_REQ_RENDER_I915_0xA278       0xA278u    // intel_gt_regs.h:909 FORCEWAKE_RENDER_GEN9
#define IG_GTW_ACK_RENDER_I915_0xD84        0xD84u     // intel_gt_regs.h:59 FORCEWAKE_ACK_RENDER_GEN9
#define IG_GTW_REQ_GT_I915_0xA188           0xA188u    // intel_gt_regs.h:862 FORCEWAKE_GT_GEN9
#define IG_GTW_ACK_GT_I915_0x130044         0x130044u  // intel_gt_regs.h:1500 FORCEWAKE_ACK_GT_GEN9

#define IG_GTW_KERNEL_I915_BIT0             (1u << 0)  // intel_gt_regs.h:1501 FORCEWAKE_KERNEL
#define IG_GTW_KERNEL_FALLBACK_I915_BIT15   (1u << 15) // intel_gt_regs.h:1503 FORCEWAKE_KERNEL_FALLBACK

// fw_domain_reset() on graphics version >= 12 clears 0xefff rather than 0xffff
// (intel_uncore.c:137-140, WaRsClearFWBitsAtReset). Composed by
// igGtwResetValue(); the shell does not issue it today, because a reset of a
// well another agent (firmware, the GuC) may hold is not this driver's call to
// make before it has observed the ack register at least once.
#define IG_GTW_RESET_BITS_GEN12_I915_0xEFFF 0xEFFFu    // intel_uncore.c:138

// intel_uncore.c:37 FORCEWAKE_ACK_TIMEOUT_MS is 50. This tree counts polls,
// not milliseconds (IntelGtReset.hpp's convention), and the shell delays 1 ms
// per poll -- so the budget below is the same 50 ms, spelled as a count. It is
// a POLICY because the 1 ms step is this project's, not i915's.
#define IG_GTW_ACK_TIMEOUT_MS_I915_50       50u        // intel_uncore.c:37
#define IG_GTW_ACK_POLL_BUDGET_POLICY_50    50u

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------
#define IG_GTW_DOMAIN_NONE          0u  // always-on or reserved: no wake needed
#define IG_GTW_DOMAIN_RENDER        1u  // FORCEWAKE_RENDER
#define IG_GTW_DOMAIN_GT            2u  // FORCEWAKE_GT
#define IG_GTW_DOMAIN_MEDIA         3u  // any VDBOX/VEBOX well -- REFUSED here
#define IG_GTW_DOMAIN_OUT_OF_TABLE  4u  // offset beyond the gen12 table
#define IG_GTW_DOMAIN_LIMIT         5u

static inline const char *igGtwDomainName(IGU32 domain)
{
    switch (domain) {
        case IG_GTW_DOMAIN_NONE:         return "none";
        case IG_GTW_DOMAIN_RENDER:       return "render";
        case IG_GTW_DOMAIN_GT:           return "gt";
        case IG_GTW_DOMAIN_MEDIA:        return "media";
        case IG_GTW_DOMAIN_OUT_OF_TABLE: return "out-of-table";
        default:                         return "unknown";
    }
}

// Only the two wells this driver knows how to hold. MEDIA is refused because
// its request/ack pairs are per-instance (FORCEWAKE_MEDIA_VDBOX_GEN11(n),
// intel_gt_regs.h:920) and this tree has no use for a video engine yet;
// refusing is cheaper than citing a family nothing exercises.
static inline bool igGtwDomainHoldable(IGU32 domain)
{
    return domain == IG_GTW_DOMAIN_RENDER || domain == IG_GTW_DOMAIN_GT;
}

static inline IGU32 igGtwReqOffset(IGU32 domain)
{
    switch (domain) {
        case IG_GTW_DOMAIN_RENDER: return IG_GTW_REQ_RENDER_I915_0xA278;
        case IG_GTW_DOMAIN_GT:     return IG_GTW_REQ_GT_I915_0xA188;
        default:                   return IG_GT_BASE_INVALID;
    }
}

static inline IGU32 igGtwAckOffset(IGU32 domain)
{
    switch (domain) {
        case IG_GTW_DOMAIN_RENDER: return IG_GTW_ACK_RENDER_I915_0xD84;
        case IG_GTW_DOMAIN_GT:     return IG_GTW_ACK_GT_I915_0x130044;
        default:                   return IG_GT_BASE_INVALID;
    }
}

// ---------------------------------------------------------------------------
// The gen12 range table -- intel_uncore.c:1337-1436 __gen12_fw_ranges[]
// ---------------------------------------------------------------------------
//
// Transcribed row for row, in upstream order, and re-parsed against the
// vendored file by tools/check-registers.py: every row's start, end and domain
// must match, and the row COUNT must match, so a dropped or merged row fails
// rather than silently widening a neighbour. Upstream's domain names map as:
// 0 -> NONE, FORCEWAKE_RENDER -> RENDER, FORCEWAKE_GT -> GT, every
// FORCEWAKE_MEDIA_* -> MEDIA. i915 selects this table for Tiger Lake at
// intel_uncore.c:2433.
//
struct IGGtwRange {
    IGU32 start;
    IGU32 end;      // inclusive, as upstream
    IGU32 domain;
};

#define IG_GTW_ROW(s, e, d) { (s), (e), (d) }

static const IGGtwRange kIgGtwGen12Ranges[] = {
    IG_GTW_ROW(0x0u,      0x1FFFu,   IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x2000u,   0x26FFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x2700u,   0x27FFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x2800u,   0x2AFFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x2B00u,   0x2FFFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x3000u,   0x3FFFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x4000u,   0x51FFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x5200u,   0x7FFFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x8000u,   0x813Fu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x8140u,   0x815Fu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x8160u,   0x81FFu,   IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x8200u,   0x82FFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x8300u,   0x84FFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x8500u,   0x94CFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x94D0u,   0x955Fu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x9560u,   0x97FFu,   IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x9800u,   0xAFFFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0xB000u,   0xB3FFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0xB400u,   0xCFFFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0xD000u,   0xD7FFu,   IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0xD800u,   0xD8FFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0xD900u,   0xDBFFu,   IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0xDC00u,   0xEFFFu,   IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0xF000u,   0x147FFu,  IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x14800u,  0x1FFFFu,  IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x20000u,  0x20FFFu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x21000u,  0x21FFFu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x22000u,  0x23FFFu,  IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x24000u,  0x2417Fu,  IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x24180u,  0x249FFu,  IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x24A00u,  0x251FFu,  IG_GTW_DOMAIN_RENDER),
    IG_GTW_ROW(0x25200u,  0x255FFu,  IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x25600u,  0x2567Fu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x25680u,  0x259FFu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x25A00u,  0x25A7Fu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x25A80u,  0x2FFFFu,  IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x30000u,  0x3FFFFu,  IG_GTW_DOMAIN_GT),
    IG_GTW_ROW(0x40000u,  0x1BFFFFu, IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x1C0000u, 0x1C3FFFu, IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x1C4000u, 0x1C7FFFu, IG_GTW_DOMAIN_NONE),
    IG_GTW_ROW(0x1C8000u, 0x1CBFFFu, IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x1CC000u, 0x1CFFFFu, IG_GTW_DOMAIN_MEDIA),
    IG_GTW_ROW(0x1D0000u, 0x1D3FFFu, IG_GTW_DOMAIN_MEDIA),
};

#define IG_GTW_GEN12_ROW_COUNT \
    (sizeof(kIgGtwGen12Ranges) / sizeof(kIgGtwGen12Ranges[0]))

// Which well an absolute GT MMIO offset needs. A linear walk -- 43 rows, and the
// caller asks once per register it is about to touch, not per access. An offset
// past the last row answers OUT_OF_TABLE rather than NONE: upstream's table is
// documented as watertight (intel_uncore.c:1201-1203) over the range it
// covers, and reading "not listed" as "needs nothing" is the exact shape of a
// silent write into a sleeping well.
static inline IGU32 igGtwDomainForOffset(IGU32 offset)
{
    for (unsigned i = 0; i < IG_GTW_GEN12_ROW_COUNT; i++) {
        if (offset >= kIgGtwGen12Ranges[i].start
            && offset <= kIgGtwGen12Ranges[i].end) {
            return kIgGtwGen12Ranges[i].domain;
        }
    }
    return IG_GTW_DOMAIN_OUT_OF_TABLE;
}

// ---------------------------------------------------------------------------
// Masked writes -- i915_reg_defs.h:194 __MASKED_FIELD, :204/:205
// ---------------------------------------------------------------------------
//
// A forcewake request register is a MASKED register: the high 16 bits say which
// low bits this write is allowed to change. Writing a bare 0x1 changes nothing
// (mask 0); writing 0x0 to release changes nothing either. Both are silent
// no-ops on the hardware, which is why the two composers below exist and why
// the suite pins their exact words.
//
static inline IGU32 igGtwMaskedEnable(IGU32 bits)
{
    bits &= 0xFFFFu;
    return (bits << 16) | bits;
}

static inline IGU32 igGtwMaskedDisable(IGU32 bits)
{
    bits &= 0xFFFFu;
    return bits << 16;
}

// fw_set(d, FORCEWAKE_KERNEL), intel_uncore.c:125 / :263.
static inline IGU32 igGtwAcquireValue(void)
{
    return igGtwMaskedEnable(IG_GTW_KERNEL_I915_BIT0);
}

// fw_clear(d, FORCEWAKE_KERNEL), intel_uncore.c:126 / :290.
static inline IGU32 igGtwReleaseValue(void)
{
    return igGtwMaskedDisable(IG_GTW_KERNEL_I915_BIT0);
}

// fw_clear(d, 0xefff), intel_uncore.c:138. Composed, not issued (see above).
static inline IGU32 igGtwResetValue(void)
{
    return igGtwMaskedDisable(IG_GTW_RESET_BITS_GEN12_I915_0xEFFF);
}

// ---------------------------------------------------------------------------
// One ack reading, judged
// ---------------------------------------------------------------------------
#define IG_GTW_ACK_NOT_ATTEMPTED    0u  // caller's initialiser, never returned
#define IG_GTW_ACK_OK               1u  // the kernel bit reads as wanted
#define IG_GTW_ACK_PENDING          2u  // not yet, budget remains
#define IG_GTW_ACK_TIMED_OUT        3u  // budget spent, bit never reached wanted
#define IG_GTW_ACK_MMIO_DEAD        4u  // read all-ones: BAR or guard refused
#define IG_GTW_ACK_BAD_BUDGET       5u  // caller passed zero
#define IG_GTW_ACK_LIMIT            6u

static inline const char *igGtwAckVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTW_ACK_NOT_ATTEMPTED: return "not-attempted";
        case IG_GTW_ACK_OK:            return "ok";
        case IG_GTW_ACK_PENDING:       return "pending";
        case IG_GTW_ACK_TIMED_OUT:     return "timed-out";
        case IG_GTW_ACK_MMIO_DEAD:     return "mmio-dead";
        case IG_GTW_ACK_BAD_BUDGET:    return "bad-budget";
        default:                       return "unknown";
    }
}

// ORDER: budget (caller bug) -> all-ones (dead BAR) -> bit -> timeout.
// All-ones before the bit is the whole point -- see the header comment.
// pollsDone counts readings INCLUDING this one, so a budget of N admits
// exactly N readings and the Nth can still succeed (IntelGtReset.hpp's
// "work that landed on the last poll landed").
static inline IGU32 igGtwAckVerdict(IGU32 ack, bool wantSet,
                                    IGU32 pollsDone, IGU32 pollBudget)
{
    if (pollBudget == 0u) {
        return IG_GTW_ACK_BAD_BUDGET;
    }
    if (ack == 0xFFFFFFFFu) {
        return IG_GTW_ACK_MMIO_DEAD;
    }
    const bool isSet = (ack & IG_GTW_KERNEL_I915_BIT0) != 0u;
    if (isSet == wantSet) {
        return IG_GTW_ACK_OK;
    }
    if (pollsDone >= pollBudget) {
        return IG_GTW_ACK_TIMED_OUT;
    }
    return IG_GTW_ACK_PENDING;
}

// ---------------------------------------------------------------------------
// The acquire sequence -- fw_domains_get_normal(), intel_uncore.c:294-310
// ---------------------------------------------------------------------------
//
// One domain at a time. The caller loops:
//
//     igGtwBegin(&s, domain, budget);
//     while (!igGtwDone(&s)) {
//         if (igGtwWantsWrite(&s)) {
//             write32(igGtwWriteOffset(&s), igGtwWriteValue(&s));
//             igGtwWrote(&s);
//         } else {
//             igGtwFeedAck(&s, read32(igGtwReadOffset(&s)));
//             if (!igGtwDone(&s)) delay(1 ms);
//         }
//     }
//     if (igGtwHeld(s.verdict)) { ... use the well ...;
//                                 write32(igGtwReqOffset(d), igGtwReleaseValue()); }
//
// The machine never asks for a write before it has seen the ack CLEAR, and
// never reports HELD before it has seen the ack SET after its own write -- an
// ack that was already set when the sequence began proves nothing about this
// driver's request.
//
#define IG_GTW_PHASE_WAIT_CLEAR     0u
#define IG_GTW_PHASE_REQUEST        1u
#define IG_GTW_PHASE_WAIT_SET       2u
#define IG_GTW_PHASE_DONE           3u

#define IG_GTW_ACQ_NOT_ATTEMPTED    0u  // caller's initialiser, never returned
#define IG_GTW_ACQ_HELD             1u  // ack set after our request: well awake
#define IG_GTW_ACQ_NOT_NEEDED       2u  // domain NONE: nothing to hold
#define IG_GTW_ACQ_BAD_DOMAIN       3u  // MEDIA / OUT_OF_TABLE / unknown
#define IG_GTW_ACQ_BAD_BUDGET       4u  // zero poll budget
#define IG_GTW_ACQ_MMIO_DEAD        5u  // an ack read answered all-ones
#define IG_GTW_ACQ_STALE_ACK        6u  // ack never cleared before our request
#define IG_GTW_ACQ_NO_ACK           7u  // ack never set after our request
#define IG_GTW_ACQ_LIMIT            8u

static inline const char *igGtwAcqVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTW_ACQ_NOT_ATTEMPTED: return "not-attempted";
        case IG_GTW_ACQ_HELD:          return "held";
        case IG_GTW_ACQ_NOT_NEEDED:    return "not-needed";
        case IG_GTW_ACQ_BAD_DOMAIN:    return "bad-domain";
        case IG_GTW_ACQ_BAD_BUDGET:    return "bad-budget";
        case IG_GTW_ACQ_MMIO_DEAD:     return "mmio-dead";
        case IG_GTW_ACQ_STALE_ACK:     return "stale-ack";
        case IG_GTW_ACQ_NO_ACK:        return "no-ack";
        default:                       return "unknown";
    }
}

// EQUALITY, one preimage. NOT_NEEDED is deliberately not "held": a caller that
// treats it as held would later write a release to IG_GT_BASE_INVALID.
static inline bool igGtwHeld(IGU32 verdict)
{
    return verdict == IG_GTW_ACQ_HELD;
}

// Whether the caller may touch registers in the domain it asked about.
static inline bool igGtwMayAccess(IGU32 verdict)
{
    return verdict == IG_GTW_ACQ_HELD || verdict == IG_GTW_ACQ_NOT_NEEDED;
}

struct IGGtwSeq {
    IGU32 domain;
    IGU32 phase;
    IGU32 polls;    // readings taken in the CURRENT wait phase
    IGU32 budget;
    IGU32 verdict;
    IGU32 lastAck;  // last raw ack reading, for publication
};

static inline void igGtwFinish(IGGtwSeq *s, IGU32 verdict)
{
    s->phase = IG_GTW_PHASE_DONE;
    s->verdict = verdict;
}

static inline void igGtwBegin(IGGtwSeq *s, IGU32 domain, IGU32 budget)
{
    s->domain = domain;
    s->phase = IG_GTW_PHASE_WAIT_CLEAR;
    s->polls = 0u;
    s->budget = budget;
    s->verdict = IG_GTW_ACQ_NOT_ATTEMPTED;
    s->lastAck = 0u;

    // Domain before budget: an unholdable domain means every offset this
    // sequence would use is IG_GT_BASE_INVALID, so nothing about the budget
    // is meaningful. NONE is not an error -- it is the always-on block.
    if (domain == IG_GTW_DOMAIN_NONE) {
        igGtwFinish(s, IG_GTW_ACQ_NOT_NEEDED);
        return;
    }
    if (!igGtwDomainHoldable(domain)) {
        igGtwFinish(s, IG_GTW_ACQ_BAD_DOMAIN);
        return;
    }
    if (budget == 0u) {
        igGtwFinish(s, IG_GTW_ACQ_BAD_BUDGET);
        return;
    }
}

static inline bool igGtwDone(const IGGtwSeq *s)
{
    return s->phase == IG_GTW_PHASE_DONE;
}

static inline bool igGtwWantsWrite(const IGGtwSeq *s)
{
    return s->phase == IG_GTW_PHASE_REQUEST;
}

static inline bool igGtwWantsRead(const IGGtwSeq *s)
{
    return s->phase == IG_GTW_PHASE_WAIT_CLEAR
           || s->phase == IG_GTW_PHASE_WAIT_SET;
}

static inline IGU32 igGtwReadOffset(const IGGtwSeq *s)
{
    return igGtwWantsRead(s) ? igGtwAckOffset(s->domain) : IG_GT_BASE_INVALID;
}

static inline IGU32 igGtwWriteOffset(const IGGtwSeq *s)
{
    return igGtwWantsWrite(s) ? igGtwReqOffset(s->domain) : IG_GT_BASE_INVALID;
}

static inline IGU32 igGtwWriteValue(const IGGtwSeq *s)
{
    return igGtwWantsWrite(s) ? igGtwAcquireValue() : 0u;
}

// The caller performed the write igGtwWriteOffset/Value asked for.
static inline void igGtwWrote(IGGtwSeq *s)
{
    if (s->phase != IG_GTW_PHASE_REQUEST) {
        return;
    }
    s->phase = IG_GTW_PHASE_WAIT_SET;
    s->polls = 0u;
}

// The caller read igGtwReadOffset() and got `ack`. Ignored outside a wait
// phase, so a confused caller cannot advance a finished sequence.
static inline void igGtwFeedAck(IGGtwSeq *s, IGU32 ack)
{
    if (!igGtwWantsRead(s)) {
        return;
    }
    const bool wantSet = (s->phase == IG_GTW_PHASE_WAIT_SET);
    s->lastAck = ack;
    s->polls++;

    const IGU32 v = igGtwAckVerdict(ack, wantSet, s->polls, s->budget);
    switch (v) {
        case IG_GTW_ACK_OK:
            if (wantSet) {
                igGtwFinish(s, IG_GTW_ACQ_HELD);
            } else {
                s->phase = IG_GTW_PHASE_REQUEST;
                s->polls = 0u;
            }
            return;
        case IG_GTW_ACK_PENDING:
            return;
        case IG_GTW_ACK_TIMED_OUT:
            igGtwFinish(s, wantSet ? IG_GTW_ACQ_NO_ACK : IG_GTW_ACQ_STALE_ACK);
            return;
        case IG_GTW_ACK_MMIO_DEAD:
            igGtwFinish(s, IG_GTW_ACQ_MMIO_DEAD);
            return;
        default:
            // BAD_BUDGET is unreachable (igGtwBegin refused it), and anything
            // else is a verdict this switch does not know: refuse.
            igGtwFinish(s, IG_GTW_ACQ_BAD_BUDGET);
            return;
    }
}

#endif // IntelGtForcewake_hpp
