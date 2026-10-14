// SPDX-License-Identifier: MIT
// Pure NVIDIA GSP-RM MSGQ v2 TX-ring planning. No shared-memory or MMIO writes.
#ifndef NV_GSP_QUEUE_TX_HPP
#define NV_GSP_QUEUE_TX_HPP

#include "NVGspRpcFrame.hpp"

using NVGspTxU32 = unsigned int;
using NVGspTxU64 = unsigned long long;

// NVIDIA open RM msgq_priv.h / msgq.c.
#define NV_GSP_MSGQ_VERSION_MAJOR                 2u
#define NV_GSP_MSGQ_VERSION_MINOR                 0u
#define NV_GSP_MSGQ_TX_HEADER_BYTES               32u
#define NV_GSP_MSGQ_TX_HEADER_DWORDS              8u
#define NV_GSP_MSGQ_ENTRY_BYTES                   NV_GSP_QUEUE_ELEMENT_MIN_BYTES
#define NV_GSP_MSGQ_ENTRY_ALIGN_BYTES             (1u << NV_GSP_QUEUE_ELEMENT_ALIGN_SHIFT)
#define NV_GSP_MSGQ_MAX_RECORD_ELEMENTS           (NV_GSP_QUEUE_ELEMENT_MAX_BYTES / NV_GSP_MSGQ_ENTRY_BYTES)

#define NV_GSP_QUEUE_TX_NOT_ATTEMPTED             0u
#define NV_GSP_QUEUE_TX_OK                        1u
#define NV_GSP_QUEUE_TX_NULL_OUTPUT               2u
#define NV_GSP_QUEUE_TX_BAD_GEOMETRY              3u
#define NV_GSP_QUEUE_TX_BAD_RECORD_SIZE           4u
#define NV_GSP_QUEUE_TX_BUSY                      5u
#define NV_GSP_QUEUE_TX_LIMIT                     6u

struct NVGspMsgqTxHeaderPlan {
    // Exact little-endian dwords of msgqTxHeader as initialized by msgqTxCreate.
    NVGspTxU32 dwords[NV_GSP_MSGQ_TX_HEADER_DWORDS];
    NVGspTxU32 entryOffset;
    NVGspTxU32 messageSize;
    NVGspTxU32 messageCount;
};

struct NVGspQueueTxPlan {
    NVGspTxU32 effectiveReadPtr;
    NVGspTxU32 freeBefore;
    NVGspTxU32 elementCount;
    NVGspTxU32 payloadBytes;
    NVGspTxU32 copyBytes;
    NVGspTxU32 paddingBytes;
    NVGspTxU32 slot[NV_GSP_MSGQ_MAX_RECORD_ELEMENTS];
    NVGspTxU32 byteOffset[NV_GSP_MSGQ_MAX_RECORD_ELEMENTS];
    NVGspTxU32 newWritePtr;
    NVGspTxU32 txHeadValue;
};

static inline bool nvGspQueueTxSucceeded(NVGspTxU32 verdict)
{
    return verdict == NV_GSP_QUEUE_TX_OK;
}

static inline const char *nvGspQueueTxVerdictName(NVGspTxU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_TX_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_QUEUE_TX_OK:            return "ok";
        case NV_GSP_QUEUE_TX_NULL_OUTPUT:   return "null-output";
        case NV_GSP_QUEUE_TX_BAD_GEOMETRY: return "bad-geometry";
        case NV_GSP_QUEUE_TX_BAD_RECORD_SIZE:return "bad-record-size";
        case NV_GSP_QUEUE_TX_BUSY:          return "busy";
        default:                            return "unknown";
    }
}

static inline NVGspTxU64 nvGspQueueTxAlignUp(NVGspTxU64 value, NVGspTxU64 alignment)
{
    if (!alignment) return 0ull;
    const NVGspTxU64 mask = alignment - 1ull;
    if ((alignment & mask) != 0ull || value > ~0ull - mask) return 0ull;
    return (value + mask) & ~mask;
}

