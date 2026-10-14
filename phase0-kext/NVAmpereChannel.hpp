// SPDX-License-Identifier: MIT
// Pure GA107 channel-readiness and one-entry GPFIFO ring contract.
#ifndef NV_AMPERE_CHANNEL_HPP
#define NV_AMPERE_CHANNEL_HPP

#include "NVAmpereSubmit.hpp"

using NVChannelU32 = unsigned int;
using NVChannelU64 = unsigned long long;

// NVIDIA UVM's UvmGpuChannelInfo returns these host mappings when a channel is
// allocated. The work-submission endpoint/token become usable only after the CE
// is allocated/bound to a runlist. This header models PRESENCE, not raw kernel
// pointers, so the identical logic can be host-tested.
struct NVAmpereChannelEvidence {
    NVChannelU32 channelClassNum;
    NVChannelU32 numGpFifoEntries;
    bool gpGetMapped;
    bool gpPutMapped;
    bool gpFifoMapped;
    bool workSubmissionMapped;
    bool workSubmissionTokenReady;
};

struct NVAmpereChannelReadiness {
    bool allocationReady;
    bool ceBoundReady;
    bool submitReady;
};

#define NV_CHANNEL_READY_NOT_ATTEMPTED              0u
#define NV_CHANNEL_READY_OK                         1u
#define NV_CHANNEL_READY_NULL_OUTPUT                2u
#define NV_CHANNEL_READY_BAD_CLASS                  3u
#define NV_CHANNEL_READY_TOO_FEW_ENTRIES            4u
#define NV_CHANNEL_READY_NO_GPGET                   5u
#define NV_CHANNEL_READY_NO_GPPUT                   6u
#define NV_CHANNEL_READY_NO_GPFIFO                  7u
#define NV_CHANNEL_READY_NO_WORK_SUBMISSION         8u
#define NV_CHANNEL_READY_NO_WORK_TOKEN              9u
#define NV_CHANNEL_READY_LIMIT                      10u

static inline bool nvChannelReadySucceeded(NVChannelU32 verdict)
{
    return verdict==NV_CHANNEL_READY_OK;
}

static inline const char *nvChannelReadyVerdictName(NVChannelU32 verdict)
{
    switch (verdict) {
        case NV_CHANNEL_READY_NOT_ATTEMPTED:      return "not-attempted";
        case NV_CHANNEL_READY_OK:                 return "ok";
        case NV_CHANNEL_READY_NULL_OUTPUT:        return "null-output";
        case NV_CHANNEL_READY_BAD_CLASS:          return "bad-class";
        case NV_CHANNEL_READY_TOO_FEW_ENTRIES:    return "too-few-entries";
        case NV_CHANNEL_READY_NO_GPGET:           return "no-gpget";
        case NV_CHANNEL_READY_NO_GPPUT:           return "no-gpput";
        case NV_CHANNEL_READY_NO_GPFIFO:          return "no-gpfifo";
        case NV_CHANNEL_READY_NO_WORK_SUBMISSION: return "no-work-submission";
        case NV_CHANNEL_READY_NO_WORK_TOKEN:      return "no-work-token";
        default:                                  return "unknown";
    }
}

