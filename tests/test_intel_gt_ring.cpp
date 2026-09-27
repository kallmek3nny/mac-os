//
//  test_intel_gt_ring.cpp
//  Host-side suite over phase0-kext/IntelGtRing.hpp -- the ring writer, and the
//  only GT header in this tree that nothing on disk executed.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  IntelGtRing.hpp is 413 lines of wrapping-store logic that no suite drove, no
//  CMake target compiled, and no shell called. Per this tree's own rule -- the
//  logic/IOKit split in CLAUDE.md, and intel-handoff26.md §1's measurement that a
//  heavily tested function with no callers passes every suite it has -- a
//  dependency-free header with no suite is a header nothing has ever run.
//
//  WHAT IS WORTH ASSERTING HERE RATHER THAN ONE LAYER DOWN
//  -------------------------------------------------------
//  IntelGtCmd.hpp owns the ring ARITHMETIC and tests/test_intel_gt_cmd.cpp
//  covers it. This header owns the one thing that arithmetic cannot express: a
//  STORE that wraps. So the assertions that belong here are the ones about
//  bytes landing in the right slots of a circular span, and about refusals
//  happening BEFORE any slot is touched.
//
//  Three properties carry most of the weight:
//
//   1. A refused call must not write. Every refusal arm is checked against a
//      canary-filled ring, because a function that refuses AFTER storing has
//      already corrupted the thing it refused to corrupt -- handoff18 §1's
//      `gFix.readCount == 0` reasoning, one subsystem over. A verdict alone
//      cannot distinguish the two orders.
//
//   2. The wrap must land the SECOND half at index zero. A store that clamps
//      instead of wrapping produces the identical verdict and the identical
//      new tail while leaving the tail end of the payload unwritten -- so the
//      separating evidence is the CONTENT of slot 0, never the return value.
//
//   3. The pad is MI_NOOP and it is inside the wrap. Padding written with a
//      non-wrapping cursor scribbles past the end of the caller's span, and
//      the verdict is OK either way.
//
//  The classify mapping gets its own group because it is a JOIN of two verdict
//  families and the failure mode is a submit refusal reported as the wrong KIND
//  of ring refusal -- BAD_GEOMETRY says "do not retry", NOT_ADMISSIBLE says
//  "retry when the engine has drained". Collapsing them is arithmetically
//  invisible and operationally opposite.
//

#include <cstdio>
#include <cstring>

#include "IntelGtRing.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void check(const char *label, bool ok)
{
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL %s\n", label);
    }
}

static void checkVerdict(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: verdict %u (%s), want %u (%s)\n",
                    label, got, igGtrRingVerdictName(got),
                    want, igGtrRingVerdictName(want));
    }
}

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: got 0x%08x, want 0x%08x\n", label, got, want);
    }
}

// The subject's own parameter order is (span, payload, engine, geometry, out).
// The groups below discuss a call as (ring, engine, geometry, payload), so one
// adapter spells the real signature once. That is deliberate rather than
// cosmetic: twelve call sites each free to pass the ring-dword count and the
// payload-dword count in the wrong order is twelve chances to write a test that
// passes for the wrong reason, and both are IGU32 so the compiler has nothing to
// say. One place to get it wrong, and getting it wrong there reddens every
// group at once.
static IGU32 writeToRing(IGU32 *ring, IGU32 ringDwords, IGU32 engine,
                         IGU32 headBytes, IGU32 tailBytes,
                         const IGU32 *payload, IGU32 payloadDwords,
                         IGU32 *outTailBytes)
{
    return igGtrWriteWrapped(ring, ringDwords, payload, payloadDwords,
                             engine, headBytes, tailBytes, outTailBytes);
}

// ---------------------------------------------------------------------------
// Group 1: the verdict family itself.
//
// Contiguity from zero to the family's own LIMIT, NOT_ATTEMPTED never returned,
// and every verdict below the limit carrying a name -- handoff20.md §1 is why
// this group exists at all: a verdict lost to a switch `default:` publishes the
// unknown string with every gate green.
// ---------------------------------------------------------------------------

