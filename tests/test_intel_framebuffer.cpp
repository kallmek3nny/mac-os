//
//  test_intel_framebuffer.cpp -- host-side suite for IntelFramebuffer.hpp.
//
//  Compiles the IDENTICAL header the kext uses, under MSVC, on a machine with
//  no Intel GPU and no macOS. That is only possible because the header is
//  dependency-free.
//
//  TWO RULES THIS SUITE OBEYS
//
//  1. Every expected value is a BARE LITERAL. Checking a decode against the
//     same #define the decode reads cancels out and proves nothing.
//
//  2. The observed register block is NOT the main driver. 0x82009000 has bit
//     15 set, so igFbInheritable() returns COMPRESSED before format, stride,
//     size or the overflow guard is ever consulted -- and its bit 23 is CLEAR,
//     so the 5-bit ICL mask and the 4-bit SKL mask both yield a supported
//     4-byte format. The one piece of live silicon evidence separates nothing.
//     Synthetic inputs chosen to SEPARATE the readings do the real work here.
//

#include <cstdio>

#include "../phase0-kext/IntelFramebuffer.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (const char *p = name; *p != '\0'; ++p)
        std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGFB32 got, IGFB32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s %s\n", label, got ? "true" : "false");
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got %s want %s\n", label,
                    got ? "true" : "false", want ? "true" : "false");
    }
}

//
//  The observed block, retyped from
//  igpu-results/tgl-capture-2026-08-16-022556.txt rather than read from the
//  header's own #defines -- so a typo in either copy separates them.
//
static void testObservedBlockIsWhatWasCaptured(void)
{
    section("observed register block agrees with the capture");

    checkU32("CTL",    IG_FB_CTL_OBSERVED_2026_08_16,    0x82009000u);
    checkU32("STRIDE", IG_FB_STRIDE_OBSERVED_2026_08_16, 0x0000003Cu);
    checkU32("SIZE",   IG_FB_SIZE_OBSERVED_2026_08_16,   0x0437077Fu);
    checkU32("SURF",   IG_FB_SURF_OBSERVED_2026_08_16,   0x01058000u);
    checkU32("KEYMAX", IG_FB_KEYMAX_OBSERVED_2026_08_16, 0xFF000000u);

    checkU32("panel width",  IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16,  1920u);
    checkU32("panel height", IG_FB_PANEL_HEIGHT_OBSERVED_2026_08_16, 1080u);
}

