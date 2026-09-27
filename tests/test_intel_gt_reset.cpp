//
//  test_intel_gt_reset.cpp
//  Host-side suite over phase0-kext/IntelGtReset.hpp -- the recovery layer.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  IntelGtReset.hpp is the tenth GT header and the first one that decides the
//  driver should STOP waiting. Per the logic/IOKit split in CLAUDE.md, a pure
//  header with no suite is a pure header nothing runs -- intel-handoff26.md
//  section 1 measured exactly that for igSampleVerdict(), a function with 421
//  checks over it and no caller. This file is the executor.
//
//  WHAT IS PINNED, AND WHY EACH ONE NEEDS PINNING
//  ---------------------------------------------
//    1. Both verdict families are BOUNDED: every value below the family's own
//       _LIMIT has a name, the names are pairwise distinct, and the value AT the
//       limit answers the sentinel. handoff20 section 1's igModeVerdictName()
//       had arms 0..5 while its limit was 6, and the lost verdict was the one
//       naming a byte-swapped register pair -- the most dangerous member of its
//       own family. A per-arm loop bounded on the header's own limit is what
//       makes a family that GROWS redden on the commit that grows it.
//
//    2. Both success predicates are EQUALITIES with exactly one preimage.
//       handoff34 section 1: `== false` was satisfied by seven refusal codes at
//       once, so an assertion green through four sessions could not tell the fix
//       from the bug it fixed. Here the walk asserts that igGtrWaitSucceeded()
//       is true for PASSED and false for every OTHER value below the limit, so
//       `true` has one preimage by measurement rather than by reading.
//
//    3. The wait ORDER. This is the whole content of the layer and it is the one
//       property where every arm composes a legal-looking verdict:
//         - cat-error BEFORE seqno, because a dead engine can hold a stale seqno
//           store from work that retired before it died, and seqno-first reports
//           that as a healthy completion.
//         - seqno BEFORE budget, because work that landed on the final poll
//           landed, and budget-first turns a success into a spurious reset that
//           destroys every other context on the engine.
//       Both orderings type-check, both return a named verdict, and only the
//       INPUT that satisfies two arms at once separates them. Every ordering
//       check below is therefore driven by a doubly-satisfying input.
//
//    4. The refused-read decode. A refused MMIO read answers all-ones
//       (handoff24), and all-ones has every bit set -- so a naive bit test
//       reports READY_TO_RESET and CAT_ERROR simultaneously off a register
//       nobody measured. That is handoff18 section 1d's fabricated-publication
//       class aimed at the two decisions that would stop the driver waiting.
//
//    5. Offset composition REFUSES an unknown engine base rather than composing
//       a plausible one, and refuses with the sentinel IntelGtCmd.hpp already
//       owns rather than a second spelling of it. CLAUDE.md's "address
//       translation is per-engine on purpose" is the NVIDIA side having been
//       burned by exactly this: a wrong base does not fault, it reads and writes
//       real hardware silently.
//
//    6. The reset handshake refuses a zero budget as a CALLER bug rather than
//       reporting NOT_READY. handoff22 section 1's ordering argument -- when an
//       input is both a driver defect and a hardware fact, report the defect,
//       because that is the one somebody can go and fix.
//
//  WHAT THIS SUITE DOES NOT ESTABLISH
//  ----------------------------------
//  No engine has ever been reset by this tree, because nothing in this tree has
//  ever submitted anything to one. Every value below is composed and judged on
//  the host; the register offsets have never been written and the readback words
//  are supplied by this file rather than by silicon.
//
#include <cstdio>
#include <cstring>

#include "IntelGtReset.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: got %s want %s\n",
                    label, got ? "true" : "false", want ? "true" : "false");
    }
}

static void check(const char *label, bool condition)
{
    gChecks++;
    if (!condition) {
        gFailures++;
        std::printf("FAIL %s\n", label);
    }
}

static void checkStr(const char *label, const char *got, const char *want)
{
    gChecks++;
    if (std::strcmp(got, want) != 0) {
        gFailures++;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", label, got, want);
    }
}

