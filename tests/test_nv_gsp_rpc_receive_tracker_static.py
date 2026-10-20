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
print("GA107 GSP RPC receive/retirement safety contract: ok")
