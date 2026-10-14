// Host tests for NVSinkPeer.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVSinkPeer.hpp"

static int gChecks=0,gFailures=0;
static void check(const char *label,bool ok){++gChecks;if(!ok){++gFailures;std::printf("  FAIL %s\n",label);}}
static void checkU32(const char *label,NVPeerU32 got,NVPeerU32 want){++gChecks;if(got!=want){++gFailures;std::printf("  FAIL %s: got %u want %u\n",label,got,want);}}

static void testHappyPaths(void)
{
    std::printf("\n1. conservative GA107 peer IOVM window\n");
    checkU32("one page succeeds",nvSinkPeerValidate(0x1000ull,4096ull,4096ull,4096ull),NV_SINK_PEER_OK);
    checkU32("64 KiB succeeds",nvSinkPeerValidate(0x100000ull,65536ull,65536ull,65536ull),NV_SINK_PEER_OK);
    const NVPeerU64 topPage=(NV_SINK_PEER_ADDR_LIMIT_POLICY+1ull)-4096ull;
    checkU32("last 4 KiB page succeeds",nvSinkPeerValidate(topPage,4096ull,4096ull,4096ull),NV_SINK_PEER_OK);
}

static void testRefusals(void)
{
    std::printf("\n2. refusal order\n");
    checkU32("zero bytes first",nvSinkPeerValidate(0ull,0ull,0ull,0ull),NV_SINK_PEER_ZERO_BYTES);
    checkU32("zero address",nvSinkPeerValidate(0ull,4096ull,4096ull,4096ull),NV_SINK_PEER_ADDR_ZERO);
    checkU32("unaligned before range",nvSinkPeerValidate(0x1001ull,4096ull,4096ull,4096ull),NV_SINK_PEER_ADDR_UNALIGNED);
    checkU32("base above 41 bits",nvSinkPeerValidate(1ull<<41,4096ull,4096ull,4096ull),NV_SINK_PEER_ADDR_RANGE);
    checkU32("range crosses 41-bit ceiling",nvSinkPeerValidate((1ull<<41)-4096ull,8192ull,8192ull,8192ull),NV_SINK_PEER_ADDR_RANGE);
    checkU32("short segment",nvSinkPeerValidate(0x1000ull,4095ull,4096ull,4096ull),NV_SINK_PEER_SEGMENT_SHORT);
    checkU32("short consumed offset",nvSinkPeerValidate(0x1000ull,4096ull,4095ull,4096ull),NV_SINK_PEER_OFFSET_SHORT);
}

static void testVerdicts(void)
{
    std::printf("\n3. verdict family\n");
    check("41-bit limit exact",NV_SINK_PEER_ADDR_LIMIT_POLICY==0x000001ffffffffffull);
    for(NVPeerU32 v=0;v<NV_SINK_PEER_LIMIT;++v){
        check("verdict named",std::strcmp(nvSinkPeerVerdictName(v),"unknown")!=0);
        check("success equality",nvSinkPeerSucceeded(v)==(v==NV_SINK_PEER_OK));
    }
}

int main(void)
{
    std::printf("NVSinkPeer.hpp -- GA107-side shared sink IOVM validation\n");
    testHappyPaths();
    testRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
