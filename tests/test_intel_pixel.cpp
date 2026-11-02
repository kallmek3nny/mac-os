//
//  test_intel_pixel.cpp
//  Drives phase0-kext/IntelPixel.hpp -- the Gen12 format -> IOPixelInformation
//  description -- under MSVC on a machine with no Intel GPU. Every function in
//  that header is a total function of its arguments, so there is no fixture, no
//  journal and no register file here.
//
//  TWO RULES, same as tests/test_intel_scanout.cpp and
//  tests/test_intel_framebuffer.cpp:
//
//    1. Every expected value is a BARE LITERAL. Checking a decode against the
//       same #define the decode reads cancels out and proves nothing.
//    2. Observed values carry the capture they came from.
//
//  WHAT EACH SECTION IS TRYING TO CATCH
//  ------------------------------------
//   1. The constants, each pinned to its own literal. A copy-paste arm giving
//      two formats the same mask satisfies every "has a mask" assertion at once,
//      and only a per-value literal can tell them apart.
//   2. THE REASON THE FILE EXISTS. igFbBytesPerPixel() returns 4 for BOTH
//      XRGB_8888 and XRGB_2101010 -- so a component width DERIVED from
//      bytes-per-pixel is right for one and wrong for the other, and the wrong
//      direction is the quiet one: a 2101010 surface described as 8-8-8
//      programs cleanly and yields wrong COLOURS rather than a broken picture.
//      This section pins the collision and pins that the component widths do
//      NOT collide. If a future edit derives the width from the byte count,
//      exactly these checks redden.
//   3. Describability is a WHITELIST BY NAME, never a range test. A range would
//      sweep in every future Gen12 format code as describable and hand IOKit a
//      component layout nobody chose -- the fabricated-publication class this
//      project has now met six times. The codes probed are the ones a range
//      test would wrongly admit: 3 and 5 sit BETWEEN the two describable codes.
//   4. REFUSAL ORDER. UNKNOWN_FORMAT before UNDESCRIBABLE, driven by the only
//      input that can separate them -- a code this tree does not name at all.
//      Reporting an unnamed code as merely undescribable states a fact about
//      IOKit's vocabulary when the truth is a hole in this driver's decode, and
//      only the second is something somebody can go and fix.
//   5. The three scalars refuse with a SENTINEL, never a plausible default. A
//      zero would be worse than the sentinel and a 32 worse still: a literal
//      written into a published field is byte-identical to a value that was
//      genuinely measured.
//   6. The masks, including the properties a per-value pin cannot express --
//      pairwise disjoint, contiguous, and with the X channel's bits DELIBERATELY
//      absent from the union. Publishing a fourth component mask would tell
//      IOKit the surface carries transparency information it does not carry.
//      Also: a refused format WRITES the sentinel into all three out-params
//      rather than leaving them untouched, because an untouched out-param
//      carries the caller's initialiser zero and a zero mask is
//      indistinguishable from a channel that is genuinely absent.
//   7. igPixelUsable() is an EQUALITY against OK, so every error code is
//      unusable including one added later. The loop is bounded on
//      IG_PIXEL_VERDICT_LIMIT with `<=`, because that limit is INCLUSIVE --
//      and so is EVERY verdict family in this track. tools/check-registers.py
//      enforces limit == max(codes) per family as an EQUALITY, so an exclusive
//      limit could not pass the register gate at all.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelPixel.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i) std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGP32 got, IGP32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
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
//  Local, so a mask's WIDTH can be asserted without spelling the mask again.
//  A width check and a value check catch different edits: a mask typed with one
//  digit wrong usually keeps its bit count.
//
static int popcount32(IGP32 v)
{
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

//
//  Contiguity: a channel mask must be one unbroken run of set bits. A mask with
//  a hole in it is arithmetically a mask and describes no hardware channel.
//
static int isContiguous(IGP32 v)
{
    if (v == 0u) return 0;
    while ((v & 1u) == 0u) v >>= 1;   // drop the low zeroes
    while (v & 1u)          v >>= 1;   // walk the run of ones
    return v == 0u;                    // anything left is a hole
}

//
//  1. The constants, spelled as bare literals.
//
static void testConstants(void)
{
    section("1. constants, spelled as bare literals");

    checkU32("OK is zero",              IG_PIXEL_OK,                  0u);
    checkU32("UNKNOWN_FORMAT is one",   IG_PIXEL_ERR_UNKNOWN_FORMAT,  1u);
    checkU32("UNDESCRIBABLE is two",    IG_PIXEL_ERR_UNDESCRIBABLE,   2u);

    //  The family limit is INCLUSIVE here, like every other verdict family in
    //  this track. Spelled as a literal so that a third verdict reddens this
    //  line rather than silently widening a loop.
    checkU32("IG_PIXEL_VERDICT_LIMIT is the largest verdict",
             IG_PIXEL_VERDICT_LIMIT, 2u);

    //  The refused-read value, pinned equal to IG_MMIO_INVALID's value in
    //  IntelDisplay.hpp. This header cannot include it, so the equality lives
    //  here or nowhere -- the same pin tests/test_intel_scanout.cpp carries for
    //  IG_SCAN_INVALID32.
    checkU32("IG_PIXEL_INVALID32 is the refused value",
             IG_PIXEL_INVALID32, 0xFFFFFFFFu);

    checkU32("every describable format has three components",
             IG_PIXEL_COMPONENT_COUNT, 3u);

    checkU32("XRGB_8888 is 32 bits per pixel",
             IG_PIXEL_BPP_XRGB_8888, 32u);
    checkU32("XRGB_8888 is 8 bits per component",
             IG_PIXEL_COMPONENT_BITS_8888, 8u);
    checkU32("XRGB_8888 red mask",   IG_PIXEL_MASK_R_8888, 0x00FF0000u);
    checkU32("XRGB_8888 green mask", IG_PIXEL_MASK_G_8888, 0x0000FF00u);
    checkU32("XRGB_8888 blue mask",  IG_PIXEL_MASK_B_8888, 0x000000FFu);

    checkU32("XRGB_2101010 is 32 bits per pixel",
             IG_PIXEL_BPP_XRGB_2101010, 32u);
    checkU32("XRGB_2101010 is 10 bits per component",
             IG_PIXEL_COMPONENT_BITS_2101010, 10u);
    checkU32("XRGB_2101010 red mask",   IG_PIXEL_MASK_R_2101010, 0x3FF00000u);
    checkU32("XRGB_2101010 green mask", IG_PIXEL_MASK_G_2101010, 0x000FFC00u);
    checkU32("XRGB_2101010 blue mask",  IG_PIXEL_MASK_B_2101010, 0x000003FFu);
}

//
//  2. The collision this file exists to survive.
//
static void testTheCollision(void)
{
    section("2. bytes-per-pixel collides; component width must not");

    //  IntelFramebuffer.hpp is reachable here because IntelPixel.hpp includes
    //  it -- deliberately, rather than respelling the format codes and giving
    //  this tree two answers to one question.
    checkU32("XRGB_8888 is four bytes per pixel",
             igFbBytesPerPixel(4u), 4u);
    checkU32("XRGB_2101010 is ALSO four bytes per pixel",
             igFbBytesPerPixel(2u), 4u);

    //  ...and this is the pair a derived width would break. Both are 32 bits
    //  per pixel and they are NOT interchangeable to IOKit.
    checkU32("both formats are 32 bits per pixel",
             igPixelBitsPerPixel(4u) == igPixelBitsPerPixel(2u) ? 1u : 0u, 1u);
    checkInt("and their component widths DIFFER",
             igPixelComponentBits(4u) != igPixelComponentBits(2u) ? 1 : 0, 1);

    checkU32("XRGB_8888 components are 8 bits",
             igPixelComponentBits(4u), 8u);
    checkU32("XRGB_2101010 components are 10 bits",
             igPixelComponentBits(2u), 10u);

    //  The two format codes are distinct in the first place. A header edit
    //  collapsing them would make every check above trivially true.
    checkInt("the two format codes are distinct",
             IG_FB_FORMAT_XRGB_8888 != IG_FB_FORMAT_XRGB_2101010 ? 1 : 0, 1);
}

//
//  3. Describability is a whitelist, not a range.
//
static void testWhitelist(void)
{
    section("3. describable is a whitelist by name, never a range");

    checkInt("XRGB_2101010 is describable", igPixelDescribable(2u),  1);
    checkInt("XRGB_8888 is describable",    igPixelDescribable(4u),  1);

    //  The two Gen12 formats this tree names and REFUSES on purpose. RGB_565 is
    //  real and IOKit's own 16-bit direct type is 5-5-5, so there is no standard
    //  macOS pixel description for it; XRGB_16161616F is 64-bit half-float and
    //  likewise has no pixel type this tree has ever witnessed a name for.
    checkInt("XRGB_16161616F is NOT describable", igPixelDescribable(6u),  0);
    checkInt("RGB_565 is NOT describable",        igPixelDescribable(14u), 0);

    //  3 and 5 sit BETWEEN the two describable codes, so a range test of the
    //  form 2 <= f <= 4 admits 3 and one of the form 2 <= f <= 6 admits both.
    //  These are the exact codes that separate a whitelist from a range.
    checkInt("code 3 is not describable", igPixelDescribable(3u), 0);
    checkInt("code 5 is not describable", igPixelDescribable(5u), 0);

    //  And the ends, so a range anchored anywhere else is caught too.
    checkInt("code 0 is not describable",   igPixelDescribable(0u),   0);
    checkInt("code 1 is not describable",   igPixelDescribable(1u),   0);
    checkInt("code 7 is not describable",   igPixelDescribable(7u),   0);
    checkInt("code 15 is not describable",  igPixelDescribable(15u),  0);
    checkInt("a wild code is not describable",
             igPixelDescribable(0xFFFFFFFFu), 0);
}

//
//  4. The refusal order.
//
static void testDescribeOrder(void)
{
    section("4. UNKNOWN_FORMAT is decided before UNDESCRIBABLE");

    checkU32("XRGB_8888 describes",    igPixelDescribe(4u),  0u);
    checkU32("XRGB_2101010 describes", igPixelDescribe(2u),  0u);

    //  Named and refused: a fact about IOKit's vocabulary.
    checkU32("XRGB_16161616F is UNDESCRIBABLE", igPixelDescribe(6u),  2u);
    checkU32("RGB_565 is UNDESCRIBABLE",        igPixelDescribe(14u), 2u);

    //  Unnamed: a hole in THIS driver's decode, and the only input that can
    //  separate the two arms. A describability-first order reports every one of
    //  these as UNDESCRIBABLE, which is a true-sounding sentence about the
    //  wrong subject.
    checkU32("code 3 is UNKNOWN_FORMAT",  igPixelDescribe(3u),  1u);
    checkU32("code 5 is UNKNOWN_FORMAT",  igPixelDescribe(5u),  1u);
    checkU32("code 0 is UNKNOWN_FORMAT",  igPixelDescribe(0u),  1u);
    checkU32("code 15 is UNKNOWN_FORMAT", igPixelDescribe(15u), 1u);
    checkU32("a refused PLANE_CTL read is UNKNOWN_FORMAT",
             igPixelDescribe(0xFFFFFFFFu), 1u);
}

//
//  5. The three scalars, and their sentinel.
//
static void testScalars(void)
{
    section("5. scalars refuse with a sentinel, never a plausible default");

    checkU32("XRGB_8888 bits per pixel",     igPixelBitsPerPixel(4u),  32u);
    checkU32("XRGB_2101010 bits per pixel",  igPixelBitsPerPixel(2u),  32u);
    checkU32("XRGB_8888 component count",    igPixelComponentCount(4u), 3u);
    checkU32("XRGB_2101010 component count", igPixelComponentCount(2u), 3u);

    //  A named-but-undescribable format gets the sentinel from all three, not a
    //  zero and not a 32. Both of those are values a real answer could take.
    checkU32("RGB_565 bits per pixel is the sentinel",
             igPixelBitsPerPixel(14u), 0xFFFFFFFFu);
    checkU32("RGB_565 component bits is the sentinel",
             igPixelComponentBits(14u), 0xFFFFFFFFu);
    checkU32("RGB_565 component count is the sentinel",
             igPixelComponentCount(14u), 0xFFFFFFFFu);

    checkU32("XRGB_16161616F bits per pixel is the sentinel",
             igPixelBitsPerPixel(6u), 0xFFFFFFFFu);
    checkU32("XRGB_16161616F component bits is the sentinel",
             igPixelComponentBits(6u), 0xFFFFFFFFu);
    checkU32("XRGB_16161616F component count is the sentinel",
             igPixelComponentCount(6u), 0xFFFFFFFFu);

    //  An unnamed code likewise. Note 16161616F is genuinely 64 bits per pixel
    //  on the hardware -- returning 64 here would be a MEASUREMENT of a surface
    //  this driver has just declined to describe, which is the fabricated
    //  publication shape one step removed.
    checkU32("an unnamed code's bits per pixel is the sentinel",
             igPixelBitsPerPixel(3u), 0xFFFFFFFFu);
    checkU32("an unnamed code's component bits is the sentinel",
             igPixelComponentBits(3u), 0xFFFFFFFFu);
    checkU32("an unnamed code's component count is the sentinel",
             igPixelComponentCount(3u), 0xFFFFFFFFu);

    //  The sentinel is not a legal answer for any of the three, which is what
    //  makes it usable as a refusal at all.
    checkInt("no real bits-per-pixel equals the sentinel",
             igPixelBitsPerPixel(4u) == 0xFFFFFFFFu ? 1 : 0, 0);
    checkInt("no real component count equals the sentinel",
             igPixelComponentCount(2u) == 0xFFFFFFFFu ? 1 : 0, 0);
}

//
//  6. The masks.
//
static void testMasks(void)
{
    section("6. channel masks: value, width, contiguity, and the absent X");

    IGP32 r = 0u, g = 0u, b = 0u;

    checkU32("XRGB_8888 masks are OK", igPixelMasks(4u, &r, &g, &b), 0u);
    checkU32("XRGB_8888 red",   r, 0x00FF0000u);
    checkU32("XRGB_8888 green", g, 0x0000FF00u);
    checkU32("XRGB_8888 blue",  b, 0x000000FFu);
    checkInt("XRGB_8888 red is 8 bits wide",   popcount32(r), 8);
    checkInt("XRGB_8888 green is 8 bits wide", popcount32(g), 8);
    checkInt("XRGB_8888 blue is 8 bits wide",  popcount32(b), 8);
    checkInt("XRGB_8888 red is contiguous",    isContiguous(r), 1);
    checkInt("XRGB_8888 green is contiguous",  isContiguous(g), 1);
    checkInt("XRGB_8888 blue is contiguous",   isContiguous(b), 1);
    checkU32("XRGB_8888 red and green do not overlap",   r & g, 0u);
    checkU32("XRGB_8888 green and blue do not overlap",  g & b, 0u);
    checkU32("XRGB_8888 red and blue do not overlap",    r & b, 0u);
    checkU32("XRGB_8888 union is the low three bytes",   r | g | b, 0x00FFFFFFu);
    //  The X channel is NOT alpha. Its eight bits must be absent from the union.
    checkU32("XRGB_8888 publishes no fourth channel",
             (r | g | b) & 0xFF000000u, 0u);

    r = 0u; g = 0u; b = 0u;
    checkU32("XRGB_2101010 masks are OK", igPixelMasks(2u, &r, &g, &b), 0u);
    checkU32("XRGB_2101010 red",   r, 0x3FF00000u);
    checkU32("XRGB_2101010 green", g, 0x000FFC00u);
    checkU32("XRGB_2101010 blue",  b, 0x000003FFu);
    checkInt("XRGB_2101010 red is 10 bits wide",   popcount32(r), 10);
    checkInt("XRGB_2101010 green is 10 bits wide", popcount32(g), 10);
    checkInt("XRGB_2101010 blue is 10 bits wide",  popcount32(b), 10);
    checkInt("XRGB_2101010 red is contiguous",     isContiguous(r), 1);
    checkInt("XRGB_2101010 green is contiguous",   isContiguous(g), 1);
    checkInt("XRGB_2101010 blue is contiguous",    isContiguous(b), 1);
    checkU32("XRGB_2101010 red and green do not overlap",  r & g, 0u);
    checkU32("XRGB_2101010 green and blue do not overlap", g & b, 0u);
    checkU32("XRGB_2101010 red and blue do not overlap",   r & b, 0u);
    checkU32("XRGB_2101010 union is the low thirty bits",
             r | g | b, 0x3FFFFFFFu);
    //  Two X bits at the top, deliberately unpublished.
    checkU32("XRGB_2101010 publishes no fourth channel",
             (r | g | b) & 0xC0000000u, 0u);

    //  The two layouts are genuinely different, so a copy-paste arm returning
    //  the sibling's masks reddens here even if every per-value pin above were
    //  loosened.
    checkInt("the two red masks differ",
             IG_PIXEL_MASK_R_8888 != IG_PIXEL_MASK_R_2101010 ? 1 : 0, 1);

    section("6b. a refused format WRITES the sentinel, never leaves it");

    //  Pre-load the out-params with a value that is neither the sentinel nor a
    //  legal mask. If the function returned without touching them these would
    //  survive, and a caller publishing them would be publishing its own
    //  initialiser -- which is the zero case one step louder.
    r = 0x11111111u; g = 0x22222222u; b = 0x33333333u;
    checkU32("RGB_565 masks refuse as UNDESCRIBABLE",
             igPixelMasks(14u, &r, &g, &b), 2u);
    checkU32("RGB_565 red is the sentinel",   r, 0xFFFFFFFFu);
    checkU32("RGB_565 green is the sentinel", g, 0xFFFFFFFFu);
    checkU32("RGB_565 blue is the sentinel",  b, 0xFFFFFFFFu);

    //  Zero is the dangerous initialiser and gets its own case: a zero mask is
    //  indistinguishable from a channel that is genuinely absent.
    r = 0u; g = 0u; b = 0u;
    checkU32("an unnamed code refuses as UNKNOWN_FORMAT",
             igPixelMasks(3u, &r, &g, &b), 1u);
    checkU32("an unnamed code's red is the sentinel, not zero",   r, 0xFFFFFFFFu);
    checkU32("an unnamed code's green is the sentinel, not zero", g, 0xFFFFFFFFu);
    checkU32("an unnamed code's blue is the sentinel, not zero",  b, 0xFFFFFFFFu);

    r = 0u; g = 0u; b = 0u;
    checkU32("XRGB_16161616F refuses as UNDESCRIBABLE",
             igPixelMasks(6u, &r, &g, &b), 2u);
    checkU32("XRGB_16161616F red is the sentinel",   r, 0xFFFFFFFFu);
    checkU32("XRGB_16161616F green is the sentinel", g, 0xFFFFFFFFu);
    checkU32("XRGB_16161616F blue is the sentinel",  b, 0xFFFFFFFFu);

    section("6c. a null out-param refuses rather than dereferences");

    //  Each pointer tested alone, because a guard written as a single test of
    //  one pointer passes every check that only ever nulls that one.
    checkU32("a null red pointer refuses",
             igPixelMasks(4u, 0, &g, &b), 1u);
    checkU32("a null green pointer refuses",
             igPixelMasks(4u, &r, 0, &b), 1u);
    checkU32("a null blue pointer refuses",
             igPixelMasks(4u, &r, &g, 0), 1u);
    checkU32("all three null refuses",
             igPixelMasks(4u, 0, 0, 0), 1u);

    //  And the null check runs BEFORE the format check, so a describable format
    //  with a null pointer still refuses rather than storing through it.
    checkU32("a describable format with a null pointer still refuses",
             igPixelMasks(2u, 0, &g, &b), 1u);
}

//
//  7. Usability, and the positive control.
//
static void testUsable(void)
{
    section("7. usable is an equality against OK");

    checkInt("OK is usable", igPixelUsable(0u), 1);

    //  Walked rather than spelled twice, so a third verdict is covered the
    //  moment it is added. The limit is INCLUSIVE for this family, so the
    //  condition is <=, not <.
    unsigned usableErrors = 0u;
    for (IGP32 v = 1u; v <= IG_PIXEL_VERDICT_LIMIT; ++v) {
        if (igPixelUsable(v)) ++usableErrors;
    }
    checkU32("no verdict below the limit is usable", usableErrors, 0u);

    //  A verdict past the family is not usable either -- igPixelUsable() is an
    //  equality, never a negation of one error arm. A `!= UNDESCRIBABLE` form
    //  would report this as usable.
    checkInt("a verdict past the family is not usable", igPixelUsable(3u),  0);
    checkInt("a wild verdict is not usable",
             igPixelUsable(0xFFFFFFFFu), 0);

    section("7b. the positive control");

    //  The format the firmware programmed on this silicon. The PLANE_CTL word
    //  is MEASURED, not chosen: igpu-results/tgl-capture-2026-08-16-022556.txt
    //  line 134 reads it as 0x82009000. An earlier revision of this line
    //  invented a plausible-looking 0x84000000 instead and the control went
    //  red. A fabricated register word beside measured ones is the exact thing
    //  CLAUDE.md forbids.
    //
    //  CORRECTED 2026-11-01 (t-a7929c). This control used to say bits 27:23
    //  decode to format 4, XRGB8888, and pinned 8-bit components and masks.
    //  The 5-bit field IS 4, but the codes are 4-bit and sit at bit 24
    //  (reference/i915/skl_universal_plane_regs.h:43-60), so field 4 is code 2,
    //  XRGB_2101010, and XRGB_8888 is field 8. That is 0x84000000, the invented
    //  word, which went red only because the old decode read it as unnamed 8.
    //  Both formats are 32 bpp, so the old reading gave the right geometry and
    //  the wrong colours.
    //
    //  With bit 23 clear, the 4-bit SKL reading (0x82009000 >> 24) & 0x0F now
    //  AGREES with the decode, so it separates nothing here. The reading that
    //  must not be mistaken for the code is the raw 5-bit field.
    IGP32 fmt = igFbFormat(0x82009000u);
    checkU32("the captured PLANE_CTL decodes to XRGB_2101010", fmt, 2u);

    //  NEGATIVE CONTROL: the raw field is 4, the XRGB_8888 code. A decoder that
    //  returns the raw field reddens here rather than silently describing this
    //  10-bit surface as 8-bit.
    checkInt("the raw 5-bit field of the same word differs",
             ((0x82009000u >> 23) & 0x1Fu) != fmt ? 1 : 0, 1);

    checkU32("and that format describes",       igPixelDescribe(fmt),       0u);
    checkInt("and reports usable",              igPixelUsable(igPixelDescribe(fmt)), 1);
    checkU32("and publishes 32 bits per pixel", igPixelBitsPerPixel(fmt),  32u);
    checkU32("and 10 bits per component",       igPixelComponentBits(fmt), 10u);
    checkU32("and three components",            igPixelComponentCount(fmt), 3u);

    IGP32 r = 0u, g = 0u, b = 0u;
    checkU32("and three real channel masks", igPixelMasks(fmt, &r, &g, &b), 0u);
    checkU32("captured surface red",   r, 0x3FF00000u);
    checkU32("captured surface green", g, 0x000FFC00u);
    checkU32("captured surface blue",  b, 0x000003FFu);
}

int main(void)
{
    std::printf("IntelPixel.hpp -- Gen12 format to IOPixelInformation\n");
    std::printf("Format codes from reference/i915/skl_universal_plane_regs.h.\n");
    std::printf("Panel format from igpu-results/tgl-capture-2026-08-16-022556.txt.\n");

    testConstants();
    testTheCollision();
    testWhitelist();
    testDescribeOrder();
    testScalars();
    testMasks();
    testUsable();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