static void testVerdictFamily(void)
{
    check("OK is not NOT_ATTEMPTED", IG_GTR_RING_OK != IG_GTR_RING_NOT_ATTEMPTED);
    check("succeeded is an equality against OK",
          igGtrRingSucceeded(IG_GTR_RING_OK));
    check("succeeded refuses NOT_ATTEMPTED",
          !igGtrRingSucceeded(IG_GTR_RING_NOT_ATTEMPTED));

    // Bound on the family's OWN limit, so a verdict added tomorrow reddens on
    // the commit that adds it rather than silently going unnamed.
    for (IGU32 v = 0u; v < IG_GTR_RING_LIMIT; v++) {
        const char *name = igGtrRingVerdictName(v);
        gChecks++;
        if (name == 0 || name[0] == '\0') {
            gFailures++;
            std::printf("FAIL verdict %u has no name\n", v);
            continue;
        }
        // A verdict lost to `default:` is honest and useless: it reports a
        // string rather than failing. Compare against the sentinel the switch
        // returns for out-of-family values, read from the subject itself
        // rather than guessed from a sibling function -- handoff21 §1b.
        gChecks++;
        if (std::strcmp(name, igGtrRingVerdictName(IG_GTR_RING_LIMIT)) == 0) {
            gFailures++;
            std::printf("FAIL verdict %u falls through to the sentinel\n", v);
        }
    }

    // Pairwise distinct: a copy-paste arm returning a sibling's string
    // satisfies every "has a name" assertion above.
    for (IGU32 a = 0u; a < IG_GTR_RING_LIMIT; a++) {
        for (IGU32 b = a + 1u; b < IG_GTR_RING_LIMIT; b++) {
            gChecks++;
            if (std::strcmp(igGtrRingVerdictName(a),
                            igGtrRingVerdictName(b)) == 0) {
                gFailures++;
                std::printf("FAIL verdicts %u and %u share a name\n", a, b);
            }
        }
    }

    // succeeded() is an equality, so every other verdict in the family --
    // including ones added later -- must refuse.
    for (IGU32 v = 0u; v < IG_GTR_RING_LIMIT; v++) {
        if (v == IG_GTR_RING_OK) { continue; }
        gChecks++;
        if (igGtrRingSucceeded(v)) {
            gFailures++;
            std::printf("FAIL verdict %u reports success\n", v);
        }
    }
}

// ---------------------------------------------------------------------------
// Group 2: pad arithmetic.
//
// The pad exists so the resulting tail satisfies IG_GT_TAIL_ALIGN_POLICY_8 one
// layer down. Asserted as a PROPERTY of the resulting tail rather than as a
// table of expected pad counts: a table can be written to match whatever the
// function currently returns, while "tail + total is a multiple of eight" is
// the thing the register actually requires.
// ---------------------------------------------------------------------------

