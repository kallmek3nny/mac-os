// SPDX-License-Identifier: MIT
// Pure NVIDIA GSP-RM shared message-queue geometry. No MMIO or queue writes.
#ifndef NV_GSP_QUEUE_PLAN_HPP
#define NV_GSP_QUEUE_PLAN_HPP

using NVGspQueueU32 = unsigned int;
using NVGspQueueU64 = unsigned long long;

// NVIDIA open RM, message_queue_cpu.c / message_queue_priv.h / rm_page_size.h.
#define NV_GSP_QUEUE_PAGE_BYTES                     4096u
#define NV_GSP_QUEUE_PAGE_SHIFT                     12u
#define NV_GSP_QUEUE_PTE_BYTES                      8u
#define NV_GSP_QUEUE_COMMAND_SILICON_BYTES          0x00040000u
#define NV_GSP_QUEUE_COMMAND_PRESILICON_MULTIPLIER  6u
#define NV_GSP_QUEUE_STATUS_DEFAULT_BYTES           0x00040000u
#define NV_GSP_QUEUE_ELEMENT_MIN_BYTES              4096u
#define NV_GSP_QUEUE_ELEMENT_MAX_BYTES              65536u
#define NV_GSP_QUEUE_HEADER_ALIGN_BYTES             4u
#define NV_GSP_QUEUE_ELEMENT_ALIGN_SHIFT            12u

// Production (non-HWASAN) GSP_MSG_QUEUE_ELEMENT framing:
// MCTP 16 bytes + NVDM payload-size/reserved 8 bytes = 24 bytes without CC.
// Confidential-compute framing inserts one 16-byte authentication tag.
#define NV_GSP_QUEUE_ELEMENT_HEADER_NO_CC_BYTES     24u
#define NV_GSP_QUEUE_ELEMENT_HEADER_CC_BYTES        40u

#define NV_GSP_QUEUE_PLAN_NOT_ATTEMPTED             0u
#define NV_GSP_QUEUE_PLAN_OK                        1u
#define NV_GSP_QUEUE_PLAN_NULL_OUTPUT               2u
#define NV_GSP_QUEUE_PLAN_BAD_STATUS_SIZE           3u
#define NV_GSP_QUEUE_PLAN_OVERFLOW                  4u
#define NV_GSP_QUEUE_PLAN_LIMIT                     5u

struct NVGspQueuePlan {
    NVGspQueueU32 commandQueueBytes;
    NVGspQueueU32 statusQueueBytes;
    NVGspQueueU32 queueElementHeaderBytes;
    NVGspQueueU32 queueElementMinBytes;
    NVGspQueueU32 queueElementMaxBytes;
    NVGspQueueU32 queueHeaderAlignBytes;
    NVGspQueueU32 queueElementAlignShift;
    NVGspQueueU32 queuePageCount;
    NVGspQueueU32 pageTableEntryCount;
    NVGspQueueU32 pageTableBytes;
    NVGspQueueU32 commandQueueOffset;
    NVGspQueueU32 statusQueueOffset;
    NVGspQueueU32 sharedBytes;
    NVGspQueueU32 maxRpcBytes;
};

static inline bool nvGspQueuePlanSucceeded(NVGspQueueU32 verdict)
{
    return verdict==NV_GSP_QUEUE_PLAN_OK;
}

static inline const char *nvGspQueuePlanVerdictName(NVGspQueueU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_PLAN_NOT_ATTEMPTED:   return "not-attempted";
        case NV_GSP_QUEUE_PLAN_OK:              return "ok";
        case NV_GSP_QUEUE_PLAN_NULL_OUTPUT:     return "null-output";
        case NV_GSP_QUEUE_PLAN_BAD_STATUS_SIZE: return "bad-status-size";
        case NV_GSP_QUEUE_PLAN_OVERFLOW:        return "overflow";
        default:                                return "unknown";
    }
}

static inline NVGspQueueU64 nvGspQueueAlignUp(NVGspQueueU64 value,
                                              NVGspQueueU64 alignment)
{
    if (!alignment) return 0ull;
    const NVGspQueueU64 mask=alignment-1ull;
    if ((alignment&mask)!=0ull || value>~0ull-mask) return 0ull;
    return (value+mask)&~mask;
}

