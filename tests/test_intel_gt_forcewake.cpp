//
//  test_intel_gt_forcewake.cpp
//  Host-side suite over phase0-kext/IntelGtForcewake.hpp -- the first GT layer
//  whose output is meant to reach the hardware.
//
//  WHAT IS PINNED
//  --------------
//    1. Cited constants, by value, so a typo reddens here as well as in
//       tools/check-registers.py.
//    2. The masked-write words. A bare 0x1 or 0x0 written to a masked register
//       is a silent no-op on hardware, so the EXACT words are pinned:
//       acquire 0x00010001, release 0x00010000, reset 0xEFFF0000.
//    3. The range table: watertight (each row starts one past the previous
//       row's end), sorted, and the engine bases this tree submits to land in
//       the domains i915 says they do -- RCS 0x2000 RENDER, BCS 0x22000 GT.
//    4. All three verdict families are bounded and named; both success
//       predicates are equalities with one preimage.
//    5. The ALL-ONES ORDER. 0xFFFFFFFF has FORCEWAKE_KERNEL set, so in the
//       wait-for-set phase it satisfies "ack is set". The ordering checks are
//       driven with exactly that input.
//    6. The sequence never writes before it has seen the ack clear, and never
//       reports HELD on an ack that was set before its own request.
//
//  WHAT THIS SUITE DOES NOT ESTABLISH
//  ----------------------------------
//  That the hardware acks. Every ack word below is supplied by this file.
//
#include <cstdio>
#include <cstring>

#include "IntelGtForcewake.hpp"

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

static void testCitedConstants(void)
{
    checkU32("FORCEWAKE_RENDER_GEN9", IG_GTW_REQ_RENDER_I915_0xA278, 0xA278u);
    checkU32("FORCEWAKE_ACK_RENDER_GEN9", IG_GTW_ACK_RENDER_I915_0xD84, 0xD84u);
    checkU32("FORCEWAKE_GT_GEN9", IG_GTW_REQ_GT_I915_0xA188, 0xA188u);
    checkU32("FORCEWAKE_ACK_GT_GEN9", IG_GTW_ACK_GT_I915_0x130044, 0x130044u);
    checkU32("FORCEWAKE_KERNEL", IG_GTW_KERNEL_I915_BIT0, 0x1u);
    checkU32("FORCEWAKE_KERNEL_FALLBACK", IG_GTW_KERNEL_FALLBACK_I915_BIT15, 0x8000u);
    checkU32("gen12 reset bits", IG_GTW_RESET_BITS_GEN12_I915_0xEFFF, 0xEFFFu);
    checkU32("poll budget equals i915 timeout at 1ms/poll",
             IG_GTW_ACK_POLL_BUDGET_POLICY_50, IG_GTW_ACK_TIMEOUT_MS_I915_50);
}

static void testMaskedWords(void)
{
    checkU32("acquire word", igGtwAcquireValue(), 0x00010001u);
    checkU32("release word", igGtwReleaseValue(), 0x00010000u);
    checkU32("reset word", igGtwResetValue(), 0xEFFF0000u);
    checkU32("enable fallback", igGtwMaskedEnable(IG_GTW_KERNEL_FALLBACK_I915_BIT15),
             0x80008000u);
    checkU32("enable truncates high bits", igGtwMaskedEnable(0x10001u), 0x00010001u);
    checkU32("disable truncates high bits", igGtwMaskedDisable(0x10001u), 0x00010000u);
    // The silent no-op this pair exists to prevent: mask half nonzero.
    check("acquire carries a mask", (igGtwAcquireValue() >> 16) != 0u);
    check("release carries a mask", (igGtwReleaseValue() >> 16) != 0u);
    checkU32("release clears the value half", igGtwReleaseValue() & 0xFFFFu, 0u);
}