// ---------------------------------------------------------------------------
// Citations
// ---------------------------------------------------------------------------
//
// These four are the cited half of this header. Pinning them by value is what
// makes a transcription error in the offset or the bit position a failure here
// rather than a silent write to the wrong register -- tools/check-registers.py
// does not parse this header at all (handoff37 section 1a), so this suite is the
// ONLY observer these constants have.
static void testCitedConstants(void)
{
    checkU32("RING_RESET_CTL is engine-relative 0xD0",
             IG_GTR_RESET_CTL_OFFSET_I915_0xD0, 0xD0u);
    checkU32("RING_MI_MODE is engine-relative 0x9C",
             IG_GTR_MI_MODE_OFFSET_I915_0x9C, 0x9Cu);

    checkU32("RESET_CTL_REQUEST_RESET is bit 0",
             IG_GTR_RESET_REQUEST_I915_BIT0, 0x1u);
    checkU32("RESET_CTL_READY_TO_RESET is bit 1",
             IG_GTR_RESET_READY_I915_BIT1, 0x2u);
    checkU32("RESET_CTL_CAT_ERROR is bit 2",
             IG_GTR_RESET_CAT_ERROR_I915_BIT2, 0x4u);
    checkU32("STOP_RING is bit 8", IG_GTR_STOP_RING_I915_BIT8, 0x100u);

    // The three RESET_CTL bits must be DISTINCT and non-overlapping. A
    // copy-paste arm giving two of them the same value satisfies every
    // individual value check above while making the decode read one bit as the
    // other -- and "ready to reset" and "catastrophic error" are the two
    // conclusions in this header that are furthest apart.
    check("request and ready are distinct bits",
          (IG_GTR_RESET_REQUEST_I915_BIT0 & IG_GTR_RESET_READY_I915_BIT1) == 0u);
    check("ready and cat-error are distinct bits",
          (IG_GTR_RESET_READY_I915_BIT1 & IG_GTR_RESET_CAT_ERROR_I915_BIT2) == 0u);
    check("request and cat-error are distinct bits",
          (IG_GTR_RESET_REQUEST_I915_BIT0 & IG_GTR_RESET_CAT_ERROR_I915_BIT2) == 0u);

    // The two budgets are POLICY and must not be equal. They wait on different
    // things -- submitted work of unknown length, versus a hardware handshake
    // that either lands promptly or never -- so one number serving both would
    // either abandon live work early or hang on a dead handshake. Asserting the
    // RELATION rather than the literals is what survives a future retune.
    check("the ready budget is smaller than the poll budget",
          IG_GTR_READY_BUDGET_POLICY_20 < IG_GTR_POLL_BUDGET_POLICY_200);
    check("both budgets are non-zero",
          IG_GTR_POLL_BUDGET_POLICY_200 > 0u
          && IG_GTR_READY_BUDGET_POLICY_20 > 0u);
}

// ---------------------------------------------------------------------------
// Verdict families are bounded and complete
// ---------------------------------------------------------------------------
static void testWaitVerdictNames(void)
{
    checkStr("wait not-attempted names itself",
             igGtrWaitVerdictName(IG_GTR_WAIT_NOT_ATTEMPTED), "not-attempted");
    checkStr("wait passed names itself",
             igGtrWaitVerdictName(IG_GTR_WAIT_PASSED), "passed");
    checkStr("wait pending names itself",
             igGtrWaitVerdictName(IG_GTR_WAIT_PENDING), "pending");
    checkStr("wait timed-out names itself",
             igGtrWaitVerdictName(IG_GTR_WAIT_TIMED_OUT), "timed-out");
    checkStr("wait cat-error names itself",
             igGtrWaitVerdictName(IG_GTR_WAIT_CAT_ERROR), "cat-error");

    // The tripwire. The value AT the limit must answer the sentinel, so whoever
    // adds a sixth verdict has to come back and extend the switch.
    checkStr("the value at the wait limit is unknown",
             igGtrWaitVerdictName(IG_GTR_WAIT_LIMIT), "unknown");

    // Every value BELOW the limit is named, bounded on the header's own limit
    // rather than on a hardcoded 5 -- a family that grows reddens on the commit
    // that grows it. A deleted arm reddens here even if nothing else references
    // the verdict.
    for (IGU32 v = 0; v < IG_GTR_WAIT_LIMIT; v++) {
        check("every wait verdict below the limit is named",
              std::strcmp(igGtrWaitVerdictName(v), "unknown") != 0);
    }

    // Pairwise distinct. A copy-paste arm returning a sibling's string satisfies
    // every "has a name" assertion above -- handoff21 section 1, where six of
    // seven architecture arms had never executed while 64 checks printed green.
    for (IGU32 a = 0; a < IG_GTR_WAIT_LIMIT; a++) {
        for (IGU32 b = a + 1; b < IG_GTR_WAIT_LIMIT; b++) {
            check("wait verdict names are pairwise distinct",
                  std::strcmp(igGtrWaitVerdictName(a),
                              igGtrWaitVerdictName(b)) != 0);
        }
    }
}