static void testPadArithmetic(void)
{
    const IGU32 alignBytes = IG_GT_TAIL_ALIGN_POLICY_8;

    for (IGU32 tailDw = 0u; tailDw < 8u; tailDw++) {
        for (IGU32 payload = 1u; payload <= 8u; payload++) {
            IGU32 tailBytes = tailDw * IG_GT_CMD_WORD_BYTES_I915_4;
            IGU32 pad = igGtrPadDwords(tailBytes, payload);
            IGU32 total = igGtrTotalDwords(tailBytes, payload);

            gChecks++;
            if (total != payload + pad) {
                gFailures++;
                std::printf("FAIL total != payload + pad at tail %u payload %u\n",
                            tailBytes, payload);
            }

            IGU32 endBytes = tailBytes + total * IG_GT_CMD_WORD_BYTES_I915_4;
            gChecks++;
            if ((endBytes % alignBytes) != 0u) {
                gFailures++;
                std::printf("FAIL padded end 0x%x unaligned at tail %u payload %u\n",
                            endBytes, tailBytes, payload);
            }

            // The pad must be MINIMAL. A pad of eight dwords also satisfies
            // the alignment property and wastes a ring the caller sized
            // exactly, so the property above alone does not pin it.
            gChecks++;
            if (pad >= (alignBytes / IG_GT_CMD_WORD_BYTES_I915_4)) {
                gFailures++;
                std::printf("FAIL pad %u is not minimal at tail %u payload %u\n",
                            pad, tailBytes, payload);
            }
        }
    }

    // An already-aligned end needs no pad at all -- the arm that would
    // otherwise add a full alignment quantum for nothing.
    checkU32("aligned tail, aligned payload needs no pad",
             igGtrPadDwords(0u, 2u), 0u);
    checkU32("aligned tail, odd payload pads to eight bytes",
             igGtrPadDwords(0u, 1u), 1u);

    // Payload bytes refuses rather than wrapping the multiplication.
    checkU32("payload bytes converts inside the ceiling",
             igGtrPayloadBytes(4u), 16u);
    checkU32("payload bytes refuses above the ceiling",
             igGtrPayloadBytes(IG_GTR_MAX_PAYLOAD_DWORDS_POLICY + 1u), 0u);
}

// ---------------------------------------------------------------------------
// Group 3: the classify mapping.
//
// This is the join, and the failure mode is a retryable refusal reported as a
// permanent one. Both halves are pinned by NAME rather than by number, and the
// two sets are asserted DISJOINT so a future arm cannot appear in both.
// ---------------------------------------------------------------------------

static void testClassifySubmit(void)
{
    // Geometry: facts about what the caller handed in. Not retryable.
    checkVerdict("bad engine classifies as geometry",
                 igGtrClassifySubmit(IG_GT_SUBMIT_BAD_ENGINE),
                 IG_GTR_RING_BAD_GEOMETRY);
    checkVerdict("ring unaligned classifies as geometry",
                 igGtrClassifySubmit(IG_GT_SUBMIT_RING_UNALIGNED),
                 IG_GTR_RING_BAD_GEOMETRY);
    checkVerdict("ring too small classifies as geometry",
                 igGtrClassifySubmit(IG_GT_SUBMIT_RING_TOO_SMALL),
                 IG_GTR_RING_BAD_GEOMETRY);
    checkVerdict("ring too large classifies as geometry",
                 igGtrClassifySubmit(IG_GT_SUBMIT_RING_TOO_LARGE),
                 IG_GTR_RING_BAD_GEOMETRY);

    // Admissibility: this submission does not fit. Retryable once the engine
    // has consumed some ring.
    checkVerdict("empty submission classifies as not admissible",
                 igGtrClassifySubmit(IG_GT_SUBMIT_EMPTY),
                 IG_GTR_RING_NOT_ADMISSIBLE);
    checkVerdict("unaligned length classifies as not admissible",
                 igGtrClassifySubmit(IG_GT_SUBMIT_UNALIGNED_LENGTH),
                 IG_GTR_RING_NOT_ADMISSIBLE);
    checkVerdict("no space classifies as not admissible",
                 igGtrClassifySubmit(IG_GT_SUBMIT_NO_SPACE),
                 IG_GTR_RING_NOT_ADMISSIBLE);

    // OK reaching the classifier at all is a bug in the caller: the store only
    // classifies on the refusal path. Refusing rather than returning OK is the
    // safe direction -- an OK synthesised here would report a store that never
    // happened as one that landed.
    check("classify never manufactures success",
          !igGtrRingSucceeded(igGtrClassifySubmit(IG_GT_SUBMIT_OK)));

    // Every submit verdict below the submit family's own limit must classify to
    // something, and never to NOT_ATTEMPTED -- an out-param nobody wrote must
    // not be confusable with a measured refusal.
    for (IGU32 v = 0u; v < IG_GT_SUBMIT_LIMIT; v++) {
        IGU32 got = igGtrClassifySubmit(v);
        gChecks++;
        if (got == IG_GTR_RING_NOT_ATTEMPTED || got >= IG_GTR_RING_LIMIT) {
            gFailures++;
            std::printf("FAIL submit verdict %u classifies to %u\n", v, got);
        }
    }
}

