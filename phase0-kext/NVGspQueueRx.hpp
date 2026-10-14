// SPDX-License-Identifier: MIT
// Pure NVIDIA GSP-RM MSGQ v2 RX-ring planning. No shared-memory or MMIO access.
#ifndef NV_GSP_QUEUE_RX_HPP
#define NV_GSP_QUEUE_RX_HPP

#include "NVGspQueueInit.hpp"

using NVGspRxU32 = unsigned int;

#define NV_GSP_QUEUE_RX_NOT_ATTEMPTED          0u
#define NV_GSP_QUEUE_RX_OK                     1u
#define NV_GSP_QUEUE_RX_NULL_INPUT             2u
#define NV_GSP_QUEUE_RX_NULL_OUTPUT            3u
#define NV_GSP_QUEUE_RX_BAD_GEOMETRY           4u
#define NV_GSP_QUEUE_RX_BAD_INDEX              5u
#define NV_GSP_QUEUE_RX_EMPTY                  6u
#define NV_GSP_QUEUE_RX_BAD_CONSUME            7u
#define NV_GSP_QUEUE_RX_LIMIT                  8u

struct NVGspQueueRxAvailablePlan {
    NVGspRxU32 queueIndex;
    NVGspRxU32 readPtr;
    NVGspRxU32 cachedWritePtr;
    NVGspRxU32 observedWritePtr;
    NVGspRxU32 effectiveWritePtr;
    NVGspRxU32 available;
    NVGspRxU32 messageCount;
    NVGspRxU32 messageSize;
    NVGspRxU32 entryOffset;
    NVGspRxU32 statusQueueOffset;
    NVGspRxU32 messageHeadRegister;
    NVGspRxU32 messageTailRegister;
    bool loadFenceAfterHeadRead;
};

struct NVGspQueueRxElementPlan {
    NVGspRxU32 queueIndex;
    NVGspRxU32 elementIndex;
    NVGspRxU32 slot;
    NVGspRxU32 queueByteOffset;
    NVGspRxU32 sharedByteOffset;
    NVGspRxU32 bytes;
};

struct NVGspQueueRxConsumePlan {
    NVGspRxU32 queueIndex;
    NVGspRxU32 consumeCount;
    NVGspRxU32 oldReadPtr;
    NVGspRxU32 newReadPtr;
    NVGspRxU32 remainingAvailable;
    NVGspRxU32 messageTailRegister;
    NVGspRxU32 messageTailValue;
    bool storeFenceBeforeTailWrite;
};

static inline bool nvGspQueueRxSucceeded(NVGspRxU32 verdict)
{
    return verdict == NV_GSP_QUEUE_RX_OK;
}

static inline const char *nvGspQueueRxVerdictName(NVGspRxU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_RX_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_QUEUE_RX_OK:            return "ok";
        case NV_GSP_QUEUE_RX_NULL_INPUT:    return "null-input";
        case NV_GSP_QUEUE_RX_NULL_OUTPUT:   return "null-output";
        case NV_GSP_QUEUE_RX_BAD_GEOMETRY:  return "bad-geometry";
        case NV_GSP_QUEUE_RX_BAD_INDEX:     return "bad-index";
        case NV_GSP_QUEUE_RX_EMPTY:         return "empty";
        case NV_GSP_QUEUE_RX_BAD_CONSUME:   return "bad-consume";
        default:                            return "unknown";
    }
}

// Exact msgqRxGetReadAvailable() recovery rule from NVIDIA open RM. HEAD is a
// monotonic NvU32 counter, not a physical slot number. A reset-looking HEAD is
// rejected in favor of the cached writer, except when the cached queue was
// exactly full and hardware reports zero; that case is treated as empty by
// moving the effective writer back to the host read pointer.
static inline NVGspRxU32 nvGspQueueRxNormalizeWritePtr(NVGspRxU32 readPtr,
                                                       NVGspRxU32 cachedWritePtr,
                                                       NVGspRxU32 observedWritePtr,
                                                       NVGspRxU32 messageCount)
{
    if (!messageCount) return cachedWritePtr;
    NVGspRxU32 writePtr = observedWritePtr;
    if ((NVGspRxU32)(writePtr - readPtr) > messageCount) {
        if (writePtr == 0u &&
            (NVGspRxU32)(cachedWritePtr - readPtr) == messageCount)
            writePtr = readPtr;
        else
            writePtr = cachedWritePtr;
    }
    return writePtr;
}

