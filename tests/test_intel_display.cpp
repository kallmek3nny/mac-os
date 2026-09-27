//
//  test_intel_display.cpp
//  Host-side suite for phase0-kext/IntelDisplay.hpp.
//
//  Compiles the IDENTICAL header the kext will use, under MSVC, on a machine
//  with no Intel GPU. Every function in that header is a total function of its
//  arguments, so there is nothing to simulate -- no fixture, no journal, no
//  register file. That is the whole benefit of the dependency-free split.
//
//  WHAT EACH SECTION IS ACTUALLY TRYING TO CATCH
//
//  1. Offsets are pinned to BARE LITERALS, never to the header's own macros.
//     A test that writes checkU32("A", igTransBase(IG_TRANSCODER_A),
//     IG_TRANS_A_OFFSET) passes for any value of that macro, including a wrong
//     one -- the same self-cancelling shape that made six container offsets in
//     NVGspStage.hpp undetectable by construction. So the expected side of
//     every address check below is a hand-typed hex number.
//
//  2. The DSI pair is checked SPECIFICALLY because it is what kills a stride.
//     A0x1000-stride implementation reproduces A..D exactly and then places
//     DSI1 at 0x6C000 and pipe DSI1 at 0x7C000. Both are real addresses in
//     other blocks, so the error would not fault. Two checks here separate
//     them by asserting the 0x800 delta directly.
//
//  3. TRANSCONF is checked against the value the WRONG function would produce.
//     igTransConfReg(A) must be 0x70008 and must NOT be 0x60008 -- and 0x60008
//     is asserted to be _TRANS_HSYNC_A, so the test states out loud what the
//     mistake would silently hit.
//
//  4. The +1 convention is checked with SEPARATING inputs. A word whose halves
//     are equal cannot tell hi from lo, and a word encoding a value of 1 cannot
//     tell "+1 applied" from "+1 omitted" (0 decodes to 1 either way in the
//     eyes of a careless reading). Every timing check uses distinct halves and
//     values above 1.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelDisplay.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i)
        std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkDec(const char *label, IGU32 got, IGU32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s %u\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got %u want %u\n", label, got, want);
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
// --- transcoder base table -------------------------------------------------
//
// Expected values are bare literals from intel_display_device.c:73-80, NOT the
// header's macros. If both sides came from the same #define the check would
// pass for any value.
//
static void testTransBase(void)
{
    section("trans_offsets[] -- the table, pinned to literals");

    checkU32("igTransBase(A)",    igTransBase(IG_TRANSCODER_A),    0x60000u);
    checkU32("igTransBase(B)",    igTransBase(IG_TRANSCODER_B),    0x61000u);
    checkU32("igTransBase(C)",    igTransBase(IG_TRANSCODER_C),    0x62000u);
    checkU32("igTransBase(D)",    igTransBase(IG_TRANSCODER_D),    0x63000u);
    checkU32("igTransBase(DSI0)", igTransBase(IG_TRANSCODER_DSI0), 0x6B000u);
    checkU32("igTransBase(DSI1)", igTransBase(IG_TRANSCODER_DSI1), 0x6B800u);

    //
    // The stride killer. A 0x1000-stride implementation agrees with every check
    // above except the last, and puts DSI1 at 0x6C000 -- a real address in a
    // different block, so the bug reads and writes without faulting.
    //
    checkU32("DSI1 - DSI0 is 0x800, not 0x1000",
             igTransBase(IG_TRANSCODER_DSI1) - igTransBase(IG_TRANSCODER_DSI0),
             0x800u);
    checkBool("igTransBase(DSI1) is NOT the strided 0x6C000",
              igTransBase(IG_TRANSCODER_DSI1) == 0x6C000u, false);

    //
    // DSI0 is 0x8000 above D, not 0x1000. Stated separately because a stride
    // formula gets D right and DSI0 wrong, and a table gets both right.
    //
    checkU32("DSI0 - D is 0x8000",
             igTransBase(IG_TRANSCODER_DSI0) - igTransBase(IG_TRANSCODER_D),
             0x8000u);

    //
    // Gen12 dropped TRANSCODER_EDP: intel_display_device.c:78 defines
    // TRANSCODER_EDP_OFFSET 0x6f000 and XE_D_DISPLAY's trans_offsets[]
    // (881-888) never references it. No index in this header may produce it,
    // which is why 0x6F000 read as unclaimed in the 2026-08-15 18:38 capture.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t)
        checkBool("no transcoder index yields the eDP block 0x6F000",
                  igTransBase(t) == 0x6F000u, false);

    //
    // Out-of-domain must be refused, not wrapped or clamped. COUNT itself is
    // the first invalid value, which is the off-by-one a <= bound would let
    // through.
    //
    checkU32("igTransBase(COUNT) refused",
             igTransBase(IG_TRANSCODER_COUNT), IG_MMIO_INVALID);
    checkU32("igTransBase(0xFFFFFFFF) refused",
             igTransBase(0xFFFFFFFFu), IG_MMIO_INVALID);
}

//
// --- pipe base table -------------------------------------------------------
//
static void testPipeBase(void)
{
    section("pipe_offsets[] -- a DIFFERENT table, same domain");

    checkU32("igPipeBase(A)",    igPipeBase(IG_TRANSCODER_A),    0x70000u);
    checkU32("igPipeBase(B)",    igPipeBase(IG_TRANSCODER_B),    0x71000u);
    checkU32("igPipeBase(C)",    igPipeBase(IG_TRANSCODER_C),    0x72000u);
    checkU32("igPipeBase(D)",    igPipeBase(IG_TRANSCODER_D),    0x73000u);
    checkU32("igPipeBase(DSI0)", igPipeBase(IG_TRANSCODER_DSI0), 0x7B000u);
    checkU32("igPipeBase(DSI1)", igPipeBase(IG_TRANSCODER_DSI1), 0x7B800u);

    checkU32("pipe DSI1 - pipe DSI0 is 0x800",
             igPipeBase(IG_TRANSCODER_DSI1) - igPipeBase(IG_TRANSCODER_DSI0),
             0x800u);

    //
    // intel_display_device.c:67 defines PIPE_EDP_OFFSET 0x7f000, and its own
    // comment says there is actually no pipe EDP. Nothing here may produce it.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t)
        checkBool("no index yields PIPE_EDP_OFFSET 0x7F000",
                  igPipeBase(t) == 0x7F000u, false);

    //
    // CHV_PIPE_C_OFFSET (line 60) is 0x74000 -- a Cherryview constant. Pinned
    // because 0x74000 is what pipe D + 0x1000 would be, so a table that ran
    // one entry long would land on another platform's pipe C.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t)
        checkBool("no index yields CHV_PIPE_C_OFFSET 0x74000",
                  igPipeBase(t) == 0x74000u, false);

    checkU32("igPipeBase(COUNT) refused",
             igPipeBase(IG_TRANSCODER_COUNT), IG_MMIO_INVALID);

    //
    // The two tables must never coincide for the same index. If a later edit
    // collapsed them into one function, this is what would go red.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t)
        checkBool("trans base and pipe base differ for this index",
                  igTransBase(t) == igPipeBase(t), false);
}

//
// --- hazard 2: TRANSCONF ---------------------------------------------------
//
static void testTransConf(void)
{
    section("TRANSCONF -- transcoder-indexed, pipe-block-addressed");

    checkU32("igTransConfReg(A)", igTransConfReg(IG_TRANSCODER_A), 0x70008u);
    checkU32("igTransConfReg(B)", igTransConfReg(IG_TRANSCODER_B), 0x71008u);
    checkU32("igTransConfReg(C)", igTransConfReg(IG_TRANSCODER_C), 0x72008u);
    checkU32("igTransConfReg(D)", igTransConfReg(IG_TRANSCODER_D), 0x73008u);

    //
    // The whole hazard, said out loud. A combined translate(base, offset)
    // helper handed the TRANSCODER base produces 0x60008, which is
    // _TRANS_HSYNC_A (i915_reg.h:1083) -- a real register. The failure mode is
    // a silently corrupted horizontal sync, not a fault.
    //
    checkBool("igTransConfReg(A) is NOT 0x60008 (_TRANS_HSYNC_A)",
              igTransConfReg(IG_TRANSCODER_A) == 0x60008u, false);
    checkU32("0x60008 is what the transcoder table would give",
             igTransBase(IG_TRANSCODER_A) + IG_PIPE_TRANSCONF, 0x60008u);
    checkU32("and 0x60008 is HSYNC, a real register",
             igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_HSYNC), 0x60008u);

    //
    // The reverse direction of the same hazard: the timing helper must be
    // incapable of producing a TRANSCONF address for any input.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t)
        checkBool("igTransTimingReg cannot reach any pipe block",
                  igTransTimingReg(t, IG_PIPE_TRANSCONF) == igTransConfReg(t),
                  false);

    checkU32("igTransConfReg(COUNT) refused",
             igTransConfReg(IG_TRANSCODER_COUNT), IG_MMIO_INVALID);
}

