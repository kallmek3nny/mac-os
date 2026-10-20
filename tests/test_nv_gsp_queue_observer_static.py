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


# Live-registry observation provenance must be invalidated on every service
# lifetime transition. A dead or unstable BAR0 read must not leave stale values.
assert "void invalidateObservation(void);" in header
start = source.split("bool NVGspQueueObserver::start(IOService *provider)",1)[1].split(
    "void NVGspQueueObserver::stop(IOService *provider)",1)[0]
stop = source.split("void NVGspQueueObserver::stop(IOService *provider)",1)[1]
free = source.split("void NVGspQueueObserver::free(void)",1)[1].split(
    "IOService *NVGspQueueObserver::probe",1)[0]
reset = source.split("void NVGspQueueObserver::invalidateObservation(void)",1)[1].split(
    "void NVGspQueueObserver::free(void)",1)[0]
assert "invalidateObservation();" in free
assert start.index("invalidateObservation();") < start.index(
    'setProperty("NVGspQueueObserverRequested",true)')
assert start.index("invalidateObservation();") < start.index("OSReadLittleInt32(")
assert stop.index("invalidateObservation();") < stop.index("clearResources();")
assert stop.index('publishState("stopped-no-live-snapshot")') < stop.index("clearResources();")
assert stop.index("clearResources();") < stop.index("super::stop(provider)")
for key in (
    "NVGspQueueObserverRequested",
    "NVGspQueueObserverBarMapped",
    "NVGspQueueObserverMemoryEnabled",
    "NVGspQueueObserverSampledTwiceUnchanged",
    "NVGspQueueTrafficEvidence",
    "NVGspQueueCountersPlausibleWithoutHistory",
):
    assert f'setProperty("{key}",false)' in reset, key
for key in (
    "NVGspQueueObserverQueueIndex",
    "NVGspQueueObserverPciCommand",
    "NVGspQueueObserverBarLength",
    "NVGspQueueObserverRequiredBarOffset",
    "NVGspQueueCommandHead",
    "NVGspQueueCommandTail",
    "NVGspQueueMessageHead",
    "NVGspQueueMessageTail",
    "NVGspQueueCommandHeadRegister",
    "NVGspQueueCommandTailRegister",
    "NVGspQueueMessageHeadRegister",
    "NVGspQueueMessageTailRegister",
    "NVGspQueueMessageCount",
    "NVGspQueueCommandUsed",
    "NVGspQueueStatusAvailable",
    "NVGspQueueSnapshotAllZero",
):
    assert f'removeProperty("{key}")' in reset, key
assert "NV_GSP_QUEUE_OBSERVE_NOT_ATTEMPTED" in reset
refusal = start.index("if (!nvGspQueueSnapshotCanPublish(verdict,&snapshot))")
first_publish = start.index('setProperty("NVGspQueueCommandHead"')
assert start.index("nvGspClassifyStableQueueSnapshot(") < refusal < first_publish
assert "return true;" in start[refusal:first_publish]
assert "queue-register-dead-read" in start[refusal:first_publish]

# A second PCI configuration sample must occur after the bounded BAR0 pair,
# but before the first IORegistry command/status counter can be published.
# Changed provider identity or lost PCI MEMORY must revoke all observations.
require_live = (
    '#include "NVGspQueueLiveEvidence.hpp"',
    "const NVGspQueuePciSample pciBefore",
    "const NVGspQueuePciSample pciAfter",
    "nvGspQueueLiveEvidenceCanPublish(&pciBefore,&pciAfter,",
    'publishState("pci-provider-changed-during-snapshot")',
)
for needle in require_live:
    if needle not in source:
        raise SystemExit(f"missing provider lifetime read boundary: {needle}")
after_idx = source.index("const NVGspQueuePciSample pciAfter")
gate_idx = source.index("if (!nvGspQueueLiveEvidenceCanPublish(")
first_counter = source.index('setProperty("NVGspQueueCommandHead"')
if not classifier < after_idx < gate_idx < first_counter:
    raise SystemExit("PCI identity revalidation must precede counter publication")
gate_block = source[gate_idx:first_counter]
if not all(item in gate_block for item in (
    "invalidateObservation();", "registerService();", "return true;"
)):
    raise SystemExit("PCI ownership change must revoke and refuse counters")
for read in (
    "fPCI->configRead16(kIOPCIConfigVendorID)",
    "fPCI->configRead16(kIOPCIConfigDeviceID)",
    "fPCI->configRead32(kIOPCIConfigRevisionID)",
    "fPCI->configRead16(kIOPCIConfigCommand)",
):
    if read not in source[after_idx:gate_idx]:
        raise SystemExit(f"missing post-BAR provider read: {read}")

print("GA107 bounded queue double-read observer source contract: ok")
