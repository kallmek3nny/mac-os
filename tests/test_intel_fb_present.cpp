//
//  test_intel_fb_present.cpp
//  Drives phase0-kext/IntelFbPresent.hpp -- the join that decides whether an
//  IOFramebuffer subclass may describe an inherited Gen12 plane -- under MSVC on a
//  machine with no Intel GPU. Every function in that header is a total function of
//  its arguments, so there is no fixture, no journal and no register file here.
//
//  TWO RULES, same as tests/test_intel_pixel.cpp and tests/test_intel_scanout.cpp:
//
//    1. Every expected value is a BARE LITERAL. Checking a decode against the
//       same #define the decode uses is a tautology -- the fixture and the
//       subject cancel, and a wrong constant passes.
//
//    2. Every ordering claim in the header's prose is pinned by an input that is
//       bad in TWO ways at once. An input bad in only one way cannot distinguish
//       one order from any other.
//
//  WHAT THIS SUITE IS FOR, BEYOND THE ARITHMETIC
//  --------------------------------------------
//  The header's whole reason to exist is that its accessors CANNOT be made to
//  answer for a group the verdict refused -- the five-times-repeated defect in
//  this tree is a caller that had a refusal available and did not consult it. So
//  section 6 walks every accessor against every refusing input class and requires
//  the sentinel from all of them. That is the section a "simplification" that
//  drops the re-derivation would redden.
//

#include <cstdio>
#include <cstring>

#include "IntelFbPresent.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i) std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGPR32 got, IGPR32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkU64(const char *label, IGPR64 got, IGPR64 want)
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
//  ONE LINEAR GROUP, spelled as bare literals and used by most sections below.
//
//  THE CAPTURED WORD CANNOT BE THE ACCEPT-PATH INPUT, AND THAT IS A FACT ABOUT
//  THE SILICON RATHER THAN A CONVENIENCE.
//
//  The 2026-08-16 Tiger Lake capture returned PLANE_CTL = 0x82009000, and bit 15
//  of that word is SET. Bit 15 is PLANE_CTL_RENDER_DECOMP, so igFbInheritable()
//  returns COMPRESSED at its second arm and never reaches format, stride or
//  geometry -- tests/test_intel_framebuffer.cpp says so in its own header, and
//  this suite's first draft ignored it and composed an acceptable group out of
//  the captured word. Every accept-path check then reported verdict 2.
//
//  The captured word is also Y-tiled (tiling field 12:10 = 4). The accept fixture
//  is therefore COMPOSED by clearing the render-compression bit and the Y-tiling
//  bit:
//
//      0x82009000 & ~0x00009000 = 0x82000000
//
//  Enabled (bit 31), tiling field (bits 12:10) = 0 = linear, format field
//  (bits 27:23) = 4, which is code 2 = XRGB_2101010; compression cleared.
//
//  The format codes are 4-bit at bit 24 (reference/i915/
//  skl_universal_plane_regs.h:43-60), so 5-bit field 4 is code 2, and
//  XRGB_8888 is field 8. The captured word remains separately pinned below.
//
//  The geometry is 1280x800 rather than the captured 1920x1080 on purpose, and it
//  must not be "fixed": handoff16 SS4 records why. A test raster equal to the
//  captured one cannot distinguish a decoder that reads the register from one that
//  returns the captured constant.
//
//  Both halves store value - 1, which is the convention the capture settled.
//  1279 = 0x4FF, 799 = 0x31F, and PLANE_SIZE packs height in the high half.
//
//  THE STRIDE UNIT IS 128 BYTES, NOT 512. IntelFramebuffer.hpp:219 derives that
//  from the capture (field 60 against a 1920x4 row), and this suite's first draft
//  wrote 16 with a comment claiming 8192 bytes -- it is 2048, which is SHORT of a
//  1280 * 4 = 5120-byte row, so igFbInheritable() refused at STRIDE as well. A
//  comment carrying arithmetic nobody executes is exactly the class this tree
//  keeps finding; the field value below is chosen so the product is stated and
//  then CHECKED in section 4 rather than asserted here.
//
//      64 * 128 = 8192 bytes, and 8192 >= 1280 * 4 = 5120, with padding left over
//      so stride-vs-visible-width is a real distinction rather than an equality.
//
#define GOOD_CTL        0x82000000u   // observed word with compression and Y tiling cleared
#define GOOD_SIZE       0x031F04FFu   // height 799+1=800, width 1279+1=1280
#define GOOD_STRIDE     0x00000040u   // 64 * 128 = 8192 bytes, >= 1280 * 4
#define GOOD_SURF       0x00100000u   // 1 MiB in, 4 KiB aligned
#define GOOD_APERTURE   0x10000000ull // 256 MiB, comfortably holds the extent