// Reproduce msgqTxCreate() for the production GSP command queue. The MSGQ v2
// header is 32 bytes, but entries are 4 KiB aligned, so normal silicon's
// 256 KiB command queue contains 63 usable 4 KiB slots after the first page.
static inline NVGspTxU32 nvGspBuildMsgqTxHeader(NVGspTxU32 queueBytes,
                                                NVGspMsgqTxHeaderPlan *out)
{
    if (!out) return NV_GSP_QUEUE_TX_NULL_OUTPUT;
    if (NV_GSP_MSGQ_ENTRY_BYTES < NV_GSP_QUEUE_ELEMENT_MIN_BYTES ||
        NV_GSP_MSGQ_ENTRY_BYTES > queueBytes)
        return NV_GSP_QUEUE_TX_BAD_GEOMETRY;

    const NVGspTxU64 entryOff64 = nvGspQueueTxAlignUp(
        NV_GSP_MSGQ_TX_HEADER_BYTES, NV_GSP_MSGQ_ENTRY_ALIGN_BYTES);
    if (!entryOff64 || entryOff64 > 0xffffffffull)
        return NV_GSP_QUEUE_TX_BAD_GEOMETRY;
    const NVGspTxU32 entryOff = (NVGspTxU32)entryOff64;
    if (queueBytes < entryOff || queueBytes - entryOff < NV_GSP_MSGQ_ENTRY_BYTES)
        return NV_GSP_QUEUE_TX_BAD_GEOMETRY;

    const NVGspTxU32 msgCount = (queueBytes - entryOff) / NV_GSP_MSGQ_ENTRY_BYTES;
    if (!msgCount) return NV_GSP_QUEUE_TX_BAD_GEOMETRY;

    NVGspMsgqTxHeaderPlan staged = {};
    // NvU16 major then NvU16 minor in the first little-endian dword.
    staged.dwords[0] = (NV_GSP_MSGQ_VERSION_MINOR << 16) | NV_GSP_MSGQ_VERSION_MAJOR;
    staged.dwords[1] = queueBytes;
    staged.dwords[2] = NV_GSP_MSGQ_ENTRY_BYTES;
    staged.dwords[3] = msgCount;
    staged.dwords[4] = entryOff;
    staged.dwords[5] = 0u;
    staged.dwords[6] = 0u;
    staged.dwords[7] = 0u;
    staged.entryOffset = entryOff;
    staged.messageSize = NV_GSP_MSGQ_ENTRY_BYTES;
    staged.messageCount = msgCount;
    *out = staged;
    return NV_GSP_QUEUE_TX_OK;
}

// Exact recovery rule from msgqTxGetFreeSpace(). Head/tail are monotonic NvU32
// counters. If a hardware tail register spuriously resets to zero and that
// observation would place it more than one ring behind the writer, NVIDIA uses
// the cached tail, except for the special "cached queue was exactly full" case,
// where zero is treated as reset-to-empty and the effective tail becomes head.
static inline NVGspTxU32 nvGspQueueTxNormalizeReadPtr(NVGspTxU32 writePtr,
                                                      NVGspTxU32 cachedReadPtr,
                                                      NVGspTxU32 observedReadPtr,
                                                      NVGspTxU32 msgCount)
{
    if (!msgCount) return cachedReadPtr;
    NVGspTxU32 readPtr = observedReadPtr;
    if ((NVGspTxU32)(writePtr - readPtr) > msgCount) {
        if (readPtr == 0u && (NVGspTxU32)(writePtr - cachedReadPtr) == msgCount)
            readPtr = writePtr;
        else
            readPtr = cachedReadPtr;
    }
    return readPtr;
}

// Plan one GSP command-queue submission without touching memory or registers.
// Each selected slot is copied as a full 4 KiB element, exactly as
// GspMsgQueueSendCommand() does, then a store fence precedes publishing the new
// monotonic TX_HEAD value. The caller remains responsible for those operations.
static inline NVGspTxU32 nvGspPlanQueueTx(NVGspTxU32 queueBytes,
                                          NVGspTxU32 writePtr,
                                          NVGspTxU32 cachedReadPtr,
                                          NVGspTxU32 observedReadPtr,
                                          NVGspTxU32 recordBytes,
                                          NVGspQueueTxPlan *out)
{
    if (!out) return NV_GSP_QUEUE_TX_NULL_OUTPUT;
    if (!recordBytes || recordBytes > NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
        return NV_GSP_QUEUE_TX_BAD_RECORD_SIZE;

    NVGspMsgqTxHeaderPlan header = {};
    if (nvGspBuildMsgqTxHeader(queueBytes, &header) != NV_GSP_QUEUE_TX_OK)
        return NV_GSP_QUEUE_TX_BAD_GEOMETRY;

    const NVGspTxU32 elements =
        (recordBytes + header.messageSize - 1u) / header.messageSize;
    if (!elements || elements > NV_GSP_MSGQ_MAX_RECORD_ELEMENTS ||
        elements > header.messageCount)
        return NV_GSP_QUEUE_TX_BAD_RECORD_SIZE;

    const NVGspTxU32 readPtr = nvGspQueueTxNormalizeReadPtr(
        writePtr, cachedReadPtr, observedReadPtr, header.messageCount);
    const NVGspTxU32 used = (NVGspTxU32)(writePtr - readPtr);
    if (used > header.messageCount)
        return NV_GSP_QUEUE_TX_BAD_GEOMETRY;
    const NVGspTxU32 freeSlots = header.messageCount - used;
    if (elements > freeSlots)
        return NV_GSP_QUEUE_TX_BUSY;

    NVGspQueueTxPlan staged = {};
    staged.effectiveReadPtr = readPtr;
    staged.freeBefore = freeSlots;
    staged.elementCount = elements;
    staged.payloadBytes = recordBytes;
    staged.copyBytes = elements * header.messageSize;
    staged.paddingBytes = staged.copyBytes - recordBytes;
    for (NVGspTxU32 i = 0; i < elements; ++i) {
        const NVGspTxU32 slot = (writePtr + i) % header.messageCount;
        staged.slot[i] = slot;
        staged.byteOffset[i] = header.entryOffset + slot * header.messageSize;
    }
    staged.newWritePtr = writePtr + elements; // NvU32 wrap is the source ABI.
    staged.txHeadValue = staged.newWritePtr;
    *out = staged;
    return NV_GSP_QUEUE_TX_OK;
}

#endif // NV_GSP_QUEUE_TX_HPP
