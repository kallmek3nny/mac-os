//
//  IntelGtImage.hpp
//  Gen12 (Tiger Lake) LOGICAL RING CONTEXT image WRITER -- the layer directly
//  above IntelGtContext.hpp. That header answers WHERE each dword of the
//  register-state page goes; this one actually PUTS them there, into a span of
//  memory the caller owns.
//
//  DEPENDENCY-FREE, same rule as every other header in phase0-kext/. The one
//  #include is a quoted sibling (IntelGtContext.hpp), which is still inside
//  phase0-kext/ and so still parses standalone under -I phase0-kext.
//
//  EVIDENCE, STATED UP FRONT
//  -------------------------
//  Every constant this header reaches for is POLICY. reference/i915/ holds
//  seven files and all seven are display; greps for LRC, CTX_DESC,
//  RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP across that
//  whole directory return ZERO matches (measured 2026-08-26). So this writer
//  composes a page whose LAYOUT this project chose, and nothing here is
//  evidence that Gen12 silicon wants these dwords in these slots.
//
//  No *_OBSERVED_* constant may enter this header. igpu-results/ captured a
//  machine whose render engine was IDLE, so there is no MMIO capture of a
//  running render engine anywhere in this tree, and inventing one would file a
//  guess beside values witnessed on silicon.
//
//  WHY A WRITER IS ITS OWN LAYER
//  -----------------------------
//  IntelGtContext.hpp is all pure arithmetic over slot indices -- it can be
//  driven with any input and answers with a number. This header is the first
//  thing in the GT stack that MUTATES caller memory, and that is a different
//  contract with a different failure mode: a refusal that has already written
//  half a page is worse than one that wrote nothing, because a half-written
//  page is a well-formed image describing a ring nobody configured.
//
//  Two rules follow and both are load-bearing:
//
//   1. NOTHING IS WRITTEN UNTIL THE WHOLE IMAGE IS ADMISSIBLE. igGtcImageVerdict()
//      is consulted FIRST, and a refusing verdict returns without touching a
//      single dword. Every occurrence of this tree's fabricated-publication
//      class has been a caller that had a refusal available and did not consult
//      it -- five times, in two tracks, in code whose own comments explained why
//      it mattered.
//
//   2. THE OUT-PARAM COUNT IS WRITTEN ON EVERY PATH, REFUSALS INCLUDED. An
//      untouched out-param carries the caller's initialiser, and a caller who
//      initialised it to the count they expected cannot tell a refusal from a
//      success. tests/test_intel_pixel.cpp section 6b is the same rule one
//      subsystem over.
//

#ifndef IntelGtImage_hpp
#define IntelGtImage_hpp

#include "IntelGtContext.hpp"

// ---------------------------------------------------------------------------
// 1. Write verdicts.
//
// A SEPARATE family from IG_GTC_IMAGE_, deliberately. Those verdicts judge the
// numbers an image would be composed FROM; these judge the act of composing it
// into a particular span, and the two fail for different reasons -- a perfectly
// admissible image cannot be written into a null pointer.
//
// The prefix is IG_GTI_WRITE_ rather than IG_GTI_, because
// intel_verdict_set_checks() in tools/check-registers.py groups families BY
// PREFIX: a bare IG_GTI_ would sweep in every unrelated constant this header
// later defines and print a contiguity failure that is correct about its own
// arithmetic and wrong about the world. handoff16 section 1 is that lesson.
// ---------------------------------------------------------------------------

#define IG_GTI_WRITE_NOT_ATTEMPTED      0u
#define IG_GTI_WRITE_OK                 1u
#define IG_GTI_WRITE_NULL_SPAN          2u
#define IG_GTI_WRITE_SPAN_TOO_SHORT     3u
#define IG_GTI_WRITE_IMAGE_REFUSED      4u
#define IG_GTI_WRITE_FIRST_PAIR_MISSING 5u
#define IG_GTI_WRITE_LIMIT              6u

static inline bool igGtiWriteSucceeded(IGU32 verdict)
{
    return verdict == IG_GTI_WRITE_OK;
}

