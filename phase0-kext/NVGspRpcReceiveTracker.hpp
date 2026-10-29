// SPDX-License-Identifier: MIT
// Pure single-flight GSP-RM response ordering and retirement precondition.
// NOT a live queue executor, hardware transaction receipt, or channel proof.
#ifndef NV_GSP_RPC_RECEIVE_TRACKER_HPP
#define NV_GSP_RPC_RECEIVE_TRACKER_HPP

#include "NVGspRpcResponse.hpp"

using NVGspRxTrackU32 = unsigned int;

#define NV_GSP_RX_TRACK_OK                0u
#define NV_GSP_RX_TRACK_NULL              1u
#define NV_GSP_RX_TRACK_UNINITIALIZED     2u
#define NV_GSP_RX_TRACK_REINIT            3u
#define NV_GSP_RX_TRACK_BUSY              4u
#define NV_GSP_RX_TRACK_BAD_TRANSPORT     5u
#define NV_GSP_RX_TRACK_NO_REQUEST        6u
#define NV_GSP_RX_TRACK_AWAIT_CONSUME     7u
#define NV_GSP_RX_TRACK_ASYNC_EVENT       8u
#define NV_GSP_RX_TRACK_MATCHED_SUCCESS   9u
#define NV_GSP_RX_TRACK_MATCHED_ERROR    10u
#define NV_GSP_RX_TRACK_RESULT_PENDING   11u
#define NV_GSP_RX_TRACK_QUARANTINED      12u
#define NV_GSP_RX_TRACK_ID_COLLISION     13u
#define NV_GSP_RX_TRACK_MALFORMED        14u
#define NV_GSP_RX_TRACK_STOPPED          15u
#define NV_GSP_RX_TRACK_SEQUENCE        16u
#define NV_GSP_RX_TRACK_GEOMETRY_MISMATCH 17u
#define NV_GSP_RX_TRACK_NOT_PUBLISHED   18u
#define NV_GSP_RX_TRACK_PUBLISH_UNCONFIRMED 19u
#define NV_GSP_RX_TRACK_PUBLISH_IDENTITY 20u
#define NV_GSP_RX_TRACK_PUBLISH_DUPLICATE 21u
#define NV_GSP_RX_TRACK_TAIL_UNCONFIRMED 22u
#define NV_GSP_RX_TRACK_TAIL_MISMATCH   23u
#define NV_GSP_RX_TRACK_HEAD_UNCONFIRMED 24u
#define NV_GSP_RX_TRACK_HEAD_EMPTY      25u
#define NV_GSP_RX_TRACK_HEAD_INVALID    26u
#define NV_GSP_RX_TRACK_HEAD_DUPLICATE  27u
#define NV_GSP_RX_TRACK_NOT_AVAILABLE   28u
#define NV_GSP_RX_TRACK_LIMIT           29u

// Bound asynchronous messages seen for one request. No polling, sleeps,
// retry automation, MMIO, RX-tail writes or dynamic allocation in this code.
#define NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS 63u

struct NVGspRpcReceiveTracker {
    bool initialized;
    bool stopped;
    bool quarantined;
    bool active;
    bool awaitingConsume;
    bool requestPublished;
    // One fresh provider-reported RX HEAD witness authorizes exactly one
    // record read attempt; it is spent by classification or a pending reply.
    bool recordAvailable;
    bool recordMatched;
    bool recordSucceeded;
    bool hasLastSequence;
    NVGspRxTrackU32 function;
    NVGspRxTrackU32 sequence;
    NVGspRxTrackU32 lastSequence;
    NVGspRxTrackU32 expectedRecordBytes;
    NVGspRxTrackU32 expectedResponseElements;
    // Monotonic NvU32 RX read pointer where the next record must begin, and
    // the planned RX ring capacity bounding any reported writer (MSGQ_HEAD).
    NVGspRxTrackU32 expectedReadPtr;
    NVGspRxTrackU32 rxMessageCount;
    // Monotonic NvU32 RX TAIL expected from the exact planned consume span.
    // The caller must independently witness its provider-owned publication.
    NVGspRxTrackU32 expectedConsumeTail;
    NVGspRxTrackU32 asyncRecordCount;
};