// Build the production shared-memory layout used by the RM command/status
// queues. statusQueueBytes=0 selects NVIDIA's 256 KiB default. A nonzero
// override follows the source policy: at least one max-size element (64 KiB)
// and page-aligned. Callers should pass the registry value after KiB->bytes
// conversion; this helper does not invent that policy boundary.
static inline NVGspQueueU32 nvGspBuildQueuePlan(bool silicon,
                                                bool confidentialCompute,
                                                NVGspQueueU32 statusQueueBytes,
                                                NVGspQueuePlan *out)
{
    if (!out) return NV_GSP_QUEUE_PLAN_NULL_OUTPUT;

    const NVGspQueueU64 commandBytes=silicon
        ? (NVGspQueueU64)NV_GSP_QUEUE_COMMAND_SILICON_BYTES
        : (NVGspQueueU64)NV_GSP_QUEUE_COMMAND_SILICON_BYTES *
          (NVGspQueueU64)NV_GSP_QUEUE_COMMAND_PRESILICON_MULTIPLIER;

    NVGspQueueU64 statusBytes=statusQueueBytes
        ? (NVGspQueueU64)statusQueueBytes
        : (NVGspQueueU64)NV_GSP_QUEUE_STATUS_DEFAULT_BYTES;
    if (statusQueueBytes) {
        if (statusBytes<NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
            statusBytes=NV_GSP_QUEUE_ELEMENT_MAX_BYTES;
        statusBytes=nvGspQueueAlignUp(statusBytes,NV_GSP_QUEUE_PAGE_BYTES);
        if (!statusBytes) return NV_GSP_QUEUE_PLAN_BAD_STATUS_SIZE;
    }
    if ((statusBytes&(NV_GSP_QUEUE_PAGE_BYTES-1u))!=0u)
        return NV_GSP_QUEUE_PLAN_BAD_STATUS_SIZE;

    const NVGspQueueU64 queueBytes=commandBytes+statusBytes;
    if (queueBytes<commandBytes ||
        (queueBytes&(NV_GSP_QUEUE_PAGE_BYTES-1u))!=0u)
        return NV_GSP_QUEUE_PLAN_OVERFLOW;

    const NVGspQueueU64 queuePages=queueBytes>>NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspQueueU64 pteStorageBytes=queuePages*NV_GSP_QUEUE_PTE_BYTES;
    if (queuePages && pteStorageBytes/queuePages!=NV_GSP_QUEUE_PTE_BYTES)
        return NV_GSP_QUEUE_PLAN_OVERFLOW;
    const NVGspQueueU64 ptePages=(pteStorageBytes+NV_GSP_QUEUE_PAGE_BYTES-1u)>>NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspQueueU64 pageTableEntryCount=queuePages+ptePages;
    const NVGspQueueU64 pageTableRawBytes=pageTableEntryCount*NV_GSP_QUEUE_PTE_BYTES;
    if (pageTableEntryCount && pageTableRawBytes/pageTableEntryCount!=NV_GSP_QUEUE_PTE_BYTES)
        return NV_GSP_QUEUE_PLAN_OVERFLOW;
    const NVGspQueueU64 pageTableBytes=nvGspQueueAlignUp(pageTableRawBytes,NV_GSP_QUEUE_PAGE_BYTES);
    if (!pageTableBytes) return NV_GSP_QUEUE_PLAN_OVERFLOW;

    const NVGspQueueU64 commandOffset=pageTableBytes;
    const NVGspQueueU64 statusOffset=commandOffset+commandBytes;
    const NVGspQueueU64 sharedBytes=statusOffset+statusBytes;
    if (statusOffset<commandOffset || sharedBytes<statusOffset ||
        sharedBytes>0xffffffffull || pageTableEntryCount>0xffffffffull ||
        queuePages>0xffffffffull)
        return NV_GSP_QUEUE_PLAN_OVERFLOW;

    const NVGspQueueU32 headerBytes=confidentialCompute
        ? NV_GSP_QUEUE_ELEMENT_HEADER_CC_BYTES
        : NV_GSP_QUEUE_ELEMENT_HEADER_NO_CC_BYTES;
    if (headerBytes>=NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
        return NV_GSP_QUEUE_PLAN_OVERFLOW;

    NVGspQueuePlan staged={};
    staged.commandQueueBytes=(NVGspQueueU32)commandBytes;
    staged.statusQueueBytes=(NVGspQueueU32)statusBytes;
    staged.queueElementHeaderBytes=headerBytes;
    staged.queueElementMinBytes=NV_GSP_QUEUE_ELEMENT_MIN_BYTES;
    staged.queueElementMaxBytes=NV_GSP_QUEUE_ELEMENT_MAX_BYTES;
    staged.queueHeaderAlignBytes=NV_GSP_QUEUE_HEADER_ALIGN_BYTES;
    staged.queueElementAlignShift=NV_GSP_QUEUE_ELEMENT_ALIGN_SHIFT;
    staged.queuePageCount=(NVGspQueueU32)queuePages;
    staged.pageTableEntryCount=(NVGspQueueU32)pageTableEntryCount;
    staged.pageTableBytes=(NVGspQueueU32)pageTableBytes;
    staged.commandQueueOffset=(NVGspQueueU32)commandOffset;
    staged.statusQueueOffset=(NVGspQueueU32)statusOffset;
    staged.sharedBytes=(NVGspQueueU32)sharedBytes;
    staged.maxRpcBytes=NV_GSP_QUEUE_ELEMENT_MAX_BYTES-headerBytes;
    *out=staged;
    return NV_GSP_QUEUE_PLAN_OK;
}

#endif // NV_GSP_QUEUE_PLAN_HPP