//
//  The observed word itself, kept as its own literal and asserted in section 4b
//  to differ from the linear fixture only by its compression and Y-tiling bits.
//
#define OBSERVED_CTL    0x82009000u

//
//  1. The constants, spelled as bare literals.
//
static void testConstants(void)
{
    section("1. constants, spelled as bare literals");

    checkU32("OK is zero",                  IG_PRESENT_OK,                  0u);
    checkU32("SAMPLE_REFUSED is one",       IG_PRESENT_ERR_SAMPLE_REFUSED,  1u);
    checkU32("NOT_INHERITABLE is two",      IG_PRESENT_ERR_NOT_INHERITABLE, 2u);
    checkU32("SCANOUT is three",            IG_PRESENT_ERR_SCANOUT,         3u);
    checkU32("PIXEL is four",               IG_PRESENT_ERR_PIXEL,           4u);

    //  The family limit is INCLUSIVE, like every other verdict family in this
    //  track. Spelled as a literal so a fifth verdict reddens this line rather
    //  than silently widening the loops in section 2.
    checkU32("IG_PRESENT_VERDICT_LIMIT is the largest verdict",
             IG_PRESENT_VERDICT_LIMIT, 4u);

    checkU32("the 32-bit sentinel is all ones", IG_PRESENT_INVALID32, 0xFFFFFFFFu);
    checkU64("the 64-bit sentinel is all ones",
             IG_PRESENT_INVALID64, 0xFFFFFFFFFFFFFFFFull);

    checkU32("the plane group is four words", IG_PRESENT_PLANE_WORD_COUNT, 4u);
}

//
//  2. The family is contiguous, bounded by its own limit, and every member is
//     distinct.
//
//  This header is NOT one of the two Intel headers tools/check-registers.py
//  parses, so this family gets no set-check from the register gate at all. These
//  loops are the only thing holding the relation.
//
static void testFamilyShape(void)
{
    section("2. the verdict family is contiguous and bounded by its own limit");

    const IGPR32 codes[] = {
        IG_PRESENT_OK,
        IG_PRESENT_ERR_SAMPLE_REFUSED,
        IG_PRESENT_ERR_NOT_INHERITABLE,
        IG_PRESENT_ERR_SCANOUT,
        IG_PRESENT_ERR_PIXEL,
    };
    const int n = (int)(sizeof(codes) / sizeof(codes[0]));

    //  Contiguous from zero: codes[i] == i. A family with a hole in it means an
    //  exhaustiveness loop bounded on the limit visits a value no arm produces.
    int contiguous = 1;
    for (int i = 0; i < n; ++i)
        if (codes[i] != (IGPR32)i) contiguous = 0;
    checkInt("the five verdicts are contiguous from zero", contiguous, 1);

    //  Pairwise distinct, because a copy-paste arm sharing a sibling's value
    //  satisfies every "has a verdict" assertion there is.
    int distinct = 1;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (codes[i] == codes[j]) distinct = 0;
    checkInt("the five verdicts are pairwise distinct", distinct, 1);

    //  The limit EQUALS the largest member, which is this track's measured
    //  convention rather than a remembered one.
    IGPR32 largest = codes[0];
    for (int i = 1; i < n; ++i)
        if (codes[i] > largest) largest = codes[i];
    checkU32("the limit equals the largest member", IG_PRESENT_VERDICT_LIMIT, largest);

    //  The count and the limit agree, so a verdict DELETED from the header
    //  reddens here rather than leaving a loop bounded above its own family.
    checkInt("the family holds limit+1 members", n, (int)IG_PRESENT_VERDICT_LIMIT + 1);
}

