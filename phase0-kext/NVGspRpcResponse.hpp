// SPDX-License-Identifier: MIT
// Pure GSP-RM synchronous-response validation. No queue, memory or MMIO access.
#ifndef NV_GSP_RPC_RESPONSE_HPP
#define NV_GSP_RPC_RESPONSE_HPP

#include "NVGspRpcTransaction.hpp"

using NVGspRespU32 = unsigned int;

#define NV_GSP_RPC_RESPONSE_NOT_ATTEMPTED      0u
#define NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS    1u
#define NV_GSP_RPC_RESPONSE_MATCHED_ERROR      2u
#define NV_GSP_RPC_RESPONSE_ASYNC_EVENT        3u
#define NV_GSP_RPC_RESPONSE_NULL_INPUT         4u
#define NV_GSP_RPC_RESPONSE_NULL_OUTPUT        5u
#define NV_GSP_RPC_RESPONSE_TOO_SHORT          6u
#define NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE    7u
#define NV_GSP_RPC_RESPONSE_BAD_MCTP           8u
#define NV_GSP_RPC_RESPONSE_BAD_NVDM           9u
#define NV_GSP_RPC_RESPONSE_BAD_LENGTH         10u
#define NV_GSP_RPC_RESPONSE_BAD_VERSION        11u
#define NV_GSP_RPC_RESPONSE_BAD_SIGNATURE      12u
#define NV_GSP_RPC_RESPONSE_RESULT_PENDING      13u
#define NV_GSP_RPC_RESPONSE_BAD_TRANSPORT       14u
#define NV_GSP_RPC_RESPONSE_LIMIT              15u

struct NVGspRpcResponsePlan {
    NVGspRespU32 recordBytes;
    NVGspRespU32 rpcBytes;
    NVGspRespU32 function;
    NVGspRespU32 sequence;
    NVGspRespU32 rpcResult;
    NVGspRespU32 rpcResultPrivate;
    bool fullFenceBeforeReceive;
    bool structurallyValid;
    bool matchesExpected;
    bool rpcSucceeded;
    bool asyncEvent;
};

struct NVGspRpcValidatedTransaction {
    NVGspRpcTransportPlan transport;
    NVGspRpcResponsePlan response;
    bool responseSemanticallyValidated;
    bool responseSucceeded;
};

static inline bool nvGspRpcResponseMatched(NVGspRespU32 verdict)
{
    return verdict == NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS ||
           verdict == NV_GSP_RPC_RESPONSE_MATCHED_ERROR;
}

static inline bool nvGspRpcResponseSucceeded(NVGspRespU32 verdict)
{
    return verdict == NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS;
}

static inline const char *nvGspRpcResponseVerdictName(NVGspRespU32 verdict)
{
    switch (verdict) {
        case NV_GSP_RPC_RESPONSE_NOT_ATTEMPTED:   return "not-attempted";
        case NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS: return "matched-success";
        case NV_GSP_RPC_RESPONSE_MATCHED_ERROR:   return "matched-error";
        case NV_GSP_RPC_RESPONSE_ASYNC_EVENT:     return "async-event";
        case NV_GSP_RPC_RESPONSE_NULL_INPUT:      return "null-input";
        case NV_GSP_RPC_RESPONSE_NULL_OUTPUT:     return "null-output";
        case NV_GSP_RPC_RESPONSE_TOO_SHORT:       return "too-short";
        case NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE: return "bad-record-size";
        case NV_GSP_RPC_RESPONSE_BAD_MCTP:        return "bad-mctp";
        case NV_GSP_RPC_RESPONSE_BAD_NVDM:        return "bad-nvdm";
        case NV_GSP_RPC_RESPONSE_BAD_LENGTH:      return "bad-length";
        case NV_GSP_RPC_RESPONSE_BAD_VERSION:     return "bad-version";
        case NV_GSP_RPC_RESPONSE_BAD_SIGNATURE:   return "bad-signature";
        case NV_GSP_RPC_RESPONSE_RESULT_PENDING:   return "result-pending";
        case NV_GSP_RPC_RESPONSE_BAD_TRANSPORT:    return "bad-transport";
        default:                                  return "unknown";
    }
}

