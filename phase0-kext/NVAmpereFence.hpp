// SPDX-License-Identifier: MIT
// Pure GA107/C7B5 completion-fence rehearsal. No channel writes/submission.
#ifndef NV_AMPERE_FENCE_HPP
#define NV_AMPERE_FENCE_HPP

#include "NVAmpereSubmit.hpp"

using NVFenceU32 = unsigned int;
using NVFenceU64 = unsigned long long;

// NVIDIA/open-gpu-kernel-modules, clc7b5.h.
#define NV_FENCE_C7B5_SET_SEMAPHORE_A             0x00000240u
#define NV_FENCE_C7B5_SET_SEMAPHORE_B             0x00000244u
#define NV_FENCE_C7B5_SET_SEMAPHORE_PAYLOAD       0x00000248u
#define NV_FENCE_C7B5_LAUNCH_DMA                  0x00000300u
#define NV_FENCE_C7B5_ADDR_UPPER_MASK_17          0x0001ffffu
#define NV_FENCE_C7B5_VA_LIMIT_49                 0x0001ffffffffffffull

// Source-backed channel-tracking release policy:
// - uvm_channel_tracking_semaphore_release() forces a SYS membar.
// - C7B5 CE semaphore release uses one-word release.
// - Ampere C7B5 plc_mode() sets DISABLE_PLC=TRUE.
// LAUNCH_DMA bits:
//   FLUSH_ENABLE=TRUE       bit 2 -> 0x00000004
//   SEMAPHORE_TYPE=release  value 1 in bits 4:3 -> 0x00000008
//   DISABLE_PLC=TRUE        bit 26 -> 0x04000000
#define NV_FENCE_C7B5_LAUNCH_SYS_FLUSH             0x00000004u
#define NV_FENCE_C7B5_LAUNCH_RELEASE_ONE_WORD      0x00000008u
#define NV_FENCE_C7B5_LAUNCH_DISABLE_PLC           0x04000000u
#define NV_FENCE_C7B5_LAUNCH_TRACKING_RELEASE \
    (NV_FENCE_C7B5_LAUNCH_SYS_FLUSH | \
     NV_FENCE_C7B5_LAUNCH_RELEASE_ONE_WORD | \
     NV_FENCE_C7B5_LAUNCH_DISABLE_PLC)

#define NV_FENCE_TRACKING_PUSH_DWORDS              6u
#define NV_FENCE_TRACKING_PUSH_BYTES               (NV_FENCE_TRACKING_PUSH_DWORDS * 4u)

#define NV_FENCE_PLAN_NOT_ATTEMPTED                0u
#define NV_FENCE_PLAN_OK                           1u
#define NV_FENCE_PLAN_NULL_OUTPUT                  2u
#define NV_FENCE_PLAN_ZERO_ADDRESS                 3u
#define NV_FENCE_PLAN_ADDRESS_UNALIGNED            4u
#define NV_FENCE_PLAN_ADDRESS_RANGE                5u
#define NV_FENCE_PLAN_PACKET                       6u
#define NV_FENCE_PLAN_ZERO_MAX_JUMP                7u
#define NV_FENCE_PLAN_COUNTER_OVERFLOW             8u
#define NV_FENCE_PLAN_COUNTER_JUMP                 9u
#define NV_FENCE_PLAN_LIMIT                        10u

struct NVAmpereFencePlan {
    NVFenceU32 push[NV_FENCE_TRACKING_PUSH_DWORDS];
    NVFenceU32 pushDwords;
    NVFenceU32 pushBytes;
    NVFenceU64 fenceGpuVa;
    NVFenceU32 payload;
};

static inline bool nvFencePlanSucceeded(NVFenceU32 verdict)
{
    return verdict==NV_FENCE_PLAN_OK;
}

