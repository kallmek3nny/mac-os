#!/usr/bin/env python3
from pathlib import Path

src = Path("phase0-kext/NVGspWprVgaObserver.cpp").read_text(encoding="utf-8")
hdr = Path("phase0-kext/NVGspWprVgaObserver.hpp").read_text(encoding="utf-8")
plist = Path("phase0-kext/Info.plist").read_text(encoding="utf-8")

required = {
    "boot-arg gate": "NV_GSP_WPR_VGA_OBSERVER_BOOT_ARG",
    "memory-decode observation": "configRead16(kIOPCIConfigCommand)",
    "memory-disabled refusal": 'publishState("pci-memory-disabled")',
    "read-only BAR mapping": "bar->map(kIOMapInhibitCache)",
    "single GFW read": "read32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT)",
    "usable-FB read": "read32(NV_USABLE_FB_SIZE_IN_MB)",
    "display-fuse read": "read32(NV_GSP_GA107_DISPLAY_FUSE_REGISTER)",
    "conditional workspace read": "read32(NV_GSP_GA107_VGA_WORKSPACE_REGISTER)",
    "writes-possible false": 'setProperty("NVGspWprVgaObserverWritesPossible",false)',
}
for label, needle in required.items():
    if needle not in src:
        raise SystemExit(f"missing {label}: {needle}")

# These are deliberately lexical. This tiny service should not contain even a
# dormant writer primitive. If a future refactor truly needs one, it must move
# to a separately gated writer service rather than weakening this observer.
forbidden = (
    "OSWriteLittleInt",
    "OSWriteBigInt",
    "configWrite",
    "setMemoryEnable",
    "setBusMasterEnable",
    "regWrite",
    "fPCI->open(",
    "provider->open(",
)
for token in forbidden:
    if token in src:
        raise SystemExit(f"read-only observer contains forbidden writer primitive: {token}")

if "void write32" in hdr or "regWrite" in hdr:
    raise SystemExit("read-only observer header exposes a writer")

personality = "<key>NVGspWprVgaObserver</key>"
category = "<string>NVGspWprVgaObserver</string>"
match = "<string>0x25a210de</string>"
if personality not in plist or category not in plist or match not in plist:
    raise SystemExit("observer personality/category/exact GA107 match missing from Info.plist")

print("WPR VGA observer static safety contract: ok")