//
//  3. The three sibling sentinels hold the same bit pattern.
//
//  Pinned here rather than by one header importing another's meaning. They are
//  three DIFFERENT claims -- refused read, no scanout answer, no pixel answer --
//  that happen to coincide, and IntelPixel.hpp and IntelScanout.hpp each pin
//  their own against IG_MMIO_INVALID for exactly this reason.
//
static void testSentinelsAgree(void)
{
    section("3. the sibling sentinels coincide, and are pinned rather than shared");

    checkU32("IG_MMIO_INVALID is all ones",    IG_MMIO_INVALID,      0xFFFFFFFFu);
    checkU32("IG_SCAN_INVALID32 is all ones",  IG_SCAN_INVALID32,    0xFFFFFFFFu);
    checkU32("IG_PIXEL_INVALID32 is all ones", IG_PIXEL_INVALID32,   0xFFFFFFFFu);
    checkU32("this header agrees with all three", IG_PRESENT_INVALID32, 0xFFFFFFFFu);

    //  The polarity decision that is NOT shared, and must not become shared:
    //  a zero word is not a refused read. Every unprogrammed display page on this
    //  silicon read 0x00000000 in the 2026-08-16 capture, so all-ones is what a
    //  block that is not decoding reports and zero is what one that is unused
    //  reports.
    checkInt("a zero word is NOT a refused read", igReadRefused(0u),          0);
    checkInt("an all-ones word IS a refused read", igReadRefused(0xFFFFFFFFu), 1);
}