static inline const char *igGtiWriteVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_GTI_WRITE_NOT_ATTEMPTED:  return "not attempted";
    case IG_GTI_WRITE_OK:             return "ok";
    case IG_GTI_WRITE_NULL_SPAN:      return "null span";
    case IG_GTI_WRITE_SPAN_TOO_SHORT: return "span shorter than the state page";
    case IG_GTI_WRITE_IMAGE_REFUSED:  return "image refused";
    case IG_GTI_WRITE_FIRST_PAIR_MISSING:
                                      return "no first pair supplied";
    default:                          return "unknown write verdict";
    }
}

// ---------------------------------------------------------------------------
// 2. The four ring-state pairs.
//
// This is the whole reason the layer exists: turning four register offsets and
// four values into eight dwords in the right eight slots. The offsets come from
// IntelGtCmd.hpp's ring block, which is the ONE part of this stack with a
// citation -- RENDER_RING_BASE at reference/i915/i915_reg.h:289, and PRB0_BASE
// spelled (0x2030 - 0x30), which is what fixes the ring block ABOVE the engine
// base rather than at it. The SLOTS those offsets land in are policy.
//
// igGtcPairValueSlot() is used rather than a hand-written + 1 at every site. A
// literal + 1 here would be correct today and is exactly the edit that survives
// a layout change: the slot constants move, the hand-written arithmetic does
// not, and the page still composes without a fault while programming registers
// nobody chose.
// ---------------------------------------------------------------------------

// FIVE pairs, not four, and the fifth is the reason this constant was WRONG on
// first writing. IntelGtContext.hpp puts HEAD at slot 0x04, so the pair slot at
// 0x02 -- the FIRST one, immediately below HEAD -- belongs to something else.
// An LRI header declaring four pairs while the composer skips slot 2/3 does not
// describe a page with a hole in it: the engine reads pairs CONTIGUOUSLY from
// the slot after the header, so it would take whatever happened to be sitting
// at 2/3 as the first register offset and walk the rest of the image one pair
// out of step. Every dword after that names a register nobody chose.
//
// This project cannot cite what that first pair holds. reference/i915/ has no
// LRC file at all, so the offset is NOT invented here and NOT given a name that
// would sit beside the four cited ring offsets as though it had the same
// standing. The caller supplies it, and a zero offset is REFUSED rather than
// written -- zero is the caller's initialiser, so accepting it would compose a
// page whose first restored register is whatever lives at the engine base.
#define IG_GTI_RING_PAIR_COUNT_POLICY_5     5u

// The number of dwords a whole image occupies: the NOOP, the LRI header, and
// five pairs. NOT the pair count times two -- the leading NOOP and the header
// are dwords the engine restores nothing from, and a span sized to the pairs
// alone is two dwords short.
//
// This was 10 when first written, and the suite's own stack-corruption crash is
// what measured it. Ten dwords reaches slot 9; the CTL pair lives at 10 and 11.
// So the length guard ADMITTED a span two dwords too small and then wrote past
// the end of it -- a refusal that should have fired, followed by a store into
// memory the caller never offered. Left standing here as a comment rather than
// silently corrected, because the defect and its detection are the same fact.
#define IG_GTI_RING_IMAGE_DWORDS_POLICY_12  12u

static inline IGU32 igGtiRingImageDwords(void)
{
    return IG_GTI_RING_IMAGE_DWORDS_POLICY_12;
}

// ---------------------------------------------------------------------------
// 3. The writer.
//
// dwords/dwordCount describe the caller's span. outDwordsWritten is written on
// EVERY path, refusals included -- an untouched out-param carries the caller's
// initialiser, and a caller who pre-loaded it with the count they expected
// cannot tell a refusal from a success. On every refusal it is ZERO, which is
// true rather than merely conventional: no refusal below writes a dword.
//
// REFUSAL ORDER is load-bearing and each step is judged before the one that
// would depend on it:
//
//   NULL first, because dwordCount is a promise about memory that does not
//   exist -- a span of a thousand dwords at address zero is still nothing, and
//   reporting its LENGTH as the complaint sends somebody to look at a number
//   that was never the problem.
//
//   LENGTH next, because the image verdict judges the numbers rather than the
//   destination, so an admissible image written into a span too short to hold
//   it is a defect in the CALL, not in the image.
//
//   The IMAGE verdict LAST. Reversing this pair would report a bad ring size
//   for a call that could not have written the page in any case, which is a
//   true statement about the wrong thing.
//
// The span may be LONGER than the image. A state page is 1024 dwords and this
// writer fills the first ten of them; refusing a full page for being bigger
// than the part we compose would refuse every real caller there is.
// ---------------------------------------------------------------------------

