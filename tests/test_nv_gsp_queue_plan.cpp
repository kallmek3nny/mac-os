// Host tests for NVGspQueuePlan.hpp. No IOKit, GSP, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVGspQueuePlan.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspQueueU32 got, NVGspQueueU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspQueueU64 got, NVGspQueueU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,(unsigned long long)got,(unsigned long long)want); }
}

static void testConstants(void) {
    std::printf("\n1. open-RM queue constants\n");
    checkU32("page bytes",NV_GSP_QUEUE_PAGE_BYTES,4096u);
    checkU32("page shift",NV_GSP_QUEUE_PAGE_SHIFT,12u);
    checkU32("PTE bytes",NV_GSP_QUEUE_PTE_BYTES,8u);
    checkU32("silicon command",NV_GSP_QUEUE_COMMAND_SILICON_BYTES,0x40000u);
    checkU32("status default",NV_GSP_QUEUE_STATUS_DEFAULT_BYTES,0x40000u);
    checkU32("element min",NV_GSP_QUEUE_ELEMENT_MIN_BYTES,4096u);
    checkU32("element max",NV_GSP_QUEUE_ELEMENT_MAX_BYTES,65536u);
    checkU32("header align",NV_GSP_QUEUE_HEADER_ALIGN_BYTES,4u);
    checkU32("element align shift",NV_GSP_QUEUE_ELEMENT_ALIGN_SHIFT,12u);
    checkU32("no-CC framing",NV_GSP_QUEUE_ELEMENT_HEADER_NO_CC_BYTES,24u);
    checkU32("CC framing",NV_GSP_QUEUE_ELEMENT_HEADER_CC_BYTES,40u);
}

static void testSiliconGolden(void) {
    std::printf("\n2. normal silicon non-CC golden layout\n");
    NVGspQueuePlan p={};
    checkU32("plan verdict",nvGspBuildQueuePlan(true,false,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    checkU32("command bytes",p.commandQueueBytes,0x40000u);
    checkU32("status bytes",p.statusQueueBytes,0x40000u);
    checkU32("header bytes",p.queueElementHeaderBytes,24u);
    checkU32("queue pages",p.queuePageCount,128u);
    checkU32("PTE entries",p.pageTableEntryCount,129u);
    checkU32("page table bytes",p.pageTableBytes,0x1000u);
    checkU32("command offset",p.commandQueueOffset,0x1000u);
    checkU32("status offset",p.statusQueueOffset,0x41000u);
    checkU32("shared bytes",p.sharedBytes,0x81000u);
    checkU32("max RPC",p.maxRpcBytes,65512u);
}

static void testCcAndPresilicon(void) {
    std::printf("\n3. CC framing and pre-silicon command sizing\n");
    NVGspQueuePlan p={};
    checkU32("CC plan",nvGspBuildQueuePlan(true,true,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    checkU32("CC header",p.queueElementHeaderBytes,40u);
    checkU32("CC max RPC",p.maxRpcBytes,65496u);
    checkU32("CC geometry unchanged",p.sharedBytes,0x81000u);

    checkU32("pre-silicon plan",nvGspBuildQueuePlan(false,false,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    checkU32("pre-silicon command",p.commandQueueBytes,0x180000u);
    checkU32("pre-silicon queue pages",p.queuePageCount,448u);
    checkU32("pre-silicon PTE entries",p.pageTableEntryCount,449u);
    checkU32("pre-silicon page table",p.pageTableBytes,0x1000u);
    checkU32("pre-silicon status offset",p.statusQueueOffset,0x181000u);
    checkU32("pre-silicon shared bytes",p.sharedBytes,0x1c1000u);
}

static void testStatusOverride(void) {
    std::printf("\n4. status override follows source max/page alignment\n");
    NVGspQueuePlan p={};
    checkU32("tiny override accepted",
        nvGspBuildQueuePlan(true,false,1u,&p),NV_GSP_QUEUE_PLAN_OK);
    checkU32("tiny raised to one max element",p.statusQueueBytes,0x10000u);
    checkU32("tiny status offset",p.statusQueueOffset,0x41000u);
    checkU32("tiny shared",p.sharedBytes,0x51000u);
    checkU32("tiny queue pages",p.queuePageCount,80u);
    checkU32("tiny PTE entries",p.pageTableEntryCount,81u);

    checkU32("unaligned override accepted",
        nvGspBuildQueuePlan(true,false,0x12345u,&p),NV_GSP_QUEUE_PLAN_OK);
    checkU32("override page rounded",p.statusQueueBytes,0x13000u);
    checkU32("rounded shared",p.sharedBytes,0x54000u);
}

static void testHelpersAndRefusals(void) {
    std::printf("\n5. helper math and atomic refusals\n");
    checkU64("align exact",nvGspQueueAlignUp(0x2000ull,0x1000ull),0x2000ull);
    checkU64("align upward",nvGspQueueAlignUp(0x2001ull,0x1000ull),0x3000ull);
    checkU64("bad alignment",nvGspQueueAlignUp(123ull,3ull),0ull);
    checkU64("zero alignment",nvGspQueueAlignUp(123ull,0ull),0ull);

    NVGspQueuePlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspQueuePlan before=p;
    checkU32("u32-max override overflows total",
        nvGspBuildQueuePlan(true,false,0xffffffffu,&p),NV_GSP_QUEUE_PLAN_OVERFLOW);
    check("overflow leaves output untouched",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null output refused",nvGspBuildQueuePlan(true,false,0u,nullptr),NV_GSP_QUEUE_PLAN_NULL_OUTPUT);

    for (NVGspQueueU32 v=0;v<NV_GSP_QUEUE_PLAN_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueuePlanVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueuePlanSucceeded(v)==(v==NV_GSP_QUEUE_PLAN_OK));
    }
}

int main(void) {
    std::printf("NVGspQueuePlan.hpp -- GSP-RM shared queue geometry\n");
    testConstants();
    testSiliconGolden();
    testCcAndPresilicon();
    testStatusOverride();
    testHelpersAndRefusals();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