//
//  THE ARM THE CAPTURE CANNOT REACH.
//
//  0x82009000 has bit 23 CLEAR, so the 5-bit decode and the 4-bit SKL reading
//  agree on it: both give 2, XRGB_2101010. Setting bit 23 is what separates
//  them: igFbFormat() carries it as IG_FB_CTL_FORMAT_ICL_FLAG (0x12, unnamed,
//  bpp 0), while the SKL reading drops it and lands on 2 (XRGB_2101010, bpp
//  4). If this header ever regresses to the SKL mask, the bpp check below is
//  the assertion that goes red.
//
//  CORRECTED 2026-11-01 (t-a7929c). This section used to pin the RAW 5-bit
//  field as the format code: 5 for 0x82800000, and 4 -- read as XRGB_8888 --
//  for the capture. The codes are REG_FIELD_PREP'd against the SKL mask
//  (reference/i915/skl_universal_plane_regs.h:43-60), so code n is 5-bit field
//  2n: raw field 4 is code 2, XRGB_2101010, and XRGB_8888 is field 8. The
//  checks after the contrast pair are the negative controls for that mapping.
//
static void testFormatMaskIsFiveBits(void)
{
    section("format field is 5 bits on tgl, codes are 4-bit");

    //  bit 23 set, bits 27:24 == 2. 5-bit field = 5, SKL code = 2.
    const IGFB32 ctl = 0x82800000u;

    checkU32("5-bit field of 0x82800000", (ctl >> 23) & 0x1Fu, 5u);
    checkU32("icl decode keeps bit 23 as the flag", igFbFormat(ctl),
             2u | IG_FB_CTL_FORMAT_ICL_FLAG);
    checkU32("bpp of that code is unsupported", igFbBytesPerPixel(igFbFormat(ctl)), 0u);
    checkU32("skl reading would have been", (ctl >> 24) & 0x0Fu, 2u);
    checkU32("and skl's code IS supported", igFbBytesPerPixel(2u), 4u);

    //  The observed CTL, for contrast: bit 23 is clear, so both readings give
    //  2, which is exactly why it proves nothing about the mask here.
    checkU32("observed decode is XRGB_2101010", igFbFormat(0x82009000u),
             IG_FB_FORMAT_XRGB_2101010);
    checkU32("observed skl reading", (0x82009000u >> 24) & 0x0Fu, 2u);

    //  NEGATIVE CONTROLS for the 2n mapping. The capture's raw field is 4, the
    //  XRGB_8888 CODE -- the coincidence the old decode fell for. A decoder
    //  that returns the raw field fails every check below except the first.
    checkU32("observed raw 5-bit field is 4", (0x82009000u >> 23) & 0x1Fu, 4u);
    checkBool("and that raw 4 is NOT read as XRGB_8888",
              igFbFormat(0x82009000u) != IG_FB_FORMAT_XRGB_8888, true);
    checkU32("XRGB_8888 is 5-bit field 8 (0x84000000)",
             igFbFormat(0x84000000u), IG_FB_FORMAT_XRGB_8888);
    checkU32("RGB_565 is 5-bit field 28 (0x8E000000)",
             igFbFormat(0x8E000000u), IG_FB_FORMAT_RGB_565);
    checkU32("the encoder puts XRGB_8888 at bit 26, not bit 25",
             igFbFormatCtlBits(IG_FB_FORMAT_XRGB_8888), 0x04000000u);
    checkU32("and RGB_565 at bits 27:25 with bit 23 clear",
             igFbFormatCtlBits(IG_FB_FORMAT_RGB_565), 0x0E000000u);

    //  igFbFormatCtlBits() inverts igFbFormat() on all 32 field values, and no
    //  odd (icl-only) field value decodes to a code with a byte size.
    IGFB32 roundTripMisses = 0u;
    IGFB32 oddSized = 0u;
    for (IGFB32 field = 0u; field < 32u; ++field) {
        const IGFB32 word = field << 23;
        const IGFB32 code = igFbFormat(word);
        if (igFbFormatCtlBits(code) != word)
            ++roundTripMisses;
        if ((field & 1u) != 0u && igFbBytesPerPixel(code) != 0u)
            ++oddSized;
    }
    checkU32("every 5-bit field value round-trips", roundTripMisses, 0u);
    checkU32("no odd field value decodes to a sized format", oddSized, 0u);
}

static void testBytesPerPixel(void)
{
    section("bytes per pixel, by bare format code");

    checkU32("RGB_565 (14)",         igFbBytesPerPixel(14u), 2u);
    checkU32("XRGB_8888 (4)",        igFbBytesPerPixel(4u),  4u);
    checkU32("XRGB_2101010 (2)",     igFbBytesPerPixel(2u),  4u);
    checkU32("XRGB_16161616F (6)",   igFbBytesPerPixel(6u),  8u);
    checkU32("unnamed code 5",       igFbBytesPerPixel(5u),  0u);
    checkU32("unnamed code 31",      igFbBytesPerPixel(31u), 0u);
    checkU32("code 0",               igFbBytesPerPixel(0u),  0u);
}

static void testCtlFlags(void)
{
    section("PLANE_CTL flag bits");

    checkBool("enable set (bit 31)",   igFbPlaneEnabled(0x80000000u), true);
    checkBool("enable clear",          igFbPlaneEnabled(0x7FFFFFFFu), false);
    checkBool("decomp set (bit 15)",   igFbRenderCompressed(0x00008000u), true);
    //  bit 16 is the far side of bit 15 -- a mask widened to 0x18000 fails here.
    checkBool("bit 16 is not decomp",  igFbRenderCompressed(0x00010000u), false);
    checkBool("bit 14 is not decomp",  igFbRenderCompressed(0x00004000u), false);
    checkBool("async set (bit 9)",     igFbAsyncFlip(0x00000200u), true);
    checkBool("bit 10 is not async",   igFbAsyncFlip(0x00000400u), false);
    checkBool("bit 8 is not async",    igFbAsyncFlip(0x00000100u), false);

    checkBool("observed is enabled",    igFbPlaneEnabled(0x82009000u), true);
    checkBool("observed IS compressed", igFbRenderCompressed(0x82009000u), true);
    checkBool("observed async clear",   igFbAsyncFlip(0x82009000u), false);
}

