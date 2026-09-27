//
//  test_intel_scanout.cpp
//  Drives phase0-kext/IntelScanout.hpp -- the Gen12 scanout descriptor -- under
//  MSVC on a machine with no Intel GPU. The header includes nothing and every
//  function in it is a total function of its arguments, so there is no fixture,
//  no journal and no register file here.
//
//  TWO RULES, same as tests/test_intel_framebuffer.cpp:
//
//    1. Every expected value is a BARE LITERAL. Checking a decode against the
//       same #define the decode reads cancels out and proves nothing.
//    2. Observed values carry the capture they came from.
//
//  WHAT EACH SECTION IS TRYING TO CATCH
//  ------------------------------------
//   1. IG_SCAN_INVALID32 equals IG_MMIO_INVALID's value. The header spells it
//      rather than including IntelDisplay.hpp, and says in its own comment that
//      "the two are pinned equal by the test suite instead". This is that pin.
//   2. The refusal POLARITY: igScanSurfRefused(0) is deliberately FALSE, because
//      zero is a legal graphics address -- the base of the aperture.
//   3. The masked sentinel. 0xFFFFFFFF masked is 0xFFFFF000, which IS 4 KiB
//      aligned -- so an alignment-first order would report a refused read as a
//      well-aligned plane. This is why NO_SURFACE runs first.
//   4. The 64-bit products. Both multiplications wrap in 32 bits, and a wrapped
//      product is SMALLER than the truth, so truncation looks SAFER while
//      pointing the display engine past the end of the mapping.
//   5. The aperture edge, pinned on BOTH sides -- containment is one-sided, so
//      an exact fit must accept and one byte more must refuse.
//   6. REFUSAL ORDER, driven by inputs bad in two ways at once. Prose in the
//      header is not the assertion; these pairs are.
//   7. igScanUsable() is an EQUALITY against OK, so every error code is
//      unusable. The loop is bounded on IG_SCAN_VERDICT_LIMIT with `<=`, because
//      that limit is INCLUSIVE -- and so is EVERY verdict family in this track.
//      An earlier revision of this line said IG_MODE_VERDICT_LIMIT was the
//      exclusive counterexample. It never was: measured 2026-08-19, it is 6 and
//      IG_MODE_SRC_MISMATCH is 6, and tools/check-registers.py enforces
//      limit == max(codes) per family, so an exclusive limit could not pass the
//      register gate. A comment inverting the convention is handoff20 SS1's own
//      defect in prose form -- arms 0..5 walked against an inclusive 6, so the
//      last verdict published the unknown string with all four gates green.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelScanout.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i) std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGS32 got, IGS32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkU64(const char *label, IGS64 got, IGS64 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%016llX\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%016llX want 0x%016llX\n", label, got, want);
    }
}

static void checkInt(const char *label, int got, int want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s %d\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got %d want %d\n", label, got, want);
    }
}

//
//  1. The sentinel pin.
//
static void testSentinelPin(void)
{
    section("1. sentinels, spelled as bare literals");

    //  IG_MMIO_INVALID in IntelDisplay.hpp is 0xFFFFFFFF. This header cannot
    //  include it, so the equality lives here or nowhere.
    checkU32("IG_SCAN_INVALID32 is the refused-read value",
             IG_SCAN_INVALID32, 0xFFFFFFFFu);
    checkU64("IG_SCAN_INVALID64 is all ones in 64 bits",
             IG_SCAN_INVALID64, 0xFFFFFFFFFFFFFFFFull);
    checkU64("the extent ceiling is a 32-bit maximum",
             IG_SCAN_EXTENT_POLICY_CEILING, 0xFFFFFFFFull);
    checkU32("the surface address mask keeps the 4 KiB-aligned bits",
             IG_SCAN_SURF_ADDR_MASK, 0xFFFFF000u);
    checkU32("the surface alignment is 4096 bytes",
             IG_SCAN_SURF_ALIGN_BYTES, 0x1000u);

    //  Every verdict pinned to its own bare literal. A copy-paste arm giving
    //  two verdicts the same number satisfies every "is an error" assertion in
    //  this file at once -- igScanUsable() would still be false for both, the
    //  ordering checks would still see a refusal, and only these seven lines
    //  can tell them apart.
    checkU32("OK is zero",                IG_SCAN_OK,                   0u);
    checkU32("NO_SURFACE is one",         IG_SCAN_ERR_NO_SURFACE,       1u);
    checkU32("UNALIGNED is two",          IG_SCAN_ERR_UNALIGNED,        2u);
    checkU32("ZERO_GEOMETRY is three",    IG_SCAN_ERR_ZERO_GEOMETRY,    3u);
    checkU32("ROW_TOO_SHORT is four",     IG_SCAN_ERR_ROW_TOO_SHORT,    4u);
    checkU32("EXTENT_OVERFLOW is five",   IG_SCAN_ERR_EXTENT_OVERFLOW,  5u);
    checkU32("OUTSIDE_APERTURE is six",   IG_SCAN_ERR_OUTSIDE_APERTURE, 6u);

    //  The family limit is INCLUSIVE here. Spelled as a literal so that a
    //  seventh verdict reddens this line rather than silently widening a loop.
    checkU32("IG_SCAN_VERDICT_LIMIT is the largest verdict",
             IG_SCAN_VERDICT_LIMIT, 6u);
}

