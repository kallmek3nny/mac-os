// Host tests for IntelGtCsb.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelGtCsb.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void check(const char *label, bool ok)
{
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  FAIL %s\n", label);
    }
}

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::printf("  FAIL %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

static bool sameEvent(const IGGtCsbEvent &a, const IGGtCsbEvent &b)
{
    return a.toContextId == b.toContextId &&
           a.awayContextId == b.awayContextId &&
           a.switchDetail == b.switchDetail &&
           a.newQueue == b.newQueue;
}

static IGU32 idWord(IGU32 id)
{
    return (id & 0x7FFu) << 15;
}

static void testSourceBackedGeometry(void)
{
    std::printf("\n1. Gen11+ CSB geometry and pointer words\n");
    checkU32("entry count", IG_GTCSB_ENTRIES_I915_12, 12u);
    checkU32("pointer mask", IG_GTCSB_PTR_MASK_I915_0xF, 0xFu);

    checkU32("slot 0 offset", igGtCsbSlotOffset(0u), 0x370u);
    checkU32("slot 5 offset", igGtCsbSlotOffset(5u), 0x398u);
    checkU32("slot 6 offset", igGtCsbSlotOffset(6u), 0x3C0u);
    checkU32("slot 11 offset", igGtCsbSlotOffset(11u), 0x3E8u);
    checkU32("slot 12 refused", igGtCsbSlotOffset(12u), 0xFFFFFFFFu);

    checkU32("next slot 0", igGtCsbNextSlot(0u), 1u);
    checkU32("next slot wraps 11 to 0", igGtCsbNextSlot(11u), 0u);
    checkU32("bad next slot sentinel", igGtCsbNextSlot(12u), 12u);

    checkU32("write pointer low nibble", igGtCsbWritePointer(0xABCDEF0Bu), 11u);
    check("write pointer 11 accepted", igGtCsbWritePointerOk(0x0000000Bu));
    check("write pointer 12 refused", !igGtCsbWritePointerOk(0x0000000Cu));
    check("write pointer 15 refused", !igGtCsbWritePointerOk(0xFFFFFFFFu));

    checkU32("ACK slot 0", igGtCsbReadPointerAckWord(0u), 0x0F000000u);
    checkU32("ACK slot 11", igGtCsbReadPointerAckWord(11u), 0x0F000B00u);
    checkU32("ACK invalid slot refuses", igGtCsbReadPointerAckWord(12u), 0u);
}

static void testFieldDecode(void)
{
    std::printf("\n2. Tiger Lake CSB field decode\n");
    checkU32("context id shift", igGtCsbContextId(idWord(0x155u)), 0x155u);
    checkU32("idle context id", igGtCsbContextId(idWord(0x7FFu)), 0x7FFu);
    check("idle predicate", igGtCsbContextIdIsIdle(0x7FFu));
    check("non-idle predicate", !igGtCsbContextIdIsIdle(0x7FEu));
    check("expected id lower bound", igGtCsbExpectedContextIdOk(1u));
    check("expected id upper bound", igGtCsbExpectedContextIdOk(0x7FEu));
    check("expected id zero refused", !igGtCsbExpectedContextIdOk(0u));
    check("idle id cannot be expected", !igGtCsbExpectedContextIdOk(0x7FFu));
}

static void testCompletionAndNonCompletion(void)
{
    std::printf("\n3. retirement classification\n");
    const IGU32 expected = 42u;
    IGGtCsbEvent event = {0u,0u,0u,false};

    IGU32 verdict = igGtCsbParseTgl(idWord(0x7FFu), idWord(expected), expected, &event);
    checkU32("completion parses", verdict, IG_GTCSB_PARSE_OK);
    checkU32("completion to idle", event.toContextId, 0x7FFu);
    checkU32("completion away expected", event.awayContextId, expected);
    checkU32("completion detail zero", event.switchDetail, 0u);
    check("completion not new queue", !event.newQueue);
    check("completion retires", igGtCsbEventRetiresContext(event, expected));

    verdict = igGtCsbParseTgl(idWord(expected), idWord(0x7FFu), expected, &event);
    checkU32("switch-to parses", verdict, IG_GTCSB_PARSE_OK);
    check("switch-to does not retire", !igGtCsbEventRetiresContext(event, expected));

    verdict = igGtCsbParseTgl(idWord(0x7FFu) | 1u, idWord(expected), expected, &event);
    checkU32("new-queue completion-shaped entry parses", verdict, IG_GTCSB_PARSE_OK);
    check("new queue bit decoded", event.newQueue);
    check("new queue blocks retirement", !igGtCsbEventRetiresContext(event, expected));

    verdict = igGtCsbParseTgl(idWord(0x7FFu), idWord(expected) | 5u, expected, &event);
    checkU32("preempted entry parses", verdict, IG_GTCSB_PARSE_OK);
    checkU32("preempted detail", event.switchDetail, 5u);
    check("preemption does not retire", !igGtCsbEventRetiresContext(event, expected));

    verdict = igGtCsbParseTgl(idWord(expected), idWord(expected), expected, &event);
    checkU32("self-to-self parses", verdict, IG_GTCSB_PARSE_OK);
    check("self-to-self does not retire", !igGtCsbEventRetiresContext(event, expected));
}

static void testRefusalsAtomic(void)
{
    std::printf("\n4. refusal paths are atomic\n");
    const IGU32 expected = 42u;
    const IGGtCsbEvent poison = {0x11111111u,0x22222222u,0x33333333u,true};
    IGGtCsbEvent out;

    out = poison;
    checkU32("null output refused",
             igGtCsbParseTgl(0u,0u,expected,0), IG_GTCSB_PARSE_NULL_OUT);

    out = poison;
    checkU32("expected id zero refused",
             igGtCsbParseTgl(0u,0u,0u,&out), IG_GTCSB_PARSE_BAD_EXPECTED_ID);
    check("bad expected leaves output untouched", sameEvent(out,poison));

    out = poison;
    checkU32("all-ones low refused",
             igGtCsbParseTgl(0xFFFFFFFFu,idWord(expected),expected,&out),
             IG_GTCSB_PARSE_UNAVAILABLE);
    check("all-ones low leaves output untouched", sameEvent(out,poison));

    out = poison;
    checkU32("all-ones high refused",
             igGtCsbParseTgl(idWord(expected),0xFFFFFFFFu,expected,&out),
             IG_GTCSB_PARSE_UNAVAILABLE);
    check("all-ones high leaves output untouched", sameEvent(out,poison));

    out = poison;
    checkU32("reserved detail 6 refused",
             igGtCsbParseTgl(idWord(0x7FFu),idWord(expected)|6u,expected,&out),
             IG_GTCSB_PARSE_BAD_DETAIL);
    check("bad detail leaves output untouched", sameEvent(out,poison));

    out = poison;
    checkU32("foreign to-context refused",
             igGtCsbParseTgl(idWord(7u),idWord(expected),expected,&out),
             IG_GTCSB_PARSE_UNEXPECTED_TO);
    check("foreign to leaves output untouched", sameEvent(out,poison));

    out = poison;
    checkU32("foreign away-context refused",
             igGtCsbParseTgl(idWord(expected),idWord(7u),expected,&out),
             IG_GTCSB_PARSE_UNEXPECTED_AWAY);
    check("foreign away leaves output untouched", sameEvent(out,poison));
}

static void testVerdictFamily(void)
{
    std::printf("\n5. verdict family\n");
    check("only OK succeeds", igGtCsbParseSucceeded(IG_GTCSB_PARSE_OK));
    for (IGU32 v = 0u; v < IG_GTCSB_PARSE_LIMIT; ++v) {
        if (v != IG_GTCSB_PARSE_OK) {
            check("non-OK verdict refuses", !igGtCsbParseSucceeded(v));
        }
        check("every in-range verdict has a name",
              std::strcmp(igGtCsbParseVerdictName(v), "unknown CSB parse verdict") != 0);
    }
}

int main(void)
{
    std::printf("IntelGtCsb.hpp -- Tiger Lake CSB decode/retirement\n");
    testSourceBackedGeometry();
    testFieldDecode();
    testCompletionAndNonCompletion();
    testRefusalsAtomic();
    testVerdictFamily();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
