#!/usr/bin/env python3
"""Read-only GA107 live PGSP queue observation source-shape safety contract."""
from pathlib import Path

source = Path("phase0-kext/NVGspQueueObserver.cpp").read_text(encoding="utf-8")
header = Path("phase0-kext/NVGspQueueObserver.hpp").read_text(encoding="utf-8")
plist = Path("phase0-kext/Info.plist").read_text(encoding="utf-8")

required = {
    "exact NVIDIA vendor": "vendor!=0x10deu",
    "exact GA107 device": "device!=0x25a2u",
    "display-class match": "baseClass!=0x03u",
    "opt-in observation gate": 'PE_parse_boot_argn("nvgsp-queue-observe"',
    "PCI command only read": "configRead16(kIOPCIConfigCommand)",
    "PCI memory enabled": 'publishState("pci-memory-disabled")',
    "noncached BAR mapping": "bar->map(kIOMapInhibitCache)",
    "BAR bounds check": 'publishState("bar0-too-short")',
    "paired pure classifier": "nvGspClassifyStableQueueSnapshot(",
    "unstable result handling": "verdict==NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ",
    "unstable state": 'publishState("snapshot-changed-during-read")',
    "bounded stability observation": 'setProperty("NVGspQueueObserverSampledTwiceUnchanged",',
}
for label, needle in required.items():
    if needle not in source:
        raise SystemExit(f"missing {label}: {needle}")

for reg in ("commandHead", "commandTail", "messageHead", "messageTail"):
    read = f"OSReadLittleInt32(fBar0Base,regs.{reg})"
    if source.count(read) != 2:
        raise SystemExit(f"{reg} must be read exactly twice, not polled")
    for sample in ("first", "second"):
        if f"{sample}.{reg}={read}" not in source:
            raise SystemExit(f"missing bounded {sample} reading for {reg}")

classifier = source.index("nvGspClassifyStableQueueSnapshot(")
instability = source.index("if (verdict==NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ)")
publish = source.index('setProperty("NVGspQueueCommandHead"')
if not classifier < instability < publish:
    raise SystemExit("inconsistent registers could escape before pair validation")
unstable_block = source[instability:publish]
if "registerService();" not in unstable_block or "return true;" not in unstable_block:
    raise SystemExit("unstable counter observation must exit before publication")
if "snapshot.sampledTwiceUnchanged" not in source:
    raise SystemExit("provenance flag must come from shared classifier")

for primitive in (
    "OSWriteLittleInt", "OSWriteBigInt", "configWrite",
    "setMemoryEnable", "setBusMasterEnable", "regWrite",
    "fPCI->open(", "provider->open(", "IODMACommand",
):
    if primitive in source or primitive in header:
        raise SystemExit(f"read-only observer contains write primitive: {primitive}")

if '#include "NVGspQueueObserve.hpp"' not in header:
    raise SystemExit("provider must use shared source-backed observation classifier")
for needle in (
    "<key>NVGspQueueObserver</key>",
    "<string>NVGspQueueObserver</string>",
    "<string>0x25a210de</string>",
    "<integer>99800</integer>",
):
    if needle not in plist:
        raise SystemExit(f"GA107 observer personality contract missing: {needle}")

print("GA107 bounded queue double-read observer source contract: ok")
