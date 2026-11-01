// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspWprGa107Facts.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *name,NVGspGa107FactU32 got,NVGspGa107FactU32 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %u\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got %u expected %u\n",name,got,expected);
}

static void checkU64(const char *name,NVGspGa107FactU64 got,NVGspGa107FactU64 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%llx\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%llx expected 0x%llx\n",name,got,expected);
}

static void checkBool(const char *name,bool got,bool expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %s\n",name,got?"true":"false"); return; }
    ++gFailures;
    std::printf("FAIL %s got %s expected %s\n",name,got?"true":"false",expected?"true":"false");
}

static void checkSame(const char *name,const void *a,const void *b,size_t n)
{
    ++gChecks;
    if (std::memcmp(a,b,n)==0) { std::printf("ok  %s unchanged\n",name); return; }
    ++gFailures;
    std::printf("FAIL %s mutated\n",name);
}

static NVGspGa107FactU32 encodeWorkspace(NVGspGa107FactU64 offset,bool valid)
{
    NVGspGa107FactU32 field=(NVGspGa107FactU32)(offset >> 16);
    return (field << 8) | (valid ? NV_GSP_GA107_VGA_WORKSPACE_VALID_BIT : 0u);
}

static NVGspGa107ObservedRegs baseRegs(void)
{
    NVGspGa107ObservedRegs r={};
    r.usableFbMb=4096u;
    r.displayFuse=0u;
    r.vgaWorkspace=encodeWorkspace(0x00000000fff00000ull,true);
    return r;
}

static void constants(void)
{
    checkU32("usable FB register",NV_GSP_GA107_USABLE_FB_MB_REG,0x001183a4u);
    checkU32("display fuse register",NV_GSP_GA107_DISPLAY_FUSE_REG,0x00820c04u);
    checkU32("VGA workspace register",NV_GSP_GA107_VGA_WORKSPACE_REG,0x00625f04u);
    checkU32("display disable bit",NV_GSP_GA107_DISPLAY_FUSE_DISABLE_BIT,1u);
    checkU32("workspace valid bit",NV_GSP_GA107_VGA_WORKSPACE_VALID_BIT,8u);
    checkU64("PRAMIN fallback bytes",NV_GSP_GA107_PRAMIN_BYTES,0x00100000ull);
    checkU64("VBIOS relocation bytes",NV_GSP_GA107_VBIOS_WORKSPACE_BYTES,0x00020000ull);
}

static void exactWorkspace(void)
{
    NVGspGa107ObservedRegs r=baseRegs();
    NVGspGa107RuntimeFacts f={};
    checkU32("exact workspace verdict",nvGspDecodeGa107WprFacts(&r,&f),NV_GSP_GA107_FACTS_OK);
    checkU64("4 GiB usable FB",f.fbSizeBytes,0x0000000100000000ull);
    checkBool("display supported",f.displaySupported,true);
    checkBool("VGA register valid",f.vgaWorkspaceRegisterValid,true);
    checkU64("raw workspace",f.vgaWorkspaceRegisterOffset,0x00000000fff00000ull);
    checkU64("policy keeps threshold workspace",f.vgaWorkspacePolicyOffset,0x00000000fff00000ull);
    checkBool("GA107 MMU lock invalid",f.mmuLockValid,false);
    checkU64("GA107 MMU lock low zero",f.mmuLockLo,0ull);
}

static void lowWorkspaceRelocates(void)
{
    NVGspGa107ObservedRegs r=baseRegs();
    r.vgaWorkspace=encodeWorkspace(0x00000000fe000000ull,true);
    NVGspGa107RuntimeFacts f={};
    checkU32("low workspace verdict",nvGspDecodeGa107WprFacts(&r,&f),NV_GSP_GA107_FACTS_OK);
    checkU64("low raw workspace",f.vgaWorkspaceRegisterOffset,0x00000000fe000000ull);
    checkU64("low workspace relocated to final 128 KiB",f.vgaWorkspacePolicyOffset,
             0x00000000fffe0000ull);
}

