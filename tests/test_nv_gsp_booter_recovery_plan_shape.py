#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path

path = Path("phase0-kext/NVGspBooterRecoveryPlan.hpp")
text = path.read_text(encoding="utf-8")

required = [
    "firmwareStagingMayStillBeReferenced",
    "wprMetaMayStillBeReferenced",
    "laterDmaIdleCanRetireFirmwareStaging",
    "mailboxReadAfterTimeoutProvesCompletion = false",
    "resetAloneProvesRetirement = false",
    "postFailureResetAllowsBackingRelease = false",
    "retryAuthorized = false",
    "runtimeRecoveryAuthorized = false",
    "runtimeWriteAuthorized = false",
]
for needle in required:
    if needle not in text:
        raise SystemExit(f"missing recovery safety shape: {needle}")

for forbidden in [
    "GPU_REG_WR",
    "kflcnRegWrite",
    "IOWrite32",
    "writeRegister",
    "registerWrites[",
    "commandStream",
    "gpuAddress",
]:
    if forbidden in text:
        raise SystemExit(f"recovery planner unexpectedly exposes/writes hardware shape: {forbidden}")

if "NVGspBooterExecSequencePlan" not in text:
    raise SystemExit("recovery planner must compose the frozen execution sequence contract")

print("GA107 SEC2 recovery planner source shape: ok")
