// End-to-end pure rehearsal for queue args -> RMARGS -> LibOS list -> mailbox.
#include <cstdio>
#include <cstring>
#include "NVGspLibosInit.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, unsigned got, unsigned want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, unsigned long long got, unsigned long long want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

int main(void)
{
    std::printf("GSP bootargs rehearsal -- queue args -> RMARGS -> LibOS -> mailbox\n");

    NVGspQueuePlan layout={};
    checkU32("queue layout",nvGspBuildQueuePlan(true,false,0u,&layout),NV_GSP_QUEUE_PLAN_OK);

    unsigned long long pageAddress[129]={};
    const unsigned long long queueBase=0x0000001800000000ull;
    for (unsigned i=0;i<129u;++i) pageAddress[i]=queueBase+(unsigned long long)i*0x1000ull;

    NVGspQueueMemoryPlan memory={};
    checkU32("queue memory",nvGspBuildQueueMemoryPlan(&layout,false,pageAddress,129u,&memory),NV_GSP_QUEUE_MEMORY_OK);
    checkU64("sharedMem",memory.initArgs.sharedMemPhysAddr,queueBase);
    checkU64("cmd offset",memory.initArgs.cmdQueueOffset,0x1000ull);
    checkU64("status offset",memory.initArgs.statQueueOffset,0x41000ull);
    checkU32("PTE count",memory.initArgs.pageTableEntryCount,129u);

    unsigned char rmargs[NV_GSP_CACHED_ARGS_PAGE_BYTES]={};
    NVGspCachedArguments *cached=reinterpret_cast<NVGspCachedArguments *>(rmargs);
    cached->magic=NV_GSP_CACHED_ARGS_MAGIC;
    cached->size=(NVGspArgsU16)sizeof(NVGspCachedArguments);
    cached->gpuInstance=0u;

    const unsigned long long rmGpu=0x0000001900000000ull;
    NVGspCachedArgsOverlayPlan overlay={};
    checkU32("RMARGS overlay",nvGspApplyCachedArgsQueueOverlay(
        rmargs,sizeof(rmargs),rmGpu,&memory.initArgs,&overlay),NV_GSP_CACHED_ARGS_OK);
    checkU32("overlay offset",overlay.queueOffset,16u);
    checkU32("overlay bytes",overlay.queueBytes,64u);
    checkU64("overlay RMARGS PA",overlay.rmArgsRegion.pa,rmGpu);

    NVGspCachedArguments *after=reinterpret_cast<NVGspCachedArguments *>(rmargs);
    checkU32("magic preserved",after->magic,NV_GSP_CACHED_ARGS_MAGIC);
    checkU32("size preserved",after->size,(unsigned)sizeof(NVGspCachedArguments));
    checkU64("queue sharedMem installed",after->messageQueueInitArguments.sharedMemPhysAddr,queueBase);
    checkU64("queue status offset installed",after->messageQueueInitArguments.statQueueOffset,0x41000ull);
    checkU32("gpuInstance preserved",after->gpuInstance,0u);

    NVGspLibosMemoryRegion list[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    list[0].id8=nvGspGenerateInitArgId("LOG0");
    list[0].pa=0x0000001a00000000ull;
    list[0].size=0x1000ull;
    list[0].kind=NV_GSP_LIBOS_REGION_CONTIGUOUS;
    list[0].loc=NV_GSP_LIBOS_REGION_LOC_SYSMEM;

    NVGspLibosInitPlan listPlan={};
    checkU32("LibOS append",nvGspApplyRmArgsRegion(
        list,sizeof(list),rmGpu,sizeof(rmargs),&listPlan),NV_GSP_LIBOS_INIT_OK);
    checkU32("RMARGS slot",listPlan.rmArgsIndex,1u);
    checkU32("terminator slot",listPlan.terminatorIndex,2u);
    checkU64("list RMARGS id",list[1].id8,NV_GSP_LIBOS_RMARGS_ID);
    checkU64("list RMARGS PA",list[1].pa,rmGpu);
    check("terminator zero",nvGspLibosRegionEmpty(list[2]));

    const unsigned long long listGpu=0x0000001babcdef000ull;
    NVGspLibosMailboxPlan mailbox={};
    checkU32("mailbox plan",nvGspPlanLibosMailboxWrites(listGpu,&mailbox),NV_GSP_LIBOS_MAILBOX_OK);
    checkU32("mailbox0 reg",mailbox.writes[0].reg,0x00110040u);
    checkU32("mailbox0 value",mailbox.writes[0].value,0xbcdef000u);
    checkU32("mailbox1 reg",mailbox.writes[1].reg,0x00110044u);
    checkU32("mailbox1 value",mailbox.writes[1].value,0x0000001au);

    // A malformed RMARGS address must fail before either page is touched.
    unsigned char rmBefore[sizeof(rmargs)];
    std::memcpy(rmBefore,rmargs,sizeof(rmargs));
    NVGspCachedArgsOverlayPlan poison;
    std::memset(&poison,0xa5,sizeof(poison));
    checkU32("misaligned overlay refused",nvGspApplyCachedArgsQueueOverlay(
        rmargs,sizeof(rmargs),rmGpu+1ull,&memory.initArgs,&poison),NV_GSP_CACHED_ARGS_BAD_RMARGS_ADDRESS);
    check("RMARGS atomic",std::memcmp(rmargs,rmBefore,sizeof(rmargs))==0);

    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