//
//  2 and 3. Refusal polarity, and the masked sentinel that fixes the order.
//
static void testSurfaceWord(void)
{
    section("2. the PLANE_SURF word: refusal, address, alignment");

    checkInt("all-ones is a refused read",
             igScanSurfRefused(0xFFFFFFFFu), 1);

    //  Zero is the base of the aperture, not an unprogrammed plane. This is a
    //  decision about THIS register, and the opposite polarity to families
    //  where zero means nothing was programmed.
    checkInt("zero is NOT a refused read",
             igScanSurfRefused(0u), 0);
    checkInt("a real plane base is not a refused read",
             igScanSurfRefused(0x00100000u), 0);

    //  The mask is applied to any input, refused ones included, so a masked
    //  all-ones address stays distinguishable from a plane at the top of the
    //  space rather than collapsing into it.
    checkU32("the low twelve bits are flags, not address",
             igScanSurfaceAddress(0x00100FFFu), 0x00100000u);
    checkU32("a refused word still masks rather than folding",
             igScanSurfaceAddress(0xFFFFFFFFu), 0xFFFFF000u);

    //  And this is the whole reason NO_SURFACE is tested before UNALIGNED:
    //  0xFFFFF000 is 4 KiB aligned, so an alignment-first order calls a read
    //  that never happened a well-aligned plane.
    checkInt("a 4 KiB-aligned base is aligned",
             igScanSurfaceAligned(0x00100000u), 1);
    checkInt("one byte off is not aligned",
             igScanSurfaceAligned(0x00100001u), 0);
    checkInt("the MASKED sentinel is aligned -- the order trap",
             igScanSurfaceAligned(0xFFFFF000u), 1);
}

//
//  4. The two multiplications, exercised past 32 bits.
//
static void testWideProducts(void)
{
    section("4. both products are 64-bit before the multiply");

    //  A 1920x1080 XRGB_8888 desktop: 7680 bytes per row, 1080 rows.
    checkU64("1920x1080 XRGB_8888 extent",
             igScanExtentBytes(7680u, 1080u), 8294400ull);

    //  0x10000 * 0x10000 is 0x1_0000_0000 -- exactly one past 32 bits, and in
    //  32-bit arithmetic it is ZERO, which passes every containment test there
    //  is while describing a framebuffer of no size.
    checkU64("a product one past 32 bits does not wrap to zero",
             igScanExtentBytes(0x10000u, 0x10000u), 0x100000000ull);
    checkU64("the widest possible extent is exact",
             igScanExtentBytes(0xFFFFFFFFu, 0xFFFFFFFFu), 0xFFFFFFFE00000001ull);

    checkU64("minimum row for 1920 at 4 bytes per pixel",
             igScanMinimumRowBytes(1920u, 4u), 7680ull);
    checkU64("a wrapped minimum row would compare as adequate",
             igScanMinimumRowBytes(0x40000000u, 4u), 0x100000000ull);

    //  The ceiling test refuses an empty extent as well as an oversized one.
    checkInt("a zero extent does not fit",       igScanExtentFits(0ull), 0);
    checkInt("an extent at the ceiling fits",    igScanExtentFits(0xFFFFFFFFull), 1);
    checkInt("one byte past the ceiling does not",
             igScanExtentFits(0x100000000ull), 0);
}

//
//  5. The aperture, pinned on both edges.
//
static void testAperture(void)
{
    section("5. containment is one-sided, so pin both edges");

    //  base + extent == apertureBytes is the last accepting case.
    checkInt("an exact fit is inside",
             igScanWithinAperture(0x1000u, 0x1000ull, 0x2000ull), 1);
    checkInt("one byte more is outside",
             igScanWithinAperture(0x1000u, 0x1001ull, 0x2000ull), 0);

    //  base < apertureBytes is the naive test this header refuses to use: a
    //  base one byte inside a mapping is inside it by that test and still
    //  overhangs by a whole framebuffer.
    checkInt("a base inside with an extent that overhangs is outside",
             igScanWithinAperture(0x1FFFu, 0x1000ull, 0x2000ull), 0);

    //  A caller that mapped nothing is refused rather than trusted.
    checkInt("a zero aperture refuses everything",
             igScanWithinAperture(0u, 0x1000ull, 0ull), 0);
    checkInt("a zero aperture refuses a zero extent too",
             igScanWithinAperture(0u, 0ull, 0ull), 0);
}