//
//  4. The synthetic linear accept path, based on a PLANE_CTL word witnessed on silicon.
//
static void testAcceptPath(void)
{
    section("4. the synthetic linear accept path, based on a captured PLANE_CTL word");

    //  CORRECTED 2026-11-01 (t-a7929c). GOOD_CTL's 5-bit field (27:23) is 4, but
    //  the codes are 4-bit and sit at bit 24 (reference/i915/
    //  skl_universal_plane_regs.h:43-60), so field 4 is code 2, XRGB_2101010.
    //  This section used to read it as XRGB_8888 (code 4, field 8) and pinned
    //  8-bit components and masks over a 10-bit surface. The accept path is now
    //  the format the firmware really programmed; section 8 composes the 8-bit
    //  side of the pair.
    checkU32("the captured word decodes to XRGB_2101010", igFbFormat(GOOD_CTL), 2u);
    checkInt("and NOT to XRGB_8888, the raw field's coincidence",
             igFbFormat(GOOD_CTL) != 4u ? 1 : 0, 1);

    checkU32("the good group is usable",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 0u);
    checkInt("igPresentUsable agrees",
             igPresentUsable(igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE,
                                             GOOD_SURF, GOOD_APERTURE)), 1);

    //  The five scalars, as bare literals. 1280 = 0x500, 800 = 0x320.
    checkU32("width is 1280",
             igPresentWidth(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE), 1280u);
    checkU32("height is 800",
             igPresentHeight(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                             GOOD_APERTURE), 800u);
    checkU32("bytes per row is 8192",
             igPresentBytesPerRow(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                  GOOD_APERTURE), 8192u);
    checkU32("bits per pixel is 32",
             igPresentBitsPerPixel(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                   GOOD_APERTURE), 32u);
    checkU32("component bits is 10",
             igPresentComponentBits(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                    GOOD_APERTURE), 10u);
    checkU32("component count is 3",
             igPresentComponentCount(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                     GOOD_APERTURE), 3u);

    //  The base is the MASKED surface word: the low twelve bits of PLANE_SURF are
    //  flags rather than address, so a naive read yields a base a few bytes off
    //  and a picture that tears rather than one obviously broken.
    checkU32("base offset is the 4 KiB-aligned surface address",
             igPresentBaseOffset(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                 GOOD_APERTURE), 0x00100000u);

    //  8192 * 800 = 6553600 = 0x640000. Bare literal, not the product spelled as
    //  an expression the header could also compute wrongly.
    checkU64("extent is 6553600 bytes",
             igPresentExtentBytes(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                  GOOD_APERTURE), 6553600ull);

    IGPR32 r = 0u, g = 0u, b = 0u;
    checkU32("masks answer OK",
             igPresentMasks(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, &r, &g, &b), 0u);
    checkU32("red mask is 10-bit XRGB",   r, 0x3FF00000u);
    checkU32("green mask is 10-bit XRGB", g, 0x000FFC00u);
    checkU32("blue mask is 10-bit XRGB",  b, 0x000003FFu);

    //
    //  4b. THE ACCEPT FIXTURE CLEARS THE OBSERVED COMPRESSION AND Y-TILING BITS.
    //
    //  Everything above runs over GOOD_CTL, which is COMPOSED from the observed
    //  word by clearing bit 15 (render compression) and bit 12 (Y tiling). The
    //  capture itself is not a valid linear accept-path input.
    //
    //  Pin the relationship and each relevant field separately:
    //
    //    - the two words differ in exactly bits 15 and 12
    //    - the observed word REFUSES, and specifically as NOT_INHERITABLE
    //    - the observed word is otherwise identical, i.e. it still decodes to the
    //      same format, so the clearing did not quietly change the subject
    //
    //  Without the field checks, clearing arbitrary bits could accidentally make
    //  a fixture look acceptable without representing a linear surface.
    //
    checkU32("observed word is the linear fixture plus compression and Y tiling",
             OBSERVED_CTL, GOOD_CTL | 0x00009000u);
    checkU32("the words differ in render compression and Y tiling bits",
             OBSERVED_CTL ^ GOOD_CTL, 0x00009000u);
    checkU32("the linear fixture has linear tiling",
             igFbTiling(GOOD_CTL), IG_FB_TILING_LINEAR);
    checkU32("the observed word has Y tiling",
             igFbTiling(OBSERVED_CTL), IG_FB_TILING_Y);
    checkU32("the observed word still decodes to XRGB_2101010",
             igFbFormat(OBSERVED_CTL), 2u);
    checkU32("the observed word is refused as NOT_INHERITABLE",
             igPresentVerdict(OBSERVED_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 2u);
    checkU32("and igFbInheritable names compression as the reason",
             igFbInheritable(OBSERVED_CTL, GOOD_STRIDE, GOOD_SIZE), 2u);

    //
    //  The stride arithmetic, checked rather than asserted in a comment. 64 * 128
    //  = 8192, and the first draft's comment said 16 * 512 = 8192 while the unit
    //  is 128, so that field value was 2048 bytes -- short of a 1280 * 4 row, and
    //  igFbInheritable() refused at STRIDE. Both halves of the relation are pinned:
    //  the byte count, and that it genuinely EXCEEDS the visible row rather than
    //  equalling it, so a decoder returning the visible width would redden.
    //
    checkU32("the stride field decodes to 8192 bytes",
             igFbStrideBytes(GOOD_STRIDE), 8192u);
    checkInt("and 8192 exceeds the 5120-byte visible row",
             igFbStrideBytes(GOOD_STRIDE) > 1280u * 4u, 1);
}

