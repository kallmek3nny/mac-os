#!/usr/bin/env python3
"""Guard both GA107 providers against start-time substitution and stale ownership."""
from pathlib import Path

helper = Path("phase0-kext/NVGa107PciIdentity.hpp").read_text(encoding="utf-8")
assert "vendor == 0x10deu && device == 0x25a2u" in helper
assert "((classRevision >> 24u) & 0xffu) == 0x03u" in helper

for filename, resources, boot in (
    ("NVGspQueueObserver.cpp", "fPCI || fBar0Map || fBar0Base", "nvgsp-queue-observe"),
    ("NVGspQueueBackingService.cpp", "fPCI || fBacking || fDMA || fDMABound", "nvgsp-queue-memory"),
):
    text = Path("phase0-kext", filename).read_text(encoding="utf-8")
    assert '#include "NVGa107PciIdentity.hpp"' in text
    start = text.split("::start(IOService *provider)", 1)[1].split(
        "::stop(IOService *provider)", 1
    )[0]
    before, after = start.split("if (!super::start(provider))", 1)
    for needle in (
        resources,
        f'PE_parse_boot_argn("{boot}"',
        "IOPCIDevice *candidate=OSDynamicCast(IOPCIDevice,provider)",
        "if (!candidate) return false;",
        "candidate->configRead16(kIOPCIConfigVendorID)",
        "candidate->configRead16(kIOPCIConfigDeviceID)",
        "candidate->configRead32(kIOPCIConfigRevisionID)",
        "if (!nvGa107PciIdentityEligible(vendor,device,classReg))",
    ):
        assert needle in before, f"{filename}: missing pre-start check {needle}"
    assert before.index(resources) < before.index("IOPCIDevice *candidate=")
    assert before.index("IOPCIDevice *candidate=") < before.index(
        "if (!nvGa107PciIdentityEligible("
    )
    assert "fPCI=candidate;" in after and "fPCI->retain();" in after
    assert "no-pci-provider" not in start, filename
    assert text.count("if (!super::start(provider))") == 1
    for banned in ("configWrite", "OSWriteLittleInt", "setBusMasterEnable",
                   "setMemoryEnable", "fPCI->open(", "provider->open("):
        assert banned not in text, f"{filename}: unsafe primitive {banned}"

print("GA107 provider start exact-device and lifetime contract: ok")
