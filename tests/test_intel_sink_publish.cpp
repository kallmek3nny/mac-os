//
//  test_intel_sink_publish.cpp
//
//  Host-side checks for IntelSinkPublish.hpp: the gate between a sound
//  iGPU sink surface and the descriptor the 3050 is allowed to DMA into.
//  Every refusal arm is reached by a singly-bad input, the fixed refusal
//  order is pinned by doubly-bad inputs, and every refusal is checked to
//  leave the descriptor zeroed.
//

#include <cstdio>

#include "../phase0-kext/IntelSinkPublish.hpp"

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

static void checkU64(const char *label, IGS64 got, IGS64 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok    %s\n", label);
    } else {
        ++gFailures;
        std::printf("  FAIL  %s: got %llu, want %llu\n", label,
                    (unsigned long long)got, (unsigned long long)want);
    }
}

// The same 1920x1080 XRGB8888 sink as test_intel_sink_surface.cpp, backed
// by a contiguous bus range at 2 GiB.
#define W       1920u
#define H       1080u
#define BPP     4u
#define STRIDE  7680u
#define BASE    0x00100000u
#define GGTT    0x10000000ull
#define BUS     0x80000000ull
#define EXTENT  ((IGS64)STRIDE * H)

// Aligned, but any nonzero extent runs past the top of the address space.
#define BUS_TOP 0xFFFFFFFFFFFFF000ull

static void poison(IGSinkDescriptor *d)
{
    d->busAddress    = 0xDEADBEEFull;
    d->extentBytes   = 0xDEADBEEFull;
    d->stride        = 0xDEADu;
    d->width         = 0xDEADu;
    d->height        = 0xDEADu;
    d->bytesPerPixel = 0xDEADu;
}

static int isZeroed(const IGSinkDescriptor *d)
{
    return d->busAddress == 0u && d->extentBytes == 0u && d->stride == 0u &&
           d->width == 0u && d->height == 0u && d->bytesPerPixel == 0u;
}

static IGS32 publish(IGS32 base, IGS64 bus, int contiguous, IGSinkDescriptor *d)
{
    poison(d);
    return igSinkPublishRefused(base, bus, contiguous, STRIDE, W, H, BPP,
                                GGTT, d);
}

static void testAccepts(void)
{
    IGSinkDescriptor d;

    section("1. a sound sink on a contiguous bus range is published");
    checkU32("1080p XRGB at bus 2 GiB",
             publish(BASE, BUS, 1, &d), IG_SCAN_OK);
    checkU64("descriptor bus address", d.busAddress, BUS);
    checkU64("descriptor extent is stride * height", d.extentBytes, EXTENT);
    checkU32("descriptor stride", d.stride, STRIDE);
    checkU32("descriptor width", d.width, W);
    checkU32("descriptor height", d.height, H);
    checkU32("descriptor bytes per pixel", d.bytesPerPixel, BPP);
    checkU32("range ending exactly at the top does not wrap",
             publish(BASE, 0ull - ((EXTENT + 4095u) & ~4095ull), 1, &d),
             IG_SCAN_OK);
}

static void testEveryArm(void)
{
    IGSinkDescriptor d;

    section("2. each refusal arm reached by a singly-bad input");
    checkU32("surface verdict passes through unchanged",
             publish(BASE + 1u, BUS, 1, &d), IG_SCAN_ERR_UNALIGNED);
    checkU32("  ...and zeroes the descriptor", isZeroed(&d), 1u);
    checkU32("caller does not claim contiguity",
             publish(BASE, BUS, 0, &d), IG_PUB_ERR_NOT_CONTIGUOUS);
    checkU32("  ...and zeroes the descriptor", isZeroed(&d), 1u);
    checkU32("zero bus address",
             publish(BASE, 0ull, 1, &d), IG_PUB_ERR_ZERO_BUS);
    checkU32("  ...and zeroes the descriptor", isZeroed(&d), 1u);
    checkU32("bus address off a page boundary",
             publish(BASE, BUS + 64u, 1, &d), IG_PUB_ERR_BUS_UNALIGNED);
    checkU32("  ...and zeroes the descriptor", isZeroed(&d), 1u);
    checkU32("bus range wraps the address space",
             publish(BASE, BUS_TOP, 1, &d), IG_PUB_ERR_BUS_WRAPS);
    checkU32("  ...and zeroes the descriptor", isZeroed(&d), 1u);
}

static void testOrder(void)
{
    IGSinkDescriptor d;

    section("3. refusal order is fixed");
    checkU32("bad surface beats not contiguous",
             publish(BASE + 1u, BUS, 0, &d), IG_SCAN_ERR_UNALIGNED);
    checkU32("not contiguous beats zero bus",
             publish(BASE, 0ull, 0, &d), IG_PUB_ERR_NOT_CONTIGUOUS);
    checkU32("unaligned beats wrap",
             publish(BASE, BUS_TOP + 64u, 1, &d), IG_PUB_ERR_BUS_UNALIGNED);
}

static void testRanges(void)
{
    section("4. verdict ranges do not collide");
    checkU32("first publish verdict follows the sink range",
             IG_PUB_ERR_NOT_CONTIGUOUS > IG_SINK_VERDICT_LIMIT, 1u);
    checkU32("publish limit is the last publish verdict",
             IG_PUB_VERDICT_LIMIT, IG_PUB_ERR_BUS_WRAPS);
}

int main(void)
{
    std::printf("IntelSinkPublish.hpp -- sink descriptor for the 3050\n");
    testAccepts();
    testEveryArm();
    testOrder();
    testRanges();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}