//
//  5b. Every arm reached on its own.
//
//  Section 6 pins the ORDER with doubly-bad inputs, and an ordering claim says
//  nothing about whether an arm is reachable at all. An arm that is dead -- as
//  NVGSP_STAGE_ERR_EMPTY_BOOTLOADER was one track over, provably unreachable
//  behind an earlier guard -- passes every ordering check ever written, because
//  every ordering check names some OTHER arm as the winner. These do the
//  complement: one defect each, so the arm under test is the only candidate.
//
static void testEveryArmReachable(void)
{
    section("5b. each refusal arm reached by a singly-bad input");

    const IGS64 ap = 0x10000000ull;

    //  Aligned, sane geometry, only the surface word refused. 0xFFFFFFFF is
    //  unavoidably also unaligned, which is exactly why NO_SURFACE has to run
    //  first -- there is no input that is refused and NOT unaligned.
    checkU32("NO_SURFACE",
             igScanDescribe(0xFFFFFFFFu, 7680u, 1920u, 1080u, 4u, ap), 1u);

    checkU32("UNALIGNED alone",
             igScanDescribe(0x00100001u, 7680u, 1920u, 1080u, 4u, ap), 2u);

    //  All four geometry terms, separately. A guard written as a three-way test
    //  passes every check that only ever zeroes one particular term.
    checkU32("ZERO_GEOMETRY from width",
             igScanDescribe(0x00100000u, 7680u, 0u, 1080u, 4u, ap), 3u);
    checkU32("ZERO_GEOMETRY from height",
             igScanDescribe(0x00100000u, 7680u, 1920u, 0u, 4u, ap), 3u);
    checkU32("ZERO_GEOMETRY from bytesPerRow",
             igScanDescribe(0x00100000u, 0u, 1920u, 1080u, 4u, ap), 3u);
    checkU32("ZERO_GEOMETRY from bytesPerPixel",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 0u, ap), 3u);

    //  One byte short of the 7680 the pixels need.
    checkU32("ROW_TOO_SHORT alone",
             igScanDescribe(0x00100000u, 7679u, 1920u, 1080u, 4u, ap), 4u);

    //  65536 * 65536 is 0x1_0000_0000, and 16384 * 4 is 65536, so the row is
    //  exactly adequate and the aperture is large enough for the true extent.
    //  The ONLY defect is that the extent exceeds the 32-bit ceiling.
    checkU32("EXTENT_OVERFLOW alone",
             igScanDescribe(0x00100000u, 0x10000u, 0x4000u, 0x10000u, 4u,
                            0xFFFFFFFFFFull), 5u);

    checkU32("OUTSIDE_APERTURE alone",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 4u, 0x101000ull),
             6u);
}

//
//  5c. The ACCEPTING edges of the three numeric tests.
//
//  A refusal test pinned only on the refusing side is satisfied by a guard that
//  refuses everything. Each pair below straddles one boundary by a single byte,
//  so an off-by-one in either direction reddens exactly one of the two.
//
static void testAcceptingEdges(void)
{
    section("5c. the last accepting value of each numeric test");

    //  A row exactly equal to the minimum is legal -- no padding required.
    checkU32("a row exactly at the minimum is accepted",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 4u, 0x10000000ull),
             0u);
    checkU32("one byte less is not",
             igScanDescribe(0x00100000u, 7679u, 1920u, 1080u, 4u, 0x10000000ull),
             4u);

    //  65537 * 65535 is exactly 0xFFFFFFFF -- the ceiling itself, which fits.
    //  The aperture is sized to hold base + extent so containment cannot be
    //  what decides these two.
    checkU32("an extent exactly at the ceiling is accepted",
             igScanDescribe(0x00100000u, 0x10001u, 0x4000u, 0xFFFFu, 4u,
                            0x200000000ull),
             0u);
    checkU32("one row more overflows",
             igScanDescribe(0x00100000u, 0x10001u, 0x4000u, 0x10000u, 4u,
                            0x200000000ull),
             5u);

    //  base + extent == apertureBytes, exactly. 0x00100000 + 0x7E9000 is the
    //  1920x1080 desktop ending on the final byte of the mapping.
    checkU32("a plane ending on the last byte of the aperture is inside",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 4u, 0x8E9000ull),
             0u);
    checkU32("an aperture one byte smaller refuses it",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 4u, 0x8E8FFFull),
             6u);
}