static void testRangeTable(void)
{
    checkU32("row count", (IGU32)IG_GTW_GEN12_ROW_COUNT, 43u);
    checkU32("table starts at 0", kIgGtwGen12Ranges[0].start, 0u);

    bool watertight = true, ordered = true, domainsLegal = true;
    for (unsigned i = 0; i < IG_GTW_GEN12_ROW_COUNT; i++) {
        if (kIgGtwGen12Ranges[i].end < kIgGtwGen12Ranges[i].start) ordered = false;
        if (i > 0 && kIgGtwGen12Ranges[i].start != kIgGtwGen12Ranges[i - 1].end + 1u)
            watertight = false;
        IGU32 d = kIgGtwGen12Ranges[i].domain;
        if (d != IG_GTW_DOMAIN_NONE && d != IG_GTW_DOMAIN_RENDER
            && d != IG_GTW_DOMAIN_GT && d != IG_GTW_DOMAIN_MEDIA)
            domainsLegal = false;
    }
    check("rows are watertight", watertight);
    check("rows are ordered", ordered);
    check("every row names a real domain", domainsLegal);

    // The offsets this tree actually intends to touch.
    checkU32("RCS ring base -> render",
             igGtwDomainForOffset(IG_GT_RENDER_RING_BASE_I915_0x02000), IG_GTW_DOMAIN_RENDER);
    checkU32("BCS ring base -> gt",
             igGtwDomainForOffset(IG_GT_BLT_RING_BASE_I915_0x22000), IG_GTW_DOMAIN_GT);
    checkU32("BCS ring tail -> gt",
             igGtwDomainForOffset(IG_GT_BLT_RING_BASE_I915_0x22000 + 0x30u), IG_GTW_DOMAIN_GT);
    checkU32("BCS execlist control (+0x550) -> gt",
             igGtwDomainForOffset(IG_GT_BLT_RING_BASE_I915_0x22000 + 0x550u), IG_GTW_DOMAIN_GT);
    checkU32("RCS execlist control (+0x550) -> render",
             igGtwDomainForOffset(IG_GT_RENDER_RING_BASE_I915_0x02000 + 0x550u), IG_GTW_DOMAIN_RENDER);
    checkU32("VCS0 base -> media",
             igGtwDomainForOffset(IG_GT_BSD_RING_BASE_I915_0x1C0000), IG_GTW_DOMAIN_MEDIA);
    checkU32("VECS0 base -> media",
             igGtwDomainForOffset(IG_GT_VEBOX_RING_BASE_I915_0x1C8000), IG_GTW_DOMAIN_MEDIA);
    // The ack registers themselves: 0xd84 is always-on (it must be, or the ack
    // could never be read while the well sleeps).
    checkU32("render ack is always-on",
             igGtwDomainForOffset(IG_GTW_ACK_RENDER_I915_0xD84), IG_GTW_DOMAIN_NONE);
    checkU32("request regs 0xa188 -> gt range",
             igGtwDomainForOffset(IG_GTW_REQ_GT_I915_0xA188), IG_GTW_DOMAIN_GT);
    // Boundaries, both sides.
    checkU32("0x1fff none", igGtwDomainForOffset(0x1FFFu), IG_GTW_DOMAIN_NONE);
    checkU32("0x2000 render", igGtwDomainForOffset(0x2000u), IG_GTW_DOMAIN_RENDER);
    checkU32("0x26ff render", igGtwDomainForOffset(0x26FFu), IG_GTW_DOMAIN_RENDER);
    checkU32("0x2700 gt", igGtwDomainForOffset(0x2700u), IG_GTW_DOMAIN_GT);
    checkU32("last row end", igGtwDomainForOffset(0x1D3FFFu), IG_GTW_DOMAIN_MEDIA);
    checkU32("past the table", igGtwDomainForOffset(0x1D4000u), IG_GTW_DOMAIN_OUT_OF_TABLE);
    checkU32("all-ones offset", igGtwDomainForOffset(0xFFFFFFFFu), IG_GTW_DOMAIN_OUT_OF_TABLE);
}

