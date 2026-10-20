#!/usr/bin/env python3
"""Source contract for the opt-in GA107 GSP queue backing provider.

There is no macOS IOKit runtime in host CI, so assert that the real provider
does not export device IOVAs or preserve live-readiness after releasing memory.
"""
from pathlib import Path

src = Path("phase0-kext/NVGspQueueBackingService.cpp").read_text(encoding="utf-8")
hdr = Path("phase0-kext/NVGspQueueBackingService.hpp").read_text(encoding="utf-8")

def require(needle: str, label: str) -> None:
    if needle not in src:
        raise SystemExit(f"missing {label}: {needle}")

for token in (
    '"NVGspQueueBackingGpuPageFirst"',
    '"NVGspQueueBackingGpuPageLast"',
    '"NVGspQueueBackingCandidateSharedMemGpuAddress"',
    'candidate sharedMem=%llx',
):
    if token in src:
        raise SystemExit(f"raw device IOVA leaked through public evidence: {token}")

for label, needle in {
    "device exact vendor": "vendor==0x10deu",
    "device exact GA107 id": "device==0x25a2u",
    "PCI display class": "baseClass==0x03u",
    "opt-in boot argument": 'PE_parse_boot_argn("nvgsp-queue-memory"',
    "hardware writes disabled": 'setProperty("NVGspQueueBackingDeviceWritesPossible",false)',
    "GSP not notified": 'setProperty("NVGspQueueBackingGspNotified",false)',
    "PGSP pointers not written": 'setProperty("NVGspQueueBackingPgspPointersWritten",false)',
    "cached arguments not installed": 'setProperty("NVGspQueueBackingCachedArgsInstalled",false)',
    "GSP queue status remains uninitialized": 'setProperty("NVGspQueueBackingStatusQueueInitializedByHost",false)',
    "source-backed DMA validation": "nvGspFlattenQueueDmaSegments(",
    "DMA failure refusal": 'publishState("dma-segments-refused")',
    "provider prepared only after synchronize": 'publishState("owned-memory-prepared-gsp-not-notified")',
}.items():
    require(needle, label)

def section(start: str, end: str) -> str:
    return src.split(start, 1)[1].split(end, 1)[0]

release = section("void NVGspQueueBackingService::releaseResources(void)", "void NVGspQueueBackingService::free(void)")
start = section("bool NVGspQueueBackingService::start(IOService *provider)", "void NVGspQueueBackingService::stop(IOService *provider)")
stop = src.split("void NVGspQueueBackingService::stop(IOService *provider)", 1)[1]

for key in (
    "NVGspQueueBackingPrepared",
    "NVGspQueueBackingDmaBound",
    "NVGspQueueBackingDeviceMapperResolved",
    "NVGspQueueBackingAllocated",
):
    require(f'setProperty("{key}",true)', f"positive live {key}")
    if f'setProperty("{key}",false)' not in release:
        raise SystemExit(f"release fails to clear live readiness: {key}")
    if f'setProperty("{key}",false)' not in start:
        raise SystemExit(f"start may inherit stale readiness: {key}")

if start.index('setProperty("NVGspQueueBackingPrepared",true)') < start.index('fDMA->synchronize(kIODirectionOut)'):
    raise SystemExit("prepared asserted before DMA synchronization")
if stop.index("releaseResources();") > stop.index("super::stop(provider);"):
    raise SystemExit("backing resources must be retired before base stop")
if 'publishState("stopped-backing-released")' not in stop:
    raise SystemExit("stop leaves stale prepared status text")

for token in ("OSWriteLittleInt", "OSWriteBigInt", "configWrite", "setBusMasterEnable",
              "setMemoryEnable", "regWrite", "fPCI->open(", "provider->open("):
    if token in src or token in hdr:
        raise SystemExit(f"queue backing must not write device registers: {token}")


# Public geometry/results and init arguments belong to one live DMA backing.
# All fields must be revoked before the DMA descriptor is cleared or released,
# including when a new activation fails halfway through preparation.
transient_properties = (
    "NVGspQueueBackingLayoutVerdict",
    "NVGspQueueBackingSharedBytes",
    "NVGspQueueBackingPageCount",
    "NVGspQueueBackingPageTableBytes",
    "NVGspQueueBackingCommandOffset",
    "NVGspQueueBackingStatusOffset",
    "NVGspQueueBackingCapacity",
    "NVGspQueueBackingDmaSegmentIOReturn",
    "NVGspQueueBackingDmaSegmentCount",
    "NVGspQueueBackingDmaConsumedBytes",
    "NVGspQueueBackingDmaVerdict",
    "NVGspQueueBackingDmaVerdictName",
    "NVGspQueueBackingMappedBytes",
    "NVGspQueueBackingSingleDmaSegment",
    "NVGspQueueBackingMemoryVerdict",
    "NVGspQueueBackingMemoryVerdictName",
    "NVGspQueueBackingPageTableVerdict",
    "NVGspQueueBackingPageTableEntriesWritten",
    "NVGspQueueBackingTxHeaderVerdict",
    "NVGspQueueBackingDmaSynchronizeIOReturn",
    "NVGspQueueBackingInitFlags",
    "NVGspQueueBackingInitCommandOffset",
    "NVGspQueueBackingInitStatusOffset",
    "NVGspQueueBackingInitElementHeaderBytes",
    "NVGspQueueBackingInitElementMinBytes",
    "NVGspQueueBackingInitElementMaxBytes",
    "NVGspQueueBackingInitHeaderAlign",
    "NVGspQueueBackingInitElementAlignShift",
    "NVGspQueueBackingInitPageTableEntryCount",
    "NVGspQueueBackingAddressKind",
)
for key in transient_properties:
    if f'setProperty("{key}"' not in start:
        raise SystemExit(f"runtime property not published: {key}")
    revoke = f'removeProperty("{key}");'
    if revoke not in release:
        raise SystemExit(f"stale provider-owned backing metadata: {key}")
    if release.index(revoke) > release.index("fDMA->clearMemoryDescriptor()"):
        raise SystemExit(f"backing evidence revoked after DMA unbind: {key}")

