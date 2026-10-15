#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path

path = Path("phase0-kext/IntelPlaneRetirementEvidence.hpp")
text = path.read_text(encoding="utf-8")

required = [
    "IG_PLANE_RETIREMENT_PRIMARY_FLIP_DONE_BIT       (1u << 3)",
    "flipDoneMustBeUnmaskedBeforeArm = true",
    "flipDoneMustBeEnabledBeforeArm = true",
    "staleFlipDoneMustBeClearedBeforeArm = true",
    "boundedCompletionWaitRequired = true",
    "vblankOnlyRetirementProofAccepted = false",
    "surfLiveOnlyRetirementProofAccepted = false",
    "targetMappingReleaseAuthorized = false",
    "rollbackSafe = false",
    "runtimeWriteAuthorized = false",
]
for needle in required:
    if needle not in text:
        raise SystemExit(f"missing retirement safety shape: {needle}")

for forbidden in [
    "IOWrite32",
    "writeRegister",
    "mmioWrite",
    "registerWrites[",
    "commandStream",
    "gpuAddress",
    "MI_DISPLAY_FLIP",
]:
    if forbidden in text:
        raise SystemExit(f"retirement planner unexpectedly exposes/writes hardware shape: {forbidden}")

if "IntelPlaneTransitionPlan.hpp" not in text:
    raise SystemExit("retirement planner must compose the frozen transition contract")

print("Intel plane retirement-evidence source shape: ok")
