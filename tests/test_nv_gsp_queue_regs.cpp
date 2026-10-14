// Host tests for NVGspQueueRegs.hpp. No IOKit, shared memory or MMIO required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueRegs.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRegU32 got, NVGspRegU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static void testConstants(void) {
    std::printf("\n1. published PGSP queue register constants\n");
    checkU32("queue count",NV_GSP_QUEUE_REGISTER_COUNT,8u);
    checkU32("RM queue index",NV_GSP_RM_QUEUE_INDEX,0u);
    checkU32("stride",NV_GSP_QUEUE_REGISTER_STRIDE,8u);
    checkU32("queue head base",NV_GSP_QUEUE_HEAD_BASE,0x00110c00u);
    checkU32("queue tail base",NV_GSP_QUEUE_TAIL_BASE,0x00110c04u);
    checkU32("msgq head base",NV_GSP_MSGQ_HEAD_BASE,0x00110c80u);
    checkU32("msgq tail base",NV_GSP_MSGQ_TAIL_BASE,0x00110c84u);
}

static void testRmQueueGolden(void) {
    std::printf("\n2. exact RM queue-0 register quartet\n");
    NVGspQueueRegisters r={};
    checkU32("verdict",nvGspPlanQueueRegisters(NV_GSP_RM_QUEUE_INDEX,&r),NV_GSP_QUEUE_REGS_OK);
    checkU32("index",r.queueIndex,0u);
    checkU32("command head",r.commandHead,0x00110c00u);
    checkU32("command tail",r.commandTail,0x00110c04u);
    checkU32("message head",r.messageHead,0x00110c80u);
    checkU32("message tail",r.messageTail,0x00110c84u);
}

static void testLastQueueGolden(void) {
    std::printf("\n3. queue-7 boundary and stride\n");
    NVGspQueueRegisters r={};
    checkU32("verdict",nvGspPlanQueueRegisters(7u,&r),NV_GSP_QUEUE_REGS_OK);
    checkU32("command head",r.commandHead,0x00110c38u);
    checkU32("command tail",r.commandTail,0x00110c3cu);
    checkU32("message head",r.messageHead,0x00110cb8u);
    checkU32("message tail",r.messageTail,0x00110cbcu);
}

static void testRegisterRefusals(void) {
    std::printf("\n4. register refusals are atomic\n");
    NVGspQueueRegisters r;
    std::memset(&r,0xa5,sizeof(r));
    const NVGspQueueRegisters before=r;
    checkU32("index 8 refused",nvGspPlanQueueRegisters(8u,&r),NV_GSP_QUEUE_REGS_BAD_INDEX);
    check("bad index leaves output",std::memcmp(&r,&before,sizeof(r))==0);
    checkU32("null output",nvGspPlanQueueRegisters(0u,nullptr),NV_GSP_QUEUE_REGS_NULL_OUTPUT);
}

static void testPublishGolden(void) {
    std::printf("\n5. copy -> store fence -> QUEUE_HEAD publish contract\n");
    NVGspQueueTxPlan tx={};
    checkU32("tx plan",nvGspPlanQueueTx(0x40000u,0u,0u,0u,56u,&tx),NV_GSP_QUEUE_TX_OK);

    NVGspQueuePublishPlan p={};
    checkU32("publish plan",nvGspPlanQueuePublish(0u,&tx,&p),NV_GSP_QUEUE_PUBLISH_OK);
    checkU32("queue index",p.queueIndex,0u);
    checkU32("elements",p.elementCount,1u);
    checkU32("full slot copy bytes",p.copyBytes,4096u);
    checkU32("head register",p.commandHeadRegister,0x00110c00u);
    checkU32("tail register for read observation",p.commandTailRegister,0x00110c04u);
    checkU32("head value",p.commandHeadValue,1u);
    check("store fence required",p.storeFenceBeforeHeadWrite);
    check("head write is notify edge",p.commandHeadWriteNotifiesGsp);
}

static void testWrappedPublish(void) {
    std::printf("\n6. multi-slot physical wrap still publishes monotonic head\n");
    NVGspQueueTxPlan tx={};
    checkU32("tx wrap",nvGspPlanQueueTx(0x40000u,62u,61u,61u,8192u,&tx),NV_GSP_QUEUE_TX_OK);
    checkU32("slot 62",tx.slot[0],62u);
    checkU32("slot wraps zero",tx.slot[1],0u);

    NVGspQueuePublishPlan p={};
    checkU32("publish wrap",nvGspPlanQueuePublish(0u,&tx,&p),NV_GSP_QUEUE_PUBLISH_OK);
    checkU32("two copies",p.elementCount,2u);
    checkU32("8K copied",p.copyBytes,8192u);
    checkU32("monotonic head is 64",p.commandHeadValue,64u);
    checkU32("same queue-0 head register",p.commandHeadRegister,0x00110c00u);
}

static void testPublishRefusals(void) {
    std::printf("\n7. publish validation and atomic refusal\n");
    NVGspQueueTxPlan good={};
    checkU32("build good tx",nvGspPlanQueueTx(0x40000u,0u,0u,0u,56u,&good),NV_GSP_QUEUE_TX_OK);

    NVGspQueuePublishPlan p;
    std::memset(&p,0x5a,sizeof(p));
    const NVGspQueuePublishPlan before=p;
    checkU32("bad queue",nvGspPlanQueuePublish(8u,&good,&p),NV_GSP_QUEUE_PUBLISH_BAD_INDEX);
    check("bad queue atomic",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null tx",nvGspPlanQueuePublish(0u,nullptr,&p),NV_GSP_QUEUE_PUBLISH_NULL_INPUT);
    check("null tx atomic",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null output",nvGspPlanQueuePublish(0u,&good,nullptr),NV_GSP_QUEUE_PUBLISH_NULL_OUTPUT);

    NVGspQueueTxPlan bad=good;
    bad.copyBytes-=4u;
    checkU32("bad copy geometry",nvGspPlanQueuePublish(0u,&bad,&p),NV_GSP_QUEUE_PUBLISH_BAD_TX_PLAN);
    check("bad copy atomic",std::memcmp(&p,&before,sizeof(p))==0);

    bad=good;
    bad.txHeadValue+=1u;
    checkU32("head mismatch",nvGspPlanQueuePublish(0u,&bad,&p),NV_GSP_QUEUE_PUBLISH_BAD_TX_PLAN);
    check("head mismatch atomic",std::memcmp(&p,&before,sizeof(p))==0);
}

static void testVerdicts(void) {
    std::printf("\n8. verdict families\n");
    for (NVGspRegU32 v=0u;v<NV_GSP_QUEUE_REGS_LIMIT;++v) {
        check("regs verdict named",std::strcmp(nvGspQueueRegsVerdictName(v),"unknown")!=0);
        check("regs success equality",nvGspQueueRegsSucceeded(v)==(v==NV_GSP_QUEUE_REGS_OK));
    }
    for (NVGspRegU32 v=0u;v<NV_GSP_QUEUE_PUBLISH_LIMIT;++v) {
        check("publish verdict named",std::strcmp(nvGspQueuePublishVerdictName(v),"unknown")!=0);
        check("publish success equality",nvGspQueuePublishSucceeded(v)==(v==NV_GSP_QUEUE_PUBLISH_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueRegs.hpp -- GA107 PGSP queue register/publish contract\n");
    testConstants();
    testRmQueueGolden();
    testLastQueueGolden();
    testRegisterRefusals();
    testPublishGolden();
    testWrappedPublish();
    testPublishRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