//
// --- transcoder timing addresses ------------------------------------------
//
static void testTimingReg(void)
{
    section("transcoder timing addresses -- literals from i915_reg.h");

    checkU32("HTOTAL  A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_HTOTAL),  0x60000u);
    checkU32("HBLANK  A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_HBLANK),  0x60004u);
    checkU32("HSYNC   A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_HSYNC),   0x60008u);
    checkU32("VTOTAL  A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_VTOTAL),  0x6000Cu);
    checkU32("VBLANK  A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_VBLANK),  0x60010u);
    checkU32("VSYNC   A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_VSYNC),   0x60014u);
    checkU32("PIPESRC A", igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_PIPESRC), 0x6001Cu);

    //
    // i915_reg.h:1114 _TRANS_HTOTAL_B 0x61000 and :1120 _PIPEBSRC 0x6101c.
    // The second is the one worth pinning: despite the _PIPEB name, PIPESRC
    // for transcoder B is in the TRANSCODER block at 0x6101C, not 0x7101C.
    //
    checkU32("HTOTAL  B (i915_reg.h:1114)",
             igTransTimingReg(IG_TRANSCODER_B, IG_TRANS_HTOTAL), 0x61000u);
    checkU32("PIPESRC B is 0x6101C (i915_reg.h:1120), not 0x7101C",
             igTransTimingReg(IG_TRANSCODER_B, IG_TRANS_PIPESRC), 0x6101Cu);
    checkBool("PIPESRC B is NOT in the pipe block",
              igTransTimingReg(IG_TRANSCODER_B, IG_TRANS_PIPESRC) == 0x7101Cu,
              false);

    checkU32("PIPESRC DSI1 (table, not stride)",
             igTransTimingReg(IG_TRANSCODER_DSI1, IG_TRANS_PIPESRC), 0x6B81Cu);

    //
    // 0x18 sits between VSYNC (0x14) and PIPESRC (0x1C) and is not in the
    // enumerated list. base + 0x18 would be a valid address into a register
    // this project has not identified, so it must be refused rather than
    // computed. This is the check that catches a helper which adds any offset
    // it is handed.
    //
    checkU32("unenumerated field 0x18 refused",
             igTransTimingReg(IG_TRANSCODER_A, 0x18u), IG_MMIO_INVALID);
    checkU32("unenumerated field 0x20 refused",
             igTransTimingReg(IG_TRANSCODER_A, 0x20u), IG_MMIO_INVALID);
    checkU32("field 0x02 (misaligned) refused",
             igTransTimingReg(IG_TRANSCODER_A, 0x02u), IG_MMIO_INVALID);
    checkU32("bad transcoder refused even with a good field",
             igTransTimingReg(IG_TRANSCODER_COUNT, IG_TRANS_HTOTAL),
             IG_MMIO_INVALID);

    checkBool("IG_TRANS_HTOTAL 0x00 is an accepted field",
              igIsTransTimingField(IG_TRANS_HTOTAL), true);
    checkBool("0x18 is not an accepted field",
              igIsTransTimingField(0x18u), false);
    checkBool("TRANSCONF's 0x08 delta collides with HSYNC by value",
              igIsTransTimingField(IG_PIPE_TRANSCONF), true);

    //
    // Every address the two helpers can produce lands in the policy range, and
    // the sentinel does not. The second half matters more: a caller that range
    // -checks a refusal instead of comparing to IG_MMIO_INVALID would treat
    // 0xFFFFFFFF as out-of-range and might clamp it into a real offset.
    //
    for (IGU32 t = 0; t < IG_TRANSCODER_COUNT; ++t) {
        checkBool("timing address is in the policy range",
                  igIsInDisplayPolicyRange(
                      igTransTimingReg(t, IG_TRANS_HTOTAL)), true);
        checkBool("pipe DSI1 top of range is still inside",
                  igIsInDisplayPolicyRange(igPipeBase(t)), true);
    }
    checkBool("IG_MMIO_INVALID is outside the policy range",
              igIsInDisplayPolicyRange(IG_MMIO_INVALID), false);
    checkBool("0x5FFFF is below the range",
              igIsInDisplayPolicyRange(0x5FFFFu), false);
    checkBool("0x60000 is the first address in range",
              igIsInDisplayPolicyRange(0x60000u), true);
    checkBool("0x7BFFF is the last address in range",
              igIsInDisplayPolicyRange(0x7BFFFu), true);
    checkBool("0x7C000 is above the range",
              igIsInDisplayPolicyRange(0x7C000u), false);
}

//
// --- hazard 3: the value-1 convention -------------------------------------
//
static void testPlusOne(void)
{
    section("the value-1 convention -- separating inputs only");

    //
    // 1920x1080 inside 2200x1125, the mode i915 reported for card1-eDP-1 in the
    // 2026-08-15 capture. Encoded BY HAND here with the -1 convention so this
    // test does not depend on any dump: htotal-1 = 2199 = 0x897,
    // hdisplay-1 = 1919 = 0x77F.
    //
    // Halves are deliberately different values, so a swapped hi/lo cannot pass.
    //
    checkDec("HTOTAL 0x0897077F -> htotal 2200",   igTimingHi(0x0897077Fu), 2200u);
    checkDec("HTOTAL 0x0897077F -> hdisplay 1920", igTimingLo(0x0897077Fu), 1920u);
    checkDec("VTOTAL 0x04640437 -> vtotal 1125",   igTimingHi(0x04640437u), 1125u);
    checkDec("VTOTAL 0x04640437 -> vdisplay 1080", igTimingLo(0x04640437u), 1080u);

    //
    // Stated as an inequality as well as an equality: an implementation that
    // omits the +1 returns exactly these values, and a mode is still nearly
    // usable under that error -- which is why the off-by-one is the thing worth
    // asserting against rather than merely asserting the right answer.
    //
    checkBool("hi is not the raw 2199", igTimingHi(0x0897077Fu) == 2199u, false);
    checkBool("lo is not the raw 1919", igTimingLo(0x0897077Fu) == 1919u, false);

    //
    // PIPESRC under its own names. i915_reg.h:1105-1108 spells the halves
    // WIDTH (31:16) and HEIGHT (15:0); intel_display.c:2814-2815 writes
    // width - 1 and height - 1.
    //
    checkDec("PIPESRC 0x077F0437 -> width 1920",
             igPipesrcWidth(0x077F0437u), 1920u);
    checkDec("PIPESRC 0x077F0437 -> height 1080",
             igPipesrcHeight(0x077F0437u), 1080u);
    checkBool("PIPESRC is not 1919 wide",
              igPipesrcWidth(0x077F0437u) == 1919u, false);

    //
    // Width and height are asymmetric in this fixture (1920 != 1080), so a
    // swapped pair fails. A square fixture would pass either way.
    //
    checkBool("width and height are not interchangeable here",
              igPipesrcWidth(0x077F0437u) == igPipesrcHeight(0x077F0437u),
              false);

    //
    // The full-scale edges. 0xFFFF decodes to 65536, which does not fit in 16
    // bits -- the reason the return type is IGU32. A 16-bit implementation
    // wraps to 0 here.
    //
    checkDec("half 0xFFFF decodes to 65536", igTimingLo(0x0000FFFFu), 65536u);
    checkDec("hi half 0xFFFF decodes to 65536", igTimingHi(0xFFFF0000u), 65536u);
    checkDec("a zero word decodes to 1, not 0", igTimingLo(0x00000000u), 1u);
    checkDec("a zero word's hi half decodes to 1", igTimingHi(0x00000000u), 1u);

    //
    // Each half must ignore the other entirely. 0xFFFF0000 and 0x0000FFFF are
    // the separating pair for a missing mask.
    //
    checkDec("lo ignores a saturated hi half", igTimingLo(0xFFFF0000u), 1u);
    checkDec("hi ignores a saturated lo half", igTimingHi(0x0000FFFFu), 1u);
}

//
// --- TRANSCONF bits --------------------------------------------------------
//
static void testTransconfBits(void)
{
    section("TRANSCONF_ENABLE(31) and TRANSCONF_STATE_ENABLE(30)");

    //
    // Adjacent bits, two different claims: ENABLE is what software asked for,
    // STATE is what the hardware is doing. Each is checked with the OTHER
    // CLEAR, which is what separates them -- a decoder that read bit 30 for
    // both would pass any test using only 0xC0000000.
    //
    checkBool("0x80000000 -> enabled",       igTransconfEnabled(0x80000000u), true);
    checkBool("0x80000000 -> state clear",   igTransconfStateEnabled(0x80000000u), false);
    checkBool("0x40000000 -> enable clear",  igTransconfEnabled(0x40000000u), false);
    checkBool("0x40000000 -> state set",     igTransconfStateEnabled(0x40000000u), true);
    checkBool("0xC0000000 -> both set",      igTransconfLive(0xC0000000u), true);
    checkBool("0x00000000 -> not live",      igTransconfLive(0x00000000u), false);
    checkBool("0x80000000 alone is not live", igTransconfLive(0x80000000u), false);
    checkBool("0x40000000 alone is not live", igTransconfLive(0x40000000u), false);

    //
    // Bit 29 is the near neighbour on the far side of STATE. Every adjacent-bit
    // check in test_falcon.cpp once probed the wrong side of its own mask, so
    // this asserts that widening either mask by one bit goes red.
    //
    checkBool("bit 29 does not read as enable",
              igTransconfEnabled(0x20000000u), false);
    checkBool("bit 29 does not read as state",
              igTransconfStateEnabled(0x20000000u), false);
    checkBool("0x3FFFFFFF (everything below 30) is not live",
              igTransconfLive(0x3FFFFFFFu), false);
    checkBool("0xFFFFFFFF is live",
              igTransconfLive(0xFFFFFFFFu), true);
}

//
// --- plausibility ----------------------------------------------------------
//
static void testPlausible(void)
{
    section("igTimingPlausible -- rejects zeros, does not validate modes");

    //
    // The failure this guards is not a fault. A powered-down transcoder returns
    // 0x00000000, which the +1 convention decodes into a perfectly well-formed
    // 1x1 mode. That is the input the function exists to reject.
    //
    checkBool("2200 total / 1920 active is plausible",
              igTimingPlausible(2200u, 1920u), true);
    checkBool("a zero register's 1/1 is rejected",
              igTimingPlausible(1u, 1u), false);
    checkBool("total == active is rejected",
              igTimingPlausible(1920u, 1920u), false);
    checkBool("total < active is rejected",
              igTimingPlausible(1920u, 2200u), false);
    checkBool("active of 1 is rejected even with a large total",
              igTimingPlausible(2200u, 1u), false);
    checkBool("active 2 / total 3 is the smallest accepted pair",
              igTimingPlausible(3u, 2u), true);
    checkBool("65536 total is accepted (full 16-bit encoding)",
              igTimingPlausible(65536u, 1080u), true);
    checkBool("65537 total is rejected -- unencodable",
              igTimingPlausible(65537u, 1080u), false);

    //
    // Said explicitly because the name invites over-reading: this is not a
    // validity test. A wrong-but-well-formed mode passes, and that is by
    // design. If a later reader expects it to catch a bad decode, this check
    // is the documentation.
    //
    checkBool("a WRONG but well-formed mode still passes -- by design",
              igTimingPlausible(2201u, 1921u), true);
}

//
// --- the composed read ----------------------------------------------------
//
// One end-to-end pass in the shape a driver would use it: pick a transcoder,
// read TRANSCONF, then read the timing registers. Every address here comes from
// the helpers rather than from a literal, so this section is the one place the
// header's own macros are trusted -- the sections above are what earn that.
//
static void testComposedRead(void)
{
    section("composed read -- transcoder A as the driver would do it");

    const IGU32 conf   = 0xC0000000u;
    const IGU32 htotal = 0x0897077Fu;
    const IGU32 vtotal = 0x04640437u;
    const IGU32 src    = 0x077F0437u;

    checkU32("TRANSCONF address", igTransConfReg(IG_TRANSCODER_A), 0x70008u);
    checkBool("transcoder A is live", igTransconfLive(conf), true);

    checkU32("HTOTAL address",
             igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_HTOTAL), 0x60000u);
    checkU32("VTOTAL address",
             igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_VTOTAL), 0x6000Cu);
    checkU32("PIPESRC address",
             igTransTimingReg(IG_TRANSCODER_A, IG_TRANS_PIPESRC), 0x6001Cu);

    checkDec("decoded hdisplay", igTimingLo(htotal), 1920u);
    checkDec("decoded vdisplay", igTimingLo(vtotal), 1080u);
    checkDec("decoded htotal",   igTimingHi(htotal), 2200u);
    checkDec("decoded vtotal",   igTimingHi(vtotal), 1125u);

    checkBool("H timing pair is plausible",
              igTimingPlausible(igTimingHi(htotal), igTimingLo(htotal)), true);
    checkBool("V timing pair is plausible",
              igTimingPlausible(igTimingHi(vtotal), igTimingLo(vtotal)), true);

    //
    // PIPESRC must agree with the active area the timing registers describe.
    // i915 programs them from the same mode, so a disagreement here means one
    // of the two decodes is wrong -- and this is the only cross-check in the
    // header that does not depend on a second source.
    //
    checkBool("PIPESRC width agrees with hdisplay",
              igPipesrcWidth(src) == igTimingLo(htotal), true);
    checkBool("PIPESRC height agrees with vdisplay",
              igPipesrcHeight(src) == igTimingLo(vtotal), true);

    //
    // A stale transcoder: TRANSCONF clear, timing registers zero. Both decodes
    // succeed and produce a 1x1 mode, and the plausibility test is the only
    // thing that says so.
    //
    checkBool("a dark transcoder is not live", igTransconfLive(0x00000000u), false);
    checkBool("its zeroed timing is implausible",
              igTimingPlausible(igTimingHi(0x00000000u),
                                igTimingLo(0x00000000u)), false);
}

//
// --- the plane layer ------------------------------------------------------
//
// Every expected value below is a BARE HAND-TYPED LITERAL transcribed from
// reference/i915/skl_universal_plane_regs.h. None of them is composed from the
// header's own macros -- per the `self-cancelling-constants-are-untestable`
// finding, a check whose expected side is the macro under test passes for any
// value of that macro, including a wrong one.
//
static void testPlaneAddress(void)
{
    section("plane addressing -- ARITHMETIC, unlike the transcoders");

    //
    // The four-corner quads. These are the entire proof that the stride is
    // 0x1000 per pipe and 0x100 per plane, and the reason arithmetic is right
    // here where it is wrong for the transcoders. Each row is the literal set
    // the vendored header spells out.
    //
    checkU32("CTL    (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_CTL), 0x70180u);
    checkU32("CTL    (A,1)", igPlaneReg(IG_TRANSCODER_A, 1u, IG_PLANE_CTL), 0x70280u);
    checkU32("CTL    (B,0)", igPlaneReg(IG_TRANSCODER_B, 0u, IG_PLANE_CTL), 0x71180u);
    checkU32("CTL    (B,1)", igPlaneReg(IG_TRANSCODER_B, 1u, IG_PLANE_CTL), 0x71280u);

    checkU32("STRIDE (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_STRIDE), 0x70188u);
    checkU32("STRIDE (A,1)", igPlaneReg(IG_TRANSCODER_A, 1u, IG_PLANE_STRIDE), 0x70288u);
    checkU32("STRIDE (B,0)", igPlaneReg(IG_TRANSCODER_B, 0u, IG_PLANE_STRIDE), 0x71188u);
    checkU32("STRIDE (B,1)", igPlaneReg(IG_TRANSCODER_B, 1u, IG_PLANE_STRIDE), 0x71288u);

    checkU32("POS    (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_POS), 0x7018Cu);
    checkU32("POS    (A,1)", igPlaneReg(IG_TRANSCODER_A, 1u, IG_PLANE_POS), 0x7028Cu);
    checkU32("POS    (B,0)", igPlaneReg(IG_TRANSCODER_B, 0u, IG_PLANE_POS), 0x7118Cu);
    checkU32("POS    (B,1)", igPlaneReg(IG_TRANSCODER_B, 1u, IG_PLANE_POS), 0x7128Cu);

    checkU32("SIZE   (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_SIZE), 0x70190u);
    checkU32("SIZE   (A,1)", igPlaneReg(IG_TRANSCODER_A, 1u, IG_PLANE_SIZE), 0x70290u);
    checkU32("SIZE   (B,0)", igPlaneReg(IG_TRANSCODER_B, 0u, IG_PLANE_SIZE), 0x71190u);
    checkU32("SIZE   (B,1)", igPlaneReg(IG_TRANSCODER_B, 1u, IG_PLANE_SIZE), 0x71290u);

    checkU32("KEYVAL (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_KEYVAL), 0x70194u);
    checkU32("KEYVAL (A,1)", igPlaneReg(IG_TRANSCODER_A, 1u, IG_PLANE_KEYVAL), 0x70294u);
    checkU32("KEYVAL (B,0)", igPlaneReg(IG_TRANSCODER_B, 0u, IG_PLANE_KEYVAL), 0x71194u);
    checkU32("KEYVAL (B,1)", igPlaneReg(IG_TRANSCODER_B, 1u, IG_PLANE_KEYVAL), 0x71294u);

    //
    // The remaining eight field offsets, each against the absolute address the
    // vendored header gives for plane 1 of pipe A. A single wrong entry in the
    // offset table lands on a real neighbouring register.
    //
    checkU32("KEYMSK   (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_KEYMSK), 0x70198u);
    checkU32("SURF     (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_SURF), 0x7019Cu);
    checkU32("KEYMAX   (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_KEYMAX), 0x701A0u);
    checkU32("OFFSET   (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_OFFSET), 0x701A4u);
    checkU32("SURFLIVE (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_SURFLIVE), 0x701ACu);
    checkU32("CC_VAL   (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_CC_VAL), 0x701B4u);
    checkU32("AUX_DIST (A,0)", igPlaneReg(IG_TRANSCODER_A, 0u, IG_PLANE_AUX_DIST), 0x701C0u);

    //
    // Pipes C and D, and the far corner of the whole block. A swapped pipe and
    // plane multiplier reproduces (A,0) exactly and disagrees everywhere else,
    // so these are the separating inputs for that mutation: with the strides
    // exchanged, (D,4) would be 0x70180 + 3*0x100 + 4*0x1000 = 0x74480 rather
    // than 0x735C0.
    //
    checkU32("CTL (C,0)", igPlaneReg(IG_TRANSCODER_C, 0u, IG_PLANE_CTL), 0x72180u);
    checkU32("CTL (D,0)", igPlaneReg(IG_TRANSCODER_D, 0u, IG_PLANE_CTL), 0x73180u);
    checkU32("CTL (A,4)", igPlaneReg(IG_TRANSCODER_A, 4u, IG_PLANE_CTL), 0x70580u);
    checkU32("AUX_DIST (D,4) -- far corner",
             igPlaneReg(IG_TRANSCODER_D, 4u, IG_PLANE_AUX_DIST), 0x735C0u);
}

//
// The refusals. Each of these is an address the arithmetic would happily
// compute into a REAL, WRITABLE part of the display block if the guard were
// removed -- which is why they are pinned individually rather than as a loop.
//
static void testPlaneRefusals(void)
{
    section("plane refusals -- the DSI pipes are off the 0x1000 ladder");

    //
    // HAZARD 4 said out loud. DSI0 is transcoder index 4, so unguarded plane
    // arithmetic gives 0x70180 + 4*0x1000 = 0x74180 -- not a plane register of
    // any pipe, but a real address inside the display block that reads and
    // writes without faulting. The DSI pipes live at 0x7B000 and 0x7B800,
    // which no multiple of 0x1000 from 0x70180 can reach.
    //
    checkU32("DSI0 is refused, not computed",
             igPlaneReg(IG_TRANSCODER_DSI0, 0u, IG_PLANE_CTL), IG_MMIO_INVALID);
    checkU32("DSI1 is refused, not computed",
             igPlaneReg(IG_TRANSCODER_DSI1, 0u, IG_PLANE_CTL), IG_MMIO_INVALID);
    checkBool("and the address it would have produced is NOT what came back",
              igPlaneReg(IG_TRANSCODER_DSI0, 0u, IG_PLANE_CTL) == 0x74180u, false);
    checkU32("a pipe index past DSI1 is refused",
             igPlaneReg(6u, 0u, IG_PLANE_CTL), IG_MMIO_INVALID);

    //
    // The policy ceiling. IG_PLANE_POLICY_COUNT is 5 because PLANE_5 is the
    // highest index the vendored set proves exists; whether Tiger Lake really
    // has 5 or 7 is unresolved (see the header). Plane 4 is the last accepted
    // one and plane 5 the first refused, so this pair pins the boundary rather
    // than a value on one side of it.
    //
    checkBool("plane 4 is accepted",
              igPlaneReg(IG_TRANSCODER_A, 4u, IG_PLANE_CTL) != IG_MMIO_INVALID, true);
    checkU32("plane 5 is refused -- policy ceiling, not a hardware fact",
             igPlaneReg(IG_TRANSCODER_A, 5u, IG_PLANE_CTL), IG_MMIO_INVALID);

    //
    // Field bounds. Both halves matter: past the limit, and misaligned within
    // it. A misaligned offset would otherwise produce an address that is not a
    // register boundary at all.
    //
    checkU32("a field past AUX_DIST is refused",
             igPlaneReg(IG_TRANSCODER_A, 0u, 0x44u), IG_MMIO_INVALID);
    checkU32("a misaligned field is refused",
             igPlaneReg(IG_TRANSCODER_A, 0u, 0x02u), IG_MMIO_INVALID);
    checkU32("a misaligned field inside the table is refused",
             igPlaneReg(IG_TRANSCODER_A, 0u, 0x0Du), IG_MMIO_INVALID);

    //
    // AND THE HONEST LIMIT OF THAT CHECK, pinned so nobody reads the three
    // refusals above as membership testing. The guard bounds the offset and its
    // alignment; it does not know which offsets the table actually contains.
    // 0x28 and 0x30 are gaps in the real block -- aligned, under the limit, and
    // accepted here. If that ever needs to be a real membership test, these two
    // checks are what will go red and say so.
    //
    checkU32("0x28 is a GAP but passes -- aligned and in range",
             igPlaneReg(IG_TRANSCODER_A, 0u, 0x28u), 0x701A8u);
    checkU32("0x30 is a GAP but passes -- aligned and in range",
             igPlaneReg(IG_TRANSCODER_A, 0u, 0x30u), 0x701B0u);
}

//
// The field decodes. Every fixture word below uses DISTINCT halves and
// non-trivial values on purpose: per the `an-assertion-that-cannot-fail-is-not
// -coverage` finding, equal halves cannot separate hi from lo, and a value of 1
// cannot separate "+1 applied" from "+1 omitted".
//
static void testPlaneFields(void)
{
    section("plane field decode -- distinct halves, so hi/lo cannot swap");

    checkBool("CTL bit 31 set is enabled", igPlaneEnabled(0x80000000u), true);
    checkBool("CTL bit 31 clear is disabled", igPlaneEnabled(0x7FFFFFFFu), false);

    //
    // Format, icl+ 5-bit at shift 23. XRGB_8888 is 4 under the SKL shift-24
    // numbering, which is 8 under the icl+ shift-23 numbering -- the extra low
    // bit is what widens the field.
    //
    checkU32("format XRGB_8888 decodes as 8 (icl+ numbering)",
             igPlaneFormat(0x04000000u), 8u);
    checkU32("format RGB_565 decodes as 28 (icl+ numbering)",
             igPlaneFormat(0x0E000000u), 28u);

    //
    // THE DISAGREEMENT, which is the reason both decodes exist in the header.
    // Y210 is format value 1 under the icl+ 5-bit mask, i.e. bit 23 set and
    // nothing above it. The pre-icl 4-bit decode at shift 24 cannot see bit 23
    // at all, so it reads that same word as format 0 -- YUV422, a real and
    // completely different format. It does not fail; it silently misreads.
    //
    checkU32("Y210 is 1 under the icl+ mask", igPlaneFormat(0x00800000u), 1u);
    checkU32("...and 0 under the pre-icl mask -- YUV422, a REAL format",
             igPlaneFormatSkl(0x00800000u), 0u);
    checkBool("so the two decodes disagree on a real Gen12 value",
              igPlaneFormat(0x00800000u) == igPlaneFormatSkl(0x00800000u), false);

    //
    // And they must AGREE wherever bit 23 is clear -- that is what the vendored
    // header's comment at :43-47 actually claims. Checking only the
    // disagreement would leave a broken icl+ decode looking correct.
    //
    checkBool("they agree on XRGB_8888 scaled by the extra bit",
              igPlaneFormat(0x04000000u) == igPlaneFormatSkl(0x04000000u) * 2u, true);

    //
    // STRIDE is 12 bits, GENMASK(11,0). The fixture sets bit 12 as well, so a
    // mask widened by one bit changes the answer.
    //
    checkU32("stride masks to 12 bits", igPlaneStride(0x00001ABCu), 0xABCu);
    checkU32("stride ignores bit 12 and above", igPlaneStride(0xFFFFF000u), 0x000u);

    //
    // POS: Y in 31:16, X in 15:0. Distinct halves, so a swapped decode fails.
    //
    checkU32("POS X is the low half", igPlanePosX(0x02580780u), 0x0780u);
    checkU32("POS Y is the high half", igPlanePosY(0x02580780u), 0x0258u);
    checkBool("and they are not the same value -- separating fixture",
              igPlanePosX(0x02580780u) == igPlanePosY(0x02580780u), false);

    //
    // HAZARD 5, NO LONGER HYPOTHETICAL. Until 2026-08-16 this section could only
    // assert the RAW fields, because the writer (skl_universal_plane.c) is not
    // vendored and the vendored intel_display.c never mentions PLANE_WIDTH /
    // PLANE_HEIGHT -- so the -1 convention was recorded as UNKNOWN and a decoded
    // wrapper would have been an invented value filed next to measured ones.
    //
    // It is now WITNESSED. IG_PLANE_SIZE_OBSERVED_2026_08_16 (IntelDisplay.hpp) was
    // read at 0x70190 on Tiger Lake 8086:9a68 while the eDP panel was lit at
    // 1920x1080: 0x0437077F, i.e. 1079 and 1919. A panel actually displaying
    // 1080 rows whose height field reads 1079 settles the convention -- the
    // register stores value-1 on BOTH halves, exactly as the timing registers do.
    //
    // The Raw pair stays, and stays asserted against the raw field, because it
    // is what the decoded pair is built out of. What changed is that the two
    // "did NOT get +1" checks are no longer the point: they now guard the RAW
    // accessors specifically, so a +1 migrating into the wrong layer is caught.
    //
    checkU32("SIZE width is raw, low half",
             igPlaneSizeWidthRaw(0x0437077Fu), 0x077Fu);
    checkU32("SIZE height is raw, high half",
             igPlaneSizeHeightRaw(0x0437077Fu), 0x0437u);
    checkBool("the RAW width still carries no +1 -- 1919, not 1920",
              igPlaneSizeWidthRaw(0x0437077Fu) == 1919u, true);
    checkBool("the RAW height still carries no +1 -- 1079, not 1080",
              igPlaneSizeHeightRaw(0x0437077Fu) == 1079u, true);

    //
    // The header's observed constant is pinned against a BARE LITERAL here, not
    // against itself. A test that fed the define into the accessor and compared
    // the answer to something else derived from the same define would pass under
    // any value at all -- see the tree's own note on self-cancelling constants.
    // This one row is what makes the define falsifiable: retype a nibble in
    // IntelDisplay.hpp and this check, alone, goes red naming the constant.
    //
    checkU32("the header's observed PLANE_SIZE word is the one that was captured",
             IG_PLANE_SIZE_OBSERVED_2026_08_16, 0x0437077Fu);

    //
    // The decoded pair, against the MEASURED word and the mode the panel was
    // actually lit at. These are the two checks the capture bought. They are
    // driven THROUGH the header constant rather than a repeated literal, so the
    // constant is load-bearing rather than a name that merely exists.
    //
    checkU32("decoded SIZE width is 1920 on the observed word",
             igPlaneSizeWidth(IG_PLANE_SIZE_OBSERVED_2026_08_16), 1920u);
    checkU32("decoded SIZE height is 1080 on the observed word",
             igPlaneSizeHeight(IG_PLANE_SIZE_OBSERVED_2026_08_16), 1080u);

    //
    // And the decoded pair is exactly raw + 1, on both halves. Asserting only
    // the 1920/1080 magnitudes above would survive a wrapper that hardcoded
    // them; asserting only this relation would survive a wrapper reading the
    // wrong half. Both together pin value and provenance.
    //
    checkBool("decoded width is raw width + 1",
              igPlaneSizeWidth(0x0437077Fu)
                  == igPlaneSizeWidthRaw(0x0437077Fu) + 1u, true);
    checkBool("decoded height is raw height + 1",
              igPlaneSizeHeight(0x0437077Fu)
                  == igPlaneSizeHeightRaw(0x0437077Fu) + 1u, true);

    //
    // THE FOLD HAZARD, which the +1 becoming shared makes WORSE rather than
    // better. PLANE_SIZE and PIPESRC now agree on the arithmetic and disagree
    // only on which half is which:
    //
    //     PIPESRC    0x077F0437   width 31:16, height 15:0
    //     PLANE_SIZE 0x0437077F   height 31:16, width 15:0
    //
    // Both were read in the same capture on the same lit panel, and both decode
    // to 1920x1080 -- from DIFFERENT words. That is the separating pair: a
    // folded implementation that used one decoder for both registers would
    // return 1080 where 1920 belongs and never fault.
    //
    checkU32("PIPESRC's own observed word decodes width 1920",
             igPipesrcWidth(0x077F0437u), 1920u);
    checkU32("PIPESRC's own observed word decodes height 1080",
             igPipesrcHeight(0x077F0437u), 1080u);
    checkBool("the two registers hold DIFFERENT words for the same mode",
              0x0437077Fu == 0x077F0437u, false);

    //
    // The negative controls: each decoder fed the OTHER register's word. These
    // are the checks that go red if the pairs are ever folded, and they are
    // asserted to the wrong-but-plausible 1080 rather than to "not 1920", so a
    // decode that broke in some third way does not pass them by accident.
    //
    checkU32("PIPESRC width on a PLANE_SIZE word reads 1080, not 1920",
             igPipesrcWidth(0x0437077Fu), 1080u);
    checkU32("SIZE width on a PIPESRC word reads 1080, not 1920",
             igPlaneSizeWidth(0x077F0437u), 1080u);

    //
    // Why the return type is IGU32 and not a 16-bit type: an all-ones field
    // decodes to 65536, which does not fit. A 16-bit return would wrap it to 0
    // and a full-screen plane on a hypothetical 65536-wide surface would read as
    // unconfigured. One check per half, because a wrapper reading the wrong half
    // would pass the other one.
    //
    checkU32("an all-ones width half decodes to 65536, not 0",
             igPlaneSizeWidth(0x0000FFFFu), 65536u);
    checkU32("an all-ones height half decodes to 65536, not 0",
             igPlaneSizeHeight(0xFFFF0000u), 65536u);

    //
    // The zero word: a plane that is off reads 0x00000000, and the -1 convention
    // makes that decode to 1x1 rather than 0x0. Recorded because it is a real
    // false-positive source -- 1x1 is a legal-looking size, so a caller must use
    // PLANE_CTL's enable bit to tell an off plane from a tiny one, exactly as the
    // transcoder path uses TRANSCONF. The decode itself is not wrong; treating a
    // nonzero return as "configured" is.
    //
    checkU32("a disabled plane's zero word decodes to width 1, not 0",
             igPlaneSizeWidth(0x00000000u), 1u);
    checkU32("a disabled plane's zero word decodes to height 1, not 0",
             igPlaneSizeHeight(0x00000000u), 1u);

    //
    // Contrast with the timing path. The +1 there was already proven from source
    // (intel_display.c:2814 writes width - 1); the plane path now rests on a
    // measurement instead. They agree on the low half BY ARITHMETIC, which is
    // why the check above on field ORDER is what actually separates them -- this
    // pair only records that the raw accessor is one below both of them.
    //
    checkU32("the TIMING decode of the same half also gives 1920",
             igTimingLo(0x0437077Fu), 1920u);
    checkBool("so timing and the RAW plane accessor differ by exactly 1",
              igTimingLo(0x0437077Fu) - igPlaneSizeWidthRaw(0x0437077Fu) == 1u, true);
}

//
// --- whole-mode consistency ------------------------------------------------
//
// The axis KERNEL, driven with decoded numbers directly. Every expected value
// is a bare literal, and every argument is a bare literal too -- deliberately
// NOT run through igTimingHi()/igTimingLo(), because this function contains no
// +1 arithmetic and a test that fed it decoded-by-helper numbers would also be
// a test of the helpers. If both were wrong the failure would be attributable
// to neither.
//
// The mode used throughout is 1280x800 @ 1680x831, NOT the captured panel's
// 1920x1080. Two reasons. It is a second independent shape, so a constant that
// happens to work for one raster is not silently ratified; and width != height
// by a different ratio, which is what makes the PIPESRC transpose arm below
// separate a correct implementation from one comparing against the wrong axis.
//
static void testAxisConsistent(void)
{
    section("igAxisConsistent -- one axis, decoded numbers");

    //  total, active, blankStart, blankEnd, syncStart, syncEnd
    checkU32("a real H axis is OK",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1344u, 1480u), 0u);

    checkU32("total == active is IMPLAUSIBLE, not a mode failure",
             igAxisConsistent(1680u, 1680u, 1680u, 1680u, 1680u, 1680u), 1u);
    checkU32("an all-zero register block decodes to 1/1 and is IMPLAUSIBLE",
             igAxisConsistent(1u, 1u, 1u, 1u, 1u, 1u), 1u);

    checkU32("blankEnd == blankStart is BLANK_RANGE",
             igAxisConsistent(1680u, 1280u, 1680u, 1680u, 1344u, 1480u), 2u);
    checkU32("blankEnd one past total is BLANK_RANGE",
             igAxisConsistent(1680u, 1280u, 1280u, 1681u, 1344u, 1480u), 2u);
    checkU32("blankEnd exactly at total is accepted",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1344u, 1480u), 0u);

    //
    //  THE >= BOUNDARY. i915 programs HBLANK_START as hactive - 1, so after the
    //  +1 the decoded blankStart is EQUAL to active on every normal mode. A >
    //  bound here would reject all real hardware, and the tell would be a check
    //  that fires on the first live read -- the kind that gets deleted rather
    //  than understood. Both sides of the edge are pinned.
    //
    checkU32("blankStart == active is the normal case, OK",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1344u, 1480u), 0u);
    checkU32("blankStart one BELOW active is BLANK_OVERLAP",
             igAxisConsistent(1680u, 1280u, 1279u, 1680u, 1344u, 1480u), 3u);

    checkU32("syncEnd == syncStart is SYNC_RANGE",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1344u, 1344u), 4u);
    checkU32("syncEnd past total is SYNC_RANGE",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1344u, 1681u), 4u);

    checkU32("sync starting before blanking is SYNC_OUTSIDE",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1200u, 1480u), 5u);
    checkU32("sync ending after blanking is SYNC_OUTSIDE",
             igAxisConsistent(1680u, 1280u, 1280u, 1600u, 1344u, 1650u), 5u);
    checkU32("sync exactly filling the blanking interval is OK",
             igAxisConsistent(1680u, 1280u, 1280u, 1680u, 1280u, 1680u), 0u);

    //
    //  PRECEDENCE. An input bad on two counts must report the EARLIER one, or
    //  the ordering comment in the header is decoration. total < active makes
    //  every later comparison meaningless, so IMPLAUSIBLE has to win over a
    //  blanking range that is also broken.
    //
    checkU32("implausible beats a broken blanking range",
             igAxisConsistent(1000u, 1280u, 1680u, 1280u, 1344u, 1480u), 1u);
    checkU32("blank range beats blank overlap",
             igAxisConsistent(1680u, 1280u, 1200u, 1200u, 1344u, 1480u), 2u);
    checkU32("blank overlap beats a broken sync range",
             igAxisConsistent(1680u, 1280u, 1200u, 1680u, 1344u, 1344u), 3u);
    checkU32("sync range beats sync containment",
             igAxisConsistent(1680u, 1280u, 1280u, 1600u, 1200u, 1200u), 4u);

    //
    //  The verdict codes themselves, pinned to bare literals. These are the
    //  values a caller switches on; renumbering them silently reassigns every
    //  refusal reason in the kext.
    //
    checkU32("IG_MODE_OK",            IG_MODE_OK,            0u);
    checkU32("IG_MODE_IMPLAUSIBLE",   IG_MODE_IMPLAUSIBLE,   1u);
    checkU32("IG_MODE_BLANK_RANGE",   IG_MODE_BLANK_RANGE,   2u);
    checkU32("IG_MODE_BLANK_OVERLAP", IG_MODE_BLANK_OVERLAP, 3u);
    checkU32("IG_MODE_SYNC_RANGE",    IG_MODE_SYNC_RANGE,    4u);
    checkU32("IG_MODE_SYNC_OUTSIDE",  IG_MODE_SYNC_OUTSIDE,  5u);
    checkU32("IG_MODE_SRC_MISMATCH",  IG_MODE_SRC_MISMATCH,  6u);
    checkU32("IG_MODE_VERDICT_LIMIT covers the largest verdict",
             IG_MODE_VERDICT_LIMIT, 6u);
}

//
// The caller-facing wrapper, driven from RAW register words. Every word below
// is spelled as a hex literal with the -1 already applied by hand, so the +1
// convention is exercised end to end rather than assumed:
//
//   HTOTAL  hi=1680-1=0x68F  lo=1280-1=0x4FF  -> 0x068F04FF
//   HBLANK  hi=1680-1=0x68F  lo=1280-1=0x4FF  -> 0x068F04FF
//   HSYNC   hi=1480-1=0x5C7  lo=1344-1=0x53F  -> 0x05C7053F
//   VTOTAL  hi= 831-1=0x33E  lo= 800-1=0x31F  -> 0x033E031F
//   VBLANK  hi= 831-1=0x33E  lo= 800-1=0x31F  -> 0x033E031F
//   VSYNC   hi= 806-1=0x325  lo= 803-1=0x322  -> 0x03250322
//   PIPESRC hi=1280-1=0x4FF  lo= 800-1=0x31F  -> 0x04FF031F
//
static void testModeConsistent(void)
{
    section("igModeConsistent -- both axes plus PIPESRC, from raw words");

    const IGU32 hTot = 0x068F04FFu;
    const IGU32 hBlk = 0x068F04FFu;
    const IGU32 hSyn = 0x05C7053Fu;
    const IGU32 vTot = 0x033E031Fu;
    const IGU32 vBlk = 0x033E031Fu;
    const IGU32 vSyn = 0x03250322u;
    const IGU32 src  = 0x04FF031Fu;

    checkU32("a whole 1280x800 raster is OK",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, vSyn, src), 0u);

    //  The words really do decode to the mode the comment claims.
    checkU32("hTotal decodes to 1680", igTimingHi(hTot), 1680u);
    checkU32("hActive decodes to 1280", igTimingLo(hTot), 1280u);
    checkU32("vTotal decodes to 831",  igTimingHi(vTot), 831u);
    checkU32("vActive decodes to 800", igTimingLo(vTot), 800u);

    //
    //  AN ALL-ZERO BLOCK. Every unused display page on this silicon reads
    //  0x00000000 rather than all-ones (2026-08-16 capture, READING NOTES),
    //  so this is the input an unprogrammed transcoder actually produces. The
    //  +1 convention turns it into a well-formed 1x1 mode, which is exactly
    //  why a refusal is needed rather than a fault.
    //
    checkU32("an unprogrammed transcoder is IMPLAUSIBLE, not OK",
             igModeConsistent(0u, 0u, 0u, 0u, 0u, 0u, 0u), 1u);

    //  H is checked before V: break both, expect the H verdict.
    checkU32("H is evaluated before V",
             igModeConsistent(hTot, 0x068F068Fu, hSyn,
                              vTot, 0x033E033Eu, vSyn, src), 2u);

    //  V alone breaking still reddens, so the V arm is not dead.
    checkU32("a broken V blanking range is caught",
             igModeConsistent(hTot, hBlk, hSyn, vTot, 0x033E033Eu, vSyn, src), 2u);
    checkU32("a V sync outside V blanking is caught",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, 0x03250100u, src), 5u);

    //
    //  PIPESRC, AND THE TRANSPOSE. src's halves are hi=width, lo=height -- the
    //  INVERSE of PLANE_SIZE. Catching a swap requires width != height, which is
    //  why the raster here is 1280x800 rather than something square. The
    //  captured 1920x1080 panel would separate a transpose too, but only
    //  barely: it is the one shape live silicon was seen holding, so anchoring
    //  the arm on it would make this suite's only transpose evidence depend on
    //  a single boot that cannot be repeated. A second, independent raster is
    //  what keeps the check from ratifying that one capture.
    //
    checkU32("PIPESRC with its halves transposed is SRC_MISMATCH",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, vSyn, 0x031F04FFu), 6u);
    checkU32("PIPESRC width one short is SRC_MISMATCH",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, vSyn, 0x04FE031Fu), 6u);
    checkU32("PIPESRC height one short is SRC_MISMATCH",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, vSyn, 0x04FF031Eu), 6u);
    checkU32("a zero PIPESRC under a valid raster is SRC_MISMATCH",
             igModeConsistent(hTot, hBlk, hSyn, vTot, vBlk, vSyn, 0u), 6u);

    //
    //  PIPESRC is checked LAST. Break the raster and PIPESRC together and the
    //  raster verdict must win, or a caller cannot tell a bad mode from a bad
    //  PIPESRC.
    //
    checkU32("an axis verdict beats SRC_MISMATCH",
             igModeConsistent(hTot, 0x068F068Fu, hSyn, vTot, vBlk, vSyn, 0u), 2u);

    //
    //  THE CAPTURED PANEL, for the record. 1920x1080 @ 2200x1125 with sync
    //  where a 60 Hz CVT mode puts it. This is the one shape live silicon was
    //  seen holding; it is here as a second raster, not as the separating
    //  input, for the reason in the comment above.
    //
    //  HTOTAL  hi=2200-1=0x0897  lo=1920-1=0x077F  -> 0x0897077F
    //  HSYNC   hi=2052-1=0x0803  lo=2008-1=0x07D7  -> 0x080307D7
    //  VTOTAL  hi=1125-1=0x0464  lo=1080-1=0x0437  -> 0x04640437
    //  VSYNC   hi=1089-1=0x0440  lo=1084-1=0x043B  -> 0x0440043B
    //  PIPESRC hi=1920-1=0x077F  lo=1080-1=0x0437  -> 0x077F0437
    //
    //  That last word is the capture's LITERAL PIPESRC value, and its halves
    //  are the byte-swap of the same boot's PLANE_SIZE 0x0437077F. The two are
    //  quoted here in the one place they can be compared, because the whole
    //  reason igModeConsistent() must not reuse a PLANE_SIZE decoder is that
    //  the swap is invisible on this panel's own numbers -- both describe
    //  1920x1080, so a transposing decoder programs cleanly.
    //
    checkU32("the captured 1920x1080 panel geometry is OK",
             igModeConsistent(0x0897077Fu, 0x0897077Fu, 0x080307D7u,
                              0x04640437u, 0x04640437u, 0x0440043Bu,
                              0x077F0437u), 0u);
    checkU32("and its PIPESRC half order matches the capture",
             igTimingHi(0x077F0437u), 1920u);
    checkU32("with the low half the panel height",
             igTimingLo(0x077F0437u), 1080u);
    checkU32("the same boot's PLANE_SIZE is the byte-swap of it",
             igTimingHi(0x0437077Fu), 1080u);
}

//
// --- TRANS_DDI_FUNC_CTL ----------------------------------------------------
//
// Two things get pinned here that no single input can separate on its own.
//
// THE SHIFT. Gen12 puts the port field at 27 with a 4-bit mask; everything
// before icl put it at 28 with 3 bits. The pre-icl reading of any word is the
// TGL reading shifted down one, so both produce a VALID port number and the
// wrong one programs a panel that is not the lit one. Separating them needs a
// word whose bit 27 is SET -- with bit 27 clear the two readings differ only in
// a bit neither mask reaches.
//
// THE +1. i915_reg.h:3757 stores port + 1, so a stored 0 means NO PORT and a
// stored 1 means port A. A decoder that returned the raw field would report
// port A for every transcoder that was never programmed, and on this silicon
// an unprogrammed page reads 0x00000000 -- so that mistake is not hypothetical,
// it is what the common case would produce. igDdiPortField() exists precisely
// so the raw read can be pinned separately from the subtraction; checking only
// igDdiPort() would let a shift error and an off-by-one cancel.
//
static void testTransDdi(void)
{
    section("igTransDdiReg -- +0x400 off every transcoder base");

    checkU32("transcoder A", igTransDdiReg(0u), 0x60400u);
    checkU32("transcoder B", igTransDdiReg(1u), 0x61400u);
    checkU32("transcoder C", igTransDdiReg(2u), 0x62400u);
    checkU32("transcoder D", igTransDdiReg(3u), 0x63400u);

    //
    //  THE DSI PAIR, which is where a stride formula would break. DSI1 sits
    //  0x800 above DSI0, not 0x1000, so igTransBase() is a switch -- and the
    //  0x400 displacement composes with it rather than replacing it. 0x6B800 +
    //  0x400 == 0x6BC00, matching i915_reg.h:3747 exactly.
    //
    checkU32("transcoder DSI0", igTransDdiReg(4u), 0x6B400u);
    checkU32("transcoder DSI1", igTransDdiReg(5u), 0x6BC00u);

    checkU32("an out-of-range transcoder is refused",
             igTransDdiReg(6u), 0xFFFFFFFFu);
    checkU32("and so is a wildly out-of-range one",
             igTransDdiReg(0xFFFFFFFFu), 0xFFFFFFFFu);

    //
    //  The displacement is the SAME for all six. Asserted as a difference so a
    //  single wrong base cannot be absorbed by a matching wrong offset.
    //
    checkBool("the displacement is 0x400 on A",
              igTransDdiReg(0u) - igTransBase(0u) == 0x400u, true);
    checkBool("and 0x400 on DSI1 too",
              igTransDdiReg(5u) - igTransBase(5u) == 0x400u, true);

    section("igDdiEnabled / igDdiPort / igDdiMode");

    checkBool("bit 31 set is enabled",  igDdiEnabled(0x80000000u), true);
    checkBool("bit 31 clear is not",    igDdiEnabled(0x7FFFFFFFu), false);
    checkBool("a zero word is not enabled", igDdiEnabled(0u), false);

    //
    //  THE SEPARATING SHIFT INPUT. Bit 27 set, bits 30:28 clear. The TGL field
    //  is 1 (port A after the -1); the pre-icl reading of the same word is 0,
    //  which under the +1 convention means NO PORT. So the two generations do
    //  not merely disagree on which port -- they disagree on whether a port is
    //  selected at all, which is the sharpest possible statement of the hazard.
    //
    checkU32("tgl reading of bit 27 alone is field 1",
             igDdiPortField(0x08000000u), 1u);
    checkU32("which decodes to port 0", igDdiPort(0x08000000u), 0u);
    checkU32("the pre-icl reading of that word would be 0",
             (0x08000000u >> 28) & 0x07u, 0u);

    //
    //  A ZERO FIELD IS NOT PORT 0. This is the arm that would go red if the -1
    //  were dropped or applied without the guard.
    //
    checkU32("a zero port field is NONE, not port 0",
             igDdiPort(0x80000000u), 0xFFFFFFFFu);
    checkU32("IG_DDI_PORT_NONE", IG_DDI_PORT_NONE, 0xFFFFFFFFu);
    checkU32("an all-zero word reports NONE", igDdiPort(0u), 0xFFFFFFFFu);

    //  Every port the 4-bit field can name, at both ends.
    checkU32("field 2 is port 1",  igDdiPort(0x10000000u), 1u);
    checkU32("field 15 is port 14", igDdiPort(0x78000000u), 14u);
    checkU32("and the field itself reads 15",
             igDdiPortField(0x78000000u), 15u);

    //
    //  FAR-SIDE BIT PROBES. Bit 31 sits immediately above the port field, so a
    //  mask widened from 0xF to 0x1F would swallow it; bit 26 sits immediately
    //  below, so a shift of 26 would pick it up. Both are probed with the
    //  neighbouring bit SET and the field otherwise zero -- per
    //  cross-checks-probe-the-far-side-bit, checking the near side proves
    //  nothing about a mask that grew in the other direction.
    //
    checkU32("bit 31 is not part of the port field",
             igDdiPortField(0x80000000u), 0u);
    checkU32("bit 26 is not either",
             igDdiPortField(0x04000000u), 0u);
    checkU32("so an enabled transcoder with no port still reports NONE",
             igDdiPort(0x84000000u), 0xFFFFFFFFu);

    //  MODE SELECT. Not +1 encoded, so 0 is HDMI -- a real mode.
    checkU32("mode HDMI",           igDdiMode(0x00000000u), 0u);
    checkU32("mode DVI",            igDdiMode(0x01000000u), 1u);
    checkU32("mode DP_SST",         igDdiMode(0x02000000u), 2u);
    checkU32("mode DP_MST",         igDdiMode(0x03000000u), 3u);
    checkU32("mode FDI_OR_128B132B", igDdiMode(0x04000000u), 4u);

    checkU32("IG_DDI_MODE_HDMI",    IG_DDI_MODE_HDMI,    0u);
    checkU32("IG_DDI_MODE_DP_SST",  IG_DDI_MODE_DP_SST,  2u);
    checkU32("IG_DDI_MODE_FDI_OR_128B132B",
             IG_DDI_MODE_FDI_OR_128B132B, 4u);

    //
    //  The mode field is three bits at 24, so bit 27 -- the LOW bit of the port
    //  field -- must not leak into it. This is the same adjacency as above read
    //  from the other side, and it is the input that separates a 3-bit mask
    //  from a 4-bit one.
    //
    checkU32("bit 27 does not leak into the mode field",
             igDdiMode(0x08000000u), 0u);
    checkU32("and bit 23 does not either",
             igDdiMode(0x00800000u), 0u);

    //
    //  A FULLY PROGRAMMED WORD, all three fields at once. Enabled, port A
    //  (field 1), DP_SST. Decoding all three from one word is what catches a
    //  field that reads correctly in isolation but overlaps a neighbour.
    //
    const IGU32 live = 0x8A000000u;
    checkBool("a live eDP-style word is enabled", igDdiEnabled(live), true);
    checkU32("its port is 0",  igDdiPort(live), 0u);
    checkU32("its mode is DP_SST", igDdiMode(live), 2u);
}

//
// --- the transcoder/DDI join ------------------------------------------------
//
// Four reachable states, and the point of the arm is that every one of them is
// a NON-FAULTING configuration. TRANSCONF needs both bit 31 and bit 30 for
// igTransconfLive(); TRANS_DDI_FUNC_CTL needs bit 31 plus a non-zero port
// field. So the two half-configured middles -- timing without an output, and an
// output without timing -- are exactly the words a driver consulting one
// register would call usable.
//
// Every fixture is a bare literal. TRANSCONF live is 0xC0000000 (both enables);
// a DDI on port A in DP_SST is 0x8A000000 (bit 31, port field 1 at shift 27,
// mode 2 at shift 24).
//
static void testTransconfDdiLink(void)
{
    section("igTransconfDdiLinked -- timing joined to output");

    const IGU32 live = 0xC0000000u;   // TRANSCONF: ENABLE | STATE_ENABLE
    const IGU32 ddiA = 0x8A000000u;   // DDI: enable, port field 1, DP_SST

    checkU32("timing and a named port is OK",
             igTransconfDdiLinked(live, ddiA), 0u);

    //
    //  NO TIMING WINS, and it wins even when the DDI is perfect. A transcoder
    //  that is not running makes the DDI state uninteresting -- reporting a DDI
    //  verdict there would name a symptom rather than the cause.
    //
    checkU32("a dark transcoder under a perfect DDI is NO_TIMING",
             igTransconfDdiLinked(0u, ddiA), 1u);

    //
    //  HALF-ENABLED TRANSCONF. Bit 31 without bit 30, and bit 30 without bit
    //  31, are both NOT live -- igTransconfLive() is a conjunction, and these
    //  are the inputs that separate it from either single bit.
    //
    checkU32("ENABLE alone is NO_TIMING",
             igTransconfDdiLinked(0x80000000u, ddiA), 1u);
    checkU32("STATE_ENABLE alone is NO_TIMING",
             igTransconfDdiLinked(0x40000000u, ddiA), 1u);

    //
    //  TIMING WITHOUT AN OUTPUT -- a raster generated into nothing.
    //
    checkU32("a live transcoder with a disabled DDI is NO_OUTPUT",
             igTransconfDdiLinked(live, 0u), 2u);
    checkU32("and a DDI with every bit but 31 set is still NO_OUTPUT",
             igTransconfDdiLinked(live, 0x7FFFFFFFu), 2u);

    //
    //  ENABLED BUT NAMELESS. Bit 31 set, port field zero -- reachable, non
    //  faulting, and the state an unprogrammed page would produce if something
    //  had set only the enable. A decoder without the +1 guard would call this
    //  port A and inherit onto an output that names nothing.
    //
    checkU32("an enabled DDI with a zero port field is NO_PORT",
             igTransconfDdiLinked(live, 0x80000000u), 3u);
    checkU32("and the mode field does not rescue it",
             igTransconfDdiLinked(live, 0x82000000u), 3u);

    //
    //  NEITHER. Both registers zero, which is what this silicon reads on an
    //  unprogrammed pipe (2026-08-16 capture, READING NOTES). NO_TIMING, not
    //  NO_OUTPUT, because the timing check runs first.
    //
    checkU32("two zero words are NO_TIMING",
             igTransconfDdiLinked(0u, 0u), 1u);

    //  The highest port the field can name still links.
    checkU32("port field 15 links",
             igTransconfDdiLinked(live, 0xF8000000u), 0u);

    checkU32("IG_LINK_OK",         IG_LINK_OK,         0u);
    checkU32("IG_LINK_NO_TIMING",  IG_LINK_NO_TIMING,  1u);
    checkU32("IG_LINK_NO_OUTPUT",  IG_LINK_NO_OUTPUT,  2u);
    checkU32("IG_LINK_NO_PORT",    IG_LINK_NO_PORT,    3u);
    checkU32("IG_LINK_VERDICT_LIMIT covers the largest",
             IG_LINK_VERDICT_LIMIT, 3u);
}

//
// --- the plane/raster join --------------------------------------------------
//
// igPlaneMatchesPipe() takes the plane rectangle ALREADY DECODED, so every
// argument below is a bare literal in the decoded domain. The pipeSrc word is
// still raw, because that half of the comparison is this header's own register.
//
// The raster is 1280x800 again, NOT the captured 1920x1080, for the reason in
// testModeConsistent(): a transpose is only catchable when width != height.
//
static void testPlaneJoin(void)
{
    section("igPlaneMatchesPipe -- plane rectangle vs raster active area");

    //  PIPESRC hi=1280-1=0x4FF, lo=800-1=0x31F.
    const IGU32 src = 0x04FF031Fu;

    checkU32("a plane matching the raster is OK",
             igPlaneMatchesPipe(1280u, 800u, src), 0u);

    checkU32("a narrower plane is WIDTH",
             igPlaneMatchesPipe(1279u, 800u, src), 1u);
    checkU32("a wider plane is WIDTH",
             igPlaneMatchesPipe(1281u, 800u, src), 1u);
    checkU32("a shorter plane is HEIGHT",
             igPlaneMatchesPipe(1280u, 799u, src), 2u);
    checkU32("a taller plane is HEIGHT",
             igPlaneMatchesPipe(1280u, 801u, src), 2u);

    //
    //  THE TRANSPOSE. Handing the plane's rectangle over with its axes swapped
    //  must be caught, and this is the arm the captured panel could never
    //  separate: on 1920x1080 a swap is still catchable, but on any square
    //  mode it is not, and PLANE_SIZE's halves are the byte-swap of PIPESRC's
    //  on that very boot. 1280 != 800 makes the swap visible here.
    //
    checkU32("plane axes swapped is WIDTH",
             igPlaneMatchesPipe(800u, 1280u, src), 1u);

    //
    //  DEGENERATE FIRST. An unprogrammed plane page reads 0x00000000 on this
    //  silicon and the -1 convention decodes it to 1x1. That is a plane that
    //  was never programmed, not a plane of the wrong width, and it wins over
    //  the width comparison it would also fail.
    //
    checkU32("a 1x1 plane is DEGENERATE, not WIDTH",
             igPlaneMatchesPipe(1u, 1u, src), 3u);
    checkU32("width 1 alone is DEGENERATE",
             igPlaneMatchesPipe(1u, 800u, src), 3u);
    checkU32("height 1 alone is DEGENERATE",
             igPlaneMatchesPipe(1280u, 1u, src), 3u);
    checkU32("a zero width is DEGENERATE",
             igPlaneMatchesPipe(0u, 800u, src), 3u);
    checkU32("2x2 is the smallest non-degenerate rectangle",
             igPlaneMatchesPipe(2u, 2u, 0x00010001u), 0u);

    //
    //  WIDTH BEATS HEIGHT. Break both and the width verdict must win, or the
    //  ordering in the header is decoration and a caller cannot tell which
    //  axis to look at.
    //
    checkU32("width is reported before height",
             igPlaneMatchesPipe(1000u, 600u, src), 1u);

    //
    //  An unprogrammed PIPESRC under a real plane. 0x00000000 decodes to 1x1,
    //  so the plane cannot match it -- and the verdict must be WIDTH rather
    //  than DEGENERATE, because DEGENERATE describes the PLANE and this plane
    //  is fine. Getting this backwards would blame the wrong register.
    //
    checkU32("a zero PIPESRC under a real plane is WIDTH",
             igPlaneMatchesPipe(1280u, 800u, 0u), 1u);

    //  The captured panel, as a second raster.
    checkU32("the captured 1920x1080 plane matches its PIPESRC",
             igPlaneMatchesPipe(1920u, 1080u, 0x077F0437u), 0u);

    checkU32("IG_JOIN_OK",          IG_JOIN_OK,          0u);
    checkU32("IG_JOIN_WIDTH",       IG_JOIN_WIDTH,       1u);
    checkU32("IG_JOIN_HEIGHT",      IG_JOIN_HEIGHT,      2u);
    checkU32("IG_JOIN_DEGENERATE",  IG_JOIN_DEGENERATE,  3u);
    checkU32("IG_JOIN_VERDICT_LIMIT covers the largest",
             IG_JOIN_VERDICT_LIMIT, 3u);
}

//
// igPipeUsable() -- the whole-pipe join.
//
// The raster is the same 1280x800 @ 1680x831 pinned in testModeConsistent(),
// deliberately NOT the captured 1920x1080: PLANE_SIZE and PIPESRC are
// byte-swapped halves of each other, so a square-ish mode would let a
// transposing decoder pass. 1280x800 separates the two readings.
//
// PLANE_SIZE for that mode is 0x031F04FF -- height 799 in 31:16, width 1279 in
// 15:0, the INVERSE of PIPESRC's 0x04FF031F. Both are bare literals here rather
// than composed from the constants under test.
//
static void testPipeUsable(void)
{
    section("igPipeUsable -- link, timing and plane in one verdict");

    const IGU32 live = 0xC0000000u;   // TRANSCONF: ENABLE | STATE_ENABLE
    const IGU32 ddiA = 0x8A000000u;   // DDI: enable, port field 1, DP_SST

    const IGU32 hTot = 0x068F04FFu;
    const IGU32 hBlk = 0x068F04FFu;
    const IGU32 hSyn = 0x05C7053Fu;
    const IGU32 vTot = 0x033E031Fu;
    const IGU32 vBlk = 0x033E031Fu;
    const IGU32 vSyn = 0x03250322u;
    const IGU32 src  = 0x04FF031Fu;
    const IGU32 psz  = 0x031F04FFu;

    checkU32("a whole live pipe is OK",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, psz), 0u);

    //
    //  An unprogrammed pipe reads 0x00000000 in BOTH registers on this
    //  silicon, so this is the case every transcoder that was never
    //  configured lands in -- not a hypothetical.
    //
    checkU32("an unprogrammed transcoder is LINK",
             igPipeUsable(0u, 0u, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, psz), 1u);

    checkU32("timing but no DDI enable is LINK",
             igPipeUsable(live, 0u, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, psz), 1u);

    //  Enabled DDI whose port field is zero: well-formed, names no output.
    checkU32("DDI enabled on no port is LINK",
             igPipeUsable(live, 0x82000000u, hTot, hBlk, hSyn, vTot, vBlk,
                          vSyn, src, psz), 1u);

    checkU32("a DDI live but transcoder dark is LINK",
             igPipeUsable(0u, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, psz), 1u);

    //  vsync pushed past vblank end -- consistent link, broken raster.
    checkU32("sync outside blank is TIMING",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk,
                          0x03400322u, src, psz), 2u);

    //  PIPESRC naming a different active area than HTOTAL's.
    checkU32("PIPESRC disagreeing with HTOTAL is TIMING",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          0x03FF031Fu, psz), 2u);

    checkU32("a plane one pixel narrow is PLANE",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, 0x031F04FEu), 3u);

    checkU32("a plane one line short is PLANE",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, 0x031E04FFu), 3u);

    //
    //  THE TRANSPOSE. Feeding PIPESRC's own word in as PLANE_SIZE is exactly
    //  what a decoder that reused one half-order for both produces: it
    //  decodes to 800x1280, plausible on both axes, wrong on this mode.
    //
    checkU32("PIPESRC's word used as PLANE_SIZE is PLANE",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, 0x04FF031Fu), 3u);

    checkU32("a 1x1 plane page is PLANE",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk, vSyn,
                          src, 0u), 3u);

    //
    //  ORDER IS CAUSAL. When more than one stage would refuse, the EARLIEST
    //  must win -- a dead transcoder's timing words are stale contents, and
    //  naming the later symptom would send a caller after the wrong register.
    //
    checkU32("link and timing both broken reports LINK",
             igPipeUsable(0u, 0u, hTot, hBlk, hSyn, vTot, vBlk,
                          0x03400322u, src, psz), 1u);

    checkU32("timing and plane both broken reports TIMING",
             igPipeUsable(live, ddiA, hTot, hBlk, hSyn, vTot, vBlk,
                          0x03400322u, src, 0u), 2u);

    checkU32("link, timing and plane all broken reports LINK",
             igPipeUsable(0u, 0u, hTot, hBlk, hSyn, vTot, vBlk,
                          0x03400322u, src, 0u), 1u);

    checkU32("IG_USABLE_OK",     IG_USABLE_OK,     0u);
    checkU32("IG_USABLE_LINK",   IG_USABLE_LINK,   1u);
    checkU32("IG_USABLE_TIMING", IG_USABLE_TIMING, 2u);
    checkU32("IG_USABLE_PLANE",  IG_USABLE_PLANE,  3u);
    checkU32("IG_USABLE_VERDICT_LIMIT covers the largest",
             IG_USABLE_VERDICT_LIMIT, 3u);

    //
    //  The prefix collision itself, pinned. IG_PIPE_TRANSCONF is a REGISTER
    //  OFFSET inside the transcoder page and has nothing to do with these
    //  verdicts; it is only here because naming the family IG_PIPE_ swept it
    //  into the gate's prefix scan and produced a contiguity failure against
    //  0x08. If somebody renames the family back, this line goes red first.
    //
    checkU32("IG_PIPE_TRANSCONF is a register offset, not a verdict",
             IG_PIPE_TRANSCONF, 0x08u);
}

