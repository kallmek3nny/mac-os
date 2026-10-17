#!/usr/bin/env python3
from pathlib import Path
import re

path = Path("phase0-kext/NVAmpereChannelRuntime.hpp")
text = path.read_text(encoding="utf-8")

match = re.search(
    r"struct NVAmpereChannelRuntime\s*\{(?P<body>.*?)\n\};",
    text,
    flags=re.S,
)
assert match, "channel runtime struct not found"
body = match.group("body")

# Provider-owned channel state is presence/readiness only. Raw submission
# authority remains private to a future executor.
for forbidden in (
    "GpuVa",
    "GPUVA",
    "gpuVa",
    "Iova",
    "IOVA",
    "iova",
    "DmaAddress",
    "dmaAddress",
    "gpfifoEntry",
    "pushbuffer",
    "registerWrite",
    "mmio",
    "providerPointer",
    "workSubmissionTokenValue",
):
    assert forbidden not in body, f"forbidden channel-runtime field surface: {forbidden}"

assert "NVAmpereChannelEvidence evidence;" in body
assert "NVAmpereChannelReadiness readiness;" in body
assert "bool allocationObserved;" in body
assert "bool submissionEndpointObserved;" in body
assert "bool sealed;" in body
assert "bool runtimeWriteAuthorized;" in body

# Facts must arrive in two monotonic stages and be sealed through the existing
# readiness evaluator before they can leave this provider-owned runtime.
allocation = text.index("nvAmpereChannelRuntimeObserveAllocation")
submission = text.index("nvAmpereChannelRuntimeObserveSubmissionEndpoint")
seal = text.index("nvAmpereChannelRuntimeSeal")
copy = text.index("nvAmpereChannelRuntimeCopyEvidence")
assert allocation < submission < seal < copy
assert "nvAmpereEvaluateChannel(runtime->evidence, &readiness)" in text
assert "nvAmpereChannelRuntimeReady(runtime)" in text
assert "NV_CHANNEL_RUNTIME_ERR_NOT_SEALED" in text

# Observation APIs expose only class/count and booleans. There is no raw token,
# address, command, ordinal, or write parameter to smuggle through the seam.
allocation_sig = re.search(
    r"nvAmpereChannelRuntimeObserveAllocation\s*\((?P<args>.*?)\)\n\{",
    text,
    flags=re.S,
)
submission_sig = re.search(
    r"nvAmpereChannelRuntimeObserveSubmissionEndpoint\s*\((?P<args>.*?)\)\n\{",
    text,
    flags=re.S,
)
assert allocation_sig and submission_sig
for sig in (allocation_sig.group("args"), submission_sig.group("args")):
    for forbidden in ("GpuVa", "Iova", "IOVA", "Address", "TokenValue", "Entry", "Ordinal"):
        assert forbidden not in sig, f"forbidden observation argument: {forbidden}"

# This phase intentionally has no reset/reuse or hardware-write path.
assert "Reset" not in text
assert "Reuse" not in text
for forbidden_write in (
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "writeRegister",
    "regWrite32",
    "setBusMasterEnable",
):
    assert forbidden_write not in text, f"unexpected write API: {forbidden_write}"

print("GA107 provider channel runtime shape: ok")
