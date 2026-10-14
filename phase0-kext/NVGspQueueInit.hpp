// SPDX-License-Identifier: MIT
// Pure GSP-RM MSGQ v2 queue-initialization planning. No memory or MMIO writes.
#ifndef NV_GSP_QUEUE_INIT_HPP
#define NV_GSP_QUEUE_INIT_HPP

#include "NVGspQueuePlan.hpp"
#include "NVGspQueueTx.hpp"
#include "NVGspQueueRegs.hpp"

using NVGspInitU32 = unsigned int;

#define NV_GSP_QUEUE_INIT_NOT_ATTEMPTED            0u
#define NV_GSP_QUEUE_INIT_OK                       1u
#define NV_GSP_QUEUE_INIT_NULL_OUTPUT              2u
#define NV_GSP_QUEUE_INIT_BAD_LAYOUT               3u
#define NV_GSP_QUEUE_INIT_BAD_REGISTERS            4u
#define NV_GSP_QUEUE_INIT_BAD_RX_HEADER            5u
#define NV_GSP_QUEUE_INIT_LIMIT                    6u

#define NV_GSP_QUEUE_INIT_STEP_NONE                0u
#define NV_GSP_QUEUE_INIT_STEP_WRITE_SHARED        1u
#define NV_GSP_QUEUE_INIT_STEP_READ_SHARED         2u
#define NV_GSP_QUEUE_INIT_STEP_STORE_FENCE         3u
#define NV_GSP_QUEUE_INIT_STEP_WRITE_REGISTER      4u
#define NV_GSP_QUEUE_INIT_STEP_FULL_FENCE          5u
#define NV_GSP_QUEUE_INIT_STEP_LIMIT               6u

struct NVGspQueueInitStep {
    NVGspInitU32 kind;
    NVGspInitU32 offset;
    NVGspInitU32 bytes;
    NVGspInitU32 value;
};

struct NVGspQueueTxInitPlan {
    NVGspInitU32 queueIndex;
    NVGspInitU32 sharedHeaderOffset;
    NVGspInitU32 sharedHeaderBytes;
    NVGspInitU32 headerDwords[NV_GSP_MSGQ_TX_HEADER_DWORDS];
    NVGspInitU32 commandHeadRegister;
    NVGspInitU32 commandTailRegister;
    NVGspInitU32 initialWritePtr;
    NVGspInitU32 initialReadPtr;
    NVGspInitU32 initialFree;
    bool headWriteNotifiesGsp;
    NVGspQueueInitStep steps[4];
};

struct NVGspQueueRxLinkPlan {
    NVGspInitU32 queueIndex;
    NVGspInitU32 sharedHeaderOffset;
    NVGspInitU32 sharedHeaderBytes;
    NVGspInitU32 messageHeadRegister;
    NVGspInitU32 messageTailRegister;
    NVGspInitU32 initialReadPtr;
    NVGspInitU32 initialAvailable;
    NVGspInitU32 peerMessageCount;
    NVGspQueueInitStep steps[3];
};

static inline bool nvGspQueueInitSucceeded(NVGspInitU32 verdict)
{
    return verdict == NV_GSP_QUEUE_INIT_OK;
}

static inline const char *nvGspQueueInitVerdictName(NVGspInitU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_INIT_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_QUEUE_INIT_OK:            return "ok";
        case NV_GSP_QUEUE_INIT_NULL_OUTPUT:   return "null-output";
        case NV_GSP_QUEUE_INIT_BAD_LAYOUT:    return "bad-layout";
        case NV_GSP_QUEUE_INIT_BAD_REGISTERS: return "bad-registers";
        case NV_GSP_QUEUE_INIT_BAD_RX_HEADER: return "bad-rx-header";
        default:                              return "unknown";
    }
}

static inline bool nvGspQueueInitStepKnown(NVGspInitU32 kind)
{
    return kind > NV_GSP_QUEUE_INIT_STEP_NONE && kind < NV_GSP_QUEUE_INIT_STEP_LIMIT;
}

// Reproduce msgqTxCreate()'s externally visible initialization contract for the
// GSP RM command queue. Open RM writes the 32-byte MSGQ v2 header into the TX
// backing store, issues a store fence, writes TX HEAD = 0, then issues a full
// fence. TX TAIL is owned by GSP and is not written by the host here.
static inline NVGspInitU32 nvGspPlanQueueTxInit(const NVGspQueuePlan *layout,
                                                NVGspInitU32 queueIndex,
                                                NVGspQueueTxInitPlan *out)
{
    if (!out) return NV_GSP_QUEUE_INIT_NULL_OUTPUT;
    if (!layout ||
        layout->commandQueueBytes < NV_GSP_MSGQ_ENTRY_BYTES ||
        layout->sharedBytes < layout->commandQueueOffset ||
        layout->sharedBytes - layout->commandQueueOffset < layout->commandQueueBytes)
        return NV_GSP_QUEUE_INIT_BAD_LAYOUT;

    NVGspMsgqTxHeaderPlan header = {};
    if (nvGspBuildMsgqTxHeader(layout->commandQueueBytes, &header) != NV_GSP_QUEUE_TX_OK)
        return NV_GSP_QUEUE_INIT_BAD_LAYOUT;

    NVGspQueueRegisters regs = {};
    if (nvGspPlanQueueRegisters(queueIndex, &regs) != NV_GSP_QUEUE_REGS_OK)
        return NV_GSP_QUEUE_INIT_BAD_REGISTERS;

    NVGspQueueTxInitPlan staged = {};
    staged.queueIndex = queueIndex;
    staged.sharedHeaderOffset = layout->commandQueueOffset;
    staged.sharedHeaderBytes = NV_GSP_MSGQ_TX_HEADER_BYTES;
    for (NVGspInitU32 i = 0; i < NV_GSP_MSGQ_TX_HEADER_DWORDS; ++i)
        staged.headerDwords[i] = header.dwords[i];
    staged.commandHeadRegister = regs.commandHead;
    staged.commandTailRegister = regs.commandTail;
    staged.initialWritePtr = 0u;
    staged.initialReadPtr = 0u;
    staged.initialFree = header.messageCount;
    staged.headWriteNotifiesGsp = true;

    staged.steps[0] = { NV_GSP_QUEUE_INIT_STEP_WRITE_SHARED,
                        staged.sharedHeaderOffset,
                        staged.sharedHeaderBytes,
                        0u };
    staged.steps[1] = { NV_GSP_QUEUE_INIT_STEP_STORE_FENCE, 0u, 0u, 0u };
    staged.steps[2] = { NV_GSP_QUEUE_INIT_STEP_WRITE_REGISTER,
                        staged.commandHeadRegister,
                        4u,
                        0u };
    staged.steps[3] = { NV_GSP_QUEUE_INIT_STEP_FULL_FENCE, 0u, 0u, 0u };
    *out = staged;
    return NV_GSP_QUEUE_INIT_OK;
}

