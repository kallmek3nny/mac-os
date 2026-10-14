// SPDX-License-Identifier: MIT
// Pure GA10x/TU10x GSP-RM queue-register and publish planning. No MMIO writes.
#ifndef NV_GSP_QUEUE_REGS_HPP
#define NV_GSP_QUEUE_REGS_HPP

#include "NVGspQueueTx.hpp"

using NVGspRegU32 = unsigned int;

// NVIDIA open RM sources:
//   published/turing/tu102/dev_gsp.h
//   kern_gpu_tu102.c::gpuGetGspMsgQueueRegisters_TU102()
// GA10x, including GA107, dispatches this GSP queue-register HAL to TU102.
#define NV_GSP_QUEUE_REGISTER_COUNT               8u
#define NV_GSP_RM_QUEUE_INDEX                     0u
#define NV_GSP_QUEUE_REGISTER_STRIDE              0x00000008u
#define NV_GSP_QUEUE_HEAD_BASE                    0x00110c00u
#define NV_GSP_QUEUE_TAIL_BASE                    0x00110c04u
#define NV_GSP_MSGQ_HEAD_BASE                     0x00110c80u
#define NV_GSP_MSGQ_TAIL_BASE                     0x00110c84u

#define NV_GSP_QUEUE_REGS_NOT_ATTEMPTED           0u
#define NV_GSP_QUEUE_REGS_OK                      1u
#define NV_GSP_QUEUE_REGS_NULL_OUTPUT             2u
#define NV_GSP_QUEUE_REGS_BAD_INDEX               3u
#define NV_GSP_QUEUE_REGS_LIMIT                   4u

#define NV_GSP_QUEUE_PUBLISH_NOT_ATTEMPTED        0u
#define NV_GSP_QUEUE_PUBLISH_OK                   1u
#define NV_GSP_QUEUE_PUBLISH_NULL_INPUT           2u
#define NV_GSP_QUEUE_PUBLISH_NULL_OUTPUT          3u
#define NV_GSP_QUEUE_PUBLISH_BAD_INDEX            4u
#define NV_GSP_QUEUE_PUBLISH_BAD_TX_PLAN          5u
#define NV_GSP_QUEUE_PUBLISH_LIMIT                6u

struct NVGspQueueRegisters {
    NVGspRegU32 queueIndex;
    NVGspRegU32 commandHead;
    NVGspRegU32 commandTail;
    NVGspRegU32 messageHead;
    NVGspRegU32 messageTail;
};

// This describes the producer-side commit boundary only. The caller still owns
// the actual memory copies, CPU store fence, and MMIO operation.
struct NVGspQueuePublishPlan {
    NVGspRegU32 queueIndex;
    NVGspRegU32 elementCount;
    NVGspRegU32 copyBytes;
    NVGspRegU32 commandHeadRegister;
    NVGspRegU32 commandTailRegister;
    NVGspRegU32 commandHeadValue;
    bool storeFenceBeforeHeadWrite;
    bool commandHeadWriteNotifiesGsp;
};

static inline bool nvGspQueueRegsSucceeded(NVGspRegU32 verdict)
{
    return verdict == NV_GSP_QUEUE_REGS_OK;
}

static inline const char *nvGspQueueRegsVerdictName(NVGspRegU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_REGS_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_QUEUE_REGS_OK:            return "ok";
        case NV_GSP_QUEUE_REGS_NULL_OUTPUT:   return "null-output";
        case NV_GSP_QUEUE_REGS_BAD_INDEX:     return "bad-index";
        default:                              return "unknown";
    }
}

static inline bool nvGspQueuePublishSucceeded(NVGspRegU32 verdict)
{
    return verdict == NV_GSP_QUEUE_PUBLISH_OK;
}

static inline const char *nvGspQueuePublishVerdictName(NVGspRegU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_PUBLISH_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_QUEUE_PUBLISH_OK:            return "ok";
        case NV_GSP_QUEUE_PUBLISH_NULL_INPUT:    return "null-input";
        case NV_GSP_QUEUE_PUBLISH_NULL_OUTPUT:   return "null-output";
        case NV_GSP_QUEUE_PUBLISH_BAD_INDEX:     return "bad-index";
        case NV_GSP_QUEUE_PUBLISH_BAD_TX_PLAN:   return "bad-tx-plan";
        default:                                 return "unknown";
    }
}

