// SPDX-License-Identifier: MIT
#include "NVGspBooterPatch.hpp"

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

static void fail(const std::string &what)
{
    std::cerr << "FAIL: " << what << "\n";
    ++gFailures;
}

static void checkThat(bool cond,const std::string &what)
{
    ++gChecks;
    if (!cond) fail(what);
}

static std::vector<unsigned char> load(const char *path)
{
    std::ifstream f(path,std::ios::binary);
    if (!f) {
        fail(std::string("open ")+path);
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});
}

static void put32(std::vector<unsigned char> &b,std::size_t off,std::uint32_t v)
{
    if (off+4>b.size()) {
        fail("put32 out of range");
        return;
    }
    b[off+0]=(unsigned char)(v>>0);
    b[off+1]=(unsigned char)(v>>8);
    b[off+2]=(unsigned char)(v>>16);
    b[off+3]=(unsigned char)(v>>24);
}

static void checkPlan(const std::vector<unsigned char> &blob,
                      unsigned fuse,
                      unsigned wantIndex,
                      unsigned wantSignatureOffset,
                      unsigned wantPatch,
                      unsigned wantFirmwareBytes,
                      const char *label)
{
    NVGspBooterPatchPlan p={};
    const auto verdict=nvGspPlanBooterPatch(blob.data(),blob.size(),fuse,&p);
    checkThat(verdict==NV_GSP_BOOTER_PATCH_OK,
          std::string(label)+" planner verdict="+nvGspBooterPatchVerdictName(verdict));
    checkThat(p.valid,std::string(label)+" valid");
    checkThat(p.hardwareFuseVersion==fuse,std::string(label)+" hardware fuse");
    checkThat(p.sourceMetadataFuseVersion==1u,std::string(label)+" source metadata fuse");
    checkThat(p.signatureIndex==wantIndex,std::string(label)+" signature index");
    checkThat(p.signatureSourceOffset==wantSignatureOffset,
          std::string(label)+" signature source offset");
    checkThat(p.signatureBytes==0x180u,std::string(label)+" signature stride");
    checkThat(p.firmwareSourceOffset==0x378u,std::string(label)+" firmware offset");
    checkThat(p.firmwareBytes==wantFirmwareBytes,std::string(label)+" firmware bytes");
    checkThat(p.patchDestinationOffset==wantPatch,std::string(label)+" patch destination");
    checkThat(p.ucodeId==3u,std::string(label)+" ucode id");
}

static void checkPrivateImage(const std::vector<unsigned char> &blob,
                              unsigned fuse,
                              unsigned wantIndex,
                              unsigned wantPatch,
                              unsigned wantFirmwareBytes,
                              const char *label)
{
    const std::vector<unsigned char> original=blob;
    std::vector<unsigned char> image(wantFirmwareBytes,0xa5u);
    NVGspBooterPatchPlan p={};
    const auto verdict=nvGspBuildBooterPrivateImage(
        blob.data(),blob.size(),fuse,image.data(),image.size(),&p);
    checkThat(verdict==NV_GSP_BOOTER_PATCH_OK,
          std::string(label)+" private-image verdict="+nvGspBooterPatchVerdictName(verdict));
    checkThat(p.valid,std::string(label)+" private plan valid");
    checkThat(p.signatureIndex==wantIndex,std::string(label)+" private signature index");
    checkThat(blob==original,std::string(label)+" source container immutable");

    const std::size_t sigSource=0x3cu+(std::size_t)wantIndex*0x180u;
    checkThat(std::equal(image.begin()+wantPatch,image.begin()+wantPatch+0x180u,
                     blob.begin()+sigSource),
          std::string(label)+" selected signature patched exactly");

    // Everything outside the patch slot must be the unmodified firmware payload.
    for (std::size_t i=0;i<image.size();++i) {
        if (i>=wantPatch && i<wantPatch+0x180u) continue;
        if (image[i]!=blob[0x378u+i]) {
            fail(std::string(label)+" non-signature payload changed at "+std::to_string(i));
            break;
        }
    }
}

