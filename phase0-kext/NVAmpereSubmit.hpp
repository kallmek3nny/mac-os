// SPDX-License-Identifier: MIT
// Pure GA107/AMPERE submission-envelope rehearsal. No channel writes/submission.
#ifndef NV_AMPERE_SUBMIT_HPP
#define NV_AMPERE_SUBMIT_HPP

#include "NVAmpereCopy.hpp"

using NVSubmitU32 = unsigned int;
using NVSubmitU64 = unsigned long long;

// NVIDIA/open-gpu-kernel-modules, clc56f.h and g_gpu_class_list.c.
// GA107 advertises AMPERE_CHANNEL_GPFIFO_A and AMPERE_DMA_COPY_B.
#define NV_SUBMIT_C56F_CLASS_ID                    0x0000c56fu
#define NV_SUBMIT_C56F_SET_OBJECT                  0x00000000u
#define NV_SUBMIT_CE_SUBCHANNEL                    4u
#define NV_SUBMIT_HOST_SUBCHANNEL                  0u

// clc56f.h DMA incrementing-method packet:
// ADDRESS 11:0 contains method byte offset >> 2
// SUBCHANNEL 15:13
// COUNT 28:16
// SEC_OP 31:29 = INC_METHOD (1)
#define NV_SUBMIT_METHOD_ADDRESS_MASK              0x00000fffu
#define NV_SUBMIT_METHOD_SUBCHANNEL_MASK           0x00000007u
#define NV_SUBMIT_METHOD_COUNT_MASK                0x00001fffu
#define NV_SUBMIT_METHOD_SEC_OP_INC                0x00000001u
#define NV_SUBMIT_METHOD_SEC_OP_SHIFT              29u
#define NV_SUBMIT_METHOD_COUNT_SHIFT               16u
#define NV_SUBMIT_METHOD_SUBCHANNEL_SHIFT          13u
#define NV_SUBMIT_METHOD_MAX_BYTE_OFFSET           0x00003ffcu

// clc56f.h GPFIFO entry:
// GP_ENTRY0.GET 31:2 stores low pushbuffer VA bits (4-byte aligned)
// GP_ENTRY1.GET_HI 7:0 stores VA bits 39:32
// GP_ENTRY1.LENGTH 30:10 stores pushbuffer length in dwords
// GP_ENTRY1.SYNC 31 is left PROCEED (0) for this rehearsal.
#define NV_SUBMIT_GPFIFO_VA_LIMIT_40               0x000000ffffffffffull
#define NV_SUBMIT_GPFIFO_LENGTH_DWORDS_MAX         0x001fffffu
#define NV_SUBMIT_GPFIFO_LENGTH_SHIFT              10u
#define NV_SUBMIT_GPFIFO_GET_HI_MASK               0x000000ffu

// SET_OBJECT header/value + 4 C7B5 packet groups:
//   2 dwords: C46F/C56F SET_OBJECT = C7B5
//   2 dwords: SET_DST_PHYS_MODE
//   5 dwords: OFFSET_IN/OUT upper/lower contiguous block
//   3 dwords: LINE_LENGTH_IN + LINE_COUNT
//   2 dwords: LAUNCH_DMA
#define NV_SUBMIT_COPY_PUSH_DWORDS                 14u
#define NV_SUBMIT_COPY_PUSH_BYTES                  (NV_SUBMIT_COPY_PUSH_DWORDS * 4u)

#define NV_SUBMIT_PLAN_NOT_ATTEMPTED               0u
#define NV_SUBMIT_PLAN_OK                          1u
#define NV_SUBMIT_PLAN_NULL_OUTPUT                 2u
#define NV_SUBMIT_PLAN_COPY_REFUSED                3u
#define NV_SUBMIT_PLAN_COPY_LAYOUT                 4u
#define NV_SUBMIT_PLAN_BAD_METHOD                  5u
#define NV_SUBMIT_PLAN_BAD_SUBCHANNEL              6u
#define NV_SUBMIT_PLAN_BAD_COUNT                   7u
#define NV_SUBMIT_PLAN_ZERO_PUSH_VA                8u
#define NV_SUBMIT_PLAN_PUSH_VA_UNALIGNED           9u
#define NV_SUBMIT_PLAN_PUSH_VA_RANGE               10u
#define NV_SUBMIT_PLAN_PUSH_LENGTH                 11u
#define NV_SUBMIT_PLAN_LIMIT                       12u

