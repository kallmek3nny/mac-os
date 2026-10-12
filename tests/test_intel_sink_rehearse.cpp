//
//  test_intel_sink_rehearse.cpp
//  Host tests for IntelSinkRehearse.hpp: one firmware-plane sample in, a sink
//  plan and five plane words out, nothing written.
//
//  Discipline, same as the sibling sink suites: every reachable refusal arm hit
//  by a singly-bad input, the refusal order pinned by doubly-bad inputs, each
//  bound pinned at its last accepting value, and every composed field proven
//  to hold the NOT_REACHED sentinel unless its stage succeeded. The success
//  case is the REAL 2026-08-18 sample
//  (igpu-results/tgl-final-capture-2026-08-18-201412.txt:1090-1091),
//  so the expected numbers below are worked
//  by hand from the capture rather than from the header's own arithmetic.
//

#include <cstdio>
#include <cstring>

#include "IntelSinkRehearse.hpp"

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

//  The 2026-08-18 capture, pipe A plane 1. SURF differs from the 2026-08-16
//  IG_FB_SURF_OBSERVED value (0x01058000): firmware does not place the plane
//  at a fixed GGTT offset across boots, which is why the kext samples it
//  rather than reusing a constant.
static const IGFB32 kCtl    = 0x82009000u;   // enabled, XRGB8888, Y-tiled, RC (bit 15)
static const IGFB32 kStride = 0x0000003Cu;   // 60 * 128 = 7680 bytes
static const IGFB32 kSize   = 0x0437077Fu;   // 1920 x 1080
static const IGFB32 kSurf   = 0x01A7C000u;
static const IGFB32 kAux    = 0x007F8000u;

//  A FIXTURE, not a measurement: 256 MiB, the aperture the fb-present suite
//  also uses. No BAR2 length for this laptop is recorded in the tree.
static const IGS64  kAperture = 0x10000000ull;

//  Worked by hand from the five words above:
//    main bytes  = 7680 * 1080            = 0x007E9000
//    main end    = 0x01A7C000 + 0x7E9000  = 0x02265000
//    aux end     = 0x01A7C000 + 0x7F8000 + 0x7E9000 = 0x02A5D000  (the larger)
//    sink base   = align_up(0x02A5D000, 0x40000)    = 0x02A80000
//    sink stride = 1920 * 4 = 7680 (already a multiple of 64)
//    sink bytes  = 7680 * 1080            = 0x007E9000
//    sink end    = 0x02A80000 + 0x7E9000  = 0x03269000
static const IGS64  kMainEnd     = 0x02265000ull;
static const IGS64  kReservedEnd = 0x02A5D000ull;
static const IGS32  kSinkBase    = 0x02A80000u;
static const IGS32  kSinkStride  = 7680u;
static const IGS32  kSinkBytes   = 0x007E9000u;
static const IGS64  kSinkEnd     = 0x03269000ull;

static const IGS32  NR = IG_SINK_REHEARSE_NOT_REACHED;

static IGS32 run(IGFB32 ctl, IGFB32 stride, IGFB32 size, IGFB32 surf,
                 IGFB32 aux, IGS64 aperture, IgSinkRehearsal *r)
{
    std::memset(r, 0xA5, sizeof *r);   // poison: proves every field is written
    return igSinkRehearse(ctl, stride, size, surf, aux, aperture, r);
}

static bool planUnreached(const IgSinkRehearsal &r)
{
    return r.plan.base == NR && r.plan.stride == NR && r.plan.size == NR;
}

static bool regsUnreached(const IgSinkRehearsal &r)
{
    return r.regs.ctl == NR && r.regs.stride == NR && r.regs.pos == NR
        && r.regs.size == NR && r.regs.surf == NR;
}

