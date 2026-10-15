#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/NvIntelSharedCopyLifetime.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "NV_INTEL_COPY_PHASE_PRE_PEER",
    "NV_INTEL_COPY_PHASE_PEER_READY",
    "NV_INTEL_COPY_PHASE_COPY_UNTRACKED",
    "NV_INTEL_COPY_PHASE_AWAIT_COMPLETION",
    "NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL",
    "NV_INTEL_COPY_PHASE_UNCERTAIN_QUARANTINE",
    "nvAmpereFenceExtendCompleted",
    "submittedTrackingValue <= progress.completedBeforeSubmission",
    "staged.nvidiaMayReferenceSink =",
    "staged.peerMappingMustRemainPinned = staged.nvidiaMayReferenceSink;",
    "staged.intelHandoffAuthorized = completionProven;",
    "staged.intelGgttMappingMayBegin = completionProven;",
    "staged.sinkBackingMustRemainPinned = true;",
    "staged.quarantineSinkBacking = staged.completionUncertain;",
    "staged.quarantinePeerMapping = staged.completionUncertain;",
    "staged.runtimeWriteAuthorized = false;",
    "*out = staged;",
]
for token in required:
    if token not in text:
        print(f"missing required cross-gpu lifetime safety token: {token}")
        sys.exit(1)

forbidden = [
    "volatile ",
    "regWrite",
    "writeRegister",
    "mmioWrite",
    "write32",
    "write64",
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
]
for token in forbidden:
    if token in text:
        print(f"forbidden write/raw-address surface in pure cross-gpu lifetime planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("runtime write authorization must be assigned false exactly once")
    sys.exit(1)

sequence_gate = text.find("nvIntelSharedCopyProgressSequenceValid")
target_gate = text.find("progress.submittedTrackingValue <= progress.completedBeforeSubmission")
fence_gate = text.find("nvAmpereFenceExtendCompleted")
handoff_gate = text.find("staged.intelHandoffAuthorized = completionProven;")
assign = text.find("*out = staged;")
if min(sequence_gate, target_gate, fence_gate, handoff_gate, assign) < 0:
    print("missing cross-gpu lifetime ordering anchors")
    sys.exit(1)
if not (sequence_gate < target_gate < fence_gate < handoff_gate < assign):
    print("cross-gpu lifetime safety gates are not in expected order")
    sys.exit(1)

print("pure ga107-to-tiger-lake shared copy lifetime planner shape ok")
