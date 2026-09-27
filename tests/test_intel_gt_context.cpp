//
//  test_intel_gt_context.cpp -- drives phase0-kext/IntelGtContext.hpp, the Gen12
//  LOGICAL RING CONTEXT image layer.
//
//  WHAT THIS SUITE CAN AND CANNOT ESTABLISH
//  ----------------------------------------
//  Its subject has NO CITED HALF AT ALL. reference/i915/ holds seven files and
//  all seven are display; greps for LRC, CTX_DESC, RING_BUFFER_START,
//  RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP across that whole directory
//  return ZERO matches (measured 2026-08-26). So every constant under test is
//  POLICY, and this suite proves the header's ARITHMETIC is self-consistent and
//  its REFUSALS fire in the documented order. It proves nothing whatsoever about
//  Gen12 silicon, and a green run here must not be read as one.
//
//  That is a weaker evidence story than every display suite in this tree, and it
//  is the same one tests/test_intel_gt_exec.cpp carries. It is written here
//  rather than left implicit because a check count is a number and a number
//  reads as confidence.
//
//  BARE LITERALS, ON PURPOSE
//  -------------------------
//  Slot indices, shifts and masks are pinned to BARE LITERALS below rather than
//  compared against the #define they came from. A decode checked against the
//  same constant the accessor reads is self-cancelling: edit the #define and
//  both sides move together while the check stays green. The literal is the
//  second opinion.
//

#include <cstdio>

#include "IntelGtContext.hpp"

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

// A separate helper rather than a cast into checkU32(): an LRCA is 64 bits and
// truncating one to compare it would make the two most interesting inputs --
// an address just above the descriptor field and one a page higher -- compare
// EQUAL. That is the exact class of bug the range predicate exists to catch.
static void checkU64(const char *label, IGU64 got, IGU64 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("  FAIL %s: got 0x%016llX want 0x%016llX\n",
                    label, (unsigned long long)got, (unsigned long long)want);
    }
}

// ---------------------------------------------------------------------------
// 1. Slot addressing.
//
// Bare literals throughout: 1024 dwords in a 4 KiB page, 4 bytes per slot,
// 0x1000 from the LRCA to the state page. Every one of those is a #define in
// the subject, and comparing against the #define would be self-cancelling.
// ---------------------------------------------------------------------------

static void testSlotAddressing(void)
{
    checkU32("slot count is one page of dwords", igGtcSlotCount(), 1024u);

    check("slot 0 is in the page",             igGtcSlotInPage(0u));
    check("slot 1023 is in the page",          igGtcSlotInPage(1023u));
    check("slot 1024 is NOT in the page",     !igGtcSlotInPage(1024u));
    check("all-ones slot is NOT in the page", !igGtcSlotInPage(0xFFFFFFFFu));

    checkU32("slot 0 offset",    igGtcSlotOffset(0u),    0x000u);
    checkU32("slot 1 offset",    igGtcSlotOffset(1u),    0x004u);
    checkU32("slot 1023 offset", igGtcSlotOffset(1023u), 0xFFCu);

    // ALL-ONES refusal here, ZERO one layer down. Not an inconsistency: zero is
    // a LEGAL slot offset -- the leading NOOP lives at slot 0 -- so zero cannot
    // carry the meaning "refused" in this family.
    checkU32("slot 1024 refused", igGtcSlotOffset(1024u),       0xFFFFFFFFu);
    checkU32("huge slot refused", igGtcSlotOffset(0x40000000u), 0xFFFFFFFFu);

    checkU32("slot 0 from LRCA",    igGtcSlotOffsetFromLrca(0u),    0x1000u);
    checkU32("slot 1 from LRCA",    igGtcSlotOffsetFromLrca(1u),    0x1004u);
    checkU32("slot 1023 from LRCA", igGtcSlotOffsetFromLrca(1023u), 0x1FFCu);

    // The refusal must PROPAGATE rather than have 0x1000 added to it. Adding
    // wraps to 0xFFF, which is small, 4-byte aligned, and inside every
    // containment test in this header -- a refusal that reads as an answer.
    checkU32("refusal propagates rather than being offset",
             igGtcSlotOffsetFromLrca(1024u), 0xFFFFFFFFu);

    checkU64("state page one page above the LRCA",
             igGtcStatePageAddress(0x00100000ull), 0x00101000ull);
    checkU64("unaligned LRCA refuses to zero",
             igGtcStatePageAddress(0x00100001ull), 0ull);
    checkU64("all-ones LRCA refuses to zero",
             igGtcStatePageAddress(0xFFFFFFFFFFFFFFFFull), 0ull);
}

