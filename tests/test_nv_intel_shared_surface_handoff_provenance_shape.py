#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/NvIntelSharedSurfaceHandoffProvenance.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "NvIntelSharedSurfaceEvidenceProvenance",
    "NvIntelSharedSurfaceHandoffProvenance",
    "handoffEpoch",
    "copySubmissionToken",
    "copyCompletionEvidence",
    "intelDisplayTransactionToken",
    "intelDisplayEvidence",
    "NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY",
    "nvIntelSharedSurfaceEvidenceProvenanceAssess",
    "nvIntelSharedSurfaceApplyProvenanceQuarantine",
    "copy->completionEvidencePresent || copy->completionProven",
    "display->retirementEvidencePresent ||",
    "return NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY;",
    "handoff->copyRetirementProven = false;",
    "const bool peerMappingStillPresent =",
    "!handoff->peerMappingTeardownSucceeded;",
    "handoff->peerMappingMustRemainPinned = peerMappingStillPresent;",
    "handoff->peerMappingSlotReuseAuthorized = false;",
    "handoff->quarantinePeerMapping = peerMappingStillPresent;",
    "handoff->intelHandoffAuthorized = false;",
    "handoff->intelMappingMayBegin = false;",
    "handoff->intelSurfaceIsLiveScanout = false;",
    "handoff->quarantineSharedBacking = true;",
    "handoff->sharedBackingMustRemainPinned = true;",
    "handoff->sharedBackingReleaseAuthorized = false;",
    "staged.runtimeWriteAuthorized = false;",
    "*out = staged;",
]
for token in required:
    if token not in text:
        print(f"missing required handoff provenance safety token: {token}")
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
    "srcGpuVa",
    "dstPeerDma",
    "pushGpuVa",
    "fenceGpuVa",
    "iovmAddress",
    "gpfifoEntry",
    "gpuAddress",
    "rawIova",
]
for token in forbidden:
    if token in text:
        print(f"forbidden write/raw-address surface in provenance planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("provenance runtime write authorization must be assigned false exactly once")
    sys.exit(1)

planner = text.find("nvIntelSharedSurfaceHandoffProvenancePlan(")
base_gate = text.find("nvIntelSharedSurfaceHandoffPlan(copy, progress, display, &base)", planner)
copy_assess = text.find("copyEvidenceAssessment", base_gate)
copy_replay = text.find("return NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY;", copy_assess)
intel_assess = text.find("intelEvidenceAssessment", copy_replay)
intel_replay = text.find("return NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY;", intel_assess)
quarantine = text.find("nvIntelSharedSurfaceApplyProvenanceQuarantine(", intel_replay)
runtime_gate = text.find("staged.runtimeWriteAuthorized = false;", quarantine)
assign = text.find("*out = staged;", runtime_gate)
if min(planner, base_gate, copy_assess, copy_replay, intel_assess,
       intel_replay, quarantine, runtime_gate, assign) < 0:
    print("missing provenance planner ordering anchors")
    sys.exit(1)
if not (planner < base_gate < copy_assess < copy_replay < intel_assess <
        intel_replay < quarantine < runtime_gate < assign):
    print("handoff provenance replay/quarantine gates are not in expected order")
    sys.exit(1)

print("pure cross-gpu handoff provenance anti-replay planner shape ok")
