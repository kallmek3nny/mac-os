// Host tests for NVAmpereCopy.hpp. No IOKit, GPU, FIFO or submission required.
#include <cstdio>
#include <cstring>
#include "NVAmpereCopy.hpp"

static int gChecks=0,gFailures=0;
static void check(const char *label,bool ok){++gChecks;if(!ok){++gFailures;std::printf("  FAIL %s\n",label);}}
static void checkU32(const char *label,NVCopyU32 got,NVCopyU32 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got 0x%08X want 0x%08X\n",label,got,want);}}
static bool same(const NVCopyMethod *a,const NVCopyMethod *b){return std::memcmp(a,b,sizeof(NVCopyMethod)*NV_COPY_C7B5_METHOD_COUNT)==0;}

static void testSourceBackedConstants(void)
{
    std::printf("\n1. source-backed C7B5 constants\n");
    checkU32("AMPERE_DMA_COPY_B class",NV_COPY_C7B5_CLASS_ID,0x0000c7b5u);
    checkU32("SET_DST_PHYS_MODE",NV_COPY_C7B5_SET_DST_PHYS_MODE,0x264u);
    checkU32("LAUNCH_DMA",NV_COPY_C7B5_LAUNCH_DMA,0x300u);
    checkU32("OFFSET_IN_UPPER",NV_COPY_C7B5_OFFSET_IN_UPPER,0x400u);
    checkU32("OFFSET_IN_LOWER",NV_COPY_C7B5_OFFSET_IN_LOWER,0x404u);
    checkU32("OFFSET_OUT_UPPER",NV_COPY_C7B5_OFFSET_OUT_UPPER,0x408u);
    checkU32("OFFSET_OUT_LOWER",NV_COPY_C7B5_OFFSET_OUT_LOWER,0x40cu);
    checkU32("LINE_LENGTH_IN",NV_COPY_C7B5_LINE_LENGTH_IN,0x418u);
    checkU32("LINE_COUNT",NV_COPY_C7B5_LINE_COUNT,0x41cu);
    checkU32("coherent sysmem target",NV_COPY_C7B5_DST_COHERENT_SYSMEM,1u);
    checkU32("rehearsal launch",NV_COPY_C7B5_LAUNCH_REHEARSAL,0x2186u);
}

static void testExactPlan(void)
{
    std::printf("\n2. exact one-line coherent-sink plan\n");
    const NVCopyU64 src=0x0000123456789abcull;
    const NVCopyU64 dst=0x0000001234500000ull;
    NVCopyMethod out[NV_COPY_C7B5_METHOD_COUNT]={};
    NVCopyU32 count=99u;
    checkU32("plan succeeds",nvCopyPlanToCoherentSink(src,dst,0x2000u,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_OK);
    checkU32("count",count,NV_COPY_C7B5_METHOD_COUNT);

    const NVCopyMethod want[NV_COPY_C7B5_METHOD_COUNT]={
        {0x264u,0x1u},
        {0x400u,0x00001234u},
        {0x404u,0x56789abcu},
        {0x408u,0x00000012u},
        {0x40cu,0x34500000u},
        {0x418u,0x00002000u},
        {0x41cu,0x00000001u},
        {0x300u,0x00002186u}
    };
    for(NVCopyU32 i=0;i<NV_COPY_C7B5_METHOD_COUNT;++i){
        checkU32("method",out[i].method,want[i].method);
        checkU32("value",out[i].value,want[i].value);
    }
    check("launch is last",out[NV_COPY_C7B5_METHOD_COUNT-1u].method==NV_COPY_C7B5_LAUNCH_DMA);
}

static void testAddressEdges(void)
{
    std::printf("\n3. address width edges\n");
    NVCopyMethod out[NV_COPY_C7B5_METHOD_COUNT]={};
    NVCopyU32 count=0;

    const NVCopyU64 srcLastPage=NV_COPY_C7B5_VA_LIMIT_49-4095ull;
    const NVCopyU64 dstLastPage=IG_SINK_PEER_DMA_LIMIT_POLICY-4095ull;
    checkU32("last source/dest pages accepted",
        nvCopyPlanToCoherentSink(srcLastPage,dstLastPage,4096u,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_OK);
    checkU32("source upper keeps 17 bits",out[1].value,0x0001ffffu);
    checkU32("dest upper within 41-bit policy",out[3].value,(NVCopyU32)(dstLastPage>>32));

    checkU32("source crosses 49-bit ceiling",
        nvCopyPlanToCoherentSink(srcLastPage,dstLastPage,8192u,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_SOURCE_RANGE);
    checkU32("dest crosses 41-bit ceiling",
        nvCopyPlanToCoherentSink(0x1000ull,dstLastPage,8192u,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_DEST_RANGE);
}

static void testAtomicRefusals(void)
{
    std::printf("\n4. refusal order and no partial method output\n");
    NVCopyMethod sentinel[NV_COPY_C7B5_METHOD_COUNT];
    for(NVCopyU32 i=0;i<NV_COPY_C7B5_METHOD_COUNT;++i){sentinel[i].method=0xaaaa0000u+i;sentinel[i].value=0x55550000u+i;}
    NVCopyMethod out[NV_COPY_C7B5_METHOD_COUNT];
    NVCopyU32 count=77u;

    std::memcpy(out,sentinel,sizeof(out));
    checkU32("small capacity",nvCopyPlanToCoherentSink(0x1000,0x2000,4,out,NV_COPY_C7B5_METHOD_COUNT-1u,&count),NV_COPY_PLAN_CAPACITY);
    check("capacity leaves output untouched",same(out,sentinel));
    checkU32("capacity resets count",count,0u);

    std::memcpy(out,sentinel,sizeof(out)); count=77u;
    checkU32("zero length",nvCopyPlanToCoherentSink(0x1000,0x2000,0,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_ZERO_LENGTH);
    check("zero length untouched",same(out,sentinel));

    std::memcpy(out,sentinel,sizeof(out)); count=77u;
    checkU32("zero source",nvCopyPlanToCoherentSink(0,0x2000,4,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_ZERO_SOURCE);
    check("zero source untouched",same(out,sentinel));

    std::memcpy(out,sentinel,sizeof(out)); count=77u;
    checkU32("zero dest",nvCopyPlanToCoherentSink(0x1000,0,4,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_ZERO_DEST);
    check("zero dest untouched",same(out,sentinel));

    std::memcpy(out,sentinel,sizeof(out)); count=77u;
    checkU32("unaligned peer IOVA",nvCopyPlanToCoherentSink(0x1000,0x2001,4,out,NV_COPY_C7B5_METHOD_COUNT,&count),NV_COPY_PLAN_DEST_UNALIGNED);
    check("unaligned dest untouched",same(out,sentinel));
}

static void testVerdicts(void)
{
    std::printf("\n5. verdict family\n");
    for(NVCopyU32 v=0;v<NV_COPY_PLAN_LIMIT;++v){
        check("verdict named",std::strcmp(nvCopyPlanVerdictName(v),"unknown")!=0);
        check("success equality",nvCopyPlanSucceeded(v)==(v==NV_COPY_PLAN_OK));
    }
}

int main(void)
{
    std::printf("NVAmpereCopy.hpp -- C7B5 coherent-sink copy rehearsal\n");
    testSourceBackedConstants();
    testExactPlan();
    testAddressEdges();
    testAtomicRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