// ---------------------------------------------------------------------------
// 2. The load-immediate header.
//
// Opcode 0x22 at bit 23 with client 0 gives 0x11000000; the low byte carries
// 2N-1. Both halves are pinned as literals, so a shift that moves in the
// sibling reddens here rather than moving silently on both sides.
// ---------------------------------------------------------------------------

static void testLriHeader(void)
{
    check("zero pairs refused",  !igGtcPairCountFits(0u));
    check("one pair fits",        igGtcPairCountFits(1u));
    check("128 pairs fits",       igGtcPairCountFits(128u));   // 2N-1 == 255
    check("129 pairs refused",   !igGtcPairCountFits(129u));   // 2N-1 == 257
    check("0x1FF pairs refused", !igGtcPairCountFits(0x1FFu));

    checkU32("header for 1 pair",    igGtcLriHeader(1u),   0x11000001u);
    checkU32("header for 4 pairs",   igGtcLriHeader(4u),   0x11000007u);
    checkU32("header for 128 pairs", igGtcLriHeader(128u), 0x110000FFu);

    checkU32("0 pairs refuses to zero",   igGtcLriHeader(0u),   0u);
    checkU32("129 pairs refuses to zero", igGtcLriHeader(129u), 0u);

    checkU32("round trip 1 pair",
             igGtcLriHeaderPairs(igGtcLriHeader(1u)),   1u);
    checkU32("round trip 4 pairs",
             igGtcLriHeaderPairs(igGtcLriHeader(4u)),   4u);
    checkU32("round trip 128 pairs",
             igGtcLriHeaderPairs(igGtcLriHeader(128u)), 128u);

    // Composed through the SIBLING's header builder, so an opcode field that
    // moves there reddens here instead of agreeing with itself.
    checkU32("NOOP is not an LRI",
             igGtcLriHeaderPairs(igGtMiHeader(0x00u, 7u)), 0u);
    checkU32("BB_END is not an LRI",
             igGtcLriHeaderPairs(igGtMiHeader(0x0Au, 0u)), 0u);
    checkU32("the refusal sentinel is not an LRI",
             igGtcLriHeaderPairs(0u), 0u);

    // An ODD dword count is not a whole number of pairs. Refused rather than
    // rounded: rounding reports a count the engine will not restore.
    checkU32("odd dword count refused",
             igGtcLriHeaderPairs(igGtMiHeader(0x22u, 6u)), 0u);
}

// ---------------------------------------------------------------------------
// 3. Ring state.
//
// The four slot indices are the meaning -- a pair written to the wrong slot
// programs a different register with a perfectly valid value -- so each is
// pinned to its own literal and all four are asserted pairwise distinct. A
// copy-paste slot constant satisfies every "has a slot" assertion there is.
// ---------------------------------------------------------------------------

