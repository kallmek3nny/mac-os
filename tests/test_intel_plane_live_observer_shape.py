#!/usr/bin/env python3
"""Pin the read-only runtime wiring for the Tiger Lake plane-live observer."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "phase0-kext" / "IntelDisplayProbe.cpp"
HPP = ROOT / "phase0-kext" / "IntelDisplayProbe.hpp"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def method_body(src: str, signature: str) -> str:
    start = src.find(signature)
    if start < 0:
        fail(f"missing method signature: {signature}")
    brace = src.find("{", start)
    if brace < 0:
        fail(f"missing opening brace for: {signature}")
    depth = 1
    i = brace + 1
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    if depth:
        fail(f"unbalanced method body: {signature}")
    return src[brace + 1:i - 1]


def ordered(body: str, needles: list[str], label: str) -> None:
    pos = -1
    for needle in needles:
        nxt = body.find(needle, pos + 1)
        if nxt < 0:
            fail(f"{label}: missing {needle!r}")
        if nxt <= pos:
            fail(f"{label}: {needle!r} is out of order")
        pos = nxt


cpp = CPP.read_text(encoding="utf-8")
hpp = HPP.read_text(encoding="utf-8")

if '#include "IntelPlaneLiveObserve.hpp"' not in hpp:
    fail("IntelDisplayProbe.hpp does not include IntelPlaneLiveObserve.hpp")
if "void   publishPlaneLiveObservation(UInt32 pipe);" not in hpp:
    fail("IntelDisplayProbe.hpp lacks publishPlaneLiveObservation declaration")

scan = method_body(cpp, "void IntelDisplayProbe::scanPlanes(void)")
if scan.count("publishPlane(pipe);") != 1:
    fail("scanPlanes must call publishPlane(pipe) exactly once")
if scan.count("publishPlaneLiveObservation(pipe);") != 1:
    fail("scanPlanes must call publishPlaneLiveObservation(pipe) exactly once")
if scan.find("publishPlane(pipe);") > scan.find("publishPlaneLiveObservation(pipe);"):
    fail("legacy publishPlane sample must run before live observation")

body = method_body(cpp, "void IntelDisplayProbe::publishPlaneLiveObservation(UInt32 pipe)")

if re.search(r"\bregWrite\w*\b", body):
    fail("live observer runtime method must not contain any regWrite path")
if "fBar0Base" in body or "volatile" in body or "igPlaneWriteOffset" in body:
    fail("live observer runtime method reached a write-capable/raw-MMIO primitive")
if body.count("IG_PLANE_SURFLIVE") != 2:
    fail("live observer must read PLANE_SURFLIVE exactly twice")
if body.count("regRead32(") != 10:
    fail("live observer must perform exactly ten bounded MMIO reads")

ordered(
    body,
    [
        "sample.surfLiveBefore = regRead32",
        "sample.transConf = regRead32",
        "sample.pipeSrc = regRead32",
        "sample.ctl = regRead32",
        "sample.stride = regRead32",
        "sample.pos = regRead32",
        "sample.size = regRead32",
        "sample.surf = regRead32",
        "sample.offset = regRead32",
        "sample.surfLiveAfter = regRead32",
        "igPlaneLiveObserve(&sample, &observed)",
    ],
    "sample/read order",
)

raw_suffixes = [
    "1-LiveObserve",
    "1-LiveWroteRegisters",
    "1-LiveBefore",
    "1-LiveAfter",
    "1-LiveTransConf",
    "1-LivePipeSrc",
    "1-LiveCtl",
    "1-LiveStride",
    "1-LivePos",
    "1-LiveSize",
    "1-LiveSurf",
    "1-LiveOffset",
]
success_suffixes = [
    "1-LiveWidth",
    "1-LiveHeight",
    "1-LiveStrideBytes",
    "1-LiveSurfaceWord",
    "1-LiveSurfaceOffset",
    "1-LiveInherit",
    "1-LiveCoherent",
]

for suffix in raw_suffixes + success_suffixes:
    if body.count(f'"{suffix}"') != 1:
        fail(f"property suffix {suffix!r} must appear exactly once")
    # IGPlane + one-character A..D pipe + suffix + NUL must fit the existing
    # 32-byte key buffer for every pipe this observer scans.
    if len("IGPlane") + 1 + len(suffix) + 1 > 32:
        fail(f"property suffix {suffix!r} exceeds IG_KEY_BUFFER_BYTES")

wrote = body.find('"1-LiveWroteRegisters"')
false_value = body.find("setProperty(key, false);", wrote)
if wrote < 0 or false_value < 0:
    fail("LiveWroteRegisters must publish the literal false")

guard = body.find("if (!igPlaneLiveSucceeded(verdict))")
if guard < 0:
    fail("missing refusal guard before decoded live-state publication")
ret = body.find("return;", guard)
if ret < 0:
    fail("live-observer refusal guard must return")

for suffix in raw_suffixes:
    if body.find(f'"{suffix}"') > guard:
        fail(f"raw diagnostic {suffix!r} is incorrectly success-only")
for suffix in success_suffixes:
    if body.find(f'"{suffix}"') < ret:
        fail(f"decoded property {suffix!r} is published before refusal return")

if body.find("igPlaneLiveObserve(&sample, &observed)") > body.find('"1-LiveObserve"'):
    fail("runtime publishes the observer verdict before computing it")

# Guard against accidentally changing the older four-word plane publisher into
# a write-capable path while adding this second sample.
legacy = method_body(cpp, "void IntelDisplayProbe::publishPlane(UInt32 pipe)")
if re.search(r"\bregWrite\w*\b", legacy):
    fail("legacy publishPlane unexpectedly gained a write path")

print("Intel plane live observer runtime shape: all checks passed")
