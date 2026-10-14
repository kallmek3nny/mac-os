// Host tests for NVGspRpcResponse.hpp. No IOKit, shared memory or MMIO required.
#include <cstdio>
#include <cstring>
#include "NVGspRpcResponse.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRespU32 got, NVGspRespU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static NVGspRpcFramePlan frame(NVGspRespU32 function, NVGspRespU32 sequence, NVGspRespU32 params=0u) {
    NVGspRpcFramePlan f={};
    checkU32("build frame",nvGspBuildRmRpcFrameNoCc(function,sequence,params,&f),NV_GSP_RPC_FRAME_OK);
    return f;
}

static void testMatchedSuccess(void) {
    std::printf("\n1. exact function+sequence response succeeds\n");
    NVGspRpcFramePlan f=frame(0x1234u,7u);
    f.prefix[10]=0u;
    f.prefix[11]=0xfeedbeefu;
    NVGspRpcResponsePlan r={};
    checkU32("success verdict",nvGspValidateRpcResponseNoCc(0x1234u,7u,f.prefix,f.queueMessageBytes,&r),
             NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS);
    check("structural",r.structurallyValid);
    check("matched",r.matchesExpected);
    check("succeeded",r.rpcSucceeded);
    check("not event",!r.asyncEvent);
    check("full fence before receive",r.fullFenceBeforeReceive);
    checkU32("function",r.function,0x1234u);
    checkU32("sequence",r.sequence,7u);
    checkU32("result",r.rpcResult,0u);
    checkU32("private result preserved",r.rpcResultPrivate,0xfeedbeefu);
}

static void testEventClassification(void) {
    std::printf("\n2. wrong function or sequence is async traffic, not a failed response\n");
    NVGspRpcFramePlan f=frame(0x200u,9u);
    f.prefix[10]=0u;
    NVGspRpcResponsePlan byFunction={};
    checkU32("wrong function",nvGspValidateRpcResponseNoCc(0x201u,9u,f.prefix,f.queueMessageBytes,&byFunction),
             NV_GSP_RPC_RESPONSE_ASYNC_EVENT);
    check("function event structural",byFunction.structurallyValid);
    check("function event",byFunction.asyncEvent);
    check("function no match",!byFunction.matchesExpected);
    check("function no success",!byFunction.rpcSucceeded);

    NVGspRpcResponsePlan bySequence={};
    checkU32("wrong sequence",nvGspValidateRpcResponseNoCc(0x200u,10u,f.prefix,f.queueMessageBytes,&bySequence),
             NV_GSP_RPC_RESPONSE_ASYNC_EVENT);
    check("sequence event",bySequence.asyncEvent);
    check("sequence no match",!bySequence.matchesExpected);
}

static void testMatchedRemoteError(void) {
    std::printf("\n3. matched nonzero rpc_result stays matched but unsuccessful\n");
    NVGspRpcFramePlan f=frame(0x777u,0xffffffffu);
    f.prefix[10]=0x00000005u;
    f.prefix[11]=0xabcdef01u;
    NVGspRpcResponsePlan r={};
    checkU32("error verdict",nvGspValidateRpcResponseNoCc(0x777u,0xffffffffu,f.prefix,f.queueMessageBytes,&r),
             NV_GSP_RPC_RESPONSE_MATCHED_ERROR);
    check("matched",r.matchesExpected);
    check("not succeeded",!r.rpcSucceeded);
    check("not event",!r.asyncEvent);
    checkU32("remote error",r.rpcResult,5u);
    checkU32("private diagnostics",r.rpcResultPrivate,0xabcdef01u);
}

static void expectMalformed(const char *label, NVGspRpcFramePlan f, NVGspRespU32 recordBytes,
                            NVGspRespU32 want) {
    NVGspRpcResponsePlan r;
    std::memset(&r,0xa5,sizeof(r));
    const NVGspRpcResponsePlan before=r;
    checkU32(label,nvGspValidateRpcResponseNoCc(f.prefix[9],f.prefix[12],f.prefix,recordBytes,&r),want);
    check("malformed atomic",std::memcmp(&r,&before,sizeof(r))==0);
}

