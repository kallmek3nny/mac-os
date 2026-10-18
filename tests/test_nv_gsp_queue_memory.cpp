// Host tests for NVGspQueueMemory.hpp. No IOKit, DMA mapping or hardware required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueMemory.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspMemU32 got, NVGspMemU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspMemU64 got, NVGspMemU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static NVGspQueuePlan normalLayout(bool cc=false) {
    NVGspQueuePlan p={};
    checkU32("layout",nvGspBuildQueuePlan(true,cc,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    return p;
}

static void fillDiscontiguous(NVGspMemU64 *pages, NVGspMemU32 count) {
    for (NVGspMemU32 i=0;i<count;++i) {
        // Intentionally alternate two distant regions. The table is a scatter
        // list of GA107-visible pages, not a promise of physical contiguity.
        const NVGspMemU64 region=(i&1u) ? 0x0000000800000000ull : 0x0000000100000000ull;
        pages[i]=region+(NVGspMemU64)i*0x2000ull;
    }
}

static void testNormalGeometryAndInitArgs(void) {
    std::printf("\n1. normal silicon shared-memory geometry and init arguments\n");
    const NVGspQueuePlan layout=normalLayout(false);
    checkU32("queue pages",layout.queuePageCount,128u);
    checkU32("table entries include table page",layout.pageTableEntryCount,129u);
    checkU32("table bytes",layout.pageTableBytes,0x1000u);
    checkU32("shared bytes",layout.sharedBytes,0x81000u);

    NVGspMemU64 pages[129]={};
    fillDiscontiguous(pages,129u);
    NVGspQueueMemoryPlan p={};
    checkU32("memory verdict",nvGspBuildQueueMemoryPlan(&layout,false,pages,129u,&p),NV_GSP_QUEUE_MEMORY_OK);
    checkU32("shared pages",p.sharedPageCount,129u);
    checkU32("entry count",p.pageTableEntryCount,129u);
    checkU32("command first page",p.commandFirstPage,1u);
    checkU32("command pages",p.commandPageCount,64u);
    checkU32("status first page",p.statusFirstPage,65u);
    checkU32("status pages",p.statusPageCount,64u);
    checkU64("first page",p.firstPageAddress,pages[0]);

    checkU32("ABI sizeof",(NVGspMemU32)sizeof(NVGspQueueInitArguments),NV_GSP_QUEUE_INIT_ARGUMENT_BYTES);
    checkU64("flags",p.initArgs.flags,0ull);
    checkU64("sharedMemPhysAddr",p.initArgs.sharedMemPhysAddr,pages[0]);
    checkU64("cmd offset",p.initArgs.cmdQueueOffset,0x1000ull);
    checkU64("status offset",p.initArgs.statQueueOffset,0x41000ull);
    checkU32("element hdr",p.initArgs.queueElementHdrSize,24u);
    checkU32("element min",p.initArgs.queueElementSizeMin,4096u);
    checkU32("element max",p.initArgs.queueElementSizeMax,65536u);
    checkU32("header align",p.initArgs.queueHeaderAlign,4u);
    checkU32("entry align shift",p.initArgs.queueElementAlign,12u);
    checkU32("pte count",p.initArgs.pageTableEntryCount,129u);
    for (unsigned i=0;i<sizeof(p.initArgs.reserved);++i)
        check("reserved zero",p.initArgs.reserved[i]==0u);
}

static void testExactScatterTable(void) {
    std::printf("\n2. page table preserves every discontiguous GPU-visible address\n");
    const NVGspQueuePlan layout=normalLayout(false);
    NVGspMemU64 pages[129]={};
    fillDiscontiguous(pages,129u);
    NVGspMemU64 table[129];
    std::memset(table,0xa5,sizeof(table));
    NVGspMemU32 written=0xffffffffu;
    checkU32("write verdict",nvGspWriteQueuePageTable(&layout,pages,129u,table,129u,&written),NV_GSP_QUEUE_MEMORY_OK);
    checkU32("written",written,129u);
    for (NVGspMemU32 i=0;i<129u;++i) {
        char label[48]; std::snprintf(label,sizeof(label),"table[%u]",i);
        checkU64(label,table[i],pages[i]);
    }
    checkU64("entry 0 self table page",table[0],pages[0]);
    checkU64("entry 1 command header page",table[1],pages[1]);
    checkU64("entry 65 status header page",table[65],pages[65]);
    check("not contiguous 0->1",table[1]!=table[0]+4096ull);
}

static void testConfidentialFlagAndHeader(void) {
    std::printf("\n3. confidential-compute flag follows queue plan\n");
    const NVGspQueuePlan layout=normalLayout(true);
    NVGspMemU64 pages[129]={};
    fillDiscontiguous(pages,129u);
    NVGspQueueMemoryPlan p={};
    checkU32("cc verdict",nvGspBuildQueueMemoryPlan(&layout,true,pages,129u,&p),NV_GSP_QUEUE_MEMORY_OK);
    checkU64("encryption flag",p.initArgs.flags,NV_GSP_QUEUE_INIT_FLAG_ENCRYPTION);
    checkU32("encrypted element header",p.initArgs.queueElementHdrSize,40u);
}

static void testRefusalsAreAtomic(void) {
    std::printf("\n4. malformed page spans refuse before output mutation\n");
    const NVGspQueuePlan layout=normalLayout(false);
    NVGspMemU64 pages[129]={};
    fillDiscontiguous(pages,129u);

    NVGspQueueMemoryPlan p;
    std::memset(&p,0x5a,sizeof(p));
    const NVGspQueueMemoryPlan before=p;
    checkU32("short page span",nvGspBuildQueueMemoryPlan(&layout,false,pages,128u,&p),NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT);
    check("short atomic",std::memcmp(&p,&before,sizeof(p))==0);

    NVGspMemU64 misaligned[129];
    std::memcpy(misaligned,pages,sizeof(pages));
    misaligned[37]|=0x80ull;
    checkU32("misaligned page",nvGspBuildQueueMemoryPlan(&layout,false,misaligned,129u,&p),NV_GSP_QUEUE_MEMORY_BAD_PAGE_ADDRESS);
    check("misaligned atomic",std::memcmp(&p,&before,sizeof(p))==0);

    NVGspMemU64 aliased[129];
    std::memcpy(aliased,pages,sizeof(pages));
    aliased[65]=aliased[1]; // Status header may never alias command header.
    checkU32("aliased page-plan refusal",
             nvGspBuildQueueMemoryPlan(&layout,false,aliased,129u,&p),
             NV_GSP_QUEUE_MEMORY_ALIASED_PAGE);
    check("aliased plan atomic",std::memcmp(&p,&before,sizeof(p))==0);

    NVGspMemU64 table[129];
    std::memset(table,0x3c,sizeof(table));
    NVGspMemU64 tableBefore[129];
    std::memcpy(tableBefore,table,sizeof(table));
    NVGspMemU32 written=99u;
    checkU32("short output",nvGspWriteQueuePageTable(&layout,pages,129u,table,128u,&written),NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT);
    checkU32("written reset",written,0u);
    check("table untouched",std::memcmp(table,tableBefore,sizeof(table))==0);

    written=99u;
    checkU32("bad page table input",nvGspWriteQueuePageTable(&layout,misaligned,129u,table,129u,&written),NV_GSP_QUEUE_MEMORY_BAD_PAGE_ADDRESS);
    checkU32("bad page written reset",written,0u);
    check("bad page table untouched",std::memcmp(table,tableBefore,sizeof(table))==0);

    written=99u;
    checkU32("aliased table refusal",
             nvGspWriteQueuePageTable(&layout,aliased,129u,table,129u,&written),
             NV_GSP_QUEUE_MEMORY_ALIASED_PAGE);
    checkU32("aliased table count reset",written,0u);
    check("aliased table untouched",
          std::memcmp(table,tableBefore,sizeof(table))==0);

    checkU32("null layout",nvGspBuildQueueMemoryPlan(nullptr,false,pages,129u,&p),NV_GSP_QUEUE_MEMORY_NULL_PLAN);
    checkU32("null pages",nvGspBuildQueueMemoryPlan(&layout,false,nullptr,129u,&p),NV_GSP_QUEUE_MEMORY_NULL_PAGES);
    checkU32("null output",nvGspBuildQueueMemoryPlan(&layout,false,pages,129u,nullptr),NV_GSP_QUEUE_MEMORY_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspMemU32 v=0u;v<NV_GSP_QUEUE_MEMORY_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueMemoryVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueMemorySucceeded(v)==(v==NV_GSP_QUEUE_MEMORY_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueMemory.hpp -- GSP shared queue page-table/init contract\n");
    testNormalGeometryAndInitArgs();
    testExactScatterTable();
    testConfidentialFlagAndHeader();
    testRefusalsAreAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