//
//  igReadDecision() -- the three guards in front of regRead32()'s one
//  dereference, extracted from the IOKit shell so that something can execute
//  them. Until 2026-08-17 they were spelled out inside
//  phase0-kext/IntelDisplayProbe.cpp, which no host suite can compile.
//
//  THE POINT OF THIS SECTION IS PRECEDENCE, NOT THE VERDICTS THEMSELVES.
//
//  All three guards make regRead32() return IG_MMIO_INVALID, so no test can
//  separate them through the return value. Worse, IG_MMIO_INVALID is 0xFFFFFFFF,
//  which is ALSO outside the policy range -- so the sentinel guard and the range
//  guard overlap on their one interesting input. Delete the sentinel arm and
//  regRead32() still refuses 0xFFFFFFFF, still returns IG_MMIO_INVALID, and
//  still declines to dereference: behaviour identical through the return value,
//  and a log line that now blames the policy range for an offset helper's
//  refusal. That is the mutation this section exists to redden, and it is only
//  reddenable because igReadDecision() reports a REASON rather than a bool.
//
static void testReadDecision(void)
{
    section("igReadDecision -- the read guards, and their order");

    //
    //  The verdicts, pinned to BARE LITERALS rather than to the #defines that
    //  produced them. An accessor checked against its own constant passes under
    //  any value; that defect has already cost this project fifteen
    //  undetectable offsets.
    //
    checkU32("IG_READ_OK",              IG_READ_OK,              0u);
    checkU32("IG_READ_UNMAPPED",        IG_READ_UNMAPPED,        1u);
    checkU32("IG_READ_SENTINEL_OFFSET", IG_READ_SENTINEL_OFFSET, 2u);
    checkU32("IG_READ_OUT_OF_POLICY",   IG_READ_OUT_OF_POLICY,   3u);
    checkU32("IG_READ_PAST_MAPPING",    IG_READ_PAST_MAPPING,    4u);
    checkU32("IG_READ_VERDICT_LIMIT covers the largest",
             IG_READ_VERDICT_LIMIT, 4u);

    //
    //  The access width, pinned to a bare literal. It is not decoration: the
    //  mapping bound is `offset + width <= length`, so a width silently edited
    //  to 1 accepts an offset three bytes short of the end and reads past it.
    //
    checkU32("IG_MMIO_ACCESS_BYTES", IG_MMIO_ACCESS_BYTES, 4u);

    //
    //  A mapping long enough for the whole display block. Spelled as a bare
    //  literal rather than as IG_BAR0_EXPECTED_LENGTH, because that constant
    //  lives in IntelDisplayProbe.hpp -- an IOKit translation unit this suite
    //  cannot include. That is the same split that hid this guard's absence in
    //  the first place, so the literal is the honest spelling here.
    //
    const IGU64 kFullBar = 0x00080000ull;

    //
    //  OK must be the zero. Every other verdict is a refusal reason, so a
    //  caller writing `if (igReadDecision(...))` reads correctly only while
    //  that holds -- and this line is what makes renumbering the family fail
    //  here rather than at a call site nothing executes.
    //
    checkU32("OK is the zero verdict", IG_READ_OK, 0u);

    //
    //  THE OVERLAP THAT MAKES ORDER OBSERVABLE. IG_MMIO_INVALID is outside the
    //  policy range, so on that one input two guards are both true. If this
    //  line ever went green as `1`, the precedence assertions below would be
    //  vacuous -- they would be separating conditions that cannot co-occur.
    //
    checkU32("IG_MMIO_INVALID is itself outside the policy range",
             igIsInDisplayPolicyRange(IG_MMIO_INVALID) ? 1u : 0u, 0u);

    //
    //  PRECEDENCE, on inputs where more than one guard is true at once.
    //
    //  Unmapped first: with no base address the offset is not yet meaningful,
    //  so naming the offset would send a reader after a constant when the real
    //  fault is that mapBar0() failed.
    //
    checkU32("unmapped beats a sentinel offset",
             igReadDecision(false, kFullBar, IG_MMIO_INVALID), 1u);
    checkU32("unmapped beats an out-of-policy offset",
             igReadDecision(false, kFullBar, 0u), 1u);
    checkU32("unmapped beats a perfectly legal offset",
             igReadDecision(false, kFullBar, IG_DISPLAY_POLICY_BASE), 1u);
    checkU32("unmapped beats a zero-length mapping",
             igReadDecision(false, 0ull, IG_DISPLAY_POLICY_BASE), 1u);

    //
    //  Sentinel before range. This is the load-bearing line of the whole
    //  section: it is the ONLY check in this tree that separates a live
    //  igReadDecision() from one with the sentinel arm deleted.
    //
    checkU32("a sentinel offset is diagnosed as a sentinel, not as out-of-policy",
             igReadDecision(true, kFullBar, IG_MMIO_INVALID), 2u);

    //
    //  And the sentinel still wins when the mapping is empty, so the fourth
    //  guard cannot steal the diagnosis for a caller who never mapped anything
    //  useful. 0xFFFFFFFF is past the end of EVERY realistic mapping, which is
    //  exactly why this needs saying.
    //
    checkU32("a sentinel beats a past-the-mapping offset",
             igReadDecision(true, 0ull, IG_MMIO_INVALID), 2u);

    //
    //  The range arm, pinned at BOTH edges and one step outside each. A bound
    //  tested only from the slack side is the containment-is-one-sided defect:
    //  widening the window produces no failure if every probe stays inside it.
    //
    checkU32("one below the base is out of policy",
             igReadDecision(true, kFullBar, IG_DISPLAY_POLICY_BASE - 1u), 3u);
    checkU32("the base itself is OK",
             igReadDecision(true, kFullBar, IG_DISPLAY_POLICY_BASE), 0u);
    checkU32("the limit itself is OK",
             igReadDecision(true, kFullBar, IG_DISPLAY_POLICY_LIMIT), 0u);
    checkU32("one above the limit is out of policy",
             igReadDecision(true, kFullBar, IG_DISPLAY_POLICY_LIMIT + 1u), 3u);
    checkU32("offset zero is out of policy",
             igReadDecision(true, kFullBar, 0u), 3u);

    //
    //  Offsets the probe really composes, run through the decision rather than
    //  through hand-written numbers. igTransBase() and igPipeBase() are the two
    //  ends of the span IG_DISPLAY_POLICY_LIMIT was derived from, so if either
    //  table grows past the window this reddens here rather than on hardware.
    //
    checkU32("transcoder A's page is readable",
             igReadDecision(true, kFullBar, igTransBase(IG_TRANSCODER_A)), 0u);
    checkU32("pipe DSI1's page is readable",
             igReadDecision(true, kFullBar, igPipeBase(IG_TRANSCODER_DSI1)), 0u);

    //
    //  And the refusal an offset helper actually produces, end to end: an
    //  out-of-range transcoder index yields IG_MMIO_INVALID from igTransBase(),
    //  which igReadDecision() must then diagnose as a sentinel. This is the
    //  real call shape in IntelDisplayProbe.cpp -- a helper's refusal handed
    //  straight to the read guard -- rather than a literal typed by a test.
    //
    checkU32("a declining offset helper's result is a sentinel",
             igReadDecision(true, kFullBar, igTransBase(IG_TRANSCODER_COUNT)), 2u);

    //
    //  ---- THE FOURTH GUARD: THE MAPPING IS NOT THE POLICY RANGE ------------
    //
    //  Every check above holds a full-length BAR constant, which is exactly the
    //  blind spot this block exists to fill. IG_DISPLAY_POLICY_LIMIT bounds the
    //  addresses the HEADER can produce; the mapping length is what the OS
    //  actually handed over, and IntelDisplayProbe::mapBar0() attaches to a
    //  short BAR on purpose so the node stays inspectable. Until
    //  igReadDecision() took a length, that decision had no consequence beyond
    //  a log line, and a 0x40000 BAR read transcoder A at 0x60000 -- 0x20000
    //  bytes past the end of the mapping, which is not a fault, just whatever
    //  the next mapping holds decoded as a transcoder register.
    //
    checkU32("a short BAR refuses transcoder A's page",
             igReadDecision(true, 0x00040000ull, igTransBase(IG_TRANSCODER_A)), 4u);
    checkU32("a zero-length mapping refuses a legal offset",
             igReadDecision(true, 0ull, IG_DISPLAY_POLICY_BASE), 4u);

    //
    //  THE EDGE, AND THE WIDTH. The test is `offset + 4 <= length`, so the
    //  largest acceptable offset is length - 4. These four lines pin that edge
    //  from both sides, and the middle two are the ones that fail under a
    //  `offset < length` bound -- the version that accepts an offset one, two
    //  or three bytes short of the end and then reads past it. A bound tested
    //  only from the slack side is the containment-is-one-sided defect again.
    //
    checkU32("an offset exactly four bytes below the end is OK",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE + 4ull,
                            IG_DISPLAY_POLICY_BASE), 0u);
    checkU32("one byte short of that refuses -- the read would overhang by one",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE + 3ull,
                            IG_DISPLAY_POLICY_BASE), 4u);

    //
    //  The other two bytes of the same window. Three lines rather than one
    //  because the overhang is a RANGE, and a bound that is wrong by a
    //  DIFFERENT amount is a different defect: `offset >= length` fails only
    //  the +3 case, `offset + 2 > length` fails only +1 and +2, and
    //  `offset + 3 > length` fails only +1. Pinning one point of the window
    //  leaves the other spellings green, so all three are pinned and a bound
    //  off by any amount reddens at least one of them.
    //
    checkU32("two bytes short refuses -- the read would overhang by two",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE + 2ull,
                            IG_DISPLAY_POLICY_BASE), 4u);
    checkU32("three bytes short refuses -- the read would overhang by three",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE + 1ull,
                            IG_DISPLAY_POLICY_BASE), 4u);
    checkU32("a mapping ending exactly at the offset refuses",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE,
                            IG_DISPLAY_POLICY_BASE), 4u);
    checkU32("a generous mapping accepts the same offset",
             igReadDecision(true, (IGU64) IG_DISPLAY_POLICY_BASE + 5ull,
                            IG_DISPLAY_POLICY_BASE), 0u);

    //
    //  PRECEDENCE, and it is the reverse of the intuitive order. An
    //  out-of-policy offset is a bug in THIS DRIVER; a short BAR is a fact
    //  about the machine. An offset that is both should be reported as the bug,
    //  because that is the one somebody can go and fix. Both inputs below are
    //  simultaneously outside the policy window and past the end of the
    //  mapping, so a guard order swapped in the header reddens here.
    //
    checkU32("out-of-policy beats past-the-mapping, below the window",
             igReadDecision(true, 0ull, 0u), 3u);
    checkU32("out-of-policy beats past-the-mapping, above the window",
             igReadDecision(true, 0ull, IG_DISPLAY_POLICY_LIMIT + 1u), 3u);

    //
    //  NO WRAP. The addition is done in 64 bits so that a large offset cannot
    //  carry past the top of a 32-bit word and land back inside the bound. The
    //  largest offset the policy range admits is nowhere near that, so the
    //  honest statement of the property is the other end: a mapping larger than
    //  any 32-bit offset accepts every legal one, and the arithmetic does not
    //  overflow on the way there.
    //
    checkU32("a mapping past the 32-bit ceiling still accepts the policy limit",
             igReadDecision(true, 0x0000000100000000ull,
                            IG_DISPLAY_POLICY_LIMIT), 0u);
    checkU32("the widest possible mapping accepts the policy limit",
             igReadDecision(true, 0xFFFFFFFFFFFFFFFFull,
                            IG_DISPLAY_POLICY_LIMIT), 0u);

    //
    //  And the full-BAR case stated as a claim rather than as a fixture: a BAR
    //  of IG_BAR0_EXPECTED_LENGTH accepts BOTH ends of the span the policy
    //  limit was derived from. If either table grows past 0x80000 this reddens
    //  here, which is the correct place for it -- the shell's warning threshold
    //  and the decode tables would otherwise drift apart silently.
    //
    checkU32("a full-length BAR accepts transcoder A",
             igReadDecision(true, kFullBar, igTransBase(IG_TRANSCODER_A)), 0u);
    checkU32("a full-length BAR accepts pipe DSI1",
             igReadDecision(true, kFullBar, igPipeBase(IG_TRANSCODER_DSI1)), 0u);
}