//  1. The real sample, end to end.
static void testObservedSample()
{
    IgSinkRehearsal r;
    checkTrue(run(kCtl, kStride, kSize, kSurf, kAux, kAperture, &r)
              == IG_SINK_REHEARSE_OK, "observed: rehearsal composes");
    checkTrue(r.allocResult == IG_SINK_ALLOC_OK, "observed: alloc stage OK");
    checkTrue(r.planeVerdict == IG_PLANE_OK, "observed: plane stage OK");
    checkTrue(r.reservedEnd == kReservedEnd,
              "observed: reserved end is past the AUX surface, not the main one");
    checkTrue(r.reservedEnd > kMainEnd,
              "observed: the aux surface moved the reserved end");
    checkTrue(r.plan.base == kSinkBase, "observed: sink base 0x02A80000");
    checkTrue(r.plan.stride == kSinkStride, "observed: sink stride 7680");
    checkTrue(r.plan.size == kSinkBytes, "observed: sink bytes 0x7E9000");
    checkTrue((IGS64) r.plan.base >= (IGS64) kSurf + kAux + 0x7E9000ull,
              "observed: sink starts at or past the end of the aux reservation");
    checkTrue((IGS64) r.plan.base + r.plan.size == kSinkEnd,
              "observed: sink ends at 0x03269000");

    //  The five words the kext WOULD write. CTL keeps every non-owned bit of
    //  the firmware word (bit 25 is format 4's top bit, so only owned bits
    //  move): ENABLE stays, format 4 stays, tiling Y (bit 12) and RC (bit 15)
    //  are cleared -> 0x82000000.
    checkTrue(r.regs.ctl == 0x82000000u, "observed: CTL 0x82000000");
    checkTrue((r.regs.ctl & IG_FB_CTL_RENDER_DECOMP) == 0u,
              "observed: the sink plane is NOT render-compressed");
    checkTrue(igFbTiling(r.regs.ctl) == IG_FB_TILING_LINEAR,
              "observed: the sink plane is linear");
    checkTrue(igFbFormat(r.regs.ctl) == igFbFormat(kCtl),
              "observed: the sink keeps the firmware format");
    checkTrue(r.regs.stride == 120u, "observed: STRIDE 7680 / 64 = 120");
    checkTrue(r.regs.size == kSize, "observed: SIZE copied from firmware");
    checkTrue(r.regs.pos == 0u, "observed: POS top-left");
    checkTrue(r.regs.surf == kSinkBase, "observed: SURF is the sink base");
}

//  2. The reserved end, on its own.
static void testReservedEnd()
{
    const IGFB32 plainCtl = kCtl & ~IG_FB_CTL_RENDER_DECOMP;

    checkTrue(igSinkRehearseReservedEnd(plainCtl, kStride, kSize, kSurf, kAux)
              == kMainEnd,
              "reserved: an uncompressed plane ignores AUX_DIST");
    checkTrue(igSinkRehearseReservedEnd(plainCtl, kStride, kSize, kSurf,
                                        0xFFFFF000u) == kMainEnd,
              "reserved: even a huge stale AUX_DIST is ignored when uncompressed");
    checkTrue(igSinkRehearseReservedEnd(kCtl, kStride, kSize, kSurf, 0u)
              == kMainEnd,
              "reserved: aux at distance 0 cannot end before the main surface");
    checkTrue(igSinkRehearseReservedEnd(kCtl, kStride, kSize, kSurf,
                                        kAux | 0xFFFu) == kReservedEnd,
              "reserved: AUX_STRIDE bits 11:0 are not a distance");

    //  64-bit, not wrapped: top-of-GGTT surface plus a top distance.
    const IGS64 hi = igSinkRehearseReservedEnd(kCtl, kStride, kSize,
                                               0xFFFFF000u, 0xFFFFF000u);
    checkTrue(hi == 0xFFFFF000ull + 0xFFFFF000ull + 0x7E9000ull,
              "reserved: SURF + AUX_DIST + extent is summed in 64 bits");
    checkTrue(hi > 0x100000000ull, "reserved: the sum really passes 2^32");
}

