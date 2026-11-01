// SPDX-License-Identifier: MIT
#include "IntelPlaneProgramPlan.hpp"

#include <cstdio>
#include <cstring>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void check(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

static IgPlaneLiveSample observedSample()
{
    IgPlaneLiveSample sample = {};
    sample.deviceId = 0x9A68u;
    sample.pipe = IG_TRANSCODER_A;
    sample.surfLiveBefore = 0x01058000u;
    sample.transConf = 0xC0000000u;
    sample.pipeSrc = 0x077F0437u;
    sample.ctl = 0x82009000u;
    sample.stride = 0x0000003Cu;
    sample.pos = 0x00000000u;
    sample.size = 0x0437077Fu;
    sample.surf = 0x01058000u;
    sample.offset = 0x00000000u;
    sample.surfLiveAfter = 0x01058000u;
    return sample;
}

static IgPlaneLiveObservation goldenObservation()
{
    const IgPlaneLiveSample sample = observedSample();
    IgPlaneLiveObservation out = {};
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_OK,
          "golden live observation accepted");
    return out;
}

static const IGGgttU64 kSurfaceBytes =
    (IGGgttU64)1920u * (IGGgttU64)1080u * 4ull;
static const IGGgttU64 kGgttBase = 0x01880000ull;

static IGSinkGgttPlan goldenMapping()
{
    IGSinkGgttPlan out = {};
    const IGGgttU32 verdict = igSinkGgttPlan(
        kGgttBase,
        kSurfaceBytes,
        0x0000000100000000ull,
        0x01000000ull,
        0x00100000u,
        &out);
    check(verdict == IG_SINK_GGTT_PLAN_OK, "golden GGTT mapping plan accepted");
    return out;
}

static IgPlaneProgramRequest goldenRequest()
{
    IgPlaneProgramRequest request = {};
    request.deviceId = 0x9A68u;
    request.pipe = IG_TRANSCODER_A;
    request.plane = IG_PLANE_PROGRAM_PLAN_PRIMARY_PLANE;
    request.format = IG_PLANE_PROGRAM_PLAN_FORMAT_XRGB8888;
    request.tiling = IG_PLANE_PROGRAM_PLAN_TILING_LINEAR;
    request.rotation = IG_PLANE_PROGRAM_PLAN_ROTATION_0;
    request.sourceX = 0u;
    request.sourceY = 0u;
    request.sourceWidth = 1920u;
    request.sourceHeight = 1080u;
    request.destX = 0u;
    request.destY = 0u;
    request.destWidth = 1920u;
    request.destHeight = 1080u;
    request.strideBytes = 1920u * 4u;
    request.surfaceBytes = kSurfaceBytes;
    request.enable = true;
    request.renderCompression = false;
    request.asyncFlip = false;
    request.ggttMappingOwned = true;
    request.ggttMappingCommitted = true;
    request.ggttReadbackExact = true;
    request.ggttQuarantined = false;
    return request;
}

static void poison(IgPlaneProgramPlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const IgPlaneProgramPlan &a, const IgPlaneProgramPlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const IgPlaneLiveObservation &prior,
                          const IGSinkGgttPlan &mapping,
                          const IgPlaneProgramRequest &request,
                          IGU32 expected,
                          const char *what)
{
    IgPlaneProgramPlan out = {};
    poison(&out);
    const IgPlaneProgramPlan before = out;
    const IGU32 verdict = igPlaneProgramPlan(&prior, &mapping, &request, &out);
    check(verdict == expected, what);
    check(sameBytes(out, before), "refusal leaves output untouched");
}