//
//  igReadRefused / igSampleVerdict -- the other half of the read guard.
//
//  igReadDecision() decides whether the dereference may happen. This pair is
//  what makes the REFUSAL visible to the code downstream of it. Until they
//  existed, IG_MMIO_INVALID appeared at exactly one place in the whole probe --
//  inside regRead32() -- and both publishers handed whatever came back straight
//  to igModeConsistent(), igPipeUsable() and igFbInheritable(). setProperty()
//  with a decode of a refused read is byte-identical to setProperty() with a
//  decode of a register that genuinely read that value.
//
//  The group is judged as a SET rather than per-field because the decoders are
//  joins: igPipeUsable() eats nine words and returns one verdict, so a single
//  refusal anywhere in the group poisons the whole answer.
//
static void testSampleVerdict(void)
{
    section("igReadRefused / igSampleVerdict -- a refusal nobody consumes");

    //
    //  This suite's own copy, spelled as a bare literal for the same reason
    //  testReadDecision() spells its own: IG_BAR0_EXPECTED_LENGTH lives in
    //  IntelDisplayProbe.hpp, an IOKit translation unit this suite cannot
    //  include, and a constant checked against the #define that produced it
    //  passes under any value.
    //
    const IGU64 kFullBar = 0x00080000ull;

    //
    //  The verdict constants, pinned to bare literals rather than to each other.
    //  IG_SAMPLE_VERDICT_LIMIT is what the names suite's exhaustiveness loop
    //  bounds on, so a third arm added here without a name reddens there.
    //
    checkU32("IG_SAMPLE_OK is 0", IG_SAMPLE_OK, 0u);
    checkU32("IG_SAMPLE_REFUSED is 1", IG_SAMPLE_REFUSED, 1u);
    checkU32("IG_SAMPLE_VERDICT_LIMIT is 1", IG_SAMPLE_VERDICT_LIMIT, 1u);

    //
    //  The predicate itself. Spelled against the sentinel VALUE, so the two
    //  neighbours of that value are the separating inputs -- a mask or a
    //  comparison written one bit loose accepts 0xFFFFFFFE too.
    //
    checkBool("the sentinel is a refusal", igReadRefused(IG_MMIO_INVALID), true);
    checkBool("one below the sentinel is not", igReadRefused(0xFFFFFFFEu), false);
    checkBool("the top bit alone is not", igReadRefused(0x80000000u), false);
    checkBool("all-ones in the low half is not", igReadRefused(0x0000FFFFu), false);

    //
    //  ZERO IS NOT A REFUSAL, and this is the check with evidence behind it.
    //  Every unprogrammed display page on this silicon read 0x00000000 in the
    //  2026-08-16 capture (handoff15 SS2) -- so zero is what an idle page
    //  reports and the probe must publish a real verdict over it rather than
    //  calling the read refused.
    //
    checkBool("zero is a measurement, not a refusal", igReadRefused(0u), false);

    //
    //  A group with nothing wrong in it.
    //
    const IGU32 clean[] = { 0x00000000u, 0x077F0437u, 0x0437077Fu, 0x00000001u };
    checkU32("a clean group is ok", igSampleVerdict(clean, 4u), 0u);

    //
    //  REFUSAL AT EVERY POSITION, not just the ends. A loop that stops early, or
    //  one that checks only values[0], is green on three of these four inputs --
    //  the same lesson as the overhang window above: a single edge check is a
    //  point sample of a range property.
    //
    for (IGU32 i = 0u; i < 4u; i++) {
        IGU32 poisoned[4];
        char  label[96];

        for (IGU32 j = 0u; j < 4u; j++)
            poisoned[j] = clean[j];
        poisoned[i] = IG_MMIO_INVALID;

        std::snprintf(label, sizeof(label),
                      "a refusal at index %u poisons the group", i);
        checkU32(label, igSampleVerdict(poisoned, 4u), 1u);
    }

    //
    //  THE GROUP THE TRANSCODER PUBLISHER ACTUALLY BUILDS. Ten words, because
    //  publishTranscoder() samples TRANSCONF, TRANS_DDI, the six timing halves,
    //  PIPESRC and PLANE_SIZE before it decodes any of them. Walked at full
    //  width so a bound written for the four-word plane group cannot pass here.
    //
    for (IGU32 i = 0u; i < 10u; i++) {
        IGU32 group[10];
        char  label[96];

        for (IGU32 j = 0u; j < 10u; j++)
            group[j] = 0x00001000u + j;
        group[i] = IG_MMIO_INVALID;

        std::snprintf(label, sizeof(label),
                      "ten-word group, refusal at index %u", i);
        checkU32(label, igSampleVerdict(group, 10u), 1u);
    }

    //
    //  THE COUNT IS THE BOUND, and it is honoured in both directions. Reading
    //  past it would find a refusal that is not in the group; stopping short of
    //  it would miss one that is.
    //
    const IGU32 tail[] = { 0x11u, 0x22u, IG_MMIO_INVALID };
    checkU32("a count that excludes the refusal is ok",
             igSampleVerdict(tail, 2u), 0u);
    checkU32("a count that includes it refuses",
             igSampleVerdict(tail, 3u), 1u);

    //
    //  AN EMPTY OR ABSENT GROUP REFUSES rather than passing. A caller that lost
    //  its array must not be told its reads were fine -- the same direction
    //  fBar0Length is zeroed on unmap for: the safe answer on missing evidence
    //  is refusal, never ok.
    //
    checkU32("a zero count refuses", igSampleVerdict(clean, 0u), 1u);
    checkU32("a null group refuses", igSampleVerdict(nullptr, 4u), 1u);
    checkU32("a null group with zero count refuses",
             igSampleVerdict(nullptr, 0u), 1u);

    //
    //  AND THE JOIN WITH THE GUARD ABOVE, which is the whole point of the pair:
    //  the value regRead32() returns on every refusal verdict is the value this
    //  predicate recognises. If either side changed its sentinel independently,
    //  refusals would flow into the decoders again in silence.
    //
    checkBool("what an unmapped BAR yields is recognised as a refusal",
              igReadRefused(IG_MMIO_INVALID) &&
              igReadDecision(false, kFullBar, IG_DISPLAY_POLICY_BASE) != 0u, true);
    checkBool("what a short BAR yields is recognised as a refusal",
              igReadRefused(IG_MMIO_INVALID) &&
              igReadDecision(true, 0ull, IG_DISPLAY_POLICY_BASE) != 0u, true);
}

