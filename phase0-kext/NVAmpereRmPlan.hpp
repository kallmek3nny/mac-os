// SPDX-License-Identifier: MIT
// Pure source-backed RM/UVM request plan for a GA107 copy-engine channel.
#ifndef NV_AMPERE_RM_PLAN_HPP
#define NV_AMPERE_RM_PLAN_HPP

#include "NVAmpereChannel.hpp"

using NVRmU32 = unsigned int;

// nv_uvm_types.h
#define NV_RM_UVM_ENGINE_TYPE_CE                 2u
#define NV_RM_UVM_COPY_ENGINE_COUNT_MAX          64u
#define NV_RM_UVM_BUFFER_LOCATION_DEFAULT        0u
#define NV_RM_UVM_BUFFER_LOCATION_SYS            1u
#define NV_RM_UVM_BUFFER_LOCATION_VID            2u

// uvm_channel.h / uvm_channel.c
#define NV_RM_GPFIFO_ENTRIES_DEFAULT             1024u
#define NV_RM_GPFIFO_ENTRIES_MIN                 32u
#define NV_RM_GPFIFO_ENTRIES_MAX                 (1024u * 1024u)

#define NV_RM_PLAN_NOT_ATTEMPTED                 0u
#define NV_RM_PLAN_OK                            1u
#define NV_RM_PLAN_NULL_OUTPUT                   2u
#define NV_RM_PLAN_BAD_ENGINE_INDEX              3u
#define NV_RM_PLAN_ENGINE_UNSUPPORTED            4u
#define NV_RM_PLAN_GPFIFO_TOO_SMALL              5u
#define NV_RM_PLAN_GPFIFO_TOO_LARGE              6u
#define NV_RM_PLAN_GPFIFO_NOT_POWER_OF_TWO       7u
#define NV_RM_PLAN_BAD_GPFIFO_LOCATION           8u
#define NV_RM_PLAN_BAD_GPPUT_LOCATION            9u
#define NV_RM_PLAN_LIMIT                         10u

struct NVAmpereRmTsgParams {
    NVRmU32 engineType;
    NVRmU32 engineIndex;
};

struct NVAmpereRmChannelParams {
    NVRmU32 numGpFifoEntries;
    NVRmU32 gpFifoLoc;
    NVRmU32 gpPutLoc;
};

struct NVAmpereRmChannelPlan {
    NVAmpereRmTsgParams tsg;
    NVAmpereRmChannelParams channel;
};

static inline bool nvRmPlanSucceeded(NVRmU32 verdict)
{
    return verdict==NV_RM_PLAN_OK;
}

static inline const char *nvRmPlanVerdictName(NVRmU32 verdict)
{
    switch (verdict) {
        case NV_RM_PLAN_NOT_ATTEMPTED:           return "not-attempted";
        case NV_RM_PLAN_OK:                      return "ok";
        case NV_RM_PLAN_NULL_OUTPUT:             return "null-output";
        case NV_RM_PLAN_BAD_ENGINE_INDEX:        return "bad-engine-index";
        case NV_RM_PLAN_ENGINE_UNSUPPORTED:      return "engine-unsupported";
        case NV_RM_PLAN_GPFIFO_TOO_SMALL:        return "gpfifo-too-small";
        case NV_RM_PLAN_GPFIFO_TOO_LARGE:        return "gpfifo-too-large";
        case NV_RM_PLAN_GPFIFO_NOT_POWER_OF_TWO: return "gpfifo-not-power-of-two";
        case NV_RM_PLAN_BAD_GPFIFO_LOCATION:     return "bad-gpfifo-location";
        case NV_RM_PLAN_BAD_GPPUT_LOCATION:      return "bad-gpput-location";
        default:                                 return "unknown";
    }
}

static inline bool nvRmLocationValid(NVRmU32 loc)
{
    return loc==NV_RM_UVM_BUFFER_LOCATION_DEFAULT ||
           loc==NV_RM_UVM_BUFFER_LOCATION_SYS ||
           loc==NV_RM_UVM_BUFFER_LOCATION_VID;
}

static inline bool nvRmPowerOfTwo(NVRmU32 value)
{
    return value!=0u && (value&(value-1u))==0u;
}

// Mirrors the public UVM request sequence for a CE-backed channel:
//   1) query CE capabilities and choose a supported engineIndex,
//   2) TsgAllocate(vaSpace, {engineType=CE, engineIndex}),
//   3) ChannelAllocate(tsg, {numGpFifoEntries,gpFifoLoc,gpPutLoc}, ...),
//   4) validate the returned UvmGpuChannelInfo with NVAmpereChannel.hpp.
//
// This function produces only the request parameters. It performs no RM/GSP
// call and cannot create or submit a hardware channel.
static inline NVRmU32 nvAmpereBuildRmChannelPlan(NVRmU32 engineIndex,
                                                 bool engineSupported,
                                                 NVRmU32 numGpFifoEntries,
                                                 NVRmU32 gpFifoLoc,
                                                 NVRmU32 gpPutLoc,
                                                 NVAmpereRmChannelPlan *out)
{
    if (!out) return NV_RM_PLAN_NULL_OUTPUT;
    if (engineIndex>=NV_RM_UVM_COPY_ENGINE_COUNT_MAX)
        return NV_RM_PLAN_BAD_ENGINE_INDEX;
    if (!engineSupported) return NV_RM_PLAN_ENGINE_UNSUPPORTED;
    if (numGpFifoEntries<NV_RM_GPFIFO_ENTRIES_MIN)
        return NV_RM_PLAN_GPFIFO_TOO_SMALL;
    if (numGpFifoEntries>NV_RM_GPFIFO_ENTRIES_MAX)
        return NV_RM_PLAN_GPFIFO_TOO_LARGE;
    if (!nvRmPowerOfTwo(numGpFifoEntries))
        return NV_RM_PLAN_GPFIFO_NOT_POWER_OF_TWO;
    if (!nvRmLocationValid(gpFifoLoc))
        return NV_RM_PLAN_BAD_GPFIFO_LOCATION;
    if (!nvRmLocationValid(gpPutLoc))
        return NV_RM_PLAN_BAD_GPPUT_LOCATION;

    NVAmpereRmChannelPlan staged={};
    staged.tsg.engineType=NV_RM_UVM_ENGINE_TYPE_CE;
    staged.tsg.engineIndex=engineIndex;
    staged.channel.numGpFifoEntries=numGpFifoEntries;
    staged.channel.gpFifoLoc=gpFifoLoc;
    staged.channel.gpPutLoc=gpPutLoc;
    *out=staged;
    return NV_RM_PLAN_OK;
}

#endif // NV_AMPERE_RM_PLAN_HPP
