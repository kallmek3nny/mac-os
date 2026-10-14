// Host tests for NVGspCachedArgs.hpp. No IOKit, GSP memory or MMIO required.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include "NVGspCachedArgs.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspArgsU32 got, NVGspArgsU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspArgsU64 got, NVGspArgsU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static void putLe16(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)(v&0xffu); p[1]=(unsigned char)((v>>8)&0xffu);
}
static void putLe32(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)(v&0xffu); p[1]=(unsigned char)((v>>8)&0xffu);
    p[2]=(unsigned char)((v>>16)&0xffu); p[3]=(unsigned char)((v>>24)&0xffu);
}
static void seedValidPage(unsigned char page[NV_GSP_CACHED_ARGS_PAGE_BYTES], unsigned size=NV_GSP_CACHED_ARGS_CURRENT_BYTES) {
    for (unsigned i=0;i<NV_GSP_CACHED_ARGS_PAGE_BYTES;++i)
        page[i]=(unsigned char)((i*37u+11u)&0xffu);
    putLe32(page,NV_GSP_CACHED_ARGS_MAGIC);
    putLe16(page+4u,size);
    page[6]=0u; page[7]=0u;
}

static NVGspQueueInitArguments sampleQueueArgs(void) {
    NVGspQueueInitArguments q={};
    q.flags=0x1122334455667788ull;
    q.sharedMemPhysAddr=0x0000001234500000ull;
    q.cmdQueueOffset=0x1000ull;
    q.statQueueOffset=0x41000ull;
    q.queueElementHdrSize=24u;
    q.queueElementSizeMin=4096u;
    q.queueElementSizeMax=65536u;
    q.queueHeaderAlign=4u;
    q.queueElementAlign=12u;
    q.pageTableEntryCount=129u;
    for (unsigned i=0;i<sizeof(q.reserved);++i) q.reserved[i]=0u;
    return q;
}

