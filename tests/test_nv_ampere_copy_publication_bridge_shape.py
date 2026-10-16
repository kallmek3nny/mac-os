#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "phase0-kext" / "NVAmpereCopyPublicationBridge.hpp"
text = HEADER.read_text(encoding="utf-8")

required = [
    "nvAmpereEvaluateChannel",
    "nvAmpereBuildCopyPublicationBridge",
    "nvIntelBuildSharedCopyTransactionAnchor",
    "copyRingEntryStored",
    "copyGpPutPublished",
    "copySubmissionKicked",
    "trackingFenceRingEntryStored",
    "trackingFenceGpPutPublished",
    "trackingFenceSubmissionKicked",
    "completionObservationMayBegin",
    "runtimeWriteAuthorized = false",
]
for needle in required:
    assert needle in text, f"missing publication bridge invariant: {needle}"

# Publication ordering must be enforced before the inherited provenance anchor
# is minted. The future executor may report milestones, but this bridge never
# receives the provider's raw submission objects or addresses.
assert "!e.copySubmissionKicked" in text
assert "!e.trackingFenceGpPutPublished || !e.copySubmissionKicked" in text
assert text.index("nvAmpereCopyPublicationSequenceValid(evidence)") < text.index(
    "nvIntelBuildSharedCopyTransactionAnchor"
)

for forbidden in (
    "srcGpuVa",
    "dstGpuVa",
    "dstPeerDma",
    "pushGpuVa",
    "fenceGpuVa",
    "iovmAddress",
    "gpuAddress",
    "gpuVa",
    "peerIova",
    "NVAmpereSubmissionPlan",
    "NVAmpereFencePlan",
    "NVAmpereRingWritePlan",
    "gpfifoEntry",
    "commandStream",
    "registerWrites",
    "writeSteps",
    "writeRegister",
    "mmioWrite",
    "write32(",
    "write64(",
):
    assert forbidden not in text, f"forbidden raw/write surface: {forbidden}"

print("GA107 copy publication bridge source-shape checks passed")
