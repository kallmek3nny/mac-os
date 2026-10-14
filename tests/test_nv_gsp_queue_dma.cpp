// Host tests for NVGspQueueDma.hpp. No IOKit, IOMMU or hardware required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueDma.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspDmaU32 got, NVGspDmaU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspDmaU64 got, NVGspDmaU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static NVGspQueuePlan normalLayout(void) {
    NVGspQueuePlan p={};
    checkU32("layout",nvGspBuildQueuePlan(true,false,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    return p;
}

static void testSingleContiguous(void) {
    std::printf("\n1. one contiguous IOVA becomes 129 queue pages\n");
    const NVGspQueuePlan layout=normalLayout();
    const NVGspQueueDmaSegment seg={0x0000001234500000ull,0x81000ull};
    NVGspDmaU64 pages[129]={};
    NVGspQueueDmaPlan p={};
    checkU32("verdict",nvGspFlattenQueueDmaSegments(&layout,&seg,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_OK);
    checkU32("input segments",p.inputSegmentCount,1u);
    checkU32("pages",p.outputPageCount,129u);
    checkU64("bytes",p.mappedBytes,0x81000ull);
    checkU64("first",p.firstPageAddress,seg.address);
    checkU64("last",p.lastPageAddress,seg.address+0x80000ull);
    check("single contiguous",p.inputWasSingleContiguousSegment);
    checkU64("page 0",pages[0],seg.address);
    checkU64("page 1 command",pages[1],seg.address+0x1000ull);
    checkU64("page 65 status",pages[65],seg.address+0x41000ull);
    checkU64("page 128",pages[128],seg.address+0x80000ull);
}

static void testThreeNoncontiguous(void) {
    std::printf("\n2. table, command and status may be separate IOVA segments\n");
    const NVGspQueuePlan layout=normalLayout();
    const NVGspQueueDmaSegment seg[3]={
        {0x0000000100000000ull,0x1000ull},
        {0x0000000800000000ull,0x40000ull},
        {0x0000000200000000ull,0x40000ull}
    };
    NVGspDmaU64 pages[129]={};
    NVGspQueueDmaPlan p={};
    checkU32("verdict",nvGspFlattenQueueDmaSegments(&layout,seg,3u,pages,129u,&p),NV_GSP_QUEUE_DMA_OK);
    checkU32("segments",p.inputSegmentCount,3u);
    checkU32("pages",p.outputPageCount,129u);
    check("not one segment",!p.inputWasSingleContiguousSegment);
    checkU64("table page",pages[0],0x0000000100000000ull);
    checkU64("command page 0",pages[1],0x0000000800000000ull);
    checkU64("command last",pages[64],0x000000080003f000ull);
    checkU64("status page 0",pages[65],0x0000000200000000ull);
    checkU64("status last",pages[128],0x000000020003f000ull);
}

static void testOneSegmentPerPage(void) {
    std::printf("\n3. 129 page-sized scatter segments preserve descriptor order\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspQueueDmaSegment seg[129]={};
    for (NVGspDmaU32 i=0;i<129u;++i) {
        seg[i].address=0x0000000400000000ull+(NVGspDmaU64)i*0x3000ull;
        seg[i].length=0x1000ull;
    }
    NVGspDmaU64 pages[129]={};
    NVGspQueueDmaPlan p={};
    checkU32("verdict",nvGspFlattenQueueDmaSegments(&layout,seg,129u,pages,129u,&p),NV_GSP_QUEUE_DMA_OK);
    checkU32("segments",p.inputSegmentCount,129u);
    checkU32("pages",p.outputPageCount,129u);
    for (NVGspDmaU32 i=0;i<129u;++i) {
        char label[40]; std::snprintf(label,sizeof(label),"page[%u]",i);
        checkU64(label,pages[i],seg[i].address);
    }
}

static void testRefusalsAreAtomic(void) {
    std::printf("\n4. bad DMA spans refuse before page-table mutation\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspDmaU64 pages[129];
    std::memset(pages,0x5a,sizeof(pages));
    NVGspDmaU64 beforePages[129];
    std::memcpy(beforePages,pages,sizeof(pages));
    NVGspQueueDmaPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspQueueDmaPlan beforePlan=p;

    NVGspQueueDmaSegment shortSeg={0x100000000ull,0x80000ull};
    checkU32("short transfer",nvGspFlattenQueueDmaSegments(&layout,&shortSeg,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_SHORT_TRANSFER);
    check("short pages atomic",std::memcmp(pages,beforePages,sizeof(pages))==0);
    check("short plan atomic",std::memcmp(&p,&beforePlan,sizeof(p))==0);

    NVGspQueueDmaSegment longSeg={0x100000000ull,0x82000ull};
    checkU32("long transfer",nvGspFlattenQueueDmaSegments(&layout,&longSeg,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_SHORT_TRANSFER);
    check("long pages atomic",std::memcmp(pages,beforePages,sizeof(pages))==0);

    NVGspQueueDmaSegment misaligned={0x100000080ull,0x81000ull};
    checkU32("misaligned address",nvGspFlattenQueueDmaSegments(&layout,&misaligned,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_BAD_SEGMENT);
    check("misaligned pages atomic",std::memcmp(pages,beforePages,sizeof(pages))==0);

    NVGspQueueDmaSegment badLen={0x100000000ull,0x80fffull};
    checkU32("misaligned length",nvGspFlattenQueueDmaSegments(&layout,&badLen,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_BAD_SEGMENT);
    check("bad length pages atomic",std::memcmp(pages,beforePages,sizeof(pages))==0);

    NVGspQueueDmaSegment good={0x100000000ull,0x81000ull};
    checkU32("short output",nvGspFlattenQueueDmaSegments(&layout,&good,1u,pages,128u,&p),NV_GSP_QUEUE_DMA_OUTPUT_SHORT);
    check("short output atomic",std::memcmp(pages,beforePages,sizeof(pages))==0);
    checkU32("zero segments",nvGspFlattenQueueDmaSegments(&layout,&good,0u,pages,129u,&p),NV_GSP_QUEUE_DMA_BAD_SEGMENT_COUNT);
    checkU32("null layout",nvGspFlattenQueueDmaSegments(nullptr,&good,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_NULL_LAYOUT);
    checkU32("null segments",nvGspFlattenQueueDmaSegments(&layout,nullptr,1u,pages,129u,&p),NV_GSP_QUEUE_DMA_NULL_SEGMENTS);
    checkU32("null pages",nvGspFlattenQueueDmaSegments(&layout,&good,1u,nullptr,129u,&p),NV_GSP_QUEUE_DMA_NULL_OUTPUT);
    checkU32("null plan",nvGspFlattenQueueDmaSegments(&layout,&good,1u,pages,129u,nullptr),NV_GSP_QUEUE_DMA_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspDmaU32 v=0u;v<NV_GSP_QUEUE_DMA_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueDmaVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueDmaSucceeded(v)==(v==NV_GSP_QUEUE_DMA_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueDma.hpp -- GSP queue DMA segment flattener\n");
    testSingleContiguous();
    testThreeNoncontiguous();
    testOneSegmentPerPage();
    testRefusalsAreAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