// Evaluate the minimum handoff NVIDIA UVM receives from RM before direct host
// submission is legal. numGpFifoEntries must leave room for UVM's permanent
// sentinel entry, so fewer than two entries is never a usable ring.
//
// The token is represented by a validity boolean intentionally: a token VALUE
// of zero is not treated as invalid unless the provider says it is unavailable.
static inline NVChannelU32 nvAmpereEvaluateChannel(
    const NVAmpereChannelEvidence &e,
    NVAmpereChannelReadiness *out)
{
    if (!out) return NV_CHANNEL_READY_NULL_OUTPUT;

    NVAmpereChannelReadiness staged={false,false,false};
    if (e.channelClassNum!=NV_SUBMIT_C56F_CLASS_ID) {
        *out=staged;
        return NV_CHANNEL_READY_BAD_CLASS;
    }
    if (e.numGpFifoEntries<2u) {
        *out=staged;
        return NV_CHANNEL_READY_TOO_FEW_ENTRIES;
    }
    if (!e.gpGetMapped) {
        *out=staged;
        return NV_CHANNEL_READY_NO_GPGET;
    }
    if (!e.gpPutMapped) {
        *out=staged;
        return NV_CHANNEL_READY_NO_GPPUT;
    }
    if (!e.gpFifoMapped) {
        *out=staged;
        return NV_CHANNEL_READY_NO_GPFIFO;
    }

    staged.allocationReady=true;
    if (!e.workSubmissionMapped) {
        *out=staged;
        return NV_CHANNEL_READY_NO_WORK_SUBMISSION;
    }
    if (!e.workSubmissionTokenReady) {
        *out=staged;
        return NV_CHANNEL_READY_NO_WORK_TOKEN;
    }

    staged.ceBoundReady=true;
    staged.submitReady=true;
    *out=staged;
    return NV_CHANNEL_READY_OK;
}

#define NV_CHANNEL_RING_NOT_ATTEMPTED               0u
#define NV_CHANNEL_RING_OK                          1u
#define NV_CHANNEL_RING_NULL_OUTPUT                 2u
#define NV_CHANNEL_RING_TOO_FEW_ENTRIES             3u
#define NV_CHANNEL_RING_BAD_CPU_PUT                 4u
#define NV_CHANNEL_RING_BAD_GPU_GET                 5u
#define NV_CHANNEL_RING_FULL                        6u
#define NV_CHANNEL_RING_LIMIT                       7u

struct NVAmpereRingWritePlan {
    NVChannelU32 slot;
    NVChannelU32 newGpPut;
    NVChannelU64 entry;
};

static inline bool nvChannelRingSucceeded(NVChannelU32 verdict)
{
    return verdict==NV_CHANNEL_RING_OK;
}

static inline const char *nvChannelRingVerdictName(NVChannelU32 verdict)
{
    switch (verdict) {
        case NV_CHANNEL_RING_NOT_ATTEMPTED:   return "not-attempted";
        case NV_CHANNEL_RING_OK:              return "ok";
        case NV_CHANNEL_RING_NULL_OUTPUT:     return "null-output";
        case NV_CHANNEL_RING_TOO_FEW_ENTRIES: return "too-few-entries";
        case NV_CHANNEL_RING_BAD_CPU_PUT:     return "bad-cpu-put";
        case NV_CHANNEL_RING_BAD_GPU_GET:     return "bad-gpu-get";
        case NV_CHANNEL_RING_FULL:            return "full";
        default:                              return "unknown";
    }
}

// Plan exactly one normal GPFIFO enqueue with the same one-sentinel ring model
// UVM uses. The caller writes `entry` into `slot`, orders that store, publishes
// `newGpPut`, orders again, and only then performs the provider-specific work
// submission kick. This function deliberately performs NONE of those writes.
static inline NVChannelU32 nvAmperePlanOneGpfifoWrite(NVChannelU32 numEntries,
                                                      NVChannelU32 cpuPut,
                                                      NVChannelU32 gpuGet,
                                                      NVChannelU64 entry,
                                                      NVAmpereRingWritePlan *out)
{
    if (!out) return NV_CHANNEL_RING_NULL_OUTPUT;
    if (numEntries<2u) return NV_CHANNEL_RING_TOO_FEW_ENTRIES;
    if (cpuPut>=numEntries) return NV_CHANNEL_RING_BAD_CPU_PUT;
    if (gpuGet>=numEntries) return NV_CHANNEL_RING_BAD_GPU_GET;

    const NVChannelU32 newPut=(cpuPut+1u==numEntries)?0u:(cpuPut+1u);
    if (newPut==gpuGet) return NV_CHANNEL_RING_FULL;

    NVAmpereRingWritePlan staged={};
    staged.slot=cpuPut;
    staged.newGpPut=newPut;
    staged.entry=entry;
    *out=staged;
    return NV_CHANNEL_RING_OK;
}

#endif // NV_AMPERE_CHANNEL_HPP
