// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspWprBootDma.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *name, NVGspWprBootDmaU32 got,
                     NVGspWprBootDmaU32 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %s\n",name,nvGspWprBootDmaVerdictName(got)); return; }
    ++gFailures;
    std::printf("FAIL %s got %s expected %s\n",name,
                nvGspWprBootDmaVerdictName(got),
                nvGspWprBootDmaVerdictName(expected));
}

static void checkU64(const char *name, NVGspWprBootDmaU64 got,
                     NVGspWprBootDmaU64 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%llX\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%llX expected 0x%llX\n",name,got,expected);
}

static void checkBool(const char *name, bool got, bool expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %s\n",name,got?"true":"false"); return; }
    ++gFailures;
    std::printf("FAIL %s got %s expected %s\n",name,got?"true":"false",expected?"true":"false");
}

static void checkSame(const char *name, const void *a, const void *b, size_t n)
{
    ++gChecks;
    if (std::memcmp(a,b,n)==0) { std::printf("ok  %s unchanged\n",name); return; }
    ++gFailures;
    std::printf("FAIL %s mutated\n",name);
}

static void happy(void)
{
    NVGspWprBootDmaPlan p={};
    const NVGspWprBootDmaU64 base=0x0000123456700000ull;
    checkU32("one contiguous GA107 IOVA",
             nvGspPlanWprBootDma(base,0x6000ull,0x5801ull,0x6000ull,&p),
             NV_GSP_WPR_BOOT_DMA_OK);
    checkBool("valid",p.valid,true);
    checkU64("base",p.gpuAddress,base);
    checkU64("exact source size",p.sourceBytes,0x5801ull);
    checkU64("allocation size",p.allocationBytes,0x6000ull);

    // Address is intentionally above 41 bits. This path is not Falcon
    // DMATRFBASE and must not silently inherit that unrelated policy ceiling.
    const NVGspWprBootDmaU64 highBase=0x0000040000000000ull;
    checkU32("above Phase-5 41-bit policy remains admissible",
             nvGspPlanWprBootDma(highBase,0x6000ull,0x5801ull,0x6000ull,&p),
             NV_GSP_WPR_BOOT_DMA_OK);
}

static void refusals(void)
{
    NVGspWprBootDmaPlan poison;
    std::memset(&poison,0xA5,sizeof(poison));
    NVGspWprBootDmaPlan before=poison;

    checkU32("zero source",
             nvGspPlanWprBootDma(0x1000ull,0x6000ull,0ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION);
    checkSame("zero source atomic",&poison,&before,sizeof(poison));

    checkU32("source exceeds allocation",
             nvGspPlanWprBootDma(0x1000ull,0x6000ull,0x7000ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION);
    checkSame("oversize source atomic",&poison,&before,sizeof(poison));

    checkU32("allocation must be page rounded",
             nvGspPlanWprBootDma(0x1000ull,0x6001ull,0x5801ull,0x6001ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION);
    checkSame("bad allocation atomic",&poison,&before,sizeof(poison));

    checkU32("zero IOVA",
             nvGspPlanWprBootDma(0ull,0x6000ull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_SEGMENT);
    checkSame("zero IOVA atomic",&poison,&before,sizeof(poison));

    checkU32("unaligned IOVA",
             nvGspPlanWprBootDma(0x1001ull,0x6000ull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT);
    checkSame("unaligned IOVA atomic",&poison,&before,sizeof(poison));

    checkU32("short IOVA segment",
             nvGspPlanWprBootDma(0x1000ull,0x5fffull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_SHORT_SEGMENT);
    checkSame("short segment atomic",&poison,&before,sizeof(poison));

    checkU32("allocation end overflow",
             nvGspPlanWprBootDma(~0ull-0x1000ull,0x6000ull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT);
    checkSame("overflow precursor atomic",&poison,&before,sizeof(poison));

    // Aligned address near UINT64_MAX that still wraps the requested range.
    checkU32("aligned range overflow",
             nvGspPlanWprBootDma(0xfffffffffffff000ull,0x6000ull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_RANGE_OVERFLOW);
    checkSame("range overflow atomic",&poison,&before,sizeof(poison));

    checkU32("null output",
             nvGspPlanWprBootDma(0x1000ull,0x6000ull,0x5801ull,0x6000ull,nullptr),
             NV_GSP_WPR_BOOT_DMA_NULL_OUTPUT);
}

int main(void)
{
    checkU64("page size",NV_GSP_WPR_BOOT_DMA_PAGE_BYTES,4096ull);
    happy();
    refusals();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