static void testDomainAccessors(void)
{
    checkU32("render req", igGtwReqOffset(IG_GTW_DOMAIN_RENDER), 0xA278u);
    checkU32("render ack", igGtwAckOffset(IG_GTW_DOMAIN_RENDER), 0xD84u);
    checkU32("gt req", igGtwReqOffset(IG_GTW_DOMAIN_GT), 0xA188u);
    checkU32("gt ack", igGtwAckOffset(IG_GTW_DOMAIN_GT), 0x130044u);
    for (IGU32 d = 0; d <= IG_GTW_DOMAIN_LIMIT; d++) {
        const bool holdable = (d == IG_GTW_DOMAIN_RENDER || d == IG_GTW_DOMAIN_GT);
        checkBool("holdable is exactly render|gt", igGtwDomainHoldable(d), holdable);
        if (!holdable) {
            checkU32("unholdable req refused", igGtwReqOffset(d), IG_GT_BASE_INVALID);
            checkU32("unholdable ack refused", igGtwAckOffset(d), IG_GT_BASE_INVALID);
        }
    }
}

static void testFamilies(void)
{
    const char *names[16];
    for (IGU32 v = 0; v < IG_GTW_DOMAIN_LIMIT; v++) {
        names[v] = igGtwDomainName(v);
        check("domain named", std::strcmp(names[v], "unknown") != 0);
        for (IGU32 w = 0; w < v; w++) check("domain names distinct", std::strcmp(names[v], names[w]) != 0);
    }
    checkStr("domain limit sentinel", igGtwDomainName(IG_GTW_DOMAIN_LIMIT), "unknown");

    for (IGU32 v = 0; v < IG_GTW_ACK_LIMIT; v++) {
        names[v] = igGtwAckVerdictName(v);
        check("ack named", std::strcmp(names[v], "unknown") != 0);
        for (IGU32 w = 0; w < v; w++) check("ack names distinct", std::strcmp(names[v], names[w]) != 0);
    }
    checkStr("ack limit sentinel", igGtwAckVerdictName(IG_GTW_ACK_LIMIT), "unknown");

    for (IGU32 v = 0; v < IG_GTW_ACQ_LIMIT; v++) {
        names[v] = igGtwAcqVerdictName(v);
        check("acq named", std::strcmp(names[v], "unknown") != 0);
        for (IGU32 w = 0; w < v; w++) check("acq names distinct", std::strcmp(names[v], names[w]) != 0);
        checkBool("held has one preimage", igGtwHeld(v), v == IG_GTW_ACQ_HELD);
        checkBool("may-access is held|not-needed", igGtwMayAccess(v),
                  v == IG_GTW_ACQ_HELD || v == IG_GTW_ACQ_NOT_NEEDED);
    }
    checkStr("acq limit sentinel", igGtwAcqVerdictName(IG_GTW_ACQ_LIMIT), "unknown");
    checkBool("limit is not held", igGtwHeld(IG_GTW_ACQ_LIMIT), false);
}