static void testTiling(void)
{
    section("tiling field, bits 12:10");

    checkU32("linear",              igFbTiling(0x00000000u), 0u);
    checkU32("X (code 1)",          igFbTiling(0x00000400u), 1u);
    checkU32("Y (code 4)",          igFbTiling(0x00001000u), 4u);
    checkU32("YF-or-Tile4 (code 5)", igFbTiling(0x00001400u), 5u);
    checkU32("code 7 saturates",    igFbTiling(0x00001C00u), 7u);
    //  bit 13 is the far side of the field -- a 4-bit mask would read 8 here.
    checkU32("bit 13 excluded",     igFbTiling(0x00002000u), 0u);
    checkU32("bit 9 excluded",      igFbTiling(0x00000200u), 0u);
    checkU32("observed tiling is Y", igFbTiling(0x82009000u), 4u);
}

//
//  PLANE_SIZE halves are INVERTED relative to PIPESRC. Both readings decode
//  to 1920x1080 on the observed pair, so the transposed value is checked
//  explicitly: 0x077F0437 is PIPESRC's byte order and must come back 1080x1920.
//
static void testSizeAndPos(void)
{
    section("PLANE_SIZE and PLANE_POS halves");

    checkU32("observed width",  igFbSizeWidth(0x0437077Fu),  1920u);
    checkU32("observed height", igFbSizeHeight(0x0437077Fu), 1080u);

    checkU32("pipesrc order transposes width",  igFbSizeWidth(0x077F0437u),  1080u);
    checkU32("pipesrc order transposes height", igFbSizeHeight(0x077F0437u), 1920u);

    //  Both halves store value - 1, so an all-zero page is a plausible 1x1.
    checkU32("zero page width",  igFbSizeWidth(0x00000000u),  1u);
    checkU32("zero page height", igFbSizeHeight(0x00000000u), 1u);
    checkU32("max half width",   igFbSizeWidth(0x0000FFFFu),  65536u);
    checkU32("max half height",  igFbSizeHeight(0xFFFF0000u), 65536u);

    //  POS is deliberately NOT +1'd -- 0,0 must be expressible.
    checkU32("pos x zero",  igFbPosX(0x00000000u), 0u);
    checkU32("pos y zero",  igFbPosY(0x00000000u), 0u);
    checkU32("pos x 100",   igFbPosX(0x00000064u), 100u);
    checkU32("pos y 200",   igFbPosY(0x00C80000u), 200u);
    checkU32("pos x ignores y half", igFbPosX(0x00C80064u), 100u);
    checkU32("pos y ignores x half", igFbPosY(0x00C80064u), 200u);
}

//
//  The 128-byte unit is DERIVED from one measurement: 1920 * 4 == 7680, and
//  7680 / 0x3C == 128. The check below reassembles that from bare literals so
//  a changed unit fails rather than cancelling out.
//
static void testStride(void)
{
    section("PLANE_STRIDE, 128-byte unit derived 2026-08-16");

    checkU32("observed stride bytes", igFbStrideBytes(0x0000003Cu), 7680u);
    checkU32("one unit",              igFbStrideBytes(0x00000001u), 128u);
    checkU32("zero",                  igFbStrideBytes(0x00000000u), 0u);
    //  bit 12 is the far side of the 12-bit field.
    checkU32("bit 12 excluded",       igFbStrideBytes(0x00001000u), 0u);
    checkU32("full field",            igFbStrideBytes(0x00000FFFu), 524160u);
    checkU32("high bits ignored",     igFbStrideBytes(0xFFFFF03Cu), 7680u);

    //  A 64-byte unit is refuted by the same measurement: it would describe a
    //  960-pixel row on a lit 1920-pixel panel.
    checkU32("a 64-byte unit would give", 0x3Cu * 64u, 3840u);
}

static void testSurface(void)
{
    section("PLANE_SURF address and flags");

    checkU32("observed offset",   igFbSurfaceOffset(0x01058000u), 0x01058000u);
    checkU32("low 12 masked off", igFbSurfaceOffset(0x01058FFFu), 0x01058000u);
    checkU32("bit 12 kept",       igFbSurfaceOffset(0x00001000u), 0x00001000u);
    checkBool("decrypt set (bit 2)", igFbSurfaceDecrypt(0x00000004u), true);
    checkBool("bit 3 is not decrypt", igFbSurfaceDecrypt(0x00000008u), false);
    checkBool("bit 1 is not decrypt", igFbSurfaceDecrypt(0x00000002u), false);
    checkBool("observed decrypt clear", igFbSurfaceDecrypt(0x01058000u), false);
}

