// SPDX-License-Identifier: MIT
// Pure end-to-end GSP-RM RPC transport rehearsal. No memory or MMIO access.
#ifndef NV_GSP_RPC_TRANSACTION_HPP
#define NV_GSP_RPC_TRANSACTION_HPP

#include "NVGspQueueRx.hpp"

using NVGspTxnU32 = unsigned int;

#define NV_GSP_RPC_TXN_NOT_ATTEMPTED          0u
#define NV_GSP_RPC_TXN_OK                     1u
#define NV_GSP_RPC_TXN_NULL_INPUT             2u
#define NV_GSP_RPC_TXN_NULL_OUTPUT            3u
#define NV_GSP_RPC_TXN_FRAME_FAILED           4u
#define NV_GSP_RPC_TXN_TX_FAILED              5u
#define NV_GSP_RPC_TXN_PUBLISH_FAILED         6u
#define NV_GSP_RPC_TXN_RX_FAILED              7u
#define NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE      8u
#define NV_GSP_RPC_TXN_RESPONSE_SHORT         9u
#define NV_GSP_RPC_TXN_CONSUME_FAILED        10u
#define NV_GSP_RPC_TXN_LIMIT                 11u

struct NVGspRpcTransportPlan {
    NVGspTxnU32 function;
    NVGspTxnU32 sequence;
    NVGspTxnU32 requestParameterBytes;
    NVGspTxnU32 requestRecordBytes;
    NVGspRpcFramePlan requestFrame;
    NVGspQueueTxPlan requestTx;
    NVGspQueuePublishPlan requestPublish;

    NVGspTxnU32 responseRecordBytes;
    NVGspTxnU32 responseElementCount;
    NVGspQueueRxAvailablePlan responseAvailable;
    NVGspQueueRxElementPlan responseElement[NV_GSP_MSGQ_MAX_RECORD_ELEMENTS];
    NVGspQueueRxConsumePlan responseConsume;

    // Deliberately false: this planner proves transport placement/order only.
    // A later response-header validator must match signature/function/sequence
    // and result fields before a runtime caller may call the RPC successful.
    bool responseSemanticallyValidated;
};

static inline bool nvGspRpcTxnSucceeded(NVGspTxnU32 verdict)
{
    return verdict == NV_GSP_RPC_TXN_OK;
}

static inline const char *nvGspRpcTxnVerdictName(NVGspTxnU32 verdict)
{
    switch (verdict) {
        case NV_GSP_RPC_TXN_NOT_ATTEMPTED:     return "not-attempted";
        case NV_GSP_RPC_TXN_OK:                return "ok";
        case NV_GSP_RPC_TXN_NULL_INPUT:        return "null-input";
        case NV_GSP_RPC_TXN_NULL_OUTPUT:       return "null-output";
        case NV_GSP_RPC_TXN_FRAME_FAILED:      return "frame-failed";
        case NV_GSP_RPC_TXN_TX_FAILED:         return "tx-failed";
        case NV_GSP_RPC_TXN_PUBLISH_FAILED:    return "publish-failed";
        case NV_GSP_RPC_TXN_RX_FAILED:         return "rx-failed";
        case NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE: return "bad-response-size";
        case NV_GSP_RPC_TXN_RESPONSE_SHORT:    return "response-short";
        case NV_GSP_RPC_TXN_CONSUME_FAILED:    return "consume-failed";
        default:                               return "unknown";
    }
}