static void checkAtomicRefusals(const std::vector<unsigned char> &loadBlob)
{
    std::puts("3. refusal paths are atomic");
    NVGspBooterPatchPlan sentinel={};
    sentinel.signatureIndex=0x12345678u;
    sentinel.valid=false;

    checkThat(nvGspPlanBooterPatch(loadBlob.data(),loadBlob.size(),2u,&sentinel)==
          NV_GSP_BOOTER_PATCH_ERR_FUSE_RANGE,"fuse 2 refused for two signatures");
    checkThat(sentinel.signatureIndex==0x12345678u && !sentinel.valid,
          "fuse refusal leaves output untouched");

    std::vector<unsigned char> tooSmall(0xebffu,0x5au);
    const auto tooSmallBefore=tooSmall;
    NVGspBooterPatchPlan out={};
    checkThat(nvGspBuildBooterPrivateImage(loadBlob.data(),loadBlob.size(),1u,
                                       tooSmall.data(),tooSmall.size(),&out)==
          NV_GSP_BOOTER_PATCH_ERR_DEST_TOO_SMALL,"short private image refused");
    checkThat(tooSmall==tooSmallBefore,"short-destination refusal writes nothing");

    auto overlap=loadBlob;
    const auto overlapBefore=overlap;
    checkThat(nvGspBuildBooterPrivateImage(
              overlap.data(),overlap.size(),1u,overlap.data()+0x378u,0xec00u,&out)==
          NV_GSP_BOOTER_PATCH_ERR_SOURCE_OVERLAP,"source/destination overlap refused");
    checkThat(overlap==overlapBefore,"overlap refusal leaves source untouched");

    auto malformed=loadBlob;
    put32(malformed,0u,0u);
    std::vector<unsigned char> untouched(0xec00u,0x6bu);
    const auto untouchedBefore=untouched;
    checkThat(nvGspBuildBooterPrivateImage(
              malformed.data(),malformed.size(),1u,untouched.data(),untouched.size(),&out)==
          NV_GSP_BOOTER_PATCH_ERR_CONTAINER,"malformed container refused");
    checkThat(untouched==untouchedBefore,"malformed refusal writes nothing");
}

static void checkSingleSignatureRule(const std::vector<unsigned char> &loadBlob)
{
    std::puts("4. one-signature image always selects index zero");
    auto one=loadBlob;
    // num_sig is at absolute offset 848 in the committed GA107 container.
    put32(one,848u,1u);
    NVGspBooterPatchPlan p={};
    const auto verdict=nvGspPlanBooterPatch(one.data(),one.size(),99u,&p);
    checkThat(verdict==NV_GSP_BOOTER_PATCH_OK,"single-signature high fuse accepted");
    checkThat(p.signatureIndex==0u,"single-signature index zero");
    checkThat(p.signatureBytes==0x300u,"single-signature stride is total signature block");
}

int main()
{
    auto loadBlob=load(NV_FIRMWARE_DIR "/booter_load-570.144.bin");
    auto unloadBlob=load(NV_FIRMWARE_DIR "/booter_unload-570.144.bin");
    if (loadBlob.empty() || unloadBlob.empty()) return 1;

    std::printf("1. production selection rule on real GA107 firmware\n");
    // NVIDIA source: for two signatures, fuse 0 selects slot 1 and fuse 1
    // selects slot 0. These offsets are absolute container offsets.
    checkPlan(loadBlob,0u,1u,0x1bcu,0x8a10u,0xec00u,"load/fuse0");
    checkPlan(loadBlob,1u,0u,0x03cu,0x8a10u,0xec00u,"load/fuse1");
    checkPlan(unloadBlob,0u,1u,0x1bcu,0x5010u,0x9d00u,"unload/fuse0");
    checkPlan(unloadBlob,1u,0u,0x03cu,0x5010u,0x9d00u,"unload/fuse1");

    std::printf("2. private copies patch only the chosen signature slot\n");
    checkPrivateImage(loadBlob,0u,1u,0x8a10u,0xec00u,"load/fuse0");
    checkPrivateImage(loadBlob,1u,0u,0x8a10u,0xec00u,"load/fuse1");
    checkPrivateImage(unloadBlob,0u,1u,0x5010u,0x9d00u,"unload/fuse0");
    checkPrivateImage(unloadBlob,1u,0u,0x5010u,0x9d00u,"unload/fuse1");

    checkAtomicRefusals(loadBlob);
    checkSingleSignatureRule(loadBlob);

    NVGspBooterPatchPlan unused={};
    checkThat(nvGspPlanBooterPatch(loadBlob.data(),loadBlob.size(),0u,nullptr)==
          NV_GSP_BOOTER_PATCH_ERR_NULL_OUTPUT,"null planner output refused");
    checkThat(nvGspBuildBooterPrivateImage(loadBlob.data(),loadBlob.size(),0u,nullptr,
                                       0xec00u,&unused)==
          NV_GSP_BOOTER_PATCH_ERR_NULL_DEST,"null destination refused");

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