//
//  THE ARMS THE CAPTURE CANNOT REACH, PART 2.
//
//  The observed CTL is compressed, so igFbInheritable() early-returns
//  COMPRESSED and never consults format, stride or geometry. Every verdict
//  below therefore uses a synthetic CTL with bit 15 CLEARED.
//
static void testInheritVerdicts(void)
{
    section("inheritance verdicts (bit 15 cleared to reach past COMPRESSED)");

    //  enabled, XRGB_2101010 (code 2 -> bit 25, still 4 bpp), Tile Y, decomp
    //  clear. CORRECTED 2026-11-01 (t-a7929c): this said XRGB_8888, which is
    //  code 4 at bit 26.
    const IGFB32 ok = 0x82001000u;

    checkU32("the synthetic base decodes to bpp 4",
             igFbBytesPerPixel(igFbFormat(ok)), 4u);
    checkBool("and is NOT compressed", igFbRenderCompressed(ok), false);

    checkU32("OK",          igFbInheritable(ok, 0x3Cu, 0x0437077Fu), 0u);
    checkU32("DISABLED",    igFbInheritable(ok & 0x7FFFFFFFu, 0x3Cu, 0x0437077Fu), 1u);
    checkU32("COMPRESSED",  igFbInheritable(ok | 0x8000u, 0x3Cu, 0x0437077Fu), 2u);
    //  format field 5 -- unsupported, and only reachable with bit 23 set.
    checkU32("FORMAT",      igFbInheritable(0x82801000u, 0x3Cu, 0x0437077Fu), 3u);
    checkU32("STRIDE zero", igFbInheritable(ok, 0x00000000u, 0x0437077Fu), 4u);
    checkU32("GEOMETRY",    igFbInheritable(ok, 0x3Cu, 0x00000000u), 5u);

    //  COMPRESSED must be checked BEFORE format: a compressed plane whose
    //  format is ALSO unsupported must still report COMPRESSED.
    checkU32("compressed beats format",
             igFbInheritable(0x82809000u, 0x3Cu, 0x0437077Fu), 2u);

    //  A row that does not FIT is a sheared picture, not a fault. 1920 * 4 is
    //  7680, so 0x3B units (7552 bytes) is one unit short.
    checkU32("stride one unit short",
             igFbInheritable(ok, 0x3Bu, 0x0437077Fu), 4u);
    checkU32("stride exactly fits",
             igFbInheritable(ok, 0x3Cu, 0x0437077Fu), 0u);

    //  A 1x1 plane is what an unprogrammed page decodes to.
    checkU32("2x2 is accepted",  igFbInheritable(ok, 0x3Cu, 0x00010001u), 0u);
    checkU32("1 wide refused",   igFbInheritable(ok, 0x3Cu, 0x00010000u), 5u);
    checkU32("1 tall refused",   igFbInheritable(ok, 0x3Cu, 0x00000001u), 5u);

    //  And the observed block, for the record: COMPRESSED, which is the whole
    //  reason this driver cannot adopt the firmware's plane as it stands.
    checkU32("observed block verdict",
             igFbInheritable(0x82009000u, 0x0000003Cu, 0x0437077Fu), 2u);
}

static void testSurfaceBytes(void)
{
    section("surface size, stride * height");

    //  7680 * 1080 == 8294400.
    checkU32("observed surface bytes",
             igFbSurfaceBytes(0x0000003Cu, 0x0437077Fu), 8294400u);
    //  NOT width * bpp * height -- they coincide here only because the stride
    //  is tight, so a padded stride is checked too: 0x40 units == 8192 bytes.
    checkU32("padded stride is honoured",
             igFbSurfaceBytes(0x00000040u, 0x0437077Fu), 8847360u);
    checkU32("zero stride refuses",
             igFbSurfaceBytes(0x00000000u, 0x0437077Fu), 0u);
    //  Full 12-bit stride (524160) times 65536 rows overflows 32 bits and must
    //  refuse rather than wrap to a small plausible number.
    checkU32("overflow refuses",
             igFbSurfaceBytes(0x00000FFFu, 0xFFFF0000u), 0u);
    checkU32("just under the ceiling",
             igFbSurfaceBytes(0x00000001u, 0x00000000u), 128u);
}

