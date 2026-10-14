#include <cstdio>
#include <cstring>
#include "NVGspWprPolicy.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *label, unsigned got, unsigned want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got %u want %u\n",label,got,want); }
}
static void checkU64(const char *label, unsigned long long got, unsigned long long want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got 0x%llx want 0x%llx\n",label,got,want); }
}
static void checkBool(const char *label, bool got, bool want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got %d want %d\n",label,(int)got,(int)want); }
}

static NVGspGa107WprPolicyInput input(unsigned long long fbGiB,
                                      unsigned long long preMiB,
                                      unsigned long long postMiB) {
    NVGspGa107WprPolicyInput in={};
    in.usableFbBytes=fbGiB*NV_GSP_WPR_POLICY_GIB;
    in.preWprHeapBytes=preMiB*NV_GSP_WPR_POLICY_MIB;
    in.postWprHeapBytes=postMiB*NV_GSP_WPR_POLICY_MIB;
    return in;
}

static void checkAtomicRefusal(const char *label,
                               const NVGspGa107WprPolicyInput &in,
                               unsigned want) {
    NVGspGa107WprPolicyPlan out;
    std::memset(&out,0xa5,sizeof(out));
    NVGspGa107WprPolicyPlan before=out;
    unsigned got=nvGspPlanGa107FirstBootWprPolicy(&in,&out);
    checkU32(label,got,want);
    ++gChecks;
    if (std::memcmp(&out,&before,sizeof(out))!=0) {
        ++gFailures;
        std::printf("FAIL %s mutated output on refusal\n",label);
    }
}

int main(void) {
    const unsigned long long M=NV_GSP_WPR_POLICY_MIB;

    checkU64("first boot margin",NV_GSP_GA107_FIRST_BOOT_WPR_END_MARGIN_BYTES,0ull);
    checkU64("FRTS",NV_GSP_GA107_FRTS_BYTES,1ull*M);
    checkU64("non-WPR heap",NV_GSP_GA107_NON_WPR_HEAP_BYTES,1ull*M);
    checkU64("prescrubbed top",NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES,256ull*M);
    checkU64("LibOS carveout",NV_GSP_GA107_LIBOS3_OS_CARVEOUT_BYTES,22ull*M);
    checkU64("RM base",NV_GSP_GA107_RM_HEAP_BASE_BYTES,8ull*M);
    checkU64("client reserve",NV_GSP_GA107_CLIENT_HEAP_BYTES,96ull*M);
    checkU64("minimum heap",NV_GSP_GA107_MIN_WPR_HEAP_BYTES,88ull*M);
    checkU64("override-only max",NV_GSP_GA107_OVERRIDE_MAX_WPR_HEAP_BYTES,280ull*M);

    NVGspGa107WprPolicyPlan out={};
    NVGspGa107WprPolicyInput in=input(4,2,4);
    checkU32("4GiB verdict",nvGspPlanGa107FirstBootWprPolicy(&in,&out),NV_GSP_WPR_POLICY_OK);
    checkBool("4GiB valid",out.valid,true);
    checkBool("4GiB clamp active",out.prescrubClampApplied,true);
    checkU64("4GiB rounded GiB",out.usableFbGiB,4ull);
    checkU64("4GiB per-FB rounded",out.perFbHeapBytes,1ull*M);
    checkU64("4GiB calculated heap",out.calculatedHeapBytes,127ull*M);
    checkU64("4GiB prescrub max",out.maxPrescrubbedHeapBytes,250ull*M);
    checkU64("4GiB final heap",out.wprHeapBytes,127ull*M);
    checkU64("4GiB margin",out.wprEndMarginBytes,0ull);
    checkU64("4GiB FRTS",out.frtsBytes,1ull*M);
    checkU64("4GiB non-WPR raw",out.nonWprHeapRawBytes,1ull*M);

    in=input(11,2,4); out={};
    checkU32("11GiB verdict",nvGspPlanGa107FirstBootWprPolicy(&in,&out),NV_GSP_WPR_POLICY_OK);
    checkU64("11GiB per-FB rounds to 2MiB",out.perFbHeapBytes,2ull*M);
    checkU64("11GiB calculated heap",out.calculatedHeapBytes,128ull*M);
    checkU64("11GiB final heap",out.wprHeapBytes,128ull*M);

    // Open RM rounds usable FB upward to whole GiB before applying 96 KiB/GiB.
    in=input(4,2,4); in.usableFbBytes+=1ull; out={};
    checkU32("4GiB+1 verdict",nvGspPlanGa107FirstBootWprPolicy(&in,&out),NV_GSP_WPR_POLICY_OK);
    checkU64("4GiB+1 rounds to 5GiB",out.usableFbGiB,5ull);
    checkU64("4GiB+1 still 1MiB growth bucket",out.perFbHeapBytes,1ull*M);

    // Normal calculation clamps to the remaining pre-scrubbed window AFTER the
    // 88 MiB minimum. This can therefore end below that nominal minimum.
    in=input(4,2,200); out={};
    checkU32("tight prescrub verdict",nvGspPlanGa107FirstBootWprPolicy(&in,&out),NV_GSP_WPR_POLICY_OK);
    checkU64("tight prescrub max",out.maxPrescrubbedHeapBytes,54ull*M);
    checkU64("tight prescrub final below min",out.wprHeapBytes,54ull*M);

    // anterior=posterior=0 disables the pre-scrub clamp in open RM. The normal
    // calculated path also does NOT use the 280 MiB registry-override maximum.
    in=input(2000,0,0); out={};
    checkU32("zero pre/post verdict",nvGspPlanGa107FirstBootWprPolicy(&in,&out),NV_GSP_WPR_POLICY_OK);
    checkBool("zero pre/post clamp disabled",out.prescrubClampApplied,false);
    checkU64("zero pre/post no max marker",out.maxPrescrubbedHeapBytes,~0ull);
    checkU64("2000GiB per-FB",out.perFbHeapBytes,188ull*M);
    checkU64("normal path exceeds override max",out.wprHeapBytes,314ull*M);

    in=input(4,128,128);
    checkAtomicRefusal("prescrub exactly consumed",in,NV_GSP_WPR_POLICY_PRESCRUB_TOO_SMALL);

    in=input(4,255,0); in.preWprHeapBytes += M/2ull;
    checkAtomicRefusal("sub-MiB remainder yields zero heap",in,NV_GSP_WPR_POLICY_ZERO_HEAP);

    in=input(4,0,0); in.preWprHeapBytes=~0ull; in.postWprHeapBytes=1ull;
    checkAtomicRefusal("pre+post overflow",in,NV_GSP_WPR_POLICY_OVERFLOW);

    in=input(4,2,4); in.usableFbBytes=0;
    checkAtomicRefusal("zero FB",in,NV_GSP_WPR_POLICY_BAD_FRAMEBUFFER);

    in=input(4,2,4);
    checkU32("null output",nvGspPlanGa107FirstBootWprPolicy(&in,nullptr),NV_GSP_WPR_POLICY_NULL_OUTPUT);
    checkU32("null input",nvGspPlanGa107FirstBootWprPolicy(nullptr,&out),NV_GSP_WPR_POLICY_NULL_INPUT);

    std::printf("GA107 WPR policy: %d checks, %d failures\n",gChecks,gFailures);
    return gFailures ? 1 : 0;
}