// ---------------------------------------------------------------------------
// Group 4: refusals must not write.
//
// Every arm is driven against a canary-filled ring. The canary is checked over
// the WHOLE span, because a refusal that stored the first dword before
// refusing leaves an executable opcode in a ring the caller believes untouched.
// ---------------------------------------------------------------------------

static const IGU32 kCanary = 0xDEADBEEFu;

static bool ringIsPristine(const IGU32 *ring, IGU32 dwords)
{
    for (IGU32 i = 0u; i < dwords; i++) {
        if (ring[i] != kCanary) { return false; }
    }
    return true;
}

static void fillCanary(IGU32 *ring, IGU32 dwords)
{
    for (IGU32 i = 0u; i < dwords; i++) { ring[i] = kCanary; }
}

// A page-aligned ring at the policy minimum, expressed in dwords.
static const IGU32 kRingDwords =
    IG_GT_RING_MIN_BYTES_POLICY_0x1000 / IG_GT_CMD_WORD_BYTES_I915_4;

static void testRefusalsDoNotWrite(void)
{
    static IGU32 ring[kRingDwords];
    IGU32 payload[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    IGU32 newTail = 0xFFFFFFFFu;
    IGU32 verdict;

    // Null spans. The out-param is still written, so even the NULL arm leaves
    // the caller with a usable value rather than an uninitialised one.
    newTail = 0xFFFFFFFFu;
    verdict = writeToRing(0, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, 4u, &newTail);
    checkVerdict("null ring refuses", verdict, IG_GTR_RING_NULL_SPAN);
    checkU32("null ring still reports the tail it was given", newTail, 0u);

    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, 0, 4u, &newTail);
    checkVerdict("null payload refuses", verdict, IG_GTR_RING_NULL_SPAN);
    check("null payload wrote nothing", ringIsPristine(ring, kRingDwords));

    // Payload shape before geometry: emptiness first, then the ceiling.
    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, 0u, &newTail);
    checkVerdict("empty payload refuses by name", verdict,
                 IG_GTR_RING_EMPTY_PAYLOAD);
    check("empty payload wrote nothing", ringIsPristine(ring, kRingDwords));

    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, payload,
                               IG_GTR_MAX_PAYLOAD_DWORDS_POLICY + 1u, &newTail);
    checkVerdict("over-long payload refuses by name", verdict,
                 IG_GTR_RING_PAYLOAD_TOO_LONG);
    check("over-long payload wrote nothing", ringIsPristine(ring, kRingDwords));

    // A zero-dword span is a bad SPAN, distinct from an empty payload. Both
    // are "nothing to work with" and only one is the caller's composition bug.
    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, 0u, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, 4u, &newTail);
    checkVerdict("zero-dword span refuses by name", verdict,
                 IG_GTR_RING_BAD_SPAN);

    // Geometry the submit layer refuses: a ring below the policy minimum is a
    // fact about the allocation, so it must classify as BAD_GEOMETRY and not
    // as a retryable NOT_ADMISSIBLE.
    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, 4u, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, 2u, &newTail);
    checkVerdict("undersized ring is geometry, not admissibility", verdict,
                 IG_GTR_RING_BAD_GEOMETRY);
    check("undersized ring wrote nothing", ringIsPristine(ring, kRingDwords));

    // An engine outside the family is geometry too, and it must refuse before
    // any store -- the register base for a bad selector is not a real base.
    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_LIMIT,
                               0u, 0u, payload, 4u, &newTail);
    checkVerdict("engine outside the family is geometry", verdict,
                 IG_GTR_RING_BAD_GEOMETRY);
    check("bad engine wrote nothing", ringIsPristine(ring, kRingDwords));

    // A payload that cannot fit the free run is NOT_ADMISSIBLE -- retryable --
    // and must also leave the ring alone. This is the arm most likely to store
    // first and refuse second, because the refusal is arithmetic rather than
    // structural.
    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, kRingDwords, &newTail);
    checkVerdict("payload larger than the ring is not admissible", verdict,
                 IG_GTR_RING_NOT_ADMISSIBLE);
    check("inadmissible payload wrote nothing",
          ringIsPristine(ring, kRingDwords));
}