// Validate the MSGQ v2 header produced by GSP for the host RX/status queue using
// the same compatibility rules as msgqRxLink(): size and msgSize must match,
// major version must match, msgCount must equal floor((size-entryOff)/msgSize),
// and entryOff must leave room for at least one message. Minor version is NOT
// rejected by NVIDIA's source and therefore is intentionally not rejected here.
static inline bool nvGspQueueRxHeaderAccepted(const NVGspQueuePlan *layout,
                                              const NVGspInitU32 dwords[NV_GSP_MSGQ_TX_HEADER_DWORDS],
                                              NVGspInitU32 *outMessageCount)
{
    if (!layout || !dwords) return false;

    const NVGspInitU32 major = dwords[0] & 0xffffu;
    const NVGspInitU32 size = dwords[1];
    const NVGspInitU32 msgSize = dwords[2];
    const NVGspInitU32 msgCount = dwords[3];
    const NVGspInitU32 entryOff = dwords[4];

    if (msgSize < 16u || msgSize > size) return false;
    if (size != layout->statusQueueBytes) return false;
    if (msgSize != NV_GSP_MSGQ_ENTRY_BYTES) return false;
    if (major != NV_GSP_MSGQ_VERSION_MAJOR) return false;
    if (entryOff > size || msgSize > size - entryOff) return false;
    if (msgCount != (size - entryOff) / msgSize) return false;

    if (outMessageCount) *outMessageCount = msgCount;
    return true;
}

// Reproduce msgqRxLink()'s externally visible status-queue link contract. The
// status header is owned and initialized by GSP. The host first reads and
// validates it, then issues a store fence and publishes RX TAIL = 0. RX HEAD is
// owned by GSP and is not written by the host here.
static inline NVGspInitU32 nvGspPlanQueueRxLink(const NVGspQueuePlan *layout,
                                                NVGspInitU32 queueIndex,
                                                const NVGspInitU32 peerHeader[NV_GSP_MSGQ_TX_HEADER_DWORDS],
                                                NVGspQueueRxLinkPlan *out)
{
    if (!out) return NV_GSP_QUEUE_INIT_NULL_OUTPUT;
    if (!layout ||
        layout->statusQueueBytes < NV_GSP_MSGQ_ENTRY_BYTES ||
        layout->sharedBytes < layout->statusQueueOffset ||
        layout->sharedBytes - layout->statusQueueOffset < layout->statusQueueBytes)
        return NV_GSP_QUEUE_INIT_BAD_LAYOUT;

    NVGspQueueRegisters regs = {};
    if (nvGspPlanQueueRegisters(queueIndex, &regs) != NV_GSP_QUEUE_REGS_OK)
        return NV_GSP_QUEUE_INIT_BAD_REGISTERS;

    NVGspInitU32 peerCount = 0u;
    if (!nvGspQueueRxHeaderAccepted(layout, peerHeader, &peerCount))
        return NV_GSP_QUEUE_INIT_BAD_RX_HEADER;

    NVGspQueueRxLinkPlan staged = {};
    staged.queueIndex = queueIndex;
    staged.sharedHeaderOffset = layout->statusQueueOffset;
    staged.sharedHeaderBytes = NV_GSP_MSGQ_TX_HEADER_BYTES;
    staged.messageHeadRegister = regs.messageHead;
    staged.messageTailRegister = regs.messageTail;
    staged.initialReadPtr = 0u;
    staged.initialAvailable = 0u;
    staged.peerMessageCount = peerCount;

    staged.steps[0] = { NV_GSP_QUEUE_INIT_STEP_READ_SHARED,
                        staged.sharedHeaderOffset,
                        staged.sharedHeaderBytes,
                        0u };
    staged.steps[1] = { NV_GSP_QUEUE_INIT_STEP_STORE_FENCE, 0u, 0u, 0u };
    staged.steps[2] = { NV_GSP_QUEUE_INIT_STEP_WRITE_REGISTER,
                        staged.messageTailRegister,
                        4u,
                        0u };
    *out = staged;
    return NV_GSP_QUEUE_INIT_OK;
}

#endif // NV_GSP_QUEUE_INIT_HPP