static inline NVGspRegU32 nvGspQueueRegisterOffset(NVGspRegU32 base,
                                                   NVGspRegU32 queueIndex)
{
    return base + queueIndex * NV_GSP_QUEUE_REGISTER_STRIDE;
}

static inline NVGspRegU32 nvGspPlanQueueRegisters(NVGspRegU32 queueIndex,
                                                  NVGspQueueRegisters *out)
{
    if (!out) return NV_GSP_QUEUE_REGS_NULL_OUTPUT;
    if (queueIndex >= NV_GSP_QUEUE_REGISTER_COUNT)
        return NV_GSP_QUEUE_REGS_BAD_INDEX;

    NVGspQueueRegisters staged = {};
    staged.queueIndex = queueIndex;
    staged.commandHead = nvGspQueueRegisterOffset(NV_GSP_QUEUE_HEAD_BASE, queueIndex);
    staged.commandTail = nvGspQueueRegisterOffset(NV_GSP_QUEUE_TAIL_BASE, queueIndex);
    staged.messageHead = nvGspQueueRegisterOffset(NV_GSP_MSGQ_HEAD_BASE, queueIndex);
    staged.messageTail = nvGspQueueRegisterOffset(NV_GSP_MSGQ_TAIL_BASE, queueIndex);
    *out = staged;
    return NV_GSP_QUEUE_REGS_OK;
}

// Compose the final producer commit contract after nvGspPlanQueueTx(). Open RM
// first copies every selected 4 KiB element, then executes a STORE fence, then
// msgqTxSubmitBuffers() advances the monotonic write pointer and writes TX_HEAD.
// msgq_priv.h explicitly states that writing NV_PGSP_QUEUE_HEAD notifies GSP by
// hardware interrupt, so there is no second software doorbell in this protocol.
static inline NVGspRegU32 nvGspPlanQueuePublish(NVGspRegU32 queueIndex,
                                                const NVGspQueueTxPlan *tx,
                                                NVGspQueuePublishPlan *out)
{
    if (!tx) return NV_GSP_QUEUE_PUBLISH_NULL_INPUT;
    if (!out) return NV_GSP_QUEUE_PUBLISH_NULL_OUTPUT;
    if (queueIndex >= NV_GSP_QUEUE_REGISTER_COUNT)
        return NV_GSP_QUEUE_PUBLISH_BAD_INDEX;

    if (!tx->elementCount ||
        tx->elementCount > NV_GSP_MSGQ_MAX_RECORD_ELEMENTS ||
        tx->copyBytes != tx->elementCount * NV_GSP_MSGQ_ENTRY_BYTES ||
        tx->payloadBytes == 0u ||
        tx->payloadBytes > tx->copyBytes ||
        tx->paddingBytes != tx->copyBytes - tx->payloadBytes ||
        tx->txHeadValue != tx->newWritePtr)
        return NV_GSP_QUEUE_PUBLISH_BAD_TX_PLAN;

    NVGspQueueRegisters regs = {};
    if (nvGspPlanQueueRegisters(queueIndex, &regs) != NV_GSP_QUEUE_REGS_OK)
        return NV_GSP_QUEUE_PUBLISH_BAD_INDEX;

    NVGspQueuePublishPlan staged = {};
    staged.queueIndex = queueIndex;
    staged.elementCount = tx->elementCount;
    staged.copyBytes = tx->copyBytes;
    staged.commandHeadRegister = regs.commandHead;
    staged.commandTailRegister = regs.commandTail;
    staged.commandHeadValue = tx->txHeadValue;
    staged.storeFenceBeforeHeadWrite = true;
    staged.commandHeadWriteNotifiesGsp = true;
    *out = staged;
    return NV_GSP_QUEUE_PUBLISH_OK;
}

#endif // NV_GSP_QUEUE_REGS_HPP