//
//  5. Each refusal arm, one at a time.
//
//  Every input below is bad in exactly ONE way, which is what makes it a test of
//  the ARM rather than of the order. Section 6 does the order.
//
static void testEachRefusalArm(void)
{
    section("5. each refusal arm, over an input bad in exactly one way");

    //  A refused read in each of the four slots. All four are checked rather than
    //  one, because a disjunction written for the first word passes every
    //  single-word test while ignoring the other three -- handoff24 SS2's reason
    //  for walking the refusal across every index of the group.
    checkU32("a refused PLANE_CTL is SAMPLE_REFUSED",
             igPresentVerdict(0xFFFFFFFFu, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 1u);
    checkU32("a refused PLANE_STRIDE is SAMPLE_REFUSED",
             igPresentVerdict(GOOD_CTL, 0xFFFFFFFFu, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 1u);
    checkU32("a refused PLANE_SIZE is SAMPLE_REFUSED",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, 0xFFFFFFFFu, GOOD_SURF,
                              GOOD_APERTURE), 1u);
    checkU32("a refused PLANE_SURF is SAMPLE_REFUSED",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, 0xFFFFFFFFu,
                              GOOD_APERTURE), 1u);

    //  A disabled plane. Clearing bit 31 of an otherwise good CTL word leaves
    //  every other field intact, so this is a test of the enable bit rather than
    //  of a malformed word.
    checkU32("a disabled plane is NOT_INHERITABLE",
             igPresentVerdict(GOOD_CTL & 0x7FFFFFFFu, GOOD_STRIDE, GOOD_SIZE,
                              GOOD_SURF, GOOD_APERTURE), 2u);

    //  A zero aperture. The plane is perfectly good; the caller has mapped
    //  nothing, which is the case igScanWithinAperture() refuses on purpose.
    checkU32("a zero aperture is SCANOUT",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF, 0ull),
             3u);

    //  An unaligned surface base. Low bits set in PLANE_SURF that are not part of
    //  the address field.
    checkU32("an unaligned surface is SCANOUT",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE,
                              GOOD_SURF | 0x00000008u, GOOD_APERTURE), 3u);

    //  An aperture that cannot hold base + extent. 0x100000 + 0x640000 =
    //  0x740000, so an aperture of 0x700000 is short by a real amount rather than
    //  by one byte.
    checkU32("an aperture shorter than base+extent is SCANOUT",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              0x700000ull), 3u);

    //  A format that is legible and undescribable. RGB_565 is format 14, so the
    //  CTL word carries 14 in the format field with every other bit unchanged.
    //  This is the arm that proves the pixel test runs at all: the plane is
    //  enabled, linear, uncompressed, and its scanout arithmetic is fine.
    const IGPR32 ctl565 = (GOOD_CTL & ~(0x1Fu << 23)) | (14u << 24);
    checkU32("the composed word really decodes to RGB_565",
             igFbFormat(ctl565), 14u);
    checkU32("an undescribable format is PIXEL",
             igPresentVerdict(ctl565, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 4u);
}