// Validate the fixed non-confidential prefix of one GSP-RM response record.
// NVIDIA's synchronous receive path identifies the wanted reply by EXACT
// function + sequence equality. A valid nonmatching message is an asynchronous
// event and must not be mistaken for a failed synchronous RPC. Once identity
// matches, rpc_result is judged separately: NV_OK (0) means success, while a
// nonzero value is a real matched RM/GSP error response.
//
// KernelGsp executes a full memory fence before GspMsgQueueReceiveStatus(); the
// flag in the output makes that higher-level ordering requirement explicit for
// the future live transport implementation.
static inline NVGspRespU32 nvGspValidateRpcResponseNoCc(
    NVGspRespU32 expectedFunction,
    NVGspRespU32 expectedSequence,
    const NVGspRespU32 prefix[NV_GSP_RPC_PREFIX_DWORDS],
    NVGspRespU32 recordBytes,
    NVGspRpcResponsePlan *out)
{
    if (!prefix) return NV_GSP_RPC_RESPONSE_NULL_INPUT;
    if (!out) return NV_GSP_RPC_RESPONSE_NULL_OUTPUT;
    if (recordBytes < NV_GSP_RPC_PREFIX_BYTES)
        return NV_GSP_RPC_RESPONSE_TOO_SHORT;
    if (recordBytes > NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
        return NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE;

    if (prefix[0] != NV_GSP_RPC_MCTP_MAGIC ||
        prefix[2] != NV_GSP_RPC_MCTP_SINGLE_HEADER)
        return NV_GSP_RPC_RESPONSE_BAD_MCTP;
    if (prefix[3] != NV_GSP_RPC_NVDM_VENDOR_HEADER_RM_RPC || prefix[5] != 0u)
        return NV_GSP_RPC_RESPONSE_BAD_NVDM;

    const NVGspRespU32 rpcBytes = prefix[4];
    if (rpcBytes < NV_GSP_RPC_HEADER_BYTES ||
        prefix[1] != recordBytes ||
        rpcBytes > recordBytes ||
        recordBytes - rpcBytes != NV_GSP_RPC_OUTER_NO_CC_BYTES ||
        prefix[8] != rpcBytes)
        return NV_GSP_RPC_RESPONSE_BAD_LENGTH;

    if (prefix[6] != NV_GSP_RPC_HEADER_VERSION)
        return NV_GSP_RPC_RESPONSE_BAD_VERSION;
    if (prefix[7] != NV_GSP_RPC_SIGNATURE_VALID)
        return NV_GSP_RPC_RESPONSE_BAD_SIGNATURE;

    // The outgoing request uses 0xffffffff as an in-flight sentinel. Until
    // GSP overwrites it with a completed result, neither success nor an RM
    // error has been observed. Refuse atomically and leave the RX tail alone.
    if (prefix[10] == NV_GSP_RPC_RESULT_PENDING)
        return NV_GSP_RPC_RESPONSE_RESULT_PENDING;

    NVGspRpcResponsePlan staged = {};
    staged.recordBytes = recordBytes;
    staged.rpcBytes = rpcBytes;
    staged.function = prefix[9];
    staged.rpcResult = prefix[10];
    staged.rpcResultPrivate = prefix[11];
    staged.sequence = prefix[12];
    staged.fullFenceBeforeReceive = true;
    staged.structurallyValid = true;
    staged.matchesExpected = staged.function == expectedFunction &&
                             staged.sequence == expectedSequence;
    staged.asyncEvent = !staged.matchesExpected;
    staged.rpcSucceeded = staged.matchesExpected && staged.rpcResult == 0u;
    *out = staged;

    if (!staged.matchesExpected)
        return NV_GSP_RPC_RESPONSE_ASYNC_EVENT;
    if (staged.rpcResult != 0u)
        return NV_GSP_RPC_RESPONSE_MATCHED_ERROR;
    return NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS;
}

// Verify that caller-supplied rehearsal metadata has not drifted from the
// command frame it describes. This is CONSISTENCY ONLY: caller-controlled plans
// are never evidence of a sent RPC or provider-owned channel allocation.
static inline bool nvGspRpcTransportIdentityConsistent(
    const NVGspRpcTransportPlan &transport)
{
    const bool basic = !transport.responseSemanticallyValidated &&
           transport.requestFrame.prefixBytes == NV_GSP_RPC_PREFIX_BYTES &&
           transport.requestFrame.parameterBytes == transport.requestParameterBytes &&
           transport.requestFrame.queueMessageBytes == transport.requestRecordBytes &&
           transport.requestFrame.prefix[0] == NV_GSP_RPC_MCTP_MAGIC &&
           transport.requestFrame.prefix[9] == transport.function &&
           transport.requestFrame.prefix[12] == transport.sequence &&
           transport.requestTx.payloadBytes == transport.requestRecordBytes &&
           transport.requestPublish.commandHeadValue == transport.requestTx.newWritePtr &&
           transport.responseConsume.consumeCount == transport.responseElementCount &&
           transport.responseConsume.messageTailValue ==
               transport.responseConsume.newReadPtr &&
           // Pinned RX record geometry must not drift from the consumption
           // span later used as provisional receive/retirement evidence.
           transport.responseRecordBytes >= NV_GSP_RPC_PREFIX_BYTES &&
           transport.responseRecordBytes <= NV_GSP_QUEUE_ELEMENT_MAX_BYTES &&
           transport.responseElementCount != 0u &&
           transport.responseElementCount <= NV_GSP_MSGQ_MAX_RECORD_ELEMENTS &&
           transport.responseElementCount ==
               (transport.responseRecordBytes + NV_GSP_MSGQ_ENTRY_BYTES - 1u) /
                   NV_GSP_MSGQ_ENTRY_BYTES &&
           transport.responseAvailable.queueIndex == NV_GSP_RM_QUEUE_INDEX &&
           transport.responseConsume.queueIndex == NV_GSP_RM_QUEUE_INDEX &&
           transport.responseAvailable.messageSize == NV_GSP_MSGQ_ENTRY_BYTES &&
           transport.responseAvailable.messageCount != 0u &&
           transport.responseAvailable.available <= transport.responseAvailable.messageCount &&
           transport.responseElementCount <= transport.responseAvailable.available &&
           transport.responseAvailable.loadFenceAfterHeadRead &&
           transport.responseConsume.storeFenceBeforeTailWrite &&
           transport.responseConsume.oldReadPtr == transport.responseAvailable.readPtr &&
           transport.responseConsume.newReadPtr ==
               static_cast<NVGspRespU32>(
                   transport.responseAvailable.readPtr + transport.responseElementCount) &&
           transport.responseConsume.remainingAvailable ==
               transport.responseAvailable.available - transport.responseElementCount &&
           transport.responseConsume.messageTailRegister ==
               transport.responseAvailable.messageTailRegister;
    if (!basic) return false;
    for (NVGspRespU32 i = 0u; i < transport.responseElementCount; ++i) {
        NVGspQueueRxElementPlan expected = {};
        if (nvGspPlanQueueRxElement(&transport.responseAvailable,i,&expected) !=
            NV_GSP_QUEUE_RX_OK)
            return false;
        const NVGspQueueRxElementPlan &actual = transport.responseElement[i];
        if (actual.queueIndex != expected.queueIndex ||
            actual.elementIndex != expected.elementIndex ||
            actual.slot != expected.slot ||
            actual.queueByteOffset != expected.queueByteOffset ||
            actual.sharedByteOffset != expected.sharedByteOffset ||
            actual.bytes != expected.bytes)
            return false;
    }
    return true;
}

// Add semantic response judgement to an already-successful pure transport
// rehearsal. Transport placement and response identity remain separate layers:
// a matched error is semantically VALID but unsuccessful, while an async event
// is valid queue traffic that is explicitly not this transaction's response.
static inline NVGspRespU32 nvGspValidateRpcTransactionResponse(
    const NVGspRpcTransportPlan *transport,
    const NVGspRespU32 prefix[NV_GSP_RPC_PREFIX_DWORDS],
    NVGspRespU32 recordBytes,
    NVGspRpcValidatedTransaction *out)
{
    if (!transport || !prefix) return NV_GSP_RPC_RESPONSE_NULL_INPUT;
    if (!out) return NV_GSP_RPC_RESPONSE_NULL_OUTPUT;
    if (!nvGspRpcTransportIdentityConsistent(*transport))
        return NV_GSP_RPC_RESPONSE_BAD_TRANSPORT;
    if (recordBytes != transport->responseRecordBytes)
        return NV_GSP_RPC_RESPONSE_BAD_RECORD_SIZE;

    NVGspRpcResponsePlan response = {};
    const NVGspRespU32 verdict = nvGspValidateRpcResponseNoCc(
        transport->function, transport->sequence, prefix, recordBytes, &response);

    if (verdict != NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS &&
        verdict != NV_GSP_RPC_RESPONSE_MATCHED_ERROR &&
        verdict != NV_GSP_RPC_RESPONSE_ASYNC_EVENT)
        return verdict;

    NVGspRpcValidatedTransaction staged = {};
    staged.transport = *transport;
    staged.response = response;
    staged.responseSemanticallyValidated = nvGspRpcResponseMatched(verdict);
    staged.responseSucceeded = nvGspRpcResponseSucceeded(verdict);
    *out = staged;
    return verdict;
}

#endif // NV_GSP_RPC_RESPONSE_HPP
