//
//  test_intel_sink_alloc.cpp
//  Host tests for IntelSinkAlloc.hpp: where in the GGTT the 3050 sink
//  surface goes, and the refusals that keep a bad plan from being mapped.
//

#include <cstdio>

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

static const IGS64 kGgtt4G = 0x100000000ull;

static bool untouched(const IgSinkAllocPlan &p)
{
    return p.base == 0xDEADBEEFu && p.stride == 0xDEADBEEFu
           && p.size == 0xDEADBEEFu;
}

static IgSinkAllocPlan sentinel()
{
    IgSinkAllocPlan p = { 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
    return p;
}

static void testRefusals()
{
    IgSinkAllocPlan p = sentinel();

    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 1920u, 1080u, 32u, 0)
              == IG_SINK_ALLOC_NULL_OUT,
          "null out refused");
    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 0u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_ZERO_GEOMETRY && untouched(p),
          "width 0 refused, out untouched");
    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 1920u, 0u, 32u, &p)
              == IG_SINK_ALLOC_ZERO_GEOMETRY && untouched(p),
          "height 0 refused, out untouched");
    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 1920u, 1080u, 24u, &p)
              == IG_SINK_ALLOC_BAD_BPP && untouched(p),
          "24 bpp refused, out untouched");
    checkTrue(igSinkAllocPlan(0x1000000ull, 0x1000001ull, 1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_RESERVED_PAST && untouched(p),
          "reserved end past the GGTT refused");

    // 1920x1080x4 = 8294400 bytes starting just below 4 GiB.
    checkTrue(igSinkAllocPlan(0x200000000ull, 0xFFFC0000ull, 1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_EXTENT_OVERFLOW && untouched(p),
          "extent past 32 bits refused even with a larger GGTT");
    checkTrue(igSinkAllocPlan(0x1000000ull, 0x900000ull, 1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_NO_ROOM && untouched(p),
          "surface running off a 16 MiB GGTT refused");
}

static void testPinnedOrder()
{
    IgSinkAllocPlan p = sentinel();

    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 0u, 1080u, 24u, &p)
              == IG_SINK_ALLOC_ZERO_GEOMETRY,
          "zero geometry reported before bad bpp");
    checkTrue(igSinkAllocPlan(0x1000000ull, 0x2000000ull, 1920u, 1080u, 24u, &p)
              == IG_SINK_ALLOC_BAD_BPP,
          "bad bpp reported before reserved-past");
    checkTrue(igSinkAllocPlan(kGgtt4G, 0xFFFC0000ull, 1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_EXTENT_OVERFLOW,
          "32-bit overflow reported before no-room");
}

static void testPlans()
{
    IgSinkAllocPlan p = sentinel();

    checkTrue(igSinkAllocPlan(kGgtt4G, 0x800001ull, 1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_OK,
          "1920x1080 fits in a 4 GiB GGTT");
    checkTrue(p.base == 0x840000u, "base rounded up to 256 KiB");
    checkTrue(p.stride == 7680u, "1920 * 4 stride kept as-is (already 64-aligned)");
    checkTrue(p.size == 7680u * 1080u, "size is stride * height");

    p = sentinel();
    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 100u, 10u, 32u, &p) == IG_SINK_ALLOC_OK
              && p.base == 0u && p.stride == 448u && p.size == 4480u,
          "400-byte row padded to 448, zero reserved keeps base 0");

    p = sentinel();
    checkTrue(igSinkAllocPlan(0x840000ull + 7680ull * 1080ull, 0x840000ull,
                          1920u, 1080u, 32u, &p)
              == IG_SINK_ALLOC_OK,
          "surface ending exactly at the GGTT end accepted");
}

//  Boundaries and order pairs the first draft left open.
static void testBoundaries()
{
    IgSinkAllocPlan p = sentinel();

    // reservedEnd == ggttSize is NOT "reserved past": the reserved range
    // ends on the last byte, so the refusal must be the later NO_ROOM arm.
    checkTrue(igSinkAllocPlan(0x1000000ull, 0x1000000ull, 1920u, 1080u, 32u, &p)
                  == IG_SINK_ALLOC_NO_ROOM && untouched(p),
              "reserved end exactly at the GGTT end is no-room, not reserved-past");

    p = sentinel();
    checkTrue(igSinkAllocPlan(kGgtt4G, 0x840000ull, 1920u, 1080u, 32u, &p)
                  == IG_SINK_ALLOC_OK && p.base == 0x840000u,
              "a base already on 256 KiB is not bumped a further step");

    p = sentinel();
    checkTrue(igSinkAllocPlan(kGgtt4G, 0, 0u, 1080u, 32u, 0)
                  == IG_SINK_ALLOC_NULL_OUT,
              "order: null out reported before zero geometry");

    // Both reserved-past AND an extent that cannot fit 32 bits.
    checkTrue(igSinkAllocPlan(kGgtt4G, kGgtt4G + 1ull, 1920u, 1080u, 32u, &p)
                  == IG_SINK_ALLOC_RESERVED_PAST && untouched(p),
              "order: reserved-past reported before extent overflow");

    // The placement the 3050 path will really ask for: past the firmware
    // plane observed 2026-08-16 (SURF 0x01058000, 7680 B * 1080 rows,
    // ending at 0x01841000), rounded up to the next 256 KiB.
    p = sentinel();
    checkTrue(igSinkAllocPlan(kGgtt4G, 0x01058000ull + 7680ull * 1080ull,
                              1920u, 1080u, 32u, &p)
                  == IG_SINK_ALLOC_OK && p.base == 0x01880000u
                  && p.stride == 7680u && p.size == 7680u * 1080u,
              "sink placed past the observed firmware plane at 0x01880000");
}
int main()
{
    testRefusals();
    testPinnedOrder();
    testPlans();
    testBoundaries();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}