struct NVAmpereSubmissionPlan {
    NVSubmitU32 push[NV_SUBMIT_COPY_PUSH_DWORDS];
    NVSubmitU32 pushDwords;
    NVSubmitU32 pushBytes;
    NVSubmitU64 pushGpuVa;
    NVSubmitU64 gpfifoEntry;
};

static inline bool nvSubmitPlanSucceeded(NVSubmitU32 verdict)
{
    return verdict==NV_SUBMIT_PLAN_OK;
}

static inline const char *nvSubmitPlanVerdictName(NVSubmitU32 verdict)
{
    switch (verdict) {
        case NV_SUBMIT_PLAN_NOT_ATTEMPTED:     return "not-attempted";
        case NV_SUBMIT_PLAN_OK:                return "ok";
        case NV_SUBMIT_PLAN_NULL_OUTPUT:       return "null-output";
        case NV_SUBMIT_PLAN_COPY_REFUSED:      return "copy-refused";
        case NV_SUBMIT_PLAN_COPY_LAYOUT:       return "copy-layout";
        case NV_SUBMIT_PLAN_BAD_METHOD:        return "bad-method";
        case NV_SUBMIT_PLAN_BAD_SUBCHANNEL:    return "bad-subchannel";
        case NV_SUBMIT_PLAN_BAD_COUNT:         return "bad-count";
        case NV_SUBMIT_PLAN_ZERO_PUSH_VA:      return "zero-push-va";
        case NV_SUBMIT_PLAN_PUSH_VA_UNALIGNED: return "push-va-unaligned";
        case NV_SUBMIT_PLAN_PUSH_VA_RANGE:     return "push-va-range";
        case NV_SUBMIT_PLAN_PUSH_LENGTH:       return "push-length";
        default:                               return "unknown";
    }
}

static inline NVSubmitU32 nvSubmitIncHeader(NVSubmitU32 subchannel,
                                            NVSubmitU32 method,
                                            NVSubmitU32 count,
                                            NVSubmitU32 *outHeader)
{
    if (!outHeader) return NV_SUBMIT_PLAN_NULL_OUTPUT;
    if ((method & 3u)!=0u || method>NV_SUBMIT_METHOD_MAX_BYTE_OFFSET)
        return NV_SUBMIT_PLAN_BAD_METHOD;
    if (subchannel>NV_SUBMIT_METHOD_SUBCHANNEL_MASK)
        return NV_SUBMIT_PLAN_BAD_SUBCHANNEL;
    if (!count || count>NV_SUBMIT_METHOD_COUNT_MASK)
        return NV_SUBMIT_PLAN_BAD_COUNT;

    *outHeader=(NV_SUBMIT_METHOD_SEC_OP_INC<<NV_SUBMIT_METHOD_SEC_OP_SHIFT) |
               (count<<NV_SUBMIT_METHOD_COUNT_SHIFT) |
               (subchannel<<NV_SUBMIT_METHOD_SUBCHANNEL_SHIFT) |
               ((method>>2)&NV_SUBMIT_METHOD_ADDRESS_MASK);
    return NV_SUBMIT_PLAN_OK;
}

static inline NVSubmitU32 nvSubmitGpfifoEntry(NVSubmitU64 pushGpuVa,
                                              NVSubmitU32 pushBytes,
                                              NVSubmitU64 *outEntry)
{
    if (!outEntry) return NV_SUBMIT_PLAN_NULL_OUTPUT;
    if (!pushGpuVa) return NV_SUBMIT_PLAN_ZERO_PUSH_VA;
    if ((pushGpuVa & 3ull)!=0ull) return NV_SUBMIT_PLAN_PUSH_VA_UNALIGNED;
    if (!pushBytes || (pushBytes & 3u)!=0u ||
        (pushBytes>>2)>NV_SUBMIT_GPFIFO_LENGTH_DWORDS_MAX)
        return NV_SUBMIT_PLAN_PUSH_LENGTH;
    if (pushGpuVa>NV_SUBMIT_GPFIFO_VA_LIMIT_40 ||
        (NVSubmitU64)pushBytes-1ull>NV_SUBMIT_GPFIFO_VA_LIMIT_40-pushGpuVa)
        return NV_SUBMIT_PLAN_PUSH_VA_RANGE;

    const NVSubmitU32 entry0=(NVSubmitU32)(pushGpuVa & 0xfffffffcu);
    const NVSubmitU32 getHi=(NVSubmitU32)((pushGpuVa>>32)&NV_SUBMIT_GPFIFO_GET_HI_MASK);
    const NVSubmitU32 entry1=getHi | ((pushBytes>>2)<<NV_SUBMIT_GPFIFO_LENGTH_SHIFT);
    *outEntry=(NVSubmitU64)entry0 | ((NVSubmitU64)entry1<<32);
    return NV_SUBMIT_PLAN_OK;
}

