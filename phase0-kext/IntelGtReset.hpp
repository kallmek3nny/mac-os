//
//  IntelGtReset.hpp
//  Gen12 (Tiger Lake) ENGINE RECOVERY composition -- the layer that answers
//  "what does this driver do when the fence never fires?", which every header
//  beneath it declines to answer.
//
//  WHAT WAS MISSING
//  ----------------
//  IntelGtBlt.hpp composes packets saying what the engine would DO.
//  IntelGtBatch.hpp assembles those into a terminated batch buffer.
//  IntelGtRing.hpp writes an MI_BATCH_BUFFER_START into a circular buffer.
//  IntelGtExec.hpp judges whether a submission is admissible.
//  IntelGtFence.hpp composes the completion signal that would let this driver
//  learn the work finished.
//
//  All six describe the path where things WORK. None of them answers the
//  question that follows immediately from the fence layer existing at all: the
//  fence is a polled observation, so the driver's own code has to decide, at
//  some point, that the observation is never going to arrive. That decision has
//  no representation anywhere in this tree, and its absence is not symmetric
//  with the others -- a driver that cannot conclude "this did not finish" waits
//  forever on the one code path where the hardware is in the worst state, which
//  is exactly the path where waiting forever is least acceptable.
//
//  Worse, the two readings are INDISTINGUISHABLE from the fence alone.
//  igGtfSeqnoPassed() returning false means "not yet" and means "never", and
//  those call for opposite actions: keep polling, or stop the engine and
//  recover. handoff34 section 1's class one layer up -- a bool satisfied by two
//  categorically different states at once -- except here the two states are a
//  healthy in-flight submission and a hung engine.
//
//  WHAT THIS HEADER DOES AND DOES NOT DO
//  -------------------------------------
//  It composes and JUDGES. It performs no MMIO, exactly like every header
//  beneath it: it says which offset a reset request would be written to and what
//  value it would carry, and it reads a supplied register word to decide whether
//  the engine reported itself ready. It writes nothing. IntelGtProbe.cpp
//  consumes the judgement and publishes it; nothing in this tree resets an
//  engine, because nothing in this tree has ever submitted anything to one.
//
//  THE CITED HALF
//  --------------
//  Unlike IntelGtExec.hpp, IntelGtContext.hpp, IntelGtImage.hpp and
//  IntelGtFence.hpp -- whose constants are all POLICY, because reference/i915/
//  holds no file describing them -- this layer's registers ARE vendored, so the
//  three reset bits and the ring-stop bit carry a file-and-line citation rather
//  than a policy name. handoff37 section 1a's asymmetry still applies to
//  everything else here: tools/check-registers.py does not parse this header at
//  all, so a POLICY constant going wrong is invisible to that gate by
//  construction, and only the host suite can see it.
//
//  Where a value is composed from the sibling that owns it rather than spelled
//  as a literal, that is deliberate -- handoff37 section 2a. A wrong NAME is a
//  compile error clang points at; a wrong VALUE compiles cleanly and every gate
//  stays green over it.
//
//  Same rule as the nine headers before it: DEPENDENCY-FREE. No IOKit, no kernel
//  types, no allocation, no sleep. The one #include is a quoted sibling,
//  IntelGtFence.hpp -- which is the layer directly beneath this one and which
//  itself includes IntelGtCmd.hpp, so IGU32, the engine bases and
//  IG_GT_BASE_INVALID all arrive through it. The include is NOT decorative:
//  igGtrWaitVerdict() calls igGtfSeqnoPassed(), because the question "did the
//  fence pass" has exactly one owner and a second copy of that comparison here
//  would be a silent duplicate of a field layout. -I phase0-kext parses this
//  file standalone.
//
#ifndef IntelGtReset_hpp
#define IntelGtReset_hpp

#include "IntelGtFence.hpp"

// ---------------------------------------------------------------------------
// Register offsets -- CITED
// ---------------------------------------------------------------------------
//
// These are engine-relative, exactly like IG_GT_RING_TAIL_OFFSET_I915_0x30 and
// its siblings in IntelGtCmd.hpp: add an engine base to get an absolute offset.
// That is the same shape the ring registers already use, and it is the reason
// the composer below takes a base rather than embedding one.
#define IG_GTR_RESET_CTL_OFFSET_I915_0xD0    0xD0u  // intel_engine_regs.h:103 RING_RESET_CTL
#define IG_GTR_MI_MODE_OFFSET_I915_0x9C      0x9Cu  // intel_engine_regs.h:73 RING_MI_MODE

