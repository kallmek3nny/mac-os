#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/NvIntelSharedSurfaceHandoff.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "NV_INTEL_HANDOFF_PHASE_WAIT_NVIDIA",
    "NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED",
    "NV_INTEL_HANDOFF_PHASE_PEER_TEARDOWN_ACTIVE",
    "NV_INTEL_HANDOFF_PHASE_INTEL_MAPPING_COMMITTED",
    "NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_ARMED",
    "NV_INTEL_HANDOFF_PHASE_INTEL_DISPLAY_LIVE",
    "NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE",
    "NvIntelSharedSurfaceIdentity",
    "copyCompletionIdentity",
    "intelTargetIdentity",
    "nvIntelSharedSurfaceCopyContractValid",
    "nvIntelSharedSurfaceDisplayContractValid",
    "staged.peerMappingSlotReuseAuthorized = progress->peerMappingTeardownSucceeded;",
    "staged.intelHandoffAuthorized = copyRetired;",
    "staged.intelMappingMayBegin = copyRetired;",
    "staged.sharedBackingMustRemainPinned = true;",
    "staged.sharedBackingReleaseAuthorized = false;",
    "staged.runtimeWriteAuthorized = false;",
    "*out = staged;",
]
for token in required:
    if token not in text:
        print(f"missing required shared-surface handoff safety token: {token}")
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
        print(f"forbidden write/raw-address surface in handoff planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("runtime write authorization must be assigned false exactly once")
    sys.exit(1)

identity_gate = text.find("nvIntelSharedSurfaceIdentityPresent")
copy_gate = text.find("nvIntelSharedSurfaceCopyContractValid")
teardown_gate = text.find("nvIntelSharedSurfacePeerTeardownSequenceValid")
premature_gate = text.find("NV_INTEL_HANDOFF_ERR_PREMATURE_INTEL")
release_gate = text.find("staged.sharedBackingReleaseAuthorized = false;")
assign = text.find("*out = staged;")
if min(identity_gate, copy_gate, teardown_gate, premature_gate, release_gate, assign) < 0:
    print("missing handoff ordering anchors")
    sys.exit(1)
if not (copy_gate < identity_gate < teardown_gate < premature_gate < release_gate < assign):
    print("shared-surface handoff safety gates are not in expected order")
    sys.exit(1)

print("pure ga107-to-tiger-lake shared-surface handoff planner shape ok")