static void testAckVerdictOrder(void)
{
    const IGU32 B = 3u;
    // All-ones BEFORE the bit, in the phase where the bit test would pass.
    checkU32("all-ones while waiting SET is dead, not ok",
             igGtwAckVerdict(0xFFFFFFFFu, true, 1u, B), IG_GTW_ACK_MMIO_DEAD);
    checkU32("all-ones while waiting CLEAR is dead",
             igGtwAckVerdict(0xFFFFFFFFu, false, 1u, B), IG_GTW_ACK_MMIO_DEAD);
    // Budget (caller bug) before everything.
    checkU32("zero budget beats all-ones",
             igGtwAckVerdict(0xFFFFFFFFu, true, 1u, 0u), IG_GTW_ACK_BAD_BUDGET);
    checkU32("zero budget beats a good ack",
             igGtwAckVerdict(0x1u, true, 1u, 0u), IG_GTW_ACK_BAD_BUDGET);
    // Bit before timeout: the last poll can succeed.
    checkU32("ok on the final poll", igGtwAckVerdict(0x1u, true, B, B), IG_GTW_ACK_OK);
    checkU32("clear ok on the final poll", igGtwAckVerdict(0x0u, false, B, B), IG_GTW_ACK_OK);
    checkU32("pending before budget", igGtwAckVerdict(0x0u, true, B - 1u, B), IG_GTW_ACK_PENDING);
    checkU32("timed out at budget", igGtwAckVerdict(0x0u, true, B, B), IG_GTW_ACK_TIMED_OUT);
    // Only bit 0 counts: a USER (bit1) or FALLBACK (bit15) reference is not ours.
    checkU32("user bit is not our ack", igGtwAckVerdict(0x2u, true, 1u, B), IG_GTW_ACK_PENDING);
    checkU32("fallback bit is not our ack", igGtwAckVerdict(0x8000u, true, 1u, B), IG_GTW_ACK_PENDING);
    checkU32("other bits do not block clear", igGtwAckVerdict(0x8002u, false, 1u, B), IG_GTW_ACK_OK);
}

// Runs the sequence against a scripted ack stream, recording what the caller
// would have done. Returns the verdict.
struct Trace {
    IGU32 writes;
    IGU32 writeOffset;
    IGU32 writeValue;
    IGU32 readsBeforeWrite;
    IGU32 reads;
    IGU32 readOffset;
};

static IGU32 runSeq(IGU32 domain, IGU32 budget, const IGU32 *acks, IGU32 nAcks, Trace *t)
{
    std::memset(t, 0, sizeof(*t));
    IGGtwSeq s;
    igGtwBegin(&s, domain, budget);
    IGU32 i = 0, guard = 0;
    while (!igGtwDone(&s) && guard++ < 1000u) {
        if (igGtwWantsWrite(&s)) {
            t->writes++;
            t->writeOffset = igGtwWriteOffset(&s);
            t->writeValue = igGtwWriteValue(&s);
            igGtwWrote(&s);
        } else {
            t->reads++;
            if (t->writes == 0u) t->readsBeforeWrite++;
            t->readOffset = igGtwReadOffset(&s);
            igGtwFeedAck(&s, i < nAcks ? acks[i] : acks[nAcks - 1u]);
            i++;
        }
    }
    return s.verdict;
}