static void testRingState(void)
{
    // CORRECTED 2026-08-26 alongside the header: these were 0x02/0x04/0x06/0x08,
    // the layout you get with the LRI header at slot 0 and no leading NOOP.
    checkU32("HEAD slot",  IG_GTC_SLOT_RING_HEAD_POLICY_0x04,  0x04u);
    checkU32("TAIL slot",  IG_GTC_SLOT_RING_TAIL_POLICY_0x06,  0x06u);
    checkU32("START slot", IG_GTC_SLOT_RING_START_POLICY_0x08, 0x08u);
    checkU32("CTL slot",   IG_GTC_SLOT_RING_CTL_POLICY_0x0A,   0x0Au);

    check("HEAD != TAIL",
          IG_GTC_SLOT_RING_HEAD_POLICY_0x04 != IG_GTC_SLOT_RING_TAIL_POLICY_0x06);
    check("HEAD != START",
          IG_GTC_SLOT_RING_HEAD_POLICY_0x04 != IG_GTC_SLOT_RING_START_POLICY_0x08);
    check("HEAD != CTL",
          IG_GTC_SLOT_RING_HEAD_POLICY_0x04 != IG_GTC_SLOT_RING_CTL_POLICY_0x0A);
    check("TAIL != START",
          IG_GTC_SLOT_RING_TAIL_POLICY_0x06 != IG_GTC_SLOT_RING_START_POLICY_0x08);
    check("TAIL != CTL",
          IG_GTC_SLOT_RING_TAIL_POLICY_0x06 != IG_GTC_SLOT_RING_CTL_POLICY_0x0A);
    check("START != CTL",
          IG_GTC_SLOT_RING_START_POLICY_0x08 != IG_GTC_SLOT_RING_CTL_POLICY_0x0A);

    check("HEAD slot in page",  igGtcSlotInPage(IG_GTC_SLOT_RING_HEAD_POLICY_0x04));
    check("TAIL slot in page",  igGtcSlotInPage(IG_GTC_SLOT_RING_TAIL_POLICY_0x06));
    check("START slot in page", igGtcSlotInPage(IG_GTC_SLOT_RING_START_POLICY_0x08));
    check("CTL slot in page",   igGtcSlotInPage(IG_GTC_SLOT_RING_CTL_POLICY_0x0A));

    // Every ring-state slot is an OFFSET dword at or above the first pair slot,
    // so each must be EVEN and none may collide with the NOOP at 0 or the LRI
    // header at 1. An odd value here would name a VALUE dword as though it were
    // an offset -- the off-by-one that programs a register nobody chose.
    check("HEAD is a pair offset slot",
          IG_GTC_SLOT_RING_HEAD_POLICY_0x04 >= IG_GTC_FIRST_PAIR_SLOT_POLICY_2 &&
          (IG_GTC_SLOT_RING_HEAD_POLICY_0x04 % 2u) == 0u);
    check("TAIL is a pair offset slot",
          IG_GTC_SLOT_RING_TAIL_POLICY_0x06 >= IG_GTC_FIRST_PAIR_SLOT_POLICY_2 &&
          (IG_GTC_SLOT_RING_TAIL_POLICY_0x06 % 2u) == 0u);
    check("START is a pair offset slot",
          IG_GTC_SLOT_RING_START_POLICY_0x08 >= IG_GTC_FIRST_PAIR_SLOT_POLICY_2 &&
          (IG_GTC_SLOT_RING_START_POLICY_0x08 % 2u) == 0u);
    check("CTL is a pair offset slot",
          IG_GTC_SLOT_RING_CTL_POLICY_0x0A >= IG_GTC_FIRST_PAIR_SLOT_POLICY_2 &&
          (IG_GTC_SLOT_RING_CTL_POLICY_0x0A % 2u) == 0u);

    // The four value dwords, pinned to BARE LITERALS rather than to slot + 1.
    // Deriving the expectation the same way the accessor derives the answer is
    // self-cancelling: both sides move together and the check stays green.
    checkU32("HEAD value slot",
             igGtcPairValueSlot(IG_GTC_SLOT_RING_HEAD_POLICY_0x04),  0x05u);
    checkU32("TAIL value slot",
             igGtcPairValueSlot(IG_GTC_SLOT_RING_TAIL_POLICY_0x06),  0x07u);
    checkU32("START value slot",
             igGtcPairValueSlot(IG_GTC_SLOT_RING_START_POLICY_0x08), 0x09u);
    checkU32("CTL value slot",
             igGtcPairValueSlot(IG_GTC_SLOT_RING_CTL_POLICY_0x0A),   0x0Bu);

    // The NOOP and the LRI header do not begin pairs, so neither has a value
    // dword. Refused rather than answered: slot 1 + 1 is 2, which is the FIRST
    // PAIR's offset dword -- a perfectly valid slot, and completely wrong.
    checkU32("NOOP slot has no value dword",
             igGtcPairValueSlot(IG_GTC_NOOP_SLOT_POLICY_0),       0xFFFFFFFFu);
    checkU32("LRI header slot has no value dword",
             igGtcPairValueSlot(IG_GTC_LRI_HEADER_SLOT_POLICY_1), 0xFFFFFFFFu);

    checkU32("first pair slot has a value dword",
             igGtcPairValueSlot(IG_GTC_FIRST_PAIR_SLOT_POLICY_2), 0x03u);

    // The last slot in the page holds an offset dword whose value dword would
    // fall off the end. Refused: a pair the page cannot hold is not a pair, and
    // wrapping to slot 0 would name the NOOP as a register value.
    checkU32("last slot cannot begin a pair",
             igGtcPairValueSlot(1023u), 0xFFFFFFFFu);
    checkU32("second-to-last slot can",
             igGtcPairValueSlot(1022u), 1023u);
    checkU32("out-of-page offset slot refused",
             igGtcPairValueSlot(1024u), 0xFFFFFFFFu);
    checkU32("all-ones offset slot refused",
             igGtcPairValueSlot(0xFFFFFFFFu), 0xFFFFFFFFu);

    // (pages - 1) at bit 12, enable at bit 0. One page: 0 << 12 | 1.
    checkU32("one-page ring control",  igGtcRingBufferControl(0x1000u),   0x00000001u);
    checkU32("two-page ring control",  igGtcRingBufferControl(0x2000u),   0x00001001u);
    checkU32("16-page ring control",   igGtcRingBufferControl(0x10000u),  0x0000F001u);
    checkU32("max ring control",       igGtcRingBufferControl(0x200000u), 0x001FF001u);

    // Geometry is judged by the sibling that OWNS it, so these three refusals
    // are igGtRingSizeVerdict()'s three arms reaching this composer.
    checkU32("unaligned ring refuses to zero",
             igGtcRingBufferControl(0x1001u), 0u);
    checkU32("undersized ring refuses to zero",
             igGtcRingBufferControl(0x800u), 0u);
    checkU32("oversized ring refuses to zero",
             igGtcRingBufferControl(0x201000u), 0u);
    checkU32("zero-byte ring refuses to zero",
             igGtcRingBufferControl(0u), 0u);

    checkU32("round trip one page",
             igGtcRingControlBytes(igGtcRingBufferControl(0x1000u)),   0x1000u);
    checkU32("round trip 16 pages",
             igGtcRingControlBytes(igGtcRingBufferControl(0x10000u)),  0x10000u);
    checkU32("round trip max",
             igGtcRingControlBytes(igGtcRingBufferControl(0x200000u)), 0x200000u);

    // A DISABLED word reads back as zero bytes. The length field of a ring the
    // engine is not using is not a measurement of anything, and reporting a
    // size for it would describe a ring that is not running.
    checkU32("disabled word reads back zero bytes",
             igGtcRingControlBytes(0x0000F000u), 0u);
    checkU32("the refusal sentinel reads back zero bytes",
             igGtcRingControlBytes(0u), 0u);
}