// ---------------------------------------------------------------------------
// Group 5: the store that does not wrap.
//
// The baseline. Payload dwords land at consecutive slots from tail/4, the pad
// is MI_NOOP, nothing outside [tail, tail+total) moves, and the new tail comes
// from IntelGtCmd.hpp's own advance rather than from this header's cursor.
// ---------------------------------------------------------------------------

static void testStraightStore(void)
{
    static IGU32 ring[kRingDwords];
    IGU32 payload[3] = { 0xA0A0A0A0u, 0xB1B1B1B1u, 0xC2C2C2C2u };
    IGU32 newTail = 0u;
    IGU32 verdict;

    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 0u, payload, 3u, &newTail);
    checkVerdict("aligned straight store lands", verdict, IG_GTR_RING_OK);

    checkU32("straight store slot 0", ring[0], payload[0]);
    checkU32("straight store slot 1", ring[1], payload[1]);
    checkU32("straight store slot 2", ring[2], payload[2]);

    // Three dwords from an aligned tail leaves the end at 12 bytes, which is
    // not a multiple of eight, so exactly one MI_NOOP must follow.
    IGU32 pad = igGtrPadDwords(0u, 3u);
    checkU32("straight store pads by one", pad, 1u);
    checkU32("straight store pad is MI_NOOP", ring[3], igGtMiNoop());

    // Nothing beyond the padded span moved.
    check("straight store left the rest pristine",
          ringIsPristine(ring + 4, kRingDwords - 4u));

    // The tail is the arithmetic layer's answer, not this header's cursor.
    IGU32 ringBytes = kRingDwords * IG_GT_CMD_WORD_BYTES_I915_4;
    checkU32("new tail comes from igGtRingAdvance", newTail,
             igGtRingAdvance(0u, (3u + pad) * IG_GT_CMD_WORD_BYTES_I915_4,
                             ringBytes));
    checkU32("new tail is byte 16", newTail, 16u);

    // A store from a non-zero aligned tail indexes from tail/4 rather than
    // from zero -- the off-by-a-division that overwrites live commands.
    fillCanary(ring, kRingDwords);
    newTail = 0u;
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               0u, 64u, payload, 2u, &newTail);
    checkVerdict("store from a non-zero tail lands", verdict, IG_GTR_RING_OK);
    checkU32("non-zero tail indexes from tail/4", ring[16], payload[0]);
    checkU32("non-zero tail slot 17", ring[17], payload[1]);
    check("non-zero tail left slot 15 alone", ring[15] == kCanary);
    checkU32("two dwords from an aligned tail need no pad",
             igGtrPadDwords(64u, 2u), 0u);
    checkU32("non-zero tail advanced by eight bytes", newTail, 72u);
}

// ---------------------------------------------------------------------------
// Group 6: the wrap.
//
// The whole reason the header exists. A store starting near the end of the span
// must continue at index zero, and the separating evidence is the CONTENT of
// the low slots -- a clamping store returns the identical verdict and the
// identical new tail while leaving the second half of the payload unwritten.
// ---------------------------------------------------------------------------

