#!/usr/bin/env python3
"""Pin the GA107 SEC2 executor planner as pure, semantic, and fail-closed."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "phase0-kext" / "NVGspBooterExecSequencePlan.hpp"
text = HEADER.read_text(encoding="utf-8")


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


for forbidden in (
    "regWrite",
    "regRead",
    "fBar0Base",
    "volatile",
    "NV_PFALCON",
    "NV_PRISCV",
    "registerOffset",
    "writeStep",
    "commandStream",
):
    if forbidden in text:
        fail(f"planner leaks a runtime/register programming surface: {forbidden}")

start = text.find("struct NVGspBooterExecSequencePlan {")
if start < 0:
    fail("missing semantic plan struct")
end = text.find("};", start)
if end < 0:
    fail("unterminated semantic plan struct")
plan = text[start:end]

for forbidden_field in (
    "wprMetaDmaAddress",
    "firmwareDmaAddress",
    "bar0",
    "mmio",
    "register",
    "writeList",
):
    if forbidden_field in plan:
        fail(f"semantic output leaks raw programming field: {forbidden_field}")

required = (
    "NV_GSP_BOOTER_EXEC_DMA_BLOCK_BYTES            256u",
    "NV_GSP_BOOTER_EXEC_RESET_READY_BEST_EFFORT_US 150u",
    "NV_GSP_BOOTER_EXEC_RESET_ASSERT_HOLD_US       10u",
    "NV_GSP_BOOTER_EXEC_CORE_SELECT_TIMEOUT_US     10000u",
    "NV_GSP_BOOTER_EXEC_MEM_SCRUB_TIMEOUT_US       20000u",
    "NV_GSP_BOOTER_EXEC_DMA_IDLE_TIMEOUT_US        2000000u",
    "NV_GSP_BOOTER_EXEC_BOOT_HALT_TIMEOUT_US       2000000u",
    "staged.dmaTargetCoherentSystemMemory = true;",
    "staged.dmaAddressTypePhysical = true;",
    "staged.secureImemBeforeDmem = true;",
    "staged.mailbox0BeforeMailbox1 = true;",
    "staged.startAfterMailboxes = true;",
    "staged.quarantineFirmwareDmaBackingOnTransferTimeout = true;",
    "staged.quarantineWprMetaBackingOnBootTimeout = true;",
    "staged.timeoutRecoveryProven = false;",
    "staged.runtimeWriteAuthorized = false;",
)
for item in required:
    if item not in text:
        fail(f"missing pinned safety/source fact: {item}")

if "staged.runtimeWriteAuthorized = true;" in text:
    fail("pure planner must never authorize runtime writes")

print("GA107 SEC2 exec-sequence source shape: all checks passed")
