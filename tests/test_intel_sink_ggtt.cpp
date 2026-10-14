// Host tests for IntelSinkGgtt.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelSinkGgtt.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label,bool ok){++gChecks;if(!ok){++gFailures;std::printf("  FAIL %s\n",label);}}
static void checkU32(const char *label,IGGgttU32 got,IGGgttU32 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got %u want %u\n",label,got,want);}}
static void checkU64(const char *label,IGGgttU64 got,IGGgttU64 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got 0x%016llX want 0x%016llX\n",label,(unsigned long long)got,(unsigned long long)want);}}
static bool zeroPlan(const IGSinkGgttPlan &p){return p.firstPte==0u&&p.pteCount==0u&&p.mappedBytes==0ull&&p.ggttBase==0ull&&p.intelDmaBase==0ull;}

static void testSourceBackedPtes(void)
{
    std::printf("\n1. source-backed Gen12 GGTT encoding and GGC sizing\n");
    checkU64("address mask bits 45:12",IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12,0x00003FFFFFFFF000ull);
    checkU64("present bit",IG_SINK_GGTT_PTE_PRESENT_I915_0x1,1ull);
    checkU64("page0 PTE",igSinkGgttPteForPage(0x0000000123400000ull,0u),0x0000000123400001ull);
    checkU64("page1 PTE",igSinkGgttPteForPage(0x0000000123400000ull,1u),0x0000000123401001ull);
    check("present predicate",igSinkGgttPtePresent(0x12345001ull));
    check("non-present predicate",!igSinkGgttPtePresent(0x12345000ull));
    checkU64("misaligned DMA refuses PTE",igSinkGgttPteForPage(0x12345001ull,0u),0ull);
    checkU64("top address accepted",igSinkGgttPteForPage(0x00003FFFFFFFF000ull,0u),0x00003FFFFFFFF001ull);
    checkU64("page beyond top refuses",igSinkGgttPteForPage(0x00003FFFFFFFF000ull,1u),0ull);

    checkU32("GGC GGMS 0 refuses",igSinkGgttTableBytesFromGgc(0u),0u);
    checkU32("GGC GGMS 1 -> 2 MiB",igSinkGgttTableBytesFromGgc(1u<<6),2u*1024u*1024u);
    checkU32("GGC GGMS 2 -> 4 MiB",igSinkGgttTableBytesFromGgc(2u<<6),4u*1024u*1024u);
    checkU32("GGC GGMS 3 -> 8 MiB",igSinkGgttTableBytesFromGgc(3u<<6),8u*1024u*1024u);
    checkU32("GGC all ones refuses",igSinkGgttTableBytesFromGgc(0xFFFFu),0u);
    checkU32("2 MiB table entry count",igSinkGgttEntriesFromGgc(1u<<6),262144u);
    checkU32("8 MiB table entry count",igSinkGgttEntriesFromGgc(3u<<6),1048576u);
}

static void testPlanGeometry(void)
{
    std::printf("\n2. page-range planning\n");
    IGSinkGgttPlan p={};
    checkU32("8 MiB surface plan",
        igSinkGgttPlan(0x00400000ull,0x00800000ull,0x0000000120000000ull,0x00800000ull,262144u,&p),
        IG_SINK_GGTT_PLAN_OK);
    checkU32("first PTE",p.firstPte,1024u);
    checkU32("page count",p.pteCount,2048u);
    checkU64("mapped bytes",p.mappedBytes,0x00800000ull);
    checkU64("GGTT base preserved",p.ggttBase,0x00400000ull);
    checkU64("Intel DMA base preserved",p.intelDmaBase,0x0000000120000000ull);

    checkU32("non-page surface rounds up",
        igSinkGgttPlan(0x00200000ull,4097ull,0x0000000040000000ull,8192ull,4096u,&p),
        IG_SINK_GGTT_PLAN_OK);
    checkU32("rounded page count",p.pteCount,2u);
    checkU64("rounded bytes",p.mappedBytes,8192ull);
}

static void testExactRangeEdges(void)
{
    std::printf("\n3. exact GGTT and DMA edges\n");
    IGSinkGgttPlan p={};
    checkU32("mapping ending exactly at GGTT end accepted",
        igSinkGgttPlan(3ull*4096ull,4096ull,0x100000ull,4096ull,4u,&p),IG_SINK_GGTT_PLAN_OK);
    checkU32("edge first PTE",p.firstPte,3u);
    checkU32("edge count",p.pteCount,1u);

    checkU32("two pages from slot3 overflow GGTT",
        igSinkGgttPlan(3ull*4096ull,8192ull,0x100000ull,8192ull,4u,&p),IG_SINK_GGTT_PLAN_GGTT_RANGE);
    check("overflow refusal clears plan",zeroPlan(p));

    checkU32("top DMA page accepted",
        igSinkGgttPlan(0ull,4096ull,0x00003FFFFFFFF000ull,4096ull,1u,&p),IG_SINK_GGTT_PLAN_OK);
    checkU32("two pages at DMA top refused",
        igSinkGgttPlan(0ull,8192ull,0x00003FFFFFFFF000ull,8192ull,2u,&p),IG_SINK_GGTT_PLAN_DMA_RANGE);
    check("DMA range refusal clears plan",zeroPlan(p));
}

static void testRefusals(void)
{
    std::printf("\n4. refusal order and atomic output\n");
    IGSinkGgttPlan p={7u,8u,9ull,10ull,11ull};
    checkU32("null output refused",igSinkGgttPlan(0,4096,0x1000,4096,1,0),IG_SINK_GGTT_PLAN_NULL_OUT);

    p={7u,8u,9ull,10ull,11ull};
    checkU32("zero size refused",igSinkGgttPlan(0,0,0x1000,4096,1,&p),IG_SINK_GGTT_PLAN_ZERO_SIZE);
    check("zero-size clears plan",zeroPlan(p));

    p={7u,8u,9ull,10ull,11ull};
    checkU32("unaligned GGTT before DMA",igSinkGgttPlan(1,4096,0x1001,4096,1,&p),IG_SINK_GGTT_PLAN_GGTT_UNALIGNED);
    check("unaligned GGTT clears plan",zeroPlan(p));

    p={7u,8u,9ull,10ull,11ull};
    checkU32("unaligned DMA refused",igSinkGgttPlan(0,4096,0x1001,4096,1,&p),IG_SINK_GGTT_PLAN_DMA_UNALIGNED);
    check("unaligned DMA clears plan",zeroPlan(p));

    p={7u,8u,9ull,10ull,11ull};
    checkU32("short DMA span refused",igSinkGgttPlan(0,4097,0x1000,4097,2,&p),IG_SINK_GGTT_PLAN_DMA_SPAN_SHORT);
    check("short span clears plan",zeroPlan(p));

    p={7u,8u,9ull,10ull,11ull};
    checkU32("zero GGTT entries refused",igSinkGgttPlan(0,4096,0x1000,4096,0,&p),IG_SINK_GGTT_PLAN_GGTT_RANGE);
    check("zero-entry clears plan",zeroPlan(p));
}

static void testVerdicts(void)
{
    std::printf("\n5. verdict family\n");
    for(IGGgttU32 v=0;v<IG_SINK_GGTT_PLAN_LIMIT;++v){
        check("verdict named",std::strcmp(igSinkGgttPlanVerdictName(v),"unknown")!=0);
        check("success equality",igSinkGgttPlanSucceeded(v)==(v==IG_SINK_GGTT_PLAN_OK));
    }
}

int main(void)
{
    std::printf("IntelSinkGgtt.hpp -- Tiger Lake sink GGTT page planning\n");
    testSourceBackedPtes();
    testPlanGeometry();
    testExactRangeEdges();
    testRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
