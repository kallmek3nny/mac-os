#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "phase0-kext" / "NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp"
text = HEADER.read_text(encoding="utf-8")

required = [
    "nvIntelBuildSharedCopyTransactionAnchor",
    "nvIntelVerifySharedCopyTransactionAnchor",
    "nvIntelBuildDisplayTransactionProvenanceAnchor",
    "nvIntelVerifyDisplayTransactionProvenanceAnchor",
    "nvIntelSharedSurfaceAnchoredProvenancePlan",
    "progress->boundedCompletionWaitUsed",
    "progress->completionPayloadReadSucceeded",
    "preEvidence->retirementEvidencePresent",
    "preEvidence->retirementProven",
    "NV_INTEL_COPY_PHASE_AWAIT_COMPLETION",
    "IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT",
    "runtimeWriteAuthorized = false",
]
for needle in required:
    assert needle in text, f"missing anchored provenance invariant: {needle}"

# The high-level evidence bundle must carry its expected identities only through
# previously-minted anchors. Do not add sibling expected epoch/token fields next
# to the evidence observations.
match = re.search(
    r"struct NvIntelSharedSurfaceAnchoredProvenance\s*\{(?P<body>.*?)\n\};",
    text,
    flags=re.S,
)
assert match, "anchored provenance input struct missing"
body = match.group("body")
for forbidden_field in (
    "NVIntelHandoffU64 handoffEpoch",
    "NVIntelHandoffU64 copySubmissionToken",
    "NVIntelHandoffU64 intelDisplayTransactionToken",
):
    assert forbidden_field not in body, (
        "evidence bundle must not accept a caller-supplied expected identity: "
        + forbidden_field
    )

# Keep this planner outside the hardware-write and raw-address ABI surfaces.
for forbidden in (
    "writeRegister",
    "mmioWrite",
    "write32(",
    "write64(",
    "registerWrites",
    "writeSteps",
    "commandStream",
    "NVCopyMethod",
    "NVAmpereSubmissionPlan",
    "NVAmpereFencePlan",
    "srcGpuVa",
    "dstPeerDma",
    "pushGpuVa",
    "fenceGpuVa",
    "iovmAddress",
    "gpfifoEntry",
):
    assert forbidden not in text, f"forbidden write/raw-address surface: {forbidden}"

print("anchored cross-gpu provenance source-shape checks passed")