//
//  6. The refusal ORDER, driven by inputs bad in two ways at once.
//
//  A single-defect input cannot separate these arms: it returns the same
//  verdict under every ordering. Only a doubly-bad input distinguishes them,
//  which is why each check below breaks exactly two rules and asserts which
//  one wins.
//
static void testRefusalOrder(void)
{
    section("6. refusal order, pinned by doubly-bad inputs");

    //  Sane geometry, for the arms that are about the surface word.
    const IGS32 row = 7680u, w = 1920u, h = 1080u, bpp = 4u;
    const IGS64 ap  = 0x10000000ull;

    //  Refused AND unaligned in the low bits: NO_SURFACE wins, because every
    //  later test is arithmetic over a word that was never read.
    checkU32("refused beats unaligned",
             igScanDescribe(0xFFFFFFFFu, row, w, h, bpp, ap),
             1u);

    //  Unaligned AND zero geometry: UNALIGNED wins, because alignment is a
    //  property of the one word already in hand.
    checkU32("unaligned beats zero geometry",
             igScanDescribe(0x00100001u, 0u, 0u, 0u, 0u, ap),
             2u);

    //  Zero width AND a row far too short: ZERO_GEOMETRY wins. With width 0 the
    //  minimum row is 0 and every bytesPerRow satisfies >= 0, so a
    //  containment-first order would call this row adequate and then produce a
    //  zero extent.
    checkU32("zero geometry beats a short row",
             igScanDescribe(0x00100000u, 4u, 0u, h, bpp, ap),
             3u);

    //  A short row AND an extent that overflows: ROW_TOO_SHORT wins.
    checkU32("a short row beats extent overflow",
             igScanDescribe(0x00100000u, 0x40000000u, 0xFFFFFFFFu, 0x10u, 4u, ap),
             4u);

    //  An overflowing extent AND a base outside the aperture: EXTENT_OVERFLOW
    //  wins, and this is the reason the file exists -- a wrapped extent is a
    //  SMALL extent and fits inside any aperture, so a containment-first order
    //  makes the overflow arm unreachable for exactly the inputs that need it.
    checkU32("extent overflow beats outside aperture",
             igScanDescribe(0x00100000u, 0x10000u, 0x4000u, 0x10000u, 4u, 0x1000ull),
             5u);

    //  And the last arm on its own.
    checkU32("a good descriptor past the end of a small aperture",
             igScanDescribe(0x00100000u, row, w, h, bpp, 0x101000ull),
             6u);
}

//
//  7. Usability, and the positive control.
//
static void testUsable(void)
{
    section("7. usable is an equality against OK");

    checkInt("OK is usable", igScanUsable(0u), 1);

    //  Walked rather than spelled six times, so a seventh verdict is covered
    //  the moment it is added. The limit is INCLUSIVE for this family, so the
    //  condition is <=, not <.
    unsigned usableErrors = 0u;
    for (IGS32 v = 1u; v <= IG_SCAN_VERDICT_LIMIT; ++v) {
        if (igScanUsable(v)) ++usableErrors;
    }
    checkU32("no verdict below the limit is usable", usableErrors, 0u);

    //  A verdict past the family is not usable either -- igScanUsable() is an
    //  equality, never a negation of one error arm.
    checkInt("a verdict past the family is not usable",
             igScanUsable(7u), 0);

    section("7b. the positive control");

    //  The panel geometry measured on this silicon:
    //  igpu-results/tgl-capture-2026-08-16-022556.txt gives PIPESRC decoding to
    //  1920x1080. Stride and aperture are plausible values rather than measured
    //  ones -- PLANE_SURF is a graphics-address-space offset and this track has
    //  never mapped that space.
    checkU32("1920x1080 XRGB_8888 at a 4 KiB base is accepted",
             igScanDescribe(0x00100000u, 7680u, 1920u, 1080u, 4u, 0x10000000ull),
             0u);
    checkInt("and reports usable",
             igScanUsable(igScanDescribe(0x00100000u, 7680u, 1920u, 1080u,
                                        4u, 0x10000000ull)), 1);

    //  A stride wider than the pixels need is legal -- padding, not a defect.
    checkU32("a padded row is still accepted",
             igScanDescribe(0x00100000u, 8192u, 1920u, 1080u, 4u, 0x10000000ull),
             0u);
}

int main(void)
{
    std::printf("IntelScanout.hpp -- Gen12 scanout descriptor\n");
    std::printf("Register semantics from reference/i915/skl_universal_plane_regs.h.\n");
    std::printf("Panel geometry from igpu-results/tgl-capture-2026-08-16-022556.txt.\n");

    testSentinelPin();
    testSurfaceWord();
    testWideProducts();
    testAperture();
    testEveryArmReachable();
    testAcceptingEdges();
    testRefusalOrder();
    testUsable();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