for key in (
    "NVGspQueueBackingRequested",
    "NVGspQueueBackingDeviceWritesPossible",
    "NVGspQueueBackingGspNotified",
    "NVGspQueueBackingPgspPointersWritten",
    "NVGspQueueBackingCachedArgsInstalled",
    "NVGspQueueBackingStatusQueueInitializedByHost",
):
    if f'setProperty("{key}",false)' not in release:
        raise SystemExit(f"stop retains provider readiness flag: {key}")

activation = 'releaseResources();\\n    publishState("preparing-provider-owned-memory");'
if activation.replace("\\n", "\n") not in start:
    raise SystemExit("new activation does not revoke previous provider evidence")
if start.index("releaseResources();") > start.index('setProperty("NVGspQueueBackingRequested",true)'):
    raise SystemExit("provider evidence reset must precede activation publication")
if release.index("removeProperty") > release.index("fDMA->clearMemoryDescriptor()"):
    raise SystemExit("DMA may be unbound before all evidence was revoked")


# Successful CPU composition and DMA synchronization do not prove the PCI
# provider still owns that mapping. Refuse publication if identity changes,
# and revoke mapped memory/IORegistry evidence before registering failure.
postread = start.index("const UInt16 currentVendor=fPCI->configRead16(kIOPCIConfigVendorID)")
if not start.index("fDMA->synchronize(kIODirectionOut)") < postread < start.index(
    'setProperty("NVGspQueueBackingPrepared",true)'
):
    raise SystemExit("PCI identity must be revalidated after sync, before readiness")
for needle in (
    "const UInt16 currentDevice=fPCI->configRead16(kIOPCIConfigDeviceID)",
    "const UInt32 currentClassReg=fPCI->configRead32(kIOPCIConfigRevisionID)",
    "if (!nvGa107PciIdentityStableAcrossStage(",
    "vendor,device,classReg,currentVendor,currentDevice,currentClassReg",
    'publishState("pci-provider-changed-before-prepared")',
):
    if needle not in start[postread:]:
        raise SystemExit(f"missing provider post-sync identity gate: {needle}")
refuse = start.split("if (!nvGa107PciIdentityStableAcrossStage(", 1)[1].split(
    "// The candidate address remains private", 1
)[0]
for needle in ("releaseResources();", "registerService();", "return true;"):
    if needle not in refuse:
        raise SystemExit(f"provider identity failure leaves DMA ownership live: {needle}")
if not refuse.index("releaseResources();") < refuse.index("registerService();"):
    raise SystemExit("provider removal must retire resources before publication")

# A partial IORegistry write must never claim an otherwise valid DMA backing
# is prepared. Failures also revoke the complete live-state snapshot before
# presenting the failure to clients.
publish = start.split("bool published = true;", 1)[1].split(
    'publishState("owned-memory-prepared-gsp-not-notified")', 1
)[0]
for name in (
    "NVGspQueueBackingInitFlags",
    "NVGspQueueBackingInitCommandOffset",
    "NVGspQueueBackingInitStatusOffset",
    "NVGspQueueBackingInitElementHeaderBytes",
    "NVGspQueueBackingInitElementMinBytes",
    "NVGspQueueBackingInitElementMaxBytes",
    "NVGspQueueBackingInitHeaderAlign",
    "NVGspQueueBackingInitElementAlignShift",
    "NVGspQueueBackingInitPageTableEntryCount",
    "NVGspQueueBackingAddressKind",
):
    if f'published = setProperty("{name}"' not in publish:
        raise SystemExit(f"required prepared field not checked: {name}")
if publish.index('setProperty("NVGspQueueBackingAddressKind"') > publish.index(
    'setProperty("NVGspQueueBackingPrepared",true)'
):
    raise SystemExit("prepared bit must be published last")
if 'if (!published || !setProperty("NVGspQueueBackingPrepared",true)) {' not in publish:
    raise SystemExit("prepared readiness must be checked and fail closed")
fail = publish.split('if (!published || !setProperty("NVGspQueueBackingPrepared",true)) {', 1)[1]
for needle in (
    'publishState("prepared-evidence-publication-failed")',
    "releaseResources();",
    "registerService();",
    "return true;",
):
    if needle not in fail:
        raise SystemExit(f"partial prepared evidence not fully revoked: {needle}")
if not fail.index("releaseResources();") < fail.index("registerService();"):
    raise SystemExit("partial metadata must be withdrawn before failure publication")
for state in (
    "requested-evidence-publication-failed",
    "queue-layout-publication-failed",
    "allocation-evidence-publication-failed",
    "mapper-evidence-publication-failed",
    "dma-bind-evidence-publication-failed",
    "mapped-evidence-publication-failed",
):
    if f'publishState("{state}")' not in start:
        raise SystemExit(f"missing fail-closed runtime publication gate: {state}")

print("GA107 GSP queue backing public evidence contract: ok")