static void testResetVerdictNames(void)
{
    checkStr("reset not-attempted names itself",
             igGtrResetVerdictName(IG_GTR_RESET_NOT_ATTEMPTED), "not-attempted");
    checkStr("reset ready names itself",
             igGtrResetVerdictName(IG_GTR_RESET_READY), "ready");
    checkStr("reset not-ready names itself",
             igGtrResetVerdictName(IG_GTR_RESET_NOT_READY), "not-ready");
    checkStr("reset bad-engine names itself",
             igGtrResetVerdictName(IG_GTR_RESET_BAD_ENGINE), "bad-engine");
    checkStr("reset bad-budget names itself",
             igGtrResetVerdictName(IG_GTR_RESET_BAD_BUDGET), "bad-budget");

    checkStr("the value at the reset limit is unknown",
             igGtrResetVerdictName(IG_GTR_RESET_LIMIT), "unknown");

    for (IGU32 v = 0; v < IG_GTR_RESET_LIMIT; v++) {
        check("every reset verdict below the limit is named",
              std::strcmp(igGtrResetVerdictName(v), "unknown") != 0);
    }

    for (IGU32 a = 0; a < IG_GTR_RESET_LIMIT; a++) {
        for (IGU32 b = a + 1; b < IG_GTR_RESET_LIMIT; b++) {
            check("reset verdict names are pairwise distinct",
                  std::strcmp(igGtrResetVerdictName(a),
                              igGtrResetVerdictName(b)) != 0);
        }
    }

    // The two families are independent and must not share a sentinel by
    // accident. handoff21 section 1b: a coverage loop comparing against a
    // sibling's default string counted every arm as named unconditionally,
    // including an arm deleted from the switch. Both sentinels are read from the
    // subject here, and asserted to be the same word deliberately -- so this
    // check documents the sharing rather than assuming it.
    checkStr("both families answer the same sentinel word",
             igGtrWaitVerdictName(IG_GTR_WAIT_LIMIT),
             igGtrResetVerdictName(IG_GTR_RESET_LIMIT));
}

// ---------------------------------------------------------------------------
// The success predicates have exactly one preimage each
// ---------------------------------------------------------------------------
static void testSuccessIsEquality(void)
{
    checkBool("wait succeeded is true for passed",
              igGtrWaitSucceeded(IG_GTR_WAIT_PASSED), true);

    // The separating walk. handoff34 section 1's whole subject: a predicate
    // whose true side has more than one preimage cannot distinguish the states
    // its callers act on. Every value below the limit that is not PASSED must be
    // false, so a `!= TIMED_OUT` reformulation -- true for PENDING and for
    // CAT_ERROR -- reddens here.
    for (IGU32 v = 0; v < IG_GTR_WAIT_LIMIT; v++) {
        if (v == IG_GTR_WAIT_PASSED) {
            continue;
        }
        check("no wait verdict other than passed succeeds",
              !igGtrWaitSucceeded(v));
    }

    checkBool("reset succeeded is true for ready",
              igGtrResetSucceeded(IG_GTR_RESET_READY), true);

    for (IGU32 v = 0; v < IG_GTR_RESET_LIMIT; v++) {
        if (v == IG_GTR_RESET_READY) {
            continue;
        }
        check("no reset verdict other than ready succeeds",
              !igGtrResetSucceeded(v));
    }

    // NOT_ATTEMPTED is the caller's initialiser and must never read as success.
    // handoff29 section 2: an initialiser zero published as a measurement is
    // byte-identical to a register that genuinely read zero.
    checkBool("the wait initialiser is not success",
              igGtrWaitSucceeded(IG_GTR_WAIT_NOT_ATTEMPTED), false);
    checkBool("the reset initialiser is not success",
              igGtrResetSucceeded(IG_GTR_RESET_NOT_ATTEMPTED), false);
}

