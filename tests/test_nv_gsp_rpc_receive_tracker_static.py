#!/usr/bin/env python3
"""Source-shape safety for GSP response/receive order without device I/O."""
from pathlib import Path

response = Path("phase0-kext/NVGspRpcResponse.hpp").read_text(encoding="utf-8")
tracker = Path("phase0-kext/NVGspRpcReceiveTracker.hpp").read_text(encoding="utf-8")
for item in (
    "NV_GSP_RPC_RESPONSE_RESULT_PENDING",
    "NV_GSP_RPC_RESULT_PENDING",
    "nvGspRpcTransportIdentityConsistent",
    "transport.requestFrame.prefix[9] == transport.function",
    "transport.requestFrame.prefix[12] == transport.sequence",
):
    assert item in response, f"missing response guard {item}"

pending = response.index("if (prefix[10] == NV_GSP_RPC_RESULT_PENDING)")
publish = response.index("*out = staged;",pending)
assert pending < publish, "unfinished response published to output"

for item in (
    "tracker->initialized",
    "tracker->quarantined",
    "tracker->active",
    "tracker->awaitingConsume",
    "tracker->lastSequence + 1u",
    "NV_GSP_RX_TRACK_ID_COLLISION",
    "NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS",
    "nvGspRpcReceiveTrackerRetirementUncertain",
    "nvGspRpcReceiveTrackerConfirmConsumed",
):
    assert item in tracker, f"missing tracker guard {item}"
observe = tracker.split("nvGspRpcReceiveTrackerObserve(",1)[1].split(
    "nvGspRpcReceiveTrackerConfirmConsumed(",1
)[0]
assert "nvGspValidateRpcResponseNoCc(" in observe
assert "NV_GSP_RPC_RESPONSE_RESULT_PENDING" in observe
assert "tracker->quarantined = true;" in observe
assert "staged.awaitingConsume = true;" in observe
assert "staged.active = false;" not in observe
confirm = tracker.split("nvGspRpcReceiveTrackerConfirmConsumed(",1)[1].split(
    "nvGspRpcReceiveTrackerRetirementUncertain(",1
)[0]
assert "if (!tracker->awaitingConsume)" in confirm
assert "staged.active = false;" in confirm
assert "staged.lastSequence = staged.sequence;" in confirm

# Neither host-only layer may acquire/write GPU resources or export secrets.
for name,text in (("response",response),("receive tracker",tracker)):
    for dangerous in (
        "OSWriteLittleInt", "OSWriteBigInt", "configWrite",
        "setBusMasterEnable", "setMemoryEnable", "regWrite",
        "IODMACommand", "IOMapper", "setProperty(",
        "gpuVa", "iova", "Iova", "submissionToken",
    ):
        assert dangerous not in text, f"{name}: forbidden device/public operation {dangerous}"
assert "NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS 63u" in tracker
assert "nvGspRpcReceiveTrackerStop(" in tracker
assert "tracker->stopped = true;" in tracker
assert "tracker->quarantined = true;" in tracker
# Transport RX placement/consumption geometry and single-flight record size
# must be checked before response classification, not after RX retirement.
for needle in (
    "transport.responseRecordBytes >= NV_GSP_RPC_PREFIX_BYTES",
    "transport.responseElementCount ==",
    "transport.responseConsume.oldReadPtr == transport.responseAvailable.readPtr",
    "transport.responseConsume.remainingAvailable ==",
    "nvGspPlanQueueRxElement(&transport.responseAvailable,i,&expected)",
):
    assert needle in response, f"missing transport RX geometry validation: {needle}"
assert "staged.expectedRecordBytes = transport->responseRecordBytes;" in tracker
assert "staged.expectedResponseElements = transport->responseElementCount;" in tracker
assert "recordBytes != tracker->expectedRecordBytes" in observe
assert observe.index("recordBytes != tracker->expectedRecordBytes") < observe.index(
    "nvGspValidateRpcResponseNoCc(")
