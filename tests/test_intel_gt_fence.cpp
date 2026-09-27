//
//  test_intel_gt_fence.cpp
//  Host-side suite over phase0-kext/IntelGtFence.hpp -- the completion layer.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  IntelGtFence.hpp composes the flush/store pair that would let this driver
//  learn that submitted work finished. Nothing else in the tree does. Per the
//  logic/IOKit split in CLAUDE.md, a pure header with no suite is a pure header
//  nothing runs -- intel-handoff26.md section 1 measured exactly that for
//  igSampleVerdict(), which had 421 checks over it and no callers.
//
//  THIS IS THE FIRST INSTALMENT. It covers the properties that can be wrong in a
//  way that composes a WELL-FORMED packet, which is the direction handoff35
//  section 2 names as the dangerous one -- a malformed page is caught by the
//  next thing that reads it and a well-formed wrong one is not:
//
//    1. The verdict family is total below its own _LIMIT and the names are
//       pairwise distinct. handoff20 section 1: igModeVerdictName() had arms
//       0..5 against a limit of 6 and published "unknown" for the one verdict
//       that mattered most.
//    2. igGtfFenceSucceeded() is an EQUALITY, so exactly ONE verdict satisfies
//       it. handoff34 section 1: a bool is satisfied by every refusal at once.
//    3. The refusal ORDER, arm by arm. A zero address trivially satisfies
//       alignment, so an alignment-first order makes ADDR_ZERO unreachable --
//       handoff32 section 3's dead-arm shape.
//    4. The composer writes NOTHING on any refusal. A partially composed packet
//       is worse than none: it is well-formed for its first dwords and the
//       engine would execute it.
//    5. The flush precedes the store, BY INDEX. Both orderings compose the same
//       SET of dwords, the same count, the same byte length, and both are
//       well-formed. Index is the only thing that separates them.
//
//  THE SECOND INSTALMENT landed 2026-08-30 and covers exactly what the first one
//  named as missing, which is why that list was written down rather than left
//  silent -- a suite that quietly covers less than its subject reads as a suite
//  that covers all of it:
//
//    6. The seqno compare is a WRAPPING compare. A plain `>=` is correct until
//       the 32-bit counter wraps and then reports every outstanding submission
//       complete AT ONCE. Both forms agree on every non-wrapping input, so the
//       separating cases are the ones that straddle the wrap -- and those are
//       the only cases in this file where the two spellings differ at all.
//    7. igGtfSeqnoRetired() refuses the RESERVED target rather than answering.
//       Zero means "nothing has happened yet", so `passed(observed, 0)` is TRUE
//       for every observed value under the raw compare: the reserved seqno is
//       the one target that reports retired the instant it is asked about.
//    8. igGtfNextSeqno() SKIPS the reserved value on wrap. One submission in
//       every 2^32 otherwise draws the seqno whose completion is by definition
//       unobservable, and the walk that finds it is 2^32 long.
//    9. The address round-trip is the identity for ALIGNED addresses and is NOT
//       claimed to be for others -- igGtBatchAddressLow() refuses an unaligned
//       address with a zero, so the round-trip is a property of the composer's
//       accept set rather than of arbitrary 64-bit values.
//   10. igGtfCompletionIsOrdered() has negative controls per refusal arm. It
//       returns the same `false` for a null span, a short span, a wrong flush
//       header and a wrong store header, and those are four different facts.
//
#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelGtFence.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void check(const char *label, bool condition)
{
    gChecks++;
    if (!condition) {
        gFailures++;
        std::printf("  FAIL  %s\n", label);
    }
}

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("  FAIL  %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

// A span deliberately larger than the pair, pre-filled with a poison word that
// is NOT zero -- so "the composer wrote nothing" is distinguishable from "the
// composer wrote zeroes". A zero-filled buffer cannot tell those apart, and one
// of them is a refusal working while the other is a refusal that scribbled.
static const IGU32 kPoison = 0xDEADBEEFu;
static const IGU32 kSpanDwords = 16u;

static void fillPoison(IGU32 *span)
{
    for (IGU32 i = 0; i < kSpanDwords; i++) {
        span[i] = kPoison;
    }
}

static bool spanIsUntouched(const IGU32 *span)
{
    for (IGU32 i = 0; i < kSpanDwords; i++) {
        if (span[i] != kPoison) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Property 1 -- the verdict family is total below its own limit
// ---------------------------------------------------------------------------
//
// Bounds on IG_GTF_FENCE_LIMIT rather than a typed count, so a family that
// grows reddens on the commit that grows it rather than silently publishing
// "unknown" for the new arm.
static void testVerdictNames(void)
{
    std::printf("testVerdictNames\n");

    const char *unknown = igGtfFenceVerdictName(IG_GTF_FENCE_LIMIT);
    check("the value one past the family answers the sentinel",
          std::strcmp(unknown, "unknown") == 0);

    for (IGU32 v = 0; v < IG_GTF_FENCE_LIMIT; v++) {
        const char *name = igGtfFenceVerdictName(v);
        check("every verdict below the limit has a name",
              std::strcmp(name, "unknown") != 0);
    }

    // Pairwise distinct. A copy-paste arm returning a sibling's string
    // satisfies every "has a name" assertion above -- handoff21 section 1.
    for (IGU32 a = 0; a < IG_GTF_FENCE_LIMIT; a++) {
        for (IGU32 b = a + 1; b < IG_GTF_FENCE_LIMIT; b++) {
            check("verdict names are pairwise distinct",
                  std::strcmp(igGtfFenceVerdictName(a),
                              igGtfFenceVerdictName(b)) != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Property 2 -- exactly one verdict satisfies the success predicate
// ---------------------------------------------------------------------------
static void testSucceededIsEquality(void)
{
    std::printf("testSucceededIsEquality\n");

    IGU32 accepting = 0;
    for (IGU32 v = 0; v <= IG_GTF_FENCE_LIMIT; v++) {
        if (igGtfFenceSucceeded(v)) {
            accepting++;
        }
    }
    checkU32("exactly one verdict satisfies igGtfFenceSucceeded", accepting, 1u);
    check("and it is OK", igGtfFenceSucceeded(IG_GTF_FENCE_OK));
    check("NOT_ATTEMPTED does not satisfy it",
          !igGtfFenceSucceeded(IG_GTF_FENCE_NOT_ATTEMPTED));
}

// ---------------------------------------------------------------------------
// Property 3 -- refusal order, arm by arm
// ---------------------------------------------------------------------------
//
// The separating case for each pair is an input that is wrong in BOTH ways at
// once. An input wrong in only one way is satisfied by either order.
static void testTargetVerdictOrder(void)
{
    std::printf("testTargetVerdictOrder\n");

    const IGU32 goodSeqno = 7u;
    const IGU64 goodAddr  = 0x1000u;

    checkU32("a good target is OK",
             igGtfSeqnoTargetVerdict(goodAddr, goodSeqno), IG_GTF_FENCE_OK);

    // Zero is ALSO qword-aligned. If alignment were tested first, ADDR_ZERO
    // would be unreachable and this check is what says so.
    check("zero is qword-aligned, so the arms genuinely overlap",
          igGtfSeqnoAddressAligned(0u));
    checkU32("a zero address reports ADDR_ZERO, not ADDR_UNALIGNED",
             igGtfSeqnoTargetVerdict(0u, goodSeqno), IG_GTF_FENCE_ADDR_ZERO);

    checkU32("an unaligned address reports ADDR_UNALIGNED",
             igGtfSeqnoTargetVerdict(0x1004u, goodSeqno),
             IG_GTF_FENCE_ADDR_UNALIGNED);

    checkU32("a reserved seqno reports SEQNO_RESERVED",
             igGtfSeqnoTargetVerdict(goodAddr, IG_GTF_SEQNO_RESERVED_POLICY_0),
             IG_GTF_FENCE_SEQNO_RESERVED);

    // Both wrong: address beats seqno.
    checkU32("a zero address beats a reserved seqno",
             igGtfSeqnoTargetVerdict(0u, IG_GTF_SEQNO_RESERVED_POLICY_0),
             IG_GTF_FENCE_ADDR_ZERO);
    checkU32("an unaligned address beats a reserved seqno",
             igGtfSeqnoTargetVerdict(0x1004u, IG_GTF_SEQNO_RESERVED_POLICY_0),
             IG_GTF_FENCE_ADDR_UNALIGNED);
}

// ---------------------------------------------------------------------------
// Property 4 -- every refusal writes NOTHING
// ---------------------------------------------------------------------------
static void testRefusalsWriteNothing(void)
{
    std::printf("testRefusalsWriteNothing\n");

    IGU32 span[kSpanDwords];

    checkU32("a null span refuses",
             igGtfWriteCompletion(0, kSpanDwords, 0x1000u, 7u),
             IG_GTF_FENCE_NULL_SPAN);

    // One dword short of the pair. The boundary, not an obviously tiny span:
    // a `<=` written for `<` passes every small-span test and fails only here.
    fillPoison(span);
    checkU32("a span one dword short refuses",
             igGtfWriteCompletion(span, IG_GTF_COMPLETION_DWORDS_POLICY - 1u,
                                  0x1000u, 7u),
             IG_GTF_FENCE_SPAN_TOO_SHORT);
    check("and wrote nothing", spanIsUntouched(span));

    fillPoison(span);
    checkU32("a zero address refuses",
             igGtfWriteCompletion(span, kSpanDwords, 0u, 7u),
             IG_GTF_FENCE_ADDR_ZERO);
    check("and wrote nothing", spanIsUntouched(span));

    fillPoison(span);
    checkU32("an unaligned address refuses",
             igGtfWriteCompletion(span, kSpanDwords, 0x1004u, 7u),
             IG_GTF_FENCE_ADDR_UNALIGNED);
    check("and wrote nothing", spanIsUntouched(span));

    fillPoison(span);
    checkU32("a reserved seqno refuses",
             igGtfWriteCompletion(span, kSpanDwords, 0x1000u,
                                  IG_GTF_SEQNO_RESERVED_POLICY_0),
             IG_GTF_FENCE_SEQNO_RESERVED);
    check("and wrote nothing", spanIsUntouched(span));

    // Exactly the pair's length is admissible. The other side of the boundary.
    fillPoison(span);
    checkU32("a span of exactly the pair's length is admissible",
             igGtfWriteCompletion(span, IG_GTF_COMPLETION_DWORDS_POLICY,
                                  0x1000u, 7u),
             IG_GTF_FENCE_OK);
}

// ---------------------------------------------------------------------------
// Property 5 -- the flush precedes the store, BY INDEX
// ---------------------------------------------------------------------------
//
// This is the property the whole header exists for. A flush AFTER the store
// retires nothing that matters: the CPU observes the seqno while the render
// caches still hold unwritten pixels. Both orderings compose the identical set
// of dwords and the identical length, so index is the only separator.
static void testCompositionOrdering(void)
{
    std::printf("testCompositionOrdering\n");

    IGU32 span[kSpanDwords];
    const IGU64 addr  = 0x0000000123456000ull;
    const IGU32 seqno = 0x0000002Au;

    fillPoison(span);
    checkU32("a good completion composes",
             igGtfWriteCompletion(span, kSpanDwords, addr, seqno),
             IG_GTF_FENCE_OK);

    checkU32("index 0 is the flush header",
             span[IG_GTF_IDX_FLUSH_HEADER_POLICY], igGtfFlushHeader());
    checkU32("index 5 is the store header",
             span[IG_GTF_IDX_STORE_HEADER_POLICY], igGtfStoreHeader());

    check("the two headers are distinct words",
          igGtfFlushHeader() != igGtfStoreHeader());
    check("the flush index precedes the store index",
          IG_GTF_IDX_FLUSH_HEADER_POLICY < IG_GTF_IDX_STORE_HEADER_POLICY);

    checkU32("the flush's address low operand is an explicit zero",
             span[IG_GTF_IDX_FLUSH_ADDR_LO_POLICY], 0u);
    checkU32("the flush's address high operand is an explicit zero",
             span[IG_GTF_IDX_FLUSH_ADDR_HI_POLICY], 0u);
    checkU32("the flush's immediate low operand is an explicit zero",
             span[IG_GTF_IDX_FLUSH_IMM_LO_POLICY], 0u);
    checkU32("the flush's immediate high operand is an explicit zero",
             span[IG_GTF_IDX_FLUSH_IMM_HI_POLICY], 0u);

    checkU32("the store's address low half is the low half of the target",
             span[IG_GTF_IDX_STORE_ADDR_LO_POLICY], igGtfAddressLow(addr));
    checkU32("the store's address high half is the high half of the target",
             span[IG_GTF_IDX_STORE_ADDR_HI_POLICY], igGtfAddressHigh(addr));
    checkU32("the store's value is the seqno",
             span[IG_GTF_IDX_STORE_VALUE_POLICY], seqno);

    // Nothing beyond the pair was touched. A composer writing one dword too
    // many is well-formed for the pair and scribbles on whatever follows it in
    // the ring -- which is the next submission's header.
    for (IGU32 i = IG_GTF_COMPLETION_DWORDS_POLICY; i < kSpanDwords; i++) {
        checkU32("nothing beyond the pair was written", span[i], kPoison);
    }

    check("the composed span reads as ordered",
          igGtfCompletionIsOrdered(span, kSpanDwords));

    // The separating mutation this property exists to catch: swap the two
    // headers. Same dwords, same count, same length, still well-formed --
    // and the completion now retires nothing.
    IGU32 swapped[kSpanDwords];
    std::memcpy(swapped, span, sizeof(span));
    swapped[IG_GTF_IDX_FLUSH_HEADER_POLICY] = igGtfStoreHeader();
    swapped[IG_GTF_IDX_STORE_HEADER_POLICY] = igGtfFlushHeader();
    check("a swapped pair does NOT read as ordered",
          !igGtfCompletionIsOrdered(swapped, kSpanDwords));
}

// ---------------------------------------------------------------------------
// Geometry -- the byte length is composed, not spelled
// ---------------------------------------------------------------------------
static void testGeometry(void)
{
    std::printf("testGeometry\n");

    checkU32("the pair is flush plus store",
             IG_GTF_COMPLETION_DWORDS_POLICY,
             IG_GTF_FLUSH_DWORDS_POLICY_5 + IG_GTF_STORE_DWORDS_POLICY_4);
    checkU32("the byte length is the dword count times the word size",
             IG_GTF_COMPLETION_BYTES_POLICY,
             IG_GTF_COMPLETION_DWORDS_POLICY * IG_GT_CMD_WORD_BYTES_I915_4);

    // The index layout must agree with the dword count. A tenth index added
    // without growing the count writes past the span every caller sized from
    // the count.
    checkU32("the last index is one below the dword count",
             IG_GTF_IDX_STORE_VALUE_POLICY,
             IG_GTF_COMPLETION_DWORDS_POLICY - 1u);
}

// ---------------------------------------------------------------------------
// Property 6 -- the seqno compare WRAPS
// ---------------------------------------------------------------------------
//
// A plain `observed >= target` is correct for every input that does not straddle
// the wrap, which is why a suite built from small numbers cannot tell the two
// spellings apart. The signed-difference form is correct on both sides. The
// separating cases below are the only ones in this file where the two forms
// disagree, and the failure they guard against is the worst available: after the
// counter wraps, a raw compare reports EVERY outstanding submission complete at
// once, which is a fence that lies in the direction of proceeding.
static void testSeqnoPassedWraps(void)
{
    std::printf("testSeqnoPassedWraps\n");

    // The non-wrapping half. Both spellings agree here; these pin that the
    // wrapping form did not break the ordinary case to fix the hard one.
    check("an observed seqno equal to the target has passed",
          igGtfSeqnoPassed(7u, 7u));
    check("an observed seqno above the target has passed",
          igGtfSeqnoPassed(8u, 7u));
    check("an observed seqno below the target has NOT passed",
          !igGtfSeqnoPassed(6u, 7u));

    // The wrap. Target sits just below the rollover, observed has wrapped past
    // it. A raw `>=` reads 1 >= 0xFFFFFFFE as false and reports work that has
    // finished as outstanding -- the timeout direction, which stalls.
    const IGU32 nearTop = 0xFFFFFFFEu;
    check("an observed seqno that wrapped PAST the target has passed",
          igGtfSeqnoPassed(1u, nearTop));
    check("and so has the value exactly at the wrap",
          igGtfSeqnoPassed(0u, nearTop));

    // The other side of the same wrap, and the dangerous direction: target has
    // wrapped and observed has not yet reached it. A raw `>=` reads
    // 0xFFFFFFFE >= 1 as TRUE and reports work that has not run as complete.
    check("an observed seqno BEFORE a wrapped target has not passed",
          !igGtfSeqnoPassed(nearTop, 1u));
    check("nor has one a single step short of it",
          !igGtfSeqnoPassed(0u, 1u));

    // The half-space boundary. A signed-difference compare splits the ring in
    // half, so this is where its own arithmetic changes answer -- the point a
    // rewrite is most likely to move by one.
    const IGU32 half = 0x80000000u;
    check("one below the half-space is still ahead",
          igGtfSeqnoPassed(half - 1u, 0u));
    check("and the half-space itself reads as behind",
          !igGtfSeqnoPassed(half, 0u));
}

// ---------------------------------------------------------------------------
// Property 7 -- the retired predicate refuses the reserved target
// ---------------------------------------------------------------------------
//
// Zero is the "nothing has happened yet" value, so under the bare compare
// igGtfSeqnoPassed(anything, 0) is TRUE. A retired test that forwarded straight
// to the compare would therefore report the one unobservable target as retired
// the instant it was asked -- and that target is exactly the one a
// zero-initialised field holds.
static void testSeqnoRetiredRefusesReserved(void)
{
    std::printf("testSeqnoRetiredRefusesReserved\n");

    check("the bare compare does report the reserved target as passed",
          igGtfSeqnoPassed(5u, IG_GTF_SEQNO_RESERVED_POLICY_0));
    check("but retired refuses it",
          !igGtfSeqnoRetired(5u, IG_GTF_SEQNO_RESERVED_POLICY_0));
    check("including when the observed value is also reserved",
          !igGtfSeqnoRetired(IG_GTF_SEQNO_RESERVED_POLICY_0,
                             IG_GTF_SEQNO_RESERVED_POLICY_0));

    // And it is a refusal of the TARGET only -- a real target observed from the
    // reserved value is an ordinary not-yet-retired case, not a refusal.
    check("a real target not yet reached is not retired",
          !igGtfSeqnoRetired(IG_GTF_SEQNO_RESERVED_POLICY_0, 4u));
    check("a real target reached IS retired",
          igGtfSeqnoRetired(4u, 4u));
    check("and one passed by wrapping is retired too",
          igGtfSeqnoRetired(1u, 0xFFFFFFFEu));
}

// ---------------------------------------------------------------------------
// Property 8 -- the generator skips the reserved value on wrap
// ---------------------------------------------------------------------------
//
// One submission in every 2^32 would otherwise draw the seqno whose completion
// is by definition unobservable. The walk that finds that by accident is 2^32
// long, so it is pinned here by driving the predecessor directly.
static void testNextSeqnoSkipsReserved(void)
{
    std::printf("testNextSeqnoSkipsReserved\n");

    checkU32("the ordinary successor is one more", igGtfNextSeqno(7u), 8u);
    checkU32("including from the reserved value itself",
             igGtfNextSeqno(IG_GTF_SEQNO_RESERVED_POLICY_0), 1u);

    // The wrap. 0xFFFFFFFF + 1 is the reserved value, so the generator must step
    // over it rather than land on it.
    const IGU32 last = 0xFFFFFFFFu;
    checkU32("the successor of the last value skips the reserved one",
             igGtfNextSeqno(last), 1u);
    check("so no successor is ever the reserved value",
          igGtfNextSeqno(last) != IG_GTF_SEQNO_RESERVED_POLICY_0);

    // A generated seqno must be admissible to the composer it feeds. The two
    // refusals are independent code, so this is the join rather than a restatement.
    IGU32 span[kSpanDwords];
    fillPoison(span);
    checkU32("a wrapped successor is admissible to the composer",
             igGtfWriteCompletion(span, kSpanDwords, 0x1000u,
                                  igGtfNextSeqno(last)),
             IG_GTF_FENCE_OK);
}

// ---------------------------------------------------------------------------
// Property 9 -- the address round-trip, and the accept set it holds over
// ---------------------------------------------------------------------------
//
// igGtfComposedSeqnoAddress() rejoins the two halves the composer split. It is
// the identity on ALIGNED addresses only, and that is a property of the split's
// own refusal rather than a limit of the join: igGtBatchAddressLow() answers an
// unaligned address with a zero, so the halves of an address the composer would
// refuse do not describe it. Claiming the identity for arbitrary 64-bit values
// would be claiming something false.
static void testAddressRoundTrip(void)
{
    std::printf("testAddressRoundTrip\n");

    const IGU64 addrs[] = {
        0x1000ull,
        0x0000000123456000ull,
        0x00000000FFFFF000ull,   // entirely inside the low half
        0x0000000100000000ull,   // entirely inside the high half
        0x0000FFFFFFFFF000ull,   // both halves non-trivial
        0x0000000000000008ull,   // the smallest admissible non-zero address
    };
    const IGU32 addrCount = (IGU32)(sizeof(addrs) / sizeof(addrs[0]));

    for (IGU32 i = 0; i < addrCount; i++) {
        const IGU64 a = addrs[i];
        check("the sample address is admissible", igGtfSeqnoAddressAligned(a));
        // The join reads a composed SPAN, so the round-trip has to go through the
        // composer rather than through the two half helpers directly. Composing
        // per address is what makes this a round-trip at all: a join fed the
        // helpers' own output would agree with them even if the composer wrote
        // the halves into the wrong two slots.
        IGU32 rt[kSpanDwords];
        fillPoison(rt);
        checkU32("the sample address composes",
                 igGtfWriteCompletion(rt, kSpanDwords, a, 7u),
                 IG_GTF_FENCE_OK);
        check("the round-trip through the composed span is the identity",
              igGtfComposedSeqnoAddress(rt, kSpanDwords) == a);
        // And the halves the composer stored are the halves the helpers name.
        // Asserted separately, because the join above is satisfied by any pair
        // of slots holding the right two words.
        check("the low half landed in the low slot",
              rt[IG_GTF_IDX_STORE_ADDR_LO_POLICY] == igGtfAddressLow(a));
        check("the high half landed in the high slot",
              rt[IG_GTF_IDX_STORE_ADDR_HI_POLICY] == igGtfAddressHigh(a));
    }

    // The join reads the composed SPAN, not just the two helpers -- that is what
    // makes it a round-trip through the composer rather than through a pair of
    // functions the composer might not be calling.
    IGU32 span[kSpanDwords];
    const IGU64 target = 0x0000ABCD12345000ull;
    fillPoison(span);
    checkU32("a good completion composes",
             igGtfWriteCompletion(span, kSpanDwords, target, 9u),
             IG_GTF_FENCE_OK);
    check("the span's own two halves rejoin to the target",
          igGtfComposedSeqnoAddress(span, kSpanDwords) == target);

    // Halves are not interchangeable, and the join reads a SPAN, so the swap is
    // performed on a COPY of the composed span rather than by transposing two
    // arguments. That is the stronger form: it is the buffer that is wrong, which
    // is what a composer storing the halves in the wrong two slots would produce,
    // and a join written with its indices transposed survives the argument-swap
    // version silently.
    IGU32 swapped[kSpanDwords];
    for (IGU32 j = 0; j < kSpanDwords; j++) {
        swapped[j] = span[j];
    }
    swapped[IG_GTF_IDX_STORE_ADDR_LO_POLICY] = span[IG_GTF_IDX_STORE_ADDR_HI_POLICY];
    swapped[IG_GTF_IDX_STORE_ADDR_HI_POLICY] = span[IG_GTF_IDX_STORE_ADDR_LO_POLICY];
    check("the halves are not interchangeable",
          igGtfComposedSeqnoAddress(swapped, kSpanDwords) != target);

    // The refusals, which are the join's own and not the composer's. A join that
    // read the two slots unconditionally would return a poison-derived address
    // here rather than zero, and every check above would still be green.
    check("a null span joins to zero",
          igGtfComposedSeqnoAddress(0, kSpanDwords) == 0u);
    check("a span one dword short joins to zero",
          igGtfComposedSeqnoAddress(span,
                                    IG_GTF_COMPLETION_DWORDS_POLICY - 1u) == 0u);
}

// ---------------------------------------------------------------------------
// Property 10 -- the ordering predicate refuses arm by arm
// ---------------------------------------------------------------------------
//
// igGtfCompletionIsOrdered() answers the identical `false` for a null span, a
// short span, a wrong flush header and a wrong store header. Those are four
// different facts about a buffer, and a suite that only ever drives the swapped
// pair pins one of them. The positive control runs FIRST in each pair, so a
// predicate that answered false unconditionally reddens here rather than passing
// every negative control at once.
static void testOrderingRefusals(void)
{
    std::printf("testOrderingRefusals\n");

    IGU32 span[kSpanDwords];
    fillPoison(span);
    checkU32("a good completion composes",
             igGtfWriteCompletion(span, kSpanDwords, 0x2000u, 11u),
             IG_GTF_FENCE_OK);
    check("and reads as ordered", igGtfCompletionIsOrdered(span, kSpanDwords));

    check("a null span is not ordered",
          !igGtfCompletionIsOrdered(0, kSpanDwords));

    // One dword short of the pair -- the boundary rather than an obviously tiny
    // span, same reasoning as the composer's own short-span check.
    check("a span one dword short is not ordered",
          !igGtfCompletionIsOrdered(span, IG_GTF_COMPLETION_DWORDS_POLICY - 1u));
    check("a span of exactly the pair's length IS ordered",
          igGtfCompletionIsOrdered(span, IG_GTF_COMPLETION_DWORDS_POLICY));

    // Each header corrupted ALONE. The swapped-pair control in
    // testCompositionOrdering() moves both at once, so on its own it cannot say
    // which of the two the predicate actually reads -- a predicate checking only
    // the flush header passes it.
    IGU32 mutant[kSpanDwords];

    std::memcpy(mutant, span, sizeof(span));
    mutant[IG_GTF_IDX_FLUSH_HEADER_POLICY] = 0u;
    check("a zeroed flush header is not ordered",
          !igGtfCompletionIsOrdered(mutant, kSpanDwords));

    std::memcpy(mutant, span, sizeof(span));
    mutant[IG_GTF_IDX_STORE_HEADER_POLICY] = 0u;
    check("a zeroed store header is not ordered",
          !igGtfCompletionIsOrdered(mutant, kSpanDwords));

    // A header replaced by the OTHER valid header, one at a time. This is the
    // half of the swap the pair control cannot isolate.
    std::memcpy(mutant, span, sizeof(span));
    mutant[IG_GTF_IDX_FLUSH_HEADER_POLICY] = igGtfStoreHeader();
    check("a flush slot holding the store header is not ordered",
          !igGtfCompletionIsOrdered(mutant, kSpanDwords));

    std::memcpy(mutant, span, sizeof(span));
    mutant[IG_GTF_IDX_STORE_HEADER_POLICY] = igGtfFlushHeader();
    check("a store slot holding the flush header is not ordered",
          !igGtfCompletionIsOrdered(mutant, kSpanDwords));

    // A poison-filled span that was never composed. The refusal must not depend
    // on the buffer having been touched by this driver at all.
    fillPoison(mutant);
    check("an uncomposed span is not ordered",
          !igGtfCompletionIsOrdered(mutant, kSpanDwords));
}

int main(void)
{
    std::printf("test_intel_gt_fence -- IntelGtFence.hpp completion layer\n\n");

    testVerdictNames();
    testSucceededIsEquality();
    testTargetVerdictOrder();
    testRefusalsWriteNothing();
    testCompositionOrdering();
    testGeometry();
    testSeqnoPassedWraps();
    testSeqnoRetiredRefusesReserved();
    testNextSeqnoSkipsReserved();
    testAddressRoundTrip();
    testOrderingRefusals();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
