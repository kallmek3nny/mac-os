// SPDX-License-Identifier: MIT
// Kernel-private GA107 GSP queue preparation handoff, with no hardware writes.
// This ABI-bearing structure must never be returned via IORegistry/userclient.
#ifndef NV_GSP_QUEUE_PRIVATE_HANDOFF_HPP
#define NV_GSP_QUEUE_PRIVATE_HANDOFF_HPP

#include "NVGspQueueMemory.hpp"

using NVGspPrivateU32 = unsigned int;

#define NV_GSP_QUEUE_PRIVATE_OK              0u
#define NV_GSP_QUEUE_PRIVATE_NULL            1u
#define NV_GSP_QUEUE_PRIVATE_NOT_OWNED       2u
#define NV_GSP_QUEUE_PRIVATE_NOT_SYNCED      3u
#define NV_GSP_QUEUE_PRIVATE_ALREADY_EXPOSED 4u
#define NV_GSP_QUEUE_PRIVATE_BAD_LAYOUT      5u
#define NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS   6u
#define NV_GSP_QUEUE_PRIVATE_LIMIT           7u

static inline const char *nvGspPrivateQueueVerdictName(NVGspPrivateU32 v)
{
    switch (v) {
    case NV_GSP_QUEUE_PRIVATE_OK:              return "ok";
    case NV_GSP_QUEUE_PRIVATE_NULL:            return "null";
    case NV_GSP_QUEUE_PRIVATE_NOT_OWNED:       return "not-owned";
    case NV_GSP_QUEUE_PRIVATE_NOT_SYNCED:      return "not-synced";
    case NV_GSP_QUEUE_PRIVATE_ALREADY_EXPOSED: return "already-exposed";
    case NV_GSP_QUEUE_PRIVATE_BAD_LAYOUT:      return "bad-layout";
    case NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS:   return "bad-arguments";
    default:                                   return "unknown";
    }
}

// Only the exact normal, non-confidential RM queue-0 layout used by the GA107
// allocation-only provider is accepted. This is a private COPY, not a lease:
// the caller must never authorize hardware use or resource retirement from it.
static inline NVGspPrivateU32 nvGspCopyPrivateQueueHandoff(
    const NVGspQueuePlan *layout,
    const NVGspQueueInitArguments *args,
    bool backingRetained,
    bool dmaBound,
    bool dmaSynchronized,
    bool gspNotified,
    NVGspQueueInitArguments *out)
{
    if (!layout || !args || !out)
        return NV_GSP_QUEUE_PRIVATE_NULL;
    if (!backingRetained || !dmaBound)
        return NV_GSP_QUEUE_PRIVATE_NOT_OWNED;
    if (!dmaSynchronized)
        return NV_GSP_QUEUE_PRIVATE_NOT_SYNCED;
    if (gspNotified)
        return NV_GSP_QUEUE_PRIVATE_ALREADY_EXPOSED;

    NVGspQueuePlan expected={};
    if (nvGspBuildQueuePlan(true,false,0u,&expected)!=NV_GSP_QUEUE_PLAN_OK ||
        layout->sharedBytes!=expected.sharedBytes ||
        layout->pageTableBytes!=expected.pageTableBytes ||
        layout->pageTableEntryCount!=expected.pageTableEntryCount ||
        layout->commandQueueBytes!=expected.commandQueueBytes ||
        layout->statusQueueBytes!=expected.statusQueueBytes ||
        layout->commandQueueOffset!=expected.commandQueueOffset ||
        layout->statusQueueOffset!=expected.statusQueueOffset ||
        layout->queueElementHeaderBytes!=expected.queueElementHeaderBytes ||
        layout->queueElementMinBytes!=expected.queueElementMinBytes ||
        layout->queueElementMaxBytes!=expected.queueElementMaxBytes ||
        layout->queueHeaderAlignBytes!=expected.queueHeaderAlignBytes ||
        layout->queueElementAlignShift!=expected.queueElementAlignShift)
        return NV_GSP_QUEUE_PRIVATE_BAD_LAYOUT;

    if (!args->sharedMemPhysAddr ||
        (args->sharedMemPhysAddr & (NV_GSP_QUEUE_PAGE_BYTES-1u))!=0ull ||
        args->flags!=0ull ||
        args->cmdQueueOffset!=expected.commandQueueOffset ||
        args->statQueueOffset!=expected.statusQueueOffset ||
        args->queueElementHdrSize!=expected.queueElementHeaderBytes ||
        args->queueElementSizeMin!=expected.queueElementMinBytes ||
        args->queueElementSizeMax!=expected.queueElementMaxBytes ||
        args->queueHeaderAlign!=expected.queueHeaderAlignBytes ||
        args->queueElementAlign!=expected.queueElementAlignShift ||
        args->pageTableEntryCount!=expected.pageTableEntryCount)
        return NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS;

    for (unsigned i=0u;i<sizeof(args->reserved);++i) {
        if (args->reserved[i]!=0u)
            return NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS;
    }

    *out=*args;
    return NV_GSP_QUEUE_PRIVATE_OK;
}

#endif // NV_GSP_QUEUE_PRIVATE_HANDOFF_HPP
