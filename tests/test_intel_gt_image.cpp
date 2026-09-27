//
//  test_intel_gt_image.cpp -- drives phase0-kext/IntelGtImage.hpp, the Gen12
//  LOGICAL RING CONTEXT image WRITER.
//
//  WHAT THIS SUITE CAN AND CANNOT ESTABLISH
//  ----------------------------------------
//  Its subject has NO CITED HALF AT ALL, same as tests/test_intel_gt_context.cpp
//  and tests/test_intel_gt_exec.cpp. reference/i915/ holds seven files and all
//  seven are display; greps for LRC, CTX_DESC, RING_BUFFER_START,
//  RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP across that whole directory
//  return ZERO matches (measured 2026-08-26). A green run here says the writer
//  puts the dwords it was told to put where the sibling header says they go,
//  and says NOTHING about whether Gen12 silicon wants that page.
//
//  WHY THIS SUITE IS DIFFERENT FROM ITS SIBLINGS
//  ---------------------------------------------
//  Every other suite in the GT stack drives PURE ARITHMETIC: give it numbers,
//  read a number back. This subject MUTATES CALLER MEMORY, and that admits a
//  failure mode none of the others have -- a refusal that already wrote.
//
//  So the span is POISONED before every call with a byte pattern the writer
//  never produces, and after every refusal the WHOLE span is asserted still
//  poisoned. An assertion that only checks the return code cannot tell a
//  refusal that wrote nothing from one that composed half a page and then
//  changed its mind, and the second leaves a well-formed image behind
//  describing a ring nobody configured.
//
//  BARE LITERALS, ON PURPOSE
//  -------------------------
//  Slot indices and register offsets are pinned to BARE LITERALS rather than
//  compared against the #define the writer read. A store checked against the
//  same constant that produced it is self-cancelling: edit the #define and both
//  sides move together while the check stays green.
//

#include <cstdio>