// Compose a complete but intentionally NON-SUBMITTED GA107 copy rehearsal:
// 1) bind C7B5 with the host SET_OBJECT method,
// 2) emit the C7B5 method stream on fixed CE subchannel 4,
// 3) describe that pushbuffer with one C56F GPFIFO entry.
//
// This helper has no IO/MMIO dependency and cannot update gpPut or a work-
// submission token. All output is staged locally and committed atomically only
// after both copy-method and GPFIFO validation succeed.
static inline NVSubmitU32 nvAmperePlanCopySubmission(NVSubmitU64 srcGpuVa,
                                                     NVSubmitU64 dstPeerDma,
                                                     NVSubmitU32 bytes,
                                                     NVSubmitU64 pushGpuVa,
                                                     NVAmpereSubmissionPlan *out)
{
    if (!out) return NV_SUBMIT_PLAN_NULL_OUTPUT;

    NVCopyMethod copy[NV_COPY_C7B5_METHOD_COUNT];
    NVCopyU32 copyCount=0u;
    const NVCopyU32 copyVerdict=nvCopyPlanToCoherentSink(
        srcGpuVa,dstPeerDma,bytes,copy,NV_COPY_C7B5_METHOD_COUNT,&copyCount);
    if (!nvCopyPlanSucceeded(copyVerdict)) return NV_SUBMIT_PLAN_COPY_REFUSED;
    if (copyCount!=NV_COPY_C7B5_METHOD_COUNT ||
        copy[0].method!=NV_COPY_C7B5_SET_DST_PHYS_MODE ||
        copy[1].method!=NV_COPY_C7B5_OFFSET_IN_UPPER ||
        copy[2].method!=NV_COPY_C7B5_OFFSET_IN_LOWER ||
        copy[3].method!=NV_COPY_C7B5_OFFSET_OUT_UPPER ||
        copy[4].method!=NV_COPY_C7B5_OFFSET_OUT_LOWER ||
        copy[5].method!=NV_COPY_C7B5_LINE_LENGTH_IN ||
        copy[6].method!=NV_COPY_C7B5_LINE_COUNT ||
        copy[7].method!=NV_COPY_C7B5_LAUNCH_DMA)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;

    NVAmpereSubmissionPlan staged={};
    NVSubmitU32 h=0u;
    NVSubmitU32 p=0u;

    if (nvSubmitIncHeader(NV_SUBMIT_HOST_SUBCHANNEL,NV_SUBMIT_C56F_SET_OBJECT,1u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;
    staged.push[p++]=h;
    staged.push[p++]=NV_COPY_C7B5_CLASS_ID;

    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,copy[0].method,1u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;
    staged.push[p++]=h;
    staged.push[p++]=copy[0].value;

    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,copy[1].method,4u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;
    staged.push[p++]=h;
    for (NVSubmitU32 i=1u;i<=4u;++i) staged.push[p++]=copy[i].value;

    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,copy[5].method,2u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;
    staged.push[p++]=h;
    staged.push[p++]=copy[5].value;
    staged.push[p++]=copy[6].value;

    if (nvSubmitIncHeader(NV_SUBMIT_CE_SUBCHANNEL,copy[7].method,1u,&h)!=NV_SUBMIT_PLAN_OK)
        return NV_SUBMIT_PLAN_COPY_LAYOUT;
    staged.push[p++]=h;
    staged.push[p++]=copy[7].value;

    if (p!=NV_SUBMIT_COPY_PUSH_DWORDS) return NV_SUBMIT_PLAN_COPY_LAYOUT;

    NVSubmitU64 entry=0ull;
    const NVSubmitU32 fifoVerdict=nvSubmitGpfifoEntry(
        pushGpuVa,NV_SUBMIT_COPY_PUSH_BYTES,&entry);
    if (fifoVerdict!=NV_SUBMIT_PLAN_OK) return fifoVerdict;

    staged.pushDwords=NV_SUBMIT_COPY_PUSH_DWORDS;
    staged.pushBytes=NV_SUBMIT_COPY_PUSH_BYTES;
    staged.pushGpuVa=pushGpuVa;
    staged.gpfifoEntry=entry;
    *out=staged;
    return NV_SUBMIT_PLAN_OK;
}

#endif // NV_AMPERE_SUBMIT_HPP
