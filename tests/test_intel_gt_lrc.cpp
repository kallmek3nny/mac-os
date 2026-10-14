// Host tests for IntelGtLrc.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include "IntelGtLrc.hpp"

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

static void checkU64(const char *label, IGU64 got, IGU64 want)
{
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::printf("  FAIL %s: got 0x%016llX want 0x%016llX\n",
                    label, (unsigned long long)got, (unsigned long long)want);
    }
}

static void poison(IGU32 *span, IGU32 count)
{
    for (IGU32 i = 0; i < count; ++i) span[i] = 0xA5A5A5A5u;
}

static bool allPoison(const IGU32 *span, IGU32 count)
{
    for (IGU32 i = 0; i < count; ++i) {
        if (span[i] != 0xA5A5A5A5u) return false;
    }
    return true;
}

static void testFixedWords(void)
{
    std::printf("\n1. fixed Gen12 XCS words\n");
    checkU32("13-register posted LRI", igGtLrcFirstBlockLriHeader(), 0x11081019u);
    checkU32("first restore inhibited", igGtLrcContextControl(true), 0x00090009u);
    checkU32("later restore allowed", igGtLrcContextControl(false), 0x00090008u);
    checkU32("Gen12 close word", igGtLrcCloseWord(), 0x05000001u);
    check("page-aligned 32-bit ring start accepted", igGtLrcRingStartOk(0xABC000ull));
    check("misaligned ring start refused", !igGtLrcRingStartOk(0xABC004ull));
    check("last 32-bit page accepted", igGtLrcRingStartOk(0xFFFFF000ull));
    check("above 32-bit ring start refused", !igGtLrcRingStartOk(0x100000000ull));
}

static void testBcsDescriptor(void)
{
    std::printf("\n2. Tiger Lake BCS0 descriptor\n");
    checkU64("legacy64 first restore",
             igGtLrcBcs0Descriptor(0x1000ull, 1u, 3u, true),
             0x200000200000111Dull);
    checkU64("legacy64 later restore",
             igGtLrcBcs0Descriptor(0x1000ull, 1u, 3u, false),
             0x2000002000001119ull);
    checkU64("legacy32 first restore",
             igGtLrcBcs0Descriptor(0x1000ull, 1u, 1u, true),
             0x200000200000110Dull);
    checkU64("largest LRCA/id",
             igGtLrcBcs0Descriptor(0xFFFFF000ull, 0x7FFu, 3u, true),
             0x2000FFE0FFFFF11Dull);
    checkU64("mode 2 refused",
             igGtLrcBcs0Descriptor(0x1000ull, 1u, 2u, true), 0ull);
    checkU64("unaligned LRCA refused",
             igGtLrcBcs0Descriptor(0x1001ull, 1u, 3u, true), 0ull);
    checkU64("context id overflow refused",
             igGtLrcBcs0Descriptor(0x1000ull, 0x800u, 3u, true), 0ull);

    const IGU64 desc = igGtLrcBcs0Descriptor(0x1000ull, 1u, 3u, true);
    checkU32("descriptor low dword", igGtxDescriptorLow(desc), 0x0000111Du);
    checkU32("descriptor high carries BCS class", igGtxDescriptorHigh(desc), 0x20000020u);
}

