#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "phase0-kext" / "NVGspWprFactsObserver.cpp").read_text()
HPP = (ROOT / "phase0-kext" / "NVGspWprFactsObserver.hpp").read_text()
POLICY = (ROOT / "phase0-kext" / "NVGspBooterFusePatchPolicy.hpp").read_text()

# The rehearsal must remain inside the already-linked exact-GA107 read-only
# observer. It is not allowed to grow a second PCI personality/owner.
assert 'void NVGspWprFactsObserver::stageBooterPrivateImage(' in CPP
assert 'NV_GSP_BOOTER_PATCH_STAGE_BOOT_ARG "nvgsp-booter-patch-stage"' in HPP
assert 'NV_GSP_BOOTER_LOAD_FIRMWARE_PROPERTY "NVGspBooterLoadFirmware"' in HPP
assert 'vendor!=0x10deu || device!=0x25a2u || baseClass!=0x03u' in CPP

# Runtime selection must consume the complete observed fuse fact through the
# policy bridge, never a loose integer fuse version.
for token in [
    'NVGspBooterFuseObservation observedBooterFuse={};',
    'observedBooterFuse=observed;',
    'stageBooterPrivateImage(observedBooterFuse);',
    'nvGspPlanObservedBooterPatch(',
    'nvGspBuildObservedBooterPrivateImage(',
]:
    assert token in CPP, f"missing full-observation policy path: {token}"

# The pure bridge must independently re-derive the register and version and
# require the parsed Booter ucode ID to equal the observed SEC2 ucode ID.
for token in [
    'nvGspBooterFuseObservationIsCanonical(',
    'observation->rawFuse == 0xffffffffu',
    'observation->fuseVersion ==',
    'nvGspBooterFuseVersionFromMask(observation->rawFuse)',
    'patch.ucodeId != observation->ucodeId',
]:
    assert token in POLICY, f"policy lost fuse/ucode validation: {token}"

# This milestone may mutate only a brand-new transient kernel buffer. There is
# deliberately no DMA map, WPR install, mailbox path, PCI ownership change, or
# MMIO write introduced by the patch stage.
stage_start = CPP.index('void NVGspWprFactsObserver::stageBooterPrivateImage(')
stage_end = CPP.index('bool NVGspWprFactsObserver::start(', stage_start)
stage = CPP[stage_start:stage_end]

for token in [
    'IOBufferMemoryDescriptor::inTaskWithOptions(',
    'bzero(privateBytes',
    'privateImage->release();',
    'firmware->release();',
    'NVGspBooterPatchStageDeviceWritesPossible",false',
    'NVGspBooterPatchStageDmaMapped",false',
    'NVGspBooterPatchStageWprWritten",false',
    'NVGspBooterPatchStageMailboxWritten",false',
    'NVGspBooterPatchStageBooterExecuted",false',
    'NVGspBooterPatchStageGspExecuted",false',
    'NVGspBooterPatchStageSourceWritten",false',
    'NVGspBooterPatchStagePrivateAddressPublished",false',
    'NVGspBooterPatchStagePrivateImageTransient",true',
    'private-image-patched-not-installed',
]:
    assert token in stage, f"patch stage missing safety marker: {token}"

for forbidden in [
    'IOMapper::',
    'IODMACommand',
    'setMemoryDescriptor(',
    'gen64IOVMSegments(',
    'synchronize(',
    'fPCI->open(',
    'fPCI->close(',
    'configWrite',
    'OSWriteLittleInt',
    'regWrite',
    'write32(',
    'PHASE4_ALLOW_WRITES',
]:
    assert forbidden not in stage, f"patch stage gained forbidden device path: {forbidden}"

# The private copy is destroyed before the helper returns. There must be no
# property that publishes its address or an IOVA/physical address.
last_zero = stage.rindex('bzero(privateBytes')
last_release = stage.rindex('privateImage->release();')
assert last_zero < last_release, "transient private image must be scrubbed before release"
for forbidden in [
    'setProperty("NVGspBooterPatchStageGpuAddress"',
    'setProperty("NVGspBooterPatchStageIova"',
    'setProperty("NVGspBooterPatchStagePhysicalAddress"',
    'setProperty("NVGspBooterPatchStageCpuAddress"',
]:
    assert forbidden not in CPP, f"private address leaked: {forbidden}"

# Existing observer ownership remains read-only. Config reads and MMIO reads are
# fine; opening the PCI device, changing command state, or any MMIO store is not.
for forbidden in [
    'fPCI->open(',
    'fPCI->close(',
    'configWrite',
    'OSWriteLittleInt',
    'PHASE4_ALLOW_WRITES',
]:
    assert forbidden not in CPP, f"observer gained forbidden write/ownership path: {forbidden}"

# The separate stage gate must not poison or replace the original WPR facts
# decode. The facts path remains present after the optional rehearsal call.
call_pos = CPP.index('stageBooterPrivateImage(observedBooterFuse);')
facts_pos = CPP.index('nvGspDecodeGa107WprFacts(', call_pos)
assert facts_pos > call_pos
assert 'fuse-observation-unavailable' in CPP

print("GA107 observed-fuse Booter private patch stage safety shape: ok")
