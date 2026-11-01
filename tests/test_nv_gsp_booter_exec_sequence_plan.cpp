// SPDX-License-Identifier: MIT
#include "NVGspBooterExecSequencePlan.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ctest runs each suite from tests/build, where a cwd-relative
// reference/ga107-firmware path resolves to nothing: MSVC ctest failed
// test_nv_gsp_booter_success_retirement_plan on 'fixture open' for exactly
// that (fixed in f5110c1). CMake injects the source-tree path as
// NV_FIRMWARE_DIR (tests/CMakeLists.txt); the fallback keeps a hand-built
// binary run from the repo root working. A wrong path still FAILS, never skips.
#ifndef NV_FIRMWARE_DIR
#define NV_FIRMWARE_DIR "reference/ga107-firmware"
#endif

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

static std::vector<unsigned char> load(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        check(false, "fixture open");
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

static NVGspBooterFuseObservation observation(unsigned raw)
{
    NVGspBooterFuseObservation observed = {};
    observed.ucodeId = NV_GSP_GA107_BOOTER_HS_UCODE_ID;
    observed.registerOffset = NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG +
        (observed.ucodeId - NV_GSP_GA107_SEC2_UCODE_FIRST_ID) *
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    observed.rawFuse = raw;
    observed.fuseVersion = nvGspBooterFuseVersionFromMask(raw);
    observed.valid = true;
    return observed;
}

static std::vector<unsigned char> privateImage(
    const std::vector<unsigned char> &blob,
    const NVGspBooterFuseObservation &observed)
{
    NVGspBooterHsPlan hs = {};
    check(nvGspPlanBooterHs(blob.data(), blob.size(), &hs) == NV_GSP_BOOTER_HS_OK,
          "fixture hs parse");
    std::vector<unsigned char> image(hs.firmwareBytes, 0x5au);
    NVGspBooterFusePatchPlan built = {};
    check(nvGspBuildObservedBooterPrivateImage(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), &built) == NV_GSP_BOOTER_FUSE_PATCH_OK,
          "fixture private image");
    check(built.valid, "fixture patch policy valid");
    return image;
}

static NVGspBooterExecSequencePlan plan(
    const std::vector<unsigned char> &blob,
    const NVGspBooterFuseObservation &observed,
    const std::vector<unsigned char> &image,
    std::uint64_t dma,
    const char *label)
{
    NVGspBooterExecSequencePlan out = {};
    const auto verdict = nvGspPlanGa107BooterExecSequence(
        blob.data(), blob.size(), &observed,
        image.data(), image.size(), dma, &out);
    if (verdict != NV_GSP_BOOTER_EXEC_OK)
        std::printf("FAIL: %s verdict=%s\n", label,
                    nvGspBooterExecSequenceVerdictName(verdict));
    check(verdict == NV_GSP_BOOTER_EXEC_OK, "sequence plan accepted");
    check(out.valid, "sequence plan valid");
    return out;
}

