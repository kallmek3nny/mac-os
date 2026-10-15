#!/usr/bin/env python3
from pathlib import Path

src = Path("phase0-kext/NVGspWprFactsObserver.cpp").read_text(encoding="utf-8")
hdr = Path("phase0-kext/NVGspWprFactsObserver.hpp").read_text(encoding="utf-8")
planner = Path("phase0-kext/NVGspBooterFuse.hpp").read_text(encoding="utf-8")

required_src = {
    "exact PCI vendor read": "configRead16(kIOPCIConfigVendorID)",
    "exact PCI device read": "configRead16(kIOPCIConfigDeviceID)",
    "BOOT_0 identity read": "OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_0)",
    "BOOT_42 identity read": "OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_42)",
    "pure fuse planner": "nvGspPlanGa107BooterFuseRead(",
    "Booter ucode source constant": "NV_GSP_GA107_BOOTER_HS_UCODE_ID",
    "planner-selected runtime offset": "fBar0Base,fusePlan.registerOffset",
    "pure fuse decoder": "nvGspDecodeGa107BooterFuse(",
    "write capability false": 'setProperty("NVGspBooterFuseObserverWritesPossible",false)',
    "success publication": 'setProperty("NVGspBooterFuseObserved",true)',
    "version publication": 'setProperty("NVGspBooterFuseVersion"',
    "dead-read raw suppression": "do not publish rawFuse on the dead-read arm",
}
for label, needle in required_src.items():
    if needle not in src:
        raise SystemExit(f"missing {label}: {needle}")

if '#include "NVGspBooterFuse.hpp"' not in hdr:
    raise SystemExit("runtime observer must consume the pure Booter fuse planner")

required_planner = {
    "exact vendor": "NV_GSP_GA107_PCI_VENDOR_ID                    0x10deu",
    "exact device": "NV_GSP_GA107_PCI_DEVICE_ID                    0x25a2u",
    "source-backed SEC2 base": "NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG      0x00824140u",
    "1-indexed lower bound": "NV_GSP_GA107_SEC2_UCODE_FIRST_ID              1u",
    "16-entry upper bound": "NV_GSP_GA107_SEC2_UCODE_LAST_ID               16u",
    "all-ones refusal": "rawFuse == 0xffffffffu",
    "zero remains valid": "zero fuse bitmask to version zero",
}
for label, needle in required_planner.items():
    if needle not in planner:
        raise SystemExit(f"planner contract missing {label}: {needle}")

# Runtime shell must not carry its own SEC2 fuse address literal. The planner is
# the single source of register arithmetic and the shell consumes its output.
for literal in ("0x00824140", "0x00824148", "0x824140", "0x824148"):
    if literal in src:
        raise SystemExit(f"runtime observer hard-codes SEC2 fuse address: {literal}")

# This observer remains incapable of mutation. If Booter execution eventually
# needs a writer, it belongs behind the existing compile-time/runtime gates in
# a separate phase, not in this telemetry path.
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
    "PHASE4_ALLOW_WRITES",
)
for token in forbidden:
    if token in src or token in hdr:
        raise SystemExit(f"Booter fuse observer contains forbidden primitive: {token}")

# A successful Booter-fuse observation is exactly three additional device reads:
# BOOT_0, BOOT_42, and the planner-selected SEC2 fuse word. Existing WPR/GFW
# reads are separate and use named fixed registers, so pin the three specific
# Booter-fuse call shapes rather than counting every OSReadLittleInt32 call.
if src.count("OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_0)") != 1:
    raise SystemExit("BOOT_0 must be read exactly once for Booter fuse admission")
if src.count("OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_42)") != 1:
    raise SystemExit("BOOT_42 must be read exactly once for Booter fuse admission")
if src.count("fBar0Base,fusePlan.registerOffset") != 1:
    raise SystemExit("planner-selected SEC2 fuse register must be read exactly once")

print("GA107 SEC2 Booter fuse runtime observer static safety contract: ok")
