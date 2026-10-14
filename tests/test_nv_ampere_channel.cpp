// Host tests for NVAmpereChannel.hpp. No IOKit, RM, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVAmpereChannel.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVChannelU32 got, NVChannelU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got %u want %u\n",label,got,want); }
}
static void checkU64(const char *label, NVChannelU64 got, NVChannelU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,(unsigned long long)got,(unsigned long long)want); }
}

static NVAmpereChannelEvidence goodEvidence(void) {
    NVAmpereChannelEvidence e={};
    e.channelClassNum=NV_SUBMIT_C56F_CLASS_ID;
    e.numGpFifoEntries=1024u;
    e.gpGetMapped=true;
    e.gpPutMapped=true;
    e.gpFifoMapped=true;
    e.workSubmissionMapped=true;
    e.workSubmissionTokenReady=true;
    return e;
}

static void testReadinessLadder(void) {
    std::printf("\n1. RM allocation -> CE bind -> submit readiness\n");
    NVAmpereChannelReadiness r={};
    NVAmpereChannelEvidence e=goodEvidence();
    checkU32("fully ready",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_OK);
    check("allocation ready",r.allocationReady);
    check("CE bound",r.ceBoundReady);
    check("submit ready",r.submitReady);

    e.workSubmissionMapped=false;
    checkU32("missing doorbell endpoint",
        nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_NO_WORK_SUBMISSION);
    check("allocation still proven",r.allocationReady);
    check("CE not proven",!r.ceBoundReady);
    check("submit not ready",!r.submitReady);

    e=goodEvidence();
    e.workSubmissionTokenReady=false;
    checkU32("missing token",
        nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_NO_WORK_TOKEN);
    check("allocation retained",r.allocationReady);
    check("token blocks CE readiness",!r.ceBoundReady && !r.submitReady);
}

static void testAllocationRefusals(void) {
    std::printf("\n2. incomplete RM channel handoff is refused\n");
    NVAmpereChannelReadiness r={true,true,true};
    NVAmpereChannelEvidence e=goodEvidence();

    e.channelClassNum=0xc46fu;
    checkU32("wrong channel class",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_BAD_CLASS);
    check("wrong class clears readiness",!r.allocationReady && !r.ceBoundReady && !r.submitReady);

    e=goodEvidence(); e.numGpFifoEntries=1u;
    checkU32("sentinel requires >=2 entries",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_TOO_FEW_ENTRIES);
    check("small ring not allocated",!r.allocationReady);

    e=goodEvidence(); e.gpGetMapped=false;
    checkU32("gpGet required",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_NO_GPGET);
    e=goodEvidence(); e.gpPutMapped=false;
    checkU32("gpPut required",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_NO_GPPUT);
    e=goodEvidence(); e.gpFifoMapped=false;
    checkU32("gpfifo mapping required",nvAmpereEvaluateChannel(e,&r),NV_CHANNEL_READY_NO_GPFIFO);
    checkU32("null readiness output",nvAmpereEvaluateChannel(goodEvidence(),nullptr),NV_CHANNEL_READY_NULL_OUTPUT);
}

static void testRingPlan(void) {
    std::printf("\n3. one-sentinel GPFIFO ring arithmetic\n");
    NVAmpereRingWritePlan p={};
    const NVChannelU64 entry=0x0000381234567000ull;

    checkU32("empty ring accepts one",nvAmperePlanOneGpfifoWrite(8u,0u,0u,entry,&p),NV_CHANNEL_RING_OK);
    checkU32("slot is old cpu put",p.slot,0u);
    checkU32("new put advances",p.newGpPut,1u);
    checkU64("entry preserved",p.entry,entry);

    checkU32("wrap enqueue",nvAmperePlanOneGpfifoWrite(8u,7u,3u,entry,&p),NV_CHANNEL_RING_OK);
    checkU32("wrap slot",p.slot,7u);
    checkU32("wrap new put",p.newGpPut,0u);

    std::memset(&p,0xa5,sizeof(p));
    NVAmpereRingWritePlan before=p;
    checkU32("full before sentinel overwrite",
        nvAmperePlanOneGpfifoWrite(8u,7u,0u,entry,&p),NV_CHANNEL_RING_FULL);
    check("full refusal atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("two-entry ring full at one pending",
        nvAmperePlanOneGpfifoWrite(2u,0u,1u,entry,&p),NV_CHANNEL_RING_FULL);
    check("two-entry refusal atomic",std::memcmp(&p,&before,sizeof(p))==0);
}

static void testRingRefusals(void) {
    std::printf("\n4. bad ring coordinates are atomic refusals\n");
    NVAmpereRingWritePlan p;
    std::memset(&p,0x5a,sizeof(p));
    NVAmpereRingWritePlan before=p;

    checkU32("too few entries",nvAmperePlanOneGpfifoWrite(1u,0u,0u,1ull,&p),NV_CHANNEL_RING_TOO_FEW_ENTRIES);
    check("too few unchanged",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("bad cpu put",nvAmperePlanOneGpfifoWrite(8u,8u,0u,1ull,&p),NV_CHANNEL_RING_BAD_CPU_PUT);
    check("bad cpu unchanged",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("bad gpu get",nvAmperePlanOneGpfifoWrite(8u,0u,8u,1ull,&p),NV_CHANNEL_RING_BAD_GPU_GET);
    check("bad gpu unchanged",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null ring output",nvAmperePlanOneGpfifoWrite(8u,0u,0u,1ull,nullptr),NV_CHANNEL_RING_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict families\n");
    for (NVChannelU32 v=0;v<NV_CHANNEL_READY_LIMIT;++v) {
        check("readiness verdict named",std::strcmp(nvChannelReadyVerdictName(v),"unknown")!=0);
        check("readiness success equality",nvChannelReadySucceeded(v)==(v==NV_CHANNEL_READY_OK));
    }
    for (NVChannelU32 v=0;v<NV_CHANNEL_RING_LIMIT;++v) {
        check("ring verdict named",std::strcmp(nvChannelRingVerdictName(v),"unknown")!=0);
        check("ring success equality",nvChannelRingSucceeded(v)==(v==NV_CHANNEL_RING_OK));
    }
}

int main(void) {
    std::printf("NVAmpereChannel.hpp -- GA107 RM handoff and ring contract\n");
    testReadinessLadder();
    testAllocationRefusals();
    testRingPlan();
    testRingRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