//
// --- the surface range ------------------------------------------------------
//
// igFbSurfaceRangeOk() joins a BASE (igFbSurfaceOffset) to an EXTENT
// (igFbSurfaceBytes). Every bound below is a bare literal, and the wrap arm is
// the one that matters: a 32-bit sum of 0xFFFFF000 + 0xFFFFF000 is 0xFFFFE000,
// which passes any 32-bit ceiling, so a correct implementation and a wrapping
// one differ ONLY on inputs whose true sum exceeds 2^32. The observed register
// block cannot separate them -- its surface sits at 0x01058000 with an extent
// of 8 MiB, nowhere near the edge.
//
static void testSurfaceRange(void)
{
    section("igFbSurfaceRangeOk -- base joined to extent");

    //  1920 * 4 == 7680 bytes per row, 1080 rows == 8294400 bytes.
    checkBool("the observed surface plus its real extent fits",
              igFbSurfaceRangeOk(0x01058000u, 8294400u), true);

    //
    //  A ZERO EXTENT IS A REFUSAL. igFbSurfaceBytes() returns 0 for every input
    //  it cannot describe, so accepting 0 here would launder that refusal into
    //  a valid range of nothing.
    //
    checkBool("a zero extent is refused",
              igFbSurfaceRangeOk(0x01058000u, 0u), false);
    checkBool("and a zero extent at a zero base is still refused",
              igFbSurfaceRangeOk(0u, 0u), false);

    //
    //  BOTH SIDES OF THE CEILING. 0xFFFFF000 is the largest page the 20-bit
    //  field can name; one page on top of it lands exactly on 2^32, which is
    //  the last address the field can express and therefore accepted.
    //
    checkBool("the highest page plus one page lands exactly on the limit",
              igFbSurfaceRangeOk(0xFFFFF000u, 4096u), true);
    checkBool("one byte past the limit is refused",
              igFbSurfaceRangeOk(0xFFFFF000u, 4097u), false);

    //
    //  THE WRAP. In 32-bit arithmetic 0xFFFFF000 + 0xFFFFF000 == 0xFFFFE000, a
    //  small plausible number that passes any ceiling. This is the separating
    //  input for the 64-bit widening, and nothing in the capture reaches it.
    //
    checkBool("a sum that would wrap in 32 bits is refused",
              igFbSurfaceRangeOk(0xFFFFF000u, 0xFFFFF000u), false);
    //
    //  A ONE-PAGE BASE UNDER THE MAXIMUM EXTENT. In 32-bit arithmetic
    //  0x1000 + 0xFFFFFFFF == 0xFFF -- four kilobytes, which passes every
    //  ceiling this function could plausibly carry. The true sum is 2^32 + 4095,
    //  so a correct implementation refuses it. This is the tightest separating
    //  input available: the wrap is to a number so small that no bound catches
    //  it, and the overrun is by only 4095 bytes.
    //
    checkBool("a one-page base under the maximum extent is refused",
              igFbSurfaceRangeOk(0x00001000u, 0xFFFFFFFFu), false);

    //
    //  AND THE ARM NEXT TO IT, which is a CORRECTION rather than an addition.
    //  This line first asserted that the maximum extent at a ZERO base is
    //  refused, and the suite went red on it -- correctly. 0 + 0xFFFFFFFF is
    //  4294967295, which is one byte BELOW 2^32, so it fits and the function
    //  said so. The expectation was wrong, not the code. It is worth leaving the
    //  reasoning here: the limit is an inclusive bound on base + extent, so the
    //  largest accepted extent at a zero base is 2^32 itself, and an off-by-one
    //  in EITHER direction shows up on this pair rather than on the wrap arms.
    //
    checkBool("the maximum 32-bit extent at a zero base fits, one byte spare",
              igFbSurfaceRangeOk(0u, 0xFFFFFFFFu), true);
    checkBool("a 4 GiB extent minus one page at a zero base fits",
              igFbSurfaceRangeOk(0u, 0xFFFFF000u), true);

    //
    //  THE FLAG BITS ARE NOT PART OF THE BASE. igFbSurfaceOffset() masks the
    //  low 12 bits, so setting DECRYPT must not shift the range. If the mask
    //  were dropped, the arm above at exactly the limit would tip over.
    //
    checkBool("flag bits in the low 12 do not move the base",
              igFbSurfaceRangeOk(0xFFFFF00Fu, 4096u), true);
    checkU32("because the base masks them off",
             igFbSurfaceOffset(0xFFFFF00Fu), 0xFFFFF000u);

    //
    //  The ceiling itself, pinned to a bare literal. It is a POLICY ceiling --
    //  nothing in this tree measures the GGTT size on this silicon -- so the
    //  number it must equal is the largest address PLANE_SURF's page field can
    //  name, not a measured aperture.
    //
    checkBool("IG_FB_GGTT_POLICY_LIMIT is 2^32",
              IG_FB_GGTT_POLICY_LIMIT == 0x100000000ull, true);
    checkBool("which is the top page plus one page",
              IG_FB_GGTT_POLICY_LIMIT == 0xFFFFF000ull + 4096ull, true);
}

