// SPDX-License-Identifier: MIT
#include "NVGspBooterFusePatchPolicy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ctest runs this binary from tests/build, where the relative
// reference/ga107-firmware path resolves to nothing (the 'fixture open'
// failure fixed for test_nv_gsp_booter_success_retirement_plan in f5110c1).
// CMake injects the source-tree path as NV_FIRMWARE_DIR (tests/CMakeLists.txt);
// the fallback keeps a hand-built binary run from the repo root working.
// A wrong path still FAILS, never skips.
#ifndef NV_FIRMWARE_DIR
#define NV_FIRMWARE_DIR "reference/ga107-firmware"
#endif

static unsigned gChecks=0;
static unsigned gFailures=0;

static void checkThat(bool cond,const std::string &what)
{
    ++gChecks;
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

static std::vector<unsigned char> load(const char *path)
{
    std::ifstream f(path,std::ios::binary);
    if (!f) {
        checkThat(false,std::string("open ")+path);
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});
}

static NVGspBooterFuseObservation observation(unsigned raw)
{
    NVGspBooterFuseObservation observed={};
    observed.ucodeId=NV_GSP_GA107_BOOTER_HS_UCODE_ID;
    observed.registerOffset=NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG+
        (NV_GSP_GA107_BOOTER_HS_UCODE_ID-NV_GSP_GA107_SEC2_UCODE_FIRST_ID)*
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    observed.rawFuse=raw;
    observed.fuseVersion=nvGspBooterFuseVersionFromMask(raw);
    observed.valid=true;
    return observed;
}

static void checkRealSelection(const std::vector<unsigned char> &blob,
                               unsigned rawFuse,
                               unsigned wantIndex,
                               unsigned wantPatch,
                               unsigned wantBytes,
                               const char *label)
{
    const auto observed=observation(rawFuse);
    NVGspBooterFusePatchPlan plan={};
    const auto verdict=nvGspPlanObservedBooterPatch(
        blob.data(),blob.size(),&observed,&plan);
    checkThat(verdict==NV_GSP_BOOTER_FUSE_PATCH_OK,
          std::string(label)+" plan verdict="+nvGspBooterFusePatchVerdictName(verdict));
    checkThat(plan.valid,std::string(label)+" policy valid");
    checkThat(plan.observation.ucodeId==3u,std::string(label)+" observation ucode 3");
    checkThat(plan.patch.ucodeId==plan.observation.ucodeId,
          std::string(label)+" parsed ucode matches observed fuse ucode");
    checkThat(plan.patch.signatureIndex==wantIndex,std::string(label)+" signature index");
    checkThat(plan.patch.patchDestinationOffset==wantPatch,std::string(label)+" patch location");
    checkThat(plan.patch.firmwareBytes==wantBytes,std::string(label)+" firmware bytes");

    const auto original=blob;
    std::vector<unsigned char> privateImage(wantBytes,0xa5u);
    NVGspBooterFusePatchPlan built={};
    const auto buildVerdict=nvGspBuildObservedBooterPrivateImage(
        blob.data(),blob.size(),&observed,
        privateImage.data(),privateImage.size(),&built);
    checkThat(buildVerdict==NV_GSP_BOOTER_FUSE_PATCH_OK,
          std::string(label)+" build verdict="+nvGspBooterFusePatchVerdictName(buildVerdict));
    checkThat(blob==original,std::string(label)+" source firmware immutable");
    checkThat(built.patch.signatureIndex==wantIndex,std::string(label)+" build keeps signature choice");

    const std::size_t sigSource=0x3cu+(std::size_t)wantIndex*0x180u;
    checkThat(std::equal(privateImage.begin()+wantPatch,
                     privateImage.begin()+wantPatch+0x180u,
                     blob.begin()+sigSource),
          std::string(label)+" selected production signature copied to private image");
}