// The caller must own a real channel/queue producer before using this guard.
// Initialization or a transport plan NEVER proves the request was submitted.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerInit(
    NVGspRpcReceiveTracker *tracker)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (tracker->initialized) return NV_GSP_RX_TRACK_REINIT;
    NVGspRpcReceiveTracker staged = {};
    staged.initialized = true;
    *tracker = staged;
    return NV_GSP_RX_TRACK_OK;
}

// Bind an already-planned request identity. Exactly one request may be live.
// Subsequent identities advance by one mod 2^32; zero after wrap is legal.
// This is NOT a transmit/publish acknowledgement or owner-authentication API.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerBegin(
    NVGspRpcReceiveTracker *tracker,
    const NVGspRpcTransportPlan *transport)
{
    if (!tracker || !transport) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->stopped) return NV_GSP_RX_TRACK_STOPPED;
    if (tracker->quarantined) return NV_GSP_RX_TRACK_QUARANTINED;
    if (tracker->active || tracker->awaitingConsume) return NV_GSP_RX_TRACK_BUSY;
    if (!nvGspRpcTransportIdentityConsistent(*transport))
        return NV_GSP_RX_TRACK_BAD_TRANSPORT;
    if (tracker->hasLastSequence &&
        transport->sequence != static_cast<NVGspRxTrackU32>(
                                   tracker->lastSequence + 1u))
        return NV_GSP_RX_TRACK_SEQUENCE;

    NVGspRpcReceiveTracker staged = *tracker;
    staged.active = true;
    staged.function = transport->function;
    staged.sequence = transport->sequence;
    staged.expectedRecordBytes = transport->responseRecordBytes;
    staged.expectedResponseElements = transport->responseElementCount;
    // RX availability is bound to the planned reader and ring capacity only;
    // the planned writer/available count is NOT evidence that a record exists.
    staged.expectedReadPtr = transport->responseAvailable.readPtr;
    staged.rxMessageCount = transport->responseAvailable.messageCount;
    staged.expectedConsumeTail = transport->responseConsume.messageTailValue;
    staged.asyncRecordCount = 0u;
    staged.awaitingConsume = false;
    staged.requestPublished = false;
    staged.recordAvailable = false;
    staged.recordMatched = false;
    staged.recordSucceeded = false;
    *tracker = staged;
    return NV_GSP_RX_TRACK_OK;
}

// This is a separate, one-shot boundary after Begin(): a planned request is
// never considered receivable solely because a caller has filled a transport
// plan. A real provider must report that the matching command queue write
// pointer was actually published. The boolean below is ONLY a caller-provided
// witness; this helper neither writes TX, authenticates an RM channel, nor
// generates proof that a GPU ever observed the request. An ambiguous identity
// is terminal quarantine rather than a chance to relabel the active request.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerRecordPublication(
    NVGspRpcReceiveTracker *tracker,
    NVGspRxTrackU32 observedFunction,
    NVGspRxTrackU32 observedSequence,
    bool providerConfirmedPublished)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->stopped) return NV_GSP_RX_TRACK_STOPPED;
    if (tracker->quarantined) return NV_GSP_RX_TRACK_QUARANTINED;
    if (!tracker->active) return NV_GSP_RX_TRACK_NO_REQUEST;
    if (tracker->awaitingConsume) return NV_GSP_RX_TRACK_AWAIT_CONSUME;
    if (tracker->requestPublished) return NV_GSP_RX_TRACK_PUBLISH_DUPLICATE;
    if (observedFunction != tracker->function ||
        observedSequence != tracker->sequence) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_PUBLISH_IDENTITY;
    }
    if (!providerConfirmedPublished)
        return NV_GSP_RX_TRACK_PUBLISH_UNCONFIRMED;
    tracker->requestPublished = true;
    return NV_GSP_RX_TRACK_OK;
}

