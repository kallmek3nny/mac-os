#!/usr/bin/env python3
from pathlib import Path

src = Path("phase0-kext/NVGspWprFactsObserver.cpp").read_text(encoding="utf-8")
hdr = Path("phase0-kext/NVGspWprFactsObserver.hpp").read_text(encoding="utf-8")
plist = Path("phase0-kext/Info.plist").read_text(encoding="utf-8")

required = {
    "boot argument gate": 'PE_parse_boot_argn("nvgsp-wpr-facts-observe"',
    "writes-possible false": 'setProperty("NVGspWprFactsObserverWritesPossible",false)',
    "PCI command read": "configRead16(kIOPCIConfigCommand)",
    "memory-disabled refusal": 'publishState("pci-memory-disabled")',
    "read-only BAR mapping": "bar->map(kIOMapInhibitCache)",
    "GFW snapshot": "NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT",
    "GFW completion refusal": 'publishState("gfw-not-complete")',
    "usable FB read": "NV_GSP_GA107_USABLE_FB_MB_REG",
    "display fuse read": "NV_GSP_GA107_DISPLAY_FUSE_REG",
    "VGA workspace read": "NV_GSP_GA107_VGA_WORKSPACE_REG",
    "shared facts decoder": "nvGspDecodeGa107WprFacts",
}
for label, needle in required.items():
    if needle not in src:
        raise SystemExit(f"missing {label}: {needle}")

# This observer is intentionally incapable of mutation. If a future stage needs
# a write primitive, it belongs in a separately gated writer service.
forbidden = (
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "configWrite",
    "setMemoryEnable",
    "setBusMasterEnable",
    "regWrite",
    "fPCI->open(",
    "provider->open(",
    "IODMACommand",
)
for token in forbidden:
    if token in src or token in hdr:
        raise SystemExit(f"read-only observer contains forbidden primitive: {token}")

if '#include "NVGspWprGa107Facts.hpp"' not in hdr:
    raise SystemExit("observer must reuse the shared GA107 WPR facts decoder")

for needle in (
    "<key>NVGspWprFactsObserver</key>",
    "<string>NVGspWprFactsObserver</string>",
    "<string>0x25a210de</string>",
    "<integer>99300</integer>",
):
    if needle not in plist:
        raise SystemExit(f"observer personality contract missing: {needle}")

print("WPR facts observer static safety contract: ok")