static bool sameBytes(const NVGspBooterExecSequencePlan &a,
                      const NVGspBooterExecSequencePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void testLoadGolden(const std::vector<unsigned char> &blob)
{
    std::puts("1. real ga107 booter_load yields exact no-write sec2 sequence contract");
    const auto observed = observation(0u);
    const auto image = privateImage(blob, observed);
    const auto sourceBefore = blob;
    const auto imageBefore = image;
    const auto p = plan(blob, observed, image, 0x12345678abcdef01ull, "load");

    check(blob == sourceBefore, "immutable source unchanged");
    check(image == imageBefore, "private image unchanged");
    check(p.engineIdMask == 1u && p.ucodeId == 3u, "exact ga107 booter identity");
    check(p.pkcDataOffset == 0x10u, "source-derived brom pkc offset");
    check(p.bootVector == 0x100u, "ga102+ boot vector is app0 code source offset");
    check(p.dmaBlockBytes == 256u, "falcon dma block is 256 bytes");
    check(p.secureImemBytes == 0x8900u && p.secureImemTransferCount == 137u,
          "load secure imem transfer count exact");
    check(p.dmemBytes == 0x6200u && p.dmemTransferCount == 98u,
          "load dmem transfer count exact");
    check(p.mailbox0Input == 0xabcdef01u && p.mailbox1Input == 0x12345678u,
          "wpr metadata mailbox split retained");
    check(p.mailbox0SuccessValue == 0u, "mailbox0 zero success retained");

    check(p.resetReadyBestEffortTimeoutUs == 150u,
          "ga102 reset-ready best-effort timeout pinned");
    check(p.resetAssertHoldUs == 10u, "falcon reset hold pinned");
    check(p.coreSelectTimeoutUs == 10000u, "falcon core-select valid timeout pinned");
    check(p.memoryScrubTimeoutUs == 20000u, "memory scrub timeout pinned");
    check(p.dmaIdleTimeoutUs == 2000000u, "per-transfer dma idle timeout pinned");
    check(p.bootHaltTimeoutUs == 2000000u, "booter halt timeout pinned");

    check(p.resetReadyTimeoutIsNonFatal && p.engineResetRequired,
          "ga102 reset semantics retained");
    check(p.memoryScrubAfterResetRequired && p.falconCoreSelectRequired &&
          p.coreSelectValidPollRequired && p.secondMemoryScrubWaitRequired,
          "reset/core-select/scrub sequencing retained");
    check(p.falconRmSeedRequired, "falcon rm seed step retained");
    check(p.dmaPhysicalNoContextRequired && p.dmaControllerResetRequired,
          "falcon dma setup retained");
    check(p.dmaTargetCoherentSystemMemory && p.dmaAddressTypePhysical,
          "coherent-system physical dma aperture retained");
    check(p.secureImemBeforeDmem && p.perTransferIdlePollRequired,
          "imem-before-dmem and retirement polling retained");
    check(p.bromProgramRequired && p.bromOrderPkcEngineUcodeRsa3k &&
          p.bromRsa3kRequired && p.bootVectorProgramRequired,
          "ga102 brom and boot-vector contract retained");
    check(p.mailbox0BeforeMailbox1 && p.startAfterMailboxes &&
          p.boundedHaltPollRequired && p.mailbox0BeforeMailbox1Readback &&
          p.nonzeroMailbox0IsFailure,
          "mailbox/start/halt/readback order retained");

    check(p.firmwareDmaBackingRetainUntilTransferIdle,
          "firmware dma backing retained until transfer retirement");
    check(p.quarantineFirmwareDmaBackingOnTransferTimeout,
          "uncertain fbdma timeout requires quarantine");
    check(p.wprMetaBackingRetainUntilHalt,
          "wpr metadata backing retained until sec2 halt");
    check(p.quarantineWprMetaBackingOnBootTimeout &&
          p.quarantineWprMetaBackingOnBootFailure,
          "uncertain/failed boot retains wpr metadata backing");
    check(!p.timeoutRecoveryProven, "timeout recovery remains explicitly unproven");
    check(!p.runtimeWriteAuthorized, "planner never authorizes runtime writes");
}

static void testUnloadGolden(const std::vector<unsigned char> &blob)
{
    std::puts("2. real ga107 booter_unload preserves the same sourced execution semantics");
    const auto observed = observation(1u);
    const auto image = privateImage(blob, observed);
    const auto p = plan(blob, observed, image, 0x100000000ull, "unload");

    check(p.pkcDataOffset == 0x10u, "unload brom pkc offset");
    check(p.bootVector == 0x100u, "unload boot vector");
    check(p.secureImemBytes == 0x4f00u && p.secureImemTransferCount == 79u,
          "unload secure imem transfers");
    check(p.dmemBytes == 0x4d00u && p.dmemTransferCount == 77u,
          "unload dmem transfers");
    check(p.mailbox0Input == 0u && p.mailbox1Input == 1u,
          "unload 64-bit mailbox boundary retained");
    check(!p.runtimeWriteAuthorized, "unload plan also cannot authorize writes");
}

static void testRunPlanFailuresAreClosed(const std::vector<unsigned char> &blob)
{
    std::puts("3. malformed source/private/wpr inputs cannot bypass the existing run planner");
    const auto observed = observation(0u);
    const auto image = privateImage(blob, observed);
    NVGspBooterExecSequencePlan out = {};

    check(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), 1u, nullptr) ==
          NV_GSP_BOOTER_EXEC_ERR_NULL_OUTPUT,
          "null output refused");

    check(nvGspPlanGa107BooterExecSequence(
              nullptr, blob.size(), &observed,
              image.data(), image.size(), 1u, &out) ==
          NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN,
          "null immutable source refused through run planner");

    auto badImage = image;
    badImage[0x50u] ^= 1u;
    check(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              badImage.data(), badImage.size(), 1u, &out) ==
          NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN,
          "unapproved private-image mutation refused through run planner");

    check(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              image.data(), image.size(), 0u, &out) ==
          NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN,
          "zero wpr metadata dma refused through run planner");
}

static void testVerdictsAndAtomicity(const std::vector<unsigned char> &blob)
{
    std::puts("4. refusals are atomic and verdict names remain stable");
    for (NVGspBooterHsU32 v = 0u; v <= NV_GSP_BOOTER_EXEC_VERDICT_LIMIT; ++v) {
        check(std::strcmp(nvGspBooterExecSequenceVerdictName(v), "unknown") != 0,
              "in-range verdict named");
    }
    check(std::strcmp(nvGspBooterExecSequenceVerdictName(
                          NV_GSP_BOOTER_EXEC_VERDICT_LIMIT + 1u),
                      "unknown") == 0,
          "out-of-range verdict unknown");

    const auto observed = observation(0u);
    const auto image = privateImage(blob, observed);
    NVGspBooterExecSequencePlan out = {};
    std::memset(&out, 0xA5, sizeof(out));
    const auto before = out;
    auto badImage = image;
    badImage[0x50u] ^= 1u;
    check(nvGspPlanGa107BooterExecSequence(
              blob.data(), blob.size(), &observed,
              badImage.data(), badImage.size(), 1u, &out) ==
          NV_GSP_BOOTER_EXEC_ERR_RUN_PLAN,
          "atomic-refusal trigger");
    check(sameBytes(out, before), "refusal leaves output byte-for-byte untouched");
}

int main()
{
    const auto loadBlob = load(NV_FIRMWARE_DIR "/booter_load-570.144.bin");
    const auto unloadBlob = load(NV_FIRMWARE_DIR "/booter_unload-570.144.bin");
    if (loadBlob.empty() || unloadBlob.empty())
        return 1;

    testLoadGolden(loadBlob);
    testUnloadGolden(unloadBlob);
    testRunPlanFailuresAreClosed(loadBlob);
    testVerdictsAndAtomicity(loadBlob);

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