// RING_RESET_CTL bits. REG_BIT(n) is (1 << n) -- these three are the whole of
// that register this driver would ever touch.
#define IG_GTR_RESET_REQUEST_I915_BIT0       (1u << 0)  // intel_engine_regs.h:106 RESET_CTL_REQUEST_RESET
#define IG_GTR_RESET_READY_I915_BIT1         (1u << 1)  // intel_engine_regs.h:105 RESET_CTL_READY_TO_RESET
#define IG_GTR_RESET_CAT_ERROR_I915_BIT2     (1u << 2)  // intel_engine_regs.h:104 RESET_CTL_CAT_ERROR

// RING_MI_MODE bit 8. Setting it asks the engine to stop fetching commands,
// which is what has to happen BEFORE a reset request -- an engine still pulling
// from the ring can retire the very work the driver decided was hung.
#define IG_GTR_STOP_RING_I915_BIT8           (1u << 8)  // intel_engine_regs.h:78 STOP_RING

// ---------------------------------------------------------------------------
// Policy constants
// ---------------------------------------------------------------------------
//
// POLICY, not measured, and not citable: reference/i915/ describes the
// registers and says nothing about how long a driver should wait. Every number
// below is this project's own choice and is named so that it cannot be mistaken
// for a hardware fact.
//
// The poll budget is a COUNT, deliberately, not a duration. A duration would
// need a clock, and a clock is exactly the kind of dependency that would stop
// this header parsing standalone and stop the host suite driving it. The caller
// owns the delay between polls; this header owns the arithmetic over how many
// have happened.
#define IG_GTR_POLL_BUDGET_POLICY_200        200u

// The ready-bit poll budget after a reset request is SEPARATE from the
// completion budget and much smaller, because the two wait on different things:
// the first waits on submitted work whose length this driver does not know, and
// the second waits on a hardware handshake that either happens promptly or is
// not going to happen at all. One number for both would either abandon live
// work early or hang on a dead handshake.
#define IG_GTR_READY_BUDGET_POLICY_20        20u

// ---------------------------------------------------------------------------
// Wait verdicts -- why the fence was not observed to pass
// ---------------------------------------------------------------------------
//
// A bool is satisfied by every refusal at once (handoff34 section 1), and this
// is the family where that matters most in the whole GT stack: "the seqno did
// not pass" is the SAME bool for work still in flight and for an engine that
// will never retire it, and those two call for opposite next actions. Naming
// them apart is the entire content of this layer.
#define IG_GTR_WAIT_NOT_ATTEMPTED       0u  // caller's initialiser, never returned
#define IG_GTR_WAIT_PASSED              1u  // the seqno reached or passed the target
#define IG_GTR_WAIT_PENDING             2u  // not yet, and budget remains -- keep polling
#define IG_GTR_WAIT_TIMED_OUT           3u  // budget exhausted, seqno never passed
#define IG_GTR_WAIT_CAT_ERROR           4u  // engine reported a catastrophic error
#define IG_GTR_WAIT_LIMIT               5u

// An EQUALITY against PASSED, never a negation of a failure arm. handoff33
// section 2's reasoning: != TIMED_OUT is true for PENDING and for CAT_ERROR, so
// a negation would report an in-flight submission and a dead engine as
// completed work.
static inline bool igGtrWaitSucceeded(IGU32 verdict)
{
    return verdict == IG_GTR_WAIT_PASSED;
}

// Every value below the family's own limit has a name, and the value AT the
// limit answers the sentinel -- the pairing that forces whoever adds a sixth
// verdict to come back and extend this switch. handoff20 section 1's
// igModeVerdictName() defect is why this is written as a bounded family rather
// than as a switch with a default.
static inline const char *igGtrWaitVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTR_WAIT_NOT_ATTEMPTED: return "not-attempted";
        case IG_GTR_WAIT_PASSED:        return "passed";
        case IG_GTR_WAIT_PENDING:       return "pending";
        case IG_GTR_WAIT_TIMED_OUT:     return "timed-out";
        case IG_GTR_WAIT_CAT_ERROR:     return "cat-error";
        default:                        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Reset verdicts -- what happened when recovery was attempted