//
//  6. The order, pinned by inputs bad in TWO ways at once.
//
//  An input bad in only one way cannot distinguish one order from any other, so
//  each check below is a pair and the expected value names which half wins.
//
static void testRefusalOrder(void)
{
    section("6. the refusal order, pinned by inputs bad in two ways at once");

    //  SAMPLE_REFUSED beats NOT_INHERITABLE, and this is the load-bearing one:
    //  0xFFFFFFFF has bit 31 set, so a refused CTL word looks ENABLED. An
    //  inheritable-first order would decode a format out of a word nobody read.
    //  The all-ones CTL is ALSO not inheritable (its format field is 0x1F, which
    //  this tree does not name), so this input is genuinely bad both ways.
    checkU32("a refused CTL reports the read, not the layout",
             igPresentVerdict(0xFFFFFFFFu, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 1u);

    //  SAMPLE_REFUSED beats SCANOUT: a refused SURF word masks to 0xFFFFF000,
    //  which IS 4 KiB aligned, so an alignment-first order reports a refused read
    //  as a well-aligned plane. Paired with a zero aperture so the scanout arm
    //  would certainly fire if it ran first.
    checkU32("a refused SURF with no aperture reports the read",
             igPresentVerdict(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, 0xFFFFFFFFu,
                              0ull), 1u);

    //  NOT_INHERITABLE beats SCANOUT. A disabled plane whose aperture is also
    //  zero: the layout claim is about the plane the firmware programmed, and
    //  every scanout number is a property OF an inheritable plane.
    checkU32("a disabled plane with no aperture reports the layout",
             igPresentVerdict(GOOD_CTL & 0x7FFFFFFFu, GOOD_STRIDE, GOOD_SIZE,
                              GOOD_SURF, 0ull), 2u);

    //  SCANOUT beats PIXEL, and this is the ordering choice the header argues
    //  about. RGB_565 with a zero aperture: the pixel refusal is a permanent fact
    //  about IOKit's vocabulary, the scanout refusal is a mapping somebody can go
    //  and make bigger. The actionable half wins.
    const IGPR32 ctl565 = (GOOD_CTL & ~(0x1Fu << 23)) | (14u << 24);
    checkU32("an undescribable format with no aperture reports the scanout",
             igPresentVerdict(ctl565, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF, 0ull),
             3u);

    //  And the same pair the other way round proves the previous check is about
    //  ORDER rather than about the pixel arm being unreachable: give that same
    //  RGB_565 plane a good aperture and the pixel arm fires.
    checkU32("the same format with a good aperture reports the pixel",
             igPresentVerdict(ctl565, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 4u);
}

//
//  7. NO accessor answers for a group the verdict refused.
//
//  This is the section the header exists for. Every occurrence of the
//  fabricated-publication class in this tree so far has been a caller that had
//  the refusal available and did not consult it -- five times, in two tracks, in
//  code whose own comments explained why it mattered. The accessors re-derive the
//  verdict so that the possibility is removed rather than documented, and a
//  "simplification" dropping the re-derivation reddens here.
//
//  Every accessor is walked against every refusing class, not one accessor
//  against one class: a guard added to the first accessor and forgotten on the
//  last passes any thinner version of this section.
//
static void testAccessorsRefuse(void)
{
    section("7. no accessor answers for a refused group");

    const IGPR32 ctl565 = (GOOD_CTL & ~(0x1Fu << 23)) | (14u << 24);

    struct BadGroup {
        const char *label;
        IGPR32      ctl;
        IGPR32      stride;
        IGPR32      size;
        IGPR32      surf;
        IGPR64      aperture;
    };

    const BadGroup bad[] = {
        { "refused CTL",     0xFFFFFFFFu, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,   GOOD_APERTURE },
        { "refused STRIDE",  GOOD_CTL,    0xFFFFFFFFu, GOOD_SIZE, GOOD_SURF,   GOOD_APERTURE },
        { "refused SIZE",    GOOD_CTL,    GOOD_STRIDE, 0xFFFFFFFFu, GOOD_SURF, GOOD_APERTURE },
        { "refused SURF",    GOOD_CTL,    GOOD_STRIDE, GOOD_SIZE, 0xFFFFFFFFu, GOOD_APERTURE },
        { "disabled plane",  GOOD_CTL & 0x7FFFFFFFu, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF, GOOD_APERTURE },
        { "zero aperture",   GOOD_CTL,    GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,   0ull },
        { "short aperture",  GOOD_CTL,    GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,   0x700000ull },
        { "undescribable",   ctl565,      GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,   GOOD_APERTURE },
    };
    const int n = (int)(sizeof(bad) / sizeof(bad[0]));

    //  The eight groups above must actually be refused, or every assertion below
    //  them is vacuous -- a group that turns out to be OK would make the sentinel
    //  checks fail loudly rather than pass silently, but the count of REFUSED
    //  groups is what this section's reach depends on.
    int refusedCount = 0;
    for (int i = 0; i < n; ++i)
        if (!igPresentUsable(igPresentVerdict(bad[i].ctl, bad[i].stride,
                                             bad[i].size, bad[i].surf,
                                             bad[i].aperture)))
            ++refusedCount;
    checkInt("all eight groups are refused", refusedCount, 8);

    //  Six 32-bit accessors, walked over all eight groups. A single failure names
    //  which accessor and which group, because a per-accessor tally cannot say
    //  which input leaked.
    for (int i = 0; i < n; ++i) {
        char label[96];

        std::snprintf(label, sizeof(label), "width refuses: %s", bad[i].label);
        checkU32(label, igPresentWidth(bad[i].ctl, bad[i].stride, bad[i].size,
                                      bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "height refuses: %s", bad[i].label);
        checkU32(label, igPresentHeight(bad[i].ctl, bad[i].stride, bad[i].size,
                                       bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "bytesPerRow refuses: %s", bad[i].label);
        checkU32(label, igPresentBytesPerRow(bad[i].ctl, bad[i].stride, bad[i].size,
                                            bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "bitsPerPixel refuses: %s", bad[i].label);
        checkU32(label, igPresentBitsPerPixel(bad[i].ctl, bad[i].stride, bad[i].size,
                                             bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "componentBits refuses: %s", bad[i].label);
        checkU32(label, igPresentComponentBits(bad[i].ctl, bad[i].stride, bad[i].size,
                                              bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "componentCount refuses: %s", bad[i].label);
        checkU32(label, igPresentComponentCount(bad[i].ctl, bad[i].stride, bad[i].size,
                                               bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "baseOffset refuses: %s", bad[i].label);
        checkU32(label, igPresentBaseOffset(bad[i].ctl, bad[i].stride, bad[i].size,
                                           bad[i].surf, bad[i].aperture), 0xFFFFFFFFu);

        std::snprintf(label, sizeof(label), "extent refuses: %s", bad[i].label);
        checkU64(label, igPresentExtentBytes(bad[i].ctl, bad[i].stride, bad[i].size,
                                            bad[i].surf, bad[i].aperture),
                 0xFFFFFFFFFFFFFFFFull);
    }
}

//
//  8. The mask out-params, including the two ways they can lie.
//
static void testMasks(void)
{
    section("8. the mask out-params write the sentinel rather than leaving values");

    const IGPR32 ctl565 = (GOOD_CTL & ~(0x1Fu << 23)) | (14u << 24);

    //  A refused group must OVERWRITE all three, never leave them. The caller's
    //  values here are the initialiser zeros 3050-handoff29 SS2 measured, and a
    //  zero mask is indistinguishable from a channel that is genuinely absent.
    IGPR32 r = 0u, g = 0u, b = 0u;
    checkU32("a disabled plane refuses the masks",
             igPresentMasks(GOOD_CTL & 0x7FFFFFFFu, GOOD_STRIDE, GOOD_SIZE,
                            GOOD_SURF, GOOD_APERTURE, &r, &g, &b), 2u);
    checkU32("red was overwritten, not left zero",   r, 0xFFFFFFFFu);
    checkU32("green was overwritten, not left zero", g, 0xFFFFFFFFu);
    checkU32("blue was overwritten, not left zero",  b, 0xFFFFFFFFu);

    //  An undescribable format is the arm that reaches igPixelMasks() and gets a
    //  refusal back from it, so this checks the forward rather than the guard.
    r = 0x11111111u; g = 0x22222222u; b = 0x33333333u;
    checkU32("an undescribable format refuses the masks",
             igPresentMasks(ctl565, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, &r, &g, &b), 4u);
    checkU32("red is the sentinel, not the caller's value",   r, 0xFFFFFFFFu);
    checkU32("green is the sentinel, not the caller's value", g, 0xFFFFFFFFu);
    checkU32("blue is the sentinel, not the caller's value",  b, 0xFFFFFFFFu);

    //  A null out-param refuses rather than dereferencing, in all three slots.
    r = 0u; g = 0u; b = 0u;
    checkU32("a null red pointer refuses",
             igPresentMasks(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, 0, &g, &b), 1u);
    checkU32("a null green pointer refuses",
             igPresentMasks(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, &r, 0, &b), 1u);
    checkU32("a null blue pointer refuses",
             igPresentMasks(GOOD_CTL, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, &r, &g, 0), 1u);

    //  The pair IntelPixel.hpp exists for: igFbBytesPerPixel() returns 4 for
    //  both XRGB_2101010 and XRGB_8888, so a component width derived from
    //  bytes-per-pixel is right for one and wrong for the other -- and the wrong
    //  direction yields a picture whose colours are wrong rather than one that is
    //  obviously broken.
    //
    //  CORRECTED 2026-11-01 (t-a7929c). GOOD_CTL is itself the 10-bit format --
    //  the capture is XRGB_2101010, code 2 at bit 25 -- so section 4 is the
    //  10-bit side of the pair and this section composes the 8-bit side. The old
    //  composition, (2u << 23) under a 0x0F << 23 clear, wrote 5-bit field 2,
    //  which is code 1 (NV12). Code n belongs at bit 24
    //  (REG_FIELD_PREP(PLANE_CTL_FORMAT_MASK_SKL, n),
    //  reference/i915/skl_universal_plane_regs.h:50-60), and the clear has to
    //  cover all five field bits, 0x1F << 23.
    const IGPR32 ctl8888 = (GOOD_CTL & ~(0x1Fu << 23)) | (4u << 24);
    checkU32("the composed word differs from GOOD_CTL only in bits 27:23",
             (ctl8888 ^ GOOD_CTL) & ~(0x1Fu << 23), 0u);
    checkU32("the composed word really decodes to XRGB_8888",
             igFbFormat(ctl8888), 4u);
    checkU32("the 8-bit plane is usable",
             igPresentVerdict(ctl8888, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                              GOOD_APERTURE), 0u);
    checkU32("the 8-bit plane also reports 32 bits per pixel",
             igPresentBitsPerPixel(ctl8888, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                   GOOD_APERTURE), 32u);
    checkU32("but 8 bits per component, not 10",
             igPresentComponentBits(ctl8888, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                                    GOOD_APERTURE), 8u);

    r = 0u; g = 0u; b = 0u;
    checkU32("the 8-bit masks answer OK",
             igPresentMasks(ctl8888, GOOD_STRIDE, GOOD_SIZE, GOOD_SURF,
                            GOOD_APERTURE, &r, &g, &b), 0u);
    checkU32("red mask is 8-bit",   r, 0x00FF0000u);
    checkU32("green mask is 8-bit", g, 0x0000FF00u);
    checkU32("blue mask is 8-bit",  b, 0x000000FFu);

    //  NEGATIVE CONTROL for the bit-23 composition this section used to make:
    //  code 2 written at bit 23 is 5-bit field 2, which decodes to code 1.
    checkU32("code 2 written at bit 23 decodes to code 1, not 2",
             igFbFormat((GOOD_CTL & ~(0x1Fu << 23)) | (2u << 23)), 1u);
}

int main(void)
{
    std::printf("IntelFbPresent.hpp -- the IOFramebuffer description join\n");

    testConstants();
    testFamilyShape();
    testSentinelsAgree();
    testAcceptPath();
    testEachRefusalArm();
    testRefusalOrder();
    testAccessorsRefuse();
    testMasks();

    std::printf("\n%d/%d passed, %d failed\n",
                gChecks - gFailures, gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