// ---------------------------------------------------------------------------
// Recovery demand is an equality set, not a negation
// ---------------------------------------------------------------------------
static void testRecoveryDemand(void)
{
    checkBool("a timeout demands recovery",
              igGtrWaitDemandsRecovery(IG_GTR_WAIT_TIMED_OUT), true);
    checkBool("a cat-error demands recovery",
              igGtrWaitDemandsRecovery(IG_GTR_WAIT_CAT_ERROR), true);

    // The one that matters. !igGtrWaitSucceeded() is TRUE for PENDING, so a
    // negation-based spelling would reset an engine with work legitimately still
    // in flight -- the single action this layer exists to prevent, because a
    // reset destroys every other context on the engine.
    checkBool("a pending wait does NOT demand recovery",
              igGtrWaitDemandsRecovery(IG_GTR_WAIT_PENDING), false);
    checkBool("a passed wait does not demand recovery",
              igGtrWaitDemandsRecovery(IG_GTR_WAIT_PASSED), false);
    checkBool("the initialiser does not demand recovery",
              igGtrWaitDemandsRecovery(IG_GTR_WAIT_NOT_ATTEMPTED), false);

    // Recovery-demanding and succeeding are disjoint over the whole family --
    // the two conclusions are opposite actions, so no verdict may satisfy both.
    for (IGU32 v = 0; v < IG_GTR_WAIT_LIMIT; v++) {
        check("no verdict both succeeds and demands recovery",
              !(igGtrWaitSucceeded(v) && igGtrWaitDemandsRecovery(v)));
    }
}

// ---------------------------------------------------------------------------
// Refused reads decode to false on both paths
// ---------------------------------------------------------------------------
static void testRefusedReadDecode(void)
{
    const IGU32 kRefused = 0xFFFFFFFFu;

    checkBool("all-ones is a refused read", igGtrRegisterRefused(kRefused), true);

    // Zero is NOT a refusal. On this silicon an unprogrammed display page reads
    // 0x00000000 (handoff15 section 2), and all-ones is what a block that is not
    // decoding reports -- so the sentinel is an EQUALITY against all-ones, never
    // a threshold and never a zero test.
    checkBool("zero is not a refused read", igGtrRegisterRefused(0u), false);
    checkBool("a word with both bits set is not a refusal",
              igGtrRegisterRefused(IG_GTR_RESET_READY_I915_BIT1
                                   | IG_GTR_RESET_CAT_ERROR_I915_BIT2), false);

    // The fabricated-publication case. All-ones has bit 1 AND bit 2 set, so a
    // naive bit test answers "ready to reset" and "catastrophic error"
    // simultaneously off a register nobody measured. Both accessors must answer
    // false, and false is the safe direction for each: not-ready keeps the
    // driver out of a reset the hardware never acknowledged, and no-cat-error
    // keeps a refused read from being reported as a fault that did not happen.
    checkBool("a refused read does not report ready",
              igGtrReportsReadyToReset(kRefused), false);
    checkBool("a refused read does not report cat-error",
              igGtrReportsCatError(kRefused), false);

    // And the positive side, so the accessors are not merely constant-false.
    checkBool("the ready bit alone reports ready",
              igGtrReportsReadyToReset(IG_GTR_RESET_READY_I915_BIT1), true);
    checkBool("the cat-error bit alone reports cat-error",
              igGtrReportsCatError(IG_GTR_RESET_CAT_ERROR_I915_BIT2), true);

    // Cross-checks: each accessor must read its OWN bit. A mask widened to cover
    // both bits survives every check that only ever sets one of them -- the
    // far-side-bit shape.
    checkBool("the cat-error bit does not report ready",
              igGtrReportsReadyToReset(IG_GTR_RESET_CAT_ERROR_I915_BIT2), false);
    checkBool("the ready bit does not report cat-error",
              igGtrReportsCatError(IG_GTR_RESET_READY_I915_BIT1), false);
    checkBool("the request bit does not report ready",
              igGtrReportsReadyToReset(IG_GTR_RESET_REQUEST_I915_BIT0), false);
    checkBool("the request bit does not report cat-error",
              igGtrReportsCatError(IG_GTR_RESET_REQUEST_I915_BIT0), false);

    checkBool("a zero register reports neither ready nor cat-error",
              igGtrReportsReadyToReset(0u) || igGtrReportsCatError(0u), false);
}

