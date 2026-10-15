#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/NVGspBooterSuccessRetirementPlan.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "allFirmwareDmaIdleObserved",
    "haltWithinBudget",
    "mailboxReadbackAfterHalt",
    "mailbox0BeforeMailbox1Readback",
    "firmwareStagingReleaseAuthorized = true",
    "wprMetaReleaseAuthorized = true",
    "recoveryGuardMayBeDismissed = true",
    "postSuccessResetRequiredForBackingRelease = false",
    "retryAuthorized = false",
    "runtimeRecoveryAuthorized = false",
    "runtimeWriteAuthorized = false",
    "NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_DMA_NOT_RETIRED",
    "NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_HALT_NOT_PROVEN",
    "NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_MAILBOX_FAILURE",
    "NV_GSP_BOOTER_SUCCESS_RETIREMENT_ERR_READBACK_ORDER",
]

for token in required:
    if token not in text:
        print(f"missing required safety token: {token}")
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
    "wprMetaDmaAddress",
    "firmwareDmaAddress",
    "registerOffset",
    "bar0Base",
    "fBar0Base",
]

for token in forbidden:
    if token in text:
        print(f"forbidden write/address surface in pure planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("runtime write authorization must be assigned false exactly once")
    sys.exit(1)

release_pos = text.find("staged.wprMetaReleaseAuthorized = true;")
halt_check_pos = text.find("if (!observation->haltObserved || !observation->haltWithinBudget)")
mailbox_check_pos = text.find("if (observation->mailbox0Value != sequence->mailbox0SuccessValue)")
if release_pos < 0 or halt_check_pos < 0 or mailbox_check_pos < 0:
    print("missing success-release ordering anchors")
    sys.exit(1)
if not (halt_check_pos < mailbox_check_pos < release_pos):
    print("wpr release must occur only after halt and mailbox success gates")
    sys.exit(1)

if "*out = staged;" not in text:
    print("missing atomic staged assignment")
    sys.exit(1)

print("pure ga107 sec2 success-retirement planner shape ok")
