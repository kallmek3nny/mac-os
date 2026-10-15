#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/IntelDisplayTransactionLifetime.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "IG_DISPLAY_TXN_PHASE_PRE_MAPPING",
    "IG_DISPLAY_TXN_PHASE_MAPPING_COMMITTED",
    "IG_DISPLAY_TXN_PHASE_NOARM_STAGED",
    "IG_DISPLAY_TXN_PHASE_ARM_CONTROL_STAGED",
    "IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT",
    "IG_DISPLAY_TXN_PHASE_RETIRED_LIVE",
    "IG_DISPLAY_TXN_PHASE_UNCERTAIN_QUARANTINE",
    "targetMappingExactReadbackVerified",
    "noArmRollbackProofRequiredForRestore",
    "ggttRestoreBlockedByDisplayState",
    "oldSurfaceBackingReleaseAuthorized",
    "newSurfaceBackingReleaseAuthorized = false",
    "targetMappingReleaseAuthorized = false",
    "quarantineOldSurface = true",
    "quarantineNewSurface = true",
    "quarantineTargetMapping = true",
    "newSurfaceIsLiveScanout",
    "staged.runtimeWriteAuthorized = false;",
]
for token in required:
    if token not in text:
        print(f"missing required lifetime safety token: {token}")
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
    "surfaceAddress",
    "ggttBase",
    "registerOffset",
    "firstPte",
    "pteCount",
    "fBar0Base",
    "IgPlaneRegs",
    "igPlaneWriteOffset",
]
for token in forbidden:
    if token in text:
        print(f"forbidden write/raw-address surface in pure lifetime planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("runtime write authorization must be assigned false exactly once")
    sys.exit(1)

mapping_gate = text.find("progress->targetMappingCommitSucceeded &&\n        (!progress->ggttTransactionPrepared ||\n         !progress->targetMappingExactReadbackVerified)")
restore_gate = text.find("staged.ggttRestoreMayBeAttempted =")
noarm_gate = text.find("staged.noArmRollbackProofRequiredForRestore =")
arm_phase = text.find("IG_DISPLAY_TXN_PHASE_ARMED_AWAIT_RETIREMENT")
retired_phase = text.find("IG_DISPLAY_TXN_PHASE_RETIRED_LIVE")
if min(mapping_gate, restore_gate, noarm_gate, arm_phase, retired_phase) < 0:
    print("missing transaction lifetime ordering anchors")
    sys.exit(1)

if "*out = staged;" not in text:
    print("missing atomic staged assignment")
    sys.exit(1)

print("pure tiger lake display transaction lifetime planner shape ok")
