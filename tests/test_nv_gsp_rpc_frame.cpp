// Host tests for NVGspRpcFrame.hpp. No IOKit, GSP, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVGspRpcFrame.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRpcU32 got, NVGspRpcU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static void testConstants(void) {
    std::printf("\n1. MCTP/NVDM/RPC constants\n");
    checkU32("MCTP magic",NV_GSP_RPC_MCTP_MAGIC,0x4d435450u);
    checkU32("single MCTP header",NV_GSP_RPC_MCTP_SINGLE_HEADER,0xc0000001u);
    checkU32("RM RPC NVDM type",NV_GSP_RPC_NVDM_TYPE_RM_RPC,0x25u);
    checkU32("NVIDIA NVDM header",NV_GSP_RPC_NVDM_VENDOR_HEADER_RM_RPC,0x2510de7eu);
    checkU32("RPC version 3.0",NV_GSP_RPC_HEADER_VERSION,0x03000000u);
    checkU32("RPC signature",NV_GSP_RPC_SIGNATURE_VALID,0x43505256u);
    checkU32("pending result",NV_GSP_RPC_RESULT_PENDING,0xffffffffu);
    checkU32("RPC header bytes",NV_GSP_RPC_HEADER_BYTES,32u);
    checkU32("outer no-CC bytes",NV_GSP_RPC_OUTER_NO_CC_BYTES,24u);
    checkU32("prefix bytes",NV_GSP_RPC_PREFIX_BYTES,56u);
    checkU32("max non-CC params",NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC,65480u);
}

static void testZeroParamGolden(void) {
    std::printf("\n2. exact zero-param RM-RPC frame prefix\n");
    NVGspRpcFramePlan p={};
    const NVGspRpcU32 function=0x12345678u;
    const NVGspRpcU32 sequence=0x9abcdef0u;
    checkU32("plan verdict",
        nvGspBuildRmRpcFrameNoCc(function,sequence,0u,&p),NV_GSP_RPC_FRAME_OK);
    checkU32("prefix bytes",p.prefixBytes,56u);
    checkU32("parameter bytes",p.parameterBytes,0u);
    checkU32("RPC bytes",p.rpcBytes,32u);
    checkU32("queue message bytes",p.queueMessageBytes,56u);

    const NVGspRpcU32 expected[NV_GSP_RPC_PREFIX_DWORDS]={
        0x4d435450u,
        0x00000038u,
        0xc0000001u,
        0x2510de7eu,
        0x00000020u,
        0x00000000u,
        0x03000000u,
        0x43505256u,
        0x00000020u,
        0x12345678u,
        0xffffffffu,
        0xffffffffu,
        0x9abcdef0u,
        0x00000000u
    };
    for (NVGspRpcU32 i=0;i<NV_GSP_RPC_PREFIX_DWORDS;++i) {
        char label[48]; std::snprintf(label,sizeof(label),"prefix[%u]",i);
        checkU32(label,p.prefix[i],expected[i]);
    }
}

static void testNestedLengths(void) {
    std::printf("\n3. nested length fields remain exact\n");
    NVGspRpcFramePlan p={};
    checkU32("12-byte params",
        nvGspBuildRmRpcFrameNoCc(7u,0u,12u,&p),NV_GSP_RPC_FRAME_OK);
    checkU32("outer full message",p.prefix[1],68u);
    checkU32("NVDM payload",p.prefix[4],44u);
    checkU32("RPC length",p.prefix[8],44u);
    checkU32("function preserved",p.prefix[9],7u);
    checkU32("sequence zero valid",p.prefix[12],0u);
    checkU32("plan RPC bytes",p.rpcBytes,44u);
    checkU32("plan queue bytes",p.queueMessageBytes,68u);

    checkU32("max params accepted",
        nvGspBuildRmRpcFrameNoCc(0xffffffffu,0xffffffffu,
                                 NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC,&p),
        NV_GSP_RPC_FRAME_OK);
    checkU32("max outer is 64K",p.queueMessageBytes,65536u);
    checkU32("max RPC bytes",p.rpcBytes,65512u);
    checkU32("max outer field",p.prefix[1],65536u);
    checkU32("max NVDM payload",p.prefix[4],65512u);
    checkU32("max RPC length",p.prefix[8],65512u);
    checkU32("max function passthrough",p.prefix[9],0xffffffffu);
    checkU32("max sequence passthrough",p.prefix[12],0xffffffffu);
}

static void testAtomicRefusal(void) {
    std::printf("\n4. oversized/null requests are atomic refusals\n");
    NVGspRpcFramePlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspRpcFramePlan before=p;
    checkU32("one byte over max refused",
        nvGspBuildRmRpcFrameNoCc(1u,2u,NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC+1u,&p),
        NV_GSP_RPC_FRAME_PARAMS_TOO_LARGE);
    check("oversize leaves output",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null output refused",
        nvGspBuildRmRpcFrameNoCc(1u,2u,0u,nullptr),NV_GSP_RPC_FRAME_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspRpcU32 v=0u;v<NV_GSP_RPC_FRAME_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspRpcFrameVerdictName(v),"unknown")!=0);
        check("success equality",nvGspRpcFrameSucceeded(v)==(v==NV_GSP_RPC_FRAME_OK));
    }
}

int main(void) {
    std::printf("NVGspRpcFrame.hpp -- GSP-RM non-CC RPC frame rehearsal\n");
    testConstants();
    testZeroParamGolden();
    testNestedLengths();
    testAtomicRefusal();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