assert "NV_GSP_RX_TRACK_GEOMETRY_MISMATCH" in observe
# A Begin() receipt is a purely planned request, not an actual TX publication.
# The matching provider-supplied publication fact is a required independent
# transition before any RX record may be semantically classified.
publish = tracker.split("nvGspRpcReceiveTrackerRecordPublication(", 1)[1].split(
    "nvGspRpcReceiveTrackerRecordAvailability(", 1
)[0]
# TX publication is not RX availability. One fresh provider-reported RX HEAD
# (MSGQ_HEAD writer) witness must arm each record read at the expected reader;
# the planned transport writer/available count is never reused as evidence.
avail = tracker.split("nvGspRpcReceiveTrackerRecordAvailability(", 1)[1].split(
    "nvGspRpcReceiveTrackerObserve(", 1
)[0]
for needle in (
    "NVGspRxTrackU32 observedHead,",
    "bool providerHeadObserved)",
    "if (tracker->awaitingConsume) return NV_GSP_RX_TRACK_AWAIT_CONSUME;",
    "if (!tracker->requestPublished) return NV_GSP_RX_TRACK_NOT_PUBLISHED;",
    "if (tracker->recordAvailable) return NV_GSP_RX_TRACK_HEAD_DUPLICATE;",
    "if (!providerHeadObserved) return NV_GSP_RX_TRACK_HEAD_UNCONFIRMED;",
    "observedHead - tracker->expectedReadPtr",
    "if (available > tracker->rxMessageCount)",
    "if (available < tracker->expectedResponseElements)",
    "return NV_GSP_RX_TRACK_HEAD_EMPTY;",
    "return NV_GSP_RX_TRACK_HEAD_INVALID;",
    "tracker->quarantined = true;",
    "tracker->recordAvailable = true;",
):
    assert needle in avail, f"missing RX head availability guard {needle}"
assert avail.count("tracker->recordAvailable = true;") == 1
arm = avail.index("tracker->recordAvailable = true;")
for gate in (
    "if (tracker->awaitingConsume)",
    "if (!tracker->requestPublished)",
    "if (tracker->recordAvailable)",
    "if (!providerHeadObserved)",
    "if (available > tracker->rxMessageCount)",
    "if (available < tracker->expectedResponseElements)",
):
    assert avail.index(gate) < arm, f"RX head witness armed before {gate}"
# Begin binds only the planned reader and ring capacity; nothing else from the
# planned RX availability (writer, available count) may reach the tracker.
assert "staged.expectedReadPtr = transport->responseAvailable.readPtr;" in tracker
assert "staged.rxMessageCount = transport->responseAvailable.messageCount;" in tracker
assert tracker.count("responseAvailable.") == 2, "tracker reuses planned RX availability"
assert "staged.recordAvailable = false;" in tracker
assert "if (!tracker->recordAvailable) return NV_GSP_RX_TRACK_NOT_AVAILABLE;" in observe
assert observe.index("if (!tracker->recordAvailable)") < observe.index(
    "nvGspValidateRpcResponseNoCc(")
pendingBranch = observe.split("if (result == NV_GSP_RPC_RESPONSE_RESULT_PENDING)", 1)[1].split(
    "return NV_GSP_RX_TRACK_RESULT_PENDING;", 1
)[0]
assert "tracker->recordAvailable = false;" in pendingBranch, "pending reply keeps its RX head witness"
assert "staged.recordAvailable = false;" in observe
# A consumed async record moves the reader to the retired tail before the
# expected tail advances, and leaves no residual witness for the next record.
assert "staged.recordAvailable = false;" in confirm
assert "staged.expectedReadPtr = staged.expectedConsumeTail;" in confirm
assert confirm.index("staged.expectedReadPtr = staged.expectedConsumeTail;") < confirm.index(
    "staged.expectedConsumeTail = static_cast<NVGspRxTrackU32>(")
assert "staged.requestPublished = false;" in tracker
assert "if (!tracker->requestPublished) return NV_GSP_RX_TRACK_NOT_PUBLISHED;" in observe
assert observe.index("if (!tracker->requestPublished)") < observe.index(
    "nvGspValidateRpcResponseNoCc(")
for needle in (
    "if (!providerConfirmedPublished)",
    "if (observedFunction != tracker->function ||",
    "observedSequence != tracker->sequence)",
    "tracker->quarantined = true;",
    "tracker->requestPublished = true;",
    "NV_GSP_RX_TRACK_PUBLISH_DUPLICATE",
):
    assert needle in publish, f"missing provider publication guard {needle}"
assert "staged.requestPublished = false;" in confirm
# The provider must explicitly witness the exact monotonic RX TAIL value.
# No parameterless helper may retire a matched reply using planned geometry.
assert "NVGspRxTrackU32 observedTail," in confirm
assert "bool providerTailPublished)" in confirm
assert "if (!providerTailPublished) return NV_GSP_RX_TRACK_TAIL_UNCONFIRMED;" in confirm
assert "if (observedTail != tracker->expectedConsumeTail)" in confirm
assert confirm.index("if (observedTail != tracker->expectedConsumeTail)") < confirm.index(
    "NVGspRpcReceiveTracker staged = *tracker;")
assert "tracker->quarantined = true;" in confirm
assert "staged.expectedConsumeTail = static_cast<NVGspRxTrackU32>(" in confirm
assert "staged.expectedConsumeTail + staged.expectedResponseElements" in confirm
assert "staged.expectedConsumeTail = transport->responseConsume.messageTailValue;" in tracker
print("GA107 GSP RPC receive/retirement safety contract: ok")