// TX publication still does not prove that any RX record exists. Before each
// record read the provider must report a fresh, normalized monotonic
// MSGQ_HEAD writer value (after its own load fence). One witness authorizes
// exactly one classification attempt for the record that begins at the
// tracker's expected RX read pointer; the planned transport writer and its
// available count are never reused as this evidence. The boolean is ONLY a
// caller-provided witness: this helper performs no MMIO read, fence, polling,
// sleep or retry. Fewer complete elements than the single-flight record
// geometry authorizes no read and changes no state; the caller must obtain a
// newer witness. A writer beyond the planned ring capacity from the expected
// reader is an inconsistent queue and is permanent quarantine.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerRecordAvailability(
    NVGspRpcReceiveTracker *tracker,
    NVGspRxTrackU32 observedHead,
    bool providerHeadObserved)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->stopped) return NV_GSP_RX_TRACK_STOPPED;
    if (tracker->quarantined) return NV_GSP_RX_TRACK_QUARANTINED;
    if (!tracker->active) return NV_GSP_RX_TRACK_NO_REQUEST;
    if (tracker->awaitingConsume) return NV_GSP_RX_TRACK_AWAIT_CONSUME;
    if (!tracker->requestPublished) return NV_GSP_RX_TRACK_NOT_PUBLISHED;
    if (tracker->recordAvailable) return NV_GSP_RX_TRACK_HEAD_DUPLICATE;
    if (!providerHeadObserved) return NV_GSP_RX_TRACK_HEAD_UNCONFIRMED;
    if (!tracker->rxMessageCount || !tracker->expectedResponseElements ||
        tracker->expectedResponseElements > tracker->rxMessageCount) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_HEAD_INVALID;
    }
    // NvU32 counters: writer minus reader is the modulo-2^32 available span.
    const NVGspRxTrackU32 available = static_cast<NVGspRxTrackU32>(
        observedHead - tracker->expectedReadPtr);
    if (available > tracker->rxMessageCount) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_HEAD_INVALID;
    }
    if (available < tracker->expectedResponseElements)
        return NV_GSP_RX_TRACK_HEAD_EMPTY;
    tracker->recordAvailable = true;
    return NV_GSP_RX_TRACK_OK;
}

// Classify only a complete caller-provided RX record made readable by one
// fresh provider RX-head witness. A matched result is *provisional* until the
// provider independently confirms RX tail retirement. A pending sentinel is
// not an error reply and must never advance the tail; it spends the witness,
// so the unretired record is re-read only under a newer RX-head report.
// A same-function OR same-sequence partial mismatch is conservatively
// quarantined: it might be a stale reply, not a dispatchable async event.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerObserve(
    NVGspRpcReceiveTracker *tracker,
    const NVGspRespU32 prefix[NV_GSP_RPC_PREFIX_DWORDS],
    NVGspRxTrackU32 recordBytes)
{
    if (!tracker || !prefix) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->stopped) return NV_GSP_RX_TRACK_STOPPED;
    if (tracker->quarantined) return NV_GSP_RX_TRACK_QUARANTINED;
    if (!tracker->active) return NV_GSP_RX_TRACK_NO_REQUEST;
    if (tracker->awaitingConsume) return NV_GSP_RX_TRACK_AWAIT_CONSUME;
    if (!tracker->requestPublished) return NV_GSP_RX_TRACK_NOT_PUBLISHED;
    if (!tracker->recordAvailable) return NV_GSP_RX_TRACK_NOT_AVAILABLE;

    // Do not classify a response against a different RX record geometry than
    // the single-flight transport. A different number of queue bytes might
    // consume additional/missing slots or be an unplanned asynchronous frame.
    // Without a separate event-specific RX consume witness, quarantine it.
    if (recordBytes != tracker->expectedRecordBytes ||
        !tracker->expectedResponseElements) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_GEOMETRY_MISMATCH;
    }

    NVGspRpcResponsePlan receipt = {};
    const NVGspRespU32 result = nvGspValidateRpcResponseNoCc(
        tracker->function,tracker->sequence,prefix,recordBytes,&receipt);
    if (result == NV_GSP_RPC_RESPONSE_RESULT_PENDING) {
        tracker->recordAvailable = false;
        return NV_GSP_RX_TRACK_RESULT_PENDING;
    }

    // Invalid RX bytes leave request ownership uncertain, not resettable.
    if (result != NV_GSP_RPC_RESPONSE_MATCHED_SUCCESS &&
        result != NV_GSP_RPC_RESPONSE_MATCHED_ERROR &&
        result != NV_GSP_RPC_RESPONSE_ASYNC_EVENT) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_MALFORMED;
    }

    if (result == NV_GSP_RPC_RESPONSE_ASYNC_EVENT &&
        (receipt.function == tracker->function ||
         receipt.sequence == tracker->sequence)) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_ID_COLLISION;
    }
    if (result == NV_GSP_RPC_RESPONSE_ASYNC_EVENT &&
        tracker->asyncRecordCount == NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_QUARANTINED;
    }

    NVGspRpcReceiveTracker staged = *tracker;
    staged.awaitingConsume = true;
    staged.recordAvailable = false;
    staged.recordMatched = nvGspRpcResponseMatched(result);
    staged.recordSucceeded = nvGspRpcResponseSucceeded(result);
    *tracker = staged;
    return result == NV_GSP_RPC_RESPONSE_ASYNC_EVENT
        ? NV_GSP_RX_TRACK_ASYNC_EVENT
        : staged.recordSucceeded ? NV_GSP_RX_TRACK_MATCHED_SUCCESS
                                 : NV_GSP_RX_TRACK_MATCHED_ERROR;
}