// ---------------------------------------------------------------------------
// Offset composition refuses an unknown engine
// ---------------------------------------------------------------------------
static void testOffsetComposition(void)
{
    checkBool("the render base is known",
              igGtrEngineBaseKnown(IG_GT_RENDER_RING_BASE_I915_0x02000), true);
    checkBool("the blitter base is known",
              igGtrEngineBaseKnown(IG_GT_BLT_RING_BASE_I915_0x22000), true);
    checkBool("the bsd base is known",
              igGtrEngineBaseKnown(IG_GT_BSD_RING_BASE_I915_0x1C0000), true);
    checkBool("the vebox base is known",
              igGtrEngineBaseKnown(IG_GT_VEBOX_RING_BASE_I915_0x1C8000), true);

    checkBool("zero is not a known engine base",
              igGtrEngineBaseKnown(0u), false);
    checkBool("the sentinel is not a known engine base",
              igGtrEngineBaseKnown(IG_GT_BASE_INVALID), false);

    // One page off a real base. This is the shape CLAUDE.md's per-engine
    // translation rule exists for: on the NVIDIA side the RISC-V block sits one
    // 4 KiB page above the Falcon block, so a base short by a page composes a
    // VALID address into the wrong engine and scribbles on it without faulting.
    checkBool("one page above the render base is not known",
              igGtrEngineBaseKnown(IG_GT_RENDER_RING_BASE_I915_0x02000 + 0x1000u),
              false);
    checkBool("one page below the blitter base is not known",
              igGtrEngineBaseKnown(IG_GT_BLT_RING_BASE_I915_0x22000 - 0x1000u),
              false);

    // Composition is base + cited offset, checked against arithmetic this file
    // performs independently rather than against the header's own expression.
    checkU32("render RESET_CTL composes to 0x020D0",
             igGtrResetCtlOffset(IG_GT_RENDER_RING_BASE_I915_0x02000), 0x020D0u);
    checkU32("blitter RESET_CTL composes to 0x220D0",
             igGtrResetCtlOffset(IG_GT_BLT_RING_BASE_I915_0x22000), 0x220D0u);
    checkU32("render MI_MODE composes to 0x0209C",
             igGtrMiModeOffset(IG_GT_RENDER_RING_BASE_I915_0x02000), 0x0209Cu);
    checkU32("vebox MI_MODE composes to 0x1C809C",
             igGtrMiModeOffset(IG_GT_VEBOX_RING_BASE_I915_0x1C8000), 0x1C809Cu);

    // The refusal, and the sentinel it refuses WITH. Two spellings of one
    // refusal drift apart, and a caller checking the wrong one treats a refusal
    // as an address -- so the sentinel is the one IntelGtCmd.hpp already owns.
    checkU32("an unknown base refuses a RESET_CTL offset",
             igGtrResetCtlOffset(0x9999u), IG_GT_BASE_INVALID);
    checkU32("an unknown base refuses an MI_MODE offset",
             igGtrMiModeOffset(0x9999u), IG_GT_BASE_INVALID);

    // The two offsets must never collide for one engine, and one engine's
    // offsets must never land on another's. A transposed constant satisfies
    // every single-value check while pointing one register at the other.
    check("RESET_CTL and MI_MODE differ for the same engine",
          igGtrResetCtlOffset(IG_GT_RENDER_RING_BASE_I915_0x02000)
          != igGtrMiModeOffset(IG_GT_RENDER_RING_BASE_I915_0x02000));
    check("two engines do not share a RESET_CTL offset",
          igGtrResetCtlOffset(IG_GT_RENDER_RING_BASE_I915_0x02000)
          != igGtrResetCtlOffset(IG_GT_BLT_RING_BASE_I915_0x22000));

    // The values a write would carry, composed from the cited bits. A
    // hand-written 0x1 here reads as "request reset" and would still be 0x1 if
    // the bit moved.
    checkU32("the reset request value is the request bit",
             igGtrResetRequestValue(), IG_GTR_RESET_REQUEST_I915_BIT0);
    checkU32("the stop-ring value is the stop-ring bit",
             igGtrStopRingValue(), IG_GTR_STOP_RING_I915_BIT8);

    // A reset request must NOT carry the ready or cat-error bits. Those are
    // status bits the hardware owns; writing them back is this driver asserting
    // a state it is supposed to be observing.
    check("the reset request carries no status bits",
          (igGtrResetRequestValue()
           & (IG_GTR_RESET_READY_I915_BIT1
              | IG_GTR_RESET_CAT_ERROR_I915_BIT2)) == 0u);
    check("the stop-ring value is not a RESET_CTL bit",
          igGtrStopRingValue() != igGtrResetRequestValue());
}