static void testStructuralRefusals(void) {
    std::printf("\n4. malformed framing is refused atomically\n");
    NVGspRpcFramePlan base=frame(1u,2u);
    base.prefix[10]=0u;

    NVGspRpcFramePlan f=base; f.prefix[0]^=1u;
    expectMalformed("bad magic",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_MCTP);
    f=base; f.prefix[2]^=1u;
    expectMalformed("bad mctp header",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_MCTP);
    f=base; f.prefix[3]^=1u;
    expectMalformed("bad nvdm",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_NVDM);
    f=base; f.prefix[5]=1u;
    expectMalformed("bad reserved",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_NVDM);
    f=base; f.prefix[1]++;
    expectMalformed("bad outer length",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_LENGTH);
    f=base; f.prefix[4]++;
    expectMalformed("bad nvdm length",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_LENGTH);
    f=base; f.prefix[8]++;
    expectMalformed("bad rpc length",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_LENGTH);
    f=base; f.prefix[6]^=0x01000000u;
    expectMalformed("bad version",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_VERSION);
    f=base; f.prefix[7]^=1u;
    expectMalformed("bad signature",f,f.queueMessageBytes,NV_GSP_RPC_RESPONSE_BAD_SIGNATURE);
    expectMalformed("short record",base,NV_GSP_RPC_PREFIX_BYTES-1u,NV_GSP_RPC_RESPONSE_TOO_SHORT);
    expectMalformed("oversize record",base,NV_GSP_QUEUE_ELEMENT_MAX_BYTES+1u,NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE);
}

static NVGspRpcTransportPlan transportPlan(void) {
    NVGspQueuePlan layout={};
    checkU32("layout",nvGspBuildQueuePlan(true,false,0u,&layout),NV_GSP_QUEUE_PLAN_OK);
    NVGspRpcTransportPlan p={};
    checkU32("transport",nvGspPlanRpcTransport(&layout,0x4444u,0x55u,0u,
                                               0u,0u,0u,0u,0u,1u,56u,&p),
             NV_GSP_RPC_TXN_OK);
    return p;
}

static void testTransactionValidation(void) {
    std::printf("\n5. transaction layer distinguishes matched success/error/event\n");
    const NVGspRpcTransportPlan t=transportPlan();
    NVGspRpcFramePlan f=frame(t.function,t.sequence);
    f.prefix[10]=0u;

    NVGspRpcValidatedTransaction v={};
    checkU32("validated success",nvGspValidateRpcTransactionResponse(&t,f.prefix,56u,&v),
             NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS);
    check("semantic validated",v.responseSemanticallyValidated);
    check("transaction success",v.responseSucceeded);
    check("transport kept transport-only",!v.transport.responseSemanticallyValidated);

    f.prefix[10]=9u;
    NVGspRpcValidatedTransaction err={};
    checkU32("validated error",nvGspValidateRpcTransactionResponse(&t,f.prefix,56u,&err),
             NV_GSP_RPC_RESPONSE_MATCHED_ERROR);
    check("error still semantic",err.responseSemanticallyValidated);
    check("error not success",!err.responseSucceeded);

    f.prefix[10]=0u;
    f.prefix[12]++;
    NVGspRpcValidatedTransaction event={};
    checkU32("validated event",nvGspValidateRpcTransactionResponse(&t,f.prefix,56u,&event),
             NV_GSP_RPC_RESPONSE_ASYNC_EVENT);
    check("event not semantic response",!event.responseSemanticallyValidated);
    check("event not success",!event.responseSucceeded);

    NVGspRpcValidatedTransaction untouched;
    std::memset(&untouched,0x3c,sizeof(untouched));
    const NVGspRpcValidatedTransaction before=untouched;
    f=frame(t.function,t.sequence);
    f.prefix[10]=0u;
    checkU32("record-size mismatch",nvGspValidateRpcTransactionResponse(&t,f.prefix,60u,&untouched),
             NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE);
    check("mismatch atomic",std::memcmp(&untouched,&before,sizeof(untouched))==0);
}

static void testNullsAndVerdicts(void) {
    std::printf("\n6. nulls and verdict family\n");
    NVGspRpcFramePlan f=frame(1u,1u);
    NVGspRpcResponsePlan r={};
    checkU32("null prefix",nvGspValidateRpcResponseNoCc(1u,1u,nullptr,56u,&r),NV_GSP_RPC_RESPONSE_NULL_INPUT);
    checkU32("null output",nvGspValidateRpcResponseNoCc(1u,1u,f.prefix,56u,nullptr),NV_GSP_RPC_RESPONSE_NULL_OUTPUT);
    for (NVGspRespU32 x=0u;x<NV_GSP_RPC_RESPONSE_LIMIT;++x)
        check("verdict named",std::strcmp(nvGspRpcResponseVerdictName(x),"unknown")!=0);
}

int main(void) {
    std::printf("NVGspRpcResponse.hpp -- GSP-RM response semantics\n");
    testMatchedSuccess();
    testEventClassification();
    testMatchedRemoteError();
    testStructuralRefusals();
    testTransactionValidation();
    testNullsAndVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
