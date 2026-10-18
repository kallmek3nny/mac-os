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

assert "bool initialized = false;" in body
assert "bool providerOwnershipUncertain = false;" in body
assert "NV_COPY_RUNTIME_ERR_OWNERSHIP_UNCERTAIN" in runtime
assert "nvAmpereCopyRuntimeMarkOwnershipUncertain" in runtime
assert runtime.count("if (runtime->providerOwnershipUncertain)") >= 6
assert "bool runtimeWriteAuthorized;" in body
assert "NV_COPY_RUNTIME_ERR_UNINITIALIZED" in runtime
assert "NV_COPY_RUNTIME_ERR_REINITIALIZATION" in runtime
assert "if (runtime->initialized)" in runtime
assert runtime.count("if (!runtime->initialized)") >= 5
assert "NVCopyRuntimeU64 nextPublicationOrdinal;" in body
assert "NVAmpereCopyPublicationEvidence evidence;" in body

# Copy publication no longer accepts a free-form channel evidence snapshot. It
# can begin only from the separately sealed provider-owned channel runtime.
begin_sig = re.search(
    r"nvAmpereCopyRuntimeBegin\s*\((?P<args>.*?)\)\n\{",
    runtime,
    flags=re.S,
)
assert begin_sig, "copy runtime begin function missing"
assert "const NVAmpereChannelRuntime &channelRuntime" in begin_sig.group("args")
assert "const NVAmpereChannelEvidence &" not in begin_sig.group("args")
assert "nvAmpereChannelRuntimeCopyEvidence(channelRuntime, &channel)" in runtime
assert "nvAmpereEvaluateChannel(channel, &readiness)" in runtime

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
assert "NVAmpereChannelRuntime fChannelRuntime;" in provider_header
assert "NVAmpereCopyPublicationRuntime fCopyPublicationRuntime;" in provider_header
assert "nvAmpereChannelRuntimeInit(&fChannelRuntime)" in provider

# This provider has no truthful channel-allocation producer yet. Owning the
# runtime must not be confused with observing or sealing channel readiness.
assert "nvAmpereChannelRuntimeObserveAllocation(" not in provider
assert "nvAmpereChannelRuntimeObserveSubmissionEndpoint(" not in provider
assert "nvAmpereChannelRuntimeSeal(" not in provider
assert 'setProperty("NVPeerMapperChannelRuntimeSealed",false)' in provider
assert '"awaiting-real-channel-observer"' in provider

# This phase must not start copy publication or authorize/perform NVIDIA writes.
assert "nvAmpereCopyRuntimeBegin(" not in provider
for forbidden_write in (
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "writeRegister",
    "regWrite32",
    "setBusMasterEnable",
):
    assert forbidden_write not in provider, f"unexpected provider write path: {forbidden_write}"
assert 'setProperty("NVPeerMapperChannelRuntimeWriteAuthorized",false)' in provider
assert 'setProperty("NVPeerMapperCopyPublicationRuntimeWriteAuthorized",false)' in provider

# Any observed channel ownership now participates in the same conservative
# teardown quarantine as an active copy transaction. stop() must pin the service,
# call super::stop(), and return before mapper ownership or PCI lifetime clears.
assert "bool fCopyPublicationLifetimePinned;" in provider_header
stop = provider.index("void NVPeerMapperProbe::stop")
channel_ownership = provider.index("const bool channelOwnershipObserved=", stop)
active = provider.index(
    "if (channelOwnershipObserved || fCopyPublicationRuntime.transactionActive)",
    channel_ownership,
)
pin = provider.index("retain();", active)
mark_pinned = provider.index("fCopyPublicationLifetimePinned=true;", pin)
super_stop = provider.index("super::stop(provider);", mark_pinned)
quarantine_return = provider.index("return;", super_stop)
clear_mapping = provider.index("nvAmpereCopyRuntimeSetPeerMapping(&fCopyPublicationRuntime,false)", quarantine_return)
release_provider = provider.index("OSSafeReleaseNULL(fPCIDevice);", clear_mapping)
assert channel_ownership < active < pin < mark_pinned < super_stop < quarantine_return < clear_mapping < release_provider
assert "fChannelRuntime.allocationObserved" in provider[channel_ownership:active]
assert "fChannelRuntime.submissionEndpointObserved" in provider[channel_ownership:active]
assert "fChannelRuntime.sealed" in provider[channel_ownership:active]
assert "nvAmpereChannelRuntimeMarkOwnershipUncertain(&fChannelRuntime)" in provider[active:pin]
assert "nvAmpereCopyRuntimeMarkOwnershipUncertain(&fCopyPublicationRuntime)" in provider[active:pin]
assert "NVPeerMapperChannelOwnershipUncertain" in provider
assert "NVPeerMapperCopyPublicationOwnershipUncertain" in provider
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

# A clean IOKit restart must not revive previously owned or quarantined
# channel/copy state. Check the pure guard and its placement before every
# provider start side effect, including super::start() and PCI retention.
assert "nvAmpereProviderRuntimeMayStart(" in runtime
for field in (
    "!lifetimePinned", "!pciDependencyRetained",
    "!channel.ownershipUncertain", "!publication.providerOwnershipUncertain",
    "!channel.allocationObserved", "!channel.submissionEndpointObserved",
    "!channel.sealed", "!publication.transactionActive",
    "!publication.anchorMinted", "!publication.completionObservationStarted",
):
    assert field in runtime, f"missing restart guard: {field}"
start_idx = provider.index("bool NVPeerMapperProbe::start")
start_end = provider.index("void NVPeerMapperProbe::stop", start_idx)
start_body = provider[start_idx:start_end]
guard_idx = start_body.index("nvAmpereProviderRuntimeMayStart(")
assert guard_idx < start_body.index("PE_parse_boot_argn(")
assert guard_idx < start_body.index("super::start(provider)")
assert guard_idx < start_body.index("fPCIDevice=OSDynamicCast(")
assert "fCopyPublicationLifetimePinned, fPCIDevice!=nullptr" in start_body

print("GA107 provider publication runtime shape: ok")
