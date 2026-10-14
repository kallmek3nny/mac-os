// Host tests for NVGspQueueTx.hpp. No IOKit, GSP, shared memory or MMIO required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueTx.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n", label); }
}
static void checkU32(const char *label, NVGspTxU32 got, NVGspTxU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static void testHeaderGolden(void) {
    std::printf("\n1. exact MSGQ v2 TX header for silicon command queue\n");
    NVGspMsgqTxHeaderPlan h={};
    checkU32("header verdict",nvGspBuildMsgqTxHeader(0x40000u,&h),NV_GSP_QUEUE_TX_OK);
    const NVGspTxU32 expected[8]={
        0x00000002u, 0x00040000u, 0x00001000u, 0x0000003fu,
        0x00001000u, 0u, 0u, 0u
    };
    for (NVGspTxU32 i=0;i<8u;++i) {
        char label[40]; std::snprintf(label,sizeof(label),"header[%u]",i);
        checkU32(label,h.dwords[i],expected[i]);
    }
    checkU32("entry offset",h.entryOffset,0x1000u);
    checkU32("message size",h.messageSize,0x1000u);
    checkU32("message count",h.messageCount,63u);
}

static void testSingleRecord(void) {
    std::printf("\n2. one short RPC consumes one full 4 KiB slot\n");
    NVGspQueueTxPlan p={};
    checkU32("plan verdict",nvGspPlanQueueTx(0x40000u,0u,0u,0u,56u,&p),NV_GSP_QUEUE_TX_OK);
    checkU32("effective read",p.effectiveReadPtr,0u);
    checkU32("free before",p.freeBefore,63u);
    checkU32("elements",p.elementCount,1u);
    checkU32("payload bytes",p.payloadBytes,56u);
    checkU32("copy bytes",p.copyBytes,4096u);
    checkU32("padding",p.paddingBytes,4040u);
    checkU32("slot",p.slot[0],0u);
    checkU32("offset",p.byteOffset[0],0x1000u);
    checkU32("new write",p.newWritePtr,1u);
    checkU32("TX_HEAD",p.txHeadValue,1u);
}

static void testPhysicalRingWrap(void) {
    std::printf("\n3. multi-element record wraps slot 62 to slot 0\n");
    NVGspQueueTxPlan p={};
    checkU32("wrap verdict",nvGspPlanQueueTx(0x40000u,62u,61u,61u,8192u,&p),NV_GSP_QUEUE_TX_OK);
    checkU32("free before",p.freeBefore,62u);
    checkU32("elements",p.elementCount,2u);
    checkU32("slot 0",p.slot[0],62u);
    checkU32("offset 0",p.byteOffset[0],0x3f000u);
    checkU32("slot 1",p.slot[1],0u);
    checkU32("offset 1",p.byteOffset[1],0x1000u);
    checkU32("new monotonic write",p.newWritePtr,64u);
}

static void testBusyAndResetRecovery(void) {
    std::printf("\n4. busy refusal and NVIDIA tail-reset recovery\n");
    NVGspQueueTxPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspQueueTxPlan before=p;
    checkU32("only two slots free",nvGspPlanQueueTx(0x40000u,62u,1u,1u,0x3000u,&p),NV_GSP_QUEUE_TX_BUSY);
    check("busy is atomic",std::memcmp(&p,&before,sizeof(p))==0);

    NVGspQueueTxPlan reset={};
    checkU32("reset recovery",nvGspPlanQueueTx(0x40000u,63u,0u,0u,56u,&reset),NV_GSP_QUEUE_TX_OK);
    checkU32("zero tail promoted to current head",reset.effectiveReadPtr,63u);
    checkU32("queue treated empty",reset.freeBefore,63u);
    checkU32("head slot wraps to zero",reset.slot[0],0u);
    checkU32("new head",reset.newWritePtr,64u);

    NVGspQueueTxPlan stale={};
    checkU32("impossible observed tail falls back to cache",
        nvGspPlanQueueTx(0x40000u,70u,10u,0u,0x3000u,&stale),NV_GSP_QUEUE_TX_OK);
    checkU32("cached tail kept",stale.effectiveReadPtr,10u);
    checkU32("three free",stale.freeBefore,3u);
    checkU32("uses all three",stale.elementCount,3u);
}

static void testCounterWrap(void) {
    std::printf("\n5. 32-bit monotonic TX_HEAD wraps naturally\n");
    NVGspQueueTxPlan p={};
    checkU32("counter wrap verdict",
        nvGspPlanQueueTx(0x40000u,0xfffffffeu,0xfffffffdu,0xfffffffdu,0x3000u,&p),
        NV_GSP_QUEUE_TX_OK);
    checkU32("slot from wrapped counter 0",p.slot[0],2u);
    checkU32("slot from wrapped counter 1",p.slot[1],3u);
    checkU32("slot from wrapped counter 2",p.slot[2],4u);
    checkU32("new head wraps",p.newWritePtr,1u);
    checkU32("published head wraps",p.txHeadValue,1u);
}

static void testMaxAndRefusals(void) {
    std::printf("\n6. max 64 KiB record and refusal family\n");
    NVGspQueueTxPlan p={};
    checkU32("max record",nvGspPlanQueueTx(0x40000u,0u,0u,0u,65536u,&p),NV_GSP_QUEUE_TX_OK);
    checkU32("max elements",p.elementCount,16u);
    checkU32("max copy bytes",p.copyBytes,65536u);
    checkU32("max padding",p.paddingBytes,0u);
    checkU32("last max slot",p.slot[15],15u);
    checkU32("last max offset",p.byteOffset[15],0x10000u);

    NVGspQueueTxPlan bad;
    std::memset(&bad,0x5a,sizeof(bad));
    const NVGspQueueTxPlan before=bad;
    checkU32("zero record refused",nvGspPlanQueueTx(0x40000u,0u,0u,0u,0u,&bad),NV_GSP_QUEUE_TX_BAD_RECORD_SIZE);
    check("zero refusal atomic",std::memcmp(&bad,&before,sizeof(bad))==0);
    checkU32("oversize record refused",nvGspPlanQueueTx(0x40000u,0u,0u,0u,65537u,&bad),NV_GSP_QUEUE_TX_BAD_RECORD_SIZE);
    check("oversize refusal atomic",std::memcmp(&bad,&before,sizeof(bad))==0);
    checkU32("tiny queue refused",nvGspPlanQueueTx(0x1000u,0u,0u,0u,56u,&bad),NV_GSP_QUEUE_TX_BAD_GEOMETRY);
    check("geometry refusal atomic",std::memcmp(&bad,&before,sizeof(bad))==0);
    checkU32("null output refused",nvGspPlanQueueTx(0x40000u,0u,0u,0u,56u,nullptr),NV_GSP_QUEUE_TX_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n7. verdict family\n");
    for (NVGspTxU32 v=0u;v<NV_GSP_QUEUE_TX_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueTxVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueTxSucceeded(v)==(v==NV_GSP_QUEUE_TX_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueTx.hpp -- GSP-RM MSGQ v2 TX planner\n");
    testHeaderGolden();
    testSingleRecord();
    testPhysicalRingWrap();
    testBusyAndResetRecovery();
    testCounterWrap();
    testMaxAndRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
