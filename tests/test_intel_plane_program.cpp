//
//  test_intel_plane_program.cpp
//  Host tests for IntelPlaneProgram.hpp: the encode side of the Intel
//  plane registers. These are the values the kext will write to flip a
//  GGTT sink (eventually the 3050's output) onto the panel's plane.
//
//  Discipline, same as the sink suites: every refusal arm reached by a
//  singly-bad input, each numeric bound pinned at its last accepting value,
//  the refusal order pinned by doubly-bad inputs, and `out` proven untouched
//  on every refusal. Every encoded field that HAS a decoder in
//  IntelFramebuffer.hpp is decoded back through it -- except the stride,
//  whose decoder is pinned as NOT matching (see testStrideDecoderAsymmetry).
//

#include <cstdio>

#include "IntelPlaneProgram.hpp"
#include "IntelSinkAlloc.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkTrue(bool ok, const char *what)
{
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

//  The geometry every success case starts from: the panel the firmware lit,
//  as a LINEAR XRGB8888 sink.
static const IGFB32 kW      = IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16;
static const IGFB32 kH      = IG_FB_PANEL_HEIGHT_OBSERVED_2026_08_16;
static const IGFB32 kStride = IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16 * 4u;
static const IGFB32 kFmt    = IG_FB_FORMAT_XRGB_8888;
static const IGFB32 kSurf   = 0x01880000u;

//  A poison value no successful build can produce in every field at once.
static const IGFB32 kPoison = 0xA5A5A5A5u;

static void poison(IgPlaneRegs *r)
{
    r->ctl = r->stride = r->pos = r->size = r->surf = kPoison;
}

static bool untouched(const IgPlaneRegs &r)
{
    return r.ctl == kPoison && r.stride == kPoison && r.pos == kPoison
           && r.size == kPoison && r.surf == kPoison;
}

//  Runs one refusal case: the verdict must match AND `out` must be untouched.
static void refuse(IGFB32 baseCtl, IGFB32 format, IGFB32 surf, IGFB32 width,
                   IGFB32 height, IGFB32 strideBytes, IGFB32 want,
                   const char *what)
{
    IgPlaneRegs r;
    poison(&r);
    IGFB32 got = igPlaneBuildLinear(baseCtl, format, surf, width, height,
                                    strideBytes, &r);
    checkTrue(got == want, what);
    checkTrue(untouched(r), what);
}

static void testLinearStride()
{
    checkTrue(igPlaneEncodeLinearStride(0u) == IG_FB_INVALID, "stride 0 rejected");
    checkTrue(igPlaneEncodeLinearStride(100u) == IG_FB_INVALID,
          "stride not a 64-byte multiple rejected");
    checkTrue(igPlaneEncodeLinearStride(63u) == IG_FB_INVALID,
          "stride one byte short of a unit rejected");
    checkTrue(igPlaneEncodeLinearStride(64u) == 1u, "stride 64 -> 1 unit");
    checkTrue(igPlaneEncodeLinearStride(kStride) == 120u,
          "1920x32bpp stride -> 120 units (not 60: linear unit is 64, not 128)");
    checkTrue(igPlaneEncodeLinearStride(IG_FB_STRIDE_MASK
                                    * IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED)
              == IG_FB_STRIDE_MASK,
          "widest stride fits the field");
    checkTrue(igPlaneEncodeLinearStride((IG_FB_STRIDE_MASK + 1u)
                                    * IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED)
              == IG_FB_INVALID,
          "stride one unit past the field rejected");
}

//  igFbStrideBytes() decodes with the 128-byte unit DERIVED from the
//  Y-tiled firmware plane. It is tiling-unaware, so it does NOT invert the
//  linear encoder: a linear stride read back through it comes out doubled.
//  Pinned so that nobody "verifies after write" a linear sink through the
//  tiled decoder and reads a wrong-but-plausible 2x stride as a mismatch --
//  or worse, "fixes" the encoder to 128 so the round trip agrees.
static void testStrideDecoderAsymmetry()
{
    IGFB32 field = igPlaneEncodeLinearStride(kStride);
    checkTrue(field * IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED == kStride,
          "linear field * 64 recovers the stride in bytes");
    checkTrue(igFbStrideBytes(field) == 2u * kStride,
          "tiled decoder reads a linear field as DOUBLE the stride");
    checkTrue(igFbStrideBytes(IG_FB_STRIDE_OBSERVED_2026_08_16) == kStride,
          "tiled decoder is right for the Y-tiled firmware plane it came from");
    checkTrue(IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED * 2u
              == IG_FB_STRIDE_UNIT_DERIVED_2026_08_16,
          "linear unit is exactly half the tiled unit");
}

static void testSize()
{
    checkTrue(igPlaneEncodeSize(0u, kH) == IG_FB_INVALID, "width 0 rejected");
    checkTrue(igPlaneEncodeSize(kW, 0u) == IG_FB_INVALID, "height 0 rejected");
    checkTrue(igPlaneEncodeSize(kW, kH)
              == ((1919u << IG_FB_SIZE_WIDTH_SHIFT)
                  | (1079u << IG_FB_SIZE_HEIGHT_SHIFT)),
          "1920x1080 stored minus one");
    checkTrue(igPlaneEncodeSize(kW, kH) == IG_FB_SIZE_OBSERVED_2026_08_16,
          "panel geometry encodes to exactly what firmware programmed");
    checkTrue(igFbSizeWidth(igPlaneEncodeSize(kW, kH)) == kW,
          "size width round-trips through igFbSizeWidth");
    checkTrue(igFbSizeHeight(igPlaneEncodeSize(kW, kH)) == kH,
          "size height round-trips through igFbSizeHeight");
    checkTrue(igPlaneEncodeSize(1u, 1u) == 0u, "1x1 encodes to zero");
    checkTrue(igPlaneEncodeSize(IG_FB_SIZE_HALF_MASK + 2u, 1u) == IG_FB_INVALID,
          "width past the field rejected");
    checkTrue(igPlaneEncodeSize(1u, IG_FB_SIZE_HALF_MASK + 2u) == IG_FB_INVALID,
          "height past the field rejected");

    // Last accepting value on one axis: 65536 wide, 65535 tall is a real
    // encoding and must round-trip.
    IGFB32 wide = igPlaneEncodeSize(IG_FB_SIZE_HALF_MASK + 1u,
                                    IG_FB_SIZE_HALF_MASK);
    checkTrue(wide != IG_FB_INVALID && igFbSizeWidth(wide) == 65536u
              && igFbSizeHeight(wide) == 65535u,
          "65536x65535 encodes and round-trips");

    // KNOWN SENTINEL COLLISION. 65536x65536 encodes to 0xFFFFFFFF, which IS
    // IG_FB_INVALID, so the largest field value is indistinguishable from a
    // refusal. It fails in the SAFE direction (refused, never mis-programmed)
    // and real Gen12 planes stop far below 65536, so it is pinned rather than
    // fixed. If this check ever flips, the sentinel changed -- re-read every
    // caller that compares against IG_FB_INVALID.
    checkTrue(igPlaneEncodeSize(IG_FB_SIZE_HALF_MASK + 1u, IG_FB_SIZE_HALF_MASK + 1u)
              == IG_FB_INVALID,
          "65536x65536 collides with the IG_FB_INVALID sentinel (safe refusal)");
}

static void testPos()
{
    checkTrue(igPlaneEncodePos(0u, 0u) == 0u, "origin encodes to zero");
    checkTrue(igPlaneEncodePos(5u, 7u)
              == ((5u << IG_FB_POS_X_SHIFT) | (7u << IG_FB_POS_Y_SHIFT)),
          "x/y land in their halves");
    checkTrue(igFbPosX(igPlaneEncodePos(123u, 456u)) == 123u
              && igFbPosY(igPlaneEncodePos(123u, 456u)) == 456u,
          "pos round-trips through igFbPosX/igFbPosY (no minus-one)");
}

static void testWriteOrder()
{
    checkTrue(igPlaneWriteOffset(0u) == IG_FB_OFF_CTL, "CTL written first");
    checkTrue(igPlaneWriteOffset(1u) == IG_FB_OFF_STRIDE
              && igPlaneWriteOffset(2u) == IG_FB_OFF_POS
              && igPlaneWriteOffset(3u) == IG_FB_OFF_SIZE,
          "STRIDE, POS, SIZE written between CTL and SURF");
    checkTrue(igPlaneWriteOffset(4u) == IG_FB_OFF_SURF,
          "SURF written last so the update arms with the new geometry");

    int surfWrites = 0;
    for (IGFB32 i = 0u; i < IG_PLANE_WRITE_STEPS; ++i)
        if (igPlaneWriteOffset(i) == IG_FB_OFF_SURF)
            ++surfWrites;
    checkTrue(surfWrites == 1, "SURF is written exactly once (one arm per flip)");
    checkTrue(igPlaneWriteOffset(IG_PLANE_WRITE_STEPS) == IG_FB_INVALID,
              "a step past the sequence yields no register offset");
}

static void testBuildFromFirmwareCtl()
{
    IgPlaneRegs r;
    poison(&r);
    IGFB32 v = igPlaneBuildLinear(IG_FB_CTL_OBSERVED_2026_08_16, kFmt, kSurf,
                                  kW, kH, kStride, &r);
    checkTrue(v == IG_PLANE_OK, "panel-sized linear sink builds");
    checkTrue(igFbPlaneEnabled(r.ctl), "built CTL is enabled");
    checkTrue(igFbFormat(r.ctl) == kFmt, "built CTL decodes to XRGB8888");
    checkTrue(igFbTiling(r.ctl) == IG_FB_TILING_LINEAR,
          "built CTL is LINEAR even though firmware's was Y-tiled");
    checkTrue(igFbTiling(IG_FB_CTL_OBSERVED_2026_08_16) == IG_FB_TILING_Y,
          "premise: firmware CTL really was Y-tiled");
    checkTrue(!igFbRenderCompressed(r.ctl) && !igFbAsyncFlip(r.ctl),
          "built CTL is uncompressed and sync-flipped");
    checkTrue((r.ctl & ~IG_PLANE_CTL_OWNED_MASK)
              == (IG_FB_CTL_OBSERVED_2026_08_16 & ~IG_PLANE_CTL_OWNED_MASK),
          "every non-owned firmware CTL bit carried over unchanged");
    checkTrue(r.stride == 120u, "built STRIDE is 120 linear units");
    checkTrue(r.size == IG_FB_SIZE_OBSERVED_2026_08_16, "built SIZE is the panel");
    checkTrue(r.pos == 0u, "built POS is the origin");
    checkTrue(r.surf == kSurf && igFbSurfaceOffset(r.surf) == kSurf,
          "built SURF is the sink base, no flag bits");
}

static void testBuildClearsOwnedBits()
{
    IgPlaneRegs r;
    poison(&r);
    IGFB32 dirty = IG_FB_CTL_RENDER_DECOMP | IG_FB_CTL_ASYNC_FLIP
                   | (IG_FB_TILING_YF_OR_4 << IG_FB_CTL_TILING_SHIFT)
                   | (IG_FB_FORMAT_RGB_565 << IG_FB_CTL_FORMAT_SHIFT);
    checkTrue(igPlaneBuildLinear(dirty, kFmt, kSurf, kW, kH, kStride, &r)
              == IG_PLANE_OK,
          "build from a base CTL with every owned bit dirty");
    checkTrue(!igFbRenderCompressed(r.ctl), "stale RENDER_DECOMP cleared");
    checkTrue(!igFbAsyncFlip(r.ctl), "stale ASYNC_FLIP cleared");
    checkTrue(igFbTiling(r.ctl) == IG_FB_TILING_LINEAR, "stale tiling replaced");
    checkTrue(igFbFormat(r.ctl) == kFmt, "stale format replaced");

    poison(&r);
    checkTrue(igPlaneBuildLinear(0u, kFmt, kSurf, kW, kH, kStride, &r)
              == IG_PLANE_OK && igFbPlaneEnabled(r.ctl),
          "a zero base CTL still yields an ENABLED plane");
}

static void testRefusalArms()
{
    // Positive control on untouched(): a successful build writes out, so the
    // poison must be gone. Without this, an untouched() that always returned
    // true would make every refusal case below pass vacuously.
    {
        IgPlaneRegs r;
        poison(&r);
        igPlaneBuildLinear(0u, kFmt, kSurf, kW, kH, kStride, &r);
        checkTrue(!untouched(r), "positive control: a successful build DOES write out");
    }

    checkTrue(igPlaneBuildLinear(0u, kFmt, kSurf, kW, kH, kStride, 0)
              == IG_PLANE_NULL_OUT,
          "null out rejected");
    refuse(0u, 0u, kSurf, kW, kH, kStride, IG_PLANE_BAD_FORMAT,
           "unknown format 0 refused");
    refuse(0u, 31u, kSurf, kW, kH, kStride, IG_PLANE_BAD_FORMAT,
           "unknown format 31 refused");
    refuse(0u, kFmt, kSurf, 0u, kH, kStride, IG_PLANE_BAD_GEOMETRY,
           "zero width refused");
    refuse(0u, kFmt, kSurf, kW, kH, 0u, IG_PLANE_BAD_STRIDE,
           "zero stride refused");
    refuse(0u, kFmt, kSurf, kW, kH, kStride + 1u, IG_PLANE_BAD_STRIDE,
           "stride not a whole 64-byte unit refused");
    refuse(0u, kFmt, kSurf, kW, kH, kStride - 64u, IG_PLANE_BAD_STRIDE,
           "stride one unit short of the row refused");
    refuse(0u, kFmt, kSurf + 0x800u, kW, kH, kStride, IG_PLANE_BAD_SURF_ALIGN,
           "surf not page aligned refused");
    refuse(0u, kFmt, kSurf | IG_FB_SURF_DECRYPT, kW, kH, kStride,
           IG_PLANE_BAD_SURF_ALIGN,
           "surf carrying a flag bit refused, not silently masked");
    refuse(0u, kFmt, 0xFFFFF000u, 16u, 65u, 64u, IG_PLANE_BAD_SURF_RANGE,
           "surface one row past the 4 GiB GGTT policy limit refused");
    refuse(0u, kFmt, 0u, 1u, IG_FB_SIZE_HALF_MASK + 1u,
           IG_FB_STRIDE_MASK * IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED,
           IG_PLANE_BAD_SURF_RANGE,
           "stride*height that would wrap 32 bits refused, not wrapped");
}

static void testLastAcceptingValues()
{
    IgPlaneRegs r;

    poison(&r);
    checkTrue(igPlaneBuildLinear(0u, kFmt, kSurf, kW, kH, kStride, &r)
              == IG_PLANE_OK,
          "stride exactly equal to the row is accepted");

    // 16 px * 4 B = 64 B row, 64 rows -> 4096 B ending exactly at 2^32.
    poison(&r);
    checkTrue(igPlaneBuildLinear(0u, kFmt, 0xFFFFF000u, 16u, 64u, 64u, &r)
              == IG_PLANE_OK,
          "surface ending on the last GGTT byte is accepted, not a wrap");

    poison(&r);
    checkTrue(igPlaneBuildLinear(0u, IG_FB_FORMAT_RGB_565, kSurf, 32u, 2u, 64u, &r)
              == IG_PLANE_OK && igFbFormat(r.ctl) == IG_FB_FORMAT_RGB_565,
          "2-byte format: 32 px fills one 64-byte unit exactly");
    poison(&r);
    checkTrue(igPlaneBuildLinear(0u, IG_FB_FORMAT_RGB_565, kSurf, 33u, 2u, 64u, &r)
              == IG_PLANE_BAD_STRIDE,
          "2-byte format: 33 px overflows one unit");
}

//  Doubly-bad inputs: each pair differs from a good input in exactly the two
//  named ways, and the EARLIER arm must win.
static void testRefusalOrder()
{
    checkTrue(igPlaneBuildLinear(0u, 0u, kSurf, kW, kH, kStride, 0)
              == IG_PLANE_NULL_OUT,
          "order: null out beats bad format");
    refuse(0u, 0u, kSurf, 0u, kH, kStride, IG_PLANE_BAD_FORMAT,
           "order: bad format beats bad geometry");
    refuse(0u, kFmt, kSurf, 0u, kH, kStride + 1u, IG_PLANE_BAD_GEOMETRY,
           "order: bad geometry beats bad stride");
    refuse(0u, kFmt, kSurf + 0x800u, kW, kH, kStride + 1u, IG_PLANE_BAD_STRIDE,
           "order: bad stride units beat bad surf alignment");
    refuse(0u, kFmt, kSurf + 0x800u, kW, kH, kStride - 64u, IG_PLANE_BAD_STRIDE,
           "order: short row beats bad surf alignment");
    refuse(0u, kFmt, 0xFFFFF800u, 16u, 65u, 64u, IG_PLANE_BAD_SURF_ALIGN,
           "order: bad surf alignment beats bad surf range");
}

//  The chain the 3050 path will actually run: plan a sink past the firmware
//  plane with IntelSinkAlloc, then hand that plan straight to the plane
//  builder. Each header passing its own suite does not prove they AGREE.
static void testAllocToPlaneChain()
{
    const unsigned long long fwEnd =
        (unsigned long long)IG_FB_SURF_OBSERVED_2026_08_16
        + (unsigned long long)kStride * kH;

    IgSinkAllocPlan plan;
    checkTrue(igSinkAllocPlan(0x100000000ull, fwEnd, kW, kH, 32u, &plan)
              == IG_SINK_ALLOC_OK,
          "chain: sink allocates past the firmware plane");
    checkTrue(plan.base >= fwEnd, "chain: sink does not overlap the firmware plane");

    IgPlaneRegs r;
    poison(&r);
    checkTrue(igPlaneBuildLinear(IG_FB_CTL_OBSERVED_2026_08_16, kFmt, plan.base,
                             kW, kH, plan.stride, &r)
              == IG_PLANE_OK,
          "chain: the allocator's plan is accepted by the plane builder");
    checkTrue(r.surf == plan.base, "chain: plane points at the planned base");
    checkTrue(r.stride * IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED == plan.stride,
          "chain: plane stride field matches the planned stride");
}

static void testVerdictTable()
{
    const IGFB32 v[] = {IG_PLANE_OK, IG_PLANE_BAD_FORMAT, IG_PLANE_BAD_GEOMETRY,
                        IG_PLANE_BAD_STRIDE, IG_PLANE_BAD_SURF_ALIGN,
                        IG_PLANE_BAD_SURF_RANGE, IG_PLANE_NULL_OUT};
    bool distinct = true, inRange = true;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; ++i) {
        if (v[i] > IG_PLANE_VERDICT_LIMIT)
            inRange = false;
        for (unsigned j = i + 1; j < sizeof v / sizeof v[0]; ++j)
            if (v[i] == v[j])
                distinct = false;
    }
    checkTrue(distinct, "verdicts are pairwise distinct");
    checkTrue(inRange, "no verdict exceeds IG_PLANE_VERDICT_LIMIT");
}

int main()
{
    testLinearStride();
    testStrideDecoderAsymmetry();
    testSize();
    testPos();
    testWriteOrder();
    testBuildFromFirmwareCtl();
    testBuildClearsOwnedBits();
    testRefusalArms();
    testLastAcceptingValues();
    testRefusalOrder();
    testAllocToPlaneChain();
    testVerdictTable();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
