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

print("GA107 GSP queue backing public evidence contract: ok")