static void testSequence(void)
{
    Trace t;
    const IGU32 B = IG_GTW_ACK_POLL_BUDGET_POLICY_50;

    {   // Happy path: clear, then set after our write.
        const IGU32 a[] = { 0x0u, 0x0u, 0x1u };
        checkU32("happy -> held", runSeq(IG_GTW_DOMAIN_GT, B, a, 3u, &t), IG_GTW_ACQ_HELD);
        checkU32("happy: one write", t.writes, 1u);
        checkU32("happy: write goes to GT req", t.writeOffset, 0xA188u);
        checkU32("happy: write is the acquire word", t.writeValue, 0x00010001u);
        checkU32("happy: one read before write", t.readsBeforeWrite, 1u);
        checkU32("happy: reads the GT ack", t.readOffset, 0x130044u);
    }
    {   // Ack already set at start: must NOT be reported held, must not write.
        const IGU32 a[] = { 0x1u };
        checkU32("stale set ack -> stale-ack", runSeq(IG_GTW_DOMAIN_RENDER, B, a, 1u, &t),
                 IG_GTW_ACQ_STALE_ACK);
        checkU32("stale: no write issued", t.writes, 0u);
        checkU32("stale: budget readings taken", t.reads, B);
    }
    {   // Clears, we request, never sets.
        const IGU32 a[] = { 0x0u };
        checkU32("never sets -> no-ack", runSeq(IG_GTW_DOMAIN_RENDER, B, a, 1u, &t),
                 IG_GTW_ACQ_NO_ACK);
        checkU32("no-ack: one write to render req", t.writeOffset, 0xA278u);
        checkU32("no-ack: reads = 1 + budget", t.reads, 1u + B);
    }
    {   // Dead BAR from the first read: never writes.
        const IGU32 a[] = { 0xFFFFFFFFu };
        checkU32("dead first -> mmio-dead", runSeq(IG_GTW_DOMAIN_GT, B, a, 1u, &t),
                 IG_GTW_ACQ_MMIO_DEAD);
        checkU32("dead first: no write", t.writes, 0u);
    }
    {   // Dies after our write -- the doubly-satisfying input. Must not be held.
        const IGU32 a[] = { 0x0u, 0xFFFFFFFFu };
        checkU32("dead after write -> mmio-dead, not held",
                 runSeq(IG_GTW_DOMAIN_GT, B, a, 2u, &t), IG_GTW_ACQ_MMIO_DEAD);
    }
    {   // Set on exactly the final allowed poll.
        IGU32 a[1 + 50];
        a[0] = 0x0u;
        for (int k = 1; k < 50; k++) a[k] = 0x0u;
        a[50] = 0x1u;
        checkU32("set on final poll -> held", runSeq(IG_GTW_DOMAIN_GT, B, a, 51u, &t),
                 IG_GTW_ACQ_HELD);
    }
    // Refusals at Begin.
    const IGU32 one[] = { 0x0u };
    checkU32("none -> not-needed", runSeq(IG_GTW_DOMAIN_NONE, B, one, 1u, &t), IG_GTW_ACQ_NOT_NEEDED);
    checkU32("not-needed: no traffic", t.reads + t.writes, 0u);
    checkU32("media -> bad-domain", runSeq(IG_GTW_DOMAIN_MEDIA, B, one, 1u, &t), IG_GTW_ACQ_BAD_DOMAIN);
    checkU32("out-of-table -> bad-domain",
             runSeq(IG_GTW_DOMAIN_OUT_OF_TABLE, B, one, 1u, &t), IG_GTW_ACQ_BAD_DOMAIN);
    checkU32("bad-domain: no traffic", t.reads + t.writes, 0u);
    checkU32("domain beats budget", runSeq(IG_GTW_DOMAIN_MEDIA, 0u, one, 1u, &t), IG_GTW_ACQ_BAD_DOMAIN);
    checkU32("zero budget -> bad-budget", runSeq(IG_GTW_DOMAIN_GT, 0u, one, 1u, &t), IG_GTW_ACQ_BAD_BUDGET);

    // A finished sequence ignores further input.
    IGGtwSeq s;
    igGtwBegin(&s, IG_GTW_DOMAIN_GT, 2u);
    igGtwFeedAck(&s, 0x0u);
    igGtwWrote(&s);
    igGtwFeedAck(&s, 0x1u);
    checkU32("held", s.verdict, IG_GTW_ACQ_HELD);
    igGtwFeedAck(&s, 0xFFFFFFFFu);
    igGtwWrote(&s);
    checkU32("done sequence is inert", s.verdict, IG_GTW_ACQ_HELD);
    checkU32("done: no read offset", igGtwReadOffset(&s), IG_GT_BASE_INVALID);
    checkU32("done: no write offset", igGtwWriteOffset(&s), IG_GT_BASE_INVALID);
    checkU32("done: no write value", igGtwWriteValue(&s), 0u);

    // Wrote() before the request phase does not skip the clear wait.
    igGtwBegin(&s, IG_GTW_DOMAIN_GT, 2u);
    igGtwWrote(&s);
    checkU32("early Wrote() ignored", s.phase, IG_GTW_PHASE_WAIT_CLEAR);
}

int main(void)
{
    std::printf("test_intel_gt_forcewake -- IntelGtForcewake.hpp\n\n");

    testCitedConstants();
    testMaskedWords();
    testRangeTable();
    testDomainAccessors();
    testFamilies();
    testAckVerdictOrder();
    testSequence();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
