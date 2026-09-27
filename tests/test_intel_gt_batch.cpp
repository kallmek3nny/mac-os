//
//  test_intel_gt_batch.cpp
//  Host-side suite over phase0-kext/IntelGtBatch.hpp -- the batch-assembly
//  layer that joins IntelGtBlt.hpp's packets to IntelGtCmd.hpp's delivery.
//
//  WHAT THIS SUITE IS FOR
//  ----------------------
//  The header exists because three questions had wrong answers that all produce
//  a well-formed, executable, catastrophic result: an unterminated batch runs
//  off the end of its allocation, a misaligned batch address sets FLAGS in
//  MI_BATCH_BUFFER_START's first dword rather than misfetching, and a ring tail
//  advanced by a count from a REFUSED compose walks into dwords the engine
//  executes. Every one of those is a property of the OUT-PARAM as much as of
//  the return value, so this suite asserts both on every arm.
//
//  WHAT IT DOES NOT ESTABLISH
//  --------------------------
//  Nothing here has been submitted to silicon. igpu-results/ captured a machine
//  whose render engine was idle; there is nothing about batch assembly to
//  observe on an idle engine, so every constant this suite pins is POLICY or is
//  cited through IntelGtCmd.hpp. A green run means the composition is
//  self-consistent and refuses what it says it refuses. It does not mean the
//  command streamer would accept it.
//

#include <cstdio>
#include <cstring>

#include "IntelGtBatch.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %-58s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %-58s got %s want %s\n", label,
                    got ? "true" : "false", want ? "true" : "false");
    }
}

static void checkStr(const char *label, const char *got, const char *want)
{
    gChecks++;
    if (got == 0 || want == 0 || std::strcmp(got, want) != 0) {
        gFailures++;
        std::printf("FAIL %-58s got \"%s\" want \"%s\"\n", label,
                    got ? got : "(null)", want ? want : "(null)");
    }
}

// ---------------------------------------------------------------------------
// 1. The verdict family.
//
// Contiguous from zero to its own LIMIT, and the LIMIT is what the loops below
// bound on -- a family that grows reddens on the commit that grows it rather
// than silently leaving a new verdict unnamed. That is handoff20 section 1's
// igModeVerdictName() defect, which published the unknown string for the one
// verdict that mattered most while all four gates stayed green.
// ---------------------------------------------------------------------------

static void testVerdictFamily(void)
{
    checkU32("IG_GTQ_BATCH_NOT_ATTEMPTED",  IG_GTQ_BATCH_NOT_ATTEMPTED,  0u);
    checkU32("IG_GTQ_BATCH_OK",             IG_GTQ_BATCH_OK,             1u);
    checkU32("IG_GTQ_BATCH_NULL_SPAN",      IG_GTQ_BATCH_NULL_SPAN,      2u);
    checkU32("IG_GTQ_BATCH_SPAN_TOO_SHORT", IG_GTQ_BATCH_SPAN_TOO_SHORT, 3u);
    checkU32("IG_GTQ_BATCH_ADDR_UNALIGNED", IG_GTQ_BATCH_ADDR_UNALIGNED, 4u);
    checkU32("IG_GTQ_BATCH_EMPTY",          IG_GTQ_BATCH_EMPTY,          5u);
    checkU32("IG_GTQ_BATCH_UNTERMINATED",   IG_GTQ_BATCH_UNTERMINATED,   6u);
    checkU32("IG_GTQ_BATCH_TOO_LARGE",      IG_GTQ_BATCH_TOO_LARGE,      7u);
    checkU32("IG_GTQ_BATCH_LIMIT",          IG_GTQ_BATCH_LIMIT,          8u);
}

// ---------------------------------------------------------------------------
// 2. igGtqBatchSucceeded() is an EQUALITY, never a negation.
//
// "!= UNTERMINATED" is true for six other refusals. This block walks the whole
// family and asserts exactly one value answers true, so a future edit to a
// negation form reddens here rather than reporting an unassemblable batch as
// ready to dispatch.
// ---------------------------------------------------------------------------

static void testSucceededIsEquality(void)
{
    IGU32 v;
    IGU32 trueCount = 0u;

    for (v = 0u; v < IG_GTQ_BATCH_LIMIT; v++) {
        if (igGtqBatchSucceeded(v)) { trueCount++; }
    }
    checkU32("exactly one verdict succeeds", trueCount, 1u);

    checkBool("OK succeeds",             igGtqBatchSucceeded(IG_GTQ_BATCH_OK), true);
    checkBool("NOT_ATTEMPTED fails",     igGtqBatchSucceeded(IG_GTQ_BATCH_NOT_ATTEMPTED), false);
    checkBool("UNTERMINATED fails",      igGtqBatchSucceeded(IG_GTQ_BATCH_UNTERMINATED), false);
    checkBool("EMPTY fails",             igGtqBatchSucceeded(IG_GTQ_BATCH_EMPTY), false);

    // One past the family. A negation form would call this success.
    checkBool("LIMIT itself fails",      igGtqBatchSucceeded(IG_GTQ_BATCH_LIMIT), false);
    checkBool("far out of range fails",  igGtqBatchSucceeded(0xFFFFFFFFu), false);
}

// ---------------------------------------------------------------------------
// 3. Every verdict below the family's own LIMIT has its own name.
//
// Per-arm plus pairwise-distinct: a copy-paste arm returning a sibling's string
// satisfies every "has a name" assertion, which is handoff21 section 1's finding
// on the NVIDIA side.
// ---------------------------------------------------------------------------

static const char *const kSentinel = "unknown batch verdict";

