// SPDX-License-Identifier: MIT
// Pure AMPERE_DMA_COPY_B (C7B5) one-line copy rehearsal. No FIFO/submission.
#ifndef NV_AMPERE_COPY_HPP
#define NV_AMPERE_COPY_HPP

#include "IntelSinkPeer.hpp"

using NVCopyU32 = unsigned int;
using NVCopyU64 = unsigned long long;

// NVIDIA/open-gpu-kernel-modules, kernel-open/nvidia-uvm/clc7b5.h.
// g_gpu_class_list.c associates AMPERE_DMA_COPY_B with Ampere-B copy engines.
#define NV_COPY_C7B5_CLASS_ID                         0x0000c7b5u
#define NV_COPY_C7B5_SET_DST_PHYS_MODE               0x00000264u
#define NV_COPY_C7B5_LAUNCH_DMA                      0x00000300u
#define NV_COPY_C7B5_OFFSET_IN_UPPER                 0x00000400u
#define NV_COPY_C7B5_OFFSET_IN_LOWER                 0x00000404u
#define NV_COPY_C7B5_OFFSET_OUT_UPPER                0x00000408u
#define NV_COPY_C7B5_OFFSET_OUT_LOWER                0x0000040cu
#define NV_COPY_C7B5_LINE_LENGTH_IN                  0x00000418u
#define NV_COPY_C7B5_LINE_COUNT                      0x0000041cu

// SET_DST_PHYS_MODE.TARGET = COHERENT_SYSMEM (bits 1:0, value 1).
#define NV_COPY_C7B5_DST_COHERENT_SYSMEM             0x00000001u

// LAUNCH_DMA field positions are source-backed by clc7b5.h. The exact policy
// below is deliberately conservative for the first rehearsal:
//   DATA_TRANSFER_TYPE = NON_PIPELINED (2)
//   FLUSH_ENABLE       = TRUE          (bit 2)
//   SRC_MEMORY_LAYOUT  = PITCH         (bit 7)
//   DST_MEMORY_LAYOUT  = PITCH         (bit 8)
//   MULTI_LINE_ENABLE  = FALSE
//   REMAP_ENABLE       = FALSE
//   SRC_TYPE           = VIRTUAL       (bit 12 clear)
//   DST_TYPE           = PHYSICAL      (bit 13 set)
//   semaphore/interrupt/reduction/VPR  = disabled
// This is a software policy for an unsubmitted rehearsal, not a claim that a
// working channel/runtime already exists.
#define NV_COPY_C7B5_LAUNCH_NONPIPELINED             0x00000002u
#define NV_COPY_C7B5_LAUNCH_FLUSH                    0x00000004u
#define NV_COPY_C7B5_LAUNCH_SRC_PITCH                0x00000080u
#define NV_COPY_C7B5_LAUNCH_DST_PITCH                0x00000100u
#define NV_COPY_C7B5_LAUNCH_DST_PHYSICAL             0x00002000u
#define NV_COPY_C7B5_LAUNCH_REHEARSAL \
    (NV_COPY_C7B5_LAUNCH_NONPIPELINED | NV_COPY_C7B5_LAUNCH_FLUSH | \
     NV_COPY_C7B5_LAUNCH_SRC_PITCH | NV_COPY_C7B5_LAUNCH_DST_PITCH | \
     NV_COPY_C7B5_LAUNCH_DST_PHYSICAL)

#define NV_COPY_C7B5_ADDR_UPPER_MASK_17              0x0001ffffu
#define NV_COPY_C7B5_VA_LIMIT_49                      0x0001ffffffffffffull
#define NV_COPY_C7B5_METHOD_COUNT                     8u

#define NV_COPY_PLAN_NOT_ATTEMPTED                    0u
#define NV_COPY_PLAN_OK                               1u
#define NV_COPY_PLAN_NULL_OUTPUT                      2u
#define NV_COPY_PLAN_CAPACITY                         3u
#define NV_COPY_PLAN_ZERO_LENGTH                      4u
#define NV_COPY_PLAN_ZERO_SOURCE                      5u
#define NV_COPY_PLAN_SOURCE_RANGE                     6u
#define NV_COPY_PLAN_ZERO_DEST                        7u
#define NV_COPY_PLAN_DEST_UNALIGNED                   8u
#define NV_COPY_PLAN_DEST_RANGE                       9u
#define NV_COPY_PLAN_LIMIT                            10u

struct NVCopyMethod {
    NVCopyU32 method;
    NVCopyU32 value;
};

static inline bool nvCopyPlanSucceeded(NVCopyU32 verdict)
{
    return verdict==NV_COPY_PLAN_OK;
}

