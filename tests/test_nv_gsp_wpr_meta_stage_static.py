#!/usr/bin/env python3
from pathlib import Path
import plistlib

src = Path("phase0-kext/NVGspWprMetaStageService.cpp").read_text(encoding="utf-8")
hdr = Path("phase0-kext/NVGspWprMetaStageService.hpp").read_text(encoding="utf-8")
mk = Path("phase0-kext/Makefile").read_text(encoding="utf-8")
plist_bytes = Path("phase0-kext/Info.plist").read_bytes()
plist = plistlib.loads(plist_bytes)

required_src = {
    "boot argument gate": "NV_GSP_WPR_META_STAGE_BOOT_ARG",
    "bounded sibling wait": "waitForMatchingService(matching,NV_GSP_WPR_META_STAGE_WAIT_NS)",
    "Radix3 dependency": 'waitOne("NVGspRmImageStageService")',
    "boot-ucode dependency": 'waitOne("NVGspWprBootUcodeStageService")',
    "runtime-facts dependency": 'waitOne("NVGspWprFactsObserver")',
    "same-provider check": "sameProvider(radix,fPCI)",
    "sysmem revalidation": "nvGspPlanWprSysmem",
    "facts revalidation": "nvGspDecodeGa107WprFacts",
    "metadata recomposition": "nvGspPlanGa107WprCandidate",
    "unverified requirement": "candidate.metadata.verified!=0ull",
    "fresh private backing": "IOBufferMemoryDescriptor::inTaskWithOptions",
    "device-specific mapper": "IOMapper::copyMapperForDevice(fPCI)",
    "DMA synchronization": "fDMA->synchronize(kIODirectionOut)",
    "prepared marker": 'setProperty("NVGspWprMetaStagePrepared",true)',
    "not-installed address kind": 'ga107-device-iovm-wpr-metadata-not-installed',
}
for label, needle in required_src.items():
    if needle not in src:
        raise SystemExit(f"missing {label}: {needle}")

required_false = (
    'setProperty("NVGspWprMetaStageDeviceWritesPossible",false)',
    'setProperty("NVGspWprMetaStageWprWritten",false)',
    'setProperty("NVGspWprMetaStageBooterExecuted",false)',
    'setProperty("NVGspWprMetaStageMailboxWritten",false)',
    'setProperty("NVGspWprMetaStageGspNotified",false)',
    'setProperty("NVGspWprMetaStageMetadataVerified",false)',
)
for needle in required_false:
    if needle not in src:
        raise SystemExit(f"missing negative safety property: {needle}")

# This service may write only its freshly allocated CPU-visible backing page.
# Any device mutation primitive belongs in a later, separately gated service.
forbidden = (
    "configWrite",
    "setMemoryEnable",
    "setBusMasterEnable",
    "fPCI->open(",
    "provider->open(",
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "regWrite",
    "write32(",
    "map(kIOMap",
    "getDeviceMemoryWithRegister",
)
for token in forbidden:
    if token in src or token in hdr:
        raise SystemExit(f"allocation-only stager contains forbidden primitive: {token}")

if '#define NV_GSP_WPR_META_STAGE_BOOT_ARG "nvgsp-wpr-meta-stage"' not in hdr:
    raise SystemExit("wrong or missing metadata-stage boot argument")
if "#define NV_GSP_WPR_META_STAGE_WAIT_NS 1000000000ull" not in hdr:
    raise SystemExit("dependency wait must remain explicitly bounded to one second")
if "NVGspWprMetaStageService.cpp" not in mk:
    raise SystemExit("metadata stager is not linked into the kext")

personalities = plist.get("IOKitPersonalities", {})
p = personalities.get("NVGspWprMetaStageService")
if not isinstance(p, dict):
    raise SystemExit("missing NVGspWprMetaStageService personality")
expected = {
    "CFBundleIdentifier": "local.nv3050.NV3050Probe",
    "IOClass": "NVGspWprMetaStageService",
    "IOProviderClass": "IOPCIDevice",
    "IOPCIMatch": "0x25a210de",
    "IOMatchCategory": "NVGspWprMetaStageService",
    "IOProbeScore": 99200,
}
for key, value in expected.items():
    if p.get(key) != value:
        raise SystemExit(f"personality mismatch for {key}: {p.get(key)!r} != {value!r}")

print("WPR metadata allocation-only staging safety contract: ok")