static void testVerdictNames(void)
{
    IGU32 a;
    IGU32 b;

    checkStr("name NOT_ATTEMPTED",  igGtqBatchVerdictName(IG_GTQ_BATCH_NOT_ATTEMPTED),  "not attempted");
    checkStr("name OK",             igGtqBatchVerdictName(IG_GTQ_BATCH_OK),             "ok");
    checkStr("name NULL_SPAN",      igGtqBatchVerdictName(IG_GTQ_BATCH_NULL_SPAN),      "null batch span");
    checkStr("name SPAN_TOO_SHORT", igGtqBatchVerdictName(IG_GTQ_BATCH_SPAN_TOO_SHORT), "batch span too short");
    checkStr("name ADDR_UNALIGNED", igGtqBatchVerdictName(IG_GTQ_BATCH_ADDR_UNALIGNED), "batch address not 4-byte aligned");
    checkStr("name EMPTY",          igGtqBatchVerdictName(IG_GTQ_BATCH_EMPTY),          "empty batch");
    checkStr("name UNTERMINATED",   igGtqBatchVerdictName(IG_GTQ_BATCH_UNTERMINATED),   "batch has no MI_BATCH_BUFFER_END");
    checkStr("name TOO_LARGE",      igGtqBatchVerdictName(IG_GTQ_BATCH_TOO_LARGE),      "batch above ceiling");

    // The sentinel is read from the header's own behaviour one past the family,
    // never guessed from a sibling switch -- a-sentinel-guessed-from-a-sibling
    // -cannot-fail is exactly how a coverage loop counts a DELETED arm as named.
    checkStr("LIMIT answers the sentinel", igGtqBatchVerdictName(IG_GTQ_BATCH_LIMIT), kSentinel);
    checkStr("far value answers sentinel", igGtqBatchVerdictName(0xDEADBEEFu), kSentinel);

    // Coverage: no verdict below LIMIT may fall through to the sentinel. This
    // is the tripwire that forces whoever adds a ninth verdict to name it.
    for (a = 0u; a < IG_GTQ_BATCH_LIMIT; a++) {
        gChecks++;
        if (std::strcmp(igGtqBatchVerdictName(a), kSentinel) == 0) {
            gFailures++;
            std::printf("FAIL verdict %u falls through to the sentinel\n", a);
        }
    }

    // Pairwise distinct.
    for (a = 0u; a < IG_GTQ_BATCH_LIMIT; a++) {
        for (b = a + 1u; b < IG_GTQ_BATCH_LIMIT; b++) {
            gChecks++;
            if (std::strcmp(igGtqBatchVerdictName(a), igGtqBatchVerdictName(b)) == 0) {
                gFailures++;
                std::printf("FAIL verdicts %u and %u share a name\n", a, b);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Geometry: terminator sizing and byte arithmetic.
// ---------------------------------------------------------------------------

static void testGeometry(void)
{
    checkU32("align policy is 2",  IG_GTQ_ALIGN_DWORDS_POLICY_2, 2u);
    checkU32("ceiling is 1 Mi dw", IG_GTQ_MAX_BATCH_DWORDS_POLICY, 0x00100000u);

    // An ODD used count means BB_END alone lands the total even -> 1 dword.
    // An EVEN used count needs BB_END plus one NOOP -> 2 dwords.
    checkU32("terminator for 0 used", igGtqTerminatorDwords(0u), 2u);
    checkU32("terminator for 1 used", igGtqTerminatorDwords(1u), 1u);
    checkU32("terminator for 2 used", igGtqTerminatorDwords(2u), 2u);
    checkU32("terminator for 3 used", igGtqTerminatorDwords(3u), 1u);
    checkU32("terminator for 6 used", igGtqTerminatorDwords(6u), 2u);
    checkU32("terminator for 7 used", igGtqTerminatorDwords(7u), 1u);

    // The point of the padding: used + terminator is ALWAYS even. Asserting the
    // property rather than only the six points above means a future terminator
    // that pads by a different rule still has to satisfy the reason it pads.
    {
        IGU32 used;
        for (used = 0u; used <= 32u; used++) {
            gChecks++;
            if (((used + igGtqTerminatorDwords(used)) % 2u) != 0u) {
                gFailures++;
                std::printf("FAIL used=%u leaves an odd terminated total\n", used);
            }
        }
    }

    checkU32("bytes for 0 dwords", igGtqBatchBytes(0u), 0u);
    checkU32("bytes for 1 dword",  igGtqBatchBytes(1u), 4u);
    checkU32("bytes for 6 dwords", igGtqBatchBytes(6u), 24u);
    checkU32("bytes at the ceiling",
             igGtqBatchBytes(IG_GTQ_MAX_BATCH_DWORDS_POLICY), 0x00400000u);

    // Above the ceiling returns 0 rather than a WRAPPED small number. The wrap
    // is the dangerous direction: it under-reserves ring space, and a caller
    // reserving too little writes past the end of the ring.
    checkU32("one past the ceiling refuses",
             igGtqBatchBytes(IG_GTQ_MAX_BATCH_DWORDS_POLICY + 1u), 0u);
    checkU32("a count that would wrap refuses",
             igGtqBatchBytes(0x40000000u), 0u);
    checkU32("all-ones refuses", igGtqBatchBytes(0xFFFFFFFFu), 0u);
}

// ---------------------------------------------------------------------------
// 5. igGtqWriteTerminator() -- the arm order, and the out-param on EVERY path.
//
// The out-param matters as much as the return value here. A caller that
// advanced a ring tail by a count left over from a refused compose walks the
// tail into dwords the engine executes as commands, so every refusal below is
// asserted to have written ZERO.
// ---------------------------------------------------------------------------

#define SPAN_DWORDS 64u

static void fillSpan(IGU32 *span, IGU32 count, IGU32 value)
{
    IGU32 i;
    for (i = 0u; i < count; i++) { span[i] = value; }
}

static void testTerminatorHappyPath(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 written;
    IGU32 verdict;

    // ODD used count -> BB_END only, no padding.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 7u, &written);
    checkU32("odd used: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("odd used: wrote 1",    written, 1u);
    checkU32("odd used: BB_END at [7]", span[7], igGtMiBatchBufferEnd());
    // The dword after the terminator is UNTOUCHED. A terminator that scribbled
    // one past its own length would corrupt the next batch in a shared buffer.
    checkU32("odd used: [8] untouched", span[8], 0xA5A5A5A5u);
    checkU32("odd used: [6] untouched", span[6], 0xA5A5A5A5u);

    // EVEN used count -> BB_END then one MI_NOOP.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 6u, &written);
    checkU32("even used: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("even used: wrote 2",    written, 2u);
    checkU32("even used: BB_END at [6]", span[6], igGtMiBatchBufferEnd());
    checkU32("even used: NOOP at [7]",   span[7], igGtMiNoop());
    checkU32("even used: [8] untouched", span[8], 0xA5A5A5A5u);

    // The NOOP is AFTER the end, never before it. Padding ahead of the
    // terminator would be EXECUTED; padding behind it never is. A terminator
    // that emitted NOOP-then-END would satisfy a count-only assertion.
    checkBool("pad follows the end, not precedes it",
              span[6] == igGtMiBatchBufferEnd() && span[7] == igGtMiNoop(),
              true);

    // Minimum real batch: one packet dword.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 1u, &written);
    checkU32("one used: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("one used: wrote 1",    written, 1u);
    checkU32("one used: BB_END at [1]", span[1], igGtMiBatchBufferEnd());
    checkU32("one used: [0] untouched", span[0], 0xA5A5A5A5u);
}

static void testTerminatorRefusals(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 written;
    IGU32 verdict;

    // 1. NULL span, before anything else is inspected. The used count here is
    //    ALSO zero, which EMPTY would complain about -- the null arm must win.
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(0, SPAN_DWORDS, 0u, &written);
    checkU32("null span: verdict", verdict, IG_GTQ_BATCH_NULL_SPAN);
    checkU32("null span: wrote 0", written, 0u);

    // A null out-param must not fault. The header tests it before every store.
    verdict = igGtqWriteTerminator(0, SPAN_DWORDS, 0u, 0);
    checkU32("null span, null out-param", verdict, IG_GTQ_BATCH_NULL_SPAN);

    // 2. Span too short. Bad in two ways at once: the used count is zero, so
    //    EMPTY also applies -- the span arm wins because writing is what
    //    scribbles and every other complaint is merely wrong.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, 1u, 0u, &written);
    checkU32("short span beats empty: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("short span beats empty: wrote 0", written, 0u);
    checkU32("short span wrote nothing at [0]", span[0], 0xA5A5A5A5u);

    //    The span must hold what is ALREADY there plus the terminator. Testing
    //    only the terminator's own length would accept a span the packets have
    //    already overflowed -- used=10 in a span of 4 needs 2 more dwords and a
    //    naive "is there room for 2" test passes.
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, 4u, 10u, &written);
    checkU32("used past the span end: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("used past the span end: wrote 0", written, 0u);

    //    Exactly one dword short. used=6 needs 2, so a span of 7 is one short.
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, 7u, 6u, &written);
    checkU32("one dword short: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("one dword short: wrote 0", written, 0u);

    //    And exactly enough is accepted. This pins the boundary from the other
    //    side: an off-by-one in the span test would fail here or above.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, 8u, 6u, &written);
    checkU32("exactly enough: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("exactly enough: wrote 2",    written, 2u);

    //    Odd case at the boundary: used=7 needs 1, span of 8 is exact.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, 8u, 7u, &written);
    checkU32("exact odd boundary: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("exact odd boundary: wrote 1",    written, 1u);

    // 3. Empty batch. A bare MI_BATCH_BUFFER_END is a VALID buffer that does
    //    nothing, so this refusal is a policy choice rather than a safety one --
    //    dispatching it burns a ring slot and an execlist port on a no-op the
    //    caller did not intend.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 0u, &written);
    checkU32("empty batch: verdict", verdict, IG_GTQ_BATCH_EMPTY);
    checkU32("empty batch: wrote 0", written, 0u);
    checkU32("empty batch wrote nothing at [0]", span[0], 0xA5A5A5A5u);
}

// ---------------------------------------------------------------------------
// 6. igGtqBatchIsTerminated() -- read rather than assumed.
//
// The composer and the dispatcher need not be the same code. This predicate is
// what a dispatcher consults, and the failure mode of getting it wrong is the
// command streamer running off the end of the allocation.
// ---------------------------------------------------------------------------

static void testIsTerminated(void)
{
    IGU32 span[SPAN_DWORDS];

    checkBool("null span is not terminated", igGtqBatchIsTerminated(0, 4u), false);

    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    checkBool("zero used is not terminated", igGtqBatchIsTerminated(span, 0u), false);

    // A batch of packets with no terminator at all -- the case this whole
    // header exists to prevent.
    checkBool("packets with no end", igGtqBatchIsTerminated(span, 6u), false);

    // Properly terminated, odd -> BB_END last.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    span[3] = igGtMiBatchBufferEnd();
    checkBool("BB_END is the last dword", igGtqBatchIsTerminated(span, 4u), true);

    // Properly terminated, even -> BB_END then a NOOP. The trailing NOOP is
    // skipped, because the terminator itself emits one.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    span[4] = igGtMiBatchBufferEnd();
    span[5] = igGtMiNoop();
    checkBool("BB_END behind one NOOP", igGtqBatchIsTerminated(span, 6u), true);

    // Several trailing NOOPs are still skipped.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    span[2] = igGtMiBatchBufferEnd();
    span[3] = igGtMiNoop();
    span[4] = igGtMiNoop();
    span[5] = igGtMiNoop();
    checkBool("BB_END behind three NOOPs", igGtqBatchIsTerminated(span, 6u), true);

    // A batch that is NOTHING but NOOPs walks the whole span and finds no
    // BB_END. Answering true here would bless a buffer with no terminator.
    fillSpan(span, SPAN_DWORDS, igGtMiNoop());
    checkBool("all NOOPs is not terminated", igGtqBatchIsTerminated(span, 6u), false);

    // A BB_END EARLIER in the buffer terminates it on hardware too, but
    // everything after that point is DEAD -- and a batch whose tail is dead is
    // a composition bug this predicate must not bless. This is the case a naive
    // "scan for BB_END anywhere" implementation gets wrong.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    span[1] = igGtMiBatchBufferEnd();
    checkBool("early BB_END with live tail", igGtqBatchIsTerminated(span, 6u), false);

    // The terminator this header writes is accepted by this predicate. The two
    // are separate functions and could drift apart; this is the round trip.
    {
        IGU32 written = 0u;
        IGU32 verdict;

        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 5u, &written);
        checkU32("round trip odd: verdict OK", verdict, IG_GTQ_BATCH_OK);
        checkBool("round trip odd: terminated",
                  igGtqBatchIsTerminated(span, 5u + written), true);

        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        written = 0u;
        verdict = igGtqWriteTerminator(span, SPAN_DWORDS, 4u, &written);
        checkU32("round trip even: verdict OK", verdict, IG_GTQ_BATCH_OK);
        checkBool("round trip even: terminated",
                  igGtqBatchIsTerminated(span, 4u + written), true);
    }
}

// ---------------------------------------------------------------------------
// 7. igGtqWriteBatchStart() -- the three ring dwords that point at a batch.
//
// The alignment refusal is DELEGATED to igGtBatchAddressOk() rather than
// restated in the header. That means this block's job is to prove the
// delegation actually happens, not to re-derive the alignment rule: a copy of
// the rule here would agree with IntelGtCmd.hpp until one of them moved.
// ---------------------------------------------------------------------------

static void testBatchStartHappyPath(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 written;
    IGU32 verdict;
    const IGU64 addr = 0x0000000123456780ull;

    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteBatchStart(span, SPAN_DWORDS, addr, &written);

    checkU32("bb_start: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("bb_start: wrote 3",    written, IG_GT_MI_BB_START_DWORDS_POLICY_3);
    checkU32("bb_start policy is 3", IG_GT_MI_BB_START_DWORDS_POLICY_3, 3u);

    // Each dword is pinned against the IntelGtCmd.hpp composer that produced
    // it, never against a hand-written literal. A literal here would be a
    // second spelling of the opcode, and the two agree until one moves.
    checkU32("bb_start: [0] is the command", span[0], igGtMiBatchBufferStart());
    checkU32("bb_start: [1] is addr low",    span[1], igGtBatchAddressLow(addr));
    checkU32("bb_start: [2] is addr high",   span[2], igGtBatchAddressHigh(addr));

    // ORDER: low before high. A composer that swapped them would satisfy every
    // "both dwords are present" assertion while pointing the engine at an
    // address 4 GiB from the one the caller asked for.
    checkU32("bb_start: low half is 0x23456780",  span[1], 0x23456780u);
    checkU32("bb_start: high half is 0x00000001", span[2], 0x00000001u);

    // Nothing past the three dwords is touched.
    checkU32("bb_start: [3] untouched", span[3], 0xA5A5A5A5u);

    // A high address exercises the upper dword for real. An implementation
    // that truncated to 32 bits passes every low-address test above.
    {
        const IGU64 high = 0x000000FEDCBA9870ull;
        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        written = 0u;
        verdict = igGtqWriteBatchStart(span, SPAN_DWORDS, high, &written);
        checkU32("bb_start high addr: verdict OK", verdict, IG_GTQ_BATCH_OK);
        checkU32("bb_start high addr: low",  span[1], 0xDCBA9870u);
        checkU32("bb_start high addr: high", span[2], 0x000000FEu);
    }

    // Address zero is legal and aligned. It must NOT be confused with a
    // refusal -- the splitters return 0 on refusal, so this is the case where
    // a "did it return zero" test would read a valid compose as a failure.
    {
        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        written = 0u;
        verdict = igGtqWriteBatchStart(span, SPAN_DWORDS, 0ull, &written);
        checkU32("bb_start addr 0: verdict OK", verdict, IG_GTQ_BATCH_OK);
        checkU32("bb_start addr 0: wrote 3",    written, 3u);
        checkU32("bb_start addr 0: low is 0",   span[1], 0u);
        checkU32("bb_start addr 0: high is 0",  span[2], 0u);
        checkU32("bb_start addr 0: cmd present", span[0], igGtMiBatchBufferStart());
    }
}

static void testBatchStartRefusals(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 written;
    IGU32 verdict;

    // NULL span first, ahead of an address that is ALSO bad. The null arm wins.
    written = 0xDEADu;
    verdict = igGtqWriteBatchStart(0, SPAN_DWORDS, 0x1001ull, &written);
    checkU32("bb_start null span: verdict", verdict, IG_GTQ_BATCH_NULL_SPAN);
    checkU32("bb_start null span: wrote 0", written, 0u);

    verdict = igGtqWriteBatchStart(0, SPAN_DWORDS, 0x1001ull, 0);
    checkU32("bb_start null span, null out", verdict, IG_GTQ_BATCH_NULL_SPAN);

    // Span before address, same reasoning as the terminator: a short span is
    // what would store past the end of the caller's array, and the address is
    // ALSO misaligned here so the ordering is pinned rather than assumed.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0xDEADu;
    verdict = igGtqWriteBatchStart(span, 2u, 0x1001ull, &written);
    checkU32("bb_start short beats unaligned: verdict",
             verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("bb_start short beats unaligned: wrote 0", written, 0u);
    checkU32("bb_start short wrote nothing", span[0], 0xA5A5A5A5u);

    // Exactly three dwords is enough. The boundary from the other side.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    written = 0u;
    verdict = igGtqWriteBatchStart(span, 3u, 0x2000ull, &written);
    checkU32("bb_start exactly 3: verdict OK", verdict, IG_GTQ_BATCH_OK);
    checkU32("bb_start exactly 3: wrote 3",    written, 3u);

    // A misaligned address in a span that is plenty long. This is the arm the
    // header delegates -- the refusal must come back as ADDR_UNALIGNED and
    // nothing may be stored, because a truncated address in the ring points the
    // engine at a batch that is not there.
    {
        IGU32 bad;
        const IGU64 badAddrs[3] = { 0x1001ull, 0x1002ull, 0x1003ull };

        for (bad = 0u; bad < 3u; bad++) {
            fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
            written = 0xDEADu;
            verdict = igGtqWriteBatchStart(span, SPAN_DWORDS,
                                           badAddrs[bad], &written);
            checkU32("bb_start unaligned: verdict",
                     verdict, IG_GTQ_BATCH_ADDR_UNALIGNED);
            checkU32("bb_start unaligned: wrote 0", written, 0u);
            checkU32("bb_start unaligned: [0] untouched", span[0], 0xA5A5A5A5u);
            checkU32("bb_start unaligned: [1] untouched", span[1], 0xA5A5A5A5u);
            checkU32("bb_start unaligned: [2] untouched", span[2], 0xA5A5A5A5u);
        }
    }

    // The refusal this header returns must AGREE with the predicate it
    // delegates to. Walk the four low-bit cases and require the two to match on
    // every one of them: an address the splitters accept must compose, and one
    // they refuse must refuse here.
    {
        IGU64 base = 0x8000ull;
        IGU32 lowBits;

        for (lowBits = 0u; lowBits < 4u; lowBits++) {
            IGU64 probe = base + (IGU64)lowBits;
            bool cmdSaysOk = igGtBatchAddressOk(probe);

            fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
            written = 0xDEADu;
            verdict = igGtqWriteBatchStart(span, SPAN_DWORDS, probe, &written);

            checkBool("bb_start agrees with igGtBatchAddressOk",
                      igGtqBatchSucceeded(verdict), cmdSaysOk);
            checkU32("bb_start agreement: written matches verdict",
                     written, cmdSaysOk ? 3u : 0u);
        }
    }

    checkU32("align policy is 4", IG_GT_BB_ADDR_ALIGN_POLICY_4, 4u);
}

// ---------------------------------------------------------------------------
// 8. igGtqBuildFillBatch() -- the whole join, and the propagated refusal.
//
// This is the first function in the tree that turns a description of work into
// a DISPATCHABLE unit. Its interesting property is not the happy path: it is
// that the blitter's refusal is PROPAGATED through its own out-param rather
// than collapsed into a batch verdict, so a caller handed a refusal goes
// looking in the layer that actually refused.
// ---------------------------------------------------------------------------

static void testBuildFillBatchHappyPath(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 blitVerdict;
    IGU32 written;
    IGU32 verdict;
    IGU32 blitOnly = 0u;
    IGU32 blitOnlyVerdict;

    // Compose the same blit standalone first, so the batch's dword count can be
    // pinned as "the packet plus its terminator" rather than against a literal
    // this suite invented.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    blitOnlyVerdict = igGtbWriteColorBlit(span, SPAN_DWORDS,
                                          0x00100000ull, 4096u,
                                          0u, 0u, 64u, 64u,
                                          0xFF00FF00u,
                                          IG_GTB_DEPTH_32_I915_0x03000000,
                                          &blitOnly);
    checkU32("reference blit composes", blitOnlyVerdict, IG_GTB_BLIT_OK);
    checkBool("reference blit wrote something", blitOnly > 0u, true);

    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    blitVerdict = 0xDEADu;
    written = 0xDEADu;
    verdict = igGtqBuildFillBatch(span, SPAN_DWORDS,
                                  0x00100000ull, 4096u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  &blitVerdict, &written);

    checkU32("fill batch: verdict OK",   verdict, IG_GTQ_BATCH_OK);
    checkU32("fill batch: blit verdict", blitVerdict, IG_GTB_BLIT_OK);
    checkU32("fill batch: packet plus terminator",
             written, blitOnly + igGtqTerminatorDwords(blitOnly));

    // The composed batch IS terminated, read through the predicate a dispatcher
    // would consult rather than by inspecting a dword this suite picked.
    checkBool("fill batch: terminated",
              igGtqBatchIsTerminated(span, written), true);

    // The total is even, so a ring pointing at its end lands on a qword
    // boundary -- the reason the terminator pads at all.
    checkU32("fill batch: even total", written % 2u, 0u);

    // The packet survived: the batch's leading dword is the blit's leading
    // dword, so terminating did not overwrite the work.
    {
        IGU32 ref[SPAN_DWORDS];
        IGU32 refWritten = 0u;
        IGU32 i;

        fillSpan(ref, SPAN_DWORDS, 0xA5A5A5A5u);
        (void)igGtbWriteColorBlit(ref, SPAN_DWORDS,
                                  0x00100000ull, 4096u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  &refWritten);
        for (i = 0u; i < refWritten; i++) {
            gChecks++;
            if (span[i] != ref[i]) {
                gFailures++;
                std::printf("FAIL fill batch dword %u differs: 0x%08X vs 0x%08X\n",
                            i, span[i], ref[i]);
            }
        }
    }

    // Null out-params must not fault on the success path either.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    verdict = igGtqBuildFillBatch(span, SPAN_DWORDS,
                                  0x00100000ull, 4096u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  0, 0);
    checkU32("fill batch: null out-params OK", verdict, IG_GTQ_BATCH_OK);
}

static void testBuildFillBatchRefusals(void)
{
    IGU32 span[SPAN_DWORDS];
    IGU32 blitVerdict;
    IGU32 written;
    IGU32 verdict;

    // NULL span, before a single argument is inspected -- and BOTH out-params
    // are still written, which is the property that stops a caller advancing a
    // ring tail by a stale count.
    blitVerdict = 0xDEADu;
    written = 0xDEADu;
    verdict = igGtqBuildFillBatch(0, SPAN_DWORDS,
                                  0x00100000ull, 4096u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  &blitVerdict, &written);
    checkU32("fill batch null span: verdict", verdict, IG_GTQ_BATCH_NULL_SPAN);
    checkU32("fill batch null span: wrote 0", written, 0u);
    checkU32("fill batch null span: blit NOT_ATTEMPTED",
             blitVerdict, IG_GTB_BLIT_NOT_ATTEMPTED);

    // A zero pitch is a BLITTER refusal. The batch verdict says EMPTY -- true,
    // no packet reached it -- while the blit out-param carries the reason
    // somebody can act on. Collapsing these two would send a caller with a bad
    // pitch looking at the terminator.
    fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
    blitVerdict = 0xDEADu;
    written = 0xDEADu;
    verdict = igGtqBuildFillBatch(span, SPAN_DWORDS,
                                  0x00100000ull, 0u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  &blitVerdict, &written);
    checkU32("zero pitch: batch verdict EMPTY", verdict, IG_GTQ_BATCH_EMPTY);
    checkU32("zero pitch: wrote 0", written, 0u);
    checkBool("zero pitch: blit verdict is a real refusal",
              blitVerdict != IG_GTB_BLIT_OK &&
              blitVerdict != IG_GTB_BLIT_NOT_ATTEMPTED, true);
    checkBool("zero pitch: nothing terminated",
              igGtqBatchIsTerminated(span, 6u), false);

    // An INVERTED rectangle is a different blitter refusal, and the two must
    // not report the same thing through the out-param -- that is the whole
    // reason the verdict is propagated rather than collapsed.
    {
        IGU32 invertedVerdict = 0xDEADu;

        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        written = 0xDEADu;
        verdict = igGtqBuildFillBatch(span, SPAN_DWORDS,
                                      0x00100000ull, 4096u,
                                      64u, 64u, 0u, 0u,
                                      0xFF00FF00u,
                                      IG_GTB_DEPTH_32_I915_0x03000000,
                                      &invertedVerdict, &written);
        checkU32("inverted rect: batch verdict EMPTY", verdict, IG_GTQ_BATCH_EMPTY);
        checkU32("inverted rect: wrote 0", written, 0u);
        checkU32("inverted rect: blit says INVERTED",
                 invertedVerdict, IG_GTB_BLIT_RECT_INVERTED);
        checkBool("two blit refusals stay distinct",
                  invertedVerdict != blitVerdict, true);
    }

    // A span long enough for the PACKET but not for the packet plus its
    // terminator. This is the arm the header exists for: the packet is in the
    // buffer, the terminator did not fit, and reporting the packet's count
    // would dispatch a batch with no MI_BATCH_BUFFER_END in it.
    {
        IGU32 packetDwords = 0u;
        IGU32 tight;

        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        (void)igGtbWriteColorBlit(span, SPAN_DWORDS,
                                  0x00100000ull, 4096u,
                                  0u, 0u, 64u, 64u,
                                  0xFF00FF00u,
                                  IG_GTB_DEPTH_32_I915_0x03000000,
                                  &packetDwords);
        checkBool("packet length is known", packetDwords > 0u, true);

        // Exactly the packet, no room for a terminator.
        tight = packetDwords;
        fillSpan(span, SPAN_DWORDS, 0xA5A5A5A5u);
        blitVerdict = 0xDEADu;
        written = 0xDEADu;
        verdict = igGtqBuildFillBatch(span, tight,
                                      0x00100000ull, 4096u,
                                      0u, 0u, 64u, 64u,
                                      0xFF00FF00u,
                                      IG_GTB_DEPTH_32_I915_0x03000000,
                                      &blitVerdict, &written);
        checkU32("no room to terminate: verdict",
                 verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
        checkU32("no room to terminate: wrote 0", written, 0u);
        // The blit itself SUCCEEDED -- that is exactly why reporting its count
        // would be catastrophic, and why the out-param says zero anyway.
        checkU32("no room to terminate: blit was OK",
                 blitVerdict, IG_GTB_BLIT_OK);
        checkBool("no room to terminate: not terminated",
                  igGtqBatchIsTerminated(span, tight), false);
    }
}

// ---------------------------------------------------------------------------
// 5. igGtqWriteTerminator() -- the accept path.
//
// The out-param is asserted on EVERY arm below, including the refusals, because
// a caller that advances a ring tail by a stale count from a refused compose
// walks the tail into dwords the engine executes as commands. That is the whole
// reason the header writes its out-param before it inspects a single argument.
// ---------------------------------------------------------------------------

// Sections 5-7 below were added after sections 5a-7a and reuse their helper
// rather than redeclaring it: SPAN_DWORDS and fillSpan() are already defined
// above, and a second definition of either is a redefinition error rather than
// a shadowing. BATCH_SPAN is an alias so the bodies below read as written.
#define BATCH_SPAN SPAN_DWORDS

static void testTerminatorAccepts(void)
{
    IGU32 span[BATCH_SPAN];
    IGU32 written;
    IGU32 verdict;

    // Odd used count: BB_END alone, no padding NOOP.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, BATCH_SPAN, 5u, &written);
    checkU32("odd used: verdict", verdict, IG_GTQ_BATCH_OK);
    checkU32("odd used: written", written, 1u);
    checkU32("odd used: BB_END at [5]", span[5], igGtMiBatchBufferEnd());
    // The dword AFTER the terminator is untouched -- the terminator writes
    // exactly what it reports and not one dword more.
    checkU32("odd used: [6] untouched", span[6], 0xA5A5A5A5u);
    // And the packets below it are untouched too.
    checkU32("odd used: [4] untouched", span[4], 0xA5A5A5A5u);

    // Even used count: BB_END plus one MI_NOOP.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, BATCH_SPAN, 6u, &written);
    checkU32("even used: verdict", verdict, IG_GTQ_BATCH_OK);
    checkU32("even used: written", written, 2u);
    checkU32("even used: BB_END at [6]", span[6], igGtMiBatchBufferEnd());
    checkU32("even used: NOOP at [7]",  span[7], igGtMiNoop());
    checkU32("even used: [8] untouched", span[8], 0xA5A5A5A5u);

    // The NOOP is AFTER the end, never before it. Padding ahead of the
    // terminator would be EXECUTED; padding behind it never is. Asserting the
    // order explicitly means a swapped pair reddens here rather than shipping a
    // batch whose last executed command is a NOOP followed by nothing.
    checkBool("even used: end precedes pad",
              span[6] == igGtMiBatchBufferEnd() && span[7] == igGtMiNoop(), true);

    // A span exactly the size it needs. The boundary case matters because an
    // off-by-one in the span test is what stores past the caller's array.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 6u, 5u, &written);
    checkU32("exact fit odd: verdict", verdict, IG_GTQ_BATCH_OK);
    checkU32("exact fit odd: written", written, 1u);

    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 8u, 6u, &written);
    checkU32("exact fit even: verdict", verdict, IG_GTQ_BATCH_OK);
    checkU32("exact fit even: written", written, 2u);

    // A one-dword batch is the smallest ACCEPTABLE one: one packet dword plus
    // BB_END. Below it is the empty case, which is a refusal.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 2u, 1u, &written);
    checkU32("minimum batch: verdict", verdict, IG_GTQ_BATCH_OK);
    checkU32("minimum batch: written", written, 1u);
    checkU32("minimum batch: BB_END at [1]", span[1], igGtMiBatchBufferEnd());
}

// ---------------------------------------------------------------------------
// 6. igGtqWriteTerminator() -- the refusals, and their ORDER.
//
// Each refusal is pinned by an input bad in TWO ways at once, because an arm
// order asserted only against singly-bad inputs is satisfied by any order at
// all. The header states the order as: null span, short span, empty, too large.
// ---------------------------------------------------------------------------

static void testTerminatorRefuses(void)
{
    IGU32 span[BATCH_SPAN];
    IGU32 written;
    IGU32 verdict;

    // Null span, with an out-param the caller pre-loaded with garbage.
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(0, BATCH_SPAN, 4u, &written);
    checkU32("null span: verdict", verdict, IG_GTQ_BATCH_NULL_SPAN);
    checkU32("null span: written zeroed", written, 0u);

    // Null span AND an empty batch AND a zero count: the null arm wins, because
    // it runs before anything else is inspected.
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(0, 0u, 0u, &written);
    checkU32("null beats empty and short", verdict, IG_GTQ_BATCH_NULL_SPAN);
    checkU32("null beats: written zeroed", written, 0u);

    // A null out-param must not crash and must still return the verdict.
    verdict = igGtqWriteTerminator(0, BATCH_SPAN, 4u, 0);
    checkU32("null span, null out-param", verdict, IG_GTQ_BATCH_NULL_SPAN);

    // Short span: one dword short of what the terminator needs.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 5u, 5u, &written);
    checkU32("short by the terminator: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("short by the terminator: written zeroed", written, 0u);
    checkU32("short by the terminator: nothing stored", span[5], 0xA5A5A5A5u);

    // Short by the PAD: an even used count needs two, and a span with room for
    // exactly one is refused rather than terminated without its padding.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 7u, 6u, &written);
    checkU32("short by the pad: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("short by the pad: written zeroed", written, 0u);
    checkU32("short by the pad: nothing stored", span[6], 0xA5A5A5A5u);

    // The span is shorter than what the packets ALREADY occupy. Testing only
    // the terminator's own length would accept a span the packets overflowed,
    // and the subtraction would wrap -- this is the arm that catches it.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 4u, 10u, &written);
    checkU32("used exceeds span: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("used exceeds span: written zeroed", written, 0u);

    // The wrap case stated at its worst: a used count near the top of the type.
    // dwordCount - usedDwords would be an enormous number if the first half of
    // the condition were dropped, and the terminator would store at [used].
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, BATCH_SPAN, 0xFFFFFFFFu, &written);
    checkU32("used at all-ones: verdict", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("used at all-ones: written zeroed", written, 0u);
    checkU32("used at all-ones: nothing stored", span[0], 0xA5A5A5A5u);

    // Empty batch. A bare MI_BATCH_BUFFER_END is a VALID buffer that does
    // nothing, so this refusal is a policy choice rather than a safety one --
    // it burns a ring slot and an execlist port on a no-op nobody asked for.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, BATCH_SPAN, 0u, &written);
    checkU32("empty batch: verdict", verdict, IG_GTQ_BATCH_EMPTY);
    checkU32("empty batch: written zeroed", written, 0u);
    checkU32("empty batch: nothing stored", span[0], 0xA5A5A5A5u);

    // Empty AND short at once. The SPAN wins, because writing is what
    // scribbles and every other complaint is merely wrong.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 1u, 0u, &written);
    checkU32("short beats empty", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("short beats empty: written zeroed", written, 0u);

    // Zero span with a zero used count -- still the span, same reasoning.
    written = 0xFFFFFFFFu;
    verdict = igGtqWriteTerminator(span, 0u, 0u, &written);
    checkU32("zero span beats empty", verdict, IG_GTQ_BATCH_SPAN_TOO_SHORT);
    checkU32("zero span beats empty: written zeroed", written, 0u);
}

// ---------------------------------------------------------------------------
// 7. igGtqBatchIsTerminated().
//
// Read rather than assumed: the caller who composes a batch and the caller who
// dispatches it need not be the same code, and the failure mode of dispatching
// an unterminated batch is the streamer running off the end of the allocation.
// ---------------------------------------------------------------------------

static void testIsTerminatedExtra(void)
{
    IGU32 span[BATCH_SPAN];

    // A null pointer and a zero count are both FALSE rather than vacuously
    // true. "Nothing to check, so it passes" is the direction that blesses an
    // unterminated batch.
    checkBool("null is not terminated", igGtqBatchIsTerminated(0, 4u), false);
    checkBool("zero used is not terminated", igGtqBatchIsTerminated(span, 0u), false);

    // The straightforward accept.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = 0x11111111u;
    span[1] = igGtMiBatchBufferEnd();
    checkBool("packet then BB_END", igGtqBatchIsTerminated(span, 2u), true);

    // Trailing NOOPs are skipped, because the terminator itself emits one.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = 0x11111111u;
    span[1] = igGtMiBatchBufferEnd();
    span[2] = igGtMiNoop();
    checkBool("BB_END then one NOOP", igGtqBatchIsTerminated(span, 3u), true);

    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = 0x11111111u;
    span[1] = igGtMiBatchBufferEnd();
    span[2] = igGtMiNoop();
    span[3] = igGtMiNoop();
    span[4] = igGtMiNoop();
    checkBool("BB_END then three NOOPs", igGtqBatchIsTerminated(span, 5u), true);

    // The refusal this predicate exists for: a batch whose last dword is a
    // packet. The streamer would keep fetching past it.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = igGtMiBatchBufferEnd();
    span[1] = 0x11111111u;
    checkBool("BB_END then a packet is not terminated",
              igGtqBatchIsTerminated(span, 2u), false);

    // A BB_END earlier in the buffer terminates it too, but everything after
    // that point is DEAD, and a batch whose tail is dead is a composition bug
    // this predicate must not bless. This is the reason it checks the last
    // meaningful dword rather than scanning for any BB_END anywhere.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = 0x11111111u;
    span[1] = igGtMiBatchBufferEnd();
    span[2] = 0x22222222u;
    span[3] = 0x33333333u;
    checkBool("early BB_END with dead tail refuses",
              igGtqBatchIsTerminated(span, 4u), false);

    // Nothing but NOOPs. The walk reaches index 0 with no BB_END found, and
    // that must be FALSE rather than falling off the front into a read at [-1].
    fillSpan(span, BATCH_SPAN, igGtMiNoop());
    checkBool("all NOOPs is not terminated", igGtqBatchIsTerminated(span, 8u), false);

    // A single NOOP, the tightest version of the same walk.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = igGtMiNoop();
    checkBool("one NOOP is not terminated", igGtqBatchIsTerminated(span, 1u), false);

    // A bare BB_END at index 0 IS terminated, even though the terminator writer
    // refuses to CREATE one. The two are different questions: composing an
    // empty batch is a policy refusal, reading one is a fact.
    fillSpan(span, BATCH_SPAN, 0xA5A5A5A5u);
    span[0] = igGtMiBatchBufferEnd();
    checkBool("a bare BB_END reads as terminated",
              igGtqBatchIsTerminated(span, 1u), true);

    // Round trip: what the terminator writes, the predicate accepts. Both the
    // odd and even paths, because the padded one is where a naive last-dword
    // check would fail.
    {
        IGU32 written = 0u;
        fillSpan(span, BATCH_SPAN, 0x11111111u);
        checkU32("round trip odd: verdict",
                 igGtqWriteTerminator(span, BATCH_SPAN, 5u, &written), IG_GTQ_BATCH_OK);
        checkBool("round trip odd: reads terminated",
                  igGtqBatchIsTerminated(span, 5u + written), true);

        fillSpan(span, BATCH_SPAN, 0x11111111u);
        checkU32("round trip even: verdict",
                 igGtqWriteTerminator(span, BATCH_SPAN, 6u, &written), IG_GTQ_BATCH_OK);
        checkBool("round trip even: reads terminated",
                  igGtqBatchIsTerminated(span, 6u + written), true);
    }
}

int main(void)
{
    testVerdictFamily();
    testSucceededIsEquality();
    testVerdictNames();
    testGeometry();
    testTerminatorHappyPath();
    testTerminatorRefusals();
    testIsTerminated();
    testTerminatorAccepts();
    testTerminatorRefuses();
    testIsTerminatedExtra();
    testBatchStartHappyPath();
    testBatchStartRefusals();
    testBuildFillBatchHappyPath();
    testBuildFillBatchRefusals();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