//
//  ============================================================================
//  THE 2026-08-18 CAPTURE, DRIVEN THROUGH THE DECODERS
//  ============================================================================
//
//  Every constant in IntelDisplay.hpp's IG_OBSERVED_*_2026_08_18 block is a
//  word read off Tiger Lake silicon on the SECOND and FINAL Linux boot. There
//  will never be a third capture. This section is what makes those words
//  falsifiable rather than decorative.
//
//  THE DIRECTION MATTERS AND IT IS EASY TO GET BACKWARDS. The decoders were
//  written from reference/i915/ before this capture existed, and nothing in
//  them is derived from it. So these checks run the DECODE against the
//  MEASUREMENT and assert they agree. A disagreement means one of two things,
//  and both are worth a red gate:
//
//    - a decoder is wrong about Gen12, in which case the capture is right and
//      the header needs fixing; or
//    - somebody edited an IG_OBSERVED_ constant, in which case a value this
//      project calls MEASURED silently became a value somebody typed, and the
//      evidence trail is gone with no way to re-derive it.
//
//  EXPECTED VALUES ARE BARE LITERALS ON BOTH SIDES. Writing
//  igPlusOne(IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18) against a second
//  expression built out of the same #define would be self-cancelling -- the
//  fixture would move with the constant and the check could not fail. The
//  decoded expectations below (1920, 1080, 2028, ...) are typed from the
//  panel's advertised mode, not from the register word.
//
static void testObservedCapture20260818(void)
{
    std::printf("\n-- values witnessed on silicon, 2026-08-18 20:14:12 UTC --\n");

    //
    //  THE POSITIVE CONTROL FIRST. Transcoder A drove the lit internal panel.
    //  If this block decodes to something other than the panel's real mode then
    //  the reader reached the wrong registers, and every 0x00000000 elsewhere in
    //  that capture stops meaning UNPROGRAMMED and starts meaning NOT READ --
    //  which would invalidate the DSI finding below and the plane count with it.
    //
    //
    //  igTimingLo() and igTimingHi() ALREADY add the +1 back (hazard 3 lives
    //  inside them), so nothing here wraps them in a second increment -- doing
    //  that would move every expectation by one and the fixture would still
    //  look self-consistent.
    //
    checkU32("HTOTAL active half is the panel width",
             igTimingLo(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18), 1920u);
    checkU32("HTOTAL total half is the horizontal total",
             igTimingHi(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18), 2080u);
    checkU32("VTOTAL active half is the panel height",
             igTimingLo(IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18), 1080u);
    checkU32("VTOTAL total half is the vertical total",
             igTimingHi(IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18), 1180u);

    //
    //  HSYNC's halves are NOT the same shape as HTOTAL's -- start and end rather
    //  than active and total -- and both land between the active edge and the
    //  total. A decoder that transposed the pair would still produce two numbers
    //  in range, so the ORDER is asserted rather than just the values.
    //
    checkU32("HSYNC start",
             igTimingLo(IG_OBSERVED_TRANS_A_HSYNC_2026_08_18), 2028u);
    checkU32("HSYNC end",
             igTimingHi(IG_OBSERVED_TRANS_A_HSYNC_2026_08_18), 2076u);
    checkBool("HSYNC falls inside the blanking interval",
              igTimingLo(IG_OBSERVED_TRANS_A_HSYNC_2026_08_18) >
              igTimingLo(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18) &&
              igTimingHi(IG_OBSERVED_TRANS_A_HSYNC_2026_08_18) <
              igTimingHi(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18), true);
    checkU32("VSYNC start",
             igTimingLo(IG_OBSERVED_TRANS_A_VSYNC_2026_08_18), 1083u);
    checkU32("VSYNC end",
             igTimingHi(IG_OBSERVED_TRANS_A_VSYNC_2026_08_18), 1088u);

    //
    //  HBLANK equals HTOTAL and VBLANK equals VTOTAL on this panel. That is a
    //  property of an unblanked-margin mode rather than of the register pair, so
    //  it is asserted as a fact ABOUT THIS CAPTURE and not generalised.
    //
    checkBool("HBLANK reads identically to HTOTAL in this capture",
              IG_OBSERVED_TRANS_A_HBLANK_2026_08_18 ==
              IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18, true);
    checkBool("VBLANK reads identically to VTOTAL in this capture",
              IG_OBSERVED_TRANS_A_VBLANK_2026_08_18 ==
              IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18, true);

    //
    //  Q4, CONFIRMED ON A SECOND BOOT. PIPESRC's +1 equals the panel's
    //  advertised 1920x1080. First measured 2026-08-16; this is an independent
    //  boot of the same machine, which is what makes it a repeated measurement
    //  rather than one reading.
    //
    checkU32("PIPESRC width +1 is 1920",
             igPipesrcWidth(IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18), 1920u);
    checkU32("PIPESRC height +1 is 1080",
             igPipesrcHeight(IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18), 1080u);

    //
    //  AND THE TWO AGREE. PIPESRC and the timing block are written by different
    //  code paths in i915, so their agreement is evidence rather than tautology.
    //
    checkBool("PIPESRC width agrees with HTOTAL's active half",
              igPipesrcWidth(IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18) ==
              igTimingLo(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18), true);
    checkBool("PIPESRC height agrees with VTOTAL's active half",
              igPipesrcHeight(IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18) ==
              igTimingLo(IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18), true);

    //
    //  THE WHOLE-MODE JOIN over the six real words. This is the check that would
    //  redden if any single timing constant were edited, because the join
    //  compares them against each other.
    //
    checkU32("the captured mode is self-consistent",
             igModeConsistent(IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18,
                              IG_OBSERVED_TRANS_A_HBLANK_2026_08_18,
                              IG_OBSERVED_TRANS_A_HSYNC_2026_08_18,
                              IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18,
                              IG_OBSERVED_TRANS_A_VBLANK_2026_08_18,
                              IG_OBSERVED_TRANS_A_VSYNC_2026_08_18,
                              IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18),
             IG_MODE_OK);

    //
    //  TRANS_DDI_FUNC_CTL, and HAZARD 8 met by a real word. The port field on
    //  Gen12 is at shift 27; the pre-icl shift is 28. Decoding 0x8A000006 with
    //  the wrong shift yields a DIFFERENT port number that is still inside the
    //  field's range -- it names the wrong port instead of failing -- so the
    //  captured word is the only thing that can separate the two readings.
    //
    checkBool("the captured DDI function is enabled",
              igDdiEnabled(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18), true);
    checkU32("the captured DDI mode is DP SST",
             igDdiMode(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18),
             IG_DDI_MODE_DP_SST);

    //
    //  The field as STORED is 1, and the +1 encoding makes that port 0. Pinning
    //  both separately matters: checking only igDdiPort() lets the shift and the
    //  -1 cancel each other out for a single input.
    //
    checkU32("the captured DDI port field is the stored +1 encoding",
             igDdiPortField(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18), 1u);
    checkU32("so the port it names is port 0",
             igDdiPort(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18), 0u);
    checkBool("and the port it names is not the no-port sentinel",
              igDdiPort(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18) ==
              IG_DDI_PORT_NONE, false);

    //
    //  HAZARD 8 MET BY A REAL WORD. The pre-icl shift reads a DIFFERENT field
    //  out of this same word -- 0 rather than 1 -- and 0 is the encoding for NO
    //  PORT SELECTED. So the wrong shift turns a lit port into no port at all,
    //  silently, on the one machine this project has evidence from.
    //
    checkU32("the pre-icl shift reads a different field from the same word",
             (IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18 >> 28) & 0x7u, 0u);
    checkBool("which disagrees with the TGL field",
              ((IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18 >> 28) & 0x7u) ==
              igDdiPortField(IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18),
              false);

    //
    //  TRANSCONF carries BOTH enable bits. This is why igTransconfDdiLinked()
    //  requires the pair rather than bit 31 alone: on real silicon driving a
    //  real panel, 30 is set too.
    //
    checkBool("the captured TRANSCONF has the enable bit",
              igTransconfEnabled(IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18), true);
    checkBool("the captured TRANSCONF has the state bit",
              igTransconfStateEnabled(IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18),
              true);

    //
    //  THE LINK JOIN over the two captured words.
    //
    checkU32("the captured pipe and DDI are linked",
             igTransconfDdiLinked(IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18,
                                  IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18),
             IG_LINK_OK);

    //
    //  PLANE 1 ON PIPE A. HAZARD 5 on real words: PLANE_SIZE's halves are
    //  INVERTED relative to PIPESRC. Both encode 1920x1080 on the same pipe in
    //  the same capture, and they are byte-swapped -- so a decoder reused across
    //  the two transposes the axes and still programs cleanly.
    //
    checkU32("PLANE_SIZE width +1 is 1920",
             igPlaneSizeWidth(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18), 1920u);
    checkU32("PLANE_SIZE height +1 is 1080",
             igPlaneSizeHeight(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18), 1080u);
    checkBool("the two words are byte-swapped relative to each other",
              IG_OBSERVED_PLANE_1A_SIZE_2026_08_18 ==
              IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18, false);
    checkBool("and reading PLANE_SIZE as PIPESRC transposes the axes",
              igPipesrcWidth(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18) ==
              igPlaneSizeWidth(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18), false);

    //
    //  THE SAME 2026-08-16 WORD, TWO BOOTS APART. This is the only constant in
    //  the header measured twice, and the equality is the closest thing this
    //  track has to a reproducibility check.
    //
    checkBool("PLANE_SIZE reads identically to the 2026-08-16 capture",
              IG_OBSERVED_PLANE_1A_SIZE_2026_08_18 ==
              IG_PLANE_SIZE_OBSERVED_2026_08_16, true);

    checkBool("the captured plane is enabled",
              igPlaneEnabled(IG_OBSERVED_PLANE_1A_CTL_2026_08_18), true);
    checkU32("the captured plane is at the pipe origin",
             igPlanePosX(IG_OBSERVED_PLANE_1A_POS_2026_08_18) |
             igPlanePosY(IG_OBSERVED_PLANE_1A_POS_2026_08_18), 0u);

    //
    //  The icl+ 5-bit format decode gives 4 (XRGB8888). The pre-icl 4-bit decode
    //  is present in the header ONLY so this disagreement can be demonstrated on
    //  a real word rather than a constructed one.
    //
    checkU32("the captured plane format decodes as icl+ format 4",
             igPlaneFormat(IG_OBSERVED_PLANE_1A_CTL_2026_08_18), 4u);
    checkU32("the captured plane stride field is 60",
             igPlaneStride(IG_OBSERVED_PLANE_1A_STRIDE_2026_08_18), 60u);

    //
    //  THE PLANE / PIPE JOIN, which is the pair HAZARD 5 exists to protect.
    //
    checkU32("the captured plane matches the captured pipe",
             igPlaneMatchesPipe(
                 igPlaneSizeWidth(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18),
                 igPlaneSizeHeight(IG_OBSERVED_PLANE_1A_SIZE_2026_08_18),
                 IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18),
             IG_JOIN_OK);

    //
    //  AND THE WHOLE-PIPE VERDICT over nine real words at once. Every decode
    //  layer in this header, driven end to end by one boot of one machine.
    //
    checkU32("the captured pipe is usable end to end",
             igPipeUsable(IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18,
                          IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18,
                          IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18,
                          IG_OBSERVED_TRANS_A_HBLANK_2026_08_18,
                          IG_OBSERVED_TRANS_A_HSYNC_2026_08_18,
                          IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18,
                          IG_OBSERVED_TRANS_A_VBLANK_2026_08_18,
                          IG_OBSERVED_TRANS_A_VSYNC_2026_08_18,
                          IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18,
                          IG_OBSERVED_PLANE_1A_SIZE_2026_08_18),
             IG_USABLE_OK);

    //
    //  NONE OF THE NINE WORDS IS A REFUSAL. igSampleVerdict() judges the read
    //  GROUP, and a capture whose words tripped the refusal sentinel would make
    //  every verdict above arithmetic over something nobody measured.
    //
    {
        const IGU32 captured[10] = {
            IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18,
            IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18,
            IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18,
            IG_OBSERVED_TRANS_A_HBLANK_2026_08_18,
            IG_OBSERVED_TRANS_A_HSYNC_2026_08_18,
            IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18,
            IG_OBSERVED_TRANS_A_VBLANK_2026_08_18,
            IG_OBSERVED_TRANS_A_VSYNC_2026_08_18,
            IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18,
            IG_OBSERVED_PLANE_1A_SIZE_2026_08_18,
        };
        checkU32("the whole captured group reads as measured, not refused",
                 igSampleVerdict(captured, 10u), 0u);
    }

    //
    //  Q1/Q2 -- THE DSI 0x800 STRIDE, WITNESSED.
    //
    //  What is asserted is exactly what was seen and no more: two pages at a
    //  fixed spacing, both holding the SAME non-trivial word where a configured
    //  transcoder would hold its mode. That is the signature of two instances of
    //  one block at that stride. It is NOT two configured transcoders, and the
    //  header says so at the constants.
    //
    checkU32("the two DSI pages sit 0x800 apart",
             IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18 -
             IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18,
             IG_OBSERVED_TRANS_DSI_STRIDE_2026_08_18);
    checkBool("and the reset-state word they share is non-trivial",
              IG_OBSERVED_TRANS_DSI_DDI_FUNC_CTL_2026_08_18 != 0x00000000u &&
              IG_OBSERVED_TRANS_DSI_DDI_FUNC_CTL_2026_08_18 != IG_MMIO_INVALID,
              true);
    checkBool("the shared word is NOT an enabled function",
              igDdiEnabled(IG_OBSERVED_TRANS_DSI_DDI_FUNC_CTL_2026_08_18),
              false);

    //
    //  THE STRIDE THE HEADER ADDRESSES WITH agrees with the one witnessed. This
    //  is the check that closes Q1/Q2: until this capture the stride was a value
    //  this project chose, and igTransBase() has been emitting it all along.
    //
    checkU32("igTransBase(DSI0) is the witnessed base",
             igTransBase(IG_TRANSCODER_DSI0),
             IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18);
    checkU32("igTransBase(DSI1) is the witnessed base",
             igTransBase(IG_TRANSCODER_DSI1),
             IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18);
    checkU32("and the stride the header emits is the stride measured",
             igTransBase(IG_TRANSCODER_DSI1) - igTransBase(IG_TRANSCODER_DSI0),
             IG_OBSERVED_TRANS_DSI_STRIDE_2026_08_18);

    //
    //  WHAT AN UNPROGRAMMED PAGE READS ON THIS SILICON, and why igReadRefused()
    //  answers FALSE for it. Transcoders B, C and D certainly exist and all read
    //  zero on both boots -- so zero is a legitimate reading of a real register
    //  here, and treating it as a refusal would call three real blocks unread.
    //
    checkBool("an unprogrammed page is not a refusal on this silicon",
              igReadRefused(IG_OBSERVED_UNPROGRAMMED_2026_08_18), false);
    checkBool("and it is not the refusal sentinel",
              IG_OBSERVED_UNPROGRAMMED_2026_08_18 == IG_MMIO_INVALID, false);

    //
    //  THE NEGATIVE CONTROL FOR THE WHOLE SECTION. An unprogrammed transcoder
    //  must NOT come back usable -- otherwise every verdict above is a function
    //  that returns OK regardless of its input, and the capture proved nothing.
    //
    checkBool("an all-zero pipe does not read as usable",
              igPipeUsable(IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18,
                           IG_OBSERVED_UNPROGRAMMED_2026_08_18) ==
              IG_USABLE_OK, false);
}