// RX tail retirement needs an explicit provider-supplied publication witness
// bound to the exact monotonic MSGQ_TAIL value from the planned consume span.
// This pure helper neither writes the tail nor authenticates the provider:
// a correct caller-supplied value is NOT a physical device completion receipt.
// Unconfirmed tail writes leave the record pending; a confidently published
// but inconsistent tail permanently quarantines ownership.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerConfirmConsumed(
    NVGspRpcReceiveTracker *tracker,
    NVGspRxTrackU32 observedTail,
    bool providerTailPublished)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->stopped) return NV_GSP_RX_TRACK_STOPPED;
    if (tracker->quarantined) return NV_GSP_RX_TRACK_QUARANTINED;
    if (!tracker->active) return NV_GSP_RX_TRACK_NO_REQUEST;
    if (!tracker->awaitingConsume) return NV_GSP_RX_TRACK_AWAIT_CONSUME;
    if (!providerTailPublished) return NV_GSP_RX_TRACK_TAIL_UNCONFIRMED;
    if (observedTail != tracker->expectedConsumeTail) {
        tracker->quarantined = true;
        return NV_GSP_RX_TRACK_TAIL_MISMATCH;
    }

    NVGspRpcReceiveTracker staged = *tracker;
    staged.awaitingConsume = false;
    if (!staged.recordMatched) {
        ++staged.asyncRecordCount;
        // Each consumed asynchronous record advances the monotonic RX reader
        // and TAIL; NvU32 rollover is intentional. The next record begins at
        // the retired tail and needs its own fresh provider RX-head witness.
        staged.expectedReadPtr = staged.expectedConsumeTail;
        staged.expectedConsumeTail = static_cast<NVGspRxTrackU32>(
            staged.expectedConsumeTail + staged.expectedResponseElements);
        staged.recordAvailable = false;
        staged.recordSucceeded = false;
        *tracker = staged;
        return NV_GSP_RX_TRACK_ASYNC_EVENT;
    }
    staged.active = false;
    staged.requestPublished = false;
    staged.hasLastSequence = true;
    staged.lastSequence = staged.sequence;
    const NVGspRxTrackU32 result = staged.recordSucceeded
        ? NV_GSP_RX_TRACK_MATCHED_SUCCESS : NV_GSP_RX_TRACK_MATCHED_ERROR;
    staged.recordMatched = false;
    staged.recordSucceeded = false;
    *tracker = staged;
    return result;
}

// Ambiguous retirement or loss of the provider is permanent quarantine.
// There is intentionally no reset, rewind or retry with the same sequence.
static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerRetirementUncertain(
    NVGspRpcReceiveTracker *tracker)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (!tracker->active) return NV_GSP_RX_TRACK_NO_REQUEST;
    tracker->quarantined = true;
    return NV_GSP_RX_TRACK_QUARANTINED;
}

static inline NVGspRxTrackU32 nvGspRpcReceiveTrackerStop(
    NVGspRpcReceiveTracker *tracker)
{
    if (!tracker) return NV_GSP_RX_TRACK_NULL;
    if (!tracker->initialized) return NV_GSP_RX_TRACK_UNINITIALIZED;
    if (tracker->active || tracker->awaitingConsume)
        tracker->quarantined = true;
    tracker->stopped = true;
    return tracker->quarantined
        ? NV_GSP_RX_TRACK_QUARANTINED : NV_GSP_RX_TRACK_OK;
}

#endif // NV_GSP_RPC_RECEIVE_TRACKER_HPP
