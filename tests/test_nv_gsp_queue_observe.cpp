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

static void testDoubleSampleRefusalAndProvenance(void) {
    std::printf("\n7. double-read queue snapshot refuses cross-epoch counter drift\n");
    NVGspQueueCounterSample first={70u,65u,100u,98u};
    NVGspQueueCounterSample second=first;
    NVGspQueueSnapshot observed={};
    checkU32("unchanged pair accepted",
        nvGspClassifyStableQueueSnapshot(0u,&first,&second,&observed),
        NV_GSP_QUEUE_OBSERVE_OK);
    check("double-read evidence explicit",observed.sampledTwiceUnchanged);
    checkU32("paired command occupancy",observed.commandUsed,5u);
    checkU32("paired response count",observed.statusAvailable,2u);

    NVGspQueueSnapshot single={};
    checkU32("ordinary one-shot classifier unchanged",
        nvGspClassifyQueueSnapshot(0u,70u,65u,100u,98u,&single),
        NV_GSP_QUEUE_OBSERVE_OK);
    check("one-shot cannot claim double-read stability",
          !single.sampledTwiceUnchanged);

    // Counter changes can occur on any of the four independent BAR0 reads.
    // Preserve a byte-for-byte sentinel on ALL refused observations.
    for (unsigned index=0u;index<4u;++index) {
        second=first;
        if (index==0u) ++second.commandHead;
        if (index==1u) ++second.commandTail;
        if (index==2u) ++second.messageHead;
        if (index==3u) ++second.messageTail;
        NVGspQueueSnapshot refusal;
        std::memset(&refusal,0xa5,sizeof(refusal));
        const NVGspQueueSnapshot prior=refusal;
        checkU32("changed field refused",
            nvGspClassifyStableQueueSnapshot(0u,&first,&second,&refusal),
            NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ);
        check("changed field refusal atomic",
              std::memcmp(&refusal,&prior,sizeof(refusal))==0);
    }

    first={1u,0xfffffffeu,2u,0xffffffffu};
    second=first;
    checkU32("unchanged monotonic counter rollover accepted",
        nvGspClassifyStableQueueSnapshot(0u,&first,&second,&observed),
        NV_GSP_QUEUE_OBSERVE_OK);
    checkU32("rolled command occupancy",observed.commandUsed,3u);
    check("rollover retains sample provenance",observed.sampledTwiceUnchanged);

    first={64u,0u,100u,0u};
    second=first;
    checkU32("same ambiguous counters remain ambiguous",
        nvGspClassifyStableQueueSnapshot(0u,&first,&second,&observed),
        NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS);
    check("stable read cannot invent cached queue history",
          observed.sampledTwiceUnchanged &&
          !observed.countersPlausibleWithoutHistory);

    first={0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    second=first;
    NVGspQueueSnapshot refused;
    std::memset(&refused,0xa5,sizeof(refused));
    const NVGspQueueSnapshot prior=refused;
    checkU32("two dead reads still refuse",
        nvGspClassifyStableQueueSnapshot(0u,&first,&second,&refused),
        NV_GSP_QUEUE_OBSERVE_DEAD_READ);
    check("dead read refuses atomically",
          std::memcmp(&refused,&prior,sizeof(refused))==0);

    first={0u,0u,0u,0u};
    second=first;
    checkU32("missing first sample",
        nvGspClassifyStableQueueSnapshot(0u,nullptr,&second,&refused),
        NV_GSP_QUEUE_OBSERVE_NULL_INPUT);
    checkU32("missing second sample",
        nvGspClassifyStableQueueSnapshot(0u,&first,nullptr,&refused),
        NV_GSP_QUEUE_OBSERVE_NULL_INPUT);
    checkU32("missing output",
        nvGspClassifyStableQueueSnapshot(0u,&first,&second,nullptr),
        NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT);
    checkU32("bad queue index",
        nvGspClassifyStableQueueSnapshot(8u,&first,&second,&refused),
        NV_GSP_QUEUE_OBSERVE_BAD_INDEX);
    check("all malformed pairs leave output untouched",
          std::memcmp(&refused,&prior,sizeof(refused))==0);
}

static void testLiveEvidencePublicationGate(void) {
    std::printf("\n8. only stable non-dead pairs can become live registry evidence\n");
    const NVGspQueueCounterSample live={70u,65u,100u,98u};
    NVGspQueueSnapshot single={};
    const NVGspObsU32 oneShot=nvGspClassifyQueueSnapshot(
        0u,live.commandHead,live.commandTail,live.messageHead,live.messageTail,
        &single);
    checkU32("one shot is plausible",oneShot,NV_GSP_QUEUE_OBSERVE_OK);
    check("one shot not publishable",
          !nvGspQueueSnapshotCanPublish(oneShot,&single));

    NVGspQueueSnapshot stable={};
    const NVGspObsU32 paired=nvGspClassifyStableQueueSnapshot(
        0u,&live,&live,&stable);
    check("paired plausible snapshot publishable",
          nvGspQueueSnapshotCanPublish(paired,&stable));
    check("null evidence never publishable",
          !nvGspQueueSnapshotCanPublish(paired,nullptr));
    check("not-attempted cannot publish",
          !nvGspQueueSnapshotCanPublish(NV_GSP_QUEUE_OBSERVE_NOT_ATTEMPTED,&stable));
    check("changed during read cannot publish",
          !nvGspQueueSnapshotCanPublish(NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ,&stable));
    check("dead BAR read cannot publish even with old snapshot",
          !nvGspQueueSnapshotCanPublish(NV_GSP_QUEUE_OBSERVE_DEAD_READ,&stable));
    check("invalid index cannot publish",
          !nvGspQueueSnapshotCanPublish(NV_GSP_QUEUE_OBSERVE_BAD_INDEX,&stable));

    const NVGspQueueCounterSample ambiguous={64u,0u,100u,0u};
    NVGspQueueSnapshot needsHistory={};
    const NVGspObsU32 ambiguousVerdict=nvGspClassifyStableQueueSnapshot(
        0u,&ambiguous,&ambiguous,&needsHistory);
    checkU32("paired history-less ambiguous verdict",ambiguousVerdict,
             NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS);
    check("ambiguous pair publishable for diagnostic counters",
          nvGspQueueSnapshotCanPublish(ambiguousVerdict,&needsHistory));
    check("ambiguous cannot claim ordinary plausibility",
          !needsHistory.countersPlausibleWithoutHistory);

    const NVGspQueueCounterSample dead={0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    NVGspQueueSnapshot untouched={};
    const NVGspObsU32 deadVerdict=nvGspClassifyStableQueueSnapshot(
        0u,&dead,&dead,&untouched);
    checkU32("dead read remains refused",deadVerdict,NV_GSP_QUEUE_OBSERVE_DEAD_READ);
    check("dead pair cannot publish",!nvGspQueueSnapshotCanPublish(deadVerdict,&untouched));
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
    testDoubleSampleRefusalAndProvenance();
    testLiveEvidencePublicationGate();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
