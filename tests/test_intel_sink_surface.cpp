//
//  test_intel_sink_surface.cpp
//
//  Host unit test for IntelSinkSurface.hpp: every refusal arm reached by a
//  singly-bad input, the refusal order pinned by doubly-bad inputs, and the
//  last accepting value of each numeric test.
//
//  The MEASURED entry from the real firmware plane (handoff40 §7 (a)) is
//  still owed: it needs a boot log of PLANE_SURF/PLANE_STRIDE on the target.
//

#include <cstdio>

#include "../phase0-kext/IntelSinkSurface.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
}

static void checkU32(const char *label, IGS32 got, IGS32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok    %s\n", label);
    } else {
        ++gFailures;
        std::printf("  FAIL  %s: got %u, want %u\n", label, got, want);
    }
}

// A 1920x1080 XRGB8888 surface at 1 MiB into a 256 MiB GGTT.
#define W       1920u
#define H       1080u
#define BPP     4u
#define STRIDE  7680u
#define BASE    0x00100000u
#define GGTT    0x10000000ull

static void testAccepts(void)
{
    section("1. a plain 1080p sink is accepted");
    checkU32("1080p XRGB at 1 MiB",
             igSinkSurfaceRefused(BASE, STRIDE, W, H, BPP, GGTT), IG_SCAN_OK);
    checkU32("base zero is a legal GGTT offset",
             igSinkSurfaceRefused(0u, STRIDE, W, H, BPP, GGTT), IG_SCAN_OK);
}

static void testEveryArm(void)
{
    section("2. each refusal arm reached by a singly-bad input");
    checkU32("unaligned base",
             igSinkSurfaceRefused(BASE + 0x800u, STRIDE, W, H, BPP, GGTT),
             IG_SCAN_ERR_UNALIGNED);
    checkU32("zero width",
             igSinkSurfaceRefused(BASE, STRIDE, 0u, H, BPP, GGTT),
             IG_SCAN_ERR_ZERO_GEOMETRY);
    checkU32("zero height",
             igSinkSurfaceRefused(BASE, STRIDE, W, 0u, BPP, GGTT),
             IG_SCAN_ERR_ZERO_GEOMETRY);
    checkU32("zero stride",
             igSinkSurfaceRefused(BASE, 0u, W, H, BPP, GGTT),
             IG_SCAN_ERR_ZERO_GEOMETRY);
    checkU32("zero bytes per pixel",
             igSinkSurfaceRefused(BASE, STRIDE, W, H, 0u, GGTT),
             IG_SCAN_ERR_ZERO_GEOMETRY);
    checkU32("row one unit short",
             igSinkSurfaceRefused(BASE, STRIDE - 64u, W, H, BPP, GGTT),
             IG_SCAN_ERR_ROW_TOO_SHORT);
    checkU32("stride not a multiple of 64",
             igSinkSurfaceRefused(BASE, STRIDE + 4u, W, H, BPP, GGTT),
             IG_SINK_ERR_STRIDE_UNITS);
    checkU32("extent past the 32-bit ceiling",
             igSinkSurfaceRefused(0u, 0x00100000u, 1024u, 0x2000u, BPP,
                                  0x1000000000ull),
             IG_SCAN_ERR_EXTENT_OVERFLOW);
    checkU32("surface runs off the GGTT",
             igSinkSurfaceRefused(BASE, STRIDE, W, H, BPP,
                                  (IGS64)BASE + STRIDE * H - 1u),
             IG_SCAN_ERR_OUTSIDE_APERTURE);
}

static void testEdges(void)
{
    section("3. the last accepting value of each numeric test");
    checkU32("surface ends exactly at the GGTT end",
             igSinkSurfaceRefused(BASE, STRIDE, W, H, BPP,
                                  (IGS64)BASE + STRIDE * H),
             IG_SCAN_OK);
    checkU32("stride exactly the minimum row",
             igSinkSurfaceRefused(BASE, W * BPP, W, H, BPP, GGTT),
             IG_SCAN_OK);
    checkU32("stride padded by one unit",
             igSinkSurfaceRefused(BASE, STRIDE + 64u, W, H, BPP, GGTT),
             IG_SCAN_OK);
}

static void testOrder(void)
{
    section("4. refusal order, pinned by doubly-bad inputs");
    checkU32("unaligned beats zero geometry",
             igSinkSurfaceRefused(BASE + 4u, STRIDE, 0u, H, BPP, GGTT),
             IG_SCAN_ERR_UNALIGNED);
    checkU32("zero geometry beats short row",
             igSinkSurfaceRefused(BASE, 4u, W, 0u, BPP, GGTT),
             IG_SCAN_ERR_ZERO_GEOMETRY);
    checkU32("short row beats stride units",
             igSinkSurfaceRefused(BASE, 100u, W, H, BPP, GGTT),
             IG_SCAN_ERR_ROW_TOO_SHORT);
    checkU32("stride units beats aperture",
             igSinkSurfaceRefused(BASE, STRIDE + 4u, W, H, BPP, 0x1000ull),
             IG_SINK_ERR_STRIDE_UNITS);
    checkU32("extent overflow beats aperture",
             igSinkSurfaceRefused(0u, 0x00100000u, 1024u, 0x2000u, BPP,
                  0x1000ull),
             IG_SCAN_ERR_EXTENT_OVERFLOW);
    checkU32("verdict limit sits past the scanout range",
             IG_SINK_VERDICT_LIMIT, IG_SCAN_VERDICT_LIMIT + 1u);
}

int main(void)
{
    std::printf("IntelSinkSurface.hpp -- driver-owned scanout sink\n");
    testAccepts();
    testEveryArm();
    testEdges();
    testOrder();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}