#include "IntelGtImage.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void check(const char *label, bool ok)
{
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("  FAIL %s\n", label);
    }
}

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("  FAIL %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

// The poison value. Chosen because the writer cannot produce it at any slot:
// it is not a legal MI header (client field non-zero), not one of the four ring
// register offsets, and not a legal ring control word. A poison the subject
// could legitimately write would make "still poisoned" and "correctly written"
// indistinguishable at that slot.
static const IGU32 kPoison = 0xDEADBEEFu;

static const IGU32 kSpanDwords = 32u;

static void poison(IGU32 *span, IGU32 count)
{
    for (IGU32 i = 0u; i < count; i++) { span[i] = kPoison; }
}

static bool wholeSpanPoisoned(const IGU32 *span, IGU32 count)
{
    for (IGU32 i = 0u; i < count; i++) {
        if (span[i] != kPoison) { return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 1. Refusals, and that a refusal writes NOTHING.
//
// Each arm asserts THREE things: the verdict, that the whole span is still
// poisoned, and that the out-param reads zero. The second is the one the other
// GT suites cannot make -- a return code alone cannot tell a refusal that wrote
// nothing from one that composed part of a page and then changed its mind.
//
// The out-param is PRE-LOADED with a plausible success count rather than with
// zero. Pre-loading it with zero would make "the writer zeroed it" and "the
// writer never touched it" indistinguishable, which is the exact vacuity this
// check exists to rule out.
// ---------------------------------------------------------------------------

static const IGU64 kGoodLrca = 0x00100000ull;
static const IGU64 kGoodRing = 0x00200000ull;
static const IGU32 kGoodBytes = 0x1000u;

// The first pair's register offset and value. This project has NOTHING to cite
// for what belongs here -- reference/i915/ has no LRC file at all -- so the
// suite supplies an arbitrary non-zero offset and a distinctive value rather
// than pretending a constant. 0x244 is used because it is recognisably not one
// of the four cited ring offsets (0x30/0x34/0x38/0x3C) and so a writer that
// stored a ring offset here reddens rather than colliding.
static const IGU32 kFirstPairOffset = 0x244u;
static const IGU32 kFirstPairValue  = 0xA5A5A5A5u;

static void testRefusalsWriteNothing(void)
{
    IGU32 span[kSpanDwords];
    IGU32 written;

    // A null span. Judged BEFORE the length, because a count is a promise about
    // memory that does not exist -- reporting the LENGTH of a span at address
    // zero sends somebody to look at a number that was never the problem. The
    // count passed here is a LEGAL one, so a length-first order would have to
    // accept it and fall through to dereference null.
    written = 0xFFFFFFFFu;
    checkU32("null span refused",
             igGtiWriteRingImage(0, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_NULL_SPAN);
    checkU32("null span writes zero to the out-param", written, 0u);

    // A null span AND a bad count, both at once. NULL must win: a single-defect
    // input satisfies every ordering, including the wrong ones.
    written = 0xFFFFFFFFu;
    checkU32("null beats length",
             igGtiWriteRingImage(0, 0u, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_NULL_SPAN);

    // One dword short. Eleven dwords holds the NOOP, the header and four whole
    // pairs -- everything but the ring control VALUE, which is the dword that
    // ENABLES the ring. A writer that filled what fit would leave a page whose
    // four good pairs read as deliberate while the ring never starts.
    //
    // This exact call is what CRASHED the first version of this suite, and the
    // crash is what measured the image size: the guard was written against 10
    // while the CTL pair lives at slots 10 and 11, so an 11-dword span was
    // ADMITTED and then written two dwords past its end. A refusal that should
    // have fired, followed by a store into memory the caller never offered.
    poison(span, kSpanDwords);
    written = 7u;
    checkU32("span one dword short refused",
             igGtiWriteRingImage(span, 11u, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_SPAN_TOO_SHORT);
    check("short span is untouched", wholeSpanPoisoned(span, kSpanDwords));
    checkU32("short span writes zero to the out-param", written, 0u);

    // Ten dwords: the size this header originally claimed a whole image was.
    // Pinned as its own refusal so a regression to that constant reddens here
    // rather than corrupting a caller's stack the way it corrupted this suite's.
    poison(span, kSpanDwords);
    checkU32("a ten-dword span is refused",
             igGtiWriteRingImage(span, 10u, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_SPAN_TOO_SHORT);
    check("the ten-dword span is untouched",
          wholeSpanPoisoned(span, kSpanDwords));

    poison(span, kSpanDwords);
    checkU32("zero-dword span refused",
             igGtiWriteRingImage(span, 0u, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_SPAN_TOO_SHORT);
    check("zero-dword span is untouched", wholeSpanPoisoned(span, kSpanDwords));

    // A span too short AND an inadmissible image. LENGTH must win: the image
    // verdict judges the numbers rather than the destination, and an image that
    // could not have been written into this span in any case is a true
    // complaint about the wrong thing.
    poison(span, kSpanDwords);
    checkU32("length beats the image verdict",
             igGtiWriteRingImage(span, 4u, 0x00100001ull, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_SPAN_TOO_SHORT);
    check("span short AND image bad is untouched",
          wholeSpanPoisoned(span, kSpanDwords));

    // A missing first pair. Zero is the caller's initialiser, so accepting it
    // would compose a page whose first restored register is whatever lives at
    // the engine base -- a page that programs cleanly and drives the engine
    // from a register nobody chose.
    poison(span, kSpanDwords);
    written = 12u;
    checkU32("a zero first-pair offset is refused",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u, 0u, kFirstPairValue,
                                 &written),
             IG_GTI_WRITE_FIRST_PAIR_MISSING);
    check("a missing first pair writes nothing",
          wholeSpanPoisoned(span, kSpanDwords));
    checkU32("a missing first pair zeroes the out-param", written, 0u);

    // A zero first-pair VALUE is legal -- plenty of registers are legitimately
    // restored to zero, and refusing it would make a whole class of contexts
    // uncomposable. Only the OFFSET carries the missing-argument meaning.
    poison(span, kSpanDwords);
    checkU32("a zero first-pair value is accepted",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u, kFirstPairOffset, 0u,
                                 &written),
             IG_GTI_WRITE_OK);

    // A missing first pair AND a bad image. The ARGUMENT LIST wins: a caller
    // who omitted a parameter should not be sent to debug ring geometry.
    poison(span, kSpanDwords);
    checkU32("the argument list beats the image verdict",
             igGtiWriteRingImage(span, kSpanDwords, 0x00100001ull, kGoodBytes,
                                 kGoodRing, 0u, 0u, 0u, kFirstPairValue,
                                 &written),
             IG_GTI_WRITE_FIRST_PAIR_MISSING);
    check("missing pair AND bad image writes nothing",
          wholeSpanPoisoned(span, kSpanDwords));

    // Each arm of igGtcImageVerdict() reaching this writer. Not a single
    // representative refusal: the writer collapses all of them onto one code,
    // so nothing else establishes that each one actually ARRIVES here rather
    // than being accepted and written.
    poison(span, kSpanDwords);
    written = 12u;
    checkU32("unaligned LRCA refused",
             igGtiWriteRingImage(span, kSpanDwords, 0x00100001ull, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_IMAGE_REFUSED);
    check("unaligned LRCA writes nothing", wholeSpanPoisoned(span, kSpanDwords));
    checkU32("unaligned LRCA zeroes the out-param", written, 0u);

    poison(span, kSpanDwords);
    checkU32("LRCA above the descriptor field refused",
             igGtiWriteRingImage(span, kSpanDwords, 0x100000000ull, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_IMAGE_REFUSED);
    check("out-of-range LRCA writes nothing",
          wholeSpanPoisoned(span, kSpanDwords));

    poison(span, kSpanDwords);
    checkU32("unaligned ring size refused",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, 0x1001u,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_IMAGE_REFUSED);
    check("unaligned ring size writes nothing",
          wholeSpanPoisoned(span, kSpanDwords));

    poison(span, kSpanDwords);
    checkU32("oversized ring refused",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, 0x201000u,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_IMAGE_REFUSED);
    check("oversized ring writes nothing", wholeSpanPoisoned(span, kSpanDwords));

    poison(span, kSpanDwords);
    checkU32("unmapped ring base refused",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 0x00200001ull, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_IMAGE_REFUSED);
    check("unmapped ring base writes nothing",
          wholeSpanPoisoned(span, kSpanDwords));

    // A NULL out-param is legal. A caller who does not want the count should
    // not have to invent storage for it, and a writer that faulted on zero here
    // would make the parameter mandatory in fact while optional in signature.
    poison(span, kSpanDwords);
    checkU32("a null out-param is accepted on the success path",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, 0),
             IG_GTI_WRITE_OK);
    checkU32("a null out-param is accepted on a refusal path",
             igGtiWriteRingImage(0, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, 0),
             IG_GTI_WRITE_NULL_SPAN);
}

// ---------------------------------------------------------------------------
// 2. The success path, dword by dword.
//
// Every slot is pinned to a BARE LITERAL index holding a BARE LITERAL value.
// Comparing dwords[IG_GTC_SLOT_RING_HEAD_POLICY_0x04] against
// IG_GT_RING_HEAD_OFFSET_I915_0x34 would be self-cancelling twice over -- both
// the index and the value would move with the #defines the writer read.
//
// The four register offsets are asserted PAIRWISE DISTINCT and each is asserted
// to sit at its own slot, because a copy-paste that programs HEAD's offset into
// TAIL's slot composes a page that is well-formed, boots, and drives the ring
// from a register nobody chose. That is handoff15's byte-swapped PLANE_SIZE
// hazard in a fourth place: it programs cleanly.
// ---------------------------------------------------------------------------

static void testSuccessPath(void)
{
    IGU32 span[kSpanDwords];
    IGU32 written = 0u;

    poison(span, kSpanDwords);
    checkU32("a well-formed image writes",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, 0x2000u,
                                 kGoodRing, 0x40u, 0x80u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_OK);
    check("and the predicate agrees", igGtiWriteSucceeded(IG_GTI_WRITE_OK));
    checkU32("twelve dwords reported", written, 12u);
    checkU32("and the header agrees with the report",
             igGtiRingImageDwords(), 12u);

    // Slot 0: the NOOP. Opcode 0 at bit 23 with client 0 and length 0 is the
    // all-zero word, which is exactly why it must be checked against a literal
    // -- a writer that skipped slot 0 entirely would leave poison there, and a
    // writer that wrote garbage would not read as zero.
    checkU32("slot 0 holds the NOOP", span[0], 0x00000000u);

    // Slot 1: MI_LOAD_REGISTER_IMM, opcode 0x22 at bit 23, low byte 2N-1.
    // FIVE pairs is ten dwords, so 0x09.
    //
    // The literal is 0x11000009 and not 0x11000007, and that difference is the
    // whole of the first-pair defect read off the header word. A header
    // declaring FOUR pairs while the composer skips slots 2/3 does not describe
    // a page with a hole in it -- the engine reads pairs CONTIGUOUSLY from the
    // slot after the header, so it would take whatever sat at 2/3 as the first
    // register offset and walk the rest of the image one pair out of step.
    checkU32("slot 1 holds the LRI header for five pairs",
             span[1], 0x11000009u);

    // Slots 2 and 3 are the FIRST pair, and they are the reason the header word
    // above says five rather than four. Both come from the caller, because this
    // tree has nothing to cite for what belongs here.
    checkU32("slot 2 holds the first pair's register offset",
             span[2], 0x244u);
    checkU32("slot 3 holds the first pair's value", span[3], 0xA5A5A5A5u);

    checkU32("slot 4 holds the HEAD register offset",  span[4], 0x34u);
    checkU32("slot 5 holds the HEAD value",            span[5], 0x40u);
    checkU32("slot 6 holds the TAIL register offset",  span[6], 0x30u);
    checkU32("slot 7 holds the TAIL value",            span[7], 0x80u);
    checkU32("slot 8 holds the START register offset", span[8], 0x38u);
    checkU32("slot 9 holds the ring base low half",    span[9], 0x00200000u);

    // Ten dwords are written, so slots 10 and 11 carry the CTL pair. The
    // reported count of 10 and the CTL pair at 10/11 are the same claim read
    // two ways, and they disagree if the count is a hand-written constant
    // rather than a measurement of what was placed.
    checkU32("slot 10 holds the CTL register offset", span[10], 0x3Cu);
    checkU32("slot 11 holds the ring control word",   span[11], 0x00001001u);

    // Everything above the image is untouched. A writer that ran past its own
    // reported count scribbles on whatever the caller put after the page, and
    // the reported count is the only thing telling the caller where that is.
    // This is the assertion the FIRST version of this suite could not make,
    // because it believed the image ended at slot 9 -- so the two dwords the
    // writer really wrote landed outside the span and MSVC's stack check found
    // them before any assertion here could.
    for (IGU32 i = 12u; i < kSpanDwords; i++) {
        checkU32("nothing is written past the image", span[i], kPoison);
    }

    // A DIFFERENT ring size must move exactly one dword. Driving one geometry
    // only would leave a writer that ignores ringBytes and emits a constant
    // control word fully green.
    poison(span, kSpanDwords);
    checkU32("a 16-page ring writes",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, 0x10000u,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_OK);
    checkU32("and the control word follows the size", span[11], 0x0000F001u);
    checkU32("while the START offset does not", span[8], 0x38u);

    // A ring base in the upper half of the 32-bit range, to pin that the low
    // half is TRUNCATED rather than shifted or sign-extended. RING_BUFFER_START
    // is a 32-bit register and the high half of a 64-bit base does not fit in
    // it; a writer that shifted instead would program a base 32 bits away.
    poison(span, kSpanDwords);
    checkU32("a high ring base writes",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 0xFFFFF000ull, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_OK);
    checkU32("the START value is the low half of the base",
             span[9], 0xFFFFF000u);

    // HEAD and TAIL are pass-through. Distinct non-zero values, because equal
    // ones are satisfied by a writer that stores one parameter twice.
    poison(span, kSpanDwords);
    checkU32("distinct head and tail write",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0x11223344u, 0x55667788u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_OK);
    checkU32("HEAD carries its own argument", span[5], 0x11223344u);
    checkU32("TAIL carries its own argument", span[7], 0x55667788u);

    // The first pair is pass-through too, and it is driven with values that
    // collide with NOTHING else in the image -- a writer storing a ring offset
    // into slot 2, or the first pair's value into a ring slot, reddens.
    poison(span, kSpanDwords);
    checkU32("a distinct first pair writes",
             igGtiWriteRingImage(span, kSpanDwords, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u, 0x2098u, 0x0BADF00Du,
                                 &written),
             IG_GTI_WRITE_OK);
    checkU32("the first pair's offset is the caller's", span[2], 0x2098u);
    checkU32("the first pair's value is the caller's",  span[3], 0x0BADF00Du);
    checkU32("and HEAD's offset is unaffected by it",   span[4], 0x34u);

    // Exactly twelve dwords, so a span of exactly twelve is legal. An off-by-one
    // in the length guard refuses the tightest correct caller there is -- and an
    // off-by-one the OTHER way is what corrupted this array in the first draft,
    // which is why this check drives a stack buffer rather than the roomy span.
    IGU32 tight[12];
    for (IGU32 i = 0u; i < 12u; i++) { tight[i] = kPoison; }
    checkU32("a span of exactly twelve dwords is accepted",
             igGtiWriteRingImage(tight, 12u, kGoodLrca, kGoodBytes,
                                 kGoodRing, 0u, 0u,
                                 kFirstPairOffset, kFirstPairValue, &written),
             IG_GTI_WRITE_OK);
    checkU32("and it reports twelve", written, 12u);
    checkU32("and its last dword is the control word", tight[11], 0x00000001u);
}

// ---------------------------------------------------------------------------
// 3. Write verdict names.
//
// The loop is bounded on the family's OWN limit rather than on a typed count,
// so a verdict added to the family without a name reddens on the commit that
// adds it -- handoff20 section 1, where IG_MODE_SRC_MISMATCH published the
// unknown string into the IORegistry with all four gates green.
// ---------------------------------------------------------------------------

static bool sameString(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) { return false; }
        a++;
        b++;
    }
    return *a == *b;
}

static void testWriteVerdictNames(void)
{
    // The sentinel is read out of the SUBJECT rather than guessed from a
    // sibling function: a comparison against a string the switch can never
    // return counts every arm as named unconditionally.
    const char *unknown = igGtiWriteVerdictName(IG_GTI_WRITE_LIMIT);

    for (IGU32 v = 0u; v < IG_GTI_WRITE_LIMIT; v++) {
        check("every write verdict below the limit has its own name",
              !sameString(igGtiWriteVerdictName(v), unknown));
    }

    for (IGU32 a = 0u; a < IG_GTI_WRITE_LIMIT; a++) {
        for (IGU32 b = a + 1u; b < IG_GTI_WRITE_LIMIT; b++) {
            check("write verdict names are pairwise distinct",
                  !sameString(igGtiWriteVerdictName(a),
                              igGtiWriteVerdictName(b)));
        }
    }

    check("one past the limit answers the sentinel",
          sameString(igGtiWriteVerdictName(IG_GTI_WRITE_LIMIT), unknown));
    check("all-ones answers the sentinel",
          sameString(igGtiWriteVerdictName(0xFFFFFFFFu), unknown));

    // NOT_ATTEMPTED is the caller's initialiser and must never read as success,
    // and the predicate is an EQUALITY against OK rather than a negation of any
    // one arm -- so a verdict added later reads as failure until somebody says
    // otherwise.
    check("NOT_ATTEMPTED is not success",
          !igGtiWriteSucceeded(IG_GTI_WRITE_NOT_ATTEMPTED));
    for (IGU32 v = 0u; v < IG_GTI_WRITE_LIMIT; v++) {
        if (v == IG_GTI_WRITE_OK) { continue; }
        check("every non-OK write verdict is a failure", !igGtiWriteSucceeded(v));
    }
}

int main(void)
{
    std::printf("\ntest_intel_gt_image -- Gen12 logical ring context image writer\n");

    testRefusalsWriteNothing();
    testSuccessPath();
    testWriteVerdictNames();

    // The caveat prints BEFORE the count, deliberately. A suite whose final
    // line is prose reads as a finding rather than as a summary, and
    // tests/test_check_count_last_line.py is the gate that pins this ordering.
    std::printf("\nEVERY constant under test is POLICY. No i915 citation exists\n");
    std::printf("for any of it, and no MMIO evidence does either. A green run\n");
    std::printf("here says the writer is self-consistent, nothing more.\n");

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