static inline const char *nvCopyPlanVerdictName(NVCopyU32 verdict)
{
    switch (verdict) {
        case NV_COPY_PLAN_NOT_ATTEMPTED: return "not-attempted";
        case NV_COPY_PLAN_OK:            return "ok";
        case NV_COPY_PLAN_NULL_OUTPUT:   return "null-output";
        case NV_COPY_PLAN_CAPACITY:      return "capacity";
        case NV_COPY_PLAN_ZERO_LENGTH:   return "zero-length";
        case NV_COPY_PLAN_ZERO_SOURCE:   return "zero-source";
        case NV_COPY_PLAN_SOURCE_RANGE:  return "source-range";
        case NV_COPY_PLAN_ZERO_DEST:     return "zero-dest";
        case NV_COPY_PLAN_DEST_UNALIGNED:return "dest-unaligned";
        case NV_COPY_PLAN_DEST_RANGE:    return "dest-range";
        default:                         return "unknown";
    }
}

static inline NVCopyU32 nvCopyUpper17(NVCopyU64 address)
{
    return (NVCopyU32)((address>>32) & NV_COPY_C7B5_ADDR_UPPER_MASK_17);
}

static inline NVCopyU32 nvCopyLower32(NVCopyU64 address)
{
    return (NVCopyU32)(address & 0xffffffffull);
}

// Build method/value pairs only. The caller still needs a validated C7B5 object,
// channel, pushbuffer encoding, synchronization, and submission path. In
// particular, this helper intentionally cannot write a doorbell or MMIO register.
// `srcGpuVa` is a future GA107 GMMU virtual source. `dstPeerDma` is the 3050's
// device-domain IOVA for the Intel-owned coherent system-memory sink.
static inline NVCopyU32 nvCopyPlanToCoherentSink(NVCopyU64 srcGpuVa,
                                                 NVCopyU64 dstPeerDma,
                                                 NVCopyU32 bytes,
                                                 NVCopyMethod *out,
                                                 NVCopyU32 capacity,
                                                 NVCopyU32 *outCount)
{
    if (outCount) *outCount=0u;
    if (!out || !outCount) return NV_COPY_PLAN_NULL_OUTPUT;
    if (capacity<NV_COPY_C7B5_METHOD_COUNT) return NV_COPY_PLAN_CAPACITY;
    if (!bytes) return NV_COPY_PLAN_ZERO_LENGTH;
    if (!srcGpuVa) return NV_COPY_PLAN_ZERO_SOURCE;
    if (srcGpuVa>NV_COPY_C7B5_VA_LIMIT_49 ||
        (NVCopyU64)bytes-1ull>NV_COPY_C7B5_VA_LIMIT_49-srcGpuVa)
        return NV_COPY_PLAN_SOURCE_RANGE;
    if (!dstPeerDma) return NV_COPY_PLAN_ZERO_DEST;
    if ((dstPeerDma & (NVCopyU64)(IG_SINK_PEER_PAGE_BYTES-1u))!=0ull)
        return NV_COPY_PLAN_DEST_UNALIGNED;
    if (dstPeerDma>IG_SINK_PEER_DMA_LIMIT_POLICY ||
        (NVCopyU64)bytes-1ull>IG_SINK_PEER_DMA_LIMIT_POLICY-dstPeerDma)
        return NV_COPY_PLAN_DEST_RANGE;

    NVCopyMethod staged[NV_COPY_C7B5_METHOD_COUNT] = {
        {NV_COPY_C7B5_SET_DST_PHYS_MODE, NV_COPY_C7B5_DST_COHERENT_SYSMEM},
        {NV_COPY_C7B5_OFFSET_IN_UPPER,   nvCopyUpper17(srcGpuVa)},
        {NV_COPY_C7B5_OFFSET_IN_LOWER,   nvCopyLower32(srcGpuVa)},
        {NV_COPY_C7B5_OFFSET_OUT_UPPER,  nvCopyUpper17(dstPeerDma)},
        {NV_COPY_C7B5_OFFSET_OUT_LOWER,  nvCopyLower32(dstPeerDma)},
        {NV_COPY_C7B5_LINE_LENGTH_IN,    bytes},
        {NV_COPY_C7B5_LINE_COUNT,        1u},
        {NV_COPY_C7B5_LAUNCH_DMA,        NV_COPY_C7B5_LAUNCH_REHEARSAL}
    };
    for (NVCopyU32 i=0;i<NV_COPY_C7B5_METHOD_COUNT;++i) out[i]=staged[i];
    *outCount=NV_COPY_C7B5_METHOD_COUNT;
    return NV_COPY_PLAN_OK;
}

#endif // NV_AMPERE_COPY_HPP
