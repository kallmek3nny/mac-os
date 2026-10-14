#include <cstdio>
#include <cstring>

#include "NVGspWprVga.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *label, unsigned int got, unsigned int want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got 0x%08x want 0x%08x\n",label,got,want);
    }
}

static void checkU64(const char *label, unsigned long long got, unsigned long long want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got %u want %u\n",label,(unsigned)got,(unsigned)want);
    }
}

static void checkPoison(const char *label,
                        const NVGspGa107VgaPlan &got,
                        const NVGspGa107VgaPlan &want)
{
    ++gChecks;
    if (std::memcmp(&got,&want,sizeof(got))!=0) {
        ++gFailures;
        std::printf("FAIL %s: refusal modified output\n",label);
    }
}

static unsigned int encodeWorkspace(unsigned long long byteOffset, bool valid)
{
    const unsigned long long field=byteOffset>>NV_GSP_GA107_VGA_WORKSPACE_UNIT_SHIFT;
    unsigned int raw=(unsigned int)((field<<NV_GSP_GA107_VGA_WORKSPACE_ADDR_SHIFT) &
                                    NV_GSP_GA107_VGA_WORKSPACE_ADDR_MASK);
    if (valid) raw|=NV_GSP_GA107_VGA_WORKSPACE_STATUS_VALID_MASK;
    return raw;
}

int main(void)
{
    checkU32("display fuse register",NV_GSP_GA107_DISPLAY_FUSE_REGISTER,0x00820c04u);
    checkU32("workspace register",NV_GSP_GA107_VGA_WORKSPACE_REGISTER,0x00625f04u);
    checkU64("PRAMIN bytes",NV_GSP_GA107_PRAMIN_BYTES,0x00100000ull);
    checkU64("relocated workspace bytes",NV_GSP_GA107_RELOCATED_WORKSPACE_BYTES,0x00020000ull);

    checkBool("fuse zero means display",nvGspGa107DisplaySupported(0u),true);
    checkBool("fuse bit means displayless",nvGspGa107DisplaySupported(1u),false);

    const unsigned int rawExample=encodeWorkspace(0x000000abcdef0000ull,true);
    checkBool("workspace valid bit",nvGspGa107WorkspaceRegisterValid(rawExample),true);
    checkU64("workspace address decode",nvGspGa107DecodeWorkspaceOffset(rawExample),
             0x000000abcdef0000ull);

    const unsigned long long fb4g=0x0000000100000000ull;
    NVGspGa107VgaPlan plan={};

    // Displayless chips never consult VGA_WORKSPACE_BASE. Even an all-ones raw
    // value is therefore irrelevant on this branch, matching open RM.
    checkU32("displayless fallback",
             nvGspPlanGa107VgaWorkspace(fb4g,1u,0xffffffffu,&plan),
             NV_GSP_WPR_VGA_OK);
    checkBool("displayless false",plan.displaySupported,false);
    checkBool("displayless uses fallback",plan.usedPraminFallback,true);
    checkU64("displayless workspace",plan.workspaceOffset,0x00000000fff00000ull);
    checkU64("displayless workspace bytes",plan.workspaceBytes,0x00100000ull);
    checkU64("displayless VBIOS boundary",plan.vbiosReservedOffset,plan.workspaceOffset);

    plan={};
    checkU32("invalid workspace status fallback",
             nvGspPlanGa107VgaWorkspace(fb4g,0u,0u,&plan),
             NV_GSP_WPR_VGA_OK);
    checkBool("display supported",plan.displaySupported,true);
    checkBool("register invalid",plan.workspaceRegisterValid,false);
    checkBool("invalid status uses fallback",plan.usedPraminFallback,true);
    checkU64("invalid status workspace",plan.workspaceOffset,0x00000000fff00000ull);

    // A valid workspace within the last 1 MiB is accepted exactly.
    const unsigned long long nearTop=0x00000000fff80000ull;
    plan={};
    checkU32("near-top workspace accepted",
             nvGspPlanGa107VgaWorkspace(fb4g,0u,encodeWorkspace(nearTop,true),&plan),
             NV_GSP_WPR_VGA_OK);
    checkBool("near-top register valid",plan.workspaceRegisterValid,true);
    checkBool("near-top not fallback",plan.usedPraminFallback,false);
    checkBool("near-top not relocated",plan.relocatedToTail,false);
    checkU64("near-top decoded",plan.decodedWorkspaceOffset,nearTop);
    checkU64("near-top workspace",plan.workspaceOffset,nearTop);
    checkU64("near-top bytes",plan.workspaceBytes,0x00080000ull);

    // A valid register below the final PRAMIN window is not trusted as-is.
    // TU102-through-Ada moves it to the final 128 KiB of FB.
    const unsigned long long tooLow=0x00000000f0000000ull;
    plan={};
    checkU32("low workspace relocated",
             nvGspPlanGa107VgaWorkspace(fb4g,0u,encodeWorkspace(tooLow,true),&plan),
             NV_GSP_WPR_VGA_OK);
    checkBool("low workspace relocated flag",plan.relocatedToTail,true);
    checkU64("low workspace decoded retained",plan.decodedWorkspaceOffset,tooLow);
    checkU64("relocated tail offset",plan.workspaceOffset,0x00000000fffe0000ull);
    checkU64("relocated tail size",plan.workspaceBytes,0x00020000ull);

    // Refusals are transactional.
    NVGspGa107VgaPlan poison;
    std::memset(&poison,0xa5,sizeof(poison));
    NVGspGa107VgaPlan before=poison;
    checkU32("dead fuse refused",
             nvGspPlanGa107VgaWorkspace(fb4g,0xffffffffu,0u,&poison),
             NV_GSP_WPR_VGA_DEAD_FUSE_READ);
    checkPoison("dead fuse atomic",poison,before);

    poison=before;
    checkU32("dead workspace refused",
             nvGspPlanGa107VgaWorkspace(fb4g,0u,0xffffffffu,&poison),
             NV_GSP_WPR_VGA_DEAD_WORKSPACE_READ);
    checkPoison("dead workspace atomic",poison,before);

    poison=before;
    checkU32("tiny framebuffer refused",
             nvGspPlanGa107VgaWorkspace(0x00080000ull,0u,0u,&poison),
             NV_GSP_WPR_VGA_BAD_FRAMEBUFFER);
    checkPoison("tiny framebuffer atomic",poison,before);

    poison=before;
    const unsigned long long atFb=fb4g;
    checkU32("workspace at framebuffer end refused",
             nvGspPlanGa107VgaWorkspace(fb4g,0u,encodeWorkspace(atFb,true),&poison),
             NV_GSP_WPR_VGA_BAD_WORKSPACE);
    checkPoison("workspace end atomic",poison,before);

    std::printf("GA107 VGA workspace: %d checks, %d failures\n",gChecks,gFailures);
    return gFailures ? 1 : 0;
}
