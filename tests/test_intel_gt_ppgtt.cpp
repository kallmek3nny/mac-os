// Host tests for IntelGtPpgtt.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelGtPpgtt.hpp"

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

static void testAddressWalk(void)
{
    std::printf("\n1. four-level virtual-address split\n");
    const IGU64 va = 0x0000FEDCBA987654ull;
    checkU32("PML4 index", igGtPpgttPml4Index(va), 0x1FDu);
    checkU32("PDP index",  igGtPpgttPdpIndex(va),  0x172u);
    checkU32("PD index",   igGtPpgttPdIndex(va),   0x1D4u);
    checkU32("PT index",   igGtPpgttPtIndex(va),   0x187u);
    checkU32("page offset",igGtPpgttPageOffset(va),0x654u);

    checkU32("zero PML4", igGtPpgttPml4Index(0), 0u);
    checkU32("last PT index", igGtPpgttPtIndex(0x1FF000ull), 0x1FFu);
    checkU32("offset masks independently", igGtPpgttPageOffset(0xFFFFFFFFFFFFFull), 0xFFFu);
}

static void testEntries(void)
{
    std::printf("\n2. PPGTT entry composition\n");
    check("aligned address accepted", igGtPpgttPageAddressOk(0x12345000ull));
    check("misaligned address refused", !igGtPpgttPageAddressOk(0x12345001ull));
    check("46-bit ceiling accepted", igGtPpgttPageAddressOk(0x00003FFFFFFFF000ull));
    check("above 46-bit policy refused", !igGtPpgttPageAddressOk(0x0000400000000000ull));

    checkU64("table entry = address | present | rw",
             igGtPpgttTableEntry(0x12345000ull), 0x12345003ull);
    checkU64("misaligned table entry refuses",
             igGtPpgttTableEntry(0x12345004ull), 0ull);

    static const IGU64 expectedPat[8] = {
        0x00ull, 0x08ull, 0x10ull, 0x18ull,
        0x80ull, 0x88ull, 0x90ull, 0x98ull
    };
    for (IGU32 i = 0; i < 8; ++i) {
        char label[48];
        std::snprintf(label, sizeof(label), "PAT index %u", (unsigned)i);
        checkU64(label, igGtPpgttPatBits(i), expectedPat[i]);
    }
    checkU64("PAT index 8 refused", igGtPpgttPatBits(8u), 0xFFFFFFFFFFFFFFFFull);

    checkU64("leaf entry carries PAT 7",
             igGtPpgttLeafEntry(0x12345000ull, 7u), 0x1234509Bull);
    checkU64("leaf invalid PAT refuses", igGtPpgttLeafEntry(0x12345000ull, 8u), 0ull);
    checkU64("leaf bad address refuses", igGtPpgttLeafEntry(0x12345001ull, 0u), 0ull);
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

static void testLegacy64RootBlock(void)
{
    std::printf("\n3. Gen12 legacy64 LRC PPGTT block\n");
    IGU32 span[64];
    poison(span, 64);
    const IGU64 root = 0x0000000123456000ull;
    IGU32 written = 0xFFFFFFFFu;
    IGU32 verdict = igGtPpgttWriteLrcRootBlock(span, 64,
        IG_GT_ENGINE_BLT, IG_GTX_CTXDESC_LEGACY64_I915_3,
        &root, 1u, &written);

    checkU32("legacy64 succeeds", verdict, IG_GTPG_ROOT_OK);
    checkU32("writer reports 24 touched dwords", written, 24u);
    for (IGU32 s = 0x1Cu; s <= 0x20u; ++s) {
        char label[48];
        std::snprintf(label, sizeof(label), "NOP slot 0x%02X", (unsigned)s);
        checkU32(label, span[s], 0u);
    }
    checkU32("posted Gen12 LRI header", span[0x21], 0x11081011u);

    static const IGU32 rel[9] = {
        0x3A8u,0x28Cu,0x288u,0x284u,0x280u,0x27Cu,0x278u,0x274u,0x270u
    };
    for (IGU32 i = 0; i < 9; ++i) {
        char label[48];
        IGU32 slot = 0x22u + i * 2u;
        std::snprintf(label, sizeof(label), "pair %u register", (unsigned)i);
        checkU32(label, span[slot], 0x22000u + rel[i]);
    }

    // Four-level mode writes only PDP0 with the PML4 DMA address.
    checkU32("PDP3 UDW clear", span[0x25], 0u);
    checkU32("PDP3 LDW clear", span[0x27], 0u);
    checkU32("PDP2 UDW clear", span[0x29], 0u);
    checkU32("PDP2 LDW clear", span[0x2B], 0u);
    checkU32("PDP1 UDW clear", span[0x2D], 0u);
    checkU32("PDP1 LDW clear", span[0x2F], 0u);
    checkU32("PDP0 UDW", span[0x31], 0x00000001u);
    checkU32("PDP0 LDW", span[0x33], 0x23456000u);

    checkU32("slot before block untouched", span[0x1B], 0xA5A5A5A5u);
    checkU32("slot after block untouched", span[0x34], 0xA5A5A5A5u);
}

static void testLegacy32Roots(void)
{
    std::printf("\n4. legacy32 four-root assignment\n");
    IGU32 span[64];
    poison(span, 64);
    const IGU64 roots[4] = {
        0x0000000011111000ull,
        0x0000000122222000ull,
        0x0000000233333000ull,
        0x0000000344444000ull
    };
    IGU32 written = 0;
    checkU32("legacy32 succeeds",
             igGtPpgttWriteLrcRootBlock(span, 64, IG_GT_ENGINE_RENDER,
                 IG_GTX_CTXDESC_LEGACY32_I915_1, roots, 4u, &written),
             IG_GTPG_ROOT_OK);
    checkU32("legacy32 write count", written, 24u);

    checkU32("PDP0 UDW", span[0x31], 0x00000000u);
    checkU32("PDP0 LDW", span[0x33], 0x11111000u);
    checkU32("PDP1 UDW", span[0x2D], 0x00000001u);
    checkU32("PDP1 LDW", span[0x2F], 0x22222000u);
    checkU32("PDP2 UDW", span[0x29], 0x00000002u);
    checkU32("PDP2 LDW", span[0x2B], 0x33333000u);
    checkU32("PDP3 UDW", span[0x25], 0x00000003u);
    checkU32("PDP3 LDW", span[0x27], 0x44444000u);
    checkU32("render-relative first register", span[0x22], 0x000023A8u);
}

static void testRefusalsDoNotWrite(void)
{
    std::printf("\n5. refusals are atomic\n");
    IGU32 span[64];
    IGU64 roots[4] = {0x1000ull,0x2000ull,0x3000ull,0x4000ull};
    IGU32 written;

    poison(span,64); written=99;
    checkU32("short span refused",
             igGtPpgttWriteLrcRootBlock(span, 0x33u, IG_GT_ENGINE_BLT,
                 IG_GTX_CTXDESC_LEGACY64_I915_3, roots, 1u, &written),
             IG_GTPG_ROOT_SPAN_TOO_SHORT);
    check("short span untouched", allPoison(span,64)); checkU32("short count zero",written,0u);

    poison(span,64); written=99;
    checkU32("bad engine refused",
             igGtPpgttWriteLrcRootBlock(span, 64, 99u,
                 IG_GTX_CTXDESC_LEGACY64_I915_3, roots, 1u, &written),
             IG_GTPG_ROOT_BAD_ENGINE);
    check("bad engine untouched", allPoison(span,64)); checkU32("bad engine count zero",written,0u);

    poison(span,64); written=99;
    checkU32("bad mode refused",
             igGtPpgttWriteLrcRootBlock(span, 64, IG_GT_ENGINE_BLT,
                 2u, roots, 1u, &written), IG_GTPG_ROOT_BAD_MODE);
    check("bad mode untouched", allPoison(span,64)); checkU32("bad mode count zero",written,0u);

    poison(span,64); written=99;
    checkU32("wrong root count refused",
             igGtPpgttWriteLrcRootBlock(span, 64, IG_GT_ENGINE_BLT,
                 IG_GTX_CTXDESC_LEGACY64_I915_3, roots, 4u, &written),
             IG_GTPG_ROOT_BAD_COUNT);
    check("wrong count untouched", allPoison(span,64)); checkU32("wrong count write count zero",written,0u);

    poison(span,64); written=99; roots[0]=0x1001ull;
    checkU32("misaligned root refused",
             igGtPpgttWriteLrcRootBlock(span, 64, IG_GT_ENGINE_BLT,
                 IG_GTX_CTXDESC_LEGACY64_I915_3, roots, 1u, &written),
             IG_GTPG_ROOT_BAD_ADDRESS);
    check("misaligned root untouched", allPoison(span,64)); checkU32("bad root count zero",written,0u);

    poison(span,64); written=99; roots[0]=0x0000400000000000ull;
    checkU32("too-high root refused",
             igGtPpgttWriteLrcRootBlock(span, 64, IG_GT_ENGINE_BLT,
                 IG_GTX_CTXDESC_LEGACY64_I915_3, roots, 1u, &written),
             IG_GTPG_ROOT_BAD_ADDRESS);
    check("too-high root untouched", allPoison(span,64)); checkU32("high root count zero",written,0u);
}

int main(void)
{
    std::printf("IntelGtPpgtt.hpp -- Tiger Lake private address space/context roots\n");
    testAddressWalk();
    testEntries();
    testLegacy64RootBlock();
    testLegacy32Roots();
    testRefusalsDoNotWrite();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
