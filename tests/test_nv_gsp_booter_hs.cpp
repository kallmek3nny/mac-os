// SPDX-License-Identifier: MIT
#include "NVGspBooterHs.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static unsigned gFailures=0;

static void fail(const std::string &what)
{
    std::cerr << "FAIL: " << what << "\n";
    ++gFailures;
}

static void check(bool cond,const std::string &what)
{
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
    if (off+4>b.size()) std::abort();
    b[off+0]=(unsigned char)(v>>0);
    b[off+1]=(unsigned char)(v>>8);
    b[off+2]=(unsigned char)(v>>16);
    b[off+3]=(unsigned char)(v>>24);
}

static NVGspBooterHsPlan plan(const std::vector<unsigned char> &b,
                              const char *label)
{
    NVGspBooterHsPlan p={};
    const auto v=nvGspPlanBooterHs(b.data(),b.size(),&p);
    if (v!=NV_GSP_BOOTER_HS_OK) {
        fail(std::string(label)+" verdict="+nvGspBooterHsVerdictName(v));
    }
    check(p.valid,std::string(label)+" valid");
    return p;
}

static void checkGa107Load(const NVGspBooterHsPlan &p)
{
    // Golden values measured from reference/ga107-firmware and independently
    // reproduced by NVIDIA's open-gpu-kernel-modules firmware extractor.
    check(p.paddedBytes==0x0000f000u,"load padded bytes");
    check(p.firmwareOffset==0x00000378u,"load firmware offset");
    check(p.firmwareBytes==0x0000ec00u,"load firmware bytes");
    check(p.signatureOffset==0x0000003cu,"load signature offset");
    check(p.signatureBytes==0x00000300u,"load signature bytes");
    check(p.signatureCount==2u,"load signature count");
    check(p.signatureStride==0x00000180u,"load signature stride");
    check(p.patchLocation==0x00008a10u,"load patch location");
    check(p.fuseVersion==1u,"load fuse version");
    check(p.engineId==1u,"load engine id");
    check(p.ucodeId==3u,"load ucode id");
    check(p.osCodeOffset==0u && p.osCodeBytes==0x100u,"load os code");
    check(p.osDataOffset==0x8a00u && p.osDataBytes==0x6200u,"load os data");
    check(p.appCodeOffset==0x100u && p.appCodeBytes==0x8900u,"load app code");
    check(p.appDataOffset==0x100u && p.appDataBytes==0u,"load app data");
}

static void checkGa107Unload(const NVGspBooterHsPlan &p)
{
    check(p.paddedBytes==0x0000a100u,"unload padded bytes");
    check(p.firmwareOffset==0x00000378u,"unload firmware offset");
    check(p.firmwareBytes==0x00009d00u,"unload firmware bytes");
    check(p.signatureOffset==0x0000003cu,"unload signature offset");
    check(p.signatureBytes==0x00000300u,"unload signature bytes");
    check(p.signatureCount==2u,"unload signature count");
    check(p.signatureStride==0x00000180u,"unload signature stride");
    check(p.patchLocation==0x00005010u,"unload patch location");
    check(p.fuseVersion==1u,"unload fuse version");
    check(p.engineId==1u,"unload engine id");
    check(p.ucodeId==3u,"unload ucode id");
    check(p.osCodeOffset==0u && p.osCodeBytes==0x100u,"unload os code");
    check(p.osDataOffset==0x5000u && p.osDataBytes==0x4d00u,"unload os data");
    check(p.appCodeOffset==0x100u && p.appCodeBytes==0x4f00u,"unload app code");
    check(p.appDataOffset==0x100u && p.appDataBytes==0u,"unload app data");
}

static void expectRefusal(const std::vector<unsigned char> &b,
                          NVGspBooterHsU32 expected,
                          const char *label)
{
    NVGspBooterHsPlan p={};
    p.signatureOffset=0xdeadbeefu;
    p.valid=false;
    const auto v=nvGspPlanBooterHs(b.data(),b.size(),&p);
    check(v==expected,std::string(label)+" verdict");
    // Atomic refusal: a bad candidate must not partially publish a plan.
    check(p.signatureOffset==0xdeadbeefu,std::string(label)+" atomic output");
    check(!p.valid,std::string(label)+" remains invalid");
}

int main()
{
    auto loadBlob=load("reference/ga107-firmware/booter_load-570.144.bin");
    auto unloadBlob=load("reference/ga107-firmware/booter_unload-570.144.bin");
    if (loadBlob.empty() || unloadBlob.empty()) return 1;

    check(loadBlob.size()==61304u,"load file bytes");
    check(unloadBlob.size()==41080u,"unload file bytes");
    checkGa107Load(plan(loadBlob,"booter_load"));
    checkGa107Unload(plan(unloadBlob,"booter_unload"));

    NVGspBooterHsPlan unused={};
    check(nvGspPlanBooterHs(loadBlob.data(),loadBlob.size(),nullptr)==
          NV_GSP_BOOTER_HS_ERR_NULL_OUTPUT,"null output refusal");
    check(nvGspPlanBooterHs(nullptr,0,&unused)==NV_GSP_BOOTER_HS_ERR_SHORT,
          "null buffer refusal");

    {
        auto b=loadBlob;
        put32(b,0,0u);
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_CONTAINER,"bad magic");
    }
    {
        auto b=loadBlob;
        put32(b,32,0x334u); // patch_loc_offset must follow 768-byte sig block.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_LAYOUT,"broken offset chain");
    }
    {
        auto b=loadBlob;
        put32(b,848,0u);
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_SIGNATURES,"zero signature count");
    }
    {
        auto b=loadBlob;
        put32(b,832,1u); // raw linux-firmware image must remain unpatched.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_SIGNATURES,"prepatched signature slot");
    }
    {
        auto b=loadBlob;
        put32(b,828,0xeb00u); // 384-byte signature would leave the payload.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_PATCH_RANGE,"patch range overflow");
    }
    {
        auto b=loadBlob;
        put32(b,840,0u); // engine id metadata is required for later fuse choice.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_METADATA,"zero engine id");
    }
    {
        auto b=loadBlob;
        put32(b,868,2u); // current GA10x fixed prefix is one-app only.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_LOAD_HEADER,"wrong app count");
    }
    {
        auto b=loadBlob;
        put32(b,856,0x101u); // extractor requires os_code_size==app_code_offset.
        expectRefusal(b,NV_GSP_BOOTER_HS_ERR_LOAD_HEADER,"code boundary mismatch");
    }

    if (gFailures) {
        std::cerr << gFailures << " Booter HS test(s) failed\n";
        return 1;
    }
    std::cout << "GA107 Booter HS planner: all tests passed\n";
    return 0;
}