// ---------------------------------------------------------------------------
#define IG_GTR_RESET_NOT_ATTEMPTED      0u  // caller's initialiser, never returned
#define IG_GTR_RESET_READY              1u  // engine reported READY_TO_RESET
#define IG_GTR_RESET_NOT_READY          2u  // handshake budget exhausted
#define IG_GTR_RESET_BAD_ENGINE         3u  // engine base is not one this driver knows
#define IG_GTR_RESET_BAD_BUDGET         4u  // caller passed a zero budget
#define IG_GTR_RESET_LIMIT              5u

static inline bool igGtrResetSucceeded(IGU32 verdict)
{
    return verdict == IG_GTR_RESET_READY;
}

static inline const char *igGtrResetVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTR_RESET_NOT_ATTEMPTED: return "not-attempted";
        case IG_GTR_RESET_READY:         return "ready";
        case IG_GTR_RESET_NOT_READY:     return "not-ready";
        case IG_GTR_RESET_BAD_ENGINE:    return "bad-engine";
        case IG_GTR_RESET_BAD_BUDGET:    return "bad-budget";
        default:                         return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Register offset composition
// ---------------------------------------------------------------------------
//
// The engine base is VALIDATED rather than trusted. An unrecognised base
// composes a plausible absolute offset into whatever block happens to live
// there, and this project has already been burned once by exactly that shape --
// the NVIDIA side's per-engine translators exist because the RISC-V block sits
// one page above the Falcon block, so a wrong base reads and writes real
// hardware silently rather than faulting (see CLAUDE.md, "Address translation
// is per-engine on purpose").
//
// The four bases are the ones IntelGtCmd.hpp already owns, composed from those
// constants rather than respelled -- handoff37 section 2a. A wrong name is a
// compile error; a wrong literal is a silent write to the wrong engine.
static inline bool igGtrEngineBaseKnown(IGU32 engineBase)
{
    return engineBase == IG_GT_RENDER_RING_BASE_I915_0x02000
           || engineBase == IG_GT_BLT_RING_BASE_I915_0x22000
           || engineBase == IG_GT_BSD_RING_BASE_I915_0x1C0000
           || engineBase == IG_GT_VEBOX_RING_BASE_I915_0x1C8000;
}

// Returns the absolute MMIO offset of RING_RESET_CTL for an engine, or
// IG_GT_BASE_INVALID for a base this driver does not recognise. The sentinel is
// the one IntelGtCmd.hpp already owns and igGtRingReg() already answers with,
// rather than a second spelling of the same idea -- two sentinels for one
// refusal drift apart, and a caller checking the wrong one treats a refusal as
// an address. Refusing rather than composing is the same direction every
// accessor in this tree takes: a refused offset is loud, and a plausible wrong
// one scribbles.
static inline IGU32 igGtrResetCtlOffset(IGU32 engineBase)
{
    if (!igGtrEngineBaseKnown(engineBase)) {
        return IG_GT_BASE_INVALID;
    }
    return engineBase + IG_GTR_RESET_CTL_OFFSET_I915_0xD0;
}

static inline IGU32 igGtrMiModeOffset(IGU32 engineBase)
{
    if (!igGtrEngineBaseKnown(engineBase)) {
        return IG_GT_BASE_INVALID;
    }
    return engineBase + IG_GTR_MI_MODE_OFFSET_I915_0x9C;
}

// ---------------------------------------------------------------------------
// Register value composition
// ---------------------------------------------------------------------------
//
// The value a reset request would carry, and the value that would stop the ring.
// Composed from the cited bit constants, never spelled -- a hand-written 0x1
// here reads as "request reset" and would still be 0x1 if the bit moved.
static inline IGU32 igGtrResetRequestValue(void)
{
    return IG_GTR_RESET_REQUEST_I915_BIT0;
}

static inline IGU32 igGtrStopRingValue(void)
{
    return IG_GTR_STOP_RING_I915_BIT8;
}

// ---------------------------------------------------------------------------
// Register readback decode
// ---------------------------------------------------------------------------
//
// A refused read answers all-ones (IG_MMIO_INVALID's convention, handoff24), and
// all-ones has every bit set -- so a naive bit test on a refused read reports
// READY_TO_RESET and CAT_ERROR simultaneously. That is the fabricated-publication
// class (handoff18 section 1d) aimed at the one decision in this layer that
// would stop the driver waiting, so the refusal is tested FIRST and separately
// rather than folded into the bit tests.
static inline bool igGtrRegisterRefused(IGU32 value)
{
    return value == 0xFFFFFFFFu;
}