static void testWrappingStore(void)
{
    static IGU32 ring[kRingDwords];
    IGU32 payload[4] = { 0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u };
    IGU32 newTail = 0u;
    IGU32 verdict;

    // Start two dwords before the end of the span with a four-dword payload:
    // two land at the top, two must land at index 0 and 1. Head is placed well
    // inside the ring so the free run is genuinely large enough.
    IGU32 startDword = kRingDwords - 2u;
    IGU32 startBytes = startDword * IG_GT_CMD_WORD_BYTES_I915_4;
    IGU32 headBytes = 512u * IG_GT_CMD_WORD_BYTES_I915_4;

    fillCanary(ring, kRingDwords);
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               headBytes, startBytes, payload, 4u, &newTail);
    checkVerdict("wrapping store lands", verdict, IG_GTR_RING_OK);

    checkU32("wrap slot n-2", ring[startDword], payload[0]);
    checkU32("wrap slot n-1", ring[startDword + 1u], payload[1]);
    checkU32("wrap continues at slot 0", ring[0], payload[2]);
    checkU32("wrap continues at slot 1", ring[1], payload[3]);

    // Four dwords from an aligned tail is 16 bytes, already a multiple of
    // eight, so no pad follows and slot 2 must be untouched. That also pins
    // that the wrap did not run one dword long.
    checkU32("wrapped payload needs no pad",
             igGtrPadDwords(startBytes, 4u), 0u);
    check("wrap left slot 2 alone", ring[2] == kCanary);

    IGU32 ringBytes = kRingDwords * IG_GT_CMD_WORD_BYTES_I915_4;
    checkU32("wrapped tail comes from igGtRingAdvance", newTail,
             igGtRingAdvance(startBytes, 4u * IG_GT_CMD_WORD_BYTES_I915_4,
                             ringBytes));
    checkU32("wrapped tail is byte 8", newTail, 8u);

    // The pad itself must respect the wrap. Start one dword before the end
    // with a two-dword payload: the second dword lands at slot 0, the end is
    // at 12 bytes past an aligned start, so one MI_NOOP must land at slot 1 --
    // a non-wrapping pad cursor would store it one past the end of the span.
    fillCanary(ring, kRingDwords);
    newTail = 0u;
    IGU32 lastDword = kRingDwords - 1u;
    IGU32 lastBytes = lastDword * IG_GT_CMD_WORD_BYTES_I915_4;

    // The tail must be eight-byte aligned for the submit layer to accept it,
    // and n-1 dwords is 0x0FFC, which is not. Use n-2 with a three-dword
    // payload instead: two at the top, one at slot 0, pad at slot 1.
    (void)lastDword;
    (void)lastBytes;
    IGU32 threeStart = (kRingDwords - 2u) * IG_GT_CMD_WORD_BYTES_I915_4;
    verdict = writeToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                               headBytes, threeStart, payload, 3u, &newTail);
    checkVerdict("wrapping store with a pad lands", verdict, IG_GTR_RING_OK);
    checkU32("pad-wrap slot n-2", ring[kRingDwords - 2u], payload[0]);
    checkU32("pad-wrap slot n-1", ring[kRingDwords - 1u], payload[1]);
    checkU32("pad-wrap payload wraps to slot 0", ring[0], payload[2]);
    checkU32("pad-wrap pad wraps to slot 1", ring[1], igGtMiNoop());
    check("pad-wrap left slot 2 alone", ring[2] == kCanary);

    // A pad of one dword means the total is four, so the tail advances four
    // dwords from n-2 and lands at byte 8.
    checkU32("pad-wrap total is four dwords",
             igGtrTotalDwords(threeStart, 3u), 4u);
    checkU32("pad-wrap tail is byte 8", newTail, 8u);
}

// ---------------------------------------------------------------------------
// Group 7: the batch-start join.
//
// igGtrWriteBatchStartToRing() composes MI_BATCH_BUFFER_START and stores it, and
// the load-bearing property is that the BATCH layer's refusal is PROPAGATED
// through an out-param rather than collapsed into a ring verdict. A caller told
// NOT_ADMISSIBLE for a misaligned batch address goes looking in the wrong layer
// entirely.
// ---------------------------------------------------------------------------

