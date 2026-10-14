// Host tests for NVAmpereFence.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVAmpereFence.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVFenceU32 got, NVFenceU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVFenceU64 got, NVFenceU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,(unsigned long long)got,(unsigned long long)want); }
}

static void testConstantsAndGoldenPacket(void) {
    std::printf("\n1. C7B5 tracking-release golden packet\n");
    checkU32("SET_SEMAPHORE_A",NV_FENCE_C7B5_SET_SEMAPHORE_A,0x240u);
    checkU32("SET_SEMAPHORE_B",NV_FENCE_C7B5_SET_SEMAPHORE_B,0x244u);
    checkU32("SET_SEMAPHORE_PAYLOAD",NV_FENCE_C7B5_SET_SEMAPHORE_PAYLOAD,0x248u);
    checkU32("LAUNCH_DMA",NV_FENCE_C7B5_LAUNCH_DMA,0x300u);
    checkU32("launch exact",NV_FENCE_C7B5_LAUNCH_TRACKING_RELEASE,0x0400000cu);
    checkU32("push dwords",NV_FENCE_TRACKING_PUSH_DWORDS,6u);
    checkU32("push bytes",NV_FENCE_TRACKING_PUSH_BYTES,24u);

    const NVFenceU64 va=0x0000123456789000ull;
    const NVFenceU32 payload=0x89abcdefu;
    NVAmpereFencePlan p={};
    checkU32("plan verdict",nvAmperePlanTrackingFence(va,payload,&p),NV_FENCE_PLAN_OK);
    checkU32("dwords",p.pushDwords,6u);
    checkU32("bytes",p.pushBytes,24u);
    checkU64("fence VA",p.fenceGpuVa,va);
    checkU32("payload",p.payload,payload);

    const NVFenceU32 expected[6]={
        0x20038090u,
        0x00001234u,
        0x56789000u,
        0x89abcdefu,
        0x200180c0u,
        0x0400000cu
    };
    for (NVFenceU32 i=0;i<6u;++i) {
        char label[48]; std::snprintf(label,sizeof(label),"push[%u]",i);
        checkU32(label,p.push[i],expected[i]);
    }
}

static void testAddressRefusalsAndZeroPayload(void) {
    std::printf("\n2. address policy and payload wrap value\n");
    NVAmpereFencePlan p={};
    checkU32("zero payload is valid",
        nvAmperePlanTrackingFence(0x4000ull,0u,&p),NV_FENCE_PLAN_OK);
    checkU32("zero payload stored",p.payload,0u);

    const NVFenceU64 lastAligned=NV_FENCE_C7B5_VA_LIMIT_49-3ull;
    checkU32("last fitting 4-byte semaphore",
        nvAmperePlanTrackingFence(lastAligned,1u,&p),NV_FENCE_PLAN_OK);

    std::memset(&p,0xa5,sizeof(p));
    NVAmpereFencePlan before=p;
    checkU32("zero VA refused",nvAmperePlanTrackingFence(0ull,1u,&p),NV_FENCE_PLAN_ZERO_ADDRESS);
    check("zero VA leaves output",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("unaligned refused",nvAmperePlanTrackingFence(0x4002ull,1u,&p),NV_FENCE_PLAN_ADDRESS_UNALIGNED);
    check("unaligned leaves output",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("range crossing refused",
        nvAmperePlanTrackingFence(lastAligned+4ull,1u,&p),NV_FENCE_PLAN_ADDRESS_RANGE);
    check("range refusal leaves output",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null plan refused",nvAmperePlanTrackingFence(0x4000ull,1u,nullptr),NV_FENCE_PLAN_NULL_OUTPUT);
}

static void testCompletionExtension(void) {
    std::printf("\n3. 32-bit semaphore to 64-bit completion extension\n");
    NVFenceU64 out=0ull;
    checkU32("ordinary advance",
        nvAmpereFenceExtendCompleted(0x0000000100000010ull,0x00000020u,0x100ull,&out),
        NV_FENCE_PLAN_OK);
    checkU64("ordinary exact",out,0x0000000100000020ull);

    checkU32("same payload allowed",
        nvAmpereFenceExtendCompleted(0x0000000100000020ull,0x00000020u,0x100ull,&out),
        NV_FENCE_PLAN_OK);
    checkU64("same exact",out,0x0000000100000020ull);

    checkU32("32-bit wrap",
        nvAmpereFenceExtendCompleted(0x00000001fffffff0ull,0x00000010u,0x100ull,&out),
        NV_FENCE_PLAN_OK);
    checkU64("wrap exact",out,0x0000000200000010ull);

    out=0x1122334455667788ull;
    checkU32("jump bound enforced",
        nvAmpereFenceExtendCompleted(0x0000000100000010ull,0x00000110u,0xffull,&out),
        NV_FENCE_PLAN_COUNTER_JUMP);
    checkU64("jump refusal leaves output",out,0x1122334455667788ull);

    checkU32("zero max jump refused",
        nvAmpereFenceExtendCompleted(0x10ull,0x11u,0ull,&out),
        NV_FENCE_PLAN_ZERO_MAX_JUMP);
    checkU64("zero max leaves output",out,0x1122334455667788ull);

    checkU32("64-bit wrap refused",
        nvAmpereFenceExtendCompleted(0xfffffffffffffff0ull,0x00000010u,0x100ull,&out),
        NV_FENCE_PLAN_COUNTER_OVERFLOW);
    checkU64("overflow leaves output",out,0x1122334455667788ull);
    checkU32("null counter output refused",
        nvAmpereFenceExtendCompleted(0x10ull,0x11u,1ull,nullptr),
        NV_FENCE_PLAN_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n4. verdict family\n");
    for (NVFenceU32 v=0u;v<NV_FENCE_PLAN_LIMIT;++v) {
        check("verdict named",std::strcmp(nvFencePlanVerdictName(v),"unknown")!=0);
        check("success equality",nvFencePlanSucceeded(v)==(v==NV_FENCE_PLAN_OK));
    }
}

int main(void) {
    std::printf("NVAmpereFence.hpp -- GA107 tracking completion rehearsal\n");
    testConstantsAndGoldenPacket();
    testAddressRefusalsAndZeroPayload();
    testCompletionExtension();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
