// Host tests for NVGspQueueInit.hpp. No IOKit, shared-memory or MMIO access.
#include <cstdio>
#include <cstring>
#include "NVGspQueueInit.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspInitU32 got, NVGspInitU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static NVGspQueuePlan normalLayout(void) {
    NVGspQueuePlan q={};
    const NVGspQueueU32 v=nvGspBuildQueuePlan(true,false,0u,&q);
    if (v!=NV_GSP_QUEUE_PLAN_OK) std::printf("  INTERNAL setup failed: %u\n",v);
    return q;
}

static void goldenPeerHeader(NVGspInitU32 out[8], NVGspInitU32 minor=0u) {
    out[0]=(minor<<16)|NV_GSP_MSGQ_VERSION_MAJOR;
    out[1]=0x00040000u;
    out[2]=0x00001000u;
    out[3]=0x0000003fu;
    out[4]=0x00001000u;
    out[5]=0u; out[6]=0u; out[7]=0u;
}

static void testTxInitGolden(void) {
    std::printf("\n1. exact RM command TX initialization\n");
    const NVGspQueuePlan q=normalLayout();
    NVGspQueueTxInitPlan p={};
    checkU32("verdict",nvGspPlanQueueTxInit(&q,0u,&p),NV_GSP_QUEUE_INIT_OK);
    checkU32("queue",p.queueIndex,0u);
    checkU32("shared header offset",p.sharedHeaderOffset,0x00001000u);
    checkU32("shared header bytes",p.sharedHeaderBytes,32u);
    const NVGspInitU32 header[8]={
        0x00000002u,0x00040000u,0x00001000u,0x0000003fu,
        0x00001000u,0u,0u,0u
    };
    for (NVGspInitU32 i=0;i<8u;++i) {
        char label[40]; std::snprintf(label,sizeof(label),"header[%u]",i);
        checkU32(label,p.headerDwords[i],header[i]);
    }
    checkU32("command head",p.commandHeadRegister,0x00110c00u);
    checkU32("command tail",p.commandTailRegister,0x00110c04u);
    checkU32("write ptr zero",p.initialWritePtr,0u);
    checkU32("read ptr zero",p.initialReadPtr,0u);
    checkU32("all 63 slots free",p.initialFree,63u);
    check("initial HEAD write notifies GSP",p.headWriteNotifiesGsp);

    checkU32("step0 kind",p.steps[0].kind,NV_GSP_QUEUE_INIT_STEP_WRITE_SHARED);
    checkU32("step0 offset",p.steps[0].offset,0x1000u);
    checkU32("step0 bytes",p.steps[0].bytes,32u);
    checkU32("step1 store fence",p.steps[1].kind,NV_GSP_QUEUE_INIT_STEP_STORE_FENCE);
    checkU32("step2 register write",p.steps[2].kind,NV_GSP_QUEUE_INIT_STEP_WRITE_REGISTER);
    checkU32("step2 register",p.steps[2].offset,0x00110c00u);
    checkU32("step2 value zero",p.steps[2].value,0u);
    checkU32("step3 full fence",p.steps[3].kind,NV_GSP_QUEUE_INIT_STEP_FULL_FENCE);
}

static void testRxLinkGolden(void) {
    std::printf("\n2. exact RM status RX link sequence\n");
    const NVGspQueuePlan q=normalLayout();
    NVGspInitU32 peer[8]; goldenPeerHeader(peer);
    NVGspQueueRxLinkPlan p={};
    checkU32("verdict",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_OK);
    checkU32("queue",p.queueIndex,0u);
    checkU32("status header offset",p.sharedHeaderOffset,0x00041000u);
    checkU32("header bytes",p.sharedHeaderBytes,32u);
    checkU32("message head",p.messageHeadRegister,0x00110c80u);
    checkU32("message tail",p.messageTailRegister,0x00110c84u);
    checkU32("read ptr zero",p.initialReadPtr,0u);
    checkU32("available zero",p.initialAvailable,0u);
    checkU32("peer count",p.peerMessageCount,63u);
    checkU32("step0 read shared",p.steps[0].kind,NV_GSP_QUEUE_INIT_STEP_READ_SHARED);
    checkU32("step0 offset",p.steps[0].offset,0x00041000u);
    checkU32("step0 bytes",p.steps[0].bytes,32u);
    checkU32("step1 store fence",p.steps[1].kind,NV_GSP_QUEUE_INIT_STEP_STORE_FENCE);
    checkU32("step2 write register",p.steps[2].kind,NV_GSP_QUEUE_INIT_STEP_WRITE_REGISTER);
    checkU32("step2 tail register",p.steps[2].offset,0x00110c84u);
    checkU32("step2 tail zero",p.steps[2].value,0u);
}

