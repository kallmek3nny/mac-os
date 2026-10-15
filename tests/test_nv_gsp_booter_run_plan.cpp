// SPDX-License-Identifier: MIT
#include "NVGspBooterRunPlan.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static unsigned gFailures=0u;

static void check(bool cond,const std::string &what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

static std::vector<unsigned char> load(const char *path)
{
    std::ifstream f(path,std::ios::binary);
    if (!f) {
        check(false,std::string("open ")+path);
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});
}

static void put32(std::vector<unsigned char> &b,std::size_t off,std::uint32_t v)
{
    check(off+4u<=b.size(),"put32 range");
    if (off+4u>b.size()) return;
    b[off+0u]=(unsigned char)(v>>0);
    b[off+1u]=(unsigned char)(v>>8);
    b[off+2u]=(unsigned char)(v>>16);
    b[off+3u]=(unsigned char)(v>>24);
}

static NVGspBooterFuseObservation observation(unsigned raw)
{
    NVGspBooterFuseObservation observed={};
    observed.ucodeId=NV_GSP_GA107_BOOTER_HS_UCODE_ID;
    observed.registerOffset=NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG+
        (observed.ucodeId-NV_GSP_GA107_SEC2_UCODE_FIRST_ID)*
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    observed.rawFuse=raw;
    observed.fuseVersion=nvGspBooterFuseVersionFromMask(raw);
    observed.valid=true;
    return observed;
}

static NVGspBooterHsPlan hsPlan(const std::vector<unsigned char> &blob)
{
    NVGspBooterHsPlan hs={};
    const auto verdict=nvGspPlanBooterHs(blob.data(),blob.size(),&hs);
    check(verdict==NV_GSP_BOOTER_HS_OK,"fixture HS parse");
    check(hs.valid,"fixture HS plan valid");
    return hs;
}

static std::vector<unsigned char> privateImage(
    const std::vector<unsigned char> &blob,
    const NVGspBooterFuseObservation &observed)
{
    const auto hs=hsPlan(blob);
    std::vector<unsigned char> image(hs.firmwareBytes,0x5au);
    NVGspBooterFusePatchPlan built={};
    const auto verdict=nvGspBuildObservedBooterPrivateImage(
        blob.data(),blob.size(),&observed,image.data(),image.size(),&built);
    check(verdict==NV_GSP_BOOTER_FUSE_PATCH_OK,"build private fixture image");
    check(built.valid,"built private fixture policy valid");
    return image;
}

static NVGspBooterRunPlan runPlan(
    const std::vector<unsigned char> &blob,
    const NVGspBooterFuseObservation &observed,
    const std::vector<unsigned char> &image,
    std::uint64_t dma,
    const char *label)
{
    NVGspBooterRunPlan plan={};
    const auto verdict=nvGspPlanGa107BooterRun(
        blob.data(),blob.size(),&observed,image.data(),image.size(),dma,&plan);
    check(verdict==NV_GSP_BOOTER_RUN_OK,
          std::string(label)+" verdict="+nvGspBooterRunPlanVerdictName(verdict));
    check(plan.valid,std::string(label)+" plan valid");
    return plan;
}

static void checkGoldenLoad(const std::vector<unsigned char> &blob)
{
    std::puts("1. real GA107 booter_load composes the sourced GA102+ run contract");
    const auto observed=observation(0u);
    const auto image=privateImage(blob,observed);
    const auto sourceBefore=blob;
    const auto imageBefore=image;
    const auto plan=runPlan(blob,observed,image,0x12345678abcdef01ull,"load");

    check(blob==sourceBefore,"planner preserves immutable source");
    check(image==imageBefore,"planner preserves private image");
    check(plan.engineId==1u,"GA107 engine id");
    check(plan.ucodeId==3u,"GA107 ucode id");
    check(plan.hardwareFuseVersion==0u,"observed fuse version");
    check(plan.signatureIndex==1u,"raw fuse zero selects last signature");
    check(plan.signatureBytes==0x180u,"signature size");
    check(plan.signaturePatchOffset==0x8a10u,"signature patch offset");
    check(plan.secureImemSourceOffset==0x100u,"secure IMEM source app0_code");
    check(plan.secureImemBytes==0x8900u,"secure IMEM bytes");
    check(plan.secureImemDestinationOffset==0u,"GA102+ secure IMEM destination zero");
    check(plan.dmemSourceOffset==0x8a00u,"DMEM source os_data");
    check(plan.dmemBytes==0x6200u,"DMEM bytes");
    check(plan.dmemDestinationOffset==0u,"DMEM destination zero");
    check(plan.mailbox0Input==0xabcdef01u,"WPR DMA low mailbox word");
    check(plan.mailbox1Input==0x12345678u,"WPR DMA high mailbox word");
    check(plan.mailbox0SuccessValue==0u,"mailbox0 zero is success");
    check(plan.resetRequired,"SEC2 reset is required");
    check(plan.secureImemLoadRequired,"secure IMEM load required");
    check(!plan.nonSecureImemLoadRequired,"GA102+ has no non-secure IMEM load");
    check(plan.dmemLoadRequired,"DMEM load required");
    check(plan.mailboxPairRequired,"mailbox pair required");
    check(plan.boundedCompletionPollRequired,"completion polling must be bounded");
    check(plan.nonzeroMailbox0IsFailure,"nonzero mailbox0 means Booter failure");
}