//
//  ============================================================================
//  THE CONFIGURATION BLOCK FROM THE SAME BOOT -- clocks, fuses, panel power
//  ============================================================================
//
//  IntelDisplay.hpp's second observed block holds ten words plus one field
//  extractor. This section exists for the same reason the one above it does: a
//  constant nothing reads is indistinguishable from a typo, and these ten came
//  off silicon that will never be measured again.
//
//  The interesting half is the EXCLUSIONS. Three section 6 reads look exactly
//  as harvestable as these ten and are deliberately absent from the header, and
//  two of the three are decidable by a predicate this suite already has -- so
//  they are asserted here rather than left as prose in a comment.
//
static void testObservedConfig20260818(void)
{
    section("configuration state witnessed 2026-08-18 20:14:12 UTC");

    //
    //  THE FIELD EXTRACTOR FIRST, because it is the only executable thing in
    //  the block. CDCLK_CTL read 0x0038017E and the decimal frequency field is
    //  the low 11 bits (CDCLK_FREQ_DECIMAL_MASK = 0x7ff, i915_reg.h:4082), so
    //  the field is 0x17E. The kHz relation is NOT vendored in this checkout
    //  and is deliberately not encoded anywhere, so this asserts the RAW field
    //  and says nothing about a frequency.
    //
    checkU32("CDCLK_CTL decimal field is the low 11 bits of the witnessed word",
             igCdclkFreqDecimal(IG_OBSERVED_CDCLK_CTL_2026_08_18), 0x17Eu);
    checkU32("the extraction mask is the anchored 0x7ff",
             IG_CDCLK_FREQ_DECIMAL_MASK, 0x000007FFu);
    checkBool("the field is narrower than the whole word",
              igCdclkFreqDecimal(IG_OBSERVED_CDCLK_CTL_2026_08_18) !=
              IG_OBSERVED_CDCLK_CTL_2026_08_18, true);

    //
    //  WHY THE TWO ALL-ONES READS ARE NOT IN THE HEADER, asserted rather than
    //  asserted-about. PCH_DREF_CONTROL (0x0C6200) and _TRANS_DP_CTL_A
    //  (0x0E0300) both read 0xFFFFFFFF, and on this silicon that is what a
    //  block which is not decoding reports -- igReadRefused() says so. Filing
    //  either as a measured value would file a refusal as a reading. The
    //  literals are spelled here BECAUSE the header does not carry them: that
    //  absence is the claim.
    //
    checkBool("the 0xFFFFFFFF section 6 reads are refusals, not values",
              igReadRefused(0xFFFFFFFFu), true);

    //
    //  AND WHY THE ZERO READ *IS* IN THE HEADER, which is the same predicate
    //  answering the other way. SFUSE_STRAP read 0x00000000, and zero on this
    //  hardware is a legitimate reading of a real register -- measured across
    //  transcoders B/C/D on two separate boots. A gate that excluded both
    //  sentinels would have dropped a real measurement.
    //
    checkBool("SFUSE_STRAP's zero is not a refusal",
              igReadRefused(IG_OBSERVED_SFUSE_STRAP_2026_08_18), false);

    //
    //  THE WHOLE BLOCK AS A READ GROUP. igSampleVerdict() is the function both
    //  Intel publishers use to judge a set of reads before decoding any of
    //  them, so running the ten harvested words through it asserts that no
    //  refusal value was harvested at all -- the group property, not ten
    //  separate ones. A future edit that pins an all-ones word into this block
    //  reddens here even if it is spelled correctly.
    //
    {
        const IGU32 harvested[10] = {
            IG_OBSERVED_CDCLK_CTL_2026_08_18,
            IG_OBSERVED_SKL_DFSM_2026_08_18,
            IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18,
            IG_OBSERVED_SFUSE_STRAP_2026_08_18,
            IG_OBSERVED_DPCLKA_CFGCR0_2026_08_18,
            IG_OBSERVED_PANEL_PWR_C7200_2026_08_18,
            IG_OBSERVED_PANEL_PWR_C7204_2026_08_18,
            IG_OBSERVED_PANEL_PWR_C7208_2026_08_18,
            IG_OBSERVED_PANEL_PWR_C720C_2026_08_18,
            IG_OBSERVED_BACKLIGHT_C8250_2026_08_18,
        };

        checkU32("no harvested configuration word is a refusal value",
                 igSampleVerdict(harvested, 10u), IG_SAMPLE_OK);
    }

    //
    //  THE TWO EQUAL BACKLIGHT WORDS. 0x0C8254 and 0x0C8258 both read
    //  0x00017700, and the header keeps them as TWO constants rather than one.
    //  This pins the equality as a fact about this panel while leaving the two
    //  registers independent -- collapsing them to one constant would assert
    //  they must always agree, which nothing measured.
    //
    checkBool("the two backlight words were equal on this panel",
              IG_OBSERVED_BACKLIGHT_C8254_2026_08_18 ==
              IG_OBSERVED_BACKLIGHT_C8258_2026_08_18, true);
    checkBool("and neither is the PWM enable word",
              IG_OBSERVED_BACKLIGHT_C8250_2026_08_18 !=
              IG_OBSERVED_BACKLIGHT_C8254_2026_08_18, true);

    //
    //  THE PANEL POWER BLOCK IS FOUR DISTINCT WORDS. A copy-paste over four
    //  adjacent #defines is the likeliest way this block goes wrong, and it
    //  would leave every check above green.
    //
    checkBool("the four panel power words are pairwise distinct",
              IG_OBSERVED_PANEL_PWR_C7200_2026_08_18 !=
              IG_OBSERVED_PANEL_PWR_C7204_2026_08_18 &&
              IG_OBSERVED_PANEL_PWR_C7204_2026_08_18 !=
              IG_OBSERVED_PANEL_PWR_C7208_2026_08_18 &&
              IG_OBSERVED_PANEL_PWR_C7208_2026_08_18 !=
              IG_OBSERVED_PANEL_PWR_C720C_2026_08_18 &&
              IG_OBSERVED_PANEL_PWR_C7200_2026_08_18 !=
              IG_OBSERVED_PANEL_PWR_C720C_2026_08_18, true);

    //
    //  AND THE FIVE ANCHORED WORDS ARE DISTINCT FROM EACH OTHER, same reason,
    //  except SFUSE_STRAP which is legitimately zero and is therefore compared
    //  against the unprogrammed constant instead of against its siblings.
    //
    checkBool("the anchored words are pairwise distinct",
              IG_OBSERVED_CDCLK_CTL_2026_08_18 !=
              IG_OBSERVED_SKL_DFSM_2026_08_18 &&
              IG_OBSERVED_SKL_DFSM_2026_08_18 !=
              IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18 &&
              IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18 !=
              IG_OBSERVED_DPCLKA_CFGCR0_2026_08_18, true);
    checkBool("SFUSE_STRAP reads exactly the unprogrammed value",
              IG_OBSERVED_SFUSE_STRAP_2026_08_18 ==
              IG_OBSERVED_UNPROGRAMMED_2026_08_18, true);

    //
    //  LAST, the one thing that ties this block to the block above it: both
    //  came out of the SAME run, so a word here that equals a timing word
    //  there would mean somebody pasted across the two blocks.
    //
    checkBool("no configuration word collides with the panel's timing words",
              IG_OBSERVED_CDCLK_CTL_2026_08_18 !=
              IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18 &&
              IG_OBSERVED_SKL_DFSM_2026_08_18 !=
              IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18, true);
}

int main(void)
{
    std::printf("IntelDisplay.hpp -- Gen12 display decode\n");
    std::printf("Constants from reference/i915/ at Linux v6.12; see\n");
    std::printf("reference/i915/PROVENANCE.md. Nothing here is derived from a dump.\n");

    testTransBase();
    testPipeBase();
    testTransConf();
    testTimingReg();
    testPlusOne();
    testTransconfBits();
    testPlausible();
    testComposedRead();
    testPlaneAddress();
    testPlaneRefusals();
    testPlaneFields();
    testAxisConsistent();
    testModeConsistent();
    testTransDdi();
    testTransconfDdiLink();
    testPlaneJoin();
    testPipeUsable();
    testReadDecision();
    testSampleVerdict();
    testObservedCapture20260818();
    testObservedConfig20260818();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