static void testAbiLayout(void) {
    std::printf("\n1. current cached-args and LibOS ABI layout\n");
    checkU32("cached sizeof",(NVGspArgsU32)sizeof(NVGspCachedArguments),NV_GSP_CACHED_ARGS_CURRENT_BYTES);
    checkU32("magic offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,magic),0u);
    checkU32("size offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,size),4u);
    checkU32("flags offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,flags),8u);
    checkU32("queue offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,messageQueueInitArguments),16u);
    checkU32("queue bytes",(NVGspArgsU32)sizeof(NVGspQueueInitArguments),64u);
    checkU32("sr offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,srInitArguments),80u);
    checkU32("gpu instance offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,gpuInstance),100u);
    checkU32("profiler offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,profilerArgs),104u);
    checkU32("sysmem heap offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,sysmemHeapArgs),120u);
    checkU32("state monitor offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,rmStateMonitorBufferArgs),136u);
    checkU32("bindata offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,bindataArgs),152u);
    checkU32("vbios offset",(NVGspArgsU32)offsetof(NVGspCachedArguments,vbiosOverrideArgs),168u);
    checkU32("LibOS region sizeof",(NVGspArgsU32)sizeof(NVGspLibosMemoryRegion),NV_GSP_LIBOS_REGION_BYTES);
}

static void testInitArgId(void) {
    std::printf("\n2. exact open-RM 8-byte init-argument ID packing\n");
    checkU64("RMARGS",nvGspGenerateInitArgId("RMARGS"),NV_GSP_LIBOS_RMARGS_ID);
    checkU64("eight chars",nvGspGenerateInitArgId("ABCDEFGH"),0x4142434445464748ull);
    checkU64("ninth ignored",nvGspGenerateInitArgId("ABCDEFGHI"),0x4142434445464748ull);
    checkU64("short",nvGspGenerateInitArgId("A"),0x41ull);
    checkU64("empty",nvGspGenerateInitArgId(""),0ull);
    checkU64("null",nvGspGenerateInitArgId(nullptr),0ull);
}

static void testPlanAndRegion(void) {
    std::printf("\n3. validated RMARGS overlay plan and LibOS region\n");
    unsigned char page[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    seedValidPage(page);
    NVGspCachedArgsOverlayPlan p={};
    const NVGspArgsU64 rmargs=0x0000000abcde0000ull;
    checkU32("plan verdict",nvGspPlanCachedArgsQueueOverlay(page,sizeof(page),rmargs,&p),NV_GSP_CACHED_ARGS_OK);
    checkU32("cached size",p.cachedArgsSize,NV_GSP_CACHED_ARGS_CURRENT_BYTES);
    checkU32("queue offset",p.queueOffset,16u);
    checkU32("queue bytes",p.queueBytes,64u);
    checkU32("preserved prefix",p.preservedPrefixBytes,16u);
    checkU32("preserved suffix",p.preservedSuffixOffset,80u);
    checkU64("region id",p.rmArgsRegion.id8,NV_GSP_LIBOS_RMARGS_ID);
    checkU64("region pa",p.rmArgsRegion.pa,rmargs);
    checkU64("region size",p.rmArgsRegion.size,4096ull);
    checkU32("region kind",p.rmArgsRegion.kind,NV_GSP_LIBOS_REGION_CONTIGUOUS);
    checkU32("region loc",p.rmArgsRegion.loc,NV_GSP_LIBOS_REGION_LOC_SYSMEM);
    for (unsigned i=0;i<sizeof(p.rmArgsRegion.padding);++i)
        check("region padding zero",p.rmArgsRegion.padding[i]==0u);
    check("full fence after region list",p.fullFenceAfterLibosRegionList);
}

static void testOverlayPreservesEverythingElse(void) {
    std::printf("\n4. queue overlay mutates only RMARGS bytes 16..79\n");
    unsigned char page[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    seedValidPage(page);
    unsigned char before[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    std::memcpy(before,page,sizeof(page));
    const NVGspQueueInitArguments q=sampleQueueArgs();
    NVGspCachedArgsOverlayPlan p={};
    checkU32("apply verdict",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x12345000ull,&q,&p),NV_GSP_CACHED_ARGS_OK);

    check("prefix untouched",std::memcmp(page,before,16u)==0);
    check("queue exact",std::memcmp(page+16u,&q,sizeof(q))==0);
    check("suffix untouched",std::memcmp(page+80u,before+80u,sizeof(page)-80u)==0);
    checkU32("magic survives",nvGspReadLe32(page),NV_GSP_CACHED_ARGS_MAGIC);
    checkU32("size survives",nvGspReadLe16(page+4u),NV_GSP_CACHED_ARGS_CURRENT_BYTES);
}

static void testAppendCompatibleSize(void) {
    std::printf("\n5. ABI-stable appended cached args remain overlay-compatible\n");
    unsigned char page[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    seedValidPage(page,256u);
    NVGspCachedArgsOverlayPlan p={};
    checkU32("larger size accepted",nvGspPlanCachedArgsQueueOverlay(page,sizeof(page),0x40000000ull,&p),NV_GSP_CACHED_ARGS_OK);
    checkU32("reported larger size",p.cachedArgsSize,256u);
}

static void testRefusalsAreAtomic(void) {
    std::printf("\n6. malformed RMARGS refuses before page mutation\n");
    const NVGspQueueInitArguments q=sampleQueueArgs();
    unsigned char page[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    NVGspCachedArgsOverlayPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspCachedArgsOverlayPlan beforePlan=p;

    seedValidPage(page);
    unsigned char before[NV_GSP_CACHED_ARGS_PAGE_BYTES];
    std::memcpy(before,page,sizeof(page));
    putLe32(page,0xdeadbeefu);
    std::memcpy(before,page,sizeof(page));
    checkU32("bad magic",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000000ull,&q,&p),NV_GSP_CACHED_ARGS_BAD_MAGIC);
    check("bad magic page atomic",std::memcmp(page,before,sizeof(page))==0);
    check("bad magic plan atomic",std::memcmp(&p,&beforePlan,sizeof(p))==0);

    seedValidPage(page,79u);
    std::memcpy(before,page,sizeof(page));
    checkU32("too-small struct",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000000ull,&q,&p),NV_GSP_CACHED_ARGS_BAD_SIZE);
    check("small page atomic",std::memcmp(page,before,sizeof(page))==0);

    seedValidPage(page,4097u);
    std::memcpy(before,page,sizeof(page));
    checkU32("oversize struct",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000000ull,&q,&p),NV_GSP_CACHED_ARGS_BAD_SIZE);
    check("oversize page atomic",std::memcmp(page,before,sizeof(page))==0);

    seedValidPage(page);
    std::memcpy(before,page,sizeof(page));
    checkU32("misaligned GPU address",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000080ull,&q,&p),NV_GSP_CACHED_ARGS_BAD_RMARGS_ADDRESS);
    check("address page atomic",std::memcmp(page,before,sizeof(page))==0);

    checkU32("short page",nvGspApplyCachedArgsQueueOverlay(page,4095u,0x10000000ull,&q,&p),NV_GSP_CACHED_ARGS_PAGE_TOO_SHORT);
    checkU32("null page",nvGspApplyCachedArgsQueueOverlay(nullptr,sizeof(page),0x10000000ull,&q,&p),NV_GSP_CACHED_ARGS_NULL_PAGE);
    checkU32("null queue",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000000ull,nullptr,&p),NV_GSP_CACHED_ARGS_NULL_QUEUE);
    checkU32("null output",nvGspApplyCachedArgsQueueOverlay(page,sizeof(page),0x10000000ull,&q,nullptr),NV_GSP_CACHED_ARGS_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n7. verdict family\n");
    for (NVGspArgsU32 v=0u;v<NV_GSP_CACHED_ARGS_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspCachedArgsVerdictName(v),"unknown")!=0);
        check("success equality",nvGspCachedArgsSucceeded(v)==(v==NV_GSP_CACHED_ARGS_OK));
    }
}

int main(void) {
    std::printf("NVGspCachedArgs.hpp -- queue overlay into validated GSP RMARGS\n");
    testAbiLayout();
    testInitArgId();
    testPlanAndRegion();
    testOverlayPreservesEverythingElse();
    testAppendCompatibleSize();
    testRefusalsAreAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
