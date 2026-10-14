// Host tests for NVGspQueueObserve.hpp. No IOKit, BAR0 or hardware required.
#include <cstdio>
#include <cstring>
#include "NVGspQueueObserve.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspObsU32 got, NVGspObsU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}

static void testEmptyZeroSnapshot(void) {
    std::printf("\n1. all-zero snapshot is honest but not proof of initialization\n");
    NVGspQueueSnapshot p={};
    checkU32("verdict",nvGspClassifyQueueSnapshot(0u,0u,0u,0u,0u,&p),NV_GSP_QUEUE_OBSERVE_OK);
    checkU32("count",p.messageCount,63u);
    checkU32("command used",p.commandUsed,0u);
    checkU32("status available",p.statusAvailable,0u);
    check("all zero",p.allZero);
    check("not all ones",!p.allOnes);
    check("no traffic evidence",!p.anyTrafficEvidence);
    check("counters plausible",p.countersPlausibleWithoutHistory);
}

static void testPlausibleTraffic(void) {
    std::printf("\n2. plausible monotonic queue traffic\n");
    NVGspQueueSnapshot p={};
    checkU32("verdict",nvGspClassifyQueueSnapshot(0u,70u,65u,100u,98u,&p),NV_GSP_QUEUE_OBSERVE_OK);
    checkU32("command head",p.commandHead,70u);
    checkU32("command tail",p.commandTail,65u);
    checkU32("command used",p.commandUsed,5u);
    checkU32("status available",p.statusAvailable,2u);
    check("not zero",!p.allZero);
    check("traffic evidence",p.anyTrafficEvidence);
}

static void testCounterRollover(void) {
    std::printf("\n3. NvU32 rollover remains a small monotonic delta\n");
    NVGspQueueSnapshot p={};
    checkU32("verdict",nvGspClassifyQueueSnapshot(0u,1u,0xfffffffeu,2u,0xffffffffu,&p),
             NV_GSP_QUEUE_OBSERVE_OK);
    checkU32("command delta",p.commandUsed,3u);
    checkU32("status delta",p.statusAvailable,3u);
    check("plausible",p.countersPlausibleWithoutHistory);
    check("single all-ones counter is legal",!p.allOnes);
}

static void testAmbiguousResetShape(void) {
    std::printf("\n4. impossible one-shot delta is ambiguous, not declared corruption\n");
    NVGspQueueSnapshot p={};
    checkU32("verdict",nvGspClassifyQueueSnapshot(0u,64u,0u,100u,0u,&p),
             NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS);
    checkU32("command delta",p.commandUsed,64u);
    checkU32("status delta",p.statusAvailable,100u);
    check("traffic evidence",p.anyTrafficEvidence);
    check("history needed",!p.countersPlausibleWithoutHistory);
}

static void testDeadReadAndRefusals(void) {
    std::printf("\n5. dead reads and bad indices never fabricate a snapshot\n");
    NVGspQueueSnapshot p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspQueueSnapshot before=p;
    checkU32("all-four dead snapshot",
        nvGspClassifyQueueSnapshot(0u,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,&p),
        NV_GSP_QUEUE_OBSERVE_DEAD_READ);
    check("dead atomic",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("bad index",nvGspClassifyQueueSnapshot(8u,0u,0u,0u,0u,&p),
             NV_GSP_QUEUE_OBSERVE_BAD_INDEX);
    check("index atomic",std::memcmp(&p,&before,sizeof(p))==0);
    checkU32("null output",nvGspClassifyQueueSnapshot(0u,0u,0u,0u,0u,nullptr),
             NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n6. verdict family\n");
    for (NVGspObsU32 v=0u;v<NV_GSP_QUEUE_OBSERVE_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspQueueObserveVerdictName(v),"unknown")!=0);
        check("success equality",nvGspQueueObserveSucceeded(v)==(v==NV_GSP_QUEUE_OBSERVE_OK));
    }
}

int main(void) {
    std::printf("NVGspQueueObserve.hpp -- read-only PGSP queue snapshot classifier\n");
    testEmptyZeroSnapshot();
    testPlausibleTraffic();
    testCounterRollover();
    testAmbiguousResetShape();
    testDeadReadAndRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