static void testFirstBlock(void)
{
    std::printf("\n3. first Gen12 XCS context block\n");
    IGU32 span[64];
    poison(span, 64);
    IGU32 written = 0xFFFFFFFFu;
    IGU32 verdict = igGtLrcWriteFirstBlock(span, 64, IG_GT_ENGINE_BLT,
                                            0x1000u, 0x00ABC000ull,
                                            0u, 0u, true, &written);
    checkU32("BCS0 first block succeeds", verdict, IG_GTLRC_WRITE_OK);
    checkU32("first block writes 28 dwords", written, 28u);
    checkU32("slot 0 NOOP", span[0], 0u);
    checkU32("slot 1 LRI", span[1], 0x11081019u);

    static const IGU32 rel[13] = {
        0x244u,0x034u,0x030u,0x038u,0x03Cu,0x168u,0x140u,
        0x110u,0x1C0u,0x1C4u,0x1C8u,0x180u,0x2B4u
    };
    for (IGU32 i = 0; i < 13u; ++i) {
        char label[48];
        const IGU32 slot = 2u + i * 2u;
        std::snprintf(label, sizeof(label), "pair %u register", (unsigned)i);
        checkU32(label, span[slot], 0x22000u + rel[i]);
    }

    checkU32("context control value", span[0x03], 0x00090009u);
    checkU32("ring head value", span[0x05], 0u);
    checkU32("ring tail value", span[0x07], 0u);
    checkU32("ring start value", span[0x09], 0x00ABC000u);
    checkU32("ring control value", span[0x0B], 0x00000001u);
    checkU32("unused pair value stays zero", span[0x0D], 0u);
    checkU32("last pair value stays zero", span[0x1B], 0u);
    checkU32("slot after first block untouched", span[0x1C], 0xA5A5A5A5u);

    poison(span, 64);
    written = 0;
    verdict = igGtLrcWriteFirstBlock(span, 64, IG_GT_ENGINE_RENDER,
                                     0x2000u, 0x00100000ull,
                                     0x20u, 0x18u, false, &written);
    checkU32("render first block succeeds", verdict, IG_GTLRC_WRITE_OK);
    checkU32("render context control", span[0x03], 0x00090008u);
    checkU32("render ring head", span[0x05], 0x20u);
    checkU32("render ring tail", span[0x07], 0x18u);
    checkU32("two-page ring control", span[0x0B], 0x00001001u);
    checkU32("render first register", span[0x02], 0x00002244u);
}

static void testRefusalsAreAtomic(void)
{
    std::printf("\n4. first-block refusals are atomic\n");
    IGU32 span[64];
    IGU32 written = 99u;

    checkU32("null span refused",
             igGtLrcWriteFirstBlock(0, 64, IG_GT_ENGINE_BLT, 0x1000u,
                                    0x100000ull, 0u, 0u, true, &written),
             IG_GTLRC_WRITE_NULL_SPAN);
    checkU32("null span count zero", written, 0u);

#define REFUSE_CASE(label, want, count, engine, bytes, start, head, tail) \
    do { \
        poison(span, 64); written = 99u; \
        checkU32(label, igGtLrcWriteFirstBlock(span, count, engine, bytes, \
                    start, head, tail, true, &written), want); \
        check(label " untouched", allPoison(span, 64)); \
        checkU32(label " count zero", written, 0u); \
    } while (0)

    REFUSE_CASE("short span", IG_GTLRC_WRITE_SPAN_TOO_SHORT,
                27u, IG_GT_ENGINE_BLT, 0x1000u, 0x100000ull, 0u, 0u);
    REFUSE_CASE("bad engine", IG_GTLRC_WRITE_BAD_ENGINE,
                64u, 99u, 0x1000u, 0x100000ull, 0u, 0u);
    REFUSE_CASE("bad ring", IG_GTLRC_WRITE_BAD_RING,
                64u, IG_GT_ENGINE_BLT, 0u, 0x100000ull, 0u, 0u);
    REFUSE_CASE("misaligned start", IG_GTLRC_WRITE_BAD_START,
                64u, IG_GT_ENGINE_BLT, 0x1000u, 0x100004ull, 0u, 0u);
    REFUSE_CASE("high start", IG_GTLRC_WRITE_BAD_START,
                64u, IG_GT_ENGINE_BLT, 0x1000u, 0x100000000ull, 0u, 0u);
    REFUSE_CASE("head outside", IG_GTLRC_WRITE_HEAD_OUT_OF_RING,
                64u, IG_GT_ENGINE_BLT, 0x1000u, 0x100000ull, 0x1000u, 0u);
    REFUSE_CASE("tail outside", IG_GTLRC_WRITE_TAIL_OUT_OF_RING,
                64u, IG_GT_ENGINE_BLT, 0x1000u, 0x100000ull, 0u, 0x1000u);
    REFUSE_CASE("tail unaligned", IG_GTLRC_WRITE_TAIL_UNALIGNED,
                64u, IG_GT_ENGINE_BLT, 0x1000u, 0x100000ull, 0u, 4u);

#undef REFUSE_CASE
}

int main(void)
{
    std::printf("IntelGtLrc.hpp -- Tiger Lake XCS context + BCS0 descriptor\n");
    testFixedWords();
    testBcsDescriptor();
    testFirstBlock();
    testRefusalsAreAtomic();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
