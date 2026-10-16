#!/usr/bin/env python3
from pathlib import Path
import re

runtime_path = Path("phase0-kext/NVAmpereCopyPublicationRuntime.hpp")
provider_path = Path("phase0-kext/NVPeerMapperProbe.cpp")
provider_header_path = Path("phase0-kext/NVPeerMapperProbe.hpp")

runtime = runtime_path.read_text(encoding="utf-8")
provider = provider_path.read_text(encoding="utf-8")
provider_header = provider_header_path.read_text(encoding="utf-8")

match = re.search(
    r"struct NVAmpereCopyPublicationRuntime\s*\{(?P<body>.*?)\n\};",
    runtime,
    flags=re.S,
)
assert match, "runtime state struct not found"
body = match.group("body")

# The provider-owned runtime state may carry bounded milestones/counters only.
# Raw addressing, pushbuffer, token and register-write surfaces belong to the
# private executor and must not leak into this handoff state.
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
    "workSubmissionToken",
    "registerWrite",
    "mmio",
):
    assert forbidden not in body, f"forbidden runtime-state field surface: {forbidden}"

assert "bool runtimeWriteAuthorized;" in body
assert "NVCopyRuntimeU64 nextPublicationOrdinal;" in body
assert "NVAmpereCopyPublicationEvidence evidence;" in body

# Ordering witnesses are assigned only by the runtime when provider kicks are
# accepted. A caller must not pass an ordinal into advance().
advance_sig = re.search(
    r"nvAmpereCopyRuntimeAdvance\s*\((?P<args>.*?)\)\n\{",
    runtime,
    flags=re.S,
)
assert advance_sig, "advance function missing"
assert "Ordinal" not in advance_sig.group("args")
assert "copyPublicationOrdinal =" in runtime
assert "trackingFencePublicationOrdinal =" in runtime
assert "nextPublicationOrdinal++" in runtime

# Completion observation must be a post-anchor transition, and there is no
# transaction reset/reuse helper in this phase. Unproven completion therefore
# cannot quietly recycle resources.
assert "nvAmpereCopyRuntimeBeginCompletionObservation" in runtime
assert "!runtime->transactionActive || !runtime->anchorMinted" in runtime
assert "Reset" not in runtime
assert "Reuse" not in runtime

# The real IOKit seam remains exact-device and read-only. Check both probe() and
# start() retain the 10de:25a2 display-function gate before binding mapper proof.
assert provider.count("vendor!=0x10de || device!=0x25a2 || baseClass!=0x03") >= 2
assert "nvAmpereCopyRuntimeSetPeerMapping(" in provider
assert "&fCopyPublicationRuntime,true" in provider
assert "NVAmpereCopyPublicationRuntime fCopyPublicationRuntime;" in provider_header

# This phase must not fake channel readiness or authorize/perform NVIDIA writes.
assert "nvAmpereCopyRuntimeBegin(" not in provider
for forbidden_write in (
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "writeRegister",
    "regWrite32",
    "setBusMasterEnable",
):
    assert forbidden_write not in provider, f"unexpected provider write path: {forbidden_write}"
assert 'setProperty("NVPeerMapperCopyPublicationRuntimeWriteAuthorized",false)' in provider

# An active transaction has no retirement/reuse proof in this phase. Teardown
# must pin the service/provider lifetime, still complete IOService stop(), and
# return before peer ownership is cleared or the PCI provider is released.
assert "bool fCopyPublicationLifetimePinned;" in provider_header
stop = provider.index("void NVPeerMapperProbe::stop")
active = provider.index("if (fCopyPublicationRuntime.transactionActive)", stop)
pin = provider.index("retain();", active)
mark_pinned = provider.index("fCopyPublicationLifetimePinned=true;", pin)
super_stop = provider.index("super::stop(provider);", mark_pinned)
quarantine_return = provider.index("return;", super_stop)
clear_mapping = provider.index("nvAmpereCopyRuntimeSetPeerMapping(&fCopyPublicationRuntime,false)", quarantine_return)
release_provider = provider.index("OSSafeReleaseNULL(fPCIDevice);", clear_mapping)
assert active < pin < mark_pinned < super_stop < quarantine_return < clear_mapping < release_provider
assert 'setProperty("NVPeerMapperCopyPublicationLifetimePinned",true)' in provider

# Do not publish internal ordering witnesses, raw addresses, IOVAs, or tokens as
# IORegistry properties from the provider service.
for forbidden_property in (
    "NVPeerMapperCopyPublicationOrdinal",
    "NVPeerMapperGpu",
    "NVPeerMapperIova",
    "NVPeerMapperIOVA",
    "NVPeerMapperWorkSubmissionToken",
):
    assert forbidden_property not in provider

print("GA107 provider publication runtime shape: ok")