// Compose one complete queue-level request/response rehearsal around RM queue 0.
// `responseRecordBytes` is the length obtained by a caller after inspecting the
// received record header; this helper deliberately does not trust or parse RPC
// semantics yet. It only proves that enough RX elements exist and plans their
// exact physical/shared offsets plus the final MSGQ_TAIL consumption boundary.
static inline NVGspTxnU32 nvGspPlanRpcTransport(const NVGspQueuePlan *layout,
                                                NVGspTxnU32 function,
                                                NVGspTxnU32 sequence,
                                                NVGspTxnU32 parameterBytes,
                                                NVGspTxnU32 txWritePtr,
                                                NVGspTxnU32 txCachedReadPtr,
                                                NVGspTxnU32 txObservedReadPtr,
                                                NVGspTxnU32 rxReadPtr,
                                                NVGspTxnU32 rxCachedWritePtr,
                                                NVGspTxnU32 rxObservedWritePtr,
                                                NVGspTxnU32 responseRecordBytes,
                                                NVGspRpcTransportPlan *out)
{
    if (!layout) return NV_GSP_RPC_TXN_NULL_INPUT;
    if (!out) return NV_GSP_RPC_TXN_NULL_OUTPUT;
    if (!responseRecordBytes || responseRecordBytes > NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
        return NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE;

    NVGspRpcFramePlan frame = {};
    if (nvGspBuildRmRpcFrameNoCc(function, sequence, parameterBytes, &frame) != NV_GSP_RPC_FRAME_OK)
        return NV_GSP_RPC_TXN_FRAME_FAILED;

    NVGspQueueTxPlan tx = {};
    if (nvGspPlanQueueTx(layout->commandQueueBytes,
                         txWritePtr, txCachedReadPtr, txObservedReadPtr,
                         frame.queueMessageBytes, &tx) != NV_GSP_QUEUE_TX_OK)
        return NV_GSP_RPC_TXN_TX_FAILED;

    NVGspQueuePublishPlan publish = {};
    if (nvGspPlanQueuePublish(NV_GSP_RM_QUEUE_INDEX, &tx, &publish) != NV_GSP_QUEUE_PUBLISH_OK)
        return NV_GSP_RPC_TXN_PUBLISH_FAILED;

    NVGspQueueRxAvailablePlan rx = {};
    if (nvGspPlanQueueRxAvailable(layout, NV_GSP_RM_QUEUE_INDEX,
                                  rxReadPtr, rxCachedWritePtr, rxObservedWritePtr,
                                  &rx) != NV_GSP_QUEUE_RX_OK)
        return NV_GSP_RPC_TXN_RX_FAILED;

    const NVGspTxnU32 responseElements =
        (responseRecordBytes + NV_GSP_MSGQ_ENTRY_BYTES - 1u) / NV_GSP_MSGQ_ENTRY_BYTES;
    if (!responseElements || responseElements > NV_GSP_MSGQ_MAX_RECORD_ELEMENTS)
        return NV_GSP_RPC_TXN_BAD_RESPONSE_SIZE;
    if (responseElements > rx.available)
        return NV_GSP_RPC_TXN_RESPONSE_SHORT;

    NVGspRpcTransportPlan staged = {};
    staged.function = function;
    staged.sequence = sequence;
    staged.requestParameterBytes = parameterBytes;
    staged.requestRecordBytes = frame.queueMessageBytes;
    staged.requestFrame = frame;
    staged.requestTx = tx;
    staged.requestPublish = publish;
    staged.responseRecordBytes = responseRecordBytes;
    staged.responseElementCount = responseElements;
    staged.responseAvailable = rx;

    for (NVGspTxnU32 i = 0u; i < responseElements; ++i) {
        if (nvGspPlanQueueRxElement(&rx, i, &staged.responseElement[i]) != NV_GSP_QUEUE_RX_OK)
            return NV_GSP_RPC_TXN_RX_FAILED;
    }

    if (nvGspPlanQueueRxConsume(&rx, responseElements, &staged.responseConsume) != NV_GSP_QUEUE_RX_OK)
        return NV_GSP_RPC_TXN_CONSUME_FAILED;

    staged.responseSemanticallyValidated = false;
    *out = staged;
    return NV_GSP_RPC_TXN_OK;
}

#endif // NV_GSP_RPC_TRANSACTION_HPP