static inline const char *nvFencePlanVerdictName(NVFenceU32 verdict)
{
    switch (verdict) {
        case NV_FENCE_PLAN_NOT_ATTEMPTED:     return "not-attempted";
        case NV_FENCE_PLAN_OK:                return "ok";
        case NV_FENCE_PLAN_NULL_OUTPUT:       return "null-output";
        case NV_FENCE_PLAN_ZERO_ADDRESS:      return "zero-address";
        case NV_FENCE_PLAN_ADDRESS_UNALIGNED: return "address-unaligned";
        case NV_FENCE_PLAN_ADDRESS_RANGE:     return "address-range";
        case NV_FENCE_PLAN_PACKET:            return "packet";
        case NV_FENCE_PLAN_ZERO_MAX_JUMP:     return "zero-max-jump";
        case NV_FENCE_PLAN_COUNTER_OVERFLOW:  return "counter-overflow";
        case NV_FENCE_PLAN_COUNTER_JUMP:      return "counter-jump";
        default:                              return "unknown";
    }
}

// Build the C7B5 tracking-semaphore release packet UVM uses to retire channel
// pushes. The fence address is a future GA107 GMMU VA, not the peer-DMA IOVA.
// Payload zero is valid because the 32-bit semaphore naturally wraps.
static inline NVFenceU32 nvAmperePlanTrackingFence(NVFenceU64 fenceGpuVa,
                                                   NVFenceU32 payload,
                                                   NVAmpereFencePlan *out)
{
    if (!out) return NV_FENCE_PLAN_NULL_OUTPUT;
    if (!fenceGpuVa) return NV_FENCE_PLAN_ZERO_ADDRESS;
    if ((fenceGpuVa & 3ull)!=0ull) return NV_FENCE_PLAN_ADDRESS_UNALIGNED;
    if (fenceGpuVa>NV_FENCE_C7B5_VA_LIMIT_49 ||
        3ull>NV_FENCE_C7B5_VA_LIMIT_49-fenceGpuVa)
        return NV_FENCE_PLAN_ADDRESS_RANGE;

    NVAmpereFencePlan staged={};
    NVFenceU32 h=0u;
    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,
                          NV_FENCE_C7B5_SET_SEMAPHORE_A,3u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_FENCE_PLAN_PACKET;

    staged.push[0]=h;
    staged.push[1]=(NVFenceU32)((fenceGpuVa>>32)&NV_FENCE_C7B5_ADDR_UPPER_MASK_17);
    staged.push[2]=(NVFenceU32)(fenceGpuVa&0xffffffffull);
    staged.push[3]=payload;

    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,
                          NV_FENCE_C7B5_LAUNCH_DMA,1u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_FENCE_PLAN_PACKET;
    staged.push[4]=h;
    staged.push[5]=NV_FENCE_C7B5_LAUNCH_TRACKING_RELEASE;
    staged.pushDwords=NV_FENCE_TRACKING_PUSH_DWORDS;
    staged.pushBytes=NV_FENCE_TRACKING_PUSH_BYTES;
    staged.fenceGpuVa=fenceGpuVa;
    staged.payload=payload;
    *out=staged;
    return NV_FENCE_PLAN_OK;
}

// Reconstruct UVM's monotonically increasing 64-bit completion value from the
// 32-bit GPU semaphore payload. NVIDIA replaces the low 32 bits and, when the
// new payload is numerically below the prior low word, advances the high word
// by one. The caller supplies the maximum credible jump for its queue depth.
static inline NVFenceU32 nvAmpereFenceExtendCompleted(NVFenceU64 oldValue,
                                                      NVFenceU32 newPayload,
                                                      NVFenceU64 maxJump,
                                                      NVFenceU64 *outValue)
{
    if (!outValue) return NV_FENCE_PLAN_NULL_OUTPUT;
    if (!maxJump) return NV_FENCE_PLAN_ZERO_MAX_JUMP;

    const NVFenceU32 oldPayload=(NVFenceU32)(oldValue&0xffffffffull);
    NVFenceU64 candidate=(oldValue&0xffffffff00000000ull)|(NVFenceU64)newPayload;
    if (newPayload<oldPayload) {
        if ((oldValue&0xffffffff00000000ull)==0xffffffff00000000ull)
            return NV_FENCE_PLAN_COUNTER_OVERFLOW;
        candidate+=0x0000000100000000ull;
    }
    if (candidate<oldValue) return NV_FENCE_PLAN_COUNTER_OVERFLOW;
    if (candidate-oldValue>maxJump) return NV_FENCE_PLAN_COUNTER_JUMP;

    *outValue=candidate;
    return NV_FENCE_PLAN_OK;
}

#endif // NV_AMPERE_FENCE_HPP