static void testGolden()
{
    std::puts("1. coherent observed pipe + owned linear mapping yields semantic plan");
    const IgPlaneLiveObservation prior = goldenObservation();
    const IGSinkGgttPlan mapping = goldenMapping();
    const IgPlaneProgramRequest request = goldenRequest();
    IgPlaneProgramPlan out = {};

    const IGU32 verdict = igPlaneProgramPlan(&prior, &mapping, &request, &out);
    check(verdict == IG_PLANE_PROGRAM_PLAN_OK, "golden request accepted");
    check(out.valid, "plan valid");
    check(out.deviceId == 0x9A68u, "exact target retained");
    check(out.pipe == IG_TRANSCODER_A, "pipe A retained");
    check(out.plane == 0u, "primary plane retained");
    check(out.width == 1920u && out.height == 1080u, "panel geometry retained");
    check(out.strideBytes == 7680u, "tight linear stride retained");
    check(out.linearStrideUnits == 120u,
          "linear 7680-byte stride uses source-backed 64-byte units");
    check(out.visibleBytes == kSurfaceBytes, "visible byte extent exact");
    check(out.surfaceBytes == kSurfaceBytes, "surface extent retained");
    check(out.mappedBytes >= out.surfaceBytes, "GGTT mapping covers surface");
    check(out.mappingPageCount == mapping.pteCount, "mapping page count retained");
    check(out.enable && out.fullPipe && out.noScaling && out.uncompressed,
          "narrow full-pipe semantic contract retained");
    check(out.synchronousOnly, "async flip remains disallowed");
    check(out.mappingOwnershipRequired && out.mappingPinRequired,
          "ownership and lifetime requirements retained");
    check(out.sequencingProofRequired && out.latchProofRequired,
          "future sequencing/latch proof explicitly still required");
    check(out.rollbackOrQuarantineRequired,
          "rollback-or-quarantine requirement retained");
    check(!out.runtimeWriteAuthorized,
          "pure plan never authorizes runtime display writes");
}

static void testNullsAndIdentity()
{
    std::puts("2. nulls and exact hardware relationship are fail-closed");
    const IgPlaneLiveObservation prior = goldenObservation();
    const IGSinkGgttPlan mapping = goldenMapping();
    const IgPlaneProgramRequest request = goldenRequest();
    IgPlaneProgramPlan out = {};

    check(igPlaneProgramPlan(&prior, &mapping, &request, nullptr) ==
              IG_PLANE_PROGRAM_PLAN_ERR_NULL_OUTPUT,
          "null output refused");
    check(igPlaneProgramPlan(nullptr, &mapping, &request, &out) ==
              IG_PLANE_PROGRAM_PLAN_ERR_NULL_OBSERVATION,
          "null observation refused");
    check(igPlaneProgramPlan(&prior, nullptr, &request, &out) ==
              IG_PLANE_PROGRAM_PLAN_ERR_NULL_MAPPING,
          "null mapping refused");
    check(igPlaneProgramPlan(&prior, &mapping, nullptr, &out) ==
              IG_PLANE_PROGRAM_PLAN_ERR_NULL_REQUEST,
          "null request refused");

    auto r = request;
    r.deviceId = 0x9A40u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_WRONG_DEVICE,
                  "nearby Tiger Lake id refused");

    r = request;
    r.pipe = IG_TRANSCODER_B;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PIPE,
                  "unobserved pipe B programming refused");

    r = request;
    r.plane = 1u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_WRONG_PLANE,
                  "secondary plane programming refused");
}

static void testPriorCoherency()
{
    std::puts("3. planner independently rechecks live-observer success facts");
    const IGSinkGgttPlan mapping = goldenMapping();
    const IgPlaneProgramRequest request = goldenRequest();

    auto prior = goldenObservation();
    prior.valid = false;
    expectRefusal(prior, mapping, request, IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR,
                  "invalid prior refused");

    prior = goldenObservation();
    prior.liveStable = false;
    expectRefusal(prior, mapping, request, IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR,
                  "unstable prior refused");

    prior = goldenObservation();
    prior.programmedSurfaceWord ^= 0x1000u;
    expectRefusal(prior, mapping, request, IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR,
                  "programmed/live surface mismatch refused");

    prior = goldenObservation();
    prior.pipeSrcWord = 0x077E0437u;
    expectRefusal(prior, mapping, request, IG_PLANE_PROGRAM_PLAN_ERR_INCOHERENT_PRIOR,
                  "prior raw geometry mismatch refused");
}

static void testModeRestrictions()
{
    std::puts("4. unsupported format/layout/transform/update modes are explicit refusals");
    const auto prior = goldenObservation();
    const auto mapping = goldenMapping();
    auto r = goldenRequest();

    r.format = 2u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_FORMAT,
                  "non-XRGB8888 format refused");
    r = goldenRequest();
    r.tiling = 2u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_TILING,
                  "non-linear tiling refused");
    r = goldenRequest();
    r.rotation = 180u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_ROTATION,
                  "rotation refused");
    r = goldenRequest();
    r.enable = false;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_DISABLED,
                  "disabled target refused");
    r = goldenRequest();
    r.renderCompression = true;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_COMPRESSION,
                  "render compression refused");
    r = goldenRequest();
    r.asyncFlip = true;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_ASYNC_FLIP,
                  "async flip refused");
}