static void checkGoldenUnload(const std::vector<unsigned char> &blob)
{
    std::puts("2. real GA107 booter_unload shares the same semantic loader contract");
    const auto observed=observation(1u);
    const auto image=privateImage(blob,observed);
    const auto plan=runPlan(blob,observed,image,0x100000000ull,"unload");
    check(plan.signatureIndex==0u,"raw fuse one selects first signature");
    check(plan.signaturePatchOffset==0x5010u,"unload patch offset");
    check(plan.secureImemSourceOffset==0x100u,"unload secure IMEM source");
    check(plan.secureImemBytes==0x4f00u,"unload secure IMEM bytes");
    check(plan.dmemSourceOffset==0x5000u,"unload DMEM source");
    check(plan.dmemBytes==0x4d00u,"unload DMEM bytes");
    check(plan.mailbox0Input==0u && plan.mailbox1Input==1u,
          "64-bit DMA split crosses 32-bit boundary exactly");
}

static void checkInputAndImageRefusals(const std::vector<unsigned char> &loadBlob)
{
    std::puts("3. malformed inputs and any unapproved private-image byte are refused");
    const auto observed0=observation(0u);
    const auto observed1=observation(1u);
    const auto image0=privateImage(loadBlob,observed0);
    NVGspBooterRunPlan out={};

    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              image0.data(),image0.size(),1u,nullptr)==
          NV_GSP_BOOTER_RUN_ERR_NULL_OUTPUT,"null output refused");
    check(nvGspPlanGa107BooterRun(
              nullptr,loadBlob.size(),&observed0,
              image0.data(),image0.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_NULL_SOURCE,"null source refused");
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),nullptr,
              image0.data(),image0.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_NULL_OBSERVATION,"null observation refused");
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              nullptr,image0.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_NULL_PRIVATE_IMAGE,"null private image refused");

    auto malformed=loadBlob;
    malformed[0u]=0u;
    check(nvGspPlanGa107BooterRun(
              malformed.data(),malformed.size(),&observed0,
              image0.data(),image0.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_CONTAINER,"malformed immutable source refused");

    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              image0.data(),image0.size()-1u,1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_SIZE,"short private image refused");

    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed1,
              image0.data(),image0.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,
          "private image patched for wrong fuse/signature refused");

    const auto hs=hsPlan(loadBlob);
    std::vector<unsigned char> missingSignature(
        loadBlob.begin()+hs.firmwareOffset,
        loadBlob.begin()+hs.firmwareOffset+hs.firmwareBytes);
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              missingSignature.data(),missingSignature.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,"missing expected production signature refused");

    auto unrelated=image0;
    unrelated[0x50u]^=0x80u;
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              unrelated.data(),unrelated.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,"unrelated private byte mutation refused");

    auto code=image0;
    code[(std::size_t)hs.appCodeOffset+0x20u]^=1u;
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              code.data(),code.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,"secure IMEM code mutation refused");

    auto dmem=image0;
    dmem[(std::size_t)hs.osDataOffset+0x200u]^=1u;
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed0,
              dmem.data(),dmem.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,"DMEM mutation refused");
}

