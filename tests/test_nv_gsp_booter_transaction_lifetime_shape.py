#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

HEADER = Path("phase0-kext/NVGspBooterTransactionLifetime.hpp")
text = HEADER.read_text(encoding="utf-8")

required = [
    "NV_GSP_BOOTER_TXN_PHASE_PRE_EXECUTION",
    "NV_GSP_BOOTER_TXN_PHASE_RESET_READY",
    "NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_IN_FLIGHT",
    "NV_GSP_BOOTER_TXN_PHASE_FIRMWARE_DMA_RETIRED",
    "NV_GSP_BOOTER_TXN_PHASE_WPR_META_PUBLISHED",
    "NV_GSP_BOOTER_TXN_PHASE_BOOT_RUNNING",
    "NV_GSP_BOOTER_TXN_PHASE_HALT_OBSERVED",
    "NV_GSP_BOOTER_TXN_PHASE_SUCCESS_RETIRED",
    "NV_GSP_BOOTER_TXN_PHASE_FAILURE_PRE_DMA",
    "NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_QUARANTINE",
    "NV_GSP_BOOTER_TXN_PHASE_FAILURE_DMA_RETIRED",
    "NV_GSP_BOOTER_TXN_PHASE_FAILURE_BOOT_QUARANTINE",
    "firmwareStagingMustRemainPinned",
    "wprMetaMustRemainPinned",
    "firmwareStagingReleaseAuthorized",
    "wprMetaReleaseAuthorized",
    "mailboxReadAfterTimeoutProvesCompletion = false",
    "resetAloneProvesRetirement = false",
    "recoveryGuardMayBeDismissed = false",
    "staged.retryAuthorized = false",
    "staged.runtimeRecoveryAuthorized = false",
    "staged.runtimeWriteAuthorized = false",
]
for token in required:
    if token not in text:
        print(f"missing required transaction lifetime token: {token}")
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
        print(f"forbidden write/raw-address surface in pure transaction planner: {token}")
        sys.exit(1)

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    print("runtime write authorization must be assigned false exactly once")
    sys.exit(1)
if text.count("staged.runtimeRecoveryAuthorized = false;") != 1:
    print("runtime recovery authorization must be assigned false exactly once")
    sys.exit(1)
if text.count("staged.retryAuthorized = false;") != 1:
    print("retry authorization must be assigned false exactly once")
    sys.exit(1)

idle_gate = text.find("progress->allFirmwareDmaIdleObserved && !progress->firmwareDmaSubmitted")
wpr_gate = text.find("progress->wprMetaAddressPublished && !progress->allFirmwareDmaIdleObserved")
boot_gate = text.find("progress->bootStarted && !progress->wprMetaAddressPublished")
halt_gate = text.find("progress->haltObservedWithinBudget && !progress->bootStarted")
atomic_assign = text.find("*out = staged;")
if min(idle_gate, wpr_gate, boot_gate, halt_gate, atomic_assign) < 0:
    print("missing monotonic lifetime/atomicity anchors")
    sys.exit(1)

print("pure ga107 sec2 transaction lifetime planner shape ok")