// ---------------------------------------------------------------------------
// The wait judgement -- ordering is the content
// ---------------------------------------------------------------------------
static void testWaitVerdictOrdering(void)
{
    const IGU32 kBudget = IG_GTR_POLL_BUDGET_POLICY_200;
    const IGU32 kTarget = 42u;

    // Plain arms first, each with only ONE arm satisfied, so a failure here is
    // about the arm rather than about precedence.
    checkU32("a passed seqno with budget left is passed",
             igGtrWaitVerdict(kTarget, kTarget, 0u, kBudget, 0u),
             IG_GTR_WAIT_PASSED);
    checkU32("a seqno beyond the target is passed",
             igGtrWaitVerdict(kTarget + 1u, kTarget, 0u, kBudget, 0u),
             IG_GTR_WAIT_PASSED);
    checkU32("an unpassed seqno with budget left is pending",
             igGtrWaitVerdict(kTarget - 1u, kTarget, 0u, kBudget, 0u),
             IG_GTR_WAIT_PENDING);
    checkU32("an unpassed seqno with budget spent is timed out",
             igGtrWaitVerdict(kTarget - 1u, kTarget, kBudget, kBudget, 0u),
             IG_GTR_WAIT_TIMED_OUT);
    checkU32("a cat-error alone is cat-error",
             igGtrWaitVerdict(kTarget - 1u, kTarget, 0u, kBudget,
                              IG_GTR_RESET_CAT_ERROR_I915_BIT2),
             IG_GTR_WAIT_CAT_ERROR);

    // PRECEDENCE 1: cat-error beats a PASSED seqno. This input satisfies both
    // arms -- the engine reports catastrophic error AND the seqno store in
    // memory shows the target reached, which is exactly what a stale store from
    // work that retired before the engine died looks like. Seqno-first reports
    // that dead engine as a healthy completion and sends the driver on to submit
    // more work to it.
    checkU32("a cat-error beats a passed seqno",
             igGtrWaitVerdict(kTarget, kTarget, 0u, kBudget,
                              IG_GTR_RESET_CAT_ERROR_I915_BIT2),
             IG_GTR_WAIT_CAT_ERROR);

    // PRECEDENCE 2: cat-error beats an exhausted budget. Both are recovery
    // states, so igGtrWaitDemandsRecovery() cannot separate them -- only the
    // verdict word tells the operator whether the engine faulted or merely
    // stopped answering, and those are different bugs.
    checkU32("a cat-error beats an exhausted budget",
             igGtrWaitVerdict(kTarget - 1u, kTarget, kBudget, kBudget,
                              IG_GTR_RESET_CAT_ERROR_I915_BIT2),
             IG_GTR_WAIT_CAT_ERROR);

    // PRECEDENCE 3: a passed seqno beats an exhausted budget. Work that landed
    // on the final poll landed. Budget-first turns a successful submission into
    // a spurious reset, and a reset destroys every other context on the engine.
    checkU32("a passed seqno beats an exhausted budget",
             igGtrWaitVerdict(kTarget, kTarget, kBudget, kBudget, 0u),
             IG_GTR_WAIT_PASSED);

    // The boundary. One poll short of the budget is still pending; AT the budget
    // it has timed out. A `>` in place of the `>=` grants one extra poll, which
    // is harmless -- and a `>=` in place of a `>` on the other side would
    // abandon work a poll early. Both edges are pinned so neither drifts.
    checkU32("one poll short of the budget is pending",
             igGtrWaitVerdict(kTarget - 1u, kTarget, kBudget - 1u, kBudget, 0u),
             IG_GTR_WAIT_PENDING);
    checkU32("past the budget is timed out",
             igGtrWaitVerdict(kTarget - 1u, kTarget, kBudget + 1u, kBudget, 0u),
             IG_GTR_WAIT_TIMED_OUT);

    // A refused RESET_CTL read must not manufacture a cat-error. All-ones has
    // bit 2 set, so without the refusal test in igGtrReportsCatError() this
    // input would report a catastrophic hardware fault off a register nobody
    // measured -- and the driver would reset a healthy engine on the strength of
    // it. The verdict must fall through to the seqno arms instead.
    checkU32("a refused RESET_CTL does not manufacture a cat-error",
             igGtrWaitVerdict(kTarget, kTarget, 0u, kBudget, 0xFFFFFFFFu),
             IG_GTR_WAIT_PASSED);
    checkU32("a refused RESET_CTL still allows a timeout",
             igGtrWaitVerdict(kTarget - 1u, kTarget, kBudget, kBudget,
                              0xFFFFFFFFu),
             IG_GTR_WAIT_TIMED_OUT);

    // The verdict is never the initialiser. NOT_ATTEMPTED is the caller's own
    // starting value and this function must never return it -- an initialiser
    // returned as a judgement is handoff29 section 2's fabricated zero.
    for (IGU32 polls = 0; polls <= 2u; polls++) {
        check("the wait verdict is never not-attempted",
              igGtrWaitVerdict(kTarget - 1u, kTarget, polls, 2u, 0u)
              != IG_GTR_WAIT_NOT_ATTEMPTED);
    }

    // And it is always inside its own family, so a future arm cannot return a
    // value igGtrWaitVerdictName() has no name for.
    check("the wait verdict is inside its family",
          igGtrWaitVerdict(kTarget - 1u, kTarget, 0u, kBudget, 0u)
          < IG_GTR_WAIT_LIMIT);
}