static void checkObservationRefusals(const std::vector<unsigned char> &loadBlob)
{
    std::puts("2. stale/forged fuse facts are refused before private mutation");
    NVGspBooterFusePatchPlan sentinel={};
    sentinel.patch.signatureIndex=0x12345678u;

    auto invalid=observation(0u);
    invalid.valid=false;
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&invalid,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION,"invalid observation refused");
    checkThat(sentinel.patch.signatureIndex==0x12345678u,"invalid observation leaves output untouched");

    auto dead=observation(0u);
    dead.rawFuse=0xffffffffu;
    dead.fuseVersion=32u;
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&dead,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION,"dead BAR sentinel refused");

    auto wrongVersion=observation(0u);
    wrongVersion.fuseVersion=1u;
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&wrongVersion,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION,"raw-mask/version mismatch refused");

    auto wrongRegister=observation(1u);
    wrongRegister.registerOffset+=4u;
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&wrongRegister,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION,"noncanonical SEC2 register refused");

    auto wrongUcode=observation(0u);
    wrongUcode.ucodeId=4u;
    wrongUcode.registerOffset=NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG+
        (wrongUcode.ucodeId-NV_GSP_GA107_SEC2_UCODE_FIRST_ID)*
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&wrongUcode,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_UCODE_MISMATCH,
          "canonical fuse fact for another SEC2 ucode cannot select Booter signature");

    auto fuse2=observation(2u); // raw 0b10 => NVIDIA highest-set-bit+1 => version 2
    checkThat(fuse2.fuseVersion==2u,"raw 0b10 decodes to version two");
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&fuse2,&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_FUSE_RANGE,
          "two-signature GA107 image refuses observed fuse version two");

    std::vector<unsigned char> untouched(0xec00u,0x6bu);
    const auto before=untouched;
    checkThat(nvGspBuildObservedBooterPrivateImage(
              loadBlob.data(),loadBlob.size(),&wrongVersion,
              untouched.data(),untouched.size(),&sentinel)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION,
          "bad observation refused before private-image build");
    checkThat(untouched==before,"bad observation mutates no private destination bytes");
}

static void checkDestinationRefusals(const std::vector<unsigned char> &loadBlob)
{
    std::puts("3. existing private-copy safety remains intact through policy layer");
    const auto observed=observation(1u);
    NVGspBooterFusePatchPlan out={};

    checkThat(nvGspBuildObservedBooterPrivateImage(
              loadBlob.data(),loadBlob.size(),&observed,nullptr,0xec00u,&out)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_DEST,"null private image refused");

    std::vector<unsigned char> shortDest(0xebffu,0x5au);
    const auto before=shortDest;
    checkThat(nvGspBuildObservedBooterPrivateImage(
              loadBlob.data(),loadBlob.size(),&observed,
              shortDest.data(),shortDest.size(),&out)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_TOO_SMALL,"short private image refused");
    checkThat(shortDest==before,"short destination refusal is atomic");

    auto overlap=loadBlob;
    const auto overlapBefore=overlap;
    checkThat(nvGspBuildObservedBooterPrivateImage(
              overlap.data(),overlap.size(),&observed,
              overlap.data()+0x378u,0xec00u,&out)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_SOURCE_OVERLAP,"source/private overlap refused");
    checkThat(overlap==overlapBefore,"overlap refusal preserves source container");
}

int main()
{
    const auto loadBlob=load(NV_FIRMWARE_DIR "/booter_load-570.144.bin");
    const auto unloadBlob=load(NV_FIRMWARE_DIR "/booter_unload-570.144.bin");
    if (loadBlob.empty() || unloadBlob.empty()) return 1;

    std::printf("1. real GA107 fuse observations drive NVIDIA production selection\n");
    checkRealSelection(loadBlob,0u,1u,0x8a10u,0xec00u,"load/raw0");
    checkRealSelection(loadBlob,1u,0u,0x8a10u,0xec00u,"load/raw1");
    checkRealSelection(unloadBlob,0u,1u,0x5010u,0x9d00u,"unload/raw0");
    checkRealSelection(unloadBlob,1u,0u,0x5010u,0x9d00u,"unload/raw1");

    checkObservationRefusals(loadBlob);
    checkDestinationRefusals(loadBlob);

    NVGspBooterFusePatchPlan unused={};
    const auto observed=observation(0u);
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),&observed,nullptr)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OUTPUT,"null policy output refused");
    checkThat(nvGspPlanObservedBooterPatch(loadBlob.data(),loadBlob.size(),nullptr,&unused)==
          NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OBSERVATION,"null observation refused");

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
