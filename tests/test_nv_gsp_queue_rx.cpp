// Host tests for NVGspQueueRx.hpp. No IOKit, shared memory or MMIO required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueRx.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRxU32 got, NVGspRxU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static NVGspQueuePlan normalLayout(void) {
    NVGspQueuePlan p={};
    checkU32("layout",nvGspBuildQueuePlan(true,false,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    return p;
}

static void testNormalAvailable(void) {
    std::printf("\n1. normal RX availability and register contract\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspQueueRxAvailablePlan p={};
    checkU32("available verdict",nvGspPlanQueueRxAvailable(&layout,0u,0u,0u,3u,&p),NV_GSP_QUEUE_RX_OK);
    checkU32("read",p.readPtr,0u);
    checkU32("effective writer",p.effectiveWritePtr,3u);
    checkU32("available",p.available,3u);
    checkU32("count",p.messageCount,63u);
    checkU32("size",p.messageSize,4096u);
    checkU32("entry offset",p.entryOffset,4096u);
    checkU32("status offset",p.statusQueueOffset,0x41000u);
    checkU32("MSGQ HEAD",p.messageHeadRegister,0x00110c80u);
    checkU32("MSGQ TAIL",p.messageTailRegister,0x00110c84u);
    check("load fence",p.loadFenceAfterHeadRead);
}

static void testSlotWrap(void) {
    std::printf("\n2. physical RX slots wrap independently from monotonic counters\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspQueueRxAvailablePlan a={};
    checkU32("avail verdict",nvGspPlanQueueRxAvailable(&layout,0u,62u,64u,64u,&a),NV_GSP_QUEUE_RX_OK);
    checkU32("two available",a.available,2u);

    NVGspQueueRxElementPlan e0={},e1={};
    checkU32("element 0",nvGspPlanQueueRxElement(&a,0u,&e0),NV_GSP_QUEUE_RX_OK);
    checkU32("element 1",nvGspPlanQueueRxElement(&a,1u,&e1),NV_GSP_QUEUE_RX_OK);
    checkU32("slot 62",e0.slot,62u);
    checkU32("slot 0",e1.slot,0u);
    checkU32("queue offset 62",e0.queueByteOffset,0x3f000u);
    checkU32("queue offset 0",e1.queueByteOffset,0x1000u);
    checkU32("shared offset 62",e0.sharedByteOffset,0x80000u);
    checkU32("shared offset 0",e1.sharedByteOffset,0x42000u);
    checkU32("full element",e0.bytes,4096u);
}

static void testResetRecovery(void) {
    std::printf("\n3. NVIDIA RX reset recovery\n");
    const NVGspQueuePlan layout=normalLayout();

    NVGspQueueRxAvailablePlan fullReset={};
    checkU32("full reset verdict",
        nvGspPlanQueueRxAvailable(&layout,0u,10u,73u,0u,&fullReset),NV_GSP_QUEUE_RX_OK);
    checkU32("full reset becomes empty",fullReset.effectiveWritePtr,10u);
    checkU32("zero available",fullReset.available,0u);

    NVGspQueueRxAvailablePlan stale={};
    checkU32("impossible head falls back",
        nvGspPlanQueueRxAvailable(&layout,0u,10u,12u,100u,&stale),NV_GSP_QUEUE_RX_OK);
    checkU32("cached writer retained",stale.effectiveWritePtr,12u);
    checkU32("two available",stale.available,2u);
}

static void testCounterWrapAndConsume(void) {
    std::printf("\n4. 32-bit counter wrap and TAIL publication\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspQueueRxAvailablePlan a={};
    checkU32("wrap avail",
        nvGspPlanQueueRxAvailable(&layout,0u,0xfffffffeu,1u,1u,&a),NV_GSP_QUEUE_RX_OK);
    checkU32("three available",a.available,3u);

    NVGspQueueRxElementPlan e0={},e1={},e2={};
    checkU32("wrap e0",nvGspPlanQueueRxElement(&a,0u,&e0),NV_GSP_QUEUE_RX_OK);
    checkU32("wrap e1",nvGspPlanQueueRxElement(&a,1u,&e1),NV_GSP_QUEUE_RX_OK);
    checkU32("wrap e2",nvGspPlanQueueRxElement(&a,2u,&e2),NV_GSP_QUEUE_RX_OK);
    checkU32("slot 2",e0.slot,2u);
    checkU32("slot 3",e1.slot,3u);
    checkU32("counter wrapped slot 0",e2.slot,0u);

    NVGspQueueRxConsumePlan c={};
    checkU32("consume two",nvGspPlanQueueRxConsume(&a,2u,&c),NV_GSP_QUEUE_RX_OK);
    checkU32("old read",c.oldReadPtr,0xfffffffeu);
    checkU32("new read wraps",c.newReadPtr,0u);
    checkU32("one remains",c.remainingAvailable,1u);
    checkU32("tail register",c.messageTailRegister,0x00110c84u);
    checkU32("tail value",c.messageTailValue,0u);
    check("store fence",c.storeFenceBeforeTailWrite);
}

static void testRefusalsAreAtomic(void) {
    std::printf("\n5. refusal family and atomic outputs\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspQueueRxAvailablePlan a;
    std::memset(&a,0xa5,sizeof(a));
    const NVGspQueueRxAvailablePlan beforeA=a;
    checkU32("bad queue",nvGspPlanQueueRxAvailable(&layout,8u,0u,0u,0u,&a),NV_GSP_QUEUE_RX_BAD_INDEX);
    check("bad queue atomic",std::memcmp(&a,&beforeA,sizeof(a))==0);

    NVGspQueueRxAvailablePlan one={};
    checkU32("one avail",nvGspPlanQueueRxAvailable(&layout,0u,0u,0u,1u,&one),NV_GSP_QUEUE_RX_OK);

    NVGspQueueRxElementPlan e;
    std::memset(&e,0x5a,sizeof(e));
    const NVGspQueueRxElementPlan beforeE=e;
    checkU32("missing element",nvGspPlanQueueRxElement(&one,1u,&e),NV_GSP_QUEUE_RX_EMPTY);
    check("missing element atomic",std::memcmp(&e,&beforeE,sizeof(e))==0);

    NVGspQueueRxConsumePlan c;
    std::memset(&c,0x3c,sizeof(c));
    const NVGspQueueRxConsumePlan beforeC=c;
    checkU32("zero consume",nvGspPlanQueueRxConsume(&one,0u,&c),NV_GSP_QUEUE_RX_BAD_CONSUME);
    check("zero consume atomic",std::memcmp(&c,&beforeC,sizeof(c))==0);
    checkU32("too much consume",nvGspPlanQueueRxConsume(&one,2u,&c),NV_GSP_QUEUE_RX_BAD_CONSUME);
    check("large consume atomic",std::memcmp(&c,&beforeC,sizeof(c))==0);
    checkU32("null availability",nvGspPlanQueueRxConsume(nullptr,1u,&c),NV_GSP_QUEUE_RX_NULL_INPUT);
    checkU32("null output",nvGspPlanQueueRxConsume(&one,1u,nullptr),NV_GSP_QUEUE_RX_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n6. verdict family\n");
    for (NVGspRxU32 v=0u;v<NV_GSP_QUEUE_RX_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueRxVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueRxSucceeded(v)==(v==NV_GSP_QUEUE_RX_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueRx.hpp -- GSP-RM MSGQ v2 RX planner\n");
    testNormalAvailable();
    testSlotWrap();
    testResetRecovery();
    testCounterWrapAndConsume();
    testRefusalsAreAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