// ---------------------------------------------------------------------------
// The reset handshake judgement
// ---------------------------------------------------------------------------
static void testResetVerdict(void)
{
    const IGU32 kRender = IG_GT_RENDER_RING_BASE_I915_0x02000;
    const IGU32 kBudget = IG_GTR_READY_BUDGET_POLICY_20;

    checkU32("a ready engine reports ready",
             igGtrResetVerdict(kRender, IG_GTR_RESET_READY_I915_BIT1, 0u, kBudget),
             IG_GTR_RESET_READY);
    checkU32("an unready engine with budget left is not ready",
             igGtrResetVerdict(kRender, 0u, 0u, kBudget),
             IG_GTR_RESET_NOT_READY);
    checkU32("an unready engine with budget spent is not ready",
             igGtrResetVerdict(kRender, 0u, kBudget, kBudget),
             IG_GTR_RESET_NOT_READY);

    // PRECEDENCE 1: the engine check runs first. This input satisfies the
    // bad-engine arm AND the ready arm -- an unrecognised base means every
    // offset this call would have used was wrong, so the word supposedly read
    // through it says nothing about readiness. Reporting READY here would be the
    // driver vouching for a register it never reached.
    checkU32("a bad engine beats a ready-looking register",
             igGtrResetVerdict(0x9999u, IG_GTR_RESET_READY_I915_BIT1, 0u, kBudget),
             IG_GTR_RESET_BAD_ENGINE);

    // PRECEDENCE 2: the engine check beats a zero budget. Both are caller bugs;
    // the engine one is reported because it invalidates the other's subject.
    checkU32("a bad engine beats a zero budget",
             igGtrResetVerdict(0x9999u, 0u, 0u, 0u),
             IG_GTR_RESET_BAD_ENGINE);

    // PRECEDENCE 3: a zero budget is a CALLER bug, not an exhausted wait.
    // handoff22 section 1's ordering argument -- when an input is both a driver
    // defect and a fact about the machine, report the defect, because that is
    // the one somebody can go and fix. Reporting NOT_READY would blame the
    // hardware for a zero this driver passed in.
    checkU32("a zero budget is a caller bug, not a not-ready",
             igGtrResetVerdict(kRender, 0u, 0u, 0u),
             IG_GTR_RESET_BAD_BUDGET);

    // PRECEDENCE 4: a zero budget beats a ready register. A ready handshake read
    // under a zero budget is still a caller bug, and accepting it would let a
    // miscounted caller proceed into a reset on a lucky read.
    checkU32("a zero budget beats a ready register",
             igGtrResetVerdict(kRender, IG_GTR_RESET_READY_I915_BIT1, 0u, 0u),
             IG_GTR_RESET_BAD_BUDGET);

    // A refused read must not report ready. This is the direction that matters
    // most in the whole header: all-ones has the ready bit set, so without the
    // refusal test the driver would conclude the hardware acknowledged a reset
    // that it never saw.
    checkU32("a refused RESET_CTL is not ready",
             igGtrResetVerdict(kRender, 0xFFFFFFFFu, 0u, kBudget),
             IG_GTR_RESET_NOT_READY);

    // Every known engine must reach the same verdict for the same inputs -- the
    // judgement is about the register word, not about which engine it came from.
    checkU32("the blitter reaches ready too",
             igGtrResetVerdict(IG_GT_BLT_RING_BASE_I915_0x22000,
                               IG_GTR_RESET_READY_I915_BIT1, 0u, kBudget),
             IG_GTR_RESET_READY);
    checkU32("the bsd engine reaches ready too",
             igGtrResetVerdict(IG_GT_BSD_RING_BASE_I915_0x1C0000,
                               IG_GTR_RESET_READY_I915_BIT1, 0u, kBudget),
             IG_GTR_RESET_READY);
    checkU32("the vebox engine reaches ready too",
             igGtrResetVerdict(IG_GT_VEBOX_RING_BASE_I915_0x1C8000,
                               IG_GTR_RESET_READY_I915_BIT1, 0u, kBudget),
             IG_GTR_RESET_READY);

    // The verdict is never the initialiser and is always inside its family.
    check("the reset verdict is never not-attempted",
          igGtrResetVerdict(kRender, 0u, 0u, kBudget)
          != IG_GTR_RESET_NOT_ATTEMPTED);
    check("the reset verdict is inside its family",
          igGtrResetVerdict(kRender, 0u, 0u, kBudget) < IG_GTR_RESET_LIMIT);

    // A cat-error word does NOT satisfy the ready handshake. The two bits are
    // adjacent, so a mask off by one reads one as the other -- and "the engine
    // is ready for me to reset it" versus "the engine has faulted" are opposite
    // conclusions about whether the handshake succeeded.
    checkU32("a cat-error word is not a ready handshake",
             igGtrResetVerdict(kRender, IG_GTR_RESET_CAT_ERROR_I915_BIT2, 0u,
                               kBudget),
             IG_GTR_RESET_NOT_READY);

    // A word with BOTH status bits set is ready -- the engine can be faulted and
    // still willing to be reset, which is in fact the expected state during
    // recovery from a catastrophic error. Pinning this stops somebody "fixing"
    // the ready test into an equality against the bare ready bit, which would
    // make recovery from the worst engine state impossible.
    checkU32("a faulted but ready engine is ready",
             igGtrResetVerdict(kRender,
                               IG_GTR_RESET_READY_I915_BIT1
                               | IG_GTR_RESET_CAT_ERROR_I915_BIT2,
                               0u, kBudget),
             IG_GTR_RESET_READY);
}

int main(void)
{
    std::printf("test_intel_gt_reset -- IntelGtReset.hpp recovery layer\n\n");

    testCitedConstants();
    testWaitVerdictNames();
    testResetVerdictNames();
    testSuccessIsEquality();
    testRecoveryDemand();
    testRefusedReadDecode();
    testOffsetComposition();
    testWaitVerdictOrdering();
    testResetVerdict();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
