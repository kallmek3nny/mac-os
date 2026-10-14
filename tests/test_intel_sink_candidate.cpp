// Host tests for IntelSinkCandidate.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelSinkCandidate.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, IGS32 got, IGS32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got %u want %u\n",label,got,want); }
}
static void checkU64(const char *label, IGS64 got, IGS64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got %llu want %llu\n",label,(unsigned long long)got,(unsigned long long)want); }
}

static IgSinkAllocPlan goodPlan(void) {
    IgSinkAllocPlan p={0x00400000u,7680u,7680u*1080u};
    return p;
}

static void testDescriptorButNotReady(void) {
    std::printf("\n1. allocated physical candidate is not peer-ready\n");
    IgSinkAllocPlan p=goodPlan();
    IGSinkCandidate c={};
    IGS32 v=igSinkCandidateBuild(&p,1920u,1080u,4u,
                                 0x0000000123400000ull,(IGS64)p.size,1,
                                 0x100000000ull,0,0,0,&c);
    checkU32("candidate composes",v,IG_SINK_CANDIDATE_OK);
    checkU32("publish verdict",c.publishVerdict,IG_SCAN_OK);
    checkU64("physical address preserved",c.descriptor.busAddress,0x0000000123400000ull);
    checkU64("extent preserved",c.descriptor.extentBytes,(IGS64)p.size);
    check("backing contiguous",c.backingContiguous!=0);
    check("GGTT not mapped",c.ggttMapped==0);
    check("peer DMA not mapped",c.peerDmaMapped==0);
    check("plane not programmed",c.planeProgrammed==0);
    check("producer not ready",c.producerReady==0);
    check("scanout not ready",c.scanoutReady==0);
}

static void testReadinessLadder(void) {
    std::printf("\n2. readiness requires every explicit stage\n");
    IgSinkAllocPlan p=goodPlan();
    IGSinkCandidate c={};

    checkU32("GGTT only composes",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size,1,
                             0x100000000ull,1,0,0,&c),IG_SINK_CANDIDATE_OK);
    check("GGTT alone not producer-ready",!c.producerReady);

    checkU32("peer plus GGTT composes",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size,1,
                             0x100000000ull,1,1,0,&c),IG_SINK_CANDIDATE_OK);
    check("producer ready after both mappings",c.producerReady!=0);
    check("scanout still not ready",c.scanoutReady==0);

    checkU32("all stages compose",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size,1,
                             0x100000000ull,1,1,1,&c),IG_SINK_CANDIDATE_OK);
    check("producer remains ready",c.producerReady!=0);
    check("scanout ready only with plane programmed",c.scanoutReady!=0);
}

static void testPublishRefusals(void) {
    std::printf("\n3. publish layer still owns address truth\n");
    IgSinkAllocPlan p=goodPlan();
    IGSinkCandidate c={};

    checkU32("zero address is candidate refusal",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0u,(IGS64)p.size,1,
                             0x100000000ull,0,0,0,&c),IG_SINK_CANDIDATE_PUBLISH_REFUSED);
    checkU32("zero address preserves publish reason",c.publishVerdict,IG_PUB_ERR_ZERO_BUS);
    checkU64("refused descriptor cleared",c.descriptor.busAddress,0ull);
    check("refused candidate never ready",!c.producerReady && !c.scanoutReady);

    checkU32("unaligned address refused",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400001ull,(IGS64)p.size,1,
                             0x100000000ull,1,1,1,&c),IG_SINK_CANDIDATE_PUBLISH_REFUSED);
    checkU32("unaligned reason propagated",c.publishVerdict,IG_PUB_ERR_BUS_UNALIGNED);
    check("readiness flags cannot override refusal",!c.producerReady && !c.scanoutReady);

    checkU32("noncontiguous backing refused",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size,0,
                             0x100000000ull,1,1,1,&c),IG_SINK_CANDIDATE_PUBLISH_REFUSED);
    checkU32("contiguity reason propagated",c.publishVerdict,IG_PUB_ERR_NOT_CONTIGUOUS);
}

static void testShortBackingAndClearing(void) {
    std::printf("\n4. short backing cannot inherit stale readiness\n");
    IgSinkAllocPlan p=goodPlan();
    IGSinkCandidate c={};
    checkU32("warm successful candidate",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size,1,
                             0x100000000ull,1,1,1,&c),IG_SINK_CANDIDATE_OK);
    check("warm candidate ready",c.scanoutReady!=0);

    checkU32("one-byte-short backing refused",
        igSinkCandidateBuild(&p,1920u,1080u,4u,0x123400000ull,(IGS64)p.size-1ull,1,
                             0x100000000ull,1,1,1,&c),IG_SINK_CANDIDATE_BACKING_TOO_SHORT);
    checkU32("publish stage not reached",c.publishVerdict,IG_SINK_CANDIDATE_NOT_ATTEMPTED);
    checkU64("descriptor cleared",c.descriptor.extentBytes,0ull);
    check("mapping flags cleared",!c.ggttMapped && !c.peerDmaMapped && !c.planeProgrammed);
    check("readiness cleared",!c.producerReady && !c.scanoutReady);
}

static void testNullsAndVerdicts(void) {
    std::printf("\n5. nulls and verdict family\n");
    IgSinkAllocPlan p=goodPlan();
    IGSinkCandidate c={};
    checkU32("null out refused",
        igSinkCandidateBuild(&p,1u,1u,4u,0x1000ull,4u,1,0x100000ull,0,0,0,0),
        IG_SINK_CANDIDATE_NULL_OUT);
    checkU32("null plan refused",
        igSinkCandidateBuild(0,1u,1u,4u,0x1000ull,4u,1,0x100000ull,0,0,0,&c),
        IG_SINK_CANDIDATE_NULL_PLAN);
    for (IGS32 v=0;v<IG_SINK_CANDIDATE_LIMIT;++v) {
        check("verdict named",std::strcmp(igSinkCandidateVerdictName(v),"unknown")!=0);
        check("success equality",igSinkCandidateSucceeded(v)==(v==IG_SINK_CANDIDATE_OK));
    }
}

int main(void) {
    std::printf("IntelSinkCandidate.hpp -- staged cross-GPU sink readiness\n");
    testDescriptorButNotReady();
    testReadinessLadder();
    testPublishRefusals();
    testShortBackingAndClearing();
    testNullsAndVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