// ---------------------------------------------------------------------------
// 4. The whole-image verdict.
//
// ARM ORDER is the subject here, and an order claim is only assertable with an
// input bad in TWO ways at once -- a single-defect input satisfies every
// ordering, including the wrong ones.
// ---------------------------------------------------------------------------

static void testImageVerdict(void)
{
    const IGU64 goodLrca = 0x00100000ull;
    const IGU64 goodRing = 0x00200000ull;

    checkU32("a well-formed image is admissible",
             igGtcImageVerdict(goodLrca, 4u, 0x1000u, goodRing),
             IG_GTC_IMAGE_OK);
    check("and the predicate agrees",
          igGtcImageAdmissible(igGtcImageVerdict(goodLrca, 4u, 0x1000u, goodRing)));

    checkU32("LRCA above the descriptor field",
             igGtcImageVerdict(0x100000000ull, 4u, 0x1000u, goodRing),
             IG_GTC_IMAGE_LRCA_RANGE);
    checkU32("unaligned LRCA",
             igGtcImageVerdict(0x00100001ull, 4u, 0x1000u, goodRing),
             IG_GTC_IMAGE_LRCA_UNALIGNED);

    // BOTH out of range AND unaligned. Range must win: complaining about the
    // alignment of an address the descriptor field cannot hold is not a
    // meaningful complaint about it.
    checkU32("range beats alignment",
             igGtcImageVerdict(0x100000001ull, 4u, 0x1000u, goodRing),
             IG_GTC_IMAGE_LRCA_RANGE);

    checkU32("pair count of zero",
             igGtcImageVerdict(goodLrca, 0u, 0x1000u, goodRing),
             IG_GTC_IMAGE_PAIR_COUNT);
    checkU32("pair count the header cannot express",
             igGtcImageVerdict(goodLrca, 129u, 0x1000u, goodRing),
             IG_GTC_IMAGE_PAIR_COUNT);

    // Bad pair count AND bad ring. The IMAGE wins, because a bad pair count is
    // a defect in the image this driver composed while the ring is a separate
    // allocation -- report the one that is provably ours.
    checkU32("pair count beats ring geometry",
             igGtcImageVerdict(goodLrca, 0u, 0x1001u, goodRing),
             IG_GTC_IMAGE_PAIR_COUNT);

    checkU32("ring geometry refused",
             igGtcImageVerdict(goodLrca, 4u, 0x1001u, goodRing),
             IG_GTC_IMAGE_RING_REFUSED);
    checkU32("ring base not a usable page",
             igGtcImageVerdict(goodLrca, 4u, 0x1000u, 0x00200001ull),
             IG_GTC_IMAGE_RING_UNMAPPED);

    // Bad ring SIZE and bad ring BASE. Geometry wins: a ring whose size is not
    // legal cannot usefully be described as mapped or unmapped, and a
    // mapping-first order reports the address of a ring nobody could allocate.
    checkU32("ring geometry beats ring mapping",
             igGtcImageVerdict(goodLrca, 4u, 0x1001u, 0x00200001ull),
             IG_GTC_IMAGE_RING_REFUSED);

    // NOT_ATTEMPTED is the caller's initialiser and must never be returned, so
    // no input above may produce it and it must not read as admissible.
    check("NOT_ATTEMPTED is not admissible",
          !igGtcImageAdmissible(IG_GTC_IMAGE_NOT_ATTEMPTED));

    // An EQUALITY against OK, never a negation of one arm: every refusal below
    // the family limit must read as inadmissible, including ones added later.
    for (IGU32 v = 0u; v < IG_GTC_IMAGE_LIMIT; v++) {
        if (v == IG_GTC_IMAGE_OK) { continue; }
        check("every non-OK verdict is inadmissible", !igGtcImageAdmissible(v));
    }
}