// Reproduce the externally visible portion of msgqRxGetReadAvailable(): read
// MSGQ_HEAD, issue a load fence, normalize the monotonic writer, then compute
// available = writer - reader. The caller owns the actual register read/fence.
static inline NVGspRxU32 nvGspPlanQueueRxAvailable(const NVGspQueuePlan *layout,
                                                   NVGspRxU32 queueIndex,
                                                   NVGspRxU32 readPtr,
                                                   NVGspRxU32 cachedWritePtr,
                                                   NVGspRxU32 observedWritePtr,
                                                   NVGspQueueRxAvailablePlan *out)
{
    if (!layout) return NV_GSP_QUEUE_RX_NULL_INPUT;
    if (!out) return NV_GSP_QUEUE_RX_NULL_OUTPUT;
    if (layout->statusQueueBytes < NV_GSP_MSGQ_ENTRY_BYTES ||
        layout->sharedBytes < layout->statusQueueOffset ||
        layout->sharedBytes - layout->statusQueueOffset < layout->statusQueueBytes)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;

    NVGspMsgqTxHeaderPlan header = {};
    if (nvGspBuildMsgqTxHeader(layout->statusQueueBytes, &header) != NV_GSP_QUEUE_TX_OK)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;

    NVGspQueueRegisters regs = {};
    if (nvGspPlanQueueRegisters(queueIndex, &regs) != NV_GSP_QUEUE_REGS_OK)
        return NV_GSP_QUEUE_RX_BAD_INDEX;

    const NVGspRxU32 effectiveWrite = nvGspQueueRxNormalizeWritePtr(
        readPtr, cachedWritePtr, observedWritePtr, header.messageCount);
    const NVGspRxU32 available = (NVGspRxU32)(effectiveWrite - readPtr);
    if (available > header.messageCount)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;

    NVGspQueueRxAvailablePlan staged = {};
    staged.queueIndex = queueIndex;
    staged.readPtr = readPtr;
    staged.cachedWritePtr = cachedWritePtr;
    staged.observedWritePtr = observedWritePtr;
    staged.effectiveWritePtr = effectiveWrite;
    staged.available = available;
    staged.messageCount = header.messageCount;
    staged.messageSize = header.messageSize;
    staged.entryOffset = header.entryOffset;
    staged.statusQueueOffset = layout->statusQueueOffset;
    staged.messageHeadRegister = regs.messageHead;
    staged.messageTailRegister = regs.messageTail;
    staged.loadFenceAfterHeadRead = true;
    *out = staged;
    return NV_GSP_QUEUE_RX_OK;
}

// Reproduce msgqRxGetReadBuffer() slot selection from an already-refreshed RX
// snapshot. Physical slots wrap independently from the monotonic counter.
static inline NVGspRxU32 nvGspPlanQueueRxElement(const NVGspQueueRxAvailablePlan *available,
                                                 NVGspRxU32 elementIndex,
                                                 NVGspQueueRxElementPlan *out)
{
    if (!available) return NV_GSP_QUEUE_RX_NULL_INPUT;
    if (!out) return NV_GSP_QUEUE_RX_NULL_OUTPUT;
    if (!available->messageCount ||
        available->messageSize != NV_GSP_MSGQ_ENTRY_BYTES ||
        available->available > available->messageCount)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;
    if (elementIndex >= available->available)
        return NV_GSP_QUEUE_RX_EMPTY;

    const NVGspRxU32 counter = available->readPtr + elementIndex;
    const NVGspRxU32 slot = counter % available->messageCount;
    const NVGspRxU32 queueOffset = available->entryOffset + slot * available->messageSize;
    if (available->statusQueueOffset > 0xffffffffu - queueOffset)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;

    NVGspQueueRxElementPlan staged = {};
    staged.queueIndex = available->queueIndex;
    staged.elementIndex = elementIndex;
    staged.slot = slot;
    staged.queueByteOffset = queueOffset;
    staged.sharedByteOffset = available->statusQueueOffset + queueOffset;
    staged.bytes = available->messageSize;
    *out = staged;
    return NV_GSP_QUEUE_RX_OK;
}

// Reproduce msgqRxMarkConsumed() after availability has been refreshed. The
// monotonic read pointer advances first; a store fence then precedes publishing
// that pointer through MSGQ_TAIL. The caller owns the actual fence/MMIO write.
static inline NVGspRxU32 nvGspPlanQueueRxConsume(const NVGspQueueRxAvailablePlan *available,
                                                 NVGspRxU32 consumeCount,
                                                 NVGspQueueRxConsumePlan *out)
{
    if (!available) return NV_GSP_QUEUE_RX_NULL_INPUT;
    if (!out) return NV_GSP_QUEUE_RX_NULL_OUTPUT;
    if (!available->messageCount || available->available > available->messageCount)
        return NV_GSP_QUEUE_RX_BAD_GEOMETRY;
    if (!consumeCount || consumeCount > available->available)
        return NV_GSP_QUEUE_RX_BAD_CONSUME;

    NVGspQueueRxConsumePlan staged = {};
    staged.queueIndex = available->queueIndex;
    staged.consumeCount = consumeCount;
    staged.oldReadPtr = available->readPtr;
    staged.newReadPtr = available->readPtr + consumeCount; // NvU32 source ABI.
    staged.remainingAvailable = available->available - consumeCount;
    staged.messageTailRegister = available->messageTailRegister;
    staged.messageTailValue = staged.newReadPtr;
    staged.storeFenceBeforeTailWrite = true;
    *out = staged;
    return NV_GSP_QUEUE_RX_OK;
}

#endif // NV_GSP_QUEUE_RX_HPP