static inline IGU32 igGtiWriteRingImage(IGU32 *dwords,
                                        IGU32 dwordCount,
                                        IGU64 lrca,
                                        IGU32 ringBytes,
                                        IGU64 ringBase,
                                        IGU32 ringHead,
                                        IGU32 ringTail,
                                        IGU32 firstPairRegOffset,
                                        IGU32 firstPairValue,
                                        IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTI_WRITE_NULL_SPAN; }

    if (dwordCount < IG_GTI_RING_IMAGE_DWORDS_POLICY_12) {
        return IG_GTI_WRITE_SPAN_TOO_SHORT;
    }

    // Judged with null and length rather than with the image verdict, because
    // all three are facts about the ARGUMENT LIST -- a caller who omitted a
    // parameter. Reporting a ring-geometry complaint to that caller sends them
    // to debug numbers that were never the problem.
    if (firstPairRegOffset == 0u) { return IG_GTI_WRITE_FIRST_PAIR_MISSING; }

    // The GROUP is judged before a single dword is written. A verdict consulted
    // after the first store leaves a half-composed page behind on refusal, and
    // a half-composed page is a well-formed image describing a ring nobody
    // configured -- the quiet direction, because it programs cleanly.
    const IGU32 verdict = igGtcImageVerdict(lrca,
                                            IG_GTI_RING_PAIR_COUNT_POLICY_5,
                                            ringBytes,
                                            ringBase);
    if (!igGtcImageAdmissible(verdict)) { return IG_GTI_WRITE_IMAGE_REFUSED; }

    // Composed through the sibling's builders, never respelled here. A ring
    // control word hand-composed at this layer would agree with itself while
    // disagreeing with the header that owns the field layout.
    const IGU32 ringCtl = igGtcRingBufferControl(ringBytes);
    if (ringCtl == 0u) { return IG_GTI_WRITE_IMAGE_REFUSED; }

    // igGtMiNoop() rather than igGtMiHeader(opcode, 0) spelled out here: the
    // sibling already owns "what a NOOP word is", and a second spelling of it
    // at this layer agrees with itself while a changed opcode moves only one of
    // the two.
    dwords[IG_GTC_NOOP_SLOT_POLICY_0]       = igGtMiNoop();
    dwords[IG_GTC_LRI_HEADER_SLOT_POLICY_1] =
        igGtcLriHeader(IG_GTI_RING_PAIR_COUNT_POLICY_5);

    // The first pair, immediately below HEAD. Written from the caller's two
    // arguments and from no constant in this tree -- see the pair-count comment
    // above for why there is nothing here to cite.
    dwords[IG_GTC_FIRST_PAIR_SLOT_POLICY_2] = firstPairRegOffset;
    dwords[igGtcPairValueSlot(IG_GTC_FIRST_PAIR_SLOT_POLICY_2)] = firstPairValue;

    dwords[IG_GTC_SLOT_RING_HEAD_POLICY_0x04] = IG_GT_RING_HEAD_OFFSET_I915_0x34;
    dwords[igGtcPairValueSlot(IG_GTC_SLOT_RING_HEAD_POLICY_0x04)] = ringHead;

    dwords[IG_GTC_SLOT_RING_TAIL_POLICY_0x06] = IG_GT_RING_TAIL_OFFSET_I915_0x30;
    dwords[igGtcPairValueSlot(IG_GTC_SLOT_RING_TAIL_POLICY_0x06)] = ringTail;

    dwords[IG_GTC_SLOT_RING_START_POLICY_0x08] = IG_GT_RING_START_OFFSET_I915_0x38;
    dwords[igGtcPairValueSlot(IG_GTC_SLOT_RING_START_POLICY_0x08)] =
        (IGU32)(ringBase & 0xFFFFFFFFull);

    dwords[IG_GTC_SLOT_RING_CTL_POLICY_0x0A] = IG_GT_RING_CTL_OFFSET_I915_0x3C;
    dwords[igGtcPairValueSlot(IG_GTC_SLOT_RING_CTL_POLICY_0x0A)] = ringCtl;

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GTI_RING_IMAGE_DWORDS_POLICY_12;
    }
    return IG_GTI_WRITE_OK;
}

#endif // IntelGtImage_hpp
