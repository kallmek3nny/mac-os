// SPDX-License-Identifier: MIT
#include "IntelPlaneLiveObserve.hpp"

#include <cstdint>
#include <iostream>
#include <string>

static unsigned gFailures = 0u;

static void check(bool cond, const std::string &what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
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

static void checkGolden()
{
    std::puts("1. observed Tiger Lake primary-plane snapshot is coherent");
    const auto sample = observedSample();
    IgPlaneLiveObservation out = {};
    const auto verdict = igPlaneLiveObserve(&sample, &out);

    check(verdict == IG_PLANE_LIVE_OK, "observed fixture accepted");
    check(out.valid, "observation valid");
    check(out.deviceId == 0x9A68u, "exact observed device retained");
    check(out.pipe == IG_TRANSCODER_A, "pipe A retained");
    check(out.width == 1920u, "plane width 1920");
    check(out.height == 1080u, "plane height 1080");
    check(out.strideBytes == 7680u, "stride decodes to 7680 bytes");
    check(out.programmedSurfaceWord == 0x01058000u, "programmed SURF retained");
    check(out.liveSurfaceWord == 0x01058000u, "live SURF retained");
    check(out.programmedSurfaceOffset == 0x01058000u, "GGTT surface offset decoded");
    check(out.transcoderEnabled, "transcoder enable recorded");
    check(out.transcoderStateEnabled, "transcoder state enable recorded");
    check(out.planeEnabled, "plane enable recorded");
    check(out.liveStable, "bracketing SURFLIVE reads stable");
    check(out.programmedSurfaceIsLive, "programmed SURF is live");

    // The observed firmware plane is render-compressed. That makes it
    // non-inheritable by the current framebuffer path, but it must remain a
    // valid live-state observation rather than being erased from evidence.
    check(out.inheritVerdict == IG_FB_INHERIT_COMPRESSED,
          "compressed firmware plane remains observable with refusal metadata");
}

static void checkNullsAndExactDeviceGate()
{
    std::puts("2. nulls, exact device identity, and pipe range are refused");
    auto sample = observedSample();
    IgPlaneLiveObservation out = {};

    check(igPlaneLiveObserve(&sample, nullptr) == IG_PLANE_LIVE_ERR_NULL_OUTPUT,
          "null output refused");
    check(igPlaneLiveObserve(nullptr, &out) == IG_PLANE_LIVE_ERR_NULL_SAMPLE,
          "null sample refused");

    sample.deviceId = 0x9A60u; // supported TGL GT1, but not the observed target
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_WRONG_DEVICE,
          "nearby supported Tiger Lake id does not inherit 9A68 evidence");

    sample = observedSample();
    sample.deviceId = 0x9A40u; // supported TGL GT2, also not the target
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_WRONG_DEVICE,
          "Tiger Lake GT2 id refused by exact target observer");

    sample = observedSample();
    sample.pipe = IG_TRANSCODER_DSI0;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_PIPE_RANGE,
          "DSI transcoder refused by ordinary-plane ladder observer");
}

static void checkEveryUnreadableWord()
{
    std::puts("3. every MMIO word in the snapshot is independently fail-closed");
    const auto base = observedSample();

    // Each entry is a pointer-to-member so this test cannot accidentally mutate
    // a copy while leaving the field consumed by the observer unchanged.
    IGU32 IgPlaneLiveSample::*fields[] = {
        &IgPlaneLiveSample::surfLiveBefore,
        &IgPlaneLiveSample::transConf,
        &IgPlaneLiveSample::pipeSrc,
        &IgPlaneLiveSample::ctl,
        &IgPlaneLiveSample::stride,
        &IgPlaneLiveSample::pos,
        &IgPlaneLiveSample::size,
        &IgPlaneLiveSample::surf,
        &IgPlaneLiveSample::offset,
        &IgPlaneLiveSample::surfLiveAfter,
    };

    for (const auto field : fields) {
        auto sample = base;
        sample.*field = IG_MMIO_INVALID;
        IgPlaneLiveObservation out = {};
        check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_SAMPLE_REFUSED,
              "IG_MMIO_INVALID field refused");
    }
}