static void testGeometryAndStride()
{
    std::puts("5. full-pipe 1:1 geometry and linear pitch are pinned");
    const auto prior = goldenObservation();
    const auto mapping = goldenMapping();
    auto r = goldenRequest();

    r.sourceX = 1u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_ORIGIN,
                  "source crop origin refused");
    r = goldenRequest();
    r.destY = 1u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_DEST_ORIGIN,
                  "destination offset refused");
    r = goldenRequest();
    r.sourceWidth = 1919u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_SOURCE_GEOMETRY,
                  "source width must match coherent pipe geometry");
    r = goldenRequest();
    r.destWidth = 0u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_DEST_GEOMETRY,
                  "zero destination width refused");
    r = goldenRequest();
    r.destWidth = 1919u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_SCALING,
                  "scaling refused");
    r = goldenRequest();
    r.strideBytes += 64u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_STRIDE,
                  "non-tight linear stride refused by narrow policy");
    r = goldenRequest();
    r.strideBytes += 1u;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_STRIDE,
                  "non-64-byte linear stride refused");
}

static void testMappingSafety()
{
    std::puts("6. ownership, exact-readback, alignment, shape and bounds are mandatory");
    const auto prior = goldenObservation();
    const auto mapping = goldenMapping();
    auto r = goldenRequest();

    r.ggttMappingOwned = false;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE,
                  "unowned mapping refused");
    r = goldenRequest();
    r.ggttMappingCommitted = false;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE,
                  "uncommitted mapping refused");
    r = goldenRequest();
    r.ggttReadbackExact = false;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE,
                  "mapping without exact readback refused");
    r = goldenRequest();
    r.ggttQuarantined = true;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_STATE,
                  "quarantined uncertain mapping refused for new programming");

    auto m = mapping;
    m.ggttBase += 1ull;
    expectRefusal(prior, m, goldenRequest(),
                  IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_ALIGNMENT,
                  "unaligned plane surface refused");

    m = mapping;
    ++m.firstPte;
    expectRefusal(prior, m, goldenRequest(),
                  IG_PLANE_PROGRAM_PLAN_ERR_MAPPING_SHAPE,
                  "GGTT base/first-PTE mismatch refused");

    m = mapping;
    m.firstPte = 0x00100000u;
    m.ggttBase = 0x0000000100000000ull;
    expectRefusal(prior, m, goldenRequest(),
                  IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_REGISTER_RANGE,
                  "mapping above 32-bit PLANE_SURF range refused");

    r = goldenRequest();
    r.surfaceBytes = mapping.mappedBytes + 1ull;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_SURFACE_SIZE,
                  "declared surface larger than owned mapping refused");

    r = goldenRequest();
    r.surfaceBytes = kSurfaceBytes - 1ull;
    expectRefusal(prior, mapping, r, IG_PLANE_PROGRAM_PLAN_ERR_FRAMEBUFFER_BOUNDS,
                  "surface one byte shorter than visible framebuffer refused");
}

static void testVerdictNamesAndAtomicity()
{
    std::puts("7. verdict names are stable and refusals remain atomic");
    for (IGU32 v = 0u; v <= IG_PLANE_PROGRAM_PLAN_VERDICT_LIMIT; ++v) {
        const char *name = igPlaneProgramPlanVerdictName(v);
        check(name != nullptr && std::strcmp(name, "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(igPlaneProgramPlanVerdictName(
                          IG_PLANE_PROGRAM_PLAN_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");

    const auto prior = goldenObservation();
    const auto mapping = goldenMapping();
    auto r = goldenRequest();
    r.asyncFlip = true;
    IgPlaneProgramPlan out = {};
    poison(&out);
    const IgPlaneProgramPlan before = out;
    check(igPlaneProgramPlan(&prior, &mapping, &r, &out) ==
              IG_PLANE_PROGRAM_PLAN_ERR_ASYNC_FLIP,
          "atomic refusal trigger");
    check(sameBytes(out, before), "atomic refusal leaves entire plan untouched");
}

int main()
{
    testGolden();
    testNullsAndIdentity();
    testPriorCoherency();
    testModeRestrictions();
    testGeometryAndStride();
    testMappingSafety();
    testVerdictNamesAndAtomicity();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