// ---------------------------------------------------------------------------
// 5. Verdict names.
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

static void testVerdictNames(void)
{
    const char *unknown = igGtcImageVerdictName(IG_GTC_IMAGE_LIMIT);

    // The sentinel is read out of the SUBJECT rather than guessed from a
    // sibling function: a comparison against a string the switch can never
    // return counts every arm as named unconditionally.
    for (IGU32 v = 0u; v < IG_GTC_IMAGE_LIMIT; v++) {
        check("every verdict below the limit has its own name",
              !sameString(igGtcImageVerdictName(v), unknown));
    }

    // Pairwise distinct. A copy-paste arm returning a sibling's string
    // satisfies every "has a name" assertion above.
    for (IGU32 a = 0u; a < IG_GTC_IMAGE_LIMIT; a++) {
        for (IGU32 b = a + 1u; b < IG_GTC_IMAGE_LIMIT; b++) {
            check("verdict names are pairwise distinct",
                  !sameString(igGtcImageVerdictName(a),
                              igGtcImageVerdictName(b)));
        }
    }

    check("one past the limit answers the sentinel",
          sameString(igGtcImageVerdictName(IG_GTC_IMAGE_LIMIT), unknown));
    check("all-ones answers the sentinel",
          sameString(igGtcImageVerdictName(0xFFFFFFFFu), unknown));
}

int main(void)
{
    std::printf("\ntest_intel_gt_context -- Gen12 logical ring context image\n");

    testSlotAddressing();
    testLriHeader();
    testRingState();
    testImageVerdict();
    testVerdictNames();

    // The caveat prints BEFORE the count, deliberately. A suite whose final
    // line is prose reads as a finding rather than as a summary, and
    // tests/test_check_count_last_line.py is the gate that pins this ordering.
    std::printf("\nEVERY constant under test is POLICY. No i915 citation exists\n");
    std::printf("for any of it, and no MMIO evidence does either. A green run\n");
    std::printf("here says the arithmetic is self-consistent, nothing more.\n");

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