static void fallbacks(void)
{
    NVGspGa107RuntimeFacts f={};

    NVGspGa107ObservedRegs r=baseRegs();
    r.displayFuse=1u;
    checkU32("display fused off verdict",nvGspDecodeGa107WprFacts(&r,&f),NV_GSP_GA107_FACTS_OK);
    checkBool("display fused off",f.displaySupported,false);
    checkU64("displayless fallback is final MiB",f.vgaWorkspacePolicyOffset,
             0x00000000fff00000ull);

    r=baseRegs();
    r.vgaWorkspace=encodeWorkspace(0x00000000fe000000ull,false);
    std::memset(&f,0,sizeof(f));
    checkU32("invalid VGA status verdict",nvGspDecodeGa107WprFacts(&r,&f),NV_GSP_GA107_FACTS_OK);
    checkBool("invalid VGA status",f.vgaWorkspaceRegisterValid,false);
    checkU64("invalid-status fallback is final MiB",f.vgaWorkspacePolicyOffset,
             0x00000000fff00000ull);
}

static void upperBoundary(void)
{
    NVGspGa107ObservedRegs r=baseRegs();
    r.vgaWorkspace=encodeWorkspace(0x0000000100000000ull,true);
    NVGspGa107RuntimeFacts f={};
    checkU32("workspace at FB end accepted",nvGspDecodeGa107WprFacts(&r,&f),NV_GSP_GA107_FACTS_OK);
    checkU64("workspace at FB end retained",f.vgaWorkspacePolicyOffset,0x0000000100000000ull);
}

static void refusalsAtomic(void)
{
    NVGspGa107RuntimeFacts poison;
    std::memset(&poison,0xa5,sizeof(poison));
    NVGspGa107RuntimeFacts before=poison;

    NVGspGa107ObservedRegs r=baseRegs();
    r.usableFbMb=0u;
    checkU32("zero usable FB",nvGspDecodeGa107WprFacts(&r,&poison),NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER);
    checkSame("zero FB atomic",&poison,&before,sizeof(poison));

    r=baseRegs(); r.usableFbMb=0xffffffffu;
    checkU32("all-ones usable FB",nvGspDecodeGa107WprFacts(&r,&poison),NV_GSP_GA107_FACTS_DEAD_READ);
    checkSame("dead FB atomic",&poison,&before,sizeof(poison));

    r=baseRegs(); r.displayFuse=0xffffffffu;
    checkU32("all-ones fuse",nvGspDecodeGa107WprFacts(&r,&poison),NV_GSP_GA107_FACTS_DEAD_READ);
    checkSame("dead fuse atomic",&poison,&before,sizeof(poison));

    r=baseRegs(); r.vgaWorkspace=0xffffffffu;
    checkU32("all-ones VGA",nvGspDecodeGa107WprFacts(&r,&poison),NV_GSP_GA107_FACTS_DEAD_READ);
    checkSame("dead VGA atomic",&poison,&before,sizeof(poison));

    r=baseRegs();
    r.vgaWorkspace=encodeWorkspace(0x0000000100010000ull,true);
    checkU32("workspace above FB",nvGspDecodeGa107WprFacts(&r,&poison),NV_GSP_GA107_FACTS_BAD_WORKSPACE);
    checkSame("bad workspace atomic",&poison,&before,sizeof(poison));

    checkU32("null input",nvGspDecodeGa107WprFacts(nullptr,&poison),NV_GSP_GA107_FACTS_NULL_INPUT);
    checkU32("null output",nvGspDecodeGa107WprFacts(&r,nullptr),NV_GSP_GA107_FACTS_NULL_OUTPUT);
}

int main(void)
{
    constants();
    exactWorkspace();
    lowWorkspaceRelocates();
    fallbacks();
    upperBoundary();
    refusalsAtomic();

    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
