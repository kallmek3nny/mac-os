// Host tests for NVAmpereRmPlan.hpp. No IOKit, GSP-RM, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVAmpereRmPlan.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVRmU32 got, NVRmU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got %u want %u\n",label,got,want); }
}

static void testConstants(void) {
    std::printf("\n1. public UVM allocation constants\n");
    checkU32("CE engine type",NV_RM_UVM_ENGINE_TYPE_CE,2u);
    checkU32("max CE slots",NV_RM_UVM_COPY_ENGINE_COUNT_MAX,64u);
    checkU32("default location",NV_RM_UVM_BUFFER_LOCATION_DEFAULT,0u);
    checkU32("sys location",NV_RM_UVM_BUFFER_LOCATION_SYS,1u);
    checkU32("vid location",NV_RM_UVM_BUFFER_LOCATION_VID,2u);
    checkU32("default entries",NV_RM_GPFIFO_ENTRIES_DEFAULT,1024u);
    checkU32("min entries",NV_RM_GPFIFO_ENTRIES_MIN,32u);
    checkU32("max entries",NV_RM_GPFIFO_ENTRIES_MAX,1024u*1024u);
}

static void testHappyPaths(void) {
    std::printf("\n2. exact TSG -> channel request plan\n");
    NVAmpereRmChannelPlan p={};
    checkU32("default request",
        nvAmpereBuildRmChannelPlan(0u,true,1024u,
                                   NV_RM_UVM_BUFFER_LOCATION_DEFAULT,
                                   NV_RM_UVM_BUFFER_LOCATION_DEFAULT,&p),
        NV_RM_PLAN_OK);
    checkU32("tsg engine type",p.tsg.engineType,2u);
    checkU32("tsg engine index",p.tsg.engineIndex,0u);
    checkU32("channel entries",p.channel.numGpFifoEntries,1024u);
    checkU32("channel fifo loc",p.channel.gpFifoLoc,0u);
    checkU32("channel put loc",p.channel.gpPutLoc,0u);

    checkU32("last CE index + minimum ring",
        nvAmpereBuildRmChannelPlan(63u,true,32u,
                                   NV_RM_UVM_BUFFER_LOCATION_SYS,
                                   NV_RM_UVM_BUFFER_LOCATION_VID,&p),
        NV_RM_PLAN_OK);
    checkU32("last CE preserved",p.tsg.engineIndex,63u);
    checkU32("min ring preserved",p.channel.numGpFifoEntries,32u);
    checkU32("SYS fifo preserved",p.channel.gpFifoLoc,1u);
    checkU32("VID put preserved",p.channel.gpPutLoc,2u);

    checkU32("maximum ring accepted",
        nvAmpereBuildRmChannelPlan(1u,true,NV_RM_GPFIFO_ENTRIES_MAX,
                                   NV_RM_UVM_BUFFER_LOCATION_VID,
                                   NV_RM_UVM_BUFFER_LOCATION_SYS,&p),
        NV_RM_PLAN_OK);
    checkU32("max ring preserved",p.channel.numGpFifoEntries,NV_RM_GPFIFO_ENTRIES_MAX);
}

static void testPowerOfTwo(void) {
    std::printf("\n3. UVM power-of-two ring policy\n");
    check("zero not power of two",!nvRmPowerOfTwo(0u));
    check("one is power of two",nvRmPowerOfTwo(1u));
    check("32 is power of two",nvRmPowerOfTwo(32u));
    check("1024 is power of two",nvRmPowerOfTwo(1024u));
    check("max is power of two",nvRmPowerOfTwo(NV_RM_GPFIFO_ENTRIES_MAX));
    check("96 not power of two",!nvRmPowerOfTwo(96u));
    check("1000 not power of two",!nvRmPowerOfTwo(1000u));
}

static void testAtomicRefusals(void) {
    std::printf("\n4. invalid RM requests leave plan untouched\n");
    NVAmpereRmChannelPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVAmpereRmChannelPlan before=p;

    checkU32("engine index 64 refused",
        nvAmpereBuildRmChannelPlan(64u,true,1024u,0u,0u,&p),NV_RM_PLAN_BAD_ENGINE_INDEX);
    check("bad engine index atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("unsupported CE refused",
        nvAmpereBuildRmChannelPlan(0u,false,1024u,0u,0u,&p),NV_RM_PLAN_ENGINE_UNSUPPORTED);
    check("unsupported CE atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("31 entries refused",
        nvAmpereBuildRmChannelPlan(0u,true,31u,0u,0u,&p),NV_RM_PLAN_GPFIFO_TOO_SMALL);
    check("small ring atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("max+1 refused",
        nvAmpereBuildRmChannelPlan(0u,true,NV_RM_GPFIFO_ENTRIES_MAX+1u,0u,0u,&p),NV_RM_PLAN_GPFIFO_TOO_LARGE);
    check("large ring atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("non-power-of-two refused",
        nvAmpereBuildRmChannelPlan(0u,true,1000u,0u,0u,&p),NV_RM_PLAN_GPFIFO_NOT_POWER_OF_TWO);
    check("non-power ring atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("bad fifo location refused",
        nvAmpereBuildRmChannelPlan(0u,true,1024u,3u,0u,&p),NV_RM_PLAN_BAD_GPFIFO_LOCATION);
    check("bad fifo location atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("bad put location refused",
        nvAmpereBuildRmChannelPlan(0u,true,1024u,0u,3u,&p),NV_RM_PLAN_BAD_GPPUT_LOCATION);
    check("bad put location atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("null output refused",
        nvAmpereBuildRmChannelPlan(0u,true,1024u,0u,0u,nullptr),NV_RM_PLAN_NULL_OUTPUT);
}

static void testLocationsAndVerdicts(void) {
    std::printf("\n5. location and verdict families\n");
    check("default loc valid",nvRmLocationValid(0u));
    check("sys loc valid",nvRmLocationValid(1u));
    check("vid loc valid",nvRmLocationValid(2u));
    check("loc 3 invalid",!nvRmLocationValid(3u));

    for (NVRmU32 v=0u;v<NV_RM_PLAN_LIMIT;++v) {
        check("verdict named",std::strcmp(nvRmPlanVerdictName(v),"unknown")!=0);
        check("success equality",nvRmPlanSucceeded(v)==(v==NV_RM_PLAN_OK));
    }
}

int main(void) {
    std::printf("NVAmpereRmPlan.hpp -- GA107 RM/UVM channel request rehearsal\n");
    testConstants();
    testHappyPaths();
    testPowerOfTwo();
    testAtomicRefusals();
    testLocationsAndVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