static void testMinorCompatibility(void) {
    std::printf("\n3. RX minor version is intentionally compatible\n");
    const NVGspQueuePlan q=normalLayout();
    NVGspInitU32 peer[8]; goldenPeerHeader(peer,0xabcdU);
    NVGspInitU32 count=0u;
    check("header accepted with different minor",nvGspQueueRxHeaderAccepted(&q,peer,&count));
    checkU32("count still 63",count,63u);
    NVGspQueueRxLinkPlan p={};
    checkU32("link accepts different minor",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_OK);
}

static void testRxRefusals(void) {
    std::printf("\n4. malformed peer headers refuse atomically\n");
    const NVGspQueuePlan q=normalLayout();
    NVGspInitU32 peer[8]; goldenPeerHeader(peer);
    NVGspQueueRxLinkPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspQueueRxLinkPlan before=p;

    peer[0]=(peer[0]&0xffff0000u)|3u;
    checkU32("wrong major",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("wrong major atomic",std::memcmp(&p,&before,sizeof(p))==0);
    goldenPeerHeader(peer);

    peer[1]=0x0003f000u;
    checkU32("wrong size",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("wrong size atomic",std::memcmp(&p,&before,sizeof(p))==0);
    goldenPeerHeader(peer);

    peer[2]=0x00002000u;
    checkU32("wrong msg size",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("wrong msg size atomic",std::memcmp(&p,&before,sizeof(p))==0);
    goldenPeerHeader(peer);

    peer[3]=62u;
    checkU32("wrong count",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("wrong count atomic",std::memcmp(&p,&before,sizeof(p))==0);
    goldenPeerHeader(peer);

    peer[4]=0x00040000u;
    checkU32("entry offset leaves no message",nvGspPlanQueueRxLink(&q,0u,peer,&p),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("entry offset atomic",std::memcmp(&p,&before,sizeof(p))==0);
}

static void testPlanRefusals(void) {
    std::printf("\n5. layout/index/output refusals are atomic\n");
    NVGspQueuePlan q=normalLayout();
    NVGspQueueTxInitPlan tx;
    std::memset(&tx,0x5a,sizeof(tx));
    const NVGspQueueTxInitPlan txBefore=tx;
    checkU32("bad index tx",nvGspPlanQueueTxInit(&q,8u,&tx),NV_GSP_QUEUE_INIT_BAD_REGISTERS);
    check("bad index tx atomic",std::memcmp(&tx,&txBefore,sizeof(tx))==0);
    checkU32("null tx output",nvGspPlanQueueTxInit(&q,0u,nullptr),NV_GSP_QUEUE_INIT_NULL_OUTPUT);

    q.sharedBytes=q.commandQueueOffset;
    checkU32("short shared TX layout",nvGspPlanQueueTxInit(&q,0u,&tx),NV_GSP_QUEUE_INIT_BAD_LAYOUT);
    check("bad tx layout atomic",std::memcmp(&tx,&txBefore,sizeof(tx))==0);

    q=normalLayout();
    NVGspInitU32 peer[8]; goldenPeerHeader(peer);
    NVGspQueueRxLinkPlan rx;
    std::memset(&rx,0x3c,sizeof(rx));
    const NVGspQueueRxLinkPlan rxBefore=rx;
    checkU32("bad index rx",nvGspPlanQueueRxLink(&q,8u,peer,&rx),NV_GSP_QUEUE_INIT_BAD_REGISTERS);
    check("bad index rx atomic",std::memcmp(&rx,&rxBefore,sizeof(rx))==0);
    checkU32("null rx output",nvGspPlanQueueRxLink(&q,0u,peer,nullptr),NV_GSP_QUEUE_INIT_NULL_OUTPUT);
    checkU32("null peer header",nvGspPlanQueueRxLink(&q,0u,nullptr,&rx),NV_GSP_QUEUE_INIT_BAD_RX_HEADER);
    check("null peer atomic",std::memcmp(&rx,&rxBefore,sizeof(rx))==0);
}

static void testStepAndVerdictFamilies(void) {
    std::printf("\n6. step and verdict families\n");
    for (NVGspInitU32 k=1u;k<NV_GSP_QUEUE_INIT_STEP_LIMIT;++k)
        check("step known",nvGspQueueInitStepKnown(k));
    check("zero step not action",!nvGspQueueInitStepKnown(0u));
    check("limit step not action",!nvGspQueueInitStepKnown(NV_GSP_QUEUE_INIT_STEP_LIMIT));
    for (NVGspInitU32 v=0u;v<NV_GSP_QUEUE_INIT_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueInitVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueInitSucceeded(v)==(v==NV_GSP_QUEUE_INIT_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueInit.hpp -- GSP-RM MSGQ v2 init/link contract\n");
    testTxInitGolden();
    testRxLinkGolden();
    testMinorCompatibility();
    testRxRefusals();
    testPlanRefusals();
    testStepAndVerdictFamilies();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