static void checkStabilityAndEnableGates()
{
    std::puts("4. torn snapshots and inactive engine state are refused in order");
    IgPlaneLiveObservation out = {};

    auto sample = observedSample();
    sample.surfLiveAfter = 0x02000000u;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_UNSTABLE,
          "SURFLIVE change across sample refused");

    sample = observedSample();
    sample.transConf = 0x00000000u;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_TRANSCODER_DISABLED,
          "transcoder software enable clear refused");

    sample = observedSample();
    sample.transConf = 0x80000000u;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_TRANSCODER_INACTIVE,
          "transcoder requested on but hardware state off refused");

    sample = observedSample();
    sample.ctl &= ~IG_FB_CTL_ENABLE;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_PLANE_DISABLED,
          "plane enable clear refused");
}

static void checkGeometryAndLiveSurface()
{
    std::puts("5. pipe geometry and currently-live surface must agree exactly");
    IgPlaneLiveObservation out = {};

    auto sample = observedSample();
    sample.pipeSrc = 0x07800437u; // width 1921, height 1080
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_GEOMETRY_MISMATCH,
          "width mismatch refused");

    sample = observedSample();
    sample.pipeSrc = 0x077F0438u; // width 1920, height 1081
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_GEOMETRY_MISMATCH,
          "height mismatch refused");

    sample = observedSample();
    sample.surf = 0x02000000u;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_SURFACE_NOT_LIVE,
          "stable live surface different from programmed SURF refused");

    sample = observedSample();
    sample.surf = 0x01058004u;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_SURFACE_NOT_LIVE,
          "raw SURF flag difference is not masked into invented SURFLIVE semantics");
}

static void checkRefusalPrecedenceAndAtomicOutput()
{
    std::puts("6. refusal order is deterministic and output publication is atomic");

    // An unreadable last sample also differs from the first. The read refusal
    // wins over instability because no comparison against an unread register is
    // evidence.
    auto sample = observedSample();
    sample.surfLiveAfter = IG_MMIO_INVALID;
    IgPlaneLiveObservation out = {};
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_SAMPLE_REFUSED,
          "unreadable word beats unstable comparison");

    // A disabled transcoder whose plane is also disabled reports the earlier
    // engine-level gate, keeping diagnostics stable as later gates grow.
    sample = observedSample();
    sample.transConf = 0u;
    sample.ctl &= ~IG_FB_CTL_ENABLE;
    check(igPlaneLiveObserve(&sample, &out) == IG_PLANE_LIVE_ERR_TRANSCODER_DISABLED,
          "transcoder disable beats plane disable");

    IgPlaneLiveObservation sentinel = {};
    sentinel.deviceId = 0xDEADu;
    sentinel.liveSurfaceWord = 0xCAFEBABEu;
    sentinel.valid = true;
    sample = observedSample();
    sample.surfLiveAfter ^= 0x1000u;
    check(igPlaneLiveObserve(&sample, &sentinel) == IG_PLANE_LIVE_ERR_UNSTABLE,
          "atomic refusal trigger");
    check(sentinel.deviceId == 0xDEADu, "refusal leaves device output untouched");
    check(sentinel.liveSurfaceWord == 0xCAFEBABEu,
          "refusal leaves live surface output untouched");
    check(sentinel.valid, "refusal leaves valid sentinel untouched");
}

static void checkVerdictNames()
{
    std::puts("7. every in-range verdict has a stable diagnostic name");
    for (IGU32 verdict = 0u; verdict <= IG_PLANE_LIVE_VERDICT_LIMIT; ++verdict) {
        const char *name = igPlaneLiveVerdictName(verdict);
        check(name != nullptr, "verdict name non-null");
        check(std::string(name) != "unknown", "in-range verdict is named");
    }
    check(std::string(igPlaneLiveVerdictName(IG_PLANE_LIVE_VERDICT_LIMIT + 1u)) == "unknown",
          "out-of-range verdict remains unknown");
}

int main()
{
    checkGolden();
    checkNullsAndExactDeviceGate();
    checkEveryUnreadableWord();
    checkStabilityAndEnableGates();
    checkGeometryAndLiveSurface();
    checkRefusalPrecedenceAndAtomicOutput();
    checkVerdictNames();

    if (gFailures != 0u) {
        std::cerr << gFailures << " Intel plane live-observer test(s) failed\n";
        return 1;
    }

    std::cout << "Intel Tiger Lake plane live observer: all tests passed\n";
    return 0;
}
