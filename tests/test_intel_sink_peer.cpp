// Host tests for IntelSinkPeer.hpp. No IOKit, IOMMU or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelSinkPeer.hpp"

static int gChecks=0,gFailures=0;
static void check(const char *label,bool ok){++gChecks;if(!ok){++gFailures;std::printf("  FAIL %s\n",label);}}
static void checkU32(const char *label,IGPeerU32 got,IGPeerU32 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got %u want %u\n",label,got,want);}}
static void checkU64(const char *label,IGPeerU64 got,IGPeerU64 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got 0x%016llX want 0x%016llX\n",label,(unsigned long long)got,(unsigned long long)want);}}
static bool zeroMap(const IGSinkPeerMap &m){return m.iovmAddress==0ull&&m.segmentBytes==0ull&&m.mappedBytes==0ull;}

static void testPolicy(void)
{
    std::printf("\n1. conservative NVIDIA peer policy\n");
    checkU32("peer bits reuse NVIDIA staging policy",IG_SINK_PEER_DMA_ADDR_BITS_POLICY,41u);
    checkU64("peer limit is 41-bit ceiling",IG_SINK_PEER_DMA_LIMIT_POLICY,0x000001ffffffffffull);
    checkU32("page alignment",IG_SINK_PEER_PAGE_BYTES,4096u);
}

static void testAcceptedMappings(void)
{
    std::printf("\n2. accepted contiguous peer mappings\n");
    IGSinkPeerMap m={};
    checkU32("8 MiB mapping accepted",
        igSinkPeerPlan(0x0000001200000000ull,0x00800000ull,0x00800000ull,&m),
        IG_SINK_PEER_OK);
    checkU64("IOVM preserved",m.iovmAddress,0x0000001200000000ull);
    checkU64("segment preserved",m.segmentBytes,0x00800000ull);
    checkU64("mapped bytes",m.mappedBytes,0x00800000ull);

    const IGPeerU64 topPage=IG_SINK_PEER_DMA_LIMIT_POLICY-4095ull;
    checkU32("last policy page accepted",
        igSinkPeerPlan(topPage,4096ull,4096ull,&m),IG_SINK_PEER_OK);
    checkU64("top page preserved",m.iovmAddress,topPage);
}

static void testRefusals(void)
{
    std::printf("\n3. refusal order and atomic output\n");
    IGSinkPeerMap m={1ull,2ull,3ull};
    checkU32("null output",igSinkPeerPlan(0x1000,4096,4096,nullptr),IG_SINK_PEER_NULL_OUT);

    m={1ull,2ull,3ull};
    checkU32("zero size",igSinkPeerPlan(0x1000,4096,0,&m),IG_SINK_PEER_ZERO_SIZE);
    check("zero size clears",zeroMap(m));

    m={1ull,2ull,3ull};
    checkU32("unaligned IOVM",igSinkPeerPlan(0x1001,4096,4096,&m),IG_SINK_PEER_IOVM_UNALIGNED);
    check("unaligned clears",zeroMap(m));

    m={1ull,2ull,3ull};
    checkU32("base above ceiling",igSinkPeerPlan(IG_SINK_PEER_DMA_LIMIT_POLICY+1ull,4096,4096,&m),IG_SINK_PEER_IOVM_RANGE);
    check("high base clears",zeroMap(m));

    const IGPeerU64 topPage=IG_SINK_PEER_DMA_LIMIT_POLICY-4095ull;
    m={1ull,2ull,3ull};
    checkU32("range crosses ceiling",igSinkPeerPlan(topPage,8192,8192,&m),IG_SINK_PEER_IOVM_RANGE);
    check("crossing range clears",zeroMap(m));

    m={1ull,2ull,3ull};
    checkU32("short segment",igSinkPeerPlan(0x1000,4095,4096,&m),IG_SINK_PEER_SEGMENT_SHORT);
    check("short segment clears",zeroMap(m));
}

static void testVerdicts(void)
{
    std::printf("\n4. verdict family\n");
    for(IGPeerU32 v=0;v<IG_SINK_PEER_LIMIT;++v){
        check("verdict named",std::strcmp(igSinkPeerVerdictName(v),"unknown")!=0);
        check("success equality",igSinkPeerSucceeded(v)==(v==IG_SINK_PEER_OK));
    }
}

int main(void)
{
    std::printf("IntelSinkPeer.hpp -- NVIDIA peer DMA mapping validation\n");
    testPolicy();
    testAcceptedMappings();
    testRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