//
// --- the verdict codes ------------------------------------------------------
//
// testInheritVerdicts() above drives igFbInheritable() and asserts the NUMBER
// it returns per input -- 0 through 5, as bare literals. That is right for a
// decode test, but it leaves the NAMES unpinned: renumber IG_FB_INHERIT_* and
// every one of those returns moves with it, silently, because the function
// returns whatever the #define says.
//
// This arm pins name to value, which is the other half of the assertion. The
// two together make a renumbering visible: this fails if a NAME moves, and
// testInheritVerdicts() fails if the FUNCTION stops returning what its inputs
// deserve.
//
// IG_FB_INHERIT_VERDICT_LIMIT was added 2026-08-17 and is NOT a verdict. It is
// a derived bound, and tools/check-registers.py is what enforces that it still
// equals the largest member -- the property a hand-kept list here cannot
// supply, since forgetting a line is exactly the failure mode.
//
static void testInheritVerdictCodes(void)
{
    section("IG_FB_INHERIT_* verdict codes");

    checkU32("IG_FB_INHERIT_OK",         IG_FB_INHERIT_OK,         0u);
    checkU32("IG_FB_INHERIT_DISABLED",   IG_FB_INHERIT_DISABLED,   1u);
    checkU32("IG_FB_INHERIT_COMPRESSED", IG_FB_INHERIT_COMPRESSED, 2u);
    checkU32("IG_FB_INHERIT_FORMAT",     IG_FB_INHERIT_FORMAT,     3u);
    checkU32("IG_FB_INHERIT_STRIDE",     IG_FB_INHERIT_STRIDE,     4u);
    checkU32("IG_FB_INHERIT_GEOMETRY",   IG_FB_INHERIT_GEOMETRY,   5u);

    checkU32("VERDICT_LIMIT covers the largest verdict",
             IG_FB_INHERIT_VERDICT_LIMIT, 5u);

    //
    //  The limit is NOT itself a verdict, and this is the check that says so.
    //  It shares a value with GEOMETRY by construction; a reader who switches
    //  on it would silently handle the geometry case.
    //
    checkBool("the limit equals GEOMETRY, so it is a bound not a case",
              IG_FB_INHERIT_VERDICT_LIMIT == IG_FB_INHERIT_GEOMETRY, true);

    //
    //  OK IS ZERO as a property, not as a coincidence of the line above. Every
    //  call site is `if (verdict != IG_FB_INHERIT_OK)`.
    //
    checkBool("OK is the zero", IG_FB_INHERIT_OK == 0u, true);
    checkBool("and every refusal is non-zero",
              IG_FB_INHERIT_DISABLED != 0u && IG_FB_INHERIT_COMPRESSED != 0u
              && IG_FB_INHERIT_FORMAT != 0u && IG_FB_INHERIT_STRIDE != 0u
              && IG_FB_INHERIT_GEOMETRY != 0u, true);
}

int main(void)
{
    std::printf("IntelFramebuffer.hpp -- Gen12 plane register decode\n");
    std::printf("Masks from reference/i915/skl_universal_plane_regs.h at v6.12.\n");
    std::printf("One observed block, from igpu-results/tgl-capture-2026-08-16-022556.txt.\n");

    testObservedBlockIsWhatWasCaptured();
    testFormatMaskIsFiveBits();
    testBytesPerPixel();
    testCtlFlags();
    testTiling();
    testSizeAndPos();
    testStride();
    testSurface();
    testInheritVerdicts();
    testSurfaceBytes();
    testSurfaceRange();
    testInheritVerdictCodes();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
