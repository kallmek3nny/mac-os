// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspWprGa107.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *name,NVGspWprU32 got,NVGspWprU32 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %u\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got %u expected %u\n",name,got,expected);
}

static void checkU64(const char *name,NVGspWprU64 got,NVGspWprU64 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%llX\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%llX expected 0x%llX\n",name,got,expected);
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

static NVGspGa107WprInput normalInput(void)
{
    NVGspGa107WprInput in={};
    in.fbSize=0x0000000100000000ull;          // synthetic 4 GiB usable FB
    in.vgaWorkspaceOffset=0x00000000fffe0000ull;
    in.mmuLockValid=false;
    in.mmuLockLo=0ull;
    in.bootloaderSize=0x00006000ull;
    in.radix3ElfSize=0x04000000ull;           // synthetic 64 MiB source image
    return in;
}

static void constants(void)
{
    checkU64("FRTS",NV_GSP_GA107_FRTS_BYTES,0x00100000ull);
    checkU64("non-WPR heap",NV_GSP_GA107_NON_WPR_HEAP_BYTES,0x00100000ull);
    checkU64("metadata reservation",NV_GSP_GA107_WPR_META_ALIGNED_BYTES,0x00100000ull);
    checkU64("pre-scrubbed top",NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES,0x10000000ull);
    checkU64("cold-boot end margin",NV_GSP_GA107_COLD_BOOT_WPR_END_MARGIN,0ull);
    checkU64("heap OS carveout",NV_GSP_GA107_FW_HEAP_OS_BYTES,0x01600000ull);
    checkU64("heap architecture base",NV_GSP_GA107_FW_HEAP_BASE_BYTES,0x00800000ull);
    checkU64("heap per GiB",NV_GSP_GA107_FW_HEAP_PER_GB_BYTES,0x00018000ull);
    checkU64("heap client reserve",NV_GSP_GA107_FW_HEAP_CLIENT_BYTES,0x06000000ull);
    checkU64("heap min",NV_GSP_GA107_FW_HEAP_MIN_BYTES,0x05800000ull);
    checkU64("heap max",NV_GSP_GA107_FW_HEAP_MAX_BYTES,0x11800000ull);
}

static void rounding(void)
{
    NVGspWprU64 gib=0;
    checkU32("exact 4 GiB rounds to 4",
             nvGspGa107RoundFbGiB(4ull*NV_GSP_GA107_GIB_BYTES,&gib),
             NV_GSP_GA107_WPR_POLICY_OK);
    checkU64("exact 4 GiB count",gib,4ull);

    checkU32("3 GiB plus one byte rounds to 4",
             nvGspGa107RoundFbGiB(3ull*NV_GSP_GA107_GIB_BYTES+1ull,&gib),
             NV_GSP_GA107_WPR_POLICY_OK);
    checkU64("3 GiB plus one count",gib,4ull);

    checkU32("zero FB refused",
             nvGspGa107RoundFbGiB(0ull,&gib),
             NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER);
}

static void normalColdBoot(void)
{
    NVGspGa107WprInput in=normalInput();
    NVGspGa107WprPlan p={};
    checkU32("normal cold boot verdict",
             nvGspPlanGa107ColdBootWpr(&in,&p),
             NV_GSP_GA107_WPR_POLICY_OK);

    checkU64("vbios boundary uses VGA",p.vbiosReservedOffset,0x00000000fffe0000ull);
    checkU64("first boot margin is zero",p.wprEndMargin,0ull);

    checkU64("rounded usable FB GiB",p.heap.roundedFbGiB,4ull);
    checkU64("heap formula = 127 MiB",p.heap.formulaBytes,0x07f00000ull);
    checkU64("pre-WPR bytes = metadata + non-WPR",p.heap.preWprBytes,0x00200000ull);
    checkU64("post-WPR bytes = 66 MiB",p.heap.postWprBytes,0x04200000ull);
    checkU64("pre-scrubbed heap ceiling = 188 MiB",p.heap.maxPrescrubbedHeapBytes,0x0bc00000ull);
    checkU64("normal final heap = 127 MiB",p.heap.heapBytes,0x07f00000ull);

    checkU64("WPR end",p.layout.gspFwWprEnd,0x00000000fffe0000ull);
    checkU64("FRTS offset",p.layout.frtsOffset,0x00000000ffee0000ull);
    checkU64("boot bin offset",p.layout.bootBinOffset,0x00000000ffeda000ull);
    checkU64("GSP FW offset",p.layout.gspFwOffset,0x00000000fbed0000ull);
    checkU64("heap offset",p.layout.gspFwHeapOffset,0x00000000f3f00000ull);
    checkU64("WPR start",p.layout.gspFwWprStart,0x00000000f3e00000ull);
    checkU64("non-WPR heap offset",p.layout.nonWprHeapOffset,0x00000000f3d00000ull);
    checkU64("reserved start",p.layout.gspFwRsvdStart,0x00000000f3d00000ull);

    checkU64("composed FRTS size",p.layoutInput.frtsSize,NV_GSP_GA107_FRTS_BYTES);
    checkU64("composed non-WPR raw size",p.layoutInput.nonWprHeapRawBytes,NV_GSP_GA107_NON_WPR_HEAP_BYTES);
    checkU64("composed WPR heap",p.layoutInput.wprHeapBytes,0x07f00000ull);
}

static void mmuLockWins(void)
{
    NVGspGa107WprInput in=normalInput();
    in.mmuLockValid=true;
    in.mmuLockLo=in.vgaWorkspaceOffset-0x00400000ull;

    NVGspGa107WprPlan p={};
    checkU32("lower MMU lock accepted",
             nvGspPlanGa107ColdBootWpr(&in,&p),
             NV_GSP_GA107_WPR_POLICY_OK);
    checkU64("MMU lock lowers VBIOS boundary",p.vbiosReservedOffset,in.mmuLockLo);
    checkBool("layout stays below MMU lock",p.layout.gspFwWprEnd<=in.mmuLockLo,true);
}

static void heapClamp(void)
{
    const NVGspWprU64 fb=0x0000000100000000ull;
    const NVGspWprU64 post=150ull*NV_GSP_GA107_MIB_BYTES;
    const NVGspWprU64 gspFwOffset=fb-post;
    NVGspGa107HeapPlan p={};

    checkU32("clamp vector verdict",
             nvGspGa107PlanFwHeap(fb,gspFwOffset,&p),
             NV_GSP_GA107_WPR_POLICY_OK);
    checkU64("clamp formula remains 127 MiB",p.formulaBytes,127ull*NV_GSP_GA107_MIB_BYTES);
    checkU64("clamp post-WPR is 150 MiB",p.postWprBytes,150ull*NV_GSP_GA107_MIB_BYTES);
    checkU64("clamp max heap is 104 MiB",p.maxPrescrubbedHeapBytes,104ull*NV_GSP_GA107_MIB_BYTES);
    checkU64("final heap clamps to 104 MiB",p.heapBytes,104ull*NV_GSP_GA107_MIB_BYTES);
}

static void refusalsAreAtomic(void)
{
    NVGspGa107WprPlan poison;
    std::memset(&poison,0xA5,sizeof(poison));
    NVGspGa107WprPlan before=poison;
    NVGspGa107WprInput in=normalInput();

    in.fbSize=0ull;
    checkU32("zero fb",
             nvGspPlanGa107ColdBootWpr(&in,&poison),
             NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER);
    checkSame("zero fb atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.vgaWorkspaceOffset=in.fbSize+1ull;
    checkU32("VGA above FB",
             nvGspPlanGa107ColdBootWpr(&in,&poison),
             NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY);
    checkSame("bad VGA atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.mmuLockValid=true; in.mmuLockLo=0ull;
    checkU32("valid MMU-lock flag with zero address",
             nvGspPlanGa107ColdBootWpr(&in,&poison),
             NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY);
    checkSame("bad MMU atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.bootloaderSize=0ull;
    checkU32("zero boot image",
             nvGspPlanGa107ColdBootWpr(&in,&poison),
             NV_GSP_GA107_WPR_POLICY_BAD_IMAGE_SIZE);
    checkSame("bad image atomic",&poison,&before,sizeof(poison));

    NVGspGa107HeapPlan heapPoison;
    std::memset(&heapPoison,0x5A,sizeof(heapPoison));
    NVGspGa107HeapPlan heapBefore=heapPoison;
    checkU32("pre-scrubbed window exhausted",
             nvGspGa107PlanFwHeap(0x100000000ull,
                                  0x100000000ull-255ull*NV_GSP_GA107_MIB_BYTES,
                                  &heapPoison),
             NV_GSP_GA107_WPR_POLICY_UNDERFLOW);
    checkSame("heap exhaustion atomic",&heapPoison,&heapBefore,sizeof(heapPoison));

    checkU32("null policy input",
             nvGspPlanGa107ColdBootWpr(nullptr,&poison),
             NV_GSP_GA107_WPR_POLICY_NULL_INPUT);
    checkU32("null policy output",
             nvGspPlanGa107ColdBootWpr(&in,nullptr),
             NV_GSP_GA107_WPR_POLICY_NULL_OUTPUT);
}

int main(void)
{
    constants();
    rounding();
    normalColdBoot();
    mmuLockWins();
    heapClamp();
    refusalsAreAtomic();

    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
