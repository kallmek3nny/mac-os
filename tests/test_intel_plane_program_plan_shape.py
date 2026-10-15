#!/usr/bin/env python3
"""Pin the pure/no-write shape of the Tiger Lake plane programming planner."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "phase0-kext" / "IntelPlaneProgramPlan.hpp"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


text = HEADER.read_text(encoding="utf-8")

for forbidden in (
    '#include "IntelPlaneProgram.hpp"',
    "IgPlaneRegs",
    "igPlaneWriteOffset",
    "IG_FB_OFF_",
    "regWrite",
    "fBar0Base",
    "volatile",
):
    if forbidden in text:
        fail(f"planner reached write-capable/register-programming surface: {forbidden}")

start = text.find("struct IgPlaneProgramPlan {")
if start < 0:
    fail("missing IgPlaneProgramPlan")
end = text.find("};", start)
if end < 0:
    fail("unterminated IgPlaneProgramPlan")
plan = text[start:end]

for forbidden_field in (
    "ggttBase",
    "surfaceAddress",
    "ctlWord",
    "surfWord",
    "registerOffset",
    "writeStep",
):
    if forbidden_field in plan:
        fail(f"semantic output leaks raw programming field: {forbidden_field}")

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    fail("planner must explicitly deny runtime write authorization exactly once")

for required in (
    "staged.sequencingProofRequired = true;",
    "staged.latchProofRequired = true;",
    "staged.mappingPinRequired = true;",
    "staged.rollbackOrQuarantineRequired = true;",
):
    if required not in text:
        fail(f"missing future safety requirement: {required}")

if "IG_PLANE_PROGRAM_PLAN_LINEAR_STRIDE_UNIT_I915     64u" not in text:
    fail("linear stride unit must remain source-backed 64 bytes")
if "IG_PLANE_PROGRAM_PLAN_SURFACE_ALIGN_I915          4096u" not in text:
    fail("Tiger Lake linear PLANE_SURF alignment must remain 4 KiB")

print("Intel plane program-plan source shape: all checks passed")