static void testBatchStartJoin(void)
{
    static IGU32 ring[kRingDwords];
    IGU32 batchVerdict = IG_GTQ_BATCH_NOT_ATTEMPTED;
    IGU32 newTail = 0u;
    IGU32 verdict;

    // A page-aligned batch address is what the composer accepts.
    IGU64 goodAddress = 0x0000000000200000ull;

    fillCanary(ring, kRingDwords);
    verdict = igGtrWriteBatchStartToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                                        0u, 0u, goodAddress,
                                        &batchVerdict, &newTail);
    checkVerdict("batch start lands in the ring", verdict, IG_GTR_RING_OK);
    gChecks++;
    if (!igGtqBatchSucceeded(batchVerdict)) {
        gFailures++;
        std::printf("FAIL batch composition refused: %s\n",
                    igGtqBatchVerdictName(batchVerdict));
    }

    // Three dwords: the header plus a 64-bit address. The low half must carry
    // the low 32 bits and the high half the top -- a swapped pair executes an
    // address as an opcode.
    checkU32("batch start low address dword", ring[1],
             (IGU32)(goodAddress & 0xFFFFFFFFull));
    checkU32("batch start high address dword", ring[2],
             (IGU32)(goodAddress >> 32));
    check("batch start header is not the address", ring[0] != ring[1]);

    // Three dwords from an aligned tail ends at 12 bytes, so one MI_NOOP pads.
    checkU32("batch start pads to alignment", ring[3], igGtMiNoop());
    check("batch start left the rest pristine",
          ringIsPristine(ring + 4, kRingDwords - 4u));
    checkU32("batch start advanced four dwords", newTail, 16u);

    // A misaligned batch address is the BATCH layer's refusal. The ring verdict
    // must say what the ring did and the batch verdict must carry the reason --
    // and nothing may be stored.
    fillCanary(ring, kRingDwords);
    batchVerdict = IG_GTQ_BATCH_NOT_ATTEMPTED;
    newTail = 0u;
    verdict = igGtrWriteBatchStartToRing(ring, kRingDwords, IG_GT_ENGINE_BLT,
                                        0u, 0u, goodAddress + 1ull,
                                        &batchVerdict, &newTail);
    check("misaligned batch address does not report ring success",
          !igGtrRingSucceeded(verdict));
    gChecks++;
    if (igGtqBatchSucceeded(batchVerdict)) {
        gFailures++;
        std::printf("FAIL misaligned address reported batch success\n");
    }
    gChecks++;
    if (batchVerdict == IG_GTQ_BATCH_NOT_ATTEMPTED) {
        gFailures++;
        std::printf("FAIL batch verdict left at NOT_ATTEMPTED\n");
    }
    check("misaligned batch address wrote nothing",
          ringIsPristine(ring, kRingDwords));

    // The batch refusal must NOT be collapsed into NOT_ADMISSIBLE, which is
    // the ring's "retry later" answer. Retrying a misaligned address forever
    // is the behaviour that collapse produces.
    check("batch refusal is not reported as retryable",
          verdict != IG_GTR_RING_NOT_ADMISSIBLE);

    // A null ring refuses through the ring family, not the batch family.
    batchVerdict = IG_GTQ_BATCH_NOT_ATTEMPTED;
    verdict = igGtrWriteBatchStartToRing(0, kRingDwords, IG_GT_ENGINE_BLT,
                                        0u, 0u, goodAddress,
                                        &batchVerdict, &newTail);
    checkVerdict("null ring refuses through the ring family", verdict,
                 IG_GTR_RING_NULL_SPAN);
}

int main(void)
{
    testVerdictFamily();
    testPadArithmetic();
    testClassifySubmit();
    testRefusalsDoNotWrite();
    testStraightStore();
    testWrappingStore();
    testBatchStartJoin();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