// Both accessors answer false on a refused read. False is the safe direction for
// each: "not ready" keeps the driver from proceeding into a reset the hardware
// never acknowledged, and "no cat error" keeps a refused read from being
// reported as a hardware fault that did not happen.
static inline bool igGtrReportsReadyToReset(IGU32 resetCtl)
{
    if (igGtrRegisterRefused(resetCtl)) {
        return false;
    }
    return (resetCtl & IG_GTR_RESET_READY_I915_BIT1) != 0u;
}

static inline bool igGtrReportsCatError(IGU32 resetCtl)
{
    if (igGtrRegisterRefused(resetCtl)) {
        return false;
    }
    return (resetCtl & IG_GTR_RESET_CAT_ERROR_I915_BIT2) != 0u;
}

// ---------------------------------------------------------------------------
// The wait judgement
// ---------------------------------------------------------------------------
//
// This is the function the whole header exists for. Given the seqno the fence
// layer observed, the target it was composed against, how many polls have
// happened, the budget, and the engine's RESET_CTL word, it says which of the
// five states the wait is in.
//
// ORDER IS THE CONTENT, and it is not the intuitive order.
//
// CAT_ERROR is tested BEFORE the seqno, because a catastrophic error is a fact
// about the engine while a passed seqno is a fact about one submission -- and an
// engine in cat-error can have a stale seqno store in memory from work that
// completed before it died. Testing the seqno first would report that dead
// engine as a healthy completion and send the driver on to submit more work to
// it.
//
// The seqno is then tested BEFORE the budget, because work that finished on the
// last possible poll finished. Testing the budget first turns a successful
// submission into a spurious engine reset, which is the expensive direction:
// resetting a healthy engine destroys every other context on it.
static inline IGU32 igGtrWaitVerdict(IGU32 observedSeqno, IGU32 targetSeqno,
                                     IGU32 pollsDone, IGU32 pollBudget,
                                     IGU32 resetCtl)
{
    if (igGtrReportsCatError(resetCtl)) {
        return IG_GTR_WAIT_CAT_ERROR;
    }

    if (igGtfSeqnoPassed(observedSeqno, targetSeqno)) {
        return IG_GTR_WAIT_PASSED;
    }

    if (pollsDone >= pollBudget) {
        return IG_GTR_WAIT_TIMED_OUT;
    }

    return IG_GTR_WAIT_PENDING;
}

// Whether a verdict means the driver should recover the engine. An EQUALITY set
// rather than !igGtrWaitSucceeded(), because that negation is TRUE for PENDING
// -- and resetting an engine with work legitimately still in flight is the one
// action this layer exists to prevent.
static inline bool igGtrWaitDemandsRecovery(IGU32 verdict)
{
    return verdict == IG_GTR_WAIT_TIMED_OUT
           || verdict == IG_GTR_WAIT_CAT_ERROR;
}

// ---------------------------------------------------------------------------
// The reset handshake judgement
// ---------------------------------------------------------------------------
//
// A zero budget is REFUSED rather than treated as "already exhausted". Those
// two readings differ: a zero budget is a bug in the caller, and reporting it
// as NOT_READY would blame the hardware for a driver defect -- handoff22
// section 1's ordering argument, where the bug somebody can go and fix must win
// over the fact about the machine.
//
// The engine check runs FIRST for the same reason one level up: an unrecognised
// base means every offset this call would have used was wrong, so nothing read
// through it says anything about readiness.
static inline IGU32 igGtrResetVerdict(IGU32 engineBase, IGU32 resetCtl,
                                      IGU32 pollsDone, IGU32 readyBudget)
{
    if (!igGtrEngineBaseKnown(engineBase)) {
        return IG_GTR_RESET_BAD_ENGINE;
    }

    if (readyBudget == 0u) {
        return IG_GTR_RESET_BAD_BUDGET;
    }

    if (igGtrReportsReadyToReset(resetCtl)) {
        return IG_GTR_RESET_READY;
    }

    if (pollsDone >= readyBudget) {
        return IG_GTR_RESET_NOT_READY;
    }

    // Budget remains and the handshake has not landed. NOT_READY is the honest
    // answer for a caller that has not finished polling -- there is deliberately
    // no PENDING arm here, because unlike the wait above, a reset handshake that
    // has not completed is never a state the driver proceeds from.
    return IG_GTR_RESET_NOT_READY;
}

#endif // IntelGtReset_hpp