//  3. Every reachable refusal arm, singly bad.
static void testRefusalArms()
{
    IgSinkRehearsal r;

    checkTrue(igSinkRehearse(kCtl, kStride, kSize, kSurf, kAux, kAperture, 0)
              == IG_SINK_REHEARSE_NULL_OUT, "refuse: null out");

    const IGFB32 w[5] = { kCtl, kStride, kSize, kSurf, kAux };
    for (int i = 0; i < 5; ++i) {
        IGFB32 v[5];
        for (int j = 0; j < 5; ++j)
            v[j] = (i == j) ? IG_FB_INVALID : w[j];
        const bool refused = run(v[0], v[1], v[2], v[3], v[4], kAperture, &r)
                             == IG_SINK_REHEARSE_SAMPLE_REFUSED;
        const bool clean = r.allocResult == NR && r.planeVerdict == NR
                           && r.reservedEnd == IG_SINK_REHEARSE_INVALID64
                           && planUnreached(r) && regsUnreached(r);
        checkTrue(refused, "refuse: each of the five words as the sentinel");
        checkTrue(clean, "refuse: sample refusal leaves every field NOT_REACHED");
    }

    checkTrue(run(kCtl & ~IG_FB_CTL_ENABLE, kStride, kSize, kSurf, kAux,
                  kAperture, &r) == IG_SINK_REHEARSE_PLANE_DISABLED,
              "refuse: firmware plane disabled");
    checkTrue(planUnreached(r) && regsUnreached(r)
              && r.reservedEnd == IG_SINK_REHEARSE_INVALID64,
              "refuse: disabled plane composes nothing");

    checkTrue(run(kCtl, kStride, kSize, kSurf, kAux, 0u, &r)
              == IG_SINK_REHEARSE_NO_BOUND, "refuse: no aperture measured");
    checkTrue(planUnreached(r) && regsUnreached(r) && r.allocResult == NR,
              "refuse: no-bound composes nothing");

    //  Alloc refusals surface as ALLOC_REFUSED with the allocator's reason
    //  carried, the reserved end published, and nothing past it composed.
    checkTrue(run(kCtl, kStride, kSize, kSurf, kAux, kReservedEnd - 1u, &r)
              == IG_SINK_REHEARSE_ALLOC_REFUSED
              && r.allocResult == IG_SINK_ALLOC_RESERVED_PAST,
              "refuse: aperture ends inside the firmware reservation");
    checkTrue(r.reservedEnd == kReservedEnd,
              "refuse: alloc refusal still reports the reserved end it judged");
    checkTrue(planUnreached(r) && regsUnreached(r) && r.planeVerdict == NR,
              "refuse: alloc refusal composes no plan and no plane words");

    checkTrue(run(kCtl, kStride, kSize, kSurf, kAux, kSinkEnd - 1u, &r)
              == IG_SINK_REHEARSE_ALLOC_REFUSED
              && r.allocResult == IG_SINK_ALLOC_NO_ROOM,
              "refuse: sink one byte too big for the aperture");

    //  A firmware format that is not 32 bpp: the sink does not convert it.
    const IGFB32 fmtMask = IG_FB_CTL_FORMAT_MASK << IG_FB_CTL_FORMAT_SHIFT;
    const IGFB32 rgb565 = (kCtl & ~fmtMask)
                        | (IG_FB_FORMAT_RGB_565 << IG_FB_CTL_FORMAT_SHIFT);
    checkTrue(run(rgb565, kStride, kSize, kSurf, kAux, kAperture, &r)
              == IG_SINK_REHEARSE_ALLOC_REFUSED
              && r.allocResult == IG_SINK_ALLOC_BAD_BPP,
              "refuse: RGB565 firmware plane is refused, not converted");
    checkTrue(igFbBytesPerPixel(0x1Fu) == 0u,
              "fixture: format 0x1F has no byte size");
    checkTrue(run(kCtl | fmtMask, kStride, kSize, kSurf, kAux, kAperture, &r)
              == IG_SINK_REHEARSE_ALLOC_REFUSED
              && r.allocResult == IG_SINK_ALLOC_BAD_BPP,
              "refuse: an undecodable format is refused at the allocator");

    //  The overflow arm, reached by a reservation past 2^32.
    checkTrue(run(kCtl, kStride, kSize, 0xFFFFF000u, 0xFFFFF000u,
                  0x100000000ull, &r) == IG_SINK_REHEARSE_ALLOC_REFUSED
              && r.allocResult == IG_SINK_ALLOC_RESERVED_PAST,
              "refuse: a reservation past 4 GiB never wraps into a plan");
}

//  4. Last accepting values.
static void testLastAccepting()
{
    IgSinkRehearsal r;
    checkTrue(run(kCtl, kStride, kSize, kSurf, kAux, kSinkEnd, &r)
              == IG_SINK_REHEARSE_OK,
              "accept: an aperture ending exactly on the sink's last byte");
    checkTrue(r.plan.base == kSinkBase, "accept: same plan at the tight bound");
}

