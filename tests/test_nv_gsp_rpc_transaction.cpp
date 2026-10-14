// Host tests for NVGspRpcTransaction.hpp. No IOKit, shared memory or MMIO required.
#include <cstdio>
#include <cstring>
#include "NVGspRpcTransaction.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspTxnU32 got, NVGspTxnU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static NVGspQueuePlan normalLayout(void) {
    NVGspQueuePlan p={};
    checkU32("layout",nvGspBuildQueuePlan(true,false,0u,&p),NV_GSP_QUEUE_PLAN_OK);
    return p;
}

static void testOneSlotRoundTrip(void) {
    std::printf("\n1. one-slot request and one-slot response transport rehearsal\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspRpcTransportPlan p={};
    checkU32("txn verdict",
        nvGspPlanRpcTransport(&layout,0x1234u,7u,0u,
                              0u,0u,0u,
                              0u,0u,1u,
                              56u,&p),NV_GSP_RPC_TXN_OK);
    checkU32("function",p.function,0x1234u);
    checkU32("sequence",p.sequence,7u);
    checkU32("request bytes",p.requestRecordBytes,56u);
    checkU32("request elements",p.requestTx.elementCount,1u);
    checkU32("request slot",p.requestTx.slot[0],0u);
    checkU32("request head",p.requestPublish.commandHeadValue,1u);
    checkU32("request HEAD register",p.requestPublish.commandHeadRegister,0x00110c00u);
    check("request store fence",p.requestPublish.storeFenceBeforeHeadWrite);
    check("request notifies GSP",p.requestPublish.commandHeadWriteNotifiesGsp);
    checkU32("response elements",p.responseElementCount,1u);
    checkU32("response slot",p.responseElement[0].slot,0u);
    checkU32("response shared offset",p.responseElement[0].sharedByteOffset,0x42000u);
    checkU32("response tail",p.responseConsume.messageTailValue,1u);
    checkU32("response TAIL register",p.responseConsume.messageTailRegister,0x00110c84u);
    check("response load fence",p.responseAvailable.loadFenceAfterHeadRead);
    check("response store fence",p.responseConsume.storeFenceBeforeTailWrite);
    check("semantic validation remains false",!p.responseSemanticallyValidated);
}

static void testWrappedMultiSlotRoundTrip(void) {
    std::printf("\n2. request and response both wrap slot 62 to slot 0\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspRpcTransportPlan p={};
    checkU32("wrapped txn",
        nvGspPlanRpcTransport(&layout,0x55aau,0xffffffffu,5000u,
                              62u,61u,61u,
                              62u,64u,64u,
                              8192u,&p),NV_GSP_RPC_TXN_OK);
    checkU32("request record bytes",p.requestRecordBytes,5056u);
    checkU32("request two slots",p.requestTx.elementCount,2u);
    checkU32("request slot 62",p.requestTx.slot[0],62u);
    checkU32("request slot 0",p.requestTx.slot[1],0u);
    checkU32("request monotonic head",p.requestPublish.commandHeadValue,64u);
    checkU32("response two slots",p.responseElementCount,2u);
    checkU32("response slot 62",p.responseElement[0].slot,62u);
    checkU32("response slot 0",p.responseElement[1].slot,0u);
    checkU32("response shared 62",p.responseElement[0].sharedByteOffset,0x80000u);
    checkU32("response shared 0",p.responseElement[1].sharedByteOffset,0x42000u);
    checkU32("response monotonic tail",p.responseConsume.messageTailValue,64u);
}

static void testResponseCounterWrap(void) {
    std::printf("\n3. response consume crosses NvU32 rollover\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspRpcTransportPlan p={};
    checkU32("counter wrap txn",
        nvGspPlanRpcTransport(&layout,1u,2u,0u,
                              0u,0u,0u,
                              0xfffffffeu,1u,1u,
                              8192u,&p),NV_GSP_RPC_TXN_OK);
    checkU32("response slot 2",p.responseElement[0].slot,2u);
    checkU32("response slot 3",p.responseElement[1].slot,3u);
    checkU32("tail wraps",p.responseConsume.messageTailValue,0u);
    checkU32("one response slot remains",p.responseConsume.remainingAvailable,1u);
}

static void testRefusalPropagation(void) {
    std::printf("\n4. queue failures propagate without partial output\n");
    const NVGspQueuePlan layout=normalLayout();
    NVGspRpcTransportPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspRpcTransportPlan before=p;

    checkU32("TX full",
        nvGspPlanRpcTransport(&layout,1u,1u,0u,
                              63u,0u,0u,
                              0u,0u,1u,
                              56u,&p),NV_GSP_RPC_TXN_TX_FAILED);
    check("TX failure atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("short response",
        nvGspPlanRpcTransport(&layout,1u,1u,0u,
                              0u,0u,0u,
                              0u,0u,1u,
                              8192u,&p),NV_GSP_RPC_TXN_RESPONSE_SHORT);
    check("short response atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("zero response size",
        nvGspPlanRpcTransport(&layout,1u,1u,0u,
                              0u,0u,0u,
                              0u,0u,1u,
                              0u,&p),NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE);
    check("zero response atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("oversized response",
        nvGspPlanRpcTransport(&layout,1u,1u,0u,
                              0u,0u,0u,
                              0u,0u,16u,
                              65537u,&p),NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE);
    check("oversize atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("oversized request parameters",
        nvGspPlanRpcTransport(&layout,1u,1u,NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC+1u,
                              0u,0u,0u,
                              0u,0u,1u,
                              56u,&p),NV_GSP_RPC_TXN_FRAME_FAILED);
    check("frame failure atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("null layout",
        nvGspPlanRpcTransport(nullptr,1u,1u,0u,0u,0u,0u,0u,0u,1u,56u,&p),
        NV_GSP_RPC_TXN_NULL_INPUT);
    checkU32("null output",
        nvGspPlanRpcTransport(&layout,1u,1u,0u,0u,0u,0u,0u,0u,1u,56u,nullptr),
        NV_GSP_RPC_TXN_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspTxnU32 v=0u;v<NV_GSP_RPC_TXN_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspRpcTxnVerdictName(v),"unknown")!=0);
        check("success equality",nvGspRpcTxnSucceeded(v)==(v==NV_GSP_RPC_TXN_OK));
    }
}

int main(void) {
    std::printf("NVGspRpcTransaction.hpp -- end-to-end GSP-RM transport rehearsal\n");
    testOneSlotRoundTrip();
    testWrappedMultiSlotRoundTrip();
    testResponseCounterWrap();
    testRefusalPropagation();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