static void checkIdentityAndRegionRefusals(const std::vector<unsigned char> &loadBlob)
{
    std::puts("4. exact GA107 identity and GA102+ load-region semantics are gated");
    const auto observed=observation(0u);
    const auto image=privateImage(loadBlob,observed);
    NVGspBooterRunPlan out={};

    auto wrongEngine=loadBlob;
    put32(wrongEngine,840u,2u);
    check(nvGspPlanGa107BooterRun(
              wrongEngine.data(),wrongEngine.size(),&observed,
              image.data(),image.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_TARGET_ID,"wrong engine id refused");

    auto wrongUcode=loadBlob;
    put32(wrongUcode,844u,4u);
    check(nvGspPlanGa107BooterRun(
              wrongUcode.data(),wrongUcode.size(),&observed,
              image.data(),image.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_TARGET_ID,"wrong ucode id refused");

    auto imemOverflow=loadBlob;
    put32(imemOverflow,876u,0xffffffffu); // app0_code_size
    check(nvGspPlanGa107BooterRun(
              imemOverflow.data(),imemOverflow.size(),&observed,
              image.data(),image.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_CONTAINER,"out-of-range secure IMEM region refused");

    auto dmemOverflow=loadBlob;
    put32(dmemOverflow,864u,0xffffffffu); // os_data_size
    check(nvGspPlanGa107BooterRun(
              dmemOverflow.data(),dmemOverflow.size(),&observed,
              image.data(),image.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_CONTAINER,"out-of-range DMEM region refused");

    auto unexpectedAppData=loadBlob;
    put32(unexpectedAppData,884u,1u); // app0_data_size; parser permits the range
    check(nvGspPlanGa107BooterRun(
              unexpectedAppData.data(),unexpectedAppData.size(),&observed,
              image.data(),image.size(),1u,&out)==
          NV_GSP_BOOTER_RUN_ERR_LOAD_SEMANTICS,
          "unsupported GA102+ app-data load shape refused");
}

static void checkDmaAndAtomicity(const std::vector<unsigned char> &loadBlob)
{
    std::puts("5. WPR metadata DMA split is exact and refusals publish nothing");
    const auto observed=observation(0u);
    const auto image=privateImage(loadBlob,observed);

    NVGspBooterRunPlan out={};
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed,
              image.data(),image.size(),0u,&out)==
          NV_GSP_BOOTER_RUN_ERR_WPR_META_DMA,"zero WPR metadata DMA refused");

    struct Boundary { std::uint64_t dma; std::uint32_t lo; std::uint32_t hi; };
    const Boundary boundaries[]={
        {1ull,0x00000001u,0x00000000u},
        {0xffffffffull,0xffffffffu,0x00000000u},
        {0x100000000ull,0x00000000u,0x00000001u},
        {0xffffffffffffffffull,0xffffffffu,0xffffffffu},
    };
    for (const auto &b:boundaries) {
        NVGspBooterRunPlan p={};
        const auto verdict=nvGspPlanGa107BooterRun(
            loadBlob.data(),loadBlob.size(),&observed,
            image.data(),image.size(),b.dma,&p);
        check(verdict==NV_GSP_BOOTER_RUN_OK,"64-bit DMA boundary accepted");
        check(p.mailbox0Input==b.lo,"64-bit DMA boundary low word");
        check(p.mailbox1Input==b.hi,"64-bit DMA boundary high word");
    }

    NVGspBooterRunPlan sentinel={};
    sentinel.mailbox0Input=0xdeadbeefu;
    sentinel.mailbox1Input=0xcafebabeu;
    sentinel.valid=true;
    auto mutated=image;
    mutated[0x50u]^=1u;
    check(nvGspPlanGa107BooterRun(
              loadBlob.data(),loadBlob.size(),&observed,
              mutated.data(),mutated.size(),1u,&sentinel)==
          NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE,"atomic refusal trigger");
    check(sentinel.mailbox0Input==0xdeadbeefu,"refusal leaves mailbox0 output untouched");
    check(sentinel.mailbox1Input==0xcafebabeu,"refusal leaves mailbox1 output untouched");
    check(sentinel.valid,"refusal leaves validity sentinel untouched");
}

int main()
{
    const auto loadBlob=load("reference/ga107-firmware/booter_load-570.144.bin");
    const auto unloadBlob=load("reference/ga107-firmware/booter_unload-570.144.bin");
    if (loadBlob.empty() || unloadBlob.empty()) return 1;

    checkGoldenLoad(loadBlob);
    checkGoldenUnload(unloadBlob);
    checkInputAndImageRefusals(loadBlob);
    checkIdentityAndRegionRefusals(loadBlob);
    checkDmaAndAtomicity(loadBlob);

    if (gFailures) {
        std::cerr << gFailures << " GA107 Booter run-plan test(s) failed\n";
        return 1;
    }
    std::cout << "GA107 SEC2 Booter run planner: all tests passed\n";
    return 0;
}