//  5. Order, by doubly-bad inputs.
static void testOrder()
{
    IgSinkRehearsal r;
    checkTrue(igSinkRehearse(IG_FB_INVALID, kStride, kSize, kSurf, kAux, 0u, 0)
              == IG_SINK_REHEARSE_NULL_OUT, "order: null out beats everything");
    checkTrue(run(kCtl & ~IG_FB_CTL_ENABLE, IG_FB_INVALID, kSize, kSurf, kAux,
                  kAperture, &r) == IG_SINK_REHEARSE_SAMPLE_REFUSED,
              "order: sample refusal beats a disabled plane");
    checkTrue(run(kCtl & ~IG_FB_CTL_ENABLE, kStride, kSize, kSurf, kAux, 0u, &r)
              == IG_SINK_REHEARSE_PLANE_DISABLED,
              "order: disabled plane beats no aperture");
    const IGFB32 fmtMask = IG_FB_CTL_FORMAT_MASK << IG_FB_CTL_FORMAT_SHIFT;
    checkTrue(run(kCtl | fmtMask, kStride, kSize, kSurf, kAux, 0u, &r)
              == IG_SINK_REHEARSE_NO_BOUND,
              "order: no aperture beats a bad format");
}

//  6. The two headers AGREE: whenever the allocator accepts, the plane builder
//  accepts its plan. That is why PLANE_REFUSED has no singly-bad input -- it
//  is a defensive arm, and this sweep is the evidence it stays unreachable.
static void testAllocPlaneAgreement()
{
    static const IGFB32 sizes[] = { 0x0437077Fu, 0x031F04FFu, 0x00000000u,
                                    0x0FFF0FFFu, 0x1FFF1FFFu, 0x00001FFFu };
    static const IGFB32 surfs[] = { 0x00000000u, kSurf, 0x7FFFF000u,
                                    0xEFFFF000u };
    static const IGS64 apertures[] = { 0x10000000ull, 0x40000000ull,
                                       0x100000000ull };
    int composed = 0;
    bool agree = true;
    for (unsigned a = 0; a < sizeof sizes / sizeof sizes[0]; ++a)
        for (unsigned b = 0; b < sizeof surfs / sizeof surfs[0]; ++b)
            for (unsigned c = 0; c < sizeof apertures / sizeof apertures[0]; ++c)
                for (IGFB32 st = 0x10u; st <= 0x200u; st += 0x2Cu) {
                    IgSinkRehearsal r;
                    const IGS32 v = run(kCtl, st, sizes[a], surfs[b], kAux,
                                        apertures[c], &r);
                    if (v == IG_SINK_REHEARSE_PLANE_REFUSED)
                        agree = false;
                    if (v == IG_SINK_REHEARSE_OK)
                        ++composed;
                }
    checkTrue(agree, "agree: no accepted plan is refused by the plane builder");
    checkTrue(composed > 0, "agree: the sweep composed at least one plan");
}

//  7. Names, walked against the family's own inclusive limit.
static void testNames()
{
    bool named = true, distinct = true;
    for (IGS32 v = 0; v <= IG_SINK_REHEARSE_VERDICT_LIMIT; ++v) {
        if (std::strcmp(igSinkRehearseVerdictName(v), "unknown") == 0)
            named = false;
        for (IGS32 u = 0; u < v; ++u)
            if (std::strcmp(igSinkRehearseVerdictName(u),
                            igSinkRehearseVerdictName(v)) == 0)
                distinct = false;
    }
    checkTrue(named, "names: every verdict up to the limit has a name");
    checkTrue(distinct, "names: no two verdicts share a name");
    checkTrue(std::strcmp(igSinkRehearseVerdictName(
                  IG_SINK_REHEARSE_VERDICT_LIMIT + 1u), "unknown") == 0,
              "names: one past the limit is unknown");
    checkTrue(std::strcmp(igSinkRehearseVerdictName(
                  IG_SINK_REHEARSE_VERDICT_LIMIT), "plane") == 0,
              "names: the inclusive limit names the last real verdict");

    bool onlyOk = igSinkRehearseUsable(IG_SINK_REHEARSE_OK) != 0;
    for (IGS32 v = 1; v <= IG_SINK_REHEARSE_VERDICT_LIMIT + 1u; ++v)
        if (igSinkRehearseUsable(v))
            onlyOk = false;
    checkTrue(onlyOk, "usable: true for OK and for nothing else");
}

int main()
{
    testObservedSample();
    testReservedEnd();
    testRefusalArms();
    testLastAccepting();
    testOrder();
    testAllocPlaneAgreement();
    testNames();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
