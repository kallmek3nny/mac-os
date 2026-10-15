#!/usr/bin/env python3
"""Pin the pure/no-write shape of the Tiger Lake plane transition planner."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "phase0-kext" / "IntelPlaneTransitionPlan.hpp"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


text = HEADER.read_text(encoding="utf-8")

for forbidden in (
    '#include "IntelPlaneProgram.hpp"',
    "IgPlaneRegs",
    "igPlaneWriteOffset",
    "regWrite",
    "fBar0Base",
    "volatile",
    "registerWrites",
    "writeSteps",
    "commandStream",
):
    if forbidden in text:
        fail(f"transition planner reached a write-capable/raw programming surface: {forbidden}")

start = text.find("struct IgPlaneTransitionPlan {")
if start < 0:
    fail("missing IgPlaneTransitionPlan")
end = text.find("};", start)
if end < 0:
    fail("unterminated IgPlaneTransitionPlan")
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
        fail(f"semantic transition output leaks raw programming field: {forbidden_field}")

if text.count("staged.runtimeWriteAuthorized = false;") != 1:
    fail("transition planner must explicitly deny runtime write authorization exactly once")

for required in (
    "staged.requiresNoArmProgramming = true;",
    "staged.requiresArmControlWrite = true;",
    "staged.requiresSurfaceArmWrite = true;",
    "staged.armControlImmediatelyBeforeSurface = true;",
    "staged.oldSurfaceMustRemainPinned = true;",
    "staged.newSurfaceMustRemainPinned = true;",
    "staged.retirementProofRequired = true;",
    "staged.flipCompletionRequired = true;",
    "staged.ggttRestoreForbiddenUntilRetirement = true;",
    "staged.postArmRollbackSafe = false;",
    "staged.quarantineOnUncertainCompletion = true;",
    "staged.retainBothSurfacesOnUncertainCompletion = true;",
):
    if required not in text:
        fail(f"missing transition/retirement safety requirement: {required}")

if "IG_PLANE_TRANSITION_ERR_ENABLE_FROM_DISABLED" not in text:
    fail("enable-from-disabled self-arm hazard must be an explicit refusal")
if "IG_PLANE_TRANSITION_ERR_SURFACE_ONLY_UNPROVEN" not in text:
    fail("surface-only classification must remain fail-closed until exact state equality is proven")

print("Intel plane transition-plan source shape: all checks passed")
